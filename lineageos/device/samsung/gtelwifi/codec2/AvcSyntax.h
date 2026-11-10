// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace android::sprd {
struct Sequence {
    uint32_t id = 0, codedWidth = 0, codedHeight = 0;
    uint32_t width = 0, height = 0, left = 0, top = 0, references = 0;
    uint32_t outputDelay = 16;
    uint32_t primaries = 2, transfer = 2, matrix = 2;
    bool videoSignal = false, fullRange = false;
    bool sameGeometry(const Sequence &s) const {
        return codedWidth == s.codedWidth && codedHeight == s.codedHeight &&
               width == s.width && height == s.height && left == s.left && top == s.top;
    }
};
// Annex A MaxDpbMbs, also used by AOSP libavc's level table. Bound both
// reordering and references, rather than assuming num_ref_frames bounds B frames.
// Unknown levels and contradictory reference counts keep the conservative bound.
inline uint32_t outputDelay(uint32_t level, uint32_t constraints,
                            uint32_t width, uint32_t height, uint32_t references) {
    uint32_t mbs;
    switch (level) {
        case 10: case 9: mbs = 396; break;
        case 11: mbs = (constraints & 0x10) ? 396 : 900; break;
        case 12: case 13: case 20: mbs = 2376; break;
        case 21: mbs = 4752; break;
        case 22: case 30: mbs = 8100; break;
        case 31: mbs = 18000; break;
        case 32: mbs = 20480; break;
        case 40: case 41: mbs = 32768; break;
        default: return 16;
    }
    if (!width || !height || width % 16 || height % 16 || width > 1920 || height > 1088)
        return 16;
    uint32_t frames = std::min(16u, mbs / ((width / 16) * (height / 16)));
    if (!frames || references > frames) return 16;
    return frames;
}
class Bits {
public:
    Bits(const uint8_t *data, size_t size, bool cabacPadding = false)
        : mData(data), mSize(size) {
        unsigned zeros = 0;
        for (size_t i = 0; i < size; ++i) {
            uint8_t value = data[i];
            if (zeros >= 2 && value == 3) {
                // H.264 VCL CABAC padding may end in 00 00 03. The final
                // prevention byte has no following payload byte (7.4.1/9.3.4.6).
                // Keep SPS/PPS parsing strict: this exception is VCL-only.
                if (i + 1 == size && cabacPadding) { mSize = i; break; }
                if (i + 1 == size || data[i + 1] > 3) { mValid = false; return; }
                zeros = 0;
                continue;
            }
            zeros = value == 0 ? zeros + 1 : 0;
        }
    }
    std::optional<uint32_t> get(unsigned count) {
        if (!mValid || count > 32) return {};
        // Validate the whole NAL in the constructor, but read only the header
        // bits requested by the caller. Never copy an entire CABAC slice merely
        // to extract its first three Exp-Golomb values.
        Bits next = *this;
        uint32_t value = 0;
        while (count--) {
            if (!next.mRemaining) {
                for (;;) {
                    if (next.mOffset == next.mSize) return {};
                    uint8_t byte = next.mData[next.mOffset++];
                    if (next.mZeros >= 2 && byte == 3) {
                        next.mZeros = 0;
                        continue;
                    }
                    next.mZeros = byte == 0 ? next.mZeros + 1 : 0;
                    next.mByte = byte;
                    next.mRemaining = 8;
                    break;
                }
            }
            value = (value << 1) | ((next.mByte >> --next.mRemaining) & 1);
        }
        *this = next;
        return value;
    }
    std::optional<uint32_t> ue() {
        unsigned zeros = 0;
        for (;;) {
            auto bit = get(1);
            if (!bit || zeros > 30) return {};
            if (*bit) break;
            ++zeros;
        }
        auto suffix = get(zeros);
        if (!suffix) return {};
        return ((uint32_t{1} << zeros) - 1) + *suffix;
    }
    std::optional<int32_t> se() {
        auto value = ue();
        if (!value || *value > INT32_MAX) return {};
        return (*value & 1) ? int32_t((*value + 1) / 2) : -int32_t(*value / 2);
    }
private:
    const uint8_t *mData;
    size_t mSize, mOffset = 0;
    unsigned mZeros = 0, mRemaining = 0;
    uint8_t mByte = 0;
    bool mValid = true;
};
inline std::optional<Sequence> sequence(const uint8_t *nal, size_t size) {
    if (size < 4 || (nal[0] & 31) != 7 || (nal[0] & 0x80)) return {};
    Bits b(nal + 1, size - 1);
    auto profile = b.get(8), constraints = b.get(8), level = b.get(8), id = b.ue();
    if (!profile || !constraints || !level || !id || *id > 31 || *level > 41) return {};
    if (*profile != 66 && *profile != 77 && *profile != 100) return {};
    if (*profile == 100) {
        auto chroma = b.ue(), lumaDepth = b.ue(), chromaDepth = b.ue();
        auto bypass = b.get(1), scaling = b.get(1);
        if (!chroma || *chroma != 1 || !lumaDepth || *lumaDepth ||
            !chromaDepth || *chromaDepth || !bypass || !scaling) return {};
        if (*scaling) {
            for (unsigned i = 0; i < 8; ++i) {
                auto present = b.get(1);
                if (!present) return {};
                if (!*present) continue;
                int last = 8, next = 8;
                for (unsigned j = 0; j < (i < 6 ? 16u : 64u); ++j) {
                    if (next) {
                        auto delta = b.se();
                        if (!delta || *delta < -128 || *delta > 127) return {};
                        next = (last + *delta + 256) % 256;
                    }
                    if (next) last = next;
                }
            }
        }
    }
    auto logFrame = b.ue(), poc = b.ue();
    if (!logFrame || *logFrame > 12 || !poc || *poc > 2) return {};
    if (*poc == 0) {
        auto logPoc = b.ue();
        if (!logPoc || *logPoc > 12) return {};
    } else if (*poc == 1) {
        if (!b.get(1) || !b.se() || !b.se()) return {};
        auto count = b.ue();
        if (!count || *count > 256) return {};
        for (uint32_t i = 0; i < *count; ++i) if (!b.se()) return {};
    }
    auto refs = b.ue(), gaps = b.get(1), width = b.ue(), height = b.ue(), progressive = b.get(1);
    if (!refs || *refs > 16 || !gaps || !width || *width >= 120 ||
        !height || *height >= 68 || !progressive || !*progressive || !b.get(1)) return {};
    Sequence s;
    s.id = *id; s.codedWidth = (*width + 1) * 16; s.codedHeight = (*height + 1) * 16;
    s.width = s.codedWidth; s.height = s.codedHeight; s.references = *refs;
    s.outputDelay = outputDelay(*level, *constraints, s.codedWidth, s.codedHeight, *refs);
    auto crop = b.get(1);
    if (!crop) return {};
    if (*crop) {
        auto left = b.ue(), right = b.ue(), top = b.ue(), bottom = b.ue();
        if (!left || !right || !top || !bottom ||
            (uint64_t(*left) + *right) * 2 >= s.width ||
            (uint64_t(*top) + *bottom) * 2 >= s.height) return {};
        s.left = *left * 2; s.top = *top * 2;
        s.width -= (*left + *right) * 2; s.height -= (*top + *bottom) * 2;
    }
    if (s.height > 1080 || s.width < 2 || s.height < 2) return {};
    auto vui = b.get(1);
    if (!vui) return {};
    if (*vui) {
        auto aspect = b.get(1);
        if (!aspect) return {};
        if (*aspect) {
            auto ratio = b.get(8);
            if (!ratio) return {};
            if (*ratio == 255 && (!b.get(16) || !b.get(16))) return {};
        }
        auto overscan = b.get(1);
        if (!overscan || (*overscan && !b.get(1))) return {};
        auto signal = b.get(1);
        if (!signal) return {};
        if (*signal) {
            auto format = b.get(3), range = b.get(1), color = b.get(1);
            if (!format || !range || !color) return {};
            s.videoSignal = true; s.fullRange = *range;
            if (*color) {
                auto primaries = b.get(8), transfer = b.get(8), matrix = b.get(8);
                if (!primaries || !transfer || !matrix) return {};
                s.primaries = *primaries; s.transfer = *transfer; s.matrix = *matrix;
            }
        }
    }
    return s;
}
struct PictureParameter { uint32_t id, sequenceId; };
inline std::optional<PictureParameter> pictureParameter(const uint8_t *nal, size_t size) {
    if (size < 2 || (nal[0] & 31) != 8 || (nal[0] & 0x80)) return {};
    Bits b(nal + 1, size - 1);
    auto id = b.ue(), sequenceId = b.ue(), entropy = b.get(1), order = b.get(1), groups = b.ue();
    if (!id || *id > 255 || !sequenceId || *sequenceId > 31 || !entropy || !order ||
        !groups || *groups) return {};
    return PictureParameter{*id, *sequenceId};
}
inline std::optional<uint32_t> sliceParameterId(const uint8_t *nal, size_t size) {
    if (size < 2 || ((nal[0] & 31) != 1 && (nal[0] & 31) != 5) || (nal[0] & 0x80)) return {};
    Bits b(nal + 1, size - 1, true);
    auto first = b.ue(), type = b.ue(), pps = b.ue();
    if (!first || !type || *type > 9 || !pps || *pps > 255) return {};
    return pps;
}
struct Nal { size_t offset, size; uint8_t type; };
inline std::optional<std::vector<Nal>> splitAnnexB(const uint8_t *data, size_t size) {
    std::vector<size_t> starts, payloads;
    for (size_t i = 0; i + 3 <= size;) {
        size_t prefix = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) prefix = 3;
        else if (i + 4 <= size && data[i] == 0 && data[i + 1] == 0 &&
                 data[i + 2] == 0 && data[i + 3] == 1) prefix = 4;
        if (prefix) { starts.push_back(i); payloads.push_back(i + prefix); i += prefix; }
        else ++i;
    }
    if (starts.empty() || starts.front() != 0) return {};
    std::vector<Nal> nals;
    for (size_t i = 0; i < starts.size(); ++i) {
        size_t end = i + 1 < starts.size() ? starts[i + 1] : size;
        if (payloads[i] >= end || (data[payloads[i]] & 0x80)) return {};
        nals.push_back({payloads[i], end - payloads[i], uint8_t(data[payloads[i]] & 31)});
    }
    return nals;
}
} // namespace android::sprd

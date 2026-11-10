// SPDX-License-Identifier: Apache-2.0
// Bounded metadata gate for tested MPEG4 Simple Profile and H263 Baseline.
// Unsupported profiles/tools must reach software instead of unvalidated DMA.
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace android::sprd::m4v {
class Bits {
    const uint8_t *mData;
    size_t mSize, mPosition = 0;
    bool mValid = true;
public:
    Bits(const uint8_t *p, size_t n) : mData(p), mSize(n) {}
    uint32_t get(unsigned n) {
        if (n > 32 || mPosition / 8 > mSize || n > mSize * 8 - mPosition) {
            mValid = false; return 0;
        }
        uint32_t v = 0;
        while (n--) {
            v = (v << 1) | ((mData[mPosition / 8] >> (7 - mPosition % 8)) & 1);
            ++mPosition;
        }
        return v;
    }
    bool is(unsigned n, uint32_t v) { return get(n) == v && mValid; }
    bool valid() const { return mValid; }
};
struct Sequence {
    uint32_t width, height, timeBits = 0;
    uint32_t stride() const { return (width + 15) & ~15U; }
    uint32_t slice() const { return (height + 15) & ~15U; }
    bool operator==(const Sequence &s) const {
        return width == s.width && height == s.height && timeBits == s.timeBits;
    }
};
inline std::optional<Sequence> vol(const uint8_t *p, size_t n) {
    if (!n || n > 2 * 1024 * 1024) return {};
    Bits b(p, n);
    b.get(1); // random accessible VOL
    if (!b.is(8, 1)) return {}; // Simple Object Type only
    if (b.get(1)) {
        if (!b.is(4, 1)) return {}; // version 1; no quarter-pel/GMC/newpred tools
        b.get(3);
    }
    auto aspect = b.get(4);
    if (!aspect || aspect > 5) {
        if (aspect != 15 || !b.get(8) || !b.get(8)) return {};
    }
    if (b.get(1)) { // VOL control
        if (!b.is(2, 1)) return {}; // 4:2:0
        b.get(1);
        if (b.get(1)) { // VBV, all syntax markers are mandatory
            b.get(15); if (!b.is(1, 1)) return {};
            b.get(15); if (!b.is(1, 1)) return {};
            b.get(15); if (!b.is(1, 1)) return {};
            b.get(3); b.get(11); if (!b.is(1, 1)) return {};
            b.get(15); if (!b.is(1, 1)) return {};
        }
    }
    if (!b.is(2, 0) || !b.is(1, 1)) return {}; // rectangular shape
    const auto resolution = b.get(16);
    if (!resolution || !b.is(1, 1)) return {};
    uint32_t bits = 1;
    while (bits < 16 && (1U << bits) < resolution) ++bits;
    if (b.get(1)) b.get(bits); // fixed VOP rate
    if (!b.is(1, 1)) return {};
    const auto width = b.get(13);
    if (!b.is(1, 1)) return {};
    const auto height = b.get(13);
    if (!b.is(1, 1) || width < 2 || height < 2 || (width | height) & 1 ||
        width > 1920 || height > 1080) return {};
    if (!b.is(1, 0) || !b.is(1, 1) || !b.is(1, 0) || !b.is(1, 0) ||
        !b.is(1, 0) || !b.is(1, 1)) return {};
    // Progressive, OBMC disabled, no sprite, 8-bit, H263 quantization,
    // complexity estimation disabled. Resync markers are permitted.
    b.get(1);
    if (!b.is(1, 0) || !b.is(1, 0) || !b.valid()) return {}; // partition/scalability
    return Sequence{width, height, bits};
}
struct Access {
    std::optional<Sequence> sequence;
    bool picture = false, key = false, coded = false;
    size_t pictureOffset = 0;
    std::vector<uint8_t> config;
};
inline std::optional<Access> mpeg4(const uint8_t *p, size_t n,
                                 std::optional<Sequence> cached = {}) {
    if (n < 4 || n > 2 * 1024 * 1024 || p[0] || p[1] || p[2] != 1) return {};
    std::vector<size_t> offsets;
    for (size_t i = 0; i + 3 < n; ++i)
        if (!p[i] && !p[i + 1] && p[i + 2] == 1) { offsets.push_back(i); i += 2; }
    offsets.push_back(n);
    Access a;
    bool newVol = false;
    a.sequence = cached;
    for (size_t i = 0; i + 1 < offsets.size(); ++i) {
        const auto start = offsets[i], end = offsets[i + 1];
        const uint8_t code = p[start + 3];
        if (code >= 0x20 && code <= 0x2f) {
            if (a.picture || newVol) return {};
            a.sequence = vol(p + start + 4, end - start - 4);
            if (!a.sequence) return {};
            newVol = true;
        } else if (code == 0xb6) {
            if (a.picture || !a.sequence) return {};
            Bits b(p + start + 4, end - start - 4);
            auto type = b.get(2);
            if (type > 1) return {}; // B/S pictures are not validated by native tests
            unsigned modulo = 0;
            while (b.get(1)) if (++modulo > 32) return {};
            if (!b.is(1, 1)) return {};
            b.get(a.sequence->timeBits);
            if (!b.is(1, 1)) return {};
            a.coded = b.get(1);
            if (!b.valid()) return {};
            a.key = type == 0; a.picture = true; a.pictureOffset = start;
        } else if (code == 0xb0) {
            if (a.picture || end - start < 5 || p[start + 4] < 1 || p[start + 4] > 6) return {};
        } else if (!(code <= 0x1f || code == 0xb1 || code == 0xb2 || code == 0xb3 || code == 0xb5)) {
            return {};
        }
    }
    if (newVol) a.config.assign(p, p + (a.picture ? a.pictureOffset : n));
    return a;
}
inline std::optional<Access> h263(const uint8_t *p, size_t n) {
    if (!n || n > 2 * 1024 * 1024) return {};
    Bits b(p, n);
    if (!b.is(22, 0x20)) return {};
    b.get(8); // temporal reference
    if (!b.is(1, 1) || !b.is(1, 0) || !b.is(3, 0)) return {};
    const auto format = b.get(3);
    static constexpr uint32_t sizes[5][2] = {{0, 0}, {128, 96}, {176, 144}, {352, 288}, {704, 576}};
    if (format < 1 || format > 4) return {}; // Baseline only; no PLUSPTYPE/custom format
    Access a; a.sequence = Sequence{sizes[format][0], sizes[format][1], 0};
    a.key = !b.get(1);
    if (!b.is(4, 0) || !b.get(5) || !b.is(1, 0)) return {}; // no UMV/SAC/AP/PB/CPM
    unsigned extra = 0;
    while (b.get(1)) {
        if (++extra > 128) return {};
        b.get(8);
    }
    if (!b.valid()) return {};
    a.picture = a.coded = true;
    return a;
}
} // namespace android::sprd::m4v

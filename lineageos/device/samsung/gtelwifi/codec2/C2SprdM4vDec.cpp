// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Deniz Kokden
#define LOG_TAG "C2SprdM4vDec"
#include <log/log.h>
#include <C2AllocatorGralloc.h>
#include <C2PlatformSupport.h>
#include <SimpleC2Component.h>
#include <SimpleC2Interface.h>
#include <util/C2InterfaceHelper.h>
#include <media/stagefright/foundation/MediaDefs.h>
#include <system/graphics.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <array>
#include <cstring>
#include <optional>
#include <vector>
#include "SprdM4vAbi.h"
#include "M4vSyntax.h"
#include "SprdVspSession.h"
#include "include/linux/sprd_vsp.h"

namespace android {
namespace {
const char *componentName(bool h263) { return h263 ? "c2.sprd.h263.decoder" : "c2.sprd.mpeg4.decoder"; }
constexpr uint32_t kStream = 2 * 1024 * 1024;
#define LEGACY __attribute__((no_sanitize("cfi-icall")))
struct Memory {
    uint8_t *cpu = nullptr;
    uint32_t dma = 0, size = 0;
    void clear() { if (cpu) munmap(cpu, size); cpu = nullptr; dma = size = 0; }
};
} // namespace

class M4vInterface : public SimpleInterface<void>::BaseParams {
public:
    M4vInterface(const std::shared_ptr<C2ReflectorHelper> &helper, bool h263)
        : BaseParams(helper, componentName(h263), C2Component::KIND_DECODER,
                     C2Component::DOMAIN_VIDEO, h263 ? MEDIA_MIMETYPE_VIDEO_H263 : MEDIA_MIMETYPE_VIDEO_MPEG4) {
        noPrivateBuffers(); noInputReferences(); noInputLatency(); noTimeStretch();
        addParameter(DefineParam(mAttrib, C2_PARAMKEY_COMPONENT_ATTRIBUTES)
            .withConstValue(new C2ComponentAttributesSetting(C2Component::ATTRIB_IS_TEMPORAL)).build());
        addParameter(DefineParam(mDelay, C2_PARAMKEY_OUTPUT_DELAY)
            .withConstValue(new C2PortActualDelayTuning::output(4u)).build());
        addParameter(DefineParam(mReferences, C2_PARAMKEY_OUTPUT_MAX_REFERENCE_COUNT)
            .withConstValue(new C2StreamMaxReferenceCountTuning::output(0u, 3u)).build());
        addParameter(DefineParam(mSize, C2_PARAMKEY_PICTURE_SIZE)
            .withDefault(new C2StreamPictureSizeInfo::output(0u, 320, 240))
            .withFields({C2F(mSize, width).inRange(2, h263 ? 704 : 1920, 2),
                         C2F(mSize, height).inRange(2, h263 ? 576 : 1080, 2)})
            .withSetter(SizeSetter).build());
        addParameter(DefineParam(mMaxSize, C2_PARAMKEY_MAX_PICTURE_SIZE)
            .withConstValue(new C2StreamMaxPictureSizeTuning::output(0u, h263 ? 704 : 1920, h263 ? 576 : 1080)).build());
        addParameter(DefineParam(mInputSize, C2_PARAMKEY_INPUT_MAX_BUFFER_SIZE)
            .withConstValue(new C2StreamMaxBufferSizeInfo::input(0u, kStream - 128)).build());
        if (h263) {
            addParameter(DefineParam(mProfile, C2_PARAMKEY_PROFILE_LEVEL)
                .withDefault(new C2StreamProfileLevelInfo::input(0u,
                    C2Config::PROFILE_H263_BASELINE, C2Config::LEVEL_H263_70))
                .withFields({C2F(mProfile, profile).equalTo(C2Config::PROFILE_H263_BASELINE),
                    C2F(mProfile, level).oneOf({C2Config::LEVEL_H263_10, C2Config::LEVEL_H263_20,
                        C2Config::LEVEL_H263_30, C2Config::LEVEL_H263_40, C2Config::LEVEL_H263_45,
                        C2Config::LEVEL_H263_50, C2Config::LEVEL_H263_60, C2Config::LEVEL_H263_70})})
                .withSetter(ProfileSetter).build());
        } else {
            addParameter(DefineParam(mProfile, C2_PARAMKEY_PROFILE_LEVEL)
                .withDefault(new C2StreamProfileLevelInfo::input(0u,
                    C2Config::PROFILE_MP4V_SIMPLE, C2Config::LEVEL_MP4V_6))
                .withFields({C2F(mProfile, profile).equalTo(C2Config::PROFILE_MP4V_SIMPLE),
                    C2F(mProfile, level).oneOf({C2Config::LEVEL_MP4V_0, C2Config::LEVEL_MP4V_0B,
                        C2Config::LEVEL_MP4V_1, C2Config::LEVEL_MP4V_2, C2Config::LEVEL_MP4V_3,
                        C2Config::LEVEL_MP4V_3B, C2Config::LEVEL_MP4V_4, C2Config::LEVEL_MP4V_4A,
                        C2Config::LEVEL_MP4V_5, C2Config::LEVEL_MP4V_6})})
                .withSetter(ProfileSetter).build());
        }
        addParameter(DefineParam(mPixelFormat, C2_PARAMKEY_PIXEL_FORMAT)
            .withConstValue(new C2StreamPixelFormatInfo::output(0u, HAL_PIXEL_FORMAT_YCBCR_420_888)).build());
        addParameter(DefineParam(mDefaultColor, C2_PARAMKEY_DEFAULT_COLOR_ASPECTS)
            .withDefault(new C2StreamColorAspectsTuning::output(0u, C2Color::RANGE_UNSPECIFIED,
                C2Color::PRIMARIES_UNSPECIFIED, C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
            .withFields({C2F(mDefaultColor, range).any(), C2F(mDefaultColor, primaries).any(),
                C2F(mDefaultColor, transfer).any(), C2F(mDefaultColor, matrix).any()})
            .withSetter(DefaultColorSetter).build());
        addParameter(DefineParam(mColor, C2_PARAMKEY_COLOR_ASPECTS)
            .withDefault(new C2StreamColorAspectsInfo::output(0u, C2Color::RANGE_UNSPECIFIED,
                C2Color::PRIMARIES_UNSPECIFIED, C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
            .withFields({C2F(mColor, range).any(), C2F(mColor, primaries).any(),
                C2F(mColor, transfer).any(), C2F(mColor, matrix).any()})
            .withSetter(ColorSetter, mDefaultColor).build());
    }
    static C2R SizeSetter(bool, const C2P<C2StreamPictureSizeInfo::output> &old,
                         C2P<C2StreamPictureSizeInfo::output> &me) {
        C2R result = C2R::Ok();
        if (!me.F(me.v.width).supportsAtAll(me.v.width)) {
            result = result.plus(C2SettingResultBuilder::BadValue(me.F(me.v.width)));
            me.set().width = old.v.width;
        }
        if (!me.F(me.v.height).supportsAtAll(me.v.height)) {
            result = result.plus(C2SettingResultBuilder::BadValue(me.F(me.v.height)));
            me.set().height = old.v.height;
        }
        return result;
    }
    static C2R ProfileSetter(bool, C2P<C2StreamProfileLevelInfo::input> &me) {
        return me.F(me.v.profile).validatePossible(me.v.profile).plus(
            me.F(me.v.level).validatePossible(me.v.level));
    }
    static C2R DefaultColorSetter(bool, C2P<C2StreamColorAspectsTuning::output> &) { return C2R::Ok(); }
    static C2R ColorSetter(bool, C2P<C2StreamColorAspectsInfo::output> &me,
                          const C2P<C2StreamColorAspectsTuning::output> &color) {
        me.set().range = color.v.range; me.set().primaries = color.v.primaries;
        me.set().transfer = color.v.transfer; me.set().matrix = color.v.matrix;
        return C2R::Ok();
    }
    std::shared_ptr<C2StreamColorAspectsInfo::output> color() const { return mColor; }
private:
    std::shared_ptr<C2PortActualDelayTuning::output> mDelay;
    std::shared_ptr<C2StreamMaxReferenceCountTuning::output> mReferences;
    std::shared_ptr<C2StreamPictureSizeInfo::output> mSize;
    std::shared_ptr<C2StreamMaxPictureSizeTuning::output> mMaxSize;
    std::shared_ptr<C2StreamMaxBufferSizeInfo::input> mInputSize;
    std::shared_ptr<C2StreamProfileLevelInfo::input> mProfile;
    std::shared_ptr<C2StreamPixelFormatInfo::output> mPixelFormat;
    std::shared_ptr<C2StreamColorAspectsTuning::output> mDefaultColor;
    std::shared_ptr<C2StreamColorAspectsInfo::output> mColor;
};

// Native-only until the contract and real MediaCodec tests pass.
class C2SprdM4vDec : public SimpleC2Component {
public:
    C2SprdM4vDec(c2_node_id_t id, const std::shared_ptr<M4vInterface> &intf, bool h263)
        : SimpleC2Component(std::make_shared<SimpleInterface<M4vInterface>>(componentName(h263), id, intf)),
          mIntf(intf), mH263(h263) {}
    ~C2SprdM4vDec() override { onRelease(); }
    c2_status_t onInit() override;
    c2_status_t onStop() override { closeDecoder(); mError = mEos = false; return C2_OK; }
    void onReset() override { onStop(); mVol.reset(); mCsd.clear(); }
    void onRelease() override;
    c2_status_t onFlush_sm() override { return onStop(); }
    void process(const std::unique_ptr<C2Work> &, const std::shared_ptr<C2BlockPool> &) override;
    c2_status_t drain(uint32_t, const std::shared_ptr<C2BlockPool> &) override;
private:
    using Sequence = sprd::m4v::Sequence;
    struct Frame {
        std::shared_ptr<C2GraphicBlock> block;
        std::unique_ptr<C2GraphicView> view;
        uint32_t importId = 0, dma = 0;
        unsigned refs = 0;
        bool pending = false;
        C2WorkOrdinalStruct ordinal;
        Sequence geometry{};
        std::shared_ptr<C2StreamColorAspectsInfo::output> color;
    };
    std::shared_ptr<M4vInterface> mIntf;
    const bool mH263;
    std::array<Frame, 12> mFrames;
    std::optional<Sequence> mGeometry, mVol;
    std::vector<uint8_t> mCsd, mAppliedCsd, mInternal;
    void *mLibrary = nullptr;
    int mFd = -1;
    bool mOwnsEngine = false, mInitialized = false, mError = false, mEos = false, mSeek = true;
    m4v_handle mHandle{};
    Memory mStream, mExtra;
    int (*mInit)(m4v_handle *, m4v_buffer *) = nullptr;
    int (*mVolHeader)(m4v_handle *, m4v_format *) = nullptr;
    int (*mMemInit)(m4v_handle *, m4v_buffer *) = nullptr;
    int (*mDecode)(m4v_handle *, m4v_input *, m4v_output *) = nullptr;
    int (*mRelease)(m4v_handle *) = nullptr;
    int (*mCapability)(m4v_handle *, int32_t *, int32_t *) = nullptr;
    void (*mDimensions)(m4v_handle *, int32_t *, int32_t *) = nullptr;
    void (*mSetPic)(m4v_handle *, uint8_t *, uint8_t *, void *) = nullptr;
    void (*mReleaseRefs)(m4v_handle *) = nullptr;
    int (*mDrain)(m4v_handle *, void **) = nullptr;
    Frame *checked(void *header) {
        for (auto &f : mFrames) if (header == &f && f.block) return &f;
        return nullptr;
    }
    static int Bind(void *self, void *header, int) {
        auto *p = static_cast<C2SprdM4vDec *>(self); auto *f = p->checked(header);
        if (!f || f->refs >= 16) { p->mError = true; return -1; }
        ++f->refs; return 0;
    }
    static int Unbind(void *self, void *header, int) {
        auto *p = static_cast<C2SprdM4vDec *>(self); auto *f = p->checked(header);
        if (!f || !f->refs) { p->mError = true; return -1; }
        --f->refs; return 0;
    }
    static LEGACY int Extra(void *self, unsigned size) {
        auto *p = static_cast<C2SprdM4vDec *>(self);
        if (!size || size > 16 * 1024 * 1024 || !p->allocate(p->mExtra, size)) return -1;
        m4v_buffer types[4]{};
        types[1] = {p->mExtra.cpu, p->mExtra.dma, p->mExtra.size, 0, nullptr, 0};
        return p->mMemInit(&p->mHandle, types);
    }
    bool allocate(Memory &m, uint32_t size) {
        if (m.cpu) return m.size >= size;
        sprd_vsp_buffer b{}; b.size = size;
        if (ioctl(mFd, VSP_ALLOC_BUFFER, &b)) return false;
        void *cpu = mmap(nullptr, b.size, PROT_READ | PROT_WRITE, MAP_SHARED, mFd, b.mmap_offset);
        if (cpu == MAP_FAILED) return false;
        m = {static_cast<uint8_t *>(cpu), b.dma_addr, b.size}; return true;
    }
    c2_status_t initDecoder(const Sequence &);
    void closeDecoder();
    bool collect();
    c2_status_t newFrame(const std::shared_ptr<C2BlockPool> &, const C2WorkOrdinalStruct &, Frame **);
    int decodeFrame(size_t, Frame *, m4v_output *);
    c2_status_t emit(void *, const std::unique_ptr<C2Work> &);
    c2_status_t drainFrames(const std::unique_ptr<C2Work> &);
    static void empty(const std::unique_ptr<C2Work> &w, c2_status_t status = C2_OK) {
        w->result = status; w->workletsProcessed = 1u;
        auto &o = w->worklets.front()->output;
        o.ordinal = w->input.ordinal; o.buffers.clear();
        o.flags = C2FrameData::flags_t(w->input.flags & C2FrameData::FLAG_END_OF_STREAM);
    }
    void fail(const std::unique_ptr<C2Work> &w, c2_status_t status, unsigned line = __builtin_LINE()) {
        ALOGE("decoder failure %d at line %u", status, line);
        for (auto &f : mFrames) if (f.pending && f.ordinal.frameIndex != w->input.ordinal.frameIndex)
            finish(f.ordinal.frameIndex.peekull(), [status](const std::unique_ptr<C2Work> &old) { empty(old, status); });
        closeDecoder(); mError = true; empty(w, status);
    }
};

c2_status_t C2SprdM4vDec::onInit() {
    if (mLibrary) return C2_OK;
    if (!SprdVspAcquireSession()) return C2_NO_MEMORY;
    mOwnsEngine = true;
    mLibrary = dlopen("libomx_m4vh263dec_hw_sprd.so", RTLD_NOW | RTLD_LOCAL);
    if (!mLibrary) { ALOGE("M4V library: %s", dlerror()); onRelease(); return C2_NOT_FOUND; }
#define RESOLVE(field, symbol) do { *reinterpret_cast<void **>(&field) = dlsym(mLibrary, symbol); \
    if (!field) { onRelease(); return C2_NOT_FOUND; } } while (0)
    RESOLVE(mInit, "MP4DecInit"); RESOLVE(mVolHeader, "MP4DecVolHeader");
    RESOLVE(mMemInit, "MP4DecMemInit"); RESOLVE(mDecode, "MP4DecDecode");
    RESOLVE(mRelease, "MP4DecRelease"); RESOLVE(mCapability, "MP4GetCodecCapability");
    RESOLVE(mDimensions, "Mp4GetBufferDimensions"); RESOLVE(mSetPic, "MP4DecSetCurRecPic");
    RESOLVE(mReleaseRefs, "MP4DecReleaseRefBuffers"); RESOLVE(mDrain, "MP4DecGetLastDspFrm");
#undef RESOLVE
    return C2_OK;
}
LEGACY c2_status_t C2SprdM4vDec::initDecoder(const Sequence &s) {
    if (!mLibrary) { auto err = onInit(); if (err != C2_OK) return err; }
    mFd = open("/dev/sprd_vsp", O_RDWR | O_CLOEXEC);
    if (mFd < 0) return C2_NO_INIT;
    mInternal.assign(kStream, 0);
    if (!allocate(mStream, kStream)) { closeDecoder(); return C2_NO_MEMORY; }
    mHandle = {nullptr, this, Bind, Unbind, Extra, 0};
    m4v_buffer internal{mInternal.data(), 0, kStream, 0, nullptr, 0};
    if (mInit(&mHandle, &internal)) { closeDecoder(); return C2_CORRUPTED; }
    mInitialized = true;
    int32_t maxw = 0, maxh = 0;
    if (mCapability(&mHandle, &maxw, &maxh) || int32_t(s.width) > maxw || int32_t(s.height) > maxh) {
        closeDecoder(); return C2_CANNOT_DO;
    }
    if (!mH263) {
        if (mCsd.empty() || mCsd.size() > kStream - 128) { closeDecoder(); return C2_CORRUPTED; }
        memcpy(mStream.cpu, mCsd.data(), mCsd.size());
        memset(mStream.cpu + mCsd.size(), 0, 128);
    }
    m4v_format format{}; format.standard = !mH263; format.width = s.width; format.height = s.height; format.yuv = 2;
    if (!mH263) { format.extra = mStream.cpu; format.extra_dma = mStream.dma; format.extra_size = mCsd.size(); }
    if (mVolHeader(&mHandle, &format) || format.width != int32_t(s.width) || format.height != int32_t(s.height)) {
        closeDecoder(); return C2_CORRUPTED;
    }
    mGeometry = s; mAppliedCsd = mCsd; mSeek = true;
    return C2_OK;
}
bool C2SprdM4vDec::collect() {
    for (auto &f : mFrames) {
        if (!f.block || f.refs || f.pending) continue;
        if (f.importId && ioctl(mFd, VSP_RELEASE_IMPORT, &f.importId)) { mError = true; return false; }
        f.view.reset(); f.block.reset(); f.importId = f.dma = 0;
    }
    return true;
}
LEGACY void C2SprdM4vDec::closeDecoder() {
    if (mInitialized) { mReleaseRefs(&mHandle); mRelease(&mHandle); }
    mInitialized = false;
    for (auto &f : mFrames) { f.refs = 0; f.pending = false; }
    collect();
    for (auto &f : mFrames) { f.view.reset(); f.block.reset(); f.importId = f.dma = 0; }
    mStream.clear(); mExtra.clear();
    if (mFd >= 0) close(mFd);
    mFd = -1; mHandle = {}; mGeometry.reset(); mAppliedCsd.clear(); mInternal.clear(); mSeek = true;
}
void C2SprdM4vDec::onRelease() {
    closeDecoder();
    if (mLibrary) dlclose(mLibrary);
    mLibrary = nullptr;
    if (mOwnsEngine) SprdVspReleaseSession();
    mOwnsEngine = false;
}
c2_status_t C2SprdM4vDec::newFrame(const std::shared_ptr<C2BlockPool> &pool,
                                const C2WorkOrdinalStruct &ordinal, Frame **out) {
    if (!collect() || !mGeometry) return C2_CORRUPTED;
    Frame *f = nullptr;
    for (auto &entry : mFrames) if (!entry.block) { f = &entry; break; }
    if (!f) return C2_NO_MEMORY;
    const auto stride = mGeometry->stride(), slice = mGeometry->slice();
    auto err = pool->fetchGraphicBlock(stride, slice, HAL_PIXEL_FORMAT_YCBCR_420_888,
        {C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE}, &f->block);
    if (err != C2_OK) return err;
    f->view = std::make_unique<C2GraphicView>(f->block->map().get());
    const auto &v = *f->view; const auto &l = v.layout();
    if (v.error() != C2_OK || l.type != C2PlanarLayout::TYPE_YUV || l.numPlanes != 3 ||
        l.planes[0].colInc != 1 || l.planes[1].colInc != 2 || l.planes[2].colInc != 2 ||
        l.planes[0].rowInc != int32_t(stride) || l.planes[1].rowInc != int32_t(stride) ||
        l.planes[2].rowInc != int32_t(stride) || v.data()[1] != v.data()[0] + stride * slice ||
        v.data()[2] != v.data()[1] + 1) return C2_CANNOT_DO;
    native_handle_t *native = UnwrapNativeCodec2GrallocHandle(f->block->handle());
    if (!native || native->numFds < 1) {
        if (native) native_handle_delete(native);
        return C2_BAD_VALUE;
    }
    sprd_vsp_import request{}; request.fd = native->data[0];
    int rc = ioctl(mFd, VSP_IMPORT_BUFFER, &request);
    native_handle_delete(native);
    if (rc) return C2_NO_MEMORY;
    f->importId = request.buffer_id; f->dma = request.dma_addr;
    if (request.size < stride * slice * 3 / 2) return C2_BAD_VALUE;
    sprd_vsp_sync sync{f->importId, 0};
    if (ioctl(mFd, VSP_SYNC_IMPORT, &sync)) return C2_CORRUPTED;
    f->ordinal = ordinal; f->geometry = *mGeometry; f->color = mIntf->color();
    f->pending = true; f->refs = 0; *out = f; return C2_OK;
}
LEGACY int C2SprdM4vDec::decodeFrame(size_t size, Frame *f, m4v_output *out) {
    for (unsigned retry = 0; retry < 3; ++retry) {
        mSetPic(&mHandle, f->view->data()[0], reinterpret_cast<uint8_t *>(uintptr_t(f->dma)), f);
        m4v_input in{}; in.stream = mStream.cpu; in.dma = mStream.dma; in.length = size;
        in.seek = mSeek; in.pts = f->ordinal.timestamp.peekll(); in.display = 1;
        *out = {};
        int rc = mDecode(&mHandle, &in, out);
        int32_t width = 0, height = 0; mDimensions(&mHandle, &width, &height);
        if (mError || !mGeometry || width != int32_t(mGeometry->stride()) ||
            height != int32_t(mGeometry->slice())) return -1;
        if (rc == -10) continue;
        return rc;
    }
    return -1;
}
c2_status_t C2SprdM4vDec::emit(void *header, const std::unique_ptr<C2Work> &current) {
    auto *f = checked(header);
    if (!f || !f->pending) return C2_CORRUPTED;
    sprd_vsp_sync sync{f->importId, 1};
    if (ioctl(mFd, VSP_SYNC_IMPORT, &sync)) return C2_CORRUPTED;
    auto buffer = createGraphicBuffer(f->block, C2Rect(f->geometry.width, f->geometry.height));
    buffer->setInfo(f->color);
    const auto ordinal = f->ordinal;
    auto fill = [buffer, ordinal](const std::unique_ptr<C2Work> &w) {
        w->result = C2_OK; w->workletsProcessed = 1u;
        auto &o = w->worklets.front()->output;
        o.ordinal = ordinal; o.flags = C2FrameData::flags_t(0); o.buffers = {buffer};
    };
    if (current && ordinal.frameIndex == current->input.ordinal.frameIndex) {
        if (current->input.flags & C2FrameData::FLAG_END_OF_STREAM)
            cloneAndSend(ordinal.frameIndex.peekull(), current, [fill](const std::unique_ptr<C2Work> &w) {
                fill(w); w->worklets.front()->output.flags = C2FrameData::FLAG_INCOMPLETE;
            });
        else fill(current);
    } else finish(ordinal.frameIndex.peekull(), fill);
    f->pending = false; return C2_OK;
}
LEGACY c2_status_t C2SprdM4vDec::drainFrames(const std::unique_ptr<C2Work> &current) {
    if (!mInitialized) return C2_OK;
    for (size_t i = 0; i < mFrames.size(); ++i) {
        void *header = nullptr;
        if (!mDrain(&mHandle, &header)) return collect() ? C2_OK : C2_CORRUPTED;
        auto err = emit(header, current);
        if (err != C2_OK) return err;
    }
    return C2_CORRUPTED;
}
void C2SprdM4vDec::process(const std::unique_ptr<C2Work> &work,
                          const std::shared_ptr<C2BlockPool> &pool) {
    work->result = C2_OK; work->workletsProcessed = 0;
    work->worklets.front()->output.buffers.clear();
    if (mError || mEos) { empty(work, C2_BAD_STATE); return; }
    std::vector<uint8_t> bytes;
    for (const auto &buffer : work->input.buffers) for (const auto &block : buffer->data().linearBlocks()) {
        C2ReadView view = block.map().get();
        if (view.error() != C2_OK || view.capacity() > kStream - 128 - bytes.size()) {
            fail(work, C2_BAD_VALUE); return;
        }
        bytes.insert(bytes.end(), view.data(), view.data() + view.capacity());
    }
    if (!bytes.empty()) {
        auto a = mH263 ? sprd::m4v::h263(bytes.data(), bytes.size())
                       : sprd::m4v::mpeg4(bytes.data(), bytes.size(), mVol);
        if (!a || (a->picture && (!a->coded || (work->input.flags & C2FrameData::FLAG_CODEC_CONFIG)))) {
            fail(work, C2_CANNOT_DO); return;
        }
        if (a->sequence) mVol = a->sequence;
        if (!a->config.empty()) mCsd = a->config;
        if (!a->picture) empty(work);
        else {
            const bool configure = !mGeometry || !(*mGeometry == *a->sequence) || mAppliedCsd != mCsd;
            if ((configure || mSeek) && !a->key) {
                empty(work);
            } else {
                if (configure) {
                    auto err = drainFrames(work);
                    if (err != C2_OK) { fail(work, err); return; }
                    closeDecoder();
                    err = initDecoder(*a->sequence);
                    if (err != C2_OK) { fail(work, err); return; }
                    C2StreamPictureSizeInfo::output size(0u, a->sequence->width, a->sequence->height);
                    std::vector<std::unique_ptr<C2SettingResult>> failures;
                    err = mIntf->config({&size}, C2_MAY_BLOCK, &failures);
                    if (err != C2_OK) { fail(work, err); return; }
                    work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(size));
                }
                Frame *frame = nullptr;
                auto err = newFrame(pool, work->input.ordinal, &frame);
                if (err != C2_OK) { fail(work, err); return; }
                memcpy(mStream.cpu, bytes.data(), bytes.size()); memset(mStream.cpu + bytes.size(), 0, 128);
                m4v_output output{};
                if (decodeFrame(bytes.size(), frame, &output) || output.errors || output.transposed || mHandle.error) {
                    fail(work, C2_CORRUPTED); return;
                }
                mSeek = false;
                if (output.effective) {
                    auto *display = checked(output.header);
                    if (!display || output.y != display->view->data()[0] || output.u != display->view->data()[1] ||
                        output.width != mGeometry->stride() || output.height != mGeometry->slice() ||
                        output.pts != display->ordinal.timestamp.peekll()) { fail(work, C2_CORRUPTED); return; }
                    err = emit(output.header, work);
                    if (err != C2_OK) { fail(work, err); return; }
                }
                if (!collect()) { fail(work, C2_CORRUPTED); return; }
            }
        }
    } else empty(work);
    if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
        auto err = drainFrames(work);
        if (err != C2_OK) { fail(work, err); return; }
        empty(work); mEos = true;
    }
}
c2_status_t C2SprdM4vDec::drain(uint32_t mode, const std::shared_ptr<C2BlockPool> &) {
    if (mode == NO_DRAIN) return C2_OK;
    if (mode == DRAIN_CHAIN) return C2_OMITTED;
    auto err = drainFrames(nullptr);
    if (mode == DRAIN_COMPONENT_WITH_EOS) mEos = true;
    return err;
}
class SprdM4vFactory : public C2ComponentFactory {
    const bool mH263;
    std::shared_ptr<C2ReflectorHelper> mHelper = std::static_pointer_cast<C2ReflectorHelper>(
        GetCodec2PlatformComponentStore()->getParamReflector());
public:
    explicit SprdM4vFactory(bool h263) : mH263(h263) {}
    c2_status_t createComponent(c2_node_id_t id, std::shared_ptr<C2Component> *out,
                                std::function<void(C2Component *)> deleter) override {
        *out = std::shared_ptr<C2Component>(new C2SprdM4vDec(id, std::make_shared<M4vInterface>(mHelper, mH263), mH263), deleter);
        return C2_OK;
    }
    c2_status_t createInterface(c2_node_id_t id, std::shared_ptr<C2ComponentInterface> *out,
                                std::function<void(C2ComponentInterface *)> deleter) override {
        *out = std::shared_ptr<C2ComponentInterface>(new SimpleInterface<M4vInterface>(componentName(mH263), id,
            std::make_shared<M4vInterface>(mHelper, mH263)), deleter);
        return C2_OK;
    }
};
} // namespace android
__attribute__((cfi_canonical_jump_table))
extern "C" C2ComponentFactory *CreateMpeg4Codec2Factory() { return new android::SprdM4vFactory(false); }
__attribute__((cfi_canonical_jump_table))
extern "C" void DestroyMpeg4Codec2Factory(C2ComponentFactory *f) { delete f; }
__attribute__((cfi_canonical_jump_table))
extern "C" C2ComponentFactory *CreateH263Codec2Factory() { return new android::SprdM4vFactory(true); }
__attribute__((cfi_canonical_jump_table))
extern "C" void DestroyH263Codec2Factory(C2ComponentFactory *f) { delete f; }


// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Deniz Kokden
#define LOG_TAG "C2SprdAvcDec"
#include <log/log.h>
#include <C2AllocatorGralloc.h>
#include <C2PlatformSupport.h>
#include <Codec2Mapper.h>
#include <media/stagefright/foundation/ColorUtils.h>
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
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include "AvcSyntax.h"
#include "SprdAvcAbi.h"
#include "SprdVspSession.h"
#include "include/linux/sprd_vsp.h"

namespace android {
namespace {
constexpr char kName[] = "c2.sprd.avc.decoder";
constexpr uint32_t kStream = 2 * 1024 * 1024;
constexpr size_t kSlots = 20;
// Opt-in diagnostics for startup stalls; the normal service does not set this.
struct AvcTiming {
    enum Part { INIT, FETCH, MAP, IMPORT, SYNC, DECODE, EMIT, COLLECT, COUNT };
    bool enabled = [] {
        const char *value = std::getenv("SPRD_AVC_TIMING");
        return value && !std::strcmp(value, "1");
    }();
    uint64_t work = 0, us[COUNT]{};
    static uint64_t now() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    struct Measure {
        AvcTiming &timing;
        Part part;
        uint64_t start;
        Measure(AvcTiming &t, Part p) : timing(t), part(p), start(t.enabled ? now() : 0) {}
        ~Measure() { if (timing.enabled) timing.us[part] += now() - start; }
    };
    struct Work {
        AvcTiming &timing;
        uint64_t start;
        explicit Work(AvcTiming &t) : timing(t), start(t.enabled ? now() : 0) {
            if (t.enabled) { ++t.work; std::memset(t.us, 0, sizeof(t.us)); }
        }
        ~Work() {
            if (!timing.enabled || (timing.work > 180 && timing.work % 60)) return;
            const auto &u = timing.us;
            ALOGI("timing work=%llu total=%llu init=%llu fetch=%llu map=%llu import=%llu sync=%llu decode=%llu emit=%llu collect=%llu us",
                (unsigned long long)timing.work, (unsigned long long)(now() - start),
                (unsigned long long)u[INIT], (unsigned long long)u[FETCH],
                (unsigned long long)u[MAP], (unsigned long long)u[IMPORT],
                (unsigned long long)u[SYNC], (unsigned long long)u[DECODE],
                (unsigned long long)u[EMIT], (unsigned long long)u[COLLECT]);
        }
    };
};
// Only the opaque legacy library dispatch bypasses indirect-call CFI. The
// component, callbacks and factory retain the normal platform CFI settings.
#define LEGACY __attribute__((no_sanitize("cfi-icall")))
struct Memory {
    uint8_t *cpu = nullptr;
    uint32_t dma = 0, size = 0;
    void clear() { if (cpu) munmap(cpu, size); cpu = nullptr; dma = size = 0; }
};
} // namespace

class AvcInterface : public SimpleInterface<void>::BaseParams {
public:
    explicit AvcInterface(const std::shared_ptr<C2ReflectorHelper> &helper)
        : BaseParams(helper, kName, C2Component::KIND_DECODER,
                     C2Component::DOMAIN_VIDEO, MEDIA_MIMETYPE_VIDEO_AVC) {
        noPrivateBuffers(); noInputReferences(); noInputLatency(); noTimeStretch();
        addParameter(DefineParam(mAttrib, C2_PARAMKEY_COMPONENT_ATTRIBUTES)
            .withConstValue(new C2ComponentAttributesSetting(C2Component::ATTRIB_IS_TEMPORAL)).build());
        addParameter(DefineParam(mActualOutputDelay, C2_PARAMKEY_OUTPUT_DELAY)
            .withDefault(new C2PortActualDelayTuning::output(4u))
            .withFields({C2F(mActualOutputDelay, value).inRange(0, 16)})
            .withSetter(Setter<decltype(*mActualOutputDelay)>::StrictValueWithNoDeps).build());
        addParameter(DefineParam(mMaxOutputReferenceAge, C2_PARAMKEY_OUTPUT_MAX_REFERENCE_AGE)
            .withConstValue(new C2StreamMaxReferenceAgeTuning::output(0u, 16u)).build());
        addParameter(DefineParam(mMaxOutputReferenceCount, C2_PARAMKEY_OUTPUT_MAX_REFERENCE_COUNT)
            .withConstValue(new C2StreamMaxReferenceCountTuning::output(0u, 16u)).build());
        addParameter(DefineParam(mSize, C2_PARAMKEY_PICTURE_SIZE)
            .withDefault(new C2StreamPictureSizeInfo::output(0u, 320, 240))
            .withFields({C2F(mSize, width).inRange(2, 1920, 2),
                         C2F(mSize, height).inRange(2, 1080, 2)})
            .withSetter(SizeSetter).build());
        addParameter(DefineParam(mMaxSize, C2_PARAMKEY_MAX_PICTURE_SIZE)
            .withConstValue(new C2StreamMaxPictureSizeTuning::output(0u, 1920, 1080)).build());
        addParameter(DefineParam(mMaxInputSize, C2_PARAMKEY_INPUT_MAX_BUFFER_SIZE)
            .withConstValue(new C2StreamMaxBufferSizeInfo::input(0u, kStream)).build());
        addParameter(DefineParam(mProfile, C2_PARAMKEY_PROFILE_LEVEL)
            .withDefault(new C2StreamProfileLevelInfo::input(0u, C2Config::PROFILE_AVC_HIGH,
                                                          C2Config::LEVEL_AVC_4_1))
            .withFields({C2F(mProfile, profile).oneOf({C2Config::PROFILE_AVC_BASELINE,
                C2Config::PROFILE_AVC_CONSTRAINED_BASELINE, C2Config::PROFILE_AVC_MAIN,
                C2Config::PROFILE_AVC_HIGH, C2Config::PROFILE_AVC_CONSTRAINED_HIGH,
                C2Config::PROFILE_AVC_PROGRESSIVE_HIGH}),
                C2F(mProfile, level).oneOf({C2Config::LEVEL_AVC_1, C2Config::LEVEL_AVC_1B,
                C2Config::LEVEL_AVC_1_1, C2Config::LEVEL_AVC_1_2, C2Config::LEVEL_AVC_1_3,
                C2Config::LEVEL_AVC_2, C2Config::LEVEL_AVC_2_1, C2Config::LEVEL_AVC_2_2,
                C2Config::LEVEL_AVC_3, C2Config::LEVEL_AVC_3_1, C2Config::LEVEL_AVC_3_2,
                C2Config::LEVEL_AVC_4, C2Config::LEVEL_AVC_4_1})})
            .withSetter(ProfileSetter).build());
        addParameter(DefineParam(mPixelFormat, C2_PARAMKEY_PIXEL_FORMAT)
            .withConstValue(new C2StreamPixelFormatInfo::output(0u, HAL_PIXEL_FORMAT_YCBCR_420_888))
            .build());
        addParameter(DefineParam(mColor, C2_PARAMKEY_DEFAULT_COLOR_ASPECTS)
            .withDefault(new C2StreamColorAspectsTuning::output(0u, C2Color::RANGE_LIMITED,
                C2Color::PRIMARIES_BT709, C2Color::TRANSFER_170M, C2Color::MATRIX_BT709))
            .withFields({C2F(mColor, range).any(), C2F(mColor, primaries).any(),
                C2F(mColor, transfer).any(), C2F(mColor, matrix).any()})
            .withSetter(DefaultColorSetter).build());
        addParameter(DefineParam(mCodedColor, C2_PARAMKEY_VUI_COLOR_ASPECTS)
            .withDefault(new C2StreamColorAspectsInfo::input(0u, C2Color::RANGE_UNSPECIFIED,
                C2Color::PRIMARIES_UNSPECIFIED, C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
            .withFields({C2F(mCodedColor, range).any(), C2F(mCodedColor, primaries).any(),
                C2F(mCodedColor, transfer).any(), C2F(mCodedColor, matrix).any()})
            .withSetter(CodedColorSetter).build());
        addParameter(DefineParam(mOutputColor, C2_PARAMKEY_COLOR_ASPECTS)
            .withDefault(new C2StreamColorAspectsInfo::output(0u, C2Color::RANGE_LIMITED,
                C2Color::PRIMARIES_BT709, C2Color::TRANSFER_170M, C2Color::MATRIX_BT709))
            .withFields({C2F(mOutputColor, range).any(), C2F(mOutputColor, primaries).any(),
                C2F(mOutputColor, transfer).any(), C2F(mOutputColor, matrix).any()})
            .withSetter(ColorSetter, mColor, mCodedColor).build());
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
    static C2R DefaultColorSetter(bool, C2P<C2StreamColorAspectsTuning::output> &) {
        return C2R::Ok();
    }
    static C2R CodedColorSetter(bool, C2P<C2StreamColorAspectsInfo::input> &) { return C2R::Ok(); }
    static C2R ColorSetter(bool, C2P<C2StreamColorAspectsInfo::output> &me,
                          const C2P<C2StreamColorAspectsTuning::output> &color,
                          const C2P<C2StreamColorAspectsInfo::input> &coded) {
        me.set().range = coded.v.range == C2Color::RANGE_UNSPECIFIED ? color.v.range : coded.v.range;
        me.set().primaries = coded.v.primaries == C2Color::PRIMARIES_UNSPECIFIED ? color.v.primaries : coded.v.primaries;
        me.set().transfer = coded.v.transfer == C2Color::TRANSFER_UNSPECIFIED ? color.v.transfer : coded.v.transfer;
        me.set().matrix = coded.v.matrix == C2Color::MATRIX_UNSPECIFIED ? color.v.matrix : coded.v.matrix;
        return C2R::Ok();
    }
    void updateColor(const sprd::Sequence &s) {
        ColorAspects aspects;
        ColorUtils::convertIsoColorAspectsToCodecAspects(s.primaries, s.transfer, s.matrix,
                                                       s.fullRange, aspects);
        C2StreamColorAspectsInfo::input coded{0u};
        if (s.videoSignal && !C2Mapper::map(aspects.mRange, &coded.range)) coded.range = C2Color::RANGE_UNSPECIFIED;
        if (!C2Mapper::map(aspects.mPrimaries, &coded.primaries)) coded.primaries = C2Color::PRIMARIES_UNSPECIFIED;
        if (!C2Mapper::map(aspects.mTransfer, &coded.transfer)) coded.transfer = C2Color::TRANSFER_UNSPECIFIED;
        if (!C2Mapper::map(aspects.mMatrixCoeffs, &coded.matrix)) coded.matrix = C2Color::MATRIX_UNSPECIFIED;
        std::vector<std::unique_ptr<C2SettingResult>> failures;
        config({&coded}, C2_MAY_BLOCK, &failures);
    }
    std::shared_ptr<C2StreamColorAspectsInfo::output> color() const { return mOutputColor; }
    c2_status_t updateDelay(uint32_t value, C2Worklet *worklet) {
        if (mActualOutputDelay->value == value) return C2_OK;
        C2PortActualDelayTuning::output delay(value);
        std::vector<std::unique_ptr<C2SettingResult>> failures;
        auto err = config({&delay}, C2_MAY_BLOCK, &failures);
        if (err == C2_OK) worklet->output.configUpdate.push_back(C2Param::Copy(delay));
        return err;
    }
private:
    std::shared_ptr<C2StreamPictureSizeInfo::output> mSize;
    std::shared_ptr<C2StreamMaxPictureSizeTuning::output> mMaxSize;
    std::shared_ptr<C2StreamMaxBufferSizeInfo::input> mMaxInputSize;
    std::shared_ptr<C2StreamProfileLevelInfo::input> mProfile;
    std::shared_ptr<C2StreamPixelFormatInfo::output> mPixelFormat;
    std::shared_ptr<C2StreamColorAspectsTuning::output> mColor;
    std::shared_ptr<C2StreamColorAspectsInfo::output> mOutputColor;
    std::shared_ptr<C2StreamColorAspectsInfo::input> mCodedColor;
};

class C2SprdAvcDec : public SimpleC2Component {
public:
    C2SprdAvcDec(c2_node_id_t id, const std::shared_ptr<AvcInterface> &intf)
        : SimpleC2Component(std::make_shared<SimpleInterface<AvcInterface>>(kName, id, intf)),
          mIntf(intf) {}
    ~C2SprdAvcDec() override { onRelease(); }
    c2_status_t onInit() override;
    c2_status_t onStop() override;
    void onReset() override { onStop(); mSps.clear(); mPps.clear(); }
    void onRelease() override;
    c2_status_t onFlush_sm() override { return onStop(); }
    void process(const std::unique_ptr<C2Work> &, const std::shared_ptr<C2BlockPool> &) override;
    c2_status_t drain(uint32_t, const std::shared_ptr<C2BlockPool> &) override;
private:
    struct Frame {
        std::shared_ptr<C2GraphicBlock> block;
        std::unique_ptr<C2GraphicView> view;
        uint32_t dma = 0, importId = 0;
        int refs = 0;
        bool pending = false;
        int32_t pictureId = 0;
        C2WorkOrdinalStruct ordinal;
        sprd::Sequence geometry;
        std::shared_ptr<C2StreamColorAspectsInfo::output> color;
    };
    std::shared_ptr<AvcInterface> mIntf;
    AvcTiming mTiming;
    void *mLibrary = nullptr;
    int mFd = -1;
    bool mInitialized = false, mOwnsEngine = false, mError = false, mEos = false, mSeek = true;
    int32_t mPictureId = 0;
    avc_handle mHandle{};
    Memory mStream, mExtra, mMbinfo, mScratch;
    uint32_t mMbStride = 0, mMbCount = 0;
    std::vector<uint8_t> mInternal;
    std::array<Frame, kSlots> mFrames;
    std::map<uint32_t, std::pair<sprd::Sequence, std::vector<uint8_t>>> mSps;
    std::map<uint32_t, std::pair<sprd::PictureParameter, std::vector<uint8_t>>> mPps;
    std::optional<sprd::Sequence> mGeometry;
    std::vector<uint8_t> mAppliedSps;
    std::map<uint32_t, std::vector<uint8_t>> mAppliedPps;
    int (*mInit)(avc_handle *, avc_buffer *, avc_format *) = nullptr;
    int (*mMemInit)(avc_handle *, avc_buffer *) = nullptr;
    int (*mDecode)(avc_handle *, avc_input *, avc_output *) = nullptr;
    int (*mRelease)(avc_handle *) = nullptr;
    int (*mInfo)(avc_handle *, avc_info *) = nullptr;
    void (*mSetPic)(avc_handle *, uint8_t *, uint8_t *, void *, int) = nullptr;
    int (*mDrain)(avc_handle *, void **, int32_t *, int64_t *) = nullptr;
    void (*mReleaseRefs)(avc_handle *) = nullptr;
    static int Bind(void *, void *);
    static int Unbind(void *, void *);
    static int Extra(void *, unsigned);
    static int Mbinfo(void *, unsigned, uint32_t *);
    Frame *checked(void *header);
    bool allocate(Memory &, uint32_t);
    LEGACY c2_status_t initDecoder();
    LEGACY void closeDecoder();
    LEGACY int decodeNal(const uint8_t *, size_t, Frame *, avc_output *);
    LEGACY c2_status_t drainFrames(const std::unique_ptr<C2Work> &);
    void collect();
    c2_status_t newFrame(const std::shared_ptr<C2BlockPool> &, const C2WorkOrdinalStruct &, Frame **);
    c2_status_t emit(void *, const std::unique_ptr<C2Work> &);
    static void empty(const std::unique_ptr<C2Work> &, c2_status_t = C2_OK);
    void fail(const std::unique_ptr<C2Work> &, c2_status_t, unsigned line = __builtin_LINE());
};

bool C2SprdAvcDec::allocate(Memory &m, uint32_t size) {
    if (m.cpu) return m.size >= size;
    sprd_vsp_buffer b{}; b.size = size;
    if (ioctl(mFd, VSP_ALLOC_BUFFER, &b)) return false;
    void *cpu = mmap(nullptr, b.size, PROT_READ | PROT_WRITE, MAP_SHARED, mFd, b.mmap_offset);
    if (cpu == MAP_FAILED) return false;
    m = {static_cast<uint8_t *>(cpu), b.dma_addr, b.size};
    return true;
}
C2SprdAvcDec::Frame *C2SprdAvcDec::checked(void *header) {
    for (auto &frame : mFrames) if (&frame == header) return &frame;
    return nullptr;
}
int C2SprdAvcDec::Bind(void *self, void *header) {
    auto *p = static_cast<C2SprdAvcDec *>(self);
    auto *f = p->checked(header);
    if (!f || !f->block || f->refs >= 32) { p->mError = true; return -1; }
    ++f->refs; return 0;
}
int C2SprdAvcDec::Unbind(void *self, void *header) {
    auto *p = static_cast<C2SprdAvcDec *>(self);
    auto *f = p->checked(header);
    if (!f || f->refs <= 0) { p->mError = true; return -1; }
    --f->refs; return 0;
}
LEGACY int C2SprdAvcDec::Extra(void *self, unsigned size) {
    auto *p = static_cast<C2SprdAvcDec *>(self);
    if (!size || size > 16 * 1024 * 1024 || !p->allocate(p->mExtra, size)) return -1;
    p->mMbCount = 0;
    memset(p->mExtra.cpu, 0, p->mExtra.size);
    avc_buffer types[4]{};
    types[1] = {p->mExtra.cpu, p->mExtra.dma, p->mExtra.size, 0, nullptr, 0};
    return p->mMemInit(&p->mHandle, types);
}
int C2SprdAvcDec::Mbinfo(void *self, unsigned size, uint32_t *dma) {
    auto *p = static_cast<C2SprdAvcDec *>(self);
    if (!dma || !size || p->mMbCount >= kSlots) return -1;
    uint32_t stride = (size + 4095u) & ~4095u;
    if (size > kStream || stride > (16 * 1024 * 1024) / kSlots) return -1;
    if (!p->mMbStride) p->mMbStride = stride;
    if (stride > p->mMbStride || !p->allocate(p->mMbinfo, p->mMbStride * kSlots)) return -1;
    *dma = p->mMbinfo.dma + p->mMbCount++ * p->mMbStride;
    return 0;
}
c2_status_t C2SprdAvcDec::onInit() {
    if (mLibrary) return C2_OK;
    if (!SprdVspAcquireSession()) return C2_NO_MEMORY;
    mOwnsEngine = true;
    mLibrary = dlopen("libomx_avcdec_hw_sprd.so", RTLD_NOW | RTLD_LOCAL);
    if (!mLibrary) { ALOGE("AVC library: %s", dlerror()); onRelease(); return C2_NOT_FOUND; }
#define RESOLVE(field, symbol) do { *reinterpret_cast<void **>(&field) = dlsym(mLibrary, symbol); \
    if (!field) { onRelease(); return C2_NOT_FOUND; } } while (0)
    RESOLVE(mInit, "H264DecInit"); RESOLVE(mMemInit, "H264DecMemInit");
    RESOLVE(mDecode, "H264DecDecode"); RESOLVE(mRelease, "H264DecRelease");
    RESOLVE(mInfo, "H264DecGetInfo"); RESOLVE(mSetPic, "H264Dec_SetCurRecPic");
    RESOLVE(mDrain, "H264Dec_GetLastDspFrm"); RESOLVE(mReleaseRefs, "H264Dec_ReleaseRefBuffers");
#undef RESOLVE
    mError = mEos = false;
    return C2_OK;
}
LEGACY c2_status_t C2SprdAvcDec::initDecoder() {
    if (mInitialized) return C2_OK;
    if (!mLibrary) { auto err = onInit(); if (err != C2_OK) return err; }
    mFd = open("/dev/sprd_vsp", O_RDWR | O_CLOEXEC);
    if (mFd < 0) return C2_NO_INIT;
    mInternal.assign(kStream, 0);
    mHandle = {nullptr, this, Bind, Unbind, Extra, Mbinfo};
    avc_buffer internal{mInternal.data(), 0, kStream, 0, nullptr, 0};
    avc_format format{}; format.standard = 4; format.yuv = 2;
    if (mInit(&mHandle, &internal, &format)) { closeDecoder(); return C2_CORRUPTED; }
    mInitialized = true; mSeek = true;
    if (!allocate(mStream, kStream) || !allocate(mScratch, 1920 * 1088 * 3 / 2)) {
        closeDecoder(); return C2_NO_MEMORY;
    }
    return C2_OK;
}
void C2SprdAvcDec::collect() {
    AvcTiming::Measure timer(mTiming, AvcTiming::COLLECT);
    for (auto &f : mFrames) {
        if (!f.block || f.refs || f.pending) continue;
        if (f.importId && ioctl(mFd, VSP_RELEASE_IMPORT, &f.importId)) {
            ALOGE("release import: %s", strerror(errno)); mError = true; continue;
        }
        f.view.reset(); f.block.reset(); f.importId = 0; f.dma = 0;
    }
}
LEGACY void C2SprdAvcDec::closeDecoder() {
    if (mInitialized) { mReleaseRefs(&mHandle); mRelease(&mHandle); }
    mInitialized = false;
    for (auto &f : mFrames) { f.refs = 0; f.pending = false; }
    collect();
    // Closing our allocator also reclaims every coherent allocation after unmap.
    mStream.clear(); mExtra.clear(); mMbinfo.clear(); mScratch.clear();
    for (auto &f : mFrames) { f.view.reset(); f.block.reset(); f.importId = 0; }
    if (mFd >= 0) close(mFd);
    mFd = -1; mHandle = {}; mInternal.clear(); mMbStride = mMbCount = 0;
    mAppliedSps.clear(); mAppliedPps.clear();
}
c2_status_t C2SprdAvcDec::onStop() {
    closeDecoder(); mGeometry.reset(); mError = mEos = false; return C2_OK;
}
void C2SprdAvcDec::onRelease() {
    closeDecoder();
    if (mLibrary) dlclose(mLibrary);
    mLibrary = nullptr;
    if (mOwnsEngine) SprdVspReleaseSession();
    mOwnsEngine = false;
}
c2_status_t C2SprdAvcDec::newFrame(const std::shared_ptr<C2BlockPool> &pool,
                                const C2WorkOrdinalStruct &ordinal, Frame **result) {
    collect();
    if (mError || !mGeometry) return C2_CORRUPTED;
    Frame *f = nullptr;
    for (auto &entry : mFrames) if (!entry.block) { f = &entry; break; }
    if (!f) return C2_NO_MEMORY;
    const auto &s = *mGeometry;
    C2MemoryUsage usage{C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE};
    c2_status_t err;
    {
        AvcTiming::Measure timer(mTiming, AvcTiming::FETCH);
        err = pool->fetchGraphicBlock(s.codedWidth, s.codedHeight,
                                     HAL_PIXEL_FORMAT_YCBCR_420_888, usage, &f->block);
    }
    if (err != C2_OK) return err;
    {
        AvcTiming::Measure timer(mTiming, AvcTiming::MAP);
        f->view = std::make_unique<C2GraphicView>(f->block->map().get());
    }
    const auto &v = *f->view;
    const auto &layout = v.layout();
    if (v.error() != C2_OK || layout.type != C2PlanarLayout::TYPE_YUV || layout.numPlanes != 3 ||
        layout.planes[0].colInc != 1 || layout.planes[1].colInc != 2 ||
        layout.planes[2].colInc != 2 || layout.planes[0].rowInc != int32_t(s.codedWidth) ||
        layout.planes[1].rowInc != int32_t(s.codedWidth) ||
        layout.planes[2].rowInc != int32_t(s.codedWidth) ||
        v.data()[1] != v.data()[0] + s.codedWidth * s.codedHeight ||
        v.data()[2] != v.data()[1] + 1) return C2_CANNOT_DO;
    native_handle_t *native = UnwrapNativeCodec2GrallocHandle(f->block->handle());
    if (!native || native->numFds < 1) {
        if (native) native_handle_delete(native);
        return C2_BAD_VALUE;
    }
    sprd_vsp_import request{}; request.fd = native->data[0];
    int ret;
    {
        AvcTiming::Measure timer(mTiming, AvcTiming::IMPORT);
        ret = ioctl(mFd, VSP_IMPORT_BUFFER, &request);
    }
    native_handle_delete(native); // Non-owning wrapper: never close its FDs.
    if (ret) return C2_NO_MEMORY;
    f->importId = request.buffer_id; f->dma = request.dma_addr;
    if (request.size < s.codedWidth * s.codedHeight * 3 / 2) return C2_BAD_VALUE;
    // The mapper's persistent CPU view must not retain dirty cache lines across
    // a device write on this non-coherent SoC. Transfer ownership before DMA.
    sprd_vsp_sync sync{f->importId, 0};
    {
        AvcTiming::Measure timer(mTiming, AvcTiming::SYNC);
        if (ioctl(mFd, VSP_SYNC_IMPORT, &sync)) return C2_CORRUPTED;
    }
    f->ordinal = ordinal; f->geometry = s; f->color = mIntf->color(); f->pending = true; f->refs = 0;
    if (mPictureId == INT32_MAX) mPictureId = 0;
    f->pictureId = ++mPictureId;
    *result = f;
    return C2_OK;
}
LEGACY int C2SprdAvcDec::decodeNal(const uint8_t *nal, size_t size, Frame *f, avc_output *output) {
    if (size > kStream - 4) return -1;
    memcpy(mStream.cpu, "\0\0\0\1", 4); memcpy(mStream.cpu + 4, nal, size);
    uint32_t remaining = size + 4;
    mSetPic(&mHandle, f ? f->view->data()[0] : mScratch.cpu,
            reinterpret_cast<uint8_t *>(uintptr_t(f ? f->dma : mScratch.dma)),
            f, f ? f->pictureId : 0);
    // One NAL per call makes geometry validation precede every DMA-writing slice.
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        avc_input input{};
        input.stream = mStream.cpu; input.dma = mStream.dma; input.length = remaining;
        input.seek_idr = mSeek; input.display = f != nullptr;
        input.pts = f ? f->ordinal.timestamp.peekll() : 0;
        *output = {};
        int ret;
        {
            AvcTiming::Measure timer(mTiming, AvcTiming::DECODE);
            ret = mDecode(&mHandle, &input, output);
        }
        if (ret == -10 && !input.length) continue; // extra-memory retry
        if (ret && ret != -9) return ret;
        if (!input.length || input.length > remaining) return -1;
        if (!ret && f && (nal[0] & 31) == 5) mSeek = false;
        if (input.length != remaining) return -1;
        avc_info info{};
        if (!mInfo(&mHandle, &info) && mGeometry &&
            (info.width > mGeometry->codedWidth || info.height > mGeometry->codedHeight)) return -1;
        return mError ? -1 : 0;
    }
    return -1;
}
void C2SprdAvcDec::empty(const std::unique_ptr<C2Work> &work, c2_status_t status) {
    if (!work) return;
    work->result = status; work->workletsProcessed = 1u;
    auto &out = work->worklets.front()->output;
    out.flags = C2FrameData::flags_t(work->input.flags & C2FrameData::FLAG_END_OF_STREAM);
    out.ordinal = work->input.ordinal; out.buffers.clear();
}
void C2SprdAvcDec::fail(const std::unique_ptr<C2Work> &work, c2_status_t status, unsigned line) {
    ALOGE("decoder failure %d at line %u", status, line);
    // Release all outstanding work deterministically on a stream failure.
    for (auto &f : mFrames) if (f.pending && f.ordinal.frameIndex != work->input.ordinal.frameIndex) {
        finish(f.ordinal.frameIndex.peekull(), [status](const std::unique_ptr<C2Work> &w) {
            empty(w, status);
        });
    }
    closeDecoder(); mError = true; empty(work, status);
}
c2_status_t C2SprdAvcDec::emit(void *header, const std::unique_ptr<C2Work> &current) {
    AvcTiming::Measure timer(mTiming, AvcTiming::EMIT);
    auto *f = checked(header);
    if (!f || !f->pending || !f->block) return C2_CORRUPTED;
    sprd_vsp_sync sync{f->importId, 1};
    if (ioctl(mFd, VSP_SYNC_IMPORT, &sync)) return C2_CORRUPTED;
    auto buffer = createGraphicBuffer(f->block,
        C2Rect(f->geometry.width, f->geometry.height).at(f->geometry.left, f->geometry.top));
    buffer->setInfo(f->color);
    const auto ordinal = f->ordinal;
    auto fill = [buffer, ordinal](const std::unique_ptr<C2Work> &w) {
        w->result = C2_OK; w->workletsProcessed = 1u;
        auto &o = w->worklets.front()->output;
        o.ordinal = ordinal; o.flags = C2FrameData::flags_t(0); o.buffers = {buffer};
    };
    if (current && ordinal.frameIndex == current->input.ordinal.frameIndex) {
        if (current->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
            cloneAndSend(ordinal.frameIndex.peekull(), current,
                [fill](const std::unique_ptr<C2Work> &w) {
                    fill(w); w->worklets.front()->output.flags = C2FrameData::FLAG_INCOMPLETE;
                });
        } else fill(current);
    } else finish(ordinal.frameIndex.peekull(), fill);
    f->pending = false;
    return C2_OK;
}
LEGACY c2_status_t C2SprdAvcDec::drainFrames(const std::unique_ptr<C2Work> &current) {
    for (size_t i = 0; i < kSlots; ++i) {
        void *header = nullptr; int32_t id = 0; int64_t pts = 0;
        if (mDrain(&mHandle, &header, &id, &pts)) { collect(); return C2_OK; }
        auto err = emit(header, current);
        if (err != C2_OK) return err;
    }
    return C2_CORRUPTED;
}
void C2SprdAvcDec::process(const std::unique_ptr<C2Work> &work,
                         const std::shared_ptr<C2BlockPool> &pool) {
    AvcTiming::Work timing(mTiming);
    work->result = C2_OK; work->workletsProcessed = 0;
    work->worklets.front()->output.buffers.clear();
    if (mError || mEos) { empty(work, C2_BAD_STATE); return; }
    c2_status_t err;
    {
        AvcTiming::Measure timer(mTiming, AvcTiming::INIT);
        err = initDecoder();
    }
    if (err != C2_OK) { fail(work, err); return; }
    std::vector<uint8_t> bytes;
    for (const auto &buffer : work->input.buffers) {
        for (const auto &block : buffer->data().linearBlocks()) {
            C2ReadView view = block.map().get();
            if (view.error() != C2_OK || view.capacity() > kStream - bytes.size()) {
                fail(work, C2_BAD_VALUE); return;
            }
            bytes.insert(bytes.end(), view.data(), view.data() + view.capacity());
        }
    }
    auto nals = bytes.empty() ? std::optional<std::vector<sprd::Nal>>(std::vector<sprd::Nal>{})
                             : sprd::splitAnnexB(bytes.data(), bytes.size());
    if (!nals) { fail(work, C2_CORRUPTED); return; }
    // Validate the entire access unit before submitting any slice to hardware.
    bool vcl = false, idr = false;
    std::optional<sprd::Sequence> active;
    for (const auto &n : *nals) {
        const uint8_t *p = bytes.data() + n.offset;
        if (vcl && (n.type == 7 || n.type == 8)) {
            fail(work, C2_CORRUPTED); return;
        }
        if (n.type == 7) {
            auto s = sprd::sequence(p, n.size);
            if (!s) { fail(work, C2_CANNOT_DO); return; }
            ALOGV("SPS id=%u size=%ux%u bytes=%zu", s->id, s->width, s->height, n.size);
            mSps[s->id] = {*s, std::vector<uint8_t>(p, p + n.size)};
        } else if (n.type == 8) {
            auto param = sprd::pictureParameter(p, n.size);
            if (!param) { fail(work, C2_CANNOT_DO); return; }
            ALOGV("PPS id=%u sequence=%u bytes=%zu", param->id, param->sequenceId, n.size);
            mPps[param->id] = {*param, std::vector<uint8_t>(p, p + n.size)};
        } else if (n.type == 1 || n.type == 5) {
            auto id = sprd::sliceParameterId(p, n.size);
            if (!id || !mPps.count(*id) || !mSps.count(mPps[*id].first.sequenceId)) {
                ALOGE("slice parameters pps=%u pps_count=%zu sps_count=%zu type=%u bytes=%zu",
                    id.value_or(UINT32_MAX), mPps.size(), mSps.size(), n.type, n.size);
                fail(work, C2_CORRUPTED); return;
            }
            auto s = mSps[mPps[*id].first.sequenceId].first;
            if (active && !active->sameGeometry(s)) { fail(work, C2_CANNOT_DO); return; }
            active = s; vcl = true; idr |= n.type == 5;
        } else if (n.type != 6 && n.type != 9 && n.type != 10 && n.type != 11 && n.type != 12) {
            fail(work, C2_CANNOT_DO); return;
        }
    }
    // Grow the pipeline while processing CSD, before a high-DPB sequence can
    // withhold its first output. Include all cached SPSs, since a later slice may
    // select any of them. The framework retains a previously grown buffer count.
    if (!mSps.empty()) {
        uint32_t delay = 0;
        for (const auto &[id, entry] : mSps) {
            (void)id;
            delay = std::max(delay, entry.first.outputDelay);
        }
        err = mIntf->updateDelay(delay, work->worklets.front().get());
        if (err != C2_OK) { fail(work, err); return; }
    }
    // Parameter-only work is cached, not submitted with an incorrectly sized picture.
    if (!vcl) empty(work);
    Frame *frame = nullptr;
    if (vcl) {
        const auto &sps = mSps[active->id].second;
        bool configure = !mGeometry || !mGeometry->sameGeometry(*active) || mAppliedSps != sps;
        if (configure && !idr) {
            // A new sequence must start at a random-access picture. Never retain
            // work that the library would silently skip while seeking an IDR.
            empty(work);
            if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
                if (mGeometry) drainFrames(work);
                mEos = true;
            }
            return;
        }
        if (configure) {
            if (mGeometry) {
                err = drainFrames(work);
                if (err != C2_OK) { fail(work, err); return; }
                closeDecoder();
                err = initDecoder();
                if (err != C2_OK) { fail(work, err); return; }
            }
            mGeometry = active;
            mIntf->updateColor(*active);
            C2StreamPictureSizeInfo::output size(0u, active->width, active->height);
            std::vector<std::unique_ptr<C2SettingResult>> failures;
            err = mIntf->config({&size}, C2_MAY_BLOCK, &failures);
            if (err != C2_OK) { fail(work, err); return; }
            work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(size));
            // Replay cached SPS/PPS after flush or a resolution change.
            for (const auto &[id, entry] : mSps) {
                if (id != active->id) continue;
                avc_output output{};
                if (decodeNal(entry.second.data(), entry.second.size(), nullptr, &output)) {
                    fail(work, C2_CORRUPTED); return;
                }
                mAppliedSps = entry.second;
            }
        }
        // A PPS may change without changing picture geometry. Submit it before
        // its first slice, even when no decoder restart is needed.
        for (const auto &[id, entry] : mPps) {
                (void)id;
                if (entry.first.sequenceId != active->id || mAppliedPps[id] == entry.second) continue;
                avc_output output{};
                if (decodeNal(entry.second.data(), entry.second.size(), nullptr, &output)) {
                    fail(work, C2_CORRUPTED); return;
                }
                mAppliedPps[id] = entry.second;
        }
        err = newFrame(pool, work->input.ordinal, &frame);
        if (err != C2_OK) { fail(work, err); return; }
        for (const auto &n : *nals) {
            // CSD is replayed explicitly above; only the active SPS is accepted.
            if (n.type == 7 || n.type == 8 || n.type == 9 || n.type == 12) continue;
            avc_output output{};
            if (decodeNal(bytes.data() + n.offset, n.size, frame, &output)) {
                fail(work, C2_CORRUPTED); return;
            }
            if (output.effective) {
                err = emit(output.header, work);
                if (err != C2_OK) { fail(work, err); return; }
            }
        }
        collect();
    }
    if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
        err = drainFrames(work);
        if (err != C2_OK) { fail(work, err); return; }
        empty(work); mEos = true;
    }
}
c2_status_t C2SprdAvcDec::drain(uint32_t mode, const std::shared_ptr<C2BlockPool> &) {
    if (mode == NO_DRAIN) return C2_OK;
    if (mode == DRAIN_CHAIN) return C2_OMITTED;
    if (!mInitialized) return C2_OK;
    auto err = drainFrames(nullptr);
    if (mode == DRAIN_COMPONENT_WITH_EOS) mEos = true;
    return err;
}

class SprdAvcFactory : public C2ComponentFactory {
    std::shared_ptr<C2ReflectorHelper> mHelper = std::static_pointer_cast<C2ReflectorHelper>(
        GetCodec2PlatformComponentStore()->getParamReflector());
public:
    c2_status_t createComponent(c2_node_id_t id, std::shared_ptr<C2Component> *out,
                                std::function<void(C2Component *)> deleter) override {
        *out = std::shared_ptr<C2Component>(new C2SprdAvcDec(id, std::make_shared<AvcInterface>(mHelper)), deleter);
        return C2_OK;
    }
    c2_status_t createInterface(c2_node_id_t id, std::shared_ptr<C2ComponentInterface> *out,
                               std::function<void(C2ComponentInterface *)> deleter) override {
        *out = std::shared_ptr<C2ComponentInterface>(
            new SimpleInterface<AvcInterface>(kName, id, std::make_shared<AvcInterface>(mHelper)), deleter);
        return C2_OK;
    }
};
} // namespace android
__attribute__((cfi_canonical_jump_table))
extern "C" C2ComponentFactory *CreateCodec2Factory() { return new android::SprdAvcFactory; }
__attribute__((cfi_canonical_jump_table))
extern "C" void DestroyCodec2Factory(C2ComponentFactory *factory) { delete factory; }

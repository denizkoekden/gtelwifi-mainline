// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Deniz Kokden
#define LOG_TAG "C2SprdVp8Dec"
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
#include "SprdVp8Abi.h"
#include "SprdVspSession.h"
#include "include/linux/sprd_vsp.h"

namespace android {
namespace {
constexpr char kName[] = "c2.sprd.vp8.decoder";
constexpr uint32_t kStream = 2 * 1024 * 1024;
#define LEGACY __attribute__((no_sanitize("cfi-icall")))
struct Memory {
    uint8_t *cpu = nullptr;
    uint32_t dma = 0, size = 0;
    void clear() { if (cpu) munmap(cpu, size); cpu = nullptr; dma = size = 0; }
};
struct Geometry {
    uint32_t width, height;
    uint32_t stride() const { return (width + 15) & ~15U; }
    uint32_t slice() const { return (height + 15) & ~15U; }
    bool operator==(const Geometry &o) const { return width == o.width && height == o.height; }
};
uint32_t le16(const uint8_t *p) { return p[0] | uint32_t(p[1]) << 8; }
} // namespace

class Vp8Interface : public SimpleInterface<void>::BaseParams {
public:
    explicit Vp8Interface(const std::shared_ptr<C2ReflectorHelper> &helper)
        : BaseParams(helper, kName, C2Component::KIND_DECODER,
                     C2Component::DOMAIN_VIDEO, MEDIA_MIMETYPE_VIDEO_VP8) {
        noPrivateBuffers(); noInputReferences(); noInputLatency(); noTimeStretch();
        addParameter(DefineParam(mAttrib, C2_PARAMKEY_COMPONENT_ATTRIBUTES)
            .withConstValue(new C2ComponentAttributesSetting(C2Component::ATTRIB_IS_TEMPORAL)).build());
        addParameter(DefineParam(mDelay, C2_PARAMKEY_OUTPUT_DELAY)
            .withConstValue(new C2PortActualDelayTuning::output(4u)).build());
        addParameter(DefineParam(mReferences, C2_PARAMKEY_OUTPUT_MAX_REFERENCE_COUNT)
            .withConstValue(new C2StreamMaxReferenceCountTuning::output(0u, 3u)).build());
        addParameter(DefineParam(mSize, C2_PARAMKEY_PICTURE_SIZE)
            .withDefault(new C2StreamPictureSizeInfo::output(0u, 320, 240))
            .withFields({C2F(mSize, width).inRange(2, 1920, 2),
                         C2F(mSize, height).inRange(2, 1080, 2)})
            .withSetter(SizeSetter).build());
        addParameter(DefineParam(mMaxSize, C2_PARAMKEY_MAX_PICTURE_SIZE)
            .withConstValue(new C2StreamMaxPictureSizeTuning::output(0u, 1920, 1080)).build());
        addParameter(DefineParam(mInputSize, C2_PARAMKEY_INPUT_MAX_BUFFER_SIZE)
            .withConstValue(new C2StreamMaxBufferSizeInfo::input(0u, kStream - 128)).build());
        addParameter(DefineParam(mProfile, C2_PARAMKEY_PROFILE_LEVEL)
            .withDefault(new C2StreamProfileLevelInfo::input(0u, C2Config::PROFILE_VP8_0,
                                                          C2Config::LEVEL_UNUSED))
            .withFields({C2F(mProfile, profile).equalTo(C2Config::PROFILE_VP8_0),
                         C2F(mProfile, level).equalTo(C2Config::LEVEL_UNUSED)})
            .withSetter(ProfileSetter).build());
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

class C2SprdVp8Dec : public SimpleC2Component {
public:
    C2SprdVp8Dec(c2_node_id_t id, const std::shared_ptr<Vp8Interface> &intf)
        : SimpleC2Component(std::make_shared<SimpleInterface<Vp8Interface>>(kName, id, intf)), mIntf(intf) {}
    ~C2SprdVp8Dec() override { onRelease(); }
    c2_status_t onInit() override;
    c2_status_t onStop() override { closeDecoder(); mError = mEos = false; return C2_OK; }
    void onReset() override { onStop(); }
    void onRelease() override;
    c2_status_t onFlush_sm() override { return onStop(); }
    void process(const std::unique_ptr<C2Work> &, const std::shared_ptr<C2BlockPool> &) override;
    c2_status_t drain(uint32_t mode, const std::shared_ptr<C2BlockPool> &) override {
        if (mode == DRAIN_CHAIN) return C2_OMITTED;
        // VP8 has no reordered presentation frames. Hidden reference frames
        // never turn into delayed output on EOS.
        if (mode == DRAIN_COMPONENT_WITH_EOS) mEos = true;
        return C2_OK;
    }
private:
    struct Frame {
        std::shared_ptr<C2GraphicBlock> block;
        std::unique_ptr<C2GraphicView> view;
        uint32_t importId = 0, dma = 0;
        unsigned refs = 0;
    };
    std::shared_ptr<Vp8Interface> mIntf;
    std::array<Frame, 12> mFrames;
    std::optional<Geometry> mGeometry;
    void *mLibrary = nullptr;
    int mFd = -1;
    bool mOwnsEngine = false, mInitialized = false, mError = false, mEos = false;
    vp8_handle mHandle{};
    std::vector<uint8_t> mInternal;
    Memory mStream, mExtra;
    int (*mInit)(vp8_handle *, vp8_buffer *, vp8_buffer *, vp8_format *) = nullptr;
    int (*mDecode)(vp8_handle *, vp8_input *, vp8_output *) = nullptr;
    int (*mRelease)(vp8_handle *) = nullptr;
    int (*mCapability)(vp8_handle *, int32_t *, int32_t *) = nullptr;
    void (*mDimensions)(vp8_handle *, int32_t *, int32_t *) = nullptr;
    void (*mSetPic)(vp8_handle *, uint8_t *, uint8_t *, void *) = nullptr;
    void (*mReleaseRefs)(vp8_handle *) = nullptr;
    Frame *checked(void *p) {
        for (auto &f : mFrames) if (p == &f && f.block) return &f;
        mError = true; return nullptr;
    }
    static int Bind(void *self, void *header, int) {
        auto *p = static_cast<C2SprdVp8Dec *>(self); auto *f = p->checked(header);
        if (!f || f->refs >= 16) { p->mError = true; return -1; }
        ++f->refs; return 0;
    }
    static int Unbind(void *self, void *header, int) {
        auto *p = static_cast<C2SprdVp8Dec *>(self); auto *f = p->checked(header);
        if (!f || !f->refs) { p->mError = true; return -1; }
        --f->refs; return 0;
    }
    bool allocate(Memory &m, uint32_t size) {
        sprd_vsp_buffer b{}; b.size = size;
        if (ioctl(mFd, VSP_ALLOC_BUFFER, &b)) return false;
        void *cpu = mmap(nullptr, b.size, PROT_READ | PROT_WRITE, MAP_SHARED, mFd, b.mmap_offset);
        if (cpu == MAP_FAILED) return false;
        m = {static_cast<uint8_t *>(cpu), b.dma_addr, b.size}; return true;
    }
    c2_status_t initDecoder();
    void closeDecoder();
    bool collect();
    c2_status_t newFrame(const std::shared_ptr<C2BlockPool> &, Frame **);
    int decodeFrame(size_t, Frame *, vp8_output *);
    static void empty(const std::unique_ptr<C2Work> &work, c2_status_t status = C2_OK) {
        work->result = status; work->workletsProcessed = 1u;
        auto &out = work->worklets.front()->output;
        out.ordinal = work->input.ordinal; out.buffers.clear();
        out.flags = C2FrameData::flags_t(work->input.flags & C2FrameData::FLAG_END_OF_STREAM);
    }
    void fail(const std::unique_ptr<C2Work> &w, c2_status_t status) {
        closeDecoder(); mError = true; empty(w, status);
    }
};

c2_status_t C2SprdVp8Dec::onInit() {
    if (mLibrary) return C2_OK;
    if (!SprdVspAcquireSession()) return C2_NO_MEMORY;
    mOwnsEngine = true;
    mLibrary = dlopen("libomx_vpxdec_hw_sprd.so", RTLD_NOW | RTLD_LOCAL);
    if (!mLibrary) { ALOGE("VP8 library: %s", dlerror()); onRelease(); return C2_NOT_FOUND; }
#define RESOLVE(field, symbol) do { *reinterpret_cast<void **>(&field) = dlsym(mLibrary, symbol); \
    if (!field) { onRelease(); return C2_NOT_FOUND; } } while (0)
    RESOLVE(mInit, "VP8DecInit"); RESOLVE(mDecode, "VP8DecDecode");
    RESOLVE(mRelease, "VP8DecRelease"); RESOLVE(mCapability, "VP8GetCodecCapability");
    RESOLVE(mDimensions, "VP8GetBufferDimensions"); RESOLVE(mSetPic, "VP8DecSetCurRecPic");
    RESOLVE(mReleaseRefs, "VP8DecReleaseRefBuffers");
#undef RESOLVE
    return C2_OK;
}
LEGACY c2_status_t C2SprdVp8Dec::initDecoder() {
    if (mInitialized) return C2_OK;
    if (!mLibrary) { auto err = onInit(); if (err != C2_OK) return err; }
    mFd = open("/dev/sprd_vsp", O_RDWR | O_CLOEXEC);
    if (mFd < 0) return C2_NO_INIT;
    mInternal.assign(kStream, 0);
    if (!allocate(mExtra, 1024 * 1024) || !allocate(mStream, kStream)) {
        closeDecoder(); return C2_NO_MEMORY;
    }
    mHandle = {nullptr, this, Bind, Unbind};
    vp8_buffer internal{mInternal.data(), 0, kStream, 0, nullptr, 0};
    vp8_buffer extra{mExtra.cpu, mExtra.dma, mExtra.size, 0, nullptr, 0};
    vp8_format format{}; format.yuv = 2;
    if (mInit(&mHandle, &internal, &extra, &format)) { closeDecoder(); return C2_CORRUPTED; }
    mInitialized = true;
    int32_t width = 0, height = 0;
    if (mCapability(&mHandle, &width, &height) || width < 1920 || height < 1088) {
        closeDecoder(); return C2_CANNOT_DO;
    }
    return C2_OK;
}
bool C2SprdVp8Dec::collect() {
    for (auto &f : mFrames) {
        if (!f.block || f.refs) continue;
        if (f.importId && ioctl(mFd, VSP_RELEASE_IMPORT, &f.importId)) {
            mError = true; return false;
        }
        f.view.reset(); f.block.reset(); f.importId = f.dma = 0;
    }
    return true;
}
LEGACY void C2SprdVp8Dec::closeDecoder() {
    if (mInitialized) { mReleaseRefs(&mHandle); mRelease(&mHandle); }
    mInitialized = false;
    for (auto &f : mFrames) f.refs = 0;
    collect();
    for (auto &f : mFrames) { f.view.reset(); f.block.reset(); f.importId = f.dma = 0; }
    mStream.clear(); mExtra.clear();
    if (mFd >= 0) close(mFd);
    mFd = -1; mHandle = {}; mGeometry.reset(); mInternal.clear();
}
void C2SprdVp8Dec::onRelease() {
    closeDecoder();
    if (mLibrary) dlclose(mLibrary);
    mLibrary = nullptr;
    if (mOwnsEngine) SprdVspReleaseSession();
    mOwnsEngine = false;
}
c2_status_t C2SprdVp8Dec::newFrame(const std::shared_ptr<C2BlockPool> &pool, Frame **out) {
    if (!collect()) return C2_CORRUPTED;
    Frame *f = nullptr;
    for (auto &entry : mFrames) if (!entry.block) { f = &entry; break; }
    if (!f || !mGeometry) return C2_NO_MEMORY;
    const uint32_t stride = mGeometry->stride(), slice = mGeometry->slice();
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
    int ret = ioctl(mFd, VSP_IMPORT_BUFFER, &request);
    native_handle_delete(native);
    if (ret) return C2_NO_MEMORY;
    f->importId = request.buffer_id; f->dma = request.dma_addr;
    if (request.size < stride * slice * 3 / 2) return C2_BAD_VALUE;
    sprd_vsp_sync sync{f->importId, 0};
    if (ioctl(mFd, VSP_SYNC_IMPORT, &sync)) return C2_CORRUPTED;
    f->refs = 0; *out = f; return C2_OK;
}
LEGACY int C2SprdVp8Dec::decodeFrame(size_t size, Frame *f, vp8_output *output) {
    for (unsigned retry = 0; retry < 3; ++retry) {
        mSetPic(&mHandle, f->view->data()[0], reinterpret_cast<uint8_t *>(uintptr_t(f->dma)), f);
        vp8_input input{}; input.stream = mStream.cpu; input.dma = mStream.dma;
        input.length = size; input.display = 1;
        *output = {};
        int rc = mDecode(&mHandle, &input, output);
        int32_t width = 0, height = 0;
        mDimensions(&mHandle, &width, &height);
        if (mError || !mGeometry || width != int32_t(mGeometry->stride()) ||
            height != int32_t(mGeometry->slice())) return -1;
        if (rc == -10) continue;
        return rc;
    }
    return -1;
}
void C2SprdVp8Dec::process(const std::unique_ptr<C2Work> &work,
                          const std::shared_ptr<C2BlockPool> &pool) {
    empty(work);
    if (mError || mEos) { empty(work, C2_BAD_STATE); return; }
    std::vector<uint8_t> bytes;
    for (const auto &buffer : work->input.buffers) for (const auto &block : buffer->data().linearBlocks()) {
        C2ReadView view = block.map().get();
        if (view.error() != C2_OK || view.capacity() > kStream - 128 - bytes.size()) {
            fail(work, C2_BAD_VALUE); return;
        }
        bytes.insert(bytes.end(), view.data(), view.data() + view.capacity());
    }
    if (bytes.empty()) { mEos = work->input.flags & C2FrameData::FLAG_END_OF_STREAM; return; }
    const uint32_t tag = bytes.size() < 3 ? 0 : bytes[0] | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16;
    if (bytes.size() < 3 || ((tag >> 1) & 7) != 0 || (tag >> 5) > bytes.size() - 3 ||
        (work->input.flags & C2FrameData::FLAG_CODEC_CONFIG)) { fail(work, C2_CANNOT_DO); return; }
    const bool key = !(tag & 1), shown = tag & 16;
    if (key) {
        if (bytes.size() < 10 || memcmp(bytes.data() + 3, "\x9d\x01\x2a", 3)) {
            fail(work, C2_CORRUPTED); return;
        }
        Geometry g{le16(bytes.data() + 6), le16(bytes.data() + 8)};
        if (g.width < 2 || g.height < 2 || (g.width | g.height) & 1 || g.width > 1920 || g.height > 1080) {
            fail(work, C2_CANNOT_DO); return;
        }
        if (!mGeometry || !(*mGeometry == g)) {
            closeDecoder(); auto err = initDecoder();
            if (err != C2_OK) { fail(work, err); return; }
            mGeometry = g;
            C2StreamPictureSizeInfo::output size(0u, g.width, g.height);
            std::vector<std::unique_ptr<C2SettingResult>> failures;
            err = mIntf->config({&size}, C2_MAY_BLOCK, &failures);
            if (err != C2_OK) { fail(work, err); return; }
            work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(size));
        }
    } else if (!mGeometry) {
        mEos = work->input.flags & C2FrameData::FLAG_END_OF_STREAM; return;
    }
    Frame *frame = nullptr;
    auto err = newFrame(pool, &frame);
    if (err != C2_OK) { fail(work, err); return; }
    memcpy(mStream.cpu, bytes.data(), bytes.size()); memset(mStream.cpu + bytes.size(), 0, 128);
    vp8_output output{};
    if (decodeFrame(bytes.size(), frame, &output) || bool(output.effective) != shown || output.errors || output.transposed ||
        output.width != mGeometry->stride() || output.height != mGeometry->slice()) {
        fail(work, C2_CORRUPTED); return;
    }
    if (!shown) {
        // Hidden pictures update the reference pool, but complete their input
        // work without a presentation buffer or a delayed output timestamp.
        mEos = work->input.flags & C2FrameData::FLAG_END_OF_STREAM;
        if (!collect()) fail(work, C2_CORRUPTED);
        return;
    }
    Frame *display = checked(output.header);
    if (!display || output.y != display->view->data()[0] ||
        output.u != display->view->data()[1]) { fail(work, C2_CORRUPTED); return; }
    sprd_vsp_sync sync{display->importId, 1};
    if (ioctl(mFd, VSP_SYNC_IMPORT, &sync)) { fail(work, C2_CORRUPTED); return; }
    auto buffer = createGraphicBuffer(display->block, C2Rect(mGeometry->width, mGeometry->height));
    buffer->setInfo(mIntf->color());
    work->worklets.front()->output.buffers = {buffer};
    mEos = work->input.flags & C2FrameData::FLAG_END_OF_STREAM;
    if (!collect()) fail(work, C2_CORRUPTED);
}

class SprdVp8Factory : public C2ComponentFactory {
    std::shared_ptr<C2ReflectorHelper> mHelper = std::static_pointer_cast<C2ReflectorHelper>(
        GetCodec2PlatformComponentStore()->getParamReflector());
public:
    c2_status_t createComponent(c2_node_id_t id, std::shared_ptr<C2Component> *out,
                                std::function<void(C2Component *)> deleter) override {
        *out = std::shared_ptr<C2Component>(new C2SprdVp8Dec(id, std::make_shared<Vp8Interface>(mHelper)), deleter);
        return C2_OK;
    }
    c2_status_t createInterface(c2_node_id_t id, std::shared_ptr<C2ComponentInterface> *out,
                                std::function<void(C2ComponentInterface *)> deleter) override {
        *out = std::shared_ptr<C2ComponentInterface>(new SimpleInterface<Vp8Interface>(kName, id,
            std::make_shared<Vp8Interface>(mHelper)), deleter);
        return C2_OK;
    }
};
} // namespace android
__attribute__((cfi_canonical_jump_table))
extern "C" C2ComponentFactory *CreateVp8Codec2Factory() { return new android::SprdVp8Factory; }
__attribute__((cfi_canonical_jump_table))
extern "C" void DestroyVp8Codec2Factory(C2ComponentFactory *factory) { delete factory; }

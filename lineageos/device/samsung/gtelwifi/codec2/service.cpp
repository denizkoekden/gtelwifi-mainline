// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Deniz Kokden
#define LOG_TAG "media.c2.sprd"
#include <log/log.h>
#include <minijail.h>
#include <C2ComponentFactory.h>
#include <C2PlatformSupport.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <codec2/aidl/ComponentStore.h>
#include <atomic>
#include <csignal>
extern "C" C2ComponentFactory *CreateCodec2Factory();
extern "C" void DestroyCodec2Factory(C2ComponentFactory *);
extern "C" C2ComponentFactory *CreateVp8Codec2Factory();
extern "C" void DestroyVp8Codec2Factory(C2ComponentFactory *);
extern "C" C2ComponentFactory *CreateMpeg4Codec2Factory();
extern "C" void DestroyMpeg4Codec2Factory(C2ComponentFactory *);
extern "C" C2ComponentFactory *CreateH263Codec2Factory();
extern "C" void DestroyH263Codec2Factory(C2ComponentFactory *);
namespace {
constexpr char kSeccompPolicy[] =
    "/vendor/etc/seccomp_policy/android.hardware.media.c2-sprd-seccomp_policy";
constexpr char kAvc[] = "c2.sprd.avc.decoder";
constexpr char kVp8[] = "c2.sprd.vp8.decoder";
constexpr char kMpeg4[] = "c2.sprd.mpeg4.decoder";
constexpr char kH263[] = "c2.sprd.h263.decoder";
class Store final : public C2ComponentStore {
    std::shared_ptr<C2ComponentStore> mPlatform = android::GetCodec2PlatformComponentStore();
    std::unique_ptr<C2ComponentFactory, decltype(&DestroyCodec2Factory)> mAvcFactory{
        CreateCodec2Factory(), DestroyCodec2Factory};
    std::unique_ptr<C2ComponentFactory, decltype(&DestroyVp8Codec2Factory)> mVp8Factory{
        CreateVp8Codec2Factory(), DestroyVp8Codec2Factory};
    std::unique_ptr<C2ComponentFactory, decltype(&DestroyMpeg4Codec2Factory)> mMpeg4Factory{
        CreateMpeg4Codec2Factory(), DestroyMpeg4Codec2Factory};
    std::unique_ptr<C2ComponentFactory, decltype(&DestroyH263Codec2Factory)> mH263Factory{
        CreateH263Codec2Factory(), DestroyH263Codec2Factory};
    std::atomic<c2_node_id_t> mId{1};
    C2ComponentFactory *factory(const C2String &name) {
        if (name == kAvc) return mAvcFactory.get();
        if (name == kVp8) return mVp8Factory.get();
        if (name == kMpeg4) return mMpeg4Factory.get();
        if (name == kH263) return mH263Factory.get();
        return nullptr;
    }
public:
    C2String getName() const override { return "sprd"; }
    c2_status_t createComponent(C2String name, std::shared_ptr<C2Component> *out) override {
        auto *f = factory(name);
        if (!f) return C2_NOT_FOUND;
        return f->createComponent(mId++, out, [](C2Component *c) { delete c; });
    }
    c2_status_t createInterface(C2String name, std::shared_ptr<C2ComponentInterface> *out) override {
        auto *f = factory(name);
        if (!f) return C2_NOT_FOUND;
        return f->createInterface(mId++, out, [](C2ComponentInterface *i) { delete i; });
    }
    std::vector<std::shared_ptr<const C2Component::Traits>> listComponents() override {
        std::vector<std::shared_ptr<const C2Component::Traits>> traits;
        for (const auto &[name, mime] : {std::pair{kAvc, "video/avc"},
                                       std::pair{kVp8, "video/x-vnd.on2.vp8"},
                                       std::pair{kMpeg4, "video/mp4v-es"},
                                       std::pair{kH263, "video/3gpp"}}) {
            auto t = std::make_shared<C2Component::Traits>();
            t->name = name; t->domain = C2Component::DOMAIN_VIDEO;
            t->kind = C2Component::KIND_DECODER; t->rank = 128;
            t->mediaType = mime; t->owner = "default";
            traits.push_back(t);
        }
        return traits;
    }
    c2_status_t copyBuffer(std::shared_ptr<C2GraphicBuffer>, std::shared_ptr<C2GraphicBuffer>) override {
        return C2_OMITTED;
    }
    c2_status_t query_sm(const std::vector<C2Param *> &params,
        const std::vector<C2Param::Index> &indices,
        std::vector<std::unique_ptr<C2Param>> *heap) const override {
        return mPlatform->query_sm(params, indices, heap);
    }
    c2_status_t config_sm(const std::vector<C2Param *> &params,
        std::vector<std::unique_ptr<C2SettingResult>> *failures) override {
        return mPlatform->config_sm(params, failures);
    }
    std::shared_ptr<C2ParamReflector> getParamReflector() const override {
        return mPlatform->getParamReflector();
    }
    c2_status_t querySupportedParams_nb(
        std::vector<std::shared_ptr<C2ParamDescriptor>> *params) const override {
        return mPlatform->querySupportedParams_nb(params);
    }
    c2_status_t querySupportedValues_sm(std::vector<C2FieldSupportedValuesQuery> &fields) const override {
        return mPlatform->querySupportedValues_sm(fields);
    }
};
}
int main() {
    using namespace aidl::android::hardware::media::c2;
    // Enter the platform Codec2 sandbox before creating any binder or decoder
    // threads so that every worker inherits the syscall filter.
    std::signal(SIGPIPE, SIG_IGN);
    ALOGI("starting sandboxed Spreadtrum Codec2 service");
    android::SetUpMinijail(kSeccompPolicy, "");
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    ABinderProcess_startThreadPool();
    auto store = ndk::SharedRefBase::make<utils::ComponentStore>(std::make_shared<Store>());
    const std::string name = std::string(IComponentStore::descriptor) + "/default";
    if (AServiceManager_addService(store->asBinder().get(), name.c_str()) != EX_NONE) {
        ALOGE("register %s failed", name.c_str()); return 1;
    }
    ALOGI("registered %s", name.c_str());
    ABinderProcess_joinThreadPool();
    return 1;
}

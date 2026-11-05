/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * android.hardware.gnss AIDL service for the SM-T560 (gtelwifi): a bridge
 * to the stock firmware's legacy GPS HAL (gps.h, Android 4.4), which talks
 * to Broadcom's glgps daemon. Modelled on hardware/interfaces/gnss
 * (aidl/default for the AIDL side, 1.0/default for the legacy callbacks).
 *
 * The HAL blob is proprietary and never part of the image: init bind-mounts
 * /data/vendor/gps/blobs on /vendor/lib/gpsblobs (README). Without it the
 * service still registers, so the declared HAL never blocks system_server,
 * and simply reports no fixes. Extensions the legacy HAL lacks answer
 * EX_UNSUPPORTED_OPERATION, which the framework treats as "not present".
 */

#define LOG_TAG "GnssGtelwifi"

#include <aidl/android/hardware/gnss/BnGnss.h>
#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <dlfcn.h>
#include <hardware/gps.h>
#include <log/log.h>
#include <pthread.h>
#include <utils/SystemClock.h>

#include <algorithm>
#include <mutex>

namespace aidl::android::hardware::gnss {

using ndk::ScopedAStatus;

namespace {

constexpr const char* kHalPath = "/vendor/lib/gpsblobs/gps.default.so";

// The blob was built against the Android 4.4 gps.h: GpsCallbacks ended
// after request_utc_time_cb.
constexpr size_t kLegacyCallbacksSize = offsetof(GpsCallbacks, set_system_info_cb);

ScopedAStatus unsupported() {
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

}  // namespace

class GnssLegacy : public BnGnss {
  public:
    ScopedAStatus setCallback(const std::shared_ptr<IGnssCallback>& callback) override;
    ScopedAStatus start() override;
    ScopedAStatus stop() override;
    ScopedAStatus close() override;
    ScopedAStatus injectTime(int64_t timeMs, int64_t timeReferenceMs,
                             int uncertaintyMs) override;
    ScopedAStatus injectLocation(const GnssLocation& location) override;
    ScopedAStatus injectBestLocation(const GnssLocation& location) override;
    ScopedAStatus deleteAidingData(GnssAidingData aidingDataFlags) override;
    ScopedAStatus setPositionMode(const PositionModeOptions& options) override;
    ScopedAStatus startSvStatus() override { return ScopedAStatus::ok(); }
    ScopedAStatus stopSvStatus() override { return ScopedAStatus::ok(); }
    ScopedAStatus startNmea() override { return ScopedAStatus::ok(); }
    ScopedAStatus stopNmea() override { return ScopedAStatus::ok(); }

    ScopedAStatus getExtensionPsds(std::shared_ptr<IGnssPsds>*) override { return unsupported(); }
    ScopedAStatus getExtensionGnssConfiguration(std::shared_ptr<IGnssConfiguration>*) override {
        return unsupported();
    }
    ScopedAStatus getExtensionGnssPowerIndication(std::shared_ptr<IGnssPowerIndication>*) override {
        return unsupported();
    }
    ScopedAStatus getExtensionGnssMeasurement(
            std::shared_ptr<IGnssMeasurementInterface>*) override {
        return unsupported();
    }
    ScopedAStatus getExtensionGnssBatching(std::shared_ptr<IGnssBatching>*) override {
        return unsupported();
    }
    ScopedAStatus getExtensionGnssGeofence(std::shared_ptr<IGnssGeofence>*) override {
        return unsupported();
    }
    ScopedAStatus getExtensionGnssNavigationMessage(
            std::shared_ptr<IGnssNavigationMessageInterface>*) override {
        return unsupported();
    }
    ScopedAStatus getExtensionAGnss(std::shared_ptr<IAGnss>*) override { return unsupported(); }
    ScopedAStatus getExtensionAGnssRil(std::shared_ptr<IAGnssRil>*) override {
        return unsupported();
    }
    ScopedAStatus getExtensionGnssDebug(std::shared_ptr<IGnssDebug>*) override {
        return unsupported();
    }
    ScopedAStatus getExtensionGnssVisibilityControl(
            std::shared_ptr<visibility_control::IGnssVisibilityControl>*) override {
        return unsupported();
    }
    ScopedAStatus getExtensionGnssAntennaInfo(std::shared_ptr<IGnssAntennaInfo>*) override {
        return unsupported();
    }
    ScopedAStatus getExtensionMeasurementCorrections(
            std::shared_ptr<measurement_corrections::IMeasurementCorrectionsInterface>*) override {
        return unsupported();
    }
    ScopedAStatus getExtensionGnssAssistanceInterface(
            std::shared_ptr<gnss_assistance::IGnssAssistanceInterface>*) override {
        return unsupported();
    }

  private:
    bool loadLegacyHal();

    // Legacy callbacks run on threads the HAL creates through create_thread_cb.
    static void locationCb(GpsLocation* location);
    static void statusCb(GpsStatus* status);
    static void svStatusCb(GpsSvStatus* svStatus);
    static void nmeaCb(GpsUtcTime timestamp, const char* nmea, int length);
    static void setCapabilitiesCb(uint32_t capabilities);
    static void acquireWakelockCb() {}
    static void releaseWakelockCb() {}
    static pthread_t createThreadCb(const char* name, void (*start)(void*), void* arg);
    static void requestUtcTimeCb();

    static std::shared_ptr<IGnssCallback> callback();

    static std::mutex sMutex;
    static std::shared_ptr<IGnssCallback> sCallback;
    static GpsCallbacks sLegacyCallbacks;

    const GpsInterface* mLegacy = nullptr;
    bool mInitialized = false;
    PositionModeOptions mMode = {.minIntervalMs = 1000};
};

std::mutex GnssLegacy::sMutex;
std::shared_ptr<IGnssCallback> GnssLegacy::sCallback;
GpsCallbacks GnssLegacy::sLegacyCallbacks = {
        .size = kLegacyCallbacksSize,
        .location_cb = GnssLegacy::locationCb,
        .status_cb = GnssLegacy::statusCb,
        .sv_status_cb = GnssLegacy::svStatusCb,
        .nmea_cb = GnssLegacy::nmeaCb,
        .set_capabilities_cb = GnssLegacy::setCapabilitiesCb,
        .acquire_wakelock_cb = GnssLegacy::acquireWakelockCb,
        .release_wakelock_cb = GnssLegacy::releaseWakelockCb,
        .create_thread_cb = GnssLegacy::createThreadCb,
        .request_utc_time_cb = GnssLegacy::requestUtcTimeCb,
};

bool GnssLegacy::loadLegacyHal() {
    if (mLegacy != nullptr) return true;

    void* handle = dlopen(kHalPath, RTLD_NOW);
    if (handle == nullptr) {
        ALOGW("no legacy GPS HAL: %s", dlerror());
        return false;
    }
    auto* module = static_cast<hw_module_t*>(dlsym(handle, HAL_MODULE_INFO_SYM_AS_STR));
    hw_device_t* device = nullptr;
    if (module == nullptr || module->methods->open(module, GPS_HARDWARE_MODULE_ID, &device)) {
        ALOGE("cannot open the legacy GPS HAL");
        return false;
    }
    auto* gps = reinterpret_cast<gps_device_t*>(device);
    mLegacy = gps->get_gps_interface(gps);
    if (mLegacy == nullptr) ALOGE("legacy GPS HAL without GpsInterface");
    return mLegacy != nullptr;
}

std::shared_ptr<IGnssCallback> GnssLegacy::callback() {
    std::lock_guard<std::mutex> lock(sMutex);
    return sCallback;
}

ScopedAStatus GnssLegacy::setCallback(const std::shared_ptr<IGnssCallback>& callback) {
    if (callback == nullptr) return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sCallback = callback;
    }
    IGnssCallback::GnssSystemInfo info = {.yearOfHw = 2016, .name = "Broadcom BCM47520 (glgps)"};
    callback->gnssSetSystemInfoCb(info);

    if (!mInitialized && loadLegacyHal()) {
        mInitialized = mLegacy->init(&sLegacyCallbacks) == 0;
        if (!mInitialized) ALOGE("legacy GPS HAL init failed");
    }
    return ScopedAStatus::ok();
}

ScopedAStatus GnssLegacy::setPositionMode(const PositionModeOptions& options) {
    mMode = options;
    if (!mInitialized) return ScopedAStatus::ok();
    // Assistance is not available (no SUPL, the LTO server is gone)
    int ret = mLegacy->set_position_mode(GPS_POSITION_MODE_STANDALONE,
                                         static_cast<GpsPositionRecurrence>(options.recurrence),
                                         std::max(1000, options.minIntervalMs),
                                         options.preferredAccuracyMeters,
                                         options.preferredTimeMs);
    return ret ? ScopedAStatus::fromExceptionCode(EX_SERVICE_SPECIFIC) : ScopedAStatus::ok();
}

ScopedAStatus GnssLegacy::start() {
    // The blobs may have been copied after boot: try again on every start
    if (!mInitialized && callback() != nullptr && loadLegacyHal()) {
        mInitialized = mLegacy->init(&sLegacyCallbacks) == 0;
        if (mInitialized) setPositionMode(mMode);
    }
    if (!mInitialized) return ScopedAStatus::ok();
    return mLegacy->start() ? ScopedAStatus::fromExceptionCode(EX_SERVICE_SPECIFIC)
                            : ScopedAStatus::ok();
}

ScopedAStatus GnssLegacy::stop() {
    if (!mInitialized) return ScopedAStatus::ok();
    return mLegacy->stop() ? ScopedAStatus::fromExceptionCode(EX_SERVICE_SPECIFIC)
                           : ScopedAStatus::ok();
}

ScopedAStatus GnssLegacy::close() {
    if (mInitialized) {
        mLegacy->cleanup();
        mInitialized = false;
    }
    std::lock_guard<std::mutex> lock(sMutex);
    sCallback = nullptr;
    return ScopedAStatus::ok();
}

ScopedAStatus GnssLegacy::injectTime(int64_t timeMs, int64_t timeReferenceMs, int uncertaintyMs) {
    if (mInitialized) mLegacy->inject_time(timeMs, timeReferenceMs, uncertaintyMs);
    return ScopedAStatus::ok();
}

ScopedAStatus GnssLegacy::injectLocation(const GnssLocation& location) {
    if (mInitialized) {
        mLegacy->inject_location(location.latitudeDegrees, location.longitudeDegrees,
                                 location.horizontalAccuracyMeters);
    }
    return ScopedAStatus::ok();
}

ScopedAStatus GnssLegacy::injectBestLocation(const GnssLocation& location) {
    return injectLocation(location);
}

ScopedAStatus GnssLegacy::deleteAidingData(GnssAidingData aidingDataFlags) {
    if (mInitialized) mLegacy->delete_aiding_data(static_cast<GpsAidingData>(aidingDataFlags));
    return ScopedAStatus::ok();
}

void GnssLegacy::locationCb(GpsLocation* in) {
    auto cb = callback();
    if (cb == nullptr || in == nullptr) return;
    GnssLocation location = {
            .gnssLocationFlags = in->flags,
            .latitudeDegrees = in->latitude,
            .longitudeDegrees = in->longitude,
            .altitudeMeters = in->altitude,
            .speedMetersPerSec = in->speed,
            .bearingDegrees = in->bearing,
            .horizontalAccuracyMeters = in->accuracy,
            .timestampMillis = in->timestamp,
            .elapsedRealtime = {.flags = ElapsedRealtime::HAS_TIMESTAMP_NS,
                                .timestampNs = ::android::elapsedRealtimeNano()},
    };
    cb->gnssLocationCb(location);
}

void GnssLegacy::statusCb(GpsStatus* status) {
    auto cb = callback();
    if (cb == nullptr || status == nullptr) return;
    cb->gnssStatusCb(static_cast<IGnssCallback::GnssStatusValue>(status->status));
}

void GnssLegacy::svStatusCb(GpsSvStatus* in) {
    auto cb = callback();
    if (cb == nullptr || in == nullptr) return;
    std::vector<IGnssCallback::GnssSvInfo> list;
    for (int i = 0; i < in->num_svs && i < GPS_MAX_SVS; i++) {
        const GpsSvInfo& sv = in->sv_list[i];
        IGnssCallback::GnssSvInfo out = {
                .svid = sv.prn,
                .constellation = GnssConstellationType::UNKNOWN,
                .cN0Dbhz = sv.snr,
                .elevationDegrees = sv.elevation,
                .azimuthDegrees = sv.azimuth,
        };
        // glgps numbering: 1-32 GPS, 33-64 SBAS (PRN 120-151), 65-96 GLONASS
        if (sv.prn >= 1 && sv.prn <= 32) {
            out.constellation = GnssConstellationType::GPS;
            uint32_t bit = 1u << (sv.prn - 1);
            if (in->ephemeris_mask & bit)
                out.svFlag |= (int)IGnssCallback::GnssSvFlags::HAS_EPHEMERIS_DATA;
            if (in->almanac_mask & bit)
                out.svFlag |= (int)IGnssCallback::GnssSvFlags::HAS_ALMANAC_DATA;
            if (in->used_in_fix_mask & bit)
                out.svFlag |= (int)IGnssCallback::GnssSvFlags::USED_IN_FIX;
        } else if (sv.prn >= 33 && sv.prn <= 64) {
            out.constellation = GnssConstellationType::SBAS;
            out.svid = sv.prn + 87;
        } else if (sv.prn >= 65 && sv.prn <= 96) {
            out.constellation = GnssConstellationType::GLONASS;
            out.svid = sv.prn - 64;
        }
        list.push_back(out);
    }
    cb->gnssSvStatusCb(list);
}

void GnssLegacy::nmeaCb(GpsUtcTime timestamp, const char* nmea, int length) {
    auto cb = callback();
    if (cb == nullptr || nmea == nullptr || length <= 0) return;
    cb->gnssNmeaCb(timestamp, std::string(nmea, length));
}

void GnssLegacy::setCapabilitiesCb(uint32_t capabilities) {
    auto cb = callback();
    if (cb == nullptr) return;
    // GPS_CAPABILITY_SCHEDULING .. ON_DEMAND_TIME share their bits with the AIDL ones
    cb->gnssSetCapabilitiesCb(capabilities & 0x1f);
}

// glgps asks for the time on a cold start; the framework answers with
// injectTime() from network time (there is no other assistance here)
void GnssLegacy::requestUtcTimeCb() {
    auto cb = callback();
    if (cb != nullptr) cb->gnssRequestTimeCb();
}

pthread_t GnssLegacy::createThreadCb(const char* name, void (*start)(void*), void* arg) {
    pthread_t thread;
    if (pthread_create(&thread, nullptr, reinterpret_cast<void* (*)(void*)>(start), arg)) {
        ALOGE("cannot create the HAL thread %s", name);
        return 0;
    }
    pthread_setname_np(thread, name);
    return thread;
}

}  // namespace aidl::android::hardware::gnss

int main() {
    using aidl::android::hardware::gnss::GnssLegacy;

    ABinderProcess_setThreadPoolMaxThreadCount(1);
    ABinderProcess_startThreadPool();
    auto gnss = ndk::SharedRefBase::make<GnssLegacy>();
    const std::string instance = std::string() + GnssLegacy::descriptor + "/default";
    CHECK_EQ(AServiceManager_addService(gnss->asBinder().get(), instance.c_str()), STATUS_OK);
    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;
}

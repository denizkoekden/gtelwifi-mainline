/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define FAILURE_DEBUG_PREFIX "RadioModem"

// Changes for the SM-T561 (gtel3g) Spreadtrum CP are marked "gtel3g".

#include <charconv>
#include <format>
#include <thread>

using namespace std::literals;

#include "RadioModem.h"

#include "atCmds.h"
#include "debug.h"
#include "makeRadioResponseInfo.h"
#include "ratUtils.h"

namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace implementation {
namespace {
constexpr char kModemUuid[] = "com.samsung.gtel3g.modem";
constexpr char kSimUuid[] = "com.samsung.gtel3g.simcard";

// gtel3g: a plain answer line, or "+CMD: value" if the CP prefixes it
const std::string* getText(const AtResponse& response, const std::string_view cmd) {
    if (const std::string* text = response.get_if<std::string>()) {
        return text;
    } else if (const AtResponse::Unknown* u = response.get_if<AtResponse::Unknown>()) {
        return u->is(cmd) ? &u->payload : nullptr;
    } else {
        return nullptr;
    }
}
}  // namespace

RadioModem::RadioModem(std::shared_ptr<AtChannel> atChannel) : mAtChannel(std::move(atChannel)) {
}

ScopedAStatus RadioModem::enableModem(const int32_t serial, const bool /*on*/) {
    NOT_NULL(mRadioModemResponse)->enableModemResponse(
        makeRadioResponseInfo(serial));
    return ScopedAStatus::ok();
}

ScopedAStatus RadioModem::getBasebandVersion(const int32_t serial) {
    // gtel3g: AT+CGMR (the T561 answered "T561XXU0AQA2")
    mAtChannel->queueRequester([this, serial](const AtChannel::RequestPipe requestPipe) -> bool {
        const AtResponsePtr response =
            mAtConversation(requestPipe, atCmds::getBasebandVersion,
                            [](const AtResponse& response) -> bool {
                                return getText(response, "+CGMR"sv) != nullptr;
                            });
        const std::string* version = response ? getText(*response, "+CGMR"sv) : nullptr;
        if (version) {
            NOT_NULL(mRadioModemResponse)->getBasebandVersionResponse(
                    makeRadioResponseInfo(serial), *version);
        } else {
            NOT_NULL(mRadioModemResponse)->getBasebandVersionResponse(
                    makeRadioResponseInfo(serial, FAILURE(RadioError::GENERIC_FAILURE)), "");
        }
        return true;
    });

    return ScopedAStatus::ok();
}

ScopedAStatus RadioModem::getImei(const int32_t serial) {
    static const char* const kFunc = __func__;
    mAtChannel->queueRequester([this, serial](const AtChannel::RequestPipe requestPipe) -> bool {
        using namespace std::literals;

        // gtel3g: AT+CGSN gives the bare IMEI (no SVN) on this CP
        const AtResponsePtr response =
            mAtConversation(requestPipe, atCmds::getIMEI,
                            [](const AtResponse& response) -> bool {
                                return getText(response, "+CGSN"sv) != nullptr;
                            });
        const std::string* imeiSvn = response ? getText(*response, "+CGSN"sv) : nullptr;
        if (!imeiSvn) {
            NOT_NULL(mRadioModemResponse)->getImeiResponse(
                    makeRadioResponseInfo(serial, FAILURE(RadioError::GENERIC_FAILURE)), {});
            return true;
        } else {
            using modem::ImeiInfo;
            (void)kFunc;

            ImeiInfo imeiInfo = {
                .type = ImeiInfo::ImeiType::PRIMARY,
                .imei = imeiSvn->substr(0, 15),
                .svn = (imeiSvn->size() >= 17) ? imeiSvn->substr(15, 2) : std::string(),
            };

            NOT_NULL(mRadioModemResponse)->getImeiResponse(
                makeRadioResponseInfo(serial), std::move(imeiInfo));
           return true;
        }
    });

    return ScopedAStatus::ok();
}

ScopedAStatus RadioModem::getHardwareConfig(const int32_t serial) {
    mAtChannel->queueRequester([this, serial](const AtChannel::RequestPipe requestPipe) -> bool {
        using modem::HardwareConfig;
        using modem::HardwareConfigModem;
        using modem::HardwareConfigSim;

        std::vector<HardwareConfig> config;

        const auto [status, rafBitmask] =
            getSupportedRadioTechs(requestPipe, mAtConversation);
        if (status == RadioError::NONE) {
            const HardwareConfigModem modemHwConfig = {
                .rilModel = 0,  // 0 - single: one-to-one relationship between a modem hardware and a ril daemon.
                .rat = static_cast<RadioTechnology>(rafBitmask),
                .maxVoiceCalls = 1,
                .maxDataCalls = 1,
                .maxStandby = 1,
            };

            HardwareConfig modemConfig = {
                .type = HardwareConfig::TYPE_MODEM,
                .uuid = kModemUuid,
                .state = HardwareConfig::STATE_ENABLED,
            };

            modemConfig.modem.push_back(modemHwConfig);

            HardwareConfig simConfig = {
                .type = HardwareConfig::TYPE_SIM,
                .uuid = kSimUuid,
                .state = HardwareConfig::STATE_ENABLED,
            };

            HardwareConfigSim simHwConfig = {
                .modemUuid = modemConfig.uuid,
            };
            simConfig.sim.push_back(simHwConfig);

            config.push_back(std::move(modemConfig));
            config.push_back(std::move(simConfig));
        }

        NOT_NULL(mRadioModemResponse)->getHardwareConfigResponse(
                makeRadioResponseInfo(serial, status), std::move(config));
        return status != RadioError::INTERNAL_ERR;
    });

    return ScopedAStatus::ok();
}

ScopedAStatus RadioModem::getModemActivityInfo(const int32_t serial) {
    // gtel3g: the emulator returned made-up numbers; the CP has no such report
    NOT_NULL(mRadioModemResponse)->getModemActivityInfoResponse(
            makeRadioResponseInfoUnsupported(serial, FAILURE_DEBUG_PREFIX, __func__), {});
    return ScopedAStatus::ok();
}

ScopedAStatus RadioModem::getModemStackStatus(const int32_t serial) {
    NOT_NULL(mRadioModemResponse)->getModemStackStatusResponse(
            makeRadioResponseInfo(serial), true);
    return ScopedAStatus::ok();
}

ScopedAStatus RadioModem::getRadioCapability(const int32_t serial) {
    mAtChannel->queueRequester([this, serial](const AtChannel::RequestPipe requestPipe) -> bool {
        using modem::RadioCapability;
        RadioCapability cap;

        const auto [status, rafBitmask] =
            getSupportedRadioTechs(requestPipe, mAtConversation);
        if (status == RadioError::NONE) {
            cap.session = serial;
            cap.phase = RadioCapability::PHASE_CONFIGURED;
            cap.raf = rafBitmask;
            cap.logicalModemUuid = kModemUuid;
            cap.status = RadioCapability::STATUS_SUCCESS;
        }

        NOT_NULL(mRadioModemResponse)->getRadioCapabilityResponse(
                makeRadioResponseInfo(serial, status), std::move(cap));
        return status != RadioError::INTERNAL_ERR;
    });

    return ScopedAStatus::ok();
}

ScopedAStatus RadioModem::requestShutdown(const int32_t serial) {
    mAtChannel->queueRequester([this, serial]
                               (const AtChannel::RequestPipe requestPipe) -> bool {
        if (setRadioPowerImpl(requestPipe, false)) {
            NOT_NULL(mRadioModemResponse)->requestShutdownResponse(
                makeRadioResponseInfo(serial));
            return true;
        } else {
            NOT_NULL(mRadioModemResponse)->requestShutdownResponse(
                makeRadioResponseInfo(serial, RadioError::GENERIC_FAILURE));
            return false;
        }
    });

    return ScopedAStatus::ok();
}

ScopedAStatus RadioModem::sendDeviceState(const int32_t serial,
                                          const modem::DeviceStateType /*stateType*/,
                                          const bool /*state*/) {
    // matches reference-ril.c
    NOT_NULL(mRadioModemResponse)->sendDeviceStateResponse(
        makeRadioResponseInfoNOP(serial));
    return ScopedAStatus::ok();
}

ScopedAStatus RadioModem::setRadioCapability(const int32_t serial,
                                             const modem::RadioCapability& /*rc*/) {
    // matches reference-ril.c
    NOT_NULL(mRadioModemResponse)->setRadioCapabilityResponse(
        makeRadioResponseInfoNOP(serial), {});
    return ScopedAStatus::ok();
}

ScopedAStatus RadioModem::setRadioPower(const int32_t serial, const bool powerOn,
                                        const bool /*forEmergencyCall*/,
                                        const bool /*preferredForEmergencyCall*/) {
    mAtChannel->queueRequester([this, serial, powerOn]
                               (const AtChannel::RequestPipe requestPipe) -> bool {
        if (setRadioPowerImpl(requestPipe, powerOn)) {
            NOT_NULL(mRadioModemResponse)->setRadioPowerResponse(
                    makeRadioResponseInfo(serial));
            return true;
        } else {
            // gtel3g: answer the request (goldfish left it unanswered)
            NOT_NULL(mRadioModemResponse)->setRadioPowerResponse(
                    makeRadioResponseInfo(serial, RadioError::GENERIC_FAILURE));
            return false;
        }
    });

    return ScopedAStatus::ok();
}

ScopedAStatus RadioModem::nvResetConfig(const int32_t serial, const modem::ResetNvType resetNvType) {
    if (resetNvType != modem::ResetNvType::RELOAD) {
        NOT_NULL(mRadioModemResponse)->nvResetConfigResponse(
            makeRadioResponseInfoDeprecated(serial));
        return ScopedAStatus::ok();
    }

    mAtChannel->queueRequester([this, serial]
                               (const AtChannel::RequestPipe requestPipe) -> bool {
        // This what the previous implementation did.

        NOT_NULL(mRadioModemResponse)->nvResetConfigResponse(
            makeRadioResponseInfo(serial));

        return setRadioPowerImpl(requestPipe, false);
    });

    return ScopedAStatus::ok();
}

ScopedAStatus RadioModem::responseAcknowledgement() {
    return ScopedAStatus::ok();
}

void RadioModem::atResponseSink(const AtResponsePtr& response) {
    if (!mAtConversation.send(response)) {
        response->visit([this](const auto& msg){ handleUnsolicited(msg); });
    }
}

void RadioModem::handleUnsolicited(const AtResponse::CFUN& cfun) {
    bool changed;
    {
        std::lock_guard<std::mutex> lock(mMtx);
        changed = (mRadioState != cfun.state);
        mRadioState = cfun.state;
    }

    if (changed && mRadioModemIndication) {
        mRadioModemIndication->radioStateChanged(
            RadioIndicationType::UNSOLICITED, cfun.state);
    }
}

ScopedAStatus RadioModem::setResponseFunctions(
        const std::shared_ptr<modem::IRadioModemResponse>& radioModemResponse,
        const std::shared_ptr<modem::IRadioModemIndication>& radioModemIndication) {
    mRadioModemResponse = NOT_NULL(radioModemResponse);
    mRadioModemIndication = NOT_NULL(radioModemIndication);

    modem::RadioState radioState;

    {
        std::lock_guard<std::mutex> lock(mMtx);
        radioState = mRadioState;
    }

    radioModemIndication->rilConnected(RadioIndicationType::UNSOLICITED);

    radioModemIndication->radioStateChanged(
            RadioIndicationType::UNSOLICITED, radioState);

    return ScopedAStatus::ok();
}

std::pair<RadioError, uint32_t> RadioModem::getSupportedRadioTechs(
            const AtChannel::RequestPipe requestPipe,
            AtChannel::Conversation& atConversation) {
    using CTEC = AtResponse::CTEC;
    using ratUtils::ModemTechnology;

    AtResponsePtr response =
        atConversation(requestPipe, atCmds::getSupportedRadioTechs,
                        [](const AtResponse& response) -> bool {
                            return response.holds<CTEC>();
                        });
    if (!response || response->isParseError()) {
        return {FAILURE(RadioError::INTERNAL_ERR), 0};
    } else if (const CTEC* ctec = response->get_if<CTEC>()) {
        uint32_t rafBitmask = 0;

        for (const std::string& mtechStr : ctec->values) {
            int mtech;
            std::from_chars(&*mtechStr.begin(), &*mtechStr.end(), mtech, 10);

            rafBitmask |= ratUtils::supportedRadioTechBitmask(
                static_cast<ModemTechnology>(mtech));
        }

        return {RadioError::NONE, rafBitmask};
    } else {
        response->unexpected(FAILURE_DEBUG_PREFIX, __func__);
        return {FAILURE(RadioError::INTERNAL_ERR), 0};
    }
}

// gtel3g: the Spreadtrum CP switches its protocol stack with AT+SFUN=4 (on)
// and AT+SFUN=5 (off) [B: vendor RIL strings, meaning V]; AT+CFUN=1/0 is
// the fallback. The new state is reported to all services as if the CP had
// sent "+CFUN: <state>" (whether AT+CFUN? follows AT+SFUN is unknown).
// On the T561 (08.11.) AT+SFUN=5 answered OK at once, AT+SFUN=4 never did
// (the CP fetches its NV from nvitemd and turns the radio on, +CFUN: 1
// follows), AT+CFUN=1 then answered OK at once: wait 10 s, not 30 s.
bool RadioModem::setRadioPowerImpl(const AtChannel::RequestPipe requestPipe,
                                   const bool powerOn) {
    using namespace std::chrono_literals;

    {
        std::lock_guard<std::mutex> lock(mMtx);
        if (powerOn == (mRadioState == modem::RadioState::ON)) {
            return true;
        }
    }

    const auto okOrError = [](const AtResponse& response) -> bool {
        return response.isOK() || response.isFinalError();
    };

    AtResponsePtr response =
        mAtConversation(requestPipe, powerOn ? "AT+SFUN=4"sv : "AT+SFUN=5"sv,
                        okOrError, powerOn ? 10s : 30s);
    if (!response || !response->isOK()) {
        RLOGW("%s: AT+SFUN failed, trying AT+CFUN", FAILURE_DEBUG_PREFIX);
        response =
            mAtConversation(requestPipe, std::format("AT+CFUN={0:d}", powerOn ? 1 : 0),
                            okOrError, 30s);
    }

    if (!response) {
        return FAILURE(false);
    } else if (!response->isOK()) {
        return FAILURE(false);
    }

    using modem::RadioState;

    const modem::RadioState newState =
        powerOn ? RadioState::ON : RadioState::OFF;

    {
        std::lock_guard<std::mutex> lock(mMtx);
        mRadioState = newState;
    }

    // all services (RadioNetwork, RadioSim) follow the radio state via +CFUN
    mAtChannel->injectResponses(powerOn ? "+CFUN: 1\r"sv : "+CFUN: 0\r"sv);

    NOT_NULL(mRadioModemIndication)->radioStateChanged(
            RadioIndicationType::UNSOLICITED, newState);

    return true;
}

/************************* deprecated *************************/
ScopedAStatus RadioModem::getDeviceIdentity(const int32_t serial) {
    NOT_NULL(mRadioModemResponse)->getDeviceIdentityResponse(
        makeRadioResponseInfoDeprecated(serial),
        "", "", "", "");
    return ScopedAStatus::ok();
}

ScopedAStatus RadioModem::nvReadItem(const int32_t serial, modem::NvItem) {
    NOT_NULL(mRadioModemResponse)->nvReadItemResponse(
        makeRadioResponseInfoDeprecated(serial), "");
    return ScopedAStatus::ok();
}

ScopedAStatus RadioModem::nvWriteCdmaPrl(const int32_t serial, const std::vector<uint8_t>&) {
    NOT_NULL(mRadioModemResponse)->nvWriteCdmaPrlResponse(
        makeRadioResponseInfoDeprecated(serial));
    return ScopedAStatus::ok();
}

ScopedAStatus RadioModem::nvWriteItem(const int32_t serial, const modem::NvWriteItem&) {
    NOT_NULL(mRadioModemResponse)->nvWriteItemResponse(
        makeRadioResponseInfoDeprecated(serial));
    return ScopedAStatus::ok();
}

}  // namespace implementation
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl

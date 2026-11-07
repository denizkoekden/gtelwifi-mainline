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
 *
 * Modified for the Samsung SM-T561 (gtel3g), changes marked "gtel3g": PDP
 * contexts the Spreadtrum way, as the vendor RIL (libreference-ril_sp.so)
 * and phoneserver of the stock/LOS16 ROM do it [B: their AT strings]:
 *   AT+CGDCONT=<cid>,"IP","<apn>","",0,0
 *   AT+CGPCO=0,"<user>","<password>",<cid>,<auth>      (only with a user)
 *   AT+CGDATA="M-ETHER",<cid>                           (CP routes the
 *                                                        context to seth)
 *   AT+SIPCONFIG=<cid>  -> +SIPCONFIG: <cid>,<n>,"..","<ip>","..","<dns1>","<dns2>"
 *   AT+CGACT=0,<cid>                                    (deactivate)
 * phoneserver then put the address on rmnet<cid-1> with mtu 1400 and
 * "arp off". Here Android configures address and routes from the result;
 * the HAL brings the interface up with IFF_NOARP (seth frames are Ethernet,
 * nobody answers ARP behind it).
 */

#define FAILURE_DEBUG_PREFIX "RadioData"

#include <chrono>
#include <format>
#include <set>
#include <string_view>

#include <android-base/properties.h>
#include <android-base/strings.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "RadioData.h"

#include "debug.h"
#include "gtel3gUtils.h"
#include "makeRadioResponseInfo.h"

namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace implementation {
using data::DataCallFailCause;
using data::DataProfileInfo;
using data::PdpProtocolType;
using data::SetupDataCallResult;
using namespace std::literals;

namespace {
// gtel3g: one seth interface per context, cid 1..3 -> rmnet0..2 (DT names
// of the seth nodes; ro.modem.w.eth=rmnet in the vendor ROM)
constexpr int kMaxCid = 3;
// phoneserver: "link set rmnet<n> mtu 1400" [B]
constexpr int kMtu = 1400;
// comma separated DNS servers for a context without DNS from the CP
constexpr char kDnsFallbackProp[] = "persist.vendor.radio.dns_fallback";

std::string getInterfaceName(const int cid) {
    return "rmnet" + std::to_string(cid - 1);
}

// gtel3g: up with IFF_NOARP, or down
bool setInterfaceUp(const std::string& interfaceName, const bool up) {
    const int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_IP);
    if (sock < 0) {
        return FAILURE_V(false, "Failed to open interface socket: %s (%d)",
                         strerror(errno), errno);
    }

    struct ifreq request;
    memset(&request, 0, sizeof(request));
    strncpy(request.ifr_name, interfaceName.c_str(), sizeof(request.ifr_name));
    request.ifr_name[sizeof(request.ifr_name) - 1] = '\0';

    if (ioctl(sock, SIOCGIFFLAGS, &request)) {
        ::close(sock);
        return FAILURE_V(false, "Failed to get interface flags for %s: %s (%d)",
                         interfaceName.c_str(), strerror(errno), errno);
    }

    const short flags = up ? (request.ifr_flags | IFF_UP | IFF_NOARP)
                           : (request.ifr_flags & ~IFF_UP);
    if (flags != request.ifr_flags) {
        request.ifr_flags = flags;
        if (ioctl(sock, SIOCSIFFLAGS, &request)) {
            ::close(sock);
            return FAILURE_V(false, "Failed to set interface flags for %s: %s (%d)",
                             interfaceName.c_str(), strerror(errno), errno);
        }
    }

    ::close(sock);
    return true;
}

bool setIpAddr(const char *addr, const int addrSize,
               const char* radioInterfaceName) {
    const int family = strchr(addr, ':') ? AF_INET6 : AF_INET;
    const int sock = socket(family, SOCK_DGRAM, 0);
    if (sock == -1) {
        return FAILURE_V(false, "Failed to open a %s socket: %s (%d)",
                         ((family == AF_INET) ? "INET" : "INET6"),
                         strerror(errno), errno);
    }

    struct ifreq req4;
    memset(&req4, 0, sizeof(req4));

    strncpy(req4.ifr_name, radioInterfaceName, sizeof(req4.ifr_name));
    req4.ifr_name[sizeof(req4.ifr_name) - 1] = '\0';

    if (family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&req4.ifr_addr;
        sin->sin_family = AF_INET;
        sin->sin_addr.s_addr = inet_addr(addr);
        if (ioctl(sock, SIOCSIFADDR, &req4) < 0) {
            ::close(sock);
            return FAILURE_V(false, "SIOCSIFADDR IPv4 failed: %s (%d)",
                             strerror(errno), errno);
        }

        sin->sin_addr.s_addr = htonl(0xFFFFFFFFu << (32 - (addrSize ? addrSize : 32)));
        if (ioctl(sock, SIOCSIFNETMASK, &req4) < 0) {
            ::close(sock);
            return FAILURE_V(false, "SIOCSIFNETMASK IPv4 failed: %s (%d)",
                             strerror(errno), errno);
        }
    } else {
        if (ioctl(sock, SIOCGIFINDEX, &req4) < 0) {
            ::close(sock);
            return FAILURE_V(false, "SIOCGIFINDEX IPv6 failed: %s (%d)",
                             strerror(errno), errno);
        }

        struct in6_ifreq req6 = {
            .ifr6_prefixlen = static_cast<__u32>(addrSize ? addrSize : 128),
            .ifr6_ifindex = req4.ifr_ifindex,
        };

        if (inet_pton(AF_INET6, addr, &req6.ifr6_addr) != 1) {
            ::close(sock);
            return FAILURE_V(false, "inet_pton(AF_INET6, '%s') failed: %s (%d)",
                             addr, strerror(errno), errno);
        }

        if (ioctl(sock, SIOCSIFADDR, &req6) < 0) {
            ::close(sock);
            return FAILURE_V(false, "SIOCSIFADDR failed: %s (%d)",
                             strerror(errno), errno);
        }
    }

    ::close(sock);
    return true;
}

DataCallFailCause toDataCallFailCause(const RadioError e) {
    switch (e) {
    case RadioError::NONE:
        return DataCallFailCause::NONE;

    case RadioError::INTERNAL_ERR:
        return DataCallFailCause::MODEM_RESTART;

    case RadioError::REQUEST_NOT_SUPPORTED:
        return DataCallFailCause::FEATURE_NOT_SUPP;

    case RadioError::RADIO_NOT_AVAILABLE:
        return DataCallFailCause::RADIO_POWER_OFF;

    case RadioError::SIM_ABSENT:
    case RadioError::SIM_PIN2:
    case RadioError::SIM_PUK2:
    case RadioError::SIM_BUSY:
    case RadioError::SIM_FULL:
        return DataCallFailCause::INVALID_SIM_STATE;

    case RadioError::NO_NETWORK_FOUND:
        return DataCallFailCause::NETWORK_FAILURE;

    case RadioError::NETWORK_REJECT:
        return DataCallFailCause::OPERATOR_BARRED;

    case RadioError::PASSWORD_INCORRECT:
        return DataCallFailCause::USER_AUTHENTICATION;

    case RadioError::INVALID_ARGUMENTS:
    case RadioError::NO_SUCH_ELEMENT:
        return DataCallFailCause::INVALID_MANDATORY_INFO;

    case RadioError::NO_MEMORY:
        return DataCallFailCause::INSUFFICIENT_RESOURCES;

    default:
    case RadioError::GENERIC_FAILURE:
        return DataCallFailCause::ERROR_UNSPECIFIED;
    }
}


DataCallFailCause causeOf(const AtResponsePtr& response) {
    if (const AtResponse::CmeError* err = response ? response->get_if<AtResponse::CmeError>()
                                                   : nullptr) {
        return toDataCallFailCause(err->error);
    } else {
        return DataCallFailCause::ERROR_UNSPECIFIED;
    }
}

struct IpConfig {
    std::string address;
    std::vector<std::string> dnses;
};

// gtel3g: +SIPCONFIG: <cid>,<n>,"<s>","<ip>","<s>","<dns1>","<dns2>"
// Field order from phoneserver's parser [B: disassembly, at_tok_* calls];
// what fields 2, 3 and 5 hold is not known [V]. Falls back to the first
// IPv4 address after the two numbers.
IpConfig parseSipconfig(const std::string_view payload) {
    using gtel3g::isIpv4Address;

    const std::vector<std::string> p = gtel3g::splitParams(payload);
    IpConfig config;

    size_t ipIndex = 0;
    if ((p.size() > 3) && isIpv4Address(p[3])) {
        ipIndex = 3;
    } else {
        for (size_t i = 2; i < p.size(); ++i) {
            if (isIpv4Address(p[i])) {
                ipIndex = i;
                break;
            }
        }
    }

    if (ipIndex == 0) {
        return config;
    }
    config.address = p[ipIndex];

    for (const size_t i : { size_t(5), size_t(6) }) {
        if ((i < p.size()) && (i != ipIndex) && isIpv4Address(p[i])) {
            config.dnses.push_back(p[i]);
        }
    }
    if (config.dnses.empty()) {
        for (size_t i = ipIndex + 1; i < p.size(); ++i) {
            if (isIpv4Address(p[i]) && (p[i] != config.address)) {
                config.dnses.push_back(p[i]);
            }
        }
    }

    return config;
}

} // namespace

RadioData::RadioData(std::shared_ptr<AtChannel> atChannel) : mAtChannel(std::move(atChannel)) {
}

ScopedAStatus RadioData::getSlicingConfig(const int32_t serial) {
    // matches reference-ril.c
    NOT_NULL(mRadioDataResponse)->getSlicingConfigResponse(
        makeRadioResponseInfo(serial), {});
    return ScopedAStatus::ok();
}

ScopedAStatus RadioData::setDataAllowed(const int32_t serial,
                                        const bool /*allow*/) {
    // matches reference-ril.c
    NOT_NULL(mRadioDataResponse)->setDataAllowedResponse(
        makeRadioResponseInfo(serial));
    return ScopedAStatus::ok();
}

ScopedAStatus RadioData::setDataProfile(const int32_t serial,
                                        const std::vector<DataProfileInfo>& /*profiles*/) {
    // matches reference-ril.c
    NOT_NULL(mRadioDataResponse)->setDataProfileResponse(
        makeRadioResponseInfo(serial));
    return ScopedAStatus::ok();
}

ScopedAStatus RadioData::setDataThrottling(const int32_t serial,
                                           const data::DataThrottlingAction /*dataThrottlingAction*/,
                                           const int64_t /*completionDurationMillis*/) {
    // matches reference-ril.c
    NOT_NULL(mRadioDataResponse)->setDataThrottlingResponse(
        makeRadioResponseInfo(serial));
    return ScopedAStatus::ok();
}

ScopedAStatus RadioData::setInitialAttachApn(const int32_t serial,
                                             const std::optional<DataProfileInfo>& /*maybeDpInfo*/) {
    // matches reference-ril.c
    NOT_NULL(mRadioDataResponse)->setInitialAttachApnResponse(
        makeRadioResponseInfo(serial));
    return ScopedAStatus::ok();
}

ScopedAStatus RadioData::allocatePduSessionId(const int32_t serial) {
    NOT_NULL(mRadioDataResponse)->allocatePduSessionIdResponse(
        makeRadioResponseInfoUnsupported(  // matches reference-ril.c
            serial, FAILURE_DEBUG_PREFIX, __func__), 0);
    return ScopedAStatus::ok();
}

ScopedAStatus RadioData::releasePduSessionId(const int32_t serial,
                                             const int32_t /*id*/) {
    NOT_NULL(mRadioDataResponse)->releasePduSessionIdResponse(
        makeRadioResponseInfo(serial));
    return ScopedAStatus::ok();
}

ScopedAStatus RadioData::setupDataCall(const int32_t serial,
                                       const AccessNetwork /*accessNetwork*/,
                                       const DataProfileInfo& dataProfileInfo,
                                       const bool /*roamingAllowed*/,
                                       const data::DataRequestReason /*reason*/,
                                       const std::vector<data::LinkAddress>& /*addresses*/,
                                       const std::vector<std::string>& /*dnses*/,
                                       const int32_t pduSessionId,
                                       const std::optional<data::SliceInfo>& /*sliceInfo*/,
                                       const bool /*matchAllRuleAllowed*/) {
    const int32_t cid = allocateId(mCallIdAllocator);

    mAtChannel->queueRequester([this, serial, cid, dataProfileInfo, pduSessionId]
                               (const AtChannel::RequestPipe requestPipe) -> bool {
        using namespace std::chrono_literals;
        using CmeError = AtResponse::CmeError;
        using Unknown = AtResponse::Unknown;

        // gtel3g: only IPv4 for now [V]: IPV4V6 and IPV6 profiles get an IP
        // context (the CP's IPv6 answer format is not known)
        SetupDataCallResult result = {
            .suggestedRetryTime = -1,
            .cid = cid,
            .active = SetupDataCallResult::DATA_CONNECTION_STATUS_ACTIVE,
            .type = PdpProtocolType::IP,
            .ifname = getInterfaceName(cid),
            .mtuV4 = kMtu,
            .mtuV6 = kMtu,
            .handoverFailureMode = SetupDataCallResult::HANDOVER_FAILURE_MODE_LEGACY,
            .pduSessionId = pduSessionId,
        };

        // a refused context is a result with a cause, not a failed request
        const auto fail = [&](const DataCallFailCause cause) -> bool {
            releaseId(mCallIdAllocator, cid);
            result.cause = cause;
            result.cid = -1;
            result.active = SetupDataCallResult::DATA_CONNECTION_STATUS_INACTIVE;
            result.suggestedRetryTime = 10000;
            result.addresses.clear();
            result.dnses.clear();
            result.gateways.clear();
            NOT_NULL(mRadioDataResponse)->setupDataCallResponse(
                    makeRadioResponseInfo(serial), result);
            return true;
        };

        const auto deactivate = [&]() {
            mAtConversation(requestPipe, std::format("AT+CGACT=0,{0:d}", cid),
                            [](const AtResponse& response) -> bool {
                                return response.isOK() || response.isFinalError();
                            }, 30s);
        };

        if (cid > kMaxCid) {
            RLOGE("%s: no interface for cid %d", FAILURE_DEBUG_PREFIX, cid);
            return fail(DataCallFailCause::INSUFFICIENT_RESOURCES);
        } else if (dataProfileInfo.apn.empty()) {
            RLOGE("%s: APN is empty", FAILURE_DEBUG_PREFIX);
            return fail(DataCallFailCause::MISSING_UNKNOWN_APN);
        } else if (dataProfileInfo.protocol != PdpProtocolType::IP) {
            RLOGW("%s: protocol %s, using IP", FAILURE_DEBUG_PREFIX,
                  toString(dataProfileInfo.protocol).c_str());
        }

        const auto okOrCme = [](const AtResponse& response) -> bool {
            return response.isOK() || response.holds<CmeError>();
        };

        // 1. context
        AtResponsePtr response =
            mAtConversation(requestPipe,
                            std::format("AT+CGDCONT={0:d},\"IP\",\"{1:s}\",\"\",0,0",
                                        cid, dataProfileInfo.apn),
                            okOrCme);
        if (!response || !response->isOK()) {
            return fail(FAILURE(causeOf(response)));
        }

        // 2. authentication, only with a user name (vendor RIL: +CGPCO)
        if (!dataProfileInfo.user.empty()) {
            response = mAtConversation(requestPipe,
                std::format("AT+CGPCO=0,\"{0:s}\",\"{1:s}\",{2:d},{3:d}",
                            dataProfileInfo.user, dataProfileInfo.password, cid,
                            static_cast<int>(dataProfileInfo.authType)),
                okOrCme);
            if (!response || !response->isOK()) {
                RLOGW("%s: AT+CGPCO refused, going on", FAILURE_DEBUG_PREFIX);
            }
        }

        // 3. activate and switch to the Ethernet path (seth/rmnet)
        response =
            mAtConversation(requestPipe,
                            std::format("AT+CGDATA=\"M-ETHER\",{0:d}", cid),
                            [](const AtResponse& response) -> bool {
                                return response.isOK() ||
                                       response.holds<AtResponse::CONNECT>() ||
                                       response.holds<CmeError>();
                            }, 90s);
        if (!response || response->holds<CmeError>()) {
            const DataCallFailCause cause = causeOf(response);
            deactivate();
            return fail(FAILURE(cause));
        }

        // 4. address and DNS
        IpConfig ipConfig;
        response =
            mAtConversation(requestPipe, std::format("AT+SIPCONFIG={0:d}", cid),
                            [](const AtResponse& response) -> bool {
                                const Unknown* u = response.get_if<Unknown>();
                                return u && u->is("+SIPCONFIG"sv);
                            }, 10s);
        if (const Unknown* u = response ? response->get_if<Unknown>() : nullptr) {
            RLOGI("%s: +SIPCONFIG: %s", FAILURE_DEBUG_PREFIX, u->payload.c_str());
            ipConfig = parseSipconfig(u->payload);
        }

        if (ipConfig.address.empty()) {
            // 27.007 fallback: +CGPADDR: <cid>,"<address>"
            response =
                mAtConversation(requestPipe, std::format("AT+CGPADDR={0:d}", cid),
                                [](const AtResponse& response) -> bool {
                                    const Unknown* u = response.get_if<Unknown>();
                                    return u && u->is("+CGPADDR"sv);
                                }, 10s);
            if (const Unknown* u = response ? response->get_if<Unknown>() : nullptr) {
                RLOGI("%s: +CGPADDR: %s", FAILURE_DEBUG_PREFIX, u->payload.c_str());
                const std::vector<std::string> p = gtel3g::splitParams(u->payload);
                if ((p.size() >= 2) && gtel3g::isIpv4Address(p[1])) {
                    ipConfig.address = p[1];
                }
            }
        }

        if (ipConfig.address.empty()) {
            RLOGE("%s: no address for cid %d", FAILURE_DEBUG_PREFIX, cid);
            deactivate();
            return fail(DataCallFailCause::UNKNOWN_PDP_ADDRESS_TYPE);
        }

        if (ipConfig.dnses.empty()) {
            const std::string fallback = ::android::base::GetProperty(kDnsFallbackProp, "");
            for (const std::string& dns : ::android::base::Split(fallback, ",")) {
                if (gtel3g::isIpv4Address(dns)) {
                    ipConfig.dnses.push_back(dns);
                }
            }
            RLOGW("%s: no DNS from the CP, %zu from %s", FAILURE_DEBUG_PREFIX,
                  ipConfig.dnses.size(), kDnsFallbackProp);
        }

        // 5. interface: up without ARP; the address is set here as well (as
        // goldfish does), Android sets it again from the result
        if (!setInterfaceUp(result.ifname, true) ||
                !setIpAddr(ipConfig.address.c_str(), 32, result.ifname.c_str())) {
            deactivate();
            return fail(FAILURE(DataCallFailCause::ERROR_UNSPECIFIED));
        }

        result.addresses.push_back({
            .address = std::format("{0:s}/32", ipConfig.address),
            .addressProperties = 0,
            .deprecationTime = -1,
            .expirationTime = -1,
        });
        // point-to-point link: "0.0.0.0" makes Android add an on-link
        // default route (DataNetwork accepts it as gateway)
        result.gateways.push_back("0.0.0.0");
        result.dnses = std::move(ipConfig.dnses);

        {
            std::lock_guard<std::mutex> lock(mMtx);
            mDataCalls.insert({ cid, result });
        }

        NOT_NULL(mRadioDataResponse)->setupDataCallResponse(
            makeRadioResponseInfo(serial), std::move(result));

        NOT_NULL(mRadioDataIndication)->dataCallListChanged(
            RadioIndicationType::UNSOLICITED, getDataCalls());
        return true;
    });

    return ScopedAStatus::ok();
}

ScopedAStatus RadioData::deactivateDataCall(
        const int32_t serial, const int32_t cid,
        const data::DataRequestReason /*reason*/) {
    // gtel3g: goldfish only forgot the call; the CP has to drop the context
    mAtChannel->queueRequester([this, serial, cid]
                               (const AtChannel::RequestPipe requestPipe) -> bool {
        using namespace std::chrono_literals;

        bool known;
        {
            std::lock_guard<std::mutex> lock(mMtx);
            known = mDataCalls.count(cid) > 0;
        }

        if (!known) {
            NOT_NULL(mRadioDataResponse)->deactivateDataCallResponse(
                makeRadioResponseInfo(serial, FAILURE(RadioError::INVALID_ARGUMENTS)));
            return true;
        }

        mAtConversation(requestPipe, std::format("AT+CGACT=0,{0:d}", cid),
                        [](const AtResponse& response) -> bool {
                            return response.isOK() || response.isFinalError();
                        }, 30s);

        setInterfaceUp(getInterfaceName(cid), false);

        {
            std::lock_guard<std::mutex> lock(mMtx);
            if (mDataCalls.erase(cid) > 0) {
                mCallIdAllocator.put(cid);
            }
        }

        NOT_NULL(mRadioDataResponse)->deactivateDataCallResponse(
            makeRadioResponseInfo(serial));

        NOT_NULL(mRadioDataIndication)->dataCallListChanged(
            RadioIndicationType::UNSOLICITED, getDataCalls());
        return true;
    });

    return ScopedAStatus::ok();
}

// gtel3g: the network or the CP dropped a context (+CGEV: NW DEACT ...,
// +CGEV: ME DEACT ...): ask the CP which contexts are still active and
// forget the others
void RadioData::handleUnsolicited(const AtResponse::Unknown& u) {
    if (!u.is("+CGEV"sv) || (u.payload.find("DEACT") == std::string::npos)) {
        return;
    }

    RLOGI("%s: +CGEV: %s", FAILURE_DEBUG_PREFIX, u.payload.c_str());

    mAtChannel->queueRequester([this](const AtChannel::RequestPipe requestPipe) -> bool {
        using namespace std::chrono_literals;
        using CGACT = AtResponse::CGACT;

        const AtResponsePtr response =
            mAtConversation(requestPipe, "AT+CGACT?"sv,
                            [](const AtResponse& response) -> bool {
                                return response.holds<CGACT>() || response.isOK();
                            }, 10s);
        if (!response || response->isParseError()) {
            return true;
        }

        std::set<int> active;
        if (const CGACT* cgact = response->get_if<CGACT>()) {
            for (const auto& [cid, state] : cgact->states) {
                if (state == 1) {
                    active.insert(cid);
                }
            }
        }

        std::vector<int32_t> gone;
        {
            std::lock_guard<std::mutex> lock(mMtx);
            for (auto i = mDataCalls.begin(); i != mDataCalls.end(); ) {
                if (active.count(i->first)) {
                    ++i;
                } else {
                    gone.push_back(i->first);
                    mCallIdAllocator.put(i->first);
                    i = mDataCalls.erase(i);
                }
            }
        }

        for (const int32_t cid : gone) {
            RLOGI("%s: cid %d is gone", FAILURE_DEBUG_PREFIX, cid);
            setInterfaceUp(getInterfaceName(cid), false);
        }

        if (!gone.empty() && mRadioDataIndication) {
            mRadioDataIndication->dataCallListChanged(
                RadioIndicationType::UNSOLICITED, getDataCalls());
        }
        return true;
    });
}

ScopedAStatus RadioData::getDataCallList(const int32_t serial) {
    NOT_NULL(mRadioDataResponse)->getDataCallListResponse(
        makeRadioResponseInfo(serial), getDataCalls());
    return ScopedAStatus::ok();
}

ScopedAStatus RadioData::startHandover(const int32_t serial,
                                       const int32_t /*callId*/) {
    NOT_NULL(mRadioDataResponse)->startHandoverResponse(
        makeRadioResponseInfoUnsupported(  // matches reference-ril.c
            serial, FAILURE_DEBUG_PREFIX, __func__));
    return ScopedAStatus::ok();
}

ScopedAStatus RadioData::cancelHandover(const int32_t serial,
                                        const int32_t /*callId*/) {
    NOT_NULL(mRadioDataResponse)->cancelHandoverResponse(
        makeRadioResponseInfoUnsupported(  // matches reference-ril.c
            serial, FAILURE_DEBUG_PREFIX, __func__));
    return ScopedAStatus::ok();
}

ScopedAStatus RadioData::startKeepalive(const int32_t serial,
                                        const data::KeepaliveRequest& keepaliveReq) {
    const RadioError keepAliveReqCheck = validateKeepaliveRequest(keepaliveReq);
    if (keepAliveReqCheck != RadioError::NONE) {
        NOT_NULL(mRadioDataResponse)->startKeepaliveResponse(
            makeRadioResponseInfo(serial, keepAliveReqCheck), {});
        return ScopedAStatus::ok();
    }

    int32_t sessionHandle;

    {
        std::lock_guard<std::mutex> lock(mMtx);
        sessionHandle = mSessionIdAllocator.get();
        mKeepAliveSessions.insert(sessionHandle);
    }

    using data::KeepaliveStatus;

    KeepaliveStatus keepaliveStatus = {
        .sessionHandle = sessionHandle,
        .code = KeepaliveStatus::CODE_ACTIVE,
    };

    NOT_NULL(mRadioDataResponse)->startKeepaliveResponse(
        makeRadioResponseInfo(serial), std::move(keepaliveStatus));
    return ScopedAStatus::ok();
}

ScopedAStatus RadioData::stopKeepalive(const int32_t serial,
                                       const int32_t sessionHandle) {
    bool removed;
    {
        std::lock_guard<std::mutex> lock(mMtx);
        if (mKeepAliveSessions.erase(sessionHandle) > 0) {
            mSessionIdAllocator.put(sessionHandle);
            removed = true;
        } else {
            removed = false;
        }
    }

    NOT_NULL(mRadioDataResponse)->stopKeepaliveResponse(
        makeRadioResponseInfo(serial, removed ?
            RadioError::NONE : FAILURE(RadioError::INVALID_ARGUMENTS)));
    return ScopedAStatus::ok();
}

ScopedAStatus RadioData::setUserDataEnabled(int32_t serial, bool /*enabled*/) {
    NOT_NULL(mRadioDataResponse)->setUserDataEnabledResponse(
        makeRadioResponseInfoUnsupported(serial, FAILURE_DEBUG_PREFIX, __func__));
    return ScopedAStatus::ok();
}

ScopedAStatus RadioData::setUserDataRoamingEnabled(int32_t serial, bool /*enabled*/) {
    NOT_NULL(mRadioDataResponse)->setUserDataRoamingEnabledResponse(
        makeRadioResponseInfoUnsupported(serial, FAILURE_DEBUG_PREFIX, __func__));
    return ScopedAStatus::ok();
}

ScopedAStatus RadioData::notifyImsDataNetwork(int32_t serial,
                                              const data::ImsDataNetworkInfo& /*info*/) {
    NOT_NULL(mRadioDataResponse)->notifyImsDataNetworkResponse(
        makeRadioResponseInfoNOP(serial));
    return ScopedAStatus::ok();
}

ScopedAStatus RadioData::responseAcknowledgement() {
    return ScopedAStatus::ok();
}

void RadioData::atResponseSink(const AtResponsePtr& response) {
    if (!mAtConversation.send(response)) {
        response->visit([this](const auto& msg){ handleUnsolicited(msg); });
    }
}

ScopedAStatus RadioData::setResponseFunctions(
        const std::shared_ptr<data::IRadioDataResponse>& radioDataResponse,
        const std::shared_ptr<data::IRadioDataIndication>& radioDataIndication) {
    mRadioDataResponse = NOT_NULL(radioDataResponse);
    mRadioDataIndication = NOT_NULL(radioDataIndication);
    return ScopedAStatus::ok();
}

int32_t RadioData::allocateId(IdAllocator& allocator) {
    std::lock_guard<std::mutex> lock(mMtx);
    return allocator.get();
}

void RadioData::releaseId(IdAllocator& allocator, const int32_t cid) {
    std::lock_guard<std::mutex> lock(mMtx);
    allocator.put(cid);
}

RadioError RadioData::validateKeepaliveRequest(const data::KeepaliveRequest& keepaliveReq) const {
    using data::KeepaliveRequest;

    switch (keepaliveReq.type) {
    case KeepaliveRequest::TYPE_NATT_IPV4:
        if ((keepaliveReq.sourceAddress.size() != 4) || (keepaliveReq.destinationAddress.size() != 4)) {
            return RadioError::INVALID_ARGUMENTS;
        }
        break;

    case KeepaliveRequest::TYPE_NATT_IPV6:
        if ((keepaliveReq.sourceAddress.size() != 16) || (keepaliveReq.destinationAddress.size() != 16)) {
            return RadioError::INVALID_ARGUMENTS;
        }
        break;

    default:
        return RadioError::REQUEST_NOT_SUPPORTED;
    }

    std::lock_guard<std::mutex> lock(mMtx);
    if (!mDataCalls.count(keepaliveReq.cid)) {
        return RadioError::INVALID_ARGUMENTS;
    }

    return RadioError::NONE;
}

std::vector<SetupDataCallResult> RadioData::getDataCalls() const {
    std::vector<SetupDataCallResult> dataCalls;

    std::lock_guard<std::mutex> lock(mMtx);
    for (const auto& kv : mDataCalls) {
        dataCalls.push_back(kv.second);
    }

    return dataCalls;
}

}  // namespace implementation
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl

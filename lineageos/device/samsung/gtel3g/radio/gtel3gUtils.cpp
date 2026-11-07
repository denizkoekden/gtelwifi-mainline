/*
 * Copyright (C) 2026 The Android Open Source Project
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

#define FAILURE_DEBUG_PREFIX "gtel3g"

#include <arpa/inet.h>

#include "gtel3gUtils.h"
#include "debug.h"

namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace implementation {
namespace gtel3g {
using namespace std::literals;

std::vector<std::string> splitParams(const std::string_view s) {
    std::vector<std::string> result;
    std::string cur;
    bool inQuotes = false;

    for (const char c : s) {
        if (c == '"') {
            inQuotes = !inQuotes;
        } else if ((c == ',') && !inQuotes) {
            result.push_back(std::move(cur));
            cur.clear();
        } else if (inQuotes || (c > 0x20)) {
            cur.push_back(c);
        }
    }

    if (!s.empty()) {
        result.push_back(std::move(cur));
    }

    return result;
}

bool isIpv4Address(const std::string_view s) {
    const std::string str(s);
    struct in_addr addr;
    return (inet_pton(AF_INET, str.c_str(), &addr) == 1) && (addr.s_addr != 0);
}

std::string readIccid(const AtChannel::RequestPipe requestPipe,
                      AtChannel::Conversation& conversation) {
    using CRSM = AtResponse::CRSM;

    const AtResponsePtr response =
        conversation(requestPipe, "AT+CRSM=176,12258,0,0,10"sv,
                     [](const AtResponse& response) -> bool {
                         return response.holds<CRSM>();
                     });
    if (!response || response->isParseError()) {
        return FAILURE_V("", "%s", "no EF_ICCID");
    }

    const CRSM* crsm = response->get_if<CRSM>();
    if (!crsm || (crsm->sw1 != 0x90)) {
        return FAILURE_V("", "EF_ICCID: sw1=%d", crsm ? crsm->sw1 : -1);
    }

    // BCD with swapped nibbles, 'F' pads the end
    std::string iccid;
    const std::string& raw = crsm->response;
    for (size_t i = 0; (i + 1) < raw.size(); i += 2) {
        for (const char c : { raw[i + 1], raw[i] }) {
            if ((c == 'F') || (c == 'f')) {
                return iccid;
            }
            iccid.push_back(c);
        }
    }

    return iccid;
}

bool isUsimCard(const AtChannel::RequestPipe requestPipe,
                AtChannel::Conversation& conversation) {
    using Unknown = AtResponse::Unknown;

    const AtResponsePtr response =
        conversation(requestPipe, "AT^CARDMODE"sv,
                     [](const AtResponse& response) -> bool {
                         const Unknown* u = response.get_if<Unknown>();
                         return u && u->is("^CARDMODE"sv);
                     });
    const Unknown* u = response ? response->get_if<Unknown>() : nullptr;
    if (!u) {
        RLOGW("%s: no ^CARDMODE, taking USIM", FAILURE_DEBUG_PREFIX);
        return true;
    }

    RLOGI("%s: ^CARDMODE: %s", FAILURE_DEBUG_PREFIX, u->payload.c_str());
    return u->payload != "1";
}

}  // namespace gtel3g
}  // namespace implementation
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl

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
 *
 * Helpers for the Samsung SM-T561 (gtel3g) Spreadtrum CP.
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "AtChannel.h"

namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace implementation {
namespace gtel3g {

// Split AT parameters at the commas outside quotes; quotes removed, blanks
// trimmed: "1,\"a,b\", 3" -> {"1", "a,b", "3"}
std::vector<std::string> splitParams(std::string_view s);

// Dotted IPv4 address other than 0.0.0.0
bool isIpv4Address(std::string_view s);

// ICCID from EF_ICCID (2FE2) via AT+CRSM=176,12258,0,0,10 (the vendor RIL
// reads SIM files with AT+CRSM the same way). Empty on failure; Android's
// SIMRecords reads EF_ICCID again by itself.
std::string readIccid(AtChannel::RequestPipe requestPipe,
                      AtChannel::Conversation& conversation);

// true: USIM (3G card), false: SIM (2G card). From AT^CARDMODE [B: vendor
// RIL string; answer format "^CARDMODE: <n>" with 2 = USIM is V]; USIM if
// the CP does not tell.
bool isUsimCard(AtChannel::RequestPipe requestPipe,
                AtChannel::Conversation& conversation);

}  // namespace gtel3g
}  // namespace implementation
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl

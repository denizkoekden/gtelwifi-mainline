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
 * Modified for the Samsung SM-T561 (gtel3g): talks to the AT tty of the
 * Spreadtrum modem core (persist.vendor.radio.at_device, default
 * /dev/stty_w0) instead of the emulator's modem port; no IMS, IMS media or
 * SAP services (3G data and SMS only).
 */

#include <string_view>
#include <memory>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <android-base/properties.h>

#include <android/binder_interface_utils.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include "AtChannel.h"
#include "RadioConfig.h"
#include "RadioData.h"
#include "RadioMessaging.h"
#include "RadioModem.h"
#include "RadioNetwork.h"
#include "RadioSim.h"
#include "RadioVoice.h"

#include "debug.h"

namespace {
using ::android::base::unique_fd;
namespace impl = ::aidl::android::hardware::radio::implementation;
using namespace std::literals;

constexpr char kAtDeviceProp[] = "persist.vendor.radio.at_device";
constexpr char kDefaultAtDevice[] = "/dev/stty_w0";

std::string getInstanceName(const std::string_view descriptor,
                            const std::string_view slot) {
    std::string result(descriptor.data(), descriptor.size());
    result.append(1, '/');
    result.append(slot.data(), slot.size());
    return result;
}

template <class S> std::shared_ptr<S> registerService(
        const std::string_view instanceSuffix,
        std::shared_ptr<impl::AtChannel> atChannel) {
    auto serice = ndk::SharedRefBase::make<S>(std::move(atChannel));
    const std::string instanceName = getInstanceName(S::descriptor, instanceSuffix);

    if (AServiceManager_addService(serice->asBinder().get(),
                                   instanceName.c_str()) != STATUS_OK) {
        return FAILURE_V(nullptr, "Failed to register: '%s'",
                         instanceName.c_str());
    }

    return serice;
}

template <class T> void addResponseSink(impl::AtChannel& atChannel,
                                        const std::shared_ptr<T>& strongObject,
                                        void(T::*method)(const impl::AtResponsePtr&)) {
    std::weak_ptr<T> weakObject(strongObject);

    atChannel.addResponseSink([weakObject = std::move(weakObject), method]
                              (const impl::AtResponsePtr& response) -> bool {
        if (const auto strongObject = weakObject.lock()) {
            (*strongObject.*method)(response);
            return true;
        } else {
            return false;
        }
    });
}

// gtel3g: settings for the Spreadtrum CP (strings as in the vendor RIL,
// libreference-ril_sp.so). Only the first command must be answered; the
// others are logged when the CP refuses them.
bool initSequence(const impl::AtChannel::RequestPipe pipe,
                  impl::AtChannel::Conversation& conversation) {
    using impl::AtResponse;
    using impl::AtResponsePtr;

    struct InitCmd {
        std::string_view cmd;
        std::chrono::seconds timeout;
        bool required;
    };

    static const InitCmd initCmds[] = {
        { "AT"sv,               2s,  true  },  // sync, the CP may still boot
        { "ATE0Q0V1"sv,         2s,  true  },  // no echo, verbose results
        { "AT+CMEE=1"sv,        2s,  false },  // numeric +CME ERROR
        { "AT+CREG=2"sv,        2s,  false },  // +CREG with LAC/CI/AcT
        { "AT+CGREG=2"sv,       2s,  false },  // +CGREG with LAC/CI/AcT
        { "AT+CMGF=0"sv,        2s,  false },  // SMS PDU mode
        { "AT+CSMS=1"sv,        2s,  false },  // phase 2+, ack with +CNMA
        { "AT+CNMI=3,2,2,1,1"sv,2s,  false },  // SMS as +CMT/+CDS (vendor RIL)
        { "AT+CGEREP=1,0"sv,    2s,  false },  // +CGEV (PDP deactivated)
        { "AT+CTZR=1"sv,        2s,  false },  // time zone reports
        { "AT+CMOD=0"sv,        2s,  false },
        { "AT+COLP=0"sv,        2s,  false },
        // [V] SIM power on: the CP answered AT+CPIN? with +CME ERROR: 14
        // (SIM busy) as long as nothing had switched the SIM on; the vendor
        // RIL has AT+SFUN=2 and the radio on/off pair AT+SFUN=4/5
        { "AT+SFUN=2"sv,        10s, false },
        { "AT+CFUN?"sv,         2s,  false },  // initial radio state
    };

    const auto anyFinal = [](const AtResponse& response) -> bool {
        return response.endsCommand();
    };

    // gtel3g: the radio state is read on the first open only. After a
    // reopen "+CFUN: 0" (the CP does not mirror AT+SFUN=4 there [V]) would
    // switch every service to OFF while the radio is on.
    static bool firstInit = true;

    for (const InitCmd& c : initCmds) {
        if (!firstInit && (c.cmd == "AT+CFUN?"sv)) {
            continue;
        }

        AtResponsePtr response;
        for (int attempt = 0; attempt < (c.required ? 3 : 1); ++attempt) {
            response = conversation(pipe, c.cmd, anyFinal, c.timeout);
            if (response) {
                break;
            }
        }

        if (!response) {
            if (c.required) {
                RLOGE("initSequence: no answer to '%*.*s'",
                      int(c.cmd.size()), int(c.cmd.size()), c.cmd.data());
                return false;
            }
            RLOGW("initSequence: no answer to '%*.*s'",
                  int(c.cmd.size()), int(c.cmd.size()), c.cmd.data());
        } else if (!response->isOK()) {
            const std::string_view what = response->what();
            RLOGW("initSequence: '%*.*s' -> '%*.*s'",
                  int(c.cmd.size()), int(c.cmd.size()), c.cmd.data(),
                  int(what.size()), int(what.size()), what.data());
        }
    }

    firstInit = false;
    return true;
}

int mainImpl(impl::AtChannel::HostChannelFactory hostChannelFactory) {
    using impl::AtChannel;
    using impl::RadioConfig;
    using impl::RadioData;
    using impl::RadioMessaging;
    using impl::RadioModem;
    using impl::RadioNetwork;
    using impl::RadioSim;
    using impl::RadioVoice;

    const auto atChannel = std::make_shared<AtChannel>(std::move(hostChannelFactory),
                                                       &initSequence);

    static constexpr std::string_view kDefaultInstance = "default"sv;
    static constexpr std::string_view kSlot1Instance = "slot1"sv;

    ABinderProcess_setThreadPoolMaxThreadCount(2);
    ABinderProcess_startThreadPool();

    const auto radioConfig = registerService<RadioConfig>(kDefaultInstance, atChannel);
    if (!radioConfig) {
        return EXIT_FAILURE;
    }

    const auto radioData = registerService<RadioData>(kSlot1Instance, atChannel);
    if (!radioData) {
        return EXIT_FAILURE;
    }

    const auto radioMessaging = registerService<RadioMessaging>(kSlot1Instance, atChannel);
    if (!radioMessaging) {
        return EXIT_FAILURE;
    }

    const auto radioModem = registerService<RadioModem>(kSlot1Instance, atChannel);
    if (!radioModem) {
        return EXIT_FAILURE;
    }

    const auto radioNetwork = registerService<RadioNetwork>(kSlot1Instance, atChannel);
    if (!radioNetwork) {
        return EXIT_FAILURE;
    }

    const auto radioSim = registerService<RadioSim>(kSlot1Instance, atChannel);
    if (!radioSim) {
        return EXIT_FAILURE;
    }

    const auto radioVoice = registerService<RadioVoice>(kSlot1Instance, atChannel);
    if (!radioVoice) {
        return EXIT_FAILURE;
    }

    addResponseSink(*atChannel, radioConfig, &RadioConfig::atResponseSink);
    addResponseSink(*atChannel, radioData, &RadioData::atResponseSink);
    addResponseSink(*atChannel, radioMessaging, &RadioMessaging::atResponseSink);
    addResponseSink(*atChannel, radioModem, &RadioModem::atResponseSink);
    addResponseSink(*atChannel, radioNetwork, &RadioNetwork::atResponseSink);
    addResponseSink(*atChannel, radioSim, &RadioSim::atResponseSink);
    addResponseSink(*atChannel, radioVoice, &RadioVoice::atResponseSink);

    // gtel3g: open the channel and run the init sequence right away, so the
    // initial +CFUN/+CREG state is known before the framework asks
    atChannel->queueRequester([](const AtChannel::RequestPipe) -> bool { return true; });

    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;    // joinThreadPool is not expected to return
}

// gtel3g: the CP's AT channel is a SIPC spipe node, not a tty. The termios
// calls only run if it ever is one (Samsung's own stty driver), and their
// errors do not matter.
unique_fd openHostChannel(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        return FAILURE_V(unique_fd(), "Could not open '%s': %s",
                         path.c_str(), strerror(errno));
    }

    if (::isatty(fd)) {
        struct termios tio;
        if (::tcgetattr(fd, &tio) == 0) {
            ::cfmakeraw(&tio);
            ::tcsetattr(fd, TCSANOW, &tio);
        }
    }

    RLOGI("AT channel: %s", path.c_str());
    return unique_fd(fd);
}

}  // namespace

int main(int /*argc*/, char** /*argv*/) {
    std::string channelName =
        android::base::GetProperty(kAtDeviceProp, kDefaultAtDevice);
    if (channelName.empty()) {
        channelName = kDefaultAtDevice;
    }

    return mainImpl([channelName = std::move(channelName)](){
        return openHostChannel(channelName);
    });
}

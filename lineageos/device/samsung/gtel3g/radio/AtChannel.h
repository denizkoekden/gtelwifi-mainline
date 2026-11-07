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
 * Modified for the Samsung SM-T561 (gtel3g): the channel is the AT tty of
 * the Spreadtrum modem core (/dev/stty_w0, a SIPC spipe node, not a real
 * tty) instead of the emulator's modem socket. Changes are marked "gtel3g".
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include <android-base/unique_fd.h>

#include "AtResponse.h"
#include "ratUtils.h"

namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace implementation {
using ::android::base::unique_fd;

struct AtChannel {
    using HostChannelFactory = std::function<unique_fd()>;

    struct RequestPipe {
        // gtel3g: the pipe knows its channel, which serializes commands
        // (one in flight at a time) and answers the emulator-only commands
        // the goldfish code still sends (see AtChannel::answerLocally)
        RequestPipe(AtChannel* channel, int fd) : mChannel(channel), mFd(fd) {}

        bool operator()(std::string_view request) const;
        void noteTimeout() const;   // gtel3g: a conversation got no answer

        RequestPipe(const RequestPipe&) = default;
        RequestPipe& operator=(const RequestPipe&) = default;

    private:
        AtChannel* mChannel;
        int mFd;
    };

    struct Conversation {
        using FilterFunc = std::function<bool(const AtResponse&)>;
        using Duration = std::chrono::steady_clock::duration;

        // gtel3g: besides a response that passes the filter, the
        // conversation also ends on a final result code the filter does not
        // want (ERROR, +CME ERROR, ... or a stray OK). It then returns
        // nullptr at once instead of waiting for the timeout.
        AtResponsePtr operator()(RequestPipe, std::string_view request,
                                 const FilterFunc& filter, Duration timeout);
        AtResponsePtr operator()(RequestPipe, std::string_view request,
                                 const FilterFunc& filter);

        bool send(const AtResponsePtr& response);

    private:
        const FilterFunc* mFilter = nullptr;
        std::promise<AtResponsePtr> mSink;
        mutable std::mutex mMtx;
    };

    using InitSequence = std::function<bool(RequestPipe, Conversation&)>;
    using Requester = std::function<bool(RequestPipe)>;
    using ResponseSink = std::function<bool(const AtResponsePtr&)>;

    AtChannel(HostChannelFactory hostChannelFactory,
              InitSequence initSequence);
    ~AtChannel();

    void queueRequester(Requester);
    void addResponseSink(ResponseSink);

    // gtel3g: parse AT text produced here (lines ending in '\r') and hand
    // the responses to all sinks as if the modem had sent them
    void injectResponses(std::string_view text);

    // gtel3g: radio technology from the last <AcT> of +CREG/+CGREG
    ratUtils::ModemTechnology getCurrentModemTechnology() const;

    static constexpr std::chrono::seconds kDefaultTimeout{5};

private:
    void requestLoop();
    void readingLoop(int fd);
    Requester getRequester();
    void broadcastResponse(const AtResponsePtr&);

    void resetHostChannel(unique_fd fd = {});
    int getHostChannelFd() const;
    RequestPipe getHostChannelPipe();
    void parseBuffer(std::string* buf);
    const char* receiveOneResponse(const char* begin, const char* end);

    // gtel3g additions
    bool sendRequest(int fd, std::string_view request);
    bool answerLocally(std::string_view request);
    void waitForIdle();
    void setInFlight(bool inFlight);
    void noteResponse(const AtResponse& response);
    void splitLines(const char* data, size_t size, std::string* line, std::string* out) const;

    const HostChannelFactory mHostChannelFactory;
    const InitSequence mInitSequence;
    Conversation mConversation;
    unique_fd mHostChannel;
    std::deque<Requester> mRequesterQueue;
    std::condition_variable mRequesterAvailable;
    std::vector<ResponseSink> mResponseSinks;
    std::thread mRequestThread;
    std::thread mReaderThread;
    mutable std::mutex mRequestQueueMtx;
    mutable std::mutex mResponseSinksMtx;
    mutable std::mutex mHostChannelMtx;

    // gtel3g additions
    std::atomic<bool> mStopReading{false};
    std::atomic<bool> mIoError{false};
    std::atomic<bool> mAtLog{false};
    std::atomic<int> mConsecutiveTimeouts{0};   // reset by any line from the CP
    bool mInFlight = false;
    std::mutex mInFlightMtx;
    std::condition_variable mInFlightCv;
    std::atomic<int> mCurrentAcT{-1};
};

}  // namespace implementation
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl

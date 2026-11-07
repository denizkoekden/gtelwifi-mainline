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
 * Modified for the Samsung SM-T561 (gtel3g), changes marked "gtel3g":
 * - the channel is a character device (SIPC spipe, /dev/stty_w0); it is
 *   read with poll() so a reset never hangs in read()
 * - the input is cut into lines first: "\r\n" framing, empty lines and
 *   command echo dropped, the SMS prompt "> " (no line end) completed
 * - one command in flight at a time: the next one waits for the final
 *   result code of the previous one (the CP sends unsolicited +CSQ every
 *   few seconds, a late OK must not end the next conversation)
 * - commands only the emulator knows (+CTEC, +WRMP, +CCSS, +WSOS, +CUSATD)
 *   are answered here
 * - an unparsable line is dropped instead of tearing down the channel; the
 *   channel is reopened only after I/O errors or repeated failures
 * - AT trace in the radio log with persist.vendor.radio.at_log=1
 */

#define FAILURE_DEBUG_PREFIX "AtChannel"

#include <cctype>
#include <cstring>
#include <format>
#include <poll.h>
#include <unistd.h>

#include <android-base/properties.h>

#include "AtChannel.h"
#include "debug.h"

namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace implementation {
namespace {
using namespace std::literals;

constexpr char kCR = '\r';
constexpr char kCtrlZ = 0x1A;

// the previous command gets this long for its final result code
constexpr auto kIdleTimeout = 3s;
// a response head that does not complete is dropped after this long
constexpr auto kStuckTimeout = 15s;
// reopen the channel after this many conversations in a row without any
// line from the CP (a failed request is normal: ERROR, +CME ERROR)
constexpr int kMaxConsecutiveTimeouts = 3;
// no new channel within this time after a failed open or init
constexpr auto kReopenBackoff = 5s;
constexpr size_t kMaxBuffered = 64 * 1024;

int writeAll(const int fd, const char* data, size_t size) {
    while (size > 0) {
        const ssize_t written = ::write(fd, data, size);
        if (written >= 0) {
            data += written;
            size -= written;
        } else if (errno == EINTR) {
            continue;
        } else {
            return FAILURE(errno);
        }
    }

    return 0;
}

std::string printable(const std::string_view s) {
    std::string r;
    r.reserve(s.size());
    for (const char c : s) {
        if (c == kCR) {
            r += "\\r";
        } else if (c == kCtrlZ) {
            r += "^Z";
        } else if ((c < 0x20) || (c > 0x7E)) {
            r += '?';
        } else {
            r += c;
        }
    }
    return r;
}

std::string toUpper(const std::string_view s) {
    std::string r(s);
    for (char& c : r) {
        c = std::toupper(static_cast<unsigned char>(c));
    }
    return r;
}

bool isEcho(const std::string_view line) {
    return (line.size() >= 2) &&
           (std::toupper(static_cast<unsigned char>(line[0])) == 'A') &&
           (std::toupper(static_cast<unsigned char>(line[1])) == 'T');
}

std::chrono::steady_clock::time_point gLastFailedOpen;
}  // namespace

bool AtChannel::RequestPipe::operator()(const std::string_view request) const {
    return mChannel->sendRequest(mFd, request);
}

void AtChannel::RequestPipe::noteTimeout() const {
    ++mChannel->mConsecutiveTimeouts;
}

AtChannel::AtChannel(HostChannelFactory hostChannelFactory,
                     InitSequence initSequence)
        : mHostChannelFactory(std::move(hostChannelFactory))
        , mInitSequence(std::move(initSequence)) {
    mRequestThread = std::thread(&AtChannel::requestLoop, this);
}

AtChannel::~AtChannel() {
    queueRequester({});
    mRequestThread.join();
    if (mReaderThread.joinable()) {
        mStopReading = true;
        mReaderThread.join();
    }
}

void AtChannel::queueRequester(Requester requester) {
    std::lock_guard<std::mutex> lock(mRequestQueueMtx);
    mRequesterQueue.push_back(std::move(requester));
    mRequesterAvailable.notify_one();
}

void AtChannel::addResponseSink(ResponseSink responseSink) {
    std::lock_guard<std::mutex> lock(mResponseSinksMtx);
    mResponseSinks.push_back(std::move(responseSink));
}

void AtChannel::requestLoop() {
    while (true) {
        const Requester requester = getRequester();
        if (!requester) {
            break;
        }

        requester(getHostChannelPipe());

        // gtel3g: goldfish reopened the channel after every failed request.
        // With the CP, ERROR/+CME ERROR are normal answers; reopening (and
        // the init sequence) would only lose state. Only I/O errors or a CP
        // that stopped answering at all count.
        if (mIoError || (mConsecutiveTimeouts >= kMaxConsecutiveTimeouts)) {
            RLOGE("%s: resetting the channel (io error: %d, timeouts: %d)",
                  FAILURE_DEBUG_PREFIX, mIoError.load(), mConsecutiveTimeouts.load());
            resetHostChannel();
            mConsecutiveTimeouts = 0;
        }
    }

    resetHostChannel();
}

void AtChannel::readingLoop(const int hostChannelFd) {
    std::string line;
    std::string lines;
    std::string parseBuf;
    std::optional<std::chrono::steady_clock::time_point> stuckSince;

    while (!mStopReading) {
        struct pollfd pfd = {
            .fd = hostChannelFd,
            .events = POLLIN,
            .revents = 0,
        };

        const int r = ::poll(&pfd, 1, 500);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            RLOGE("%s: poll failed: %s", FAILURE_DEBUG_PREFIX, ::strerror(errno));
            break;
        } else if (r == 0) {
            // nothing new: a head that waits for more lines for too long
            // (a multi-line answer without OK) is given up line by line
            if (stuckSince && (std::chrono::steady_clock::now() - *stuckSince > kStuckTimeout)) {
                const size_t cr = parseBuf.find(kCR);
                RLOGE("%s: dropping stuck line '%s'", FAILURE_DEBUG_PREFIX,
                      printable(parseBuf.substr(0, cr)).c_str());
                parseBuf.erase(0, (cr == parseBuf.npos) ? parseBuf.size() : (cr + 1));
                parseBuffer(&parseBuf);
                stuckSince.reset();
                if (!parseBuf.empty()) {
                    stuckSince = std::chrono::steady_clock::now();
                }
            }
            continue;
        } else if (pfd.revents & POLLNVAL) {
            break;
        } else if (!(pfd.revents & POLLIN)) {
            if (pfd.revents & (POLLERR | POLLHUP)) {
                RLOGE("%s: poll revents=0x%x", FAILURE_DEBUG_PREFIX, pfd.revents);
                break;
            }
            continue;
        }

        char buf[1024];
        const ssize_t len = ::read(hostChannelFd, buf, sizeof(buf));
        if (len < 0) {
            if ((errno == EINTR) || (errno == EAGAIN)) {
                continue;
            }
            RLOGE("%s: read failed: %s", FAILURE_DEBUG_PREFIX, ::strerror(errno));
            break;
        } else if (len == 0) {
            ::usleep(100000);
            continue;
        }

        lines.clear();
        splitLines(buf, len, &line, &lines);
        if (!lines.empty()) {
            parseBuf += lines;
            parseBuffer(&parseBuf);
            if (parseBuf.empty()) {
                stuckSince.reset();
            } else if (!stuckSince) {
                stuckSince = std::chrono::steady_clock::now();
            }
        }
    }

    if (!mStopReading) {
        mIoError = true;
        setInFlight(false);
    }
}

// gtel3g: cut the byte stream into lines ("\r\n", "\r" or "\n"), drop empty
// lines and echoed commands, complete the SMS prompt. *line keeps a line
// that is not finished yet, *out gets complete lines ending in '\r' (the
// framing the goldfish parser expects).
void AtChannel::splitLines(const char* data, const size_t size,
                           std::string* line, std::string* out) const {
    for (size_t i = 0; i < size; ++i) {
        const char c = data[i];
        if ((c == '\r') || (c == '\n')) {
            if (!line->empty()) {
                if (mAtLog) {
                    RLOGD("AT< %s", printable(*line).c_str());
                }
                if (!isEcho(*line)) {
                    out->append(*line);
                    out->push_back(kCR);
                }
                line->clear();
            }
        } else if (c != '\0') {
            line->push_back(c);
        }
    }

    if ((*line == "> ") || (*line == ">")) {
        if (mAtLog) {
            RLOGD("AT< > (prompt)");
        }
        out->append("> \r");
        line->clear();
    } else if (line->size() > 4096) {
        RLOGE("%s: line too long, dropped", FAILURE_DEBUG_PREFIX);
        line->clear();
    }
}

void AtChannel::parseBuffer(std::string* buf) {
    size_t pos = 0;
    while (pos < buf->size()) {
        const char* const begin = buf->data() + pos;
        const char* const end = buf->data() + buf->size();
        const char* const next = receiveOneResponse(begin, end);
        if (next == begin) {
            break;  // incomplete, wait for more
        } else if (next == nullptr) {
            const void* const cr = ::memchr(begin, kCR, end - begin);
            const size_t len = cr ? (static_cast<const char*>(cr) - begin) : (end - begin);
            RLOGE("%s: can't parse '%s', dropped", FAILURE_DEBUG_PREFIX,
                  printable(std::string_view(begin, len)).c_str());
            pos += cr ? (len + 1) : len;
        } else {
            pos = next - buf->data();
        }
    }

    buf->erase(0, pos);
    if (buf->size() > kMaxBuffered) {
        RLOGE("%s: %zu bytes unparsed, dropped", FAILURE_DEBUG_PREFIX, buf->size());
        buf->clear();
    }
}

AtChannel::Requester AtChannel::getRequester() {
    std::unique_lock<std::mutex> lock(mRequestQueueMtx);
    while (true) {
        if (!mRequesterQueue.empty()) {
            Requester requester(std::move(mRequesterQueue.front()));
            mRequesterQueue.pop_front();
            return requester;
        } else {
            mRequesterAvailable.wait(lock);
        }
    }
}

void AtChannel::broadcastResponse(const AtResponsePtr& response) {
    mConsecutiveTimeouts = 0;
    mConversation.send(response);

    {
        std::lock_guard<std::mutex> lock(mResponseSinksMtx);

        const auto newEnd = std::remove_if(mResponseSinks.begin(), mResponseSinks.end(),
            [&response](const ResponseSink& responseSink) -> bool {
                return !responseSink(response);
            });

        mResponseSinks.erase(newEnd, mResponseSinks.end());
    }

    noteResponse(*response);
}

void AtChannel::injectResponses(const std::string_view text) {
    if (mAtLog) {
        RLOGD("AT< (local) %s", printable(text).c_str());
    }

    std::string buf(text);
    parseBuffer(&buf);
    if (!buf.empty()) {
        RLOGE("%s: incomplete local answer '%s'", FAILURE_DEBUG_PREFIX,
              printable(buf).c_str());
    }
}

// gtel3g: commands the goldfish code sends that only the emulator's modem
// knows. Answered locally in the emulator's format, so RadioNetwork & co.
// keep working unchanged. The CP itself is 3G (GSM + WCDMA).
bool AtChannel::answerLocally(const std::string_view request) {
    const std::string r = toUpper(request);
    const unsigned current = 1U << static_cast<unsigned>(getCurrentModemTechnology());
    constexpr unsigned kSupported = (1U << static_cast<unsigned>(ratUtils::ModemTechnology::GSM)) |
                                    (1U << static_cast<unsigned>(ratUtils::ModemTechnology::WCDMA));

    std::string answer;
    if (r == "AT+CTEC=?") {
        answer = std::format("+CTEC: {0:d},{1:d}\rOK\r",
                             static_cast<int>(ratUtils::ModemTechnology::GSM),
                             static_cast<int>(ratUtils::ModemTechnology::WCDMA));
    } else if (r == "AT+CTEC?") {
        answer = std::format("+CTEC: {0:d},\"{1:X}\"\rOK\r", current, kSupported);
    } else if (r.starts_with("AT+CTEC=")) {
        // [V] the preferred network type is not passed to the CP yet (the
        // vendor RIL used AT^SYSCONFIG=<mode>,3,2,4)
        answer = std::format("+CTEC: {0:d}\rOK\r", current);
    } else if (r == "AT+WRMP?") {
        answer = "+WRMP: 0\rOK\r";
    } else if (r == "AT+CCSS?") {
        answer = "+CCSS: 0\rOK\r";
    } else if (r == "AT+CUSATD?") {
        answer = "+CUSATD: 0,0\rOK\r";
    } else if (r == "AT+CEREG?") {
        answer = "+CEREG: 0,4\rOK\r";
    } else if (r.starts_with("AT+WRMP=") || r.starts_with("AT+CCSS=") ||
               r.starts_with("AT+WSOS") || r.starts_with("AT+CEREG=")) {
        answer = "OK\r";
    } else {
        return false;
    }

    if (mAtLog) {
        RLOGD("AT> (local) %s", printable(request).c_str());
    }
    injectResponses(answer);
    return true;
}

bool AtChannel::sendRequest(const int fd, const std::string_view request) {
    waitForIdle();

    if (answerLocally(request)) {
        return true;
    } else if (fd < 0) {
        return FAILURE_V(false, "no channel for '%s'", printable(request).c_str());
    }

    if (mAtLog) {
        RLOGD("AT> %s", printable(request).c_str());
    }

    setInFlight(true);
    int err = writeAll(fd, request.data(), request.size());
    // the PDU after the SMS prompt ends with Ctrl-Z, no CR after it
    if ((err == 0) && (request.empty() || (request.back() != kCtrlZ))) {
        err = writeAll(fd, &kCR, 1);
    }

    if (err != 0) {
        setInFlight(false);
        mIoError = true;
        return false;
    }

    return true;
}

void AtChannel::waitForIdle() {
    std::unique_lock<std::mutex> lock(mInFlightMtx);
    if (!mInFlightCv.wait_for(lock, kIdleTimeout, [this]{ return !mInFlight; })) {
        RLOGW("%s: no final result for the previous command, going on",
              FAILURE_DEBUG_PREFIX);
        mInFlight = false;
    }
}

void AtChannel::setInFlight(const bool inFlight) {
    std::lock_guard<std::mutex> lock(mInFlightMtx);
    mInFlight = inFlight;
    mInFlightCv.notify_all();
}

void AtChannel::noteResponse(const AtResponse& response) {
    if (const AtResponse::CREG* creg = response.get_if<AtResponse::CREG>()) {
        if (creg->networkType >= 0) {
            mCurrentAcT = creg->networkType;
        }
    } else if (const AtResponse::CGREG* cgreg = response.get_if<AtResponse::CGREG>()) {
        if (cgreg->networkType >= 0) {
            mCurrentAcT = cgreg->networkType;
        }
    }

    if (response.endsCommand()) {
        setInFlight(false);
    }
}

ratUtils::ModemTechnology AtChannel::getCurrentModemTechnology() const {
    using ratUtils::ModemTechnology;

    switch (mCurrentAcT.load()) {
    case 0:     // GSM
    case 1:     // GSM compact
    case 3:     // GSM w/EGPRS
        return ModemTechnology::GSM;
    case 7:     // E-UTRAN (not on this CP)
        return ModemTechnology::LTE;
    case 2:     // UTRAN
    case 4:     // UTRAN w/HSDPA
    case 5:     // UTRAN w/HSUPA
    case 6:     // UTRAN w/HSDPA and HSUPA
    default:    // [V] unknown until the first +CREG/+CGREG with <AcT>
        return ModemTechnology::WCDMA;
    }
}

void AtChannel::resetHostChannel(unique_fd fd) {
    if (mReaderThread.joinable()) {
        mStopReading = true;
        mReaderThread.join();
    }

    std::lock_guard<std::mutex> lock(mHostChannelMtx);
    mHostChannel = std::move(fd);
}

int AtChannel::getHostChannelFd() const {
    std::lock_guard<std::mutex> lock(mHostChannelMtx);
    return mHostChannel.get();
}

AtChannel::RequestPipe AtChannel::getHostChannelPipe() {
    int hostChannelFd = getHostChannelFd();
    if ((hostChannelFd >= 0) && mIoError) {
        resetHostChannel();
        hostChannelFd = -1;
    }

    if (hostChannelFd < 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now - gLastFailedOpen < kReopenBackoff) {
            return RequestPipe(this, -1);
        }

        unique_fd newHostChannel = mHostChannelFactory();
        if (!newHostChannel.ok()) {
            gLastFailedOpen = now;
            return RequestPipe(this, FAILURE_V(-1, "%s", "mHostChannelFactory failed"));
        }

        hostChannelFd = newHostChannel.get();
        {
            std::lock_guard<std::mutex> lock(mHostChannelMtx);
            mHostChannel = std::move(newHostChannel);
        }

        mAtLog = ::android::base::GetBoolProperty("persist.vendor.radio.at_log", false);
        mStopReading = false;
        mIoError = false;
        setInFlight(false);

        mReaderThread = std::thread([this, hostChannelFd](){
            readingLoop(hostChannelFd);
        });

        // gtel3g: no LOG_ALWAYS_FATAL: init restarts us anyway, and a CP that
        // is still booting would crash-loop the service
        if (!mInitSequence(RequestPipe(this, hostChannelFd), mConversation)) {
            RLOGE("%s: can't init the AT channel", FAILURE_DEBUG_PREFIX);
            gLastFailedOpen = std::chrono::steady_clock::now();
            resetHostChannel();
            return RequestPipe(this, -1);
        }
    }

    return RequestPipe(this, hostChannelFd);
}

const char* AtChannel::receiveOneResponse(const char* const begin, const char* const end) {
    switch (*begin) {
    case '\r':
    case '\n':
        return begin + 1;
    }

    auto [consumed, response] = AtResponse::parse(std::string_view(begin, end - begin));
    if (response) {
        broadcastResponse(response);
    }

    return (consumed >= 0) ? (begin + consumed) : nullptr;
}

AtResponsePtr AtChannel::Conversation::operator()(
        const RequestPipe requestPipe,
        const std::string_view request,
        const AtChannel::Conversation::FilterFunc& filter,
        const AtChannel::Conversation::Duration timeout) {
    std::future<AtResponsePtr> futureResponse;

    {
        std::lock_guard<std::mutex> lock(mMtx);
        mFilter = &filter;
        mSink = decltype(mSink)();
        futureResponse = mSink.get_future();
    }

    if (!requestPipe(request)) {
        std::lock_guard<std::mutex> lock(mMtx);
        mFilter = nullptr;
        return nullptr;
    } else if (futureResponse.wait_for(timeout) == std::future_status::ready) {
        return futureResponse.get();
    } else {
        {
            std::lock_guard<std::mutex> lock(mMtx);
            mFilter = nullptr;
        }

        // the answer may have come in between
        if (futureResponse.wait_for(0s) == std::future_status::ready) {
            return futureResponse.get();
        }

        requestPipe.noteTimeout();

        const int requestLen = request.size();
        return FAILURE_V(nullptr, "Timeout for '%*.*s'",
                         requestLen, requestLen, request.data());
    }
}

AtResponsePtr AtChannel::Conversation::operator()(
        const RequestPipe requestPipe,
        const std::string_view request,
        const FilterFunc& filter) {
    return (*this)(requestPipe, request, filter, kDefaultTimeout);
}

bool AtChannel::Conversation::send(const AtResponsePtr& response) {
    std::lock_guard<std::mutex> lock(mMtx);
    if (!mFilter) {
        return false;
    } else if ((*mFilter)(*response)) {
        mFilter = nullptr;
        mSink.set_value(response);
        return true;
    } else if (response->endsCommand()) {
        // gtel3g: the command is over without the answer the caller wants
        // (ERROR, +CME ERROR, ...): no point in waiting for the timeout
        const std::string_view what = response->what();
        RLOGW("%s: command ended with '%*.*s'", FAILURE_DEBUG_PREFIX,
              int(what.size()), int(what.size()), what.data());
        mFilter = nullptr;
        mSink.set_value(nullptr);
        return true;
    } else {
        return false;
    }
}

}  // namespace implementation
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl

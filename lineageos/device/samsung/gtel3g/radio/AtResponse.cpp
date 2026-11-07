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

#define FAILURE_DEBUG_PREFIX "AtResponse"

// Changes for the SM-T561 (gtel3g) Spreadtrum CP are marked "gtel3g".

#include <algorithm>
#include <charconv>
#include <format>
#include <numeric>
#include <string>
#include <string_view>
#include <tuple>
#include <optional>
#include <vector>

#include "atCmds.h"
#include "AtResponse.h"
#include "Parser.h"
#include "debug.h"
#include "hexbin.h"

namespace aidl {
namespace android {
namespace hardware {
namespace radio {
namespace implementation {
using namespace std::literals;

namespace {
constexpr char kCR = '\r';
constexpr std::string_view krOKr = "\rOK\r"sv;

struct ValueParser {
    std::string_view cmd;
    AtResponsePtr (*parse)(std::string_view str);
    bool multiline;
};

struct CmdIdVisitor {
    std::string_view operator()(const AtResponse::OK&) const {
        return "OK"sv;
    }

    std::string_view operator()(const AtResponse::ERROR&) const {
        return "ERROR"sv;
    }

    std::string_view operator()(const AtResponse::RING&) const {
        return "RING"sv;
    }

    std::string_view operator()(const AtResponse::SmsPrompt&) const {
        return "SmsPrompt"sv;
    }

    std::string_view operator()(const AtResponse::ParseError&) const {
        return "ParseError"sv;
    }

    std::string_view operator()(const std::string&) const {
        return "string"sv;
    }

    template <class T> std::string_view operator()(const T&) const {
        return T::id();
    }
};

std::string_view ltrim(std::string_view s) {
    while (!s.empty()) {
        if (s.front() <= 0x20) {
            s.remove_prefix(1);
        } else {
            break;
        }
    }
    return s;
}

std::string toString(std::string_view s) {
    return std::string(s.data(), s.size());
}

// gtel3g: one AT parameter, quotes removed, blanks trimmed
struct Param {
    std::string value;
    bool quoted = false;
};

// gtel3g: split "a,\"b,c\",d" at the commas outside quotes
std::vector<Param> splitParamsQ(const std::string_view s) {
    std::vector<Param> result;
    Param cur;
    bool inQuotes = false;
    bool any = false;

    for (const char c : s) {
        any = true;
        if (c == '"') {
            inQuotes = !inQuotes;
            cur.quoted = true;
        } else if ((c == ',') && !inQuotes) {
            result.push_back(std::move(cur));
            cur = Param();
        } else if (inQuotes || (c > 0x20)) {
            cur.value.push_back(c);
        }
    }

    if (any) {
        result.push_back(std::move(cur));
    }

    return result;
}

bool parseIntParam(const std::string_view s, int* result, const int base = 10) {
    if (s.empty()) {
        return false;
    }
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), *result, base);
    return (ec == std::errc()) && (ptr == s.data() + s.size());
}

// gtel3g: a line without a parser, never fatal (see AtResponse::Unknown)
AtResponsePtr makeUnknown(const std::string_view line) {
    AtResponse::Unknown u;
    if (!line.empty() && ((line.front() == '+') || (line.front() == '^') ||
                          (line.front() == '%'))) {
        const size_t colon = line.find(':');
        if (colon != std::string_view::npos) {
            u.cmd = toString(line.substr(0, colon));
            u.payload = toString(ltrim(line.substr(colon + 1)));
        } else {
            u.cmd = toString(line);
        }
    } else {
        u.payload = toString(line);
    }

    return AtResponse::make(std::move(u));
}

AtResponse::ParseResult parseCmds(const std::string_view str,
                                  const ValueParser* vp,
                                  const ValueParser* const vpEnd) {
    const std::string_view str1 = str.substr(1);  // skip + or %
    if (str1.empty()) {
        return { 0, nullptr };
    }

    bool maybeIncomplete = false;
    for (; vp != vpEnd; ++vp) {
        const std::string_view& cmd = vp->cmd;

        if (str1.starts_with(cmd.substr(0, str1.size()))) {
            size_t skipSize;
            std::string_view payload;

            if (str1.size() <= cmd.size()) {
                maybeIncomplete = true;
                continue;
            } else if (str1[cmd.size()] == ':') {
                skipSize = 1 + cmd.size() + 1; // `+CMD:`
            } else if (str1[cmd.size()] == '\r') {
                skipSize = 1 + cmd.size(); // `+CMD`
            } else {
                continue;
            }

            int consumed;

            if (vp->multiline) {
                const size_t payloadEnd = str.find(krOKr, skipSize);
                if (payloadEnd != str.npos) {
                    // keep '+CMD:' and add extra '\r' to keep lines consistent
                    payload = str.substr(0, payloadEnd + 1);
                    consumed = payloadEnd + krOKr.size();
                } else {
                    // gtel3g: the command failed after some lines; give the
                    // lines up as a parse error and let the error line
                    // through as the final result
                    size_t errPos = str.npos;
                    for (const std::string_view e : { "\rERROR\r"sv, "\r+CME ERROR:"sv,
                                                      "\r+CMS ERROR:"sv }) {
                        errPos = std::min(errPos, str.find(e, skipSize));
                    }
                    if (errPos != str.npos) {
                        AtResponse::ParseError parseError = {
                            .cmd = vp->cmd,
                        };
                        return { int(errPos + 1), AtResponse::make(std::move(parseError)) };
                    }
                    return { 0, nullptr };
                }
            } else {
                const size_t payloadEnd = str.find(kCR, skipSize);
                if (payloadEnd != str.npos) {
                    payload = ltrim(str.substr(skipSize, payloadEnd - skipSize));
                    consumed = payloadEnd + 1;
                } else {
                    return { 0, nullptr };
                }
            }

            return { consumed, (*vp->parse)(payload) };
        }
    }

    if (maybeIncomplete) {
        return { 0, nullptr };
    }

    // gtel3g: no parser for this command: one line of AtResponse::Unknown
    // (the goldfish code returned -1 here and dropped the channel)
    const size_t cr = str.find(kCR);
    if (cr == str.npos) {
        return { 0, nullptr };
    }

    return { int(cr + 1), makeUnknown(str.substr(0, cr)) };
}
}  // namespace

AtResponse::ParseResult AtResponse::parse(const std::string_view str) {
#define CMD(C) AtResponse::C::id(), &AtResponse::C::parse
    static const ValueParser plusValueParsers[] = {
        { CMD(CPIN),        false },
        { CMD(CPINR),       false },
        { CMD(CRSM),        false },
        { CMD(CFUN),        false },
        { CMD(CREG),        false },
        { CMD(CEREG),       false },
        { CMD(CGREG),       false },
        { CMD(CTEC),        false },
        { CMD(COPS),        true },
        { CMD(WRMP),        false },
        { CMD(CCSS),        false },
        { CMD(CSQ),         false },
        { CMD(CLCC),        true },
        { CMD(CCFCU),       true },
        { CMD(CCWA),        false },
        { CMD(CUSATD),      false },
        { CMD(CUSATP),      false },
        { CMD(CUSATE),      false },
        { CMD(CUSATT),      false },
        { CMD(CUSATEND),    false },
        { CMD(CGDCONT),     true },
        { CMD(CGACT),       true },
        { CMD(CGCONTRDP),   false },
        { CMD(CLCK),        false },
        { CMD(CSIM),        false },
        { CMD(CGLA),        false },
        { CMD(CCHC),        false },
        { CMD(CLIP),        false },
        { CMD(CLIR),        false },
        { CMD(CMUT),        false },
        { CMD(WSOS),        false },
        { CMD(CSCA),        false },
        { CMD(CSCB),        false },
        { CMD(CMGS),        false },
        { CMD(CMGW),        false },
        { CMD(CmeError),    false },
        { CMD(CmsError),    false },
    };

    static const ValueParser percentValueParsers[] = {
        { CMD(CTZV),     false },
        { CMD(CGFPCCFG), false },
    };

    static const ValueParser caretValueParsers[] = {
        { CMD(MBAU),     false },
    };
#undef CMD

    static constexpr std::string_view kRING = "RING\r"sv;
    if (str.starts_with(kRING)) {
        return { int(kRING.size()), AtResponse::make(RING()) };
    }

    static constexpr std::string_view kCMT = "+CMT:"sv;
    if (str.starts_with(kCMT)) {
        const std::string_view trimmed = ltrim(str.substr(kCMT.size()));
        const auto [consumed, response] = AtResponse::CMT::parse(trimmed);
        if (consumed > 0) {
            return { int(consumed + str.size() - trimmed.size()), std::move(response) };
        } else {
            return { consumed, nullptr };
        }
    }

    static constexpr std::string_view kCDS = "+CDS:"sv;
    if (str.starts_with(kCDS)) {
        const std::string_view trimmed = ltrim(str.substr(kCDS.size()));
        const auto [consumed, response] = AtResponse::CDS::parse(trimmed);
        if (consumed > 0) {
            return { int(consumed + str.size() - trimmed.size()), std::move(response) };
        } else {
            return { consumed, nullptr };
        }
    }

    switch (str.front()) {
    case '+': return parseCmds(str,
                               std::begin(plusValueParsers),
                               std::end(plusValueParsers));

    case '%': return parseCmds(str,
                               std::begin(percentValueParsers),
                               std::end(percentValueParsers));
    case '^': return parseCmds(str,
                               std::begin(caretValueParsers),
                               std::end(caretValueParsers));
    }

    // AtChannel turns the modem's "\r\n> " (no line end) into "> \r"
    static constexpr std::string_view kSmsPrompt = "> \r"sv;
    if (str.starts_with(kSmsPrompt)) {
        return { int(kSmsPrompt.size()), AtResponse::make(SmsPrompt()) };
    }

    static constexpr std::string_view kOKr = "OK\r"sv;
    if (str.starts_with(kOKr)) {
        return { int(kOKr.size()), AtResponse::make(OK()) };
    }

    static constexpr std::string_view kERRORr = "ERROR\r"sv;
    if (str.starts_with(kERRORr)) {
        return { int(kERRORr.size()), AtResponse::make(ERROR()) };
    }

    const size_t cr = str.find(kCR);
    if (cr == str.npos) {
        return { 0, nullptr };
    }
    const std::string_view line = str.substr(0, cr);

    // gtel3g: final result codes of AT+CGDATA / ATD
    if (line.starts_with("CONNECT"sv)) {
        return { int(cr + 1), AtResponse::make(CONNECT()) };
    }
    for (const std::string_view f : { "NO CARRIER"sv, "BUSY"sv, "NO ANSWER"sv,
                                      "NO DIALTONE"sv }) {
        if (line == f) {
            NOCARRIER nocarrier = {
                .text = toString(line),
            };
            return { int(cr + 1), AtResponse::make(std::move(nocarrier)) };
        }
    }

    // gtel3g: a plain line (IMSI, IMEI, AT+CGMR) counts as the answer only
    // if OK follows right after it; a plain line followed by anything else
    // is an unsolicited or unknown line of its own. The goldfish code took
    // everything up to the next "\rOK\r".
    const std::string_view rest = str.substr(cr + 1);
    if (rest.starts_with(kOKr)) {
        return { int(cr + 1 + kOKr.size()), make(toString(line)) };
    } else if (rest.find(kCR) != rest.npos) {
        return { int(cr + 1), makeUnknown(line) };
    }

    return { 0, nullptr };
}

bool AtResponse::endsCommand() const {
    if (std::holds_alternative<OK>(value) ||
            std::holds_alternative<ERROR>(value) ||
            std::holds_alternative<CmeError>(value) ||
            std::holds_alternative<CmsError>(value) ||
            std::holds_alternative<CONNECT>(value) ||
            std::holds_alternative<NOCARRIER>(value) ||
            std::holds_alternative<SmsPrompt>(value) ||
            std::holds_alternative<std::string>(value) ||
            // multi-line parsers swallow the trailing OK
            std::holds_alternative<COPS>(value) ||
            std::holds_alternative<CLCC>(value) ||
            std::holds_alternative<CCFCU>(value) ||
            std::holds_alternative<CGDCONT>(value) ||
            std::holds_alternative<CGACT>(value)) {
        return true;
    } else if (const ParseError* e = std::get_if<ParseError>(&value)) {
        return (e->cmd == COPS::id()) || (e->cmd == CLCC::id()) ||
               (e->cmd == CCFCU::id()) || (e->cmd == CGDCONT::id()) ||
               (e->cmd == CGACT::id());
    } else {
        return false;
    }
}

bool AtResponse::isFinalError() const {
    return std::holds_alternative<ERROR>(value) ||
           std::holds_alternative<CmeError>(value) ||
           std::holds_alternative<CmsError>(value) ||
           std::holds_alternative<NOCARRIER>(value);
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CmeError"
AtResponsePtr AtResponse::CmeError::parse(const std::string_view str) {
    RadioError err;

    if (str.compare(atCmds::kCmeErrorOperationNotAllowed) == 0) {
        err = RadioError::OPERATION_NOT_ALLOWED;
    } else if (str.compare(atCmds::kCmeErrorOperationNotSupported) == 0) {
        err = RadioError::REQUEST_NOT_SUPPORTED;
    } else if (str.compare(atCmds::kCmeErrorSimNotInserted) == 0) {
        err = RadioError::SIM_ABSENT;
    } else if (str.compare(atCmds::kCmeErrorSimPinRequired) == 0) {
        err = RadioError::SIM_PIN2;
    } else if (str.compare(atCmds::kCmeErrorSimPukRequired) == 0) {
        err = RadioError::SIM_PUK2;
    } else if (str.compare(atCmds::kCmeErrorSimBusy) == 0) {
        err = RadioError::SIM_BUSY;
    } else if (str.compare(atCmds::kCmeErrorIncorrectPassword) == 0) {
        err = RadioError::PASSWORD_INCORRECT;
    } else if (str.compare(atCmds::kCmeErrorMemoryFull) == 0) {
        err = RadioError::SIM_FULL;
    } else if (str.compare(atCmds::kCmeErrorInvalidIndex) == 0) {
        err = RadioError::INVALID_ARGUMENTS;
    } else if (str.compare(atCmds::kCmeErrorNotFound) == 0) {
        err = RadioError::NO_SUCH_ELEMENT;
    } else if (str.compare(atCmds::kCmeErrorInvalidCharactersInTextString) == 0) {
        err = RadioError::GENERIC_FAILURE;
    } else if (str.compare(atCmds::kCmeErrorNoNetworkService) == 0) {
        err = RadioError::NO_NETWORK_FOUND;
    } else if (str.compare(atCmds::kCmeErrorNetworkNotAllowedEmergencyCallsOnly) == 0) {
        err = RadioError::NETWORK_REJECT;
    } else if (str.compare(atCmds::kCmeErrorInCorrectParameters) == 0) {
        err = RadioError::INVALID_ARGUMENTS;
    } else if (str.compare(atCmds::kCmeErrorNetworkNotAttachedDueToMTFunctionalRestrictions) == 0) {
        err = RadioError::NETWORK_REJECT;
    } else if (str.compare(atCmds::kCmeErrorFixedDialNumberOnlyAllowed) == 0) {
        err = RadioError::GENERIC_FAILURE;
    } else {
        err = RadioError::GENERIC_FAILURE;
    }

    CmeError cmeErr = {
        .error = err,
    };

    return make(std::move(cmeErr));
}

RadioError AtResponse::CmeError::getErrorAndLog(
        const char* klass, const char* func, int line) const {
    RLOGE("%s:%s:%d failure: %s", klass, func, line,
          toString(error).c_str());
    return error;
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CmsError"
AtResponsePtr AtResponse::CmsError::parse(const std::string_view str) {
    CmsError cmsErr = {
        .message = toString(str),
    };

    return make(std::move(cmsErr));
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CPIN"
AtResponsePtr AtResponse::CPIN::parse(const std::string_view str) {
    CPIN cpin;
    if (str == "READY"sv) {
        cpin.state = CPIN::State::READY;
    } else if (str == "SIM PIN"sv) {
        cpin.state = CPIN::State::PIN;
    } else if (str == "SIM PUK"sv) {
        cpin.state = CPIN::State::PUK;
    } else if ((str == "SIM PIN2"sv) || (str == "SIM PUK2"sv)) {
        // gtel3g: PIN2 only guards FDN, the card itself is usable
        cpin.state = CPIN::State::READY;
    } else {
        // gtel3g: "NOT READY", "PH-NET PIN", ... (was a parse error)
        RLOGW("%s: '%*.*s' taken as NOT_READY", FAILURE_DEBUG_PREFIX,
              int(str.size()), int(str.size()), str.data());
        cpin.state = CPIN::State::NOT_READY;
    }

    return make(std::move(cpin));
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CPINR"
AtResponsePtr AtResponse::CPINR::parse(const std::string_view str) {
    CPINR cpinr;

    Parser parser(str);
    std::string_view unused;
    if (!parser(&unused, ',')
               (&cpinr.remainingRetryTimes).skip(',')
               (&cpinr.maxRetryTimes).fullMatch()) {
        return FAILURE_V(makeParseErrorFor<CPINR>(),
                         "Can't parse: '%*.*s'",
                         int(str.size()), int(str.size()), str.data());
    }

    return make(std::move(cpinr));
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CRSM"
AtResponsePtr AtResponse::CRSM::parse(const std::string_view str) {
    // gtel3g: +CRSM: <sw1>,<sw2>[,"<response>"], the CP quotes the response
    CRSM crsm;

    const std::vector<Param> params = splitParamsQ(str);
    if ((params.size() < 2) || (params.size() > 3) ||
            !parseIntParam(params[0].value, &crsm.sw1) ||
            !parseIntParam(params[1].value, &crsm.sw2)) {
        return FAILURE_V(makeParseErrorFor<CRSM>(),
                         "Can't parse: '%*.*s'",
                         int(str.size()), int(str.size()), str.data());
    }

    if (params.size() == 3) {
        crsm.response = params[2].value;
    }

    return make(std::move(crsm));
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CFUN"
AtResponsePtr AtResponse::CFUN::parse(const std::string_view str) {
    using modem::RadioState;

    int state;
    Parser parser(str);
    if (parser(&state).fullMatch()) {
        CFUN cfun = {
            .state = state ? RadioState::ON : RadioState::OFF,
        };
        return make(std::move(cfun));
    } else {
        return FAILURE_V(makeParseErrorFor<CFUN>(),
                         "Can't parse: '%*.*s'",
                         int(str.size()), int(str.size()), str.data());
    }
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CREG"
namespace {
// gtel3g: one parser for +CREG/+CGREG/+CEREG, solicited and unsolicited:
//   <stat>                              unsolicited, n=1
//   <n>,<stat>                          AT+CxREG? with n<2 or no cell info
//   <stat>,"<lac>","<ci>"[,<AcT>...]    unsolicited, n=2 (lac quoted)
//   <n>,<stat>,"<lac>","<ci>"[,<AcT>...] AT+CxREG? with n=2
// (+CGREG may carry a <rac> after AcT, ignored). The goldfish parser needed
// exactly the emulator's field counts.
template <class R> AtResponsePtr parseRegistration(const std::string_view str) {
    R r;
    const std::vector<Param> f = splitParamsQ(str);

    size_t i;
    if (f.size() == 2) {
        i = 1;
    } else if ((f.size() >= 3) && !f[1].quoted) {
        i = 1;
    } else {
        i = 0;
    }

    int state;
    if (f.empty() || !parseIntParam(f[i].value, &state)) {
        return FAILURE_V(AtResponse::makeParseErrorFor<R>(),
                         "Can't parse: '%*.*s'",
                         int(str.size()), int(str.size()), str.data());
    }

    switch (state) {
    case 6: state = 1; break;   // registered, SMS only, home
    case 7: state = 5; break;   // registered, SMS only, roaming
    default:
        if ((state < 0) || (state > 5)) {
            state = 4;          // unknown
        }
        break;
    }
    r.state = static_cast<network::RegState>(state);

    if ((f.size() > i + 2) && !f[i + 1].value.empty() && !f[i + 2].value.empty()) {
        parseIntParam(f[i + 1].value, &r.areaCode, 16);
        parseIntParam(f[i + 2].value, &r.cellId, 16);
    }
    if (f.size() > i + 3) {
        parseIntParam(f[i + 3].value, &r.networkType);   // 27.007 <AcT>
    }

    return AtResponse::make(std::move(r));
}
}  // namespace

AtResponsePtr AtResponse::CREG::parse(const std::string_view str) {
    return parseRegistration<CREG>(str);
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CGREG"
AtResponsePtr AtResponse::CGREG::parse(const std::string_view str) {
    return parseRegistration<CGREG>(str);
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CEREG"
AtResponsePtr AtResponse::CEREG::parse(const std::string_view str) {
    return parseRegistration<CEREG>(str);
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CTEC"
/*      +CTEC: current (decimal),preferred_bitmask (hex)
 *  OR
 *      +CTEC: comma_separated_list_of_supported (decimal)
 *  OR
 *      +CTEC: current (decimal)
 *  OR
 *      +CTEC: DONE
*/
AtResponsePtr AtResponse::CTEC::parse(const std::string_view str) {
    CTEC ctec;

    size_t i = 0;
    while (true) {
        const size_t comma = str.find(',', i);
        if (comma != std::string_view::npos) {
            ctec.values.push_back(std::string(str.substr(i, comma - i)));
            i = comma + 1;
        } else {
            ctec.values.push_back(std::string(str.substr(i)));
            break;
        }
    }

    return make(std::move(ctec));
}

std::optional<ratUtils::ModemTechnology> AtResponse::CTEC::getCurrentModemTechnology() const {
    using ratUtils::ModemTechnology;

    if ((values.size() == 0) || (values.size() > 2) ||
            ((values.size() == 1) && (values[0] == "DONE"))) {
        return std::nullopt;
    }

    int mtech;
    std::from_chars_result r =
        std::from_chars(&*values[0].begin(), &*values[0].end(), mtech, 10);

    if ((r.ec != std::errc()) || (r.ptr != &*values[0].end())) {
        return FAILURE(std::nullopt);
    }

    for (unsigned i = static_cast<unsigned>(ModemTechnology::GSM);
                  i <= static_cast<unsigned>(ModemTechnology::NR); ++i) {
        if (mtech & (1U << i)) {
            return static_cast<ratUtils::ModemTechnology>(i);
        }
    }

    return FAILURE(std::nullopt);
}

bool AtResponse::CTEC::isDONE() const {
    return (values.size() == 1) && (values[0] == "DONE");
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "COPS"
/* gtel3g: 27.007 forms with quotes and optional AcT, as the CP sends them:
 *      "+COPS: 0,0,\"long\",2\r+COPS: 0,1,\"short\",2\r+COPS: 0,2,\"26201\",2\r"
 *  OR  "+COPS: 0\r"  (not registered: only the mode)
 *  OR  "+COPS: (2,\"long\",\"short\",\"26201\",2),(...),,(0,1,2,3,4),(0,1,2)\r"
 *  Lines that do not start with "+COPS:" (an unsolicited +CSQ between them)
 *  are skipped. An operator is only reported with a numeric of 5-6 digits
 *  (mcc()/mnc() split it).
 */
AtResponsePtr AtResponse::COPS::parse(const std::string_view str) {
    using namespace std::literals;
    COPS cops;
    COPS::OperatorInfo current;
    current.state = COPS::OperatorInfo::State::CURRENT;
    bool haveMode = false;
    bool haveOperator = false;
    bool haveList = false;

    size_t pos = 0;
    while (pos < str.size()) {
        size_t cr = str.find(kCR, pos);
        if (cr == str.npos) {
            cr = str.size();
        }
        const std::string_view line = str.substr(pos, cr - pos);
        pos = cr + 1;

        static constexpr std::string_view kPrefix = "+COPS:"sv;
        if (!line.starts_with(kPrefix)) {
            continue;
        }
        std::string_view body = ltrim(line.substr(kPrefix.size()));

        if (!body.empty() && (body.front() == '(')) {
            haveList = true;
            while (!body.empty() && (body.front() == '(')) {
                const size_t close = body.find(')');
                if (close == body.npos) {
                    break;
                }
                const std::vector<Param> f = splitParamsQ(body.substr(1, close - 1));
                body.remove_prefix(close + 1);

                int state;
                if ((f.size() >= 4) && parseIntParam(f[0].value, &state) &&
                        (f[3].value.size() >= 5)) {
                    COPS::OperatorInfo operatorInfo;
                    operatorInfo.state = static_cast<COPS::OperatorInfo::State>(state);
                    operatorInfo.longName = f[1].value;
                    operatorInfo.shortName = f[2].value;
                    operatorInfo.numeric = f[3].value;
                    cops.operators.push_back(std::move(operatorInfo));
                }

                // ",," ends the operator list (mode and format lists follow)
                if (body.starts_with(",("sv)) {
                    body.remove_prefix(1);
                } else {
                    break;
                }
            }
            continue;
        }

        const std::vector<Param> f = splitParamsQ(body);
        int mode;
        if (!f.empty() && parseIntParam(f[0].value, &mode)) {
            cops.networkSelectionMode = static_cast<COPS::NetworkSelectionMode>(mode);
            haveMode = true;
        }

        int format;
        if ((f.size() >= 3) && parseIntParam(f[1].value, &format)) {
            switch (format) {
            case 0: current.longName = f[2].value; break;
            case 1: current.shortName = f[2].value; break;
            case 2: current.numeric = f[2].value; break;
            default: break;
            }
            haveOperator = true;
        }
    }

    if (haveOperator) {
        if (current.numeric.size() >= 5) {
            cops.numeric = current.numeric;
            cops.operators.push_back(std::move(current));
        } else {
            RLOGW("%s: operator without numeric: '%*.*s'", FAILURE_DEBUG_PREFIX,
                  int(str.size()), int(str.size()), str.data());
        }
    } else if (!haveMode && !haveList) {
        return FAILURE_V(makeParseErrorFor<COPS>(),
                         "Can't parse: '%*.*s'",
                         int(str.size()), int(str.size()), str.data());
    }

    return make(std::move(cops));
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "WRMP"
AtResponsePtr AtResponse::WRMP::parse(const std::string_view str) {
    int cdmaRoamingPreference;

    Parser parser(str);
    if (!parser(&cdmaRoamingPreference).fullMatch()) {
        return FAILURE_V(makeParseErrorFor<WRMP>(),
                         "Can't parse: '%*.*s'",
                         int(str.size()), int(str.size()), str.data());
    }

    WRMP wrmp = {
        .cdmaRoamingPreference =
            static_cast<network::CdmaRoamingType>(cdmaRoamingPreference),
    };

    return make(std::move(wrmp));
}


#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CCSS"
AtResponsePtr AtResponse::CCSS::parse(const std::string_view str) {
    CCSS ccss;
    int source;

    Parser parser(str);
    if (parser(&source).fullMatch()) {
        ccss.source = static_cast<sim::CdmaSubscriptionSource>(source);
        return make(std::move(ccss));
    } else {
        return FAILURE_V(makeParseErrorFor<CCSS>(),
                         "Can't parse: '%*.*s'",
                         int(str.size()), int(str.size()), str.data());
    }
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CSQ"
AtResponsePtr AtResponse::CSQ::parse(const std::string_view str) {
    constexpr size_t kMaxSize = 22;
    int values[kMaxSize];

    Parser parser(str);
    if (!parser(&values[0]).matchSoFar()) {
        return FAILURE_V(makeParseErrorFor<CSQ>(),
                         "Can't parse: '%*.*s'",
                         int(str.size()), int(str.size()), str.data());
    }

    size_t n;
    for (n = 1; parser.hasMore() && (n < kMaxSize); ++n) {
        if (!parser.skip(',')(&values[n]).matchSoFar()) {
            return FAILURE_V(makeParseErrorFor<CSQ>(),
                             "Can't parse: '%*.*s'",
                             int(str.size()), int(str.size()), str.data());
        }
    }

    if (!parser.fullMatch() || (n < 2)) {
        return FAILURE_V(makeParseErrorFor<CSQ>(),
                         "Can't parse: '%*.*s'",
                         int(str.size()), int(str.size()), str.data());
    }

    CSQ csq;

    // gtel3g: a real modem answers "+CSQ: <rssi>,<ber>" (27.007); the CP
    // also sends it unsolicited every few seconds. Reported for GSM and
    // WCDMA alike, Android picks the one of the current RAT.
    if (n < 12) {
        csq.gsm_signalStrength = values[0];
        csq.gsm_bitErrorRate = values[1];
        csq.wcdma_signalStrength = values[0];
        csq.wcdma_bitErrorRate = values[1];
        return make(std::move(csq));
    }

    switch (n) {
    case 22:
        csq.wcdma_signalStrength = values[14];
        if (csq.wcdma_signalStrength != kUnknown) {
            csq.wcdma_rscp = 42;
            csq.wcdma_ecno = 19;
        }
        csq.wcdma_bitErrorRate = values[15];
        csq.nr_ssRsrp = values[16];
        csq.nr_ssRsrq = values[17];
        csq.nr_ssSinr = values[18];
        csq.nr_csiRsrp = values[19];
        csq.nr_csiRsrq = values[20];
        csq.nr_csiSinr = values[21];
        [[fallthrough]];

    case 14:
        csq.tdscdma_rscp = values[13];
        [[fallthrough]];

    case 13:
        csq.lte_timingAdvance = values[12];
        [[fallthrough]];

    case 12:
        csq.gsm_signalStrength = values[0];
        csq.gsm_bitErrorRate = values[1];
        csq.cdma_dbm = values[2];
        csq.cdma_ecio = values[3];
        csq.evdo_dbm = values[4];
        csq.evdo_ecio = values[5];
        csq.evdo_signalNoiseRatio = values[6];
        csq.lte_signalStrength = values[7];
        csq.lte_rsrp = values[8];
        csq.lte_rsrq = values[9];
        csq.lte_rssnr = values[10];
        csq.lte_cqi = values[11];
        break;

    default:
        return FAILURE_V(makeParseErrorFor<CSQ>(),
                         "Unexpected size: %zu", n);
    }

    return make(std::move(csq));
}

network::SignalStrength AtResponse::CSQ::toSignalStrength() const {
    return {
        .gsm = {
            .signalStrength = gsm_signalStrength,
            .bitErrorRate = gsm_bitErrorRate,
            .timingAdvance = gsm_timingAdvance,
        },
        .cdma = {
            .dbm = cdma_dbm,
            .ecio = cdma_ecio,
        },
        .evdo = {
            .dbm = evdo_dbm,
            .ecio = evdo_ecio,
            .signalNoiseRatio = evdo_signalNoiseRatio,
        },
        .lte = {
            .signalStrength = lte_signalStrength,
            .rsrp = lte_rsrp,
            .rsrq = lte_rsrq,
            .rssnr = lte_rssnr,
            .cqi = lte_cqi,
            .timingAdvance = lte_timingAdvance,
            .cqiTableIndex = lte_cqiTableIndex,
        },
        .tdscdma = {
            .signalStrength = tdscdma_signalStrength,
            .bitErrorRate = tdscdma_bitErrorRate,
            .rscp = tdscdma_rscp,
        },
        .wcdma = {
            .signalStrength = wcdma_signalStrength,
            .bitErrorRate = wcdma_bitErrorRate,
            .rscp = wcdma_rscp,
            .ecno = wcdma_ecno,
        },
        .nr = {
            .ssRsrp = nr_ssRsrp,
            .ssRsrq = nr_ssRsrq,
            .ssSinr = nr_ssSinr,
            .csiRsrp = nr_csiRsrp,
            .csiRsrq = nr_csiRsrq,
            .csiSinr = nr_csiSinr,
            .csiCqiTableIndex = nr_csiCqiTableIndex,
            .timingAdvance = nr_timingAdvance,
        },
    };
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CLCC"
AtResponsePtr AtResponse::CLCC::parse(const std::string_view str) {
    CLCC clcc;

    Parser parser(str);
    while (parser.hasMore()) {
        int index;
        int dir;
        int state;
        int mode;
        int mpty;
        int type;
        std::string number;

        // +CLCC: <index>,<dir>,<state>,<mode>,<mpty>,<number>,<type>\r
        if (parser.skip("+CLCC:").skip(' ')(&index).skip(',')
                  (&dir).skip(',')(&state).skip(',')
                  (&mode).skip(',')(&mpty).skip(',')
                  (&number, ',')(&type).skip(kCR).matchSoFar()) {

            voice::Call call = {
                .state = state,
                .index = index,
                .toa = type,
                .isMpty = (mpty != 0),
                .isMT = (dir != 0),
                .isVoice = (mode == 0),
                .number = std::move(number),
            };

            clcc.calls.push_back(std::move(call));
        } else {
            return FAILURE_V(makeParseErrorFor<CLCC>(),
                             "Can't parse '%*.*s'",
                             int(str.size()), int(str.size()), str.data());
        }
    }

    return make(std::move(clcc));
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CCFCU"
AtResponsePtr AtResponse::CCFCU::parse(const std::string_view str) {
    CCFCU ccfcu;

    Parser parser(str);
    while (parser.hasMore()) {
        voice::CallForwardInfo cfi;
        int numberType;
        std::string_view ignore;
        if (parser.skip("+CCFCU:").skip(' ')(&cfi.status).skip(',')
                (&cfi.serviceClass).skip(',')(&numberType).skip(',')
                (&cfi.toa).skip(',').skip('"')(&cfi.number, '"').matchSoFar()) {
            switch (parser.front()) {
            case ',':
                if (!parser.skip(',')(&ignore, ',')(&ignore, ',')
                          (&cfi.timeSeconds).skip(kCR).matchSoFar()) {
                    return FAILURE_V(makeParseErrorFor<CCFCU>(),
                                     "Can't parse '%*.*s'",
                                     int(str.size()), int(str.size()), str.data());
                }
                break;

            case kCR:
                parser.skip(kCR);
                break;

            default:
                return FAILURE_V(makeParseErrorFor<CCFCU>(),
                                 "Can't parse '%*.*s'",
                                 int(str.size()), int(str.size()), str.data());
            }

            ccfcu.callForwardInfos.push_back(std::move(cfi));
        } else {
            return FAILURE_V(makeParseErrorFor<CCFCU>(),
                             "Can't parse '%*.*s'",
                             int(str.size()), int(str.size()), str.data());
        }
    }

    return make(std::move(ccfcu));
}


#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CCWA"
AtResponsePtr AtResponse::CCWA::parse(const std::string_view str) {
    CCWA ccwa;
    int mode;

    Parser parser(str);
    if (parser(&mode).skip(',')(&ccwa.serviceClass).fullMatch()) {
        ccwa.enable = (mode == 1);
        return make(std::move(ccwa));
    } else {
        return FAILURE_V(makeParseErrorFor<CCWA>(),
                         "Can't parse '%*.*s'",
                         int(str.size()), int(str.size()), str.data());
    }
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CUSATD"
AtResponsePtr AtResponse::CUSATD::parse(const std::string_view str) {
    CUSATD cusatd;

    Parser parser(str);
    if (parser(&cusatd.a).skip(',').skip(' ')(&cusatd.a).fullMatch()) {
        return make(std::move(cusatd));
    } else {
        return FAILURE_V(makeParseErrorFor<CUSATD>(),
                         "Can't parse '%*.*s'",
                         int(str.size()), int(str.size()), str.data());
    }
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CUSATP"
AtResponsePtr AtResponse::CUSATP::parse(const std::string_view str) {
    CUSATP cusatp = {
        .cmd = toString(str),
    };

    return make(std::move(cusatp));
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CUSATE"
AtResponsePtr AtResponse::CUSATE::parse(const std::string_view str) {
    CUSATE cusate = {
        .response = toString(str),
    };

    return make(std::move(cusate));
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CUSATT"
AtResponsePtr AtResponse::CUSATT::parse(const std::string_view str) {
    CUSATT cusatt;

    Parser parser(str);
    if (parser(&cusatt.value).fullMatch()) {
        return make(std::move(cusatt));
    } else {
        return FAILURE_V(makeParseErrorFor<CUSATT>(),
                         "Can't parse '%*.*s'",
                         int(str.size()), int(str.size()), str.data());
    }
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CUSATEND"
AtResponsePtr AtResponse::CUSATEND::parse(const std::string_view) {
    return make(CUSATEND());
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CLCK"
AtResponsePtr AtResponse::CLCK::parse(const std::string_view str) {
    CLCK clck;

    switch (str.front()) {
    case '0': clck.locked = false; break;
    case '1': clck.locked = true; break;
    default:
        return FAILURE_V(makeParseErrorFor<CLCK>(),
                         "Can't parse '%*.*s'",
                         int(str.size()), int(str.size()), str.data());
    }

    return make(std::move(clck));
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CSIM"
AtResponsePtr AtResponse::CSIM::parse(const std::string_view str) {
    Parser parser(str);
    int len;

    if (parser(&len).skip(',').matchSoFar()) {
        const std::string_view response = parser.remaining();

        if (len == response.size()) {
            CSIM csim = {
                .response = toString(response),
            };

            return make(std::move(csim));
        }
    }

    return FAILURE_V(makeParseErrorFor<CSIM>(),
                     "Can't parse '%*.*s'",
                     int(str.size()), int(str.size()), str.data());
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CGLA"
AtResponsePtr AtResponse::CGLA::parse(const std::string_view str) {
    Parser parser(str);
    int len;

    if (parser(&len).skip(',').matchSoFar()) {
        const std::string_view response = parser.remaining();

        if (len == response.size()) {
            CGLA cgla = {
                .response = toString(response),
            };

            return make(std::move(cgla));
        }
    }

    return FAILURE_V(makeParseErrorFor<CGLA>(),
                     "Can't parse '%*.*s'",
                     int(str.size()), int(str.size()), str.data());
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CCHC"
AtResponsePtr AtResponse::CCHC::parse(const std::string_view) {
    return make(CCHC());
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CLIP"
AtResponsePtr AtResponse::CLIP::parse(const std::string_view str) {
    int enable;
    int status;

    Parser parser(str);
    if (parser(&enable).skip(',')(&status).fullMatch()) {
        CLIP clip;
        clip.enable = enable != 0;
        clip.status = static_cast<voice::ClipStatus>(status);

        return make(std::move(clip));
    } else {
        return FAILURE_V(makeParseErrorFor<CLIP>(),
                         "Can't parse '%*.*s'",
                         int(str.size()), int(str.size()), str.data());
    }
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CLIR"
AtResponsePtr AtResponse::CLIR::parse(const std::string_view str) {
    CLIR clir;
    Parser parser(str);
    if (parser(&clir.n).skip(',')(&clir.m).fullMatch()) {
        return make(std::move(clir));
    } else {
        return FAILURE_V(makeParseErrorFor<CLIR>(),
                         "Can't parse '%*.*s'",
                         int(str.size()), int(str.size()), str.data());
    }
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CMUT"
AtResponsePtr AtResponse::CMUT::parse(const std::string_view str) {
    int on;
    Parser parser(str);
    if (parser(&on).fullMatch()) {
        CMUT cmut;
        cmut.on = (on != 0);
        return make(std::move(cmut));
    } else {
        return FAILURE_V(makeParseErrorFor<CMUT>(),
                         "Can't parse '%*.*s'",
                         int(str.size()), int(str.size()), str.data());
    }
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "WSOS"
AtResponsePtr AtResponse::WSOS::parse(const std::string_view str) {
    int isEmergencyMode;
    Parser parser(str);
    if (parser(&isEmergencyMode).fullMatch()) {
        WSOS wsos;
        wsos.isEmergencyMode = (isEmergencyMode != 0);
        return make(std::move(wsos));
    } else {
        return FAILURE_V(makeParseErrorFor<WSOS>(),
                         "Can't parse '%*.*s'",
                         int(str.size()), int(str.size()), str.data());
    }
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CSCA"
AtResponsePtr AtResponse::CSCA::parse(const std::string_view str) {
    CSCA csca;

    Parser parser(str);
    if (!parser(&csca.sca, ',')(&csca.tosca).fullMatch()) {
        return FAILURE_V(makeParseErrorFor<CSCA>(),
                         "Can't parse '%*.*s'",
                         int(str.size()), int(str.size()), str.data());
    }

    return make(std::move(csca));
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CSCB"
AtResponsePtr AtResponse::CSCB::parse(const std::string_view str) {
    CSCB cscb;
    std::string_view serviceId;
    std::string_view codeScheme;

    Parser parser(str);
    if (!parser(&cscb.mode).skip(',')
               .skip('"')(&serviceId, '"').skip(',')
               .skip('"')(&codeScheme, '"').fullMatch()) {
fail:   return FAILURE_V(makeParseErrorFor<CSCB>(),
                         "Can't parse '%*.*s'",
                         int(str.size()), int(str.size()), str.data());
    }

    auto maybeIds = parseIds(serviceId);
    if (!maybeIds) {
        goto fail;
    }
    cscb.serviceId = std::move(maybeIds.value());

    maybeIds = parseIds(codeScheme);
    if (!maybeIds) {
        goto fail;
    }
    cscb.codeScheme = std::move(maybeIds.value());

    return make(std::move(cscb));
}

std::optional<std::vector<AtResponse::CSCB::Association>>
AtResponse::CSCB::parseIds(const std::string_view str) {
    std::vector<Association> ids;
    Parser parser(str);
    while (parser.hasMore()) {
        Association a;
        if (!parser(&a.from).matchSoFar()) {
            return std::nullopt;
        }

        if (parser.fullMatch()) {
            a.to = a.from;
            ids.push_back(a);
            break;
        }

        switch (parser.front()) {
        case '-':
            if (!parser.skip('-')(&a.to).matchSoFar()) {
                return std::nullopt;
            }
            ids.push_back(a);
            if (parser.fullMatch()) {
                break;
            } else if (parser.front() == ',') {
                parser.skip(',');
            } else {
                return std::nullopt;
            }
            break;

        case ',':
            parser.skip(',');
            break;

        default:
            return std::nullopt;
        }
    }

    return ids;
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CMGS"
AtResponsePtr AtResponse::CMGS::parse(const std::string_view str) {
    CMGS cmgs;

    Parser parser(str);
    if (!parser(&cmgs.messageRef).fullMatch()) {
        return FAILURE_V(makeParseErrorFor<CMGS>(),
                         "Can't parse '%*.*s'",
                         int(str.size()), int(str.size()), str.data());
    }

    return make(std::move(cmgs));
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CMGW"
AtResponsePtr AtResponse::CMGW::parse(const std::string_view str) {
    CMGW cmgw;

    Parser parser(str);
    if (!parser(&cmgw.messageRef).fullMatch()) {
        return FAILURE_V(makeParseErrorFor<CMGS>(),
                         "Can't parse '%*.*s'",
                         int(str.size()), int(str.size()), str.data());
    }

    return make(std::move(cmgw));
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CMT"
namespace {
// gtel3g: 27.005 PDU mode "[<alpha>],<length>\r<pdu>\r" (+CMT) and
// "<length>\r<pdu>\r" (+CDS); the goldfish parser only took a bare number.
// Returns the bytes consumed (0: incomplete), the length field and the PDU.
std::tuple<int, int, std::optional<std::vector<uint8_t>>>
parsePduIndication(const std::string_view str) {
    const size_t cr1 = str.find(kCR);
    if (cr1 == str.npos) {
        return { 0, -1, std::nullopt };
    }
    const size_t cr2 = str.find(kCR, cr1 + 1);
    if (cr2 == str.npos) {
        return { 0, -1, std::nullopt };
    }

    const std::string_view header = str.substr(0, cr1);
    const size_t comma = header.rfind(',');
    const std::string_view lengthStr =
        ltrim((comma == header.npos) ? header : header.substr(comma + 1));
    int length = -1;
    parseIntParam(lengthStr, &length);

    std::vector<uint8_t> pdu;
    if (hex2bin(str.substr(cr1 + 1, cr2 - cr1 - 1), &pdu)) {
        return { int(cr2 + 1), length, std::move(pdu) };
    } else {
        return { int(cr2 + 1), length, std::nullopt };
    }
}
}  // namespace

std::pair<int, AtResponsePtr> AtResponse::CMT::parse(const std::string_view str) {
    auto [consumed, length, pdu] = parsePduIndication(str);
    if (consumed == 0) {
        return { 0, nullptr };
    } else if (!pdu) {
        return FAILURE_V(std::make_pair(consumed, makeParseErrorFor<CMT>()),
                         "Can't parse '%*.*s'", consumed, consumed, str.data());
    }

    CMT cmt;
    cmt.something = length;
    cmt.pdu = std::move(pdu.value());
    return { consumed, make(std::move(cmt)) };
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CDS"
std::pair<int, AtResponsePtr> AtResponse::CDS::parse(const std::string_view str) {
    auto [consumed, length, pdu] = parsePduIndication(str);
    if (consumed == 0) {
        return { 0, nullptr };
    } else if (!pdu) {
        return FAILURE_V(std::make_pair(consumed, makeParseErrorFor<CDS>()),
                         "Can't parse '%*.*s'", consumed, consumed, str.data());
    }

    CDS cds;
    cds.pduSize = length;
    cds.pdu = std::move(pdu.value());
    return { consumed, make(std::move(cds)) };
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CGACT"
// gtel3g: "+CGACT: 1,1\r+CGACT: 2,0\r"
AtResponsePtr AtResponse::CGACT::parse(const std::string_view str) {
    using namespace std::literals;
    CGACT cgact;

    size_t pos = 0;
    while (pos < str.size()) {
        size_t cr = str.find(kCR, pos);
        if (cr == str.npos) {
            cr = str.size();
        }
        const std::string_view line = str.substr(pos, cr - pos);
        pos = cr + 1;

        static constexpr std::string_view kPrefix = "+CGACT:"sv;
        if (!line.starts_with(kPrefix)) {
            continue;
        }

        const std::vector<Param> f = splitParamsQ(line.substr(kPrefix.size()));
        int cid, state;
        if ((f.size() >= 2) && parseIntParam(f[0].value, &cid) &&
                parseIntParam(f[1].value, &state)) {
            cgact.states.push_back({cid, state});
        }
    }

    return make(std::move(cgact));
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CGDCONT"
// +CGDCONT: <cid>,<pdp_type>,<APN>,<pdp_addr>,<d_comp>,<h_comp>\r
// 1,"IPV6","fast.t-mobile.com",,0,0
AtResponsePtr AtResponse::CGDCONT::parse(const std::string_view str) {
    CGDCONT cgdcont;

    Parser parser(str);
    while (parser.hasMore()) {
        CGDCONT::PdpContext pdpContext;

        if (parser.skip("+CGDCONT:").skip(' ')(&pdpContext.index).skip(',')
                  .skip('"')(&pdpContext.type, '"').skip(',')
                  .skip('"')(&pdpContext.apn, '"').skip(',')
                  (&pdpContext.addr, ',')(&pdpContext.dComp)
                  .skip(',')(&pdpContext.hComp).skip(' ').matchSoFar()) {
            cgdcont.contexts.push_back(std::move(pdpContext));
        } else {
            return FAILURE_V(makeParseErrorFor<CGDCONT>(),
                             "Can't parse '%*.*s'",
                             int(str.size()), int(str.size()), str.data());
        }
    }

    return make(std::move(cgdcont));
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CGCONTRDP"
// 1,5,"epc.tmobile.com",10.0.2.15/24,10.0.2.2,10.0.2.3
// 1,5,"epc.tmobile.com",10.0.2.15,10.0.2.2,10.0.2.3
AtResponsePtr AtResponse::CGCONTRDP::parse(const std::string_view str) {
    CGCONTRDP cgcontrdp;
    std::string_view localAddr;

    Parser parser(str);
    if (parser(&cgcontrdp.cid).skip(',')(&cgcontrdp.bearer).skip(',')
               .skip('"')(&cgcontrdp.apn, '"').skip(',')
               (&localAddr, ',')
               (&cgcontrdp.gwAddr, ',').matchSoFar()) {
        cgcontrdp.dns1 = parser.remainingAsString();
    } else {
        return FAILURE_V(makeParseErrorFor<CGCONTRDP>(),
                         "Can't parse '%*.*s'",
                         int(str.size()), int(str.size()), str.data());
    }

    Parser localAddrParser(localAddr);
    if (!localAddrParser(&cgcontrdp.localAddr, '/')
                        (&cgcontrdp.localAddrSize).fullMatch()) {
        cgcontrdp.localAddr = std::string(localAddr.data(), localAddr.size());
        cgcontrdp.localAddrSize = 0;
    }

    return make(std::move(cgcontrdp));
}


#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CGFPCCFG"
// 1,5000,32,0,1
AtResponsePtr AtResponse::CGFPCCFG::parse(const std::string_view str) {
    CGFPCCFG cgfpccfg;
    int status;
    int mtech;

    Parser parser(str);
    if (!parser(&status).skip(',')
               (&cgfpccfg.bandwidth).skip(',')
               (&mtech).skip(',')
               (&cgfpccfg.freq).skip(',')
               (&cgfpccfg.contextId).fullMatch()) {
        return FAILURE_V(makeParseErrorFor<CGFPCCFG>(),
                         "Can't parse '%*.*s'",
                         int(str.size()), int(str.size()), str.data());
    }

    cgfpccfg.status = static_cast<network::CellConnectionStatus>(status);
    cgfpccfg.mtech = static_cast<ratUtils::ModemTechnology>(mtech);

    return make(std::move(cgfpccfg));
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "MBAU"
// <STATUS>,<KC>,<SRES>
// <STATUS>,<CK>,<IK>,<RES/AUTS>
AtResponsePtr AtResponse::MBAU::parse(const std::string_view str) {
    MBAU mbau;

    std::string_view kc;
    std::string_view sres;
    std::string_view ck;
    std::string_view ik;
    std::string_view resAuts;

    Parser parser(str);
    switch (std::count(str.begin(), str.end(), ',')) {
    default:
failed:
        return FAILURE_V(makeParseErrorFor<MBAU>(),
                         "Can't parse '%*.*s'",
                         int(str.size()), int(str.size()), str.data());

    case 0:
        if (!parser(&mbau.status).fullMatch()) {
            goto failed;
        }
        break;

    case 2:
        if (parser(&mbau.status).skip(',')(&kc, ',').matchSoFar()) {
            sres = parser.remaining();
        } else {
            goto failed;
        }
        break;

    case 3:
        if (parser(&mbau.status).skip(',')(&ck, ',')(&ik, ',').matchSoFar()) {
            resAuts = parser.remaining();
        } else {
            goto failed;
        }
        break;
    }

    if (!hex2bin(kc, &mbau.kc) || !hex2bin(sres, &mbau.sres) || !hex2bin(ck, &mbau.ck) ||
            !hex2bin(ik, &mbau.ik) || !hex2bin(resAuts, &mbau.resAuts)) {
        goto failed;
    }

    return make(std::move(mbau));
}

#undef FAILURE_DEBUG_PREFIX
#define FAILURE_DEBUG_PREFIX "CTZV"
// 24/11/05:17:01:32-32:0:America!Los_Angeles
AtResponsePtr AtResponse::CTZV::parse(const std::string_view str) {
    int yy, month, day, hh, mm, ss, tzOffset15m;
    char tzSign, daylight;

    Parser parser(str);
    parser.skip(' ')(&yy).skip('/')(&month).skip('/')(&day).skip(':')
          (&hh).skip(':')(&mm).skip(':')(&ss)
          (&tzSign)(&tzOffset15m).skip(':')(&daylight).skip(':');

    if (!parser.matchSoFar()) {
        return FAILURE_V(makeParseErrorFor<CTZV>(),
                         "Can't parse: '%*.*s'",
                         int(str.size()), int(str.size()), str.data());
    }

    switch (tzSign) {
    case '+': break;
    case '-': tzOffset15m = -tzOffset15m; break;
    default:
        return FAILURE_V(makeParseErrorFor<CTZV>(),
                         "Unexpected timezone offset sign: '%*.*s'",
                         int(str.size()), int(str.size()), str.data());
    }

    CTZV ctzv = {
        .tzName = toString(parser.remaining()),
        .year = uint16_t(yy + 2000),
        .month = uint8_t(month),
        .day = uint8_t(day),
        .hour = uint8_t(hh),
        .isDaylightSaving = uint8_t(daylight != '0'),
        .minute = uint8_t(mm),
        .second = uint8_t(ss),
        .tzOffset15m = int8_t(tzOffset15m),
    };

    return make(std::move(ctzv));
}

std::string AtResponse::CTZV::nitzString() const {
    return std::format("{:02d}/{:02d}/{:02d}:{:02d}:{:02d}:{:02d}{:+d}:{:d}:{:s}",
                       year % 100, month, day, hour, minute, second,
                       tzOffset15m, isDaylightSaving, tzName.c_str());
}

std::string_view AtResponse::what() const {
    return visitR<std::string_view>(CmdIdVisitor());
}

void AtResponse::unexpected(const char* klass, const char* request,
                            const std::source_location location) const {
    const std::string_view r = what();
    const int rl = r.size();

    LOG_ALWAYS_FATAL("Unexpected response: '%*.*s' in %s:%s at %s:%d in %s", rl, rl, r.data(),
                     klass, request, location.function_name(), location.line(),
                     location.file_name());
}

}  // namespace implementation
}  // namespace radio
}  // namespace hardware
}  // namespace android
}  // namespace aidl

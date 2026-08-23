#pragma once
// LineParser: stateless, non-throwing parser for BYDA radar log lines.
//
// Validation pipeline (each stage can reject; the first failure is reported):
//
//   1. Structural   timestamp, three [n] header fields, BYDA::<Module>:,
//                   balanced brackets in the payload
//   2. Semantic     known payload keys must have the right *shape*
//                   (nodeUID/rfLane -> integer, lockState -> int->int).
//                   Unknown keys are never inspected: pattern[SW3] and
//                   command[RUN] are legitimate non-numeric values.
//   3. Speed        spd[...] -> finite, 0 <= v < 1e9 (Task 2 input)

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

enum class SkipReason {
    None = 0,
    EmptyLine,
    OversizedLine,         // exceeded StreamParser's 1 MiB cap without '\n'
    InvalidTimestamp,
    InvalidHeaderField,    // the [n][n][n] block
    InvalidModule,         // " BYDA::<ident>:" prefix
    UnbalancedBrackets,    // payload '[' / ']' do not pair up
    InvalidNodeUid,
    InvalidRfLane,
    InvalidLockState,
    InvalidSpeed,
    Count_                 // sentinel for array sizing, not a reason
};

inline constexpr size_t kSkipReasonCount = static_cast<size_t>(SkipReason::Count_);

inline std::string_view toString(SkipReason r) noexcept {
    switch (r) {
        case SkipReason::None:               return "None";
        case SkipReason::EmptyLine:          return "EmptyLine";
        case SkipReason::OversizedLine:      return "OversizedLine";
        case SkipReason::InvalidTimestamp:   return "InvalidTimestamp";
        case SkipReason::InvalidHeaderField: return "InvalidHeaderField";
        case SkipReason::InvalidModule:      return "InvalidModule";
        case SkipReason::UnbalancedBrackets: return "UnbalancedBrackets";
        case SkipReason::InvalidNodeUid:     return "InvalidNodeUid";
        case SkipReason::InvalidRfLane:      return "InvalidRfLane";
        case SkipReason::InvalidLockState:   return "InvalidLockState";
        case SkipReason::InvalidSpeed:       return "InvalidSpeed";
        case SkipReason::Count_:             break;
    }
    return "Unknown";
}

struct ParsedLine {
    std::string dateHour;              // "2026-06-19 22"  (Task 1 grouping key)
    std::string module;                // "RadarTrackNodeState"
    std::optional<double> speed;       // value of spd[...] if present (Task 2)
};

struct ParseResult {
    bool       valid = false;
    SkipReason reason = SkipReason::None;
    ParsedLine line;

    static ParseResult ok(ParsedLine l) {
        ParseResult r;
        r.valid = true;
        r.line = std::move(l);
        return r;
    }
    static ParseResult skip(SkipReason why) {
        ParseResult r;
        r.valid = false;
        r.reason = why;
        return r;
    }
};

class LineParser {
public:
    static ParseResult parse(std::string_view line) noexcept {
        if (line.empty()) return ParseResult::skip(SkipReason::EmptyLine);

        // ---- 1a. Timestamp block: [YYYY-MM-DD_HH:MM:SS.ffffff] ------------
        // Fixed width: '[' + 26 chars + ']' = 28 chars total.
        constexpr size_t kTsLen = 28;
        if (line.size() < kTsLen || line[0] != '[' || line[kTsLen - 1] != ']')
            return ParseResult::skip(SkipReason::InvalidTimestamp);

        std::string_view ts = line.substr(1, 26);      // 2026-06-19_22:00:00.045000
        if (ts[4] != '-' || ts[7] != '-' || ts[10] != '_' ||
            ts[13] != ':' || ts[16] != ':' || ts[19] != '.')
            return ParseResult::skip(SkipReason::InvalidTimestamp);
        if (!allDigits(ts.substr(0, 4))  || !allDigits(ts.substr(5, 2)) ||
            !allDigits(ts.substr(8, 2))  || !allDigits(ts.substr(11, 2)) ||
            !allDigits(ts.substr(14, 2)) || !allDigits(ts.substr(17, 2)) ||
            !allDigits(ts.substr(20, 6)))
            return ParseResult::skip(SkipReason::InvalidTimestamp);

        // Range sanity. Hour is what Task 1 groups by, but an out-of-range
        // month/day is just as much a "format mismatch" as a bad hour.
        const int month = two(ts, 5);
        const int day   = two(ts, 8);
        const int hour  = two(ts, 11);
        const int min   = two(ts, 14);
        const int sec   = two(ts, 17);
        if (month < 1 || month > 12 || day < 1 || day > 31 ||
            hour > 23 || min > 59 || sec > 59)
            return ParseResult::skip(SkipReason::InvalidTimestamp);

        ParsedLine out;
        out.dateHour.reserve(13);
        out.dateHour.assign(ts.substr(0, 10));         // date
        out.dateHour += ' ';
        out.dateHour += ts.substr(11, 2);              // hour

        // ---- 1b. Three numeric [n] header fields ---------------------------
        size_t pos = kTsLen;
        for (int i = 0; i < 3; ++i) {
            if (pos >= line.size() || line[pos] != '[')
                return ParseResult::skip(SkipReason::InvalidHeaderField);
            size_t close = line.find(']', pos + 1);
            if (close == std::string_view::npos ||
                close == pos + 1 ||                     // empty []
                !allDigits(line.substr(pos + 1, close - pos - 1)))
                return ParseResult::skip(SkipReason::InvalidHeaderField);
            pos = close + 1;
        }

        // ---- 1c. " BYDA::<Module>:" ----------------------------------------
        constexpr std::string_view kPrefix = " BYDA::";
        if (line.compare(pos, kPrefix.size(), kPrefix) != 0)
            return ParseResult::skip(SkipReason::InvalidModule);
        pos += kPrefix.size();
        size_t modEnd = line.find(':', pos);
        if (modEnd == std::string_view::npos || modEnd == pos)
            return ParseResult::skip(SkipReason::InvalidModule);
        const std::string_view module = line.substr(pos, modEnd - pos);
        // A module name is an identifier. Anything else ("unexpected chars" in
        // the assignment's corruption taxonomy) makes the line corrupt: it
        // would otherwise become an unbounded, unescapable CSV key.
        if (module.size() > kMaxModuleLen)
            return ParseResult::skip(SkipReason::InvalidModule);
        for (char c : module)
            if (!isIdentChar(c)) return ParseResult::skip(SkipReason::InvalidModule);
        out.module.assign(module);

        // ---- Payload: one pass, three logical checks -------------------------
        // Payload validation is logically divided into:
        //   1) bracket / structural integrity  (every '[' closed by the next
        //      ']', no stray ']', no nesting)
        //   2) semantic validation for known fields
        //   3) deferred spd validation
        //
        // The implementation intentionally performs the payload scan in a
        // single memchr-based pass to minimise CPU overhead for multi-million-
        // line logs (a separate per-char bracket pass measured ~0.35 s slower
        // on the 500 MB sample). The logical stages and the number of passes
        // are independent.
        //
        // Unknown fields are not semantically validated because payload syntax
        // is module-specific (e.g. pattern[SW3], command[RUN], element[1][2][3]).
        // The key is the maximal run of identifier chars before '[' (so
        // "wspd[" is key "wspd", never "spd").
        //
        // SkipReason precedence (deterministic, holds for the whole line):
        //   Header/Structural  >  first semantic field error  >  deferred spd.
        // A structural fault returns immediately. A semantic fault is only
        // *remembered* (first one wins) and the scan continues, so a broken
        // bracket further right still takes precedence. Valid lines never
        // pay for this: they have no semantic fault to remember.
        const std::string_view payload = line.substr(modEnd + 1);
        std::optional<std::string_view> spdText;
        SkipReason firstSemantic = SkipReason::None;
        auto noteSemantic = [&](SkipReason why) {
            if (firstSemantic == SkipReason::None) firstSemantic = why;
        };
        constexpr size_t npos = std::string_view::npos;
        size_t open  = payload.find('[');   // next '[' (carried across iterations)
        size_t after = 0;                   // one past the previous ']'
        for (;;) {
            // First ']' at/after the previous field. Before the next '[' it
            // is a stray; absent while a '[' is pending it is an unclosed one.
            const size_t close = payload.find(']', after);
            if (open == npos) {
                if (close != npos) return ParseResult::skip(SkipReason::UnbalancedBrackets);
                break;
            }
            if (close == npos || close < open)
                return ParseResult::skip(SkipReason::UnbalancedBrackets);
            const size_t nextOpen = payload.find('[', open + 1);
            if (nextOpen != npos && nextOpen < close)               // nested '['
                return ParseResult::skip(SkipReason::UnbalancedBrackets);
            const std::string_view value = payload.substr(open + 1, close - open - 1);

            size_t kb = open;
            while (kb > 0 && isIdentChar(payload[kb - 1])) --kb;
            const std::string_view key = payload.substr(kb, open - kb);
            after = close + 1;
            open  = nextOpen;

            if (key == "nodeUID") {
                if (!allDigits(value)) noteSemantic(SkipReason::InvalidNodeUid);
            } else if (key == "rfLane") {
                if (!allDigits(value)) noteSemantic(SkipReason::InvalidRfLane);
            } else if (key == "lockState") {
                if (!isTransition(value)) noteSemantic(SkipReason::InvalidLockState);
            } else if (key == "spd") {
                if (!spdText) spdText = value;     // first spd[] is the one
            }
        }
        if (firstSemantic != SkipReason::None)
            return ParseResult::skip(firstSemantic);

        // ---- 3) spd[...] validated last, after the structural/semantic gates.
        if (spdText) {
            double v{};
            const char* first = spdText->data();
            const char* last  = first + spdText->size();
            auto [ptr, ec] = std::from_chars(first, last, v);
            if (ec != std::errc{} || ptr != last)
                return ParseResult::skip(SkipReason::InvalidSpeed);  // non-numeric
            if (!std::isfinite(v) || v < 0.0 || v >= kMaxPlausibleSpeed)
                return ParseResult::skip(SkipReason::InvalidSpeed);  // poison value
            out.speed = v;
        }

        return ParseResult::ok(std::move(out));
    }

    // Plausibility bound for spd values. Normal data sits around 1.4e5;
    // anything at or beyond 1e9 (or negative / non-finite) is corruption.
    static constexpr double kMaxPlausibleSpeed = 1e9;

    // Longest accepted BYDA::<Module> name. Real modules are ~20 chars; the
    // bound stops corrupt data from turning a whole log line into a map key.
    static constexpr size_t kMaxModuleLen = 128;

private:
    // Two ASCII digits at `off` as an int; callers have already verified that
    // the positions are digits.
    static int two(std::string_view s, size_t off) noexcept {
        return (s[off] - '0') * 10 + (s[off + 1] - '0');
    }

    static bool allDigits(std::string_view s) noexcept {
        if (s.empty()) return false;
        for (char c : s)
            if (c < '0' || c > '9') return false;
        return true;
    }

    // "<int>-><int>", e.g. lockState[1->0]
    static bool isTransition(std::string_view s) noexcept {
        const size_t arrow = s.find("->");
        if (arrow == std::string_view::npos) return false;
        return allDigits(s.substr(0, arrow)) && allDigits(s.substr(arrow + 2));
    }

    static bool isIdentChar(char c) noexcept {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_';
    }
};

#pragma once
// LineParser: stateless, non-throwing parser for BYDA radar log lines.
//
// Expected format:
// [2026-06-19_22:00:00.045000][7710][30482][1885246073] BYDA::RadarTrackNodeState: node_state_synced: ...
// |<---- timestamp (28ch) --->|<-- 3 numeric fields -->| |<-- module -->|
//
// Any structural violation returns std::nullopt (poison-data safe).

#include <charconv>
#include <optional>
#include <string>
#include <string_view>

struct ParsedLine {
    std::string dateHour;              // "2026-06-19 22"  (Task 1 grouping key)
    std::string module;                // "RadarTrackNodeState"
    std::optional<double> speed;       // value of spd[...] if present (Task 2)
};

class LineParser {
public:
    static std::optional<ParsedLine> parse(std::string_view line) noexcept {
        // --- 1. Timestamp block: [YYYY-MM-DD_HH:MM:SS.ffffff] --------------
        // Fixed width: '[' + 26 chars + ']' = 28 chars total.
        constexpr size_t kTsLen = 28;
        if (line.size() < kTsLen || line[0] != '[' || line[kTsLen - 1] != ']')
            return std::nullopt;

        std::string_view ts = line.substr(1, 26);      // 2026-06-19_22:00:00.045000
        // Structural checks on separators.
        if (ts[4] != '-' || ts[7] != '-' || ts[10] != '_' ||
            ts[13] != ':' || ts[16] != ':' || ts[19] != '.')
            return std::nullopt;
        if (!allDigits(ts.substr(0, 4))  || !allDigits(ts.substr(5, 2)) ||
            !allDigits(ts.substr(8, 2))  || !allDigits(ts.substr(11, 2)) ||
            !allDigits(ts.substr(14, 2)) || !allDigits(ts.substr(17, 2)) ||
            !allDigits(ts.substr(20, 6)))
            return std::nullopt;

        // Range sanity (hour 00-23 is what Task 1 groups by).
        int hour = (ts[11] - '0') * 10 + (ts[12] - '0');
        if (hour > 23) return std::nullopt;

        ParsedLine out;
        out.dateHour.reserve(13);
        out.dateHour.assign(ts.substr(0, 10));         // date
        out.dateHour += ' ';
        out.dateHour += ts.substr(11, 2);              // hour

        // --- 2. Three numeric [n] fields ------------------------------------
        size_t pos = kTsLen;
        for (int i = 0; i < 3; ++i) {
            if (pos >= line.size() || line[pos] != '[') return std::nullopt;
            size_t close = line.find(']', pos + 1);
            if (close == std::string_view::npos ||
                close == pos + 1 ||                     // empty []
                !allDigits(line.substr(pos + 1, close - pos - 1)))
                return std::nullopt;
            pos = close + 1;
        }

        // --- 3. " BYDA::<Module>:" ------------------------------------------
        constexpr std::string_view kPrefix = " BYDA::";
        if (line.compare(pos, kPrefix.size(), kPrefix) != 0) return std::nullopt;
        pos += kPrefix.size();
        size_t modEnd = line.find(':', pos);
        if (modEnd == std::string_view::npos || modEnd == pos) return std::nullopt;
        out.module.assign(line.substr(pos, modEnd - pos));

        // --- 4. Optional spd[<double>] with semantic (range) validation -----
        // A line that carries a spd[] field whose value is unparseable or
        // physically implausible is corrupt data: reject the whole line so
        // poison values (e.g. spd[8.9e20]) can never contaminate the average.
        switch (extractSpeed(line.substr(modEnd), out.speed)) {
            case SpeedField::Corrupt: return std::nullopt;
            case SpeedField::None:
            case SpeedField::Valid:   return out;
        }
        return std::nullopt;   // unreachable; silences -Wreturn-type
    }

    // Plausibility bound for spd values. Normal data sits around 1.4e5;
    // anything at or beyond 1e9 (or negative) is treated as corruption.
    static constexpr double kMaxPlausibleSpeed = 1e9;

private:
    enum class SpeedField { None, Valid, Corrupt };
    static bool allDigits(std::string_view s) noexcept {
        if (s.empty()) return false;
        for (char c : s)
            if (c < '0' || c > '9') return false;
        return true;
    }

    // Finds "spd[" (word-boundary checked so "wspd["/"spdX[" never match) and
    // parses the bracketed value. Three outcomes:
    //   None    - no spd field on this line
    //   Valid   - parsed and within [0, kMaxPlausibleSpeed); stored in `out`
    //   Corrupt - field present but unparseable, unterminated, or implausible
    static SpeedField extractSpeed(std::string_view rest,
                                   std::optional<double>& out) noexcept {
        constexpr std::string_view kKey = "spd[";
        size_t p = 0;
        while ((p = rest.find(kKey, p)) != std::string_view::npos) {
            if (p > 0 && isIdentChar(rest[p - 1])) {    // e.g. advDelta... no,
                p += kKey.size();                        // wspd[: not our field
                continue;
            }
            size_t valBegin = p + kKey.size();
            size_t valEnd = rest.find(']', valBegin);
            if (valEnd == std::string_view::npos)
                return SpeedField::Corrupt;              // unterminated spd[

            double v{};
            const char* first = rest.data() + valBegin;
            const char* last  = rest.data() + valEnd;
            auto [ptr, ec] = std::from_chars(first, last, v);
            if (ec != std::errc{} || ptr != last)
                return SpeedField::Corrupt;              // non-numeric payload
            if (!(v >= 0.0 && v < kMaxPlausibleSpeed))
                return SpeedField::Corrupt;              // out of physical range
            out = v;
            return SpeedField::Valid;
        }
        return SpeedField::None;
    }

    static bool isIdentChar(char c) noexcept {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_';
    }
};

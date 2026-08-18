#pragma once
// StreamParser: feeds arbitrary network chunks, emits complete lines to the
// LineParser, aggregates statistics. Memory stays O(one line + stats map).

#include "LineParser.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

struct Stats {
    // Task 1: (dateHour, module) -> occurrence count. std::map keeps CSV sorted.
    std::map<std::pair<std::string, std::string>, uint64_t> moduleCounts;
    // Task 2: running sum/count -> average computed once at the end.
    double   speedSum   = 0.0;
    uint64_t speedCount = 0;

    uint64_t totalLines   = 0;
    uint64_t skippedLines = 0;

    // Upper bound on distinct (dateHour, module) keys. Corrupt data can invent
    // an unbounded number of module names; without a cap the map — and thus the
    // process — would grow with the line count, breaking the 50 MB budget.
    // Everything past the cap is folded into one overflow bucket per hour so
    // the counts still reconcile with parsed_lines.
    static constexpr size_t kMaxDistinctKeys = 100000;
    static constexpr std::string_view kOverflowModule = "__overflow__";

    void accumulate(const ParsedLine& p) {
        const std::pair<std::string, std::string> key{p.dateHour, p.module};
        if (moduleCounts.size() >= kMaxDistinctKeys &&
            moduleCounts.find(key) == moduleCounts.end()) {
            ++moduleCounts[{p.dateHour, std::string(kOverflowModule)}];
        } else {
            ++moduleCounts[key];
        }
        if (p.speed) {
            speedSum += *p.speed;
            ++speedCount;
        }
    }

    // Never underflows, even if a future change breaks the counting invariant.
    uint64_t parsedLines() const noexcept {
        return totalLines >= skippedLines ? totalLines - skippedLines : 0;
    }
};

class StreamParser {
public:
    // Called for each malformed line: (lineNumber, rawLine) — hook for spdlog.
    using MalformedHook = std::function<void(uint64_t, std::string_view)>;

    explicit StreamParser(MalformedHook onMalformed = {})
        : onMalformed_(std::move(onMalformed)) {}

    // Feed one received chunk. Complete lines are parsed immediately;
    // a trailing partial line is carried over to the next feed().
    void feed(std::string_view chunk) {
        carry_.append(chunk);

        // Defense against a poison "line" with no newline for megabytes:
        // if the carry buffer exceeds the cap without a newline, drop it as
        // one malformed line and resynchronize at the next '\n'.
        // The line is counted exactly once, however many feeds it spans:
        // `resyncing_` means we already reported it and are still discarding
        // the tail. Counting totalLines here too keeps the
        // totalLines = parsed + skipped invariant that writeCsv() relies on.
        if (carry_.size() > kMaxLineBytes &&
            carry_.find('\n') == std::string::npos) {
            if (!resyncing_) {
                ++stats_.totalLines;
                reportMalformed(carry_);
            }
            carry_.clear();
            resyncing_ = true;
            return;
        }

        size_t start = 0, pos;
        while ((pos = carry_.find('\n', start)) != std::string::npos) {
            std::string_view line(carry_.data() + start, pos - start);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (resyncing_) {
                resyncing_ = false;      // discard remainder of oversized line
            } else {
                processLine(line);
            }
            start = pos + 1;
        }
        carry_.erase(0, start);
    }

    // Call once after the last chunk (handles a final line without '\n').
    void finish() {
        if (!carry_.empty() && !resyncing_) processLine(carry_);
        carry_.clear();
        resyncing_ = false;
    }

    const Stats& stats() const { return stats_; }

    // result.csv layout:
    //   section,date_hour,module,count
    //   module_count,2026-06-19 22,RadarTrackNodeState,21
    //   ...
    //   summary,average_speed,,137500.000000
    //   summary,parsed_lines,,29
    //   summary,skipped_lines,,1
    void writeCsv(std::ostream& os) const {
        os << "section,date_hour,module,count\n";
        for (const auto& [key, count] : stats_.moduleCounts) {
            os << "module_count,";
            writeCsvField(os, key.first);
            os << ',';
            writeCsvField(os, key.second);
            os << ',' << count << '\n';
        }

        os << "summary,average_speed,,";
        if (stats_.speedCount > 0) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.6f",
                          stats_.speedSum / static_cast<double>(stats_.speedCount));
            os << buf;
        } else {
            os << "N/A";
        }
        os << '\n';
        os << "summary,parsed_lines,," << stats_.parsedLines() << '\n';
        os << "summary,skipped_lines,," << stats_.skippedLines << '\n';
    }

private:
    // RFC 4180 field: quote only when needed, doubling embedded quotes. The
    // module name comes from log data, so it must never be trusted to be
    // comma-free even though LineParser already restricts its character set.
    static void writeCsvField(std::ostream& os, std::string_view field) {
        if (field.find_first_of(",\"\r\n") == std::string_view::npos) {
            os << field;
            return;
        }
        os << '"';
        for (char c : field) {
            if (c == '"') os << '"';
            os << c;
        }
        os << '"';
    }

    void processLine(std::string_view line) {
        ++stats_.totalLines;
        if (line.empty()) { reportMalformed(line); return; }
        if (auto parsed = LineParser::parse(line)) {
            stats_.accumulate(*parsed);
        } else {
            reportMalformed(line);
        }
    }

    void reportMalformed(std::string_view line) {
        ++stats_.skippedLines;
        if (onMalformed_) onMalformed_(stats_.totalLines, line);
    }

    static constexpr size_t kMaxLineBytes = 1 << 20;   // 1 MiB safety cap

    std::string   carry_;          // partial line across chunk boundaries
    bool          resyncing_ = false;
    Stats         stats_;
    MalformedHook onMalformed_;
};

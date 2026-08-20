// Verifies LineParser/StreamParser against the real BYDA sample + poison data,
// while simulating arbitrary network chunk boundaries.
#include "StreamParser.h"

#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

static int gFailures = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (cond) { std::cout << "  PASS  " << msg << "\n"; }                \
        else      { std::cout << "  FAIL  " << msg << "\n"; ++gFailures; }   \
    } while (0)

static constexpr const char* kHdr =
    "[2026-06-19_22:00:00.309000][7710][30482][1885246073] ";

// Expect a rejection with a specific reason.
static void expectSkip(std::string_view line, SkipReason why, const char* msg) {
    const ParseResult r = LineParser::parse(line);
    const bool ok = !r.valid && r.reason == why;
    CHECK(ok, std::string(msg) + (ok ? "" :
          "  (got " + std::string(toString(r.reason)) + ")"));
}

int main(int argc, char** argv) {
    // ---- 1. Unit checks on single lines --------------------------------
    std::cout << "[LineParser: valid lines]\n";
    {
        auto r = LineParser::parse(std::string(kHdr) +
            "BYDA::BeamSteerCtrlUnitImpl: unitAddr[4181], spd[137500.000000], "
            "advDelta[62750.000000]");
        CHECK(r.valid, "valid spd line parses");
        CHECK(r.valid && r.line.module == "BeamSteerCtrlUnitImpl", "module extracted");
        CHECK(r.valid && r.line.dateHour == "2026-06-19 22", "dateHour bucket");
        CHECK(r.valid && r.line.speed && std::abs(*r.line.speed - 137500.0) < 1e-9,
              "spd value = 137500 (advDelta not confused)");
    }
    {
        auto r = LineParser::parse(std::string(kHdr) +
            "BYDA::RadarTrackNodeState: node_state_synced: nodeUID[47], "
            "rfLane[3], lockState[1->0]");
        CHECK(r.valid && !r.line.speed, "non-spd line: speed == nullopt");
        CHECK(r.valid && r.line.module == "RadarTrackNodeState", "module w/ nested colons");
    }
    // Every real module's payload shape must pass (from the sample file).
    CHECK(LineParser::parse(std::string(kHdr) +
          "BYDA::SectorSchedulerRTS: scan started: RT_SWEEP jobID[12], pattern[SW3], gatedFlag[1]").valid,
          "pattern[SW3]: unknown key with alphabetic value accepted");
    CHECK(LineParser::parse(std::string(kHdr) +
          "BYDA::DetectionTaskRunner: Sector Command: jobID[12], command[RUN], sectorID[3], bearing[270]").valid,
          "command[RUN]: unknown key with alphabetic value accepted");
    CHECK(LineParser::parse(std::string(kHdr) +
          "BYDA::AntennaProfileSpec: applyElement: sectorID[3], element[1][2][3]").valid,
          "element[1][2][3]: consecutive brackets accepted");
    CHECK(LineParser::parse(std::string(kHdr) + "BYDA::Radar_Track2: x").valid,
          "identifier module (digits/underscore) accepted");

    std::cout << "[LineParser: structural rejections (Level 1)]\n";
    expectSkip("", SkipReason::EmptyLine, "empty line");
    expectSkip("garbage without any structure", SkipReason::InvalidTimestamp,
               "no structure at all");
    expectSkip("[2026-06-19_22:00:00.04", SkipReason::InvalidTimestamp, "truncated");
    expectSkip("2026-06-19_22:20:00.111111][7710][30482][1885246073] BYDA::HeadBraceLoss: raw[9]",
               SkipReason::InvalidTimestamp, "HeadBraceLoss: missing opening bracket");
    expectSkip("[2026-06-19_25:00:00.045000][7710][30482][1885246073] BYDA::X: y",
               SkipReason::InvalidTimestamp, "hour 25");
    expectSkip("[2026-99-19_22:00:00.309000][7710][30482][1885246073] BYDA::Beam: x",
               SkipReason::InvalidTimestamp, "month 99");
    expectSkip("[2026-06-19_22:99:00.309000][7710][30482][1885246073] BYDA::Beam: x",
               SkipReason::InvalidTimestamp, "minute 99");
    expectSkip("[2026-06-19_22:00:00.045000][7710][30482] BYDA::RadarTrackNodeState: x",
               SkipReason::InvalidHeaderField, "only 2 header fields");
    expectSkip("[2026-06-19_22:00:00.045000][7710][3O482][1885246073] BYDA::X: y",
               SkipReason::InvalidHeaderField, "letter O in header field");
    expectSkip("[2026-06-19_22:15:00.000000] !@#$RAW_FRAME_DECODE_FAILURE_GARBAGE_OCTETS%^&*()",
               SkipReason::InvalidHeaderField, "garbage after timestamp");
    expectSkip("[2026-06-19_22:05:00.123456][7710][30482][1885246073 BYDA::OpenBraceLeak: rfLane[3]",
               SkipReason::InvalidHeaderField, "OpenBraceLeak: unclosed header field");
    expectSkip(std::string(kHdr) + "NOPE::Mod: x", SkipReason::InvalidModule, "wrong prefix");
    expectSkip(std::string(kHdr) + "BYDA::: x", SkipReason::InvalidModule, "empty module");
    expectSkip(std::string(kHdr) + "BYDA::Mod,With,Commas: x", SkipReason::InvalidModule,
               "comma in module name");
    expectSkip(std::string(kHdr) + "BYDA::" + std::string(200, 'M') + ": x",
               SkipReason::InvalidModule, "over-long module name");
    expectSkip(std::string(kHdr) + "BYDA::Beam: unitAddr[4181], spd[1.0",
               SkipReason::UnbalancedBrackets, "unterminated bracket in payload");
    expectSkip(std::string(kHdr) + "BYDA::Beam: unitAddr 4181], spd[1.0]",
               SkipReason::UnbalancedBrackets, "stray ']' in payload");

    std::cout << "[LineParser: semantic rejections (Level 2)]\n";
    expectSkip(std::string(kHdr) + "BYDA::CorruptPayload: nodeUID[NONE], rfLane[X]",
               SkipReason::InvalidNodeUid, "CorruptPayload: nodeUID[NONE] (first bad key wins)");
    expectSkip(std::string(kHdr) + "BYDA::RadarTrackNodeState: nodeUID[47], rfLane[X]",
               SkipReason::InvalidRfLane, "rfLane[X]");
    expectSkip(std::string(kHdr) + "BYDA::RadarTrackNodeState: nodeUID[47], rfLane[3], lockState[open]",
               SkipReason::InvalidLockState, "lockState[open]");
    expectSkip(std::string(kHdr) + "BYDA::RadarTrackNodeState: nodeUID[47], rfLane[3], lockState[1->]",
               SkipReason::InvalidLockState, "lockState[1->]");
    CHECK(LineParser::parse(std::string(kHdr) + "BYDA::X: myrfLane[X], rfLane[3]").valid,
          "myrfLane[X]: key match is word-bounded (not rfLane)");

    std::cout << "[LineParser: speed rejections (Level 3)]\n";
    expectSkip(std::string(kHdr) + "BYDA::BeyondLimit: spd[888888888888888888888.88]",
               SkipReason::InvalidSpeed, "BeyondLimit spd[8.9e20] -> range");
    expectSkip(std::string(kHdr) + "BYDA::Beam: spd[not_a_number]",
               SkipReason::InvalidSpeed, "spd[not_a_number]");
    expectSkip(std::string(kHdr) + "BYDA::Beam: spd[-5.0]",
               SkipReason::InvalidSpeed, "negative spd");
    expectSkip(std::string(kHdr) + "BYDA::Beam: spd[inf]",
               SkipReason::InvalidSpeed, "spd[inf] (non-finite)");
    CHECK(LineParser::parse(std::string(kHdr) + "BYDA::Beam: wspd[abc], spd[1.5]").valid,
          "wspd[abc] ignored (word boundary), spd[1.5] accepted");
    {
        // Pipeline order: a structural fault is reported even if spd is also bad.
        expectSkip(std::string(kHdr) + "BYDA::Beam: spd[9e20], rfLane[X",
                   SkipReason::UnbalancedBrackets, "structural reason wins over semantic/spd");
        expectSkip(std::string(kHdr) + "BYDA::Beam: spd[9e20], rfLane[X]",
                   SkipReason::InvalidRfLane, "semantic reason wins over spd");
        // Structural wins even when the semantic fault is further LEFT: the
        // semantic error is remembered, not returned, until the scan ends.
        expectSkip(std::string(kHdr) + "BYDA::Beam: rfLane[X], spd[1.0",
                   SkipReason::UnbalancedBrackets, "structural wins over earlier semantic fault");
        expectSkip(std::string(kHdr) + "BYDA::Beam: rfLane[X], nodeUID[NONE]",
                   SkipReason::InvalidRfLane, "first (leftmost) semantic fault is reported");
    }

    // ---- 1b. Regression: oversized-line accounting ----------------------
    // A "line" that never terminates is dropped at the 1 MiB cap. It must be
    // counted exactly once no matter how many feed() calls it spans, and the
    // totalLines = parsed + skipped invariant must hold.
    std::cout << "[oversized-line regression]\n";
    {
        auto summaryOf = [](const StreamParser& p) {
            std::ostringstream os;
            p.writeCsv(os);
            return os.str();
        };
        const std::string good = std::string(kHdr) + "BYDA::Beam: spd[137500.000000]\n";

        StreamParser sp;
        std::string poison(5u << 20, 'X');       // 5 MiB, no newline
        poison += '\n';
        poison += good;
        for (size_t i = 0; i < poison.size(); i += 65536)
            sp.feed(std::string_view(poison).substr(i, 65536));
        sp.finish();

        CHECK(sp.stats().skippedLines == 1,
              "5 MiB unterminated line counted once, not once per feed()");
        CHECK(sp.stats().skippedByReason[size_t(SkipReason::OversizedLine)] == 1,
              "counted under OversizedLine");
        CHECK(sp.stats().totalLines == 2, "totalLines counts the dropped line");
        CHECK(summaryOf(sp).find("parsed_lines,,1") != std::string::npos,
              "parsed_lines = 1 (no unsigned underflow)");
        CHECK(summaryOf(sp).find("skipped_OversizedLine,,1") != std::string::npos,
              "per-reason summary row emitted");

        StreamParser only;                       // nothing but the poison
        std::string lone(2u << 20, 'X');
        only.feed(lone);
        only.finish();
        CHECK(summaryOf(only).find("parsed_lines,,0") != std::string::npos,
              "parsed_lines = 0 when every line was dropped");
    }

    // ---- 2. Stream test: sample file fed in tiny chunks ----------------
    std::cout << "[StreamParser chunk-boundary test]\n";
    const char* path = (argc > 1) ? argv[1] : "sample.log";
    std::ifstream in(path, std::ios::binary);
    if (!in) { std::cerr << "cannot open " << path << "\n"; return 1; }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string all = ss.str();

    // Feed with a pathological chunk size (7 bytes) to force partial lines.
    StreamParser sp([](uint64_t no, SkipReason why, std::string_view line) {
        std::cout << "  [skip] line " << no << " [" << toString(why) << "]: "
                  << line.substr(0, 60) << "\n";
    });
    for (size_t i = 0; i < all.size(); i += 7)
        sp.feed(std::string_view(all).substr(i, 7));
    sp.finish();

    const Stats& st = sp.stats();
    std::cout << "  total=" << st.totalLines
              << " skipped=" << st.skippedLines
              << " speedCount=" << st.speedCount << "\n";

    // Reference run with one giant chunk must give identical results.
    StreamParser ref;
    ref.feed(all);
    ref.finish();
    CHECK(st.moduleCounts == ref.stats().moduleCounts &&
          st.speedCount == ref.stats().speedCount &&
          st.skippedLines == ref.stats().skippedLines &&
          st.skippedByReason == ref.stats().skippedByReason,
          "7-byte chunking == single-chunk (boundary handling correct)");

    // Known answer for the assignment's sample file (skip if another log).
    if (st.totalLines == 3483528) {
        std::cout << "[known answer: Test_Log.log]\n";
        CHECK(st.skippedLines == 26, "26 corrupted lines skipped");
        CHECK(st.skippedByReason[size_t(SkipReason::InvalidTimestamp)]   == 2, "  2 InvalidTimestamp (HeadBraceLoss)");
        CHECK(st.skippedByReason[size_t(SkipReason::InvalidHeaderField)] == 11, " 11 InvalidHeaderField (5 garbage + 6 OpenBraceLeak)");
        CHECK(st.skippedByReason[size_t(SkipReason::InvalidNodeUid)]     == 7, "  7 InvalidNodeUid (CorruptPayload)");
        CHECK(st.skippedByReason[size_t(SkipReason::InvalidSpeed)]       == 6, "  6 InvalidSpeed (BeyondLimit)");
        CHECK(st.moduleCounts.count({"2026-06-19 22", "CorruptPayload"}) == 0,
              "CorruptPayload no longer appears as a module");
        CHECK(st.speedCount == 580661, "580661 speed samples");
    }

    // ---- 3. CSV output --------------------------------------------------
    std::cout << "[result.csv]\n";
    sp.writeCsv(std::cout);

    std::cout << (gFailures == 0 ? "\nALL CHECKS PASSED\n"
                                 : "\nFAILURES PRESENT\n");
    return gFailures == 0 ? 0 : 1;
}

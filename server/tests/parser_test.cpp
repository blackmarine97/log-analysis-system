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

int main(int argc, char** argv) {
    // ---- 1. Unit checks on single lines --------------------------------
    std::cout << "[LineParser unit checks]\n";
    {
        auto p = LineParser::parse(
            "[2026-06-19_22:00:00.309000][7710][30482][1885246073] "
            "BYDA::BeamSteerCtrlUnitImpl: unitAddr[4181], spd[137500.000000], "
            "advDelta[62750.000000]");
        CHECK(p.has_value(), "valid spd line parses");
        CHECK(p && p->module == "BeamSteerCtrlUnitImpl", "module extracted");
        CHECK(p && p->dateHour == "2026-06-19 22", "dateHour bucket");
        CHECK(p && p->speed && std::abs(*p->speed - 137500.0) < 1e-9,
              "spd value = 137500 (advDelta not confused)");
    }
    {
        auto p = LineParser::parse(
            "[2026-06-19_22:00:00.045000][7710][30482][1885246073] "
            "BYDA::RadarTrackNodeState: node_state_synced: nodeUID[47], "
            "rfLane[3], lockState[1->0]");
        CHECK(p && !p->speed, "non-spd line: speed == nullopt");
        CHECK(p && p->module == "RadarTrackNodeState", "module w/ nested colons");
    }
    // Poison variants (each must be rejected, never crash):
    const char* poison[] = {
        "",                                                        // empty
        "garbage without any structure",
        "[2026-06-19_22:00:00.045000][7710][30482]"                // 2 fields only
        " BYDA::RadarTrackNodeState: x",
        "[2026-06-19_25:00:00.045000][7710][30482][1885246073]"    // hour 25
        " BYDA::X: y",
        "[2026-06-19_22:00:00.045000][7710][30482][1885246073] NOPE::Mod: x",
        "[2026-06-19_22:00:00.045000][7710][3O482][1885246073]"    // letter O
        " BYDA::X: y",
        "[2026-06-19_22:00:00.045000][7710][30482][1885246073] BYDA::: x",
        "[2026-06-19_22:00:00.04",                                 // truncated
        "[2026-06-19_22:00:00.045000][7710][30482][1885246073] "
        "BYDA::Beam: spd[not_a_number]",                           // corrupt spd
    };
    int rejected = 0;
    for (auto* s : poison)
        if (!LineParser::parse(s)) ++rejected;
    CHECK(rejected == 9, "all 9 poison lines rejected");

    // Real poison from Test_Log.log:
    {
        auto p = LineParser::parse(
            "[2026-06-19_22:25:00.999999][7710][30482][1885246073] "
            "BYDA::BeyondLimit: spd[888888888888888888888.88]");
        CHECK(!p.has_value(), "BeyondLimit spd[8.9e20] -> line rejected (range)");
    }
    {
        auto p = LineParser::parse(
            "[2026-06-19_22:10:00.654321][7710][30482][1885246073] "
            "BYDA::CorruptPayload: nodeUID[NONE], rfLane[X]");
        CHECK(p && !p->speed && p->module == "CorruptPayload",
              "CorruptPayload (no spd) -> counted, no speed");
    }
    {
        auto p = LineParser::parse(
            "[2026-06-19_22:00:00.309000][7710][30482][1885246073] "
            "BYDA::Beam: spd[-5.0]");
        CHECK(!p.has_value(), "negative spd -> line rejected");
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
    StreamParser sp([](uint64_t no, std::string_view line) {
        std::cout << "  [skip] line " << no << ": "
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
          st.skippedLines == ref.stats().skippedLines,
          "7-byte chunking == single-chunk (boundary handling correct)");

    // ---- 3. CSV output --------------------------------------------------
    std::cout << "[result.csv]\n";
    sp.writeCsv(std::cout);

    std::cout << (gFailures == 0 ? "\nALL CHECKS PASSED\n"
                                 : "\nFAILURES PRESENT\n");
    return gFailures == 0 ? 0 : 1;
}

// Verifies the asynchronous Logger: nothing is lost, per-producer order is
// preserved, and everything queued is flushed by the time the destructor
// returns (the daemon's SIGTERM path relies on that).
#include "Logger.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

static int gFailures = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (cond) { std::cout << "  PASS  " << msg << "\n"; }                \
        else      { std::cout << "  FAIL  " << msg << "\n"; ++gFailures; }   \
    } while (0)

int main() {
    const std::string path = "logger_test.tmp.log";
    std::remove(path.c_str());

    constexpr int kProducers = 4;
    constexpr int kPerProducer = 50000;   // 200k lines, > kMaxQueued -> exercises backpressure

    {
        Logger log(path);
        std::vector<std::thread> producers;
        for (int p = 0; p < kProducers; ++p) {
            producers.emplace_back([&log, p] {
                for (int i = 0; i < kPerProducer; ++i)
                    log.info("p" + std::to_string(p) + " seq " + std::to_string(i));
            });
        }
        for (auto& t : producers) t.join();
        std::cout << "  backpressure events: " << log.backpressureEvents() << "\n";
    }   // ~Logger must drain + join before we read the file

    std::ifstream in(path);
    std::vector<int> nextSeq(kProducers, 0);
    uint64_t lines = 0;
    bool ordered = true, wellFormed = true;
    std::string line;
    while (std::getline(in, line)) {
        ++lines;
        // "<ts> [INFO ] p<N> seq <i>"
        const size_t p = line.find("] p");
        const size_t s = line.find(" seq ");
        if (p == std::string::npos || s == std::string::npos) { wellFormed = false; continue; }
        const int prod = std::stoi(line.substr(p + 3, s - (p + 3)));
        const int seq  = std::stoi(line.substr(s + 5));
        if (prod < 0 || prod >= kProducers || seq != nextSeq[prod]) ordered = false;
        else ++nextSeq[prod];
    }
    std::remove(path.c_str());

    std::cout << "[Logger]\n";
    CHECK(lines == uint64_t(kProducers) * kPerProducer,
          "all lines written (" + std::to_string(lines) + ")");
    CHECK(wellFormed, "every line carries timestamp/level prefix");
    CHECK(ordered, "per-producer order preserved");

    std::cout << (gFailures == 0 ? "\nALL CHECKS PASSED\n" : "\nFAILURES PRESENT\n");
    return gFailures == 0 ? 0 : 1;
}

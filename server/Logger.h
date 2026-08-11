#pragma once
// Minimal RAII file logger for the server daemon. Each line is prefixed with
// a wall-clock timestamp. Intentionally dependency-free (no spdlog) so the
// server builds with nothing beyond libuv + the standard library.

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

class Logger {
public:
    // If `path` is empty or cannot be opened, log lines go to stderr instead
    // (useful in foreground mode; a daemon should always pass a real path).
    explicit Logger(const std::string& path = {}) {
        if (!path.empty()) {
            file_.open(path, std::ios::app);
            if (!file_) std::cerr << "logger: cannot open " << path
                                  << ", falling back to stderr\n";
        }
    }

    void info(std::string_view msg)  { write("INFO ", msg); }
    void warn(std::string_view msg)  { write("WARN ", msg); }
    void error(std::string_view msg) { write("ERROR", msg); }

private:
    void write(const char* level, std::string_view msg) {
        std::ostream& os = file_ ? static_cast<std::ostream&>(file_)
                                 : std::cerr;
        os << timestamp() << " [" << level << "] " << msg << '\n';
        os.flush();   // daemon logs must survive an abrupt kill
    }

    static std::string timestamp() {
        using namespace std::chrono;
        const auto now = system_clock::now();
        const auto ms  = duration_cast<milliseconds>(now.time_since_epoch())
                             .count() % 1000;
        const std::time_t t = system_clock::to_time_t(now);
        std::tm tm{};
        localtime_r(&t, &tm);
        char buf[80];
        std::snprintf(buf, sizeof(buf),
                      "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec,
                      static_cast<int>(ms));
        return buf;
    }

    std::ofstream file_;
};

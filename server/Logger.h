#pragma once
// Asynchronous RAII file logger for the server daemon.
//
// Producer / consumer split: any thread (in practice the libuv loop thread)
// formats a line and pushes it onto a bounded queue; one dedicated writer
// thread drains the queue, writes and flushes. The event loop therefore
// never waits on disk I/O, however many malformed lines a poisoned upload
// produces. Lines are timestamped by the producer so ordering in the file
// reflects when events happened, not when they were flushed.
//
// Overflow policy: if the queue reaches kMaxQueued the producer blocks until
// the writer catches up (backpressure, never silent loss). The cap is large
// enough that this only triggers under pathological disk stalls.
//
// Dependency-free (no spdlog) so the server builds with libuv + the standard
// library only. The writer thread is owned by the Logger and joined in the
// destructor after draining everything still queued.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

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
        writer_ = std::thread(&Logger::run, this);
    }

    // Drains whatever is still queued, then joins the writer thread.
    ~Logger() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stopping_ = true;
        }
        cv_.notify_all();
        if (writer_.joinable()) writer_.join();
    }

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void info(std::string_view msg)  { enqueue("INFO ", msg); }
    void warn(std::string_view msg)  { enqueue("WARN ", msg); }
    void error(std::string_view msg) { enqueue("ERROR", msg); }

    // Diagnostics: how many times a producer had to wait for the writer.
    uint64_t backpressureEvents() const {
        std::lock_guard<std::mutex> lk(mu_);
        return backpressure_;
    }

private:
    static constexpr size_t kMaxQueued = 50000;   // ~200 B/line -> <= ~10 MB
    static constexpr std::chrono::milliseconds kFlushInterval{5};

    void enqueue(const char* level, std::string_view msg) {
        std::string line;
        line.reserve(32 + msg.size());
        line += timestamp();
        line += " [";
        line += level;
        line += "] ";
        line += msg;
        line += '\n';

        std::unique_lock<std::mutex> lk(mu_);
        if (queue_.size() >= kMaxQueued) {
            ++backpressure_;
            spaceCv_.wait(lk, [this] { return queue_.size() < kMaxQueued; });
        }
        queue_.push_back(std::move(line));
        // Deliberately no notify here: the writer polls every kFlushInterval.
        // Waking it per line would put a futex syscall (and a scheduler
        // bounce) on the producer's — i.e. the event loop's — critical path,
        // which measured slower than the old synchronous write. A few ms of
        // flush latency is the price; shutdown still drains synchronously.
    }

    void run() {
        std::deque<std::string> batch;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(mu_);
                // wait_until(system_clock) rather than wait_for: libstdc++
                // maps wait_for to pthread_cond_clockwait, which GCC 11's
                // ThreadSanitizer does not intercept (false "double lock").
                // A clock jump only shifts one 5 ms poll, which is harmless.
                cv_.wait_until(lk,
                               std::chrono::system_clock::now() + kFlushInterval,
                               [this] { return stopping_; });
                if (queue_.empty() && stopping_) return;
                if (queue_.empty()) continue;
                batch.swap(queue_);       // take everything in one go
            }
            spaceCv_.notify_all();
            // One write + one flush per batch instead of per line: under load
            // the writer naturally coalesces, which is where the throughput
            // win over the old synchronous per-line flush comes from.
            std::ostream& os = file_ ? static_cast<std::ostream&>(file_)
                                     : std::cerr;
            for (const std::string& l : batch) os << l;
            os.flush();   // daemon logs must survive an abrupt kill
            batch.clear();
        }
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

    std::ofstream            file_;
    mutable std::mutex       mu_;
    std::condition_variable  cv_;        // writer: periodic wake or stop
    std::condition_variable  spaceCv_;   // producer waits: queue has room
    std::deque<std::string>  queue_;
    bool                     stopping_ = false;
    uint64_t                 backpressure_ = 0;
    std::thread              writer_;
};

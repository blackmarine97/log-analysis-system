#pragma once
// ResultCache: recently produced result.csv bodies, keyed by the upload's
// (fileSize, payload CRC-32) — the same values the client already holds.
//
// Purpose: response-direction robustness. If the connection drops while the
// *result* is in flight, the client re-requests it with a 12-byte
// FetchResult frame instead of re-uploading 500 MB. The analysis the server
// just paid for is not thrown away with the socket.
//
// Two tiers:
//   memory  - bounded by total bytes (8 MB default), oldest evicted first.
//   disk    - each result is also written to <dir>/<size>-<crc>.csv on the
//             libuv thread pool (write-to-temp + atomic rename, the same
//             pattern CsvWriter uses), so the cache SURVIVES A SERVER
//             RESTART. A memory miss falls back to reading that file; a
//             hit is promoted back into memory. The directory is pruned
//             oldest-first past its own byte budget (64 MB default).
//
// A miss in both tiers is answered with an explicit Error and the client
// falls back to a normal re-upload. Passing an empty dir disables the disk
// tier (memory-only, the pre-persistence behaviour).
//
// Threading: put()/get() run on the loop thread; only the blocking file
// write + prune run on the pool. Concurrent prunes may race to delete the
// same file — remove() failing because the other job won is harmless and
// ignored. uv_run() does not return while a work request is pending, so
// every queued write completes before the cache is destroyed.

#include "Logger.h"

#include <uv.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class ResultCache {
public:
    ResultCache(uv_loop_t* loop, Logger& log, std::string dir,
                size_t maxMemBytes = 8 * 1024 * 1024,
                uint64_t maxDiskBytes = 64 * 1024 * 1024)
        : loop_(loop), log_(log), dir_(std::move(dir)),
          maxMemBytes_(maxMemBytes), maxDiskBytes_(maxDiskBytes) {
        if (dir_.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
        if (ec) {
            log_.warn("result cache: cannot create '" + dir_ + "' (" +
                      ec.message() + "), persistence disabled");
            dir_.clear();
        }
    }

    ResultCache(const ResultCache&) = delete;
    ResultCache& operator=(const ResultCache&) = delete;

    void put(uint64_t fileSize, uint32_t crc, std::string csv) {
        putMemory(fileSize, crc, csv);
        if (dir_.empty()) return;

        auto job = std::make_unique<Job>();
        job->owner   = this;
        job->dstPath = filePath(fileSize, crc);
        job->tmpPath = job->dstPath + ".tmp." + std::to_string(nextJobId_++);
        job->body    = std::move(csv);
        job->req.data = job.get();
        Job* raw = job.get();
        jobs_.emplace(raw, std::move(job));
        const int rc = uv_queue_work(loop_, &raw->req,
                                     &ResultCache::work,
                                     &ResultCache::afterWork);
        if (rc != 0) {
            log_.warn("result cache: cannot queue write (" +
                      std::string(uv_strerror(rc)) + ')');
            jobs_.erase(raw);
        }
    }

    std::optional<std::string> get(uint64_t fileSize, uint32_t crc) {
        for (const auto& e : entries_)
            if (e.fileSize == fileSize && e.crc == crc) return e.csv;
        if (dir_.empty()) return std::nullopt;

        // Disk fallback: this is what survives a server restart. The file
        // is small (a result.csv); reading it synchronously on the loop
        // thread is a few KB for a rare recovery path.
        std::ifstream in(filePath(fileSize, crc),
                         std::ios::binary | std::ios::ate);
        if (!in) return std::nullopt;
        const auto size = in.tellg();
        if (size <= 0 || static_cast<uint64_t>(size) > kMaxDiskEntryBytes)
            return std::nullopt;                 // absent or implausible
        std::string csv(static_cast<size_t>(size), '\0');
        in.seekg(0);
        if (!in.read(csv.data(), size)) return std::nullopt;
        putMemory(fileSize, crc, csv);           // promote for next time
        return csv;
    }

    size_t bytes() const { return bytes_; }

private:
    // Refuse to slurp a file that cannot be a sane cached result (e.g. a
    // foreign file dropped into the cache directory).
    static constexpr uint64_t kMaxDiskEntryBytes = 16 * 1024 * 1024;

    struct Entry {
        uint64_t    fileSize;
        uint32_t    crc;
        std::string csv;
    };

    struct Job {
        uv_work_t    req{};
        ResultCache* owner = nullptr;
        std::string  dstPath;
        std::string  tmpPath;
        std::string  body;
        std::string  error;   // filled on the pool thread; empty on success
    };

    std::string filePath(uint64_t fileSize, uint32_t crc) const {
        char name[64];
        std::snprintf(name, sizeof(name), "%llu-%08x.csv",
                      static_cast<unsigned long long>(fileSize), crc);
        return dir_ + '/' + name;
    }

    void putMemory(uint64_t fileSize, uint32_t crc, const std::string& csv) {
        if (csv.size() > maxMemBytes_) return;
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->fileSize == fileSize && it->crc == crc) {
                bytes_ -= it->csv.size();
                entries_.erase(it);
                break;
            }
        }
        bytes_ += csv.size();
        entries_.push_back({fileSize, crc, csv});
        while (bytes_ > maxMemBytes_ && !entries_.empty()) {
            bytes_ -= entries_.front().csv.size();
            entries_.pop_front();
        }
    }

    // ---- pool thread: blocking I/O allowed ------------------------------
    static void work(uv_work_t* req) {
        auto* job = static_cast<Job*>(req->data);
        {
            std::ofstream out(job->tmpPath,
                              std::ios::trunc | std::ios::binary);
            if (!out) { job->error = "cannot open " + job->tmpPath; return; }
            out << job->body;
            out.flush();
            if (!out) {
                job->error = "failed writing " + job->tmpPath;
                std::remove(job->tmpPath.c_str());
                return;
            }
        }
        if (std::rename(job->tmpPath.c_str(), job->dstPath.c_str()) != 0) {
            job->error = "cannot rename " + job->tmpPath;
            std::remove(job->tmpPath.c_str());
            return;
        }
        prune(job->owner->dir_, job->owner->maxDiskBytes_);
    }

    // Oldest-first deletion past the disk budget. std::error_code overloads
    // throughout: a file another prune already removed is not an error.
    static void prune(const std::string& dir, uint64_t budget) {
        namespace fs = std::filesystem;
        struct F { fs::file_time_type t; uint64_t size; fs::path p; };
        std::vector<F> files;
        uint64_t total = 0;
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (!it->is_regular_file(ec) ||
                it->path().extension() != ".csv") continue;
            const uint64_t sz = it->file_size(ec);
            if (ec) continue;
            files.push_back({it->last_write_time(ec), sz, it->path()});
            total += sz;
        }
        if (total <= budget) return;
        std::sort(files.begin(), files.end(),
                  [](const F& a, const F& b) { return a.t < b.t; });
        for (const auto& f : files) {
            if (total <= budget) break;
            fs::remove(f.p, ec);
            if (!ec) total -= f.size;
        }
    }

    // ---- back on the loop thread ---------------------------------------
    static void afterWork(uv_work_t* req, int status) {
        auto* job = static_cast<Job*>(req->data);
        ResultCache* self = job->owner;
        if (status != 0)
            self->log_.warn("result cache: write cancelled (" +
                            std::string(uv_strerror(status)) + ')');
        else if (!job->error.empty())
            self->log_.warn("result cache: " + job->error);
        self->jobs_.erase(job);   // last use of `job`
    }

    uv_loop_t*  loop_;
    Logger&     log_;
    std::string dir_;
    size_t      maxMemBytes_;
    uint64_t    maxDiskBytes_;
    size_t      bytes_ = 0;
    uint64_t    nextJobId_ = 1;
    std::deque<Entry> entries_;   // front = oldest
    std::unordered_map<Job*, std::unique_ptr<Job>> jobs_;
};

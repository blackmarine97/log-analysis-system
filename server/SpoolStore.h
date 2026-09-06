#pragma once
// SpoolStore: on-disk copies of partially received uploads, so a broken
// upload can be RESUMED instead of restarted.
//
// Every UploadLog payload byte is appended to <dir>/<uploadId>.part while it
// streams through the parser. If the connection dies at 300 MB, those bytes
// are already on disk; the client's next attempt asks ResumeQuery ->
// ResumeOffset and continues from byte 300M. On success the spool is
// deleted (the finished analysis lives in ResultCache instead). Because the
// spool is a plain file, resume also works across a server restart.
//
// Correctness does not depend on the spool: the CRC-32 trailer covers the
// whole file, so a stale or mismatched spool (different file, truncated
// write) fails verification and the client falls back to a full upload.
//
// Bounded: before creating a new spool, oldest .part files are pruned until
// the directory fits the byte budget (default 2 GiB — about four
// half-finished 500 MB uploads). Appends are small buffered writes on the
// loop thread (page cache; microseconds per 64 KiB chunk).
//
// An empty dir disables spooling: uploads still work, resume replies 0.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Logger.h"

class SpoolStore {
public:
    SpoolStore(Logger& log, std::string dir,
               uint64_t maxDiskBytes = 2ull * 1024 * 1024 * 1024)
        : log_(log), dir_(std::move(dir)), maxDiskBytes_(maxDiskBytes) {
        if (dir_.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
        if (ec) {
            log_.warn("spool: cannot create '" + dir_ + "' (" + ec.message() +
                      "), resume disabled");
            dir_.clear();
        }
    }

    SpoolStore(const SpoolStore&) = delete;
    SpoolStore& operator=(const SpoolStore&) = delete;

    bool enabled() const { return !dir_.empty(); }

    std::string pathFor(uint64_t uploadId) const {
        char name[32];
        std::snprintf(name, sizeof(name), "%016llx.part",
                      static_cast<unsigned long long>(uploadId));
        return dir_ + '/' + name;
    }

    // Bytes already spooled for this upload; 0 when there is nothing usable.
    // A spool larger than the file being announced is stale (a different or
    // truncated file reused the id) and is discarded.
    uint64_t offsetOf(uint64_t uploadId, uint64_t fileSize) const {
        if (dir_.empty()) return 0;
        std::error_code ec;
        const auto size = std::filesystem::file_size(pathFor(uploadId), ec);
        if (ec) return 0;
        if (size > fileSize) {
            std::filesystem::remove(pathFor(uploadId), ec);
            return 0;
        }
        return size;
    }

    // Open the spool for appending at exactly `offset` (0 truncates). Prunes
    // old spools first so a new upload always has room. Returns an unopened
    // stream when spooling is disabled or the open fails (the caller treats
    // that as "no spool": upload proceeds, resume just won't be offered).
    std::ofstream openAppend(uint64_t uploadId, uint64_t offset) {
        std::ofstream out;
        if (dir_.empty()) return out;
        prune();
        const std::string p = pathFor(uploadId);
        std::error_code ec;
        const auto cur = std::filesystem::file_size(p, ec);
        if (offset == 0 || ec || cur != offset) {
            out.open(p, std::ios::binary | std::ios::trunc);
        } else {
            out.open(p, std::ios::binary | std::ios::app);
        }
        if (!out) log_.warn("spool: cannot open " + p);
        return out;
    }

    void remove(uint64_t uploadId) {
        if (dir_.empty()) return;
        std::error_code ec;
        std::filesystem::remove(pathFor(uploadId), ec);
    }

private:
    void prune() {
        namespace fs = std::filesystem;
        struct F { fs::file_time_type t; uint64_t size; fs::path p; };
        std::vector<F> files;
        uint64_t total = 0;
        std::error_code ec;
        for (fs::directory_iterator it(dir_, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (!it->is_regular_file(ec) ||
                it->path().extension() != ".part") continue;
            const uint64_t sz = it->file_size(ec);
            if (ec) continue;
            files.push_back({it->last_write_time(ec), sz, it->path()});
            total += sz;
        }
        if (total <= maxDiskBytes_) return;
        std::sort(files.begin(), files.end(),
                  [](const F& a, const F& b) { return a.t < b.t; });
        for (const auto& f : files) {
            if (total <= maxDiskBytes_) break;
            fs::remove(f.p, ec);
            if (!ec) {
                total -= f.size;
                log_.info("spool: pruned " + f.p.filename().string());
            }
        }
    }

    Logger&     log_;
    std::string dir_;
    uint64_t    maxDiskBytes_;
};

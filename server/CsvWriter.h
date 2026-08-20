#pragma once
// CsvWriter: writes each session's result.csv copy off the event loop.
//
// The blocking part (open / write / fsync-ish flush / rename) runs on the
// libuv thread pool via uv_queue_work(); only the completion log runs back on
// the loop thread. The loop therefore never waits on disk, even on a slow or
// networked filesystem.
//
// Ownership: CsvWriter owns every in-flight Job via std::unique_ptr and erases
// it from its map in the after-work callback — the same "destroy only after
// libuv is done with it" pattern TcpServer uses for sessions. uv_run() does
// not return while a work request is pending, so all jobs complete before
// the writer is destroyed.
//
// Atomicity: the body goes to a per-session temporary which is then renamed
// over the shared target; rename(2) is atomic, so concurrent sessions never
// interleave inside the same file and a reader never sees a partial CSV.

#include "Logger.h"

#include <uv.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

class CsvWriter {
public:
    CsvWriter(uv_loop_t* loop, Logger& log, std::string csvPath)
        : loop_(loop), log_(log), csvPath_(std::move(csvPath)) {}

    CsvWriter(const CsvWriter&) = delete;
    CsvWriter& operator=(const CsvWriter&) = delete;

    bool enabled() const { return !csvPath_.empty(); }

    // Queue one write. `body` is moved into the job and outlives the work.
    void submit(uint64_t sessionId, std::string body) {
        if (!enabled()) return;
        auto job = std::make_unique<Job>();
        job->owner     = this;
        job->sessionId = sessionId;
        job->tmpPath   = csvPath_ + ".tmp." + std::to_string(sessionId);
        job->dstPath   = csvPath_;
        job->body      = std::move(body);
        job->req.data  = job.get();

        Job* raw = job.get();
        jobs_.emplace(raw, std::move(job));
        const int rc = uv_queue_work(loop_, &raw->req,
                                     &CsvWriter::work, &CsvWriter::afterWork);
        if (rc != 0) {
            log_.warn("session " + std::to_string(sessionId) +
                      ": cannot queue csv write (" + uv_strerror(rc) + ')');
            jobs_.erase(raw);
        }
    }

    size_t pending() const { return jobs_.size(); }

private:
    struct Job {
        uv_work_t   req{};
        CsvWriter*  owner = nullptr;
        uint64_t    sessionId = 0;
        std::string tmpPath;
        std::string dstPath;
        std::string body;
        std::string error;     // empty on success; filled on the pool thread
    };

    // ---- runs on a libuv thread-pool thread: blocking I/O allowed ----------
    static void work(uv_work_t* req) {
        auto* job = static_cast<Job*>(req->data);
        {
            std::ofstream out(job->tmpPath, std::ios::trunc | std::ios::binary);
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
            job->error = "cannot rename " + job->tmpPath + " -> " + job->dstPath;
            std::remove(job->tmpPath.c_str());
        }
    }

    // ---- back on the loop thread ------------------------------------------
    static void afterWork(uv_work_t* req, int status) {
        auto* job  = static_cast<Job*>(req->data);
        CsvWriter* self = job->owner;
        const std::string prefix = "session " + std::to_string(job->sessionId);
        if (status != 0) {
            self->log_.warn(prefix + ": csv write cancelled (" +
                            uv_strerror(status) + ')');
        } else if (!job->error.empty()) {
            self->log_.warn(prefix + ": " + job->error);
        } else {
            self->log_.info(prefix + ": csv copy written to " + job->dstPath +
                            " (" + std::to_string(job->body.size()) + " bytes)");
        }
        self->jobs_.erase(job);   // last use of `job`
    }

    uv_loop_t*  loop_;
    Logger&     log_;
    std::string csvPath_;
    std::unordered_map<Job*, std::unique_ptr<Job>> jobs_;
};

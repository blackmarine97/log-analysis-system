#pragma once
// TcpServer: libuv listener that owns all live Sessions via std::unique_ptr.
// A session is erased (and thus destroyed) only after libuv reports its
// handle fully closed, which makes use-after-free impossible by construction.

#include "Logger.h"
#include "ResultCache.h"
#include "SpoolStore.h"
#include "Session.h"

#include <uv.h>

#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>

class TcpServer {
public:
    // maxSessions: hard cap on concurrent connections. Arrivals beyond it
    // are accepted only to receive an Error frame and are closed, so a
    // connection flood cannot exhaust fds / per-session memory.
    TcpServer(uv_loop_t* loop, Logger& log, CsvWriter& csv,
              ResultCache& cache, SpoolStore& spool,
              uint64_t idleTimeoutMs, size_t maxSessions)
        : loop_(loop), log_(log), csv_(csv), cache_(cache), spool_(spool),
          idleTimeoutMs_(idleTimeoutMs), maxSessions_(maxSessions) {
        uv_tcp_init(loop_, &listener_);
        listener_.data = this;
    }

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    bool listen(const std::string& bindAddr, int port, int backlog = 8) {
        sockaddr_in addr{};
        // uv_ip4_addr() leaves sin_addr zeroed on failure, which is INADDR_ANY.
        // Ignoring the return code would silently turn a typo ("127.0.0.O") or
        // a hostname ("localhost") into a bind on *every* interface.
        int rc = uv_ip4_addr(bindAddr.c_str(), port, &addr);
        if (rc != 0) {
            log_.error("invalid bind address '" + bindAddr +
                       "' (expected an IPv4 literal such as 0.0.0.0): " +
                       uv_strerror(rc));
            return false;
        }
        rc = uv_tcp_bind(&listener_,
                         reinterpret_cast<const sockaddr*>(&addr), 0);
        if (rc == 0)
            rc = uv_listen(reinterpret_cast<uv_stream_t*>(&listener_),
                           backlog, &TcpServer::onConnection);
        if (rc != 0) {
            log_.error("listen on " + bindAddr + ':' + std::to_string(port) +
                       " failed: " + uv_strerror(rc));
            return false;
        }
        log_.info("listening on " + bindAddr + ':' + std::to_string(port));
        return true;
    }

    // Graceful shutdown: stop accepting, close every live session. The loop
    // exits naturally once the last handle finishes closing.
    void shutdown() {
        if (shuttingDown_) return;
        shuttingDown_ = true;
        log_.info("shutdown requested, closing " +
                  std::to_string(sessions_.size()) + " active session(s)");
        uv_close(reinterpret_cast<uv_handle_t*>(&listener_), nullptr);
        for (auto& [ptr, sess] : sessions_) sess->close();
    }

private:
    static void onConnection(uv_stream_t* listener, int status) {
        auto* self = static_cast<TcpServer*>(listener->data);
        if (status != 0) {
            self->log_.error(std::string("accept error: ") +
                             uv_strerror(status));
            return;
        }
        const uint64_t id = self->nextId_++;
        auto session = std::make_unique<Session>(
            self->loop_, id, self->log_, self->csv_, self->cache_,
            self->spool_, self->idleTimeoutMs_,
            [self](Session* s) { self->sessions_.erase(s); });

        Session* raw = session.get();
        self->sessions_.emplace(raw, std::move(session));

        // At capacity (the new session included): refuse politely. The
        // rejected session still lives in the map until its close callback,
        // like any other, so no special lifetime path exists.
        if (self->sessions_.size() > self->maxSessions_) {
            self->log_.warn("session " + std::to_string(id) +
                            ": rejected, connection limit (" +
                            std::to_string(self->maxSessions_) + ") reached");
            if (!raw->acceptBusy(listener)) raw->close();
            return;
        }

        self->log_.info("session " + std::to_string(id) + ": accepted");
        if (!raw->accept(listener)) {
            self->log_.error("session " + std::to_string(id) +
                             ": accept/read_start failed");
            raw->close();   // close_cb will erase it from the map
        }
    }

    uv_loop_t*  loop_;
    uv_tcp_t    listener_{};
    Logger&     log_;
    CsvWriter&  csv_;
    ResultCache& cache_;
    SpoolStore& spool_;
    uint64_t    idleTimeoutMs_;
    size_t      maxSessions_;
    bool        shuttingDown_ = false;
    uint64_t    nextId_ = 1;
    std::unordered_map<Session*, std::unique_ptr<Session>> sessions_;
};

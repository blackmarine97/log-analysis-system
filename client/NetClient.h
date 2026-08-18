#pragma once
// NetClient: worker-thread uploader for the Windows GUI client.
//
// The UI thread only starts/cancels transfers and polls atomics + a small
// mutex-guarded status block, so the message pump never blocks on I/O:
// the 500 MB upload runs entirely on the worker std::thread.
//
// Robustness: every socket is wrapped in UniqueSocket (RAII), all blocking
// calls carry SO_SNDTIMEO/SO_RCVTIMEO, and a cancel/yanked-cable simply
// surfaces as a failed send()/recv() -> worker logs, cleans up, exits.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include "../common/Protocol.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

// Process-wide WinSock init/teardown (RAII).
class WsaSession {
public:
    WsaSession()  { ok_ = (WSAStartup(MAKEWORD(2, 2), &data_) == 0); }
    ~WsaSession() { if (ok_) WSACleanup(); }
    bool ok() const { return ok_; }

private:
    WSADATA data_{};
    bool    ok_ = false;
};

class UniqueSocket {
public:
    explicit UniqueSocket(SOCKET s = INVALID_SOCKET) : s_(s) {}
    ~UniqueSocket() { reset(); }
    UniqueSocket(const UniqueSocket&) = delete;
    UniqueSocket& operator=(const UniqueSocket&) = delete;
    void reset(SOCKET s = INVALID_SOCKET) {
        if (s_ != INVALID_SOCKET) closesocket(s_);
        s_ = s;
    }
    SOCKET get() const { return s_; }
    explicit operator bool() const { return s_ != INVALID_SOCKET; }

private:
    SOCKET s_;
};

enum class XferState {
    Idle, Connecting, Uploading, WaitingResult, Done, Failed, Cancelled
};

class NetClient {
public:
    ~NetClient() {
        cancel();
        if (worker_.joinable()) worker_.join();
    }

    bool busy() const {
        const XferState s = state_.load();
        return s == XferState::Connecting || s == XferState::Uploading ||
               s == XferState::WaitingResult;
    }

    // Kicks off one upload on the worker thread. Returns false while a
    // previous transfer is still running.
    bool start(std::string host, int port, std::wstring filePath) {
        if (busy()) return false;
        if (worker_.joinable()) worker_.join();   // reap finished worker

        state_ = XferState::Connecting;
        bytesSent_ = 0;
        bytesTotal_ = 0;
        {
            std::lock_guard<std::mutex> lk(mu_);
            resultCsv_.clear();
            status_ = "connecting to " + host + ':' + std::to_string(port);
        }
        cancelFlag_ = false;
        worker_ = std::thread(&NetClient::run, this, std::move(host), port,
                              std::move(filePath));
        return true;
    }

    // Called from the UI thread. shutdown() makes any blocking send/recv on
    // the worker fail immediately; the worker then cleans up and exits.
    void cancel() {
        cancelFlag_ = true;
        std::lock_guard<std::mutex> lk(sockMu_);
        if (liveSock_ != INVALID_SOCKET) shutdown(liveSock_, SD_BOTH);
    }

    // ---- UI-thread observers -------------------------------------------
    XferState state() const   { return state_.load(); }
    uint64_t bytesSent() const  { return bytesSent_.load(); }
    uint64_t bytesTotal() const { return bytesTotal_.load(); }
    float progress() const {
        const uint64_t total = bytesTotal_.load();
        return total ? static_cast<float>(bytesSent_.load()) /
                       static_cast<float>(total)
                     : 0.0f;
    }
    std::string status() {
        std::lock_guard<std::mutex> lk(mu_);
        return status_;
    }
    std::string resultCsv() {
        std::lock_guard<std::mutex> lk(mu_);
        return resultCsv_;
    }
    // Moves accumulated log lines out for the UI log pane.
    std::vector<std::string> drainLog() {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<std::string> out(log_.begin(), log_.end());
        log_.clear();
        return out;
    }

private:
    static constexpr size_t kChunkSize = 1 << 20;        // 1 MiB per read
    static constexpr DWORD  kIoTimeoutMs = 30 * 1000;
    static constexpr uint64_t kMaxResultBytes = 256ull << 20;

    void setStatus(std::string s) {
        std::lock_guard<std::mutex> lk(mu_);
        status_ = std::move(s);
        log_.push_back(status_);
        while (log_.size() > 500) log_.pop_front();
    }

    void finish(XferState st, std::string msg) {
        setStatus(std::move(msg));
        state_ = st;
    }

    bool sendAll(SOCKET s, const char* data, size_t len) {
        size_t sent = 0;
        while (sent < len) {
            if (cancelFlag_) return false;
            const int n = ::send(s, data + sent,
                                 static_cast<int>(len - sent), 0);
            if (n == SOCKET_ERROR) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    bool recvAll(SOCKET s, char* data, size_t len) {
        size_t got = 0;
        while (got < len) {
            if (cancelFlag_) return false;
            const int n = ::recv(s, data + got,
                                 static_cast<int>(len - got), 0);
            if (n <= 0) return false;
            got += static_cast<size_t>(n);
        }
        return true;
    }

    void run(std::string host, int port, std::wstring filePath) {
        // ---- open the file ---------------------------------------------
        std::ifstream in(filePath, std::ios::binary | std::ios::ate);
        if (!in) { finish(XferState::Failed, "cannot open selected file"); return; }
        const uint64_t fileSize = static_cast<uint64_t>(in.tellg());
        in.seekg(0);
        bytesTotal_ = fileSize;

        // ---- resolve + connect -----------------------------------------
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(),
                        &hints, &res) != 0 || !res) {
            finish(XferState::Failed, "cannot resolve host " + host);
            return;
        }
        UniqueSocket sock(::socket(res->ai_family, res->ai_socktype,
                                   res->ai_protocol));
        int rc = SOCKET_ERROR;
        if (sock) rc = ::connect(sock.get(), res->ai_addr,
                                 static_cast<int>(res->ai_addrlen));
        freeaddrinfo(res);
        if (!sock || rc == SOCKET_ERROR) {
            finish(XferState::Failed,
                   "connect failed (WSA error " +
                   std::to_string(WSAGetLastError()) + ')');
            return;
        }
        setsockopt(sock.get(), SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&kIoTimeoutMs),
                   sizeof(kIoTimeoutMs));
        setsockopt(sock.get(), SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&kIoTimeoutMs),
                   sizeof(kIoTimeoutMs));
        {
            std::lock_guard<std::mutex> lk(sockMu_);
            liveSock_ = sock.get();       // expose for cancel()
        }

        // ---- upload frame ----------------------------------------------
        state_ = XferState::Uploading;
        setStatus("uploading " + std::to_string(fileSize >> 20) + " MiB...");

        std::array<char, proto::kHeaderSize> hdr{};
        proto::encodeHeader({proto::MsgType::UploadLog, fileSize}, hdr);
        bool ok = sendAll(sock.get(), hdr.data(), hdr.size());

        std::vector<char> buf(kChunkSize);
        uint64_t sent = 0;
        while (ok && sent < fileSize) {
            // Never read or send past the size announced in the header: if the
            // log grows while we upload, the extra bytes would land after the
            // frame payload and desynchronize the stream.
            const uint64_t remaining = fileSize - sent;
            const size_t want = remaining < buf.size()
                                    ? static_cast<size_t>(remaining)
                                    : buf.size();
            in.read(buf.data(), static_cast<std::streamsize>(want));
            const std::streamsize n = in.gcount();
            if (n <= 0) { ok = false; break; }
            ok = sendAll(sock.get(), buf.data(), static_cast<size_t>(n));
            if (ok) {
                sent += static_cast<uint64_t>(n);
                bytesSent_ = sent;
            }
        }
        if (!ok) {
            dropSocket(sock);
            finish(cancelFlag_ ? XferState::Cancelled : XferState::Failed,
                   cancelFlag_ ? "upload cancelled"
                               : "upload failed (connection lost, WSA error " +
                                 std::to_string(WSAGetLastError()) + ')');
            return;
        }

        // ---- response frame --------------------------------------------
        state_ = XferState::WaitingResult;
        setStatus("upload complete, waiting for server analysis...");

        std::array<char, proto::kHeaderSize> rhdr{};
        std::string body;
        ok = recvAll(sock.get(), rhdr.data(), rhdr.size());
        std::optional<proto::FrameHeader> h;
        if (ok) {
            h = proto::decodeHeader(std::string_view(rhdr.data(), rhdr.size()));
            // Only the two server->client types are legal here; an UploadLog
            // reply would otherwise be saved as if it were the CSV.
            ok = h.has_value() && h->payloadSize <= kMaxResultBytes &&
                 (h->type == proto::MsgType::ResultCsv ||
                  h->type == proto::MsgType::Error);
        }
        if (ok && h->payloadSize > 0) {
            body.resize(static_cast<size_t>(h->payloadSize));
            ok = recvAll(sock.get(), body.data(), body.size());
        }
        dropSocket(sock);

        if (!ok) {
            finish(cancelFlag_ ? XferState::Cancelled : XferState::Failed,
                   cancelFlag_ ? "cancelled while waiting for result"
                               : "failed to receive result from server");
            return;
        }
        if (h->type == proto::MsgType::Error) {
            finish(XferState::Failed, "server rejected upload: " + body);
            return;
        }
        const size_t csvBytes = body.size();
        {
            std::lock_guard<std::mutex> lk(mu_);
            resultCsv_ = std::move(body);
        }
        finish(XferState::Done,
               "result.csv received (" + std::to_string(csvBytes) + " bytes)");
    }

    void dropSocket(UniqueSocket& sock) {
        std::lock_guard<std::mutex> lk(sockMu_);
        liveSock_ = INVALID_SOCKET;
        sock.reset();
    }

    std::thread            worker_;
    std::atomic<XferState> state_{XferState::Idle};
    std::atomic<uint64_t>  bytesSent_{0};
    std::atomic<uint64_t>  bytesTotal_{0};
    std::atomic<bool>      cancelFlag_{false};

    std::mutex  sockMu_;
    SOCKET      liveSock_ = INVALID_SOCKET;

    std::mutex              mu_;
    std::string             status_;
    std::string             resultCsv_;
    std::deque<std::string> log_;
};

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
#include <memory>
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
    // Upload retry schedule (same backoff): a drop mid-upload is retried
    // with RESUME — the client asks the server how much it already has and
    // continues from there, so the cost of a retry is the missing tail,
    // not the whole 500 MB.
    static constexpr int   kUploadRetries        = 5;
    // Re-fetch schedule: 5 attempts with exponential backoff
    // (0.5, 1, 2, 4, 8 s = ~15.5 s window) so even a server *restart* —
    // whose disk-persisted cache still holds the result — is covered.
    static constexpr int   kFetchRetries         = 5;
    static constexpr DWORD kFetchBaseDelayMs     = 500;
    static constexpr DWORD kFetchMaxDelayMs      = 8000;

    // Human-readable text for the WSA errors a user can realistically hit;
    // the raw code is still appended so the log stays diagnosable.
    static std::string wsaText(int e) {
        switch (e) {
            case 0:               return "malformed or unexpected server response";
            case WSAECONNREFUSED: return "server not reachable - is it running?";
            case WSAETIMEDOUT:    return "timed out - server not responding";
            case WSAECONNRESET:   return "connection reset by the server";
            case WSAECONNABORTED: return "connection dropped (I/O timeout)";
            case WSAEHOSTUNREACH: return "host unreachable - check the address";
            case WSAENETUNREACH:  return "network unreachable";
            case WSAENETDOWN:     return "network is down";
            default:              return "network error";
        }
    }
    static std::string wsaMsg(const char* what, int e) {
        return std::string(what) + ": " + wsaText(e) +
               " (WSA " + std::to_string(e) + ')';
    }

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

    // Resolve, connect, set timeouts and register the socket for cancel().
    // On failure `err` carries a user-readable message.
    bool openConnection(const std::string& host, int port,
                        UniqueSocket& sock, std::string& err) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        // getaddrinfo() hands back a heap list that must be released with
        // freeaddrinfo(); own it through unique_ptr so every early return
        // below frees it (RAII, no manual cleanup path to forget).
        addrinfo* raw = nullptr;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(),
                        &hints, &raw) != 0 || !raw) {
            err = "cannot resolve host " + host;
            return false;
        }
        std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> res(raw,
                                                               &freeaddrinfo);
        sock.reset(::socket(res->ai_family, res->ai_socktype,
                            res->ai_protocol));
        int rc = SOCKET_ERROR;
        if (sock) rc = ::connect(sock.get(), res->ai_addr,
                                 static_cast<int>(res->ai_addrlen));
        res.reset();
        if (!sock || rc == SOCKET_ERROR) {
            err = wsaMsg("connect failed", WSAGetLastError());
            return false;
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
        return true;
    }

    // Receives one response frame (header + body + CRC trailer). Returns
    // false on any socket/framing failure; on success `crcOk` says whether
    // the trailer matched the body.
    bool recvResponse(SOCKET s, proto::MsgType& type, std::string& body,
                      bool& crcOk) {
        std::array<char, proto::kHeaderSize> rhdr{};
        if (!recvAll(s, rhdr.data(), rhdr.size())) return false;
        const auto h = proto::decodeHeader(
            std::string_view(rhdr.data(), rhdr.size()));
        // Only server->client types are legal here; an UploadLog reply
        // would otherwise be saved as if it were the CSV.
        if (!h || h->payloadSize > kMaxResultBytes ||
            (h->type != proto::MsgType::ResultCsv &&
             h->type != proto::MsgType::Error &&
             h->type != proto::MsgType::ResumeOffset))
            return false;
        body.clear();
        if (h->payloadSize > 0) {
            body.resize(static_cast<size_t>(h->payloadSize));
            if (!recvAll(s, body.data(), body.size())) return false;
        }
        std::array<char, proto::kTrailerSize> tr{};
        if (!recvAll(s, tr.data(), tr.size())) return false;
        crcOk = proto::decodeTrailer(tr) ==
                proto::crc32Final(proto::crc32Update(
                    proto::kCrc32Init, body.data(), body.size()));
        type = h->type;
        return true;
    }

    // Deterministic upload id: CRC of the first MiB + low 32 bits of the
    // size. Needs no state across attempts or app restarts; a collision is
    // caught by the full-file CRC trailer and merely costs a fresh upload.
    static uint64_t computeUploadId(std::ifstream& in, uint64_t fileSize) {
        std::vector<char> buf(1 << 20);
        in.clear();
        in.seekg(0);
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto n = in.gcount();
        in.clear();
        in.seekg(0);
        const uint32_t c = proto::crc32Final(proto::crc32Update(
            proto::kCrc32Init, buf.data(),
            static_cast<size_t>(n > 0 ? n : 0)));
        return static_cast<uint64_t>(c) |
               (static_cast<uint64_t>(static_cast<uint32_t>(fileSize)) << 32);
    }

    // "How much of this upload do you already have?" — 0 on any failure,
    // which simply means a full upload. Uses its own short connection.
    uint64_t queryResumeOffset(const std::string& host, int port,
                               uint64_t uploadId, uint64_t fileSize) {
        UniqueSocket s;
        std::string err;
        if (!openConnection(host, port, s, err)) return 0;
        std::array<char, proto::kHeaderSize> h{};
        proto::encodeHeader({proto::MsgType::ResumeQuery,
                             proto::kResumeKeySize}, h);
        std::array<char, proto::kResumeKeySize> key{};
        proto::encodeResumeKey(uploadId, fileSize, key);
        std::array<char, proto::kTrailerSize> tr{};
        proto::encodeTrailer(
            proto::crc32Final(proto::crc32Update(proto::kCrc32Init,
                                                 key.data(), key.size())),
            tr);
        uint64_t offset = 0;
        if (sendAll(s.get(), h.data(), h.size()) &&
            sendAll(s.get(), key.data(), key.size()) &&
            sendAll(s.get(), tr.data(), tr.size())) {
            proto::MsgType type{};
            std::string body;
            bool crcOk = false;
            if (recvResponse(s.get(), type, body, crcOk) && crcOk &&
                type == proto::MsgType::ResumeOffset && body.size() == 8)
                offset = proto::leGet64(body.data());
        }
        dropSocket(s);
        return offset;
    }

    // Asks the server for the cached analysis of (fileSize, uploadCrc):
    // 12 bytes on the wire instead of re-uploading the whole file.
    bool sendFetchRequest(SOCKET s, uint64_t fileSize, uint32_t uploadCrc) {
        std::array<char, proto::kHeaderSize> fh{};
        proto::encodeHeader({proto::MsgType::FetchResult,
                             proto::kFetchKeySize}, fh);
        std::array<char, proto::kFetchKeySize> key{};
        proto::encodeFetchKey(fileSize, uploadCrc, key);
        std::array<char, proto::kTrailerSize> ftr{};
        proto::encodeTrailer(
            proto::crc32Final(proto::crc32Update(proto::kCrc32Init,
                                                 key.data(), key.size())),
            ftr);
        return sendAll(s, fh.data(), fh.size()) &&
               sendAll(s, key.data(), key.size()) &&
               sendAll(s, ftr.data(), ftr.size());
    }

    // One upload attempt from `offset`: connects, streams the file tail,
    // sends the whole-file CRC trailer. On success `sock` holds the live
    // connection (the response arrives on it) and fullCrc the file CRC.
    bool uploadOnce(const std::string& host, int port, std::ifstream& in,
                    uint64_t fileSize, uint64_t uploadId, uint64_t offset,
                    UniqueSocket& sock, uint32_t& fullCrc, std::string& err) {
        if (!openConnection(host, port, sock, err)) return false;

        state_ = XferState::Uploading;
        if (offset > 0)
            setStatus("resuming upload from " +
                      std::to_string((offset * 100) / (fileSize ? fileSize : 1)) +
                      "% (server kept " + std::to_string(offset >> 20) +
                      " MiB)...");
        else
            setStatus("uploading " + std::to_string(fileSize >> 20) +
                      " MiB...");

        std::array<char, proto::kHeaderSize> hdr{};
        proto::encodeHeader({proto::MsgType::UploadLog,
                             proto::kUploadPrefixSize + (fileSize - offset)},
                            hdr);
        std::array<char, proto::kUploadPrefixSize> prefix{};
        proto::encodeUploadPrefix(uploadId, offset, prefix);
        bool ok = sendAll(sock.get(), hdr.data(), hdr.size()) &&
                  sendAll(sock.get(), prefix.data(), prefix.size());
        // Only move the bar once this attempt has actually started talking
        // to the server; a dead connection then never dips a held bar.
        if (ok) bytesSent_ = offset;   // the bar CONTINUES from the resume point

        // The trailer covers the WHOLE file: re-read the local prefix to
        // bring the running CRC up to `offset` before sending the tail.
        std::vector<char> buf(kChunkSize);
        uint32_t crc = proto::kCrc32Init;
        in.clear();
        in.seekg(0);
        uint64_t done = 0;
        while (ok && done < offset) {
            if (cancelFlag_) { err = "upload cancelled"; return false; }
            const uint64_t remaining = offset - done;
            const size_t want = remaining < buf.size()
                                    ? static_cast<size_t>(remaining)
                                    : buf.size();
            in.read(buf.data(), static_cast<std::streamsize>(want));
            const std::streamsize n = in.gcount();
            if (n <= 0) { err = "cannot re-read file prefix"; return false; }
            crc = proto::crc32Update(crc, buf.data(),
                                     static_cast<size_t>(n));
            done += static_cast<uint64_t>(n);
        }
        while (ok && done < fileSize) {
            // Never read or send past the size announced in the header: if
            // the log grows while we upload, the extra bytes would land
            // after the frame payload and desynchronize the stream.
            const uint64_t remaining = fileSize - done;
            const size_t want = remaining < buf.size()
                                    ? static_cast<size_t>(remaining)
                                    : buf.size();
            in.read(buf.data(), static_cast<std::streamsize>(want));
            const std::streamsize n = in.gcount();
            if (n <= 0) { ok = false; break; }
            crc = proto::crc32Update(crc, buf.data(), static_cast<size_t>(n));
            ok = sendAll(sock.get(), buf.data(), static_cast<size_t>(n));
            if (ok) {
                done += static_cast<uint64_t>(n);
                bytesSent_ = done;
            }
        }
        if (ok) {
            std::array<char, proto::kTrailerSize> tr{};
            proto::encodeTrailer(proto::crc32Final(crc), tr);
            ok = sendAll(sock.get(), tr.data(), tr.size());
        }
        if (!ok) {
            const int e = WSAGetLastError();
            err = cancelFlag_ ? "upload cancelled"
                              : wsaMsg("upload failed", e);
            return false;
        }
        fullCrc = proto::crc32Final(crc);
        return true;
    }

    void run(std::string host, int port, std::wstring filePath) {
        // ---- open the file ---------------------------------------------
        std::ifstream in(filePath, std::ios::binary | std::ios::ate);
        if (!in) { finish(XferState::Failed, "cannot open selected file"); return; }
        const uint64_t fileSize = static_cast<uint64_t>(in.tellg());
        in.seekg(0);
        bytesTotal_ = fileSize;
        const uint64_t uploadId = computeUploadId(in, fileSize);

        // ---- upload, retrying with RESUME on connection loss ------------
        // Every attempt first asks the server how much it already spooled
        // (also picks up leftovers from a previous run or a cancel), then
        // sends only the missing tail. The progress bar continues from the
        // resume point instead of restarting at zero.
        UniqueSocket sock;
        uint32_t uploadCrc = 0;
        for (int attempt = 0;; ++attempt) {
            if (attempt > 0) {
                DWORD delay = kFetchBaseDelayMs << (attempt - 1);
                if (delay > kFetchMaxDelayMs) delay = kFetchMaxDelayMs;
                setStatus("connection lost - retrying upload with resume "
                          "(attempt " + std::to_string(attempt + 1) + "/" +
                          std::to_string(kUploadRetries) + ")");
                for (DWORD waited = 0; waited < delay && !cancelFlag_;
                     waited += 100)
                    Sleep(100);
            }
            if (cancelFlag_) {
                finish(XferState::Cancelled, "upload cancelled");
                return;
            }
            const uint64_t offset =
                queryResumeOffset(host, port, uploadId, fileSize);
            if (cancelFlag_) {
                finish(XferState::Cancelled, "upload cancelled");
                return;
            }
            std::string err;
            if (uploadOnce(host, port, in, fileSize, uploadId, offset,
                           sock, uploadCrc, err))
                break;
            dropSocket(sock);
            if (cancelFlag_) {
                finish(XferState::Cancelled, "upload cancelled");
                return;
            }
            setStatus(err);
            if (attempt + 1 >= kUploadRetries) {
                finish(XferState::Failed,
                       err + " - giving up after " +
                       std::to_string(kUploadRetries) + " attempts");
                return;
            }
        }



        // ---- response frame --------------------------------------------
        state_ = XferState::WaitingResult;
        setStatus("upload delivered, waiting for server verification "
                  "and analysis...");

        proto::MsgType type{};
        std::string body;
        bool crcOk = false;
        bool ok = recvResponse(sock.get(), type, body, crcOk);
        int recvErr = ok ? 0 : WSAGetLastError();
        dropSocket(sock);

        // The upload itself completed, so the server has (or is finishing)
        // the analysis. If only the *response* was lost or damaged, retry
        // with a 12-byte FetchResult instead of re-uploading 500 MB.
        int attempt = 0;
        while ((!ok || !crcOk) && !cancelFlag_ && attempt < kFetchRetries) {
            DWORD delay = kFetchBaseDelayMs << attempt;   // 0.5s, 1, 2, 4, 8
            if (delay > kFetchMaxDelayMs) delay = kFetchMaxDelayMs;
            ++attempt;
            setStatus("result lost in transit - re-fetching from server "
                      "cache (attempt " + std::to_string(attempt) + "/" +
                      std::to_string(kFetchRetries) + ")");
            // Sleep in short slices so Cancel stays responsive during the
            // multi-second backoff.
            for (DWORD waited = 0; waited < delay && !cancelFlag_;
                 waited += 100)
                Sleep(100);
            if (cancelFlag_) break;
            UniqueSocket s2;
            std::string e2;
            if (!openConnection(host, port, s2, e2)) continue;
            if (sendFetchRequest(s2.get(), fileSize, uploadCrc)) {
                ok = recvResponse(s2.get(), type, body, crcOk);
                recvErr = ok ? 0 : WSAGetLastError();
            }
            dropSocket(s2);
        }

        if (ok && !crcOk) {
            finish(XferState::Failed,
                   "result rejected: checksum mismatch (corrupted in transit)");
            return;
        }

        if (!ok) {
            finish(cancelFlag_ ? XferState::Cancelled : XferState::Failed,
                   cancelFlag_ ? "cancelled while waiting for result"
                               : wsaMsg("failed to receive result", recvErr));
            return;
        }
        if (type == proto::MsgType::Error) {
            finish(XferState::Failed,
                   (attempt > 0 ? "re-fetch failed: " : "server rejected upload: ")
                       + body);
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

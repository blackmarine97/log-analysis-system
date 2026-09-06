// test_client: minimal blocking-socket CLI used to exercise log_server
// end-to-end before (and alongside) the Windows GUI client.
//
//   ./test_client <host> <port> <logfile> [out.csv]
//   ./test_client <host> <port> <logfile> --abort <bytes>
//   ./test_client <host> <port> <logfile> --corrupt <bytes>
//   ./test_client <host> <port> <logfile> --drop-result
//   ./test_client <host> <port> <logfile> --fetch [out.csv]
//
// --abort N hard-closes the socket after sending ~N payload bytes, which is
// the "network yanked mid-transfer" robustness scenario from the assignment:
// the server must log the error and clean up, never crash.
// --corrupt N flips one payload byte at offset ~N while still sending the
// CRC of the ORIGINAL file: simulates corruption in transit, which the
// server must detect and answer with an Error frame, never a result.
// --drop-result uploads everything, then closes without reading the
// response — "connection lost while the result was in flight". --fetch then
// retrieves that result with a 12-byte FetchResult frame (the key is the
// file's size + CRC, recomputed locally) instead of a 500 MB re-upload.
//
// Uploads are resumable (protocol v3): before uploading, the client asks
// ResumeQuery("how much of upload <id> do you have?") and continues from
// the server's answer instead of resending those bytes. The upload id is
// derived deterministically from the file (CRC of the first MiB + size),
// so a rerun after --abort automatically picks up where it stopped.

#include "common/Protocol.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

// RAII wrapper so every exit path returns the fd to the system.
class UniqueFd {
public:
    explicit UniqueFd(int fd = -1) : fd_(fd) {}
    ~UniqueFd() { reset(); }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    void reset(int fd = -1) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = fd;
    }
    int get() const { return fd_; }
    explicit operator bool() const { return fd_ >= 0; }

private:
    int fd_;
};

bool sendAll(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        const ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::cerr << "send failed: " << std::strerror(errno) << '\n';
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool recvAll(int fd, char* data, size_t len) {
    size_t got = 0;
    while (got < len) {
        const ssize_t n = ::recv(fd, data + got, len - got, 0);
        if (n == 0) { std::cerr << "server closed early\n"; return false; }
        if (n < 0) {
            if (errno == EINTR) continue;
            std::cerr << "recv failed: " << std::strerror(errno) << '\n';
            return false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

// Deterministic upload id: CRC of the first MiB + the low 32 bits of the
// size. Needs no client-side state across runs; a collision (different file,
// same id) is caught by the full-file CRC trailer and merely costs a restart.
uint64_t computeUploadId(std::ifstream& in, uint64_t fileSize) {
    std::vector<char> buf(1 << 20);
    in.seekg(0);
    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    const auto n = in.gcount();
    in.clear();
    in.seekg(0);
    const uint32_t c = proto::crc32Final(proto::crc32Update(
        proto::kCrc32Init, buf.data(), static_cast<size_t>(n > 0 ? n : 0)));
    return static_cast<uint64_t>(c) |
           (static_cast<uint64_t>(static_cast<uint32_t>(fileSize)) << 32);
}

int connectTo(const std::string& host, const std::string& port, UniqueFd& fd) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* raw = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &raw) != 0 || !raw)
        return -1;
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> res(raw, &freeaddrinfo);
    fd.reset(::socket(res->ai_family, res->ai_socktype, res->ai_protocol));
    if (!fd) return -1;
    return ::connect(fd.get(), res->ai_addr, res->ai_addrlen);
}

// Sends a fixed-size-payload request frame (key + trailer). Returns false on
// any socket failure.
bool sendSmallFrame(int fd, proto::MsgType type,
                    const char* payload, size_t n) {
    std::array<char, proto::kHeaderSize> h{};
    proto::encodeHeader({type, n}, h);
    std::array<char, proto::kTrailerSize> tr{};
    proto::encodeTrailer(
        proto::crc32Final(proto::crc32Update(proto::kCrc32Init, payload, n)),
        tr);
    return sendAll(fd, h.data(), h.size()) && sendAll(fd, payload, n) &&
           sendAll(fd, tr.data(), tr.size());
}

// Asks the server how much of this upload is already spooled. 0 on any
// failure — the caller just uploads from the start.
uint64_t queryResumeOffset(const std::string& host, const std::string& port,
                           uint64_t uploadId, uint64_t fileSize) {
    UniqueFd fd;
    if (connectTo(host, port, fd) != 0) return 0;
    std::array<char, proto::kResumeKeySize> key{};
    proto::encodeResumeKey(uploadId, fileSize, key);
    if (!sendSmallFrame(fd.get(), proto::MsgType::ResumeQuery,
                        key.data(), key.size()))
        return 0;
    std::array<char, proto::kHeaderSize> rh{};
    if (!recvAll(fd.get(), rh.data(), rh.size())) return 0;
    const auto h = proto::decodeHeader(std::string_view(rh.data(), rh.size()));
    if (!h || h->type != proto::MsgType::ResumeOffset || h->payloadSize != 8)
        return 0;
    std::array<char, 8> body{};
    std::array<char, proto::kTrailerSize> tr{};
    if (!recvAll(fd.get(), body.data(), body.size()) ||
        !recvAll(fd.get(), tr.data(), tr.size()))
        return 0;
    if (proto::decodeTrailer(tr) !=
        proto::crc32Final(proto::crc32Update(proto::kCrc32Init,
                                             body.data(), body.size())))
        return 0;
    return proto::leGet64(body.data());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: test_client <host> <port> <logfile>"
                     " [out.csv | --abort <bytes>]\n";
        return 1;
    }
    const std::string host = argv[1];
    const std::string port = argv[2];
    const std::string path = argv[3];
    std::string outPath = "result.csv";
    long long abortAfter = -1, corruptAt = -1;
    bool dropResult = false, fetchMode = false;
    if (argc >= 6 && std::string(argv[4]) == "--abort")
        abortAfter = std::atoll(argv[5]);
    else if (argc >= 6 && std::string(argv[4]) == "--corrupt")
        corruptAt = std::atoll(argv[5]);
    else if (argc >= 5 && std::string(argv[4]) == "--drop-result")
        dropResult = true;
    else if (argc >= 5 && std::string(argv[4]) == "--fetch") {
        fetchMode = true;
        if (argc >= 6) outPath = argv[5];
    } else if (argc >= 5)
        outPath = argv[4];

    std::signal(SIGPIPE, SIG_IGN);

    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) { std::cerr << "cannot open " << path << '\n'; return 1; }
    const uint64_t fileSize = static_cast<uint64_t>(in.tellg());
    in.seekg(0);

    UniqueFd fd;
    if (connectTo(host, port, fd) != 0) {
        std::cerr << "connect failed: " << std::strerror(errno) << '\n';
        return 1;
    }

    if (fetchMode) {
        // ---- FetchResult: re-request a cached analysis by (size, crc) ----
        std::vector<char> fbuf(1 << 20);
        uint32_t fcrc = proto::kCrc32Init;
        while (in) {
            in.read(fbuf.data(), static_cast<std::streamsize>(fbuf.size()));
            if (in.gcount() > 0)
                fcrc = proto::crc32Update(fcrc, fbuf.data(),
                                          static_cast<size_t>(in.gcount()));
        }
        std::array<char, proto::kFetchKeySize> key{};
        proto::encodeFetchKey(fileSize, proto::crc32Final(fcrc), key);
        std::array<char, proto::kHeaderSize> fh{};
        proto::encodeHeader({proto::MsgType::FetchResult, key.size()}, fh);
        std::array<char, proto::kTrailerSize> ftr{};
        proto::encodeTrailer(
            proto::crc32Final(proto::crc32Update(proto::kCrc32Init,
                                                 key.data(), key.size())),
            ftr);
        if (!sendAll(fd.get(), fh.data(), fh.size()) ||
            !sendAll(fd.get(), key.data(), key.size()) ||
            !sendAll(fd.get(), ftr.data(), ftr.size())) return 1;
        std::cout << "FETCH: requested cached result (12-byte key, "
                     "no re-upload)\n";
    } else {

    // ---- upload: resume-aware [header][prefix][file bytes] --------------
    const uint64_t uploadId = computeUploadId(in, fileSize);
    uint64_t offset = queryResumeOffset(host, port, uploadId, fileSize);
    if (offset > 0)
        std::cout << "RESUME: server already has " << offset
                  << " bytes, continuing from there\n";

    std::array<char, proto::kHeaderSize> hdr{};
    proto::encodeHeader({proto::MsgType::UploadLog,
                         proto::kUploadPrefixSize + (fileSize - offset)}, hdr);
    if (!sendAll(fd.get(), hdr.data(), hdr.size())) return 1;
    std::array<char, proto::kUploadPrefixSize> prefix{};
    proto::encodeUploadPrefix(uploadId, offset, prefix);
    if (!sendAll(fd.get(), prefix.data(), prefix.size())) return 1;

    // The trailer CRC covers the WHOLE file: on resume, re-read the local
    // prefix to bring the running CRC up to `offset` before sending.
    std::vector<char> buf(1 << 20);
    uint32_t crc = proto::kCrc32Init;
    uint64_t sent = 0;
    while (sent < offset) {
        const uint64_t remaining = offset - sent;
        const size_t want = remaining < buf.size()
                                ? static_cast<size_t>(remaining) : buf.size();
        in.read(buf.data(), static_cast<std::streamsize>(want));
        const std::streamsize n = in.gcount();
        if (n <= 0) { std::cerr << "file read error\n"; return 1; }
        crc = proto::crc32Update(crc, buf.data(), static_cast<size_t>(n));
        sent += static_cast<uint64_t>(n);
    }
    while (sent < fileSize) {
        // Clamp to the size announced in the header: a log that grows during
        // the upload must not push bytes past the end of the frame payload.
        const uint64_t remaining = fileSize - sent;
        const size_t want = remaining < buf.size()
                                ? static_cast<size_t>(remaining)
                                : buf.size();
        in.read(buf.data(), static_cast<std::streamsize>(want));
        const std::streamsize n = in.gcount();
        if (n <= 0) { std::cerr << "file read error\n"; return 1; }
        // CRC over the ORIGINAL bytes, then (optionally) corrupt one byte:
        // the server must notice that what arrived is not what was hashed.
        crc = proto::crc32Update(crc, buf.data(), static_cast<size_t>(n));
        if (corruptAt >= 0 &&
            static_cast<uint64_t>(corruptAt) >= sent &&
            static_cast<uint64_t>(corruptAt) < sent + static_cast<uint64_t>(n)) {
            buf[static_cast<size_t>(corruptAt - static_cast<long long>(sent))] ^= 0x01;
            std::cout << "CORRUPT: flipped payload byte at offset "
                      << corruptAt << "\n";
        }
        if (!sendAll(fd.get(), buf.data(), static_cast<size_t>(n))) return 1;
        sent += static_cast<uint64_t>(n);

        if (abortAfter >= 0 && sent >= static_cast<uint64_t>(abortAfter)) {
            std::cout << "ABORT: hard-closing socket after " << sent
                      << " bytes (robustness test)\n";
            return 0;   // UniqueFd closes the socket on scope exit
        }
        std::cout << "\rupload " << (sent * 100 / fileSize) << "% ("
                  << (sent >> 20) << " MiB)" << std::flush;
    }
    std::array<char, proto::kTrailerSize> tr{};
    proto::encodeTrailer(proto::crc32Final(crc), tr);
    if (!sendAll(fd.get(), tr.data(), tr.size())) return 1;
    if (dropResult) {
        std::cout << "\nDROP-RESULT: upload complete, closing without "
                     "reading the response (simulates a download-side "
                     "disconnect)\n";
        return 0;   // UniqueFd closes the socket
    }
    std::cout << "\nupload done, waiting for result...\n";
    }   // !fetchMode

    // ---- response frame -------------------------------------------------
    std::array<char, proto::kHeaderSize> rhdr{};
    if (!recvAll(fd.get(), rhdr.data(), rhdr.size())) return 1;
    const auto h = proto::decodeHeader(
        std::string_view(rhdr.data(), rhdr.size()));
    if (!h) { std::cerr << "bad response header\n"; return 1; }

    std::string body(h->payloadSize, '\0');
    if (h->payloadSize > 0 &&
        !recvAll(fd.get(), body.data(), body.size())) return 1;
    std::array<char, proto::kTrailerSize> rtr{};
    if (!recvAll(fd.get(), rtr.data(), rtr.size())) return 1;
    if (proto::decodeTrailer(rtr) !=
        proto::crc32Final(proto::crc32Update(proto::kCrc32Init,
                                             body.data(), body.size()))) {
        std::cerr << "response checksum mismatch\n";
        return 1;
    }

    if (h->type == proto::MsgType::Error) {
        std::cerr << "server error: " << body << '\n';
        return 1;
    }
    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    out << body;
    std::cout << "saved " << body.size() << " bytes -> " << outPath << '\n';
    return 0;
}

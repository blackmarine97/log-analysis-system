// test_client: minimal blocking-socket CLI used to exercise log_server
// end-to-end before (and alongside) the Windows GUI client.
//
//   ./test_client <host> <port> <logfile> [out.csv]
//   ./test_client <host> <port> <logfile> --abort <bytes>
//
// --abort N hard-closes the socket after sending ~N payload bytes, which is
// the "network yanked mid-transfer" robustness scenario from the assignment:
// the server must log the error and clean up, never crash.

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
    long long abortAfter = -1;
    if (argc >= 6 && std::string(argv[4]) == "--abort")
        abortAfter = std::atoll(argv[5]);
    else if (argc >= 5)
        outPath = argv[4];

    std::signal(SIGPIPE, SIG_IGN);

    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) { std::cerr << "cannot open " << path << '\n'; return 1; }
    const uint64_t fileSize = static_cast<uint64_t>(in.tellg());
    in.seekg(0);

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* raw = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &raw) != 0 || !raw) {
        std::cerr << "cannot resolve " << host << '\n';
        return 1;
    }
    // Owned by unique_ptr so freeaddrinfo() runs on every exit path.
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> res(raw, &freeaddrinfo);
    UniqueFd fd(::socket(res->ai_family, res->ai_socktype, res->ai_protocol));
    const int rc = fd ? ::connect(fd.get(), res->ai_addr, res->ai_addrlen) : -1;
    res.reset();
    if (!fd || rc != 0) {
        std::cerr << "connect failed: " << std::strerror(errno) << '\n';
        return 1;
    }

    // ---- upload: [header][file bytes in 1 MiB chunks] -------------------
    std::array<char, proto::kHeaderSize> hdr{};
    proto::encodeHeader({proto::MsgType::UploadLog, fileSize}, hdr);
    if (!sendAll(fd.get(), hdr.data(), hdr.size())) return 1;

    std::vector<char> buf(1 << 20);
    uint64_t sent = 0;
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
    std::cout << "\nupload done, waiting for result...\n";

    // ---- response frame -------------------------------------------------
    std::array<char, proto::kHeaderSize> rhdr{};
    if (!recvAll(fd.get(), rhdr.data(), rhdr.size())) return 1;
    const auto h = proto::decodeHeader(
        std::string_view(rhdr.data(), rhdr.size()));
    if (!h) { std::cerr << "bad response header\n"; return 1; }

    std::string body(h->payloadSize, '\0');
    if (h->payloadSize > 0 &&
        !recvAll(fd.get(), body.data(), body.size())) return 1;

    if (h->type == proto::MsgType::Error) {
        std::cerr << "server error: " << body << '\n';
        return 1;
    }
    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    out << body;
    std::cout << "saved " << body.size() << " bytes -> " << outPath << '\n';
    return 0;
}

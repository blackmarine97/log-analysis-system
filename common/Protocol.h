#pragma once
// Wire protocol shared by the Windows client and the Linux server.
//
// Every message is one frame (protocol version 2):
//
//   +-------------+-----+------+----------+---------------------+
//   | magic "BLOG"| ver | type | reserved | payloadSize (8, LE) |  = 16-byte header
//   +-------------+-----+------+----------+---------------------+
//   | payload ... payloadSize bytes                             |
//   +-----------------------------------------------------------+
//   | CRC-32 of the payload (4, LE)                             |  = 4-byte trailer
//   +-----------------------------------------------------------+
//
// The trailer lets the receiver detect payload corruption in transit (bit
// flips TCP checksums missed, proxy truncation, a buggy middlebox). Both
// sides compute the CRC incrementally while streaming, so verification
// costs no extra pass and no buffering. A mismatch is answered with an
// Error frame (server side) or surfaces as a failed transfer (client side);
// corrupted data never silently becomes an "analysis result".
//
// Flow: client connects, sends one UploadLog frame (payload = raw log file),
// server streams the payload through the parser and answers with a single
// ResultCsv frame (payload = result.csv bytes) or an Error frame, then the
// connection is closed. Integers are little-endian on the wire, encoded and
// decoded byte-by-byte so both sides are endianness-independent.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

namespace proto {

inline constexpr std::array<char, 4> kMagic = {'B', 'L', 'O', 'G'};
inline constexpr uint8_t kVersion = 3;      // v3: resumable uploads (v2: CRC trailer)
inline constexpr size_t kHeaderSize = 16;
inline constexpr size_t kTrailerSize = 4;   // CRC-32, little-endian

// Upper bound accepted by the server for any payload (defense against a
// corrupt/hostile header announcing an absurd size). 2 GiB covers the
// 500 MB assignment file with ample margin.
inline constexpr uint64_t kMaxPayloadSize = 2ull * 1024 * 1024 * 1024;

enum class MsgType : uint8_t {
    UploadLog   = 1,  // client -> server, payload = log file bytes
    ResultCsv   = 2,  // server -> client, payload = result.csv content
    Error       = 3,  // server -> client, payload = UTF-8 error message
    FetchResult = 4,  // client -> server, payload = kFetchKeySize bytes:
                      //   fileSize (8B LE) + payload CRC-32 (4B LE).
                      // Re-requests the result of an upload the server has
                      // already analysed, so a connection lost while the
                      // *response* was in flight costs a 12-byte retry, not
                      // a 500 MB re-upload. The upload's own CRC doubles as
                      // the lookup key: the client already has it, and it
                      // identifies the exact bytes that were analysed.
    ResumeQuery = 5,  // client -> server, payload = kResumeKeySize bytes:
                      //   uploadId (8B LE) + fileSize (8B LE).
                      // "How much of this upload do you already have?" The
                      // server answers with ResumeOffset; the client then
                      // continues the upload from that byte instead of
                      // resending everything before it.
    ResumeOffset = 6, // server -> client, payload = 8 bytes LE: the number
                      // of payload bytes safely spooled for that uploadId
                      // (0 = nothing, start from scratch).
};

inline constexpr size_t kFetchKeySize = 12;

// UploadLog (v3) payload layout: a 16-byte prefix, then the file bytes
// starting at `startOffset`:
//   uploadId (8B LE) + startOffset (8B LE) + file[startOffset..fileSize)
// The CRC-32 trailer always covers the WHOLE file (offset 0..fileSize), so
// a resumed upload whose spooled prefix does not match the file being sent
// -- a different file with a colliding id, a truncated spool -- is caught
// exactly like corruption in transit.
inline constexpr size_t kUploadPrefixSize = 16;
inline constexpr size_t kResumeKeySize = 16;

struct FrameHeader {
    MsgType  type;
    uint64_t payloadSize;
};

inline void encodeHeader(const FrameHeader& h,
                         std::array<char, kHeaderSize>& out) noexcept {
    std::memcpy(out.data(), kMagic.data(), kMagic.size());
    out[4] = static_cast<char>(kVersion);
    out[5] = static_cast<char>(h.type);
    out[6] = 0;
    out[7] = 0;
    for (int i = 0; i < 8; ++i)
        out[8 + i] = static_cast<char>((h.payloadSize >> (8 * i)) & 0xFF);
}

// Returns nullopt on any structural violation (bad magic/version/type or an
// implausible payload size). `raw` must hold at least kHeaderSize bytes.
inline std::optional<FrameHeader> decodeHeader(std::string_view raw) noexcept {
    if (raw.size() < kHeaderSize) return std::nullopt;
    if (std::memcmp(raw.data(), kMagic.data(), kMagic.size()) != 0)
        return std::nullopt;
    if (static_cast<uint8_t>(raw[4]) != kVersion) return std::nullopt;

    const uint8_t t = static_cast<uint8_t>(raw[5]);
    if (t < static_cast<uint8_t>(MsgType::UploadLog) ||
        t > static_cast<uint8_t>(MsgType::ResumeOffset))
        return std::nullopt;

    uint64_t size = 0;
    for (int i = 0; i < 8; ++i)
        size |= static_cast<uint64_t>(static_cast<uint8_t>(raw[8 + i]))
                << (8 * i);
    if (size > kMaxPayloadSize) return std::nullopt;

    return FrameHeader{static_cast<MsgType>(t), size};
}

// ---- CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) ----------------------
// Incremental: start from kCrc32Init, feed chunks in any split, finish with
// crc32Final(). Slice-by-8: processes 8 bytes per iteration through 8 derived
// tables (8 KiB, built once on first use, thread-safe since C++11). The
// byte-at-a-time version measured ~400 MB/s, which doubled the 500 MB upload
// time; slice-by-8 keeps checksumming far below the network in the profile.
inline constexpr uint32_t kCrc32Init = 0xFFFFFFFFu;

inline uint32_t crc32Update(uint32_t crc, const char* data, size_t len) noexcept {
    static const std::array<std::array<uint32_t, 256>, 8> tables = [] {
        std::array<std::array<uint32_t, 256>, 8> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; ++i)
            for (int j = 1; j < 8; ++j)
                t[j][i] = (t[j - 1][i] >> 8) ^ t[0][t[j - 1][i] & 0xFFu];
        return t;
    }();

    const auto* p = reinterpret_cast<const uint8_t*>(data);
    while (len >= 8) {
        // Little-endian-independent: bytes are combined explicitly.
        const uint32_t lo = crc ^ (static_cast<uint32_t>(p[0]) |
                                   static_cast<uint32_t>(p[1]) << 8 |
                                   static_cast<uint32_t>(p[2]) << 16 |
                                   static_cast<uint32_t>(p[3]) << 24);
        crc = tables[7][lo & 0xFFu] ^ tables[6][(lo >> 8) & 0xFFu] ^
              tables[5][(lo >> 16) & 0xFFu] ^ tables[4][lo >> 24] ^
              tables[3][p[4]] ^ tables[2][p[5]] ^
              tables[1][p[6]] ^ tables[0][p[7]];
        p += 8;
        len -= 8;
    }
    while (len--)
        crc = tables[0][(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    return crc;
}

inline uint32_t crc32Final(uint32_t crc) noexcept { return crc ^ 0xFFFFFFFFu; }

inline void encodeTrailer(uint32_t crc, std::array<char, kTrailerSize>& out) noexcept {
    for (int i = 0; i < 4; ++i)
        out[i] = static_cast<char>((crc >> (8 * i)) & 0xFF);
}

inline uint32_t decodeTrailer(const std::array<char, kTrailerSize>& raw) noexcept {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= static_cast<uint32_t>(static_cast<uint8_t>(raw[i])) << (8 * i);
    return v;
}

// Little-endian u64 helpers shared by the fixed-size payloads below.
inline void lePut64(char* out, uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i)
        out[i] = static_cast<char>((v >> (8 * i)) & 0xFF);
}
inline uint64_t leGet64(const char* in) noexcept {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(static_cast<uint8_t>(in[i])) << (8 * i);
    return v;
}

inline void encodeUploadPrefix(uint64_t uploadId, uint64_t startOffset,
                               std::array<char, kUploadPrefixSize>& out) noexcept {
    lePut64(out.data(), uploadId);
    lePut64(out.data() + 8, startOffset);
}
inline void decodeUploadPrefix(const std::array<char, kUploadPrefixSize>& raw,
                               uint64_t& uploadId, uint64_t& startOffset) noexcept {
    uploadId    = leGet64(raw.data());
    startOffset = leGet64(raw.data() + 8);
}

inline void encodeResumeKey(uint64_t uploadId, uint64_t fileSize,
                            std::array<char, kResumeKeySize>& out) noexcept {
    lePut64(out.data(), uploadId);
    lePut64(out.data() + 8, fileSize);
}
inline void decodeResumeKey(const std::array<char, kResumeKeySize>& raw,
                            uint64_t& uploadId, uint64_t& fileSize) noexcept {
    uploadId = leGet64(raw.data());
    fileSize = leGet64(raw.data() + 8);
}

inline void encodeFetchKey(uint64_t fileSize, uint32_t crc,
                           std::array<char, kFetchKeySize>& out) noexcept {
    for (int i = 0; i < 8; ++i)
        out[i] = static_cast<char>((fileSize >> (8 * i)) & 0xFF);
    for (int i = 0; i < 4; ++i)
        out[8 + i] = static_cast<char>((crc >> (8 * i)) & 0xFF);
}

inline void decodeFetchKey(const std::array<char, kFetchKeySize>& raw,
                           uint64_t& fileSize, uint32_t& crc) noexcept {
    fileSize = 0;
    crc = 0;
    for (int i = 0; i < 8; ++i)
        fileSize |= static_cast<uint64_t>(static_cast<uint8_t>(raw[i]))
                    << (8 * i);
    for (int i = 0; i < 4; ++i)
        crc |= static_cast<uint32_t>(static_cast<uint8_t>(raw[8 + i]))
               << (8 * i);
}

}  // namespace proto

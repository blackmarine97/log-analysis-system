#pragma once
// Wire protocol shared by the Windows client and the Linux server.
//
// Every message is one frame:
//
//   +-------------+-----+------+----------+---------------------+
//   | magic "BLOG"| ver | type | reserved | payloadSize (8, LE) |  = 16-byte header
//   +-------------+-----+------+----------+---------------------+
//   | payload ... payloadSize bytes                             |
//   +-----------------------------------------------------------+
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
inline constexpr uint8_t kVersion = 1;
inline constexpr size_t kHeaderSize = 16;

// Upper bound accepted by the server for any payload (defense against a
// corrupt/hostile header announcing an absurd size). 2 GiB covers the
// 500 MB assignment file with ample margin.
inline constexpr uint64_t kMaxPayloadSize = 2ull * 1024 * 1024 * 1024;

enum class MsgType : uint8_t {
    UploadLog = 1,   // client -> server, payload = log file bytes
    ResultCsv = 2,   // server -> client, payload = result.csv content
    Error     = 3,   // server -> client, payload = UTF-8 error message
};

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
        t > static_cast<uint8_t>(MsgType::Error))
        return std::nullopt;

    uint64_t size = 0;
    for (int i = 0; i < 8; ++i)
        size |= static_cast<uint64_t>(static_cast<uint8_t>(raw[8 + i]))
                << (8 * i);
    if (size > kMaxPayloadSize) return std::nullopt;

    return FrameHeader{static_cast<MsgType>(t), size};
}

}  // namespace proto

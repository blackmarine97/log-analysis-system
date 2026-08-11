#pragma once
// Session: one accepted TCP connection, driven entirely by libuv callbacks.
//
// State machine:
//
//   ReadHeader --(16 bytes, valid UploadLog)--> ReadPayload
//   ReadPayload --(payloadSize bytes streamed into StreamParser)--> Responding
//   Responding --(ResultCsv frame fully written)--> close
//   any state --(read error / EOF / bad frame)--> close
//
// The payload is NEVER buffered: every received chunk is handed straight to
// StreamParser::feed(), so memory stays O(64 KiB read buffer + one log line
// + stats map) regardless of file size.
//
// Lifetime: TcpServer owns each Session via std::unique_ptr and destroys it
// only after libuv confirms the handle is closed (onClosed callback). All
// libuv callback trampolines recover `this` from handle->data; no manual
// new/delete anywhere.

#include "../common/Protocol.h"
#include "Logger.h"
#include "StreamParser.h"

#include <uv.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

class Session {
public:
    using ClosedCallback = std::function<void(Session*)>;

    Session(uv_loop_t* loop, uint64_t id, Logger& log,
            std::string csvPath, ClosedCallback onClosed)
        : id_(id),
          log_(log),
          csvPath_(std::move(csvPath)),
          onClosed_(std::move(onClosed)),
          readBuf_(kReadBufSize),
          parser_([this](uint64_t lineNo, std::string_view line) {
              std::ostringstream os;
              os << "session " << id_ << ": skipped malformed line "
                 << lineNo << ": " << line.substr(0, 120);
              log_.warn(os.str());
          }) {
        uv_tcp_init(loop, &tcp_);
        tcp_.data = this;
    }

    // Non-copyable, non-movable: libuv holds a stable pointer to tcp_.
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Accept the pending connection and start reading. On failure the caller
    // must still close() the session to release the handle.
    bool accept(uv_stream_t* listener) {
        if (uv_accept(listener, streamHandle()) != 0) return false;
        return uv_read_start(streamHandle(), &Session::onAlloc,
                             &Session::onRead) == 0;
    }

    void close() {
        if (closing_) return;
        closing_ = true;
        uv_close(reinterpret_cast<uv_handle_t*>(&tcp_), &Session::onClosed);
    }

    uint64_t id() const { return id_; }

private:
    enum class State { ReadHeader, ReadPayload, Responding };

    static constexpr size_t   kReadBufSize      = 64 * 1024;
    static constexpr uint64_t kProgressLogEvery = 100ull * 1024 * 1024;

    uv_stream_t* streamHandle() {
        return reinterpret_cast<uv_stream_t*>(&tcp_);
    }

    // ---- libuv trampolines ---------------------------------------------
    static void onAlloc(uv_handle_t* h, size_t /*suggested*/, uv_buf_t* buf) {
        auto* self = static_cast<Session*>(h->data);
        buf->base = self->readBuf_.data();
        buf->len  = self->readBuf_.size();
    }

    static void onRead(uv_stream_t* s, ssize_t nread, const uv_buf_t* buf) {
        auto* self = static_cast<Session*>(s->data);
        if (nread > 0) {
            self->consume(std::string_view(buf->base,
                                           static_cast<size_t>(nread)));
        } else if (nread < 0) {
            // EOF or socket error (ECONNRESET on a yanked cable, etc.).
            // Never fatal: log, then release everything via close().
            std::ostringstream os;
            os << "session " << self->id_ << ": connection ended ("
               << uv_strerror(static_cast<int>(nread)) << ") after "
               << self->payloadReceived_ << '/' << self->payloadExpected_
               << " payload bytes";
            if (self->state_ == State::ReadPayload &&
                self->payloadReceived_ < self->payloadExpected_)
                self->log_.error(os.str());   // upload aborted mid-transfer
            else
                self->log_.info(os.str());
            self->close();
        }
    }

    static void onWrite(uv_write_t* req, int status) {
        auto* self = static_cast<Session*>(req->data);
        if (status != 0) {
            std::ostringstream os;
            os << "session " << self->id_ << ": response write failed ("
               << uv_strerror(status) << ')';
            self->log_.error(os.str());
        } else {
            std::ostringstream os;
            os << "session " << self->id_ << ": result sent ("
               << self->response_.size() << " bytes), closing";
            self->log_.info(os.str());
        }
        self->close();
    }

    static void onClosed(uv_handle_t* h) {
        auto* self = static_cast<Session*>(h->data);
        // Last line of the session's life: hand ourselves back to the owner.
        if (self->onClosed_) self->onClosed_(self);
    }

    // ---- protocol state machine ----------------------------------------
    void consume(std::string_view chunk) {
        while (!chunk.empty()) {
            switch (state_) {
            case State::ReadHeader: {
                const size_t want = proto::kHeaderSize - headerFill_;
                const size_t take = chunk.size() < want ? chunk.size() : want;
                std::memcpy(header_.data() + headerFill_, chunk.data(), take);
                headerFill_ += take;
                chunk.remove_prefix(take);
                if (headerFill_ < proto::kHeaderSize) return;

                auto hdr = proto::decodeHeader(
                    std::string_view(header_.data(), header_.size()));
                if (!hdr || hdr->type != proto::MsgType::UploadLog) {
                    log_.error("session " + std::to_string(id_) +
                               ": invalid frame header, dropping connection");
                    sendError("invalid frame header");
                    return;
                }
                payloadExpected_ = hdr->payloadSize;
                payloadReceived_ = 0;
                log_.info("session " + std::to_string(id_) +
                          ": upload started, expecting " +
                          std::to_string(payloadExpected_) + " bytes");
                state_ = State::ReadPayload;
                if (payloadExpected_ == 0) { finalize(); return; }
                break;
            }
            case State::ReadPayload: {
                const uint64_t want = payloadExpected_ - payloadReceived_;
                const size_t   take = chunk.size() < want
                                        ? chunk.size()
                                        : static_cast<size_t>(want);
                parser_.feed(chunk.substr(0, take));
                payloadReceived_ += take;
                chunk.remove_prefix(take);

                if (payloadReceived_ / kProgressLogEvery != progressMark_) {
                    progressMark_ = payloadReceived_ / kProgressLogEvery;
                    log_.info("session " + std::to_string(id_) + ": received " +
                              std::to_string(payloadReceived_ >> 20) + " MiB");
                }
                if (payloadReceived_ == payloadExpected_) { finalize(); return; }
                break;
            }
            case State::Responding:
                // Trailing bytes after a complete payload: protocol violation
                // by the peer; ignore them, the response is already in flight.
                return;
            }
        }
    }

    void finalize() {
        state_ = State::Responding;
        uv_read_stop(streamHandle());
        parser_.finish();

        std::ostringstream csv;
        parser_.writeCsv(csv);
        const std::string body = csv.str();

        // Keep a server-side copy as evidence/debug artifact.
        if (!csvPath_.empty()) {
            std::ofstream out(csvPath_, std::ios::trunc);
            if (out) out << body;
            else log_.warn("session " + std::to_string(id_) +
                           ": cannot write " + csvPath_);
        }

        const Stats& st = parser_.stats();
        log_.info("session " + std::to_string(id_) + ": parse complete, lines=" +
                  std::to_string(st.totalLines) + " skipped=" +
                  std::to_string(st.skippedLines) + " speedCount=" +
                  std::to_string(st.speedCount));

        sendFrame(proto::MsgType::ResultCsv, body);
    }

    void sendError(std::string_view message) {
        state_ = State::Responding;
        uv_read_stop(streamHandle());
        sendFrame(proto::MsgType::Error, std::string(message));
    }

    // Builds the frame in response_ (kept alive until onWrite fires) and
    // queues it. libuv copies nothing, so the buffer must outlive the write.
    void sendFrame(proto::MsgType type, std::string body) {
        std::array<char, proto::kHeaderSize> hdr{};
        proto::encodeHeader({type, body.size()}, hdr);
        response_.assign(hdr.data(), hdr.size());
        response_ += body;

        writeReq_.data = this;
        uv_buf_t buf = uv_buf_init(response_.data(),
                                   static_cast<unsigned>(response_.size()));
        if (uv_write(&writeReq_, streamHandle(), &buf, 1,
                     &Session::onWrite) != 0) {
            log_.error("session " + std::to_string(id_) +
                       ": uv_write failed to queue");
            close();
        }
    }

    // ---- members --------------------------------------------------------
    uv_tcp_t       tcp_{};
    uv_write_t     writeReq_{};
    uint64_t       id_;
    Logger&        log_;
    std::string    csvPath_;
    ClosedCallback onClosed_;

    State    state_ = State::ReadHeader;
    bool     closing_ = false;

    std::array<char, proto::kHeaderSize> header_{};
    size_t   headerFill_ = 0;
    uint64_t payloadExpected_ = 0;
    uint64_t payloadReceived_ = 0;
    uint64_t progressMark_ = 0;

    std::vector<char> readBuf_;
    std::string       response_;
    StreamParser      parser_;
};

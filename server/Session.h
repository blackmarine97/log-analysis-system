#pragma once
// Session: one accepted TCP connection, driven entirely by libuv callbacks.
//
// State machine:
//
//   ReadHeader --(UploadLog)---> ReadUploadPrefix --(id+offset)-------+
//   ReadHeader --(FetchResult)-> ReadFetchKey --(12-byte key)---------+
//   ReadHeader --(ResumeQuery)-> ReadResumeKey --(16-byte key)--------+
//                                                                     v
//              [UploadLog: ReadPayload --(file bytes streamed)--] ReadTrailer
//   ReadTrailer --(CRC-32 over the WHOLE file matches)--> Responding
//   Responding --(response frame fully written)--> close
//   any state --(read error / EOF / bad frame / CRC mismatch)--> close
//
// FetchResult re-serves a cached result (see ResultCache.h): a client whose
// result download was cut off retries with 12 bytes instead of 500 MB.
// ResumeQuery + the UploadLog start offset make uploads resumable: received
// bytes are spooled to disk (see SpoolStore.h); after a disconnect the next
// attempt continues from the spooled offset. Resuming re-parses the spooled
// prefix synchronously to rebuild parser + CRC state — a deliberate trade:
// ~0.6 s of loop time per 500 MB prefix on the rare recovery path, instead
// of cross-thread session lifetime management on every upload.
//
// The payload is NEVER buffered: every received chunk is handed straight to
// StreamParser::feed(), so memory stays O(64 KiB read buffer + one log line
// + stats map) regardless of file size.
//
// Lifetime: TcpServer owns each Session via std::unique_ptr and destroys it
// only after libuv confirms the handle is closed (onClosed callback). All
// libuv callback trampolines recover `this` from handle->data; no manual
// new/delete anywhere.
//
// Threading: everything here runs on the loop thread. The two things that
// could block it — log file I/O and the result.csv copy — are delegated to
// Logger's writer thread and CsvWriter (libuv thread pool) respectively.

#include "../common/Protocol.h"
#include "CsvWriter.h"
#include "Logger.h"
#include "ResultCache.h"
#include "SpoolStore.h"
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

    // idleTimeoutMs: close the connection if no byte arrives (or, while
    // responding, the write does not complete) for this long. 0 disables.
    Session(uv_loop_t* loop, uint64_t id, Logger& log,
            CsvWriter& csv, ResultCache& cache, SpoolStore& spool,
            uint64_t idleTimeoutMs, ClosedCallback onClosed)
        : id_(id),
          log_(log),
          csv_(csv),
          cache_(cache),
          spool_(spool),
          idleTimeoutMs_(idleTimeoutMs),
          onClosed_(std::move(onClosed)),
          parser_([this](uint64_t lineNo, SkipReason why,
                         std::string_view line) {
              // While rebuilding from the spool these lines were already
              // logged by the attempt that received them; re-logging them
              // would make every resume look like fresh corruption. They
              // are still COUNTED (the per-session statistics are exact) —
              // the rebuild summary reports how many were re-seen.
              if (rebuilding_) { ++rebuiltSkips_; return; }
              std::ostringstream os;
              // sanitizeForLog: corrupt data may contain control characters
              // or ANSI escapes that would forge entries / garble terminals.
              os << "session " << id_ << ": skipped malformed line "
                 << lineNo << " [" << toString(why) << "]: "
                 << sanitizeForLog(line);
              log_.warn(os.str());
          }) {
        uv_tcp_init(loop, &tcp_);
        tcp_.data = this;
        uv_timer_init(loop, &idle_);
        idle_.data = this;
    }

    // Non-copyable, non-movable: libuv holds a stable pointer to tcp_.
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Accept the pending connection and start reading. On failure the caller
    // must still close() the session to release the handle.
    bool accept(uv_stream_t* listener) {
        if (uv_accept(listener, streamHandle()) != 0) return false;
        // The 64 KiB read buffer is allocated here, not in the constructor:
        // a session created only to be refused (acceptBusy) never reads, so
        // a connection flood cannot hold a buffer per rejected socket.
        readBuf_.resize(kReadBufSize);
        touchIdle();
        return uv_read_start(streamHandle(), &Session::onAlloc,
                             &Session::onRead) == 0;
    }

    // Closes both handles. The session is handed back to its owner only
    // after libuv has confirmed *both* are closed (see onHandleClosed).
    void close() {
        if (closing_) return;
        closing_ = true;
        pendingCloses_ = 2;
        uv_close(reinterpret_cast<uv_handle_t*>(&idle_),
                 &Session::onHandleClosed);
        uv_close(reinterpret_cast<uv_handle_t*>(&tcp_),
                 &Session::onHandleClosed);
    }

    // Accept a connection only to refuse it politely: used by TcpServer when
    // the concurrent-session cap is reached. Sends an Error frame (so the
    // client shows a real message instead of a dropped connection) and closes
    // after the write. The idle timer guards the write like any response.
    bool acceptBusy(uv_stream_t* listener) {
        if (uv_accept(listener, streamHandle()) != 0) return false;
        touchIdle();
        sendError("server busy: connection limit reached, try again later");
        return true;
    }

    uint64_t id() const { return id_; }

private:
    enum class State { ReadHeader, ReadUploadPrefix, ReadPayload,
                       ReadFetchKey, ReadResumeKey, ReadTrailer, Responding };

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
            self->touchIdle();            // progress: push the deadline out
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
            // Mid-protocol drop: payload unfinished, or finished but the
            // CRC trailer never arrived — both are aborted uploads.
            if (self->state_ == State::ReadTrailer ||
                (self->state_ == State::ReadPayload &&
                 self->payloadReceived_ < self->payloadExpected_))
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

    // Fired once per handle (tcp_, idle_). Only the last one hands the
    // session back to its owner — until then libuv may still touch the
    // other handle, so the object must stay alive.
    static void onHandleClosed(uv_handle_t* h) {
        auto* self = static_cast<Session*>(h->data);
        if (--self->pendingCloses_ > 0) return;
        if (self->onClosed_) self->onClosed_(self);
    }

    // Idle deadline expired: nothing arrived (or the response did not
    // finish writing) for idleTimeoutMs_. A silently vanished peer — power
    // loss, pulled cable, NAT eviction — produces no FIN/RST, so without
    // this the session (fd + ~70 KB) would linger until the daemon exits.
    static void onIdle(uv_timer_t* t) {
        auto* self = static_cast<Session*>(t->data);
        std::ostringstream os;
        os << "session " << self->id_ << ": idle timeout ("
           << self->idleTimeoutMs_ << " ms without progress) after "
           << self->payloadReceived_ << '/' << self->payloadExpected_
           << " payload bytes, closing";
        self->log_.warn(os.str());
        self->close();
    }

    // (Re)arm the idle deadline. uv_timer_start on a running timer simply
    // restarts it, so calling this on every received chunk is cheap.
    void touchIdle() {
        if (idleTimeoutMs_ == 0 || closing_) return;
        uv_timer_start(&idle_, &Session::onIdle, idleTimeoutMs_, 0);
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
                if (!hdr || (hdr->type != proto::MsgType::UploadLog &&
                             hdr->type != proto::MsgType::FetchResult &&
                             hdr->type != proto::MsgType::ResumeQuery)) {
                    log_.error("session " + std::to_string(id_) +
                               ": invalid frame header, dropping connection");
                    sendError("invalid frame header");
                    return;
                }
                msgType_ = hdr->type;
                payloadExpected_ = hdr->payloadSize;
                payloadReceived_ = 0;
                crc_ = proto::kCrc32Init;
                if (msgType_ == proto::MsgType::FetchResult) {
                    if (payloadExpected_ != proto::kFetchKeySize) {
                        sendError("invalid FetchResult payload size");
                        return;
                    }
                    state_ = State::ReadFetchKey;
                    break;
                }
                if (msgType_ == proto::MsgType::ResumeQuery) {
                    if (payloadExpected_ != proto::kResumeKeySize) {
                        sendError("invalid ResumeQuery payload size");
                        return;
                    }
                    state_ = State::ReadResumeKey;
                    break;
                }
                if (payloadExpected_ < proto::kUploadPrefixSize) {
                    sendError("invalid UploadLog payload size");
                    return;
                }
                state_ = State::ReadUploadPrefix;
                break;
            }
            case State::ReadUploadPrefix: {
                const size_t want = proto::kUploadPrefixSize - prefixFill_;
                const size_t take = chunk.size() < want ? chunk.size() : want;
                std::memcpy(uploadPrefix_.data() + prefixFill_,
                            chunk.data(), take);
                prefixFill_ += take;
                chunk.remove_prefix(take);
                if (prefixFill_ < proto::kUploadPrefixSize) return;

                proto::decodeUploadPrefix(uploadPrefix_, uploadId_,
                                          startOffset_);
                payloadExpected_ -= proto::kUploadPrefixSize;  // file bytes
                fileSize_ = startOffset_ + payloadExpected_;
                log_.info("session " + std::to_string(id_) +
                          ": upload started, file " +
                          std::to_string(fileSize_) + " bytes from offset " +
                          std::to_string(startOffset_));

                if (startOffset_ > 0 && !rebuildFromSpool()) {
                    // Spool and offset disagree (pruned, restarted with a
                    // different file, ...). Refuse; the client falls back to
                    // a full upload from offset 0.
                    sendError("resume offset mismatch: please restart the "
                              "upload from the beginning");
                    return;
                }
                spoolOut_ = spool_.openAppend(uploadId_, startOffset_);
                state_ = payloadExpected_ == 0 ? State::ReadTrailer
                                               : State::ReadPayload;
                break;
            }
            case State::ReadPayload: {
                const uint64_t want = payloadExpected_ - payloadReceived_;
                const size_t   take = chunk.size() < want
                                        ? chunk.size()
                                        : static_cast<size_t>(want);
                // CRC and parse from the same pass over the chunk; nothing
                // is buffered, so integrity checking costs no extra memory.
                crc_ = proto::crc32Update(crc_, chunk.data(), take);
                parser_.feed(chunk.substr(0, take));
                if (spoolOut_.is_open()) {
                    // Same bytes onto disk: this is what a later ResumeQuery
                    // is answered from. A failed write only disables resume
                    // for this upload, never the upload itself.
                    spoolOut_.write(chunk.data(),
                                    static_cast<std::streamsize>(take));
                    if (!spoolOut_) {
                        log_.warn("session " + std::to_string(id_) +
                                  ": spool write failed, resume disabled "
                                  "for this upload");
                        spoolOut_.close();
                        spool_.remove(uploadId_);
                    }
                }
                payloadReceived_ += take;
                chunk.remove_prefix(take);

                if (payloadReceived_ / kProgressLogEvery != progressMark_) {
                    progressMark_ = payloadReceived_ / kProgressLogEvery;
                    log_.info("session " + std::to_string(id_) + ": received " +
                              std::to_string(payloadReceived_ >> 20) + " MiB");
                }
                if (payloadReceived_ == payloadExpected_)
                    state_ = State::ReadTrailer;
                break;
            }
            case State::ReadResumeKey: {
                const size_t want = proto::kResumeKeySize - resumeKeyFill_;
                const size_t take = chunk.size() < want ? chunk.size() : want;
                std::memcpy(resumeKey_.data() + resumeKeyFill_,
                            chunk.data(), take);
                resumeKeyFill_ += take;
                chunk.remove_prefix(take);
                if (resumeKeyFill_ < proto::kResumeKeySize) return;
                crc_ = proto::crc32Update(crc_, resumeKey_.data(),
                                          resumeKey_.size());
                state_ = State::ReadTrailer;
                break;
            }
            case State::ReadFetchKey: {
                const size_t want = proto::kFetchKeySize - fetchKeyFill_;
                const size_t take = chunk.size() < want ? chunk.size() : want;
                std::memcpy(fetchKey_.data() + fetchKeyFill_,
                            chunk.data(), take);
                fetchKeyFill_ += take;
                chunk.remove_prefix(take);
                if (fetchKeyFill_ < proto::kFetchKeySize) return;
                crc_ = proto::crc32Update(crc_, fetchKey_.data(),
                                          fetchKey_.size());
                state_ = State::ReadTrailer;
                break;
            }
            case State::ReadTrailer: {
                const size_t want = proto::kTrailerSize - trailerFill_;
                const size_t take = chunk.size() < want ? chunk.size() : want;
                std::memcpy(trailer_.data() + trailerFill_, chunk.data(), take);
                trailerFill_ += take;
                chunk.remove_prefix(take);
                if (trailerFill_ < proto::kTrailerSize) return;

                const uint32_t expect = proto::decodeTrailer(trailer_);
                const uint32_t actual = proto::crc32Final(crc_);
                if (actual != expect) {
                    // Corruption in transit: the statistics computed from the
                    // damaged stream must never be presented as a result.
                    std::ostringstream os;
                    os << "session " << id_ << ": payload CRC mismatch (got "
                       << std::hex << actual << ", expected " << expect
                       << "), discarding analysis";
                    log_.error(os.str());
                    if (msgType_ == proto::MsgType::UploadLog) {
                        // The spool now holds bytes that failed verification;
                        // it must never seed a future resume.
                        if (spoolOut_.is_open()) spoolOut_.close();
                        spool_.remove(uploadId_);
                    }
                    sendError("payload checksum mismatch: upload corrupted "
                              "in transit, please retry");
                    return;
                }
                if (msgType_ == proto::MsgType::FetchResult)      serveCached();
                else if (msgType_ == proto::MsgType::ResumeQuery) serveResumeOffset();
                else                                              finalize();
                return;
            }
            case State::Responding:
                // Trailing bytes after a complete payload: protocol violation
                // by the peer; ignore them, the response is already in flight.
                return;
            }
        }
    }

    // FetchResult: look the key up in the cache and re-serve the analysis
    // without any re-upload. A miss (evicted, or the server restarted) is an
    // explicit Error so the client knows to fall back to a full upload.
    void serveCached() {
        state_ = State::Responding;
        uv_read_stop(streamHandle());
        touchIdle();
        uint64_t size = 0;
        uint32_t crc = 0;
        proto::decodeFetchKey(fetchKey_, size, crc);
        std::ostringstream os;
        os << "session " << id_ << ": fetch request for size=" << size
           << " crc=" << std::hex << crc;
        if (auto csv = cache_.get(size, crc)) {
            os << " -> served from cache (" << std::dec << csv->size()
               << " bytes)";
            log_.info(os.str());
            sendFrame(proto::MsgType::ResultCsv, std::move(*csv));
        } else {
            os << " -> not cached";
            log_.warn(os.str());
            sendError("result not cached: please re-upload");
        }
    }

    // ResumeQuery: how many bytes of this upload are already spooled?
    void serveResumeOffset() {
        state_ = State::Responding;
        uv_read_stop(streamHandle());
        touchIdle();
        uint64_t uploadId = 0, fileSize = 0;
        proto::decodeResumeKey(resumeKey_, uploadId, fileSize);
        const uint64_t offset = spool_.offsetOf(uploadId, fileSize);
        std::ostringstream os;
        os << "session " << id_ << ": resume query for upload "
           << std::hex << uploadId << std::dec << " (" << fileSize
           << " bytes) -> offset " << offset;
        log_.info(os.str());
        std::string body(8, '\0');
        proto::lePut64(body.data(), offset);
        sendFrame(proto::MsgType::ResumeOffset, std::move(body));
    }

    // Resume: replay the spooled prefix through a fresh parser + CRC so the
    // live stream can continue exactly where the previous attempt stopped.
    // Runs synchronously on the loop thread — see the header comment for
    // why that trade was chosen over a worker thread.
    bool rebuildFromSpool() {
        if (spool_.offsetOf(uploadId_, fileSize_) != startOffset_)
            return false;
        std::ifstream in(spool_.pathFor(uploadId_), std::ios::binary);
        if (!in) return false;
        rebuilding_ = true;
        rebuiltSkips_ = 0;
        std::vector<char> buf(1 << 20);
        uint64_t total = 0;
        while (in) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            const std::streamsize n = in.gcount();
            if (n <= 0) break;
            crc_ = proto::crc32Update(crc_, buf.data(),
                                      static_cast<size_t>(n));
            parser_.feed(std::string_view(buf.data(),
                                          static_cast<size_t>(n)));
            total += static_cast<uint64_t>(n);
        }
        rebuilding_ = false;
        if (total != startOffset_) return false;
        log_.info("session " + std::to_string(id_) +
                  ": resumed, parser/CRC state rebuilt from " +
                  std::to_string(total >> 20) + " MiB of spool (" +
                  std::to_string(rebuiltSkips_) + " malformed lines "
                  "re-counted; already logged by the earlier attempt)");
        return true;
    }

    void finalize() {
        state_ = State::Responding;
        uv_read_stop(streamHandle());
        touchIdle();   // the response write gets a full idle window too
        parser_.finish();

        std::ostringstream csv;
        parser_.writeCsv(csv);
        std::string body = csv.str();

        // Retain the result: if the response write below never reaches the
        // client, a FetchResult retry gets this analysis instead of forcing
        // a 500 MB re-upload. Keyed by what was actually analysed (the whole
        // file, not just this attempt's slice).
        cache_.put(fileSize_, proto::crc32Final(crc_), body);

        // The upload is complete and verified: its spool has served its
        // purpose (the recoverable artifact is now the cached result).
        if (spoolOut_.is_open()) spoolOut_.close();
        spool_.remove(uploadId_);

        // Server-side copy as evidence/debug artifact: handed to CsvWriter,
        // which does the blocking file I/O on the libuv thread pool. The
        // session does not wait for it — the response goes out immediately.
        csv_.submit(id_, body);

        const Stats& st = parser_.stats();
        log_.info("session " + std::to_string(id_) + ": parse complete, lines=" +
                  std::to_string(st.totalLines) + " skipped=" +
                  std::to_string(st.skippedLines) + " speedCount=" +
                  std::to_string(st.speedCount));

        sendFrame(proto::MsgType::ResultCsv, std::move(body));
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
        std::array<char, proto::kTrailerSize> tr{};
        proto::encodeTrailer(
            proto::crc32Final(
                proto::crc32Update(proto::kCrc32Init, body.data(), body.size())),
            tr);
        response_.append(tr.data(), tr.size());

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
    uv_timer_t     idle_{};
    uv_write_t     writeReq_{};
    uint64_t       id_;
    Logger&        log_;
    CsvWriter&     csv_;
    ResultCache&   cache_;
    SpoolStore&    spool_;
    uint64_t       idleTimeoutMs_;
    ClosedCallback onClosed_;

    State    state_ = State::ReadHeader;
    bool     closing_ = false;
    int      pendingCloses_ = 0;

    std::array<char, proto::kHeaderSize>       header_{};
    std::array<char, proto::kTrailerSize>      trailer_{};
    std::array<char, proto::kFetchKeySize>     fetchKey_{};
    std::array<char, proto::kResumeKeySize>    resumeKey_{};
    std::array<char, proto::kUploadPrefixSize> uploadPrefix_{};
    proto::MsgType msgType_ = proto::MsgType::UploadLog;
    size_t   headerFill_ = 0;
    size_t   trailerFill_ = 0;
    size_t   fetchKeyFill_ = 0;
    size_t   resumeKeyFill_ = 0;
    size_t   prefixFill_ = 0;
    uint64_t uploadId_ = 0;
    uint64_t startOffset_ = 0;
    uint64_t fileSize_ = 0;
    bool     rebuilding_ = false;
    uint64_t rebuiltSkips_ = 0;
    std::ofstream spoolOut_;
    uint32_t crc_ = proto::kCrc32Init;
    uint64_t payloadExpected_ = 0;
    uint64_t payloadReceived_ = 0;
    uint64_t progressMark_ = 0;

    std::vector<char> readBuf_;
    std::string       response_;
    StreamParser      parser_;
};

// log_server: receives a large log file over TCP, parses it as a stream
// (memory O(64 KiB), never buffering the file), and returns result.csv.
//
// Usage:
//   ./log_server [--port N] [--bind ADDR] [--daemon]
//                [--log FILE] [--csv FILE] [--idle-timeout SEC]
//
// Defaults: port 5555, bind 0.0.0.0, foreground, log to stderr,
//           server-side CSV copy to ./result.csv, idle timeout 60 s
//           (0 disables; a connection that makes no progress for this
//           long is closed and its resources reclaimed).

#include "CsvWriter.h"
#include "Logger.h"
#include "TcpServer.h"

#include <uv.h>

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

#include <unistd.h>   // daemon(3)

namespace {

struct Options {
    int         port = 5555;
    std::string bindAddr = "0.0.0.0";
    bool        daemonize = false;
    std::string logPath;              // empty -> stderr
    std::string csvPath = "result.csv";
    int         idleTimeoutSec = 60;  // 0 = disabled
};

void printUsage() {
    std::cerr <<
        "usage: log_server [--port N] [--bind ADDR] [--daemon]\n"
        "                  [--log FILE] [--csv FILE] [--idle-timeout SEC]\n";
}

bool parseArgs(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](std::string& dst) {
            if (i + 1 >= argc) return false;
            dst = argv[++i];
            return true;
        };
        std::string v;
        if (arg == "--port" && next(v))      opt.port = std::atoi(v.c_str());
        else if (arg == "--bind" && next(v)) opt.bindAddr = v;
        else if (arg == "--log" && next(v))  opt.logPath = v;
        else if (arg == "--csv" && next(v))  opt.csvPath = v;
        else if (arg == "--idle-timeout" && next(v))
            opt.idleTimeoutSec = std::atoi(v.c_str());
        else if (arg == "--daemon")          opt.daemonize = true;
        else { printUsage(); return false; }
    }
    if (opt.port <= 0 || opt.port > 65535) { printUsage(); return false; }
    if (opt.idleTimeoutSec < 0) { printUsage(); return false; }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) return 1;

    if (opt.daemonize) {
        if (opt.logPath.empty()) opt.logPath = "log_server.log";
        if (daemon(/*nochdir=*/1, /*noclose=*/0) != 0) {
            std::cerr << "daemon() failed\n";
            return 1;
        }
    }

    // A peer vanishing mid-write must produce an error return, not SIGPIPE.
    std::signal(SIGPIPE, SIG_IGN);

    uv_loop_t loop;
    uv_loop_init(&loop);

    // Declaration order == reverse destruction order, and that order matters:
    // Logger (own writer thread) must outlive everything that logs; CsvWriter
    // must outlive uv_run() so its thread-pool jobs can report back.
    Logger log(opt.logPath);
    log.info("log_server starting (pid " + std::to_string(getpid()) + ')');

    CsvWriter csvWriter(&loop, log, opt.csvPath);
    TcpServer server(&loop, log, csvWriter,
                     static_cast<uint64_t>(opt.idleTimeoutSec) * 1000);
    if (!server.listen(opt.bindAddr, opt.port)) {
        uv_loop_close(&loop);
        return 1;
    }

    // SIGINT/SIGTERM -> graceful shutdown; the loop drains remaining close
    // callbacks and uv_run returns once no handle is left alive.
    uv_signal_t sigint{}, sigterm{};
    uv_signal_init(&loop, &sigint);
    uv_signal_init(&loop, &sigterm);
    struct Ctx { TcpServer* srv; uv_signal_t* a; uv_signal_t* b; };
    Ctx ctx{&server, &sigint, &sigterm};
    sigint.data = sigterm.data = &ctx;
    // Both handles are closed on the first signal. The uv_is_closing() guards
    // matter when SIGINT and SIGTERM land in the same loop iteration: the
    // second callback would otherwise uv_close() an already-closing handle.
    auto onSignal = [](uv_signal_t* h, int /*signum*/) {
        auto* c = static_cast<Ctx*>(h->data);
        c->srv->shutdown();
        for (uv_signal_t* s : {c->a, c->b}) {
            auto* handle = reinterpret_cast<uv_handle_t*>(s);
            if (!uv_is_closing(handle)) uv_close(handle, nullptr);
        }
    };
    uv_signal_start(&sigint, onSignal, SIGINT);
    uv_signal_start(&sigterm, onSignal, SIGTERM);

    uv_run(&loop, UV_RUN_DEFAULT);

    uv_loop_close(&loop);
    log.info("log_server stopped");
    return 0;
}

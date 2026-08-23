#!/usr/bin/env sh
# Start the server with sensible defaults. Works from the repository root
# (binary in ./bin) or from inside bin/ itself (binary next to this script).
# Extra arguments are passed through, e.g. ./run_server.sh --port 6000
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -x "$HERE/log_server" ]; then BIN="$HERE/log_server"; else BIN="$HERE/bin/log_server"; fi
cd "$HERE" && exec "$BIN" --csv result.csv "$@"

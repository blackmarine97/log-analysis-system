#!/usr/bin/env sh
# Start the server from the repository root with sensible defaults.
# Extra arguments are passed through, e.g. ./run_server.sh --port 6000
cd "$(dirname "$0")" && exec ./bin/log_server --log server.log --csv result.csv "$@"

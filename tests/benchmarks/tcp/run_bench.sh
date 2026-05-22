#!/bin/bash
#
# TCP Echo Benchmark Runner
# Starts each echo server, runs the benchmark client, saves results.
#
# Usage:
#   ./run_bench.sh              # Run all servers
#   ./run_bench.sh xray         # Run only xray
#   ./run_bench.sh xray go      # Run xray and go
#
# Requirements:
#   - xray binary (../../build/xray or in PATH)
#   - go (for Go server)
#   - node (for Node.js server)
#   - python3 (for Python server and benchmark client)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RESULTS_DIR="$SCRIPT_DIR/results"
PORT=9001

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# ========== Utility Functions ==========

log_info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*"; }
log_test()  { echo -e "${CYAN}[TEST]${NC} $*"; }

cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null || true
    fi
}

trap cleanup EXIT

wait_for_port() {
    local port=$1
    local timeout=${2:-10}
    local elapsed=0
    while ! nc -z 127.0.0.1 "$port" 2>/dev/null; do
        sleep 0.2
        elapsed=$(echo "$elapsed + 0.2" | bc)
        if (( $(echo "$elapsed >= $timeout" | bc -l) )); then
            return 1
        fi
    done
    return 0
}

kill_port() {
    local port=$1
    local pids
    pids=$(lsof -ti :"$port" 2>/dev/null || true)
    if [ -n "$pids" ]; then
        echo "$pids" | xargs kill -9 2>/dev/null || true
        sleep 0.5
    fi
}

run_benchmark() {
    local server_name=$1
    local output_file="$RESULTS_DIR/${server_name}.json"

    log_test "Running benchmark for $server_name..."
    python3 "$SCRIPT_DIR/tcp_bench.py" \
        --host 127.0.0.1 \
        --port "$PORT" \
        --server "$server_name" \
        --output "$output_file" \
        --wait

    if [ -f "$output_file" ]; then
        log_info "Results saved to $output_file"
    else
        log_error "Benchmark failed for $server_name"
    fi
}

# ========== Find xray binary ==========

find_xray() {
    # Try common locations
    local candidates=(
        "$SCRIPT_DIR/../../build/xray"
        "$SCRIPT_DIR/../../build-release/xray"
        "$(which xray 2>/dev/null || true)"
    )
    for c in "${candidates[@]}"; do
        if [ -n "$c" ] && [ -x "$c" ]; then
            echo "$c"
            return 0
        fi
    done
    return 1
}

# ========== Server Runners ==========

run_xray() {
    local xray_bin
    xray_bin=$(find_xray) || {
        log_warn "xray binary not found, skipping xray test"
        return 1
    }
    log_info "Using xray: $xray_bin"

    kill_port "$PORT"
    "$xray_bin" "$SCRIPT_DIR/echo_server.xr" -- "$PORT" &
    SERVER_PID=$!

    if ! wait_for_port "$PORT" 5; then
        log_error "xray server failed to start"
        kill "$SERVER_PID" 2>/dev/null || true
        return 1
    fi

    run_benchmark "xray"

    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
    sleep 0.5
}

run_go() {
    if ! command -v go &>/dev/null; then
        log_warn "go not found, skipping Go test"
        return 1
    fi

    log_info "Building Go echo server..."
    (cd "$SCRIPT_DIR" && go build -o echo_server_go echo_server.go)

    kill_port "$PORT"
    "$SCRIPT_DIR/echo_server_go" "$PORT" &
    SERVER_PID=$!

    if ! wait_for_port "$PORT" 5; then
        log_error "Go server failed to start"
        kill "$SERVER_PID" 2>/dev/null || true
        return 1
    fi

    run_benchmark "go"

    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
    sleep 0.5
}

run_node() {
    if ! command -v node &>/dev/null; then
        log_warn "node not found, skipping Node.js test"
        return 1
    fi

    kill_port "$PORT"
    node "$SCRIPT_DIR/echo_server.js" "$PORT" &
    SERVER_PID=$!

    if ! wait_for_port "$PORT" 5; then
        log_error "Node.js server failed to start"
        kill "$SERVER_PID" 2>/dev/null || true
        return 1
    fi

    run_benchmark "node"

    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
    sleep 0.5
}

run_python() {
    if ! command -v python3 &>/dev/null; then
        log_warn "python3 not found, skipping Python test"
        return 1
    fi

    kill_port "$PORT"
    python3 "$SCRIPT_DIR/echo_server.py" "$PORT" &
    SERVER_PID=$!

    if ! wait_for_port "$PORT" 5; then
        log_error "Python server failed to start"
        kill "$SERVER_PID" 2>/dev/null || true
        return 1
    fi

    run_benchmark "python"

    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
    sleep 0.5
}

# ========== Main ==========

main() {
    echo ""
    echo "╔═══════════════════════════════════════════╗"
    echo "║       TCP Echo Server Benchmark           ║"
    echo "║  xray vs Go vs Node.js vs Python          ║"
    echo "╚═══════════════════════════════════════════╝"
    echo ""

    mkdir -p "$RESULTS_DIR"

    # Determine which servers to test
    local servers=("$@")
    if [ ${#servers[@]} -eq 0 ]; then
        servers=(xray go node python)
    fi

    local tested=0
    for server in "${servers[@]}"; do
        echo ""
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        log_info "Testing: $server"
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

        case "$server" in
            xray)   run_xray   && ((tested++)) || true ;;
            go)     run_go     && ((tested++)) || true ;;
            node)   run_node   && ((tested++)) || true ;;
            python) run_python && ((tested++)) || true ;;
            *)      log_warn "Unknown server: $server" ;;
        esac
    done

    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    log_info "Benchmarks complete! Tested $tested servers."
    echo ""

    # Auto-compare if multiple results exist
    local result_count
    result_count=$(ls "$RESULTS_DIR"/*.json 2>/dev/null | wc -l | tr -d ' ')
    if [ "$result_count" -gt 1 ]; then
        log_info "Running comparison..."
        echo ""
        python3 "$SCRIPT_DIR/compare.py" "$RESULTS_DIR/"
    elif [ "$result_count" -eq 1 ]; then
        log_info "Only 1 result file, skipping comparison."
        log_info "Run more servers to see a comparison."
    fi
}

main "$@"

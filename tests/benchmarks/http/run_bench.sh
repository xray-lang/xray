#!/bin/bash
#
# run_bench.sh - HTTP server benchmark: xray vs Go vs fasthttp vs Node.js vs Python
#
# Uses wrk for accurate high-throughput benchmarking (no client bottleneck).
#
# Usage:
#   ./tests/benchmarks/http/run_bench.sh [OPTIONS]
#
# Options:
#   --quick           Shorter duration (4s instead of 10s)
#   --highconn        Also test at 512, 1024, 4096 connections
#   --only NAME       Only test one server: xray|go|fasthttp|node|python
#   --port PORT       Port to use (default: 8080)
#   --threads N       wrk threads (default: 4)
#   --connections N   wrk connections (default: 128)
#   --duration N      wrk duration in seconds (default: 10)
#   --compare         Print side-by-side comparison from saved JSON results

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
XRAY_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
PORT="${PORT:-8080}"
ONLY=""
HIGHCONN=0
COMPARE=0
RESULTS_DIR="$SCRIPT_DIR/results"

# wrk parameters
WRK_THREADS=4
WRK_CONNS=128
WRK_DURATION=10
WRK_WARMUP=3

# ── Parse args ───────────────────────────────────────────────────────────────

while [[ $# -gt 0 ]]; do
    case $1 in
        --quick)       WRK_DURATION=4; WRK_WARMUP=2; shift ;;
        --highconn)    HIGHCONN=1; shift ;;
        --only)        ONLY="$2"; shift 2 ;;
        --port)        PORT="$2"; shift 2 ;;
        --threads)     WRK_THREADS="$2"; shift 2 ;;
        --connections) WRK_CONNS="$2"; shift 2 ;;
        --duration)    WRK_DURATION="$2"; shift 2 ;;
        --compare)     COMPARE=1; shift ;;
        *)             echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ── Compare saved results ────────────────────────────────────────────────────

if [ "$COMPARE" = "1" ]; then
    python3 "$SCRIPT_DIR/compare.py" "$RESULTS_DIR"
    exit 0
fi

# ── Check wrk ────────────────────────────────────────────────────────────────

if ! command -v wrk &>/dev/null; then
    echo "ERROR: wrk not found. Install with: brew install wrk"
    exit 1
fi

# ── Find xray binary ────────────────────────────────────────────────────────

find_xray() {
    if [ -n "$XRAY_BIN" ] && [ -f "$XRAY_BIN" ]; then
        return 0
    fi
    for d in build-embed build-release build build-new; do
        if [ -f "$XRAY_ROOT/$d/xray" ]; then
            XRAY_BIN="$XRAY_ROOT/$d/xray"
            return 0
        fi
        if [ -f "$XRAY_ROOT/$d/xray.exe" ]; then
            XRAY_BIN="$XRAY_ROOT/$d/xray.exe"
            return 0
        fi
    done
    echo "ERROR: xray binary not found. Set XRAY_BIN env var or build first."
    return 1
}

# ── Port management ─────────────────────────────────────────────────────────

cleanup_port() {
    lsof -ti :$PORT 2>/dev/null | xargs kill -9 2>/dev/null || true
    sleep 0.5
    lsof -ti :$PORT 2>/dev/null | xargs kill -9 2>/dev/null || true
    sleep 0.5
}

wait_for_server() {
    local name=$1
    local max_wait=20
    local i=0
    while [ $i -lt $max_wait ]; do
        if curl -s -o /dev/null "http://127.0.0.1:$PORT/plaintext" 2>/dev/null; then
            return 0
        fi
        sleep 0.5
        i=$((i + 1))
    done
    echo "  WARNING: $name did not start within 10s"
    return 1
}

# ── wrk benchmark runner ────────────────────────────────────────────────────

run_wrk() {
    local endpoint=$1
    local label=$2
    local conns=${3:-$WRK_CONNS}
    local url="http://127.0.0.1:$PORT$endpoint"

    # Warmup
    wrk -t2 -c$conns -d${WRK_WARMUP}s "$url" > /dev/null 2>&1
    sleep 0.5

    # Actual benchmark
    echo "  $label: wrk -t$WRK_THREADS -c$conns -d${WRK_DURATION}s $url"
    wrk -t$WRK_THREADS -c$conns -d${WRK_DURATION}s "$url" 2>&1 | \
        grep -E "Requests/sec|Latency|Transfer/sec|Socket errors" | \
        sed 's/^/    /'
}

run_all_endpoints() {
    local name=$1
    local log_file="$RESULTS_DIR/${name}.txt"

    echo "  --- /plaintext (GET, c=$WRK_CONNS) ---"
    run_wrk "/plaintext" "plaintext" "$WRK_CONNS" 2>&1 | tee -a "$log_file"
    echo ""
    echo "  --- /json (GET, c=$WRK_CONNS) ---"
    run_wrk "/json" "json" "$WRK_CONNS" 2>&1 | tee -a "$log_file"
    echo ""

    if [ "$HIGHCONN" = "1" ]; then
        for conns in 512 1024 2048; do
            echo "  --- /plaintext (GET, c=$conns) ---"
            run_wrk "/plaintext" "plaintext-c$conns" "$conns" 2>&1 | tee -a "$log_file"
            echo ""
        done
    fi
}

# ── Server launchers ─────────────────────────────────────────────────────────

bench_xray() {
    find_xray || return 1

    echo ""
    echo "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓"
    echo "┃  SERVER: xray (stdlib/http)                           ┃"
    echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"
    echo "  Binary: $XRAY_BIN"

    cleanup_port
    $XRAY_BIN "$SCRIPT_DIR/http_server.xr" -- $PORT > /dev/null 2>&1 &
    local PID=$!
    sleep 1

    if ! kill -0 $PID 2>/dev/null; then
        echo "  ERROR: xray server failed to start"
        return 1
    fi

    if wait_for_server "xray"; then
        > "$RESULTS_DIR/xray.txt"
        run_all_endpoints "xray"
    fi

    kill $PID 2>/dev/null || true
    wait $PID 2>/dev/null || true
    sleep 0.5
}

bench_go() {
    if ! command -v go &>/dev/null; then
        echo "  SKIP: Go not installed"
        return 0
    fi

    echo ""
    echo "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓"
    echo "┃  SERVER: Go (net/http)                                ┃"
    echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"

    cleanup_port

    local GO_BIN="$SCRIPT_DIR/http_server_go"
    if [ ! -f "$GO_BIN" ] || [ "$SCRIPT_DIR/http_server.go" -nt "$GO_BIN" ]; then
        echo "  Building Go server..."
        (cd "$SCRIPT_DIR" && go build -o http_server_go http_server.go) || {
            echo "  ERROR: Go build failed"
            return 1
        }
    fi

    "$GO_BIN" "$PORT" > /dev/null 2>&1 &
    local PID=$!
    sleep 1

    if ! kill -0 $PID 2>/dev/null; then
        echo "  ERROR: Go server failed to start"
        return 1
    fi

    if wait_for_server "Go"; then
        > "$RESULTS_DIR/go.txt"
        run_all_endpoints "go"
    fi

    kill $PID 2>/dev/null || true
    wait $PID 2>/dev/null || true
    sleep 0.5
}

bench_fasthttp() {
    if ! command -v go &>/dev/null; then
        echo "  SKIP: Go not installed"
        return 0
    fi

    echo ""
    echo "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓"
    echo "┃  SERVER: Go (fasthttp)                                ┃"
    echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"

    cleanup_port

    local FASTHTTP_BIN="$SCRIPT_DIR/http_server_fasthttp_bin"
    if [ ! -f "$FASTHTTP_BIN" ] || [ "$SCRIPT_DIR/http_server_fasthttp.go" -nt "$FASTHTTP_BIN" ]; then
        echo "  Building fasthttp server..."
        (cd "$SCRIPT_DIR" && go build -o http_server_fasthttp_bin http_server_fasthttp.go) || {
            echo "  ERROR: fasthttp build failed"
            return 1
        }
    fi

    "$FASTHTTP_BIN" "$PORT" > /dev/null 2>&1 &
    local PID=$!
    sleep 1

    if ! kill -0 $PID 2>/dev/null; then
        echo "  ERROR: fasthttp server failed to start"
        return 1
    fi

    if wait_for_server "fasthttp"; then
        > "$RESULTS_DIR/fasthttp.txt"
        run_all_endpoints "fasthttp"
    fi

    kill $PID 2>/dev/null || true
    wait $PID 2>/dev/null || true
    sleep 0.5
}

bench_node() {
    if ! command -v node &>/dev/null; then
        echo "  SKIP: Node.js not installed"
        return 0
    fi

    echo ""
    echo "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓"
    echo "┃  SERVER: Node.js (http)                               ┃"
    echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"

    cleanup_port
    node "$SCRIPT_DIR/http_server.js" "$PORT" > /dev/null 2>&1 &
    local PID=$!
    sleep 1

    if ! kill -0 $PID 2>/dev/null; then
        echo "  ERROR: Node.js server failed to start"
        return 1
    fi

    if wait_for_server "Node.js"; then
        > "$RESULTS_DIR/node.txt"
        run_all_endpoints "node"
    fi

    kill $PID 2>/dev/null || true
    wait $PID 2>/dev/null || true
    sleep 0.5
}

bench_python() {
    echo ""
    echo "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓"
    echo "┃  SERVER: Python (aiohttp)                             ┃"
    echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"

    cleanup_port
    python3 "$SCRIPT_DIR/http_server.py" "$PORT" > /dev/null 2>&1 &
    local PID=$!
    sleep 2

    if ! kill -0 $PID 2>/dev/null; then
        echo "  ERROR: Python server failed to start"
        return 1
    fi

    if wait_for_server "Python"; then
        > "$RESULTS_DIR/python.txt"
        run_all_endpoints "python"
    fi

    kill $PID 2>/dev/null || true
    wait $PID 2>/dev/null || true
    sleep 0.5
}

# ── Main ─────────────────────────────────────────────────────────────────────

mkdir -p "$RESULTS_DIR"

echo ""
echo "╔══════════════════════════════════════════════════════╗"
echo "║       HTTP Server Benchmark (wrk)                    ║"
echo "╠══════════════════════════════════════════════════════╣"
echo "║  Port        : $PORT"
echo "║  wrk threads : $WRK_THREADS"
echo "║  Connections : $WRK_CONNS"
echo "║  Duration    : ${WRK_DURATION}s (warmup: ${WRK_WARMUP}s)"
if [ "$HIGHCONN" = "1" ]; then
echo "║  High-conn   : 512 / 1024 / 2048"
fi
echo "║  Output      : $RESULTS_DIR/"
echo "╚══════════════════════════════════════════════════════╝"

if [ -n "$ONLY" ]; then
    case "$ONLY" in
        xray)     bench_xray ;;
        go)       bench_go ;;
        fasthttp) bench_fasthttp ;;
        node)     bench_node ;;
        python)   bench_python ;;
        *)        echo "Unknown: $ONLY (choose: xray|go|fasthttp|node|python)"; exit 1 ;;
    esac
else
    bench_xray
    bench_go
    bench_fasthttp
    bench_node
    bench_python
fi

echo ""
echo "╔══════════════════════════════════════════════════════╗"
echo "║  All benchmarks complete                             ║"
echo "╚══════════════════════════════════════════════════════╝"
echo ""
echo "Results in: $RESULTS_DIR/"
echo ""

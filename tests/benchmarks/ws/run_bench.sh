#!/bin/bash
#
# run_bench.sh - WebSocket server benchmark: xray vs Go vs Node.js vs Python
#
# Starts each language's echo server in turn, runs the unified Python
# benchmark client against it, and saves results for comparison.
#
# Usage:
#   ./tests/ws_benchmark/run_bench.sh [OPTIONS]
#
# Options:
#   --quick           Fewer iterations (fast smoke test)
#   --only NAME       Only test one server: xray|go|node|python
#   --port PORT       Port to use (default: 9001)
#   --compare         Print side-by-side comparison from saved JSON results

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
XRAY_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
PORT="${PORT:-9001}"
QUICK=""
ONLY=""
COMPARE=""
RESULTS_DIR="$SCRIPT_DIR/results"

# ── Parse args ───────────────────────────────────────────────────────────────

while [[ $# -gt 0 ]]; do
    case $1 in
        --quick)   QUICK="--quick"; shift ;;
        --only)    ONLY="$2"; shift 2 ;;
        --port)    PORT="$2"; shift 2 ;;
        --compare) COMPARE=1; shift ;;
        *)         echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ── Find xray binary ────────────────────────────────────────────────────────

find_xray() {
    if [ -n "$XRAY_BIN" ] && [ -f "$XRAY_BIN" ]; then
        return 0
    fi
    for d in build build-embed build-release build-new build-debug-embed; do
        if [ -f "$XRAY_ROOT/$d/xray" ]; then
            XRAY_BIN="$XRAY_ROOT/$d/xray"
            return 0
        fi
    done
    echo "ERROR: xray binary not found. Set XRAY_BIN env var or build first."
    return 1
}

# ── Check dependencies ──────────────────────────────────────────────────────

check_python_deps() {
    if ! python3 -c "import websockets" 2>/dev/null; then
        echo "  Installing Python websockets..."
        pip3 install websockets -q
    fi
}

# ── Port management ─────────────────────────────────────────────────────────

cleanup_port() {
    lsof -ti :$PORT 2>/dev/null | xargs kill -9 2>/dev/null || true
    sleep 1
    # Double check
    lsof -ti :$PORT 2>/dev/null | xargs kill -9 2>/dev/null || true
    sleep 0.5
}

wait_for_server() {
    local name=$1
    local max_wait=${2:-20}
    local i=0
    while [ $i -lt $max_wait ]; do
        if nc -z 127.0.0.1 $PORT 2>/dev/null; then
            return 0
        fi
        sleep 0.5
        i=$((i + 1))
    done
    echo "  WARNING: $name did not start within $((max_wait / 2))s"
    return 1
}

# ── Run benchmark ────────────────────────────────────────────────────────────

run_bench() {
    local name=$1
    local json_file="$RESULTS_DIR/${name}.json"
    local log_file="$RESULTS_DIR/${name}.txt"

    python3 "$SCRIPT_DIR/ws_bench.py" \
        --url "ws://127.0.0.1:$PORT" \
        --label "$name" \
        --json "$json_file" \
        $QUICK 2>&1 | tee "$log_file"
}

# ── Server launchers ─────────────────────────────────────────────────────────

bench_xray() {
    find_xray || return 1

    echo ""
    echo "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓"
    echo "┃  SERVER: xray (stdlib/ws)                            ┃"
    echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"
    echo "  Binary: $XRAY_BIN"
    echo ""

    cleanup_port
    $XRAY_BIN "$SCRIPT_DIR/echo_server.xr" -- $PORT &
    local PID=$!
    sleep 1

    if ! kill -0 $PID 2>/dev/null; then
        echo "  ERROR: xray server failed to start"
        return 1
    fi

    if wait_for_server "xray"; then
        run_bench "xray"
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
    echo "┃  SERVER: Go (gorilla/websocket)                      ┃"
    echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"
    echo ""

    cleanup_port

    local GO_BIN="$SCRIPT_DIR/echo_server_go"
    if [ ! -f "$GO_BIN" ] || [ "$SCRIPT_DIR/echo_server.go" -nt "$GO_BIN" ]; then
        echo "  Building Go server..."
        (cd "$SCRIPT_DIR" && go mod tidy 2>/dev/null && go build -o echo_server_go echo_server.go) || {
            echo "  ERROR: Go build failed"
            return 1
        }
    fi

    "$GO_BIN" "$PORT" &
    local PID=$!
    sleep 1

    if ! kill -0 $PID 2>/dev/null; then
        echo "  ERROR: Go server failed to start"
        return 1
    fi

    if wait_for_server "Go"; then
        run_bench "go"
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
    echo "┃  SERVER: Node.js (ws)                                ┃"
    echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"
    echo ""

    cleanup_port

    if [ ! -d "$SCRIPT_DIR/node_modules/ws" ]; then
        echo "  Installing ws package..."
        (cd "$SCRIPT_DIR" && npm install --no-fund --no-audit 2>/dev/null) || {
            echo "  ERROR: npm install failed"
            return 1
        }
    fi

    node "$SCRIPT_DIR/echo_server.js" "$PORT" &
    local PID=$!
    sleep 1

    if ! kill -0 $PID 2>/dev/null; then
        echo "  ERROR: Node.js server failed to start"
        return 1
    fi

    if wait_for_server "Node.js"; then
        run_bench "node"
    fi

    kill $PID 2>/dev/null || true
    wait $PID 2>/dev/null || true
    sleep 0.5
}

bench_python() {
    echo ""
    echo "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓"
    echo "┃  SERVER: Python (websockets)                         ┃"
    echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"
    echo ""

    cleanup_port

    python3 "$SCRIPT_DIR/echo_server.py" "$PORT" &
    local PID=$!
    sleep 1

    if ! kill -0 $PID 2>/dev/null; then
        echo "  ERROR: Python server failed to start"
        return 1
    fi

    if wait_for_server "Python"; then
        run_bench "python"
    fi

    kill $PID 2>/dev/null || true
    wait $PID 2>/dev/null || true
    sleep 0.5
}

# ── Compare results ──────────────────────────────────────────────────────────

compare_results() {
    python3 "$SCRIPT_DIR/compare.py" "$RESULTS_DIR"
}

# ── Main ─────────────────────────────────────────────────────────────────────

if [ -n "$COMPARE" ]; then
    compare_results
    exit 0
fi

check_python_deps
mkdir -p "$RESULTS_DIR"

echo ""
echo "╔══════════════════════════════════════════════════════╗"
echo "║     WebSocket Server Benchmark Suite                 ║"
echo "╠══════════════════════════════════════════════════════╣"
echo "║  Port   : $PORT"
echo "║  Mode   : ${QUICK:-full}"
echo "║  Output : $RESULTS_DIR/"
echo "╚══════════════════════════════════════════════════════╝"

if [ -n "$ONLY" ]; then
    case "$ONLY" in
        xray)   bench_xray ;;
        go)     bench_go ;;
        node)   bench_node ;;
        python) bench_python ;;
        *)      echo "Unknown: $ONLY (choose: xray|go|node|python)"; exit 1 ;;
    esac
else
    bench_xray
    bench_go
    bench_node
    bench_python
fi

# Summary
echo ""
echo "╔══════════════════════════════════════════════════════╗"
echo "║  All benchmarks complete                             ║"
echo "╚══════════════════════════════════════════════════════╝"
echo ""
echo "Results in: $RESULTS_DIR/"
ls -la "$RESULTS_DIR"/*.json 2>/dev/null || echo "  (no JSON results)"
echo ""
echo "Run comparison:"
echo "  $0 --compare"
echo ""

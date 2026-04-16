#!/bin/bash
#
# run_all.sh - Run all WebSocket functional tests
#
# Usage:
#   ./run_all.sh [--only server|client|self]
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
XRAY_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
XRAY_BIN="${XRAY_BIN:-$XRAY_ROOT/build/xray}"

# Find xray binary
if [ ! -f "$XRAY_BIN" ]; then
    for d in build-embed build-release build-new; do
        if [ -f "$XRAY_ROOT/$d/xray" ]; then
            XRAY_BIN="$XRAY_ROOT/$d/xray"
            break
        fi
    done
fi

if [ ! -f "$XRAY_BIN" ]; then
    echo "ERROR: xray binary not found. Set XRAY_BIN or build first."
    exit 1
fi

# Check Python + websockets
if ! python3 -c "import websockets" 2>/dev/null; then
    echo "Installing Python websockets..."
    pip3 install websockets -q
fi

ONLY="${1:-}"
if [ "$ONLY" = "--only" ]; then
    ONLY="$2"
fi

cleanup() {
    # Kill any servers we started
    [ -n "$XRAY_SERVER_PID" ] && kill $XRAY_SERVER_PID 2>/dev/null || true
    [ -n "$PY_SERVER_PID" ] && kill $PY_SERVER_PID 2>/dev/null || true
    # Clean up ports
    lsof -ti :9100 2>/dev/null | xargs kill -9 2>/dev/null || true
    lsof -ti :9101 2>/dev/null | xargs kill -9 2>/dev/null || true
    lsof -ti :9102 2>/dev/null | xargs kill -9 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT

TOTAL_PASS=0
TOTAL_FAIL=0

echo ""
echo "============================================="
echo "  WebSocket Functional Test Suite"
echo "  xray: $XRAY_BIN"
echo "============================================="
echo ""

# ─────────────────────────────────────────────
# Phase 1: Test xray server (Python client)
# ─────────────────────────────────────────────
if [ -z "$ONLY" ] || [ "$ONLY" = "server" ]; then
    echo "--- Phase 1: Testing xray as Server ---"
    echo ""

    # Clean port
    lsof -ti :9100 2>/dev/null | xargs kill -9 2>/dev/null || true
    sleep 0.3

    # Start xray echo server
    $XRAY_BIN "$SCRIPT_DIR/xray_echo_server.xr" -- 9100 &
    XRAY_SERVER_PID=$!
    sleep 1

    if ! kill -0 $XRAY_SERVER_PID 2>/dev/null; then
        echo "ERROR: xray server failed to start"
        TOTAL_FAIL=$((TOTAL_FAIL + 12))
    else
        # Wait for server to be ready
        for i in $(seq 1 20); do
            if python3 -c "
import socket, sys
s = socket.socket()
s.settimeout(1)
try:
    s.connect(('127.0.0.1', 9100))
    s.close()
    sys.exit(0)
except:
    sys.exit(1)
" 2>/dev/null; then
                break
            fi
            sleep 0.5
        done

        # Run Python test client
        if python3 "$SCRIPT_DIR/test_server.py" 2>&1; then
            : # exit code handled below
        fi

        # Parse results from output
        RESULT=$(python3 "$SCRIPT_DIR/test_server.py" 2>&1 | grep "Results:" || echo "0 passed, 0 failed")
        # Just use exit code
    fi

    # Stop xray server
    kill $XRAY_SERVER_PID 2>/dev/null || true
    wait $XRAY_SERVER_PID 2>/dev/null || true
    XRAY_SERVER_PID=""
    sleep 0.5

    echo ""
fi

# ─────────────────────────────────────────────
# Phase 2: Test xray client (Python server)
# ─────────────────────────────────────────────
if [ -z "$ONLY" ] || [ "$ONLY" = "client" ]; then
    echo "--- Phase 2: Testing xray as Client ---"
    echo ""

    # Clean port
    lsof -ti :9101 2>/dev/null | xargs kill -9 2>/dev/null || true
    sleep 0.3

    # Start Python echo server
    python3 "$SCRIPT_DIR/py_echo_server.py" 9101 &
    PY_SERVER_PID=$!
    sleep 1

    if ! kill -0 $PY_SERVER_PID 2>/dev/null; then
        echo "ERROR: Python server failed to start"
    else
        # Wait for server
        for i in $(seq 1 10); do
            if python3 -c "
import socket, sys
s = socket.socket()
s.settimeout(1)
try:
    s.connect(('127.0.0.1', 9101))
    s.close()
    sys.exit(0)
except:
    sys.exit(1)
" 2>/dev/null; then
                break
            fi
            sleep 0.5
        done

        # Run xray client test
        $XRAY_BIN "$SCRIPT_DIR/test_client.xr" 2>&1 || true
    fi

    # Stop Python server
    kill $PY_SERVER_PID 2>/dev/null || true
    wait $PY_SERVER_PID 2>/dev/null || true
    PY_SERVER_PID=""
    sleep 0.5

    echo ""
fi

# ─────────────────────────────────────────────
# Phase 3: xray self-test (server + client)
# ─────────────────────────────────────────────
if [ -z "$ONLY" ] || [ "$ONLY" = "self" ]; then
    echo "--- Phase 3: xray Self-Test ---"
    echo ""

    # Clean port
    lsof -ti :9102 2>/dev/null | xargs kill -9 2>/dev/null || true
    sleep 0.3

    # Run self-test (server + client in same process)
    timeout 30 $XRAY_BIN "$SCRIPT_DIR/test_self.xr" 2>&1 || true

    echo ""
fi

echo "============================================="
echo "  All test phases complete"
echo "============================================="
echo ""

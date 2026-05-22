#!/bin/bash
# HTTP Benchmark: xray vs Go vs Node.js
# Usage: bash run_bench_simple.sh

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
XRAY="$DIR/../../../build/xray"
if [ ! -f "$XRAY" ] && [ -f "$DIR/../../../build/xray.exe" ]; then
    XRAY="$DIR/../../../build/xray.exe"
fi
WRK_THREADS=4
WRK_CONNS=100
WRK_DURATION=10s

echo "============================================"
echo "  HTTP Benchmark: xray vs Go vs Node.js"
echo "  wrk -t${WRK_THREADS} -c${WRK_CONNS} -d${WRK_DURATION}"
echo "  Platform: $(uname -m) / $(uname -s)"
echo "============================================"
echo ""

cleanup() {
    kill $XRAY_PID $GO_PID $NODE_PID 2>/dev/null || true
    wait $XRAY_PID $GO_PID $NODE_PID 2>/dev/null || true
}
trap cleanup EXIT

# --- Build Go server ---
echo "[1/6] Building Go server..."
cd "$DIR"
go build -o /tmp/bench_http_go bench_http_simple.go

# --- Start all servers ---
echo "[2/6] Starting servers..."

$XRAY "$DIR/bench_http_simple.xr" &
XRAY_PID=$!

/tmp/bench_http_go &
GO_PID=$!

node "$DIR/bench_http_simple.js" &
NODE_PID=$!

sleep 2

# Verify servers are up
curl -s http://127.0.0.1:8080/ > /dev/null || { echo "xray server failed to start"; exit 1; }
curl -s http://127.0.0.1:8081/ > /dev/null || { echo "Go server failed to start"; exit 1; }
curl -s http://127.0.0.1:8082/ > /dev/null || { echo "Node.js server failed to start"; exit 1; }
echo "All servers running."
echo ""

# --- Plaintext benchmark ---
echo "========== Plaintext: GET / =========="
echo ""

echo "--- xray (port 8080) ---"
wrk -t${WRK_THREADS} -c${WRK_CONNS} -d${WRK_DURATION} http://127.0.0.1:8080/
echo ""

echo "--- Go (port 8081) ---"
wrk -t${WRK_THREADS} -c${WRK_CONNS} -d${WRK_DURATION} http://127.0.0.1:8081/
echo ""

echo "--- Node.js (port 8082) ---"
wrk -t${WRK_THREADS} -c${WRK_CONNS} -d${WRK_DURATION} http://127.0.0.1:8082/
echo ""

# --- JSON benchmark ---
echo "========== JSON: GET /json =========="
echo ""

echo "--- xray (port 8080) ---"
wrk -t${WRK_THREADS} -c${WRK_CONNS} -d${WRK_DURATION} http://127.0.0.1:8080/json
echo ""

echo "--- Go (port 8081) ---"
wrk -t${WRK_THREADS} -c${WRK_CONNS} -d${WRK_DURATION} http://127.0.0.1:8081/json
echo ""

echo "--- Node.js (port 8082) ---"
wrk -t${WRK_THREADS} -c${WRK_CONNS} -d${WRK_DURATION} http://127.0.0.1:8082/json
echo ""

echo "============================================"
echo "  Benchmark complete."
echo "============================================"

#!/bin/bash
# Fibonacci Benchmark: xray (AOT-C/JIT/interp) vs Go vs LuaJIT vs Node.js(V8) vs Dart
#
# Usage: bash tests/jit_benchmark/run_fib.sh
# Run from project root directory

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../.." && pwd)"

# Auto-detect xray binary: prefer release, then debug
XRAY=""
for candidate in "$ROOT/build-release/xray" "$ROOT/build/xray" "$ROOT/build-debug/xray"; do
    if [ -x "$candidate" ]; then
        XRAY="$candidate"
        break
    fi
done
if [ -z "$XRAY" ]; then
    echo "ERROR: xray binary not found. Build the project first."
    exit 1
fi

EXPECTED=102334155  # fib(40)

echo "============================================"
echo "  Fibonacci(40) Benchmark"
echo "  Platform: $(uname -m) / $(uname -s)"
echo "  xray binary: $XRAY"
echo "============================================"
echo ""

run_bench() {
    local name="$1"
    shift
    printf "%-20s" "$name"

    # Verify correctness
    local output
    output=$("$@" 2>/dev/null | tail -1)
    if [ "$output" != "$EXPECTED" ]; then
        echo "FAIL (got: $output, expected: $EXPECTED)"
        return
    fi

    # Run 3 times, take best
    local best=999.0
    for i in 1 2 3; do
        local t
        t=$( { time "$@" > /dev/null 2>&1; } 2>&1 | grep real | awk '{print $2}' )
        # Parse time format: 0m0.123s -> seconds
        local secs
        secs=$(echo "$t" | sed 's/^0m//;s/s$//')
        if (( $(echo "$secs < $best" | bc -l) )); then
            best=$secs
        fi
    done
    printf "%8ss\n" "$best"
}

# xray: AOT compile (transpile to C, then cc -O2)
echo "Compiling xray (AOT → C)..."
$XRAY build "$DIR/fib.xr" --native -o /tmp/fib_xray_aot 2>/dev/null

# Go: compile first, then run binary
echo "Compiling Go..."
go build -o /tmp/fib_go "$DIR/fib.go" 2>/dev/null

# Dart: compile AOT first, then run binary
echo "Compiling Dart (AOT)..."
dart compile exe "$DIR/fib.dart" -o /tmp/fib_dart 2>/dev/null
echo ""

echo "--- Results (best of 3 runs) ---"
echo ""
run_bench "xray (AOT→C)"      /tmp/fib_xray_aot 40
run_bench "xray (JIT)"        $XRAY "$DIR/fib.xr"
run_bench "Go (compiled)"     /tmp/fib_go
run_bench "Dart (AOT)"        /tmp/fib_dart
run_bench "LuaJIT"            luajit "$DIR/fib.lua"
run_bench "Node.js (V8)"      node "$DIR/fib.js"
run_bench "xray (interp)"     $XRAY "$DIR/fib_nojit.xr"

echo ""
echo "--- Done ---"

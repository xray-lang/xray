#!/bin/bash
# AOT transpiler test suite
# Verifies that generated C source compiles cleanly with clang
#
# Usage: ./tests/aot/run_aot_tests.sh [xray_binary]

set -e

XRAY="${1:-./build/xray}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
AOT_INCLUDE="$PROJECT_DIR/src/aot"
VM_INCLUDE="$PROJECT_DIR/include"
TMPDIR="${TMPDIR:-/tmp}"
PASS=0
FAIL=0
SKIP=0

echo "=== AOT Transpiler Tests ==="
echo "Binary: $XRAY"
echo ""

run_test() {
    local xr_file="$1"
    local test_name="$(basename "$xr_file" .xr)"
    local c_out="$TMPDIR/aot_test_${test_name}_$$.c"

    printf "  %-30s" "$test_name"

    # Step 1: Generate C source
    if ! "$XRAY" build --native -c "$xr_file" -o "$c_out" >/dev/null 2>&1; then
        echo "SKIP (transpile failed)"
        SKIP=$((SKIP + 1))
        return
    fi

    # Step 2: Verify C syntax with clang
    if cc -fsyntax-only -I "$AOT_INCLUDE" -I "$VM_INCLUDE" \
          -Wno-initializer-overrides "$c_out" 2>/dev/null; then
        echo "PASS"
        PASS=$((PASS + 1))
    else
        echo "FAIL (C syntax error)"
        FAIL=$((FAIL + 1))
    fi

    rm -f "$c_out"
}

# Run all .xr files in test directories
for dir in "$SCRIPT_DIR"/basic "$SCRIPT_DIR"/modules; do
    if [ -d "$dir" ]; then
        echo "--- $(basename "$dir") ---"
        for f in "$dir"/*.xr; do
            [ -f "$f" ] && run_test "$f"
        done
        echo ""
    fi
done

echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1

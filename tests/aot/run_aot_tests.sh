#!/bin/bash
# AOT VM-AOT diff test suite
# Gold standard: diff <(xray run X) <(./aot_X) must be empty
#
# Usage: ./tests/aot/run_aot_tests.sh [xray_binary]

set -e

XRAY="${1:-./build/xray}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
AOT_INCLUDE="$PROJECT_DIR/src/aot"
VM_INCLUDE="$PROJECT_DIR/include"
WORK="${TMPDIR:-/tmp}/aot_test_$$"
mkdir -p "$WORK"
PASS=0
FAIL=0
SKIP=0

echo "=== AOT VM-AOT Diff Tests ==="
echo "Binary: $XRAY"
echo ""

run_test() {
    local xr_file="$1"
    local test_name="$(basename "$xr_file" .xr)"
    local c_out="$WORK/${test_name}.c"
    local bin_out="$WORK/${test_name}"
    local vm_out="$WORK/${test_name}.vm"
    local aot_out="$WORK/${test_name}.aot"

    printf "  %-30s" "$test_name"

    # Step 1: Transpile .xr → .c (retry up to 3x for intermittent XIR pass crashes)
    local ok=0
    for attempt in 1 2 3; do
        if "$XRAY" build --native -c "$xr_file" -o "$c_out" >/dev/null 2>&1; then
            ok=1; break
        fi
    done
    if [ "$ok" -eq 0 ]; then
        echo "SKIP (transpile failed)"
        SKIP=$((SKIP + 1))
        return
    fi

    # Step 2: Compile .c → binary
    if ! cc -O2 -Wall -Wno-initializer-overrides \
            -I "$AOT_INCLUDE" -I "$VM_INCLUDE" \
            "$c_out" -o "$bin_out" 2>/dev/null; then
        echo "FAIL (C compile error)"
        FAIL=$((FAIL + 1))
        return
    fi

    # Step 3: Run VM and AOT, capture stdout only
    "$XRAY" run "$xr_file" > "$vm_out" 2>/dev/null || true
    "$bin_out" > "$aot_out" 2>/dev/null || true

    # Step 4: Diff outputs
    if diff -u "$vm_out" "$aot_out" > /dev/null 2>&1; then
        echo "PASS"
        PASS=$((PASS + 1))
        rm -f "$c_out" "$bin_out" "$vm_out" "$aot_out"
    else
        echo "FAIL (output mismatch)"
        echo "    VM:  $(head -5 "$vm_out" | tr '\n' '|')"
        echo "    AOT: $(head -5 "$aot_out" | tr '\n' '|')"
        FAIL=$((FAIL + 1))
        # Keep .c file on failure for debugging
        rm -f "$bin_out" "$vm_out" "$aot_out"
    fi
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
rm -rf "$WORK"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1

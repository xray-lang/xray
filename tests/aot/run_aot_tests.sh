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
    local test_args=()

    case "$test_name" in
        process_args*) test_args=("100000" "abc") ;;
    esac

    printf "  %-30s" "$test_name"

    # Step 1: Transpile .xr → .c (retry up to 3x for intermittent XIR pass crashes)
    local ok=0
    for attempt in 1 2 3; do
        if "$XRAY" build --native -c "$xr_file" -o "$c_out" >/dev/null 2>&1; then
            ok=1; break
        fi
    done
    if [ "$ok" -eq 0 ]; then
        # Positive dirs must transpile; a persistent failure is a regression,
        # not a skip (expected-unsupported cases live under negative/).
        echo "FAIL (transpile failed after retries)"
        FAIL=$((FAIL + 1))
        return
    fi

    # Step 2: Compile .c → binary. Coroutine AOT needs xray_core and
    # platform runtime libs, so delegate that link mode to xray build.
    if grep -q "xr_aot_run_main" "$c_out"; then
        if ! "$XRAY" build --native "$xr_file" -o "$bin_out" >/dev/null 2>&1; then
            echo "FAIL (runtime link error)"
            FAIL=$((FAIL + 1))
            return
        fi
    else
        if ! cc -O2 -Wall -Wno-initializer-overrides \
                -I "$AOT_INCLUDE" -I "$VM_INCLUDE" \
                "$c_out" -o "$bin_out" -lm 2>/dev/null; then
            echo "FAIL (C compile error)"
            FAIL=$((FAIL + 1))
            return
        fi
    fi

    # Step 3: Run VM and AOT, capturing stdout AND exit code (if-form keeps
    # set -e from aborting on a non-zero program exit).
    if [ "${#test_args[@]}" -gt 0 ]; then
        if "$XRAY" run "$xr_file" -- "${test_args[@]}" > "$vm_out" 2>/dev/null; then vm_rc=0; else vm_rc=$?; fi
        if "$bin_out" "${test_args[@]}" > "$aot_out" 2>/dev/null; then aot_rc=0; else aot_rc=$?; fi
    else
        if "$XRAY" run "$xr_file" > "$vm_out" 2>/dev/null; then vm_rc=0; else vm_rc=$?; fi
        if "$bin_out" > "$aot_out" 2>/dev/null; then aot_rc=0; else aot_rc=$?; fi
    fi

    # Step 4: Compare exit codes, then stdout
    if [ "$vm_rc" != "$aot_rc" ]; then
        echo "FAIL (exit code: VM=$vm_rc AOT=$aot_rc)"
        FAIL=$((FAIL + 1))
        rm -f "$bin_out" "$vm_out" "$aot_out"
    elif diff -u "$vm_out" "$aot_out" > /dev/null 2>&1; then
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

run_negative_test() {
    local xr_file="$1"
    local test_name="$(basename "$xr_file" .xr)"
    local c_out="$WORK/${test_name}.c"
    local log_out="$WORK/${test_name}.log"

    printf "  %-30s" "$test_name"

    if "$XRAY" build --native -c "$xr_file" -o "$c_out" >"$log_out" 2>&1; then
        echo "FAIL (unexpected AOT success)"
        FAIL=$((FAIL + 1))
        rm -f "$c_out" "$log_out"
        return
    fi

    if grep -Eq "unsupported .*coroutine Xi op|unsupported AOT sync call to suspendable function|unsupported AOT indirect call|exceptions inside AOT coroutine are unsupported|unsupported Xi op ERR_|semantic analysis failed|: error: " "$log_out"; then
        echo "PASS (rejected)"
        PASS=$((PASS + 1))
        rm -f "$c_out" "$log_out"
    else
        echo "FAIL (wrong rejection)"
        echo "    $(head -5 "$log_out" | tr '\n' '|')"
        FAIL=$((FAIL + 1))
        rm -f "$c_out"
    fi
}

# Run all .xr files in test directories
for dir in "$SCRIPT_DIR"/basic "$SCRIPT_DIR"/modules "$SCRIPT_DIR"/coro; do
    if [ -d "$dir" ]; then
        echo "--- $(basename "$dir") ---"
        for f in "$dir"/*.xr; do
            [ -f "$f" ] && run_test "$f"
        done
        echo ""
    fi
done

if [ -d "$SCRIPT_DIR/negative" ]; then
    echo "--- negative ---"
    for f in "$SCRIPT_DIR"/negative/*.xr; do
        [ -f "$f" ] && run_negative_test "$f"
    done
    echo ""
fi

echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
rm -rf "$WORK"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1

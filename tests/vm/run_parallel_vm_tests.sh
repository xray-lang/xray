#!/usr/bin/env bash
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
XRAY="${1:-${XRAY_BIN:-$PROJECT_DIR/build/xray}}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_parallel_vm.XXXXXX")" || exit 1
PASS=0
FAIL=0

cleanup() {
    rm -rf "$WORK"
}
trap cleanup EXIT

record_pass() {
    printf '  PASS: %s\n' "$1"
    PASS=$((PASS + 1))
}

record_fail() {
    printf '  FAIL: %s\n' "$1"
    FAIL=$((FAIL + 1))
}

expect_output_workers() {
    local name="$1"
    local src="$2"
    local expected="$3"
    local workers="$4"
    local out="$WORK/$name.out"
    local err="$WORK/$name.err"

    if "$XRAY" run --workers "$workers" "$src" >"$out" 2>"$err" &&
        [ "$(cat "$out")" = "$expected" ] && [ ! -s "$err" ]; then
        record_pass "$name output"
    else
        record_fail "$name output"
        sed 's/^/      stdout: /' "$out" | sed -n '1,40p'
        sed 's/^/      stderr: /' "$err" | sed -n '1,40p'
    fi
}

expect_output_workers_env() {
    local name="$1"
    local src="$2"
    local expected="$3"
    local workers="$4"
    shift 4
    local out="$WORK/$name.out"
    local err="$WORK/$name.err"

    if env "$@" "$XRAY" run --workers "$workers" "$src" >"$out" 2>"$err" &&
        [ "$(cat "$out")" = "$expected" ] && [ ! -s "$err" ]; then
        record_pass "$name output"
    else
        record_fail "$name output"
        sed 's/^/      stdout: /' "$out" | sed -n '1,40p'
        sed 's/^/      stderr: /' "$err" | sed -n '1,40p'
    fi
}

printf '=== VM parallel Tests ===\n'
printf 'Binary: %s\n\n' "$XRAY"

if [ ! -x "$XRAY" ]; then
    printf 'FAIL: xray binary not executable: %s\n' "$XRAY" >&2
    exit 1
fi

expect_output_workers \
    "parallel_vm_multilane_threadlocal" \
    "$PROJECT_DIR/tests/vm/parallel_vm_multilane_threadlocal.xr" \
    $'true\ntrue\ntrue' \
    4

expect_output_workers \
    "parallel_shadowing_no_intrinsic" \
    "$PROJECT_DIR/tests/vm/parallel_shadowing_no_intrinsic.xr" \
    "3" \
    1

expect_output_workers_env \
    "parallel_vm_deterministic_single_lane" \
    "$PROJECT_DIR/tests/vm/parallel_vm_deterministic_single_lane.xr" \
    $'true\n1' \
    4 \
    XRAY_CORO_DETERMINISTIC=1

expect_output_workers \
    "parallel_vm_small_range_single_lane" \
    "$PROJECT_DIR/tests/vm/parallel_vm_small_range_single_lane.xr" \
    $'1\n1' \
    4

printf '\nSummary: %d passed, %d failed\n' "$PASS" "$FAIL"
if [ "$FAIL" -ne 0 ]; then
    exit 1
fi

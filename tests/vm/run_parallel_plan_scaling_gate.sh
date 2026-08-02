#!/usr/bin/env bash
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
XRAY="${1:-${XRAY_BIN:-$PROJECT_DIR/build/xray}}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_parallel_plan_scaling.XXXXXX")" || exit 1
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
    local err_effective="$err"
    local actual

    "$XRAY" run --workers "$workers" "$src" >"$out" 2>"$err"
    local rc=$?

    # This gate validates lane scaling and exact observable results, not the
    # scheduler watchdog.  A correct lane can legitimately spend >100 ms in a
    # non-yielding CPU callback on a loaded or sanitizer host and trigger the
    # warn-only sysmon diagnostic even though every lane completes.  Drop only
    # that diagnostic family; any other stderr output remains a hard failure.
    # Dedicated scheduler tests retain strict sysmon coverage.
    err_effective="$WORK/$name.err.filtered"
    grep -v '\[sysmon\]' "$err" >"$err_effective" 2>/dev/null || true
    # The Windows console provider currently writes CRLF while Unix providers
    # write LF.  This gate compares logical output lines; task 257 owns the
    # process/console codec convergence itself.
    actual="$(tr -d '\r' <"$out")"

    if [ "$rc" -eq 0 ] &&
        [ "$actual" = "$expected" ] && [ ! -s "$err_effective" ]; then
        record_pass "$name output"
    else
        record_fail "$name output"
        sed 's/^/      stdout: /' "$out" | sed -n '1,40p'
        sed 's/^/      stderr: /' "$err" | sed -n '1,40p'
    fi
}

printf '=== Parallel Plan VM Scaling Gate ===\n'
printf 'Binary: %s\n\n' "$XRAY"

if [ ! -x "$XRAY" ]; then
    printf 'FAIL: xray binary not executable: %s\n' "$XRAY" >&2
    exit 1
fi

expect_output_workers \
    "parallel_plan_vm_scaling_baseline" \
    "$PROJECT_DIR/tests/vm/parallel_plan_vm_scaling_baseline.xr" \
    $'true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue' \
    8

printf '\nSummary: %d passed, %d failed\n' "$PASS" "$FAIL"
if [ "$FAIL" -ne 0 ]; then
    exit 1
fi

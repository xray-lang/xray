#!/usr/bin/env bash
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
XRAY="${1:-${XRAY_BIN:-$PROJECT_DIR/build/xray}}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_sys_process_lifecycle.XXXXXX")" || exit 1
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

expect_output() {
    local name="$1"
    local src="$2"
    local expected="$3"
    local out="$WORK/$name.out"
    local err="$WORK/$name.err"

    if "$XRAY" run "$src" >"$out" 2>"$err" && [ "$(cat "$out")" = "$expected" ] &&
        [ ! -s "$err" ]; then
        record_pass "$name output"
    else
        record_fail "$name output"
        sed 's/^/      stdout: /' "$out" | sed -n '1,40p'
        sed 's/^/      stderr: /' "$err" | sed -n '1,80p'
    fi
}

expect_warning() {
    local name="$1"
    local src="$2"
    local expected="$3"
    local needle="$4"
    local out="$WORK/$name.out"
    local err="$WORK/$name.err"

    if "$XRAY" run "$src" >"$out" 2>"$err" && [ "$(cat "$out")" = "$expected" ] &&
        grep -Fq "$needle" "$err"; then
        record_pass "$name warning"
    else
        record_fail "$name warning"
        sed 's/^/      stdout: /' "$out" | sed -n '1,40p'
        sed 's/^/      stderr: /' "$err" | sed -n '1,80p'
    fi
}

printf '=== VM sys.Process/Pipe Lifecycle Tests ===\n'
printf 'Binary: %s\n\n' "$XRAY"

if [ ! -x "$XRAY" ]; then
    printf 'FAIL: xray binary not executable: %s\n' "$XRAY" >&2
    exit 1
fi

OK_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_ok.xr"
PROCESS_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_warning.xr"
PIPE_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_pipe_lifecycle_warning.xr"
PIPE_HALF_CLOSE_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_pipe_lifecycle_half_close_warning.xr"
PROCESS_TRYWAIT_ONCE_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_trywait_once_warning.xr"
CONTROL_FLOW_OK_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_control_flow_ok.xr"
PROCESS_TRY_CATCH_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_try_catch_warning.xr"
PIPE_MATCH_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_pipe_lifecycle_match_warning.xr"

expect_output "process_pipe_lifecycle_ok" "$OK_SRC" $'0\ntrue\ntrue\ntrue\ntrue'
expect_output "process_pipe_lifecycle_control_flow_ok" "$CONTROL_FLOW_OK_SRC" $'0\n0\n0\n0\ntrue\ntrue'
expect_warning "process_lifecycle_warning" "$PROCESS_WARNING_SRC" "" \
    "Process handle 'p' from sys.Process.spawn is not waited before leaving scope"
expect_warning "process_lifecycle_trywait_once_warning" "$PROCESS_TRYWAIT_ONCE_WARNING_SRC" "" \
    "Process handle 'p' from sys.Process.spawn is not waited before leaving scope"
expect_warning "process_lifecycle_try_catch_warning" "$PROCESS_TRY_CATCH_WARNING_SRC" \
    $'0\ntry-catch-open' \
    "Process handle 'p' from sys.Process.spawn is not waited before leaving scope"
expect_warning "pipe_lifecycle_warning" "$PIPE_WARNING_SRC" "pipe-open" \
    "Pipe handle 'pipe' from sys.Pipe.open is not closed before leaving scope"
expect_warning "pipe_lifecycle_half_close_warning" "$PIPE_HALF_CLOSE_WARNING_SRC" "true" \
    "Pipe handle 'pipe' from sys.Pipe.open is not closed before leaving scope"
expect_warning "pipe_lifecycle_match_warning" "$PIPE_MATCH_WARNING_SRC" \
    $'pipe-match-open\nafter-match' \
    "Pipe handle 'pipe' from sys.Pipe.open is not closed before leaving scope"

printf '\nPassed: %d\nFailed: %d\n' "$PASS" "$FAIL"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi

#!/usr/bin/env bash
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
XRAY="${1:-${XRAY_BIN:-$PROJECT_DIR/build/xray}}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_sys_thread_vm.XXXXXX")" || exit 1
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

    if "$XRAY" run "$src" >"$out" 2>"$err" && [ "$(cat "$out")" = "$expected" ]; then
        record_pass "$name output"
    else
        record_fail "$name output"
        sed 's/^/      stdout: /' "$out" | sed -n '1,40p'
        sed 's/^/      stderr: /' "$err" | sed -n '1,40p'
    fi
}

printf '=== VM sys.Thread Tests ===\n'
printf 'Binary: %s\n\n' "$XRAY"

if [ ! -x "$XRAY" ]; then
    printf 'FAIL: xray binary not executable: %s\n' "$XRAY" >&2
    exit 1
fi

JOIN_SRC="$PROJECT_DIR/tests/vm/sys_thread_spawn_join.xr"
DETACH_SRC="$PROJECT_DIR/tests/vm/sys_thread_detach.xr"
ARRAY_SRC="$PROJECT_DIR/tests/vm/sys_thread_join_array.xr"
OPTIONS_SRC="$PROJECT_DIR/tests/vm/sys_thread_spawn_options.xr"

expect_output "spawn_join" "$JOIN_SRC" "42"
expect_output "detach" "$DETACH_SRC" "detached"
expect_output "join_array" "$ARRAY_SRC" "42"
expect_output "spawn_options" "$OPTIONS_SRC" "42"

"$XRAY" run --dump-bytecode "$JOIN_SRC" >"$WORK/join.dump" 2>"$WORK/join.dump.err"
if grep -Eq '^[0-9]+.*[[:space:]]THREAD_SPAWN[[:space:]]' "$WORK/join.dump"; then
    record_pass "spawn_join bytecode uses THREAD_SPAWN"
else
    record_fail "spawn_join bytecode uses THREAD_SPAWN"
    sed 's/^/      /' "$WORK/join.dump" | sed -n '1,120p'
    sed 's/^/      stderr: /' "$WORK/join.dump.err" | sed -n '1,40p'
fi

printf '\nPassed: %d\nFailed: %d\n' "$PASS" "$FAIL"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi

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

    if "$XRAY" run "$src" >"$out" 2>"$err" && [ "$(cat "$out")" = "$expected" ] &&
        [ ! -s "$err" ]; then
        record_pass "$name output"
    else
        record_fail "$name output"
        sed 's/^/      stdout: /' "$out" | sed -n '1,40p'
        sed 's/^/      stderr: /' "$err" | sed -n '1,40p'
    fi
}

expect_warning() {
    local name="$1"
    local src="$2"
    local expected="$3"
    local needle="$4"
    local needle2="${5:-}"
    local out="$WORK/$name.out"
    local err="$WORK/$name.err"

    if "$XRAY" run "$src" >"$out" 2>"$err" && [ "$(cat "$out")" = "$expected" ] &&
        grep -Fq "$needle" "$err" &&
        { [ -z "$needle2" ] || grep -Fq "$needle2" "$err"; }; then
        record_pass "$name warning"
    else
        record_fail "$name warning"
        sed 's/^/      stdout: /' "$out" | sed -n '1,40p'
        sed 's/^/      stderr: /' "$err" | sed -n '1,80p'
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
ORPHAN_SRC="$PROJECT_DIR/tests/vm/sys_thread_orphan_warning.xr"
UNUSED_LOCAL_SRC="$PROJECT_DIR/tests/vm/sys_thread_unused_local_warning.xr"
DONE_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_thread_lifecycle_done_warning.xr"
ALIAS_JOIN_SRC="$PROJECT_DIR/tests/vm/sys_thread_alias_join.xr"
RETURN_TRANSFER_SRC="$PROJECT_DIR/tests/vm/sys_thread_return_transfer.xr"
BRANCH_JOIN_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_thread_branch_join_warning.xr"
BRANCH_BOTH_CLOSE_SRC="$PROJECT_DIR/tests/vm/sys_thread_branch_both_close.xr"
TRY_CATCH_BOTH_CLOSE_SRC="$PROJECT_DIR/tests/vm/sys_thread_try_catch_both_close.xr"
TRY_CATCH_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_thread_try_catch_warning.xr"
MATCH_ALL_CLOSE_SRC="$PROJECT_DIR/tests/vm/sys_thread_match_all_close.xr"
MATCH_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_thread_match_warning.xr"
SELECT_ALL_CLOSE_SRC="$PROJECT_DIR/tests/vm/sys_thread_select_all_close.xr"
SELECT_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_thread_select_warning.xr"
DESTRUCTURE_JOIN_SRC="$PROJECT_DIR/tests/vm/sys_thread_destructure_join.xr"

expect_output "spawn_join" "$JOIN_SRC" "42"
for i in 1 2 3 4 5 6 7 8 9 10; do
    expect_output "detach_$i" "$DETACH_SRC" "detached"
done
expect_output "join_array" "$ARRAY_SRC" "42"
expect_output "spawn_options" "$OPTIONS_SRC" "42"
expect_output "alias_join" "$ALIAS_JOIN_SRC" "42"
expect_output "return_transfer" "$RETURN_TRANSFER_SRC" "42"
expect_output "branch_both_close" "$BRANCH_BOTH_CLOSE_SRC" "42"
expect_output "try_catch_both_close" "$TRY_CATCH_BOTH_CLOSE_SRC" "42"
expect_output "match_all_close" "$MATCH_ALL_CLOSE_SRC" "42"
expect_output "select_all_close" "$SELECT_ALL_CLOSE_SRC" "42"
expect_output "destructure_join" "$DESTRUCTURE_JOIN_SRC" "42"
expect_warning "orphan" "$ORPHAN_SRC" "orphan" \
    "sys.Thread.spawn returns a Thread handle; call join() or detach() explicitly"
expect_warning "unused_local" "$UNUSED_LOCAL_SRC" "unused-local" \
    "Thread handle 't' from sys.Thread.spawn is never used" \
    "call join() or detach() explicitly"
expect_warning "done_warning" "$DONE_WARNING_SRC" "done-check" \
    "Thread handle 't' from sys.Thread.spawn is not joined or detached before leaving scope"
expect_warning "branch_join_warning" "$BRANCH_JOIN_WARNING_SRC" "conditional-join" \
    "Thread handle 't' from sys.Thread.spawn is not joined or detached before leaving scope"
expect_warning "try_catch_warning" "$TRY_CATCH_WARNING_SRC" $'42\ntry-catch-open' \
    "Thread handle 't' from sys.Thread.spawn is not joined or detached before leaving scope"
expect_warning "match_warning" "$MATCH_WARNING_SRC" $'match-open\nafter-match' \
    "Thread handle 't' from sys.Thread.spawn is not joined or detached before leaving scope"
expect_warning "select_warning" "$SELECT_WARNING_SRC" $'select-open\nafter-select' \
    "Thread handle 't' from sys.Thread.spawn is not joined or detached before leaving scope"

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

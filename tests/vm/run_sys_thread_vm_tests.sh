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

expect_output_workers() {
    local name="$1"
    local src="$2"
    local expected="$3"
    local workers="$4"
    local out="$WORK/$name.out"
    local err="$WORK/$name.err"

    if "$XRAY" run --workers "$workers" "$src" >"$out" 2>"$err" && [ "$(cat "$out")" = "$expected" ] &&
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
BRANCH_ALIAS_BOTH_CLOSE_SRC="$PROJECT_DIR/tests/vm/sys_thread_branch_alias_both_close.xr"
TRY_CATCH_BOTH_CLOSE_SRC="$PROJECT_DIR/tests/vm/sys_thread_try_catch_both_close.xr"
TRY_CATCH_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_thread_try_catch_warning.xr"
MATCH_ALL_CLOSE_SRC="$PROJECT_DIR/tests/vm/sys_thread_match_all_close.xr"
MATCH_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_thread_match_warning.xr"
SELECT_ALL_CLOSE_SRC="$PROJECT_DIR/tests/vm/sys_thread_select_all_close.xr"
SELECT_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_thread_select_warning.xr"
TERNARY_BOTH_CLOSE_SRC="$PROJECT_DIR/tests/vm/sys_thread_ternary_both_close.xr"
TERNARY_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_thread_ternary_warning.xr"
DESTRUCTURE_JOIN_SRC="$PROJECT_DIR/tests/vm/sys_thread_destructure_join.xr"
DESTRUCTURE_ALIAS_JOIN_SRC="$PROJECT_DIR/tests/vm/sys_thread_destructure_alias_join.xr"
ASSIGNMENT_ALIAS_JOIN_SRC="$PROJECT_DIR/tests/vm/sys_thread_assignment_alias_join.xr"
LOOP_JOIN_SRC="$PROJECT_DIR/tests/vm/sys_thread_loop_join.xr"
LOOP_JOIN_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_thread_loop_join_warning.xr"
FOR_LOOP_JOIN_SRC="$PROJECT_DIR/tests/vm/sys_thread_for_loop_join.xr"
LOOP_NESTED_CONTINUE_JOIN_SRC="$PROJECT_DIR/tests/vm/sys_thread_loop_nested_continue_join.xr"
LOOP_CONTINUE_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_thread_loop_continue_warning.xr"
LOOP_TRY_CONTINUE_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_thread_loop_try_continue_warning.xr"
TEMPLATE_JOIN_SRC="$PROJECT_DIR/tests/vm/sys_thread_template_join.xr"
SLICE_BOUND_JOIN_SRC="$PROJECT_DIR/tests/vm/sys_thread_slice_bound_join.xr"
CHANNEL_CAPACITY_JOIN_SRC="$PROJECT_DIR/tests/vm/sys_thread_channel_capacity_join.xr"
UNSAFE_JOIN_SRC="$PROJECT_DIR/tests/vm/sys_thread_unsafe_join.xr"
THREADLOCAL_BASIC_SRC="$PROJECT_DIR/tests/vm/sys_threadlocal_basic.xr"
YIELDABLE_JOIN_SRC="$PROJECT_DIR/tests/vm/sys_thread_yieldable_join.xr"
DEFER_JOIN_SRC="$PROJECT_DIR/tests/vm/sys_thread_defer_join.xr"
HELPER_JOIN_SRC="$PROJECT_DIR/tests/vm/sys_thread_lifecycle_helper_join.xr"
FN_VALUE_JOIN_SRC="$PROJECT_DIR/tests/vm/sys_thread_lifecycle_fn_value_join.xr"
TOP_CONST_FN_VALUE_JOIN_SRC="$PROJECT_DIR/tests/vm/sys_thread_lifecycle_top_const_fn_value_join.xr"
HELPER_EARLY_RETURN_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_thread_lifecycle_helper_early_return_warning.xr"

expect_output "spawn_join" "$JOIN_SRC" "42"
for i in 1 2 3 4 5 6 7 8 9 10; do
    expect_output "detach_$i" "$DETACH_SRC" "detached"
done
expect_output "join_array" "$ARRAY_SRC" "42"
expect_output "spawn_options" "$OPTIONS_SRC" "42"
expect_output "alias_join" "$ALIAS_JOIN_SRC" "42"
expect_output "return_transfer" "$RETURN_TRANSFER_SRC" "42"
expect_output "branch_both_close" "$BRANCH_BOTH_CLOSE_SRC" "42"
expect_output "branch_alias_both_close" "$BRANCH_ALIAS_BOTH_CLOSE_SRC" "42"
expect_output "try_catch_both_close" "$TRY_CATCH_BOTH_CLOSE_SRC" "42"
expect_output "match_all_close" "$MATCH_ALL_CLOSE_SRC" "42"
expect_output "select_all_close" "$SELECT_ALL_CLOSE_SRC" "42"
expect_output "ternary_both_close" "$TERNARY_BOTH_CLOSE_SRC" "42"
expect_output "destructure_join" "$DESTRUCTURE_JOIN_SRC" "42"
expect_output "destructure_alias_join" "$DESTRUCTURE_ALIAS_JOIN_SRC" "42"
expect_output "assignment_alias_join" "$ASSIGNMENT_ALIAS_JOIN_SRC" "42"
expect_output "loop_join" "$LOOP_JOIN_SRC" "42"
expect_output "for_loop_join" "$FOR_LOOP_JOIN_SRC" "42"
expect_output "loop_nested_continue_join" "$LOOP_NESTED_CONTINUE_JOIN_SRC" "42"
expect_output "template_join" "$TEMPLATE_JOIN_SRC" "joined 42"
expect_output "slice_bound_join" "$SLICE_BOUND_JOIN_SRC" $'2\n20'
expect_output "channel_capacity_join" "$CHANNEL_CAPACITY_JOIN_SRC" "channel"
expect_output "unsafe_join" "$UNSAFE_JOIN_SRC" "42"
expect_output "threadlocal_basic" "$THREADLOCAL_BASIC_SRC" $'10\n20\n15\n20'
expect_output_workers "yieldable_join" "$YIELDABLE_JOIN_SRC" $'tick\n1\njoined 7\n7' 1
expect_output "defer_join" "$DEFER_JOIN_SRC" $'thread-defer\n42'
expect_output "helper_join" "$HELPER_JOIN_SRC" $'helper joined 42\n42'
expect_output "fn_value_join" "$FN_VALUE_JOIN_SRC" $'fn-value joined 42\n42'
expect_output "top_const_fn_value_join" "$TOP_CONST_FN_VALUE_JOIN_SRC" \
    $'top-fn-value joined 42\n42'
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
expect_warning "ternary_warning" "$TERNARY_WARNING_SRC" $'0\nafter-ternary' \
    "Thread handle 't' from sys.Thread.spawn is not joined or detached before leaving scope"
expect_warning "loop_join_warning" "$LOOP_JOIN_WARNING_SRC" "conditional-loop" \
    "Thread handle 't' from sys.Thread.spawn is not joined or detached before leaving scope"
expect_warning "loop_continue_warning" "$LOOP_CONTINUE_WARNING_SRC" "42" \
    "Thread handle 't' from sys.Thread.spawn is not joined or detached before leaving scope"
expect_warning "loop_try_continue_warning" "$LOOP_TRY_CONTINUE_WARNING_SRC" "42" \
    "Thread handle 't' from sys.Thread.spawn is not joined or detached before leaving scope"
expect_warning "helper_early_return_warning" "$HELPER_EARLY_RETURN_WARNING_SRC" "helper-skip" \
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

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
    shift 3
    local missing=0

    "$XRAY" run "$src" >"$out" 2>"$err"
    local run_status=$?
    for needle in "$@"; do
        if [ -n "$needle" ] && ! grep -Fq "$needle" "$err"; then
            missing=1
        fi
    done

    if [ "$run_status" -eq 0 ] && [ "$(cat "$out")" = "$expected" ] && [ "$missing" -eq 0 ]; then
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
PROCESS_ORPHAN_SRC="$PROJECT_DIR/tests/vm/sys_process_orphan_warning.xr"
PIPE_ORPHAN_SRC="$PROJECT_DIR/tests/vm/sys_pipe_orphan_warning.xr"
PROCESS_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_warning.xr"
PIPE_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_pipe_lifecycle_warning.xr"
PIPE_HALF_CLOSE_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_pipe_lifecycle_half_close_warning.xr"
PROCESS_TRYWAIT_ONCE_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_trywait_once_warning.xr"
PROCESS_CONTINUE_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_continue_warning.xr"
PROCESS_MATCH_CONTINUE_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_match_continue_warning.xr"
CONTROL_FLOW_OK_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_control_flow_ok.xr"
CONST_TRUE_LOOP_OK_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_const_true_loop_ok.xr"
EXPR_CONTAINERS_OK_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_expr_containers_ok.xr"
DESTRUCTURE_ALIAS_OK_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_destructure_alias_ok.xr"
DESTRUCTURE_HELPER_RETURN_ALIAS_OK_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_destructure_helper_return_alias_ok.xr"
ASSIGNMENT_ALIAS_OK_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_assignment_alias_ok.xr"
PROCESS_TRY_CATCH_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_try_catch_warning.xr"
PIPE_MATCH_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_pipe_lifecycle_match_warning.xr"
DETACHED_NO_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_process_detached_no_warning.xr"
SIGNAL_ON_SIGNAL_SRC="$PROJECT_DIR/tests/vm/sys_signal_on_signal.xr"
YIELDABLE_WAIT_SRC="$PROJECT_DIR/tests/vm/sys_process_yieldable_wait.xr"
YIELDABLE_PIPE_READ_SRC="$PROJECT_DIR/tests/vm/sys_pipe_yieldable_read.xr"
PROCESS_DEFER_WAIT_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_defer_wait.xr"
PIPE_DEFER_CLOSE_SRC="$PROJECT_DIR/tests/vm/sys_pipe_lifecycle_defer_close.xr"
PROCESS_HELPER_WAIT_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_helper_wait.xr"
PIPE_HELPER_CLOSE_SRC="$PROJECT_DIR/tests/vm/sys_pipe_lifecycle_helper_close.xr"
PROCESS_FN_VALUE_WAIT_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_fn_value_wait.xr"
PIPE_FN_VALUE_CLOSE_SRC="$PROJECT_DIR/tests/vm/sys_pipe_lifecycle_fn_value_close.xr"
PROCESS_TOP_CONST_FN_VALUE_WAIT_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_top_const_fn_value_wait.xr"
PIPE_TOP_CONST_FN_VALUE_CLOSE_SRC="$PROJECT_DIR/tests/vm/sys_pipe_lifecycle_top_const_fn_value_close.xr"
PROCESS_HELPER_RETURN_ALIAS_WAIT_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_helper_return_alias_wait.xr"
PIPE_HELPER_RETURN_ALIAS_CLOSE_SRC="$PROJECT_DIR/tests/vm/sys_pipe_lifecycle_helper_return_alias_close.xr"
NESTED_HELPER_RETURN_ALIAS_OK_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_nested_helper_return_alias_ok.xr"
TERNARY_HELPER_RETURN_ALIAS_OK_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_ternary_helper_return_alias_ok.xr"
NULLISH_HELPER_RETURN_ALIAS_OK_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_nullish_helper_return_alias_ok.xr"
MATCH_HELPER_RETURN_ALIAS_OK_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_match_helper_return_alias_ok.xr"
HELPER_MATCH_RETURN_ALIAS_OK_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_helper_match_return_alias_ok.xr"
MOVE_ALIAS_OK_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_move_alias_ok.xr"
HELPER_ARG_ALIAS_WRAPPERS_OK_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_helper_arg_alias_wrappers_ok.xr"
UNSAFE_HELPER_RETURN_ALIAS_OK_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_unsafe_helper_return_alias_ok.xr"
UNSAFE_RETURN_ALIAS_OK_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_unsafe_return_alias_ok.xr"
PROCESS_HELPER_CHAINED_RETURN_ALIAS_WAIT_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_helper_chained_return_alias_wait.xr"
PIPE_HELPER_CHAINED_RETURN_ALIAS_CLOSE_SRC="$PROJECT_DIR/tests/vm/sys_pipe_lifecycle_helper_chained_return_alias_close.xr"
PROCESS_HELPER_FORWARD_RETURN_ALIAS_WAIT_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_helper_forward_return_alias_wait.xr"
PIPE_HELPER_FORWARD_RETURN_ALIAS_CLOSE_SRC="$PROJECT_DIR/tests/vm/sys_pipe_lifecycle_helper_forward_return_alias_close.xr"
PROCESS_HELPER_DIRECT_RETURN_ALIAS_WAIT_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_helper_direct_return_alias_wait.xr"
PIPE_HELPER_DIRECT_RETURN_ALIAS_CLOSE_SRC="$PROJECT_DIR/tests/vm/sys_pipe_lifecycle_helper_direct_return_alias_close.xr"
PROCESS_HELPER_FINALIZER_RETURN_ARG_WAIT_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_helper_finalizer_return_arg_wait.xr"
PIPE_HELPER_FINALIZER_RETURN_ARG_CLOSE_SRC="$PROJECT_DIR/tests/vm/sys_pipe_lifecycle_helper_finalizer_return_arg_close.xr"
PROCESS_CONST_ALIAS_RETURN_RECEIVER_WAIT_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_const_alias_return_receiver_wait.xr"
PIPE_CONST_ALIAS_RETURN_RECEIVER_CLOSE_SRC="$PROJECT_DIR/tests/vm/sys_pipe_lifecycle_const_alias_return_receiver_close.xr"
PROCESS_TOP_CONST_ALIAS_RETURN_RECEIVER_WAIT_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_top_const_alias_return_receiver_wait.xr"
PIPE_TOP_CONST_ALIAS_RETURN_RECEIVER_CLOSE_SRC="$PROJECT_DIR/tests/vm/sys_pipe_lifecycle_top_const_alias_return_receiver_close.xr"
FOR_IN_LITERAL_OK_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_for_in_literal_ok.xr"
FOR_IN_CONST_LITERAL_OK_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_for_in_const_literal_ok.xr"
REASSIGNED_ALIAS_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_reassigned_alias_warning.xr"
DESTRUCTURE_REASSIGNED_ALIAS_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_destructure_reassigned_alias_warning.xr"
BRANCH_ALIAS_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_branch_alias_warning.xr"
BRANCH_ALIAS_MERGE_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_branch_alias_merge_warning.xr"
MULTIPATH_ALIAS_MERGE_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_multipath_alias_merge_warning.xr"
MOVE_ALIAS_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_move_alias_warning.xr"
PROCESS_HELPER_EARLY_RETURN_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_helper_early_return_warning.xr"
HELPER_CONTROL_EXIT_WARNING_SRC="$PROJECT_DIR/tests/vm/sys_process_lifecycle_helper_control_exit_warning.xr"

expect_output "process_pipe_lifecycle_ok" "$OK_SRC" $'0\ntrue\ntrue\ntrue\ntrue'
expect_output "process_pipe_lifecycle_control_flow_ok" "$CONTROL_FLOW_OK_SRC" \
    $'0\n0\n0\n0\ntrue\ntrue\n0\ntrue\n0\ntrue'
expect_output "process_pipe_const_true_loop_ok" "$CONST_TRUE_LOOP_OK_SRC" $'0\ntrue'
expect_output "process_pipe_lifecycle_expr_containers_ok" "$EXPR_CONTAINERS_OK_SRC" \
    $'0\ntrue\ncode 0\n2\n0\ntrue\ntrue'
expect_output "process_pipe_lifecycle_destructure_alias_ok" "$DESTRUCTURE_ALIAS_OK_SRC" $'0\ntrue'
expect_output "process_pipe_lifecycle_destructure_helper_return_alias_ok" \
    "$DESTRUCTURE_HELPER_RETURN_ALIAS_OK_SRC" $'0\ntrue'
expect_output "process_pipe_lifecycle_assignment_alias_ok" "$ASSIGNMENT_ALIAS_OK_SRC" $'0\ntrue'
expect_output "process_detached_no_warning" "$DETACHED_NO_WARNING_SRC" \
    $'true\ntrue\n-1\nfalse\ndetached-orphan-ok'
expect_output "signal_on_signal" "$SIGNAL_ON_SIGNAL_SRC" $'true\nfalse\ntrue\ntrue'
expect_output_workers "process_yieldable_wait" "$YIELDABLE_WAIT_SRC" $'tick\n1\nwaited 7\n7' 1
expect_output_workers "pipe_yieldable_read" "$YIELDABLE_PIPE_READ_SRC" $'tick\n1\nread 2\n2\n0\ntrue' 1
expect_output "process_defer_wait" "$PROCESS_DEFER_WAIT_SRC" $'process-defer\n0'
expect_output "pipe_defer_close" "$PIPE_DEFER_CLOSE_SRC" $'pipe-defer\ntrue\ntrue'
expect_output "process_helper_wait" "$PROCESS_HELPER_WAIT_SRC" $'helper waited 0\n0'
expect_output "pipe_helper_close" "$PIPE_HELPER_CLOSE_SRC" $'true\ntrue\npipe-helper'
expect_output "process_fn_value_wait" "$PROCESS_FN_VALUE_WAIT_SRC" $'fn-value waited 0\n0'
expect_output "pipe_fn_value_close" "$PIPE_FN_VALUE_CLOSE_SRC" $'true\ntrue\npipe-fn-value'
expect_output "process_top_const_fn_value_wait" "$PROCESS_TOP_CONST_FN_VALUE_WAIT_SRC" \
    $'top-fn-value waited 0\n0'
expect_output "pipe_top_const_fn_value_close" "$PIPE_TOP_CONST_FN_VALUE_CLOSE_SRC" \
    $'true\ntrue\npipe-top-fn-value'
expect_output "process_helper_return_alias_wait" "$PROCESS_HELPER_RETURN_ALIAS_WAIT_SRC" "0"
expect_output "pipe_helper_return_alias_close" "$PIPE_HELPER_RETURN_ALIAS_CLOSE_SRC" "true"
expect_output "process_pipe_nested_helper_return_alias_ok" \
    "$NESTED_HELPER_RETURN_ALIAS_OK_SRC" $'0\ntrue'
expect_output "process_pipe_ternary_helper_return_alias_ok" \
    "$TERNARY_HELPER_RETURN_ALIAS_OK_SRC" $'0\ntrue'
expect_output "process_pipe_nullish_helper_return_alias_ok" \
    "$NULLISH_HELPER_RETURN_ALIAS_OK_SRC" $'0\ntrue'
expect_output "process_pipe_match_helper_return_alias_ok" \
    "$MATCH_HELPER_RETURN_ALIAS_OK_SRC" $'0\ntrue'
expect_output "process_pipe_helper_match_return_alias_ok" \
    "$HELPER_MATCH_RETURN_ALIAS_OK_SRC" $'0\ntrue'
expect_output "process_pipe_move_alias_ok" "$MOVE_ALIAS_OK_SRC" $'0\ntrue'
expect_output "process_pipe_helper_arg_alias_wrappers_ok" \
    "$HELPER_ARG_ALIAS_WRAPPERS_OK_SRC" $'0\ntrue\n0\ntrue'
expect_output "process_pipe_unsafe_helper_return_alias_ok" \
    "$UNSAFE_HELPER_RETURN_ALIAS_OK_SRC" $'0\ntrue'
expect_output "process_pipe_unsafe_return_alias_ok" \
    "$UNSAFE_RETURN_ALIAS_OK_SRC" $'0\ntrue'
expect_output "process_helper_chained_return_alias_wait" \
    "$PROCESS_HELPER_CHAINED_RETURN_ALIAS_WAIT_SRC" "0"
expect_output "pipe_helper_chained_return_alias_close" \
    "$PIPE_HELPER_CHAINED_RETURN_ALIAS_CLOSE_SRC" "true"
expect_output "process_helper_forward_return_alias_wait" \
    "$PROCESS_HELPER_FORWARD_RETURN_ALIAS_WAIT_SRC" "0"
expect_output "pipe_helper_forward_return_alias_close" \
    "$PIPE_HELPER_FORWARD_RETURN_ALIAS_CLOSE_SRC" "true"
expect_output "process_helper_direct_return_alias_wait" \
    "$PROCESS_HELPER_DIRECT_RETURN_ALIAS_WAIT_SRC" "0"
expect_output "pipe_helper_direct_return_alias_close" \
    "$PIPE_HELPER_DIRECT_RETURN_ALIAS_CLOSE_SRC" "true"
expect_output "process_helper_finalizer_return_arg_wait" \
    "$PROCESS_HELPER_FINALIZER_RETURN_ARG_WAIT_SRC" "0"
expect_output "pipe_helper_finalizer_return_arg_close" \
    "$PIPE_HELPER_FINALIZER_RETURN_ARG_CLOSE_SRC" "true"
expect_output "process_const_alias_return_receiver_wait" \
    "$PROCESS_CONST_ALIAS_RETURN_RECEIVER_WAIT_SRC" "0"
expect_output "pipe_const_alias_return_receiver_close" \
    "$PIPE_CONST_ALIAS_RETURN_RECEIVER_CLOSE_SRC" "true"
expect_output "process_top_const_alias_return_receiver_wait" \
    "$PROCESS_TOP_CONST_ALIAS_RETURN_RECEIVER_WAIT_SRC" "0"
expect_output "pipe_top_const_alias_return_receiver_close" \
    "$PIPE_TOP_CONST_ALIAS_RETURN_RECEIVER_CLOSE_SRC" "true"
expect_output "process_pipe_for_in_literal_ok" "$FOR_IN_LITERAL_OK_SRC" $'0\ntrue'
expect_output "process_pipe_for_in_const_literal_ok" "$FOR_IN_CONST_LITERAL_OK_SRC" $'0\ntrue'
expect_warning "process_orphan" "$PROCESS_ORPHAN_SRC" "process-orphan" \
    "sys.Process.spawn returns a Process handle; call wait() explicitly"
expect_warning "pipe_orphan" "$PIPE_ORPHAN_SRC" "pipe-orphan" \
    "sys.Pipe.open returns a Pipe handle; call close() explicitly"
expect_warning "process_lifecycle_warning" "$PROCESS_WARNING_SRC" "" \
    "Process handle 'p' from sys.Process.spawn is not waited before leaving scope"
expect_warning "process_lifecycle_trywait_once_warning" "$PROCESS_TRYWAIT_ONCE_WARNING_SRC" "" \
    "Process handle 'p' from sys.Process.spawn is not waited before leaving scope"
expect_warning "process_lifecycle_continue_warning" "$PROCESS_CONTINUE_WARNING_SRC" "0" \
    "Process handle 'p' from sys.Process.spawn is not waited before leaving scope"
expect_warning "process_lifecycle_match_continue_warning" "$PROCESS_MATCH_CONTINUE_WARNING_SRC" "0" \
    "Process handle 'p' from sys.Process.spawn is not waited before leaving scope"
expect_warning "process_lifecycle_try_catch_warning" "$PROCESS_TRY_CATCH_WARNING_SRC" \
    $'0\ntry-catch-open' \
    "Process handle 'p' from sys.Process.spawn is not waited before leaving scope"
expect_warning "process_helper_early_return_warning" "$PROCESS_HELPER_EARLY_RETURN_WARNING_SRC" \
    "process-helper-skip" \
    "Process handle 'p0' from sys.Process.spawn is not waited before leaving scope"
expect_warning "process_pipe_helper_control_exit_warning" "$HELPER_CONTROL_EXIT_WARNING_SRC" \
    $'process-after\npipe-after' \
    "Process handle 'spawned' from sys.Process.spawn is not waited before leaving scope" \
    "Pipe handle 'opened' from sys.Pipe.open is not closed before leaving scope"
expect_warning "process_pipe_move_alias_warning" "$MOVE_ALIAS_WARNING_SRC" \
    $'process-moved\npipe-moved' \
    "Process handle 'spawned' from sys.Process.spawn is not waited before leaving scope" \
    "Pipe handle 'opened' from sys.Pipe.open is not closed before leaving scope"
expect_warning "process_pipe_reassigned_alias_warning" "$REASSIGNED_ALIAS_WARNING_SRC" \
    $'0\ntrue' \
    "Process handle 'processOriginal' from sys.Process.spawn is not waited before leaving scope" \
    "Pipe handle 'pipeOriginal' from sys.Pipe.open is not closed before leaving scope"
expect_warning "process_pipe_destructure_reassigned_alias_warning" \
    "$DESTRUCTURE_REASSIGNED_ALIAS_WARNING_SRC" $'0\ntrue' \
    "Process handle 'processFirst' from sys.Process.spawn is not waited before leaving scope" \
    "Pipe handle 'pipeFirst' from sys.Pipe.open is not closed before leaving scope"
expect_warning "process_pipe_branch_alias_warning" "$BRANCH_ALIAS_WARNING_SRC" $'0\ntrue' \
    "Process handle 'processLeaked' from sys.Process.spawn is not waited before leaving scope" \
    "Pipe handle 'pipeLeaked' from sys.Pipe.open is not closed before leaving scope"
expect_warning "process_pipe_branch_alias_merge_warning" "$BRANCH_ALIAS_MERGE_WARNING_SRC" \
    $'0\ntrue' \
    "Process handle 'processOther' from sys.Process.spawn is not waited before leaving scope" \
    "Pipe handle 'pipeOther' from sys.Pipe.open is not closed before leaving scope"
expect_warning "process_pipe_multipath_alias_merge_warning" \
    "$MULTIPATH_ALIAS_MERGE_WARNING_SRC" $'0\n0\n0\ntrue' \
    "Process handle 'oldTry' from sys.Process.spawn is not waited before leaving scope" \
    "Process handle 'oldMatch' from sys.Process.spawn is not waited before leaving scope" \
    "Process handle 'oldSelect' from sys.Process.spawn is not waited before leaving scope" \
    "Pipe handle 'oldPipe' from sys.Pipe.open is not closed before leaving scope"
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

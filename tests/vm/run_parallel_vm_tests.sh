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

expect_output_workers \
    "parallel_callback_nested_function_effect_not_lane" \
    "$PROJECT_DIR/tests/vm/parallel_callback_nested_function_effect_not_lane.xr" \
    "6" \
    2

expect_output_workers \
    "parallel_callback_const_function_value_safe" \
    "$PROJECT_DIR/tests/vm/parallel_callback_const_function_value_safe.xr" \
    "10" \
    2

expect_output_workers \
    "parallel_callback_channel_try_ops_safe" \
    "$PROJECT_DIR/tests/vm/parallel_callback_channel_try_ops_safe.xr" \
    "4" \
    2

expect_output_workers \
    "parallel_callback_stdlib_normal_call_safe" \
    "$PROJECT_DIR/tests/vm/parallel_callback_stdlib_normal_call_safe.xr" \
    "3" \
    2

expect_output_workers \
    "parallel_plan_for_each_vm_batch" \
    "$PROJECT_DIR/tests/vm/parallel_plan_for_each_vm_batch.xr" \
    $'true\ntrue\ntrue\ntrue' \
    4

expect_output_workers \
    "parallel_plan_map_vm_batch" \
    "$PROJECT_DIR/tests/vm/parallel_plan_map_vm_batch.xr" \
    $'true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue' \
    4

expect_output_workers \
    "parallel_plan_reduce_vm_batch" \
    "$PROJECT_DIR/tests/vm/parallel_plan_reduce_vm_batch.xr" \
    $'true\ntrue\ntrue\ntrue\ntrue' \
    4

expect_output_workers \
    "parallel_plan_reduce_aggregate_vm_batch" \
    "$PROJECT_DIR/tests/vm/parallel_plan_reduce_aggregate_vm_batch.xr" \
    $'true\ntrue\ntrue\ntrue\ntrue\ntrue' \
    4

expect_output_workers \
    "parallel_plan_reduce_reference_accumulator" \
    "$PROJECT_DIR/tests/vm/parallel_plan_reduce_reference_accumulator.xr" \
    $'40\n40' \
    2

expect_output_workers \
    "parallel_reduce_generic_failure_cleanup" \
    "$PROJECT_DIR/tests/vm/parallel_reduce_generic_failure_cleanup.xr" \
    $'true\ntrue\ntrue\n1000' \
    4

expect_output_workers \
    "parallel_plan_reduce_generic_failure_cleanup" \
    "$PROJECT_DIR/tests/vm/parallel_plan_reduce_generic_failure_cleanup.xr" \
    $'true\ntrue\ntrue\n1000\n4' \
    4

expect_output_workers \
    "parallel_plan_reduce_combine_cleanup_after_panic" \
    "$PROJECT_DIR/tests/vm/parallel_plan_reduce_combine_cleanup_after_panic.xr" \
    $'caught\ntrue' \
    4

expect_output_workers \
    "parallel_plan_reduce_combine_close_after_panic_cleanup" \
    "$PROJECT_DIR/tests/vm/parallel_plan_reduce_combine_close_after_panic_cleanup.xr" \
    $'true\ntrue\nclosed\ntrue' \
    4

expect_output_workers \
    "parallel_plan_cleanup_after_panic" \
    "$PROJECT_DIR/tests/vm/parallel_plan_cleanup_after_panic.xr" \
    $'caught\ntrue' \
    2

expect_output_workers \
    "parallel_plan_for_each_close_after_panic_cleanup" \
    "$PROJECT_DIR/tests/vm/parallel_plan_for_each_close_after_panic_cleanup.xr" \
    $'true\ntrue\ntrue\nclosed\ntrue' \
    4

expect_output_workers \
    "parallel_plan_map_cleanup_after_panic" \
    "$PROJECT_DIR/tests/vm/parallel_plan_map_cleanup_after_panic.xr" \
    $'true\ntrue\ntrue\ntrue\ntrue\ntrue' \
    2

expect_output_workers \
    "parallel_plan_map_close_after_panic_cleanup" \
    "$PROJECT_DIR/tests/vm/parallel_plan_map_close_after_panic_cleanup.xr" \
    $'true\ntrue\ntrue\nmap-closed\ntrue\ntrue\ntrue\nmapinto-closed\ntrue' \
    2

expect_output_workers \
    "parallel_plan_map_close_during_dispatch" \
    "$PROJECT_DIR/tests/vm/parallel_plan_map_close_during_dispatch.xr" \
    $'true\nmap-closed\ntrue\ntrue\nmapinto-closed\ndone' \
    2

expect_output_workers \
    "parallel_plan_close_during_dispatch" \
    "$PROJECT_DIR/tests/vm/parallel_plan_close_during_dispatch.xr" \
    $'true\nclosed\ndone' \
    2

expect_output_workers \
    "parallel_plan_close_after_error_cleanup" \
    "$PROJECT_DIR/tests/vm/parallel_plan_close_after_error_cleanup.xr" \
    $'true\ntrue\nclosed\ntrue' \
    2

expect_output_workers \
    "parallel_lane_panic_propagates_original" \
    "$PROJECT_DIR/tests/vm/parallel_lane_panic_propagates_original.xr" \
    $'true\ntrue\ntrue' \
    2

expect_output_workers \
    "parallel_plan_nested_dispatch_gate" \
    "$PROJECT_DIR/tests/vm/parallel_plan_nested_dispatch_gate.xr" \
    $'seq-concurrent\ntrue\nvm-concurrent\ntrue' \
    2

expect_output_workers \
    "parallel_plan_vm_scaling_baseline" \
    "$PROJECT_DIR/tests/vm/parallel_plan_vm_scaling_baseline.xr" \
    $'true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue' \
    8

expect_output_workers \
    "parallel_plan_fallback_single_lane" \
    "$PROJECT_DIR/tests/vm/parallel_plan_fallback_single_lane.xr" \
    $'0\n128\n0\n128\n0' \
    1

printf '\nSummary: %d passed, %d failed\n' "$PASS" "$FAIL"
if [ "$FAIL" -ne 0 ]; then
    exit 1
fi

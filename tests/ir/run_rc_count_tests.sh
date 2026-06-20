#!/bin/bash
# Borrow/ownership RC-count smoke.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
XRAY="${1:-${XRAY_BIN:-$PROJECT_DIR/build/xray}}"
PASS=0
FAIL=0

record_pass() {
    echo "  PASS: $1"
    PASS=$((PASS + 1))
}

record_fail() {
    echo "  FAIL: $1"
    FAIL=$((FAIL + 1))
}

run_case() {
    local src="$1"
    local stdout_file="$2"
    local stderr_file="$3"
    shift 3
    "$@" "$XRAY" "$src" >"$stdout_file" 2>"$stderr_file"
}

extract_total() {
    sed -n 's/.*\[xi-rc-count\].* total=\([0-9][0-9]*\).*/\1/p' "$1" | head -1
}

expect_output() {
    local out="$1"
    local want="$2"
    local name="$3"
    local got
    got="$(cat "$out")"
    if [ "$got" = "$want" ]; then
        record_pass "$name"
    else
        record_fail "$name: output '$got' != '$want'"
    fi
}

expect_no_count_line() {
    local err="$1"
    local name="$2"
    if grep -Fq "[xi-rc-count]" "$err"; then
        record_fail "$name: rc-count line emitted while disabled"
        sed 's/^/      /' "$err" | sed -n '1,80p'
    else
        record_pass "$name"
    fi
}

expect_count_max() {
    local err="$1"
    local max_total="$2"
    local name="$3"
    local total
    if ! grep -Eq '\[xi-rc-count\] func=[^ ]+ retain=[0-9]+ release=[0-9]+ total=[0-9]+' "$err"; then
        record_fail "$name: missing parseable rc-count line"
        sed 's/^/      /' "$err" | sed -n '1,80p'
        return
    fi
    total="$(extract_total "$err")"
    if [ -z "$total" ]; then
        record_fail "$name: missing total"
    elif [ "$total" -le "$max_total" ]; then
        record_pass "$name: total=$total <= $max_total"
    else
        record_fail "$name: total=$total > $max_total"
        sed 's/^/      /' "$err" | sed -n '1,80p'
    fi
}

expect_count_min() {
    local err="$1"
    local min_total="$2"
    local name="$3"
    local total
    if ! grep -Eq '\[xi-rc-count\] func=[^ ]+ retain=[0-9]+ release=[0-9]+ total=[0-9]+' "$err"; then
        record_fail "$name: missing parseable rc-count line"
        sed 's/^/      /' "$err" | sed -n '1,80p'
        return
    fi
    total="$(extract_total "$err")"
    if [ -z "$total" ]; then
        record_fail "$name: missing total"
    elif [ "$total" -ge "$min_total" ]; then
        record_pass "$name: total=$total >= $min_total"
    else
        record_fail "$name: total=$total < $min_total"
        sed 's/^/      /' "$err" | sed -n '1,80p'
    fi
}

echo "=== Xi RC Count Tests ==="
echo "Binary: $XRAY"
echo ""

if [ ! -x "$XRAY" ]; then
    echo "FAIL: xray binary not executable: $XRAY" >&2
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_rc_count.XXXXXX")" || exit 1
cleanup() {
    rm -rf "$WORK"
}
trap cleanup EXIT

SCALAR="$PROJECT_DIR/tests/ir/rc_count/scalar_no_rc.xr"
BORROWED="$PROJECT_DIR/tests/ir/rc_count/value_struct_borrowed_param.xr"
BORROWED_WRAPPER="$PROJECT_DIR/tests/ir/rc_count/value_struct_wrapper_borrowed_param.xr"
HEAP_READONLY="$PROJECT_DIR/tests/ir/rc_count/heap_readonly_param.xr"
CLASS_READONLY="$PROJECT_DIR/tests/ir/rc_count/class_readonly_param.xr"
CLASS_ALIAS_RETURN="$PROJECT_DIR/tests/ir/rc_count/class_alias_return_downgrade.xr"

run_case "$SCALAR" "$WORK/scalar_disabled.out" "$WORK/scalar_disabled.err" env
expect_output "$WORK/scalar_disabled.out" "2" "scalar: program output"
expect_no_count_line "$WORK/scalar_disabled.err" "scalar: disabled by default"

run_case "$SCALAR" "$WORK/scalar_enabled.out" "$WORK/scalar_enabled.err" \
    env XRAY_XI_RC_COUNT=1
expect_output "$WORK/scalar_enabled.out" "2" "scalar: enabled program output"
expect_count_max "$WORK/scalar_enabled.err" 0 "scalar: no heap RC ops"

run_case "$BORROWED" "$WORK/borrowed.out" "$WORK/borrowed.err" env XRAY_XI_RC_COUNT=1
expect_output "$WORK/borrowed.out" "3" "borrowed value param: program output"
expect_count_max "$WORK/borrowed.err" 2 "borrowed value param: RC budget"

run_case "$BORROWED_WRAPPER" "$WORK/borrowed_wrapper.out" "$WORK/borrowed_wrapper.err" \
    env XRAY_XI_RC_COUNT=1
expect_output "$WORK/borrowed_wrapper.out" "3
1" "borrowed value wrapper param: program output"
expect_count_max "$WORK/borrowed_wrapper.err" 1 "borrowed value wrapper param: RC budget"

run_case "$HEAP_READONLY" "$WORK/heap_readonly.out" "$WORK/heap_readonly.err" env \
    XRAY_XI_RC_COUNT=1
expect_output "$WORK/heap_readonly.out" "3" "heap readonly param: program output"
expect_count_max "$WORK/heap_readonly.err" 0 "heap readonly param: RC budget"

run_case "$CLASS_READONLY" "$WORK/class_readonly.out" "$WORK/class_readonly.err" env \
    XRAY_XI_RC_COUNT=1
expect_output "$WORK/class_readonly.out" "7" "class readonly param: program output"
expect_count_max "$WORK/class_readonly.err" 0 "class readonly param: RC budget"

run_case "$CLASS_ALIAS_RETURN" "$WORK/class_alias_return.out" "$WORK/class_alias_return.err" env \
    XRAY_XI_RC_COUNT=1
expect_output "$WORK/class_alias_return.out" "9
9" "class alias-return downgrade: program output"
expect_count_min "$WORK/class_alias_return.err" 1 "class alias-return downgrade: not borrowed"
expect_count_max "$WORK/class_alias_return.err" 4 "class alias-return downgrade: RC budget"

echo ""
echo "Passed: $PASS"
echo "Failed: $FAIL"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi

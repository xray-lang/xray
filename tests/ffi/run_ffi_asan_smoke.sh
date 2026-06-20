#!/bin/bash
# FFI ASan smoke tests.
#
# Normal builds skip with 77. ASan builds must run representative FFI memory
# cases through both VM and AOT, and AOT native binaries must inherit ASan flags.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
XRAY="${1:-${XRAY_BIN:-$PROJECT_DIR/build-sanitizers/xray}}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_ffi_asan.XXXXXX")" || {
    echo "FAIL: cannot create temporary directory" >&2
    exit 1
}
CACHE="$WORK/.cache"
PASS=0
FAIL=0
SKIP=0
AOT_BIN=""
AOT_OUTPUT=""

cleanup() {
    rm -rf "$WORK"
}
trap cleanup EXIT

record_pass() {
    echo "  PASS: $1"
    PASS=$((PASS + 1))
}

record_fail() {
    echo "  FAIL: $1"
    FAIL=$((FAIL + 1))
}

record_skip() {
    echo "  SKIP: $1"
    SKIP=$((SKIP + 1))
}

strip_ansi() {
    sed -E $'s/\x1B\\[[0-9;]*[a-zA-Z]//g'
}

normalize_output() {
    strip_ansi | sed -e 's/[[:space:]]*$//' -e '/^$/d'
}

expect_output() {
    local got="$1"
    local expected="$2"
    local name="$3"
    if [ "$got" = "$expected" ]; then
        record_pass "$name"
    else
        record_fail "$name"
        echo "      expected:"
        printf '%s\n' "$expected" | sed 's/^/        /'
        echo "      got:"
        printf '%s\n' "$got" | sed 's/^/        /'
    fi
}

run_vm_case() {
    local src="$1"
    local stem="$2"
    local out="$WORK/$stem.vm.out"
    local err="$WORK/$stem.vm.err"
    ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:halt_on_error=1:abort_on_error=1}" \
        "$XRAY" run "$src" >"$out" 2>"$err"
    local status=$?
    if [ "$status" -ne 0 ]; then
        if grep -Fq "this build has no libffi" "$err" "$out" 2>/dev/null; then
            return 77
        fi
        echo "      VM case failed with status $status" >&2
        { cat "$err"; cat "$out"; } | sed 's/^/      /' >&2
        return "$status"
    fi
    normalize_output <"$out"
}

build_aot_case() {
    local src="$1"
    local stem="$2"
    local bin="$WORK/$stem.bin"
    local log="$WORK/$stem.build.log"
    "$XRAY" build --native --dump-link-manifest --dump-link-command \
        --cache-dir "$CACHE" -o "$bin" "$src" >"$log" 2>&1
    local status=$?
    if [ "$status" -ne 0 ]; then
        echo "      AOT build failed with status $status" >&2
        normalize_output <"$log" | sed 's/^/      /' >&2
        return "$status"
    fi
    if grep -Fq -- "-fsanitize=address" "$log"; then
        record_pass "$stem: AOT manifest carries ASan flags"
    else
        record_fail "$stem: AOT manifest carries ASan flags"
        normalize_output <"$log" | sed 's/^/      /'
    fi
    AOT_BIN="$bin"
}

run_aot_case() {
    local src="$1"
    local stem="$2"
    build_aot_case "$src" "$stem" || return $?
    local bin="$AOT_BIN"
    local out="$WORK/$stem.aot.out"
    local err="$WORK/$stem.aot.err"
    ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:halt_on_error=1:abort_on_error=1}" \
        "$bin" >"$out" 2>"$err"
    local status=$?
    if [ "$status" -ne 0 ]; then
        echo "      AOT case failed with status $status" >&2
        { cat "$err"; cat "$out"; } | sed 's/^/      /' >&2
        return "$status"
    fi
    AOT_OUTPUT="$(normalize_output <"$out")"
}

echo "=== FFI ASan Smoke Tests ==="
echo "Binary: $XRAY"
echo ""

if [ ! -x "$XRAY" ]; then
    echo "FAIL: xray binary not executable: $XRAY" >&2
    exit 1
fi

PROBE_SRC="$WORK/asan_probe.xr"
cat >"$PROBE_SRC" <<'XR'
print(1)
XR
PROBE_LOG="$WORK/asan_probe.log"
if ! "$XRAY" build --native --dump-link-manifest --cache-dir "$CACHE" \
    -o "$WORK/asan_probe" "$PROBE_SRC" >"$PROBE_LOG" 2>&1; then
    echo "FAIL: cannot build ASan probe" >&2
    normalize_output <"$PROBE_LOG" | sed 's/^/      /' >&2
    exit 1
fi
if ! grep -Fq -- "-fsanitize=address" "$PROBE_LOG"; then
    record_skip "xray binary is not an ASan build"
    echo ""
    echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
    exit 77
fi
record_pass "xray ASan build detected"

declare -a CASE_NAMES=(
    "ffi_ptr_memory"
    "ffi_cfn_bsearch"
    "repr_c_struct"
)
declare -a CASE_SRCS=(
    "$PROJECT_DIR/tests/diff/cases/semantics/ffi/ptr_memory.xr"
    "$PROJECT_DIR/tests/diff/cases/semantics/ffi/cfn_bsearch.xr"
    "$PROJECT_DIR/tests/diff/cases/semantics/oop/repr_c_struct.xr"
)
declare -a CASE_EXPECTED=(
    $'10\n20\n30\n40\n30\nfalse\nfalse'
    "false"
    $'12\n20\ntrue\n4\n2\n3'
)

for i in "${!CASE_NAMES[@]}"; do
    name="${CASE_NAMES[$i]}"
    src="${CASE_SRCS[$i]}"
    expected="${CASE_EXPECTED[$i]}"
    echo ""
    echo "--- $name ---"
    if [ ! -f "$src" ]; then
        record_fail "$name: source exists"
        continue
    fi
    vm_out="$(run_vm_case "$src" "$name")"
    vm_status=$?
    if [ "$vm_status" -eq 77 ]; then
        record_skip "$name: VM libffi disabled"
    elif [ "$vm_status" -eq 0 ]; then
        expect_output "$vm_out" "$expected" "$name: VM output"
    else
        record_fail "$name: VM run"
    fi

    if run_aot_case "$src" "$name"; then
        expect_output "$AOT_OUTPUT" "$expected" "$name: AOT output"
    else
        record_fail "$name: AOT run"
    fi
done

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0

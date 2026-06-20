#!/bin/bash
# FFI diagnostic regression tests.
#
# Locks user-facing error contracts for @extern/@dylib failures that are not
# compile errors: VM runtime resolution and AOT native link failures.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
XRAY="${1:-${XRAY_BIN:-$PROJECT_DIR/build/xray}}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_ffi_diag.XXXXXX")" || {
    echo "FAIL: cannot create temporary directory" >&2
    exit 1
}
CACHE="$WORK/.cache"
PASS=0
FAIL=0
SKIP=0

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

expect_contains() {
    local text="$1"
    local needle="$2"
    local name="$3"
    if printf '%s' "$text" | grep -Fq -- "$needle"; then
        record_pass "$name"
    else
        record_fail "$name"
        printf '%s\n' "$text" | sed 's/^/      /' | sed -n '1,100p'
    fi
}

expect_not_contains() {
    local text="$1"
    local needle="$2"
    local name="$3"
    if printf '%s' "$text" | grep -Fq -- "$needle"; then
        record_fail "$name"
        printf '%s\n' "$text" | sed 's/^/      /' | sed -n '1,100p'
    else
        record_pass "$name"
    fi
}

run_vm_case() {
    local src="$1"
    local out="$2"
    "$XRAY" run "$src" >"$out.stdout" 2>"$out.stderr" || true
    { cat "$out.stderr"; cat "$out.stdout"; } | strip_ansi
}

build_native_case() {
    local src="$1"
    local out="$2"
    local log="$3"
    "$XRAY" build --native --dump-link-command --cache-dir "$CACHE" -o "$out" "$src" \
        >"$log" 2>&1
}

echo "=== FFI Diagnostics Tests ==="
echo "Binary: $XRAY"
echo ""

if [ ! -x "$XRAY" ]; then
    echo "FAIL: xray binary not executable: $XRAY" >&2
    exit 1
fi

MISSING_LIB="xray_missing_library_nope_zz"
MISSING_SYMBOL="xray_missing_symbol_nope_zz"

VM_MISSING_LIB_SRC="$WORK/vm_missing_lib.xr"
cat > "$VM_MISSING_LIB_SRC" <<XR
@extern("C") @dylib("$MISSING_LIB") fn nope(x: int32) -> int32
print(unsafe { nope(1) })
XR

VM_LIB_TEXT="$(run_vm_case "$VM_MISSING_LIB_SRC" "$WORK/vm_missing_lib")"
if printf '%s' "$VM_LIB_TEXT" | grep -Fq "this build has no libffi"; then
    record_skip "vm missing library: libffi disabled"
else
    expect_contains "$VM_LIB_TEXT" "FFI: cannot load library '$MISSING_LIB'" \
        "vm missing library: reports load failure"
    expect_not_contains "$VM_LIB_TEXT" "FFI: symbol 'nope' not found" \
        "vm missing library: no misleading symbol error"
fi

VM_MISSING_SYMBOL_SRC="$WORK/vm_missing_symbol.xr"
cat > "$VM_MISSING_SYMBOL_SRC" <<XR
@extern("C") fn $MISSING_SYMBOL(x: int32) -> int32
print(unsafe { $MISSING_SYMBOL(1) })
XR

VM_SYM_TEXT="$(run_vm_case "$VM_MISSING_SYMBOL_SRC" "$WORK/vm_missing_symbol")"
if printf '%s' "$VM_SYM_TEXT" | grep -Fq "this build has no libffi"; then
    record_skip "vm missing symbol: libffi disabled"
else
    expect_contains "$VM_SYM_TEXT" "FFI: symbol '$MISSING_SYMBOL' not found" \
        "vm missing symbol: reports symbol failure"
    expect_not_contains "$VM_SYM_TEXT" "FFI: cannot load library" \
        "vm missing symbol: no library failure"
fi

AOT_MISSING_LIB_BIN="$WORK/aot_missing_lib"
AOT_MISSING_LIB_LOG="$WORK/aot_missing_lib.log"
if build_native_case "$VM_MISSING_LIB_SRC" "$AOT_MISSING_LIB_BIN" "$AOT_MISSING_LIB_LOG"; then
    record_fail "aot missing library: build should fail"
else
    AOT_LIB_TEXT="$(strip_ansi < "$AOT_MISSING_LIB_LOG")"
    expect_contains "$AOT_LIB_TEXT" "Link command:" "aot missing library: emits link command"
    expect_contains "$AOT_LIB_TEXT" "$MISSING_LIB" "aot missing library: names missing library"
    expect_contains "$AOT_LIB_TEXT" "AOT manifest linking failed" \
        "aot missing library: reports AOT link failure"
fi

AOT_MISSING_SYMBOL_BIN="$WORK/aot_missing_symbol"
AOT_MISSING_SYMBOL_LOG="$WORK/aot_missing_symbol.log"
if build_native_case "$VM_MISSING_SYMBOL_SRC" "$AOT_MISSING_SYMBOL_BIN" "$AOT_MISSING_SYMBOL_LOG"; then
    record_fail "aot missing symbol: build should fail"
else
    AOT_SYM_TEXT="$(strip_ansi < "$AOT_MISSING_SYMBOL_LOG")"
    expect_contains "$AOT_SYM_TEXT" "Link command:" "aot missing symbol: emits link command"
    expect_contains "$AOT_SYM_TEXT" "$MISSING_SYMBOL" "aot missing symbol: names missing symbol"
    expect_contains "$AOT_SYM_TEXT" "AOT manifest linking failed" \
        "aot missing symbol: reports AOT link failure"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0

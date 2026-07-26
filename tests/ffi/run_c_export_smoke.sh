#!/bin/bash
# Manifest-driven C export smoke test.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
XRAY="${1:-${XRAY_BIN:-$PROJECT_DIR/build/xray}}"
case "$XRAY" in
    /*) ;;
    *) XRAY="$(cd "$(dirname "$XRAY")" && pwd)/$(basename "$XRAY")" ;;
esac
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_manifest_export.XXXXXX")" || exit 1
CC_BIN="${CC:-cc}"
FIXTURE_ROOT="$PROJECT_DIR/tests/fixtures/manifest_export"
PASS=0
FAIL=0

cleanup() {
    if [ "${XRAY_FFI_KEEP_WORK:-0}" = "1" ]; then
        echo "Work dir: $WORK"
    else
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT

pass() {
    echo "  PASS: $1"
    PASS=$((PASS + 1))
}

fail() {
    echo "  FAIL: $1"
    FAIL=$((FAIL + 1))
}

expect_contains() {
    local file="$1"
    local needle="$2"
    local name="$3"
    if [ -f "$file" ] && grep -Fq -- "$needle" "$file"; then
        pass "$name"
    else
        fail "$name"
        [ ! -f "$file" ] || sed -n '1,100p' "$file" | sed 's/^/      /'
    fi
}

if [ ! -x "$XRAY" ]; then
    echo "FAIL: xray binary not executable: $XRAY" >&2
    exit 1
fi

GEN_C="$WORK/generated.c"
GEN_H="$WORK/exports.h"
GEN_O="$WORK/generated.o"
CALLER_O="$WORK/caller.o"
CALLER_BIN="$WORK/caller"
BUILD_LOG="$WORK/build.log"

if (cd "$FIXTURE_ROOT/positive" && "$XRAY" build --native --c-only \
        --c-header "$GEN_H" -o "$GEN_C" main.xr >"$BUILD_LOG" 2>&1); then
    pass "manifest export generates C and header"
else
    fail "manifest export generates C and header"
    sed -n '1,140p' "$BUILD_LOG" | sed 's/^/      /'
fi

expect_contains "$GEN_H" "xr_add_i32" "header exposes manifest symbol"
expect_contains "$GEN_C" "xr_add_i32" "generated C defines manifest symbol"

if "$CC_BIN" -std=c11 -Dmain=xray_generated_main \
        -I "$PROJECT_DIR/include" -I "$PROJECT_DIR/src/aot" \
        -c "$GEN_C" -o "$GEN_O" >"$WORK/cc-generated.log" 2>&1 &&
   "$CC_BIN" -std=c11 -I "$WORK" -c "$FIXTURE_ROOT/positive/caller.c" \
        -o "$CALLER_O" >"$WORK/cc-caller.log" 2>&1 &&
   "$CC_BIN" "$GEN_O" "$CALLER_O" -pthread -lm -o "$CALLER_BIN" \
        >"$WORK/link.log" 2>&1 &&
   "$CALLER_BIN" >"$WORK/run.log" 2>&1; then
    pass "external C caller links and observes 42"
else
    fail "external C caller links and observes 42"
    for log in "$WORK/cc-generated.log" "$WORK/cc-caller.log" \
               "$WORK/link.log" "$WORK/run.log"; do
        [ ! -f "$log" ] || sed -n '1,100p' "$log" | sed 's/^/      /'
    done
fi

if (cd "$FIXTURE_ROOT/duplicate" && "$XRAY" build --native --c-only \
        -o "$WORK/duplicate.c" main.xr >"$WORK/duplicate.log" 2>&1); then
    fail "duplicate manifest symbol is rejected"
else
    expect_contains "$WORK/duplicate.log" "E-EXPORT-SCHEMA" \
        "duplicate manifest symbol is rejected"
fi

if (cd "$FIXTURE_ROOT/managed" && "$XRAY" build --native --c-only \
        -o "$WORK/managed.c" main.xr >"$WORK/managed.log" 2>&1); then
    fail "managed export signature is rejected"
else
    if grep -Eq 'C ABI|managed|export' "$WORK/managed.log"; then
        pass "managed export signature is rejected"
    else
        fail "managed export signature is rejected"
        sed -n '1,120p' "$WORK/managed.log" | sed 's/^/      /'
    fi
fi

if (cd "$FIXTURE_ROOT/amalgam" && "$XRAY" build --native --c-only \
        -o "$WORK/amalgam.c" main.xr >"$WORK/amalgam.log" 2>&1) &&
   "$CC_BIN" -std=c11 -I "$PROJECT_DIR/include" -I "$PROJECT_DIR/src/aot" \
        -c "$WORK/amalgam.c" -o "$WORK/amalgam.o" \
        >"$WORK/cc-amalgam.log" 2>&1; then
    pass "multi-module C-only output is one compilable translation unit"
else
    fail "multi-module C-only output is one compilable translation unit"
    for log in "$WORK/amalgam.log" "$WORK/cc-amalgam.log"; do
        [ ! -f "$log" ] || sed -n '1,140p' "$log" | sed 's/^/      /'
    done
fi

echo ""
echo "Manifest C export smoke: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]

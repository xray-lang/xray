#!/bin/bash
# AOT @c_export smoke test.
#
# Generates C from a tiny Xray module, compiles the generated translation unit
# as an object, then links a separate C caller that invokes the exported symbols.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
XRAY="${1:-${XRAY_BIN:-$PROJECT_DIR/build/xray}}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_c_export.XXXXXX")" || {
    echo "FAIL: cannot create temporary directory" >&2
    exit 1
}
PASS=0
FAIL=0

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

expect_file_contains() {
    local file="$1"
    local needle="$2"
    local name="$3"
    if grep -Fq -- "$needle" "$file"; then
        record_pass "$name"
    else
        record_fail "$name"
        sed 's/^/      /' "$file" | sed -n '1,120p'
    fi
}

expect_file_not_contains() {
    local file="$1"
    local needle="$2"
    local name="$3"
    if grep -Fq -- "$needle" "$file"; then
        record_fail "$name"
        sed 's/^/      /' "$file" | sed -n '1,120p'
    else
        record_pass "$name"
    fi
}

echo "=== AOT C Export Smoke ==="
echo "Binary: $XRAY"
echo ""

if [ ! -x "$XRAY" ]; then
    echo "FAIL: xray binary not executable: $XRAY" >&2
    exit 1
fi

SRC="$WORK/c_export_link.xr"
GEN_C="$WORK/c_export_link.c"
GEN_OBJ="$WORK/c_export_link.o"
CALLER_C="$WORK/caller.c"
CALLER_BIN="$WORK/caller"
GEN_LOG="$WORK/generate.log"
OBJ_LOG="$WORK/generated_obj.log"
LINK_LOG="$WORK/link.log"
RUN_LOG="$WORK/run.log"

cat >"$SRC" <<'XR'
@c_export("xr_add_i32")
fn add_i32(a: int32, b: int32) -> int32 {
    return a + b
}

@c_export("xr_mix_i64")
fn mix_i64(a: int64, b: int64) -> int64 {
    return a * b + 7
}

@c_export("xr_mix_f64")
fn mix_f64(a: float64, b: float64) -> float64 {
    return a * 2.0 + b
}

@c_export("xr_read_i32_ptr")
fn read_i32_ptr(p: RawPtr<int32>) -> int32 {
    unsafe {
        return p[0]
    }
}

@c_export("xr_write_i32_ptr")
fn write_i32_ptr(p: RawMut<int32>, v: int32) -> int32 {
    unsafe {
        p[0] = v
        return p[0]
    }
}

print(add_i32(19, 23))
XR

if "$XRAY" build --native -c -o "$GEN_C" "$SRC" >"$GEN_LOG" 2>&1; then
    record_pass "generate native C source"
else
    record_fail "generate native C source"
    sed 's/^/      /' "$GEN_LOG" | sed -n '1,120p'
fi

if [ -f "$GEN_C" ]; then
    expect_file_contains "$GEN_C" "int32_t xr_add_i32(int32_t p0, int32_t p1);" \
        "declares int32 export"
    expect_file_contains "$GEN_C" "int64_t xr_mix_i64(int64_t p0, int64_t p1);" \
        "declares int64 export"
    expect_file_contains "$GEN_C" "double xr_mix_f64(double p0, double p1);" \
        "declares float64 export"
    expect_file_contains "$GEN_C" "int32_t xr_read_i32_ptr(const void * p0);" \
        "declares RawPtr export"
    expect_file_contains "$GEN_C" "int32_t xr_write_i32_ptr(void * p0, int32_t p1);" \
        "declares RawMut export"
    expect_file_not_contains "$GEN_C" "static int32_t xr_add_i32" \
        "int32 export is public"
fi

if cc -O2 -Wall -Wno-initializer-overrides -Wno-unused-function -Wno-unused-variable \
    -Dmain=xray_generated_main \
    -I "$PROJECT_DIR/src/aot" -I "$PROJECT_DIR/include" \
    -c "$GEN_C" -o "$GEN_OBJ" >"$OBJ_LOG" 2>&1; then
    record_pass "compile generated C object"
else
    record_fail "compile generated C object"
    sed 's/^/      /' "$OBJ_LOG" | sed -n '1,160p'
fi

cat >"$CALLER_C" <<'C'
#include <math.h>
#include <stdint.h>
#include <stdio.h>

int32_t xr_add_i32(int32_t a, int32_t b);
int64_t xr_mix_i64(int64_t a, int64_t b);
double xr_mix_f64(double a, double b);
int32_t xr_read_i32_ptr(const void *p);
int32_t xr_write_i32_ptr(void *p, int32_t v);

int main(void) {
    double d;
    int32_t value = 10;
    if (xr_add_i32(40, 2) != 42)
        return 10;
    if (xr_mix_i64(6, 7) != 49)
        return 11;
    d = xr_mix_f64(1.25, 2.0);
    if (fabs(d - 4.5) > 0.000001)
        return 12;
    if (xr_read_i32_ptr(&value) != 10)
        return 13;
    if (xr_write_i32_ptr(&value, 77) != 77)
        return 14;
    if (value != 77)
        return 15;
    puts("ok");
    return 0;
}
C

if [ -f "$GEN_OBJ" ] &&
    cc -O2 -Wall "$CALLER_C" "$GEN_OBJ" -lm -o "$CALLER_BIN" >"$LINK_LOG" 2>&1; then
    record_pass "link C caller with exported symbols"
else
    record_fail "link C caller with exported symbols"
    sed 's/^/      /' "$LINK_LOG" | sed -n '1,160p'
fi

if [ -x "$CALLER_BIN" ]; then
    if "$CALLER_BIN" >"$RUN_LOG" 2>&1 && [ "$(cat "$RUN_LOG")" = "ok" ]; then
        record_pass "C caller invokes @c_export wrappers"
    else
        record_fail "C caller invokes @c_export wrappers"
        sed 's/^/      /' "$RUN_LOG" | sed -n '1,80p'
    fi
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0

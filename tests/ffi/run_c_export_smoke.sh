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
    if [ "${XRAY_FFI_KEEP_WORK:-0}" = "1" ]; then
        echo "Work dir: $WORK"
    else
        rm -rf "$WORK"
    fi
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
GEN_H="$WORK/c_export_link.h"
GEN_OBJ="$WORK/c_export_link.o"
CALLER_C="$WORK/caller.c"
CALLER_BIN="$WORK/caller"
CALLER_SHARED_BIN="$WORK/caller_shared"
GEN_LOG="$WORK/generate.log"
OBJ_LOG="$WORK/generated_obj.log"
LINK_LOG="$WORK/link.log"
RUN_LOG="$WORK/run.log"
SHARED_LOG="$WORK/shared.log"
SHARED_LINK_LOG="$WORK/shared_link.log"
SHARED_RUN_LOG="$WORK/shared_run.log"
MAIN_SRC="$WORK/c_export_main.xr"
MAIN_LIB_LOG="$WORK/c_export_main_shared.log"
AMALGAM_HELPER="$WORK/c_export_amalgam_helper.xr"
AMALGAM_SRC="$WORK/c_export_amalgam.xr"
AMALGAM_C="$WORK/c_export_amalgam.c"
AMALGAM_OBJ="$WORK/c_export_amalgam.o"
AMALGAM_LOG="$WORK/c_export_amalgam.log"
AMALGAM_CROSS_C="$WORK/c_export_amalgam_x86_64.c"
AMALGAM_CROSS_OBJ="$WORK/c_export_amalgam_x86_64.o"
AMALGAM_CROSS_LOG="$WORK/c_export_amalgam_x86_64.log"
case "$(uname -s)" in
    Darwin)
        GEN_LIB="$WORK/libxray_c_export.dylib"
        MAIN_LIB="$WORK/libxray_c_export_main.dylib"
        SHARED_LINK_FLAG="-dynamiclib"
        ;;
    *)
        GEN_LIB="$WORK/libxray_c_export.so"
        MAIN_LIB="$WORK/libxray_c_export_main.so"
        SHARED_LINK_FLAG="-shared"
        ;;
esac
PAIR_TYPE=""
PAIR_TYPE_FOR_C="struct xray_missing_pair_type"
OUTER_TYPE=""
OUTER_TYPE_FOR_C="struct xray_missing_outer_type"
BYTES4_TYPE=""
BYTES4_TYPE_FOR_C="struct xray_missing_bytes4_type"
PACKED_TYPE=""
PACKED_TYPE_FOR_C="struct xray_missing_packed_type"
ALIGNED_TYPE=""
ALIGNED_TYPE_FOR_C="struct xray_missing_aligned_type"
PACKED_ALIGNED_TYPE=""
PACKED_ALIGNED_TYPE_FOR_C="struct xray_missing_packed_aligned_type"

cat >"$SRC" <<'XR'
struct Pair {
    a: int32
    b: int32
}

struct Inner {
    x: int32
}

struct Outer {
    inner: Inner
    y: int32
}

struct Bytes4 {
    data: [uint8; 4]
    bias: int32
}

packed struct PackedPair {
    tag: uint8
    value: uint32
}

struct AlignedWord align(16) {
    value: int32
}

packed struct PackedAligned align(16) {
    tag: uint8
    value: uint32
}

const XR_C_EXPORT_SECRET: [byte; 4] = comptime [40, 2, 7, 9]

@c_export("xr_add_i32")
fn add_i32(a: int32, b: int32) -> int32 {
    return a + b
}

@c_export("xr_top_const_sum")
fn top_const_sum() -> int32 {
    return XR_C_EXPORT_SECRET[0] as int32 + XR_C_EXPORT_SECRET[1] as int32
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
fn read_i32_ptr(p: Ptr<int32>) -> int32 {
    unsafe {
        return p[0]
    }
}

@c_export("xr_write_i32_ptr")
fn write_i32_ptr(p: MutPtr<int32>, v: int32) -> int32 {
    unsafe {
        p[0] = v
        return p[0]
    }
}

@c_export("xr_pair_sum")
fn pair_sum(p: Pair) -> int32 {
    return p.a + p.b
}

@c_export("xr_pair_make")
fn pair_make(a: int32, b: int32) -> Pair {
    return Pair{a: a, b: b}
}

@c_export("xr_outer_sum")
fn outer_sum(p: Outer) -> int32 {
    return p.inner.x + p.y
}

@c_export("xr_outer_make")
fn outer_make(x: int32, y: int32) -> Outer {
    return Outer{inner: Inner{x: x}, y: y}
}

@c_export("xr_bytes4_sum")
fn bytes4_sum(p: Bytes4) -> int32 {
    return p.data[0] as int32 + p.data[1] as int32 + p.bias
}

@c_export("xr_bytes4_make")
fn bytes4_make(a: uint8, b: uint8, c: uint8, d: uint8, bias: int32) -> Bytes4 {
    return Bytes4{data: [a, b, c, d], bias: bias}
}

@c_export("xr_packed_sum")
fn packed_sum(p: PackedPair) -> int32 {
    return p.tag as int32 + p.value as int32
}

@c_export("xr_packed_make")
fn packed_make(tag: uint8, value: uint32) -> PackedPair {
    return PackedPair{tag: tag, value: value}
}

@c_export("xr_aligned_sum")
fn aligned_sum(p: AlignedWord) -> int32 {
    return p.value
}

@c_export("xr_aligned_make")
fn aligned_make(value: int32) -> AlignedWord {
    return AlignedWord{value: value}
}

@c_export("xr_packed_aligned_sum")
fn packed_aligned_sum(p: PackedAligned) -> int32 {
    return p.tag as int32 + p.value as int32
}

@c_export("xr_packed_aligned_make")
fn packed_aligned_make(tag: uint8, value: uint32) -> PackedAligned {
    return PackedAligned{tag: tag, value: value}
}

print(add_i32(19, 23))
XR

if "$XRAY" build --native -c --c-header "$GEN_H" -o "$GEN_C" "$SRC" >"$GEN_LOG" 2>&1; then
    record_pass "generate native C source and export header"
else
    record_fail "generate native C source and export header"
    sed 's/^/      /' "$GEN_LOG" | sed -n '1,120p'
fi

if [ -f "$GEN_H" ]; then
    PAIR_TYPE=$(awk '/xr_pair_make/ { print $1; exit }' "$GEN_H")
    OUTER_TYPE=$(awk '/xr_outer_make/ { print $1; exit }' "$GEN_H")
    BYTES4_TYPE=$(awk '/xr_bytes4_make/ { print $1; exit }' "$GEN_H")
    PACKED_TYPE=$(awk '/xr_packed_make/ { print $1; exit }' "$GEN_H")
    ALIGNED_TYPE=$(awk '/xr_aligned_make/ { print $1; exit }' "$GEN_H")
    PACKED_ALIGNED_TYPE=$(awk '/xr_packed_aligned_make/ { print $1; exit }' "$GEN_H")
    if [ -n "$PAIR_TYPE" ] && grep -Fq "typedef struct $PAIR_TYPE {" "$GEN_H"; then
        case "$PAIR_TYPE" in
            xrt_struct_*)
                PAIR_TYPE_FOR_C="$PAIR_TYPE"
                record_pass "header exposes fixed-layout struct typedef"
                ;;
            *)
                record_fail "header exposes fixed-layout struct typedef"
                sed 's/^/      /' "$GEN_H" | sed -n '1,120p'
                ;;
        esac
    else
        record_fail "header exposes fixed-layout struct typedef"
        sed 's/^/      /' "$GEN_H" | sed -n '1,120p'
    fi
    if [ -n "$OUTER_TYPE" ] && grep -Fq "typedef struct $OUTER_TYPE {" "$GEN_H"; then
        case "$OUTER_TYPE" in
            xrt_struct_*)
                OUTER_TYPE_FOR_C="$OUTER_TYPE"
                record_pass "header exposes nested fixed-layout struct typedef"
                ;;
            *)
                record_fail "header exposes nested fixed-layout struct typedef"
                sed 's/^/      /' "$GEN_H" | sed -n '1,120p'
                ;;
        esac
    else
        record_fail "header exposes nested fixed-layout struct typedef"
        sed 's/^/      /' "$GEN_H" | sed -n '1,120p'
    fi
    if [ -n "$BYTES4_TYPE" ] && grep -Fq "typedef struct $BYTES4_TYPE {" "$GEN_H"; then
        case "$BYTES4_TYPE" in
            xrt_struct_*)
                BYTES4_TYPE_FOR_C="$BYTES4_TYPE"
                record_pass "header exposes fixed-array fixed-layout struct typedef"
                ;;
            *)
                record_fail "header exposes fixed-array fixed-layout struct typedef"
                sed 's/^/      /' "$GEN_H" | sed -n '1,120p'
                ;;
        esac
    else
        record_fail "header exposes fixed-array fixed-layout struct typedef"
        sed 's/^/      /' "$GEN_H" | sed -n '1,120p'
    fi
    if [ -n "$PACKED_TYPE" ] && grep -Fq "typedef struct __attribute__((packed)) $PACKED_TYPE {" "$GEN_H"; then
        case "$PACKED_TYPE" in
            xrt_struct_*)
                PACKED_TYPE_FOR_C="$PACKED_TYPE"
                record_pass "header exposes packed struct typedef"
                ;;
            *)
                record_fail "header exposes packed struct typedef"
                sed 's/^/      /' "$GEN_H" | sed -n '1,160p'
                ;;
        esac
    else
        record_fail "header exposes packed struct typedef"
        sed 's/^/      /' "$GEN_H" | sed -n '1,160p'
    fi
    if [ -n "$ALIGNED_TYPE" ] && grep -Fq "typedef struct __attribute__((aligned(16))) $ALIGNED_TYPE {" "$GEN_H"; then
        case "$ALIGNED_TYPE" in
            xrt_struct_*)
                ALIGNED_TYPE_FOR_C="$ALIGNED_TYPE"
                record_pass "header exposes aligned fixed-layout struct typedef"
                ;;
            *)
                record_fail "header exposes aligned fixed-layout struct typedef"
                sed 's/^/      /' "$GEN_H" | sed -n '1,160p'
                ;;
        esac
    else
        record_fail "header exposes aligned fixed-layout struct typedef"
        sed 's/^/      /' "$GEN_H" | sed -n '1,160p'
    fi
    if [ -n "$PACKED_ALIGNED_TYPE" ] && grep -Fq "typedef struct __attribute__((packed, aligned(16))) $PACKED_ALIGNED_TYPE {" "$GEN_H"; then
        case "$PACKED_ALIGNED_TYPE" in
            xrt_struct_*)
                PACKED_ALIGNED_TYPE_FOR_C="$PACKED_ALIGNED_TYPE"
                record_pass "header exposes packed aligned struct typedef"
                ;;
            *)
                record_fail "header exposes packed aligned struct typedef"
                sed 's/^/      /' "$GEN_H" | sed -n '1,160p'
                ;;
        esac
    else
        record_fail "header exposes packed aligned struct typedef"
        sed 's/^/      /' "$GEN_H" | sed -n '1,160p'
    fi
fi

if [ -f "$GEN_C" ]; then
    expect_file_contains "$GEN_C" "int32_t xr_add_i32(int32_t p0, int32_t p1);" \
        "declares int32 export"
    expect_file_contains "$GEN_C" "int64_t xr_mix_i64(int64_t p0, int64_t p1);" \
        "declares int64 export"
    expect_file_contains "$GEN_C" "double xr_mix_f64(double p0, double p1);" \
        "declares float64 export"
    expect_file_contains "$GEN_C" "int32_t xr_read_i32_ptr(const void * p0);" \
        "declares Ptr export"
    expect_file_contains "$GEN_C" "int32_t xr_write_i32_ptr(void * p0, int32_t p1);" \
        "declares MutPtr export"
    if [ -n "$PAIR_TYPE" ]; then
        expect_file_contains "$GEN_C" "int32_t xr_pair_sum($PAIR_TYPE p0);" \
            "declares fixed-layout struct parameter export"
        expect_file_contains "$GEN_C" "$PAIR_TYPE xr_pair_make(int32_t p0, int32_t p1);" \
            "declares fixed-layout struct return export"
    fi
    if [ -n "$OUTER_TYPE" ]; then
        expect_file_contains "$GEN_C" "int32_t xr_outer_sum($OUTER_TYPE p0);" \
            "declares nested fixed-layout struct parameter export"
        expect_file_contains "$GEN_C" "$OUTER_TYPE xr_outer_make(int32_t p0, int32_t p1);" \
            "declares nested fixed-layout struct return export"
    fi
    if [ -n "$BYTES4_TYPE" ]; then
        expect_file_contains "$GEN_C" "int32_t xr_bytes4_sum($BYTES4_TYPE p0);" \
            "declares fixed-array fixed-layout struct parameter export"
        expect_file_contains "$GEN_C" \
            "$BYTES4_TYPE xr_bytes4_make(uint8_t p0, uint8_t p1, uint8_t p2, uint8_t p3, int32_t p4);" \
            "declares fixed-array fixed-layout struct return export"
    fi
    if [ -n "$PACKED_TYPE" ]; then
        expect_file_contains "$GEN_C" "int32_t xr_packed_sum($PACKED_TYPE p0);" \
            "declares packed struct parameter export"
        expect_file_contains "$GEN_C" "$PACKED_TYPE xr_packed_make(uint8_t p0, uint32_t p1);" \
            "declares packed struct return export"
    fi
    if [ -n "$ALIGNED_TYPE" ]; then
        expect_file_contains "$GEN_C" "int32_t xr_aligned_sum($ALIGNED_TYPE p0);" \
            "declares aligned fixed-layout struct parameter export"
        expect_file_contains "$GEN_C" "$ALIGNED_TYPE xr_aligned_make(int32_t p0);" \
            "declares aligned fixed-layout struct return export"
    fi
    if [ -n "$PACKED_ALIGNED_TYPE" ]; then
        expect_file_contains "$GEN_C" "int32_t xr_packed_aligned_sum($PACKED_ALIGNED_TYPE p0);" \
            "declares packed aligned struct parameter export"
        expect_file_contains "$GEN_C" "$PACKED_ALIGNED_TYPE xr_packed_aligned_make(uint8_t p0, uint32_t p1);" \
            "declares packed aligned struct return export"
    fi
    expect_file_not_contains "$GEN_C" "static int32_t xr_add_i32" \
        "int32 export is public"
fi

if [ -f "$GEN_H" ]; then
    expect_file_contains "$GEN_H" "#ifndef XRAY_AOT_C_EXPORTS_H" \
        "header has include guard"
    expect_file_contains "$GEN_H" "#ifdef __cplusplus" \
        "header is C++ friendly"
    expect_file_contains "$GEN_H" "int32_t xr_add_i32(int32_t p0, int32_t p1);" \
        "header declares int32 export"
    expect_file_contains "$GEN_H" "int64_t xr_mix_i64(int64_t p0, int64_t p1);" \
        "header declares int64 export"
    expect_file_contains "$GEN_H" "double xr_mix_f64(double p0, double p1);" \
        "header declares float64 export"
    expect_file_contains "$GEN_H" "int32_t xr_read_i32_ptr(const void * p0);" \
        "header declares Ptr export"
    expect_file_contains "$GEN_H" "int32_t xr_write_i32_ptr(void * p0, int32_t p1);" \
        "header declares MutPtr export"
    if [ -n "$PAIR_TYPE" ]; then
        expect_file_contains "$GEN_H" "int32_t xr_pair_sum($PAIR_TYPE p0);" \
            "header declares fixed-layout struct parameter export"
        expect_file_contains "$GEN_H" "$PAIR_TYPE xr_pair_make(int32_t p0, int32_t p1);" \
            "header declares fixed-layout struct return export"
    fi
    if [ -n "$OUTER_TYPE" ]; then
        expect_file_contains "$GEN_H" "int32_t xr_outer_sum($OUTER_TYPE p0);" \
            "header declares nested fixed-layout struct parameter export"
        expect_file_contains "$GEN_H" "$OUTER_TYPE xr_outer_make(int32_t p0, int32_t p1);" \
            "header declares nested fixed-layout struct return export"
    fi
    if [ -n "$BYTES4_TYPE" ]; then
        expect_file_contains "$GEN_H" "int32_t xr_bytes4_sum($BYTES4_TYPE p0);" \
            "header declares fixed-array fixed-layout struct parameter export"
        expect_file_contains "$GEN_H" \
            "$BYTES4_TYPE xr_bytes4_make(uint8_t p0, uint8_t p1, uint8_t p2, uint8_t p3, int32_t p4);" \
            "header declares fixed-array fixed-layout struct return export"
    fi
    if [ -n "$PACKED_TYPE" ]; then
        expect_file_contains "$GEN_H" "int32_t xr_packed_sum($PACKED_TYPE p0);" \
            "header declares packed struct parameter export"
        expect_file_contains "$GEN_H" "$PACKED_TYPE xr_packed_make(uint8_t p0, uint32_t p1);" \
            "header declares packed struct return export"
    fi
    if [ -n "$ALIGNED_TYPE" ]; then
        expect_file_contains "$GEN_H" "int32_t xr_aligned_sum($ALIGNED_TYPE p0);" \
            "header declares aligned fixed-layout struct parameter export"
        expect_file_contains "$GEN_H" "$ALIGNED_TYPE xr_aligned_make(int32_t p0);" \
            "header declares aligned fixed-layout struct return export"
    fi
    if [ -n "$PACKED_ALIGNED_TYPE" ]; then
        expect_file_contains "$GEN_H" "int32_t xr_packed_aligned_sum($PACKED_ALIGNED_TYPE p0);" \
            "header declares packed aligned struct parameter export"
        expect_file_contains "$GEN_H" "$PACKED_ALIGNED_TYPE xr_packed_aligned_make(uint8_t p0, uint32_t p1);" \
            "header declares packed aligned struct return export"
    fi
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

cat >"$CALLER_C" <<C
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

#include "c_export_link.h"

_Static_assert(sizeof($PACKED_TYPE_FOR_C) == 5, "packed size");
_Static_assert(offsetof($PACKED_TYPE_FOR_C, value) == 1, "packed offset");
_Static_assert(sizeof($ALIGNED_TYPE_FOR_C) == 16, "aligned size");
_Static_assert(_Alignof($ALIGNED_TYPE_FOR_C) == 16, "aligned alignment");
_Static_assert(sizeof($PACKED_ALIGNED_TYPE_FOR_C) == 16, "packed aligned size");
_Static_assert(_Alignof($PACKED_ALIGNED_TYPE_FOR_C) == 16, "packed aligned alignment");
_Static_assert(offsetof($PACKED_ALIGNED_TYPE_FOR_C, value) == 1, "packed aligned offset");

int main(void) {
    double d;
    int32_t value = 10;
    if (xr_add_i32(40, 2) != 42)
        return 10;
#if defined(XR_SHARED_CALLER)
    if (xr_top_const_sum() != 42)
        return 23;
#endif
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
    $PAIR_TYPE_FOR_C pair = xr_pair_make(20, 22);
    if (xr_pair_sum(pair) != 42)
        return 16;
    $OUTER_TYPE_FOR_C outer = xr_outer_make(35, 7);
    if (xr_outer_sum(outer) != 42)
        return 17;
    $BYTES4_TYPE_FOR_C bytes = xr_bytes4_make(10, 20, 30, 40, 7);
    if (xr_bytes4_sum(bytes) != 37)
        return 18;
    if (bytes.data[2] != 30 || bytes.bias != 7)
        return 19;
    $PACKED_TYPE_FOR_C packed = xr_packed_make(5, 37);
    if (xr_packed_sum(packed) != 42)
        return 20;
    $ALIGNED_TYPE_FOR_C aligned = xr_aligned_make(42);
    if (xr_aligned_sum(aligned) != 42)
        return 21;
    $PACKED_ALIGNED_TYPE_FOR_C packed_aligned = xr_packed_aligned_make(6, 36);
    if (xr_packed_aligned_sum(packed_aligned) != 42)
        return 22;
    puts("ok");
    return 0;
}
C

if "$XRAY" build --native --shared --dump-link-command --c-header "$GEN_H" -o "$GEN_LIB" \
    "$SRC" >"$SHARED_LOG" 2>&1; then
    record_pass "build native shared library"
else
    record_fail "build native shared library"
    sed 's/^/      /' "$SHARED_LOG" | sed -n '1,160p'
fi

expect_file_contains "$SHARED_LOG" "$SHARED_LINK_FLAG" \
    "shared build uses platform dynamic-library link flag"

if [ -f "$GEN_LIB" ]; then
    record_pass "shared library artifact exists"
else
    record_fail "shared library artifact exists"
fi

if [ -f "$GEN_OBJ" ] &&
    cc -O2 -Wall -I "$WORK" "$CALLER_C" "$GEN_OBJ" -lm -o "$CALLER_BIN" >"$LINK_LOG" 2>&1; then
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

CALLER_SHARED_SAN_FLAGS=""
if grep -Fq -- "-fsanitize=address" "$SHARED_LOG"; then
    if grep -Fq -- "-fsanitize=undefined" "$SHARED_LOG"; then
        CALLER_SHARED_SAN_FLAGS="-fsanitize=address,undefined"
    else
        CALLER_SHARED_SAN_FLAGS="-fsanitize=address"
    fi
elif grep -Fq -- "-fsanitize=undefined" "$SHARED_LOG"; then
    CALLER_SHARED_SAN_FLAGS="-fsanitize=undefined"
fi

if [ -f "$GEN_LIB" ] &&
    cc -O2 -Wall -DXR_SHARED_CALLER -I "$WORK" \
        ${CALLER_SHARED_SAN_FLAGS:+$CALLER_SHARED_SAN_FLAGS} "$CALLER_C" "$GEN_LIB" \
        -Wl,-rpath,"$WORK" -lm \
        -o "$CALLER_SHARED_BIN" >"$SHARED_LINK_LOG" 2>&1; then
    record_pass "link C caller with shared library"
else
    record_fail "link C caller with shared library"
    sed 's/^/      /' "$SHARED_LINK_LOG" | sed -n '1,160p'
fi

if [ -x "$CALLER_SHARED_BIN" ]; then
    if "$CALLER_SHARED_BIN" >"$SHARED_RUN_LOG" 2>&1 &&
        [ "$(cat "$SHARED_RUN_LOG")" = "42
ok" ]; then
        record_pass "shared library initializes top-level aggregates before C exports"
    else
        record_fail "C caller invokes shared-library exports"
        sed 's/^/      /' "$SHARED_RUN_LOG" | sed -n '1,80p'
    fi
fi

cat >"$MAIN_SRC" <<'XR'
@c_export("main")
fn exported_main() -> int32 {
    return 0
}
XR

if "$XRAY" build --native --shared -o "$MAIN_LIB" "$MAIN_SRC" >"$MAIN_LIB_LOG" 2>&1; then
    record_pass "shared library permits exported main symbol"
else
    record_fail "shared library permits exported main symbol"
    sed 's/^/      /' "$MAIN_LIB_LOG" | sed -n '1,160p'
fi

cat >"$AMALGAM_HELPER" <<'XR'
export struct AmalgamPair {
    left: int32
    right: int32
}

export fn makeAmalgamPair(left: int32, right: int32) -> AmalgamPair {
    return AmalgamPair{left: left, right: right}
}
XR

cat >"$AMALGAM_SRC" <<'XR'
import { AmalgamPair, makeAmalgamPair } from "./c_export_amalgam_helper"

@c_export("xr_amalgam_sum")
fn amalgamSum(left: int32, right: int32) -> int32 {
    const pair: AmalgamPair = makeAmalgamPair(left, right)
    return pair.left + pair.right
}
XR

if "$XRAY" build --native --shared --c-only -o "$AMALGAM_C" "$AMALGAM_SRC" \
        >"$AMALGAM_LOG" 2>&1 &&
    cc -O2 -fPIC -I "$PROJECT_DIR/src/aot" -I "$PROJECT_DIR/src/runtime" \
        -c "$AMALGAM_C" -o "$AMALGAM_OBJ" >>"$AMALGAM_LOG" 2>&1; then
    record_pass "multi-module shared C-only output is a compilable amalgamation"
else
    record_fail "multi-module shared C-only output is a compilable amalgamation"
    sed 's/^/      /' "$AMALGAM_LOG" | sed -n '1,160p'
fi

if command -v zig >/dev/null 2>&1; then
    if "$XRAY" build --native --shared --c-only --target x86_64-linux-musl \
            --toolchain zig -o "$AMALGAM_CROSS_C" "$AMALGAM_SRC" \
            >"$AMALGAM_CROSS_LOG" 2>&1 &&
        zig cc -target x86_64-linux-musl -O2 -fPIC -I "$PROJECT_DIR/src/aot" \
            -I "$PROJECT_DIR/src/runtime" -c "$AMALGAM_CROSS_C" \
            -o "$AMALGAM_CROSS_OBJ" >>"$AMALGAM_CROSS_LOG" 2>&1; then
        record_pass "cross-target shared C-only amalgamation compiles with Zig"
    else
        record_fail "cross-target shared C-only amalgamation compiles with Zig"
        sed 's/^/      /' "$AMALGAM_CROSS_LOG" | sed -n '1,160p'
    fi
else
    echo "  SKIP: cross-target shared C-only amalgamation (zig unavailable)"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0

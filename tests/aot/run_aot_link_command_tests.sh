#!/bin/bash
# AOT link-command tests (115 stdlib-neutral ABI / symbol-level linking).
#
# The link manifest is the source of truth, but this smoke verifies the native
# build driver actually obeys it: core math direct calls stay freestanding, while
# runtime-backed stdlib modules link only their capability runtime archive.
#
# By default the large link matrix uses `xray build --dry-run-link`: it exercises
# the real AOT/link planning path and validates the resolved link command without
# paying for a native compile/link for every case. A small real-link smoke at the
# end still proves the generated commands are executable. Set
# XRAY_LINK_COMMAND_REAL_BUILDS=1 to force the old full native-build matrix.
#
# Environment:
#   XRAY_LINK_COMMAND_PLAN_CACHE_DIR
#       persistent dry-run link-command log cache
#   XRAY_LINK_COMMAND_BUILD_CACHE_DIR
#       persistent AOT object cache for real smoke builds
#   XRAY_LINK_COMMAND_BIN_CACHE_DIR
#       persistent native binary cache for real smoke builds
#   XRAY_TEST_DISABLE_RUN_CACHE=1
#       bypass persistent plan/binary caches

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$PROJECT_DIR/tests/test_common.sh"
XRAY="${1:-${XRAY_BIN:-$PROJECT_DIR/build/xray}}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_aot_linkcmd.XXXXXX")" || {
    echo "FAIL: cannot create temporary directory" >&2
    exit 1
}
WORK_CACHE="$WORK/.cache"
PLAN_CACHE="${XRAY_LINK_COMMAND_PLAN_CACHE_DIR:-$(xray_test_stable_cache_dir "$PROJECT_DIR" "aot-link-command-plan" "$XRAY")}"
BUILD_CACHE="${XRAY_LINK_COMMAND_BUILD_CACHE_DIR:-$(xray_test_shared_cache_dir "$PROJECT_DIR" "aot-link-command-objects")}"
BIN_CACHE="${XRAY_LINK_COMMAND_BIN_CACHE_DIR:-$(xray_test_stable_cache_dir "$PROJECT_DIR" "aot-link-command-bin" "$XRAY")}"
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

build_native() {
    local src="$1"
    local out="$2"
    local log="$3"
    if [ "${XRAY_LINK_COMMAND_REAL_BUILDS:-0}" = "1" ]; then
        build_native_real "$src" "$out" "$log"
    else
        build_native_plan "$src" "$out" "$log"
    fi
}

build_native_plan() {
    local src="$1"
    local out="$2"
    local log="$3"
    local rel safe key plan_dir cached tmp

    rel="${src#"$PROJECT_DIR"/}"
    safe="$(printf '%s' "${rel%.xr}" | sed 's#[^A-Za-z0-9_.-]#_#g')"
    key="$(xray_test_case_dir_key "$src")"
    plan_dir="$PLAN_CACHE/$safe-$key"
    cached="$plan_dir/link.log"
    tmp="$plan_dir/link.log.$$"

    if [ "${XRAY_TEST_DISABLE_RUN_CACHE:-0}" != "1" ] && [ -f "$cached" ]; then
        cp "$cached" "$log"
        return 0
    fi

    mkdir -p "$plan_dir" || return 1
    if ! xray_test_lock_dir "$plan_dir.lock"; then
        printf 'cannot lock plan cache: %s\n' "$plan_dir.lock" >"$log"
        return 1
    fi
    if [ "${XRAY_TEST_DISABLE_RUN_CACHE:-0}" != "1" ] && [ -f "$cached" ]; then
        cp "$cached" "$log"
        xray_test_unlock_dir "$plan_dir.lock"
        return 0
    fi

    if "$XRAY" build --native --dry-run-link --dump-link-command --cache-dir "$BUILD_CACHE" -o "$out" \
            "$src" >"$tmp" 2>&1; then
        cp "$tmp" "$log"
        if [ "${XRAY_TEST_DISABLE_RUN_CACHE:-0}" != "1" ]; then
            mv "$tmp" "$cached"
        else
            rm -f "$tmp"
        fi
        xray_test_unlock_dir "$plan_dir.lock"
        return 0
    fi

    cp "$tmp" "$log" 2>/dev/null || true
    rm -f "$tmp"
    xray_test_unlock_dir "$plan_dir.lock"
    return 1
}

build_native_real() {
    local src="$1"
    local out="$2"
    local log="$3"
    local rel safe key bin_dir cached cached_log tmp tmp_log

    case "$src" in
        "$PROJECT_DIR"/*) ;;
        *)
            "$XRAY" build --native --dump-link-command --cache-dir "$WORK_CACHE" -o "$out" "$src" \
                >"$log" 2>&1
            return $?
            ;;
    esac

    rel="${src#"$PROJECT_DIR"/}"
    safe="$(printf '%s' "${rel%.xr}" | sed 's#[^A-Za-z0-9_.-]#_#g')"
    key="$(xray_test_case_dir_key "$src")"
    bin_dir="$BIN_CACHE/$safe-$key"
    cached="$bin_dir/aot"
    cached_log="$bin_dir/build.log"
    tmp="$bin_dir/aot.$$"
    tmp_log="$bin_dir/build.log.$$"

    if [ "${XRAY_TEST_DISABLE_RUN_CACHE:-0}" != "1" ] && [ -x "$cached" ] && [ -f "$cached_log" ]; then
        cp "$cached" "$out"
        chmod +x "$out"
        cp "$cached_log" "$log"
        return 0
    fi

    mkdir -p "$bin_dir" || return 1
    if ! xray_test_lock_dir "$bin_dir.lock"; then
        printf 'cannot lock binary cache: %s\n' "$bin_dir.lock" >"$log"
        return 1
    fi
    if [ "${XRAY_TEST_DISABLE_RUN_CACHE:-0}" != "1" ] && [ -x "$cached" ] && [ -f "$cached_log" ]; then
        cp "$cached" "$out"
        chmod +x "$out"
        cp "$cached_log" "$log"
        xray_test_unlock_dir "$bin_dir.lock"
        return 0
    fi

    rm -f "$tmp" "$tmp_log"
    if "$XRAY" build --native --dump-link-command --cache-dir "$BUILD_CACHE" -o "$tmp" "$src" \
            >"$tmp_log" 2>&1; then
        cp "$tmp" "$out"
        chmod +x "$out"
        cp "$tmp_log" "$log"
        if [ "${XRAY_TEST_DISABLE_RUN_CACHE:-0}" != "1" ]; then
            mv "$tmp" "$cached"
            mv "$tmp_log" "$cached_log"
        else
            rm -f "$tmp" "$tmp_log"
        fi
        xray_test_unlock_dir "$bin_dir.lock"
        return 0
    fi

    cp "$tmp_log" "$log" 2>/dev/null || true
    rm -f "$tmp" "$tmp_log"
    xray_test_unlock_dir "$bin_dir.lock"
    return 1
}

expect_log_contains() {
    local log="$1"
    local needle="$2"
    local name="$3"
    if grep -Fq -- "$needle" "$log"; then
        record_pass "$name"
    else
        record_fail "$name"
        sed 's/^/      /' "$log" | sed -n '1,80p'
    fi
}

expect_log_not_contains() {
    local log="$1"
    local needle="$2"
    local name="$3"
    if grep -Fq -- "$needle" "$log"; then
        record_fail "$name"
        sed 's/^/      /' "$log" | sed -n '1,80p'
    else
        record_pass "$name"
    fi
}

expect_output() {
    local bin="$1"
    local want="$2"
    local name="$3"
    local got
    if [ ! -x "$bin" ] && [ "${XRAY_LINK_COMMAND_REAL_BUILDS:-0}" != "1" ]; then
        record_pass "$name (covered by real-link smoke)"
        return
    fi
    got="$("$bin" 2>/dev/null)"
    if [ "$got" = "$want" ]; then
        record_pass "$name"
    else
        record_fail "$name: output '$got' != '$want'"
    fi
}

echo "=== AOT Link Command Tests ==="
echo "Binary: $XRAY"
echo "PlanCache: $PLAN_CACHE"
echo "BuildCache: $BUILD_CACHE"
echo "BinCache: $BIN_CACHE"
echo ""

if [ ! -x "$XRAY" ]; then
    echo "FAIL: xray binary not executable: $XRAY" >&2
    exit 1
fi

FFI_LIB_SRC="$WORK/xrayffi_smoke.c"
FFI_LIB_EXT=".so"
FFI_LIB_CC_ARGS=(-shared -fPIC)
case "$(uname -s 2>/dev/null)" in
    Darwin)
        FFI_LIB_EXT=".dylib"
        ;;
esac
FFI_LIB="$WORK/libxrayffi_smoke$FFI_LIB_EXT"
cat > "$FFI_LIB_SRC" <<'C'
#include <stdint.h>

int32_t xrayffi_add1(int32_t x) {
    return x + 1;
}
C
if [ "$FFI_LIB_EXT" = ".dylib" ]; then
    FFI_LIB_CC_ARGS=(-dynamiclib -install_name "$FFI_LIB")
fi

FFI_SRC="$WORK/ffi_dylib_path.xr"
cat > "$FFI_SRC" <<XR
@extern("C") @dylib("$FFI_LIB") fn xrayffi_add1(x: int32) -> int32

print(unsafe { xrayffi_add1(41) })
XR
FFI_BIN="$WORK/ffi_dylib_path"
FFI_LOG="$WORK/ffi_dylib_path.log"
if [ "${XRAY_LINK_COMMAND_REAL_BUILDS:-0}" = "1" ] &&
        ! cc "${FFI_LIB_CC_ARGS[@]}" -o "$FFI_LIB" "$FFI_LIB_SRC" >"$WORK/ffi_lib_build.log" 2>&1; then
    record_fail "ffi-dylib: helper library build failed"
    sed 's/^/      /' "$WORK/ffi_lib_build.log" | sed -n '1,80p'
fi

if build_native "$FFI_SRC" "$FFI_BIN" "$FFI_LOG"; then
    expect_log_contains "$FFI_LOG" "Link command:" "ffi-dylib: emitted link command"
    expect_log_contains "$FFI_LOG" "$FFI_LIB" "ffi-dylib: links explicit dylib path"
    expect_log_not_contains "$FFI_LOG" "-lxray_core" "ffi-dylib: does not link xray_core"
    if [ "${XRAY_LINK_COMMAND_REAL_BUILDS:-0}" = "1" ]; then
        got="$(DYLD_LIBRARY_PATH="$WORK" LD_LIBRARY_PATH="$WORK" "$FFI_BIN" 2>/dev/null)"
        if [ "$got" = "42" ]; then
            record_pass "ffi-dylib: binary output"
        else
            record_fail "ffi-dylib: output '$got' != '42'"
        fi
    else
        expect_output "$FFI_BIN" "42" "ffi-dylib: binary output"
    fi
else
    record_fail "ffi-dylib: build failed"
    sed 's/^/      /' "$FFI_LOG" | sed -n '1,120p'
fi

CORE_SRC="$PROJECT_DIR/tests/aot/filetests/link/core_math_single_symbol.xr"
CORE_BIN="$WORK/core_math"
CORE_LOG="$WORK/core_math.log"
if build_native "$CORE_SRC" "$CORE_BIN" "$CORE_LOG"; then
    expect_log_contains "$CORE_LOG" "Link command:" "core-math: emitted link command"
    expect_log_not_contains "$CORE_LOG" "-lxray_core" "core-math: does not link xray_core"
    expect_log_not_contains "$CORE_LOG" "-lpthread" "core-math: does not link pthread"
    expect_log_not_contains "$CORE_LOG" "-lz" "core-math: does not link zlib"
    expect_log_contains "$CORE_LOG" "-lm" "core-math: links math lib only"
    expect_output "$CORE_BIN" "9.0" "core-math: binary output"
else
    record_fail "core-math: build failed"
    sed 's/^/      /' "$CORE_LOG" | sed -n '1,120p'
fi

FREESTANDING_EXPORT_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_export.xr"
FREESTANDING_EXPORT_BIN="$WORK/freestanding_export"
FREESTANDING_EXPORT_LOG="$WORK/freestanding_export.log"
if "$XRAY" build --native --profile freestanding --dry-run-link --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_EXPORT_BIN" \
        "$FREESTANDING_EXPORT_SRC" >"$FREESTANDING_EXPORT_LOG" 2>&1; then
    expect_log_contains "$FREESTANDING_EXPORT_LOG" "Link command:" \
        "freestanding-profile: emitted link command"
    expect_log_contains "$FREESTANDING_EXPORT_LOG" "-DXRAY_PROFILE_FREESTANDING=1" \
        "freestanding-profile: defines profile macro"
    expect_log_contains "$FREESTANDING_EXPORT_LOG" "-ffreestanding" \
        "freestanding-profile: passes freestanding compile flag"
    expect_log_contains "$FREESTANDING_EXPORT_LOG" "-nostdlib" \
        "freestanding-profile: passes nostdlib link flag"
    expect_log_not_contains "$FREESTANDING_EXPORT_LOG" "-lm" \
        "freestanding-profile: strips hosted math library"
    expect_log_not_contains "$FREESTANDING_EXPORT_LOG" "-lpthread" \
        "freestanding-profile: strips hosted pthread library"
    expect_log_not_contains "$FREESTANDING_EXPORT_LOG" "-lxray_aot_core" \
        "freestanding-profile: does not link AOT core stdlib"
else
    record_fail "freestanding-profile: build failed"
    sed 's/^/      /' "$FREESTANDING_EXPORT_LOG" | sed -n '1,120p'
fi

FREESTANDING_EXPORT_OBJ="$WORK/freestanding_export.o"
FREESTANDING_EXPORT_REAL_LOG="$WORK/freestanding_export_real.log"
if "$XRAY" build --native --profile freestanding --shared --keep-c --rebuild \
        --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_EXPORT_OBJ" \
        "$FREESTANDING_EXPORT_SRC" >"$FREESTANDING_EXPORT_REAL_LOG" 2>&1; then
    expect_log_contains "$FREESTANDING_EXPORT_REAL_LOG" "Link command:" \
        "freestanding-profile: real object emitted link command"
    expect_log_contains "$FREESTANDING_EXPORT_REAL_LOG" " -r " \
        "freestanding-profile: shared output is relocatable object"
    expect_log_contains "$FREESTANDING_EXPORT_REAL_LOG" "-nostdlib" \
        "freestanding-profile: real object links without libc"
    expect_log_not_contains "$FREESTANDING_EXPORT_REAL_LOG" "-dynamiclib" \
        "freestanding-profile: does not request hosted dylib"
    FREESTANDING_KEPT_C="$(sed -n 's/^Kept C source: //p' "$FREESTANDING_EXPORT_REAL_LOG" | tail -n 1)"
    if [ -f "$FREESTANDING_KEPT_C" ]; then
        expect_log_contains "$FREESTANDING_KEPT_C" "#include \"xrt_core_freestanding.h\"" \
            "freestanding-profile: generated C uses freestanding prelude"
        expect_log_not_contains "$FREESTANDING_KEPT_C" "#include \"xrt.h\"" \
            "freestanding-profile: generated C avoids hosted xrt umbrella"
        expect_log_not_contains "$FREESTANDING_KEPT_C" "#include \"xaot_coro.h\"" \
            "freestanding-profile: generated C avoids coroutine bridge"
    else
        record_fail "freestanding-profile: kept C source missing"
        sed 's/^/      /' "$FREESTANDING_EXPORT_REAL_LOG" | sed -n '1,120p'
    fi
    FREESTANDING_UNDEFINED="$(nm -u "$FREESTANDING_EXPORT_OBJ" 2>&1 | sed '/^[[:space:]]*$/d')"
    if [ -z "$FREESTANDING_UNDEFINED" ]; then
        record_pass "freestanding-profile: no undefined symbols in relocatable object"
    else
        record_fail "freestanding-profile: unexpected undefined symbols"
        printf '%s\n' "$FREESTANDING_UNDEFINED" | sed 's/^/      /'
    fi
    if nm -g "$FREESTANDING_EXPORT_OBJ" 2>/dev/null | grep -Eq '(^|[[:space:]])_?xray_add$'; then
        record_pass "freestanding-profile: exports c symbol"
    else
        record_fail "freestanding-profile: missing c export symbol"
        nm -g "$FREESTANDING_EXPORT_OBJ" 2>/dev/null | sed 's/^/      /'
    fi
else
    record_fail "freestanding-profile: real object build failed"
    sed 's/^/      /' "$FREESTANDING_EXPORT_REAL_LOG" | sed -n '1,120p'
fi

FREESTANDING_NON_NATIVE_LOG="$WORK/freestanding_non_native.log"
if "$XRAY" build --profile freestanding -o "$WORK/freestanding_non_native" \
        "$FREESTANDING_EXPORT_SRC" >"$FREESTANDING_NON_NATIVE_LOG" 2>&1; then
    record_fail "freestanding-profile: requires native backend"
    sed 's/^/      /' "$FREESTANDING_NON_NATIVE_LOG" | sed -n '1,120p'
else
    expect_log_contains "$FREESTANDING_NON_NATIVE_LOG" "--profile freestanding requires --native" \
        "freestanding-profile: requires native backend"
fi

FREESTANDING_QUOTED_STDLIB_SRC="$WORK/freestanding_quoted_stdlib.xr"
cat > "$FREESTANDING_QUOTED_STDLIB_SRC" <<'XR'
import { now } from "time"

print(now())
XR
FREESTANDING_QUOTED_STDLIB_LOG="$WORK/freestanding_quoted_stdlib.log"
if "$XRAY" build --native --profile freestanding --dry-run-link --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$WORK/freestanding_quoted_stdlib" \
        "$FREESTANDING_QUOTED_STDLIB_SRC" >"$FREESTANDING_QUOTED_STDLIB_LOG" 2>&1; then
    record_fail "freestanding-profile: rejects quoted hosted stdlib imports"
    sed 's/^/      /' "$FREESTANDING_QUOTED_STDLIB_LOG" | sed -n '1,120p'
else
    expect_log_contains "$FREESTANDING_QUOTED_STDLIB_LOG" \
        "freestanding profile rejects stdlib module 'time'" \
        "freestanding-profile: rejects quoted hosted stdlib imports"
fi

CORE_FAST_BIN="$WORK/core_math_fast"
CORE_FAST_LOG="$WORK/core_math_fast.log"
case "$(uname -m 2>/dev/null)" in
    arm64|aarch64|arm*)
        CORE_FAST_CPU_FLAG="-mcpu=native"
        ;;
    *)
        CORE_FAST_CPU_FLAG="-march=native"
        ;;
esac
if "$XRAY" build --native -O fast --dry-run-link --dump-link-command --cache-dir "$WORK_CACHE" \
        -o "$CORE_FAST_BIN" "$CORE_SRC" >"$CORE_FAST_LOG" 2>&1; then
    expect_log_contains "$CORE_FAST_LOG" "Link command:" "core-math-fast: emitted link command"
    expect_log_contains "$CORE_FAST_LOG" "-O3" "core-math-fast: keeps semantic-safe O3"
    expect_log_contains "$CORE_FAST_LOG" "-flto" "core-math-fast: enables LTO"
    expect_log_contains "$CORE_FAST_LOG" "$CORE_FAST_CPU_FLAG" \
        "core-math-fast: tunes for native CPU"
    expect_log_not_contains "$CORE_FAST_LOG" "-lxray_core" "core-math-fast: does not link xray_core"
else
    record_fail "core-math-fast: build failed"
    sed 's/^/      /' "$CORE_FAST_LOG" | sed -n '1,120p'
fi

CORE_FULL_SRC="$PROJECT_DIR/tests/aot/filetests/link/core_math.xr"
CORE_FULL_BIN="$WORK/core_math_full"
CORE_FULL_LOG="$WORK/core_math_full.log"
if build_native "$CORE_FULL_SRC" "$CORE_FULL_BIN" "$CORE_FULL_LOG"; then
    expect_log_contains "$CORE_FULL_LOG" "Link command:" "core-math-full: emitted link command"
    expect_log_not_contains "$CORE_FULL_LOG" "-lxray_core" "core-math-full: does not link xray_core"
    expect_log_not_contains "$CORE_FULL_LOG" "-lpthread" "core-math-full: does not link pthread"
    expect_log_not_contains "$CORE_FULL_LOG" "-lz" "core-math-full: does not link zlib"
    expect_log_contains "$CORE_FULL_LOG" "-lm" "core-math-full: links math lib only"
    expect_output "$CORE_FULL_BIN" $'9.0\n8.0\n81.0\n7\n1\n2\n2\n1\n0.0\n1.0\n0.0\n0.0\n0.0\n0.0\n0.0\n0.0\n2.0\n3.0\n1.0\n0.0\n1.0\n0.0\n5.0\n3.0\n3.0\n0.0\n0.0\n2.5\n0.0\n0.0\n-1\ntrue\ntrue\nfalse\n3\n7\n5' "core-math-full: binary output"
else
    record_fail "core-math-full: build failed"
    sed 's/^/      /' "$CORE_FULL_LOG" | sed -n '1,120p'
fi

RANDOM_MATH_SRC="$PROJECT_DIR/tests/aot/filetests/link/system_math_random.xr"
RANDOM_MATH_BIN="$WORK/system_math_random"
RANDOM_MATH_LOG="$WORK/system_math_random.log"
if build_native "$RANDOM_MATH_SRC" "$RANDOM_MATH_BIN" "$RANDOM_MATH_LOG"; then
    expect_log_contains "$RANDOM_MATH_LOG" "Link command:" "system-math-random: emitted link command"
    expect_log_not_contains "$RANDOM_MATH_LOG" "-lxray_core" "system-math-random: does not link xray_core"
    expect_log_not_contains "$RANDOM_MATH_LOG" "-lpthread" "system-math-random: does not link pthread"
    expect_log_not_contains "$RANDOM_MATH_LOG" "-lz" "system-math-random: does not link zlib"
    expect_log_contains "$RANDOM_MATH_LOG" "-lxray_aot_core" "system-math-random: links AOT core stdlib only"
    expect_log_contains "$RANDOM_MATH_LOG" "-lm" "system-math-random: links math lib"
    expect_output "$RANDOM_MATH_BIN" $'true\n7' "system-math-random: binary output"
else
    record_fail "system-math-random: build failed"
    sed 's/^/      /' "$RANDOM_MATH_LOG" | sed -n '1,120p'
fi

RANDOM_CRYPTO_SRC="$PROJECT_DIR/tests/aot/filetests/link/system_crypto_random.xr"
RANDOM_CRYPTO_BIN="$WORK/system_crypto_random"
RANDOM_CRYPTO_LOG="$WORK/system_crypto_random.log"
if build_native "$RANDOM_CRYPTO_SRC" "$RANDOM_CRYPTO_BIN" "$RANDOM_CRYPTO_LOG"; then
    expect_log_contains "$RANDOM_CRYPTO_LOG" "Link command:" "system-crypto-random: emitted link command"
    expect_log_not_contains "$RANDOM_CRYPTO_LOG" "-lxray_core" "system-crypto-random: does not link xray_core"
    expect_log_not_contains "$RANDOM_CRYPTO_LOG" "-lpthread" "system-crypto-random: does not link pthread"
    expect_log_not_contains "$RANDOM_CRYPTO_LOG" "-lz" "system-crypto-random: does not link zlib"
    expect_log_not_contains "$RANDOM_CRYPTO_LOG" "-lssl" "system-crypto-random: does not link ssl"
    expect_log_not_contains "$RANDOM_CRYPTO_LOG" "-lcrypto" "system-crypto-random: does not link crypto"
    expect_log_contains "$RANDOM_CRYPTO_LOG" "-lxray_aot_core" "system-crypto-random: links AOT core stdlib only"
    expect_log_contains "$RANDOM_CRYPTO_LOG" "-lm" "system-crypto-random: links math lib"
    expect_output "$RANDOM_CRYPTO_BIN" $'true\ntrue\ntrue' "system-crypto-random: binary output"
else
    record_fail "system-crypto-random: build failed"
    sed 's/^/      /' "$RANDOM_CRYPTO_LOG" | sed -n '1,120p'
fi

TIME_QUERY_SRC="$PROJECT_DIR/tests/aot/filetests/link/system_time_queries.xr"
TIME_QUERY_BIN="$WORK/system_time_queries"
TIME_QUERY_LOG="$WORK/system_time_queries.log"
if build_native "$TIME_QUERY_SRC" "$TIME_QUERY_BIN" "$TIME_QUERY_LOG"; then
    expect_log_contains "$TIME_QUERY_LOG" "Link command:" "system-time-query: emitted link command"
    expect_log_not_contains "$TIME_QUERY_LOG" "-lxray_core" "system-time-query: does not link xray_core"
    expect_log_not_contains "$TIME_QUERY_LOG" "-lpthread" "system-time-query: does not link pthread"
    expect_log_not_contains "$TIME_QUERY_LOG" "-lz" "system-time-query: does not link zlib"
    expect_log_contains "$TIME_QUERY_LOG" "-lxray_aot_core" "system-time-query: links AOT core stdlib only"
    expect_log_contains "$TIME_QUERY_LOG" "-lm" "system-time-query: links math lib"
    expect_output "$TIME_QUERY_BIN" $'true\ntrue\ntrue\ntrue\ntrue' "system-time-query: binary output"
else
    record_fail "system-time-query: build failed"
    sed 's/^/      /' "$TIME_QUERY_LOG" | sed -n '1,120p'
fi

DATETIME_OFFSET_SRC="$PROJECT_DIR/tests/aot/filetests/link/system_datetime_offset.xr"
DATETIME_OFFSET_BIN="$WORK/system_datetime_offset"
DATETIME_OFFSET_LOG="$WORK/system_datetime_offset.log"
if build_native "$DATETIME_OFFSET_SRC" "$DATETIME_OFFSET_BIN" "$DATETIME_OFFSET_LOG"; then
    expect_log_contains "$DATETIME_OFFSET_LOG" "Link command:" "system-datetime-offset: emitted link command"
    expect_log_not_contains "$DATETIME_OFFSET_LOG" "-lxray_core" "system-datetime-offset: does not link xray_core"
    expect_log_not_contains "$DATETIME_OFFSET_LOG" "-lxray_aot_core" "system-datetime-offset: does not link AOT core"
    expect_log_not_contains "$DATETIME_OFFSET_LOG" "-lpthread" "system-datetime-offset: does not link pthread"
    expect_log_not_contains "$DATETIME_OFFSET_LOG" "-lz" "system-datetime-offset: does not link zlib"
    expect_log_contains "$DATETIME_OFFSET_LOG" "-lm" "system-datetime-offset: links math lib only"
    expect_output "$DATETIME_OFFSET_BIN" $'true\ntrue\ntrue' "system-datetime-offset: binary output"
else
    record_fail "system-datetime-offset: build failed"
    sed 's/^/      /' "$DATETIME_OFFSET_LOG" | sed -n '1,120p'
fi

CORE_DATETIME_SRC="$PROJECT_DIR/tests/aot/filetests/link/core_datetime.xr"
CORE_DATETIME_BIN="$WORK/core_datetime"
CORE_DATETIME_LOG="$WORK/core_datetime.log"
if build_native "$CORE_DATETIME_SRC" "$CORE_DATETIME_BIN" "$CORE_DATETIME_LOG"; then
    expect_log_contains "$CORE_DATETIME_LOG" "Link command:" "core-datetime: emitted link command"
    expect_log_not_contains "$CORE_DATETIME_LOG" "-lxray_core" "core-datetime: does not link xray_core"
    expect_log_not_contains "$CORE_DATETIME_LOG" "-lxray_aot_core" "core-datetime: does not link AOT core"
    expect_log_not_contains "$CORE_DATETIME_LOG" "-lpthread" "core-datetime: does not link pthread"
    expect_log_not_contains "$CORE_DATETIME_LOG" "-lz" "core-datetime: does not link zlib"
    expect_log_contains "$CORE_DATETIME_LOG" "-lm" "core-datetime: links math lib only"
    expect_output "$CORE_DATETIME_BIN" $'2024\n2024-02-29T23:45:06.000Z\n2024/02/29 23:45:06\n1\n-24\n1970-01-01T00:00:00.000Z\n999\n123' "core-datetime: binary output"
else
    record_fail "core-datetime: build failed"
    sed 's/^/      /' "$CORE_DATETIME_LOG" | sed -n '1,120p'
fi

OS_QUERY_SRC="$PROJECT_DIR/tests/aot/filetests/link/system_os_queries.xr"
OS_QUERY_BIN="$WORK/system_os_queries"
OS_QUERY_LOG="$WORK/system_os_queries.log"
if build_native "$OS_QUERY_SRC" "$OS_QUERY_BIN" "$OS_QUERY_LOG"; then
    expect_log_contains "$OS_QUERY_LOG" "Link command:" "system-os-query: emitted link command"
    expect_log_not_contains "$OS_QUERY_LOG" "-lxray_core" "system-os-query: does not link xray_core"
    expect_log_not_contains "$OS_QUERY_LOG" "-lxray_aot_core" "system-os-query: does not link AOT core"
    expect_log_not_contains "$OS_QUERY_LOG" "-lpthread" "system-os-query: does not link pthread"
    expect_log_not_contains "$OS_QUERY_LOG" "-lz" "system-os-query: does not link zlib"
    expect_log_contains "$OS_QUERY_LOG" "-lm" "system-os-query: links math lib only"
    expect_output "$OS_QUERY_BIN" $'true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue' "system-os-query: binary output"
else
    record_fail "system-os-query: build failed"
    sed 's/^/      /' "$OS_QUERY_LOG" | sed -n '1,120p'
fi

OS_EXEC_SRC="$PROJECT_DIR/tests/aot/filetests/link/system_os_exec.xr"
OS_EXEC_BIN="$WORK/system_os_exec"
OS_EXEC_LOG="$WORK/system_os_exec.log"
if build_native "$OS_EXEC_SRC" "$OS_EXEC_BIN" "$OS_EXEC_LOG"; then
    expect_log_contains "$OS_EXEC_LOG" "Link command:" "system-os-exec: emitted link command"
    expect_log_not_contains "$OS_EXEC_LOG" "-lxray_core" "system-os-exec: does not link xray_core"
    expect_log_not_contains "$OS_EXEC_LOG" "-lxray_aot_core" "system-os-exec: does not link AOT core"
    expect_log_not_contains "$OS_EXEC_LOG" "-lpthread" "system-os-exec: does not link pthread"
    expect_log_not_contains "$OS_EXEC_LOG" "-lz" "system-os-exec: does not link zlib"
    expect_log_contains "$OS_EXEC_LOG" "-lm" "system-os-exec: links math lib only"
    expect_output "$OS_EXEC_BIN" $'true\nabc\ntrue\n0\ntrue\ntrue\nerr\n7\ntrue\n0\n200000' "system-os-exec: binary output"
else
    record_fail "system-os-exec: build failed"
    sed 's/^/      /' "$OS_EXEC_LOG" | sed -n '1,120p'
fi

SYSTEM_IO_SRC="$PROJECT_DIR/tests/aot/filetests/link/system_io_basic.xr"
SYSTEM_IO_BIN="$WORK/system_io_basic"
SYSTEM_IO_LOG="$WORK/system_io_basic.log"
if build_native "$SYSTEM_IO_SRC" "$SYSTEM_IO_BIN" "$SYSTEM_IO_LOG"; then
    expect_log_contains "$SYSTEM_IO_LOG" "Link command:" "system-io-basic: emitted link command"
    expect_log_not_contains "$SYSTEM_IO_LOG" "-lxray_core" "system-io-basic: does not link xray_core"
    expect_log_not_contains "$SYSTEM_IO_LOG" "-lxray_aot_core" "system-io-basic: does not link AOT core"
    expect_log_not_contains "$SYSTEM_IO_LOG" "-lpthread" "system-io-basic: does not link pthread"
    expect_log_not_contains "$SYSTEM_IO_LOG" "-lz" "system-io-basic: does not link zlib"
    expect_log_not_contains "$SYSTEM_IO_LOG" "-lssl" "system-io-basic: does not link ssl"
    expect_log_not_contains "$SYSTEM_IO_LOG" "-lcrypto" "system-io-basic: does not link crypto"
    expect_log_contains "$SYSTEM_IO_LOG" "-lm" "system-io-basic: links math lib only"
    expect_output "$SYSTEM_IO_BIN" $'true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\n5\ntrue\ntrue\n2\nhello\nworld\ntrue\n3\n65\n0\n255\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue' "system-io-basic: binary output"
else
    record_fail "system-io-basic: build failed"
    sed 's/^/      /' "$SYSTEM_IO_LOG" | sed -n '1,120p'
fi

PATH_SRC="$PROJECT_DIR/tests/aot/filetests/link/core_path.xr"
PATH_BIN="$WORK/core_path"
PATH_LOG="$WORK/core_path.log"
if build_native "$PATH_SRC" "$PATH_BIN" "$PATH_LOG"; then
    expect_log_contains "$PATH_LOG" "Link command:" "core-path: emitted link command"
    expect_log_not_contains "$PATH_LOG" "-lxray_core" "core-path: does not link xray_core"
    expect_log_not_contains "$PATH_LOG" "-lpthread" "core-path: does not link pthread"
    expect_log_not_contains "$PATH_LOG" "-lz" "core-path: does not link zlib"
    expect_log_not_contains "$PATH_LOG" "-lssl" "core-path: does not link ssl"
    expect_log_not_contains "$PATH_LOG" "-lcrypto" "core-path: does not link crypto"
    expect_log_contains "$PATH_LOG" "-lm" "core-path: links math lib only"
    expect_output "$PATH_BIN" $'true\ntrue\ntrue\nxray\n/usr/local/bin\n.gz\n/usr/bin/xray\nbaz\n/foo\n.\n../lib\n../foobar\nbin\n..\nfoo/bar\nfoo/bar/baz\n/usr/local/bin\nfoo/bar\n/bar/baz\n/x\ntrue\nx\n/foo/bar\n/bar/baz\n/var\n/\n/\n/home/user\nfile.txt\nfile\n.txt\nfoo.txt\nfoo\n.txt\n/home/user/file.txt\n/home/user/file.txt\narchive.tar' "core-path: binary output"
else
    record_fail "core-path: build failed"
    sed 's/^/      /' "$PATH_LOG" | sed -n '1,120p'
fi

ENC_SRC="$PROJECT_DIR/tests/aot/filetests/link/core_encoding.xr"
ENC_BIN="$WORK/core_encoding"
ENC_LOG="$WORK/core_encoding.log"
if build_native "$ENC_SRC" "$ENC_BIN" "$ENC_LOG"; then
    expect_log_contains "$ENC_LOG" "Link command:" "core-encoding: emitted link command"
    expect_log_not_contains "$ENC_LOG" "-lxray_core" "core-encoding: does not link xray_core"
    expect_log_not_contains "$ENC_LOG" "-lpthread" "core-encoding: does not link pthread"
    expect_log_not_contains "$ENC_LOG" "-lz" "core-encoding: does not link zlib"
    expect_log_not_contains "$ENC_LOG" "-lssl" "core-encoding: does not link ssl"
    expect_log_not_contains "$ENC_LOG" "-lcrypto" "core-encoding: does not link crypto"
    expect_log_contains "$ENC_LOG" "-lm" "core-encoding: links math lib only"
    expect_output "$ENC_BIN" $'48656c6c6f\n5\n72\n111\nHello\ntrue\ntrue\n0\ntrue\nfalse\ntrue\n2\n6\n0\n1\n4\n65\n0\n66\n0\nAB\n0\n65\nAB\nAB\nA\nfalse\ntrue' "core-encoding: binary output"
else
    record_fail "core-encoding: build failed"
    sed 's/^/      /' "$ENC_LOG" | sed -n '1,120p'
fi

LOG_CONST_SRC="$PROJECT_DIR/tests/aot/filetests/link/core_log_constants.xr"
LOG_CONST_BIN="$WORK/core_log_constants"
LOG_CONST_LOG="$WORK/core_log_constants.log"
if build_native "$LOG_CONST_SRC" "$LOG_CONST_BIN" "$LOG_CONST_LOG"; then
    expect_log_contains "$LOG_CONST_LOG" "Link command:" "core-log-constants: emitted link command"
    expect_log_not_contains "$LOG_CONST_LOG" "-lxray_core" "core-log-constants: does not link xray_core"
    expect_log_not_contains "$LOG_CONST_LOG" "-lxray_aot_core" "core-log-constants: does not link AOT core"
    expect_log_not_contains "$LOG_CONST_LOG" "-lpthread" "core-log-constants: does not link pthread"
    expect_log_not_contains "$LOG_CONST_LOG" "-lz" "core-log-constants: does not link zlib"
    expect_log_not_contains "$LOG_CONST_LOG" "-lssl" "core-log-constants: does not link ssl"
    expect_log_not_contains "$LOG_CONST_LOG" "-lcrypto" "core-log-constants: does not link crypto"
    expect_log_contains "$LOG_CONST_LOG" "-lm" "core-log-constants: links math lib only"
    expect_output "$LOG_CONST_BIN" $'10\n20\n30\n40\n50\ntrue\ntrue' "core-log-constants: binary output"
else
    record_fail "core-log-constants: build failed"
    sed 's/^/      /' "$LOG_CONST_LOG" | sed -n '1,120p'
fi

BASE64_SRC="$PROJECT_DIR/tests/aot/filetests/link/core_base64.xr"
BASE64_BIN="$WORK/core_base64"
BASE64_LOG="$WORK/core_base64.log"
if build_native "$BASE64_SRC" "$BASE64_BIN" "$BASE64_LOG"; then
    expect_log_contains "$BASE64_LOG" "Link command:" "core-base64: emitted link command"
    expect_log_not_contains "$BASE64_LOG" "-lxray_core" "core-base64: does not link xray_core"
    expect_log_not_contains "$BASE64_LOG" "-lpthread" "core-base64: does not link pthread"
    expect_log_not_contains "$BASE64_LOG" "-lz" "core-base64: does not link zlib"
    expect_log_not_contains "$BASE64_LOG" "-lssl" "core-base64: does not link ssl"
    expect_log_not_contains "$BASE64_LOG" "-lcrypto" "core-base64: does not link crypto"
    expect_log_contains "$BASE64_LOG" "-lm" "core-base64: links math lib only"
    expect_output "$BASE64_BIN" $'SGVsbG8=\nHello\nQUI\nAB\nnull\ntrue\nSGVs\n3\n72\n101\n108\ntrue' "core-base64: binary output"
else
    record_fail "core-base64: build failed"
    sed 's/^/      /' "$BASE64_LOG" | sed -n '1,120p'
fi

URL_SRC="$PROJECT_DIR/tests/aot/filetests/link/core_url.xr"
URL_BIN="$WORK/core_url"
URL_LOG="$WORK/core_url.log"
if build_native "$URL_SRC" "$URL_BIN" "$URL_LOG"; then
    expect_log_contains "$URL_LOG" "Link command:" "core-url: emitted link command"
    expect_log_not_contains "$URL_LOG" "-lxray_core" "core-url: does not link xray_core"
    expect_log_not_contains "$URL_LOG" "-lpthread" "core-url: does not link pthread"
    expect_log_not_contains "$URL_LOG" "-lz" "core-url: does not link zlib"
    expect_log_not_contains "$URL_LOG" "-lssl" "core-url: does not link ssl"
    expect_log_not_contains "$URL_LOG" "-lcrypto" "core-url: does not link crypto"
    expect_log_contains "$URL_LOG" "-lm" "core-url: links math lib only"
    expect_output "$URL_BIN" $'hello%20%E4%B8%96%E7%95%8C%21\nhello 世界!\na+b%2Bc\na b+c\nhttps:\nexample.com\n8080\n/path\n?q=1\n#top\nexample.com:8080\nhttps://example.com:8080\nhttps://example.com:8080/path?q=1#top\nhello world\na&b\nmsg=hello+world&key=a%26b\nhttps://example.com/a/b/d.html\n/api/v1/users' "core-url: binary output"
else
    record_fail "core-url: build failed"
    sed 's/^/      /' "$URL_LOG" | sed -n '1,120p'
fi

COMPRESS_SRC="$PROJECT_DIR/tests/aot/filetests/link/core_compress.xr"
COMPRESS_BIN="$WORK/core_compress"
COMPRESS_LOG="$WORK/core_compress.log"
if build_native "$COMPRESS_SRC" "$COMPRESS_BIN" "$COMPRESS_LOG"; then
    expect_log_contains "$COMPRESS_LOG" "Link command:" "core-compress: emitted link command"
    expect_log_not_contains "$COMPRESS_LOG" "-lxray_core" "core-compress: does not link xray_core"
    expect_log_not_contains "$COMPRESS_LOG" "-lpthread" "core-compress: does not link pthread"
    expect_log_not_contains "$COMPRESS_LOG" "-lz" "core-compress: does not link zlib"
    expect_log_not_contains "$COMPRESS_LOG" "-lssl" "core-compress: does not link ssl"
    expect_log_not_contains "$COMPRESS_LOG" "-lcrypto" "core-compress: does not link crypto"
    expect_log_contains "$COMPRESS_LOG" "-lxray_aot_core" "core-compress: links AOT core stdlib only"
    expect_log_contains "$COMPRESS_LOG" "-lm" "core-compress: links math lib only"
    expect_output "$COMPRESS_BIN" $'3421780262\n0\n4157704578\n300286872\n1\n93061621\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\nfalse\nfalse\ntrue\ntrue\ntrue' "core-compress: binary output"
else
    record_fail "core-compress: build failed"
    sed 's/^/      /' "$COMPRESS_LOG" | sed -n '1,120p'
fi

FREESTANDING_STDLIB_LOG="$WORK/freestanding_stdlib_reject.log"
if "$XRAY" build --native --profile freestanding --dry-run-link --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$WORK/freestanding_stdlib_reject" \
        "$COMPRESS_SRC" >"$FREESTANDING_STDLIB_LOG" 2>&1; then
    record_fail "freestanding-profile: rejects hosted stdlib imports"
    sed 's/^/      /' "$FREESTANDING_STDLIB_LOG" | sed -n '1,120p'
else
    expect_log_contains "$FREESTANDING_STDLIB_LOG" \
        "freestanding profile rejects stdlib module 'compress'" \
        "freestanding-profile: rejects hosted stdlib imports"
fi

CRYPTO_SRC="$PROJECT_DIR/tests/aot/filetests/link/core_crypto.xr"
CRYPTO_BIN="$WORK/core_crypto"
CRYPTO_LOG="$WORK/core_crypto.log"
if build_native "$CRYPTO_SRC" "$CRYPTO_BIN" "$CRYPTO_LOG"; then
    expect_log_contains "$CRYPTO_LOG" "Link command:" "core-crypto: emitted link command"
    expect_log_not_contains "$CRYPTO_LOG" "-lxray_core" "core-crypto: does not link xray_core"
    expect_log_not_contains "$CRYPTO_LOG" "-lpthread" "core-crypto: does not link pthread"
    expect_log_not_contains "$CRYPTO_LOG" "-lz" "core-crypto: does not link zlib"
    expect_log_not_contains "$CRYPTO_LOG" "-lssl" "core-crypto: does not link ssl"
    expect_log_not_contains "$CRYPTO_LOG" "-lcrypto" "core-crypto: does not link crypto"
    expect_log_contains "$CRYPTO_LOG" "-lxray_aot_core" "core-crypto: links AOT core stdlib only"
    expect_log_contains "$CRYPTO_LOG" "-lm" "core-crypto: links math lib only"
    expect_output "$CRYPTO_BIN" $'true\nfalse\nfalse\ntrue\ntrue\nfalse\ntrue\ntrue\ntrue\ntrue\ntrue\n900150983cd24fb0d6963f7d28e17f72\na9993e364706816aba3e25717850c26c9cd0d89d\nba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\nddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f\n9d5c73ef85594d34ec4438b7c97e51d8\n104152c5bfdca07bc633eebd46199f0255c9f49d\n5031fe3d989c6d1537a013fa6e739da23463fdaec3b70137d828e36ace221bd0\n3c5953a18f7303ec653ba170ae334fafa08e3846f2efe317b87efce82376253cb52a8c31ddcde5a3a2eee183c2b34cb91f85e64ddbc325f7692b199473579c58\ntrue' "core-crypto: binary output"
else
    record_fail "core-crypto: build failed"
    sed 's/^/      /' "$CRYPTO_LOG" | sed -n '1,120p'
fi

REGEX_SRC="$PROJECT_DIR/tests/aot/filetests/link/core_regex.xr"
REGEX_BIN="$WORK/core_regex"
REGEX_LOG="$WORK/core_regex.log"
if build_native "$REGEX_SRC" "$REGEX_BIN" "$REGEX_LOG"; then
    expect_log_contains "$REGEX_LOG" "Link command:" "core-regex: emitted link command"
    expect_log_not_contains "$REGEX_LOG" "-lxray_core" "core-regex: does not link xray_core"
    expect_log_not_contains "$REGEX_LOG" "-lpthread" "core-regex: does not link pthread"
    expect_log_not_contains "$REGEX_LOG" "-lz" "core-regex: does not link zlib"
    expect_log_not_contains "$REGEX_LOG" "-lssl" "core-regex: does not link ssl"
    expect_log_not_contains "$REGEX_LOG" "-lcrypto" "core-regex: does not link crypto"
    expect_log_contains "$REGEX_LOG" "-lxray_aot_core" "core-regex: links AOT core stdlib only"
    expect_log_contains "$REGEX_LOG" "-lm" "core-regex: links math lib only"
    expect_output "$REGEX_BIN" $'a\\.b\\*c\\?\nplain\nx\\+y\n\\[abc\\]\ntrue\nfalse\ntrue\nfalse\n3\n1\ntrue\n2\nabc123\nabc\n123\ntrue\nabc123\n0\n6\n3\nabc123\nabc\n123\ntrue\nabc123\n0\n6\nabc\ntrue\n2\nabc123\n0\n123\nxyz456\n7\n1\nabc123\n0\n0\naN b22 c333\naN bN cN\n123-abc xyz\nabc\n3\na\nb\nc\n2\na\nb, c' "core-regex: binary output"
else
    record_fail "core-regex: build failed"
    sed 's/^/      /' "$REGEX_LOG" | sed -n '1,120p'
fi

RUNTIME_SRC="$PROJECT_DIR/tests/aot/filetests/link/runtime_time.xr"
RUNTIME_BIN="$WORK/runtime_time"
RUNTIME_LOG="$WORK/runtime_time.log"
if build_native "$RUNTIME_SRC" "$RUNTIME_BIN" "$RUNTIME_LOG"; then
    expect_log_contains "$RUNTIME_LOG" "Link command:" "runtime-time: emitted link command"
    expect_log_not_contains "$RUNTIME_LOG" "-lxray_core" "runtime-time: does not link xray_core"
    expect_log_contains "$RUNTIME_LOG" "-lxray_rt_coro" "runtime-time: links AOT runtime archive"
    expect_log_contains "$RUNTIME_LOG" "-lpthread" "runtime-time: links pthread"
    expect_log_not_contains "$RUNTIME_LOG" "-lz" "runtime-time: does not link zlib"
    expect_log_not_contains "$RUNTIME_LOG" "-lffi" "runtime-time: does not link libffi"
    expect_log_not_contains "$RUNTIME_LOG" "-lssl" "runtime-time: does not link ssl"
    expect_log_not_contains "$RUNTIME_LOG" "-lcrypto" "runtime-time: does not link crypto"
    expect_output "$RUNTIME_BIN" "7" "runtime-time: binary output"
else
    record_fail "runtime-time: build failed"
    sed 's/^/      /' "$RUNTIME_LOG" | sed -n '1,120p'
fi

FREESTANDING_RUNTIME_LOG="$WORK/freestanding_runtime_reject.log"
if "$XRAY" build --native --profile freestanding --dry-run-link --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$WORK/freestanding_runtime_reject" \
        "$RUNTIME_SRC" >"$FREESTANDING_RUNTIME_LOG" 2>&1; then
    record_fail "freestanding-profile: rejects hosted time stdlib"
    sed 's/^/      /' "$FREESTANDING_RUNTIME_LOG" | sed -n '1,120p'
else
    expect_log_contains "$FREESTANDING_RUNTIME_LOG" \
        "freestanding profile rejects stdlib module 'time'" \
        "freestanding-profile: rejects hosted time stdlib"
fi

RUNTIME_TASK_SRC="$PROJECT_DIR/tests/aot/filetests/link/runtime_task.xr"
FREESTANDING_TASK_LOG="$WORK/freestanding_task_reject.log"
if "$XRAY" build --native --profile freestanding --dry-run-link --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$WORK/freestanding_task_reject" \
        "$RUNTIME_TASK_SRC" >"$FREESTANDING_TASK_LOG" 2>&1; then
    record_fail "freestanding-profile: rejects coroutine constructs"
    sed 's/^/      /' "$FREESTANDING_TASK_LOG" | sed -n '1,120p'
else
    expect_log_contains "$FREESTANDING_TASK_LOG" \
        "freestanding profile rejects go expression" \
        "freestanding-profile: rejects coroutine constructs"
fi

if [ "${XRAY_LINK_COMMAND_REAL_BUILDS:-0}" != "1" ]; then
    echo ""
    echo "-- Real Link Smoke --"

    SMOKE_CORE_BIN="$WORK/smoke_core_math"
    SMOKE_CORE_LOG="$WORK/smoke_core_math.log"
    if build_native_real "$CORE_SRC" "$SMOKE_CORE_BIN" "$SMOKE_CORE_LOG"; then
        record_pass "real-smoke/core-math: native link"
        expect_output "$SMOKE_CORE_BIN" "9.0" "real-smoke/core-math: binary output"
    else
        record_fail "real-smoke/core-math: native link"
        sed 's/^/      /' "$SMOKE_CORE_LOG" | sed -n '1,120p'
    fi

    SMOKE_AOT_CORE_BIN="$WORK/smoke_system_math_random"
    SMOKE_AOT_CORE_LOG="$WORK/smoke_system_math_random.log"
    if build_native_real "$RANDOM_MATH_SRC" "$SMOKE_AOT_CORE_BIN" "$SMOKE_AOT_CORE_LOG"; then
        record_pass "real-smoke/system-math-random: native link"
        expect_output "$SMOKE_AOT_CORE_BIN" $'true\n7' \
            "real-smoke/system-math-random: binary output"
    else
        record_fail "real-smoke/system-math-random: native link"
        sed 's/^/      /' "$SMOKE_AOT_CORE_LOG" | sed -n '1,120p'
    fi

    SMOKE_RUNTIME_BIN="$WORK/smoke_runtime_time"
    SMOKE_RUNTIME_LOG="$WORK/smoke_runtime_time.log"
    if build_native_real "$RUNTIME_SRC" "$SMOKE_RUNTIME_BIN" "$SMOKE_RUNTIME_LOG"; then
        record_pass "real-smoke/runtime-time: native link"
        expect_output "$SMOKE_RUNTIME_BIN" "7" "real-smoke/runtime-time: binary output"
    else
        record_fail "real-smoke/runtime-time: native link"
        sed 's/^/      /' "$SMOKE_RUNTIME_LOG" | sed -n '1,120p'
    fi
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1

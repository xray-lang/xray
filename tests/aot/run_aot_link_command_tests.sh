#!/bin/bash
# Manifest-first AOT link smoke.
#
# The broad source/code-shape matrix lives in run_aot_filetests.sh --mode link.
# This gate stays deliberately small: it proves that the build driver consumes
# typed native/export/link plans, emits an executable link command, and never
# needs the retired production attributes or source-level dylib clauses.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
XRAY="${1:-${XRAY_BIN:-$PROJECT_DIR/build/xray}}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_manifest_link.XXXXXX")" || {
    echo "FAIL: cannot create temporary directory" >&2
    exit 1
}
CACHE="$WORK/cache"
PASS=0
FAIL=0

cleanup() {
    rm -rf "$WORK"
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

expect_file_contains() {
    local file="$1"
    local needle="$2"
    local name="$3"
    if grep -Fq -- "$needle" "$file"; then
        pass "$name"
    else
        fail "$name"
        sed 's/^/      /' "$file" | sed -n '1,100p'
    fi
}

expect_file_not_contains() {
    local file="$1"
    local needle="$2"
    local name="$3"
    if grep -Fq -- "$needle" "$file"; then
        fail "$name"
        sed 's/^/      /' "$file" | sed -n '1,100p'
    else
        pass "$name"
    fi
}

echo "=== Manifest-first AOT Link Smoke ==="
echo "Binary: $XRAY"
echo ""

if [ ! -x "$XRAY" ]; then
    echo "FAIL: xray binary not executable: $XRAY" >&2
    exit 1
fi

ATTR_LOG="$WORK/attributes.log"
if "$XRAY" language attributes >"$ATTR_LOG" 2>&1; then
    expect_file_contains "$ATTR_LOG" "Public attributes (9):" \
        "language surface exposes exactly nine public attributes"
    for retired in c_export section weak used naked interrupt no_alloc zero_cost; do
        expect_file_not_contains "$ATTR_LOG" "@$retired" \
            "language surface excludes retired @$retired"
    done
else
    fail "language attributes command succeeds"
fi

# A prebuilt dynamic library is an audited NativePackagePlan input. The source
# only contains an extern signature; the library path and symbol mapping live in
# xray.toml.
FFI_DIR="$WORK/dynamic"
mkdir -p "$FFI_DIR"
FFI_C="$FFI_DIR/native.c"
FFI_LIB="$FFI_DIR/libmanifest_smoke.so"
FFI_CC_ARGS=(-shared -fPIC)
case "$(uname -s 2>/dev/null)" in
    Darwin)
        FFI_LIB="$FFI_DIR/libmanifest_smoke.dylib"
        FFI_CC_ARGS=(-dynamiclib -install_name "$FFI_LIB")
        ;;
esac
cat >"$FFI_C" <<'C'
#include <stdint.h>
int32_t xr_manifest_add1(int32_t value) { return value + 1; }
C
if cc "${FFI_CC_ARGS[@]}" -o "$FFI_LIB" "$FFI_C" >"$WORK/native-build.log" 2>&1; then
    pass "native fixture library builds"
else
    fail "native fixture library builds"
fi
if command -v sha256sum >/dev/null 2>&1; then
    FFI_HASH="$(sha256sum "$FFI_LIB" | awk '{print $1}')"
elif command -v shasum >/dev/null 2>&1; then
    FFI_HASH="$(shasum -a 256 "$FFI_LIB" | awk '{print $1}')"
else
    echo "FAIL: SHA-256 utility not found (need sha256sum or shasum)" >&2
    exit 1
fi
FFI_NAME="$(basename "$FFI_LIB")"
cat >"$FFI_DIR/main.xr" <<'XR'
extern "C" {
    fn manifestAdd1(value: i32) -> i32
}
print(unsafe { manifestAdd1(41) })
XR
cat >"$FFI_DIR/xray.toml" <<TOML
[package]
name = "manifest-dynamic-smoke"
version = "1.0.0"
license = "MIT"
main = "main.xr"

[native]
name = "manifest-dynamic-smoke"
version = "1"
license = "test"
source = "generated test fixture"
audit_mode = "exploratory"
vm = "verified-dynamic"

[[native.unit]]
name = "fixture"
kind = "dynamic-library"
sources = ["$FFI_NAME"]
source_hashes = ["$FFI_HASH"]
optimization = "none"
visibility = "default"
warnings = "system"
purpose = "prove audited prebuilt library linking"

[[native.symbol]]
xray = "manifestAdd1"
native = "xr_manifest_add1"
kind = "function"
calling_convention = "c"
unit = "fixture"
TOML

FFI_BIN="$FFI_DIR/app"
FFI_LOG="$WORK/dynamic-link.log"
if "$XRAY" build --native --dump-link-command --cache-dir "$CACHE" \
        -o "$FFI_BIN" "$FFI_DIR/main.xr" >"$FFI_LOG" 2>&1; then
    pass "typed prebuilt dynamic-library plan links"
    expect_file_contains "$FFI_LOG" "Link command:" \
        "dynamic-library build emits link command"
    expect_file_contains "$FFI_LOG" "$FFI_NAME" \
        "dynamic-library link command uses audited artifact path"
    FFI_OUTPUT="$(DYLD_LIBRARY_PATH="$FFI_DIR" LD_LIBRARY_PATH="$FFI_DIR" \
        "$FFI_BIN" 2>/dev/null)"
    if [ "$FFI_OUTPUT" = "42" ]; then
        pass "dynamic-library binary executes"
    else
        fail "dynamic-library binary executes with output 42"
    fi
else
    fail "typed prebuilt dynamic-library plan links"
    sed 's/^/      /' "$FFI_LOG" | sed -n '1,120p'
fi

# A core math call should link only the reachable platform capability.
MATH_SRC="$PROJECT_DIR/tests/aot/filetests/link/core_math_single_symbol.xr"
MATH_BIN="$WORK/core-math"
MATH_LOG="$WORK/core-math.log"
if "$XRAY" build --native --dump-link-command --cache-dir "$CACHE" \
        -o "$MATH_BIN" "$MATH_SRC" >"$MATH_LOG" 2>&1; then
    pass "core math native binary links"
    expect_file_contains "$MATH_LOG" "-lm" "core math links libm"
    expect_file_not_contains "$MATH_LOG" "-lxray_core" \
        "core math does not link hosted umbrella runtime"
    if [ "$("$MATH_BIN" 2>/dev/null)" = "9.0" ]; then
        pass "core math binary executes"
    else
        fail "core math binary executes with output 9.0"
    fi
    if [ "$(uname -m 2>/dev/null)" = "arm64" ] && cc --version 2>/dev/null | grep -qi clang; then
        expect_file_contains "$MATH_LOG" "-mno-outline" \
            "AArch64 speed build disables the machine outliner"
        MATH_SIZE_LOG="$WORK/core-math-size.log"
        if "$XRAY" build --native -O s --dump-link-command --cache-dir "$CACHE-size" \
                -o "$WORK/core-math-size" "$MATH_SRC" >"$MATH_SIZE_LOG" 2>&1; then
            expect_file_not_contains "$MATH_SIZE_LOG" "-mno-outline" \
                "AArch64 size build retains outlining policy"
        else
            fail "AArch64 size-policy probe links"
        fi
    fi
else
    fail "core math native binary links"
    sed 's/^/      /' "$MATH_LOG" | sed -n '1,120p'
fi

# Explicit SIMD selection is provider-neutral manifest intent. GNU-family
# providers keep AVX2/AVX-512 in attributed function islands so unrelated
# scalar code and runtime dispatch both retain the x86_64 baseline.
case "$(uname -s 2>/dev/null):$(uname -m 2>/dev/null)" in
    Linux:x86_64|Darwin:x86_64)
        SIMD_SRC="$WORK/simd-intent.xr"
        cat >"$SIMD_SRC" <<'XR'
import { Capabilities } from simd
print(Capabilities.nativeBytes())
XR
        for mode in avx2 avx512 dispatch; do
            SIMD_LOG="$WORK/simd-$mode.log"
            if "$XRAY" build --native --simd "$mode" --rebuild \
                    --dump-link-command --dump-link-manifest \
                    --cache-dir "$WORK/cache-simd-$mode" \
                    -o "$WORK/simd-$mode" "$SIMD_SRC" >"$SIMD_LOG" 2>&1; then
                pass "semantic SIMD $mode native build succeeds"
                expect_file_contains "$SIMD_LOG" '"provider_cc_flags": []' \
                    "semantic SIMD $mode does not use raw manifest flags"
                case "$mode" in
                    avx2)
                        expect_file_not_contains "$SIMD_LOG" "-mavx2" \
                            "AVX2 intent does not retarget the whole GNU translation unit"
                        ;;
                    avx512)
                        expect_file_not_contains "$SIMD_LOG" "-mavx512f" \
                            "AVX-512 intent does not retarget the whole GNU translation unit"
                        ;;
                    dispatch)
                        expect_file_not_contains "$SIMD_LOG" "-mavx2" \
                            "dispatch keeps baseline compile target"
                        expect_file_not_contains "$SIMD_LOG" "-mavx512f" \
                            "dispatch keeps AVX-512 in attributed islands"
                        ;;
                esac
            else
                fail "semantic SIMD $mode native build succeeds"
                sed 's/^/      /' "$SIMD_LOG" | sed -n '1,160p'
            fi
        done

        DISPATCH_ISLAND_SRC="$PROJECT_DIR/tests/aot/filetests/cgen/simd_x86_runtime_dispatch.xr"
        DISPATCH_ISLAND_LOG="$WORK/simd-dispatch-islands.log"
        if "$XRAY" build --native --simd dispatch --rebuild \
                --dump-link-command --dump-link-manifest \
                --cache-dir "$WORK/cache-simd-dispatch-islands" \
                -o "$WORK/simd-dispatch-islands" "$DISPATCH_ISLAND_SRC" \
                >"$DISPATCH_ISLAND_LOG" 2>&1; then
            pass "dispatch AVX2/AVX-512 attributed islands compile at baseline"
            expect_file_not_contains "$DISPATCH_ISLAND_LOG" "-mavx2" \
                "dispatch island fixture has no global AVX2 flag"
            expect_file_not_contains "$DISPATCH_ISLAND_LOG" "-mavx512f" \
                "dispatch island fixture has no global AVX-512 flag"
        else
            fail "dispatch AVX2/AVX-512 attributed islands compile at baseline"
            sed 's/^/      /' "$DISPATCH_ISLAND_LOG" | sed -n '1,180p'
        fi
        for mode in avx2 avx512; do
            STATIC_ISLAND_SRC="$PROJECT_DIR/tests/aot/filetests/cgen/simd_x86_static_${mode}_island.xr"
            STATIC_ISLAND_LOG="$WORK/simd-static-$mode-islands.log"
            if "$XRAY" build --native --simd "$mode" --rebuild \
                    --dump-link-command --dump-link-manifest \
                    --cache-dir "$WORK/cache-simd-static-$mode-islands" \
                    -o "$WORK/simd-static-$mode-islands" "$STATIC_ISLAND_SRC" \
                    >"$STATIC_ISLAND_LOG" 2>&1; then
                pass "static $mode attributed island compiles with a scoped unit target"
                if [ "$mode" = avx2 ]; then
                    expect_file_contains "$STATIC_ISLAND_LOG" "-mavx2" \
                        "static avx2 vector-bearing unit receives AVX2"
                    expect_file_not_contains "$STATIC_ISLAND_LOG" "-mavx512f" \
                        "static avx2 unit does not receive AVX-512"
                else
                    expect_file_contains "$STATIC_ISLAND_LOG" "-mavx512f" \
                        "static avx512 vector-bearing unit receives AVX-512"
                    expect_file_not_contains "$STATIC_ISLAND_LOG" "-mavx2" \
                        "static avx512 unit does not receive a separate AVX2 flag"
                fi
            else
                fail "static $mode attributed island compiles at baseline"
                sed 's/^/      /' "$STATIC_ISLAND_LOG" | sed -n '1,180p'
            fi
        done
        ;;
esac

# Projectify a committed manifest fixture to prove that export and link-symbol
# policies reach the freestanding command and generated C/object path.
FREE_DIR="$WORK/freestanding"
mkdir -p "$FREE_DIR"
cp "$PROJECT_DIR/tests/aot/filetests/link/freestanding_symbol_attrs.xr" "$FREE_DIR/main.xr"
cp "$PROJECT_DIR/tests/aot/filetests/link/freestanding_symbol_attrs.toml" "$FREE_DIR/xray.toml"
sed -i.bak 's/main = "freestanding_symbol_attrs.xr"/main = "main.xr"/' "$FREE_DIR/xray.toml"
rm -f "$FREE_DIR/xray.toml.bak"
FREE_OBJ="$FREE_DIR/kernel.o"
FREE_LOG="$WORK/freestanding.log"
if "$XRAY" build --native --profile freestanding --shared --keep-c --rebuild \
        --dump-link-command --dump-link-manifest --cache-dir "$CACHE" \
        -o "$FREE_OBJ" "$FREE_DIR/main.xr" >"$FREE_LOG" 2>&1; then
    pass "freestanding manifest object builds"
    expect_file_contains "$FREE_LOG" "-nostdlib" \
        "freestanding command remains nostdlib"
    expect_file_contains "$FREE_LOG" '"runtime_objects": []' \
        "freestanding manifest has no hosted runtime objects"
    FREE_C="$(sed -n 's/^Kept C source: //p' "$FREE_LOG" | tail -n 1)"
    if [ -f "$FREE_C" ]; then
        expect_file_contains "$FREE_C" 'XRT_ATTR_SECTION("__TEXT,.xray_boot")' \
            "link.symbol section plan reaches generated C"
        expect_file_contains "$FREE_C" "XRT_ATTR_WEAK" \
            "link.symbol weak plan reaches generated C"
        expect_file_contains "$FREE_C" "XRT_ATTR_USED" \
            "link.symbol used plan reaches generated C"
    else
        fail "freestanding kept C source is available"
    fi
    if nm -g "$FREE_OBJ" 2>/dev/null | grep -Eq '[[:space:]]_?xray_boot_score$'; then
        pass "export.c symbol is present in freestanding object"
    else
        fail "export.c symbol is present in freestanding object"
    fi
else
    fail "freestanding manifest object builds"
    sed 's/^/      /' "$FREE_LOG" | sed -n '1,140p'
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0

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
    expect_file_contains "$ATTR_LOG" "Public attributes (7):" \
        "language surface exposes exactly seven public attributes"
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
FFI_HASH="$(shasum -a 256 "$FFI_LIB" | awk '{print $1}')"
FFI_NAME="$(basename "$FFI_LIB")"
cat >"$FFI_DIR/main.xr" <<'XR'
extern "C" {
    fn manifestAdd1(value: int32) -> int32
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
else
    fail "core math native binary links"
    sed 's/^/      /' "$MATH_LOG" | sed -n '1,120p'
fi

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

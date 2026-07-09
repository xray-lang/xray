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

record_skip() {
    echo "  SKIP: $1"
}

nm_undefined_normalized() {
    nm -u "$1" 2>&1 |
        sed '/^[[:space:]]*$/d' |
        sed 's/.*[[:space:]]//; s/^_//'
}

object_has_weak_symbol() {
    local obj="$1"
    local sym="$2"
    local dump="$3"
    if nm -m "$obj" >"$dump" 2>/dev/null; then
        grep -E "weak external.*_?${sym}$" "$dump" >/dev/null
        return $?
    fi
    if nm -g "$obj" >"$dump" 2>/dev/null; then
        grep -Eq "[[:space:]][WwVv][[:space:]]+_?${sym}$" "$dump"
        return $?
    fi
    return 1
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
FREESTANDING_UINTSIZE_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_uintsize_target.xr"
FREESTANDING_ENDIAN_NATIVE_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_endian_native_target.xr"
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
    expect_log_contains "$FREESTANDING_EXPORT_LOG" "-fno-stack-protector" \
        "freestanding-profile: disables hosted stack protector runtime"
    expect_log_contains "$FREESTANDING_EXPORT_LOG" "-fno-unwind-tables" \
        "freestanding-profile: disables hosted unwind table dependency"
    expect_log_contains "$FREESTANDING_EXPORT_LOG" "-fno-asynchronous-unwind-tables" \
        "freestanding-profile: disables async unwind table dependency"
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

FREESTANDING_LINKER_SCRIPT="$WORK/freestanding.ld"
printf '%s\n' 'SECTIONS { . = 0x100000; .text : { *(.text*) } }' \
    >"$FREESTANDING_LINKER_SCRIPT"
FREESTANDING_LINKER_SCRIPT_BIN="$WORK/freestanding_linker_script"
FREESTANDING_LINKER_SCRIPT_LOG="$WORK/freestanding_linker_script.log"
if "$XRAY" build --native --profile freestanding --dry-run-link --dump-link-command \
        --linker-script "$FREESTANDING_LINKER_SCRIPT" \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_LINKER_SCRIPT_BIN" \
        "$FREESTANDING_EXPORT_SRC" >"$FREESTANDING_LINKER_SCRIPT_LOG" 2>&1; then
    expect_log_contains "$FREESTANDING_LINKER_SCRIPT_LOG" \
        "-Wl,-T,$FREESTANDING_LINKER_SCRIPT" \
        "freestanding-profile/linker-script: passes linker script"
    expect_log_contains "$FREESTANDING_LINKER_SCRIPT_LOG" "-nostdlib" \
        "freestanding-profile/linker-script: keeps freestanding link flags"
else
    record_fail "freestanding-profile/linker-script: build failed"
    sed 's/^/      /' "$FREESTANDING_LINKER_SCRIPT_LOG" | sed -n '1,120p'
fi

FREESTANDING_TARGET_CONFIG_DIR="$WORK/freestanding_target_config"
mkdir -p "$FREESTANDING_TARGET_CONFIG_DIR/src"
cp "$FREESTANDING_EXPORT_SRC" "$FREESTANDING_TARGET_CONFIG_DIR/src/main.xr"
FREESTANDING_TARGET_CONFIG_SCRIPT="$FREESTANDING_TARGET_CONFIG_DIR/kernel.ld"
printf '%s\n' 'SECTIONS { . = 0x100000; .text : { *(.text*) } }' \
    >"$FREESTANDING_TARGET_CONFIG_SCRIPT"
FREESTANDING_TARGET_CONFIG_SCRIPT_REAL="$(
    cd "$(dirname "$FREESTANDING_TARGET_CONFIG_SCRIPT")" && pwd -P
)/$(basename "$FREESTANDING_TARGET_CONFIG_SCRIPT")"
cat >"$FREESTANDING_TARGET_CONFIG_DIR/xray.toml" <<EOF
[project]
name = "freestanding-target-config"
main = "src/main.xr"

[target.x86_64-unknown-none]
profile = "freestanding"
toolchain = "zig"
linker_script = "kernel.ld"
objcopy = "llvm-objcopy"
objcopy_output = "kernel.bin"
cc_flags = ["-DXRAY_TARGET_CONFIG_TEST=1"]
ld_flags = ["-Wl,-z,max-page-size=4096"]
objcopy_flags = ["-O", "binary"]
EOF
FREESTANDING_TARGET_CONFIG_BIN="$WORK/freestanding_target_config.elf"
FREESTANDING_TARGET_CONFIG_OBJ_REAL="$(
    cd "$FREESTANDING_TARGET_CONFIG_DIR" && pwd -P
)/kernel.bin"
FREESTANDING_TARGET_CONFIG_LOG="$WORK/freestanding_target_config.log"
if "$XRAY" build --native --target x86_64-unknown-none --dry-run-link --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_TARGET_CONFIG_BIN" \
        "$FREESTANDING_TARGET_CONFIG_DIR/src/main.xr" >"$FREESTANDING_TARGET_CONFIG_LOG" 2>&1; then
    expect_log_contains "$FREESTANDING_TARGET_CONFIG_LOG" \
        "Link command: zig cc -target x86_64-freestanding-none" \
        "freestanding-profile/target-config: maps bare-metal target through configured toolchain"
    expect_log_contains "$FREESTANDING_TARGET_CONFIG_LOG" "-DXRAY_PROFILE_FREESTANDING=1" \
        "freestanding-profile/target-config: uses configured profile"
    expect_log_contains "$FREESTANDING_TARGET_CONFIG_LOG" "-nostdlib" \
        "freestanding-profile/target-config: keeps freestanding link flags"
    expect_log_contains "$FREESTANDING_TARGET_CONFIG_LOG" "-DXR_AOT_TARGET_PTR_BITS=64" \
        "freestanding-profile/target-config: defines target pointer width"
    expect_log_contains "$FREESTANDING_TARGET_CONFIG_LOG" "-DXR_AOT_TARGET_LITTLE_ENDIAN=1" \
        "freestanding-profile/target-config: defines target endian"
    expect_log_contains "$FREESTANDING_TARGET_CONFIG_LOG" \
        "-Wl,-T,$FREESTANDING_TARGET_CONFIG_SCRIPT_REAL" \
        "freestanding-profile/target-config: resolves linker script from project root"
    expect_log_contains "$FREESTANDING_TARGET_CONFIG_LOG" "-DXRAY_TARGET_CONFIG_TEST=1" \
        "freestanding-profile/target-config: applies configured cc flags"
    expect_log_contains "$FREESTANDING_TARGET_CONFIG_LOG" "-Wl,-z,max-page-size=4096" \
        "freestanding-profile/target-config: applies configured ld flags"
    expect_log_contains "$FREESTANDING_TARGET_CONFIG_LOG" \
        "Objcopy command: llvm-objcopy -O binary $FREESTANDING_TARGET_CONFIG_BIN $FREESTANDING_TARGET_CONFIG_OBJ_REAL" \
        "freestanding-profile/target-config: applies configured objcopy"
else
    record_fail "freestanding-profile/target-config: build failed"
    sed 's/^/      /' "$FREESTANDING_TARGET_CONFIG_LOG" | sed -n '1,120p'
fi

FREESTANDING_RISCV32_TARGET_OBJ="$WORK/freestanding_kernel_shape_riscv32.o"
FREESTANDING_RISCV32_TARGET_LOG="$WORK/freestanding_kernel_shape_riscv32.log"
if "$XRAY" build --native --profile freestanding --shared \
        --target riscv32imac-unknown-none-elf --toolchain zig \
        --dry-run-link --dump-link-command --dump-link-manifest \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_RISCV32_TARGET_OBJ" \
        "$FREESTANDING_EXPORT_SRC" >"$FREESTANDING_RISCV32_TARGET_LOG" 2>&1; then
    expect_log_contains "$FREESTANDING_RISCV32_TARGET_LOG" \
        "Link command: zig cc -target riscv32-freestanding-none" \
        "freestanding-profile/riscv32-target: maps through zig freestanding target"
    expect_log_contains "$FREESTANDING_RISCV32_TARGET_LOG" "-DXR_AOT_TARGET_PTR_BITS=32" \
        "freestanding-profile/riscv32-target: defines 32-bit pointer width"
    expect_log_contains "$FREESTANDING_RISCV32_TARGET_LOG" "-DXR_AOT_TARGET_LITTLE_ENDIAN=1" \
        "freestanding-profile/riscv32-target: defines little-endian target"
    expect_log_contains "$FREESTANDING_RISCV32_TARGET_LOG" '"arch": "riscv32"' \
        "freestanding-profile/riscv32-target: manifest records arch"
    expect_log_contains "$FREESTANDING_RISCV32_TARGET_LOG" '"object_format": "elf"' \
        "freestanding-profile/riscv32-target: manifest records object format"
    expect_log_contains "$FREESTANDING_RISCV32_TARGET_LOG" '"pointer_bits": 32' \
        "freestanding-profile/riscv32-target: manifest records pointer width"
    expect_log_contains "$FREESTANDING_RISCV32_TARGET_LOG" '"endian": "little"' \
        "freestanding-profile/riscv32-target: manifest records endian"
else
    record_fail "freestanding-profile/riscv32-target: dry-run build failed"
    sed 's/^/      /' "$FREESTANDING_RISCV32_TARGET_LOG" | sed -n '1,120p'
fi

FREESTANDING_THUMB_TARGET_OBJ="$WORK/freestanding_export_thumbv7em.o"
FREESTANDING_THUMB_TARGET_LOG="$WORK/freestanding_export_thumbv7em.log"
if "$XRAY" build --native --profile freestanding --shared \
        --target thumbv7em-none-eabi --toolchain zig \
        --dry-run-link --dump-link-command --dump-link-manifest \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_THUMB_TARGET_OBJ" \
        "$FREESTANDING_EXPORT_SRC" >"$FREESTANDING_THUMB_TARGET_LOG" 2>&1; then
    expect_log_contains "$FREESTANDING_THUMB_TARGET_LOG" \
        "Link command: zig cc -target thumb-freestanding-eabi" \
        "freestanding-profile/thumbv7em-target: maps through zig thumb target"
    expect_log_contains "$FREESTANDING_THUMB_TARGET_LOG" "-mcpu=cortex_m4" \
        "freestanding-profile/thumbv7em-target: selects Cortex-M4 CPU"
    expect_log_contains "$FREESTANDING_THUMB_TARGET_LOG" "-DXR_AOT_TARGET_PTR_BITS=32" \
        "freestanding-profile/thumbv7em-target: defines 32-bit pointer width"
    expect_log_contains "$FREESTANDING_THUMB_TARGET_LOG" "-DXR_AOT_TARGET_LITTLE_ENDIAN=1" \
        "freestanding-profile/thumbv7em-target: defines little-endian target"
    expect_log_contains "$FREESTANDING_THUMB_TARGET_LOG" "-fno-unwind-tables" \
        "freestanding-profile/thumbv7em-target: disables ARM unwind dependency"
    expect_log_contains "$FREESTANDING_THUMB_TARGET_LOG" '"arch": "thumbv7em"' \
        "freestanding-profile/thumbv7em-target: manifest records arch"
    expect_log_contains "$FREESTANDING_THUMB_TARGET_LOG" '"abi": "eabi"' \
        "freestanding-profile/thumbv7em-target: manifest records EABI"
    expect_log_contains "$FREESTANDING_THUMB_TARGET_LOG" '"object_format": "elf"' \
        "freestanding-profile/thumbv7em-target: manifest records object format"
    expect_log_contains "$FREESTANDING_THUMB_TARGET_LOG" '"pointer_bits": 32' \
        "freestanding-profile/thumbv7em-target: manifest records pointer width"
    expect_log_contains "$FREESTANDING_THUMB_TARGET_LOG" '"endian": "little"' \
        "freestanding-profile/thumbv7em-target: manifest records endian"
else
    record_fail "freestanding-profile/thumbv7em-target: dry-run build failed"
    sed 's/^/      /' "$FREESTANDING_THUMB_TARGET_LOG" | sed -n '1,120p'
fi

if command -v zig >/dev/null 2>&1 && command -v llvm-readelf >/dev/null 2>&1 &&
        command -v llvm-nm >/dev/null 2>&1; then
    FREESTANDING_RISCV32_REAL_OBJ="$WORK/freestanding_export_riscv32_real.o"
    FREESTANDING_RISCV32_REAL_LOG="$WORK/freestanding_export_riscv32_real.log"
    FREESTANDING_RISCV32_REAL_ELF="$WORK/freestanding_export_riscv32_real.elf"
    FREESTANDING_RISCV32_REAL_UNDEF="$WORK/freestanding_export_riscv32_real.undefined"
    if ZIG_GLOBAL_CACHE_DIR="${ZIG_GLOBAL_CACHE_DIR:-$WORK/zig-global-cache}" \
            ZIG_LOCAL_CACHE_DIR="${ZIG_LOCAL_CACHE_DIR:-$WORK/zig-local-cache}" \
            "$XRAY" build --native --profile freestanding --shared \
            --target riscv32imac-unknown-none-elf --toolchain zig --keep-c --rebuild \
            --dump-link-command --cache-dir "$BUILD_CACHE" \
            -o "$FREESTANDING_RISCV32_REAL_OBJ" \
            "$FREESTANDING_EXPORT_SRC" >"$FREESTANDING_RISCV32_REAL_LOG" 2>&1; then
        llvm-readelf -h "$FREESTANDING_RISCV32_REAL_OBJ" >"$FREESTANDING_RISCV32_REAL_ELF" 2>&1
        expect_log_contains "$FREESTANDING_RISCV32_REAL_ELF" "Class:                             ELF32" \
            "freestanding-profile/riscv32-target: real object is ELF32"
        expect_log_contains "$FREESTANDING_RISCV32_REAL_ELF" \
            "Data:                              2's complement, little endian" \
            "freestanding-profile/riscv32-target: real object is little-endian"
        expect_log_contains "$FREESTANDING_RISCV32_REAL_ELF" "Type:                              REL" \
            "freestanding-profile/riscv32-target: real object is relocatable"
        expect_log_contains "$FREESTANDING_RISCV32_REAL_ELF" \
            "Machine:                           RISC-V" \
            "freestanding-profile/riscv32-target: real object targets RISC-V"
        if llvm-nm -u "$FREESTANDING_RISCV32_REAL_OBJ" >"$FREESTANDING_RISCV32_REAL_UNDEF" \
                2>&1; then
            if [ ! -s "$FREESTANDING_RISCV32_REAL_UNDEF" ]; then
                record_pass "freestanding-profile/riscv32-target: real object has no undefined symbols"
            else
                record_fail "freestanding-profile/riscv32-target: real object has undefined symbols"
                sed 's/^/      /' "$FREESTANDING_RISCV32_REAL_UNDEF" | sed -n '1,80p'
            fi
        else
            record_fail "freestanding-profile/riscv32-target: llvm-nm failed"
            sed 's/^/      /' "$FREESTANDING_RISCV32_REAL_UNDEF" | sed -n '1,80p'
        fi
    else
        record_fail "freestanding-profile/riscv32-target: real object build failed"
        sed 's/^/      /' "$FREESTANDING_RISCV32_REAL_LOG" | sed -n '1,120p'
    fi

    FREESTANDING_UINTSIZE_RISCV32_OBJ="$WORK/freestanding_uintsize_riscv32.o"
    FREESTANDING_UINTSIZE_RISCV32_LOG="$WORK/freestanding_uintsize_riscv32.log"
    FREESTANDING_UINTSIZE_RISCV32_UNDEF="$WORK/freestanding_uintsize_riscv32.undefined"
    if ZIG_GLOBAL_CACHE_DIR="${ZIG_GLOBAL_CACHE_DIR:-$WORK/zig-global-cache}" \
            ZIG_LOCAL_CACHE_DIR="${ZIG_LOCAL_CACHE_DIR:-$WORK/zig-local-cache}" \
            "$XRAY" build --native --profile freestanding --shared \
            --target riscv32imac-unknown-none-elf --toolchain zig --keep-c --rebuild \
            --dump-link-command --cache-dir "$BUILD_CACHE" \
            -o "$FREESTANDING_UINTSIZE_RISCV32_OBJ" \
            "$FREESTANDING_UINTSIZE_SRC" >"$FREESTANDING_UINTSIZE_RISCV32_LOG" 2>&1; then
        FREESTANDING_UINTSIZE_RISCV32_C="$(sed -n 's/^Kept C source: //p' \
            "$FREESTANDING_UINTSIZE_RISCV32_LOG" | tail -n 1)"
        if [ -f "$FREESTANDING_UINTSIZE_RISCV32_C" ]; then
            expect_log_contains "$FREESTANDING_UINTSIZE_RISCV32_C" \
                "size_t xray_uintsize_roundtrip(size_t" \
                "freestanding-profile/riscv32-uintsize: uintsize C ABI uses size_t"
            expect_log_contains "$FREESTANDING_UINTSIZE_RISCV32_C" \
                "ptrdiff_t xray_intsize_roundtrip(ptrdiff_t" \
                "freestanding-profile/riscv32-uintsize: intsize C ABI uses ptrdiff_t"
            expect_log_contains "$FREESTANDING_UINTSIZE_RISCV32_C" "sizeof(size_t)" \
                "freestanding-profile/riscv32-uintsize: sizeOf<uintsize> is target C sizeof"
            expect_log_contains "$FREESTANDING_UINTSIZE_RISCV32_C" "_Alignof(size_t)" \
                "freestanding-profile/riscv32-uintsize: alignOf<uintsize> is target C alignof"
            expect_log_contains "$FREESTANDING_UINTSIZE_RISCV32_C" "sizeof(ptrdiff_t)" \
                "freestanding-profile/riscv32-uintsize: sizeOf<intsize> is target C sizeof"
            expect_log_contains "$FREESTANDING_UINTSIZE_RISCV32_C" "_Alignof(ptrdiff_t)" \
                "freestanding-profile/riscv32-uintsize: alignOf<intsize> is target C alignof"
            expect_log_not_contains "$FREESTANDING_UINTSIZE_RISCV32_C" "INT64_C(32)" \
                "freestanding-profile/riscv32-uintsize: layout is not host-folded"
        else
            record_fail "freestanding-profile/riscv32-uintsize: kept C source missing"
            sed 's/^/      /' "$FREESTANDING_UINTSIZE_RISCV32_LOG" | sed -n '1,120p'
        fi
        if llvm-nm -u "$FREESTANDING_UINTSIZE_RISCV32_OBJ" >"$FREESTANDING_UINTSIZE_RISCV32_UNDEF" \
                2>&1; then
            if [ ! -s "$FREESTANDING_UINTSIZE_RISCV32_UNDEF" ]; then
                record_pass "freestanding-profile/riscv32-uintsize: real object has no undefined symbols"
            else
                record_fail "freestanding-profile/riscv32-uintsize: real object has undefined symbols"
                sed 's/^/      /' "$FREESTANDING_UINTSIZE_RISCV32_UNDEF" | sed -n '1,80p'
            fi
        else
            record_fail "freestanding-profile/riscv32-uintsize: llvm-nm failed"
            sed 's/^/      /' "$FREESTANDING_UINTSIZE_RISCV32_UNDEF" | sed -n '1,80p'
        fi
    else
        record_fail "freestanding-profile/riscv32-uintsize: real object build failed"
        sed 's/^/      /' "$FREESTANDING_UINTSIZE_RISCV32_LOG" | sed -n '1,120p'
    fi

    FREESTANDING_ENDIAN_NATIVE_RISCV32_OBJ="$WORK/freestanding_endian_native_riscv32.o"
    FREESTANDING_ENDIAN_NATIVE_RISCV32_LOG="$WORK/freestanding_endian_native_riscv32.log"
    FREESTANDING_ENDIAN_NATIVE_RISCV32_UNDEF="$WORK/freestanding_endian_native_riscv32.undefined"
    if ZIG_GLOBAL_CACHE_DIR="${ZIG_GLOBAL_CACHE_DIR:-$WORK/zig-global-cache}" \
            ZIG_LOCAL_CACHE_DIR="${ZIG_LOCAL_CACHE_DIR:-$WORK/zig-local-cache}" \
            "$XRAY" build --native --profile freestanding --shared \
            --target riscv32imac-unknown-none-elf --toolchain zig --keep-c --rebuild \
            --dump-link-command --cache-dir "$BUILD_CACHE" \
            -o "$FREESTANDING_ENDIAN_NATIVE_RISCV32_OBJ" \
            "$FREESTANDING_ENDIAN_NATIVE_SRC" >"$FREESTANDING_ENDIAN_NATIVE_RISCV32_LOG" 2>&1; then
        FREESTANDING_ENDIAN_NATIVE_RISCV32_C="$(sed -n 's/^Kept C source: //p' \
            "$FREESTANDING_ENDIAN_NATIVE_RISCV32_LOG" | tail -n 1)"
        if [ -f "$FREESTANDING_ENDIAN_NATIVE_RISCV32_C" ]; then
            expect_log_contains "$FREESTANDING_ENDIAN_NATIVE_RISCV32_C" \
                "XRT_TARGET_NATIVE_ENDIAN" \
                "freestanding-profile/riscv32-endian-native: Endian.Native uses target macro"
            expect_log_not_contains "$FREESTANDING_ENDIAN_NATIVE_RISCV32_C" "XR_ENDIAN_NATIVE" \
                "freestanding-profile/riscv32-endian-native: Endian.Native is not emitted as raw Native ordinal"
        else
            record_fail "freestanding-profile/riscv32-endian-native: kept C source missing"
            sed 's/^/      /' "$FREESTANDING_ENDIAN_NATIVE_RISCV32_LOG" | sed -n '1,120p'
        fi
        if llvm-nm -u "$FREESTANDING_ENDIAN_NATIVE_RISCV32_OBJ" \
                >"$FREESTANDING_ENDIAN_NATIVE_RISCV32_UNDEF" 2>&1; then
            FREESTANDING_ENDIAN_NATIVE_RISCV32_UNEXPECTED="$(
                sed '/^[[:space:]]*$/d' "$FREESTANDING_ENDIAN_NATIVE_RISCV32_UNDEF" |
                    sed 's/.*[[:space:]]//; s/^_//' |
                    grep -Ev '^(memcpy|memmove|memset|memcmp|xr_hook_panic)$' || true)"
            if [ -z "$FREESTANDING_ENDIAN_NATIVE_RISCV32_UNEXPECTED" ]; then
                record_pass \
                    "freestanding-profile/riscv32-endian-native: undefined symbols stay in hook/memcpy family"
            else
                record_fail \
                    "freestanding-profile/riscv32-endian-native: unexpected undefined symbols"
                printf '%s\n' "$FREESTANDING_ENDIAN_NATIVE_RISCV32_UNEXPECTED" | sed 's/^/      /'
            fi
        else
            record_fail "freestanding-profile/riscv32-endian-native: llvm-nm failed"
            sed 's/^/      /' "$FREESTANDING_ENDIAN_NATIVE_RISCV32_UNDEF" | sed -n '1,80p'
        fi
    else
        record_fail "freestanding-profile/riscv32-endian-native: real object build failed"
        sed 's/^/      /' "$FREESTANDING_ENDIAN_NATIVE_RISCV32_LOG" | sed -n '1,120p'
    fi

    FREESTANDING_THUMB_REAL_OBJ="$WORK/freestanding_export_thumbv7em_real.o"
    FREESTANDING_THUMB_REAL_LOG="$WORK/freestanding_export_thumbv7em_real.log"
    FREESTANDING_THUMB_REAL_ELF="$WORK/freestanding_export_thumbv7em_real.elf"
    FREESTANDING_THUMB_REAL_ATTRS="$WORK/freestanding_export_thumbv7em_real.attrs"
    FREESTANDING_THUMB_REAL_UNDEF="$WORK/freestanding_export_thumbv7em_real.undefined"
    if ZIG_GLOBAL_CACHE_DIR="${ZIG_GLOBAL_CACHE_DIR:-$WORK/zig-global-cache}" \
            ZIG_LOCAL_CACHE_DIR="${ZIG_LOCAL_CACHE_DIR:-$WORK/zig-local-cache}" \
            "$XRAY" build --native --profile freestanding --shared \
            --target thumbv7em-none-eabi --toolchain zig --keep-c --rebuild \
            --dump-link-command --cache-dir "$BUILD_CACHE" \
            -o "$FREESTANDING_THUMB_REAL_OBJ" \
            "$FREESTANDING_EXPORT_SRC" >"$FREESTANDING_THUMB_REAL_LOG" 2>&1; then
        llvm-readelf -h "$FREESTANDING_THUMB_REAL_OBJ" >"$FREESTANDING_THUMB_REAL_ELF" 2>&1
        llvm-readelf -A "$FREESTANDING_THUMB_REAL_OBJ" >"$FREESTANDING_THUMB_REAL_ATTRS" 2>&1
        expect_log_contains "$FREESTANDING_THUMB_REAL_ELF" "Class:                             ELF32" \
            "freestanding-profile/thumbv7em-target: real object is ELF32"
        expect_log_contains "$FREESTANDING_THUMB_REAL_ELF" \
            "Data:                              2's complement, little endian" \
            "freestanding-profile/thumbv7em-target: real object is little-endian"
        expect_log_contains "$FREESTANDING_THUMB_REAL_ELF" "Type:                              REL" \
            "freestanding-profile/thumbv7em-target: real object is relocatable"
        expect_log_contains "$FREESTANDING_THUMB_REAL_ELF" "Machine:                           ARM" \
            "freestanding-profile/thumbv7em-target: real object targets ARM"
        expect_log_contains "$FREESTANDING_THUMB_REAL_ATTRS" "CPU_name" \
            "freestanding-profile/thumbv7em-target: real object records CPU attributes"
        expect_log_contains "$FREESTANDING_THUMB_REAL_ATTRS" "cortex-m4" \
            "freestanding-profile/thumbv7em-target: real object targets Cortex-M4"
        expect_log_contains "$FREESTANDING_THUMB_REAL_ATTRS" "ARM v7E-M" \
            "freestanding-profile/thumbv7em-target: real object records v7E-M"
        expect_log_contains "$FREESTANDING_THUMB_REAL_ATTRS" "Microcontroller" \
            "freestanding-profile/thumbv7em-target: real object records MCU profile"
        expect_log_contains "$FREESTANDING_THUMB_REAL_ATTRS" "Thumb-2" \
            "freestanding-profile/thumbv7em-target: real object records Thumb-2 ISA"
        if llvm-nm -u "$FREESTANDING_THUMB_REAL_OBJ" >"$FREESTANDING_THUMB_REAL_UNDEF" \
                2>&1; then
            if [ ! -s "$FREESTANDING_THUMB_REAL_UNDEF" ]; then
                record_pass "freestanding-profile/thumbv7em-target: real object has no undefined symbols"
            else
                record_fail "freestanding-profile/thumbv7em-target: real object has undefined symbols"
                sed 's/^/      /' "$FREESTANDING_THUMB_REAL_UNDEF" | sed -n '1,80p'
            fi
        else
            record_fail "freestanding-profile/thumbv7em-target: llvm-nm failed"
            sed 's/^/      /' "$FREESTANDING_THUMB_REAL_UNDEF" | sed -n '1,80p'
        fi
    else
        record_fail "freestanding-profile/thumbv7em-target: real object build failed"
        sed 's/^/      /' "$FREESTANDING_THUMB_REAL_LOG" | sed -n '1,120p'
    fi
else
    record_skip "freestanding-profile/riscv32/thumb targets: real objects (requires zig, llvm-readelf and llvm-nm)"
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

FREESTANDING_ATTR_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_symbol_attrs.xr"
FREESTANDING_ATTR_OBJ="$WORK/freestanding_symbol_attrs.o"
FREESTANDING_ATTR_LOG="$WORK/freestanding_symbol_attrs.log"
if "$XRAY" build --native --profile freestanding --shared --keep-c --rebuild \
        --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_ATTR_OBJ" \
        "$FREESTANDING_ATTR_SRC" >"$FREESTANDING_ATTR_LOG" 2>&1; then
    FREESTANDING_ATTR_C="$(sed -n 's/^Kept C source: //p' "$FREESTANDING_ATTR_LOG" | tail -n 1)"
    if [ -f "$FREESTANDING_ATTR_C" ]; then
        expect_log_contains "$FREESTANDING_ATTR_C" "XRT_ATTR_SECTION(\"__TEXT,.xray_boot\")" \
            "freestanding-profile/symbol-attrs: emits boot section attribute"
        expect_log_contains "$FREESTANDING_ATTR_C" "XRT_ATTR_SECTION(\"__TEXT,.xray_hook\") XRT_ATTR_WEAK XRT_ATTR_USED int32_t xray_hook_score" \
            "freestanding-profile/symbol-attrs: emits weak used hook export"
    else
        record_fail "freestanding-profile/symbol-attrs: kept C source missing"
        sed 's/^/      /' "$FREESTANDING_ATTR_LOG" | sed -n '1,120p'
    fi
    FREESTANDING_ATTR_NM="$WORK/freestanding_symbol_attrs.nm"
    if nm -m "$FREESTANDING_ATTR_OBJ" >"$FREESTANDING_ATTR_NM" 2>/dev/null; then
        if grep -E 'weak external.*_?xray_hook_score$' "$FREESTANDING_ATTR_NM" >/dev/null; then
            record_pass "freestanding-profile/symbol-attrs: hook export is weak"
        else
            record_fail "freestanding-profile/symbol-attrs: hook export is not weak"
            sed 's/^/      /' "$FREESTANDING_ATTR_NM" | sed -n '1,80p'
        fi
    elif nm -g "$FREESTANDING_ATTR_OBJ" >"$FREESTANDING_ATTR_NM" 2>/dev/null; then
        if grep -Eq '[[:space:]][WwVv][[:space:]]+_?xray_hook_score$' "$FREESTANDING_ATTR_NM"; then
            record_pass "freestanding-profile/symbol-attrs: hook export is weak"
        else
            record_fail "freestanding-profile/symbol-attrs: hook export is not weak"
            sed 's/^/      /' "$FREESTANDING_ATTR_NM" | sed -n '1,80p'
        fi
    else
        record_fail "freestanding-profile/symbol-attrs: nm failed"
    fi
    FREESTANDING_ATTR_SECTIONS="$WORK/freestanding_symbol_attrs.sections"
    if otool -l "$FREESTANDING_ATTR_OBJ" >"$FREESTANDING_ATTR_SECTIONS" 2>/dev/null; then
        if grep -Fq "sectname .xray_boot" "$FREESTANDING_ATTR_SECTIONS" &&
           grep -Fq "sectname .xray_hook" "$FREESTANDING_ATTR_SECTIONS"; then
            record_pass "freestanding-profile/symbol-attrs: object contains custom sections"
        else
            record_fail "freestanding-profile/symbol-attrs: object missing custom sections"
            sed 's/^/      /' "$FREESTANDING_ATTR_SECTIONS" | sed -n '1,120p'
        fi
    elif objdump -h "$FREESTANDING_ATTR_OBJ" >"$FREESTANDING_ATTR_SECTIONS" 2>/dev/null; then
        if grep -Fq ".xray_boot" "$FREESTANDING_ATTR_SECTIONS" &&
           grep -Fq ".xray_hook" "$FREESTANDING_ATTR_SECTIONS"; then
            record_pass "freestanding-profile/symbol-attrs: object contains custom sections"
        else
            record_fail "freestanding-profile/symbol-attrs: object missing custom sections"
            sed 's/^/      /' "$FREESTANDING_ATTR_SECTIONS" | sed -n '1,120p'
        fi
    else
        record_fail "freestanding-profile/symbol-attrs: section dump failed"
    fi
else
    record_fail "freestanding-profile/symbol-attrs: object build failed"
    sed 's/^/      /' "$FREESTANDING_ATTR_LOG" | sed -n '1,120p'
fi

FREESTANDING_KERNEL_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_kernel_shape.xr"
FREESTANDING_KERNEL_OBJ="$WORK/freestanding_kernel_shape.o"
FREESTANDING_KERNEL_LOG="$WORK/freestanding_kernel_shape.log"
if "$XRAY" build --native --profile freestanding --shared --keep-c --rebuild \
        --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_KERNEL_OBJ" \
        "$FREESTANDING_KERNEL_SRC" >"$FREESTANDING_KERNEL_LOG" 2>&1; then
    FREESTANDING_KERNEL_C="$(sed -n 's/^Kept C source: //p' "$FREESTANDING_KERNEL_LOG" | tail -n 1)"
    if [ -f "$FREESTANDING_KERNEL_C" ]; then
        expect_log_contains "$FREESTANDING_KERNEL_C" \
            "XRT_ATTR_SECTION(\"__DATA,.xray_multiboot\") XRT_ATTR_USED" \
            "freestanding-profile/kernel-shape: emits multiboot data section"
        expect_log_contains "$FREESTANDING_KERNEL_C" \
            "XRT_ATTR_SECTION(\"__TEXT,.xray_boot\") XRT_ATTR_USED int32_t xray_kernel_entry" \
            "freestanding-profile/kernel-shape: emits boot entry section"
        expect_log_not_contains "$FREESTANDING_KERNEL_C" "xrt_shared[" \
            "freestanding-profile/kernel-shape: generated C avoids shared slots"
        expect_log_not_contains "$FREESTANDING_KERNEL_C" "static XrValue xrt_shared" \
            "freestanding-profile/kernel-shape: generated C avoids shared storage"
    else
        record_fail "freestanding-profile/kernel-shape: kept C source missing"
        sed 's/^/      /' "$FREESTANDING_KERNEL_LOG" | sed -n '1,120p'
    fi
    FREESTANDING_KERNEL_UNDEFINED="$(nm_undefined_normalized "$FREESTANDING_KERNEL_OBJ")"
    if [ -z "$FREESTANDING_KERNEL_UNDEFINED" ]; then
        record_pass "freestanding-profile/kernel-shape: no undefined symbols"
    else
        record_fail "freestanding-profile/kernel-shape: unexpected undefined symbols"
        nm -u "$FREESTANDING_KERNEL_OBJ" 2>&1 | sed '/^[[:space:]]*$/d' | sed 's/^/      /'
    fi
    if nm -g "$FREESTANDING_KERNEL_OBJ" 2>/dev/null |
            grep -Eq '(^|[[:space:]])_?xray_kernel_entry$'; then
        record_pass "freestanding-profile/kernel-shape: exports boot symbol"
    else
        record_fail "freestanding-profile/kernel-shape: missing boot symbol"
        nm -g "$FREESTANDING_KERNEL_OBJ" 2>/dev/null | sed 's/^/      /'
    fi
    FREESTANDING_KERNEL_SECTIONS="$WORK/freestanding_kernel_shape.sections"
    if otool -l "$FREESTANDING_KERNEL_OBJ" >"$FREESTANDING_KERNEL_SECTIONS" 2>/dev/null; then
        if grep -Fq "sectname .xray_boot" "$FREESTANDING_KERNEL_SECTIONS" &&
           grep -Fq "sectname .xray_multiboot" "$FREESTANDING_KERNEL_SECTIONS"; then
            record_pass "freestanding-profile/kernel-shape: object contains boot sections"
        else
            record_fail "freestanding-profile/kernel-shape: object missing boot sections"
            sed 's/^/      /' "$FREESTANDING_KERNEL_SECTIONS" | sed -n '1,120p'
        fi
    elif objdump -h "$FREESTANDING_KERNEL_OBJ" >"$FREESTANDING_KERNEL_SECTIONS" 2>/dev/null; then
        if grep -Fq ".xray_boot" "$FREESTANDING_KERNEL_SECTIONS" &&
           grep -Fq ".xray_multiboot" "$FREESTANDING_KERNEL_SECTIONS"; then
            record_pass "freestanding-profile/kernel-shape: object contains boot sections"
        else
            record_fail "freestanding-profile/kernel-shape: object missing boot sections"
            sed 's/^/      /' "$FREESTANDING_KERNEL_SECTIONS" | sed -n '1,120p'
        fi
    else
        record_fail "freestanding-profile/kernel-shape: section dump failed"
    fi
else
    record_fail "freestanding-profile/kernel-shape: object build failed"
    sed 's/^/      /' "$FREESTANDING_KERNEL_LOG" | sed -n '1,120p'
fi

FREESTANDING_KERNEL_TARGET_OBJ="$WORK/freestanding_kernel_shape_x86_64.o"
FREESTANDING_KERNEL_TARGET_LOG="$WORK/freestanding_kernel_shape_x86_64.log"
if "$XRAY" build --native --profile freestanding --shared --target x86_64-linux-musl \
        --toolchain zig --dry-run-link --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_KERNEL_TARGET_OBJ" \
        "$FREESTANDING_KERNEL_SRC" >"$FREESTANDING_KERNEL_TARGET_LOG" 2>&1; then
    expect_log_contains "$FREESTANDING_KERNEL_TARGET_LOG" \
        "Link command: zig cc -target x86_64-linux-musl" \
        "freestanding-profile/kernel-shape: cross target uses zig target"
    expect_log_contains "$FREESTANDING_KERNEL_TARGET_LOG" " -r " \
        "freestanding-profile/kernel-shape: cross shared output is relocatable object"
    expect_log_contains "$FREESTANDING_KERNEL_TARGET_LOG" "-nostdlib" \
        "freestanding-profile/kernel-shape: cross target keeps no-libc link"
    expect_log_contains "$FREESTANDING_KERNEL_TARGET_LOG" "-DXR_AOT_CROSS_TARGET=1" \
        "freestanding-profile/kernel-shape: cross target defines target marker"
    expect_log_not_contains "$FREESTANDING_KERNEL_TARGET_LOG" "-Wl,--gc-sections" \
        "freestanding-profile/kernel-shape: cross relocatable keeps boot sections"
    expect_log_not_contains "$FREESTANDING_KERNEL_TARGET_LOG" "-dynamiclib" \
        "freestanding-profile/kernel-shape: cross target avoids hosted dylib"
    expect_log_not_contains "$FREESTANDING_KERNEL_TARGET_LOG" " -shared " \
        "freestanding-profile/kernel-shape: cross target avoids hosted shared library"
else
    record_fail "freestanding-profile/kernel-shape: cross target dry-run failed"
    sed 's/^/      /' "$FREESTANDING_KERNEL_TARGET_LOG" | sed -n '1,120p'
fi

if command -v zig >/dev/null 2>&1 && command -v llvm-readelf >/dev/null 2>&1; then
    FREESTANDING_KERNEL_TARGET_REAL_OBJ="$WORK/freestanding_kernel_shape_x86_64_real.o"
    FREESTANDING_KERNEL_TARGET_REAL_LOG="$WORK/freestanding_kernel_shape_x86_64_real.log"
    FREESTANDING_KERNEL_TARGET_REAL_ELF="$WORK/freestanding_kernel_shape_x86_64_real.elf"
    FREESTANDING_KERNEL_TARGET_REAL_SECTIONS="$WORK/freestanding_kernel_shape_x86_64_real.sections"
    FREESTANDING_KERNEL_TARGET_REAL_SYMBOLS="$WORK/freestanding_kernel_shape_x86_64_real.symbols"
    if ZIG_GLOBAL_CACHE_DIR="${ZIG_GLOBAL_CACHE_DIR:-$WORK/zig-global-cache}" \
            ZIG_LOCAL_CACHE_DIR="${ZIG_LOCAL_CACHE_DIR:-$WORK/zig-local-cache}" \
            "$XRAY" build --native --profile freestanding --shared \
            --target x86_64-linux-musl --toolchain zig --keep-c --rebuild \
            --dump-link-command --cache-dir "$BUILD_CACHE" \
            -o "$FREESTANDING_KERNEL_TARGET_REAL_OBJ" \
            "$FREESTANDING_KERNEL_SRC" >"$FREESTANDING_KERNEL_TARGET_REAL_LOG" 2>&1; then
        llvm-readelf -h "$FREESTANDING_KERNEL_TARGET_REAL_OBJ" >"$FREESTANDING_KERNEL_TARGET_REAL_ELF" 2>&1
        llvm-readelf -S "$FREESTANDING_KERNEL_TARGET_REAL_OBJ" >"$FREESTANDING_KERNEL_TARGET_REAL_SECTIONS" 2>&1
        llvm-readelf -s "$FREESTANDING_KERNEL_TARGET_REAL_OBJ" >"$FREESTANDING_KERNEL_TARGET_REAL_SYMBOLS" 2>&1
        expect_log_contains "$FREESTANDING_KERNEL_TARGET_REAL_ELF" "Class:                             ELF64" \
            "freestanding-profile/kernel-shape: cross object is ELF64"
        expect_log_contains "$FREESTANDING_KERNEL_TARGET_REAL_ELF" \
            "Type:                              REL (Relocatable file)" \
            "freestanding-profile/kernel-shape: cross object is relocatable"
        expect_log_contains "$FREESTANDING_KERNEL_TARGET_REAL_ELF" \
            "Machine:                           Advanced Micro Devices X86-64" \
            "freestanding-profile/kernel-shape: cross object targets x86-64"
        expect_log_contains "$FREESTANDING_KERNEL_TARGET_REAL_SECTIONS" ".xray_boot" \
            "freestanding-profile/kernel-shape: cross object keeps boot section"
        expect_log_contains "$FREESTANDING_KERNEL_TARGET_REAL_SECTIONS" ".xray_multiboot" \
            "freestanding-profile/kernel-shape: cross object keeps multiboot section"
        expect_log_contains "$FREESTANDING_KERNEL_TARGET_REAL_SYMBOLS" "xray_kernel_entry" \
            "freestanding-profile/kernel-shape: cross object exports boot symbol"
        FREESTANDING_KERNEL_TARGET_REAL_UNDEFINED="$(nm_undefined_normalized "$FREESTANDING_KERNEL_TARGET_REAL_OBJ")"
        if [ -z "$FREESTANDING_KERNEL_TARGET_REAL_UNDEFINED" ]; then
            record_pass "freestanding-profile/kernel-shape: cross object has no undefined symbols"
        else
            record_fail "freestanding-profile/kernel-shape: cross object has unexpected undefined symbols"
            nm -u "$FREESTANDING_KERNEL_TARGET_REAL_OBJ" 2>&1 |
                sed '/^[[:space:]]*$/d' | sed 's/^/      /'
        fi
    else
        record_fail "freestanding-profile/kernel-shape: cross target real object build failed"
        sed 's/^/      /' "$FREESTANDING_KERNEL_TARGET_REAL_LOG" | sed -n '1,120p'
    fi
else
    record_skip "freestanding-profile/kernel-shape: cross target real object (requires zig and llvm-readelf)"
fi

if command -v zig >/dev/null 2>&1 && command -v llvm-readelf >/dev/null 2>&1; then
    FREESTANDING_KERNEL_ELF_SCRIPT="$WORK/freestanding_kernel_shape_x86_64.ld"
    printf '%s\n' \
        'EXTERN(xray_kernel_entry)' \
        'ENTRY(_start)' \
        'SECTIONS {' \
        '  . = 0x100000;' \
        '  _start = xray_kernel_entry;' \
        '  .xray_multiboot ALIGN(4) : { KEEP(*("__DATA,.xray_multiboot")) }' \
        '  .text ALIGN(16) : { KEEP(*("__TEXT,.xray_boot")) *(.text*) }' \
        '  .rodata ALIGN(16) : { *(.rodata*) }' \
        '  .eh_frame_hdr ALIGN(4) : { *(.eh_frame_hdr) }' \
        '  .eh_frame ALIGN(8) : { *(.eh_frame) }' \
        '  .data ALIGN(16) : { *(.data*) }' \
        '  .bss ALIGN(16) : { *(.bss*) *(COMMON) }' \
        '}' >"$FREESTANDING_KERNEL_ELF_SCRIPT"
    FREESTANDING_KERNEL_ELF="$WORK/freestanding_kernel_shape_x86_64.elf"
    FREESTANDING_KERNEL_ELF_LOG="$WORK/freestanding_kernel_shape_x86_64_elf.log"
    FREESTANDING_KERNEL_ELF_HEADER="$WORK/freestanding_kernel_shape_x86_64_elf.header"
    FREESTANDING_KERNEL_ELF_SECTIONS="$WORK/freestanding_kernel_shape_x86_64_elf.sections"
    FREESTANDING_KERNEL_ELF_SYMBOLS="$WORK/freestanding_kernel_shape_x86_64_elf.symbols"
    if ZIG_GLOBAL_CACHE_DIR="${ZIG_GLOBAL_CACHE_DIR:-$WORK/zig-global-cache}" \
            ZIG_LOCAL_CACHE_DIR="${ZIG_LOCAL_CACHE_DIR:-$WORK/zig-local-cache}" \
            "$XRAY" build --native --profile freestanding --target x86_64-linux-musl \
            --toolchain zig --linker-script "$FREESTANDING_KERNEL_ELF_SCRIPT" \
            --keep-c --rebuild --dump-link-command --cache-dir "$BUILD_CACHE" \
            -o "$FREESTANDING_KERNEL_ELF" "$FREESTANDING_KERNEL_SRC" \
            >"$FREESTANDING_KERNEL_ELF_LOG" 2>&1; then
        expect_log_contains "$FREESTANDING_KERNEL_ELF_LOG" \
            "-Wl,-T,$FREESTANDING_KERNEL_ELF_SCRIPT" \
            "freestanding-profile/kernel-shape: final ELF uses linker script"
        expect_log_not_contains "$FREESTANDING_KERNEL_ELF_LOG" "cannot find entry symbol" \
            "freestanding-profile/kernel-shape: final ELF resolves entry symbol"
        llvm-readelf -h "$FREESTANDING_KERNEL_ELF" >"$FREESTANDING_KERNEL_ELF_HEADER" 2>&1
        llvm-readelf -S "$FREESTANDING_KERNEL_ELF" >"$FREESTANDING_KERNEL_ELF_SECTIONS" 2>&1
        llvm-readelf -s "$FREESTANDING_KERNEL_ELF" >"$FREESTANDING_KERNEL_ELF_SYMBOLS" 2>&1
        expect_log_contains "$FREESTANDING_KERNEL_ELF_HEADER" \
            "Type:                              EXEC (Executable file)" \
            "freestanding-profile/kernel-shape: final ELF is executable"
        expect_log_contains "$FREESTANDING_KERNEL_ELF_HEADER" \
            "Machine:                           Advanced Micro Devices X86-64" \
            "freestanding-profile/kernel-shape: final ELF targets x86-64"
        expect_log_not_contains "$FREESTANDING_KERNEL_ELF_HEADER" \
            "Entry point address:               0x0" \
            "freestanding-profile/kernel-shape: final ELF has nonzero entry"
        expect_log_contains "$FREESTANDING_KERNEL_ELF_SECTIONS" ".xray_multiboot" \
            "freestanding-profile/kernel-shape: final ELF keeps multiboot section"
        expect_log_contains "$FREESTANDING_KERNEL_ELF_SECTIONS" ".text" \
            "freestanding-profile/kernel-shape: final ELF keeps text section"
        expect_log_contains "$FREESTANDING_KERNEL_ELF_SYMBOLS" "xray_kernel_entry" \
            "freestanding-profile/kernel-shape: final ELF exports boot symbol"
        expect_log_contains "$FREESTANDING_KERNEL_ELF_SYMBOLS" "_start" \
            "freestanding-profile/kernel-shape: final ELF defines start alias"
        FREESTANDING_KERNEL_ELF_UNDEFINED="$(nm_undefined_normalized "$FREESTANDING_KERNEL_ELF")"
        if [ -z "$FREESTANDING_KERNEL_ELF_UNDEFINED" ]; then
            record_pass "freestanding-profile/kernel-shape: final ELF has no undefined symbols"
        else
            record_fail "freestanding-profile/kernel-shape: final ELF has unexpected undefined symbols"
            nm -u "$FREESTANDING_KERNEL_ELF" 2>&1 |
                sed '/^[[:space:]]*$/d' | sed 's/^/      /'
        fi
    else
        record_fail "freestanding-profile/kernel-shape: final ELF link failed"
        sed 's/^/      /' "$FREESTANDING_KERNEL_ELF_LOG" | sed -n '1,120p'
    fi
else
    record_skip "freestanding-profile/kernel-shape: final ELF link (requires zig and llvm-readelf)"
fi

if command -v zig >/dev/null 2>&1 && command -v llvm-readelf >/dev/null 2>&1 &&
        command -v llvm-nm >/dev/null 2>&1; then
    FREESTANDING_KERNEL_RISCV32_ELF_SCRIPT="$WORK/freestanding_kernel_shape_riscv32.ld"
    printf '%s\n' \
        'EXTERN(xray_kernel_entry)' \
        'ENTRY(_start)' \
        'SECTIONS {' \
        '  . = 0x80000000;' \
        '  _start = xray_kernel_entry;' \
        '  .xray_multiboot ALIGN(4) : { KEEP(*("__DATA,.xray_multiboot")) }' \
        '  .text ALIGN(4) : { KEEP(*("__TEXT,.xray_boot")) *(.text*) }' \
        '  .rodata ALIGN(4) : { *(.rodata*) }' \
        '  .sdata ALIGN(4) : { *(.sdata*) }' \
        '  .data ALIGN(4) : { *(.data*) }' \
        '  .bss ALIGN(8) : { *(.bss*) *(COMMON) }' \
        '}' >"$FREESTANDING_KERNEL_RISCV32_ELF_SCRIPT"
    FREESTANDING_KERNEL_RISCV32_ELF="$WORK/freestanding_kernel_shape_riscv32.elf"
    FREESTANDING_KERNEL_RISCV32_ELF_LOG="$WORK/freestanding_kernel_shape_riscv32_elf.log"
    FREESTANDING_KERNEL_RISCV32_ELF_HEADER="$WORK/freestanding_kernel_shape_riscv32_elf.header"
    FREESTANDING_KERNEL_RISCV32_ELF_SECTIONS="$WORK/freestanding_kernel_shape_riscv32_elf.sections"
    FREESTANDING_KERNEL_RISCV32_ELF_SYMBOLS="$WORK/freestanding_kernel_shape_riscv32_elf.symbols"
    FREESTANDING_KERNEL_RISCV32_ELF_UNDEF="$WORK/freestanding_kernel_shape_riscv32_elf.undefined"
    if ZIG_GLOBAL_CACHE_DIR="${ZIG_GLOBAL_CACHE_DIR:-$WORK/zig-global-cache}" \
            ZIG_LOCAL_CACHE_DIR="${ZIG_LOCAL_CACHE_DIR:-$WORK/zig-local-cache}" \
            "$XRAY" build --native --profile freestanding \
            --target riscv32imac-unknown-none-elf --toolchain zig \
            --linker-script "$FREESTANDING_KERNEL_RISCV32_ELF_SCRIPT" \
            --keep-c --rebuild --dump-link-command --cache-dir "$BUILD_CACHE" \
            -o "$FREESTANDING_KERNEL_RISCV32_ELF" "$FREESTANDING_KERNEL_SRC" \
            >"$FREESTANDING_KERNEL_RISCV32_ELF_LOG" 2>&1; then
        expect_log_contains "$FREESTANDING_KERNEL_RISCV32_ELF_LOG" \
            "-Wl,-T,$FREESTANDING_KERNEL_RISCV32_ELF_SCRIPT" \
            "freestanding-profile/kernel-shape: riscv32 final ELF uses linker script"
        expect_log_not_contains "$FREESTANDING_KERNEL_RISCV32_ELF_LOG" \
            "cannot find entry symbol" \
            "freestanding-profile/kernel-shape: riscv32 final ELF resolves entry symbol"
        llvm-readelf -h "$FREESTANDING_KERNEL_RISCV32_ELF" \
            >"$FREESTANDING_KERNEL_RISCV32_ELF_HEADER" 2>&1
        llvm-readelf -S "$FREESTANDING_KERNEL_RISCV32_ELF" \
            >"$FREESTANDING_KERNEL_RISCV32_ELF_SECTIONS" 2>&1
        llvm-readelf -s "$FREESTANDING_KERNEL_RISCV32_ELF" \
            >"$FREESTANDING_KERNEL_RISCV32_ELF_SYMBOLS" 2>&1
        expect_log_contains "$FREESTANDING_KERNEL_RISCV32_ELF_HEADER" \
            "Class:                             ELF32" \
            "freestanding-profile/kernel-shape: riscv32 final ELF is ELF32"
        expect_log_contains "$FREESTANDING_KERNEL_RISCV32_ELF_HEADER" \
            "Data:                              2's complement, little endian" \
            "freestanding-profile/kernel-shape: riscv32 final ELF is little-endian"
        expect_log_contains "$FREESTANDING_KERNEL_RISCV32_ELF_HEADER" \
            "Type:                              EXEC (Executable file)" \
            "freestanding-profile/kernel-shape: riscv32 final ELF is executable"
        expect_log_contains "$FREESTANDING_KERNEL_RISCV32_ELF_HEADER" \
            "Machine:                           RISC-V" \
            "freestanding-profile/kernel-shape: riscv32 final ELF targets RISC-V"
        expect_log_not_contains "$FREESTANDING_KERNEL_RISCV32_ELF_HEADER" \
            "Entry point address:               0x0" \
            "freestanding-profile/kernel-shape: riscv32 final ELF has nonzero entry"
        expect_log_contains "$FREESTANDING_KERNEL_RISCV32_ELF_SECTIONS" ".xray_multiboot" \
            "freestanding-profile/kernel-shape: riscv32 final ELF keeps multiboot section"
        expect_log_contains "$FREESTANDING_KERNEL_RISCV32_ELF_SECTIONS" ".text" \
            "freestanding-profile/kernel-shape: riscv32 final ELF keeps text section"
        expect_log_contains "$FREESTANDING_KERNEL_RISCV32_ELF_SYMBOLS" "xray_kernel_entry" \
            "freestanding-profile/kernel-shape: riscv32 final ELF exports boot symbol"
        expect_log_contains "$FREESTANDING_KERNEL_RISCV32_ELF_SYMBOLS" "_start" \
            "freestanding-profile/kernel-shape: riscv32 final ELF defines start alias"
        if llvm-nm -u "$FREESTANDING_KERNEL_RISCV32_ELF" \
                >"$FREESTANDING_KERNEL_RISCV32_ELF_UNDEF" 2>&1; then
            if [ ! -s "$FREESTANDING_KERNEL_RISCV32_ELF_UNDEF" ]; then
                record_pass "freestanding-profile/kernel-shape: riscv32 final ELF has no undefined symbols"
            else
                record_fail "freestanding-profile/kernel-shape: riscv32 final ELF has unexpected undefined symbols"
                sed 's/^/      /' "$FREESTANDING_KERNEL_RISCV32_ELF_UNDEF" | sed -n '1,80p'
            fi
        else
            record_fail "freestanding-profile/kernel-shape: riscv32 final ELF llvm-nm failed"
            sed 's/^/      /' "$FREESTANDING_KERNEL_RISCV32_ELF_UNDEF" | sed -n '1,80p'
        fi
    else
        record_fail "freestanding-profile/kernel-shape: riscv32 final ELF link failed"
        sed 's/^/      /' "$FREESTANDING_KERNEL_RISCV32_ELF_LOG" | sed -n '1,120p'
    fi
else
    record_skip "freestanding-profile/kernel-shape: riscv32 final ELF link (requires zig, llvm-readelf and llvm-nm)"
fi

FREESTANDING_RAWPTR_NULL_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_rawptr_null.xr"
FREESTANDING_RAWPTR_NULL_OBJ="$WORK/freestanding_rawptr_null.o"
FREESTANDING_RAWPTR_NULL_LOG="$WORK/freestanding_rawptr_null.log"
if "$XRAY" build --native --profile freestanding --shared --keep-c --rebuild \
        --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_RAWPTR_NULL_OBJ" \
        "$FREESTANDING_RAWPTR_NULL_SRC" >"$FREESTANDING_RAWPTR_NULL_LOG" 2>&1; then
    FREESTANDING_RAWPTR_NULL_UNDEFINED="$(nm_undefined_normalized "$FREESTANDING_RAWPTR_NULL_OBJ")"
    if [ -z "$FREESTANDING_RAWPTR_NULL_UNDEFINED" ]; then
        record_pass "freestanding-profile/rawptr-null: no undefined symbols"
    else
        record_fail "freestanding-profile/rawptr-null: unexpected undefined symbols"
        nm -u "$FREESTANDING_RAWPTR_NULL_OBJ" 2>&1 | sed '/^[[:space:]]*$/d' | sed 's/^/      /'
    fi
else
    record_fail "freestanding-profile/rawptr-null: object build failed"
    sed 's/^/      /' "$FREESTANDING_RAWPTR_NULL_LOG" | sed -n '1,120p'
fi

FREESTANDING_FIXED_ARRAY_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_fixed_array.xr"
FREESTANDING_FIXED_ARRAY_OBJ="$WORK/freestanding_fixed_array.o"
FREESTANDING_FIXED_ARRAY_LOG="$WORK/freestanding_fixed_array.log"
if "$XRAY" build --native --profile freestanding --shared --keep-c --rebuild \
        --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_FIXED_ARRAY_OBJ" \
        "$FREESTANDING_FIXED_ARRAY_SRC" >"$FREESTANDING_FIXED_ARRAY_LOG" 2>&1; then
    FREESTANDING_FIXED_ARRAY_KEPT_C="$(sed -n 's/^Kept C source: //p' \
        "$FREESTANDING_FIXED_ARRAY_LOG" | tail -n 1)"
    if [ -f "$FREESTANDING_FIXED_ARRAY_KEPT_C" ]; then
        expect_log_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" \
            "#include \"xrt_core_freestanding.h\"" \
            "freestanding-profile/fixed-array: generated C uses freestanding prelude"
        expect_log_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" "xr_span_t" \
            "freestanding-profile/fixed-array: generated C uses freestanding span value"
        expect_log_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" "xrt_span_from_array_slice" \
            "freestanding-profile/fixed-array: generated C slices fixed array into span"
        expect_log_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" \
            "xr_array_core_bytes_store_u16" \
            "freestanding-profile/fixed-array: ByteSpan.store<uint16> uses freestanding bytes helper"
        expect_log_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" \
            "xrt_span_bytes_load_u16_le_unchecked_raw" \
            "freestanding-profile/fixed-array: ByteSpan.load<uint16> uses freestanding span bytes helper"
        expect_log_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" \
            "xrt_span_bytes_load_u64_le_unchecked_raw" \
            "freestanding-profile/fixed-array: ByteSpan.load<uint64> uses freestanding span bytes helper"
        expect_log_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" \
            "xr_array_core_bytes_store_f32" \
            "freestanding-profile/fixed-array: ByteSpan.store<float32> uses freestanding bytes helper"
        expect_log_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" \
            "xr_array_core_bytes_load_f64" \
            "freestanding-profile/fixed-array: ByteSpan.load<float64> uses freestanding bytes helper"
        expect_log_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" \
            "xr_array_core_copy_or_move_bytes" \
            "freestanding-profile/fixed-array: ByteSpan.copyFrom uses freestanding bytes copy helper"
        expect_log_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" \
            "xr_array_core_bytes_common_prefix_raw" \
            "freestanding-profile/fixed-array: ByteSpan.commonPrefix uses freestanding bytes helper"
        expect_log_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" \
            "xr_array_core_bytes_repeat_copy" \
            "freestanding-profile/fixed-array: ByteSpan.repeatFrom uses freestanding bytes helper"
        expect_log_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" \
            "Span.fill(value) byte length overflow" \
            "freestanding-profile/fixed-array: Span.fill uses freestanding POD path"
        expect_log_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" \
            "for (int64_t _i = 0; _i < _s.length; _i++)" \
            "freestanding-profile/fixed-array: Span.fill lowers to local POD loop"
        expect_log_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" \
            "memmove(_dst.data, _src.data" \
            "freestanding-profile/fixed-array: Span.copyFrom lowers to no-libc memmove"
        expect_log_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" \
            "Span.asBytes() byte length overflow" \
            "freestanding-profile/fixed-array: Span.asBytes uses local metadata rewrite"
        expect_log_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" \
            "_out.length = _s.length / (int64_t)2; _out.elem_type =" \
            "freestanding-profile/fixed-array: ByteSpan.reinterpret uses local metadata rewrite"
        expect_log_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" \
            "Span.compare(other) byte length overflow" \
            "freestanding-profile/fixed-array: Span.compare uses freestanding POD path"
        expect_log_not_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" "#include \"xrt.h\"" \
            "freestanding-profile/fixed-array: generated C avoids hosted umbrella"
        expect_log_not_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" "xrt_throw_exc" \
            "freestanding-profile/fixed-array: generated C avoids hosted exception throw"
        expect_log_not_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" "xrt_exception" \
            "freestanding-profile/fixed-array: generated C avoids hosted exception objects"
        expect_log_not_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" \
            "xrt_span_bytes_store_u64_checked_raw" \
            "freestanding-profile/fixed-array: generated C avoids hosted checked ByteSpan.store"
        expect_log_not_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" \
            "xrt_span_bytes_load_f64_value" \
            "freestanding-profile/fixed-array: generated C avoids hosted boxed ByteSpan.load"
        expect_log_not_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" \
            "xrt_span_bytes_copy_checked_raw" \
            "freestanding-profile/fixed-array: generated C avoids hosted checked ByteSpan.copyFrom"
        expect_log_not_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" \
            "xrt_span_bytes_common_prefix_checked_raw" \
            "freestanding-profile/fixed-array: generated C avoids hosted checked ByteSpan.commonPrefix"
        expect_log_not_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" \
            "xrt_span_bytes_repeat_from_checked_raw" \
            "freestanding-profile/fixed-array: generated C avoids hosted checked ByteSpan.repeatFrom"
        expect_log_not_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" \
            "xrt_span_fill_checked_raw" \
            "freestanding-profile/fixed-array: generated C avoids hosted checked Span.fill"
        expect_log_not_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" \
            "xrt_span_copy_checked_raw" \
            "freestanding-profile/fixed-array: generated C avoids hosted checked Span.copyFrom"
        expect_log_not_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" \
            "xrt_span_compare_checked_raw" \
            "freestanding-profile/fixed-array: generated C avoids hosted checked Span.compare"
        expect_log_not_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" \
            "xrt_span_as_bytes_checked_raw" \
            "freestanding-profile/fixed-array: generated C avoids hosted checked Span.asBytes"
        expect_log_not_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" \
            "xrt_span_reinterpret_checked_raw" \
            "freestanding-profile/fixed-array: generated C avoids hosted checked ByteSpan.reinterpret"
        expect_log_not_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" \
            "ByteSpan.reinterpret<T>() length is not divisible by target size" \
            "freestanding-profile/fixed-array: generated C proves ByteSpan.reinterpret length relation"
        expect_log_not_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" "__memcpy_chk" \
            "freestanding-profile/fixed-array: generated C avoids checked memcpy builtin"
        expect_log_not_contains "$FREESTANDING_FIXED_ARRAY_KEPT_C" "___memcpy_chk" \
            "freestanding-profile/fixed-array: generated C avoids Darwin checked memcpy symbol"
    else
        record_fail "freestanding-profile/fixed-array: kept C source missing"
        sed 's/^/      /' "$FREESTANDING_FIXED_ARRAY_LOG" | sed -n '1,120p'
    fi
    FREESTANDING_FIXED_ARRAY_UNDEFINED="$(nm_undefined_normalized "$FREESTANDING_FIXED_ARRAY_OBJ")"
    FREESTANDING_FIXED_ARRAY_UNEXPECTED="$(printf '%s\n' "$FREESTANDING_FIXED_ARRAY_UNDEFINED" |
        sed '/^[[:space:]]*$/d' |
        grep -Ev '^(memcpy|memmove|memset|memcmp|xr_hook_panic)$' || true)"
    if [ -z "$FREESTANDING_FIXED_ARRAY_UNEXPECTED" ]; then
        record_pass "freestanding-profile/fixed-array: undefined symbols stay in hook/memcpy family"
    else
        record_fail "freestanding-profile/fixed-array: unexpected undefined symbols"
        printf '%s\n' "$FREESTANDING_FIXED_ARRAY_UNEXPECTED" | sed 's/^/      /'
    fi
else
    record_fail "freestanding-profile/fixed-array: object build failed"
    sed 's/^/      /' "$FREESTANDING_FIXED_ARRAY_LOG" | sed -n '1,120p'
fi

FREESTANDING_MATH_CONST_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_math_constants.xr"
FREESTANDING_MATH_CONST_OBJ="$WORK/freestanding_math_constants.o"
FREESTANDING_MATH_CONST_LOG="$WORK/freestanding_math_constants.log"
if "$XRAY" build --native --profile freestanding --shared --keep-c --rebuild \
        --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_MATH_CONST_OBJ" \
        "$FREESTANDING_MATH_CONST_SRC" >"$FREESTANDING_MATH_CONST_LOG" 2>&1; then
    FREESTANDING_MATH_CONST_KEPT_C="$(sed -n 's/^Kept C source: //p' \
        "$FREESTANDING_MATH_CONST_LOG" | tail -n 1)"
    if [ -f "$FREESTANDING_MATH_CONST_KEPT_C" ]; then
        expect_log_contains "$FREESTANDING_MATH_CONST_KEPT_C" \
            "#include \"xrt_core_freestanding.h\"" \
            "freestanding-profile/math-constants: generated C uses freestanding prelude"
        expect_log_not_contains "$FREESTANDING_MATH_CONST_KEPT_C" "#include \"xrt.h\"" \
            "freestanding-profile/math-constants: generated C avoids hosted umbrella"
    else
        record_fail "freestanding-profile/math-constants: kept C source missing"
        sed 's/^/      /' "$FREESTANDING_MATH_CONST_LOG" | sed -n '1,120p'
    fi
    FREESTANDING_MATH_CONST_UNDEFINED="$(nm_undefined_normalized "$FREESTANDING_MATH_CONST_OBJ")"
    FREESTANDING_MATH_CONST_UNEXPECTED="$(printf '%s\n' "$FREESTANDING_MATH_CONST_UNDEFINED" |
        sed '/^[[:space:]]*$/d' |
        grep -Ev '^(memcpy|memmove|memset|memcmp|xr_hook_panic)$' || true)"
    if [ -z "$FREESTANDING_MATH_CONST_UNEXPECTED" ]; then
        record_pass "freestanding-profile/math-constants: undefined symbols stay in hook/memcpy family"
    else
        record_fail "freestanding-profile/math-constants: unexpected undefined symbols"
        printf '%s\n' "$FREESTANDING_MATH_CONST_UNEXPECTED" | sed 's/^/      /'
    fi
else
    record_fail "freestanding-profile/math-constants: object build failed"
    sed 's/^/      /' "$FREESTANDING_MATH_CONST_LOG" | sed -n '1,120p'
fi

FREESTANDING_SCALAR_COMPARE_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_scalar_compare.xr"
FREESTANDING_SCALAR_COMPARE_OBJ="$WORK/freestanding_scalar_compare.o"
FREESTANDING_SCALAR_COMPARE_LOG="$WORK/freestanding_scalar_compare.log"
if "$XRAY" build --native --profile freestanding --shared --keep-c --rebuild \
        --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_SCALAR_COMPARE_OBJ" \
        "$FREESTANDING_SCALAR_COMPARE_SRC" >"$FREESTANDING_SCALAR_COMPARE_LOG" 2>&1; then
    FREESTANDING_SCALAR_COMPARE_KEPT_C="$(sed -n 's/^Kept C source: //p' \
        "$FREESTANDING_SCALAR_COMPARE_LOG" | tail -n 1)"
    if [ -f "$FREESTANDING_SCALAR_COMPARE_KEPT_C" ]; then
        expect_log_contains "$FREESTANDING_SCALAR_COMPARE_KEPT_C" \
            "#include \"xrt_core_freestanding.h\"" \
            "freestanding-profile/scalar-compare: generated C uses freestanding prelude"
        expect_log_contains "$FREESTANDING_SCALAR_COMPARE_KEPT_C" " < " \
            "freestanding-profile/scalar-compare: generated C uses direct less-than compare"
        expect_log_contains "$FREESTANDING_SCALAR_COMPARE_KEPT_C" " <= " \
            "freestanding-profile/scalar-compare: generated C uses direct less-or-equal compare"
        expect_log_not_contains "$FREESTANDING_SCALAR_COMPARE_KEPT_C" "#include \"xrt.h\"" \
            "freestanding-profile/scalar-compare: generated C avoids hosted umbrella"
    else
        record_fail "freestanding-profile/scalar-compare: kept C source missing"
        sed 's/^/      /' "$FREESTANDING_SCALAR_COMPARE_LOG" | sed -n '1,120p'
    fi
    FREESTANDING_SCALAR_COMPARE_UNDEFINED="$(nm_undefined_normalized "$FREESTANDING_SCALAR_COMPARE_OBJ")"
    FREESTANDING_SCALAR_COMPARE_UNEXPECTED="$(printf '%s\n' "$FREESTANDING_SCALAR_COMPARE_UNDEFINED" |
        sed '/^[[:space:]]*$/d' |
        grep -Ev '^(memcpy|memmove|memset|memcmp|xr_hook_panic)$' || true)"
    if [ -z "$FREESTANDING_SCALAR_COMPARE_UNEXPECTED" ]; then
        record_pass "freestanding-profile/scalar-compare: undefined symbols stay in hook/memcpy family"
    else
        record_fail "freestanding-profile/scalar-compare: unexpected undefined symbols"
        printf '%s\n' "$FREESTANDING_SCALAR_COMPARE_UNEXPECTED" | sed 's/^/      /'
    fi
else
    record_fail "freestanding-profile/scalar-compare: object build failed"
    sed 's/^/      /' "$FREESTANDING_SCALAR_COMPARE_LOG" | sed -n '1,120p'
fi

FREESTANDING_MATH_CALL_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_math_call_reject.xr"
FREESTANDING_MATH_CALL_LOG="$WORK/freestanding_math_call_reject.log"
if "$XRAY" build --native --profile freestanding --dry-run-link --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$WORK/freestanding_math_call_reject" \
        "$FREESTANDING_MATH_CALL_SRC" >"$FREESTANDING_MATH_CALL_LOG" 2>&1; then
    record_fail "freestanding-profile: rejects libm/system math helpers"
    sed 's/^/      /' "$FREESTANDING_MATH_CALL_LOG" | sed -n '1,120p'
else
    expect_log_contains "$FREESTANDING_MATH_CALL_LOG" \
        "freestanding profile rejects math.sqrt" \
        "freestanding-profile: rejects libm-backed math helper"
    expect_log_contains "$FREESTANDING_MATH_CALL_LOG" \
        "freestanding profile rejects math.random" \
        "freestanding-profile: rejects system-random math helper"
fi

FREESTANDING_SHARED_CONST_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_shared_const_reject.xr"
FREESTANDING_SHARED_CONST_LOG="$WORK/freestanding_shared_const_reject.log"
if "$XRAY" build --native --profile freestanding --dry-run-link --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$WORK/freestanding_shared_const_reject" \
        "$FREESTANDING_SHARED_CONST_SRC" >"$FREESTANDING_SHARED_CONST_LOG" 2>&1; then
    record_fail "freestanding-profile: rejects shared declarations"
    sed 's/^/      /' "$FREESTANDING_SHARED_CONST_LOG" | sed -n '1,120p'
else
    expect_log_contains "$FREESTANDING_SHARED_CONST_LOG" \
        "freestanding profile rejects shared declaration" \
        "freestanding-profile: rejects shared declarations"
fi

FREESTANDING_SHARED_SCALAR_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_shared_scalar_static.xr"
FREESTANDING_SHARED_SCALAR_OBJ="$WORK/freestanding_shared_scalar_static.o"
FREESTANDING_SHARED_SCALAR_LOG="$WORK/freestanding_shared_scalar_static.log"
if "$XRAY" build --native --profile freestanding --shared --keep-c --rebuild \
        --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_SHARED_SCALAR_OBJ" \
        "$FREESTANDING_SHARED_SCALAR_SRC" >"$FREESTANDING_SHARED_SCALAR_LOG" 2>&1; then
    FREESTANDING_SHARED_SCALAR_C="$(sed -n 's/^Kept C source: //p' \
        "$FREESTANDING_SHARED_SCALAR_LOG" | tail -n 1)"
    if [ -f "$FREESTANDING_SHARED_SCALAR_C" ]; then
        expect_log_contains "$FREESTANDING_SHARED_SCALAR_C" \
            "static XrValue xrt_shared[2] = {" \
            "freestanding-profile/shared-scalar-static: materializes shared slots as static data"
        expect_log_contains "$FREESTANDING_SHARED_SCALAR_C" \
            "[0] = XR_FROM_INT(INT64_C(41))," \
            "freestanding-profile/shared-scalar-static: initializes scalar shared slot statically"
        expect_log_contains "$FREESTANDING_SHARED_SCALAR_C" "xrt_shared[0]" \
            "freestanding-profile/shared-scalar-static: reads shared slot storage"
        expect_log_not_contains "$FREESTANDING_SHARED_SCALAR_C" \
            "xrt_shared[0] = XR_FROM_INT(INT64_C(41))" \
            "freestanding-profile/shared-scalar-static: elides module-init scalar write"
        expect_log_not_contains "$FREESTANDING_SHARED_SCALAR_C" "#include \"xrt.h\"" \
            "freestanding-profile/shared-scalar-static: avoids hosted umbrella"
    else
        record_fail "freestanding-profile/shared-scalar-static: kept C source missing"
        sed 's/^/      /' "$FREESTANDING_SHARED_SCALAR_LOG" | sed -n '1,120p'
    fi
    FREESTANDING_SHARED_SCALAR_UNDEFINED="$(
        nm_undefined_normalized "$FREESTANDING_SHARED_SCALAR_OBJ")"
    FREESTANDING_SHARED_SCALAR_UNEXPECTED="$(
        printf '%s\n' "$FREESTANDING_SHARED_SCALAR_UNDEFINED" |
            sed '/^[[:space:]]*$/d' |
            grep -Ev '^(memcpy|memmove|memset|memcmp)$' || true)"
    if [ -z "$FREESTANDING_SHARED_SCALAR_UNEXPECTED" ]; then
        record_pass "freestanding-profile/shared-scalar-static: undefined symbols stay in memcpy family"
    else
        record_fail "freestanding-profile/shared-scalar-static: unexpected undefined symbols"
        printf '%s\n' "$FREESTANDING_SHARED_SCALAR_UNEXPECTED" | sed 's/^/      /'
    fi
else
    record_fail "freestanding-profile/shared-scalar-static: object build failed"
    sed 's/^/      /' "$FREESTANDING_SHARED_SCALAR_LOG" | sed -n '1,120p'
fi

FREESTANDING_SHARED_STRING_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_shared_string_static.xr"
FREESTANDING_SHARED_STRING_OBJ="$WORK/freestanding_shared_string_static.o"
FREESTANDING_SHARED_STRING_LOG="$WORK/freestanding_shared_string_static.log"
if "$XRAY" build --native --profile freestanding --shared --keep-c --rebuild \
        --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_SHARED_STRING_OBJ" \
        "$FREESTANDING_SHARED_STRING_SRC" >"$FREESTANDING_SHARED_STRING_LOG" 2>&1; then
    FREESTANDING_SHARED_STRING_C="$(sed -n 's/^Kept C source: //p' \
        "$FREESTANDING_SHARED_STRING_LOG" | tail -n 1)"
    if [ -f "$FREESTANDING_SHARED_STRING_C" ]; then
        expect_log_contains "$FREESTANDING_SHARED_STRING_C" \
            "static const xrt_str_t xrt_shared_init_str_0 = {INT64_C(4)" \
            "freestanding-profile/shared-string-static: materializes string literal header"
        expect_log_contains "$FREESTANDING_SHARED_STRING_C" \
            "[0] = {.tag = XR_TAG_STR, .ptr = (void *) &xrt_shared_init_str_0}" \
            "freestanding-profile/shared-string-static: initializes string shared slot statically"
        expect_log_contains "$FREESTANDING_SHARED_STRING_C" "xrt_shared[0]" \
            "freestanding-profile/shared-string-static: reads shared slot storage"
        expect_log_not_contains "$FREESTANDING_SHARED_STRING_C" \
            "xrt_shared[0] = {.tag = XR_TAG_STR" \
            "freestanding-profile/shared-string-static: elides module-init string write"
        expect_log_not_contains "$FREESTANDING_SHARED_STRING_C" "#include \"xrt.h\"" \
            "freestanding-profile/shared-string-static: avoids hosted umbrella"
    else
        record_fail "freestanding-profile/shared-string-static: kept C source missing"
        sed 's/^/      /' "$FREESTANDING_SHARED_STRING_LOG" | sed -n '1,120p'
    fi
    FREESTANDING_SHARED_STRING_UNDEFINED="$(
        nm_undefined_normalized "$FREESTANDING_SHARED_STRING_OBJ")"
    FREESTANDING_SHARED_STRING_UNEXPECTED="$(
        printf '%s\n' "$FREESTANDING_SHARED_STRING_UNDEFINED" |
            sed '/^[[:space:]]*$/d' |
            grep -Ev '^(memcpy|memmove|memset|memcmp|xr_hook_write)$' || true)"
    if [ -z "$FREESTANDING_SHARED_STRING_UNEXPECTED" ]; then
        record_pass "freestanding-profile/shared-string-static: undefined symbols stay in write-hook/memcpy family"
    else
        record_fail "freestanding-profile/shared-string-static: unexpected undefined symbols"
        printf '%s\n' "$FREESTANDING_SHARED_STRING_UNEXPECTED" | sed 's/^/      /'
    fi
else
    record_fail "freestanding-profile/shared-string-static: object build failed"
    sed 's/^/      /' "$FREESTANDING_SHARED_STRING_LOG" | sed -n '1,120p'
fi

FREESTANDING_TOP_CONST_SCALAR_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_top_const_scalar.xr"
FREESTANDING_TOP_CONST_SCALAR_OBJ="$WORK/freestanding_top_const_scalar.o"
FREESTANDING_TOP_CONST_SCALAR_LOG="$WORK/freestanding_top_const_scalar.log"
if "$XRAY" build --native --profile freestanding --shared --keep-c --rebuild \
        --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_TOP_CONST_SCALAR_OBJ" \
        "$FREESTANDING_TOP_CONST_SCALAR_SRC" >"$FREESTANDING_TOP_CONST_SCALAR_LOG" 2>&1; then
    FREESTANDING_TOP_CONST_SCALAR_C="$(sed -n 's/^Kept C source: //p' \
        "$FREESTANDING_TOP_CONST_SCALAR_LOG" | tail -n 1)"
    if [ -f "$FREESTANDING_TOP_CONST_SCALAR_C" ]; then
        expect_log_contains "$FREESTANDING_TOP_CONST_SCALAR_C" "static const int64_t _xctscalar_" \
            "freestanding-profile/top-const-scalar: materializes attributed integer/bool/char const as static data"
        expect_log_contains "$FREESTANDING_TOP_CONST_SCALAR_C" \
            "const int64_t xray_const_freestanding_top_const_scalar_MAGIC" \
            "freestanding-profile/top-const-scalar: weak integer const uses stable external data symbol"
        expect_log_contains "$FREESTANDING_TOP_CONST_SCALAR_C" "static const double _xctscalar_" \
            "freestanding-profile/top-const-scalar: materializes attributed float const as static data"
        expect_log_contains "$FREESTANDING_TOP_CONST_SCALAR_C" \
            "const xrt_str_t xray_const_freestanding_top_const_scalar_LABEL" \
            "freestanding-profile/top-const-scalar: weak string const uses stable external data symbol"
        expect_log_contains "$FREESTANDING_TOP_CONST_SCALAR_C" "static const XrValue _xctvalue_" \
            "freestanding-profile/top-const-scalar: materializes attributed null const as static data"
        expect_log_contains "$FREESTANDING_TOP_CONST_SCALAR_C" \
            "XRT_ATTR_SECTION(\"__DATA,.xray_magic\") XRT_ATTR_WEAK XRT_ATTR_USED" \
            "freestanding-profile/top-const-scalar: emits section/weak/used attrs on scalar const"
        expect_log_contains "$FREESTANDING_TOP_CONST_SCALAR_C" \
            "XRT_ATTR_SECTION(\"__DATA,.xray_label\") XRT_ATTR_WEAK XRT_ATTR_USED" \
            "freestanding-profile/top-const-scalar: emits section/weak/used attrs on string const"
        expect_log_not_contains "$FREESTANDING_TOP_CONST_SCALAR_C" "(xrt_shared[0] =" \
            "freestanding-profile/top-const-scalar: elides const slot initialization"
        expect_log_not_contains "$FREESTANDING_TOP_CONST_SCALAR_C" "xrt_shared[" \
            "freestanding-profile/top-const-scalar: avoids shared-slot storage"
        expect_log_not_contains "$FREESTANDING_TOP_CONST_SCALAR_C" "xrt_array_ref_to_owned" \
            "freestanding-profile/top-const-scalar: avoids hosted shared-slot ownership path"
    else
        record_fail "freestanding-profile/top-const-scalar: kept C source missing"
        sed 's/^/      /' "$FREESTANDING_TOP_CONST_SCALAR_LOG" | sed -n '1,120p'
    fi
    FREESTANDING_TOP_CONST_SCALAR_UNDEFINED="$(
        nm_undefined_normalized "$FREESTANDING_TOP_CONST_SCALAR_OBJ")"
    FREESTANDING_TOP_CONST_SCALAR_UNEXPECTED="$(
        printf '%s\n' "$FREESTANDING_TOP_CONST_SCALAR_UNDEFINED" |
            sed '/^[[:space:]]*$/d' |
            grep -Ev '^(memcpy|memmove|memset|memcmp|xr_hook_panic|xr_hook_write)$' || true)"
    if [ -z "$FREESTANDING_TOP_CONST_SCALAR_UNEXPECTED" ]; then
        record_pass "freestanding-profile/top-const-scalar: undefined symbols stay in hook/memcpy family"
    else
        record_fail "freestanding-profile/top-const-scalar: unexpected undefined symbols"
        printf '%s\n' "$FREESTANDING_TOP_CONST_SCALAR_UNEXPECTED" | sed 's/^/      /'
    fi
    FREESTANDING_TOP_CONST_SCALAR_NM="$WORK/freestanding_top_const_scalar.nm"
    if object_has_weak_symbol "$FREESTANDING_TOP_CONST_SCALAR_OBJ" \
            "xray_const_freestanding_top_const_scalar_MAGIC" \
            "$FREESTANDING_TOP_CONST_SCALAR_NM"; then
        record_pass "freestanding-profile/top-const-scalar: weak integer data symbol is external"
    else
        record_fail "freestanding-profile/top-const-scalar: weak integer data symbol missing"
        sed 's/^/      /' "$FREESTANDING_TOP_CONST_SCALAR_NM" | sed -n '1,80p'
    fi
    if object_has_weak_symbol "$FREESTANDING_TOP_CONST_SCALAR_OBJ" \
            "xray_const_freestanding_top_const_scalar_LABEL" \
            "$FREESTANDING_TOP_CONST_SCALAR_NM"; then
        record_pass "freestanding-profile/top-const-scalar: weak string data symbol is external"
    else
        record_fail "freestanding-profile/top-const-scalar: weak string data symbol missing"
        sed 's/^/      /' "$FREESTANDING_TOP_CONST_SCALAR_NM" | sed -n '1,80p'
    fi
else
    record_fail "freestanding-profile/top-const-scalar: object build failed"
    sed 's/^/      /' "$FREESTANDING_TOP_CONST_SCALAR_LOG" | sed -n '1,120p'
fi

FREESTANDING_TOP_CONST_AGG_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_top_const_aggregate.xr"
FREESTANDING_TOP_CONST_AGG_OBJ="$WORK/freestanding_top_const_aggregate.o"
FREESTANDING_TOP_CONST_AGG_LOG="$WORK/freestanding_top_const_aggregate.log"
if "$XRAY" build --native --profile freestanding --shared --keep-c --rebuild \
        --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_TOP_CONST_AGG_OBJ" \
        "$FREESTANDING_TOP_CONST_AGG_SRC" >"$FREESTANDING_TOP_CONST_AGG_LOG" 2>&1; then
    FREESTANDING_TOP_CONST_AGG_C="$(sed -n 's/^Kept C source: //p' \
        "$FREESTANDING_TOP_CONST_AGG_LOG" | tail -n 1)"
    if [ -f "$FREESTANDING_TOP_CONST_AGG_C" ]; then
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" "INT64_C(14)" \
            "freestanding-profile/top-const-aggregate: folds aggregate uses into export"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "const int64_t xray_const_freestanding_top_const_aggregate_TABLE" \
            "freestanding-profile/top-const-aggregate: weak scalar table uses stable external data symbol"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "XRT_ATTR_SECTION(\"__DATA,.xray_table\") XRT_ATTR_WEAK XRT_ATTR_USED" \
            "freestanding-profile/top-const-aggregate: emits section/weak/used attrs on static table"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" "static const uint8_t _xctarr_" \
            "freestanding-profile/top-const-aggregate: materializes byte table as static data"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" "static const double _xctarr_" \
            "freestanding-profile/top-const-aggregate: materializes float table as static data"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "const uint8_t xray_const_freestanding_top_const_aggregate_MATRIX[2][4]" \
            "freestanding-profile/top-const-aggregate: materializes fixed-array matrix as static data"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "xray_const_freestanding_top_const_aggregate_MATRIX[2][4] XRT_ATTR_SECTION(\"__DATA,.xray_matrix\") XRT_ATTR_WEAK XRT_ATTR_USED" \
            "freestanding-profile/top-const-aggregate: emits attrs on fixed-array matrix data"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "xray_const_freestanding_top_const_aggregate_MATRIX[_outer_idx][_idx]" \
            "freestanding-profile/top-const-aggregate: reads fixed-array matrix directly"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "const XrValue xray_const_freestanding_top_const_aggregate_LABEL_MATRIX[2][2]" \
            "freestanding-profile/top-const-aggregate: materializes string fixed-array matrix as static data"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "xray_const_freestanding_top_const_aggregate_LABEL_MATRIX[2][2] XRT_ATTR_SECTION(\"__DATA,.xray_lmat\") XRT_ATTR_WEAK XRT_ATTR_USED" \
            "freestanding-profile/top-const-aggregate: emits attrs on string fixed-array matrix data"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "xray_const_freestanding_top_const_aggregate_LABEL_MATRIX[_outer_idx][_idx]" \
            "freestanding-profile/top-const-aggregate: reads string fixed-array matrix directly"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "const XrValue xray_const_freestanding_top_const_aggregate_LABEL_CUBE[2][2][2]" \
            "freestanding-profile/top-const-aggregate: materializes string fixed-array cube as static data"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "xray_const_freestanding_top_const_aggregate_LABEL_CUBE[2][2][2] XRT_ATTR_SECTION(\"__DATA,.xray_lcube\") XRT_ATTR_WEAK XRT_ATTR_USED" \
            "freestanding-profile/top-const-aggregate: emits attrs on string fixed-array cube data"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "xray_const_freestanding_top_const_aggregate_LABEL_CUBE[_outer_idx][_middle_idx][_idx]" \
            "freestanding-profile/top-const-aggregate: reads string fixed-array cube directly"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" "static const struct" \
            "freestanding-profile/top-const-aggregate: materializes scalar struct as static data"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" "_xctstruct_" \
            "freestanding-profile/top-const-aggregate: names scalar struct static data"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "xray_const_freestanding_top_const_aggregate_HEADER" \
            "freestanding-profile/top-const-aggregate: weak struct uses stable external data symbol"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "XRT_ATTR_SECTION(\"__DATA,.xray_header\") XRT_ATTR_WEAK" \
            "freestanding-profile/top-const-aggregate: emits section/weak attrs on static struct"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" "__attribute__((packed" \
            "freestanding-profile/top-const-aggregate: preserves packed static struct layout"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" "aligned(16)" \
            "freestanding-profile/top-const-aggregate: preserves aligned static struct layout"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" "static const union" \
            "freestanding-profile/top-const-aggregate: materializes union as static data"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" ".i = (int32_t)INT64_C(-1)" \
            "freestanding-profile/top-const-aggregate: initializes active union integer field"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" ".u = (uint32_t)INT64_C(16909060)" \
            "freestanding-profile/top-const-aggregate: initializes active union word field"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" ".magic" \
            "freestanding-profile/top-const-aggregate: reads static struct magic field"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" ".flags" \
            "freestanding-profile/top-const-aggregate: reads static struct flags field"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" ".tag" \
            "freestanding-profile/top-const-aggregate: reads packed static struct tag field"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" ".value" \
            "freestanding-profile/top-const-aggregate: reads packed/aligned static struct value field"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" ".u" \
            "freestanding-profile/top-const-aggregate: reads static union integer lane directly"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" ".b[" \
            "freestanding-profile/top-const-aggregate: reads static union fixed-array lane directly"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" ".bytes" \
            "freestanding-profile/top-const-aggregate: reads static struct fixed-array field"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" ".inner" \
            "freestanding-profile/top-const-aggregate: materializes nested static struct field"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" ".inner.code" \
            "freestanding-profile/top-const-aggregate: reads nested static struct scalar field"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" "XrValue label" \
            "freestanding-profile/top-const-aggregate: materializes string struct field as XrValue"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" ".label = (XrValue){.tag = XR_TAG_STR" \
            "freestanding-profile/top-const-aggregate: initializes string struct field statically"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" "XrValue labels[2]" \
            "freestanding-profile/top-const-aggregate: materializes string fixed-array struct field as XrValue lanes"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" ".labels = {(XrValue){.tag = XR_TAG_STR" \
            "freestanding-profile/top-const-aggregate: initializes string fixed-array struct field statically"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" ".labels[" \
            "freestanding-profile/top-const-aggregate: reads static string fixed-array struct field directly"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" "_xcttuple_" \
            "freestanding-profile/top-const-aggregate: names scalar tuple static data"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "xray_const_freestanding_top_const_aggregate_PAIR" \
            "freestanding-profile/top-const-aggregate: weak tuple uses stable external data symbol"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "XRT_ATTR_SECTION(\"__DATA,.xray_pair\") XRT_ATTR_WEAK XRT_ATTR_USED" \
            "freestanding-profile/top-const-aggregate: emits section/weak/used attrs on static tuple"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" ".f0" \
            "freestanding-profile/top-const-aggregate: reads static tuple first field"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" ".f2" \
            "freestanding-profile/top-const-aggregate: reads static tuple float field"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" "static const xrt_str_t _xstr_" \
            "freestanding-profile/top-const-aggregate: materializes string const as static data"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" "static const XrValue _xctarr_" \
            "freestanding-profile/top-const-aggregate: materializes string fixed-array as static data"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "const struct { int64_t code; uint8_t flag; } xray_const_freestanding_top_const_aggregate_ENTRIES[2]" \
            "freestanding-profile/top-const-aggregate: materializes struct fixed-array as static data"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "xray_const_freestanding_top_const_aggregate_ENTRIES[2] XRT_ATTR_SECTION(\"__DATA,.xray_entries\") XRT_ATTR_WEAK XRT_ATTR_USED" \
            "freestanding-profile/top-const-aggregate: emits attrs on struct fixed-array data"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "xray_const_freestanding_top_const_aggregate_ENTRIES[_idx].code" \
            "freestanding-profile/top-const-aggregate: reads struct fixed-array integer field directly"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "xray_const_freestanding_top_const_aggregate_ENTRIES[_idx].flag" \
            "freestanding-profile/top-const-aggregate: reads struct fixed-array bool field directly"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "const struct { uint8_t samples[4]; int64_t weight; } xray_const_freestanding_top_const_aggregate_ROWS[2]" \
            "freestanding-profile/top-const-aggregate: materializes struct-array fixed-field data"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "xray_const_freestanding_top_const_aggregate_ROWS[2] XRT_ATTR_SECTION(\"__DATA,.xray_rows\") XRT_ATTR_WEAK XRT_ATTR_USED" \
            "freestanding-profile/top-const-aggregate: emits attrs on struct-array fixed-field data"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "xray_const_freestanding_top_const_aggregate_ROWS[_outer_idx].samples[_idx]" \
            "freestanding-profile/top-const-aggregate: reads struct-array fixed-array field directly"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "xray_const_freestanding_top_const_aggregate_ROWS[_idx].weight" \
            "freestanding-profile/top-const-aggregate: still reads sibling struct-array scalar field directly"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "const struct { struct { int64_t code; uint8_t flag; } inner; int64_t base; } xray_const_freestanding_top_const_aggregate_GROUPS[2]" \
            "freestanding-profile/top-const-aggregate: materializes struct-array nested data"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "xray_const_freestanding_top_const_aggregate_GROUPS[2] XRT_ATTR_SECTION(\"__DATA,.xray_groups\") XRT_ATTR_WEAK XRT_ATTR_USED" \
            "freestanding-profile/top-const-aggregate: emits attrs on struct-array nested data"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "xray_const_freestanding_top_const_aggregate_GROUPS[_idx].inner.code" \
            "freestanding-profile/top-const-aggregate: reads struct-array nested scalar field directly"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "xray_const_freestanding_top_const_aggregate_GROUPS[_idx].base" \
            "freestanding-profile/top-const-aggregate: reads struct-array nested sibling field directly"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "const struct { struct { XrValue label; int64_t code; } inner; int64_t base; } xray_const_freestanding_top_const_aggregate_LABEL_GROUPS[2]" \
            "freestanding-profile/top-const-aggregate: materializes struct-array nested string data"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "xray_const_freestanding_top_const_aggregate_LABEL_GROUPS[2] XRT_ATTR_SECTION(\"__DATA,.xray_lgroups\") XRT_ATTR_WEAK XRT_ATTR_USED" \
            "freestanding-profile/top-const-aggregate: emits attrs on struct-array nested string data"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "xray_const_freestanding_top_const_aggregate_LABEL_GROUPS[_idx].inner.label" \
            "freestanding-profile/top-const-aggregate: reads struct-array nested string field directly"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "xray_const_freestanding_top_const_aggregate_LABEL_GROUPS[_idx].inner.code" \
            "freestanding-profile/top-const-aggregate: reads struct-array nested string sibling scalar directly"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" \
            "xray_const_freestanding_top_const_aggregate_LABEL_GROUPS[_idx].base" \
            "freestanding-profile/top-const-aggregate: reads struct-array nested string base directly"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" "xr_str_lit(&_xstr_" \
            "freestanding-profile/top-const-aggregate: reads string const through literal header"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" "XrValue f0" \
            "freestanding-profile/top-const-aggregate: materializes string tuple lane as XrValue"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" ".f0 = (XrValue){.tag = XR_TAG_STR" \
            "freestanding-profile/top-const-aggregate: initializes string tuple lane statically"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" "struct { int64_t f0; XrValue f1; } f0" \
            "freestanding-profile/top-const-aggregate: materializes nested tuple first lane as static struct"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" "struct { double f0; int64_t f1; } f1" \
            "freestanding-profile/top-const-aggregate: materializes nested tuple second lane as static struct"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" ".f0.f0" \
            "freestanding-profile/top-const-aggregate: reads nested tuple integer lane directly"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" ".f1.f0" \
            "freestanding-profile/top-const-aggregate: reads nested tuple float lane directly"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" ".f0.f1" \
            "freestanding-profile/top-const-aggregate: reads nested tuple string lane directly"
        expect_log_contains "$FREESTANDING_TOP_CONST_AGG_C" "xrt_println(" \
            "freestanding-profile/top-const-aggregate: prints string const through write hook"
        expect_log_not_contains "$FREESTANDING_TOP_CONST_AGG_C" "xrt_shared[" \
            "freestanding-profile/top-const-aggregate: avoids shared-slot storage"
        expect_log_not_contains "$FREESTANDING_TOP_CONST_AGG_C" "xrt_arc_alloc" \
            "freestanding-profile/top-const-aggregate: avoids hosted aggregate allocation"
        expect_log_not_contains "$FREESTANDING_TOP_CONST_AGG_C" "xr_array_ref" \
            "freestanding-profile/top-const-aggregate: avoids runtime fixed-array materialization"
        expect_log_not_contains "$FREESTANDING_TOP_CONST_AGG_C" "xr_aggregate_ref" \
            "freestanding-profile/top-const-aggregate: avoids runtime nested-struct materialization"
        expect_log_not_contains "$FREESTANDING_TOP_CONST_AGG_C" "xrt_tuple_get" \
            "freestanding-profile/top-const-aggregate: avoids runtime tuple materialization"
        expect_log_not_contains "$FREESTANDING_TOP_CONST_AGG_C" "xrt_tuple_make" \
            "freestanding-profile/top-const-aggregate: avoids runtime tuple construction"
        expect_log_not_contains "$FREESTANDING_TOP_CONST_AGG_C" "xrt_tuple_new" \
            "freestanding-profile/top-const-aggregate: avoids empty tuple runtime construction"
    else
        record_fail "freestanding-profile/top-const-aggregate: kept C source missing"
        sed 's/^/      /' "$FREESTANDING_TOP_CONST_AGG_LOG" | sed -n '1,120p'
    fi
    FREESTANDING_TOP_CONST_AGG_UNDEFINED="$(
        nm_undefined_normalized "$FREESTANDING_TOP_CONST_AGG_OBJ")"
    FREESTANDING_TOP_CONST_AGG_UNEXPECTED="$(
        printf '%s\n' "$FREESTANDING_TOP_CONST_AGG_UNDEFINED" |
            sed '/^[[:space:]]*$/d' |
            grep -Ev '^(memcpy|memmove|memset|memcmp|xr_hook_panic|xr_hook_write)$' || true)"
    if [ -z "$FREESTANDING_TOP_CONST_AGG_UNEXPECTED" ]; then
        record_pass "freestanding-profile/top-const-aggregate: undefined symbols stay in hook/memcpy family"
    else
        record_fail "freestanding-profile/top-const-aggregate: unexpected undefined symbols"
        printf '%s\n' "$FREESTANDING_TOP_CONST_AGG_UNEXPECTED" | sed 's/^/      /'
    fi
    FREESTANDING_TOP_CONST_AGG_NM="$WORK/freestanding_top_const_aggregate.nm"
    if object_has_weak_symbol "$FREESTANDING_TOP_CONST_AGG_OBJ" \
            "xray_const_freestanding_top_const_aggregate_TABLE" \
            "$FREESTANDING_TOP_CONST_AGG_NM"; then
        record_pass "freestanding-profile/top-const-aggregate: weak fixed-array data symbol is external"
    else
        record_fail "freestanding-profile/top-const-aggregate: weak fixed-array data symbol missing"
        sed 's/^/      /' "$FREESTANDING_TOP_CONST_AGG_NM" | sed -n '1,80p'
    fi
    if object_has_weak_symbol "$FREESTANDING_TOP_CONST_AGG_OBJ" \
            "xray_const_freestanding_top_const_aggregate_MATRIX" \
            "$FREESTANDING_TOP_CONST_AGG_NM"; then
        record_pass "freestanding-profile/top-const-aggregate: weak fixed-array matrix data symbol is external"
    else
        record_fail "freestanding-profile/top-const-aggregate: weak fixed-array matrix data symbol missing"
        sed 's/^/      /' "$FREESTANDING_TOP_CONST_AGG_NM" | sed -n '1,80p'
    fi
    if object_has_weak_symbol "$FREESTANDING_TOP_CONST_AGG_OBJ" \
            "xray_const_freestanding_top_const_aggregate_LABEL_MATRIX" \
            "$FREESTANDING_TOP_CONST_AGG_NM"; then
        record_pass "freestanding-profile/top-const-aggregate: weak string fixed-array matrix data symbol is external"
    else
        record_fail "freestanding-profile/top-const-aggregate: weak string fixed-array matrix data symbol missing"
        sed 's/^/      /' "$FREESTANDING_TOP_CONST_AGG_NM" | sed -n '1,80p'
    fi
    if object_has_weak_symbol "$FREESTANDING_TOP_CONST_AGG_OBJ" \
            "xray_const_freestanding_top_const_aggregate_LABEL_CUBE" \
            "$FREESTANDING_TOP_CONST_AGG_NM"; then
        record_pass "freestanding-profile/top-const-aggregate: weak string fixed-array cube data symbol is external"
    else
        record_fail "freestanding-profile/top-const-aggregate: weak string fixed-array cube data symbol missing"
        sed 's/^/      /' "$FREESTANDING_TOP_CONST_AGG_NM" | sed -n '1,80p'
    fi
    if object_has_weak_symbol "$FREESTANDING_TOP_CONST_AGG_OBJ" \
            "xray_const_freestanding_top_const_aggregate_HEADER" \
            "$FREESTANDING_TOP_CONST_AGG_NM"; then
        record_pass "freestanding-profile/top-const-aggregate: weak struct data symbol is external"
    else
        record_fail "freestanding-profile/top-const-aggregate: weak struct data symbol missing"
        sed 's/^/      /' "$FREESTANDING_TOP_CONST_AGG_NM" | sed -n '1,80p'
    fi
    if object_has_weak_symbol "$FREESTANDING_TOP_CONST_AGG_OBJ" \
            "xray_const_freestanding_top_const_aggregate_PAIR" \
            "$FREESTANDING_TOP_CONST_AGG_NM"; then
        record_pass "freestanding-profile/top-const-aggregate: weak tuple data symbol is external"
    else
        record_fail "freestanding-profile/top-const-aggregate: weak tuple data symbol missing"
        sed 's/^/      /' "$FREESTANDING_TOP_CONST_AGG_NM" | sed -n '1,80p'
    fi
else
    record_fail "freestanding-profile/top-const-aggregate: object build failed"
    sed 's/^/      /' "$FREESTANDING_TOP_CONST_AGG_LOG" | sed -n '1,120p'
fi

FREESTANDING_STATIC_IMPORT_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_static_data_import.xr"
FREESTANDING_STATIC_IMPORT_OBJ="$WORK/freestanding_static_data_import.o"
FREESTANDING_STATIC_IMPORT_LOG="$WORK/freestanding_static_data_import.log"
if "$XRAY" build --native --profile freestanding --shared --keep-c --rebuild \
        --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_STATIC_IMPORT_OBJ" \
        "$FREESTANDING_STATIC_IMPORT_SRC" >"$FREESTANDING_STATIC_IMPORT_LOG" 2>&1; then
    FREESTANDING_STATIC_IMPORT_C_FILES="$(sed -n 's/^Kept C source: //p' \
        "$FREESTANDING_STATIC_IMPORT_LOG")"
    FREESTANDING_STATIC_IMPORT_EXPORT_C="$(
        printf '%s\n' "$FREESTANDING_STATIC_IMPORT_C_FILES" |
            while IFS= read -r cfile; do
                if [ -f "$cfile" ] &&
                   grep -Fq 'XRT_ATTR_SECTION("__DATA,.xr_imag")' "$cfile"; then
                    printf '%s\n' "$cfile"
                    break
                fi
            done)"
    FREESTANDING_STATIC_IMPORT_ENTRY_C="$(
        printf '%s\n' "$FREESTANDING_STATIC_IMPORT_C_FILES" |
            while IFS= read -r cfile; do
                if [ -f "$cfile" ] &&
                   grep -Fq 'extern const int64_t xray_const__freestanding_static_data_lib_MAGIC' "$cfile"; then
                    printf '%s\n' "$cfile"
                    break
                fi
            done)"
    if [ -n "$FREESTANDING_STATIC_IMPORT_EXPORT_C" ] &&
       [ -f "$FREESTANDING_STATIC_IMPORT_EXPORT_C" ]; then
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_EXPORT_C" \
            'xray_const__freestanding_static_data_lib_MAGIC XRT_ATTR_SECTION("__DATA,.xr_imag") XRT_ATTR_WEAK XRT_ATTR_USED' \
            "freestanding-profile/static-import: exporter keeps scalar section/weak/used attrs"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_EXPORT_C" \
            'xray_const__freestanding_static_data_lib_HEADER XRT_ATTR_SECTION("__DATA,.xr_ihead") XRT_ATTR_WEAK XRT_ATTR_USED' \
            "freestanding-profile/static-import: exporter keeps struct section/weak/used attrs"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_EXPORT_C" \
            'xray_const__freestanding_static_data_lib_MATRIX[2][4] XRT_ATTR_SECTION("__DATA,.xr_imatrix") XRT_ATTR_WEAK XRT_ATTR_USED' \
            "freestanding-profile/static-import: exporter keeps fixed-array matrix section/weak/used attrs"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_EXPORT_C" \
            'xray_const__freestanding_static_data_lib_LABEL_MATRIX[2][2] XRT_ATTR_SECTION("__DATA,.xr_ilmat") XRT_ATTR_WEAK XRT_ATTR_USED' \
            "freestanding-profile/static-import: exporter keeps string fixed-array matrix section/weak/used attrs"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_EXPORT_C" \
            'xray_const__freestanding_static_data_lib_LABEL_CUBE[2][2][2] XRT_ATTR_SECTION("__DATA,.xr_ilcube") XRT_ATTR_WEAK XRT_ATTR_USED' \
            "freestanding-profile/static-import: exporter keeps string fixed-array cube section/weak/used attrs"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_EXPORT_C" \
            'const struct { struct { int64_t f0; XrValue f1; } f0; struct { XrValue f0; int64_t f1; } f1; } xray_const__freestanding_static_data_lib_NESTED_LABEL' \
            "freestanding-profile/static-import: exporter materializes nested tuple string lanes"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_EXPORT_C" \
            "const struct __attribute__((packed, aligned(16)))" \
            "freestanding-profile/static-import: exporter preserves packed/aligned struct layout"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_EXPORT_C" \
            'xray_const__freestanding_static_data_lib_ENTRIES[2] XRT_ATTR_SECTION("__DATA,.xr_ient") XRT_ATTR_WEAK XRT_ATTR_USED' \
            "freestanding-profile/static-import: exporter keeps struct-array section/weak/used attrs"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_EXPORT_C" \
            'xray_const__freestanding_static_data_lib_ROWS[2] XRT_ATTR_SECTION("__DATA,.xr_irow") XRT_ATTR_WEAK XRT_ATTR_USED' \
            "freestanding-profile/static-import: exporter keeps struct-array fixed-field section/weak/used attrs"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_EXPORT_C" \
            'xray_const__freestanding_static_data_lib_GROUPS[2] XRT_ATTR_SECTION("__DATA,.xr_igroup") XRT_ATTR_WEAK XRT_ATTR_USED' \
            "freestanding-profile/static-import: exporter keeps struct-array nested section/weak/used attrs"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_EXPORT_C" \
            'xray_const__freestanding_static_data_lib_LABEL_GROUPS[2] XRT_ATTR_SECTION("__DATA,.xr_ilgrp") XRT_ATTR_WEAK XRT_ATTR_USED' \
            "freestanding-profile/static-import: exporter keeps struct-array nested string section/weak/used attrs"
    else
        record_fail "freestanding-profile/static-import: exporter kept C source missing"
        sed 's/^/      /' "$FREESTANDING_STATIC_IMPORT_LOG" | sed -n '1,120p'
    fi
    if [ -n "$FREESTANDING_STATIC_IMPORT_ENTRY_C" ] &&
       [ -f "$FREESTANDING_STATIC_IMPORT_ENTRY_C" ]; then
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" \
            'extern const int64_t xray_const__freestanding_static_data_lib_MAGIC' \
            "freestanding-profile/static-import: importer declares scalar extern"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" \
            'extern const uint8_t xray_const__freestanding_static_data_lib_MATRIX[2][4]' \
            "freestanding-profile/static-import: importer declares fixed-array matrix extern"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" \
            'extern const XrValue xray_const__freestanding_static_data_lib_LABEL_MATRIX[2][2]' \
            "freestanding-profile/static-import: importer declares string fixed-array matrix extern"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" \
            'extern const XrValue xray_const__freestanding_static_data_lib_LABEL_CUBE[2][2][2]' \
            "freestanding-profile/static-import: importer declares string fixed-array cube extern"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" \
            'extern const struct { struct { int64_t f0; XrValue f1; } f0; struct { XrValue f0; int64_t f1; } f1; } xray_const__freestanding_static_data_lib_NESTED_LABEL' \
            "freestanding-profile/static-import: importer declares nested tuple string extern"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" \
            'extern const struct { int64_t magic; int64_t flags; } xray_const__freestanding_static_data_lib_HEADER' \
            "freestanding-profile/static-import: importer declares struct extern"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" \
            'extern const struct __attribute__((packed, aligned(16))) { uint8_t tag; uint32_t value; } xray_const__freestanding_static_data_lib_PACKED_ALIGNED' \
            "freestanding-profile/static-import: importer preserves packed/aligned extern layout"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" \
            'extern const struct { int64_t code; uint8_t flag; } xray_const__freestanding_static_data_lib_ENTRIES[2]' \
            "freestanding-profile/static-import: importer declares struct-array extern"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" \
            'extern const struct { uint8_t samples[4]; int64_t weight; } xray_const__freestanding_static_data_lib_ROWS[2]' \
            "freestanding-profile/static-import: importer declares struct-array fixed-field extern"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" \
            'extern const struct { struct { int64_t code; } inner; int64_t base; } xray_const__freestanding_static_data_lib_GROUPS[2]' \
            "freestanding-profile/static-import: importer declares struct-array nested extern"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" \
            'extern const struct { struct { XrValue label; int64_t code; } inner; int64_t base; } xray_const__freestanding_static_data_lib_LABEL_GROUPS[2]' \
            "freestanding-profile/static-import: importer declares struct-array nested string extern"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" \
            'xray_const__freestanding_static_data_lib_MATRIX[_outer_idx][_idx]' \
            "freestanding-profile/static-import: importer reads fixed-array matrix directly"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" \
            'xray_const__freestanding_static_data_lib_LABEL_MATRIX[_outer_idx][_idx]' \
            "freestanding-profile/static-import: importer reads string fixed-array matrix directly"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" \
            'xray_const__freestanding_static_data_lib_LABEL_CUBE[_outer_idx][_middle_idx][_idx]' \
            "freestanding-profile/static-import: importer reads string fixed-array cube directly"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" \
            'xray_const__freestanding_static_data_lib_NESTED_LABEL.f0.f0' \
            "freestanding-profile/static-import: importer reads nested tuple first scalar lane directly"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" \
            'xray_const__freestanding_static_data_lib_NESTED_LABEL.f0.f1' \
            "freestanding-profile/static-import: importer reads nested tuple first string lane directly"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" \
            'xray_const__freestanding_static_data_lib_NESTED_LABEL.f1.f0' \
            "freestanding-profile/static-import: importer reads nested tuple second string lane directly"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" \
            'xray_const__freestanding_static_data_lib_NESTED_LABEL.f1.f1' \
            "freestanding-profile/static-import: importer reads nested tuple second scalar lane directly"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" \
            'xray_const__freestanding_static_data_lib_ENTRIES[_idx].code' \
            "freestanding-profile/static-import: importer reads struct-array integer field directly"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" \
            'xray_const__freestanding_static_data_lib_ENTRIES[_idx].flag' \
            "freestanding-profile/static-import: importer reads struct-array bool field directly"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" \
            'xray_const__freestanding_static_data_lib_ROWS[_outer_idx].samples[_idx]' \
            "freestanding-profile/static-import: importer reads struct-array fixed-array field directly"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" \
            'xray_const__freestanding_static_data_lib_ROWS[_idx].weight' \
            "freestanding-profile/static-import: importer reads struct-array sibling scalar field directly"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" \
            'xray_const__freestanding_static_data_lib_GROUPS[_idx].inner.code' \
            "freestanding-profile/static-import: importer reads struct-array nested scalar field directly"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" \
            'xray_const__freestanding_static_data_lib_GROUPS[_idx].base' \
            "freestanding-profile/static-import: importer reads struct-array nested sibling field directly"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" \
            'xray_const__freestanding_static_data_lib_LABEL_GROUPS[_idx].inner.label' \
            "freestanding-profile/static-import: importer reads struct-array nested string field directly"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" \
            'xray_const__freestanding_static_data_lib_LABEL_GROUPS[_idx].inner.code' \
            "freestanding-profile/static-import: importer reads struct-array nested string sibling scalar directly"
        expect_log_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" \
            'xray_const__freestanding_static_data_lib_LABEL_GROUPS[_idx].base' \
            "freestanding-profile/static-import: importer reads struct-array nested string base field directly"
        expect_log_not_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" "xrt_getprop_name" \
            "freestanding-profile/static-import: avoids dynamic property helper"
        expect_log_not_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" "xr_array_ref" \
            "freestanding-profile/static-import: avoids runtime array helper"
        expect_log_not_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" "xr_aggregate_ref" \
            "freestanding-profile/static-import: avoids runtime aggregate helper"
        expect_log_not_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" "xrt_tuple_get" \
            "freestanding-profile/static-import: avoids runtime tuple helper"
        expect_log_not_contains "$FREESTANDING_STATIC_IMPORT_ENTRY_C" "xrt_index_get" \
            "freestanding-profile/static-import: avoids dynamic index helper"
    else
        record_fail "freestanding-profile/static-import: importer kept C source missing"
        sed 's/^/      /' "$FREESTANDING_STATIC_IMPORT_LOG" | sed -n '1,120p'
    fi
    FREESTANDING_STATIC_IMPORT_UNDEFINED="$(
        nm_undefined_normalized "$FREESTANDING_STATIC_IMPORT_OBJ")"
    FREESTANDING_STATIC_IMPORT_UNEXPECTED="$(
        printf '%s\n' "$FREESTANDING_STATIC_IMPORT_UNDEFINED" |
            sed '/^[[:space:]]*$/d' |
            grep -Ev '^(memcpy|memmove|memset|memcmp|xr_hook_panic|xr_hook_write)$' || true)"
    if [ -z "$FREESTANDING_STATIC_IMPORT_UNEXPECTED" ]; then
        record_pass "freestanding-profile/static-import: undefined symbols stay in hook/memcpy family"
    else
        record_fail "freestanding-profile/static-import: unexpected undefined symbols"
        printf '%s\n' "$FREESTANDING_STATIC_IMPORT_UNEXPECTED" | sed 's/^/      /'
    fi
    FREESTANDING_STATIC_IMPORT_NM="$WORK/freestanding_static_data_import.nm"
    if object_has_weak_symbol "$FREESTANDING_STATIC_IMPORT_OBJ" \
            "xray_const__freestanding_static_data_lib_MAGIC" \
            "$FREESTANDING_STATIC_IMPORT_NM"; then
        record_pass "freestanding-profile/static-import: weak scalar data symbol is external"
    else
        record_fail "freestanding-profile/static-import: weak scalar data symbol missing"
        sed 's/^/      /' "$FREESTANDING_STATIC_IMPORT_NM" | sed -n '1,80p'
    fi
    if object_has_weak_symbol "$FREESTANDING_STATIC_IMPORT_OBJ" \
            "xray_const__freestanding_static_data_lib_MATRIX" \
            "$FREESTANDING_STATIC_IMPORT_NM"; then
        record_pass "freestanding-profile/static-import: weak fixed-array matrix data symbol is external"
    else
        record_fail "freestanding-profile/static-import: weak fixed-array matrix data symbol missing"
        sed 's/^/      /' "$FREESTANDING_STATIC_IMPORT_NM" | sed -n '1,80p'
    fi
    if object_has_weak_symbol "$FREESTANDING_STATIC_IMPORT_OBJ" \
            "xray_const__freestanding_static_data_lib_LABEL_MATRIX" \
            "$FREESTANDING_STATIC_IMPORT_NM"; then
        record_pass "freestanding-profile/static-import: weak string fixed-array matrix data symbol is external"
    else
        record_fail "freestanding-profile/static-import: weak string fixed-array matrix data symbol missing"
        sed 's/^/      /' "$FREESTANDING_STATIC_IMPORT_NM" | sed -n '1,80p'
    fi
    if object_has_weak_symbol "$FREESTANDING_STATIC_IMPORT_OBJ" \
            "xray_const__freestanding_static_data_lib_LABEL_CUBE" \
            "$FREESTANDING_STATIC_IMPORT_NM"; then
        record_pass "freestanding-profile/static-import: weak string fixed-array cube data symbol is external"
    else
        record_fail "freestanding-profile/static-import: weak string fixed-array cube data symbol missing"
        sed 's/^/      /' "$FREESTANDING_STATIC_IMPORT_NM" | sed -n '1,80p'
    fi
    if object_has_weak_symbol "$FREESTANDING_STATIC_IMPORT_OBJ" \
            "xray_const__freestanding_static_data_lib_HEADER" \
            "$FREESTANDING_STATIC_IMPORT_NM"; then
        record_pass "freestanding-profile/static-import: weak struct data symbol is external"
    else
        record_fail "freestanding-profile/static-import: weak struct data symbol missing"
        sed 's/^/      /' "$FREESTANDING_STATIC_IMPORT_NM" | sed -n '1,80p'
    fi
    if object_has_weak_symbol "$FREESTANDING_STATIC_IMPORT_OBJ" \
            "xray_const__freestanding_static_data_lib_NESTED_LABEL" \
            "$FREESTANDING_STATIC_IMPORT_NM"; then
        record_pass "freestanding-profile/static-import: weak nested tuple string data symbol is external"
    else
        record_fail "freestanding-profile/static-import: weak nested tuple string data symbol missing"
        sed 's/^/      /' "$FREESTANDING_STATIC_IMPORT_NM" | sed -n '1,80p'
    fi
    if object_has_weak_symbol "$FREESTANDING_STATIC_IMPORT_OBJ" \
            "xray_const__freestanding_static_data_lib_ENTRIES" \
            "$FREESTANDING_STATIC_IMPORT_NM"; then
        record_pass "freestanding-profile/static-import: weak struct-array data symbol is external"
    else
        record_fail "freestanding-profile/static-import: weak struct-array data symbol missing"
        sed 's/^/      /' "$FREESTANDING_STATIC_IMPORT_NM" | sed -n '1,80p'
    fi
    if object_has_weak_symbol "$FREESTANDING_STATIC_IMPORT_OBJ" \
            "xray_const__freestanding_static_data_lib_LABEL_GROUPS" \
            "$FREESTANDING_STATIC_IMPORT_NM"; then
        record_pass "freestanding-profile/static-import: weak struct-array nested string data symbol is external"
    else
        record_fail "freestanding-profile/static-import: weak struct-array nested string data symbol missing"
        sed 's/^/      /' "$FREESTANDING_STATIC_IMPORT_NM" | sed -n '1,80p'
    fi
    FREESTANDING_STATIC_IMPORT_SECTIONS="$WORK/freestanding_static_data_import.sections"
    if otool -l "$FREESTANDING_STATIC_IMPORT_OBJ" >"$FREESTANDING_STATIC_IMPORT_SECTIONS" 2>/dev/null; then
        if grep -Fq "sectname .xr_imag" "$FREESTANDING_STATIC_IMPORT_SECTIONS" &&
           grep -Fq "sectname .xr_ihead" "$FREESTANDING_STATIC_IMPORT_SECTIONS" &&
           grep -Fq "sectname .xr_imatrix" "$FREESTANDING_STATIC_IMPORT_SECTIONS" &&
           grep -Fq "sectname .xr_ilmat" "$FREESTANDING_STATIC_IMPORT_SECTIONS" &&
           grep -Fq "sectname .xr_ilcube" "$FREESTANDING_STATIC_IMPORT_SECTIONS" &&
           grep -Fq "sectname .xr_ient" "$FREESTANDING_STATIC_IMPORT_SECTIONS" &&
           grep -Fq "sectname .xr_ilgrp" "$FREESTANDING_STATIC_IMPORT_SECTIONS"; then
            record_pass "freestanding-profile/static-import: object contains imported data sections"
        else
            record_fail "freestanding-profile/static-import: object missing imported data sections"
            sed 's/^/      /' "$FREESTANDING_STATIC_IMPORT_SECTIONS" | sed -n '1,120p'
        fi
    elif objdump -h "$FREESTANDING_STATIC_IMPORT_OBJ" >"$FREESTANDING_STATIC_IMPORT_SECTIONS" 2>/dev/null; then
        if grep -Fq ".xr_imag" "$FREESTANDING_STATIC_IMPORT_SECTIONS" &&
           grep -Fq ".xr_ihead" "$FREESTANDING_STATIC_IMPORT_SECTIONS" &&
           grep -Fq ".xr_imatrix" "$FREESTANDING_STATIC_IMPORT_SECTIONS" &&
           grep -Fq ".xr_ilmat" "$FREESTANDING_STATIC_IMPORT_SECTIONS" &&
           grep -Fq ".xr_ilcube" "$FREESTANDING_STATIC_IMPORT_SECTIONS" &&
           grep -Fq ".xr_ient" "$FREESTANDING_STATIC_IMPORT_SECTIONS"; then
            record_pass "freestanding-profile/static-import: object contains imported data sections"
        else
            record_fail "freestanding-profile/static-import: object missing imported data sections"
            sed 's/^/      /' "$FREESTANDING_STATIC_IMPORT_SECTIONS" | sed -n '1,120p'
        fi
    else
        record_fail "freestanding-profile/static-import: section dump failed"
    fi
else
    record_fail "freestanding-profile/static-import: object build failed"
    sed 's/^/      /' "$FREESTANDING_STATIC_IMPORT_LOG" | sed -n '1,120p'
fi

FREESTANDING_TOP_CONST_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_top_const_reject.xr"
FREESTANDING_TOP_CONST_LOG="$WORK/freestanding_top_const_reject.log"
if "$XRAY" build --native --profile freestanding --dry-run-link --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$WORK/freestanding_top_const_reject" \
        "$FREESTANDING_TOP_CONST_SRC" >"$FREESTANDING_TOP_CONST_LOG" 2>&1; then
    record_fail "freestanding-profile: rejects top-level const declarations"
    sed 's/^/      /' "$FREESTANDING_TOP_CONST_LOG" | sed -n '1,120p'
else
    expect_log_contains "$FREESTANDING_TOP_CONST_LOG" \
        "freestanding profile rejects top-level const declaration" \
        "freestanding-profile: rejects top-level const declarations"
fi

FREESTANDING_ARRAY_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_array_reject.xr"
FREESTANDING_ARRAY_LOG="$WORK/freestanding_array_reject.log"
if "$XRAY" build --native --profile freestanding --dry-run-link --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$WORK/freestanding_array_reject" \
        "$FREESTANDING_ARRAY_SRC" >"$FREESTANDING_ARRAY_LOG" 2>&1; then
    record_fail "freestanding-profile: rejects array literals"
    sed 's/^/      /' "$FREESTANDING_ARRAY_LOG" | sed -n '1,120p'
else
    expect_log_contains "$FREESTANDING_ARRAY_LOG" \
        "freestanding profile rejects Array literal" \
        "freestanding-profile: rejects array literals"
fi

FREESTANDING_STRING_MEMBER_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_string_member_reject.xr"
FREESTANDING_STRING_MEMBER_LOG="$WORK/freestanding_string_member_reject.log"
if "$XRAY" build --native --profile freestanding --dry-run-link --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$WORK/freestanding_string_member_reject" \
        "$FREESTANDING_STRING_MEMBER_SRC" >"$FREESTANDING_STRING_MEMBER_LOG" 2>&1; then
    record_fail "freestanding-profile: rejects hosted string member helpers"
    sed 's/^/      /' "$FREESTANDING_STRING_MEMBER_LOG" | sed -n '1,120p'
else
    expect_log_contains "$FREESTANDING_STRING_MEMBER_LOG" \
        "freestanding profile rejects string.toBytes" \
        "freestanding-profile: rejects hosted string-to-Bytes bridge"
    expect_log_contains "$FREESTANDING_STRING_MEMBER_LOG" \
        "freestanding profile rejects string.byteLength" \
        "freestanding-profile: rejects hosted string property helper"
    expect_log_contains "$FREESTANDING_STRING_MEMBER_LOG" \
        "freestanding profile rejects string index access" \
        "freestanding-profile: rejects hosted string index helper"
    expect_log_contains "$FREESTANDING_STRING_MEMBER_LOG" \
        "freestanding profile rejects string slice expression" \
        "freestanding-profile: rejects hosted string slice helper"
fi

FREESTANDING_BYTES_STATIC_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_bytes_static_reject.xr"
FREESTANDING_BYTES_STATIC_LOG="$WORK/freestanding_bytes_static_reject.log"
if "$XRAY" build --native --profile freestanding --dry-run-link --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$WORK/freestanding_bytes_static_reject" \
        "$FREESTANDING_BYTES_STATIC_SRC" >"$FREESTANDING_BYTES_STATIC_LOG" 2>&1; then
    record_fail "freestanding-profile: rejects owned Bytes static constructors"
    sed 's/^/      /' "$FREESTANDING_BYTES_STATIC_LOG" | sed -n '1,120p'
else
    expect_log_contains "$FREESTANDING_BYTES_STATIC_LOG" \
        "freestanding profile rejects Bytes.withCapacity" \
        "freestanding-profile: rejects Bytes.withCapacity"
    expect_log_contains "$FREESTANDING_BYTES_STATIC_LOG" \
        "freestanding profile rejects Bytes.fromString" \
        "freestanding-profile: rejects Bytes.fromString"
fi

FREESTANDING_BUILTIN_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_builtin_reject.xr"
FREESTANDING_BUILTIN_LOG="$WORK/freestanding_builtin_reject.log"
if "$XRAY" build --native --profile freestanding --dry-run-link --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$WORK/freestanding_builtin_reject" \
        "$FREESTANDING_BUILTIN_SRC" >"$FREESTANDING_BUILTIN_LOG" 2>&1; then
    record_fail "freestanding-profile: rejects hosted builtin helpers"
    sed 's/^/      /' "$FREESTANDING_BUILTIN_LOG" | sed -n '1,120p'
else
    expect_log_contains "$FREESTANDING_BUILTIN_LOG" \
        "freestanding profile rejects builtin string()" \
        "freestanding-profile: rejects builtin string conversion"
    expect_log_contains "$FREESTANDING_BUILTIN_LOG" \
        "freestanding profile rejects builtin typename()" \
        "freestanding-profile: rejects builtin typename"
    expect_log_contains "$FREESTANDING_BUILTIN_LOG" \
        "freestanding profile rejects builtin copy()" \
        "freestanding-profile: rejects builtin copy"
    expect_log_contains "$FREESTANDING_BUILTIN_LOG" \
        "freestanding profile rejects builtin char()" \
        "freestanding-profile: rejects builtin char conversion"
    expect_log_contains "$FREESTANDING_BUILTIN_LOG" \
        "freestanding profile rejects builtin chr()" \
        "freestanding-profile: rejects builtin chr conversion"
    expect_log_contains "$FREESTANDING_BUILTIN_LOG" \
        "freestanding profile rejects builtin dump()" \
        "freestanding-profile: rejects builtin dump"
fi

FREESTANDING_TAGGED_TYPE_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_tagged_type_reject.xr"
FREESTANDING_TAGGED_TYPE_LOG="$WORK/freestanding_tagged_type_reject.log"
if "$XRAY" build --native --profile freestanding --dry-run-link --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$WORK/freestanding_tagged_type_reject" \
        "$FREESTANDING_TAGGED_TYPE_SRC" >"$FREESTANDING_TAGGED_TYPE_LOG" 2>&1; then
    record_fail "freestanding-profile: rejects tagged/dynamic value types"
    sed 's/^/      /' "$FREESTANDING_TAGGED_TYPE_LOG" | sed -n '1,120p'
else
    expect_log_contains "$FREESTANDING_TAGGED_TYPE_LOG" \
        "freestanding profile rejects tagged/dynamic value type in function parameter 'x'" \
        "freestanding-profile: rejects Json function parameters"
    expect_log_contains "$FREESTANDING_TAGGED_TYPE_LOG" \
        "freestanding profile rejects tagged/dynamic value type in function return type" \
        "freestanding-profile: rejects Json function returns"
    expect_log_contains "$FREESTANDING_TAGGED_TYPE_LOG" \
        "freestanding profile rejects tagged/dynamic value type in variable 'x'" \
        "freestanding-profile: rejects Json locals"
fi

FREESTANDING_ENUM_STATIC_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_enum_static.xr"
FREESTANDING_ENUM_STATIC_OBJ="$WORK/freestanding_enum_static.o"
FREESTANDING_ENUM_STATIC_LOG="$WORK/freestanding_enum_static.log"
if "$XRAY" build --native --profile freestanding --shared --keep-c --rebuild \
        --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_ENUM_STATIC_OBJ" \
        "$FREESTANDING_ENUM_STATIC_SRC" >"$FREESTANDING_ENUM_STATIC_LOG" 2>&1; then
    FREESTANDING_ENUM_STATIC_C="$(sed -n 's/^Kept C source: //p' \
        "$FREESTANDING_ENUM_STATIC_LOG" | tail -n 1)"
    if [ -f "$FREESTANDING_ENUM_STATIC_C" ]; then
        expect_log_contains "$FREESTANDING_ENUM_STATIC_C" "int64_t v3 = INT64_C(0)" \
            "freestanding-profile/enum-static: lowers Boot to ordinal"
        expect_log_contains "$FREESTANDING_ENUM_STATIC_C" "uint8_t v4 = v0 == v3" \
            "freestanding-profile/enum-static: match uses ordinal compare"
        expect_log_contains "$FREESTANDING_ENUM_STATIC_C" "int64_t v7 = INT64_C(1)" \
            "freestanding-profile/enum-static: lowers Run to ordinal"
        expect_log_contains "$FREESTANDING_ENUM_STATIC_C" "int64_t v11 = INT64_C(2)" \
            "freestanding-profile/enum-static: lowers Halt to ordinal"
        expect_log_not_contains "$FREESTANDING_ENUM_STATIC_C" "_xenum_" \
            "freestanding-profile/enum-static: avoids static enum boxes"
        expect_log_not_contains "$FREESTANDING_ENUM_STATIC_C" "XR_TAG_ENUM" \
            "freestanding-profile/enum-static: avoids tagged enum values"
        expect_log_not_contains "$FREESTANDING_ENUM_STATIC_C" "xrt_enum_box_ordinal" \
            "freestanding-profile/enum-static: avoids boxed enum ordinal helper"
        expect_log_not_contains "$FREESTANDING_ENUM_STATIC_C" "xrt_eq(" \
            "freestanding-profile/enum-static: equality uses ordinal compare"
        expect_log_not_contains "$FREESTANDING_ENUM_STATIC_C" "xrt_map_new" \
            "freestanding-profile/enum-static: avoids enum namespace map allocation"
        expect_log_not_contains "$FREESTANDING_ENUM_STATIC_C" "xrt_map_set" \
            "freestanding-profile/enum-static: avoids enum namespace map initialization"
        expect_log_not_contains "$FREESTANDING_ENUM_STATIC_C" "xrt_map_get_owned" \
            "freestanding-profile/enum-static: avoids enum namespace map lookup"
        expect_log_not_contains "$FREESTANDING_ENUM_STATIC_C" "xrt_shared[" \
            "freestanding-profile/enum-static: avoids shared-slot enum namespace state"
        expect_log_not_contains "$FREESTANDING_ENUM_STATIC_C" "xrt_enum_box_new" \
            "freestanding-profile/enum-static: avoids heap enum box construction"
    else
        record_fail "freestanding-profile/enum-static: kept C source missing"
        sed 's/^/      /' "$FREESTANDING_ENUM_STATIC_LOG" | sed -n '1,120p'
    fi
    FREESTANDING_ENUM_STATIC_UNDEFINED="$(
        nm_undefined_normalized "$FREESTANDING_ENUM_STATIC_OBJ")"
    FREESTANDING_ENUM_STATIC_UNEXPECTED="$(
        printf '%s\n' "$FREESTANDING_ENUM_STATIC_UNDEFINED" |
            sed '/^[[:space:]]*$/d' |
            grep -Ev '^(memcpy|memmove|memset|memcmp|xr_hook_panic|xr_hook_write)$' || true)"
    if [ -z "$FREESTANDING_ENUM_STATIC_UNEXPECTED" ]; then
        record_pass "freestanding-profile/enum-static: undefined symbols stay in hook/memcpy family"
    else
        record_fail "freestanding-profile/enum-static: unexpected undefined symbols"
        printf '%s\n' "$FREESTANDING_ENUM_STATIC_UNEXPECTED" | sed 's/^/      /'
    fi
else
    record_fail "freestanding-profile/enum-static: object build failed"
    sed 's/^/      /' "$FREESTANDING_ENUM_STATIC_LOG" | sed -n '1,120p'
fi

FREESTANDING_ENUM_ERR_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_enum_error_channel.xr"
FREESTANDING_ENUM_ERR_OBJ="$WORK/freestanding_enum_error_channel.o"
FREESTANDING_ENUM_ERR_LOG="$WORK/freestanding_enum_error_channel.log"
if "$XRAY" build --native --profile freestanding --shared --keep-c --rebuild \
        --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_ENUM_ERR_OBJ" \
        "$FREESTANDING_ENUM_ERR_SRC" >"$FREESTANDING_ENUM_ERR_LOG" 2>&1; then
    FREESTANDING_ENUM_ERR_C="$(sed -n 's/^Kept C source: //p' \
        "$FREESTANDING_ENUM_ERR_LOG" | tail -n 1)"
    if [ -f "$FREESTANDING_ENUM_ERR_C" ]; then
        expect_log_contains "$FREESTANDING_ENUM_ERR_C" "xrt_pending_error =" \
            "freestanding-profile/enum-error: writes pending enum value"
        expect_log_contains "$FREESTANDING_ENUM_ERR_C" "xrt_has_pending_error()" \
            "freestanding-profile/enum-error: checks pending enum value"
        expect_log_contains "$FREESTANDING_ENUM_ERR_C" "xrt_pending_error = XR_NULL_VAL" \
            "freestanding-profile/enum-error: catch clears pending enum value"
        expect_log_contains "$FREESTANDING_ENUM_ERR_C" "xrt_pending_error = XR_FROM_INT(" \
            "freestanding-profile/enum-error: stores ordinal in tagged pending slot"
        expect_log_contains "$FREESTANDING_ENUM_ERR_C" "uint8_t v10 = xrt_eq(v7, XR_FROM_INT(v9))" \
            "freestanding-profile/enum-error: untyped catch compares pending ordinal"
        expect_log_not_contains "$FREESTANDING_ENUM_ERR_C" "_xenum_" \
            "freestanding-profile/enum-error: avoids static enum boxes"
        expect_log_not_contains "$FREESTANDING_ENUM_ERR_C" "XR_TAG_ENUM" \
            "freestanding-profile/enum-error: avoids tagged enum boxes"
        expect_log_not_contains "$FREESTANDING_ENUM_ERR_C" "xrt_enum_box_ordinal" \
            "freestanding-profile/enum-error: avoids boxed enum ordinal helper"
        expect_log_not_contains "$FREESTANDING_ENUM_ERR_C" "xrt_throw_exc" \
            "freestanding-profile/enum-error: avoids hosted throw helper"
        expect_log_not_contains "$FREESTANDING_ENUM_ERR_C" "setjmp" \
            "freestanding-profile/enum-error: avoids hosted unwind setup"
        expect_log_not_contains "$FREESTANDING_ENUM_ERR_C" "longjmp" \
            "freestanding-profile/enum-error: avoids hosted unwind transfer"
        expect_log_not_contains "$FREESTANDING_ENUM_ERR_C" "xrt_map_new" \
            "freestanding-profile/enum-error: avoids enum namespace map allocation"
        expect_log_not_contains "$FREESTANDING_ENUM_ERR_C" "xrt_map_set" \
            "freestanding-profile/enum-error: avoids enum namespace map initialization"
        expect_log_not_contains "$FREESTANDING_ENUM_ERR_C" "xrt_shared[" \
            "freestanding-profile/enum-error: avoids shared-slot enum namespace state"
        expect_log_not_contains "$FREESTANDING_ENUM_ERR_C" "xrt_enum_box_new" \
            "freestanding-profile/enum-error: avoids heap enum box construction"
    else
        record_fail "freestanding-profile/enum-error: kept C source missing"
        sed 's/^/      /' "$FREESTANDING_ENUM_ERR_LOG" | sed -n '1,120p'
    fi
    FREESTANDING_ENUM_ERR_UNDEFINED="$(
        nm_undefined_normalized "$FREESTANDING_ENUM_ERR_OBJ")"
    FREESTANDING_ENUM_ERR_UNEXPECTED="$(
        printf '%s\n' "$FREESTANDING_ENUM_ERR_UNDEFINED" |
            sed '/^[[:space:]]*$/d' |
            grep -Ev '^(memcpy|memmove|memset|memcmp|xr_hook_panic|xr_hook_write)$' || true)"
    if [ -z "$FREESTANDING_ENUM_ERR_UNEXPECTED" ]; then
        record_pass "freestanding-profile/enum-error: undefined symbols stay in hook/memcpy family"
    else
        record_fail "freestanding-profile/enum-error: unexpected undefined symbols"
        printf '%s\n' "$FREESTANDING_ENUM_ERR_UNEXPECTED" | sed 's/^/      /'
    fi
else
    record_fail "freestanding-profile/enum-error: object build failed"
    sed 's/^/      /' "$FREESTANDING_ENUM_ERR_LOG" | sed -n '1,120p'
fi

FREESTANDING_ENUM_PAYLOAD_VALUE_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_enum_payload_value.xr"
FREESTANDING_ENUM_PAYLOAD_VALUE_OBJ="$WORK/freestanding_enum_payload_value.o"
FREESTANDING_ENUM_PAYLOAD_VALUE_LOG="$WORK/freestanding_enum_payload_value.log"
if "$XRAY" build --native --profile freestanding --shared --keep-c --rebuild \
        --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_ENUM_PAYLOAD_VALUE_OBJ" \
        "$FREESTANDING_ENUM_PAYLOAD_VALUE_SRC" >"$FREESTANDING_ENUM_PAYLOAD_VALUE_LOG" 2>&1; then
    FREESTANDING_ENUM_PAYLOAD_VALUE_C="$(sed -n 's/^Kept C source: //p' \
        "$FREESTANDING_ENUM_PAYLOAD_VALUE_LOG" | tail -n 1)"
    if [ -f "$FREESTANDING_ENUM_PAYLOAD_VALUE_C" ]; then
        expect_log_contains "$FREESTANDING_ENUM_PAYLOAD_VALUE_C" "typedef struct xrt_enum_" \
            "freestanding-profile/enum-payload: emits typed enum aggregate"
        expect_log_contains "$FREESTANDING_ENUM_PAYLOAD_VALUE_C" "xrt_enum_aggregate_make(" \
            "freestanding-profile/enum-payload: constructs payload value without heap"
        expect_log_contains "$FREESTANDING_ENUM_PAYLOAD_VALUE_C" \
            "xrt_enum_aggregate_check_payload_type(" \
            "freestanding-profile/enum-payload: keeps payload type checks local"
        expect_log_not_contains "$FREESTANDING_ENUM_PAYLOAD_VALUE_C" "xrt_enum_aggregate_box" \
            "freestanding-profile/enum-payload: avoids tagged payload boxing"
        expect_log_not_contains "$FREESTANDING_ENUM_PAYLOAD_VALUE_C" "xrt_array_with_capacity" \
            "freestanding-profile/enum-payload: avoids payload array allocation"
        expect_log_not_contains "$FREESTANDING_ENUM_PAYLOAD_VALUE_C" "xrt_map_new" \
            "freestanding-profile/enum-payload: avoids enum namespace map allocation"
        expect_log_not_contains "$FREESTANDING_ENUM_PAYLOAD_VALUE_C" "xrt_shared[" \
            "freestanding-profile/enum-payload: avoids shared-slot enum namespace state"
        expect_log_not_contains "$FREESTANDING_ENUM_PAYLOAD_VALUE_C" "xrt_enum_box_new" \
            "freestanding-profile/enum-payload: avoids heap enum box construction"
        expect_log_not_contains "$FREESTANDING_ENUM_PAYLOAD_VALUE_C" "xrt_throw_exc" \
            "freestanding-profile/enum-payload: avoids hosted throw helper"
    else
        record_fail "freestanding-profile/enum-payload: kept C source missing"
        sed 's/^/      /' "$FREESTANDING_ENUM_PAYLOAD_VALUE_LOG" | sed -n '1,120p'
    fi
    FREESTANDING_ENUM_PAYLOAD_VALUE_UNDEFINED="$(
        nm_undefined_normalized "$FREESTANDING_ENUM_PAYLOAD_VALUE_OBJ")"
    FREESTANDING_ENUM_PAYLOAD_VALUE_UNEXPECTED="$(
        printf '%s\n' "$FREESTANDING_ENUM_PAYLOAD_VALUE_UNDEFINED" |
            sed '/^[[:space:]]*$/d' |
            grep -Ev '^(memcpy|memmove|memset|memcmp|xr_hook_panic|xr_hook_write)$' || true)"
    if [ -z "$FREESTANDING_ENUM_PAYLOAD_VALUE_UNEXPECTED" ]; then
        record_pass "freestanding-profile/enum-payload: undefined symbols stay in hook/memcpy family"
    else
        record_fail "freestanding-profile/enum-payload: unexpected undefined symbols"
        printf '%s\n' "$FREESTANDING_ENUM_PAYLOAD_VALUE_UNEXPECTED" | sed 's/^/      /'
    fi
else
    record_fail "freestanding-profile/enum-payload: object build failed"
    sed 's/^/      /' "$FREESTANDING_ENUM_PAYLOAD_VALUE_LOG" | sed -n '1,120p'
fi

FREESTANDING_ENUM_PAYLOAD_ERR_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_enum_payload_error_channel.xr"
FREESTANDING_ENUM_PAYLOAD_ERR_OBJ="$WORK/freestanding_enum_payload_error_channel.o"
FREESTANDING_ENUM_PAYLOAD_ERR_LOG="$WORK/freestanding_enum_payload_error_channel.log"
if "$XRAY" build --native --profile freestanding --shared --keep-c --rebuild \
        --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_ENUM_PAYLOAD_ERR_OBJ" \
        "$FREESTANDING_ENUM_PAYLOAD_ERR_SRC" >"$FREESTANDING_ENUM_PAYLOAD_ERR_LOG" 2>&1; then
    FREESTANDING_ENUM_PAYLOAD_ERR_C="$(sed -n 's/^Kept C source: //p' \
        "$FREESTANDING_ENUM_PAYLOAD_ERR_LOG" | tail -n 1)"
    if [ -f "$FREESTANDING_ENUM_PAYLOAD_ERR_C" ]; then
        expect_log_contains "$FREESTANDING_ENUM_PAYLOAD_ERR_C" "xrt_pending_enum_error =" \
            "freestanding-profile/enum-payload-error: writes typed pending payload"
        expect_log_contains "$FREESTANDING_ENUM_PAYLOAD_ERR_C" "xrt_pending_enum_error_active = 1" \
            "freestanding-profile/enum-payload-error: marks typed payload active"
        expect_log_contains "$FREESTANDING_ENUM_PAYLOAD_ERR_C" "_from_base(xrt_pending_enum_error)" \
            "freestanding-profile/enum-payload-error: catch reads typed pending payload"
        expect_log_contains "$FREESTANDING_ENUM_PAYLOAD_ERR_C" ".payload.raw[0]" \
            "freestanding-profile/enum-payload-error: catch can read payload lane"
        expect_log_not_contains "$FREESTANDING_ENUM_PAYLOAD_ERR_C" "xrt_enum_aggregate_box" \
            "freestanding-profile/enum-payload-error: avoids tagged payload boxing"
        expect_log_not_contains "$FREESTANDING_ENUM_PAYLOAD_ERR_C" "xrt_array_with_capacity" \
            "freestanding-profile/enum-payload-error: avoids payload array allocation"
        expect_log_not_contains "$FREESTANDING_ENUM_PAYLOAD_ERR_C" "xrt_throw_exc" \
            "freestanding-profile/enum-payload-error: avoids hosted throw helper"
        expect_log_not_contains "$FREESTANDING_ENUM_PAYLOAD_ERR_C" "setjmp" \
            "freestanding-profile/enum-payload-error: avoids hosted unwind setup"
        expect_log_not_contains "$FREESTANDING_ENUM_PAYLOAD_ERR_C" "longjmp" \
            "freestanding-profile/enum-payload-error: avoids hosted unwind transfer"
    else
        record_fail "freestanding-profile/enum-payload-error: kept C source missing"
        sed 's/^/      /' "$FREESTANDING_ENUM_PAYLOAD_ERR_LOG" | sed -n '1,120p'
    fi
    FREESTANDING_ENUM_PAYLOAD_ERR_UNDEFINED="$(
        nm_undefined_normalized "$FREESTANDING_ENUM_PAYLOAD_ERR_OBJ")"
    FREESTANDING_ENUM_PAYLOAD_ERR_UNEXPECTED="$(
        printf '%s\n' "$FREESTANDING_ENUM_PAYLOAD_ERR_UNDEFINED" |
            sed '/^[[:space:]]*$/d' |
            grep -Ev '^(memcpy|memmove|memset|memcmp|xr_hook_panic|xr_hook_write)$' || true)"
    if [ -z "$FREESTANDING_ENUM_PAYLOAD_ERR_UNEXPECTED" ]; then
        record_pass "freestanding-profile/enum-payload-error: undefined symbols stay in hook/memcpy family"
    else
        record_fail "freestanding-profile/enum-payload-error: unexpected undefined symbols"
        printf '%s\n' "$FREESTANDING_ENUM_PAYLOAD_ERR_UNEXPECTED" | sed 's/^/      /'
    fi
else
    record_fail "freestanding-profile/enum-payload-error: object build failed"
    sed 's/^/      /' "$FREESTANDING_ENUM_PAYLOAD_ERR_LOG" | sed -n '1,120p'
fi

FREESTANDING_FORCE_UNWRAP_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_force_unwrap_reject.xr"
FREESTANDING_FORCE_UNWRAP_LOG="$WORK/freestanding_force_unwrap_reject.log"
if "$XRAY" build --native --profile freestanding --dry-run-link --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$WORK/freestanding_force_unwrap_reject" \
        "$FREESTANDING_FORCE_UNWRAP_SRC" >"$FREESTANDING_FORCE_UNWRAP_LOG" 2>&1; then
    record_fail "freestanding-profile: rejects force unwrap value-error channel"
    sed 's/^/      /' "$FREESTANDING_FORCE_UNWRAP_LOG" | sed -n '1,120p'
else
    expect_log_contains "$FREESTANDING_FORCE_UNWRAP_LOG" \
        "freestanding profile rejects force unwrap" \
        "freestanding-profile: rejects force unwrap value-error channel"
fi

FREESTANDING_MATCH_PANIC_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_match_panic_hook.xr"
FREESTANDING_MATCH_PANIC_OBJ="$WORK/freestanding_match_panic_hook.o"
FREESTANDING_MATCH_PANIC_LOG="$WORK/freestanding_match_panic_hook.log"
if "$XRAY" build --native --profile freestanding --shared --keep-c --rebuild \
        --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_MATCH_PANIC_OBJ" \
        "$FREESTANDING_MATCH_PANIC_SRC" >"$FREESTANDING_MATCH_PANIC_LOG" 2>&1; then
    FREESTANDING_MATCH_PANIC_KEPT_C="$(sed -n 's/^Kept C source: //p' \
        "$FREESTANDING_MATCH_PANIC_LOG" | tail -n 1)"
    if [ -f "$FREESTANDING_MATCH_PANIC_KEPT_C" ]; then
        expect_log_contains "$FREESTANDING_MATCH_PANIC_KEPT_C" "xrt_freestanding_trap" \
            "freestanding-profile/match: generated C uses panic hook trap"
        expect_log_not_contains "$FREESTANDING_MATCH_PANIC_KEPT_C" "xrt_throw_exc" \
            "freestanding-profile/match: generated C avoids hosted throw helper"
        expect_log_not_contains "$FREESTANDING_MATCH_PANIC_KEPT_C" \
            "xrt_exception_from_message_value" \
            "freestanding-profile/match: generated C avoids hosted exception constructor"
    else
        record_fail "freestanding-profile/match: kept C source missing"
        sed 's/^/      /' "$FREESTANDING_MATCH_PANIC_LOG" | sed -n '1,120p'
    fi
    FREESTANDING_MATCH_PANIC_UNDEFINED="$(nm_undefined_normalized "$FREESTANDING_MATCH_PANIC_OBJ")"
    if [ "$FREESTANDING_MATCH_PANIC_UNDEFINED" = "xr_hook_panic" ]; then
        record_pass "freestanding-profile/match: panic path depends only on panic hook"
    else
        record_fail "freestanding-profile/match: unexpected undefined symbols"
        nm -u "$FREESTANDING_MATCH_PANIC_OBJ" 2>&1 | sed '/^[[:space:]]*$/d' | sed 's/^/      /'
    fi
else
    record_fail "freestanding-profile/match: object build failed"
    sed 's/^/      /' "$FREESTANDING_MATCH_PANIC_LOG" | sed -n '1,120p'
fi

FREESTANDING_HOOK_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_panic_hook.xr"
FREESTANDING_HOOK_OBJ="$WORK/freestanding_panic_hook.o"
FREESTANDING_HOOK_REAL_LOG="$WORK/freestanding_panic_hook.log"
if "$XRAY" build --native --profile freestanding --shared --keep-c --rebuild \
        --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_HOOK_OBJ" \
        "$FREESTANDING_HOOK_SRC" >"$FREESTANDING_HOOK_REAL_LOG" 2>&1; then
    FREESTANDING_HOOK_UNDEFINED="$(nm_undefined_normalized "$FREESTANDING_HOOK_OBJ")"
    if [ "$FREESTANDING_HOOK_UNDEFINED" = "xr_hook_panic" ]; then
        record_pass "freestanding-profile: panic path depends only on panic hook"
    else
        record_fail "freestanding-profile: panic path has unexpected undefined symbols"
        nm -u "$FREESTANDING_HOOK_OBJ" 2>&1 | sed '/^[[:space:]]*$/d' | sed 's/^/      /'
    fi
else
    record_fail "freestanding-profile: panic hook object build failed"
    sed 's/^/      /' "$FREESTANDING_HOOK_REAL_LOG" | sed -n '1,120p'
fi

FREESTANDING_ASSERT_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_assert_hook.xr"
FREESTANDING_ASSERT_OBJ="$WORK/freestanding_assert_hook.o"
FREESTANDING_ASSERT_REAL_LOG="$WORK/freestanding_assert_hook.log"
if "$XRAY" build --native --profile freestanding --shared --keep-c --rebuild \
        --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_ASSERT_OBJ" \
        "$FREESTANDING_ASSERT_SRC" >"$FREESTANDING_ASSERT_REAL_LOG" 2>&1; then
    FREESTANDING_ASSERT_KEPT_C="$(sed -n 's/^Kept C source: //p' "$FREESTANDING_ASSERT_REAL_LOG" | tail -n 1)"
    if [ -f "$FREESTANDING_ASSERT_KEPT_C" ]; then
        expect_log_contains "$FREESTANDING_ASSERT_KEPT_C" "xrt_freestanding_trap(\"Assertion failed" \
            "freestanding-profile/assert: generated C uses panic hook trap"
        expect_log_not_contains "$FREESTANDING_ASSERT_KEPT_C" "fprintf(stderr" \
            "freestanding-profile/assert: generated C avoids fprintf"
        expect_log_not_contains "$FREESTANDING_ASSERT_KEPT_C" "abort()" \
            "freestanding-profile/assert: generated C avoids abort"
    else
        record_fail "freestanding-profile/assert: kept C source missing"
        sed 's/^/      /' "$FREESTANDING_ASSERT_REAL_LOG" | sed -n '1,120p'
    fi
    FREESTANDING_ASSERT_UNDEFINED="$(nm_undefined_normalized "$FREESTANDING_ASSERT_OBJ")"
    if [ "$FREESTANDING_ASSERT_UNDEFINED" = "xr_hook_panic" ]; then
        record_pass "freestanding-profile: assert path depends only on panic hook"
    else
        record_fail "freestanding-profile: assert path has unexpected undefined symbols"
        nm -u "$FREESTANDING_ASSERT_OBJ" 2>&1 | sed '/^[[:space:]]*$/d' | sed 's/^/      /'
    fi
else
    record_fail "freestanding-profile: assert hook object build failed"
    sed 's/^/      /' "$FREESTANDING_ASSERT_REAL_LOG" | sed -n '1,120p'
fi

FREESTANDING_WRITE_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_write_hook.xr"
FREESTANDING_WRITE_OBJ="$WORK/freestanding_write_hook.o"
FREESTANDING_WRITE_REAL_LOG="$WORK/freestanding_write_hook.log"
if "$XRAY" build --native --profile freestanding --shared --keep-c --rebuild \
        --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_WRITE_OBJ" \
        "$FREESTANDING_WRITE_SRC" >"$FREESTANDING_WRITE_REAL_LOG" 2>&1; then
    FREESTANDING_WRITE_KEPT_C="$(sed -n 's/^Kept C source: //p' "$FREESTANDING_WRITE_REAL_LOG" | tail -n 1)"
    if [ -f "$FREESTANDING_WRITE_KEPT_C" ]; then
        expect_log_not_contains "$FREESTANDING_WRITE_KEPT_C" "printf(" \
            "freestanding-profile/write hook: generated C avoids printf"
        expect_log_not_contains "$FREESTANDING_WRITE_KEPT_C" "putchar(" \
            "freestanding-profile/write hook: generated C avoids putchar"
    else
        record_fail "freestanding-profile/write hook: kept C source missing"
        sed 's/^/      /' "$FREESTANDING_WRITE_REAL_LOG" | sed -n '1,120p'
    fi
    FREESTANDING_WRITE_UNDEFINED="$(nm_undefined_normalized "$FREESTANDING_WRITE_OBJ")"
    if [ "$FREESTANDING_WRITE_UNDEFINED" = "xr_hook_write" ]; then
        record_pass "freestanding-profile: print path depends only on write hook"
    else
        record_fail "freestanding-profile: print path has unexpected undefined symbols"
        nm -u "$FREESTANDING_WRITE_OBJ" 2>&1 | sed '/^[[:space:]]*$/d' | sed 's/^/      /'
    fi
else
    record_fail "freestanding-profile: write hook object build failed"
    sed 's/^/      /' "$FREESTANDING_WRITE_REAL_LOG" | sed -n '1,120p'
fi

FREESTANDING_MEM_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_mem_core.xr"
FREESTANDING_MEM_OBJ="$WORK/freestanding_mem_core.o"
FREESTANDING_MEM_REAL_LOG="$WORK/freestanding_mem_core.log"
if "$XRAY" build --native --profile freestanding --shared --keep-c --rebuild \
        --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_MEM_OBJ" \
        "$FREESTANDING_MEM_SRC" >"$FREESTANDING_MEM_REAL_LOG" 2>&1; then
    FREESTANDING_MEM_KEPT_C="$(sed -n 's/^Kept C source: //p' "$FREESTANDING_MEM_REAL_LOG" | tail -n 1)"
    if [ -f "$FREESTANDING_MEM_KEPT_C" ]; then
        expect_log_contains "$FREESTANDING_MEM_KEPT_C" "#include \"xrt_core_freestanding.h\"" \
            "freestanding-profile/mem: generated C uses freestanding prelude"
        expect_log_not_contains "$FREESTANDING_MEM_KEPT_C" "#include \"xrt_mem.h\"" \
            "freestanding-profile/mem: generated C avoids hosted mem helper"
    else
        record_fail "freestanding-profile/mem: kept C source missing"
        sed 's/^/      /' "$FREESTANDING_MEM_REAL_LOG" | sed -n '1,120p'
    fi
    FREESTANDING_MEM_UNDEFINED="$(nm_undefined_normalized "$FREESTANDING_MEM_OBJ")"
    FREESTANDING_MEM_UNEXPECTED="$(printf '%s\n' "$FREESTANDING_MEM_UNDEFINED" |
        sed '/^[[:space:]]*$/d' |
        grep -Ev '^(memcpy|memmove|memset|memcmp)$' || true)"
    if [ -z "$FREESTANDING_MEM_UNEXPECTED" ]; then
        record_pass "freestanding-profile/mem: undefined symbols stay in memcpy family"
    else
        record_fail "freestanding-profile/mem: unexpected undefined symbols"
        printf '%s\n' "$FREESTANDING_MEM_UNEXPECTED" | sed 's/^/      /'
    fi
else
    record_fail "freestanding-profile/mem: core object build failed"
    sed 's/^/      /' "$FREESTANDING_MEM_REAL_LOG" | sed -n '1,120p'
fi

FREESTANDING_REG_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_register_abstraction.xr"
FREESTANDING_REG_OBJ="$WORK/freestanding_register_abstraction.o"
FREESTANDING_REG_REAL_LOG="$WORK/freestanding_register_abstraction.log"
if "$XRAY" build --native --profile freestanding --shared --keep-c --rebuild \
        --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_REG_OBJ" \
        "$FREESTANDING_REG_SRC" >"$FREESTANDING_REG_REAL_LOG" 2>&1; then
    FREESTANDING_REG_KEPT_C="$(sed -n 's/^Kept C source: //p' "$FREESTANDING_REG_REAL_LOG" | tail -n 1)"
    if [ -f "$FREESTANDING_REG_KEPT_C" ]; then
        expect_log_contains "$FREESTANDING_REG_KEPT_C" "xrt_mem_volatile_load" \
            "freestanding-profile/register: sample uses volatile load"
        expect_log_contains "$FREESTANDING_REG_KEPT_C" "xrt_mem_volatile_store" \
            "freestanding-profile/register: sample uses volatile store"
        expect_log_not_contains "$FREESTANDING_REG_KEPT_C" "xrt_shared[" \
            "freestanding-profile/register: sample avoids shared storage"
        expect_log_not_contains "$FREESTANDING_REG_KEPT_C" "xrt_arc_alloc" \
            "freestanding-profile/register: sample avoids heap allocation"
    else
        record_fail "freestanding-profile/register: kept C source missing"
        sed 's/^/      /' "$FREESTANDING_REG_REAL_LOG" | sed -n '1,120p'
    fi
    FREESTANDING_REG_UNDEFINED="$(nm_undefined_normalized "$FREESTANDING_REG_OBJ")"
    if [ -z "$FREESTANDING_REG_UNDEFINED" ]; then
        record_pass "freestanding-profile/register: no undefined symbols"
    else
        record_fail "freestanding-profile/register: unexpected undefined symbols"
        printf '%s\n' "$FREESTANDING_REG_UNDEFINED" | sed 's/^/      /'
    fi
else
    record_fail "freestanding-profile/register: object build failed"
    sed 's/^/      /' "$FREESTANDING_REG_REAL_LOG" | sed -n '1,120p'
fi

FREESTANDING_MEM_ALLOC_HOOK_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_mem_alloc_hook.xr"
FREESTANDING_MEM_ALLOC_HOOK_OBJ="$WORK/freestanding_mem_alloc_hook.o"
FREESTANDING_MEM_ALLOC_HOOK_REAL_LOG="$WORK/freestanding_mem_alloc_hook.log"
if "$XRAY" build --native --profile freestanding --shared --keep-c --rebuild \
        --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_MEM_ALLOC_HOOK_OBJ" \
        "$FREESTANDING_MEM_ALLOC_HOOK_SRC" >"$FREESTANDING_MEM_ALLOC_HOOK_REAL_LOG" 2>&1; then
    FREESTANDING_MEM_ALLOC_HOOK_C="$(sed -n 's/^Kept C source: //p' "$FREESTANDING_MEM_ALLOC_HOOK_REAL_LOG" | tail -n 1)"
    if [ -f "$FREESTANDING_MEM_ALLOC_HOOK_C" ]; then
        expect_log_contains "$FREESTANDING_MEM_ALLOC_HOOK_C" "xrt_mem_alloc(" \
            "freestanding-profile/mem: Buffer alloc lowers through hook-backed helper"
        expect_log_contains "$FREESTANDING_MEM_ALLOC_HOOK_C" "xrt_mem_alloc_zeroed(" \
            "freestanding-profile/mem: Buffer zeroed alloc lowers through hook-backed helper"
        expect_log_contains "$FREESTANDING_MEM_ALLOC_HOOK_C" "xrt_mem_alloc_aligned(" \
            "freestanding-profile/mem: Buffer aligned alloc lowers through hook-backed helper"
        expect_log_contains "$FREESTANDING_MEM_ALLOC_HOOK_C" "xrt_release(" \
            "freestanding-profile/mem: generated code releases Buffer values"
        expect_log_not_contains "$FREESTANDING_MEM_ALLOC_HOOK_C" "xrt_arc_alloc" \
            "freestanding-profile/mem: Buffer alloc avoids hosted ARC allocator"
        expect_log_not_contains "$FREESTANDING_MEM_ALLOC_HOOK_C" "xrt_array_with_capacity" \
            "freestanding-profile/mem: Buffer alloc avoids hosted array allocation"
    else
        record_fail "freestanding-profile/mem: Buffer alloc kept C source missing"
        sed 's/^/      /' "$FREESTANDING_MEM_ALLOC_HOOK_REAL_LOG" | sed -n '1,120p'
    fi
    FREESTANDING_MEM_ALLOC_HOOK_UNDEFINED="$(nm_undefined_normalized "$FREESTANDING_MEM_ALLOC_HOOK_OBJ")"
    FREESTANDING_MEM_ALLOC_HOOK_UNEXPECTED="$(printf '%s\n' "$FREESTANDING_MEM_ALLOC_HOOK_UNDEFINED" |
        sed '/^[[:space:]]*$/d' |
        grep -Ev '^(xr_hook_alloc|xr_hook_free|xr_hook_panic|memcpy|memmove|memset|memcmp)$' || true)"
    if [ -z "$FREESTANDING_MEM_ALLOC_HOOK_UNEXPECTED" ]; then
        record_pass "freestanding-profile/mem: Buffer alloc undefined symbols stay hook/memcpy-only"
    else
        record_fail "freestanding-profile/mem: Buffer alloc unexpected undefined symbols"
        printf '%s\n' "$FREESTANDING_MEM_ALLOC_HOOK_UNEXPECTED" | sed 's/^/      /'
    fi
else
    record_fail "freestanding-profile/mem: Buffer allocator object build failed"
    sed 's/^/      /' "$FREESTANDING_MEM_ALLOC_HOOK_REAL_LOG" | sed -n '1,120p'
fi

FREESTANDING_MEM_ALLOC_SELECTIVE_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_mem_alloc_selective.xr"
FREESTANDING_MEM_ALLOC_SELECTIVE_OBJ="$WORK/freestanding_mem_alloc_selective.o"
FREESTANDING_MEM_ALLOC_SELECTIVE_LOG="$WORK/freestanding_mem_alloc_selective.log"
if "$XRAY" build --native --profile freestanding --shared --keep-c --rebuild \
        --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_MEM_ALLOC_SELECTIVE_OBJ" \
        "$FREESTANDING_MEM_ALLOC_SELECTIVE_SRC" >"$FREESTANDING_MEM_ALLOC_SELECTIVE_LOG" 2>&1; then
    FREESTANDING_MEM_ALLOC_SELECTIVE_C="$(sed -n 's/^Kept C source: //p' "$FREESTANDING_MEM_ALLOC_SELECTIVE_LOG" | tail -n 1)"
    if [ -f "$FREESTANDING_MEM_ALLOC_SELECTIVE_C" ]; then
        expect_log_contains "$FREESTANDING_MEM_ALLOC_SELECTIVE_C" "xrt_mem_alloc_zeroed(" \
            "freestanding-profile/mem: selective Buffer alloc import lowers"
        expect_log_contains "$FREESTANDING_MEM_ALLOC_SELECTIVE_C" "xrt_release(" \
            "freestanding-profile/mem: selective Buffer alloc releases"
        expect_log_not_contains "$FREESTANDING_MEM_ALLOC_SELECTIVE_C" "xrt_arc_alloc" \
            "freestanding-profile/mem: selective Buffer alloc avoids hosted ARC allocator"
    else
        record_fail "freestanding-profile/mem: selective Buffer alloc kept C source missing"
        sed 's/^/      /' "$FREESTANDING_MEM_ALLOC_SELECTIVE_LOG" | sed -n '1,120p'
    fi
else
    record_fail "freestanding-profile/mem: selective Buffer allocator object build failed"
    sed 's/^/      /' "$FREESTANDING_MEM_ALLOC_SELECTIVE_LOG" | sed -n '1,120p'
fi

FREESTANDING_NO_ALLOC_STACK_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_no_alloc_stack.xr"
FREESTANDING_NO_ALLOC_STACK_OBJ="$WORK/freestanding_no_alloc_stack.o"
FREESTANDING_NO_ALLOC_STACK_LOG="$WORK/freestanding_no_alloc_stack.log"
if "$XRAY" build --native --profile freestanding --shared --keep-c --rebuild \
        --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$FREESTANDING_NO_ALLOC_STACK_OBJ" \
        "$FREESTANDING_NO_ALLOC_STACK_SRC" >"$FREESTANDING_NO_ALLOC_STACK_LOG" 2>&1; then
    FREESTANDING_NO_ALLOC_STACK_C="$(sed -n 's/^Kept C source: //p' "$FREESTANDING_NO_ALLOC_STACK_LOG" | tail -n 1)"
    if [ -f "$FREESTANDING_NO_ALLOC_STACK_C" ]; then
        expect_log_contains "$FREESTANDING_NO_ALLOC_STACK_C" "xrt_span_from_array_slice" \
            "freestanding-profile/no-alloc: fixed-array Span path remains allowed"
        expect_log_not_contains "$FREESTANDING_NO_ALLOC_STACK_C" "xrt_mem_alloc" \
            "freestanding-profile/no-alloc: fixed-array path avoids allocator hook"
        expect_log_not_contains "$FREESTANDING_NO_ALLOC_STACK_C" "xrt_arc_alloc" \
            "freestanding-profile/no-alloc: fixed-array path avoids hosted ARC allocator"
        expect_log_not_contains "$FREESTANDING_NO_ALLOC_STACK_C" "xrt_array_with_capacity" \
            "freestanding-profile/no-alloc: fixed-array path avoids hosted array allocation"
    else
        record_fail "freestanding-profile/no-alloc: kept C source missing"
        sed 's/^/      /' "$FREESTANDING_NO_ALLOC_STACK_LOG" | sed -n '1,120p'
    fi
    FREESTANDING_NO_ALLOC_STACK_UNDEFINED="$(nm_undefined_normalized "$FREESTANDING_NO_ALLOC_STACK_OBJ")"
    FREESTANDING_NO_ALLOC_STACK_UNEXPECTED="$(printf '%s\n' "$FREESTANDING_NO_ALLOC_STACK_UNDEFINED" |
        sed '/^[[:space:]]*$/d' |
        grep -Ev '^(xr_hook_panic|memcpy|memmove|memset|memcmp)$' || true)"
    if [ -z "$FREESTANDING_NO_ALLOC_STACK_UNEXPECTED" ]; then
        record_pass "freestanding-profile/no-alloc: undefined symbols stay hook/memcpy-only"
    else
        record_fail "freestanding-profile/no-alloc: unexpected undefined symbols"
        printf '%s\n' "$FREESTANDING_NO_ALLOC_STACK_UNEXPECTED" | sed 's/^/      /'
    fi
else
    record_fail "freestanding-profile/no-alloc: fixed-array object build failed"
    sed 's/^/      /' "$FREESTANDING_NO_ALLOC_STACK_LOG" | sed -n '1,120p'
fi

FREESTANDING_NO_ALLOC_HEAP_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_no_alloc_heap_reject.xr"
FREESTANDING_NO_ALLOC_HEAP_LOG="$WORK/freestanding_no_alloc_heap_reject.log"
if "$XRAY" build --native --profile freestanding --shared --keep-c --rebuild \
        --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$WORK/freestanding_no_alloc_heap_reject.o" \
        "$FREESTANDING_NO_ALLOC_HEAP_SRC" >"$FREESTANDING_NO_ALLOC_HEAP_LOG" 2>&1; then
    record_fail "freestanding-profile/no-alloc: rejects heap allocator use"
    sed 's/^/      /' "$FREESTANDING_NO_ALLOC_HEAP_LOG" | sed -n '1,120p'
else
    expect_log_contains "$FREESTANDING_NO_ALLOC_HEAP_LOG" \
        "@no_alloc function 'xray_no_alloc_heap_reject' allocates via stdlib 'mem.alloc'" \
        "freestanding-profile/no-alloc: rejects heap allocator use"
fi

FREESTANDING_NO_ALLOC_RESIZE_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_no_alloc_buffer_resize_reject.xr"
FREESTANDING_NO_ALLOC_RESIZE_LOG="$WORK/freestanding_no_alloc_buffer_resize_reject.log"
if "$XRAY" build --native --profile freestanding --shared --keep-c --rebuild \
        --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$WORK/freestanding_no_alloc_buffer_resize_reject.o" \
        "$FREESTANDING_NO_ALLOC_RESIZE_SRC" >"$FREESTANDING_NO_ALLOC_RESIZE_LOG" 2>&1; then
    record_fail "freestanding-profile/no-alloc: rejects Buffer.resize allocation"
    sed 's/^/      /' "$FREESTANDING_NO_ALLOC_RESIZE_LOG" | sed -n '1,120p'
else
    expect_log_contains "$FREESTANDING_NO_ALLOC_RESIZE_LOG" \
        "@no_alloc function 'xray_no_alloc_buffer_resize_reject' allocates via method 'Buffer.resize'" \
        "freestanding-profile/no-alloc: rejects Buffer.resize allocation"
fi

FREESTANDING_NO_ALLOC_CALL_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_no_alloc_call_reject.xr"
FREESTANDING_NO_ALLOC_CALL_LOG="$WORK/freestanding_no_alloc_call_reject.log"
if "$XRAY" build --native --profile freestanding --shared --keep-c --rebuild \
        --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$WORK/freestanding_no_alloc_call_reject.o" \
        "$FREESTANDING_NO_ALLOC_CALL_SRC" >"$FREESTANDING_NO_ALLOC_CALL_LOG" 2>&1; then
    record_fail "freestanding-profile/no-alloc: rejects indirect allocator use"
    sed 's/^/      /' "$FREESTANDING_NO_ALLOC_CALL_LOG" | sed -n '1,120p'
else
    expect_log_contains "$FREESTANDING_NO_ALLOC_CALL_LOG" \
        "@no_alloc function 'xray_no_alloc_call_reject' allocates via call 'xray_no_alloc_helper_allocates'" \
        "freestanding-profile/no-alloc: rejects indirect allocator use"
fi

FREESTANDING_MEM_PAGE_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_mem_page_reject.xr"
FREESTANDING_MEM_PAGE_LOG="$WORK/freestanding_mem_page_reject.log"
if "$XRAY" build --native --profile freestanding --dry-run-link --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$WORK/freestanding_mem_page_reject" \
        "$FREESTANDING_MEM_PAGE_SRC" >"$FREESTANDING_MEM_PAGE_LOG" 2>&1; then
    record_fail "freestanding-profile/mem: rejects page allocator member"
    sed 's/^/      /' "$FREESTANDING_MEM_PAGE_LOG" | sed -n '1,120p'
else
    expect_log_contains "$FREESTANDING_MEM_PAGE_LOG" \
        "freestanding profile rejects mem.pageAlloc" \
        "freestanding-profile/mem: rejects page allocator member"
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

LINKER_SCRIPT_NON_NATIVE_LOG="$WORK/linker_script_non_native.log"
if "$XRAY" build --linker-script "$FREESTANDING_LINKER_SCRIPT" \
        -o "$WORK/linker_script_non_native" \
        "$FREESTANDING_EXPORT_SRC" >"$LINKER_SCRIPT_NON_NATIVE_LOG" 2>&1; then
    record_fail "linker-script: requires native backend"
    sed 's/^/      /' "$LINKER_SCRIPT_NON_NATIVE_LOG" | sed -n '1,120p'
else
    expect_log_contains "$LINKER_SCRIPT_NON_NATIVE_LOG" "--linker-script requires --native" \
        "linker-script: requires native backend"
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
    expect_log_contains "$DATETIME_OFFSET_LOG" "-lxray_aot_core" "system-datetime-offset: links AOT core stdlib only"
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
    expect_log_contains "$CORE_DATETIME_LOG" "-lxray_aot_core" "core-datetime: links AOT core stdlib only"
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

FREESTANDING_CATCH_PANIC_SRC="$PROJECT_DIR/tests/aot/filetests/link/freestanding_catch_panic_reject.xr"
FREESTANDING_CATCH_PANIC_LOG="$WORK/freestanding_catch_panic_reject.log"
if "$XRAY" build --native --profile freestanding --dry-run-link --dump-link-command \
        --cache-dir "$BUILD_CACHE" -o "$WORK/freestanding_catch_panic_reject" \
        "$FREESTANDING_CATCH_PANIC_SRC" >"$FREESTANDING_CATCH_PANIC_LOG" 2>&1; then
    record_fail "freestanding-profile: rejects catch panic"
    sed 's/^/      /' "$FREESTANDING_CATCH_PANIC_LOG" | sed -n '1,120p'
else
    expect_log_contains "$FREESTANDING_CATCH_PANIC_LOG" \
        "freestanding profile rejects catch panic" \
        "freestanding-profile: rejects catch panic"
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

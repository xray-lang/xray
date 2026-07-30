#!/usr/bin/env bash
# Cross-target no-libc regression for the shared numeric conversion core.

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
XRAY=${1:-$ROOT/build/xray}
CASE="$ROOT/tests/aot/provider/freestanding_numeric_core.c"

skip() {
    echo "SKIP: $1"
    exit 77
}

ZIG=${XRAY_ZIG:-}
if [ -z "$ZIG" ]; then
    ZIG=$(command -v zig 2>/dev/null || true)
fi
[ -n "$ZIG" ] || skip "zig not found"

LLVM_NM=${LLVM_NM:-}
if [ -z "$LLVM_NM" ]; then
    LLVM_NM=$(command -v llvm-nm 2>/dev/null || true)
fi
[ -n "$LLVM_NM" ] || skip "llvm-nm not found"
[ -x "$XRAY" ] || skip "xray binary not found: $XRAY"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/xray-freestanding-numeric.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
PROBE_HOME="$WORK/home"
mkdir -p "$PROBE_HOME"

compile_target() {
    local target=$1
    local cpu=${2:-}
    local object="$WORK/${target}.o"
    local undefined="$WORK/${target}.undefined"
    local log="$WORK/${target}.compile.log"
    local cpu_arg=
    if [ -n "$cpu" ]; then
        cpu_arg="-mcpu=$cpu"
    fi

    if ! ZIG_GLOBAL_CACHE_DIR="$WORK/zig-global-cache" \
            ZIG_LOCAL_CACHE_DIR="$WORK/zig-local-cache" \
            "$ZIG" cc -target "$target" ${cpu_arg:+"$cpu_arg"} \
            -std=c11 -O2 -fno-inline -ffreestanding -fno-builtin \
            -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
            -I"$ROOT/src" -c "$CASE" -o "$object" >"$log" 2>&1; then
        sed 's/^/  /' "$log" >&2
        echo "FAIL: numeric core did not compile for $target" >&2
        exit 1
    fi

    "$LLVM_NM" -u "$object" >"$undefined"
    if grep -Eq 'mem(cpy|move)' "$undefined"; then
        sed 's/^/  /' "$undefined" >&2
        echo "FAIL: numeric core retained a memory-copy dependency for $target" >&2
        exit 1
    fi
}

probe_target() {
    local target=$1
    local output="$WORK/${target}.probe.json"
    if ! HOME="$PROBE_HOME" "$XRAY" toolchain probe \
            --target "$target" --provider zig --zig "$ZIG" \
            --profile freestanding --no-run --refresh --json >"$output" 2>&1; then
        sed 's/^/  /' "$output" >&2
        echo "FAIL: freestanding SDK compile probe failed for $target" >&2
        exit 1
    fi
    grep -Fq '"sdkCompile":"ok"' "$output" || {
        sed 's/^/  /' "$output" >&2
        echo "FAIL: freestanding SDK compile was not ready for $target" >&2
        exit 1
    }
}

compile_target riscv32-freestanding-none
compile_target riscv64-freestanding-none
compile_target thumb-freestanding-eabi cortex_m4
compile_target x86_64-freestanding-none

probe_target riscv32-freestanding-none
probe_target riscv64-freestanding-none
probe_target thumb-freestanding-eabi
probe_target x86_64-freestanding-none

echo "PASS: freestanding numeric core compiled without libc for RV32/RV64/Thumb/x86"

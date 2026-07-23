#!/usr/bin/env bash
# Optional freestanding Cortex-M QEMU boot smoke.
#
# The test builds a no-libc Thumb ELF for qemu-system-arm mps2-an385, then
# boots it and checks serial output from direct UART MMIO writes.
# Set XRAY_FREESTANDING_QEMU_REQUIRED=1 in CI to turn dependency skips into
# hard failures.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
XRAY="${1:-$PROJECT_DIR/build/xray}"

skip() {
    if [ "${XRAY_FREESTANDING_QEMU_REQUIRED:-0}" = "1" ]; then
        echo "FAIL: required freestanding QEMU smoke dependency missing: $1" >&2
        exit 1
    fi
    echo "SKIP: $1"
    exit 77
}

fail() {
    echo "FAIL: $1" >&2
    exit 1
}

[ -x "$XRAY" ] || skip "xray binary not found: $XRAY"

ZIG="${XRAY_ZIG:-}"
if [ -z "$ZIG" ]; then
    ZIG="$(command -v zig 2>/dev/null || true)"
fi
[ -n "$ZIG" ] || skip "zig not found"

READELF="${LLVM_READELF:-}"
if [ -z "$READELF" ]; then
    READELF="$(command -v llvm-readelf 2>/dev/null || true)"
fi
[ -n "$READELF" ] || skip "llvm-readelf not found"

LLVM_NM="${LLVM_NM:-}"
if [ -z "$LLVM_NM" ]; then
    LLVM_NM="$(command -v llvm-nm 2>/dev/null || true)"
fi
[ -n "$LLVM_NM" ] || skip "llvm-nm not found"

QEMU="${XRAY_QEMU_SYSTEM_ARM:-}"
if [ -z "$QEMU" ]; then
    QEMU="$(command -v qemu-system-arm 2>/dev/null || true)"
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_freestanding_thumb_qemu.XXXXXX")" || {
    echo "error: cannot create temporary directory" >&2
    exit 1
}
QEMU_PID=""
cleanup() {
    if [ -n "$QEMU_PID" ]; then
        kill "$QEMU_PID" >/dev/null 2>&1 || true
        wait "$QEMU_PID" >/dev/null 2>&1 || true
    fi
    if [ "${XRAY_KEEP_TEST_TMP:-0}" = "1" ]; then
        echo "kept freestanding Cortex-M smoke workdir: $WORK" >&2
        return
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

SRC="$WORK/freestanding_thumb_uart.xr"
cat >"$SRC" <<'XR'
import mem

const UART0_DATA = 0x40004000
const UART0_CTRL = 0x40004008
const MPS2_LEDS = 0x40028000

fn kernel_entry() -> i32 {
    mem.volatileStore(mem.mutPtr<byte>(MPS2_LEDS), 0x5a, 4)
    // CMSDK APB UART reset leaves transmit disabled; enable TX explicitly.
    mem.volatileStore(mem.mutPtr<byte>(UART0_CTRL), 0x01, 4)
    mem.volatileStore(mem.mutPtr<byte>(UART0_DATA), int('M'.toUInt32()), 4)
    mem.volatileStore(mem.mutPtr<byte>(UART0_DATA), int('4'.toUInt32()), 4)
    mem.volatileStore(mem.mutPtr<byte>(UART0_DATA), 10, 4)
    while (true) {
    }
    return 0
}
XR

cat >"$WORK/xray.toml" <<'TOML'
[package]
name = "freestanding-thumb-qemu-smoke"
version = "1.0.0"
license = "MIT"
main = "freestanding_thumb_uart.xr"

[[freestanding.entry]]
xray = "kernel_entry"
symbol = "xray_kernel_entry"
kind = "start"
section = ".text.xray"
TOML

LD="$WORK/freestanding_thumb.ld"
cat >"$LD" <<'LD'
EXTERN(Reset_Handler)
EXTERN(xray_kernel_entry)
ENTRY(Reset_Handler)
MEMORY {
  FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 512K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 256K
}
_stack_top = ORIGIN(RAM) + LENGTH(RAM);
SECTIONS {
  .vectors ORIGIN(FLASH) : { KEEP(*(.vectors)) } > FLASH
  .text ALIGN(4) : { KEEP(*(".text.xray")) *(.text*) } > FLASH
  .rodata ALIGN(4) : { *(.rodata*) } > FLASH
  .data ALIGN(4) : { *(.data*) } > RAM AT> FLASH
  .bss ALIGN(8) (NOLOAD) : { *(.bss*) *(COMMON) } > RAM
}
LD

GEN_C="$WORK/freestanding_thumb_uart.c"
BUILD_LOG="$WORK/build.log"
if ! (cd "$WORK" && "$XRAY" build --native --profile freestanding --target thumbv7em-none-eabi \
        --toolchain zig --zig "$ZIG" --c-only --rebuild \
        --cache-dir "$WORK/build-cache" -o "$GEN_C" "$SRC") >"$BUILD_LOG" 2>&1; then
    sed 's/^/  /' "$BUILD_LOG" >&2
    fail "freestanding Cortex-M C generation failed"
fi

BOOT_C="$WORK/freestanding_thumb_boot.c"
cat >"$BOOT_C" <<'C'
#include <stdint.h>

extern int32_t xray_kernel_entry(void);
extern uint32_t _stack_top;

void Reset_Handler(void);

__attribute__((noreturn)) void Reset_Handler(void) {
    (void) xray_kernel_entry();
    for (;;) {
    }
}

__attribute__((section(".vectors"), used))
void (*const vector_table[])(void) = {
    (void (*)(void)) (&_stack_top),
    Reset_Handler,
};
C

ELF="$WORK/freestanding_thumb.elf"
GEN_OBJ="$WORK/freestanding_thumb_uart.o"
BOOT_OBJ="$WORK/freestanding_thumb_boot.o"
LINK_LOG="$WORK/link.log"
if ! ZIG_GLOBAL_CACHE_DIR="${ZIG_GLOBAL_CACHE_DIR:-$WORK/zig-global-cache}" \
        ZIG_LOCAL_CACHE_DIR="${ZIG_LOCAL_CACHE_DIR:-$WORK/zig-local-cache}" \
        "$ZIG" cc -target thumb-freestanding-eabi -mcpu=cortex_m4 \
        -O2 -ffp-contract=off -ffreestanding -fno-stack-protector \
        -fno-unwind-tables -fno-asynchronous-unwind-tables \
        -ffunction-sections -fdata-sections -DXRT_IMPL \
        -DXR_AOT_CROSS_TARGET=1 -DXR_AOT_TARGET_PTR_BITS=32 \
        -DXR_AOT_TARGET_LITTLE_ENDIAN=1 -DXRAY_PROFILE_FREESTANDING=1 \
        -I"$PROJECT_DIR/src/aot" -I"$PROJECT_DIR/src" \
        -c "$GEN_C" -o "$GEN_OBJ" >"$LINK_LOG" 2>&1; then
    sed 's/^/  /' "$LINK_LOG" >&2
    fail "freestanding Cortex-M generated C compile failed"
fi
if ! ZIG_GLOBAL_CACHE_DIR="${ZIG_GLOBAL_CACHE_DIR:-$WORK/zig-global-cache}" \
        ZIG_LOCAL_CACHE_DIR="${ZIG_LOCAL_CACHE_DIR:-$WORK/zig-local-cache}" \
        "$ZIG" cc -target thumb-freestanding-eabi -mcpu=cortex_m4 \
        -O2 -ffp-contract=off -ffreestanding -fno-stack-protector \
        -fno-unwind-tables -fno-asynchronous-unwind-tables \
        -ffunction-sections -fdata-sections -I"$PROJECT_DIR/src/aot" \
        -I"$PROJECT_DIR/src" -c "$BOOT_C" -o "$BOOT_OBJ" \
        >>"$LINK_LOG" 2>&1; then
    sed 's/^/  /' "$LINK_LOG" >&2
    fail "freestanding Cortex-M boot C compile failed"
fi
if ! ZIG_GLOBAL_CACHE_DIR="${ZIG_GLOBAL_CACHE_DIR:-$WORK/zig-global-cache}" \
        ZIG_LOCAL_CACHE_DIR="${ZIG_LOCAL_CACHE_DIR:-$WORK/zig-local-cache}" \
        "$ZIG" cc -target thumb-freestanding-eabi -mcpu=cortex_m4 \
        -O2 -ffp-contract=off -nostdlib -Wl,-T,"$LD" -Wl,--gc-sections \
        -o "$ELF" "$BOOT_OBJ" "$GEN_OBJ" >>"$LINK_LOG" 2>&1; then
    sed 's/^/  /' "$LINK_LOG" >&2
    fail "freestanding Cortex-M ELF link failed"
fi

"$READELF" -h "$ELF" >"$WORK/elf.header" 2>&1 ||
    fail "llvm-readelf failed on Cortex-M ELF"
"$READELF" -S "$ELF" >"$WORK/elf.sections" 2>&1 ||
    fail "llvm-readelf section dump failed on Cortex-M ELF"
"$READELF" -s "$ELF" >"$WORK/elf.symbols" 2>&1 ||
    fail "llvm-readelf symbol dump failed on Cortex-M ELF"
"$READELF" -A "$ELF" >"$WORK/elf.attributes" 2>&1 ||
    fail "llvm-readelf attribute dump failed on Cortex-M ELF"
grep -Fq "Class:                             ELF32" "$WORK/elf.header" ||
    fail "freestanding Cortex-M ELF is not ELF32"
grep -Fq "Data:                              2's complement, little endian" "$WORK/elf.header" ||
    fail "freestanding Cortex-M ELF is not little-endian"
grep -Fq "Type:                              EXEC (Executable file)" "$WORK/elf.header" ||
    fail "freestanding Cortex-M ELF is not executable"
grep -Fq "Machine:                           ARM" "$WORK/elf.header" ||
    fail "freestanding Cortex-M ELF is not ARM"
grep -Fq ".vectors" "$WORK/elf.sections" ||
    fail "freestanding Cortex-M ELF is missing .vectors"
grep -Fq ".text" "$WORK/elf.sections" ||
    fail "freestanding Cortex-M ELF is missing .text"
grep -Fq "Reset_Handler" "$WORK/elf.symbols" ||
    fail "freestanding Cortex-M ELF is missing Reset_Handler"
grep -Fq "xray_kernel_entry" "$WORK/elf.symbols" ||
    fail "freestanding Cortex-M ELF is missing xray_kernel_entry"
grep -Fq "_stack_top" "$WORK/elf.symbols" ||
    fail "freestanding Cortex-M ELF is missing _stack_top"
grep -Fq "cortex-m4" "$WORK/elf.attributes" ||
    fail "freestanding Cortex-M ELF attributes do not record cortex-m4"
grep -Fq "Thumb-2" "$WORK/elf.attributes" ||
    fail "freestanding Cortex-M ELF attributes do not record Thumb-2"
if "$LLVM_NM" -u "$ELF" >"$WORK/elf.undefined" 2>&1; then
    if [ -s "$WORK/elf.undefined" ]; then
        sed 's/^/  /' "$WORK/elf.undefined" >&2
        fail "freestanding Cortex-M ELF has undefined symbols"
    fi
else
    sed 's/^/  /' "$WORK/elf.undefined" >&2
    fail "llvm-nm failed on Cortex-M ELF"
fi

[ -n "$QEMU" ] || skip "qemu-system-arm not found"

SERIAL_LOG="$WORK/serial.log"
"$QEMU" -machine mps2-an385 -kernel "$ELF" -display none \
    -serial "file:$SERIAL_LOG" -monitor none -no-reboot -no-shutdown \
    >"$WORK/qemu.log" 2>&1 &
QEMU_PID="$!"

for _ in 1 2 3 4 5 6 7 8 9 10 \
         11 12 13 14 15 16 17 18 19 20 \
         21 22 23 24 25 26 27 28 29 30 \
         31 32 33 34 35 36 37 38 39 40; do
    if [ -f "$SERIAL_LOG" ] && grep -aq "M4" "$SERIAL_LOG"; then
        echo "PASS: freestanding Cortex-M QEMU boot wrote serial output"
        exit 0
    fi
    sleep 0.05
done

echo "QEMU log:" >&2
sed 's/^/  /' "$WORK/qemu.log" >&2
if [ -f "$SERIAL_LOG" ]; then
    echo "Serial log:" >&2
    sed 's/^/  /' "$SERIAL_LOG" >&2
fi
fail "freestanding Cortex-M QEMU serial output did not contain M4"

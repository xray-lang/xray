#!/usr/bin/env bash
# Optional freestanding RISC-V QEMU boot smoke.
#
# The test builds a no-libc ELF for qemu-system-riscv32 virt, then boots it and
# checks serial output from direct UART MMIO writes.
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

QEMU="${XRAY_QEMU_SYSTEM_RISCV32:-}"
if [ -z "$QEMU" ]; then
    QEMU="$(command -v qemu-system-riscv32 2>/dev/null || true)"
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_freestanding_riscv_qemu.XXXXXX")" || {
    echo "error: cannot create temporary directory" >&2
    exit 1
}
QEMU_PID=""
cleanup() {
    if [ -n "$QEMU_PID" ]; then
        kill "$QEMU_PID" >/dev/null 2>&1 || true
        wait "$QEMU_PID" >/dev/null 2>&1 || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

SRC="$WORK/freestanding_riscv_uart.xr"
cat >"$SRC" <<'XR'
import mem

const UART0 = 0x10000000

@section(".text.entry")
@used
@c_export("xray_kernel_entry")
fn kernel_entry() -> int32 {
    mem.volatileStore(mem.fromAddress(UART0), 'R' as int, 1)
    mem.volatileStore(mem.fromAddress(UART0), 'V' as int, 1)
    mem.volatileStore(mem.fromAddress(UART0), 10, 1)
    while (true) {
    }
    return 0
}
XR

LD="$WORK/freestanding_riscv.ld"
cat >"$LD" <<'LD'
EXTERN(xray_kernel_entry)
ENTRY(_start)
SECTIONS {
  . = 0x80000000;
  _start = xray_kernel_entry;
  .text ALIGN(4) : { KEEP(*(".text.entry")) *(.text*) }
  .rodata ALIGN(4) : { *(.rodata*) }
  .sdata ALIGN(4) : { *(.sdata*) }
  .data ALIGN(4) : { *(.data*) }
  .bss ALIGN(8) : { *(.bss*) *(COMMON) }
}
LD

ELF="$WORK/freestanding_riscv.elf"
BUILD_LOG="$WORK/build.log"
if ! ZIG_GLOBAL_CACHE_DIR="${ZIG_GLOBAL_CACHE_DIR:-$WORK/zig-global-cache}" \
        ZIG_LOCAL_CACHE_DIR="${ZIG_LOCAL_CACHE_DIR:-$WORK/zig-local-cache}" \
        "$XRAY" build --native --profile freestanding \
        --target riscv32imac-unknown-none-elf --toolchain zig --zig "$ZIG" \
        --linker-script "$LD" --keep-c --rebuild --dump-link-command \
        --cache-dir "$WORK/build-cache" -o "$ELF" "$SRC" >"$BUILD_LOG" 2>&1; then
    sed 's/^/  /' "$BUILD_LOG" >&2
    fail "freestanding RISC-V ELF build failed"
fi

grep -Fq -- "-Wl,-T,$LD" "$BUILD_LOG" ||
    fail "freestanding RISC-V build did not pass linker script"
if grep -Fq "cannot find entry symbol" "$BUILD_LOG"; then
    fail "freestanding RISC-V ELF link reported missing entry symbol"
fi

"$READELF" -h "$ELF" >"$WORK/elf.header" 2>&1 ||
    fail "llvm-readelf failed on RISC-V ELF"
"$READELF" -S "$ELF" >"$WORK/elf.sections" 2>&1 ||
    fail "llvm-readelf section dump failed on RISC-V ELF"
"$READELF" -s "$ELF" >"$WORK/elf.symbols" 2>&1 ||
    fail "llvm-readelf symbol dump failed on RISC-V ELF"
grep -Fq "Class:                             ELF32" "$WORK/elf.header" ||
    fail "freestanding RISC-V ELF is not ELF32"
grep -Fq "Data:                              2's complement, little endian" "$WORK/elf.header" ||
    fail "freestanding RISC-V ELF is not little-endian"
grep -Fq "Type:                              EXEC (Executable file)" "$WORK/elf.header" ||
    fail "freestanding RISC-V ELF is not executable"
grep -Fq "Machine:                           RISC-V" "$WORK/elf.header" ||
    fail "freestanding RISC-V ELF is not RISC-V"
if grep -Fq "Entry point address:               0x0" "$WORK/elf.header"; then
    fail "freestanding RISC-V ELF has a zero entry"
fi
grep -Fq ".text" "$WORK/elf.sections" ||
    fail "freestanding RISC-V ELF is missing .text"
grep -Fq "xray_kernel_entry" "$WORK/elf.symbols" ||
    fail "freestanding RISC-V ELF is missing xray_kernel_entry"
grep -Fq "_start" "$WORK/elf.symbols" ||
    fail "freestanding RISC-V ELF is missing _start"
if "$LLVM_NM" -u "$ELF" >"$WORK/elf.undefined" 2>&1; then
    if [ -s "$WORK/elf.undefined" ]; then
        sed 's/^/  /' "$WORK/elf.undefined" >&2
        fail "freestanding RISC-V ELF has undefined symbols"
    fi
else
    sed 's/^/  /' "$WORK/elf.undefined" >&2
    fail "llvm-nm failed on RISC-V ELF"
fi

[ -n "$QEMU" ] || skip "qemu-system-riscv32 not found"

SERIAL_LOG="$WORK/serial.log"
"$QEMU" -machine virt -m 128M -bios none -kernel "$ELF" -display none \
    -serial "file:$SERIAL_LOG" -monitor none -no-reboot -no-shutdown \
    >"$WORK/qemu.log" 2>&1 &
QEMU_PID="$!"

for _ in 1 2 3 4 5 6 7 8 9 10 \
         11 12 13 14 15 16 17 18 19 20 \
         21 22 23 24 25 26 27 28 29 30 \
         31 32 33 34 35 36 37 38 39 40; do
    if [ -f "$SERIAL_LOG" ] && grep -aq "RV" "$SERIAL_LOG"; then
        echo "PASS: freestanding RISC-V QEMU boot wrote serial output"
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
fail "freestanding RISC-V QEMU serial output did not contain RV"

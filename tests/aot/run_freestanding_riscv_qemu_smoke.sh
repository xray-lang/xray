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
    if [ "${XRAY_KEEP_TEST_TMP:-0}" = "1" ]; then
        echo "kept freestanding RISC-V smoke workdir: $WORK" >&2
        return
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

SRC="$WORK/freestanding_riscv_uart.xr"
cat >"$SRC" <<'XR'
import mem

const UART0 = 0x10000000

fn uart_init() {
    // 16550A power-on state is not a usable cross-version contract. Program a
    // deterministic 8N1 configuration before observing the transmit port.
    mem.volatileStore(mem.mutPtr<byte>(UART0 + 1), 0x00, 1)
    mem.volatileStore(mem.mutPtr<byte>(UART0 + 3), 0x80, 1)
    mem.volatileStore(mem.mutPtr<byte>(UART0 + 0), 0x03, 1)
    mem.volatileStore(mem.mutPtr<byte>(UART0 + 1), 0x00, 1)
    mem.volatileStore(mem.mutPtr<byte>(UART0 + 3), 0x03, 1)
    mem.volatileStore(mem.mutPtr<byte>(UART0 + 2), 0x07, 1)
}

fn kernel_entry() -> i32 {
    uart_init()
    unsafe { startupLinked() }
    // Keep the boot fixture below the freestanding runtime boundary: rune
    // methods require hosted builtin initialization and are tested elsewhere.
    mem.volatileStore(mem.mutPtr<byte>(UART0), 82, 1)
    mem.volatileStore(mem.mutPtr<byte>(UART0), 86, 1)
    mem.volatileStore(mem.mutPtr<byte>(UART0), 10, 1)
    while (true) {
    }
    return 0
}

extern "C" {
    fn startupLinked()
}
XR

START_ASM="$WORK/freestanding_riscv_start.S"
cat >"$START_ASM" <<'ASM'
.section .text.start, "ax", @progbits
.globl _start
.type _start, @function
_start:
    la sp, _stack_top
    call xray_kernel_entry
1:
    wfi
    j 1b
.size _start, . - _start

.globl xray_startup_linked
.type xray_startup_linked, @function
xray_startup_linked:
    ret
.size xray_startup_linked, .-xray_startup_linked
ASM

if command -v sha256sum >/dev/null 2>&1; then
    START_HASH="$(sha256sum "$START_ASM" | awk '{print $1}')"
elif command -v shasum >/dev/null 2>&1; then
    START_HASH="$(shasum -a 256 "$START_ASM" | awk '{print $1}')"
else
    skip "SHA-256 utility not found"
fi

cat >"$WORK/xray.toml" <<TOML
[package]
name = "freestanding-riscv-qemu-smoke"
version = "1.0.0"
license = "MIT"
main = "freestanding_riscv_uart.xr"

[native]
name = "freestanding-riscv-start"
version = "1.0.0"
license = "MIT"
source = "in-tree freestanding QEMU regression stub"
audit_mode = "shipping"
vm = "unsupported"

[[native.unit]]
name = "riscv-start"
kind = "asm"
sources = ["freestanding_riscv_start.S"]
source_hashes = ["$START_HASH"]
optimization = "release"
visibility = "hidden"
warnings = "strict"
output = "freestanding_riscv_start.o"
purpose = "initialize the machine stack before entering generated Xray code"

[[native.symbol]]
xray = "startupLinked"
native = "xray_startup_linked"
kind = "function"
calling_convention = "c"
unit = "riscv-start"

[native.symbol.contract]
params = []
return = { ownership = "value", nullable = false, validity = "void" }
effects = ["foreign"]
callbacks = []
failure = "none"
allocation = "none"
blocking = "never"
suspend = "never"
io = "none"
sync = "none"
panic = "never"
error = "none"

[[freestanding.entry]]
xray = "kernel_entry"
symbol = "xray_kernel_entry"
kind = "start"
section = ".text.entry"
stub = "freestanding_riscv_start.S"
TOML

LD="$WORK/freestanding_riscv.ld"
cat >"$LD" <<'LD'
EXTERN(xray_kernel_entry)
ENTRY(_start)
SECTIONS {
  . = 0x80000000;
  .text ALIGN(4) : { KEEP(*(.text.start)) KEEP(*(".text.entry")) *(.text*) }
  .rodata ALIGN(4) : { *(.rodata*) }
  .sdata ALIGN(4) : { *(.sdata*) }
  .data ALIGN(4) : { *(.data*) }
  .bss ALIGN(8) : { *(.bss*) *(COMMON) }
  _stack_top = 0x87fff000;
}
LD

ELF="$WORK/freestanding_riscv.elf"
BUILD_LOG="$WORK/build.log"
if ! (cd "$WORK" && ZIG_GLOBAL_CACHE_DIR="${ZIG_GLOBAL_CACHE_DIR:-$WORK/zig-global-cache}" \
        ZIG_LOCAL_CACHE_DIR="${ZIG_LOCAL_CACHE_DIR:-$WORK/zig-local-cache}" \
        "$XRAY" build --native --profile freestanding \
        --target riscv32-freestanding-none --toolchain zig --zig "$ZIG" \
        --linker-script "$LD" --keep-c --rebuild --dump-link-command \
        --cache-dir "$WORK/build-cache" -o "$ELF" "$SRC") >"$BUILD_LOG" 2>&1; then
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

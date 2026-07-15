#!/usr/bin/env bash
# Optional freestanding x86_64 QEMU boot smoke.
#
# The test builds a no-libc ELF with a multiboot header, boots it under QEMU,
# then checks both VGA text memory and serial output from xr_hook_write.
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

PYTHON="${PYTHON:-python3}"
command -v "$PYTHON" >/dev/null 2>&1 || skip "python3 not found"

QEMU="${XRAY_QEMU_SYSTEM_X86_64:-}"
if [ -z "$QEMU" ]; then
    QEMU="$(command -v qemu-system-x86_64 2>/dev/null || true)"
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_freestanding_qemu.XXXXXX")" || {
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

SRC="$WORK/freestanding_qemu_io.xr"
cat >"$SRC" <<'XR'
import mem

const MULTIBOOT_MAGIC: uint32 = 0x1BADB002
const MULTIBOOT_FLAGS: uint32 = 0x00000003

struct MultibootHeader {
    magic: uint32
    flags: uint32
    checksum: uint32
}

@section("__DATA,.xray_multiboot")
@used
const MULTIBOOT = comptime MultibootHeader{
    magic: MULTIBOOT_MAGIC,
    flags: MULTIBOOT_FLAGS,
    checksum: 0 - (MULTIBOOT_MAGIC + MULTIBOOT_FLAGS),
}

@section("__TEXT,.xray_boot")
@used
@c_export("xray_kernel_entry")
fn kernel_entry() -> int32 {
    const vga = 0xb8000
    mem.volatileStore(mem.mutPtr<byte>(vga), 'X' as int, 1)
    mem.volatileStore(mem.mutPtr<byte>(vga + 1), 0x0f, 1)
    print("XR")
    while (true) {
    }
    return MULTIBOOT.magic as int32
}
XR

LD="$WORK/freestanding_qemu.ld"
cat >"$LD" <<'LD'
EXTERN(xray_kernel_entry)
ENTRY(_start)
SECTIONS {
  . = 0x100000;
  _start = xray_kernel_entry;
  .xray_multiboot ALIGN(4) : { KEEP(*("__DATA,.xray_multiboot")) }
  .text ALIGN(16) : { KEEP(*("__TEXT,.xray_boot")) *(.text*) }
  .rodata ALIGN(16) : { *(.rodata*) }
  .eh_frame_hdr ALIGN(4) : { *(.eh_frame_hdr) }
  .eh_frame ALIGN(8) : { *(.eh_frame) }
  .data ALIGN(16) : { *(.data*) }
  .bss ALIGN(16) : { *(.bss*) *(COMMON) }
}
LD

GEN_C="$WORK/freestanding_qemu.c"
BUILD_LOG="$WORK/build.log"
if ! "$XRAY" build --native --profile freestanding --target x86_64-linux-musl \
        --toolchain zig --c-only --rebuild --cache-dir "$WORK/build-cache" \
        -o "$GEN_C" "$SRC" >"$BUILD_LOG" 2>&1; then
    sed 's/^/  /' "$BUILD_LOG" >&2
    fail "freestanding QEMU C generation failed"
fi

HOOK_C="$WORK/freestanding_qemu_hooks.c"
cat >"$HOOK_C" <<'C'
#include <stddef.h>
#include <stdint.h>

#define COM1 0x3f8u

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void serial_init_once(void) {
    static int initialized;
    if (initialized)
        return;
    outb(COM1 + 1u, 0x00u);
    outb(COM1 + 3u, 0x80u);
    outb(COM1 + 0u, 0x03u);
    outb(COM1 + 1u, 0x00u);
    outb(COM1 + 3u, 0x03u);
    outb(COM1 + 2u, 0xc7u);
    outb(COM1 + 4u, 0x0bu);
    initialized = 1;
}

void xr_hook_write(const char *bytes, size_t len) {
    serial_init_once();
    for (size_t i = 0; i < len; i++) {
        while ((inb(COM1 + 5u) & 0x20u) == 0u) {
        }
        outb(COM1, (uint8_t) bytes[i]);
    }
}

__attribute__((noreturn)) void xr_hook_panic(const char *message, size_t len) {
    xr_hook_write("PANIC:", 6);
    xr_hook_write(message, len);
    for (;;) {
    }
}
C

ELF="$WORK/freestanding_qemu.elf"
GEN_OBJ="$WORK/freestanding_qemu.o"
HOOK_OBJ="$WORK/freestanding_qemu_hooks.o"
LINK_LOG="$WORK/link.log"
if ! ZIG_GLOBAL_CACHE_DIR="${ZIG_GLOBAL_CACHE_DIR:-$WORK/zig-global-cache}" \
        ZIG_LOCAL_CACHE_DIR="${ZIG_LOCAL_CACHE_DIR:-$WORK/zig-local-cache}" \
        "$ZIG" cc -target x86_64-linux-musl -O2 -ffp-contract=off \
        -ffreestanding -fno-stack-protector -fno-unwind-tables \
        -fno-asynchronous-unwind-tables -ffunction-sections \
        -fdata-sections -DXRT_IMPL -DXR_AOT_CROSS_TARGET=1 \
        -DXRAY_PROFILE_FREESTANDING=1 -I"$PROJECT_DIR/src/aot" \
        -I"$PROJECT_DIR/src" -c "$GEN_C" -o "$GEN_OBJ" >"$LINK_LOG" 2>&1; then
    sed 's/^/  /' "$LINK_LOG" >&2
    fail "freestanding QEMU generated C compile failed"
fi
if ! ZIG_GLOBAL_CACHE_DIR="${ZIG_GLOBAL_CACHE_DIR:-$WORK/zig-global-cache}" \
        ZIG_LOCAL_CACHE_DIR="${ZIG_LOCAL_CACHE_DIR:-$WORK/zig-local-cache}" \
        "$ZIG" cc -target x86_64-linux-musl -O2 -ffp-contract=off \
        -ffreestanding -fno-stack-protector -fno-unwind-tables \
        -fno-asynchronous-unwind-tables -ffunction-sections \
        -fdata-sections -I"$PROJECT_DIR/src/aot" -I"$PROJECT_DIR/src" \
        -c "$HOOK_C" -o "$HOOK_OBJ" >>"$LINK_LOG" 2>&1; then
    sed 's/^/  /' "$LINK_LOG" >&2
    fail "freestanding QEMU hook C compile failed"
fi
if ! ZIG_GLOBAL_CACHE_DIR="${ZIG_GLOBAL_CACHE_DIR:-$WORK/zig-global-cache}" \
        ZIG_LOCAL_CACHE_DIR="${ZIG_LOCAL_CACHE_DIR:-$WORK/zig-local-cache}" \
        "$ZIG" cc -target x86_64-linux-musl -O2 -ffp-contract=off \
        -nostdlib -Wl,-T,"$LD" -Wl,--gc-sections \
        -o "$ELF" "$GEN_OBJ" "$HOOK_OBJ" >>"$LINK_LOG" 2>&1; then
    sed 's/^/  /' "$LINK_LOG" >&2
    fail "freestanding QEMU ELF link failed"
fi

"$READELF" -h "$ELF" >"$WORK/elf.header" 2>&1 ||
    fail "llvm-readelf failed on QEMU ELF"
grep -Fq "Type:                              EXEC (Executable file)" "$WORK/elf.header" ||
    fail "freestanding QEMU ELF is not executable"
grep -Fq "Machine:                           Advanced Micro Devices X86-64" "$WORK/elf.header" ||
    fail "freestanding QEMU ELF is not x86_64"
if grep -Fq "Entry point address:               0x0" "$WORK/elf.header"; then
    fail "freestanding QEMU ELF has a zero entry"
fi

[ -n "$QEMU" ] || skip "qemu-system-x86_64 not found"

QMP="$WORK/qmp.sock"
VGA_DUMP="$WORK/vga.bin"
SERIAL_LOG="$WORK/serial.log"
"$QEMU" -machine accel=tcg -m 64M -kernel "$ELF" -display none -no-reboot \
    -no-shutdown -serial "file:$SERIAL_LOG" -monitor none -S \
    -qmp "unix:$QMP,server=on,wait=off" >"$WORK/qemu.log" 2>&1 &
QEMU_PID="$!"

"$PYTHON" - "$QMP" "$VGA_DUMP" "$SERIAL_LOG" <<'PY'
import json
import os
import socket
import sys
import time

sock_path = sys.argv[1]
dump_path = sys.argv[2]
serial_path = sys.argv[3]

deadline = time.time() + 10.0
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
while True:
    try:
        sock.connect(sock_path)
        break
    except FileNotFoundError:
        if time.time() >= deadline:
            raise RuntimeError("QMP socket did not appear")
        time.sleep(0.05)
    except ConnectionRefusedError:
        if time.time() >= deadline:
            raise RuntimeError("QMP socket refused connections")
        time.sleep(0.05)

fp = sock.makefile("rwb", buffering=0)

def recv_msg():
    line = fp.readline()
    if not line:
        raise RuntimeError("QMP connection closed")
    return json.loads(line.decode("utf-8"))

def execute(name, arguments=None):
    payload = {"execute": name}
    if arguments is not None:
        payload["arguments"] = arguments
    fp.write(json.dumps(payload).encode("utf-8") + b"\r\n")
    while True:
        msg = recv_msg()
        if "error" in msg:
            raise RuntimeError(f"QMP {name} failed: {msg['error']}")
        if "return" in msg:
            return msg["return"]

recv_msg()  # greeting
execute("qmp_capabilities")
execute("cont")
time.sleep(0.25)
execute("stop")
execute("human-monitor-command", {
    "command-line": f"pmemsave 0xb8000 2 {dump_path}",
})
try:
    execute("quit")
except Exception:
    pass

deadline = time.time() + 5.0
while not os.path.exists(dump_path):
    if time.time() >= deadline:
        raise RuntimeError("VGA memory dump was not created")
    time.sleep(0.05)

with open(dump_path, "rb") as fh:
    data = fh.read()
if data != b"X\x0f":
    raise RuntimeError(f"unexpected VGA bytes: {data.hex()}")

deadline = time.time() + 5.0
serial = b""
while time.time() < deadline:
    if os.path.exists(serial_path):
        with open(serial_path, "rb") as fh:
            serial = fh.read()
        if b"XR\n" in serial:
            break
    time.sleep(0.05)
if b"XR\n" not in serial:
    raise RuntimeError(f"serial output did not contain XR newline: {serial!r}")
PY

echo "PASS: freestanding QEMU boot wrote VGA cell and serial output"

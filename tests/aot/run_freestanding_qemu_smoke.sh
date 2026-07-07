#!/usr/bin/env bash
# Optional freestanding x86_64 QEMU boot smoke.
#
# The test builds a no-libc ELF with a multiboot header, boots it under QEMU,
# then reads VGA text memory through QMP to prove the kernel entry executed.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
XRAY="${1:-$PROJECT_DIR/build/xray}"

skip() {
    echo "SKIP: $1"
    exit 77
}

fail() {
    echo "FAIL: $1" >&2
    exit 1
}

[ -x "$XRAY" ] || skip "xray binary not found: $XRAY"

QEMU="${XRAY_QEMU_SYSTEM_X86_64:-}"
if [ -z "$QEMU" ]; then
    QEMU="$(command -v qemu-system-x86_64 2>/dev/null || true)"
fi
[ -n "$QEMU" ] || skip "qemu-system-x86_64 not found"

command -v zig >/dev/null 2>&1 || skip "zig not found"
READELF="${LLVM_READELF:-}"
if [ -z "$READELF" ]; then
    READELF="$(command -v llvm-readelf 2>/dev/null || true)"
fi
[ -n "$READELF" ] || skip "llvm-readelf not found"

PYTHON="${PYTHON:-python3}"
command -v "$PYTHON" >/dev/null 2>&1 || skip "python3 not found"

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

SRC="$WORK/freestanding_qemu_vga.xr"
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
    mem.volatileStore(mem.fromAddress(vga), 'X' as int, 1)
    mem.volatileStore(mem.fromAddress(vga + 1), 0x0f, 1)
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

ELF="$WORK/freestanding_qemu.elf"
BUILD_LOG="$WORK/build.log"
if ! ZIG_GLOBAL_CACHE_DIR="${ZIG_GLOBAL_CACHE_DIR:-$WORK/zig-global-cache}" \
        ZIG_LOCAL_CACHE_DIR="${ZIG_LOCAL_CACHE_DIR:-$WORK/zig-local-cache}" \
        "$XRAY" build --native --profile freestanding --target x86_64-linux-musl \
        --toolchain zig --linker-script "$LD" --keep-c --rebuild --dump-link-command \
        --cache-dir "$WORK/build-cache" -o "$ELF" "$SRC" >"$BUILD_LOG" 2>&1; then
    sed 's/^/  /' "$BUILD_LOG" >&2
    fail "freestanding QEMU ELF build failed"
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

QMP="$WORK/qmp.sock"
VGA_DUMP="$WORK/vga.bin"
"$QEMU" -machine accel=tcg -m 64M -kernel "$ELF" -display none -no-reboot \
    -no-shutdown -serial none -monitor none -S \
    -qmp "unix:$QMP,server=on,wait=off" >"$WORK/qemu.log" 2>&1 &
QEMU_PID="$!"

"$PYTHON" - "$QMP" "$VGA_DUMP" <<'PY'
import json
import os
import socket
import sys
import time

sock_path = sys.argv[1]
dump_path = sys.argv[2]

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
PY

echo "PASS: freestanding QEMU boot wrote VGA cell"

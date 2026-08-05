#!/usr/bin/env python3
"""Freestanding QEMU boot smoke, for every supported bare-metal target.

Builds a no-libc kernel from a small Xray fixture, links it with the target's
boot code and linker script, asserts the ELF shape, then boots it under QEMU and
checks what the kernel actually wrote to hardware.

One runner covers all three targets. The shell version was three scripts of
324+281+261 lines that shared their entire structure and differed only in a
handful of parameters -- the second piece of wholesale duplication this task
exists to remove. Everything target-specific now lives in TARGETS below and in
tests/aot/fixtures/freestanding/<target>/; the flow is written once.

Two verification strategies, because the targets genuinely differ:
  - "serial": poll the serial log for a marker (RISC-V, Cortex-M)
  - "qmp_vga": drive QEMU's QMP socket to dump VGA memory, then check the cell
    and the serial log (x86_64, whose kernel writes text-mode memory)

The x86 shell script already embedded a Python block to speak QMP -- shell had
run out of road there. That code is now a first-class function.

Usage:
    run_freestanding_qemu_smoke.py --target x86_64 [--xray PATH]

Exit codes: 0 pass, 1 fail, 77 skip (a missing dependency; set
XRAY_FREESTANDING_QEMU_REQUIRED=1 to turn skips into hard failures).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import socket
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Sequence, Tuple


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, workspace  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent
FIXTURE_ROOT = SCRIPT_DIR / "fixtures" / "freestanding"

SKIP_EXIT = 77

# Include dirs for every cross compile. include/ carries the public ABI headers
# the runtime headers pull in by bare name (xrt_core_freestanding.h ->
# "xray_value_abi.h"); omitting it failed every one of these lanes.
INCLUDE_DIRS = ("src/aot", "include", "src")

# Flags shared by every freestanding cross compile.
COMMON_CFLAGS = (
    "-O2", "-ffp-contract=off", "-ffreestanding", "-fno-stack-protector",
    "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
    "-ffunction-sections", "-fdata-sections",
)


@dataclass(frozen=True)
class Target:
    """Everything that differs between the bare-metal targets."""

    name: str
    # Target triple passed to `zig cc`.
    triple: str
    # Target triple passed to `xray build --target`. Xray and Zig disagree on
    # the 32-bit x86 spelling (i386-linux-musl vs x86-linux-musl), so the two
    # are separate fields rather than one reused string.
    build_target: str
    qemu_binary: str
    qemu_env_var: str
    machine_args: Tuple[str, ...]
    # Extra compiler flags beyond COMMON_CFLAGS (e.g. -mcpu for Cortex-M).
    extra_cflags: Tuple[str, ...] = ()
    # Defines applied only to the generated C (not the boot/hook translation unit).
    generated_defines: Tuple[str, ...] = ()
    # Fixture file names. These match what xray.toml references by name, so they
    # are the manifest's names, not generic ones: the manifest pins `main` and
    # (for RISC-V) the assembly unit's `sources`, and a rename breaks the build.
    main_source: str = ""
    boot_source: Optional[str] = None
    linker_script: str = ""
    # readelf -h / -S / -s / -A substrings that must all be present.
    elf_header_contains: Tuple[str, ...] = ()
    elf_sections_contains: Tuple[str, ...] = ()
    elf_symbols_contains: Tuple[str, ...] = ()
    elf_attributes_contains: Tuple[str, ...] = ()
    # Substrings that must NOT appear in the header (e.g. a zero entry point).
    elf_header_absent: Tuple[str, ...] = ()
    # Reject any undefined symbol in the final ELF (llvm-nm -u must be empty).
    require_no_undefined: bool = False
    # xray.toml carries the boot source's SHA-256 under this placeholder.
    hash_placeholder: Optional[str] = None
    verify: str = "serial"          # "serial" | "qmp_vga"
    serial_marker: bytes = b""
    vga_expect: bytes = b""
    pass_message: str = ""


TARGETS = {
    # 32-bit, and it has to be. QEMU's `-kernel` on x86 goes through the
    # multiboot loader, and multiboot1 only defines a 32-bit protected-mode
    # entry: an ELF64 image is rejected outright with "Cannot load x86-64
    # image, give a 32bit one." The kernel this fixture builds is a multiboot
    # image (magic 0x1BADB002 in .xray_multiboot, placed first by the linker
    # script so it lands inside multiboot's 8 KiB search window), so i386 is
    # what the boot protocol asks for -- not a limitation of the freestanding
    # profile, which is target-neutral and also covers riscv32 and thumb here.
    # The ELF32 header assertion below is what keeps this from regressing:
    # without it, a 64-bit image builds and links cleanly and only fails once
    # QEMU refuses it. The lane keeps the name "x86_64" because that names the
    # emulator it runs under -- qemu-system-x86_64 boots a 32-bit guest fine --
    # not the width of the image it builds.
    "x86_64": Target(
        name="x86_64",
        triple="x86-linux-musl",
        build_target="i386-linux-musl",
        qemu_binary="qemu-system-x86_64",
        qemu_env_var="XRAY_QEMU_SYSTEM_X86_64",
        machine_args=("-machine", "accel=tcg", "-m", "64M"),
        generated_defines=("-DXRT_IMPL", "-DXR_AOT_CROSS_TARGET=1",
                           "-DXR_AOT_TARGET_PTR_BITS=32",
                           "-DXR_AOT_TARGET_LITTLE_ENDIAN=1",
                           "-DXRAY_PROFILE_FREESTANDING=1"),
        main_source="freestanding_qemu_io.xr",
        boot_source="freestanding_qemu_hooks.c",
        linker_script="freestanding_qemu.ld",
        elf_header_contains=(
            "Class:                             ELF32",
            "Data:                              2's complement, little endian",
            "Type:                              EXEC (Executable file)",
            "Machine:                           Intel 80386",
        ),
        elf_header_absent=("Entry point address:               0x0",),
        elf_sections_contains=(".xray_multiboot", ".text"),
        verify="qmp_vga",
        vga_expect=b"X\x0f",
        serial_marker=b"XR\n",
        pass_message="PASS: freestanding QEMU boot wrote VGA cell and serial output",
    ),
    "riscv32": Target(
        name="riscv32",
        triple="riscv32-freestanding-none",
        build_target="riscv32-freestanding-none",
        qemu_binary="qemu-system-riscv32",
        qemu_env_var="XRAY_QEMU_SYSTEM_RISCV32",
        machine_args=("-machine", "virt", "-bios", "none"),
        generated_defines=("-DXRT_IMPL", "-DXR_AOT_CROSS_TARGET=1",
                           "-DXR_AOT_TARGET_PTR_BITS=32",
                           "-DXR_AOT_TARGET_LITTLE_ENDIAN=1",
                           "-DXRAY_PROFILE_FREESTANDING=1"),
        main_source="freestanding_riscv_uart.xr",
        boot_source="freestanding_riscv_start.S",
        linker_script="freestanding_riscv.ld",
        hash_placeholder="$START_HASH",
        elf_header_contains=(
            "Class:                             ELF32",
            "Data:                              2's complement, little endian",
            "Type:                              EXEC (Executable file)",
            "Machine:                           RISC-V",
        ),
        elf_header_absent=("Entry point address:               0x0",),
        elf_sections_contains=(".text",),
        elf_symbols_contains=("xray_kernel_entry", "_start"),
        require_no_undefined=True,
        verify="serial",
        serial_marker=b"RV",
        pass_message="PASS: freestanding RISC-V QEMU boot wrote serial output",
    ),
    "thumb": Target(
        name="thumb",
        triple="thumb-freestanding-eabi",
        build_target="thumb-freestanding-eabi",
        qemu_binary="qemu-system-arm",
        qemu_env_var="XRAY_QEMU_SYSTEM_ARM",
        machine_args=("-machine", "mps2-an385"),
        extra_cflags=("-mcpu=cortex_m4",),
        generated_defines=("-DXRT_IMPL", "-DXR_AOT_CROSS_TARGET=1",
                           "-DXR_AOT_TARGET_PTR_BITS=32",
                           "-DXR_AOT_TARGET_LITTLE_ENDIAN=1",
                           "-DXRAY_PROFILE_FREESTANDING=1"),
        main_source="freestanding_thumb_uart.xr",
        boot_source="freestanding_thumb_boot.c",
        linker_script="freestanding_thumb.ld",
        elf_header_contains=(
            "Class:                             ELF32",
            "Data:                              2's complement, little endian",
            "Type:                              EXEC (Executable file)",
            "Machine:                           ARM",
        ),
        elf_sections_contains=(".vectors", ".text"),
        elf_symbols_contains=("Reset_Handler", "xray_kernel_entry", "_stack_top"),
        elf_attributes_contains=("cortex-m4", "Thumb-2"),
        require_no_undefined=True,
        verify="serial",
        serial_marker=b"M4",
        pass_message="PASS: freestanding Cortex-M QEMU boot wrote serial output",
    ),
}


class Skip(Exception):
    """A dependency is missing. Honors XRAY_FREESTANDING_QEMU_REQUIRED."""


class Fail(Exception):
    """A real failure of the thing under test."""


def resolve_tool(env_var: str, binary: str) -> str:
    """Env override wins, else PATH. Raises Skip when neither resolves."""
    override = os.environ.get(env_var)
    if override:
        return override
    found = shutil.which(binary)
    if not found:
        raise Skip(f"{binary} not found")
    return found


def run_step(argv: Sequence, log: List[bytes], what: str, cwd: "Optional[Path]" = None) -> None:
    """Run a build step, accumulating output and failing with context."""
    result = proc.run(argv, cwd=cwd, timeout=platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300))
    log.append(result.stdout + result.stderr)
    if result.timed_out:
        raise Fail(f"{what} timed out")
    if not result.ok:
        raise Fail(what)


def readelf_dump(readelf: str, flag: str, elf: Path, out: Path, what: str) -> str:
    result = proc.run([readelf, flag, elf], timeout=120)
    out.write_bytes(result.stdout + result.stderr)
    if not result.ok:
        raise Fail(what)
    return (result.stdout + result.stderr).decode("utf-8", "replace")


def check_contains(text: str, needles: Sequence, label: str) -> None:
    for needle in needles:
        if needle not in text:
            raise Fail(f"{label}: missing {needle!r}")


def check_qemu_alive(qemu_proc) -> None:
    """Fail immediately, and with the real reason, if QEMU already exited.

    A QEMU that refuses the image dies before the guest runs, and every
    downstream wait then reports its own timeout instead -- a stale socket, an
    absent marker. Those messages describe the symptom and hide the cause, so
    the exit is checked directly at each wait.
    """
    code = qemu_proc.poll()
    if code is not None:
        raise Fail(f"QEMU exited before the guest was checked (exit code {code})")


def verify_serial(qemu_proc, serial_log: Path, marker: bytes, timeout: float) -> None:
    """Poll the serial log for the kernel's marker."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if serial_log.is_file():
            data = serial_log.read_bytes()
            if marker in data:
                return
        check_qemu_alive(qemu_proc)
        time.sleep(0.05)
    raise Fail(f"serial output did not contain {marker!r}")


def _qmp_execute(fp, name: str, arguments=None):
    payload = {"execute": name}
    if arguments is not None:
        payload["arguments"] = arguments
    fp.write(json.dumps(payload).encode("utf-8") + b"\r\n")
    while True:
        line = fp.readline()
        if not line:
            raise Fail("QMP connection closed")
        msg = json.loads(line.decode("utf-8"))
        if "error" in msg:
            raise Fail(f"QMP {name} failed: {msg['error']}")
        if "return" in msg:
            return msg["return"]


def verify_qmp_vga(qemu_proc, qmp_path: Path, dump_path: Path, serial_log: Path,
                   vga_expect: bytes, serial_marker: bytes) -> None:
    """Drive QEMU over QMP: resume, snapshot VGA memory, check the cell.

    The kernel writes a character and its attribute byte into text-mode VGA
    memory at 0xb8000, which no serial log can show -- so this reads the guest's
    physical memory directly instead of inferring from output.
    """
    deadline = time.time() + 10.0
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    while True:
        try:
            sock.connect(str(qmp_path))
            break
        except FileNotFoundError:
            check_qemu_alive(qemu_proc)
            if time.time() >= deadline:
                raise Fail("QMP socket did not appear")
            time.sleep(0.05)
        except ConnectionRefusedError:
            check_qemu_alive(qemu_proc)
            if time.time() >= deadline:
                raise Fail("QMP socket refused connections")
            time.sleep(0.05)

    fp = sock.makefile("rwb", buffering=0)
    if not fp.readline():
        raise Fail("QMP greeting missing")
    _qmp_execute(fp, "qmp_capabilities")
    _qmp_execute(fp, "cont")
    time.sleep(0.25)
    _qmp_execute(fp, "stop")
    # Native QMP pmemsave, not human-monitor-command. The HMP form parses its
    # size argument with the monitor's expression evaluator, which reads the
    # leading '/' of an absolute path as a division operator and rejects the
    # command ("invalid char 't' in expression" for /tmp/...). HMP also reports
    # that failure in its return text rather than as a QMP error, so the caller
    # sees success and only the missing dump file gives it away. The typed
    # command takes the filename as a string and surfaces failures as errors.
    _qmp_execute(fp, "pmemsave",
                 {"val": 0xB8000, "size": len(vga_expect), "filename": str(dump_path)})
    try:
        _qmp_execute(fp, "quit")
    except Exception:
        pass

    deadline = time.time() + 5.0
    while not dump_path.exists():
        if time.time() >= deadline:
            raise Fail("VGA memory dump was not created")
        time.sleep(0.05)
    data = dump_path.read_bytes()
    if data != vga_expect:
        raise Fail(f"unexpected VGA bytes: {data.hex()}")

    deadline = time.time() + 5.0
    serial = b""
    while time.time() < deadline:
        if serial_log.is_file():
            serial = serial_log.read_bytes()
            if serial_marker in serial:
                return
        time.sleep(0.05)
    raise Fail(f"serial output did not contain {serial_marker!r}: {serial!r}")


def run_target(target: Target, xray: Path, ws: workspace.Workspace) -> None:
    fixture_dir = FIXTURE_ROOT / target.name
    if not fixture_dir.is_dir():
        raise Fail(f"fixture directory missing: {fixture_dir}")

    zig = resolve_tool("XRAY_ZIG", "zig")
    readelf = resolve_tool("LLVM_READELF", "llvm-readelf")
    llvm_nm = resolve_tool("LLVM_NM", "llvm-nm") if target.require_no_undefined else None

    # Stage the fixture into the workspace so the build runs on a private copy.
    # File names are preserved exactly: xray.toml references `main` and the
    # assembly unit's `sources` by name, so renaming them breaks the build.
    project = ws.subdir("project")
    shutil.copy2(fixture_dir / target.main_source, project / target.main_source)
    boot_src = None
    if target.boot_source:
        boot_src = project / target.boot_source
        shutil.copy2(fixture_dir / target.boot_source, boot_src)
    linker_script = project / target.linker_script
    shutil.copy2(fixture_dir / target.linker_script, linker_script)

    manifest_text = (fixture_dir / "xray.toml").read_text(encoding="utf-8")
    if target.hash_placeholder:
        # The manifest pins the boot source by content hash, so a silently
        # edited start.S fails the build instead of booting something else.
        digest = hashlib.sha256(boot_src.read_bytes()).hexdigest()
        manifest_text = manifest_text.replace(target.hash_placeholder, digest)
    platform.write_text_lf(project / "xray.toml", manifest_text)

    # 1. Xray -> C. --target is not decoration: it is the target the generated
    # C is compiled for two steps down, and passing it keeps Xray's own target
    # resolution inside the test instead of letting the build fall back to the
    # host triple, which on a 64-bit machine contradicts the 32-bit image the
    # x86 lane produces.
    gen_c = ws.path("generated.c")
    log: List[bytes] = []
    run_step([xray, "build", "--native", "--profile", "freestanding",
              "--target", target.build_target, "--toolchain", "zig", "--zig", zig,
              "--rebuild", "-c", "-o", gen_c, target.main_source],
             log, "freestanding build failed", cwd=project)

    includes = [f"-I{PROJECT_DIR / d}" for d in INCLUDE_DIRS]
    zig_env = os.environ.copy()
    zig_env.setdefault("ZIG_GLOBAL_CACHE_DIR", str(ws.path("zig-global-cache")))
    zig_env.setdefault("ZIG_LOCAL_CACHE_DIR", str(ws.path("zig-local-cache")))
    base = [zig, "cc", "-target", target.triple, *target.extra_cflags, *COMMON_CFLAGS]

    # 2. generated C -> object
    gen_obj = ws.path("generated.o")
    result = proc.run([*base, *target.generated_defines, *includes, "-c", gen_c, "-o", gen_obj],
                      env=zig_env, timeout=300)
    log.append(result.stdout + result.stderr)
    if not result.ok:
        raise Fail("generated C compile failed")

    objects = [gen_obj]
    # 3. boot translation unit -> object
    if boot_src is not None:
        boot_obj = ws.path("boot.o")
        result = proc.run([*base, *includes, "-c", boot_src, "-o", boot_obj],
                          env=zig_env, timeout=300)
        log.append(result.stdout + result.stderr)
        if not result.ok:
            raise Fail("boot source compile failed")
        objects.append(boot_obj)

    # 4. link
    elf = ws.path("kernel.elf")
    result = proc.run([*base, "-nostdlib", f"-Wl,-T,{linker_script}", "-Wl,--gc-sections",
                       "-o", elf, *objects],
                      env=zig_env, timeout=300)
    log.append(result.stdout + result.stderr)
    if not result.ok:
        raise Fail("ELF link failed")

    # 5. ELF shape
    header = readelf_dump(readelf, "-h", elf, ws.path("elf.header"), "llvm-readelf -h failed")
    check_contains(header, target.elf_header_contains, "ELF header")
    for needle in target.elf_header_absent:
        if needle in header:
            raise Fail(f"ELF header must not contain {needle!r}")
    if target.elf_sections_contains:
        sections = readelf_dump(readelf, "-S", elf, ws.path("elf.sections"),
                                "llvm-readelf -S failed")
        check_contains(sections, target.elf_sections_contains, "ELF sections")
    if target.elf_symbols_contains:
        symbols = readelf_dump(readelf, "-s", elf, ws.path("elf.symbols"),
                               "llvm-readelf -s failed")
        check_contains(symbols, target.elf_symbols_contains, "ELF symbols")
    if target.elf_attributes_contains:
        attrs = readelf_dump(readelf, "-A", elf, ws.path("elf.attributes"),
                             "llvm-readelf -A failed")
        check_contains(attrs, target.elf_attributes_contains, "ELF attributes")
    if llvm_nm is not None:
        result = proc.run([llvm_nm, "-u", elf], timeout=120)
        if not result.ok:
            raise Fail("llvm-nm failed")
        if result.stdout.strip():
            raise Fail("ELF has undefined symbols: "
                       + result.stdout.decode("utf-8", "replace").strip())

    # 6. boot under QEMU
    qemu = resolve_tool(target.qemu_env_var, target.qemu_binary)
    serial_log = ws.path("serial.log")
    qemu_log = ws.path("qemu.log")

    if target.verify == "qmp_vga":
        qmp = ws.path("qmp.sock")
        argv = [qemu, *target.machine_args, "-kernel", elf, "-display", "none",
                "-no-reboot", "-no-shutdown", "-serial", f"file:{serial_log}",
                "-monitor", "none", "-S",
                "-qmp", f"unix:{qmp},server=on,wait=off"]
    else:
        argv = [qemu, *target.machine_args, "-kernel", elf, "-display", "none",
                "-serial", f"file:{serial_log}", "-monitor", "none",
                "-no-reboot", "-no-shutdown"]

    import subprocess

    with qemu_log.open("wb") as logf:
        qemu_proc = subprocess.Popen([str(a) for a in argv], stdout=logf, stderr=logf,
                                     stdin=subprocess.DEVNULL,
                                     start_new_session=(os.name != "nt"))
    try:
        if target.verify == "qmp_vga":
            verify_qmp_vga(qemu_proc, qmp, ws.path("vga.bin"), serial_log,
                           target.vga_expect, target.serial_marker)
        else:
            verify_serial(qemu_proc, serial_log, target.serial_marker, timeout=2.0)
    except Fail:
        # Surface what the guest actually produced; a bare assertion message is
        # not enough to tell a build problem from a boot problem.
        sys.stderr.write("QEMU log:\n")
        for line in qemu_log.read_bytes().decode("utf-8", "replace").splitlines():
            sys.stderr.write(f"  {line}\n")
        if serial_log.is_file():
            sys.stderr.write("Serial log:\n")
            for line in serial_log.read_bytes().decode("utf-8", "replace").splitlines():
                sys.stderr.write(f"  {line}\n")
        raise
    finally:
        if qemu_proc.poll() is None:
            qemu_proc.kill()
            qemu_proc.wait()


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(description="Freestanding QEMU boot smoke")
    ap.add_argument("--target", required=True, choices=sorted(TARGETS))
    ap.add_argument("--xray", default=None)
    ap.add_argument("xray_positional", nargs="?", default=None)
    ns = ap.parse_args(argv[1:])

    xray_raw = ns.xray or ns.xray_positional or os.environ.get("XRAY_BIN")
    if not xray_raw:
        build_dir = os.environ.get("XRAY_BUILD_DIR")
        xray_raw = str(Path(build_dir) / "xray") if build_dir else str(PROJECT_DIR / "build" / "xray")
    # Absolute, because the build step runs with cwd set to the staged project
    # directory: a relative --xray resolves against the caller's cwd when it is
    # checked here and against the staging directory when it is executed.
    xray = Path(xray_raw).resolve()

    target = TARGETS[ns.target]
    required = platform.env_flag("XRAY_FREESTANDING_QEMU_REQUIRED")

    try:
        if not (xray.is_file() and os.access(xray, os.X_OK)):
            raise Skip(f"xray binary not found: {xray}")
        with workspace.Workspace(f"xray_freestanding_{target.name}") as ws:
            run_target(target, xray, ws)
    except Skip as exc:
        if required:
            sys.stderr.write(f"FAIL: required freestanding QEMU smoke dependency missing: {exc}\n")
            return 1
        print(f"SKIP: {exc}")
        return SKIP_EXIT
    except Fail as exc:
        sys.stderr.write(f"FAIL: {exc}\n")
        return 1

    print(target.pass_message)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

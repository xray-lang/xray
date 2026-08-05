#!/usr/bin/env python3
"""A bytecode-embedding host links the VM, and only the VM.

An application that ships precompiled `.xrc` bytecode needs the runtime, never
the compiler. If the parser, analyzer, Xi pipeline or AOT backend reach the
link, every such application carries a compiler it cannot use -- so this gate
checks both the example host and the shipped `xray_vm_runtime` archive for
toolchain symbols, and runs the host to prove the runtime half still works.

Usage:
    run_bytecode_embed_symbol_tests.py <runner> <libxray_vm_runtime.a>
                                       <bytecode.xrc> <expected-line>
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import binary, platform, proc  # noqa: E402

# Any of these in a bytecode-embedding host means the compiler came along.
#
# `xr_aot_` is deliberately NOT here, though it reads like it belongs. It names
# the runtime ABI that AOT-generated code CALLS -- atomics, semaphores,
# countdown latches, frame alloc -- defined in src/coro/xaot_runtime.c and
# src/aot/xrt_*.c, all of which are runtime, not toolchain. Forbidding it made
# this gate fail on every build that supports coroutines. The AOT backend
# itself is the Xi code generator, and `xi_` already covers it.
TOOLCHAIN_SYMBOL = re.compile(
    r"(^|\s)_?(xr_parse|xr_compile|xa_analyzer|xanalyzer_|xi_|"
    r"xray_build|xr_bundle|xr_module_graph)")

USAGE = ("usage: run_bytecode_embed_symbol_tests.py <bytecode-embed-runner> "
         "<libxray_vm_runtime.a> <bytecode.xrc> <expected-line>\n")


class Recorder:
    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0

    def ok(self, name: str) -> None:
        print(f"  PASS: {name}")
        self.passed += 1

    def bad(self, name: str, detail: str = "") -> None:
        print(f"  FAIL: {name}")
        self.failed += 1
        if detail:
            print(detail, end="" if detail.endswith("\n") else "\n")


def check_no_symbols(rec: Recorder, path: Path, label: str,
                     timeout: float | None) -> None:
    lines = binary.symbols(path, global_only=True, timeout=timeout)
    offenders = [line for line in (lines or []) if TOOLCHAIN_SYMBOL.search(line)]
    if offenders:
        rec.bad(f"{label} has compiler/toolchain symbols",
                "".join(f"    {line}\n" for line in offenders))
    else:
        rec.ok(f"{label} has no compiler/toolchain symbols")


def main(argv: list[str]) -> int:
    if len(argv) < 5 or not all(argv[1:5]):
        sys.stderr.write(USAGE)
        return 2
    runner, archive, bytecode, expected = (Path(argv[1]), Path(argv[2]),
                                           Path(argv[3]), argv[4])

    if not (runner.is_file() and os.access(runner, os.X_OK)):
        sys.stderr.write(f"runner not executable: {runner}\n")
        return 2
    if not archive.is_file():
        sys.stderr.write(f"archive not found: {archive}\n")
        return 2
    if not bytecode.is_file():
        sys.stderr.write(f"bytecode not found: {bytecode}\n")
        return 2

    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300)

    print("=== Bytecode Embed Symbol Gate ===")
    print(f"Runner:  {runner}")
    print(f"Archive: {archive}")
    print("")
    print(f"Bytecode: {bytecode}")
    print("")

    rec = Recorder()
    result = proc.run([runner, bytecode, expected], timeout=timeout)
    if result.ok:
        rec.ok("bytecode embed runner executes")
    else:
        detail = "".join(
            f"    stdout: {line}\n"
            for line in result.stdout.decode("utf-8", "replace").splitlines())
        detail += "".join(
            f"    stderr: {line}\n"
            for line in result.stderr.decode("utf-8", "replace").splitlines())
        detail += f"    exit: {result.returncode}\n"
        rec.bad("bytecode embed runner executes", detail)

    check_no_symbols(rec, runner, "bytecode embed runner", timeout)
    check_no_symbols(rec, archive, "xray_vm_runtime archive", timeout)

    print("")
    print(f"=== Results: {rec.passed} passed, {rec.failed} failed ===")
    return 1 if rec.failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

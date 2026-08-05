#!/usr/bin/env python3
"""Concurrency memory-model litmus tests.

Each case runs a classic shared-memory shape many times and prints
"<name> forbidden=<count>" as its last line. A non-zero count is a memory-model
violation.

The AOT backend is the one that matters. The VM is an interpreter and does not
reorder, so a litmus run against it alone proves nothing about the memory
model; every failure mode these cases exist to catch lives in the Xi optimiser,
which only runs from -O2 (XI_OPT_FULL). The VM lane is kept as a control: if a
case fails there too, the bug is in the runtime or the test, not in code motion.

Environment:
    XRAY_BIN                xray binary (default: build/xray)
    XRAY_LITMUS_BACKENDS    comma list subset of vm,aot (default: vm,aot)
    XRAY_LITMUS_OPT         AOT optimization level (default: 2)
    XRAY_LITMUS_TIMEOUT     per-case seconds (default: 300)

Usage: run_litmus.py [xray_binary]
"""

from __future__ import annotations

import os
import sys
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, workspace  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent

MARKER = "forbidden="


def combined(result) -> str:
    """These cases are judged on their whole output, as the shell's 2>&1 was."""
    return result.combined_text()


def run_vm(xray: Path, case: Path, work: Path, opt: str,
           timeout: float | None) -> tuple[bool, str]:
    result = proc.run([xray, "run", case], timeout=timeout)
    if not result.ok:
        why = "timed out" if result.timed_out else "run failed"
        return False, f"! vm {why}: {combined(result)}"
    return True, combined(result)


def run_aot(xray: Path, case: Path, work: Path, opt: str,
            timeout: float | None) -> tuple[bool, str]:
    binary = work / (case.stem + ".aot")
    build = proc.run([xray, "build", "--native", "-O", opt, case, "-o", binary],
                     timeout=timeout)
    if not build.ok:
        why = "timed out" if build.timed_out else "build failed"
        return False, f"! aot {why}: {combined(build)}"
    result = proc.run([binary], timeout=timeout)
    if not result.ok:
        why = "timed out" if result.timed_out else "run failed"
        return False, f"! aot {why}: {combined(result)}"
    return True, combined(result)


BACKENDS = {"vm": run_vm, "aot": run_aot}


def verdict(output: str) -> str | None:
    """None when the case is clean, else the reason it is not."""
    lines = output.splitlines()
    summary = lines[-1] if lines else ""
    if MARKER not in summary:
        return f"no {MARKER} summary; got: {summary}"
    count = summary.rsplit(MARKER, 1)[1]
    if count != "0":
        return f"{count} forbidden observations"
    return None


def main(argv: list[str]) -> int:
    xray = Path(argv[1] if len(argv) > 1
                else os.environ.get("XRAY_BIN", str(REPO_ROOT / "build" / "xray")))
    enabled = [b for b in os.environ.get("XRAY_LITMUS_BACKENDS", "vm,aot").split(",")
               if b in BACKENDS]
    opt = os.environ.get("XRAY_LITMUS_OPT", "2")
    timeout = platform.env_timeout("XRAY_LITMUS_TIMEOUT", 300)

    if not (xray.is_file() and os.access(xray, os.X_OK)):
        sys.stderr.write(
            f"run_litmus: xray binary not found or not executable: {xray}\n")
        return 1

    failures = 0
    total = 0
    with workspace.Workspace("xray_litmus") as ws:
        for case in sorted(SCRIPT_DIR.glob("*.xr")):
            for backend in ("vm", "aot"):
                if backend not in enabled:
                    continue
                total += 1
                sys.stdout.write(f"  {case.stem:<32} {backend:<4} ")
                sys.stdout.flush()
                ok, output = BACKENDS[backend](xray, case, ws.root, opt, timeout)
                if not ok:
                    print("FAIL")
                    print(f"        {output}")
                    failures += 1
                    continue
                problem = verdict(output)
                if problem:
                    print(f"FAIL ({problem})")
                    failures += 1
                else:
                    print("PASS")

    print("")
    print(f"AOT opt: -O{opt}")
    print(f"=== Litmus: {total - failures} passed, {failures} failed ===")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

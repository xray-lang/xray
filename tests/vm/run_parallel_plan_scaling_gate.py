#!/usr/bin/env python3
"""Parallel-plan VM scaling gate: every lane runs, and the result is exact.

Runs the baseline case with an explicit worker count and requires the exact
output. Scaling bugs show up as missing or duplicated lines rather than as a
crash, so the comparison is against a full expected string, not a substring.

Two deliberate normalizations, both about the host rather than the program:

  - `[sysmon]` lines are dropped. This gate validates lane scaling and exact
    observable results, not the scheduler watchdog: a correct lane can spend
    >100 ms in a non-yielding CPU callback on a loaded or sanitizer host and
    trip the warn-only diagnostic while every lane still completes. Any other
    stderr output remains a hard failure, and the dedicated scheduler tests
    keep strict sysmon coverage.
  - CR is stripped. The Windows console provider writes CRLF where Unix writes
    LF; this gate compares logical lines, and task 257 owns codec convergence.

Usage: run_parallel_plan_scaling_gate.py [xray]
"""

from __future__ import annotations

import os
import sys
from pathlib import Path
from typing import List


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, workspace  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent

SYSMON_MARKER = "[sysmon]"


class Recorder:
    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0

    def ok(self, name: str) -> None:
        print(f"  PASS: {name}")
        self.passed += 1

    def bad(self, name: str, stdout: str, stderr: str) -> None:
        print(f"  FAIL: {name}")
        self.failed += 1
        for line in stdout.splitlines()[:40]:
            print(f"      stdout: {line}")
        for line in stderr.splitlines()[:40]:
            print(f"      stderr: {line}")


def expect_output_workers(rec: Recorder, xray: Path, name: str, source: Path,
                          expected: str, workers: int,
                          timeout: "float | None") -> None:
    result = proc.run([xray, "run", "--workers", str(workers), source], timeout=timeout)
    stdout = result.stdout.decode("utf-8", "replace").replace("\r", "")
    stderr = result.stderr.decode("utf-8", "replace")
    effective_stderr = "\n".join(
        line for line in stderr.splitlines() if SYSMON_MARKER not in line).strip()

    if result.ok and stdout.strip() == expected and not effective_stderr:
        rec.ok(f"{name} output")
    else:
        rec.bad(f"{name} output", stdout, stderr)


def main(argv: List[str]) -> int:
    xray = Path(argv[1] if len(argv) > 1
                else os.environ.get("XRAY_BIN", str(PROJECT_DIR / "build" / "xray")))
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300)

    print("=== Parallel Plan VM Scaling Gate ===")
    print(f"Binary: {xray}")
    print("")

    if not (xray.is_file() and os.access(xray, os.X_OK)):
        sys.stderr.write(f"FAIL: xray binary not executable: {xray}\n")
        return 1

    rec = Recorder()
    with workspace.Workspace("xray_parallel_plan_scaling"):
        expect_output_workers(
            rec, xray,
            "parallel_plan_vm_scaling_baseline",
            SCRIPT_DIR / "parallel_plan_vm_scaling_baseline.xr",
            "\n".join(["true"] * 8),
            8, timeout,
        )

    print("")
    print(f"Summary: {rec.passed} passed, {rec.failed} failed")
    return 1 if rec.failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

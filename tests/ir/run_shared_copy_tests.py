#!/usr/bin/env python3
"""Copying into shared storage lowers to a direct COPY, not a TO_SHARED wrapper.

Three assertions on one program: it prints the right value, its bytecode copies
straight into the shared register, and no TO_SHARED wrapper survives. The third
is the point -- a wrapper would still produce the right answer while adding a
per-copy allocation, so only the bytecode shape can tell the optimization apart
from its absence.

Usage: run_shared_copy_tests.py [xray]
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
from xraytest import platform, proc, workspace  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent
SOURCE = SCRIPT_DIR / "shared_copy" / "shared_copy_to_shared_bytecode.xr"

EXPECTED_OUTPUT = "2"

# COPY whose destination is R[1], the shared slot. Anchored per line: the dump
# is many lines and an unanchored match could pick up an unrelated operand.
SHARED_COPY_RE = re.compile(
    r"^[0-9]+.*\sCOPY\s+R\[[0-9]+\]\s+R\[[0-9]+\]\s+R\[1\](\s|$)", re.MULTILINE)
TO_SHARED_RE = re.compile(r"^[0-9]+.*\sTO_SHARED\s", re.MULTILINE)


class Recorder:
    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0

    def ok(self, name: str) -> None:
        print(f"  PASS: {name}")
        self.passed += 1

    def bad(self, name: str, detail: str = "", limit: int = 40) -> None:
        print(f"  FAIL: {name}")
        self.failed += 1
        for line in detail.splitlines()[:limit]:
            print(f"      {line}")


def main(argv: list[str]) -> int:
    xray = Path(argv[1] if len(argv) > 1
                else os.environ.get("XRAY_BIN", str(PROJECT_DIR / "build" / "xray")))
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300)

    print("=== Xi Shared Copy Tests ===")
    print(f"Binary: {xray}")
    print("")

    if not (xray.is_file() and os.access(xray, os.X_OK)):
        sys.stderr.write(f"FAIL: xray binary not executable: {xray}\n")
        return 1

    rec = Recorder()
    with workspace.Workspace("xray_shared_copy"):
        run = proc.run([xray, SOURCE], timeout=timeout)
        if run.stdout.decode("utf-8", "replace").strip() == EXPECTED_OUTPUT:
            rec.ok("const copy: program output")
        else:
            rec.bad("const copy: unexpected output",
                    "stdout: " + run.stdout.decode("utf-8", "replace") +
                    "\nstderr: " + run.stderr.decode("utf-8", "replace"))

        dump = proc.run([xray, "run", "--dump-bytecode", SOURCE], timeout=timeout)
        text = dump.combined_text()

        if SHARED_COPY_RE.search(text):
            rec.ok("const copy: bytecode targets shared storage")
        else:
            rec.bad("const copy: missing shared-target COPY bytecode", text, 120)

        if TO_SHARED_RE.search(text):
            rec.bad("const copy: bytecode still wraps copy in TO_SHARED", text, 120)
        else:
            rec.ok("const copy: bytecode avoids TO_SHARED wrapper")

    print("")
    print(f"Passed: {rec.passed}")
    print(f"Failed: {rec.failed}")
    return 1 if rec.failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

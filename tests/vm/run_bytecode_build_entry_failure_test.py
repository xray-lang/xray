#!/usr/bin/env python3
"""A failed entry module must fail the bundle instead of emitting dependencies only."""

from __future__ import annotations

import os
from pathlib import Path
import sys


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, workspace  # noqa: E402


SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent


def main(argv: list[str]) -> int:
    xray = Path(argv[1] if len(argv) > 1 else PROJECT_DIR / "build" / "xray")
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 60)
    with workspace.Workspace("xray_bytecode_build_entry_failure") as work:
        suffix = ".exe" if os.name == "nt" else ""
        binary = work.path(f"invalid-entry{suffix}")
        build = proc.run(
            [xray, "build", "-o", binary, SCRIPT_DIR / "bytecode_build_entry_failure.xr"],
            cwd=PROJECT_DIR,
            timeout=timeout,
        )
        if build.ok:
            sys.stderr.write("default bytecode build unexpectedly accepted an invalid entry\n")
            sys.stderr.write(build.combined_text())
            return 1
        if binary.exists():
            sys.stderr.write(f"failed bytecode build left an executable behind: {binary}\n")
            return 1
        if "compilation failed:" not in build.combined_text():
            sys.stderr.write("failed bytecode build did not report the entry failure\n")
            sys.stderr.write(build.combined_text())
            return 1
    print("bytecode build entry failure: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

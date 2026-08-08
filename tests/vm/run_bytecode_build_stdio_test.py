#!/usr/bin/env python3
"""Bytecode executables must publish stdout before the process exits."""

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
    with workspace.Workspace("xray_bytecode_build_stdio") as work:
        suffix = ".exe" if os.name == "nt" else ""
        binary = work.path(f"stdio{suffix}")
        build = proc.run(
            [xray, "build", "-O", "2", "-o", binary, SCRIPT_DIR / "bytecode_build_stdio.xr"],
            cwd=PROJECT_DIR,
            timeout=timeout,
        )
        if not build.ok:
            sys.stderr.write(build.combined_text())
            return 1

        result = proc.run([binary], cwd=PROJECT_DIR, timeout=1)
        if (
            result.timed_out
            and result.stdout_text().strip() == "BYTECODE_BUILD_READY"
            and not result.stderr
        ):
            print("bytecode build stdio: PASS")
            return 0
        sys.stderr.write(result.combined_text())
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

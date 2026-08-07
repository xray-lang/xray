#!/usr/bin/env python3
"""Default bytecode executables must let the entry plan start the scheduler."""

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
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300)
    with workspace.Workspace("xray_bytecode_build_scheduler") as work:
        suffix = ".exe" if os.name == "nt" else ""
        binary = work.path(f"scheduler{suffix}")
        build = proc.run(
            [xray, "build", "-O", "2", "-o", binary, SCRIPT_DIR / "bytecode_build_scheduler.xr"],
            cwd=PROJECT_DIR,
            timeout=timeout,
        )
        if not build.ok:
            sys.stderr.write(build.combined_text())
            return 1

        env = os.environ.copy()
        env["XRAY_WORKERS"] = "4"
        result = proc.run([binary], cwd=PROJECT_DIR, env=env, timeout=timeout)
        if result.ok and result.stdout_text().strip() == "true" and not result.stderr.strip():
            print("bytecode build scheduler: PASS")
            return 0
        sys.stderr.write(result.combined_text())
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

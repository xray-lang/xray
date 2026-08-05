#!/usr/bin/env python3
"""Xray's ordinary struct layout must equal the host C ABI, byte for byte.

Two probes compute the same table of offsets and sizes -- one compiled by the
host C compiler, one run by Xray -- and their stdout must match exactly. The C
side is the oracle precisely because it is not ours: it is whatever the platform
ABI actually says, so agreement means Xray structs can cross an FFI boundary
unchanged.

Usage: run_extern_layout_probe.py [xray]
"""

from __future__ import annotations

import difflib
import os
import sys
from pathlib import Path
from typing import List


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, toolchain, workspace  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent

C_PROBE = SCRIPT_DIR / "extern_layout_probe.c"
XRAY_PROBE = PROJECT_DIR / "tests/diff/cases/semantics/ffi/extern_layout_introspection.xr"


def main(argv: List[str]) -> int:
    xray = Path(argv[1] if len(argv) > 1
                else os.environ.get("XRAY_BIN", str(PROJECT_DIR / "build" / "xray")))
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300)

    if not (xray.is_file() and os.access(xray, os.X_OK)):
        sys.stderr.write(f"FAIL: xray binary not executable: {xray}\n")
        return 1

    cc = toolchain.find_c_compiler()
    if cc is None:
        sys.stderr.write("FAIL: C compiler not found\n")
        return 1

    with workspace.Workspace("xray_extern_layout") as ws:
        c_probe = ws.path("c_probe")
        built = proc.run(
            [cc.path, "-std=c11", "-Wall", "-Wextra", "-Werror", C_PROBE, "-o", c_probe],
            timeout=timeout)
        if not built.ok:
            sys.stderr.write("FAIL: C layout probe did not compile\n")
            sys.stderr.write(built.combined_text())
            return 1

        c_run = proc.run([c_probe], timeout=timeout)
        if not c_run.ok:
            sys.stderr.write("FAIL: C layout probe did not run\n")
            sys.stderr.write(c_run.combined_text())
            return 1

        xray_run = proc.run([xray, "run", XRAY_PROBE], timeout=timeout)
        if not xray_run.ok:
            sys.stderr.write("FAIL: Xray layout probe did not run\n")
            for line in xray_run.stderr.decode("utf-8", "replace").splitlines():
                sys.stderr.write(f"    {line}\n")
            return 1

        if c_run.stdout != xray_run.stdout:
            sys.stderr.write("FAIL: Xray ordinary layout differs from the host C ABI\n")
            diff = difflib.unified_diff(
                c_run.stdout.decode("utf-8", "replace").splitlines(),
                xray_run.stdout.decode("utf-8", "replace").splitlines(),
                fromfile="c.out", tofile="xray.out", lineterm="")
            for line in diff:
                sys.stderr.write(line + "\n")
            return 1

    print("PASS: Xray ordinary layout matches the host C ABI")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

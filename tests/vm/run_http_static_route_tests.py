#!/usr/bin/env python3
"""HTTP static-route parsing must behave identically in the VM and in AOT.

Runs the VM test file, then builds the AOT entry natively and runs it. Both
halves matter: the route table is built at startup, so a VM-only pass would not
prove the natively compiled form initializes it the same way.

Usage: run_http_static_route_tests.py <xray>
"""

from __future__ import annotations

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

VM_CASE = SCRIPT_DIR / "http_static_route_full_parse.xr"
AOT_ENTRY = SCRIPT_DIR / "http_static_route_full_parse_main.xr"


def main(argv: List[str]) -> int:
    if len(argv) < 2:
        sys.stderr.write(f"usage: {argv[0]} /path/to/xray\n")
        return 2

    xray = Path(argv[1])
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 600)

    vm = proc.run([xray, "test", VM_CASE], timeout=timeout)
    sys.stdout.write(vm.combined_text())
    if not vm.ok:
        return 1

    with workspace.Workspace("xray-http-static-route-aot") as ws:
        binary = ws.path("http_static_route_full_parse")
        built = proc.run(
            [xray, "build", "--native", "--cache-dir", ws.path("cache"),
             "-o", binary, AOT_ENTRY],
            timeout=timeout,
        )
        sys.stdout.write(built.combined_text())
        if not built.ok:
            return 1

        run = proc.run([binary], timeout=timeout)
        sys.stdout.write(run.combined_text())
        if not run.ok:
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

#!/usr/bin/env python3
"""Contract check: the differential runner drives the CLI a user actually has.

The runner builds the native side with `xray build --native` and runs the VM side
with `xray run`. If either invocation names an option the product no longer
accepts, every case is refused for a reason that has nothing to do with the
backends -- and because a refusal is not a divergence, the full lane still exits
green. That is how the whole corpus went unmeasured while the CLI changed under
it.

This check is deliberately small (two programs, one process, a few seconds) and
end to end: it runs the real runner against the real binary over cases that must
be comparable. A refusal here is a hard failure, and the console names the
command that was refused.

Usage: check_diff_cli_contract.py <xray-binary>
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent

# Small enough to build fast, wide enough to exercise the two forms: one
# constant-printing program and one with a call and a loop.
CASES = {
    "contract/hello.xr": 'print("hello")\n',
    "contract/arith.xr": (
        "fn sum(n: i64) -> i64 {\n"
        "    var total = 0\n"
        "    for (i in 0..n) { total = total + i }\n"
        "    return total\n"
        "}\n"
        "print(sum(5))\n"
    ),
}


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        sys.stderr.write("usage: check_diff_cli_contract.py <xray-binary>\n")
        return 2
    xray = Path(argv[1]).resolve()
    if not (xray.is_file() and os.access(xray, os.X_OK)):
        sys.stderr.write(f"xray binary is missing or not executable: {xray}\n")
        return 1
    with tempfile.TemporaryDirectory(prefix="xray-diff-contract-") as temporary:
        root = Path(temporary)
        manifest = root / "cases.txt"
        lines: list[str] = []
        for name, text in CASES.items():
            case = root / name
            case.parent.mkdir(parents=True, exist_ok=True)
            case.write_text(text, encoding="utf-8")
            lines.append(str(case))
        manifest.write_text("\n".join(lines) + "\n", encoding="utf-8")
        environment = dict(os.environ)
        environment.update(
            {
                "XRAY_DIFF_CASES_FILE": str(manifest),
                "XRAY_DIFF_BASELINE": str(root / "known_failures.txt"),
                "XRAY_DIFF_BIN_CACHE_DIR": str(root / "bin"),
                "XRAY_DIFF_JOBS": "1",
                "XRAY_TEST_CASE_TIMEOUT": "120",
            }
        )
        environment.pop("XRAY_DIFF_BACKENDS", None)
        result = subprocess.run(
            [sys.executable, str(SCRIPT_DIR / "run_backend_diff.py"), str(xray)],
            cwd=str(SCRIPT_DIR.parent.parent),
            env=environment,
            capture_output=True,
            text=True,
            timeout=600,
        )
        output = result.stdout + result.stderr
    if result.returncode != 0:
        print(output)
        print("FAIL: the differential runner could not compare two trivial cases")
        print("      A refusal means its build or run command no longer matches the CLI.")
        return 1
    if "unknown option" in output:
        print(output)
        print("FAIL: the runner passed an option the CLI rejected")
        return 1
    if "2 passed" not in output:
        print(output)
        print("FAIL: expected both contract cases to be compared")
        return 1
    print("backend-diff contract: two trivial cases compare on VM and native: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

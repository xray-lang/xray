#!/usr/bin/env python3
"""Retired source-string entry points stay closed; run uses a file authority.

The historical filename is retained because CTest owns it as a long-lived
regression entry. The product boundary is now explicit: neither `eval` nor its
`-e` alias may execute source, while the ordinary source compiler/runtime path
executes a file-backed source with the same independent output oracle.

Usage: run_eval_stdlib_overlay_tests.py [xray]
"""

from __future__ import annotations

import os
import sys
import tempfile
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent

def normalize(data: bytes) -> str:
    """Trailing whitespace trimmed and blank lines dropped, as the shell did."""
    lines = [line.rstrip() for line in data.decode("utf-8", "replace").splitlines()]
    return "\n".join(line for line in lines if line)


def main(argv: list[str]) -> int:
    xray = Path(argv[1] if len(argv) > 1
                else os.environ.get("XRAY", str(PROJECT_DIR / "build" / "xray")))
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300)

    residue = 'print("runtime-eval-residue")'
    for label, argv_tail in (
        ("eval", ["eval", residue]),
        ("eval_alias", ["-e", residue]),
    ):
        result = proc.run([xray, *argv_tail], cwd=PROJECT_DIR, timeout=timeout)
        if result.returncode == 0 or "unknown command" not in result.combined_text().lower():
            sys.stderr.write(f"FAIL {label}: expected unknown-command rejection\n"
                             f"{result.combined_text()}\n")
            return 1

    source = b'print("source-run-kept")\n'
    rejected = proc.run([xray, "run", "-"], cwd=PROJECT_DIR, stdin=source,
                        timeout=timeout)
    if (rejected.returncode != 1 or rejected.stdout or rejected.stderr !=
            b"XR_RUN_6011: canonical run requires a file-backed source authority\n"):
        sys.stderr.write("FAIL run_stdin: expected file-authority rejection\n"
                         f"{rejected.combined_text()}\n")
        return 1

    with tempfile.TemporaryDirectory(prefix="xray-source-authority-") as temporary:
        entry = Path(temporary) / "main.xr"
        entry.write_bytes(source)
        result = proc.run([xray, "run", entry], cwd=PROJECT_DIR, timeout=timeout)
        if (not result.ok or result.stdout != b"source-run-kept\n" or
                result.stderr or entry.read_bytes() != source):
            sys.stderr.write("FAIL run_file: ordinary source execution regressed\n"
                             f"{result.combined_text()}\n")
            return 1

    print("source eval cutover tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

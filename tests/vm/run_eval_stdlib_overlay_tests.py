#!/usr/bin/env python3
"""The retired source-string entry points stay closed while `run -` remains.

The historical filename is retained because CTest owns it as a long-lived
regression entry. The product boundary is now explicit: neither `eval` nor its
`-e` alias may execute source, while the ordinary source compiler/runtime path
continues to accept stdin.

Usage: run_eval_stdlib_overlay_tests.py [xray]
"""

from __future__ import annotations

import sys
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
    result = proc.run([xray, "run", "-"], cwd=PROJECT_DIR, stdin=source,
                      timeout=timeout)
    if not result.ok or normalize(result.stdout) != "source-run-kept":
        sys.stderr.write("FAIL run_stdin: ordinary source execution regressed\n"
                         f"{result.combined_text()}\n")
        return 1

    print("source eval cutover tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

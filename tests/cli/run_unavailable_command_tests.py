"""Assert that the debug adapter and the REPL still answer with their stub."""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path


def main(argv: list[str]) -> int:
    xray = Path(argv[1])
    command = argv[2]
    expected = f"xray {command}: temporarily unavailable"
    completed = subprocess.run(
        [str(xray), command], capture_output=True, text=True, timeout=30,
        encoding="utf-8", errors="strict")
    status = 0
    if completed.returncode != 1:
        print(f"{command}: expected exit 1, got {completed.returncode}")
        status = 1
    if completed.stdout:
        print(f"{command}: unexpected stdout: {completed.stdout!r}")
        status = 1
    if completed.stderr.strip() != expected:
        print(f"{command}: expected {expected!r}, got {completed.stderr.strip()!r}")
        status = 1
    if not status:
        print(f"{command}: stub contract holds")
    return status


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

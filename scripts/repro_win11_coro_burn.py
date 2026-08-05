#!/usr/bin/env python3
"""Burn-in driver for the coroutine teardown races found in May 2026.

The bug class is heap corruption on coroutine teardown, exercised by cancel /
await_any / explicit yield -- the scenarios that surfaced
STATUS_HEAP_CORRUPTION on Windows. The driver is not Windows-specific on
purpose: the underlying race is heap corruption, so it is equally useful on a
Linux/macOS sanitizer host during local triage.

Each case runs N times; N defaults to 5, matching nightly.yml.

Environment:
    XRAY_BIN  xray binary path (default: build/xray.exe, build-release, build)

Usage: repro_win11_coro_burn.py [N]
"""

from __future__ import annotations

import os
import sys
from pathlib import Path
from typing import List


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parent.parent / "tests" / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import proc  # noqa: E402

PROJECT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ROUNDS = 5
FAIL_TAIL_LINES = 30

CASES = (
    "tests/regression/11_coroutine/1115_cancel.xr",
    "tests/regression/11_coroutine/1109_await_any.xr",
    "tests/regression/11_coroutine/1128_yield.xr",
)

OUT_DIR = PROJECT_ROOT / "tests" / "tmp" / "win11_coro"
FAIL_LOG = OUT_DIR / "failures.log"


def find_xray() -> "Path | None":
    """Ninja is the project's one generator and is single-config, so a build
    tree has exactly one binary location -- no per-configuration subdirectory."""
    override = os.environ.get("XRAY_BIN")
    if override:
        candidate = Path(override)
        return candidate if candidate.is_file() else candidate
    for relative in ("build/xray.exe", "build-release/xray", "build/xray"):
        candidate = PROJECT_ROOT / relative
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def main(argv: List[str]) -> int:
    raw = argv[1] if len(argv) > 1 else str(DEFAULT_ROUNDS)
    if not raw.isdigit():
        sys.stderr.write(f"FAIL: N must be integer, got: {raw}\n")
        return 2
    rounds = int(raw)
    if rounds < 1:
        sys.stderr.write("FAIL: N must be >= 1\n")
        return 2

    xray = find_xray()
    if xray is None:
        sys.stderr.write("FAIL: xray binary not found; build first or set "
                         "XRAY_BIN\n")
        return 2
    if not (xray.is_file() and os.access(xray, os.X_OK)):
        sys.stderr.write(f"FAIL: xray binary not executable: {xray}\n")
        return 2

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    FAIL_LOG.write_text("", encoding="utf-8")

    passed = failed = 0
    print(f"win11-coro-burn: N={rounds} bin={xray}")
    print(f"win11-coro-burn: cases={len(CASES)}")

    with FAIL_LOG.open("a", encoding="utf-8", newline="\n") as log:
        for round_number in range(1, rounds + 1):
            print(f"==== round {round_number}/{rounds} ====")
            for relative in CASES:
                path = PROJECT_ROOT / relative
                if not path.is_file():
                    print(f"  SKIP: missing {path}")
                    continue
                # One run, captured. The shell re-ran a failing case to collect
                # its output -- which for a race this driver exists to catch is
                # the one thing that must not happen: the second run often
                # passes, and the log then describes a run that did not fail.
                result = proc.run([xray, "test", path])
                if result.ok:
                    passed += 1
                    continue
                failed += 1
                tail = result.combined_text().splitlines()[-FAIL_TAIL_LINES:]
                log.write(f"==== FAIL round={round_number} test={path} ====\n")
                log.write("\n".join(tail) + "\n\n")
                print(f"  FAIL: round={round_number} {path.name}")

    print("")
    print(f"win11-coro-burn: pass={passed} fail={failed} "
          f"(cases={len(CASES)} rounds={rounds})")
    if failed:
        print(f"win11-coro-burn: failure tails -> {FAIL_LOG}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

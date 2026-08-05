#!/usr/bin/env python3
"""The cycle detector must leave NO trace in a default build.

It is a compile-time switch, not a runtime flag: the production binary must
contain none of it, and a no-op stdlib entry point standing in for it is
explicitly forbidden (the spec was criticised once for a tracing-GC hook that
did nothing). Checking the symbol table is what makes that enforceable --
a build option can be flipped by accident, a missing symbol cannot.

Skips (exit 0) when the binary or a symbol reader is unavailable: this gate
proves an absence, and without a reader it has proven nothing either way.

Usage: check_cycle_detector_absent.py [binary]
"""

from __future__ import annotations

import sys
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parent.parent / "tests" / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import binary as binlib  # noqa: E402

PROJECT_DIR = Path(__file__).resolve().parent.parent
SYMBOL = "xr_cycle_detector"


def main(argv: list[str]) -> int:
    import os

    target = Path(argv[1]) if len(argv) > 1 else PROJECT_DIR / "build" / "xray"
    if not (target.is_file() and os.access(target, os.X_OK)):
        print(f"SKIP: binary not found: {target}")
        return 0

    lines = binlib.symbols(target)
    if lines is None:
        print("SKIP: no symbol reader available")
        return 0

    hits = [line for line in lines if SYMBOL in line]
    if hits:
        print(f"FAIL: {target} contains {len(hits)} cycle-detector symbol(s); "
              "it must be compiled out by default")
        for line in hits[:10]:
            print(line)
        return 1

    print(f"OK: no cycle-detector symbols in {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

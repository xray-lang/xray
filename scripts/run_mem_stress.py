#!/usr/bin/env python3
"""Memory-heavy regression burn-in for nightly CI.

Cycles through the memory-heavy regression files N times. Returns 0 only if
every round of every test passed.

On the old `mode` parameter from the 082 plan: Xray exposes reference counting
plus explicit cycle collection, not user-selectable collector modes. The
pragmatic stress amplifier is round-count -- sanitizers plus N rounds is the
mechanism that surfaced the May 2026 allocator bugs.

Environment:
    XRAY_BIN          xray binary path (default: build-release, then build)
    MEM_STRESS_ROUNDS default for the rounds argument (default: 10)

Usage: run_mem_stress.py [rounds]
"""

from __future__ import annotations

import os
import sys
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parent.parent / "tests" / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc  # noqa: E402

PROJECT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ROUNDS = 10
FAIL_TAIL_LINES = 30

# The interpreter is the canonical baseline for these.
MEM_TESTS = (
    "tests/regression/10_stdlib/1205_runtime_alloc_pressure.xr",
    "tests/regression/10_stdlib/1206_runtime_enhanced.xr",
    "tests/regression/10_stdlib/1207_runtime_stress.xr",
)

FAIL_LOG = PROJECT_ROOT / "tests" / "tmp" / "mem_stress_failures.log"


def find_xray() -> Path | None:
    override = os.environ.get("XRAY_BIN")
    if override:
        return Path(override)
    for relative in ("build-release/xray", "build/xray", "build/xray.exe"):
        candidate = PROJECT_ROOT / relative
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def main(argv: list[str]) -> int:
    raw = argv[1] if len(argv) > 1 else os.environ.get("MEM_STRESS_ROUNDS",
                                                       str(DEFAULT_ROUNDS))
    if not raw.isdigit():
        sys.stderr.write(f"FAIL: rounds must be integer, got: {raw}\n")
        return 2
    rounds = int(raw)
    if rounds < 1:
        sys.stderr.write("FAIL: rounds must be >= 1\n")
        return 2

    xray = find_xray()
    if xray is None:
        sys.stderr.write("FAIL: xray binary not found; build first or set "
                         "XRAY_BIN\n")
        return 2

    FAIL_LOG.parent.mkdir(parents=True, exist_ok=True)
    FAIL_LOG.write_text("", encoding="utf-8")

    passed = failed = 0
    print(f"mem-stress: rounds={rounds} bin={xray}")
    print(f"mem-stress: tests={len(MEM_TESTS)}")

    with FAIL_LOG.open("a", encoding="utf-8", newline="\n") as log:
        for round_number in range(1, rounds + 1):
            print(f"==== round {round_number}/{rounds} ====")
            for relative in MEM_TESTS:
                path = PROJECT_ROOT / relative
                if not path.is_file():
                    print(f"  SKIP: missing {path}")
                    continue
                # Captured on the first and only run. The shell ran a failing
                # test a second time to collect its output, which doubled the
                # cost and, on these tests especially, could log a tail from a
                # run that behaved differently than the one that was counted.
                result = proc.run([xray, "test", path])
                if result.ok:
                    passed += 1
                    continue
                failed += 1
                tail = result.combined_text().splitlines()[-FAIL_TAIL_LINES:]
                log.write(f"==== FAIL round={round_number} test={path} ====\n")
                log.write("\n".join(tail) + "\n\n")
                print(f"  FAIL: round={round_number} {path}")

    print("")
    print(f"mem-stress: pass={passed} fail={failed}")
    if failed:
        print(f"mem-stress: failure tails -> {FAIL_LOG}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

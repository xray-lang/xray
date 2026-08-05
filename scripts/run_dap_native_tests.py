#!/usr/bin/env python3
"""`xray dap --native`: source-level debugging of a `.xr` built to a native binary.

Drives the real lldb-dap through tests/regression/dap/native_driver.py: set a
breakpoint on a `.xr` line, hit it, and read a stack trace that points back at
the `.xr` source. Exits 77 (SKIP_RETURN_CODE) when the LLVM debug tools are
absent, so hosts without them do not fail.

The breakpoint line is read from a BREAKPOINT marker in the fixture rather than
hard-coded here. A hard-coded number silently retargets itself whenever the
fixture is edited, and the failure it produces then looks like a debug-info bug.

Environment overrides:
    XRAY_LLDB_DAP    path to lldb-dap (otherwise probed as the bridge does)
    XRAY_BUILD_DIR   build directory holding xray

Usage: run_dap_native_tests.py
"""

from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parent.parent / "tests" / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc  # noqa: E402

PROJECT_DIR = Path(__file__).resolve().parent.parent
FIXTURE_DIR = PROJECT_DIR / "tests" / "regression" / "dap" / "fixtures"
DRIVER = PROJECT_DIR / "tests" / "regression" / "dap" / "native_driver.py"

SKIP_EXIT = 77
MARKER = "// BREAKPOINT"

# Probed in the same order as the native bridge, so the test and the product
# agree on which adapter is in use.
LLDB_DAP_CANDIDATES: tuple[str, ...] = (
    "/opt/homebrew/opt/llvm/bin/lldb-dap",
    "/usr/local/opt/llvm/bin/lldb-dap",
    "/usr/bin/lldb-dap",
    "/usr/local/bin/lldb-dap",
    "/Library/Developer/CommandLineTools/usr/bin/lldb-dap",
)

CASES: tuple[str, ...] = ("native_bp.xr",)


def skip(reason: str) -> int:
    print(f"SKIP - {reason}")
    return SKIP_EXIT


def find_build_dir() -> Path:
    override = os.environ.get("XRAY_BUILD_DIR")
    if override:
        return Path(override)
    for name in ("build", "build-release"):
        if (PROJECT_DIR / name / platform.exe_name("xray")).is_file():
            return PROJECT_DIR / name
    return PROJECT_DIR / "build"


def find_lldb_dap() -> str | None:
    override = os.environ.get("XRAY_LLDB_DAP")
    if override and os.access(override, os.X_OK):
        return override
    for candidate in LLDB_DAP_CANDIDATES:
        if os.access(candidate, os.X_OK):
            return candidate
    return shutil.which("lldb-dap")


def has_debugserver() -> bool:
    """macOS needs debugserver, which ships only with full Xcode."""
    if sys.platform != "darwin":
        return True
    if shutil.which("debugserver"):
        return True
    return proc.run(["xcrun", "-f", "debugserver"], timeout=60).ok


def breakpoint_line(fixture: Path) -> int | None:
    for number, text in enumerate(
            fixture.read_text(encoding="utf-8").splitlines(), start=1):
        if MARKER in text:
            return number
    return None


def main(argv: list[str]) -> int:
    print("======================================")
    print("DAP Native Backend Tests")
    print("======================================")

    xray = find_build_dir() / platform.exe_name("xray")
    if not xray.is_file():
        return skip(f"xray binary not found at {xray} (build first)")
    if not has_debugserver():
        return skip("debugserver not found; install full Xcode or provide "
                    "debugserver to run native DAP tests")
    lldb_dap = find_lldb_dap()
    if not lldb_dap:
        return skip("lldb-dap not found (set XRAY_LLDB_DAP to enable native "
                    "DAP tests)")
    print(f"Using lldb-dap: {lldb_dap}")

    env = dict(os.environ)
    env["XRAY_LLDB_DAP"] = lldb_dap
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300)

    passed = failed = 0
    for name in CASES:
        fixture = FIXTURE_DIR / name
        line = breakpoint_line(fixture)
        if line is None:
            print(f"[{name:<28}] FAIL: no {MARKER!r} marker in {fixture}")
            failed += 1
            continue
        print(f"[{name:<28}] line {line} ... ")
        code = proc.run_passthrough(
            [sys.executable, DRIVER, xray, fixture, str(line)],
            env=env, timeout=timeout)
        if code == 0:
            passed += 1
        else:
            failed += 1

    print("")
    print("======================================")
    print(f"Summary: pass={passed} fail={failed}")
    print("======================================")
    if failed:
        print("FAILED")
        return 1
    print("ALL PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

#!/usr/bin/env python3
"""Run a generated pure-AOT executable and reject compiler/runtime residue."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TEST_LIB = ROOT / "tests" / "lib"
if str(TEST_LIB) not in sys.path:
    sys.path.insert(0, str(TEST_LIB))
from xraytest import toolchain  # noqa: E402


FORBIDDEN = (
    r"xr_program_(?:validate|write|decode)",
    r"xr_backend_ir",
    r"xr_vm_",
    r"xvm_",
    r"xaot_",
    r"xr_target_plan",
    r"xr_core_ir_",
    r"xi_(?:lower|pipeline|cgen)",
)


def forbidden_symbol_family(symbol_text: str) -> str | None:
    # Inventories contain normalized symbol names, not object filenames. Match
    # compiler prefixes at symbol boundaries, preserving C/COFF decoration and
    # C++ or unwind prefixes without rejecting source names containing a token.
    for pattern in FORBIDDEN:
        if re.search(r"(?<![A-Za-z0-9_])(?:__imp_)?_*" + pattern, symbol_text, re.IGNORECASE):
            return pattern
    return None


def expected_process_exit(logical_result: int, *, windows: bool) -> int:
    _ = windows
    return logical_result & 0xFF


def load_symbol_inventory(executable: Path) -> tuple[str | None, str]:
    if os.name == "nt":
        map_path = executable.with_suffix(".map")
        ok, symbol_text = toolchain.load_msvc_link_map_symbols(executable, map_path)
        if not ok:
            return None, symbol_text
        return symbol_text, ""
    dumper = toolchain.find_symbol_dumper()
    if dumper is None:
        return None, "no verified defined-symbol dumper is available"
    ok, symbol_text = dumper.dump_defined_symbols(executable)
    if not ok:
        return None, symbol_text
    if not symbol_text.strip():
        return None, "defined-symbol dumper returned an empty inventory"
    return symbol_text, ""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--expected-exit", type=int, required=True)
    parser.add_argument("--expected-stdout-hex")
    parser.add_argument("--expected-stderr-hex")
    args = parser.parse_args()
    executable = args.executable.resolve()
    if not executable.is_file():
        print(f"missing pure-AOT executable: {executable}", file=sys.stderr)
        return 1
    result = subprocess.run([str(executable)], check=False, capture_output=True)
    if result.stderr:
        sys.stderr.buffer.write(result.stderr)
    if args.expected_stderr_hex is not None and result.stderr != bytes.fromhex(args.expected_stderr_hex):
        print(f"pure-AOT stderr mismatch: got {result.stderr!r}", file=sys.stderr)
        return 1
    expected_exit = expected_process_exit(args.expected_exit, windows=os.name == "nt")
    if result.returncode != expected_exit:
        print(
            f"pure-AOT result mismatch: expected exit {expected_exit} "
            f"(logical result {args.expected_exit}), "
            f"got {result.returncode}",
            file=sys.stderr,
        )
        return 1
    if args.expected_stdout_hex is not None:
        expected_stdout = bytes.fromhex(args.expected_stdout_hex)
        if result.stdout != expected_stdout:
            print(
                f"pure-AOT stdout mismatch: expected {expected_stdout!r}, got {result.stdout!r}",
                file=sys.stderr,
            )
            return 1
    symbol_text, symbol_error = load_symbol_inventory(executable)
    if symbol_text is None:
        print(f"pure-AOT symbol inspection failed: {symbol_error}", file=sys.stderr)
        return 1
    forbidden = forbidden_symbol_family(symbol_text)
    if forbidden is not None:
        print(f"pure-AOT executable contains forbidden symbol {forbidden}", file=sys.stderr)
        return 1
    print("pure-AOT executable: PASS (result and symbol inventory)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

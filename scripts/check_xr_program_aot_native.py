#!/usr/bin/env python3
"""Run a generated pure-AOT executable and reject compiler/runtime residue."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path


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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--expected-exit", type=int, required=True)
    args = parser.parse_args()
    executable = args.executable.resolve()
    if not executable.is_file():
        print(f"missing pure-AOT executable: {executable}", file=sys.stderr)
        return 1
    result = subprocess.run([str(executable)], check=False)
    if result.returncode != args.expected_exit:
        print(
            f"pure-AOT result mismatch: expected exit {args.expected_exit}, "
            f"got {result.returncode}",
            file=sys.stderr,
        )
        return 1
    symbols = subprocess.run(
        ["nm", "-g", str(executable)], check=False, capture_output=True, text=True
    )
    if symbols.returncode != 0:
        print(f"nm failed: {symbols.stderr.strip()}", file=sys.stderr)
        return 1
    for pattern in FORBIDDEN:
        if re.search(pattern, symbols.stdout, re.IGNORECASE):
            print(f"pure-AOT executable contains forbidden symbol {pattern}", file=sys.stderr)
            return 1
    print("pure-AOT executable: PASS (result and symbol inventory)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

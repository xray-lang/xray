#!/usr/bin/env python3
"""Check exact diagnostics for compiler-owned source surfaces that were deleted."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


DIAGNOSTIC_RE = re.compile(
    r"(?:^|:\d+:\d+: )error(?:\[E\d+\])?: (?P<analyzer>.*)$"
    r"|^Error: (?P<module>.*)$",
    re.MULTILINE,
)


@dataclass(frozen=True)
class Case:
    source: str
    expected: tuple[str, ...]


CASES = (
    Case(
        "tests/compile_errors/type/branch_hint_builtins_removed.xr",
        ("Undeclared variable 'likely'", "Undeclared variable 'unlikely'"),
    ),
    Case(
        "tests/compile_errors/type/retired_scalar_builtins_removed.xr",
        (
            "Undeclared variable 'int'",
            "Undeclared variable 'byte'",
            "Undeclared variable 'float'",
        ),
    ),
    Case(
        "tests/compile_errors/type/retired_scalar_type_spellings_removed.xr",
        ("undefined type 'int'", "undefined type 'byte'", "undefined type 'float'"),
    ),
    Case(
        "tests/fixtures/removed_compiler_surface/strconv_module_removed.xr",
        ("module 'strconv' not found in stdlib",),
    ),
)


def diagnostics(output: str) -> tuple[str, ...]:
    return tuple(
        match.group("analyzer") or match.group("module")
        for match in DIAGNOSTIC_RE.finditer(output)
    )


def run_case(xray: Path, root: Path, case: Case) -> str | None:
    source = root / case.source
    if not source.is_file():
        return f"{case.source}: fixture is missing"

    result = subprocess.run(
        [str(xray), "check", str(source)],
        cwd=root,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=30,
        check=False,
    )
    output = result.stdout + result.stderr
    actual = diagnostics(output)
    if result.returncode == 0:
        return f"{case.source}: check unexpectedly succeeded"
    if actual != case.expected:
        return (
            f"{case.source}: exact diagnostics changed\n"
            f"  expected first: {case.expected[0]!r}\n"
            f"  actual first:   {(actual[0] if actual else '<none>')!r}\n"
            f"  expected all:   {case.expected!r}\n"
            f"  actual all:     {actual!r}\n"
            f"  output:\n{output.rstrip()}"
        )
    return None


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--xray", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args(argv)

    xray = args.xray.resolve()
    root = args.root.resolve()
    if not xray.is_file():
        print(f"ERROR: xray binary is missing: {xray}")
        return 2

    failures = [failure for case in CASES if (failure := run_case(xray, root, case))]
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}")
        return 1

    print(f"PASS: {len(CASES)} deleted compiler surfaces fail with exact diagnostics")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

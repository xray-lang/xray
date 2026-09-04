#!/usr/bin/env python3
"""Run the source/Reference/VM/native-AOT Wave 4 qualification entry."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from collections import Counter
from pathlib import Path


EXPECTED_TESTS = (
    "test_xi_pipeline",
    "test_xr_program_imported_callable",
    "test_xr_program_vm",
    "test_xr_program_aot",
    "test_xr_program_aot_callable",
    "test_xr_program_aot_existential",
    "test_xr_program_source_aot_native",
    "test_xr_program_imported_callable_native",
)

TEST_REGEX = (
    "^(test_xi_pipeline|test_xr_program_imported_callable|test_xr_program_vm|"
    "test_xr_program_aot|test_xr_program_aot_callable|"
    "test_xr_program_aot_existential|test_xr_program_source_aot_native|"
    "test_xr_program_imported_callable_native)$"
)


class RunnerError(ValueError):
    """Raised when CTest discovery cannot prove the exact Wave 4 test set."""


def run(command: list[str], cwd: Path, *, capture_output: bool = False
        ) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        check=False,
        capture_output=capture_output,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


def validate_discovery(output: str) -> None:
    try:
        document = json.loads(output)
    except json.JSONDecodeError as exc:
        raise RunnerError(f"CTest discovery did not return valid JSON: {exc}") from exc
    if not isinstance(document, dict) or not isinstance(document.get("tests"), list):
        raise RunnerError("CTest discovery JSON has no test list")

    names: list[str] = []
    for index, row in enumerate(document["tests"]):
        if not isinstance(row, dict) or not isinstance(row.get("name"), str):
            raise RunnerError(f"CTest discovery row {index} has no valid name")
        names.append(row["name"])

    counts = Counter(names)
    duplicates = sorted(name for name, count in counts.items() if count != 1)
    expected = set(EXPECTED_TESTS)
    actual = set(names)
    missing = sorted(expected - actual)
    unexpected = sorted(actual - expected)
    if duplicates or missing or unexpected or len(names) != len(EXPECTED_TESTS):
        raise RunnerError(
            "CTest discovery does not match the exact Wave 4 set: "
            f"expected={len(EXPECTED_TESTS)} discovered={len(names)} "
            f"missing={missing} unexpected={unexpected} duplicates={duplicates}"
        )


def expect_rejected(document: object, label: str) -> None:
    output = document if isinstance(document, str) else json.dumps(document)
    try:
        validate_discovery(output)
    except RunnerError:
        return
    raise RunnerError(f"self-test accepted {label}")


def self_test() -> None:
    exact = {"tests": [{"name": name} for name in EXPECTED_TESTS]}
    validate_discovery(json.dumps(exact))
    expect_rejected({"tests": []}, "an empty discovery")
    expect_rejected({"tests": exact["tests"][:-1]}, "a missing test")
    expect_rejected(
        {"tests": [*exact["tests"], {"name": "unexpected_wave4_test"}]},
        "an unexpected test",
    )
    expect_rejected(
        {"tests": [*exact["tests"], exact["tests"][0]]},
        "a duplicate test",
    )
    expect_rejected({"tests": [{}]}, "a malformed test row")
    expect_rejected("not JSON", "malformed JSON")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--ctest", default="ctest")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        print("XrProgram Wave 4 differential runner self-test: PASS")
        return 0
    root = args.root.resolve()
    build_dir = args.build_dir
    if not build_dir.is_absolute():
        build_dir = (root / build_dir).resolve()

    gate = root / "scripts/check_xr_program_wave4_exit.py"
    status = run([
        sys.executable,
        str(gate),
        "--root",
        str(root),
        "--require-ready",
    ], root)
    if status.returncode != 0:
        return status.returncode

    discovery = run([
        args.ctest,
        "--test-dir",
        str(build_dir),
        "--show-only=json-v1",
        "-R",
        TEST_REGEX,
    ], root, capture_output=True)
    if discovery.returncode != 0:
        if discovery.stdout:
            print(discovery.stdout, end="", file=sys.stderr)
        if discovery.stderr:
            print(discovery.stderr, end="", file=sys.stderr)
        print("Wave 4 CTest discovery failed", file=sys.stderr)
        return discovery.returncode
    try:
        validate_discovery(discovery.stdout)
    except RunnerError as exc:
        print(f"Wave 4 differential discovery: FAIL: {exc}", file=sys.stderr)
        return 1

    result = run([
        args.ctest,
        "--test-dir",
        str(build_dir),
        "--output-on-failure",
        "-R",
        TEST_REGEX,
    ], root)
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Task 255: close the go/defer grammar and defer-owner lifetime contract."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys


def invoke(xray: Path, root: Path, mode: str, source: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(xray), mode, str(source)],
        cwd=root,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def require_rejected(xray: Path, root: Path, relative: str, expected: str) -> None:
    source = root / relative
    result = invoke(xray, root, "check", source)
    if result.returncode == 0:
        raise RuntimeError(f"expected rejection: {relative}")
    if expected not in result.stdout:
        raise RuntimeError(
            f"wrong diagnostic for {relative}; expected {expected!r}:\n{result.stdout.rstrip()}"
        )


def require_output(xray: Path, root: Path, relative: str) -> None:
    source = root / relative
    expected_path = source.with_suffix(source.suffix + ".expected")
    expected = expected_path.read_text(encoding="utf-8").replace("\r\n", "\n").rstrip("\n")
    result = invoke(xray, root, "run", source)
    actual = result.stdout.replace("\r\n", "\n").rstrip("\n")
    if result.returncode != 0:
        raise RuntimeError(f"positive case failed: {relative}:\n{actual}")
    if actual != expected:
        raise RuntimeError(
            f"output mismatch for {relative}:\nexpected:\n{expected}\nactual:\n{actual}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--xray", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()

    xray = args.xray.resolve()
    root = args.root.resolve()
    negative_cases = (
        ("tests/compile_errors/syntax/036_defer_assignment_removed.xr",
         "defer takes a call or a block"),
        ("tests/compile_errors/syntax/037_defer_member_assignment_removed.xr",
         "defer takes a call or a block"),
        ("tests/compile_errors/syntax/038_defer_bare_value_removed.xr",
         "defer takes a call or a block"),
        ("tests/compile_errors/syntax/039_defer_noncall_expression_removed.xr",
         "defer takes a call or a block"),
        ("tests/compile_errors/concurrency/go_block_form_removed.xr",
         "go takes a call"),
        ("tests/compile_errors/concurrency/go_noncall_removed.xr",
         "go takes a call"),
        ("tests/compile_errors/concurrency/linked_go_block_form_removed.xr",
         "go takes a call"),
        ("tests/compile_errors/ownership/180_defer_snapshot_blocks_move.xr",
         "cannot move 'buf': a defer in this block holds it (snapshotted"),
        ("tests/compile_errors/ownership/181_defer_capture_blocks_move.xr",
         "cannot move 'buf': a defer in this block holds it (captured"),
        ("tests/compile_errors/ownership/182_defer_snapshot_blocks_return.xr",
         "cannot return 'buf': a defer in this block holds it (snapshotted"),
        ("tests/compile_errors/ownership/183_defer_capture_blocks_return.xr",
         "cannot return 'buf': a defer in this block holds it (captured"),
    )
    for relative, expected in negative_cases:
        require_rejected(xray, root, relative, expected)

    require_output(
        xray, root, "tests/diff/cases/semantics/cleanup/defer_receiver_snapshot.xr"
    )
    require_output(xray, root, "tests/diff/cases/semantics/cleanup/defer_block_assignment.xr")
    require_output(
        xray, root, "tests/diff/cases/semantics/concurrency/go_lambda_local_scope.xr"
    )
    print("task255_go_defer_ok")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)

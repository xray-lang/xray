#!/usr/bin/env python3
"""Gate the tests/regression corpus against its only-grow pass baseline.

The corpus is the deepest behavioural evidence the language has. It runs
through the public `xray test` runner, which writes one JSON record per test.
Most of it does not pass yet: files that cannot be compiled by the single
Source pipeline report their first failure instead of any test. The baseline
therefore records what passes and may only grow:

  - a recorded test that no longer passes fails the run;
  - a test that starts passing and is not recorded also fails, so the change
    that fixed it records it in the same commit (`--update` adds newly passing
    tests and refuses to write when any recorded test no longer passes).

Failing files stay in the denominator: they appear as failures in the report and
are never filtered out. A file that fails as a whole is identified by
`<path>::<file>`.

Exit 0 when the corpus matches its baseline, 1 otherwise.

Usage:
  check_regression_corpus.py --xray build/xray.exe
  check_regression_corpus.py --xray build/xray.exe --update
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CORPUS = REPO_ROOT / "tests" / "regression"
BASELINE = CORPUS / "xir_passing.txt"
# Directories hold importable fixtures rather than standalone cases.
FIXTURE_PARTS = ("fixtures", "modules", "reexport_test")
HEADER = """\
# Passing tests of the tests/regression corpus under the single Source pipeline.
#
# Only-grow ratchet maintained by scripts/check_regression_corpus.py. Every line is
# `<corpus-relative path>::<test name>`. A listed test that stops passing fails the
# gate; a newly passing test must be added (`--update`) by the change that fixes it.
# Files that fail to compile are not listed here and remain failures in the report.
"""


def run_corpus(xray: Path, jobs: int, report: Path, timeout: int) -> subprocess.CompletedProcess:
    command = [str(xray), "test", str(CORPUS), "--jobs", str(jobs), "--quiet", "--report", str(report)]
    env = dict(os.environ)
    env["NO_COLOR"] = "1"
    return subprocess.run(command, cwd=REPO_ROOT, env=env, capture_output=True, timeout=timeout)


def collect(report: Path) -> tuple[set[str], dict[str, str], int]:
    payload = json.loads(report.read_text(encoding="utf-8"))
    if payload.get("schema") != 1:
        raise ValueError("unsupported report schema")
    passing: set[str] = set()
    failing: dict[str, str] = {}
    files = 0
    for entry in payload["files"]:
        relative_path = Path(entry["path"]).resolve().relative_to(CORPUS)
        if any(part in FIXTURE_PARTS for part in relative_path.parts[:-1]):
            continue
        files += 1
        relative = relative_path.as_posix()
        cases = entry["cases"]
        for case in cases:
            identity = f"{relative}::{case['name']}"
            if case["status"] == "passed":
                passing.add(identity)
            elif case["status"] != "skipped":
                failing[identity] = case.get("message", case["status"])
        if "error" in entry:
            failing[f"{relative}::<file>"] = entry["error"]
    return passing, failing, files


def read_baseline(path: Path) -> list[str]:
    if not path.is_file():
        return []
    return [line.strip() for line in path.read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.startswith("#")]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--xray", required=True, type=Path)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("--baseline", type=Path, default=BASELINE)
    parser.add_argument("--json", type=Path, default=None, help="retain the full report at this path")
    parser.add_argument("--update", action="store_true", help="add passing tests only when all recorded tests still pass")
    args = parser.parse_args(argv[1:])

    with tempfile.TemporaryDirectory(prefix="xray_regression_gate_") as temporary:
        report = args.json.resolve() if args.json else Path(temporary) / "regression.json"
        report.parent.mkdir(parents=True, exist_ok=True)
        report.unlink(missing_ok=True)
        try:
            result = run_corpus(args.xray.resolve(), args.jobs, report, args.timeout)
        except subprocess.TimeoutExpired:
            print(f"FAIL: the corpus did not finish within {args.timeout} seconds")
            return 1
        if not report.is_file():
            print(f"FAIL: xray test produced no report (exit {result.returncode})")
            sys.stdout.write(result.stderr.decode(errors="replace"))
            return 1
        try:
            passing, failing, files = collect(report)
        except (ValueError, KeyError, TypeError) as error:
            print(f"FAIL: invalid report: {error}")
            return 1

    print(f"corpus: {files} files, {len(passing)} tests pass, {len(failing)} fail or error")
    recorded = read_baseline(args.baseline)
    recorded_set = set(recorded)
    regressions = sorted(recorded_set - passing)
    fixed = sorted(passing - recorded_set)

    if regressions:
        print(f"FAIL: {len(regressions)} recorded test(s) no longer pass:")
        for name in regressions[:60]:
            reason = failing.get(name) or failing.get(name.split("::")[0] + "::<file>", "not executed")
            print(f"  {name}: {reason}")
        if len(regressions) > 60:
            print(f"  ... {len(regressions) - 60} more")
        if args.update:
            print("FAIL: refusing --update because recorded tests no longer pass; baseline unchanged")
            return 1

    if args.update:
        args.baseline.write_text(HEADER + "".join(f"{name}\n" for name in sorted(passing)),
                                 encoding="utf-8", newline="\n")
        print(f"baseline updated: {len(passing)} entries ({len(fixed)} added, none removed)")
        return 0

    if fixed:
        print(f"FAIL: {len(fixed)} test(s) pass but are not recorded; rerun with --update in the fixing change:")
        for name in fixed[:60]:
            print(f"  {name}")
        if len(fixed) > 60:
            print(f"  ... {len(fixed) - 60} more")
    if regressions or fixed:
        return 1
    print("regression corpus matches its baseline")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

#!/usr/bin/env python3
"""Run a source corpus through canonical `xray run` and record each case's first refusal.

The census is the diagnostic behind capability-family targeting: every case is
executed once, its exit code, first `XR_RUN_*` line and stdout are kept, and a
second census can be diffed against it to report newly passing cases, lost
cases and how refusal kinds moved between layers.  Results carry the binary's
embedded commit so two censuses are never compared across unknown sources.

Usage:
  canonical_first_refusal_census.py run --binary build/xray --cases tests/diff/cases \
      --output census.json [--jobs N]
  canonical_first_refusal_census.py diff --base base.json --new new.json
"""

from __future__ import annotations

import argparse
import collections
import json
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


def binary_identity(binary: Path) -> dict:
    try:
        result = subprocess.run([str(binary), "--version", "--json"], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=30, check=False)
        return json.loads(result.stdout.decode("utf-8", errors="replace") or "{}")
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return {}


def run_case(binary: Path, cases_dir: Path, case: Path, timeout: float) -> tuple[str, dict]:
    stdin_file = case.with_suffix(".xr.stdin")
    args_file = case.with_suffix(".xr.args")
    argv = [str(binary), "run", str(case)]
    if args_file.exists():
        argv.extend(args_file.read_text(encoding="utf-8").split())
    stdin_data = stdin_file.read_bytes() if stdin_file.exists() else b""
    try:
        result = subprocess.run(argv, input=stdin_data, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=timeout, check=False)
        rc = result.returncode
        err = result.stderr.decode("utf-8", errors="replace")
        out = result.stdout.decode("utf-8", errors="replace")
    except subprocess.TimeoutExpired:
        rc, err, out = -999, "TIMEOUT", ""
    first = ""
    for line in err.splitlines():
        if line.startswith("XR_RUN_"):
            first = line.strip()
            break
    return str(case.relative_to(cases_dir)), {"rc": rc, "first": first, "stdout": out[:4096]}


def normalize(message: str) -> str:
    message = re.sub(r"\bv\d+\b", "vN", message)
    message = re.sub(r"\bb\d+\b", "bN", message)
    message = re.sub(r"\d+", "N", message)
    return message[:120]


def command_run(args: argparse.Namespace) -> int:
    binary = Path(args.binary).resolve()
    cases_dir = Path(args.cases).resolve()
    cases = sorted(cases_dir.rglob("*.xr"))
    records: dict[str, dict] = {}
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for rel, record in pool.map(lambda c: run_case(binary, cases_dir, c, args.timeout), cases):
            records[rel] = record
    ok = sum(1 for r in records.values() if r["rc"] == 0 and not r["first"])
    refused = sum(1 for r in records.values() if r["first"])
    payload = {
        "schema": 1,
        "binary": binary_identity(binary),
        "cases_dir": str(cases_dir),
        "summary": {"cases": len(records), "ok": ok, "refused": refused,
                    "other": len(records) - ok - refused},
        "cases": records,
    }
    Path(args.output).write_text(json.dumps(payload, indent=1, sort_keys=True) + "\n",
                                 encoding="utf-8")
    print(f"cases={len(records)} ok={ok} refused={refused} other={len(records) - ok - refused}")
    return 0


def command_diff(args: argparse.Namespace) -> int:
    base = json.loads(Path(args.base).read_text(encoding="utf-8"))["cases"]
    new = json.loads(Path(args.new).read_text(encoding="utf-8"))["cases"]
    ok_base = {k for k, v in base.items() if v["rc"] == 0 and not v["first"]}
    ok_new = {k for k, v in new.items() if v["rc"] == 0 and not v["first"]}
    print(f"ok base={len(ok_base)} new={len(ok_new)} gained={len(ok_new - ok_base)} "
          f"lost={len(ok_base - ok_new)}")
    for case in sorted(ok_new - ok_base):
        print(f"  + {case}")
    for case in sorted(ok_base - ok_new):
        print(f"  - {case}")
    kinds_base = collections.Counter(normalize(v["first"]) for v in base.values() if v["first"])
    kinds_new = collections.Counter(normalize(v["first"]) for v in new.values() if v["first"])
    print("refusal kinds that shrank:")
    for kind, count in kinds_base.most_common():
        if kinds_new.get(kind, 0) < count:
            print(f"  {count:4d} -> {kinds_new.get(kind, 0):4d}  {kind}")
    print("refusal kinds first seen in the new census:")
    for kind, count in kinds_new.most_common():
        if kind not in kinds_base:
            print(f"  {count:4d}  {kind}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    subparsers = parser.add_subparsers(dest="command", required=True)
    run = subparsers.add_parser("run")
    run.add_argument("--binary", required=True)
    run.add_argument("--cases", required=True)
    run.add_argument("--output", required=True)
    run.add_argument("--jobs", type=int, default=4)
    run.add_argument("--timeout", type=float, default=20.0)
    diff = subparsers.add_parser("diff")
    diff.add_argument("--base", required=True)
    diff.add_argument("--new", required=True)
    args = parser.parse_args()
    return command_run(args) if args.command == "run" else command_diff(args)


if __name__ == "__main__":
    sys.exit(main())

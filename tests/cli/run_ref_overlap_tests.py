#!/usr/bin/env python3
"""Exclusive loans of a call, checked by `xray check` against independent expectations.

The cases cover the ref places of one call that must not overlap, a module
binding or class field lent by ref to code that may reach it by another path,
and a Slice of a module binding lent to code that may write that binding.

Every `<name>.xr` under tests/fixtures/ref_overlap, plus the original class
projection alias cases under tests/fixtures/xir_cell_owner, is paired with
`<name>.check` holding exactly one line, written from the overlap rules rather
than captured from a run:

  ok                          `xray check` accepts the program: exit 0 and no
                              output at all
  <line>:<col>: error: <msg>  `xray check` rejects the program and that is its
                              only diagnostic, at exactly that position

The cases stop at `check` because these places do not lower to LIR yet; the
static overlap rule is decided entirely before lowering.

Cases run concurrently and are reported in sorted order, so the report is the
same as a serial run.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

FIXTURES = Path(__file__).resolve().parent.parent / "fixtures" / "ref_overlap"
DIAGNOSTIC = re.compile(r"^(?P<file>.+?):(?P<rest>[0-9]+:[0-9]+: .*)$")


def collect() -> list[tuple[Path, str]]:
    class_owner = FIXTURES.parent / "xir_cell_owner"
    sources = sorted([
        *FIXTURES.glob("*.xr"),
        class_owner / "class_project_alias_read.xr",
        class_owner / "class_project_alias_write.xr",
    ])
    expectations = sorted(FIXTURES.glob("*.check"))
    paired = {source.with_suffix(".check") for source in sources}
    stray = [path.name for path in expectations if path not in paired]
    missing = [source.name for source in sources if not source.with_suffix(".check").is_file()]
    if stray or missing or not sources:
        raise SystemExit(f"unpaired ref overlap fixtures: missing={missing} stray={stray}")
    cases = []
    for source in sources:
        lines = source.with_suffix(".check").read_text(encoding="utf-8").splitlines()
        if len(lines) != 1 or not lines[0].strip():
            raise SystemExit(f"{source.name}: .check must hold exactly one line")
        cases.append((source, lines[0].strip()))
    return cases


def run_case(xray: str, source: Path, expected: str) -> str | None:
    env = dict(os.environ)
    env["NO_COLOR"] = "1"
    result = subprocess.run([xray, "check", str(source)], capture_output=True, timeout=30, env=env)
    stdout = result.stdout.decode("utf-8", errors="replace")
    stderr = result.stderr.decode("utf-8", errors="replace").replace("\r\n", "\n")
    if expected == "ok":
        if result.returncode != 0 or stdout or stderr:
            return f"expected acceptance, got exit {result.returncode}: {stderr.strip() or stdout.strip()}"
        return None
    if result.returncode == 0:
        return f"expected rejection `{expected}`, but check accepted the program"
    lines = [line for line in stderr.split("\n") if line]
    if len(lines) != 1:
        return f"expected exactly one diagnostic `{expected}`, got: {stderr.strip()}"
    match = DIAGNOSTIC.match(lines[0])
    if not match or Path(match.group("file")).name != source.name or match.group("rest") != expected:
        return f"expected `{source.name}:{expected}`, got `{lines[0]}`"
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--xray", required=True)
    args = parser.parse_args()
    xray = str(Path(args.xray).resolve())
    cases = collect()
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        failures = list(pool.map(lambda case: run_case(xray, *case), cases))
    for (source, _), failure in zip(cases, failures):
        print(f"{'FAIL' if failure else 'ok  '} {source.name}" + (f": {failure}" if failure else ""))
    failed = sum(1 for failure in failures if failure)
    print(f"{len(cases) - failed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

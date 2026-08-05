#!/usr/bin/env python3
"""Borrow/ownership RC-count smoke.

Each case runs a fixture under XRAY_XI_RC_COUNT and holds the emitted
retain/release total to a budget. Budgets are one-sided on purpose:

  - a MAX pins an optimisation that must not regress (a borrowed parameter
    that starts retaining again shows up as a total above its ceiling);
  - a MIN pins the opposite, that a case which must NOT be treated as borrowed
    still performs its RC work -- without it, a bug that dropped RC entirely
    would satisfy every ceiling and look like an improvement.

Every case also asserts the program's own output, so a fixture that stopped
computing the right answer cannot pass on its RC numbers alone.

Usage: run_rc_count_tests.py [xray_binary]
"""

from __future__ import annotations

import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, workspace  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent
FIXTURES = SCRIPT_DIR / "rc_count"

MARKER = "[xi-rc-count]"
COUNT_LINE = re.compile(
    r"\[xi-rc-count\] func=\S+ retain=\d+ release=\d+ total=(\d+)")


@dataclass(frozen=True)
class Bound:
    kind: str  # "max" | "min" | "absent"
    value: int
    label: str


@dataclass(frozen=True)
class Case:
    source: str
    expect_stdout: str
    stdout_label: str
    rc_count: bool
    bounds: tuple[Bound, ...] = ()


CASES: tuple[Case, ...] = (
    Case("scalar_no_rc.xr", "2", "scalar: program output", False,
         (Bound("absent", 0, "scalar: disabled by default"),)),
    Case("scalar_no_rc.xr", "2", "scalar: enabled program output", True,
         (Bound("max", 0, "scalar: no heap RC ops"),)),
    Case("value_struct_borrowed_param.xr", "3",
         "borrowed value param: program output", True,
         (Bound("max", 2, "borrowed value param: RC budget"),)),
    Case("value_struct_wrapper_borrowed_param.xr", "3\n1",
         "borrowed value wrapper param: program output", True,
         (Bound("max", 1, "borrowed value wrapper param: RC budget"),)),
    Case("heap_readonly_param.xr", "3", "heap readonly param: program output", True,
         (Bound("max", 0, "heap readonly param: RC budget"),)),
    Case("class_readonly_param.xr", "7", "class readonly param: program output", True,
         (Bound("max", 0, "class readonly param: RC budget"),)),
    # Both directions: this one must still do RC work, and not too much of it.
    Case("class_alias_return_downgrade.xr", "9\n9",
         "class alias-return downgrade: program output", True,
         (Bound("min", 1, "class alias-return downgrade: not borrowed"),
          Bound("max", 4, "class alias-return downgrade: RC budget"))),
)


class Recorder:
    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0

    def ok(self, label: str) -> None:
        print(f"  PASS: {label}")
        self.passed += 1

    def bad(self, label: str, detail: str = "") -> None:
        print(f"  FAIL: {label}")
        self.failed += 1
        if detail:
            for line in detail.splitlines()[:80]:
                print(f"      {line}")


def first_total(stderr: str) -> int | None:
    match = COUNT_LINE.search(stderr)
    return int(match.group(1)) if match else None


def check_bound(rec: Recorder, bound: Bound, stderr: str) -> None:
    if bound.kind == "absent":
        if MARKER in stderr:
            rec.bad(f"{bound.label}: rc-count line emitted while disabled", stderr)
        else:
            rec.ok(bound.label)
        return

    total = first_total(stderr)
    if total is None:
        rec.bad(f"{bound.label}: missing parseable rc-count line", stderr)
        return
    if bound.kind == "max":
        if total <= bound.value:
            rec.ok(f"{bound.label}: total={total} <= {bound.value}")
        else:
            rec.bad(f"{bound.label}: total={total} > {bound.value}", stderr)
    else:
        if total >= bound.value:
            rec.ok(f"{bound.label}: total={total} >= {bound.value}")
        else:
            rec.bad(f"{bound.label}: total={total} < {bound.value}", stderr)


def main(argv: list[str]) -> int:
    xray = Path(argv[1] if len(argv) > 1
                else os.environ.get("XRAY_BIN", str(PROJECT_DIR / "build" / "xray")))

    print("=== Xi RC Count Tests ===")
    print(f"Binary: {xray}")
    print("")

    if not (xray.is_file() and os.access(xray, os.X_OK)):
        sys.stderr.write(f"FAIL: xray binary not executable: {xray}\n")
        return 1
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300)

    rec = Recorder()
    with workspace.Workspace("xray_rc_count"):
        for case in CASES:
            env = dict(os.environ)
            if case.rc_count:
                env["XRAY_XI_RC_COUNT"] = "1"
            else:
                env.pop("XRAY_XI_RC_COUNT", None)
            result = proc.run([xray, FIXTURES / case.source], env=env,
                              timeout=timeout)
            stdout = result.stdout.decode("utf-8", "replace").rstrip("\n")
            stderr = result.stderr.decode("utf-8", "replace")

            if stdout == case.expect_stdout:
                rec.ok(case.stdout_label)
            else:
                rec.bad(f"{case.stdout_label}: output {stdout!r} != "
                        f"{case.expect_stdout!r}")
            for bound in case.bounds:
                check_bound(rec, bound, stderr)

    print("")
    print(f"Passed: {rec.passed}")
    print(f"Failed: {rec.failed}")
    return 1 if rec.failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

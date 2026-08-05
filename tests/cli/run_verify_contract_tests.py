#!/usr/bin/env python3
"""`xray verify --contract`: it proves what it claims, and it fails closed.

Every case names a contract file, whether verification must succeed, and the
wording that has to appear. The wording matters as much as the exit status: a
contract that fails for the wrong reason is not evidence, and a diagnostic that
overstates what the compiler knows (\"detected a cycle\" rather than \"cannot
prove acyclicity\") is a claim the compiler cannot back.

Usage: run_verify_contract_tests.py <xray> <fixture-dir>
"""

from __future__ import annotations

import shutil
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Tuple


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, workspace  # noqa: E402


@dataclass(frozen=True)
class Case:
    contract: str
    should_verify: bool
    # Wording that must appear. Split by stream because which stream carries a
    # verdict is itself part of the contract CLI's surface.
    stdout_contains: Tuple[str, ...] = ()
    stderr_contains: Tuple[str, ...] = ()
    # Printed when a contract that must fail instead passed.
    unexpected_pass: str = ""


CASES: Tuple[Case, ...] = (
    Case("positive.toml", True, stdout_contains=("verified symbol-id=",)),
    Case("negative-effect.toml", False,
         stderr_contains=("contract 'allocates' failed", "effect summary"),
         unexpected_pass="allocation contract unexpectedly passed"),
    Case("negative-order.toml", False,
         stdout_contains=("symbol=pure scope=semantic",),
         stderr_contains=("contract 'allocates' failed",),
         unexpected_pass="ordered negative contract unexpectedly passed"),
    Case("negative-schema.toml", False,
         stderr_contains=("unknown contract key 'legacy_allow'",),
         unexpected_pass="unknown contract key unexpectedly passed"),
    # A bodyless extern "C" with no audited native contract is not evidence of
    # anything. LANGUAGE_SPEC 5.2.11 requires native unknowns to fail closed.
    Case("negative-native-unknown.toml", False,
         stderr_contains=("contract 'callsUncontractedNative' failed",
                          "proof incomplete"),
         unexpected_pass='uncontracted extern "C" call unexpectedly proved a '
                         "semantic contract"),
    # A requirement with no inference source must be rejected, not granted.
    Case("negative-uninferred.toml", False,
         stderr_contains=("has no inference source",),
         unexpected_pass="requirement without an inference source unexpectedly "
                         "passed"),
    # The two suspension dimensions must stay distinguishable: the same
    # generator that proves `no_reschedule` in positive.toml must still fail
    # `no_suspend`.
    Case("negative-generator-suspend.toml", False,
         stderr_contains=("contract 'counter' failed",
                          "forbidden semantic effect"),
         unexpected_pass="generator body unexpectedly proved no_suspend"),
    # no_reference_cycles, proven from the L0 type graph.
    Case("cycles-positive.toml", True, stdout_contains=("verified symbol-id=",)),
    # A recursive type must fail, and the wording must say the compiler cannot
    # PROVE acyclicity -- not that it detected a cycle. Those are different
    # claims and only the first one is true.
    Case("cycles-negative.toml", False,
         stderr_contains=("contract 'CyclicNode' failed",
                          "cannot prove acyclicity", "L0 type graph"),
         unexpected_pass="recursive type unexpectedly proved no_reference_cycles"),
    # A `weak` back-edge takes the class out of the candidate set, so the same
    # contract becomes provable.
    Case("cycles-weak-broken.toml", True,
         stdout_contains=("verified symbol-id=",)),
)

# The edit the LSP code action offers, applied verbatim.
WEAK_EDIT_FROM = "    next: CyclicNode?"
WEAK_EDIT_TO = "    weak next: CyclicNode?"


def check(result, case: Case) -> bool:
    stdout = result.stdout.decode("utf-8", "replace")
    stderr = result.stderr.decode("utf-8", "replace")
    if case.should_verify:
        if not result.ok:
            sys.stderr.write(f"{case.contract}: verification failed\n{stderr}")
            return False
    elif result.ok:
        sys.stderr.write(f"{case.unexpected_pass}\n")
        return False

    for needle in case.stdout_contains:
        if needle not in stdout:
            sys.stderr.write(f"{case.contract}: stdout missing {needle!r}\n{stdout}")
            return False
    for needle in case.stderr_contains:
        if needle not in stderr:
            sys.stderr.write(f"{case.contract}: stderr missing {needle!r}\n{stderr}")
            return False
    return True


def run_weak_edit_case(xray: Path, fixture: Path, work: Path,
                       timeout: "float | None") -> bool:
    """Apply the code action's own edit and require the contract to become provable.

    Two hand-written classes would only prove that `weak` works somewhere. This
    proves the specific edit the tooling offers fixes the specific failure.
    Work on a copy: a run that dies partway must not leave the fixture edited.
    """
    copy = work / "h-fixture"
    shutil.copytree(fixture, copy)
    main = copy / "main.xr"
    text = main.read_text(encoding="utf-8")
    if WEAK_EDIT_FROM + "\n" not in text:
        sys.stderr.write(f"fixture no longer has the field the edit targets: "
                         f"{WEAK_EDIT_FROM!r}\n")
        return False
    platform.write_text_lf(main, text.replace(WEAK_EDIT_FROM + "\n",
                                              WEAK_EDIT_TO + "\n"))

    result = proc.run([xray, "verify", "--contract", "cycles-negative.toml"],
                      cwd=copy, timeout=timeout)
    if not result.ok:
        sys.stderr.write("applying weak did not make no_reference_cycles provable\n")
        sys.stderr.write(result.stderr.decode("utf-8", "replace"))
        return False
    if "verified symbol-id=" not in result.stdout.decode("utf-8", "replace"):
        sys.stderr.write("weak-edited contract passed without a verified symbol-id\n")
        return False
    return True


def main(argv: List[str]) -> int:
    if len(argv) < 3:
        sys.stderr.write("usage: run_verify_contract_tests.py <xray> <fixture-dir>\n")
        return 1
    xray, fixture = Path(argv[1]), Path(argv[2])
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300)

    with workspace.Workspace("xray_verify_contract") as ws:
        for case in CASES:
            result = proc.run([xray, "verify", "--contract", case.contract],
                              cwd=fixture, timeout=timeout)
            if not check(result, case):
                return 1
        if not run_weak_edit_case(xray, fixture, ws.root, timeout):
            return 1

    print("PASS: versioned verify contract succeeds and fails closed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

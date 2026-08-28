#!/usr/bin/env python3
"""`byte` and `u8` are one canonical type, checked at all three layers.

The identity has to hold in the language, in what the LSP shows the user, and
in the global evidence cache's type keys. Checking only the language would let
the other two drift into presenting `byte` and `u8` as distinct types, which is
exactly the confusion the canonicalisation exists to prevent.

Two of those three layers are proved by cases that live inside large shared
unit executables: test_lsp_document and test_xglobal_summary each carry more
than a hundred cases about everything else their component does. This gate used
to run each executable whole and judge it by its exit code, which silently
promoted every case in them to a byte/u8 case. A coroutine runtime control
plane regression, or an evidence-cache schema bump, turned the byte/u8 identity
gate red while every byte/u8 case in the same executable was green.

A red has to name the invariant it broke. When a gate's red can mean anything
that happens to share an executable with its evidence, the red stops carrying
information: the reader learns only that something, somewhere, is wrong, and
the cheapest correct response becomes to look past this gate at the suite it
borrowed its evidence from. A gate that trains people to look past it is worse
than no gate at all, because it also occupies the name of the check that nobody
is performing any more. Whole-suite-as-one-gate is the shape that produces
that: it couples a narrow claim to an unbounded set of unrelated ones, so the
gate's failure rate is dominated by causes it never claimed to watch, and its
signal decays as the borrowed suite grows.

So this gate reads the framework's per-case verdicts and judges only the cases
that carry its own invariant. Everything else in those executables is reported,
by name, as explicitly not judged here, together with the ctest entry that does
own it -- test_lsp_document and test_xglobal_summary are both registered as
ctest entries of their own, so declining to judge them here narrows this gate
without dropping any coverage. Narrowing, not skipping: the byte/u8 cases
inside those same executables are still run and still judged, and a gate that
loses sight of its own evidence (a renamed or deleted anchor case, an
executable that dies before reaching it) is red, because a green that proves
nothing is the other way a gate stops meaning anything.

Environment:
    XRAY_BIN                  the xray executable
    XRAY_TEST_LSP_DOCUMENT    the test_lsp_document binary
    XRAY_TEST_XGLOBAL_SUMMARY the test_xglobal_summary binary

Usage: run_byte_u8_canonical_audit.py
"""

from __future__ import annotations

import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Sequence


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parent.parent / "tests" / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc  # noqa: E402

PROJECT_ROOT = Path(__file__).resolve().parent.parent
LABEL = "[byte-u8-canonical]"

REGRESSION_CASE = (PROJECT_ROOT / "tests" / "regression" / "14_typed_array"
                   / "1409_byte_u8_canonical_identity.xr")

# The two executables this gate borrows evidence from print their per-case
# verdicts in two different shapes, because test_lsp_document defines its own
# runner macros instead of including tests/unit/test_framework.h.
#
# test_framework.h: two spaces, the case name padded to 50 columns, then a
# colourised PASS or FAIL, with any failure detail on following indented lines.
# Colour is unconditional there, but matching it optionally keeps this parser
# working if the framework ever learns to honour a no-colour terminal.
FRAMEWORK_CASE = re.compile(
    r"^ {2}(?P<name>\w+)[ \t]+(?:\x1b\[3[12]m)?(?P<verdict>PASS|FAIL)(?:\x1b\[0m)?[ \t]*$")

# test_lsp_document: "  Testing <name>... " with no newline, then PASS, or a
# one-line "FAIL at line N: <cond>" from the assertion itself. Its verbose
# server logging goes to stderr, so these stdout lines stay intact.
LSP_CASE = re.compile(r"^ {2}Testing (?P<name>\w+)\.\.\.[ \t]*(?P<rest>.*)$")

# In test_framework.h a failing assertion prints FAIL plus indented detail and
# returns out of the case, after which RUN_TEST unconditionally prints its own
# PASS -- a bare, name-less line. FRAMEWORK_CASE requires a name, so that stray
# line is ignored rather than credited to the next case.
DETAIL_LINE = re.compile(r"^ {4}\S")

# A case whose name carries byte, u8 or uint8 is claiming to be about this
# identity, including the negative direction (an int array must NOT offer the
# u8 registry methods). New byte/u8 cases are picked up without editing this
# gate. Cases that merely use `u8` in fixture source while proving something
# else -- capacity plans, map literals -- do not name it, and are not judged
# here.
RELEVANT_NAME = re.compile(r"byte|u8|uint8", re.IGNORECASE)


@dataclass
class CaseVerdict:
    name: str
    # None means the case announced itself but never reported a verdict -- it
    # crashed, hung, or its line was cut short. Never read as a pass.
    passed: bool | None
    detail: list = field(default_factory=list)


@dataclass(frozen=True)
class SuiteStep:
    """One layer proved by cases inside a shared unit executable."""

    label: str
    binary: str
    # The suite's own ctest entry: the owner of every case this gate declines
    # to judge. Named in the diagnostic so an unrelated failure sends the
    # reader to the gate that does claim it.
    owner_test: str
    # The cases this gate was built on. They must run and pass. Anchors are
    # what stops a green from going vacuous when a case is renamed away or the
    # executable dies before reaching it.
    anchors: frozenset


def require_env(name: str) -> str | None:
    value = os.environ.get(name)
    if not value:
        sys.stderr.write(f"{name} must point to the executable\n")
        return None
    return value


def parse_cases(text: str) -> list:
    """Turn either runner's console output into per-case verdicts."""
    cases: list = []
    open_failure: CaseVerdict | None = None
    for line in text.splitlines():
        match = FRAMEWORK_CASE.match(line)
        if match:
            case = CaseVerdict(match.group("name"), match.group("verdict") == "PASS")
            cases.append(case)
            open_failure = None if case.passed else case
            continue
        match = LSP_CASE.match(line)
        if match:
            rest = match.group("rest").strip()
            if rest.startswith("PASS"):
                case = CaseVerdict(match.group("name"), True)
            elif rest.startswith("FAIL"):
                case = CaseVerdict(match.group("name"), False, [rest])
            else:
                case = CaseVerdict(match.group("name"), None,
                                   [rest] if rest else [])
            cases.append(case)
            open_failure = None
            continue
        if open_failure is not None and DETAIL_LINE.match(line):
            open_failure.detail.append(line.strip())
            continue
        open_failure = None
    return cases


def run_regression_step(label: str, argv: Sequence, timeout: float | None) -> bool:
    """Judge a whole-file byte/u8 regression by its exit code.

    This step runs one .xr file that exists only to prove the identity, so the
    file's own verdict is exactly this gate's claim -- no narrowing to do.
    """
    print(f"{LABEL} {label}")
    return proc.run_passthrough(argv, timeout=timeout) == 0


def run_suite_step(step: SuiteStep, timeout: float | None) -> bool:
    """Run one shared unit executable and judge only its byte/u8 cases."""
    print(f"{LABEL} {step.label}")
    result = proc.run([step.binary], timeout=timeout)
    # Verdicts are printed to stdout by both runners; stderr carries whatever
    # the component under test logs, which would only add noise here.
    cases = parse_cases(result.stdout_text(errors="replace"))

    if not cases:
        print(f"{LABEL}   no case verdicts parsed from {step.owner_test}")
        print(f"{LABEL}   exit={result.returncode} timed_out={result.timed_out}")
        excerpt = result.combined_text().strip().splitlines()[-20:]
        for line in excerpt:
            print(f"{LABEL}   | {line}")
        print(f"{LABEL}   FIX: this gate cannot see its own evidence -- run "
              f"`ctest -R {step.owner_test}` and repair that executable first")
        return False

    judged = [c for c in cases if c.name in step.anchors or RELEVANT_NAME.search(c.name)]
    judged_names = {c.name for c in judged}
    unrelated_failures = [c for c in cases
                          if c.name not in judged_names and c.passed is False]

    print(f"{LABEL}   judged {len(judged)} byte/u8 case(s) of {len(cases)} in {step.owner_test}:")
    for case in judged:
        verdict = "PASS" if case.passed else ("FAIL" if case.passed is False else "NO VERDICT")
        print(f"{LABEL}     {verdict}  {case.name}")

    ok = True

    resolved = {c.name for c in judged if c.passed is not None}
    # An anchor that never appeared and a byte/u8 case that announced itself
    # but never reported are the same hole: evidence this gate claims to have
    # read, and did not.
    missing = sorted({c.name for c in judged if c.passed is None} | (step.anchors - resolved))
    if missing:
        print(f"{LABEL}   {len(missing)} anchor case(s) never reported a verdict:")
        for name in missing:
            print(f"{LABEL}     {name}")
        print(f"{LABEL}   FIX: this gate's byte/u8 evidence is gone. If the case was renamed, "
              f"update the anchors in {Path(__file__).name}; if the executable died before "
              f"reaching it (exit={result.returncode} timed_out={result.timed_out}), run "
              f"`ctest -R {step.owner_test}`")
        ok = False

    failed = [c for c in judged if c.passed is False]
    if failed:
        print(f"{LABEL}   {len(failed)} byte/u8 case(s) failed:")
        for case in failed:
            print(f"{LABEL}     {case.name}")
            for line in case.detail:
                print(f"{LABEL}       {line}")
        print(f"{LABEL}   FIX: `byte` and `u8` are drifting apart at this layer -- repair the "
              f"canonicalisation the case names, not the case")
        ok = False

    # Everything below here is reported, never judged. These cases share an
    # executable with this gate's evidence and nothing else; they are guarded
    # by their own ctest entry, and letting them redden a byte/u8 gate is what
    # this narrowing exists to stop.
    if unrelated_failures:
        print(f"{LABEL}   note: {len(unrelated_failures)} case(s) in this executable failed for "
              f"reasons unrelated to byte/u8; this gate does not judge them:")
        for case in unrelated_failures:
            print(f"{LABEL}     {case.name}")
        print(f"{LABEL}   they are owned by the `{step.owner_test}` ctest entry -- run "
              f"`ctest -R {step.owner_test}` to see them as a red of their own")
    if result.timed_out or result.returncode < 0:
        print(f"{LABEL}   note: {step.owner_test} terminated abnormally "
              f"(exit={result.returncode} timed_out={result.timed_out}); that belongs to the "
              f"`{step.owner_test}` ctest entry, not to this gate")

    return ok


def main(argv: list) -> int:
    xray = require_env("XRAY_BIN")
    lsp_document = require_env("XRAY_TEST_LSP_DOCUMENT")
    xglobal_summary = require_env("XRAY_TEST_XGLOBAL_SUMMARY")
    if not (xray and lsp_document and xglobal_summary):
        return 1
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300)

    print(f"{LABEL} guarding: `byte` and `u8` are one canonical type in the language, "
          f"in LSP display/docs, and in global evidence type keys")

    if not run_regression_step("language identity regression",
                               [xray, "test", REGRESSION_CASE], timeout):
        return 1

    suites = (
        SuiteStep(
            label="LSP canonical byte display/docs",
            binary=lsp_document,
            owner_test="test_lsp_document",
            anchors=frozenset({
                "completion_u8_array_registry_methods",
                "completion_uint8_array_uses_canonical_byte_docs",
                "completion_int_array_excludes_u8_registry_methods",
                "completion_u8_slice_registry_methods",
                "hover_u8_array_registry_method",
                "signature_help_u8_array_registry_method",
            }),
        ),
        SuiteStep(
            label="global evidence/cache canonical U8 type keys",
            binary=xglobal_summary,
            owner_test="test_xglobal_summary",
            anchors=frozenset({
                "global_evidence_producer_canonicalizes_byte_u8_sequence_type_keys",
            }),
        ),
    )
    failed = False
    for step in suites:
        if not run_suite_step(step, timeout):
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

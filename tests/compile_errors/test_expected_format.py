#!/usr/bin/env python3
"""Self-test for the matcher every compile-error case is judged by.

Without this, a regression in `expected_format` would not turn any case red --
it would turn all 870 of them silently green, which is precisely the failure
mode this suite was rewritten to escape. So the cases the matcher must REJECT
are the ones asserted here: a right message at the wrong column, at the wrong
line, in the wrong file, under the wrong code, and two assertions trying to
live off one diagnostic.

Usage: test_expected_format.py
"""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path


def _bootstrap() -> None:
    here = str(Path(__file__).resolve().parent)
    if here not in sys.path:
        sys.path.insert(0, here)


_bootstrap()
from expected_format import (  # noqa: E402
    ExpectedFormatError, parse_diagnostics, parse_expected, match_assertions)

CASE = "010_invalid_index.xr"

RENDERED = """error[E0352]: Index type 'string' is not assignable to expected type 'i64'
  --> /abs/path/010_invalid_index.xr:5:13

error: aborting due to 1 previous error"""

# The checker's one-line shape, and a `hint:` line sitting between a message
# and its own location -- both real shapes the compiler emits today.
INLINE = "/abs/path/010_invalid_index.xr:9:0: error: body cannot suspend"
HINTED = """error[E0363]: go closure cannot capture mutable variable 'counter'
hint: bind a Channel handle as const
   --> /abs/path/010_invalid_index.xr:11:9"""
MODULE = "Error: E0504: circular dependency: a.xr -> b.xr -> a.xr"

failures: list[str] = []


def expect(condition: bool, what: str) -> None:
    if not condition:
        failures.append(what)


def matches(expected_text: str, output: str = RENDERED) -> list[str]:
    with tempfile.TemporaryDirectory() as workspace:
        path = Path(workspace) / f"{CASE}.expected"
        path.write_text(expected_text, encoding="utf-8")
        assertions = parse_expected(path)
    return match_assertions(assertions, parse_diagnostics(output), CASE)


def rejects(expected_text: str, output: str = RENDERED) -> bool:
    return bool(matches(expected_text, output))


def raises(expected_text: str) -> bool:
    with tempfile.TemporaryDirectory() as workspace:
        path = Path(workspace) / f"{CASE}.expected"
        path.write_text(expected_text, encoding="utf-8")
        try:
            parse_expected(path)
        except ExpectedFormatError:
            return True
    return False


WORDING = "Index type 'string' is not assignable"

# The position is the point of the whole format: right words, wrong place fails.
expect(not rejects(f"--> 5:13 E0352\n{WORDING}\n"), "the correct position must pass")
expect(rejects(f"--> 5:1 E0352\n{WORDING}\n"), "a wrong column must fail")
expect(rejects(f"--> 4:13 E0352\n{WORDING}\n"), "a wrong line must fail")
expect(rejects(f"--> other.xr:5:13 E0352\n{WORDING}\n"), "a wrong file must fail")
expect(rejects(f"--> 5:13 E0999\n{WORDING}\n"), "a wrong code must fail")
expect(rejects("--> 5:13 E0352\nIndex type 'i64' is not assignable\n"),
       "wrong wording must fail")
# One diagnostic satisfies one assertion, so N assertions demand N diagnostics.
expect(rejects(f"--> 5:13 E0352\n{WORDING}\n\n--> 5:13 E0352\nexpected type 'i64'\n"),
       "two assertions must not share one diagnostic")

# Every emitted shape has to parse, or a case could pin words nothing checks.
expect(not rejects("--> 9:0\nbody cannot suspend\n", INLINE),
       "the checker's one-line shape must parse")
expect(not rejects("--> 11:9 E0363\ngo closure cannot capture\nhint: bind a Channel",
                   HINTED),
       "a location after a hint line must bind to its own diagnostic")
expect(not rejects("--> ? E0504\ncircular dependency\n", MODULE),
       "a locationless module diagnostic must parse, code and all")
expect(rejects("--> 3:1 E0504\ncircular dependency\n", MODULE),
       "a locationless diagnostic must not satisfy a positioned assertion")

# Malformed expected files fail loudly rather than matching nothing.
expect(raises("just some wording\n"), "text with no assertion must raise")
expect(raises("--> nonsense E0352\n"), "a malformed position must raise")
expect(raises("--> 5:13 NOTACODE\n"), "a malformed code must raise")
expect(raises("--> 5:13 E0352\nmisplaced 5:13\n"),
       "a `misplaced` repeating the asserted position must raise")
expect(raises("# only a comment\n"), "an assertion-free file must raise")

# A degenerate position is recorded, not rewritten, and counts as a gap.
with tempfile.TemporaryDirectory() as tmp:
    probe = Path(tmp) / f"{CASE}.expected"
    probe.write_text("--> 9:0\nbody cannot suspend\n", encoding="utf-8")
    expect(parse_expected(probe)[0].where.degenerate,
           "a zero column must be flagged as a position gap")
    probe.write_text("--> ? E0504\ncircular dependency\n", encoding="utf-8")
    expect(parse_expected(probe)[0].where.degenerate,
           "a diagnostic with no location at all must be flagged as a gap")
    probe.write_text("--> 5:13 E0352\nmisplaced 5:1\nwording\n", encoding="utf-8")
    expect(parse_expected(probe)[0].misplaced is not None,
           "a `misplaced` line must be read back")

if failures:
    print(f"expected_format self-test: {len(failures)} failure(s)")
    for line in failures:
        print(f"  - {line}")
    raise SystemExit(1)
print("expected_format self-test: all checks passed")
raise SystemExit(0)

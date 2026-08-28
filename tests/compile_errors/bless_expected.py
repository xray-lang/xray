#!/usr/bin/env python3
"""Re-anchor expected files against what the compiler currently reports.

What this tool rewrites is deliberately narrow: the POSITION and the ERROR CODE
of each assertion. It never touches the wording. A case's substrings are a
human statement of what the diagnostic must say; letting a bless silently
absorb a reworded message would turn this suite back into a rubber stamp. When
wording drifts, the case fails and someone reads the new sentence.

Positions are the opposite: they are pure fact about where the compiler points,
they move whenever a case file gains a line, and nobody can keep 857 of them
correct by hand.

  --check   report what would change, write nothing (the default)
  --write   rewrite the expected files
  --new     also create expected files for cases that have none, seeding the
            wording from the diagnostic's own message

A case whose wording no longer matches any emitted diagnostic is reported and
left alone: that is exactly the review this tool must not skip.

Environment:
    XRAY / XRAY_BIN   the xray binary
    XRAY_TEST_JOBS    parallelism (default: number of CPUs)
"""

from __future__ import annotations

import os
import re
import sys
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))
    here = str(Path(__file__).resolve().parent)
    if here not in sys.path:
        sys.path.insert(0, here)


_bootstrap()
from expected_format import (  # noqa: E402
    Assertion, Diagnostic, ExpectedFormatError, Position, expected_path_for,
    parse_diagnostics, parse_expected)
from xraytest import platform, proc  # noqa: E402

platform.configure_utf8_stdio()

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent



@dataclass
class Blessing:
    case: Path
    expected: Path
    text: str | None       # the file to write, None when nothing to write
    changed: bool
    notes: list[str]


def _run(xray: Path, case: Path, timeout: float | None) -> list[Diagnostic]:
    env = dict(os.environ)
    inherited = os.environ.get("XRAY_TYPEPATH", "")
    env["XRAY_TYPEPATH"] = (f"{case.parent}{os.pathsep}{inherited}"
                            if inherited else str(case.parent))
    env["NO_COLOR"] = "1"
    result = proc.run([xray, case], env=env, timeout=timeout)
    return parse_diagnostics(result.combined_text().rstrip("\n"))


def _reanchor(old: list[Assertion], emitted: list[Diagnostic],
              case_name: str) -> tuple[list[Assertion], list[str]]:
    """Re-point each existing assertion at the diagnostic that still carries it.

    An assertion claims the first unclaimed diagnostic whose text contains all
    of its wording, so N assertions keep demanding N diagnostics. The code is
    tried as a filter first and then dropped: a code that moved is a fact worth
    recording, while wording that vanished is a review this tool must not
    absorb, so it is reported and the assertion left untouched.
    """
    claimed: set[int] = set()
    reanchored: list[Assertion] = []
    orphans: list[str] = []

    def claim(assertion: Assertion, with_code: bool) -> int | None:
        for index, diagnostic in enumerate(emitted):
            if index in claimed:
                continue
            if with_code and assertion.code and diagnostic.code != assertion.code:
                continue
            if all(text in diagnostic.text for text in assertion.texts):
                return index
        return None

    for assertion in old:
        found = claim(assertion, True)
        if found is None:
            found = claim(assertion, False)
        if found is None:
            orphans.append("; ".join(assertion.texts) or assertion.code or "(empty)")
            continue
        claimed.add(found)
        diagnostic = emitted[found]
        file_part = None
        if diagnostic.file and Path(diagnostic.file).name != case_name:
            file_part = Path(diagnostic.file).name
        where = (Position(None, None, None) if diagnostic.line is None
                 else Position(file_part, diagnostic.line, diagnostic.col))
        reanchored.append(Assertion(where, diagnostic.code,
                                    misplaced=assertion.misplaced,
                                    texts=list(assertion.texts)))
    return reanchored, orphans


def _seed(emitted: list[Diagnostic], case_name: str) -> list[Assertion]:
    """A first assertion for a case that has no expected file yet.

    Only the first diagnostic is seeded, and its whole message becomes the
    wording. Both are starting points a human is meant to trim: a case should
    demand the sentence it is about, not every sentence the compiler happened
    to print.
    """
    first = emitted[0]
    texts = [line for line in first.text.splitlines() if line.strip()]
    file_part = None
    if first.file and Path(first.file).name != case_name:
        file_part = Path(first.file).name
    where = (Position(None, None, None) if first.line is None
             else Position(file_part, first.line, first.col))
    return [Assertion(where, first.code, texts=texts)]


def bless_one(xray: Path, case: Path, timeout: float | None,
              create_new: bool) -> Blessing:
    expected, _ = expected_path_for(case)

    if not expected.is_file():
        if not create_new:
            return Blessing(case, expected, None, False,
                            ["no expected file (use --new)"])
        emitted = _run(xray, case, timeout)
        if not emitted:
            return Blessing(case, expected, None, False,
                            ["no expected file and no diagnostic to seed one from"])
        seeded = _seed(emitted, case.name)
        return Blessing(case, expected, _render(seeded), True,
                        ["seeded from the first diagnostic; trim the wording"])

    try:
        old = parse_expected(expected)
    except ExpectedFormatError as bad:
        return Blessing(case, expected, None, False, [str(bad)])

    emitted = _run(xray, case, timeout)
    reanchored, orphans = _reanchor(old, emitted, case.name)
    notes = [f"wording not found in any diagnostic: {o}" for o in orphans]
    if not reanchored:
        return Blessing(case, expected, None, False, notes or ["nothing to anchor"])
    if orphans:
        # Rewriting now would drop the assertions whose wording vanished.
        return Blessing(case, expected, None, False, notes)

    rendered = _render(reanchored)
    return Blessing(case, expected, rendered,
                    rendered != expected.read_text(encoding="utf-8"), notes)


def _render(assertions: list[Assertion]) -> str:
    return "\n".join(a.render() for a in assertions)


def collect_cases() -> list[Path]:
    cases: list[Path] = []
    for directory in sorted(p for p in SCRIPT_DIR.iterdir() if p.is_dir()):
        cases.extend(sorted(directory.glob("*.xr")))
    return cases


def main(argv: list[str]) -> int:
    write = "--write" in argv
    create_new = "--new" in argv
    only = [a for a in argv[1:] if not a.startswith("--")]

    xray = Path(os.environ.get("XRAY")
                or os.environ.get("XRAY_BIN")
                or str(PROJECT_DIR / "build" / platform.exe_name("xray")))
    if not (xray.is_file() and os.access(xray, os.X_OK)):
        print(f"Error: xray not found at {xray}")
        return 1

    cases = collect_cases()
    if only:
        cases = [c for c in cases if any(token in str(c) for token in only)]
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300)
    jobs = platform.env_int("XRAY_TEST_JOBS", platform.cpu_count())

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        results = list(pool.map(
            lambda c: bless_one(xray, c, timeout, create_new), cases))
    results.sort(key=lambda b: str(b.case))

    changed = written = 0
    for item in results:
        if item.changed and item.text is not None:
            changed += 1
            if write:
                item.expected.write_text(item.text, encoding="utf-8")
                written += 1
        for note in item.notes:
            print(f"  {item.case.relative_to(SCRIPT_DIR)}: {note}")

    print("")
    print(f"cases: {len(results)}   would change: {changed}"
          + (f"   written: {written}" if write else "   (dry run; pass --write)"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

#!/usr/bin/env python3
"""Hold the CI ctest exclusions to the discipline known_failures.txt already has.

Twenty-one named ctest lanes are skipped on every pull request and every trunk
push. `.github/workflows/ci.yml` used to spell them as two inline regexes, and
its own comment said "Adding a name is a decision to stop testing something, so
it needs a reason recorded next to it" -- while none of the twenty-one carried
a reason, a date, or an owner. The only prose next to them described names that
had already been *removed* from the lists. That is a larger suppression surface
than known_failures.txt has ever held, governed by nothing.

So the exclusions live in tests/ci_exclusions.txt now, one line per test, with
the same four fields known_failures.txt requires plus a lane and a reason:

    <test name>  LANE=<lanes>  ISSUE=<url>  ADDED=YYYY-MM-DD  OWNER=<email>
                 REASON=<prose to end of line>

and the workflow derives its regexes from that file rather than restating them.
A single source cannot drift from itself.

What this checks:

  1. Every entry carries all six fields, and ADDED parses as a date.
  2. No entry is older than the freshness window. An exclusion that nobody has
     re-justified in thirty days is not a decision any more, it is sediment.
  3. Every excluded name is a test this tree actually registers. A name that
     survives the test it was written for silently excludes nothing, and the
     next person to read the list learns something false.
  4. The workflow no longer hardcodes an exclusion regex, so the manifest
     cannot be bypassed by editing YAML.
  5. scripts/run_release_09_gate.py's own hardcoded boundary regex agrees with
     the manifest's `gate` lane.
  6. The manifest does not exclude this gate, which would let it delete itself.

Lanes:

  core       XRAY_CI_CORE_CTEST_EXCLUDE_RE -- the sanitizer and coverage jobs'
             direct `ctest --exclude-regex`.
  release09  XRAY_CI_RELEASE09_EXTRA_EXCLUDE_RE -- passed into the 0.9
             consolidation gate by the unit-tests job.
  gate       scripts/run_release_09_gate.py's KNOWN_AOT_BOUNDARY_RE.

Exit 0 when every check passes, 1 otherwise.

Usage:
  check_ci_exclusions.py                     # audit the manifest
  check_ci_exclusions.py --emit-regex core   # print the regex for one lane
  check_ci_exclusions.py --self-test         # prove the gate rejects bad entries
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from datetime import date, datetime
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MANIFEST = Path("tests/ci_exclusions.txt")
WORKFLOW = Path(".github/workflows/ci.yml")
GATE_SCRIPT = Path("scripts/run_release_09_gate.py")
CMAKE_FILES = (Path("CMakeLists.txt"), Path("tests/unit/CMakeLists.txt"),
               Path("tests/fuzz/CMakeLists.txt"))

# The same window known_failures.txt uses. Two suppression channels with two
# different expiry rules would just teach people which one to use.
MAX_AGE_DAYS = 30

VALID_LANES = ("core", "release09", "gate")
# Both this gate and its own mutation self-test. Excluding either one lets
# the manifest stop auditing itself, which is the one exclusion that can
# never be justified.
PROTECTED_NAMES = ("ci_exclusion_discipline",
                   "ci_exclusion_discipline_self_test")

FIELD_RE = {
    "LANE": re.compile(r"LANE=([A-Za-z0-9_,]+)"),
    "ISSUE": re.compile(r"ISSUE=(\S+)"),
    "ADDED": re.compile(r"ADDED=(\S+)"),
    "OWNER": re.compile(r"OWNER=(\S+)"),
    "REASON": re.compile(r"REASON=(.+)$"),
}
NAME_RE = re.compile(r"^(\S+)\s")

# `add_test(NAME foo ...)` and the unit-test macro `add_xray_unit_test(foo ...)`
# are the two ways a name enters ctest here. Matched as text rather than by
# running `ctest -N`, because several of these registrations sit inside
# sanitizer guards: a live query would report a name as missing merely because
# the configured build excludes it.
def _registration_patterns(name: str) -> tuple[re.Pattern[str], ...]:
    quoted = re.escape(name)
    return (
        re.compile(rf"NAME\s+{quoted}\b"),
        re.compile(rf"add_xray_unit_test\(\s*{quoted}\b"),
    )


USE_COLOR = sys.stdout.isatty() and not os.environ.get("NO_COLOR")
RED = "\033[31m" if USE_COLOR else ""
GREEN = "\033[32m" if USE_COLOR else ""
YELLOW = "\033[33m" if USE_COLOR else ""
NC = "\033[0m" if USE_COLOR else ""


def red(message: str) -> None:
    print(f"{RED}{message}{NC}")


def green(message: str) -> None:
    print(f"{GREEN}{message}{NC}")


def yellow(message: str) -> None:
    print(f"{YELLOW}{message}{NC}")


def section(title: str) -> None:
    print("")
    print(f"=== {title} ===")


class Entry:
    """One excluded test and the fields that justify excluding it."""

    def __init__(self, line_number: int, raw: str) -> None:
        self.line_number = line_number
        self.raw = raw
        self.name = ""
        self.lanes: tuple[str, ...] = ()
        self.issue = ""
        self.added: date | None = None
        self.owner = ""
        self.reason = ""
        self.problems: list[str] = []
        self._parse()

    def _parse(self) -> None:
        name_match = NAME_RE.match(self.raw)
        if name_match is None:
            self.problems.append("line does not start with a test name")
            return
        self.name = name_match.group(1)

        for field, pattern in FIELD_RE.items():
            match = pattern.search(self.raw)
            if match is None:
                self.problems.append(f"missing {field}=")
                continue
            value = match.group(1).strip()
            if field == "LANE":
                lanes = tuple(part for part in value.split(",") if part)
                bad = [lane for lane in lanes if lane not in VALID_LANES]
                if bad:
                    self.problems.append(
                        f"unknown lane(s) {','.join(bad)}; "
                        f"valid: {', '.join(VALID_LANES)}")
                self.lanes = lanes
            elif field == "ISSUE":
                self.issue = value
            elif field == "ADDED":
                try:
                    self.added = datetime.strptime(value, "%Y-%m-%d").date()
                except ValueError:
                    self.problems.append(
                        f"ADDED={value} is not YYYY-MM-DD")
            elif field == "OWNER":
                self.owner = value
            elif field == "REASON":
                self.reason = value
                if len(value) < 20:
                    self.problems.append(
                        "REASON is too short to be a reason; say what the "
                        "exclusion costs and what would let it come back")


def read_manifest(path: Path) -> list[Entry]:
    entries: list[Entry] = []
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(),
                                 start=1):
        stripped = raw.strip()
        if not stripped or stripped.startswith("#"):
            continue
        entries.append(Entry(number, stripped))
    return entries


def lane_regex(entries: list[Entry], lane: str) -> str:
    """The anchored alternation ctest -E consumes for one lane."""
    names = sorted({entry.name for entry in entries if lane in entry.lanes})
    if not names:
        # An empty alternation would match nothing, but `^()$` is easy to
        # misread as a mistake. Say it in a way that cannot match a test name.
        return "^(?!)$"
    return "^(" + "|".join(names) + ")$"


def cmake_text() -> str:
    parts = []
    for path in CMAKE_FILES:
        target = REPO_ROOT / path
        if target.is_file():
            parts.append(target.read_text(encoding="utf-8", errors="replace"))
    return "\n".join(parts)


def check_fields(entries: list[Entry]) -> bool:
    section("field completeness")
    malformed = [entry for entry in entries if entry.problems]
    if malformed:
        red(f"FAIL: {len(malformed)} malformed entr(ies):")
        for entry in malformed:
            for problem in entry.problems:
                print(f"  {MANIFEST}:{entry.line_number}: {problem}")
        print("")
        print("  Every line needs all six fields:")
        print("    <test>  LANE=<lanes>  ISSUE=<url>  ADDED=YYYY-MM-DD  "
              "OWNER=<email>  REASON=<prose>")
        return False
    green(f"OK: all {len(entries)} entr(ies) carry six well-formed fields.")
    return True


def check_freshness(entries: list[Entry], today: date) -> bool:
    section(f"freshness (<= {MAX_AGE_DAYS} days)")
    expired = [(entry, (today - entry.added).days)
               for entry in entries
               if entry.added is not None
               and (today - entry.added).days > MAX_AGE_DAYS]
    if expired:
        red(f"FAIL: {len(expired)} exclusion(s) are past the "
            f"{MAX_AGE_DAYS}-day window:")
        for entry, age in expired:
            print(f"  {MANIFEST}:{entry.line_number}: {entry.name} is "
                  f"{age} days old")
        print("")
        print("  Either restore the lane to CI by deleting its line, or")
        print("  re-justify it with a new ADDED= date and a reason that is")
        print("  true today. Permanent exclusions are not allowed.")
        return False
    green(f"OK: all {len(entries)} exclusion(s) were justified within "
          f"{MAX_AGE_DAYS} days.")
    return True


def check_names_exist(entries: list[Entry]) -> bool:
    section("excluded names are real tests")
    text = cmake_text()
    missing = [entry for entry in entries
               if not any(pattern.search(text)
                          for pattern in _registration_patterns(entry.name))]
    if missing:
        red(f"FAIL: {len(missing)} excluded name(s) are not registered by any "
            "CMakeLists:")
        for entry in missing:
            print(f"  {MANIFEST}:{entry.line_number}: {entry.name}")
        print("")
        print("  A name that no longer names a test excludes nothing. Delete")
        print("  the line, or fix the spelling if the test was renamed.")
        return False
    green(f"OK: all {len(entries)} excluded name(s) are registered.")
    return True


def check_workflow_has_no_inline_regex() -> bool:
    section("workflow does not restate the manifest")
    target = REPO_ROOT / WORKFLOW
    if not target.is_file():
        red(f"FAIL: {WORKFLOW} is missing.")
        return False
    text = target.read_text(encoding="utf-8")
    offenders = [
        (number, line)
        for number, line in enumerate(text.splitlines(), start=1)
        if "CTEST_EXCLUDE_RE:" in line or "EXTRA_EXCLUDE_RE:" in line
    ]
    if offenders:
        red(f"FAIL: {WORKFLOW} declares an exclusion regex inline:")
        for number, line in offenders:
            print(f"  {WORKFLOW}:{number}: {line.strip()[:100]}")
        print("")
        print("  The workflow must derive its regexes from the manifest:")
        print("    python3 ./scripts/check_ci_exclusions.py "
              "--emit-regex core")
        print("  A second copy is a second thing to forget to update.")
        return False
    green(f"OK: {WORKFLOW} derives its exclusions from {MANIFEST}.")
    return True


def check_gate_script_agrees(entries: list[Entry]) -> bool:
    section("release-09 gate agrees with the manifest")
    target = REPO_ROOT / GATE_SCRIPT
    if not target.is_file():
        yellow(f"Note: {GATE_SCRIPT} is missing; nothing to reconcile.")
        return True
    text = target.read_text(encoding="utf-8")
    match = re.search(r'KNOWN_AOT_BOUNDARY_RE\s*=\s*r?"([^"]+)"', text)
    if match is None:
        red(f"FAIL: cannot find KNOWN_AOT_BOUNDARY_RE in {GATE_SCRIPT}.")
        return False
    want = lane_regex(entries, "gate")
    if match.group(1) != want:
        red(f"FAIL: {GATE_SCRIPT}'s KNOWN_AOT_BOUNDARY_RE disagrees with the "
            "manifest's `gate` lane.")
        print(f"  script:   {match.group(1)}")
        print(f"  manifest: {want}")
        print("")
        print("  Update whichever is wrong. The gate script hardcodes its own")
        print("  copy; that copy is checked here rather than generated only")
        print("  because the script owns other release policy too.")
        return False
    green(f"OK: {GATE_SCRIPT} matches the manifest's `gate` lane.")
    return True


def check_not_self_excluding(entries: list[Entry]) -> bool:
    section("the gate cannot exclude itself")
    offenders = [entry for entry in entries if entry.name in PROTECTED_NAMES]
    if offenders:
        red(f"FAIL: {MANIFEST} excludes {offenders[0].name}, which is this "
            "check's own ctest entry.")
        for entry in offenders:
            print(f"  {MANIFEST}:{entry.line_number}")
        print("")
        print("  An exclusion list that can exclude its own auditor is not a")
        print("  list, it is a suggestion.")
        return False
    green(f"OK: {' and '.join(PROTECTED_NAMES)} are not excluded.")
    return True


SELF_TEST_MUTATIONS = (
    ("a line with no REASON",
     "some_test  LANE=core  ISSUE=x  ADDED=2026-08-28  OWNER=a@b.c"),
    ("a line with no OWNER",
     "some_test  LANE=core  ISSUE=x  ADDED=2026-08-28  REASON=" + "x" * 30),
    ("a line with no ISSUE",
     "some_test  LANE=core  ADDED=2026-08-28  OWNER=a@b.c  REASON=" + "x" * 30),
    ("an ADDED date that is not a date",
     "some_test  LANE=core  ISSUE=x  ADDED=soon  OWNER=a@b.c  REASON="
     + "x" * 30),
    ("a lane nobody consumes",
     "some_test  LANE=nightly  ISSUE=x  ADDED=2026-08-28  OWNER=a@b.c  "
     "REASON=" + "x" * 30),
    ("a REASON too short to be one",
     "some_test  LANE=core  ISSUE=x  ADDED=2026-08-28  OWNER=a@b.c  "
     "REASON=flaky"),
)


def self_test() -> int:
    """Prove the gate rejects each way an entry can be wrong.

    A checker nobody has tried to fool is a checker nobody knows works. This
    mirrors the mutation self-tests contract_freeze and semantic_owners carry:
    every rule above gets one synthetic entry that breaks exactly it, and the
    parse must object. The expiry rule is checked with --today rather than a
    stale date, so the self-test does not start failing on its own thirty days
    from now.
    """
    failures: list[str] = []
    yellow("Every FAIL line below is the expected output: these are synthetic")
    yellow("entries fed to the gate on purpose, and the gate objecting to them")
    yellow("is the result being asserted. Read the OK lines for the verdict.")

    section("mutation: malformed entries")
    for label, line in SELF_TEST_MUTATIONS:
        entry = Entry(1, line)
        if entry.problems:
            green(f"OK: rejected {label}")
        else:
            red(f"FAIL: accepted {label}")
            print(f"  {line}")
            failures.append(label)

    section("mutation: a well-formed entry is accepted")
    good = Entry(1, "some_test  LANE=core,release09  ISSUE=x  "
                    "ADDED=2026-08-28  OWNER=a@b.c  REASON="
                 + "long enough to say something real" )
    if good.problems:
        red("FAIL: rejected a well-formed entry")
        for problem in good.problems:
            print(f"  {problem}")
        failures.append("well-formed entry")
    else:
        green("OK: accepted a well-formed entry.")

    section("mutation: an expired entry")
    old_entry = Entry(1, "some_test  LANE=core  ISSUE=x  ADDED=2026-01-01  "
                         "OWNER=a@b.c  REASON=" + "x" * 30)
    if check_freshness([old_entry], date(2026, 8, 28)):
        red("FAIL: a 239-day-old exclusion passed the freshness window")
        failures.append("expired entry")
    else:
        green(f"OK: rejected an entry past the {MAX_AGE_DAYS}-day window.")

    section("mutation: a name that is not a registered test")
    ghost = Entry(1, "test_that_does_not_exist_anywhere  LANE=core  ISSUE=x  "
                     "ADDED=2026-08-28  OWNER=a@b.c  REASON=" + "x" * 30)
    if check_names_exist([ghost]):
        red("FAIL: accepted a name no CMakeLists registers")
        failures.append("ghost name")
    else:
        green("OK: rejected a name no CMakeLists registers.")

    section("mutation: the gate excluding itself")
    if check_not_self_excluding([Entry(1, f"{PROTECTED_NAMES[0]}  LANE=core  "
                                          "ISSUE=x  ADDED=2026-08-28  "
                                          "OWNER=a@b.c  REASON=" + "x" * 30)]):
        red("FAIL: the gate allowed itself to be excluded")
        failures.append("self-exclusion")
    else:
        green("OK: rejected the gate excluding itself.")

    section("summary")
    if failures:
        red(f"self-test: {len(failures)} mutation(s) were not caught.")
        return 1
    green("self-test: every mutation was rejected.")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--emit-regex", choices=VALID_LANES, default=None,
                        help="print one lane's ctest --exclude-regex and exit")
    parser.add_argument("--self-test", action="store_true",
                        help="prove the gate rejects each way an entry can be "
                             "wrong, then exit")
    parser.add_argument("--manifest", type=Path, default=MANIFEST,
                        help=f"exclusion manifest (default: {MANIFEST})")
    parser.add_argument("--today", default=None,
                        help="override today's date as YYYY-MM-DD, for tests")
    args = parser.parse_args(argv[1:])

    os.chdir(REPO_ROOT)
    if args.self_test:
        return self_test()
    if not args.manifest.is_file():
        red(f"FAIL: {args.manifest} is missing.")
        return 1
    entries = read_manifest(args.manifest)

    if args.emit_regex:
        # Emitting must stay silent apart from the regex: the workflow reads
        # this on stdout.
        print(lane_regex(entries, args.emit_regex))
        return 0

    today = (datetime.strptime(args.today, "%Y-%m-%d").date()
             if args.today else date.today())

    section("manifest")
    if not entries:
        green(f"OK: {args.manifest} excludes nothing. Every ctest lane gates "
              "pull requests.")
        return 0
    green(f"Parsed {len(entries)} exclusion(s) from {args.manifest}.")
    for lane in VALID_LANES:
        count = sum(1 for entry in entries if lane in entry.lanes)
        print(f"  {lane}: {count}")

    ok = check_fields(entries)
    ok = check_freshness(entries, today) and ok
    ok = check_names_exist(entries) and ok
    ok = check_workflow_has_no_inline_regex() and ok
    ok = check_gate_script_agrees(entries) and ok
    ok = check_not_self_excluding(entries) and ok

    section("summary")
    if not ok:
        red("CI exclusion discipline: one or more checks failed.")
        return 1
    green(f"CI exclusion discipline: {len(entries)} justified exclusion(s), "
          f"all within {MAX_AGE_DAYS} days.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

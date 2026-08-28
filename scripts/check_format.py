#!/usr/bin/env python3
"""Enforce .clang-format on C sources without reformatting the whole tree.

This replaces the pre-commit hook that used to run `clang-format -i` on staged
files. That hook was removed because it rewrote files after the commit had been
composed: contract anchors went stale immediately, and every lane that touched a
shared source file got a reformat of lines it never wrote, which showed up as
merge conflicts against every other lane. Formatting is a check now, not a
mutation, and it runs where a check belongs.

Two modes, because the tree carries real debt:

  ratchet (default)
      Scan every non-generated .c/.h under src/, include/ and stdlib/ and
      compare the set of unformatted files against tests/format_debt.txt.
      A file that is unformatted and not listed fails. A file that is listed
      and now formatted also fails -- the baseline only shrinks, so a fixed
      file must leave it in the same commit that fixed it. This catches new
      files and regressions in files that were already clean, and it does not
      ask anybody to reformat 300 files they did not write.

  changed --base <rev>
      Format-check only the lines this branch added or modified relative to
      <rev>, using clang-format --lines. This is the half the ratchet cannot
      see: a new, badly formatted line inside a file that is already listed as
      debt. Run it in CI against the merge base.

Neither mode ever writes a source file.

Exit 0 when every check passes, 1 otherwise.

Usage:
  check_format.py                          # ratchet against the baseline
  check_format.py --mode changed --base origin/main
  check_format.py --write-baseline         # regenerate tests/format_debt.txt
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BASELINE_FILE = Path("tests/format_debt.txt")
SCAN_DIRS = ("src", "include", "stdlib")
SOURCE_SUFFIXES = (".c", ".h")

# One version formats this tree. Two clang-format releases do not always agree,
# so a check that takes whichever binary the PATH happens to surface first
# reports drift that only exists between the two binaries.
REQUIRED_MAJOR = 22

# ctest reads this as SKIP rather than a failure; see --skip-if-missing.
SKIP_EXIT = 77

# Generated artifacts must stay byte-for-byte identical to their generator
# output; the generator, not clang-format, owns their spelling.
GENERATED_MARKERS = ("auto-generated", "automatically generated", "@generated")

HUNK_HEADER = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")
VERSION_LINE = re.compile(r"version (\d+)\.")

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


class ToolMissing(Exception):
    """The pinned clang-format is absent or the wrong release."""


def resolve_clang_format() -> str:
    """The pinned binary, or ToolMissing with the reason already printed."""
    binary = shutil.which("clang-format")
    if binary is None:
        binary = shutil.which(f"clang-format-{REQUIRED_MAJOR}")
    if binary is None:
        red(f"clang-format not found; this tree formats with "
            f"{REQUIRED_MAJOR}.")
        print("  macOS:  brew install clang-format")
        print(f"  Debian: apt install clang-format-{REQUIRED_MAJOR}")
        print(f"  pip:    pip install 'clang-format~={REQUIRED_MAJOR}.1'")
        raise ToolMissing()

    probe = subprocess.run([binary, "--version"], capture_output=True,
                           text=True)
    match = VERSION_LINE.search(probe.stdout)
    if match is None:
        red(f"cannot read a version out of `{binary} --version`:")
        print(f"  {probe.stdout.strip()}")
        raise ToolMissing()
    found = int(match.group(1))
    if found != REQUIRED_MAJOR:
        red(f"found clang-format {found}, but this tree formats with "
            f"{REQUIRED_MAJOR}.")
        print("  A different release reports drift in lines nobody touched.")
        raise ToolMissing()
    return binary


def is_generated(path: Path) -> bool:
    try:
        first = path.open("r", errors="replace").readline().lower()
    except OSError:
        return False
    return any(marker in first for marker in GENERATED_MARKERS)


def source_files() -> list[Path]:
    """Every non-generated C source the format rules apply to, repo-relative."""
    found: list[Path] = []
    for directory in SCAN_DIRS:
        root = REPO_ROOT / directory
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
                continue
            if is_generated(path):
                continue
            found.append(path.relative_to(REPO_ROOT))
    return sorted(found)


def formatted_text(binary: str, path: Path,
                   line_ranges: list[tuple[int, int]] | None = None) -> str:
    """What clang-format would produce; the file itself is never written."""
    command = [binary]
    for start, end in line_ranges or []:
        command.append(f"--lines={start}:{end}")
    command.append(str(path))
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"clang-format failed on {path}: {result.stderr.strip()}")
    return result.stdout


def is_unformatted(binary: str, path: Path) -> bool:
    current = (REPO_ROOT / path).read_text(encoding="utf-8", errors="replace")
    return formatted_text(binary, REPO_ROOT / path) != current


def scan_unformatted(binary: str, paths: list[Path]) -> list[Path]:
    """Files whose full text clang-format would change, in scan order.

    Threaded because this is 1500 subprocess round trips; the work is entirely
    in the child processes, so the GIL is not in the way.
    """
    from concurrent.futures import ThreadPoolExecutor

    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        verdicts = list(pool.map(lambda p: is_unformatted(binary, p), paths))
    return [path for path, bad in zip(paths, verdicts) if bad]


def read_baseline() -> tuple[list[str], list[Path]]:
    """(header lines, listed paths). A missing file is an empty baseline."""
    target = REPO_ROOT / BASELINE_FILE
    if not target.is_file():
        return ([], [])
    header: list[str] = []
    entries: list[Path] = []
    for line in target.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            if not entries:
                header.append(line)
            continue
        entries.append(Path(stripped))
    return (header, entries)


def git_changed_line_ranges(base: str) -> dict[Path, list[tuple[int, int]]]:
    """Lines this branch added or modified, per file, from `git diff -U0`.

    Three-dot so a stale base branch does not drag in other people's lines:
    the comparison is against the merge base, which is what review sees.
    """
    command = ["git", "diff", "-U0", f"{base}...HEAD", "--"]
    command += [f"*{suffix}" for suffix in SOURCE_SUFFIXES]
    result = subprocess.run(command, capture_output=True, text=True,
                            cwd=REPO_ROOT)
    if result.returncode != 0:
        raise RuntimeError(
            f"`git diff` against {base} failed: {result.stderr.strip()}")

    ranges: dict[Path, list[tuple[int, int]]] = {}
    current: Path | None = None
    for line in result.stdout.splitlines():
        if line.startswith("+++ b/"):
            current = Path(line[len("+++ b/"):])
            continue
        if line.startswith("+++ /dev/null"):
            current = None
            continue
        if current is None:
            continue
        match = HUNK_HEADER.match(line)
        if match is None:
            continue
        start = int(match.group(1))
        count = 1 if match.group(2) is None else int(match.group(2))
        if count == 0:
            # A pure deletion adds no line to check.
            continue
        ranges.setdefault(current, []).append((start, start + count - 1))
    return ranges


def unified_diff(path: Path, want: str) -> str:
    have = (REPO_ROOT / path).read_text(encoding="utf-8", errors="replace")
    import difflib
    return "".join(difflib.unified_diff(
        have.splitlines(keepends=True), want.splitlines(keepends=True),
        fromfile=f"a/{path}", tofile=f"b/{path}"))


def write_baseline(binary: str) -> int:
    paths = source_files()
    unformatted = scan_unformatted(binary, paths)
    body = "\n".join(str(path) for path in unformatted)
    target = REPO_ROOT / BASELINE_FILE
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(BASELINE_HEADER + body + "\n", encoding="utf-8")
    green(f"Wrote {BASELINE_FILE} with {len(unformatted)} entr(ies) out of "
          f"{len(paths)} scanned file(s).")
    yellow("This list only shrinks. Adding to it needs a reason in review.")
    return 0


BASELINE_HEADER = """\
# Files that predate the format check and do not satisfy .clang-format.
#
# This list only shrinks. It exists because the removed pre-commit hook
# formatted staged files only, so a file that was never staged since the hook
# landed was never formatted -- 335 of them at the time this baseline was
# taken. Reformatting them in one commit would collide with every branch in
# flight, so they are recorded as debt instead of rewritten.
#
# scripts/check_format.py enforces both directions:
#   - a file that is unformatted and NOT listed here fails the gate
#   - a file that is listed here and now formatted ALSO fails, so a fix and
#     its baseline removal land in the same commit
#
# To clear an entry: run `clang-format -i <file>` and delete its line. Do that
# in a commit that changes nothing else, so review can skip it.
#
# New lines are governed separately: CI runs
# `check_format.py --mode changed --base <merge-base>`, which format-checks
# only the lines a branch touched. Being listed here does not license new
# unformatted code inside the file.
"""


def run_ratchet(binary: str) -> int:
    paths = source_files()
    section("scan")
    green(f"Scanning {len(paths)} non-generated source file(s) under "
          f"{', '.join(SCAN_DIRS)}/.")

    unformatted = set(scan_unformatted(binary, paths))
    _, listed = read_baseline()
    listed_set = set(listed)

    failed = False

    section("new unformatted files")
    new_bad = sorted(unformatted - listed_set)
    if new_bad:
        red(f"FAIL: {len(new_bad)} file(s) do not satisfy .clang-format and "
            f"are not in {BASELINE_FILE}:")
        for path in new_bad:
            print(f"  {path}")
        print("")
        print("  Run `clang-format -i <file>` on each. Do not add them to the")
        print("  baseline; that list is for files that predate this gate.")
        failed = True
    else:
        green("OK: every unformatted file is a recorded baseline entry.")

    section("stale baseline entries")
    missing = sorted(path for path in listed_set
                     if not (REPO_ROOT / path).is_file())
    fixed = sorted((listed_set - unformatted) - set(missing))
    if fixed:
        red(f"FAIL: {len(fixed)} baseline entr(ies) now satisfy "
            ".clang-format:")
        for path in fixed:
            print(f"  {path}")
        print("")
        print(f"  Delete these line(s) from {BASELINE_FILE}. The baseline only")
        print("  shrinks; leaving a fixed file listed lets it rot again "
              "unnoticed.")
        failed = True
    if missing:
        red(f"FAIL: {len(missing)} baseline entr(ies) name a file that no "
            "longer exists:")
        for path in missing:
            print(f"  {path}")
        failed = True
    if not fixed and not missing:
        green(f"OK: all {len(listed_set)} baseline entr(ies) are still real "
              "and still unformatted.")

    section("summary")
    if failed:
        red("format ratchet: one or more checks failed.")
        return 1
    green(f"format ratchet: {len(paths) - len(unformatted)} clean, "
          f"{len(unformatted)} in recorded debt.")
    return 0


def run_changed(binary: str, base: str) -> int:
    section("changed lines")
    try:
        ranges = git_changed_line_ranges(base)
    except RuntimeError as error:
        red(f"FAIL: {error}")
        print(f"  Fetch enough history for {base} to exist "
              "(actions/checkout needs fetch-depth: 0).")
        return 1

    checkable: dict[Path, list[tuple[int, int]]] = {}
    for path, line_ranges in sorted(ranges.items()):
        absolute = REPO_ROOT / path
        if not absolute.is_file():
            continue
        if absolute.suffix not in SOURCE_SUFFIXES:
            continue
        if is_generated(absolute):
            continue
        if not any(str(path).startswith(f"{d}/") for d in SCAN_DIRS):
            continue
        checkable[path] = line_ranges

    if not checkable:
        green(f"OK: no non-generated C source lines changed since {base}.")
        return 0
    green(f"Checking {len(checkable)} changed file(s) against {base}, "
          "changed lines only.")

    offenders: list[tuple[Path, str]] = []
    for path, line_ranges in checkable.items():
        want = formatted_text(binary, REPO_ROOT / path, line_ranges)
        if want != (REPO_ROOT / path).read_text(encoding="utf-8",
                                                errors="replace"):
            offenders.append((path, unified_diff(path, want)))

    section("verdict")
    if offenders:
        red(f"FAIL: {len(offenders)} file(s) have changed lines that do not "
            "satisfy .clang-format:")
        for path, diff in offenders:
            print(f"  {path}")
        print("")
        for _, diff in offenders:
            print(diff)
        print("  Fix with:")
        print(f"    scripts/check_format.py --mode changed --base {base} "
              "--fix")
        return 1
    green("OK: every changed line satisfies .clang-format.")
    return 0


def run_changed_fix(binary: str, base: str) -> int:
    """Apply the changed-lines formatting in place. Never used by CI."""
    try:
        ranges = git_changed_line_ranges(base)
    except RuntimeError as error:
        red(f"FAIL: {error}")
        return 1
    fixed = 0
    for path, line_ranges in sorted(ranges.items()):
        absolute = REPO_ROOT / path
        if not absolute.is_file() or is_generated(absolute):
            continue
        if not any(str(path).startswith(f"{d}/") for d in SCAN_DIRS):
            continue
        command = [binary, "-i"]
        command += [f"--lines={start}:{end}" for start, end in line_ranges]
        command.append(str(absolute))
        subprocess.run(command, check=True)
        fixed += 1
    green(f"Formatted the changed lines of {fixed} file(s).")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Check C sources against .clang-format.")
    parser.add_argument("--mode", choices=("ratchet", "changed"),
                        default="ratchet",
                        help="ratchet: whole tree against the debt baseline. "
                             "changed: only lines touched since --base.")
    parser.add_argument("--base", default="origin/main",
                        help="Revision to diff against in changed mode.")
    parser.add_argument("--fix", action="store_true",
                        help="changed mode only: format the changed lines in "
                             "place instead of reporting them.")
    parser.add_argument("--write-baseline", action="store_true",
                        help=f"Regenerate {BASELINE_FILE} from the current "
                             "tree. Review the diff; it must only shrink.")
    parser.add_argument("--skip-if-missing", action="store_true",
                        help=f"Exit {SKIP_EXIT} (ctest's SKIP_RETURN_CODE) "
                             "instead of failing when the pinned clang-format "
                             "is absent. For the ctest entry, where a "
                             "developer machine may not have it; never for "
                             "CI, which installs it.")
    args = parser.parse_args(argv[1:])

    os.chdir(REPO_ROOT)
    try:
        binary = resolve_clang_format()
    except ToolMissing:
        if args.skip_if_missing:
            yellow(f"SKIP: the format check needs clang-format "
                   f"{REQUIRED_MAJOR}.")
            return SKIP_EXIT
        red("FAIL: the format check cannot run without its pinned tool.")
        return 1

    if args.write_baseline:
        return write_baseline(binary)
    if args.mode == "changed":
        if args.fix:
            return run_changed_fix(binary, args.base)
        return run_changed(binary, args.base)
    if args.fix:
        red("FAIL: --fix applies to --mode changed only.")
        print("  The ratchet never rewrites files; that is the whole point.")
        return 1
    return run_ratchet(binary)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

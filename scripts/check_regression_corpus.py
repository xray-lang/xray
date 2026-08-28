#!/usr/bin/env python3
"""Gate the tests/regression corpus against its only-shrink baseline.

The corpus is the deepest behavioural evidence this repository has -- 522 files
and roughly 3200 `@test` declarations -- and until this gate existed it had no
blocking entry on a pull request. `scripts/run_regression_tests.py` had zero
CMakeLists references, so the only place the whole corpus ran was the nightly
t3 tier: a Debug build, one day late, while local development defaults to
Release. A stdlib regression that broke any of those assertions could merge
cleanly and be found the next morning by whoever read the nightly.

This script is the ctest entry the corpus was missing. It runs the corpus,
reads the runner's structured report rather than scraping its localized console
summary, and applies the shared only-shrink ratchet from
tests/lib/xraytest/ratchet.py:

  - a failure NOT in tests/regression/baseline_failures.txt fails the run;
  - a baseline entry that starts passing also fails, so the fix and its
    baseline deletion land in the same change.

The cross-backend differential net is deliberately NOT folded in. The runner
offers it, but backend_diff already has its own ctest entry with its own
baseline, and running it twice costs about four minutes to reach a verdict the
suite already has. Pass --with-backend-diff to opt back in.

Exit 0 when the corpus matches its baseline, 1 otherwise.

Usage:
  check_regression_corpus.py --xray build/xray
  check_regression_corpus.py --jobs 4 --timeout 20
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


def _bootstrap() -> None:
    lib = REPO_ROOT / "tests" / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import ratchet  # noqa: E402

RUNNER = Path("scripts/run_regression_tests.py")
BASELINE_FILE = Path("tests/regression/baseline_failures.txt")
NONDETERMINISTIC_FILE = Path("tests/regression/nondeterministic_cases.txt")

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


def run_corpus(args: argparse.Namespace, report: Path) -> tuple[int, str]:
    """Run the corpus runner, returning (exit code, combined output)."""
    env = dict(os.environ)
    env["XRAY_SKIP_BUILD"] = "1"
    env["XRAY_SKIP_BACKEND_DIFF"] = "0" if args.with_backend_diff else "1"
    if args.xray:
        env["XRAY_PATH"] = str(Path(args.xray).resolve())
    if args.build_dir:
        env["XRAY_BUILD_DIR"] = str(Path(args.build_dir).resolve())
    if args.jobs:
        env["XRAY_TEST_JOBS"] = str(args.jobs)
    if args.timeout:
        env["XRAY_TEST_TIMEOUT"] = str(args.timeout)
    if args.dump_failed:
        env["XRAY_TEST_DUMP_FAILED"] = "1"

    command = [sys.executable, str(REPO_ROOT / RUNNER), "--json", str(report)]
    result = subprocess.run(command, cwd=REPO_ROOT, env=env,
                            capture_output=True, text=True)
    return (result.returncode, result.stdout + result.stderr)


def echo_summary(output: str) -> None:
    """Reprint the runner's own tally lines; it owns those numbers."""
    for line in output.splitlines():
        stripped = line.strip()
        if stripped.startswith(("总文件数", "执行测试", "通过", "失败", "耗时")):
            print(f"  {stripped}")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--xray", default=None,
                        help="xray binary to test with (default: the runner's "
                             "own build-directory search)")
    parser.add_argument("--build-dir", default=None,
                        help="build directory holding the xray binary")
    parser.add_argument("--baseline", type=Path, default=BASELINE_FILE,
                        help=f"ratchet baseline (default: {BASELINE_FILE})")
    parser.add_argument("--nondeterministic", type=Path,
                        default=NONDETERMINISTIC_FILE,
                        help="cases whose verdict is not reproducible; they "
                             "are passed to the ratchet as skipped, so neither "
                             "outcome is read as a change "
                             f"(default: {NONDETERMINISTIC_FILE})")
    parser.add_argument("--jobs", type=int, default=None,
                        help="corpus parallelism (default: CPU count)")
    parser.add_argument("--timeout", type=int, default=None,
                        help="per-case seconds (default: 10)")
    parser.add_argument("--with-backend-diff", action="store_true",
                        help="also run the VM/AOT differential net, which has "
                             "its own ctest entry and its own baseline")
    parser.add_argument("--dump-failed", action="store_true",
                        help="dump each failing case's output")
    parser.add_argument("--rebaseline", action="store_true",
                        help="rewrite the baseline's entries from this run. "
                             "The header is preserved; review the diff.")
    args = parser.parse_args(argv[1:])

    os.chdir(REPO_ROOT)
    if not (REPO_ROOT / RUNNER).is_file():
        red(f"FAIL: {RUNNER} is missing.")
        return 1

    section("corpus run")
    with tempfile.TemporaryDirectory(prefix="xray_regression_gate_") as tmp:
        report = Path(tmp) / "regression.json"
        code, output = run_corpus(args, report)
        if not report.is_file():
            red("FAIL: the runner produced no JSON report.")
            print(f"  exit code {code}; its output follows.")
            sys.stdout.write(output)
            return 1
        payload = json.loads(report.read_text(encoding="utf-8"))

    echo_summary(output)
    failed = set(payload["failed_tests"])

    if args.rebaseline:
        return rewrite_baseline(args.baseline, sorted(failed), payload)

    section("ratchet")
    baseline = ratchet.read_baseline(args.baseline)
    # A case that decides at random produced no verdict worth reading, which is
    # precisely ratchet.py's definition of skipped: neither a new failure nor
    # evidence that a baselined entry is fixed. Judging one would make this gate
    # report a coin flip.
    unjudgeable = ratchet.read_baseline(args.nondeterministic)
    stale = sorted(unjudgeable - baseline)
    if stale:
        red(f"FAIL: {len(stale)} entr(ies) in {args.nondeterministic} are not "
            f"in {args.baseline}:")
        for name in stale:
            print(f"  {name}")
        print("")
        print("  A case can only be unjudgeable if it is also allowed to fail.")
        print("  Add it to the baseline, or drop it from the other list.")
        return 1
    if unjudgeable:
        yellow(f"{len(unjudgeable)} case(s) are recorded as nondeterministic "
               "and are not judged:")
        for name in sorted(unjudgeable):
            landed = "failed" if name in failed else "passed"
            print(f"  {name} ({landed} this run)")
    verdict = ratchet.evaluate(failed=failed, baseline=baseline,
                               skipped=unjudgeable)

    if verdict.new_failures:
        red(f"FAIL: {len(verdict.new_failures)} case(s) fail and are not in "
            f"{args.baseline}:")
        for name in verdict.new_failures:
            print(f"  {name}")
        print("")
        print("  Fix the case. Adding a line to the baseline instead needs a")
        print("  written reason, and the header records what each group is.")
        print("  Reproduce one case with:")
        print("    build/xray test tests/regression/<dir>/<case>.xr")
    if verdict.now_passing:
        red(f"FAIL: {len(verdict.now_passing)} baseline entr(ies) now pass:")
        for name in verdict.now_passing:
            print(f"  {name}")
        print("")
        print(f"  Delete those line(s) from {args.baseline} in the change that")
        print("  fixed them. The baseline only shrinks; a stale allowance is a")
        print("  place a future regression can hide.")
    if verdict.ok:
        green(f"OK: {verdict.failing_count} failing, all "
              f"{verdict.baseline_count} baselined, none newly passing.")

    section("summary")
    if not verdict.ok:
        red("regression corpus: the baseline no longer describes this tree.")
        return 1
    green(f"regression corpus: {payload['passed']} passed, "
          f"{payload['executed']} assertions executed, "
          f"{payload['elapsed_seconds']}s.")
    return 0


def rewrite_baseline(path: Path, failed: list[str],
                     payload: dict) -> int:
    """Replace the baseline's entries, keeping its leading header prose.

    Only the header above the first entry survives. Section headings placed
    BETWEEN entries -- the `--- exposed-on-gate-install ---` grouping, for
    instance -- are lost, because this cannot know which new entry belongs to
    which group. That is called out loudly below rather than guessed at: a
    grouping silently flattened is worse than one the caller is told to
    restore.
    """
    header: list[str] = []
    interior_comments = 0
    seen_entry = False
    if path.is_file():
        for line in path.read_text(encoding="utf-8").splitlines():
            is_entry = bool(line.strip()) and not line.lstrip().startswith("#")
            if is_entry:
                seen_entry = True
                continue
            if seen_entry:
                if line.strip():
                    interior_comments += 1
                continue
            header.append(line)
    body = "\n".join(failed)
    path.write_text("\n".join(header) + "\n" + body + "\n", encoding="utf-8")
    yellow(f"Rewrote {path} with {len(failed)} entr(ies) out of "
           f"{payload['total_files']} case(s).")
    yellow("The header still describes the previous capture. Update its counts")
    yellow("and its classification before committing.")
    if interior_comments:
        yellow(f"{interior_comments} comment line(s) sat between entries and "
               "were dropped;")
        yellow("`git diff` shows them. Re-group the new entries by hand.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

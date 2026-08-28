#!/usr/bin/env python3
"""Liveness differential: both backends must make progress, not merely agree.

The ordinary differential net compares the byte-for-byte output of programs
that TERMINATE. A program that never terminates has no output to compare, so a
progress divergence -- the VM finishes while the AOT binary spins forever --
was invisible to it, and one such divergence survived thirteen months without a
test turning red. This runner closes that class: every case must produce its
expected observable ON BOTH BACKENDS within a wall-clock budget, and a backend
still running when the budget expires fails the case. That is the whole point;
a timeout here is a verdict, not an infrastructure hiccup.

Case layout: tests/diff/cases/liveness/<name>.xr with a sidecar
tests/diff/cases/liveness/<name>.live:

  timeout=SECONDS          wall-clock budget per backend run (required)
  exit=N | exit=nonzero    expected exit code (default: 0)
  contains=SUBSTRING       required in that backend's combined output
                           (repeatable; checked on both backends)
  env=NAME=VALUE           extra environment for both runs (repeatable); how
                           the single-worker profile variants pin N=1
  vm_only=1                skip the AOT half (documented reason required in
                           the case header) -- use sparingly

Blank lines and `#` comments are ignored. Everything else is parsed strictly:
an unknown field, a repeated single-valued field, a malformed value, a missing
`timeout=`, or an .xr without its .live (or the reverse) is a hard failure
naming file and line. A lenient parser lets a typo silently disable the very
assertion the case was written to make.

Three properties this runner does not inherit from its shell predecessor:

  - It never exits 0 for want of a precondition. A missing xray binary is exit
    1, not a printed SKIP that CTest would record as green. A gate whose whole
    purpose is catching a silent non-failure must not have one of its own.
  - The wall-clock budget is enforced in-process rather than by `timeout(1)`,
    which does not exist on a stock macOS or Windows host. proc.run kills the
    whole child process group on expiry, so the workers and children an `xray
    run` spawned do not outlive the case that started them.
  - The AOT half is skipped, and counted as skipped in the verdict line, when
    the host has no C compiler at all. That is distinct from a build failure
    with a compiler present, which stays a hard failure: the difference is
    whether the host cannot build anything or the compiler rejected this
    program. The probe, not the build log, answers that -- a build log is the
    wrong place to learn what the host has.

Usage: run_liveness_diff.py [xray]
"""

from __future__ import annotations

import argparse
import os
import sys
from dataclasses import dataclass
from pathlib import Path


def _bootstrap() -> None:
    """Put tests/lib on sys.path so `import xraytest` works without an install."""
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, ratchet, toolchain, workspace  # noqa: E402

platform.configure_utf8_stdio()

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent
CASE_DIR = SCRIPT_DIR / "cases" / "liveness"
BASELINE_FILE = SCRIPT_DIR / "liveness_known_failures.txt"

# Fields a sidecar may set, and which of them may appear more than once.
SINGLE_VALUED_FIELDS = ("timeout", "exit", "vm_only")
REPEATABLE_FIELDS = ("contains", "env")
KNOWN_FIELDS = SINGLE_VALUED_FIELDS + REPEATABLE_FIELDS

# Exit codes a shell's `timeout` reports for an expired budget. We enforce the
# budget ourselves now, but a case may still be killed by an inner supervisor,
# and that is the same verdict: no progress.
TIMEOUT_EXIT_CODES = (124, 137)

# Name column width, and how much of a failing run's output is quoted back.
_NAME_WIDTH = 44
_OUTPUT_PREVIEW_LINES = 6
_BUILD_LOG_TAIL_LINES = 4

# A native build is not the thing under budget: the case's `timeout=` is the
# run's wall clock, typically ten seconds, while compiling and linking a binary
# legitimately takes longer. The build gets the suite-wide ceiling instead.
DEFAULT_BUILD_TIMEOUT = 300


class SidecarError(Exception):
    """A sidecar could not be read as written. Carries a file:line location."""


@dataclass(frozen=True)
class LivenessCase:
    """One .xr plus the parsed contract its .live sidecar states."""

    name: str
    source: Path
    sidecar: Path
    budget: float
    # None means `exit=nonzero`: any non-zero code satisfies the case.
    expect_exit: "int | None"
    expect_exit_text: str
    contains: "tuple[str, ...]"
    env: "tuple[tuple[str, str], ...]"
    vm_only: bool

    def child_env(self) -> "dict[str, str]":
        """The parent environment plus this case's pinned variables."""
        env = os.environ.copy()
        env.update(dict(self.env))
        return env

    def describe_fields(self) -> "list[str]":
        """Sidecar fields in a stable order, for `--list`."""
        fields = [f"timeout={self.budget:g}", f"exit={self.expect_exit_text}"]
        fields.extend(f"contains={needle}" for needle in self.contains)
        fields.extend(f"env={name}={value}" for name, value in self.env)
        if self.vm_only:
            fields.append("vm_only=1")
        return fields


def parse_sidecar(source: Path, sidecar: Path) -> LivenessCase:
    """Read one .live file strictly, or raise SidecarError naming file:line.

    Strictness is the point. These sidecars are the only statement of what a
    case proves, and every lenient reading of them -- an unrecognized key
    ignored, a second `timeout=` silently dropped -- turns a typo into a case
    that runs and asserts nothing.
    """
    try:
        text = sidecar.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise SidecarError(f"{sidecar}: cannot read sidecar: {exc}") from exc

    budget: "float | None" = None
    expect_exit: "int | None" = 0
    expect_exit_text = "0"
    contains: "list[str]" = []
    env: "list[tuple[str, str]]" = []
    vm_only = False
    seen: "set[str]" = set()

    for lineno, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        here = f"{sidecar}:{lineno}"
        if "=" not in line:
            raise SidecarError(f"{here}: not a NAME=VALUE line: {line!r}")
        key, value = line.split("=", 1)
        key = key.strip()
        if key not in KNOWN_FIELDS:
            raise SidecarError(
                f"{here}: unknown field {key!r} "
                f"(known: {', '.join(sorted(KNOWN_FIELDS))})"
            )
        if key in SINGLE_VALUED_FIELDS and key in seen:
            raise SidecarError(f"{here}: field {key!r} set more than once")
        seen.add(key)

        if key == "timeout":
            try:
                budget = float(value)
            except ValueError:
                raise SidecarError(f"{here}: timeout is not a number: {value!r}") from None
            if not budget > 0:
                raise SidecarError(f"{here}: timeout must be positive: {value!r}")
        elif key == "exit":
            if value == "nonzero":
                expect_exit = None
            elif value.isdigit():
                expect_exit = int(value)
            else:
                raise SidecarError(
                    f"{here}: exit must be a non-negative integer or 'nonzero': {value!r}"
                )
            expect_exit_text = value
        elif key == "contains":
            if not value:
                raise SidecarError(f"{here}: contains needs a non-empty substring")
            contains.append(value)
        elif key == "env":
            if "=" not in value:
                raise SidecarError(f"{here}: env must be NAME=VALUE: {value!r}")
            env_name, env_value = value.split("=", 1)
            if not env_name:
                raise SidecarError(f"{here}: env has an empty variable name: {value!r}")
            env.append((env_name, env_value))
        elif key == "vm_only":
            if value not in ("0", "1"):
                raise SidecarError(f"{here}: vm_only must be 0 or 1: {value!r}")
            vm_only = value == "1"

    if budget is None:
        raise SidecarError(f"{sidecar}: sidecar has no timeout= budget")

    return LivenessCase(
        name=source.stem,
        source=source,
        sidecar=sidecar,
        budget=budget,
        expect_exit=expect_exit,
        expect_exit_text=expect_exit_text,
        contains=tuple(contains),
        env=tuple(env),
        vm_only=vm_only,
    )


def collect_cases(case_dir: Path) -> "tuple[list[LivenessCase], list[str]]":
    """Pair every .xr with its .live and parse them, collecting all errors.

    An orphan on either side is an error rather than a skip: a .live with no .xr
    is a case that was deleted or renamed without its contract, and an .xr with
    no .live is a case that would otherwise run under no assertion at all.
    """
    cases: "list[LivenessCase]" = []
    errors: "list[str]" = []

    sources = sorted(case_dir.glob("*.xr"))
    sidecars = sorted(case_dir.glob("*.live"))
    source_stems = {path.stem for path in sources}
    for sidecar in sidecars:
        if sidecar.stem not in source_stems:
            errors.append(f"{sidecar}: sidecar has no matching {sidecar.stem}.xr")

    for source in sources:
        sidecar = source.with_suffix(".live")
        if not sidecar.is_file():
            errors.append(f"{source}: missing {source.stem}.live sidecar")
            continue
        try:
            cases.append(parse_sidecar(source, sidecar))
        except SidecarError as exc:
            errors.append(str(exc))
    return cases, errors


@dataclass(frozen=True)
class HalfResult:
    """One backend's run of one case: the verdict plus what it printed."""

    ok: bool
    detail: str
    output: str


def judge_run(label: str, case: LivenessCase, result: proc.ProcResult) -> HalfResult:
    """Decide one backend's half of a case from its exit code and output."""
    output = result.combined_text()
    if result.timed_out or result.returncode in TIMEOUT_EXIT_CODES:
        return HalfResult(
            False,
            f"{label}: no progress within {case.budget:g}s budget -- killed",
            output,
        )
    if case.expect_exit is None:
        if result.returncode == 0:
            return HalfResult(False, f"{label}: expected nonzero exit, got 0", output)
    elif result.returncode != case.expect_exit:
        return HalfResult(
            False,
            f"{label}: exit {result.returncode}, expected {case.expect_exit}",
            output,
        )
    for needle in case.contains:
        if needle not in output:
            return HalfResult(False, f"{label}: output missing: {needle}", output)
    return HalfResult(True, "", output)


def run_vm_half(xray: Path, case: LivenessCase) -> HalfResult:
    result = proc.run(
        [xray, "run", case.source],
        env=case.child_env(),
        timeout=case.budget,
    )
    return judge_run("vm", case, result)


def run_aot_half(
    xray: Path, case: LivenessCase, work_dir: Path, build_timeout: "float | None"
) -> HalfResult:
    """Build the case natively, then run the binary under the same budget."""
    binary = work_dir / f"{case.name}.bin"
    build = proc.run(
        [xray, "build", "--native", case.source, "-o", binary],
        cwd=work_dir,
        timeout=build_timeout,
    )
    if not build.ok:
        state = "timed out" if build.timed_out else f"exit {build.returncode}"
        log = build.combined_text().splitlines()[-_BUILD_LOG_TAIL_LINES:]
        return HalfResult(False, f"aot build failed ({state})", "\n".join(log))
    result = proc.run([binary], env=case.child_env(), timeout=case.budget)
    return judge_run("aot", case, result)


class Recorder:
    """Per-case verdicts and the counts the summary line reports."""

    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0
        self.skipped = 0
        # Names, not just counts: the ratchet needs to know which cases these
        # were to compare them against the baseline.
        self.failed_names: "list[str]" = []
        self.skipped_names: "list[str]" = []

    def ok(self, case: LivenessCase, note: str = "") -> None:
        suffix = f" ({note})" if note else ""
        print(f"  {case.name:<{_NAME_WIDTH}} PASS{suffix}")
        self.passed += 1

    def skip(self, case: LivenessCase, reason: str) -> None:
        print(f"  {case.name:<{_NAME_WIDTH}} SKIP ({reason})")
        self.skipped += 1
        self.skipped_names.append(case.name)

    def bad(self, case: LivenessCase, half: HalfResult) -> None:
        print(f"  {case.name:<{_NAME_WIDTH}} FAIL ({half.detail})")
        for line in half.output.splitlines()[:_OUTPUT_PREVIEW_LINES]:
            print(f"      | {line}")
        self.failed += 1
        self.failed_names.append(case.name)


def run_case(
    rec: Recorder,
    xray: Path,
    case: LivenessCase,
    work_dir: Path,
    vm_only_lane: bool,
    have_cc: bool,
    build_timeout: "float | None",
) -> None:
    vm = run_vm_half(xray, case)
    if not vm.ok:
        rec.bad(case, vm)
        return
    if case.vm_only:
        rec.ok(case, "vm only")
        return
    if vm_only_lane:
        rec.ok(case, "vm half only: --vm-only lane")
        return
    if not have_cc:
        # The VM half already passed; only the AOT half is unverifiable here.
        rec.skip(case, "vm passed, aot skipped: no host toolchain")
        return
    aot = run_aot_half(xray, case, work_dir, build_timeout)
    if not aot.ok:
        rec.bad(case, aot)
        return
    rec.ok(case)


def plural(count: int, noun: str) -> str:
    return f"{count} {noun}" if count == 1 else f"{count} {noun}s"


def list_cases(cases: "list[LivenessCase]", errors: "list[str]") -> int:
    print("=== liveness differential cases ===")
    print(f"Case directory: {CASE_DIR}")
    print("")
    for case in cases:
        print(f"  {case.name:<{_NAME_WIDTH}} {'  '.join(case.describe_fields())}")
    print("")
    # Errors belong on stderr, but the listing above must reach the terminal
    # first: two independently buffered streams otherwise report them in
    # whichever order the host flushed.
    sys.stdout.flush()
    for error in errors:
        sys.stderr.write(f"ERROR: {error}\n")
    sys.stderr.flush()
    print(f"{plural(len(cases), 'case')}, {plural(len(errors), 'structural error')}")
    return 1 if errors else 0


def verdict_line(rec: Recorder, total: int) -> str:
    if rec.failed:
        return f"VERDICT: FAIL  ({rec.passed} passed, {rec.failed} failed)"
    skipped = f"{rec.skipped} skipped"
    if rec.skipped:
        skipped += " (no host toolchain)"
    return f"VERDICT: PASS  ({total} cases, {skipped})"


def main(argv: "list[str] | None" = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        "xray", nargs="?", default=None,
        help="xray binary (default: $XRAY_BIN, then build/xray)")
    parser.add_argument(
        "--vm-only", action="store_true",
        help="run only the VM half, for hosts with no AOT toolchain")
    parser.add_argument(
        "--list", action="store_true",
        help="list the cases and their sidecar fields, then exit")
    parser.add_argument(
        "--baseline", type=Path, default=BASELINE_FILE,
        help=f"only-shrink ratchet baseline (default: {BASELINE_FILE.name})")
    parser.add_argument(
        "--no-baseline", action="store_true",
        help="judge every case directly, ignoring the baseline")
    args = parser.parse_args(argv)

    cases, errors = collect_cases(CASE_DIR)
    if args.list:
        return list_cases(cases, errors)

    # Absolute, because the AOT half builds from a scratch directory: a binary
    # named relatively on the command line would stop resolving the moment the
    # working directory changed, and the case would report a build failure that
    # is really a lost path.
    xray = Path(args.xray or os.environ.get("XRAY_BIN")
                or (PROJECT_DIR / "build" / platform.exe_name("xray"))).resolve()
    build_timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", DEFAULT_BUILD_TIMEOUT)

    print("=== liveness differential ===")
    print(f"Binary: {xray}")

    if not (xray.is_file() and os.access(xray, os.X_OK)):
        # Not a SKIP. A gate that goes green when its subject is absent is the
        # exact failure mode this suite exists to catch.
        sys.stdout.flush()
        sys.stderr.write(f"FAIL: xray binary not executable: {xray}\n")
        return 1

    have_cc = True
    if not args.vm_only:
        have_cc = toolchain.find_c_compiler() is not None
        if not have_cc:
            print("Note: no host C compiler; the AOT half is skipped and counted.")
    if args.vm_only:
        print("Note: --vm-only lane; the AOT half is not exercised.")
    print("")

    for error in errors:
        print(f"  ERROR: {error}")

    rec = Recorder()
    with workspace.Workspace("xray_liveness") as ws:
        for case in cases:
            run_case(rec, xray, case, ws.root, args.vm_only, have_cc, build_timeout)

    print("")
    if errors:
        print(f"VERDICT: FAIL  ({rec.passed} passed, {rec.failed} failed, "
              f"{plural(len(errors), 'structural error')})")
        return 1
    if args.no_baseline:
        print(verdict_line(rec, len(cases)))
        return 1 if rec.failed else 0

    # Only-shrink ratchet, matching the differential net next door. A case
    # whose AOT half was skipped for want of a host compiler counts as skipped,
    # not as evidence that a baselined entry is fixed.
    verdict = ratchet.evaluate(
        failed=rec.failed_names,
        baseline=ratchet.read_baseline(args.baseline),
        skipped=rec.skipped_names,
    )
    if not verdict.ok:
        print(verdict_line(rec, len(cases)))
        print("")
        print(ratchet.format_report(
            verdict, baseline_path=str(args.baseline.name)))
        return 1
    if verdict.baseline_count:
        # Not verdict_line(): with a baseline in play, "FAIL" beside exit 0
        # reads as a broken runner. Say what actually happened.
        print(f"VERDICT: PASS  ({len(cases)} cases, {rec.passed} passing, "
              f"{verdict.baseline_count} baselined, {rec.skipped} skipped)")
        return 0
    print(verdict_line(rec, len(cases)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

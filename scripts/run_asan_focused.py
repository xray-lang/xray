#!/usr/bin/env python3
"""Focused ASan+UBSan lane: the compiler's memory-safety defense line.

Builds the compiler with AddressSanitizer + UndefinedBehaviorSanitizer and runs
a focused, time-bounded surface that reliably reproduces the compiler
memory-safety accident classes this lane exists for:

  1. all C unit tests
  2. a fast backend-diff subset (VM vs native, task-190 cases)
  3. two real workloads compiled end-to-end to C: the xxhash port and the
     committed bili-analysis-server fixture

Leaks are deliberately OFF (detect_leaks=0): Apple/Homebrew clang ships ASan
without a leak detector, and process-exit leaks are the separate lsan_strict
lane's job. ASan/UBSan themselves stay fully on and abort on first error.

ONE IMPLEMENTATION, ALL PLATFORMS. This replaces three drifted scripts:
run_asan_focused.sh (214 lines), run_asan_focused.ps1 (104), and
run_asan_focused_windows.ps1 (221). The drift was not cosmetic -- the .ps1 ran
only the bili fixture, skipping both the xxhash workload and the backend-diff
subset, while claiming the same lane name. It also had no reference from
CMakeLists or CI: dead code that could only rot.

Environment overrides:
    XR_ASAN_PROFILE       full (default), canonical-program, generic-identity,
                          or generic-scalar-class
    XR_ASAN_JOBS          parallel build/test jobs (default: all cores)
    XR_ASAN_BUILD_DIR     ASan build directory (default: build-asan)
    XR_ASAN_CTEST_REGEX   unit test name regex (default: ^test_)
    XR_ASAN_CTEST_EXCLUDE unit tests kept out of the memory-safety surface
    XR_ASAN_CTEST_SERIAL_REGEX subprocess tests kept out of the saturated lane
    XR_ASAN_DIFF_REGEX    backend-diff subset regex
    XR_ASAN_XXHASH_MAIN   path to the xxhash port entry
    XR_ASAN_BILI_MAIN     path to the committed bili fixture
    XR_BUILD_LOCK_TIMEOUT build-tree lease wait in seconds (default: 600)
"""

from __future__ import annotations

import os
import sys
import tempfile
import time
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parent.parent / "tests" / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import buildlock, platform, proc, sanitizer, workspace  # noqa: E402
import canonical_program_test_profile as canonical_profile  # noqa: E402

PROJECT_DIR = Path(__file__).resolve().parent.parent
LANE = "asan_focused"

# Native-toolchain / subprocess integration tests are out of the memory-safety
# surface: they drive a full native AOT compile+link (assuming the default
# build/ cache layout) or run the compiler as a subprocess under tight
# hardcoded timeouts, so under ASan they fail for environmental reasons
# (slowdown, cache mismatch) rather than any memory bug.
# test_xi_cgen is a Release code-shape contract: many assertions intentionally
# pin cache/elision decisions that a Debug sanitizer build is allowed to change.
# This lane still exercises the sanitized compiler and C generator below via
# real AOT workloads and the backend-diff subset.
DEFAULT_CTEST_EXCLUDE = (
    "native_error_abi|param_mode_diagnostics|param_contract|test_cli_toolchain|"
    "test_lsp_protocol_transcript|test_xi_cgen|test_xr_program_aot_providers"
)
# These tests start short-lived subprocesses under public protocol timeouts;
# run them serially so a saturated ASan lane cannot consume those budgets
# through scheduler starvation.
DEFAULT_SERIAL_REGEX = "^(test_cli_toolchain|test_lsp_protocol_transcript)$"
DEFAULT_DIFF_REGEX = "task190_.*_backend_diff"

ASAN_OPTIONS = ("detect_leaks=0:abort_on_error=1:symbolize=1:"
                "strict_string_checks=1:detect_stack_use_after_return=1")
UBSAN_OPTIONS = "print_stacktrace=1:halt_on_error=1"

EXACT_PROFILES = {
    "canonical-program": (
        canonical_profile.CTEST_NAMES,
        canonical_profile.BUILD_TARGETS,
    ),
    "generic-identity": (
        canonical_profile.GENERIC_IDENTITY_CTEST_NAMES,
        canonical_profile.GENERIC_IDENTITY_BUILD_TARGETS,
    ),
    "generic-scalar-class": (
        canonical_profile.GENERIC_SCALAR_CLASS_CTEST_NAMES,
        canonical_profile.GENERIC_SCALAR_CLASS_BUILD_TARGETS,
    ),
}

USAGE = """usage: run_asan_focused.py [-h|--help]

Run the configured ASan+UBSan lane. Select its bounded profile and build tree
with XR_ASAN_PROFILE and XR_ASAN_BUILD_DIR; all execution settings are explicit
environment variables documented in this module's header.

XR_ASAN_PROFILE choices:
    full                  complete qualifying lane (default)
    canonical-program     partial canonical Program preflight
    generic-identity      partial generic identity preflight
    generic-scalar-class  generic identity plus scalar-class native AOT witness
"""


def compile_workload(log, xray: Path, main: Path, label: str,
                     timeout: float | None, required: bool) -> bool:
    """AOT-compile a real workload to C only. Exercises the whole front end and
    code generator on code far larger than any unit test."""
    if not main.is_file():
        if required:
            log(f"required {label} fixture missing at {main}", error=True)
            return False
        log(f"{label} not found at {main}; skipping real-workload compile")
        return True

    project = main.parent.parent
    handle, out_path = tempfile.mkstemp(prefix=f"asan_{label}_", suffix=".c")
    os.close(handle)
    out = Path(out_path)
    try:
        log(f"AOT-compiling (emit C only) {label}: {main}")
        result = proc.run([xray, "build", main, "--native", "--c-only", "-o", out],
                          cwd=project, timeout=timeout)
        if not result.ok:
            log(f"{label} AOT emit failed", error=True)
            sanitizer.write_console(sys.stderr, result.combined_text()[-8000:])
            return False
        log(f"{label} AOT emit OK: {out.stat().st_size} bytes")
        return True
    finally:
        out.unlink(missing_ok=True)


def verify_exact_execution(report: Path, expected_names: tuple[str, ...], profile_name: str,
                           log) -> bool:
    """Prove a sanitizer preflight ran its shared exact CTest set."""
    try:
        executed = canonical_profile.executed_ctest_names(report)
    except canonical_profile.source_fixtures.FixtureError as error:
        log(f"{profile_name} execution inventory invalid: {error}", error=True)
        return False
    actual = set(executed)
    expected = set(expected_names)
    if actual == expected and len(executed) == len(expected_names):
        return True
    for name in sorted(expected - actual):
        log(f"{profile_name} test was not executed: {name}", error=True)
    for name in sorted(actual - expected):
        log(f"unexpected {profile_name} test executed: {name}", error=True)
    return False


def _run_main(argv: list[str]) -> int:
    log = sanitizer.LaneLog(LANE)
    profile = os.environ.get("XR_ASAN_PROFILE", "full")
    exact_profile = EXACT_PROFILES.get(profile)
    if profile != "full" and exact_profile is None:
        log(f"unknown XR_ASAN_PROFILE={profile!r}", error=True)
        return 1
    jobs = sanitizer.default_jobs("XR_ASAN_JOBS")
    build_dir = PROJECT_DIR / os.environ.get("XR_ASAN_BUILD_DIR", "build-asan")
    timeout = platform.env_timeout("XR_ASAN_TIMEOUT", 3600)
    build_targets: tuple[str, ...] = ()

    ctest_regex = os.environ.get("XR_ASAN_CTEST_REGEX", "^test_")
    ctest_exclude = os.environ.get("XR_ASAN_CTEST_EXCLUDE", DEFAULT_CTEST_EXCLUDE)
    serial_regex = os.environ.get("XR_ASAN_CTEST_SERIAL_REGEX", DEFAULT_SERIAL_REGEX)
    diff_regex = os.environ.get("XR_ASAN_DIFF_REGEX", DEFAULT_DIFF_REGEX)

    exact_tests: tuple[str, ...] = ()
    if exact_profile is not None:
        exact_tests, build_targets = exact_profile
        conflicting = tuple(
            name
            for name in (
                "XR_ASAN_BUILD_TARGETS",
                "XR_ASAN_CTEST_REGEX",
                "XR_ASAN_CTEST_EXCLUDE",
                "XR_ASAN_CTEST_SERIAL_REGEX",
                "XR_ASAN_DIFF_REGEX",
            )
            if name in os.environ
        )
        if conflicting:
            log(f"{profile} profile owns its exact inventory; remove overrides: " +
                ", ".join(conflicting), error=True)
            return 1
        ctest_regex = canonical_profile.ctest_regex(exact_tests)
        ctest_exclude = ""
    elif "XR_ASAN_BUILD_TARGETS" in os.environ:
        log(
            "full profile owns the complete build inventory; "
            "XR_ASAN_BUILD_TARGETS is not allowed",
            error=True,
        )
        return 1

    xxhash_main = Path(os.environ.get(
        "XR_ASAN_XXHASH_MAIN",
        str(PROJECT_DIR.parent / "xray-ports" / "ports" / "xxhash" / "src" / "main.xr")))
    bili_main = Path(os.environ.get(
        "XR_ASAN_BILI_MAIN",
        str(PROJECT_DIR / "tests/meta/fixtures/bili-analysis-server/src/main.xr")))

    os.environ["ASAN_OPTIONS"] = ASAN_OPTIONS
    os.environ["UBSAN_OPTIONS"] = UBSAN_OPTIONS
    os.environ["PYTHONUTF8"] = "1"
    os.environ["PYTHONIOENCODING"] = "utf-8"
    os.environ["XRAY_STDLIB_PATH"] = str(PROJECT_DIR / "stdlib")

    log(f"ROOT={PROJECT_DIR}")
    log(f"profile={profile} build dir={build_dir.name} jobs={jobs}")
    if exact_profile is not None:
        log("PARTIAL preflight: this does not qualify the full asan_focused lane")
    if build_targets:
        log(f"build targets={' '.join(build_targets)}")

    # The unified target-machine line has one supported bootstrap shape: the
    # optional generated stdlib VM fastpaths are disabled. Their generator is
    # itself a native AOT consumer, so enabling it makes the sanitizer build
    # depend on target families that this lane is meant to validate. Load the
    # stdlib from source as well: the focused compiler lane must not depend on
    # completing the separate self-hosted-bytecode bootstrap before ASan can
    # execute the compiler under test. Pin both options instead of inheriting a
    # cache/default and accidentally measuring a different compiler.
    c_compiler = "clang-cl" if platform.IS_WINDOWS else "clang"
    cxx_compiler = "clang-cl" if platform.IS_WINDOWS else "clang++"
    resolved_c_compiler = Path(sanitizer.resolve_compiler_command(c_compiler)).as_posix()
    resolved_cxx_compiler = Path(sanitizer.resolve_compiler_command(cxx_compiler)).as_posix()
    spec = sanitizer.BuildSpec(
        build_dir=build_dir,
        sanitizer_flags=(
            "ENABLE_ASAN=ON",
            "ENABLE_UBSAN=ON",
            "XR_STDLIB_FROM_FILE=ON",
            "XRAY_STDLIB_VM_FASTPATHS=OFF",
        ),
        # clang-cl's ASan runtime interposes the release UCRT allocator.  A
        # Debug-profile build that links ucrtbased.dll instead reports false
        # cross-allocator bad-free failures from _putenv_s before reaching the
        # code under test.  Keep Debug code generation while selecting the
        # supported dynamic release CRT explicitly.
        extra_cache=("CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL",),
        c_compiler=c_compiler,
        cxx_compiler=cxx_compiler,
        targets=build_targets,
        verify_cache_contains=(
            "ENABLE_ASAN=ON",
            "ENABLE_UBSAN=ON",
            "XR_STDLIB_FROM_FILE=ON",
            "XRAY_STDLIB_VM_FASTPATHS=OFF",
            "CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL",
            f"CMAKE_C_COMPILER={resolved_c_compiler}",
            f"CMAKE_CXX_COMPILER={resolved_cxx_compiler}",
        ),
    )

    if not sanitizer.activate_windows_msvc_environment(log):
        return 1
    if not sanitizer.activate_windows_dynamic_asan_runtime(spec, log):
        return 1

    xray = build_dir / platform.exe_name("xray")
    if not sanitizer.configure(spec, PROJECT_DIR, jobs, timeout, log):
        return 1
    if exact_profile is not None:
        log(f"incrementally building exact {profile} sanitizer targets")
    else:
        log("incrementally building the complete ASan/UBSan tree")
    # Ninja, not the CLI timestamp, owns target-level dependency freshness.
    # Invoking it on every run is cheap when current and cannot omit changed
    # tests, scripts, generated inputs, or deleted dependencies from the gate.
    if not sanitizer.build(spec, jobs, timeout, log):
        return 1

    problem = sanitizer.verify_build_tree(spec, PROJECT_DIR)
    if problem:
        log(problem, error=True)
        return 1
    if profile == "full" and not (xray.is_file() and os.access(xray, os.X_OK)):
        log(f"ASan xray binary not found at {xray}", error=True)
        return 1

    if exact_profile is not None:
        listing = proc.run(["ctest", "-N", "-R", canonical_profile.ctest_regex(exact_tests)],
                           cwd=build_dir)
        if not listing.ok:
            log(f"could not enumerate {profile} preflight tests", error=True)
            sanitizer.write_console(sys.stderr, listing.combined_text())
            return 1
        registered = set(canonical_profile.listed_ctest_names(
            listing.stdout.decode("utf-8", "replace")))
        expected = set(exact_tests)
        if registered != expected:
            for name in sorted(expected - registered):
                log(f"{profile} preflight test missing: {name}", error=True)
            for name in sorted(registered - expected):
                log(f"unexpected {profile} preflight test: {name}", error=True)
            return 1

    log(f"running unit tests (regex: {ctest_regex}, exclude: {ctest_exclude})")
    if exact_profile is not None:
        with workspace.Workspace("xray_asan_ctest_evidence") as evidence:
            report = evidence.path("ctest.xml")
            result = sanitizer.ctest(
                build_dir, include=ctest_regex, exclude=ctest_exclude,
                jobs=jobs, timeout_each=300, timeout=timeout, junit=report
            )
            if not result.ok:
                sanitizer.write_console(sys.stdout, result.combined_text())
                return 1
            if not verify_exact_execution(report, exact_tests, profile, log):
                return 1
    else:
        result = sanitizer.ctest(build_dir, include=ctest_regex, exclude=ctest_exclude,
                                 jobs=jobs, timeout_each=300, timeout=timeout)
        if not result.ok:
            sanitizer.write_console(sys.stdout, result.combined_text())
            return 1

    if exact_profile is not None:
        log(f"PASS ({len(exact_tests)} exact preflight tests; full lane not run)")
        return 0

    log(f"running subprocess-sensitive unit tests serially (regex: {serial_regex})")
    if sanitizer.ctest_has_match(build_dir, serial_regex):
        result = sanitizer.ctest(build_dir, include=serial_regex, jobs=1,
                                 timeout_each=300, timeout=timeout)
        if not result.ok:
            sanitizer.write_console(sys.stdout, result.combined_text())
            return 1
    else:
        log(f"no serial unit tests matched {serial_regex}; skipping")

    log(f"running fast backend-diff subset (regex: {diff_regex})")
    if sanitizer.ctest_has_match(build_dir, diff_regex):
        result = sanitizer.ctest(build_dir, include=diff_regex, jobs=jobs,
                                 timeout_each=600, timeout=timeout)
        if not result.ok:
            sanitizer.write_console(sys.stdout, result.combined_text())
            return 1
    else:
        log(f"no backend-diff tests matched {diff_regex}; skipping")

    if not compile_workload(log, xray, xxhash_main, "xxhash", timeout, required=False):
        return 1
    if not compile_workload(log, xray, bili_main, "bili", timeout, required=True):
        return 1

    log("PASS")
    return 0


def main(argv: list[str]) -> int:
    arguments = argv[1:]
    if arguments:
        if len(arguments) == 1 and arguments[0] in ("-h", "--help"):
            print(USAGE, end="")
            return 0
        print(f"run_asan_focused.py: unrecognized arguments: {' '.join(arguments)}",
              file=sys.stderr)
        return 2

    build_dir = PROJECT_DIR / os.environ.get("XR_ASAN_BUILD_DIR", "build-asan")
    log = sanitizer.LaneLog(LANE)
    try:
        lease = buildlock.BuildTreeLock(build_dir)
        started = time.perf_counter()
        with lease:
            log(f"build-tree lease acquired after "
                f"{time.perf_counter() - started:.3f}s: {lease.build_dir}")
            return _run_main(argv)
    except (ValueError, TimeoutError) as error:
        log(f"build-tree lease failed: {error}", error=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

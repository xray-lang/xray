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
    XR_ASAN_JOBS          parallel build/test jobs (default: all cores)
    XR_ASAN_BUILD_DIR     ASan build directory (default: build-asan)
    XR_ASAN_CTEST_REGEX   unit test name regex (default: ^test_)
    XR_ASAN_CTEST_EXCLUDE unit tests kept out of the memory-safety surface
    XR_ASAN_CTEST_SERIAL_REGEX subprocess tests kept out of the saturated lane
    XR_ASAN_DIFF_REGEX    backend-diff subset regex
    XR_ASAN_XXHASH_MAIN   path to the xxhash port entry
    XR_ASAN_BILI_MAIN     path to the committed bili fixture
"""

from __future__ import annotations

import os
import sys
import tempfile
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parent.parent / "tests" / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, sanitizer  # noqa: E402

PROJECT_DIR = Path(__file__).resolve().parent.parent
LANE = "asan_focused"

# Native-toolchain / subprocess integration tests are out of the memory-safety
# surface: they drive a full native AOT compile+link (assuming the default
# build/ cache layout) or run the compiler as a subprocess under tight
# hardcoded timeouts, so under ASan they fail for environmental reasons
# (slowdown, cache mismatch) rather than any memory bug.
DEFAULT_CTEST_EXCLUDE = "native_error_abi|param_mode_diagnostics|param_contract|test_cli_toolchain"
# The toolchain unit test starts short-lived provider processes under the
# public 5s version-probe bound; run it serially so a saturated ASan lane
# cannot consume that budget through scheduler starvation.
DEFAULT_SERIAL_REGEX = "^test_cli_toolchain$"
DEFAULT_DIFF_REGEX = "task190_.*_backend_diff"

ASAN_OPTIONS = ("detect_leaks=0:abort_on_error=1:symbolize=1:"
                "strict_string_checks=1:detect_stack_use_after_return=1")
UBSAN_OPTIONS = "print_stacktrace=1:halt_on_error=1"


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
            sys.stderr.write(result.combined_text()[-8000:])
            return False
        log(f"{label} AOT emit OK: {out.stat().st_size} bytes")
        return True
    finally:
        out.unlink(missing_ok=True)


def main(argv: list[str]) -> int:
    log = sanitizer.LaneLog(LANE)
    jobs = sanitizer.default_jobs("XR_ASAN_JOBS")
    build_dir = PROJECT_DIR / os.environ.get("XR_ASAN_BUILD_DIR", "build-asan")
    timeout = platform.env_timeout("XR_ASAN_TIMEOUT", 3600)

    ctest_regex = os.environ.get("XR_ASAN_CTEST_REGEX", "^test_")
    ctest_exclude = os.environ.get("XR_ASAN_CTEST_EXCLUDE", DEFAULT_CTEST_EXCLUDE)
    serial_regex = os.environ.get("XR_ASAN_CTEST_SERIAL_REGEX", DEFAULT_SERIAL_REGEX)
    diff_regex = os.environ.get("XR_ASAN_DIFF_REGEX", DEFAULT_DIFF_REGEX)

    xxhash_main = Path(os.environ.get(
        "XR_ASAN_XXHASH_MAIN",
        str(PROJECT_DIR.parent / "xray-ports" / "ports" / "xxhash" / "src" / "main.xr")))
    bili_main = Path(os.environ.get(
        "XR_ASAN_BILI_MAIN",
        str(PROJECT_DIR / "tests/meta/fixtures/bili-analysis-server/src/main.xr")))

    os.environ["ASAN_OPTIONS"] = ASAN_OPTIONS
    os.environ["UBSAN_OPTIONS"] = UBSAN_OPTIONS

    log(f"ROOT={PROJECT_DIR}")
    log(f"build dir={build_dir.name} jobs={jobs}")

    # The stdlib VM fastpaths stay ON. Turning them off looked like a free
    # saving -- they are unrelated to compiler memory safety -- but
    # test_stdlib_vm_fastpath_abi is registered under
    # if(XRAY_STDLIB_VM_FASTPATHS), so disabling the option does not make that
    # test cheaper, it makes it *disappear* from the lane. Coverage is the one
    # thing a gate may not trade for speed.
    spec = sanitizer.BuildSpec(
        build_dir=build_dir,
        sanitizer_flags=("ENABLE_ASAN=ON", "ENABLE_UBSAN=ON"),
    )

    xray = build_dir / platform.exe_name("xray")
    reason = sanitizer.rebuild_reason(xray, PROJECT_DIR)
    if reason:
        log(f"building compiler + tests (ASan/UBSan): {reason}")
        if not sanitizer.configure(spec, PROJECT_DIR, jobs, timeout, log):
            return 1
        if not sanitizer.build(spec, jobs, timeout, log):
            return 1
    else:
        log("reusing the up-to-date ASan build")

    if not (xray.is_file() and os.access(xray, os.X_OK)):
        log(f"ASan xray binary not found at {xray}", error=True)
        return 1

    problem = sanitizer.verify_configured(build_dir, "ENABLE_ASAN=ON")
    if problem:
        log(problem, error=True)
        return 1

    log(f"running unit tests (regex: {ctest_regex}, exclude: {ctest_exclude})")
    result = sanitizer.ctest(build_dir, include=ctest_regex, exclude=ctest_exclude,
                            jobs=jobs, timeout_each=300, timeout=timeout)
    if not result.ok:
        sys.stdout.write(result.combined_text())
        return 1

    log(f"running subprocess-sensitive unit tests serially (regex: {serial_regex})")
    if sanitizer.ctest_has_match(build_dir, serial_regex):
        result = sanitizer.ctest(build_dir, include=serial_regex, jobs=1,
                                 timeout_each=300, timeout=timeout)
        if not result.ok:
            sys.stdout.write(result.combined_text())
            return 1
    else:
        log(f"no serial unit tests matched {serial_regex}; skipping")

    log(f"running fast backend-diff subset (regex: {diff_regex})")
    if sanitizer.ctest_has_match(build_dir, diff_regex):
        result = sanitizer.ctest(build_dir, include=diff_regex, jobs=jobs,
                                 timeout_each=600, timeout=timeout)
        if not result.ok:
            sys.stdout.write(result.combined_text())
            return 1
    else:
        log(f"no backend-diff tests matched {diff_regex}; skipping")

    if not compile_workload(log, xray, xxhash_main, "xxhash", timeout, required=False):
        return 1
    if not compile_workload(log, xray, bili_main, "bili", timeout, required=True):
        return 1

    log("PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

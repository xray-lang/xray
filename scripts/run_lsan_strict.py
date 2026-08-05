#!/usr/bin/env python3
"""Strict LeakSanitizer lane: the leak budget, enforced.

Builds with ASan (+UBSan) and leak detection ON, then runs the focused unit-test
surface with the committed suppression file. Process-wide registries are
released via xr_process_shutdown(), so a clean exit is a real signal rather than
a suppressed one.

PLATFORM: standalone leak detection is only supported under ASan on Linux.
Apple/Homebrew clang on macOS ships ASan WITHOUT a leak detector -- which is
exactly why the asan_focused lane runs detect_leaks=0. On non-Linux hosts this
lane skips loudly rather than pretending to be green.

Environment overrides:
    XR_LSAN_JOBS         parallel build/test jobs (default: all cores)
    XR_LSAN_BUILD_DIR    build directory (default: build-lsan)
    XR_LSAN_CTEST_REGEX  unit test name regex (default: ^test_)
"""

from __future__ import annotations

import os
import sys
from pathlib import Path
from typing import List


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parent.parent / "tests" / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, sanitizer  # noqa: E402

PROJECT_DIR = Path(__file__).resolve().parent.parent
LANE = "lsan_strict"


def main(argv: List[str]) -> int:
    log = sanitizer.LaneLog(LANE)

    if sys.platform != "linux":
        log(f"LeakSanitizer needs Linux (host is {sys.platform}); skipping.")
        log("Run this lane on Linux CI to enforce the leak budget.")
        return 0

    jobs = sanitizer.default_jobs("XR_LSAN_JOBS")
    build_dir = PROJECT_DIR / os.environ.get("XR_LSAN_BUILD_DIR", "build-lsan")
    ctest_regex = os.environ.get("XR_LSAN_CTEST_REGEX", "^test_")
    suppressions = PROJECT_DIR / "scripts" / "lsan.supp"
    timeout = platform.env_timeout("XR_LSAN_TIMEOUT", 3600)

    os.environ["ASAN_OPTIONS"] = (
        "detect_leaks=1:abort_on_error=1:symbolize=1:detect_stack_use_after_return=1")
    os.environ["UBSAN_OPTIONS"] = "print_stacktrace=1:halt_on_error=1"
    os.environ["LSAN_OPTIONS"] = (
        f"suppressions={suppressions}:print_suppressions=1:report_objects=1")

    spec = sanitizer.BuildSpec(
        build_dir=build_dir,
        sanitizer_flags=("ENABLE_ASAN=ON", "ENABLE_UBSAN=ON"),
    )

    log(f"configuring {build_dir.name} (ASan+UBSan, detect_leaks=1)")
    if not sanitizer.configure(spec, PROJECT_DIR, jobs, timeout, log):
        return 1
    log(f"building (jobs={jobs})")
    if not sanitizer.build(spec, jobs, timeout, log):
        return 1

    problem = sanitizer.verify_configured(build_dir, "ENABLE_ASAN=ON")
    if problem:
        log(problem, error=True)
        return 1

    log(f"running unit tests under LSan (regex: {ctest_regex})")
    result = sanitizer.ctest(build_dir, include=ctest_regex, jobs=jobs,
                             timeout_each=300, timeout=timeout)
    if not result.ok:
        sys.stdout.write(result.combined_text())
        return 1

    log("PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

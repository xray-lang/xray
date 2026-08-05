#!/usr/bin/env python3
"""Focused ThreadSanitizer lane: the scheduler and the two-band refcount.

The work-stealing scheduler and the two-band reference counter are exactly the
code TSan exists for, and until this lane nothing ran them under it.

Scope is the CURRENT canonical concurrency assets, not the three legacy
directories an earlier analysis pointed at (tests/coroutine_safety,
tests/work_stealing, tests/network) -- their own READMEs mark them as historical
hand-run drafts on pre-clean-slate surface, superseded by
tests/regression/11_coroutine and the diff nets.

VM mode only: the TSan-instrumented binary is the compiler+runtime itself, and
AOT child binaries are produced by the system cc, so they would not be
instrumented and would prove nothing.

Races are collected rather than halted on (halt_on_error=0), so one warning does
not hide the rest; the lane fails when any warning appeared.

Environment overrides:
    XR_TSAN_JOBS        parallel build jobs (default: all cores)
    XR_TSAN_BUILD_DIR   TSan build directory (default: build-tsan)
    XR_TSAN_SKIP_BUILD  1 = reuse the existing TSan build as-is
"""

from __future__ import annotations

import os
import sys
from pathlib import Path
from typing import List, Tuple


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parent.parent / "tests" / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, sanitizer, workspace  # noqa: E402

PROJECT_DIR = Path(__file__).resolve().parent.parent
LANE = "tsan_focused"

# (mode, case). Channel / work-stealing / scope shapes from the canonical suite,
# plus the progress shapes the liveness lane pinned.
CASES: Tuple[Tuple[str, str], ...] = (
    ("test", "tests/regression/11_coroutine/1101_channel_basic.xr"),
    ("test", "tests/regression/11_coroutine/1102_go_await.xr"),
    ("test", "tests/regression/11_coroutine/1104_coroutine_combined.xr"),
    ("test", "tests/regression/11_coroutine/1148_scope_race_stress.xr"),
    ("test", "tests/regression/11_coroutine/1151_inject_queue_spill.xr"),
    ("test", "tests/regression/11_coroutine/1167_channel_explicit_transfer.xr"),
    ("run", "tests/diff/cases/liveness/busy_poll_progress.xr"),
    ("run", "tests/diff/cases/liveness/blocking_recv_progress.xr"),
    ("run", "tests/diff/cases/liveness/cancel_responsive_blocking.xr"),
)

# Multi-worker is forced: the default profile would stay single-threaded, and a
# race needs real parallelism to surface.
WORKERS = "4"
CASE_TIMEOUT = 60

TSAN_WARNING = "WARNING: ThreadSanitizer"


def main(argv: List[str]) -> int:
    log = sanitizer.LaneLog(LANE)
    jobs = sanitizer.default_jobs("XR_TSAN_JOBS")
    build_dir = PROJECT_DIR / os.environ.get("XR_TSAN_BUILD_DIR", "build-tsan")
    skip_build = platform.env_flag("XR_TSAN_SKIP_BUILD")
    timeout = platform.env_timeout("XR_TSAN_TIMEOUT", 3600)

    # Instrument through raw compiler flags, NOT -DENABLE_TSAN=ON. The option
    # additionally defines XR_BUILD_TSAN=1, which makes xray pass
    # -fsanitize=thread to the C compiler it spawns for AOT children. This lane
    # is VM-only by design (an AOT child built by the system cc would not be
    # instrumented and would prove nothing), so that define is out of scope --
    # switching to the option would silently widen what the lane builds.
    spec = sanitizer.BuildSpec(
        build_dir=build_dir,
        sanitizer_flags=(
            'CMAKE_C_FLAGS=-fsanitize=thread -fno-omit-frame-pointer',
            'CMAKE_EXE_LINKER_FLAGS=-fsanitize=thread',
        ),
        targets=("xray",),
        # No ENABLE_* BOOL exists here, so reuse is verified by looking for the
        # instrumentation flag itself in the cache.
        verify_cache_contains=("-fsanitize=thread",),
    )

    if not skip_build:
        if not sanitizer.configure(spec, PROJECT_DIR, jobs, timeout, log):
            return 1
        log(f"building xray (TSan, jobs={jobs})")
        if not sanitizer.build(spec, jobs, timeout, log):
            return 1

    xray = build_dir / platform.exe_name("xray")
    if not (xray.is_file() and os.access(xray, os.X_OK)):
        log(f"TSan xray binary not found at {xray}", error=True)
        return 1

    problem = next((p for p in (sanitizer.verify_configured(build_dir, f)
                                for f in spec.verification_targets()) if p), None)
    if problem:
        log(problem, error=True)
        return 1

    print("=== TSan focused lane ===")
    print(f"Binary: {xray}")

    with workspace.Workspace("xray_tsan") as ws:
        log_prefix = ws.path("tsan")
        env = dict(os.environ)
        # Collect every race instead of aborting on the first: one warning must
        # not hide the others.
        env["TSAN_OPTIONS"] = f"halt_on_error=0 exitcode=0 log_path={log_prefix}"
        env["XRAY_WORKERS"] = WORKERS

        for mode, case in CASES:
            name = Path(case).stem
            sys.stdout.write(f"  {name:<40}")
            sys.stdout.flush()
            result = proc.run([xray, mode, PROJECT_DIR / case], env=env,
                              timeout=CASE_TIMEOUT)
            if result.timed_out:
                print(f"ran (timed out after {CASE_TIMEOUT}s)")
            elif result.ok:
                print("ran")
            else:
                print(f"ran (nonzero exit {result.returncode})")

        reports = []
        for path in sorted(ws.root.glob("tsan.*")):
            text = path.read_text(encoding="utf-8")
            if TSAN_WARNING in text:
                reports.append(text)

        count = sum(t.count(TSAN_WARNING) for t in reports)
        if count:
            print("")
            print(f"ThreadSanitizer reported {count} warning(s):")
            shown = 0
            for text in reports:
                for line in text.splitlines():
                    if TSAN_WARNING in line or shown:
                        print(line)
                        shown += 1
                        if shown >= 60:
                            break
                if shown >= 60:
                    break
            print(f"VERDICT: FAIL ({count} TSan warnings)")
            return 1

    print("")
    print(f"VERDICT: PASS (no ThreadSanitizer warnings across {len(CASES)} cases)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

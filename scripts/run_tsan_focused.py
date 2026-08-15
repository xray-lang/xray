#!/usr/bin/env python3
"""Focused ThreadSanitizer lane for runtime concurrency primitives.

The work-stealing scheduler and the two-band reference counter are exactly the
code TSan exists for, and until this lane nothing ran them under it. The
ownership audit's fail-closed record gate is also built and exercised here,
because diagnostic-only sources are intentionally absent from release archives.

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
"""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path


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
CASES: tuple[tuple[str, str], ...] = (
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

# These are the Task 276 concurrency boundaries that must be present in the
# sanitized build and selected by CTest.  Target names and CTest identities are
# deliberately the same, so one frozen tuple controls both surfaces.
TASK276_TSAN_TESTS: tuple[str, ...] = (
    "test_vm_decoded_cache",
    "test_xtp_resource_stress",
    "test_runtime_generation",
    "test_entry_cell_runtime_archive",
)
TASK276_TSAN_REGEX = "^(?:" + "|".join(TASK276_TSAN_TESTS) + ")$"
TSAN_PROBE_TIMEOUT = 60


def focused_build_spec(build_dir: Path) -> sanitizer.BuildSpec:
    """Return the exact instrumented target set owned by this lane."""
    return sanitizer.BuildSpec(
        build_dir=build_dir,
        sanitizer_flags=(
            'CMAKE_C_FLAGS=-fsanitize=thread -fno-omit-frame-pointer',
            'CMAKE_EXE_LINKER_FLAGS=-fsanitize=thread',
            'XRAY_STDLIB_VM_FASTPATHS=OFF',
        ),
        targets=("xray", "test_ownership_audit", *TASK276_TSAN_TESTS),
        # No ENABLE_* BOOL exists here, so reuse is verified by looking for the
        # instrumentation flag itself in the cache.
        verify_cache_contains=("-fsanitize=thread", "XRAY_STDLIB_VM_FASTPATHS=OFF"),
    )


def tsan_runtime_available(log, *, run=proc.run,
                           is_windows: bool | None = None) -> bool:
    """Compile and execute a tiny TSan binary; unavailable means lane failure."""
    if is_windows is None:
        is_windows = platform.IS_WINDOWS
    if is_windows:
        log("Clang ThreadSanitizer is unsupported on Windows", error=True)
        return False

    source = b"int main(void) { return 0; }\n"
    with workspace.Workspace("xray_tsan_probe") as ws:
        binary = ws.path("probe")
        compile_result = run(
            [
                "clang", "-fsanitize=thread", "-fno-omit-frame-pointer",
                "-x", "c", "-", "-o", binary,
            ],
            stdin=source,
            timeout=TSAN_PROBE_TIMEOUT,
        )
        if not compile_result.ok:
            state = (f"timed out after {TSAN_PROBE_TIMEOUT}s"
                     if compile_result.timed_out
                     else f"exited {compile_result.returncode}")
            log(f"TSan runtime probe compile {state}", error=True)
            log(compile_result.combined_text()[-4000:], error=True)
            return False

        env = dict(os.environ)
        env["TSAN_OPTIONS"] = "halt_on_error=1 exitcode=86"
        runtime_result = run([binary], env=env, timeout=TSAN_PROBE_TIMEOUT)
        if not runtime_result.ok:
            state = (f"timed out after {TSAN_PROBE_TIMEOUT}s"
                     if runtime_result.timed_out
                     else f"exited {runtime_result.returncode}")
            log(f"TSan runtime probe execution {state}", error=True)
            log(runtime_result.combined_text()[-4000:], error=True)
            return False
    return True


def discover_task276_ctests(build_dir: Path, log, *, run=proc.run,
                            timeout: float | None = 120) -> tuple[str, ...] | None:
    """Return the exact focused CTest identities, failing closed on drift."""
    result = run(
        [
            "ctest", "--test-dir", build_dir, "--show-only=json-v1",
            "-R", TASK276_TSAN_REGEX,
        ],
        timeout=timeout,
    )
    if not result.ok:
        state = "timed out" if result.timed_out else f"exited {result.returncode}"
        log(f"focused CTest discovery {state}", error=True)
        log(result.combined_text()[-4000:], error=True)
        return None

    try:
        payload = json.loads(result.stdout_text())
        tests = payload["tests"]
        names = tuple(test["name"] for test in tests)
        if any(not isinstance(name, str) for name in names):
            raise TypeError("CTest names must be strings")
    except (KeyError, TypeError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        log(f"focused CTest discovery returned invalid JSON: {exc}", error=True)
        return None

    required = set(TASK276_TSAN_TESTS)
    selected = set(names)
    if len(names) != len(TASK276_TSAN_TESTS) or selected != required:
        missing = sorted(required - selected)
        unexpected = sorted(selected - required)
        details = []
        if not names:
            details.append("0 tests selected")
        if missing:
            details.append("missing " + ", ".join(missing))
        if unexpected:
            details.append("unexpected " + ", ".join(unexpected))
        if len(names) != len(selected):
            details.append("duplicate test identities")
        log("focused CTest identity mismatch: " + "; ".join(details), error=True)
        return None
    return names


def run_task276_ctests(build_dir: Path, env: dict[str, str], timeout: float | None,
                       log, *, run=proc.run) -> bool:
    """Validate registration, then run every Task 276 concurrency target."""
    selected = discover_task276_ctests(build_dir, log, run=run, timeout=timeout)
    if selected is None:
        return False
    log("focused CTest identities: " + ", ".join(selected))
    result = run(
        [
            "ctest", "--test-dir", build_dir, "--output-on-failure",
            "--no-tests=error", "-j", "1", "--timeout", "300",
            "-R", TASK276_TSAN_REGEX,
        ],
        env=env,
        timeout=timeout,
    )
    sanitizer.write_console(sys.stdout, result.combined_text())
    if not result.ok:
        state = "timed out" if result.timed_out else f"exited {result.returncode}"
        log(f"focused Task 276 CTest run {state}", error=True)
        return False
    return True


def main(argv: list[str]) -> int:
    log = sanitizer.LaneLog(LANE)
    jobs = sanitizer.default_jobs("XR_TSAN_JOBS")
    build_dir = PROJECT_DIR / os.environ.get("XR_TSAN_BUILD_DIR", "build-tsan")
    timeout = platform.env_timeout("XR_TSAN_TIMEOUT", 3600)

    # A configured tree is not proof that the host can initialize the TSan
    # runtime.  Probe first so unsupported hosts and broken installations are
    # red, never a skipped green lane.
    if not tsan_runtime_available(log):
        return 1

    # Instrument through raw compiler flags, NOT -DENABLE_TSAN=ON. The option
    # additionally defines XR_BUILD_TSAN=1, which makes xray pass
    # -fsanitize=thread to the C compiler it spawns for AOT children. This lane
    # is VM-only by design (an AOT child built by the system cc would not be
    # instrumented and would prove nothing), so that define is out of scope --
    # switching to the option would silently widen what the lane builds.
    spec = focused_build_spec(build_dir)

    xray = build_dir / platform.exe_name("xray")
    unit_dir = build_dir / "tests" / "unit"
    audit_test = unit_dir / platform.exe_name("test_ownership_audit")
    focused_binaries = tuple(
        unit_dir / platform.exe_name(name) for name in TASK276_TSAN_TESTS
    )
    reason = next(
        (
            f"{name}: {candidate}"
            for name, binary in (
                ("xray", xray),
                ("ownership audit", audit_test),
                *zip(TASK276_TSAN_TESTS, focused_binaries),
            )
            if (candidate := sanitizer.rebuild_reason(binary, PROJECT_DIR))
        ),
        None,
    )
    if reason:
        log(f"building the explicit TSan target set (jobs={jobs}): {reason}")
        if not sanitizer.configure(spec, PROJECT_DIR, jobs, timeout, log):
            return 1
        if not sanitizer.build(spec, jobs, timeout, log):
            return 1
    else:
        log("reusing the up-to-date TSan build")

    for name, binary in (
        ("xray", xray),
        ("ownership audit", audit_test),
        *zip(TASK276_TSAN_TESTS, focused_binaries),
    ):
        if not (binary.is_file() and os.access(binary, os.X_OK)):
            log(f"TSan {name} binary not found at {binary}", error=True)
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

        focused_failed = not run_task276_ctests(build_dir, env, timeout, log)

        sys.stdout.write("  ownership_audit_concurrency             ")
        sys.stdout.flush()
        audit_result = proc.run([audit_test], env=env, timeout=CASE_TIMEOUT)
        audit_failed = audit_result.timed_out or not audit_result.ok
        if audit_result.timed_out:
            print(f"failed (timed out after {CASE_TIMEOUT}s)")
        elif audit_result.ok:
            print("passed")
        else:
            print(f"failed (nonzero exit {audit_result.returncode})")

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

    if focused_failed or audit_failed:
        print("")
        failed_steps = []
        if focused_failed:
            failed_steps.append("Task 276 focused CTest set")
        if audit_failed:
            failed_steps.append("ownership audit concurrency test")
        print("VERDICT: FAIL (" + " and ".join(failed_steps) + " failed)")
        return 1

    print("")
    print(
        "VERDICT: PASS (no ThreadSanitizer warnings across "
        f"{len(TASK276_TSAN_TESTS)} Task 276 concurrency tests, ownership audit, "
        f"and {len(CASES)} VM cases)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

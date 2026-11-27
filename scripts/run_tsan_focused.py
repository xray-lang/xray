#!/usr/bin/env python3
"""Focused ThreadSanitizer lane for runtime kernels and VM concurrency cases.

The kernel targets cover the runtime currently used by the VM. The canonical
coroutine and progress inputs remain required for the VM execution verdict.
Collect races across all selected tests. A failed command or any race warning
fails the lane.

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

# Explicit build target and CTest name pairs from the current Thread kernels gate.
KERNELS: tuple[tuple[str, str], ...] = (
    ("test_os_thread_token", "os_thread_token"),
    ("test_xir_values", "test_xir_values"),
    ("test_xir_signed_object_rc", "test_xir_signed_object_rc"),
    ("test_native_managed_kernel", "native_managed_kernel"),
    ("test_native_managed_root", "native_managed_root"),
    ("test_native_backing", "native_backing"),
    ("test_native_string_backing_ops", "native_string_backing_ops"),
    ("test_native_place", "native_place"),
    ("test_native_value_plan", "native_value_plan"),
    ("test_native_managed_status", "native_managed_status"),
    ("test_native_capture_backing", "native_capture_backing"),
    ("test_rbc_code_owner", "rbc_code_owner"),
)
CLOSED_THREADS_TARGET = "test_xir_value_allocations"
KERNEL_TESTS = tuple(name for _, name in KERNELS)
KERNEL_REGEX = "^(" + "|".join(KERNEL_TESTS) + ")$"
TSAN_PROBE_TIMEOUT = 60


def focused_build_spec(build_dir: Path) -> sanitizer.BuildSpec:
    """Return the exact instrumented target set owned by this lane."""
    return sanitizer.BuildSpec(
        build_dir=build_dir,
        sanitizer_flags=("ENABLE_TSAN=ON", "XR_XIR_PARANOID=ON"),
        targets=("xray", *(target for target, _ in KERNELS), CLOSED_THREADS_TARGET),
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


def discover_kernel_ctests(build_dir: Path, log, *, run=proc.run,
                           timeout: float | None = 120) -> tuple[str, ...] | None:
    """Return the exact focused CTest identities, failing closed on drift."""
    result = run(
        [
            "ctest", "--test-dir", build_dir, "--show-only=json-v1",
            "-R", KERNEL_REGEX,
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

    required = set(KERNEL_TESTS)
    selected = set(names)
    if len(names) != len(KERNEL_TESTS) or selected != required:
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


def run_kernel_ctests(build_dir: Path, env: dict[str, str], timeout: float | None,
                      log, *, run=proc.run) -> bool:
    """Validate registration and run every required kernel test."""
    selected = discover_kernel_ctests(build_dir, log, run=run, timeout=timeout)
    if selected is None:
        return False
    log("focused CTest identities: " + ", ".join(selected))
    result = run(
        [
            "ctest", "--test-dir", build_dir, "--output-on-failure",
            "--no-tests=error", "-j", "1", "--timeout", "300",
            "-R", KERNEL_REGEX,
        ],
        env=env,
        timeout=timeout,
    )
    sanitizer.write_console(sys.stdout, result.combined_text())
    if not result.ok:
        state = "timed out" if result.timed_out else f"exited {result.returncode}"
        log(f"focused kernel CTest run {state}", error=True)
        return False
    return True


def vm_test_report_error(path: Path) -> str | None:
    """Check that the public test command actually executed the selected file."""
    try:
        report = json.loads(path.read_bytes())
        if report["schema"] != 1 or len(report["files"]) != 1:
            return "unexpected VM test report schema or file count"
        row = report["files"][0]
        counts = tuple(row[name] for name in (
            "tests", "passed", "skipped", "failed", "errors", "timeouts",
        ))
        if any(type(count) is not int or count < 0 for count in counts):
            return "invalid VM test report counts"
        tests, passed, skipped, failed, errors, timeouts = counts
        if passed == 0 or failed or errors or timeouts or tests != passed + skipped:
            detail = row.get("error")
            return detail if isinstance(detail, str) and detail else (
                "VM test report has no successful execution or contains failures"
            )
    except (OSError, ValueError, TypeError, KeyError) as exc:
        return f"cannot read VM test report: {exc}"
    return None


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

    spec = focused_build_spec(build_dir)
    if reason := sanitizer.verify_build_tree(spec, PROJECT_DIR):
        log(reason)
        if not sanitizer.configure(spec, PROJECT_DIR, jobs, timeout, log):
            return 1
    if not sanitizer.build(spec, jobs, timeout, log):
        return 1
    if problem := sanitizer.verify_build_tree(spec, PROJECT_DIR):
        log(problem, error=True)
        return 1
    xray = build_dir / platform.exe_name("xray")
    closed_threads = build_dir / "tests" / "unit" / platform.exe_name(CLOSED_THREADS_TARGET)
    for binary in (xray, closed_threads):
        if not (binary.is_file() and os.access(binary, os.X_OK)):
            log(f"TSan executable not found at {binary}", error=True)
            return 1

    print("=== TSan focused lane ===")
    print(f"Binary: {xray}")

    with workspace.Workspace("xray_tsan") as ws:
        log_prefix = ws.path("tsan")
        env = dict(os.environ)
        # Collect every race instead of aborting on the first: one warning must
        # not hide the others.
        env["TSAN_OPTIONS"] = f"halt_on_error=0 exitcode=86 log_path={log_prefix}"
        env["XRAY_WORKERS"] = WORKERS

        focused_failed = not run_kernel_ctests(build_dir, env, timeout, log)

        sys.stdout.write("  closed_thread_allocations               ")
        sys.stdout.flush()
        closed_result = proc.run([closed_threads, "--closed-threads"], env=env,
                                 timeout=CASE_TIMEOUT)
        if closed_result.timed_out:
            print(f"failed (timed out after {CASE_TIMEOUT}s)")
        elif closed_result.ok:
            print("passed")
        else:
            print(f"failed (nonzero exit {closed_result.returncode})")
        if not closed_result.ok:
            sanitizer.write_console(sys.stdout, closed_result.combined_text())

        failed_cases = []
        for mode, case in CASES:
            name = Path(case).stem
            sys.stdout.write(f"  {name:<40}")
            sys.stdout.flush()
            command = [xray, mode]
            report = None
            if mode == "test":
                report = ws.path(f"{name}.json")
                command.extend(("--report", report))
            command.append(PROJECT_DIR / case)
            result = proc.run(command, env=env, timeout=CASE_TIMEOUT)
            report_error = vm_test_report_error(report) if report is not None else None
            if result.timed_out:
                print(f"failed (timed out after {CASE_TIMEOUT}s)")
            elif not result.ok:
                print(f"failed (nonzero exit {result.returncode})")
            elif report_error:
                print("failed (no successful test report)")
            else:
                print("passed")
            if not result.ok or report_error:
                failed_cases.append(case)
                sanitizer.write_console(sys.stdout, result.combined_text())
                if report_error:
                    sanitizer.write_console(sys.stdout, report_error + "\n")

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

    if focused_failed or not closed_result.ok or failed_cases:
        print("")
        failed_steps = []
        if focused_failed:
            failed_steps.append("focused kernel CTest set")
        if not closed_result.ok:
            failed_steps.append("closed thread allocations")
        if failed_cases:
            failed_steps.append("VM cases: " + ", ".join(failed_cases))
        print("VERDICT: FAIL (" + " and ".join(failed_steps) + " failed)")
        return 1

    print("")
    print(
        "VERDICT: PASS (no ThreadSanitizer warnings across "
        f"{len(KERNEL_TESTS)} kernel CTests, closed thread allocations, "
        f"and {len(CASES)} VM cases)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

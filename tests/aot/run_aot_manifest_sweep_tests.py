#!/usr/bin/env python3
"""Sweep diff cases for accidental AOT runtime-link regressions.

This is a broad 115/132 guard: ordinary language/value/stdlib cases should not
quietly start pulling the coroutine/runtime archive. Explicit coroutine cases
are allowed; every other runtime dependency must be justified here.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
import tempfile
from typing import Any


PROJECT_DIR = pathlib.Path(__file__).resolve().parents[2]
DIFF_CASE_DIR = PROJECT_DIR / "tests" / "diff" / "cases"

EXPECTED_RUNTIME_PREFIXES = (
    "tests/diff/cases/semantics/coro/",
    # Generators lower to stackless coroutines, so they legitimately pull the
    # coroutine runtime archive.
    "tests/diff/cases/semantics/generator/",
)

EXPECTED_RUNTIME_CASES = {
    "tests/diff/cases/limits/select_cases_over_32.xr",
    "tests/diff/cases/semantics/cleanup/defer_async_await.xr",
    "tests/diff/cases/semantics/cleanup/defer_coroutine.xr",
    # sync primitives compose coroutine-aware CountdownLatch/Semaphore (and
    # Atomic lives in the same runtime archive), so these legitimately link
    # the coroutine runtime archive.
    "tests/diff/cases/semantics/concurrency/atomic_rawptr.xr",
    "tests/diff/cases/semantics/concurrency/barrier_compose.xr",
    "tests/diff/cases/semantics/concurrency/condvar_compose.xr",
    "tests/diff/cases/semantics/concurrency/mutex_cross_coroutine.xr",
    "tests/diff/cases/semantics/concurrency/mutex_generic_compose.xr",
    "tests/diff/cases/semantics/concurrency/once_compose.xr",
    "tests/diff/cases/semantics/concurrency/rwlock_generic_compose.xr",
    # Scheduler liveness cases intentionally exercise progress, cancellation,
    # deadlock, and orphan diagnostics. Their observable contract is provided
    # by the coroutine scheduler rather than the freestanding runtime.
    "tests/diff/cases/liveness/blocking_recv_progress.xr",
    "tests/diff/cases/liveness/busy_poll_progress.xr",
    "tests/diff/cases/liveness/busy_poll_progress_single_worker.xr",
    "tests/diff/cases/liveness/cancel_reaches_cpu_loop.xr",
    "tests/diff/cases/liveness/cancel_responsive_blocking.xr",
    "tests/diff/cases/liveness/deadlock_reported.xr",
    "tests/diff/cases/liveness/orphan_warned.xr",
    "tests/diff/cases/liveness/channel_handoff_not_deadlock.xr",
    # These fixtures contain a reachable coroutine/generator body.  The
    # executable closure therefore needs the coroutine runtime even though the
    # surrounding ownership/generic assertions are not scheduler tests.
    "tests/diff/cases/semantics/cleanup/defer_coroutine_local_panic_handler.xr",
    "tests/diff/cases/semantics/collections/method_self_return_ownership.xr",
    "tests/diff/cases/semantics/generic_iterable_constraint.xr",
    # runtime.liveBytes/liveObjects observe the execution-local reclamation
    # domain, whose identity and counters are owned by the coroutine runtime.
    "tests/diff/cases/semantics/modules/value_struct_return_release.xr",
    # time.sleep is a yieldable timer operation and the class payload exercises
    # runtime-owned object accounting.
    "tests/diff/cases/semantics/oop/native_class_reference_field_transfer.xr",
    # This fixture's `go` lambda is itself the runtime-backed behavior under
    # test, even though the captured values are otherwise ordinary locals.
    "tests/diff/cases/semantics/concurrency/go_lambda_local_scope.xr",
    # The hosted parallel module uses runtime-backed worker/task state. These
    # cases are the explicit positive coverage for that API surface, so the
    # coroutine runtime archive is expected.
    "tests/diff/cases/semantics/stdlib/parallel_api_reference.xr",
    "tests/diff/cases/semantics/stdlib/parallel_for_each_vm_batch.xr",
    "tests/diff/cases/semantics/stdlib/parallel_map_vm_batch.xr",
    "tests/diff/cases/semantics/stdlib/parallel_plan_close_during_dispatch.xr",
    "tests/diff/cases/semantics/stdlib/parallel_plan_close_lifecycle.xr",
    "tests/diff/cases/semantics/stdlib/parallel_plan_map_cleanup_after_panic.xr",
    "tests/diff/cases/semantics/stdlib/parallel_plan_nested_dispatch_cleanup.xr",
    "tests/diff/cases/semantics/stdlib/parallel_reduce_vm_batch.xr",
    # net.copyBidirectional launches two transfer tasks so both half-duplex
    # directions can make progress concurrently; these cases intentionally
    # exercise that runtime-backed implementation and therefore pull coro/task.
    "tests/diff/cases/semantics/stdlib/net_copy_bidirectional_error_enum.xr",
    "tests/diff/cases/semantics/stdlib/net_copy_bidirectional_cancel.xr",
    "tests/diff/cases/semantics/stdlib/net_copy_bidirectional_transfer.xr",
    # The direct byte-I/O loopback case runs its echo endpoint in a `go` task
    # and awaits it after the client exchange. The net calls stay direct, while
    # the fixture's orchestration intentionally requires coro/task support.
    "tests/diff/cases/semantics/stdlib/net_byte_io_direct.xr",
    # Typed Coro diagnostics lower directly to XI_CORO_OP and therefore need
    # the coroutine runtime that produces their object and enum values.
    "tests/diff/cases/semantics/stdlib/coro_typed_diagnostics_direct.xr",
    # Typed coroutine-local slots and CoroPool.submit are explicit wrappers
    # around coroutine runtime storage and task scheduling.
    "tests/diff/cases/semantics/stdlib/coro_typed_local_pool.xr",
    "tests/diff/cases/semantics/stdlib/coro_typed_local_shell_reuse.xr",
    "tests/diff/cases/semantics/stdlib/coro_typed_local_sync.xr",
    # These fixtures execute file, process, logging, or HTTP form-data paths
    # whose reachable stdlib operations may suspend on hosted I/O.
    "tests/diff/cases/semantics/stdlib/http_formdata_pure_direct.xr",
    "tests/diff/cases/semantics/stdlib/io_binary_file_boundary_direct.xr",
    "tests/diff/cases/semantics/stdlib/io_chmod_shared_core.xr",
    "tests/diff/cases/semantics/stdlib/io_path_result_shared_core.xr",
    "tests/diff/cases/semantics/stdlib/io_read_dir_shared_core.xr",
    "tests/diff/cases/semantics/stdlib/io_remove_all_shared_core.xr",
    "tests/diff/cases/semantics/stdlib/io_system_direct.xr",
    "tests/diff/cases/semantics/stdlib/io_touch_shared_core.xr",
    "tests/diff/cases/semantics/stdlib/io_write_shared_core.xr",
    "tests/diff/cases/semantics/stdlib/log_pure_module_direct.xr",
    "tests/diff/cases/semantics/stdlib/sys_process_direct.xr",
    # Timer, parallel worker, and runtime-domain channel fixtures directly
    # exercise services implemented by the coroutine runtime archive.
    "tests/diff/cases/semantics/stdlib/os_sleep_system_direct.xr",
    "tests/diff/cases/semantics/stdlib/parallel_default_options.xr",
    "tests/diff/cases/semantics/stdlib/runtime_domain_bytes.xr",
    # `sync.fence` itself lowers to the freestanding mem.fence helper, but the
    # hosted `sync` module is a pure-Xray script module that also exports
    # coroutine-aware Mutex/RwLock/Once/Barrier/Condvar. Current AOT compiles
    # script modules as whole units, so importing sync intentionally pulls those
    # runtime-backed definitions into the manifest.
    "tests/diff/cases/semantics/stdlib/sync_fence_alias.xr",
    "tests/diff/cases/semantics/stdlib/sync_cache_padded.xr",
    "tests/diff/cases/semantics/stdlib/sync_module_import.xr",
    "tests/diff/cases/semantics/stdlib/sync_namespace_import.xr",
    "tests/diff/cases/semantics/stdlib/sys_thread_const_function_value.xr",
    "tests/diff/cases/semantics/stdlib/sys_thread_os_primitives_cross_thread.xr",
    "tests/diff/cases/semantics/stdlib/sys_threadlocal_basic.xr",
    "tests/diff/cases/semantics/stdlib/sys_threadlocal_nullable.xr",
    "tests/diff/cases/semantics/stdlib/sys_signal_on_signal.xr",
    "tests/diff/cases/semantics/modules/xmod_coro.xr",
    "tests/diff/cases/semantics/ownership/in_go_copy_argument_allowed.xr",
    "tests/diff/cases/semantics/ownership/move_into_go.xr",
    "tests/diff/cases/semantics/ownership/converged_capability_positive.xr",
    "tests/diff/cases/semantics/ownership/shared_class_copy_go.xr",
    "tests/diff/cases/semantics/ownership/shared_freeze_go.xr",
}


def rel(path: pathlib.Path) -> str:
    return path.relative_to(PROJECT_DIR).as_posix()


def is_expected_runtime_case(case: str) -> bool:
    return case in EXPECTED_RUNTIME_CASES or any(
        case.startswith(prefix) for prefix in EXPECTED_RUNTIME_PREFIXES
    )


def expects_closed_world_rejection(case: pathlib.Path) -> bool:
    """A VM-only differential case is still checked, not silently skipped.

    The sole VM-only fixture pins the intentional closed-world boundary: AOT
    must reject its open callable target with the canonical user diagnostic.
    """
    return "// diff-backends: vm" in case.read_text(encoding="utf-8").splitlines()


def run_closed_world_rejection(xray: pathlib.Path, case: pathlib.Path,
                               out_c: pathlib.Path) -> None:
    result = subprocess.run(
        [str(xray), "build", "--native", "-c", "-o", str(out_c), str(case)],
        cwd=PROJECT_DIR,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    output = result.stdout + result.stderr
    canonical = b"native compilation cannot prove the target of an indirect call"
    if result.returncode == 0 or canonical not in output:
        excerpt = repr(b"\n".join(output.strip().splitlines()[:40]))
        raise RuntimeError(
            f"expected canonical closed-world AOT rejection for {rel(case)}\n{excerpt}"
        )


def manifest_from_stdout(stdout: bytes) -> dict[str, Any]:
    start = stdout.find(b"{")
    end = stdout.rfind(b"}")
    if start < 0 or end < start:
        raise ValueError("xray output did not contain a JSON manifest")
    return json.loads(stdout[start : end + 1].decode("utf-8", "strict"))


def run_manifest(xray: pathlib.Path, case: pathlib.Path, out_c: pathlib.Path) -> dict[str, Any]:
    proc = subprocess.run(
        [
            str(xray),
            "build",
            "--native",
            "--dump-link-manifest",
            "-c",
            "-o",
            str(out_c),
            str(case),
        ],
        cwd=PROJECT_DIR,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if proc.returncode != 0:
        detail = (proc.stderr or proc.stdout).strip().splitlines()
        excerpt = repr(b"\n".join(detail[:40]))
        raise RuntimeError(f"xray build failed for {rel(case)}\n{excerpt}")
    return manifest_from_stdout(proc.stdout)


def collect_cases() -> list[pathlib.Path]:
    return sorted(
        path
        for path in DIFF_CASE_DIR.rglob("*.xr")
        if path.is_file() and not path.name.startswith("_")
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Check that non-runtime diff cases keep AOT manifests freestanding."
    )
    parser.add_argument(
        "--xray",
        default=str(PROJECT_DIR / "build" / "xray"),
        help="Path to the xray binary",
    )
    args = parser.parse_args(argv)

    xray = pathlib.Path(args.xray)
    cases = collect_cases()
    failures: list[str] = []
    checked = 0
    expected_runtime = 0
    checked_rejections = 0

    with tempfile.TemporaryDirectory(prefix="xray_aot_manifest_sweep.") as tmp:
        out_c = pathlib.Path(tmp) / "case.c"
        for case in cases:
            case_rel = rel(case)
            try:
                if expects_closed_world_rejection(case):
                    run_closed_world_rejection(xray, case, out_c)
                    checked_rejections += 1
                    continue
                manifest = run_manifest(xray, case, out_c)
            except Exception as exc:  # keep sweeping so the report is actionable
                failures.append(str(exc))
                continue

            checked += 1
            runtime_caps = manifest.get("runtime_caps") or []
            runtime_objects = manifest.get("runtime_objects") or []
            stdlib_objects = manifest.get("stdlib_objects") or []

            if "xray_core" in runtime_objects or "xray_core" in stdlib_objects:
                failures.append(f"{case_rel}: manifest references xray_core")

            if runtime_objects:
                if is_expected_runtime_case(case_rel):
                    expected_runtime += 1
                    if runtime_objects != ["xray_rt_coro"]:
                        failures.append(
                            f"{case_rel}: runtime contract has unexpected runtime_objects "
                            f"{runtime_objects!r}"
                        )
                    continue
                failures.append(
                    f"{case_rel}: unexpected runtime dependency caps={runtime_caps!r} "
                    f"objects={runtime_objects!r}"
                )

    if failures:
        print("FAIL: AOT manifest sweep found runtime-link regressions", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1

    print(
        f"PASS: AOT manifest sweep checked {checked} diff cases; "
        f"{expected_runtime} explicit runtime contracts and "
        f"{checked_rejections} closed-world rejection contracts verified"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

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

ALLOWED_RUNTIME_PREFIXES = (
    "tests/diff/cases/semantics/coro/",
    # Generators lower to stackless coroutines, so they legitimately pull the
    # coroutine runtime archive.
    "tests/diff/cases/semantics/generator/",
)

ALLOWED_RUNTIME_CASES = {
    "tests/diff/cases/limits/select_cases_over_32.xr",
    "tests/diff/cases/semantics/cleanup/defer_async_await.xr",
    "tests/diff/cases/semantics/cleanup/defer_coroutine.xr",
    # sync primitives compose coroutine-aware CountdownLatch/Semaphore, so
    # they legitimately link the coroutine runtime archive.
    "tests/diff/cases/semantics/concurrency/barrier_compose.xr",
    "tests/diff/cases/semantics/concurrency/condvar_compose.xr",
    "tests/diff/cases/semantics/concurrency/mutex_cross_coroutine.xr",
    "tests/diff/cases/semantics/concurrency/mutex_generic_compose.xr",
    "tests/diff/cases/semantics/concurrency/once_compose.xr",
    "tests/diff/cases/semantics/concurrency/rwlock_generic_compose.xr",
    "tests/diff/cases/semantics/stdlib/sync_module_import.xr",
    "tests/diff/cases/semantics/stdlib/sync_namespace_import.xr",
    "tests/diff/cases/semantics/modules/xmod_coro.xr",
    "tests/diff/cases/semantics/ownership/in_go_copy_argument_allowed.xr",
    "tests/diff/cases/semantics/ownership/move_into_go.xr",
    "tests/diff/cases/semantics/ownership/shared_class_copy_go.xr",
    "tests/diff/cases/semantics/ownership/shared_freeze_go.xr",
}


def rel(path: pathlib.Path) -> str:
    return path.relative_to(PROJECT_DIR).as_posix()


def is_allowed_runtime_case(case: str) -> bool:
    return case in ALLOWED_RUNTIME_CASES or any(
        case.startswith(prefix) for prefix in ALLOWED_RUNTIME_PREFIXES
    )


def manifest_from_stdout(stdout: str) -> dict[str, Any]:
    start = stdout.find("{")
    end = stdout.rfind("}")
    if start < 0 or end < start:
        raise ValueError("xray output did not contain a JSON manifest")
    return json.loads(stdout[start : end + 1])


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
        text=True,
    )
    if proc.returncode != 0:
        detail = (proc.stderr or proc.stdout).strip().splitlines()
        excerpt = "\n".join(detail[:40])
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
    allowed_runtime = 0

    with tempfile.TemporaryDirectory(prefix="xray_aot_manifest_sweep.") as tmp:
        out_c = pathlib.Path(tmp) / "case.c"
        for case in cases:
            case_rel = rel(case)
            try:
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
                if is_allowed_runtime_case(case_rel):
                    allowed_runtime += 1
                    if runtime_objects != ["xray_rt_coro"]:
                        failures.append(
                            f"{case_rel}: allowed runtime case has unexpected runtime_objects "
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
        f"{allowed_runtime} explicit runtime cases allowed"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

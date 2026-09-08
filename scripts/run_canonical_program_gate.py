#!/usr/bin/env python3
"""Run a small, explicit canonical-Program build-and-test lane.

The lane never configures a build tree.  It first builds the exact executables
owned by the selected lane, then asks CTest to run only those tests.  Missing
targets, registrations, executables, or test results fail closed.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests/lib"))

from xraytest import sanitizer  # noqa: E402


LANES = {
    "semantic": (
        (
            "test_xr_program",
            "test_xr_program_source_build",
            "test_xr_program_verify",
            "test_xr_program_vm",
            "test_xr_program_aot",
        ),
        (
            "test_xr_program",
            "test_xr_program_source_build",
            "test_xr_program_verify",
            "test_xr_program_vm",
            "test_xr_program_aot",
        ),
    ),
    "native": (
        (
            "test_xr_program",
            "test_xr_program_source_build",
            "test_xr_program_verify",
            "test_xr_program_vm",
            "test_xr_program_aot",
            "test_xr_program_coroutine_graph_aot_native",
            "test_xr_program_child_coroutine_trap_cleanup_aot_native",
        ),
        (
            "xr_program_coroutine_graph_native_self_test",
            "test_xr_program_coroutine_graph_aot_native",
            "test_xr_program_child_coroutine_trap_cleanup_aot_native",
            "test_xr_program",
            "test_xr_program_source_build",
            "test_xr_program_verify",
            "test_xr_program_vm",
            "test_xr_program_aot",
        ),
    ),
    "asan-source": (
        ("test_xr_program_source_build",),
        ("test_xr_program_source_build",),
    ),
}


def exact_regex(names: tuple[str, ...]) -> str:
    return "^(" + "|".join(re.escape(name) for name in names) + ")$"


def build_plan(lane: str, build_dir: Path, jobs: int) -> tuple[list[str], list[str]]:
    targets, tests = LANES[lane]
    build = ["cmake", "--build", str(build_dir), "-j", str(jobs), "--target", *targets]
    test = [
        "ctest",
        "--test-dir",
        str(build_dir),
        "--output-on-failure",
        "--no-tests=error",
        "-j",
        str(min(jobs, len(tests))),
        "-R",
        exact_regex(tests),
    ]
    return build, test


def activate_environment(lane: str, build_dir: Path) -> None:
    def log(message: str, **kwargs: object) -> None:
        stream = sys.stderr if kwargs.get("error") else sys.stdout
        print(message, file=stream)

    if not sanitizer.activate_windows_msvc_environment(log):
        raise RuntimeError("MSVC x64 SDK environment is unavailable")
    if lane != "asan-source":
        return
    spec = sanitizer.BuildSpec(
        build_dir=build_dir,
        sanitizer_flags=("ENABLE_ASAN=ON", "ENABLE_UBSAN=ON"),
        c_compiler="clang-cl" if os.name == "nt" else "clang",
        cxx_compiler="clang-cl" if os.name == "nt" else "clang++",
    )
    mismatches = [
        error
        for flag in spec.verification_targets()
        if (error := sanitizer.verify_configured(build_dir, flag))
    ]
    if mismatches:
        raise RuntimeError("; ".join(mismatches))
    if not sanitizer.activate_windows_dynamic_asan_runtime(spec, log):
        raise RuntimeError("dynamic ASan runtime is unavailable")
    os.environ.setdefault("ASAN_OPTIONS", "detect_leaks=0:halt_on_error=1:abort_on_error=1")
    os.environ.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")


def run(command: list[str]) -> float:
    started = time.perf_counter()
    result = subprocess.run(command, cwd=ROOT, check=False)
    elapsed = time.perf_counter() - started
    if result.returncode:
        raise subprocess.CalledProcessError(result.returncode, command)
    return elapsed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--lane", choices=tuple(LANES), default="native")
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--jobs", type=int, default=max(1, min(os.cpu_count() or 1, 8)))
    parser.add_argument("--plan", action="store_true",
                        help="print the exact commands without executing them")
    args = parser.parse_args()
    if args.jobs <= 0:
        parser.error("--jobs must be positive")
    build_dir = (
        args.build_dir or ROOT / ("build-asan" if args.lane == "asan-source" else "build")
    )
    build_dir = build_dir.resolve()
    build, test = build_plan(args.lane, build_dir, args.jobs)
    if args.plan:
        print(json.dumps({"lane": args.lane, "build": build, "test": test}, indent=2))
        return 0
    if not (build_dir / "CMakeCache.txt").is_file():
        parser.error(f"configured build tree is required: {build_dir}")
    try:
        activate_environment(args.lane, build_dir)
        build_seconds = run(build)
        test_seconds = run(test)
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"canonical Program {args.lane} gate failed: {error}", file=sys.stderr)
        return 1
    print(
        json.dumps(
            {
                "lane": args.lane,
                "build_seconds": round(build_seconds, 3),
                "test_seconds": round(test_seconds, 3),
                "total_seconds": round(build_seconds + test_seconds, 3),
                "targets": len(LANES[args.lane][0]),
                "tests": len(LANES[args.lane][1]),
            },
            separators=(",", ":"),
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

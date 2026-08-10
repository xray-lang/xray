#!/usr/bin/env python3
"""Record a reproducible correctness baseline for the target-machine cutover."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import re
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any


if sys.version_info < (3, 11):
    raise SystemExit("run_baseline.py requires Python 3.11 or newer")


SCHEMA = 1
RUNNER_VERSION = "target-machine-baseline/1"
STARTUP_PATTERN = (
    "^(backend_diff|aot_filetests|aot_manifest_sweep|aot_standalone_suite|"
    "aot_isolate_symbol_gate|compile_error_tests|http_static_route_full_parse|"
    "test_xrt_execution_arena)$"
)
DIFF_PATTERN = "^(backend_diff_optimized|backend_diff_deterministic)$"
META_PATTERN = "^(meta_ownership_inventory|contract_freeze|target_machine_inventory)$"


@dataclass(frozen=True)
class Lane:
    name: str
    ctest_args: tuple[str, ...]
    timeout_seconds: int
    repeat: bool


CORE_LANES = (
    Lane("startup-debt", ("-R", STARTUP_PATTERN), 1200, True),
    Lane("differential-modes", ("-R", DIFF_PATTERN), 1200, True),
    Lane("architecture-contracts", ("-R", META_PATTERN), 120, True),
)

FULL_LANES = (
    Lane("non-sanitizer-suite", ("-LE", "sanitizer"), 3600, False),
    Lane("asan-ubsan", ("-R", "^asan_focused$"), 1800, False),
    Lane("tsan", ("-R", "^tsan_focused$"), 2400, False),
)


def run_capture(command: list[str], cwd: Path, timeout: int) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
        check=False,
        env={**os.environ, "NO_COLOR": "1"},
    )


def command_text(command: list[str], root: Path) -> list[str]:
    root_text = str(root)
    return [part.replace(root_text, "${SOURCE_ROOT}") for part in command]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git(root: Path, *args: str) -> str:
    result = run_capture(["git", *args], root, 30)
    if result.returncode != 0:
        raise RuntimeError(result.stdout.strip() or "git command failed")
    return result.stdout.strip()


def tool_version(command: list[str], root: Path) -> str:
    result = run_capture(command, root, 30)
    first = result.stdout.strip().splitlines()
    return first[0] if result.returncode == 0 and first else "unavailable"


def compiler_identity(root: Path, build: Path, allow_dirty: bool) -> dict[str, Any]:
    binary = build / "xray"
    if not binary.is_file():
        raise RuntimeError(f"compiler binary missing: {binary}")
    status = git(root, "status", "--porcelain=v1")
    head = git(root, "rev-parse", "HEAD")
    tree = git(root, "rev-parse", "HEAD^{tree}")
    result = run_capture([str(binary), "--version", "--json"], root, 30)
    if result.returncode != 0:
        raise RuntimeError(f"compiler version query failed: {result.stdout.strip()}")
    try:
        version = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError(f"compiler version is not JSON: {error}") from error
    errors = []
    if version.get("commit") != head:
        errors.append(f"binary commit {version.get('commit')} != HEAD {head}")
    if bool(version.get("dirty")) != bool(status):
        errors.append("binary dirty flag does not match Git status")
    if status and not allow_dirty:
        errors.append("worktree is dirty")
    if errors:
        raise RuntimeError("; ".join(errors))
    return {
        "git_commit": head,
        "git_tree": tree,
        "git_dirty": bool(status),
        "version": version,
        "binary_sha256": sha256(binary),
        "binary_size_bytes": binary.stat().st_size,
    }


def host_info(root: Path, build: Path) -> dict[str, Any]:
    cache = build / "CMakeCache.txt"
    cache_text = cache.read_text(encoding="utf-8") if cache.is_file() else ""

    def cache_value(name: str) -> str:
        match = re.search(rf"^{re.escape(name)}:[^=]*=(.*)$", cache_text, re.M)
        return match.group(1).strip() if match else "unknown"

    cpu = platform.processor() or platform.machine()
    memory = "unknown"
    if sys.platform == "darwin":
        cpu_result = run_capture(["sysctl", "-n", "machdep.cpu.brand_string"], root, 10)
        if cpu_result.returncode == 0 and cpu_result.stdout.strip():
            cpu = cpu_result.stdout.strip()
        memory_result = run_capture(["sysctl", "-n", "hw.memsize"], root, 10)
        if memory_result.returncode == 0 and memory_result.stdout.strip().isdigit():
            memory = int(memory_result.stdout.strip())
    return {
        "os": platform.platform(),
        "system": platform.system(),
        "release": platform.release(),
        "machine": platform.machine(),
        "cpu": cpu,
        "ram_bytes": memory,
        "python": platform.python_version(),
        "cmake": tool_version(["cmake", "--version"], root),
        "ninja": tool_version(["ninja", "--version"], root),
        "c_compiler": cache_value("CMAKE_C_COMPILER"),
        "c_compiler_id": cache_value("CMAKE_C_COMPILER_ID"),
        "c_compiler_version": cache_value("CMAKE_C_COMPILER_VERSION"),
        "build_type": cache_value("CMAKE_BUILD_TYPE"),
        "xray_python": cache_value("XRAY_PYTHON"),
        "logical_cpus": os.cpu_count(),
    }


def parse_ctest_summary(output: str) -> dict[str, int | None]:
    match = re.search(r"([0-9]+)% tests passed, ([0-9]+) tests failed out of ([0-9]+)", output)
    if not match:
        return {"percent_passed": None, "failed": None, "total": None}
    return {
        "percent_passed": int(match.group(1)),
        "failed": int(match.group(2)),
        "total": int(match.group(3)),
    }


def percentile(values: list[float], quantile: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    position = (len(ordered) - 1) * quantile
    low = math.floor(position)
    high = math.ceil(position)
    if low == high:
        return ordered[low]
    fraction = position - low
    return ordered[low] * (1.0 - fraction) + ordered[high] * fraction


def lane_stats(results: list[dict[str, Any]]) -> dict[str, Any]:
    durations = [float(row["duration_seconds"]) for row in results]
    mean = statistics.fmean(durations) if durations else 0.0
    deviation = statistics.pstdev(durations) if len(durations) > 1 else 0.0
    return {
        "runs": len(results),
        "p50_seconds": round(percentile(durations, 0.50), 6),
        "p95_seconds": round(percentile(durations, 0.95), 6),
        "coefficient_of_variation": round(deviation / mean, 6) if mean else 0.0,
    }


def execute_lane(root: Path, build: Path, output: Path, lane: Lane, iteration: int) -> dict[str, Any]:
    command = [
        "ctest", "--test-dir", str(build), "--output-on-failure", *lane.ctest_args
    ]
    started = time.perf_counter()
    timed_out = False
    try:
        result = run_capture(command, root, lane.timeout_seconds)
        returncode = result.returncode
        content = result.stdout
    except subprocess.TimeoutExpired as error:
        timed_out = True
        returncode = 124
        content = (error.stdout or "") + "\nTIMEOUT\n"
    duration = time.perf_counter() - started
    log = output / f"{lane.name}.run-{iteration}.log"
    log.write_text(content, encoding="utf-8")
    summary = parse_ctest_summary(content)
    status = "passed" if returncode == 0 and not timed_out and summary["failed"] == 0 else "failed"
    return {
        "iteration": iteration,
        "status": status,
        "returncode": returncode,
        "timed_out": timed_out,
        "duration_seconds": round(duration, 6),
        "ctest": summary,
        "log": log.name,
        "log_sha256": sha256(log),
        "command": command_text(command, root),
    }


def render_manifest(root: Path, build: Path, output: Path, repeat: int, scope: str,
                    allow_dirty: bool) -> tuple[dict[str, Any], bool]:
    identity = compiler_identity(root, build, allow_dirty)
    lanes = list(CORE_LANES)
    if scope == "full":
        lanes.extend(FULL_LANES)
    grouped = []
    all_passed = True
    for lane in lanes:
        runs = repeat if lane.repeat else 1
        results = []
        for iteration in range(1, runs + 1):
            print(f"[{lane.name}] run {iteration}/{runs}", flush=True)
            result = execute_lane(root, build, output, lane, iteration)
            results.append(result)
            all_passed = all_passed and result["status"] == "passed"
            print(
                f"[{lane.name}] {result['status']} in {result['duration_seconds']:.3f}s ",
                f"({result['ctest']['total']} tests)",
                flush=True,
            )
        grouped.append({
            "name": lane.name,
            "repeat_policy": "three-or-more" if lane.repeat else "single-heavy-gate",
            "timeout_seconds": lane.timeout_seconds,
            "runs": results,
            "timing": lane_stats(results),
        })
    manifest = {
        "schema": SCHEMA,
        "runner": RUNNER_VERSION,
        "scope": scope,
        "source": identity,
        "environment": host_info(root, build),
        "policy": {
            "repeat_count": repeat,
            "minimum_python": "3.11",
            "known_failure_allowlist": False,
            "failed_to_skip_reclassification": False,
            "logs_retained_out_of_tree": True,
            "artifact_identity": "git-tree plus compiler-version-json plus binary-sha256",
        },
        "result": "passed" if all_passed else "failed",
        "lanes": grouped,
    }
    return manifest, all_passed


def self_test() -> int:
    parsed = parse_ctest_summary("100% tests passed, 0 tests failed out of 8")
    assert parsed == {"percent_passed": 100, "failed": 0, "total": 8}
    assert percentile([1.0, 2.0, 3.0], 0.5) == 2.0
    assert round(percentile([1.0, 2.0, 3.0], 0.95), 3) == 2.9
    stats = lane_stats([{"duration_seconds": 1.0}, {"duration_seconds": 1.0}])
    assert stats["coefficient_of_variation"] == 0.0
    print("target-machine baseline runner self-test: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".")
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--output-dir", default="build/target-machine/phase0")
    parser.add_argument("--manifest")
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--scope", choices=("core", "full"), default="core")
    parser.add_argument("--allow-dirty", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.repeat < 3:
        parser.error("--repeat must be at least 3")
    root = Path(args.root).resolve()
    build = (root / args.build_dir).resolve() if not Path(args.build_dir).is_absolute() else Path(args.build_dir)
    output = (root / args.output_dir).resolve() if not Path(args.output_dir).is_absolute() else Path(args.output_dir)
    output.mkdir(parents=True, exist_ok=True)
    try:
        manifest, passed = render_manifest(root, build, output, args.repeat, args.scope, args.allow_dirty)
    except (OSError, RuntimeError, subprocess.SubprocessError, json.JSONDecodeError) as error:
        print(f"target-machine baseline: ERROR: {error}", file=sys.stderr)
        return 2
    evidence = output / "baseline-evidence.json"
    rendered = json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    evidence.write_text(rendered, encoding="utf-8")
    if args.manifest:
        destination = (root / args.manifest).resolve() if not Path(args.manifest).is_absolute() else Path(args.manifest)
        destination.write_text(rendered, encoding="utf-8")
    print(f"target-machine baseline: {manifest['result'].upper()} ({evidence})")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())

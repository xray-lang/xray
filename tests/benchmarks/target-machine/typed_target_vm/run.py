#!/usr/bin/env python3
"""Reproducible gate for the typed TargetPlan scalar/call/frame probe."""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import statistics
import subprocess
import sys
from typing import Any


RUNNER = "typed-target-vm-performance/2"
DEFAULT_POLICY = Path("contracts/target-machine/baseline-manifest.json")
ARITHMETIC_OPERATIONS = 512
SCALAR_EXECUTIONS = 512
CALL_EXECUTIONS = 512
FRAME_ITERATIONS = 1024


def percentile(values: list[float], quantile: float) -> float:
    ordered = sorted(values)
    position = (len(ordered) - 1) * quantile
    low = math.floor(position)
    high = math.ceil(position)
    if low == high:
        return ordered[low]
    fraction = position - low
    return ordered[low] * (1.0 - fraction) + ordered[high] * fraction


def distribution(values: list[float], unit: str) -> dict[str, Any]:
    mean = statistics.fmean(values)
    deviation = statistics.pstdev(values) if len(values) > 1 else 0.0
    return {
        "samples": len(values),
        "unit": unit,
        "p50": round(percentile(values, 0.50), 6),
        "p95": round(percentile(values, 0.95), 6),
        "mean": round(mean, 6),
        "coefficient_of_variation": round(deviation / mean, 6) if mean else 0.0,
    }


def geometric_mean(values: list[float]) -> float:
    if not values or any(value <= 0 for value in values):
        raise ValueError("geometric mean requires positive values")
    return math.exp(statistics.fmean(math.log(value) for value in values))


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_identity(root: Path) -> dict[str, Any]:
    def git(*arguments: str) -> str:
        return subprocess.check_output(
            ["git", *arguments], cwd=root, text=True, encoding="utf-8",
            errors="strict",
        ).strip()

    return {
        "git_commit": git("rev-parse", "HEAD"),
        "git_dirty": bool(git("status", "--porcelain=v1", "--untracked-files=all")),
    }


def load_protocol(path: Path) -> dict[str, Any]:
    policy = json.loads(path.read_text(encoding="utf-8"))
    performance = policy.get("performance", {})
    if performance.get("sample_count") != 7:
        raise ValueError("typed VM gate requires the frozen seven-sample protocol")
    max_cv = performance.get("max_coefficient_of_variation")
    if not isinstance(max_cv, (int, float)) or not 0 < max_cv <= 1:
        raise ValueError("invalid frozen coefficient-of-variation policy")
    logical_cpu = performance.get("logical_cpu")
    if not isinstance(logical_cpu, int) or isinstance(logical_cpu, bool):
        raise ValueError("invalid frozen logical CPU")
    timeout = performance.get("timeout_seconds")
    if not isinstance(timeout, int) or timeout < 1:
        raise ValueError("invalid frozen timeout")
    return performance


def pin_runner(cpu: int) -> str:
    if os.name != "nt":
        if not hasattr(os, "sched_setaffinity"):
            raise RuntimeError("host cannot enforce benchmark CPU affinity")
        os.sched_setaffinity(0, {cpu})
        return f"logical-cpu-{cpu}"
    if cpu < 0 or cpu >= ctypes.sizeof(ctypes.c_size_t) * 8:
        raise ValueError("logical CPU is outside the Windows processor group")
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    current = kernel32.GetCurrentProcess()
    kernel32.SetProcessAffinityMask.argtypes = [wintypes.HANDLE, ctypes.c_size_t]
    kernel32.SetProcessAffinityMask.restype = wintypes.BOOL
    if not kernel32.SetProcessAffinityMask(current, ctypes.c_size_t(1 << cpu)):
        raise OSError(ctypes.get_last_error(), "SetProcessAffinityMask failed")
    kernel32.SetPriorityClass.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel32.SetPriorityClass.restype = wintypes.BOOL
    if not kernel32.SetPriorityClass(current, 0x00000080):
        raise OSError(ctypes.get_last_error(), "SetPriorityClass failed")
    return f"logical-cpu-{cpu}"


def active_power_policy(performance: dict[str, Any]) -> str:
    if os.name != "nt":
        return "not-governed-by-this-windows-slice"
    expected = performance.get("power_policy", {}).get(
        "windows_active_scheme_guid"
    )
    result = subprocess.run(
        ["powercfg", "/getactivescheme"], text=True, encoding="utf-8",
        errors="replace", stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        check=False, timeout=10,
    )
    match = re.search(
        r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}",
        result.stdout.lower(),
    )
    if result.returncode != 0 or not match or match.group(0) != expected:
        raise RuntimeError(
            f"active Windows power scheme is not frozen scheme {expected}"
        )
    return match.group(0)


def normalized_samples(raw: dict[str, Any], count: int, units: int) -> list[float]:
    samples = raw.get("samples_ns")
    if (not isinstance(samples, list) or len(samples) != count or units < 1 or
            any(not isinstance(value, int) or value <= 0 for value in samples)):
        raise ValueError("probe returned invalid timing samples")
    return [value / units for value in samples]


def validate_footprint(probe: dict[str, Any]) -> tuple[dict[str, Any], list[str]]:
    errors: list[str] = []
    footprint = probe.get("footprint", {})
    keys = (
        "fixed_frame_bytes", "arena_allocation_bytes",
        "alignment_padding_bytes", "slot_state_metadata_bytes",
        "lifecycle_state_metadata_bytes", "total_bytes",
        "slot_count", "plan_frame_bytes", "frame_alignment",
        "max_total_bytes",
    )
    if any(not isinstance(footprint.get(key), int) for key in keys):
        return {}, ["footprint fields are not exact integers"]
    components = (
        footprint["fixed_frame_bytes"] + footprint["arena_allocation_bytes"] +
        footprint["alignment_padding_bytes"] +
        footprint["slot_state_metadata_bytes"] +
        footprint["lifecycle_state_metadata_bytes"]
    )
    if components != footprint["total_bytes"]:
        errors.append("frame total is not the exact sum of its payload components")
    if footprint["arena_allocation_bytes"] != footprint["plan_frame_bytes"]:
        errors.append("frame arena bytes do not equal the verified packed slot layout")
    if footprint["total_bytes"] > footprint["max_total_bytes"]:
        errors.append("frame payload exceeds the production frame limit")
    if (footprint["frame_alignment"] < 1 or
            footprint["alignment_padding_bytes"] >=
            footprint["frame_alignment"]):
        errors.append("frame alignment padding is not bounded by frame alignment")
    if (not probe.get("release_build") or
            probe.get("slot_state_metadata_enabled") != 0 or
            footprint["slot_state_metadata_bytes"] != 0):
        errors.append("Release frame retains per-slot state/tag metadata")
    derived = {
        **footprint,
        "bounded_metadata_bytes": (
            footprint["fixed_frame_bytes"] +
            footprint["alignment_padding_bytes"] +
            footprint["slot_state_metadata_bytes"] +
            footprint["lifecycle_state_metadata_bytes"]
        ),
        "external_allocator_overhead_included": False,
    }
    return derived, errors


def run_gate(args: argparse.Namespace) -> dict[str, Any]:
    performance = load_protocol(args.policy)
    samples = performance["sample_count"]
    max_cv = float(performance["max_coefficient_of_variation"])
    warmups = max(row["warmups"] for row in performance["lanes"])
    affinity = pin_runner(performance["logical_cpu"])
    power = active_power_policy(performance)
    command = [
        str(args.benchmark), str(warmups), str(samples),
        str(ARITHMETIC_OPERATIONS),
        str(SCALAR_EXECUTIONS), str(CALL_EXECUTIONS), str(FRAME_ITERATIONS),
    ]
    completed = subprocess.run(
        command, text=True, encoding="utf-8", errors="replace",
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        timeout=performance["timeout_seconds"],
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"benchmark probe failed ({completed.returncode}): {completed.stderr.strip()}"
        )
    probe = json.loads(completed.stdout)
    if probe.get("schema") != 2 or probe.get("sample_count") != samples:
        raise ValueError("benchmark probe schema/protocol mismatch")
    if (probe.get("warmup_runs") != warmups or
            probe.get("scalar", {}).get("arithmetic_operations_per_execution") !=
            ARITHMETIC_OPERATIONS or
            probe.get("scalar", {}).get("executions_per_sample") !=
            SCALAR_EXECUTIONS or
            probe.get("call", {}).get("direct_calls_per_execution") != 1 or
            probe.get("call", {}).get("executions_per_sample") !=
            CALL_EXECUTIONS or
            probe.get("call", {}).get("argument_count") != 1 or
            probe.get("call", {}).get("adapter_count") != 0 or
            probe.get("call", {}).get("argument_staging") !=
            "immutable-target-plan-call-argument-rows" or
            probe.get("frame_memory", {}).get("frames_per_sample") !=
            FRAME_ITERATIONS):
        raise ValueError("benchmark probe workload/protocol mismatch")
    scalar_units = ARITHMETIC_OPERATIONS * SCALAR_EXECUTIONS
    scalar_samples = normalized_samples(probe["scalar"], samples, scalar_units)
    call_samples = normalized_samples(
        probe["call"], samples, CALL_EXECUTIONS
    )
    frame_samples = normalized_samples(
        probe["frame_memory"], samples, FRAME_ITERATIONS
    )
    scalar = distribution(scalar_samples, "nanoseconds-per-arithmetic-operation")
    call = distribution(call_samples, "nanoseconds-per-direct-call")
    frame = distribution(frame_samples, "nanoseconds-per-frame-create-free")
    footprint, errors = validate_footprint(probe)
    if probe["call"].get("runtime_generic_argument_array_bytes") != 0:
        errors.append("direct call uses a runtime generic argument array")
    identity = source_identity(args.policy.resolve().parents[2])
    if identity["git_dirty"]:
        errors.append("source worktree is dirty")
    if probe.get("build_dirty"):
        errors.append("benchmark binary was configured from a dirty worktree")
    if probe.get("build_commit") != identity["git_commit"]:
        errors.append("benchmark binary commit does not match source commit")
    if probe.get("build_profile") != "Release" or not probe.get("release_build"):
        errors.append("benchmark binary is not the Release profile")
    for name, lane in (("scalar", scalar), ("direct-call", call),
                       ("frame-memory", frame)):
        if lane["coefficient_of_variation"] > max_cv:
            errors.append(
                f"{name} coefficient of variation exceeds {max_cv}"
            )
    summary = {
        "geometric_mean_lane_median_ns_per_work_unit": round(
            geometric_mean([scalar["p50"], call["p50"], frame["p50"]]), 6
        ),
        "throughput_qualification": "recorded-baseline-no-frozen-numeric-threshold",
    }
    return {
        "schema": "xray.typed-target-vm.performance.v2",
        "runner": RUNNER,
        "result": "passed" if not errors else "failed",
        "errors": errors,
        "protocol": {
            "warmup_runs": probe["warmup_runs"],
            "sample_count": samples,
            "maximum_coefficient_of_variation": max_cv,
            "logical_cpu": performance["logical_cpu"],
            "affinity": affinity,
            "windows_active_power_scheme_guid": power,
            "timeout_seconds": performance["timeout_seconds"],
        },
        "identity": {
            **identity,
            "binary_commit": probe.get("build_commit"),
            "binary_dirty": probe.get("build_dirty"),
            "build_profile": probe.get("build_profile"),
            "benchmark_executable_sha256": sha256(args.benchmark),
            "runner_sha256": sha256(Path(__file__)),
            "policy_sha256": sha256(args.policy),
        },
        "host": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "python": platform.python_version(),
        },
        "scalar": {**probe["scalar"], "normalized": scalar},
        "call": {**probe["call"], "normalized": call},
        "frame_memory": {**probe["frame_memory"], "normalized": frame},
        "footprint": footprint,
        "summary": summary,
    }


def self_test() -> None:
    stable = distribution([10.0] * 7, "nanoseconds")
    if stable["p50"] != 10.0 or stable["coefficient_of_variation"] != 0.0:
        raise RuntimeError("distribution self-test failed")
    if round(percentile([1.0, 2.0, 3.0], 0.95), 3) != 2.9:
        raise RuntimeError("percentile self-test failed")
    if round(geometric_mean([4.0, 9.0]), 6) != 6.0:
        raise RuntimeError("geometric-mean self-test failed")
    footprint, errors = validate_footprint({
        "release_build": True,
        "slot_state_metadata_enabled": 0,
        "footprint": {
            "fixed_frame_bytes": 64, "arena_allocation_bytes": 32,
            "alignment_padding_bytes": 7, "slot_state_metadata_bytes": 0,
            "lifecycle_state_metadata_bytes": 0,
            "total_bytes": 103, "slot_count": 4, "frame_alignment": 8,
            "plan_frame_bytes": 32, "max_total_bytes": 128,
        },
    })
    if errors or footprint["bounded_metadata_bytes"] != 71:
        raise RuntimeError("footprint self-test failed")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--benchmark", type=Path)
    parser.add_argument("--policy", type=Path, default=DEFAULT_POLICY)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        print(json.dumps({"runner": RUNNER, "result": "passed"}))
        return 0
    if not args.benchmark or not args.benchmark.is_file():
        parser.error("--benchmark must name the built probe")
    try:
        payload = run_gate(args)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError,
            json.JSONDecodeError) as error:
        payload = {
            "schema": "xray.typed-target-vm.performance.v2",
            "runner": RUNNER,
            "result": "failed",
            "errors": [str(error)],
        }
    rendered = json.dumps(payload, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered, encoding="utf-8")
    sys.stdout.write(rendered)
    return 0 if payload["result"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())

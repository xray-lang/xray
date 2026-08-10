#!/usr/bin/env python3
"""Build and measure the disposable Phase-0 typed-slot architecture spike."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import statistics
import subprocess
import sys
import time
import tomllib
from pathlib import Path
from typing import Any


if sys.version_info < (3, 11):
    raise SystemExit("typed-slot calibration requires Python 3.11 or newer")


SCHEMA = 1
RUNNER = "typed-slot-calibration/1"


def capture(command: list[str], cwd: Path, timeout: int) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
        check=False,
        env={**os.environ, "NO_COLOR": "1"},
    )


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def cache_value(cache: Path, key: str) -> str | None:
    if not cache.is_file():
        return None
    prefix = f"{key}:"
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(prefix) and "=" in line:
            return line.split("=", 1)[1]
    return None


def percentile(values: list[int], quantile: float) -> float:
    ordered = sorted(values)
    if len(ordered) == 1:
        return float(ordered[0])
    position = (len(ordered) - 1) * quantile
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return float(ordered[lower])
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def summary(values: list[int]) -> dict[str, float | int | list[int]]:
    mean = statistics.fmean(values)
    return {
        "samples_ns": values,
        "p50_ns": statistics.median(values),
        "p95_ns": percentile(values, 0.95),
        "mean_ns": mean,
        "cv": statistics.pstdev(values) / mean if mean else 0.0,
    }


def timed_run(command: list[str], cwd: Path, timeout: int, expected: str) -> tuple[int, str]:
    started = time.perf_counter_ns()
    result = capture(command, cwd, timeout)
    duration = time.perf_counter_ns() - started
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(command)}\n{result.stderr}"
        )
    if result.stdout != expected:
        raise RuntimeError(
            f"independent oracle mismatch for {' '.join(command)}: "
            f"expected {expected!r}, got {result.stdout!r}"
        )
    return duration, result.stderr


def compiler_identity(root: Path, xray: Path) -> dict[str, Any]:
    git_head = capture(["git", "rev-parse", "HEAD"], root, 30)
    git_tree = capture(["git", "rev-parse", "HEAD^{tree}"], root, 30)
    git_status = capture(["git", "status", "--porcelain=v1"], root, 30)
    version = capture([str(xray), "--version", "--json"], root, 30)
    if any(row.returncode != 0 for row in (git_head, git_tree, git_status, version)):
        raise RuntimeError("cannot establish source/compiler identity")
    version_data = json.loads(version.stdout)
    head = git_head.stdout.strip()
    if git_status.stdout.strip():
        raise RuntimeError("calibration requires a clean source tree")
    if version_data.get("git", {}).get("commit") != head:
        raise RuntimeError("calibration binary commit does not match Git HEAD")
    if version_data.get("git", {}).get("dirty"):
        raise RuntimeError("calibration binary reports dirty build inputs")
    return {
        "git_commit": head,
        "git_tree": git_tree.stdout.strip(),
        "git_dirty": False,
        "compiler": version_data,
        "compiler_sha256": digest(xray),
    }


def build_spike(root: Path, build: Path, source: Path) -> tuple[Path, list[str]]:
    cache = build / "CMakeCache.txt"
    cc = cache_value(cache, "CMAKE_C_COMPILER") or "cc"
    binary = build / "target-machine" / "phase0" / "typed-slot-calibration" / "calibrate"
    binary.parent.mkdir(parents=True, exist_ok=True)
    command = [cc, "-std=c11", "-O3", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)]
    result = capture(command, root, 120)
    if result.returncode != 0:
        raise RuntimeError(f"cannot build typed-slot spike\n{result.stdout}{result.stderr}")
    return binary, command


def run_spike(binary: Path, root: Path, warmups: int, measurements: int) -> dict[str, Any]:
    rows: list[dict[str, Any]] = []
    for index in range(warmups + measurements):
        result = capture([str(binary)], root, 30)
        if result.returncode != 0:
            raise RuntimeError(f"typed-slot spike failed\n{result.stdout}{result.stderr}")
        data = json.loads(result.stdout)
        if data.get("result") != "passed":
            raise RuntimeError("typed-slot spike reported failure")
        if index >= warmups:
            rows.append(data)
    first = rows[0]
    for row in rows[1:]:
        for key in ("frame_bytes", "execution", "mutations"):
            if row[key] != first[key]:
                raise RuntimeError(f"non-deterministic typed-slot result: {key}")
    metric_names = ("typed_plan_ns", "legacy_tagged_ns", "mailbox_ns")
    return {
        "frame_bytes": first["frame_bytes"],
        "execution": first["execution"],
        "mutations": first["mutations"],
        "checksums": {
            key: first["benchmarks"][key]
            for key in ("typed_checksum", "tagged_checksum", "mailbox_checksum")
        },
        "measurements": {
            name: summary([row["benchmarks"][name] for row in rows])
            for name in metric_names
        },
    }


def build_native(root: Path, xray: Path, source: Path, destination: Path) -> dict[str, Any]:
    destination.parent.mkdir(parents=True, exist_ok=True)
    command = [str(xray), "build", "--native", "-o", str(destination), str(source)]
    result = capture(command, root, 300)
    if result.returncode != 0:
        raise RuntimeError(f"native build failed for {source}\n{result.stdout}{result.stderr}")
    return {
        "command": command,
        "stdout": result.stdout,
        "stderr": result.stderr,
        "binary_sha256": digest(destination),
        "binary_bytes": destination.stat().st_size,
    }


def measure_xray_case(
    root: Path,
    xray: Path,
    build: Path,
    case: dict[str, Any],
    warmups: int,
    measurements: int,
) -> dict[str, Any]:
    source = root / case["source"]
    destination = (
        build / "target-machine" / "phase0" / "typed-slot-calibration" / source.stem
    )
    native_build = build_native(root, xray, source, destination)
    commands = {
        "legacy_vm": [str(xray), "run", str(source)],
        "aot_native": [str(destination)],
    }
    deadline = int(case["deadline_seconds"])
    measured: dict[str, Any] = {}
    for mode, command in commands.items():
        values = []
        stderr_values = []
        for index in range(warmups + measurements):
            duration, stderr = timed_run(command, root, deadline, case["expected_stdout"])
            if index >= warmups:
                values.append(duration)
                stderr_values.append(stderr)
        measured[mode] = {**summary(values), "stderr": stderr_values}
    return {
        "source": case["source"],
        "source_sha256": digest(source),
        "expected_stdout": case["expected_stdout"],
        "native_build": native_build,
        "measurements": measured,
    }


def self_test() -> int:
    assert summary([10, 20, 30, 40, 50])["p50_ns"] == 30
    assert percentile([10, 20, 30, 40, 50], 0.95) == 48
    print("typed-slot calibration runner self-test: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".")
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--xray", default="build/xray")
    parser.add_argument("--report")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()

    root = Path(args.root).resolve()
    build = (root / args.build_dir).resolve()
    xray = (root / args.xray).resolve()
    manifest_path = Path(__file__).with_name("manifest.toml")
    source_path = Path(__file__).with_name("calibrate.c")
    manifest = tomllib.loads(manifest_path.read_text(encoding="utf-8"))
    warmups = int(manifest["warmup_runs"])
    measurements = int(manifest["measurement_runs"])
    identity = compiler_identity(root, xray)
    spike_binary, build_command = build_spike(root, build, source_path)

    report = {
        "schema": SCHEMA,
        "runner": RUNNER,
        "result": "passed",
        "source": identity,
        "environment": {
            "python": sys.version,
            "python_executable": sys.executable,
            "c_compiler": build_command[0],
        },
        "inputs": {
            "manifest_sha256": digest(manifest_path),
            "spike_source_sha256": digest(source_path),
            "spike_binary_sha256": digest(spike_binary),
            "spike_build_command": build_command,
            "warmup_runs": warmups,
            "measurement_runs": measurements,
        },
        "spike": run_spike(spike_binary, root, warmups, measurements),
        "xray_workload": measure_xray_case(
            root, xray, build, manifest["xray_workload"], warmups, measurements
        ),
        "xray_mailbox": measure_xray_case(
            root, xray, build, manifest["xray_mailbox"], warmups, measurements
        ),
        "interpretation": {
            "architecture_decision": "typed slot arena is viable; proceed to governed TargetPlan",
            "universal_tagged_frame_exception": False,
            "dispatch_policy": "switch/computed-goto/superinstruction remains executor policy",
            "limits": [
                "the C spike is a disposable architecture calibration, not the product VM",
                "synthetic tagged-loop timings are not legacy VM throughput",
                "external VM/AOT wall timings include process startup and are cross-checks, not a backend speed ratio",
                "correctness comes from exact output and lifecycle oracles, not VM/AOT parity alone",
            ],
        },
    }
    rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
    output = build / "target-machine" / "phase0" / "typed-slot-calibration" / "report.json"
    output.write_text(rendered, encoding="utf-8")
    if args.report:
        destination = (root / args.report).resolve()
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(rendered, encoding="utf-8")
    print(f"typed-slot calibration: PASS ({output})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

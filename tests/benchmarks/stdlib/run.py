#!/usr/bin/env python3
"""Task-196 cross-backend stdlib benchmark runner with raw sample evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))

from stdlib_manifest import load_manifest, load_toml  # noqa: E402


MANIFEST_PATH = ROOT / "tests/benchmarks/stdlib/manifest.toml"
VALID_KINDS = {"tiny_helper", "cpu_kernel", "parser", "protocol_helper", "server", "async_io", "startup"}


def validate_manifest(data: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    if data.get("schema") != 1:
        errors.append("benchmark manifest schema must be 1")
    boundary = load_manifest(ROOT)
    governed = set(data.get("governed_suites", ()))
    expected = {str(module["perf_suite"]) for module in boundary.modules}
    if governed != expected:
        missing = sorted(expected - governed)
        stale = sorted(governed - expected)
        if missing:
            errors.append(f"governed_suites misses boundary suites: {', '.join(missing)}")
        if stale:
            errors.append(f"governed_suites has stale suites: {', '.join(stale)}")
    seen: set[str] = set()
    for entry in data.get("benchmark", ()):
        bench_id = str(entry.get("id", ""))
        if not bench_id or bench_id in seen:
            errors.append(f"benchmark id is missing or duplicated: {bench_id!r}")
        seen.add(bench_id)
        module = str(entry.get("module", ""))
        if module not in boundary.by_name:
            errors.append(f"benchmark {bench_id}: unknown boundary module {module!r}")
        elif entry.get("suite") != boundary.by_name[module].get("perf_suite"):
            errors.append(f"benchmark {bench_id}: suite does not match module boundary")
        if entry.get("kind") not in VALID_KINDS:
            errors.append(f"benchmark {bench_id}: invalid kind {entry.get('kind')!r}")
        source = ROOT / str(entry.get("source", ""))
        if not source.is_file():
            errors.append(f"benchmark {bench_id}: source does not exist: {entry.get('source')}")
        if entry.get("metrics") != ["wall_ns"]:
            errors.append(f"benchmark {bench_id}: unsupported metrics; runner currently records wall_ns")
        if entry.get("compare") != ["vm", "aot"]:
            errors.append(f"benchmark {bench_id}: compare must be ['vm', 'aot']")
        for field in ("warmup", "iterations", "quick_iterations"):
            if not isinstance(entry.get(field), int) or entry[field] < 1:
                errors.append(f"benchmark {bench_id}: {field} must be a positive integer")
        if not isinstance(entry.get("vm_budget_ratio"), (int, float)) or entry["vm_budget_ratio"] <= 0:
            errors.append(f"benchmark {bench_id}: vm_budget_ratio must be positive")
    return errors


def find_xray(value: str | None) -> Path:
    candidates = [Path(value)] if value else []
    candidates.extend((ROOT / "build/xray", ROOT / "build-release/xray"))
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate.resolve()
    raise RuntimeError("xray executable not found; pass --xray or build the xray target")


def run_sample(command: list[str]) -> tuple[int, bytes, bytes, int]:
    start = time.perf_counter_ns()
    proc = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    elapsed = time.perf_counter_ns() - start
    return proc.returncode, proc.stdout, proc.stderr, elapsed


def execute(entry: dict[str, Any], xray: Path, quick: bool, work: Path) -> dict[str, Any]:
    source = ROOT / str(entry["source"])
    binary = work / str(entry["id"]).replace("/", "_")
    build = subprocess.run(
        [str(xray), "build", "--native", "-O", "2", "-o", str(binary), str(source)],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if build.returncode:
        raise RuntimeError(build.stderr.decode("utf-8", "replace") or "AOT build failed")
    commands = {"vm": [str(xray), "run", str(source)], "aot": [str(binary)]}
    warmup = int(entry["warmup"])
    iterations = int(entry["quick_iterations"] if quick else entry["iterations"])
    samples: dict[str, list[int]] = {"vm": [], "aot": []}
    outputs: dict[str, bytes] = {}
    for backend, command in commands.items():
        for _ in range(warmup):
            rc, _stdout, stderr, _elapsed = run_sample(command)
            if rc:
                raise RuntimeError(f"{backend} warmup failed: {stderr.decode('utf-8', 'replace')}")
        for _ in range(iterations):
            rc, stdout, stderr, elapsed = run_sample(command)
            if rc:
                raise RuntimeError(f"{backend} sample failed: {stderr.decode('utf-8', 'replace')}")
            outputs.setdefault(backend, stdout)
            if outputs[backend] != stdout:
                raise RuntimeError(f"{backend} output is nondeterministic")
            samples[backend].append(elapsed)
    if outputs["vm"] != outputs["aot"]:
        raise RuntimeError("VM/AOT observable output mismatch")
    medians = {backend: int(statistics.median(values)) for backend, values in samples.items()}
    ratio = medians["vm"] / max(medians["aot"], 1)
    if ratio > float(entry["vm_budget_ratio"]):
        raise RuntimeError(
            f"VM/AOT ratio {ratio:.2f} exceeds budget {float(entry['vm_budget_ratio']):.2f}"
        )
    return {
        "id": entry["id"],
        "module": entry["module"],
        "kind": entry["kind"],
        "samples_ns": samples,
        "median_ns": medians,
        "vm_aot_ratio": ratio,
        "output_sha256": hashlib.sha256(outputs["vm"]).hexdigest(),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--validate-only", action="store_true")
    parser.add_argument("--only", action="append", default=[])
    parser.add_argument("--xray")
    parser.add_argument("--results", type=Path, default=ROOT / "tests/benchmarks/stdlib/results/latest.json")
    args = parser.parse_args()
    data = load_toml(MANIFEST_PATH)
    errors = validate_manifest(data)
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    selected = list(data.get("benchmark", ()))
    requested = {name for value in args.only for name in value.split(",") if name}
    if requested:
        selected = [entry for entry in selected if entry["id"] in requested or entry["module"] in requested]
        found = {entry["id"] for entry in selected} | {entry["module"] for entry in selected}
        unknown = sorted(requested - found)
        if unknown:
            print(f"unknown benchmark/module selection: {', '.join(unknown)}", file=sys.stderr)
            return 1
    if args.validate_only:
        print(f"OK: {len(selected)} active benchmarks and all boundary suites are governed")
        return 0
    try:
        xray = find_xray(args.xray)
    except RuntimeError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    results: list[dict[str, Any]] = []
    with tempfile.TemporaryDirectory(prefix="xray-stdlib-bench-") as temp:
        work = Path(temp)
        for entry in selected:
            print(f"== {entry['id']} ==", flush=True)
            try:
                result = execute(entry, xray, args.quick, work)
            except RuntimeError as exc:
                print(f"benchmark {entry['id']} failed: {exc}", file=sys.stderr)
                return 1
            results.append(result)
            print(
                f"vm={result['median_ns']['vm']}ns aot={result['median_ns']['aot']}ns "
                f"ratio={result['vm_aot_ratio']:.2f}"
            )
    payload = {
        "schema": 1,
        "quick": args.quick,
        "xray": str(xray),
        "results": results,
    }
    args.results.parent.mkdir(parents=True, exist_ok=True)
    args.results.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"wrote raw samples: {args.results}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

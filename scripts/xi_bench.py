#!/usr/bin/env python3
import argparse
import json
import math
import os
import platform
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BENCH_DIR = REPO_ROOT / "bench" / "xi_opt_bench"
DEFAULT_XRAY = REPO_ROOT / "build" / "xray"


def parse_args():
    parser = argparse.ArgumentParser(description="Run Xi optimization benchmarks and emit JSON snapshots.")
    parser.add_argument("baseline_tag", nargs="?", help="Optional git tag or snapshot JSON to compare as baseline")
    parser.add_argument("current_tag", nargs="?", help="Optional git tag or snapshot JSON to compare as current")
    parser.add_argument("--xray", default=str(DEFAULT_XRAY), help="Path to the xray executable")
    parser.add_argument("--bench-dir", default=str(DEFAULT_BENCH_DIR), help="Directory containing .xr benchmarks")
    parser.add_argument("--samples", type=int, default=5, help="Samples per benchmark")
    parser.add_argument("--warmups", type=int, default=1, help="Warmup runs per benchmark")
    parser.add_argument("--output", help="Write current snapshot JSON to this path")
    parser.add_argument("--compare", help="Compare current run against a baseline snapshot JSON")
    parser.add_argument("--filter", default="", help="Regex filter applied to benchmark file names")
    parser.add_argument("--timeout", type=float, default=60.0, help="Timeout per run in seconds")
    parser.add_argument("--build-mode", default=None)
    parser.add_argument("--fail-ratio", type=float, default=1.15, help="Fail if current mean exceeds baseline by this ratio")
    return parser.parse_args()


def repo_git_sha():
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"], cwd=REPO_ROOT, text=True
        ).strip()
    except subprocess.CalledProcessError:
        return "unknown"


def detect_build_mode():
    env_mode = os.environ.get("XRAY_BUILD_MODE")
    if env_mode:
        return env_mode
    cache = REPO_ROOT / "build" / "CMakeCache.txt"
    if cache.is_file():
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("CMAKE_BUILD_TYPE:"):
                value = line.split("=", 1)[-1].strip()
                return value if value else "default"
    return "unknown"


def arch_name():
    machine = platform.machine() or "unknown"
    system = platform.system() or "unknown"
    return f"{system.lower()}-{machine}"


def scenario_from_file(path):
    return path.stem


def discover_benchmarks(bench_dir, pattern):
    files = sorted(Path(bench_dir).glob("*.xr"))
    if pattern:
        rx = re.compile(pattern)
        files = [p for p in files if rx.search(p.name)]
    return files


def run_one(xray, bench_file, timeout):
    started = time.perf_counter_ns()
    proc = subprocess.run(
        [xray, str(bench_file)],
        cwd=REPO_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=timeout,
    )
    elapsed = time.perf_counter_ns() - started
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        raise RuntimeError(f"benchmark failed: {bench_file} exited {proc.returncode}")
    return elapsed


def summarize(samples):
    mean = statistics.fmean(samples)
    stddev = statistics.pstdev(samples) if len(samples) > 1 else 0.0
    return int(mean), int(stddev)


def build_snapshot(args):
    xray = str(Path(args.xray))
    if not Path(xray).is_file():
        raise FileNotFoundError(f"xray executable not found: {xray}")
    if args.samples < 1:
        raise ValueError("--samples must be >= 1")
    if args.warmups < 0:
        raise ValueError("--warmups must be >= 0")

    benches = discover_benchmarks(args.bench_dir, args.filter)
    if not benches:
        raise FileNotFoundError(f"no benchmarks found in {args.bench_dir}")

    entries = []
    for bench in benches:
        rel = bench.relative_to(REPO_ROOT).as_posix()
        print(f"==> {rel}")
        for _ in range(args.warmups):
            run_one(xray, bench, args.timeout)
        samples = []
        for idx in range(args.samples):
            elapsed = run_one(xray, bench, args.timeout)
            samples.append(elapsed)
            print(f"    sample {idx + 1}/{args.samples}: {elapsed} ns")
        mean_ns, stddev_ns = summarize(samples)
        entries.append(
            {
                "file": rel,
                "scenario": scenario_from_file(bench),
                "build_mode": args.build_mode or detect_build_mode(),
                "arch": arch_name(),
                "mean_ns": mean_ns,
                "stddev_ns": stddev_ns,
                "samples": len(samples),
                "git_sha": repo_git_sha(),
            }
        )

    return {
        "schema": "xray.xi_bench.v1",
        "generated_at_unix": int(time.time()),
        "entries": entries,
    }


def load_snapshot(path):
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    if "entries" not in data or not isinstance(data["entries"], list):
        raise ValueError(f"invalid snapshot: {path}")
    return data


def resolve_snapshot_arg(arg):
    candidate = Path(arg)
    if candidate.is_file():
        return candidate
    doc_candidate = REPO_ROOT / "docs" / "bench" / f"{arg}-baseline.json"
    if doc_candidate.is_file():
        return doc_candidate
    if arg.endswith("-complete"):
        short_name = arg[: -len("-complete")]
        doc_candidate = REPO_ROOT / "docs" / "bench" / f"{short_name}-baseline.json"
        if doc_candidate.is_file():
            return doc_candidate
    raise FileNotFoundError(f"snapshot not found: {arg}")


def write_snapshot(path, data):
    out = Path(path)
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2, sort_keys=True)
        f.write("\n")


def index_entries(snapshot):
    indexed = {}
    for entry in snapshot.get("entries", []):
        indexed[entry.get("file")] = entry
    return indexed


def compare_snapshots(baseline, current, fail_ratio):
    base = index_entries(baseline)
    curr = index_entries(current)
    failed = False
    print("\ncomparison:")
    for file_name in sorted(curr):
        current_entry = curr[file_name]
        base_entry = base.get(file_name)
        if not base_entry:
            print(f"  NEW  {file_name}: {current_entry['mean_ns']} ns")
            continue
        b = float(base_entry["mean_ns"])
        c = float(current_entry["mean_ns"])
        ratio = c / b if b > 0.0 else math.inf
        status = "PASS" if ratio <= fail_ratio else "FAIL"
        if status == "FAIL":
            failed = True
        print(f"  {status} {file_name}: {int(c)} ns vs {int(b)} ns ({ratio:.3f}x)")
    return 1 if failed else 0


def main():
    args = parse_args()

    if args.baseline_tag and args.current_tag:
        baseline_path = resolve_snapshot_arg(args.baseline_tag)
        current_path = resolve_snapshot_arg(args.current_tag)
        return compare_snapshots(load_snapshot(baseline_path), load_snapshot(current_path), args.fail_ratio)

    try:
        current = build_snapshot(args)
        if args.output:
            write_snapshot(args.output, current)
            print(f"\nwrote snapshot: {args.output}")
        if args.compare:
            baseline = load_snapshot(args.compare)
            return compare_snapshots(baseline, current, args.fail_ratio)
        return 0
    except (FileNotFoundError, RuntimeError, subprocess.TimeoutExpired, ValueError) as exc:
        print(f"xi_bench: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

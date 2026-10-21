"""Run scalar/container benchmarks with checked results and retained failures."""

from __future__ import annotations

import argparse
import csv
from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import statistics
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
CASES = {"fib": 32, "integer_loop": 100_000_000, "push_sum": 10_000_000}
QUICK = {"fib": 10, "integer_loop": 1000, "push_sum": 1000}
QUOTA = re.compile(rb"quota|budget|fuel|resource.exhausted|work.limit", re.IGNORECASE)


def expected(case: str, n: int) -> bytes:
    if case == "fib":
        previous, current = 0, 1
        for _ in range(n):
            previous, current = current, previous + current
        result = previous
    elif case == "integer_loop":
        groups, remainder = divmod(n, 97)
        result = groups * 4656 + remainder * (remainder - 1) // 2
    else:
        result = n * (n - 1) // 2
    # Workloads emit LF explicitly, including on Windows. No normalization
    # is applied to an observed child process.
    return (str(result) + "\n").encode("ascii")


def git(*args: str) -> str:
    return subprocess.check_output(["git", "-C", str(ROOT), *args], text=True).strip()


def binary_identity(command: list[str]) -> dict:
    resolved = shutil.which(command[0])
    if resolved is None:
        raise ValueError(f"executable not found: {command[0]}")
    path = Path(resolved).resolve()
    return {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def run_sample(command: list[str], wanted: bytes, timeout: float, prefix: Path) -> dict:
    start = time.perf_counter_ns()
    try:
        process = subprocess.run(command, cwd=ROOT, capture_output=True, timeout=timeout)
        elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000
        stdout, stderr, returncode = process.stdout, process.stderr, process.returncode
        if returncode:
            status = "quota-failed" if QUOTA.search(stderr) else "failed"
        else:
            status = "ok" if stdout == wanted else "wrong-output"
    except subprocess.TimeoutExpired as error:
        elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000
        stdout, stderr, returncode = error.stdout or b"", error.stderr or b"", None
        status = "timeout"
    except OSError as error:
        elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000
        stdout, stderr, returncode = b"", str(error).encode("utf-8"), None
        status = "launch-failed"
    prefix.with_suffix(".stdout").write_bytes(stdout)
    prefix.with_suffix(".stderr").write_bytes(stderr)
    return {"status": status, "elapsed_ms": elapsed_ms, "returncode": returncode,
            "command": command, "stdout": str(prefix.with_suffix(".stdout")),
            "stderr": str(prefix.with_suffix(".stderr"))}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", required=True, type=Path)
    parser.add_argument("--xray", type=Path)
    parser.add_argument("--cpython", action="store_true")
    parser.add_argument("--candidates", type=Path,
                        help="JSON list of {name, command: [tokens with {source}/{case}/{n}]} for LIR drivers")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--quick", action="store_true", help="small functional workloads, never performance evidence")
    parser.add_argument("--case", choices=tuple(CASES), action="append")
    parser.add_argument("--measurement", choices=("coarse", "isolated"), default="coarse",
                        help="isolated only when run in the team's exclusive measurement window")
    args = parser.parse_args()
    if args.samples < 1 or args.timeout <= 0:
        parser.error("samples and timeout must be positive")
    if args.measurement == "isolated" and (args.quick or args.samples != 5):
        parser.error("isolated measurements require full workloads and exactly five samples")
    return args


def main() -> int:
    args = parse_args()
    stamp = datetime.now(timezone(timedelta(hours=8))).strftime("%Y%m%d-%H%M%S-%f")
    output = args.output.resolve() / f"run-{stamp}-{os.getpid()}"
    output.mkdir(parents=True, exist_ok=False)
    candidates = [{"name": "c", "command": [str(args.reference.resolve()), "{case}", "{n}"]}]
    if args.xray:
        candidates.append({"name": "current-vm", "command": [str(args.xray.resolve()), "run", "{source}"]})
    if args.cpython:
        python_source = output / "reference.py"
        python_source.write_bytes((HERE / "reference.py").read_bytes())
        candidates.append({"name": "cpython", "command": [sys.executable, str(python_source), "{case}", "{n}"],
                           "script_sha256": hashlib.sha256(python_source.read_bytes()).hexdigest()})
    if args.candidates:
        candidates.extend(json.loads(args.candidates.read_text(encoding="utf-8")))
    names: set[str] = set()
    for candidate in candidates:
        name, command = candidate["name"], candidate["command"]
        if not re.fullmatch(r"[a-z][a-z0-9-]*", name) or name in names:
            raise ValueError(f"invalid or duplicate candidate name: {name}")
        if not isinstance(command, list) or not command or not all(isinstance(token, str) for token in command):
            raise ValueError(f"candidate command must be a nonempty list of strings: {name}")
        names.add(name)
        candidate["binary"] = binary_identity(command)
    metadata = {"started_at": stamp, "timezone": "Asia/Shanghai", "machine": platform.node(),
                "platform": platform.platform(), "processor": platform.processor(),
                "python": sys.version, "source_commit": git("rev-parse", "HEAD"),
                "source_status": git("status", "--porcelain"), "samples": args.samples,
                "measurement": args.measurement, "quick": args.quick, "candidates": candidates}
    metadata["c_reference_identity"] = subprocess.check_output(
        [str(args.reference.resolve()), "--identity"], text=True).strip()
    if args.xray:
        metadata["xray_version"] = json.loads(subprocess.check_output(
            [str(args.xray.resolve()), "--version", "--json"], text=True))
    (output / "metadata.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    rows, samples = [], []
    for case in args.case or CASES:
        n = (QUICK if args.quick else CASES)[case]
        source = output / f"{case}.xr"
        source.write_text(re.sub(r"(?m)^const n = \d+$", f"const n = {n}",
                                 (HERE / f"{case}.xr").read_text(encoding="utf-8")), encoding="utf-8")
        wanted = expected(case, n)
        by_candidate: dict[str, list[dict]] = {candidate["name"]: [] for candidate in candidates}
        # Alternate candidates within each repetition to reduce order drift.
        for repetition in range(args.samples):
            for candidate in candidates:
                name = candidate["name"]
                command = [token.format(source=source, case=case, n=n) for token in candidate["command"]]
                sample = run_sample(command, wanted, args.timeout, output / f"{case}-{name}-{repetition + 1}")
                sample.update(case=case, candidate=name, repetition=repetition + 1, n=n)
                by_candidate[name].append(sample)
                samples.append(sample)
                (output / "samples.json").write_text(json.dumps(samples, indent=2), encoding="utf-8")
                progress = {name: {"completed": len(group), "expected": args.samples,
                                   "statuses": [item["status"] for item in group]}
                            for name, group in by_candidate.items()}
                (output / "progress.json").write_text(json.dumps({"case": case, "candidates": progress}, indent=2), encoding="utf-8")
                print(f"{case}/{name} {repetition + 1}/{args.samples}: {sample['status']}", flush=True)
        for name, group in by_candidate.items():
            statuses = sorted({sample["status"] for sample in group})
            passed = statuses == ["ok"]
            rows.append({"case": case, "candidate": name, "n": n, "samples": len(group),
                         "status": "+".join(statuses),
                         "median_ms": f"{statistics.median(sample['elapsed_ms'] for sample in group):.6f}" if passed else "",
                         "measurement": args.measurement, "quick": args.quick,
                         "source_commit": metadata["source_commit"],
                         "machine": metadata["machine"], "platform": metadata["platform"]})
        with (output / "results.csv").open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
    for candidate in candidates:
        if candidate["binary"] != binary_identity(candidate["command"]):
            raise RuntimeError(f"binary changed during measurement: {candidate['name']}")
    with (output / "results.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    print(f"results: {output / 'results.csv'}", flush=True)
    return 0 if all(row["status"] == "ok" for row in rows) else 1


if __name__ == "__main__":
    raise SystemExit(main())

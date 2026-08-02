#!/usr/bin/env python3
"""Task-196 cross-backend stdlib benchmark runner with raw sample evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import socket
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))

from stdlib_manifest import load_manifest, load_toml  # noqa: E402


MANIFEST_PATH = ROOT / "tests/benchmarks/stdlib/manifest.toml"
VALID_KINDS = {
    "tiny_helper",
    "cpu_kernel",
    "parser",
    "protocol_helper",
    "server",
    "async_io",
    "startup",
}
VALID_FIXTURES = {"tcp_echo", "tcp_client"}


def validate_manifest(data: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    if data.get("schema") != 1:
        errors.append("benchmark manifest schema must be 1")
    boundary = load_manifest(ROOT)
    governed_units = boundary.by_name
    governed = set(data.get("governed_suites", ()))
    expected = {str(unit["perf_suite"]) for unit in governed_units.values()}
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
        if module not in governed_units:
            errors.append(f"benchmark {bench_id}: unknown boundary module {module!r}")
        elif entry.get("suite") != governed_units[module].get("perf_suite"):
            errors.append(f"benchmark {bench_id}: suite does not match module boundary")
        if entry.get("kind") not in VALID_KINDS:
            errors.append(f"benchmark {bench_id}: invalid kind {entry.get('kind')!r}")
        source = ROOT / str(entry.get("source", ""))
        if not source.is_file():
            errors.append(f"benchmark {bench_id}: source does not exist: {entry.get('source')}")
        vm_source_value = entry.get("vm_source")
        if vm_source_value and not (ROOT / str(vm_source_value)).is_file():
            errors.append(f"benchmark {bench_id}: vm_source does not exist: {vm_source_value}")
        if entry.get("metrics") != ["wall_ns"]:
            errors.append(f"benchmark {bench_id}: unsupported metrics; runner currently records wall_ns")
        if entry.get("compare") != ["vm", "aot"]:
            errors.append(f"benchmark {bench_id}: compare must be ['vm', 'aot']")
        fixture = entry.get("fixture")
        if fixture is not None and fixture not in VALID_FIXTURES:
            errors.append(f"benchmark {bench_id}: invalid fixture {fixture!r}")
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


def recv_exact(conn: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = conn.recv(size - len(data))
        if not chunk:
            raise RuntimeError("TCP fixture peer closed early")
        data.extend(chunk)
    return bytes(data)


def run_sample(command: list[str], fixture: str | None = None) -> tuple[int, bytes, bytes, int]:
    if fixture is None:
        start = time.perf_counter_ns()
        proc = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        elapsed = time.perf_counter_ns() - start
        return proc.returncode, proc.stdout, proc.stderr, elapsed

    exchanges = 64
    env = os.environ.copy()
    fixture_errors: list[str] = []
    if fixture == "tcp_echo":
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        env["XRAY_BENCH_PORT"] = str(listener.getsockname()[1])

        def echo_server() -> None:
            try:
                conn, _addr = listener.accept()
                with conn:
                    for _ in range(exchanges):
                        payload = recv_exact(conn, 4)
                        conn.sendall(payload)
            except Exception as exc:  # pragma: no cover - failure evidence only
                fixture_errors.append(str(exc))
            finally:
                listener.close()

        thread = threading.Thread(target=echo_server, daemon=True)
        thread.start()
        start = time.perf_counter_ns()
        proc = subprocess.run(
            command, cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        elapsed = time.perf_counter_ns() - start
        thread.join(timeout=5)
        if thread.is_alive():
            fixture_errors.append("TCP echo fixture did not finish")
        stderr = proc.stderr
        if fixture_errors:
            stderr += ("\n" + "; ".join(fixture_errors)).encode()
        output = b"64\n" if proc.returncode == 0 and not fixture_errors else proc.stdout
        return proc.returncode if not fixture_errors else 1, output, stderr, elapsed

    reservation = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    reservation.bind(("127.0.0.1", 0))
    port = reservation.getsockname()[1]
    reservation.close()
    env["XRAY_BENCH_PORT"] = str(port)
    start = time.perf_counter_ns()
    proc = subprocess.Popen(
        command, cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    try:
        conn: socket.socket | None = None
        deadline = time.monotonic() + 5.0
        while conn is None and time.monotonic() < deadline:
            candidate = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            candidate.settimeout(0.1)
            try:
                candidate.connect(("127.0.0.1", port))
                conn = candidate
            except OSError:
                candidate.close()
                if proc.poll() is not None:
                    break
                time.sleep(0.01)
        if conn is None:
            raise RuntimeError("Xray TCP server fixture did not become ready")
        with conn:
            for _ in range(exchanges):
                conn.sendall(b"ping")
                if recv_exact(conn, 4) != b"ping":
                    raise RuntimeError("Xray TCP server returned mismatched payload")
        stdout, stderr = proc.communicate(timeout=5)
        elapsed = time.perf_counter_ns() - start
        output = b"64\n" if proc.returncode == 0 else stdout
        return proc.returncode, output, stderr, elapsed
    except Exception as exc:
        proc.kill()
        stdout, stderr = proc.communicate()
        elapsed = time.perf_counter_ns() - start
        return 1, stdout, stderr + ("\n" + str(exc)).encode(), elapsed

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
    vm_source = entry.get("vm_source")
    commands = {
        "vm": [str(xray), "test", str(ROOT / str(vm_source))]
        if vm_source
        else [str(xray), "run", str(source)],
        "aot": [str(binary)],
    }
    warmup = int(entry["warmup"])
    iterations = int(entry["quick_iterations"] if quick else entry["iterations"])
    samples: dict[str, list[int]] = {"vm": [], "aot": []}
    outputs: dict[str, bytes] = {}
    for backend, command in commands.items():
        for _ in range(warmup):
            rc, _stdout, stderr, _elapsed = run_sample(command, entry.get("fixture"))
            if rc:
                raise RuntimeError(f"{backend} warmup failed: {stderr.decode('utf-8', 'replace')}")
        for _ in range(iterations):
            rc, stdout, stderr, elapsed = run_sample(command, entry.get("fixture"))
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
        "aot_binary_size_bytes": binary.stat().st_size,
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

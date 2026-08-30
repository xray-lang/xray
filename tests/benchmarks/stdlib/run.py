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
from stdlib_migration import contract_modules, validate_contract  # noqa: E402


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
    if data.get("schema") != 2:
        errors.append("benchmark manifest schema must be 2")
    boundary = load_manifest(ROOT)
    governed_units = boundary.by_name
    contract_backends: dict[str, list[str]] = {}
    for module in contract_modules(ROOT):
        contract_errors, contract = validate_contract(ROOT, module)
        errors.extend(contract_errors)
        contract_backends[module] = list(contract.get("backends", ["vm", "aot"]))
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
        source_value = str(entry.get("source", ""))
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
        source = ROOT / source_value
        if not source.is_file():
            errors.append(f"benchmark {bench_id}: source does not exist: {entry.get('source')}")
        vm_source_value = entry.get("vm_source")
        if vm_source_value and not (ROOT / str(vm_source_value)).is_file():
            errors.append(f"benchmark {bench_id}: vm_source does not exist: {vm_source_value}")
        if entry.get("metrics") != ["wall_ns"]:
            errors.append(f"benchmark {bench_id}: unsupported metrics; runner currently records wall_ns")
        compare = entry.get("compare")
        if compare not in (["vm"], ["vm", "aot"]):
            errors.append(
                f"benchmark {bench_id}: compare must be ['vm'] or ['vm', 'aot']"
            )
        elif compare != contract_backends.get(module):
            errors.append(
                f"benchmark {bench_id}: compare {compare!r} disagrees with "
                f"the {module} correctness contract {contract_backends.get(module)!r}"
            )
        output_oracle = entry.get("output_oracle")
        if compare == ["vm"]:
            expected_oracle = f"{source_value}.expected"
            if not isinstance(output_oracle, str) or not output_oracle:
                errors.append(
                    f"benchmark {bench_id}: VM-only benchmark requires output_oracle"
                )
            elif output_oracle != expected_oracle:
                errors.append(
                    f"benchmark {bench_id}: output_oracle must be {expected_oracle}"
                )
            elif not (ROOT / output_oracle).is_file():
                errors.append(
                    f"benchmark {bench_id}: output_oracle does not exist: {output_oracle}"
                )
        elif output_oracle is not None:
            errors.append(
                f"benchmark {bench_id}: output_oracle is only valid for VM-only benchmark"
            )
        fixture = entry.get("fixture")
        if fixture is not None and fixture not in VALID_FIXTURES:
            errors.append(f"benchmark {bench_id}: invalid fixture {fixture!r}")
        for field in ("warmup", "iterations", "quick_iterations"):
            if not isinstance(entry.get(field), int) or entry[field] < 1:
                errors.append(f"benchmark {bench_id}: {field} must be a positive integer")
        if compare == ["vm", "aot"]:
            if (
                not isinstance(entry.get("vm_budget_ratio"), (int, float))
                or entry["vm_budget_ratio"] <= 0
            ):
                errors.append(f"benchmark {bench_id}: vm_budget_ratio must be positive")
        elif "vm_budget_ratio" in entry:
            errors.append(
                f"benchmark {bench_id}: vm_budget_ratio is only valid for VM/AOT comparison"
            )
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
    backends = list(entry["compare"])
    if "aot" in backends:
        build = subprocess.run(
            [str(xray), "build", "--native", "-O", "2", "-o", str(binary), str(source)],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if build.returncode:
            raise RuntimeError(build.stderr.decode("utf-8", "replace") or "AOT build failed")
    vm_source = entry.get("vm_source")
    all_commands = {
        "vm": [str(xray), "test", str(ROOT / str(vm_source))]
        if vm_source
        else [str(xray), "run", str(source)],
        "aot": [str(binary)],
    }
    commands = {backend: all_commands[backend] for backend in backends}
    warmup = int(entry["warmup"])
    iterations = int(entry["quick_iterations"] if quick else entry["iterations"])
    samples: dict[str, list[int]] = {backend: [] for backend in backends}
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
    if len(outputs) > 1 and len(set(outputs.values())) != 1:
        raise RuntimeError("VM/AOT observable output mismatch")
    output_oracle: str | None = None
    if backends == ["vm"]:
        output_oracle = entry.get("output_oracle")
        if not isinstance(output_oracle, str) or not output_oracle:
            raise RuntimeError("VM-only benchmark requires output_oracle")
        expected_oracle = f"{entry['source']}.expected"
        if output_oracle != expected_oracle:
            raise RuntimeError(f"VM-only benchmark output_oracle must be {expected_oracle}")
        oracle_path = ROOT / output_oracle
        try:
            expected_output = oracle_path.read_bytes()
        except OSError as exc:
            raise RuntimeError(f"cannot read output oracle {output_oracle}: {exc}") from exc
        if outputs["vm"] != expected_output:
            raise RuntimeError(f"VM output does not match byte-exact oracle {output_oracle}")
    medians = {backend: int(statistics.median(values)) for backend, values in samples.items()}
    result = {
        "id": entry["id"],
        "module": entry["module"],
        "kind": entry["kind"],
        "backends": backends,
        "samples_ns": samples,
        "median_ns": medians,
        "output_sha256": hashlib.sha256(outputs["vm"]).hexdigest(),
    }
    if "aot" in backends:
        result["aot_binary_size_bytes"] = binary.stat().st_size
    if output_oracle is not None:
        result["output_oracle"] = output_oracle
    if backends == ["vm", "aot"]:
        ratio = medians["vm"] / max(medians["aot"], 1)
        if ratio > float(entry["vm_budget_ratio"]):
            raise RuntimeError(
                f"VM/AOT ratio {ratio:.2f} exceeds budget {float(entry['vm_budget_ratio']):.2f}"
            )
        result["vm_aot_ratio"] = ratio
    return result


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
            timing = " ".join(
                f"{backend}={result['median_ns'][backend]}ns" for backend in result["backends"]
            )
            if "vm_aot_ratio" in result:
                timing += f" ratio={result['vm_aot_ratio']:.2f}"
            print(timing)
    payload = {
        "schema": 2,
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

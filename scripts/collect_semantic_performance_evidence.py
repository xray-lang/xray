#!/usr/bin/env python3
"""Collect task-262 O0/O2 sort code-shape and machine-code evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tests" / "benchmarks" / "aot_c90" / "runtime" / "sort_i64_random.xr"


def timed(command: list[str], *, cwd: Path = ROOT, timeout: int = 240) -> int:
    started = time.perf_counter_ns()
    subprocess.run(command, cwd=cwd, check=True, timeout=timeout,
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return time.perf_counter_ns() - started


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xray", required=True)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--samples", type=int, default=7)
    args = parser.parse_args()
    xray = str(Path(args.xray).resolve())
    rows: list[dict[str, object]] = []

    with tempfile.TemporaryDirectory(prefix="xray-semantic-performance-") as raw:
        work = Path(raw)
        for opt in (0, 2):
            generated = work / f"sort-o{opt}.c"
            object_file = work / f"sort-o{opt}.o"
            binary = work / f"sort-o{opt}"
            codegen_ns = timed([
                xray, "build", "--native", "-c", "-O", str(opt), "-C", args.cc,
                "-o", str(generated), str(SOURCE),
            ])
            object_compile_ns = timed([
                args.cc, "-std=c11", f"-O{opt}", "-I", str(ROOT / "include"),
                "-I", str(ROOT / "src" / "aot"), "-c", str(generated),
                "-o", str(object_file),
            ])
            native_build_ns = timed([
                xray, "build", "--native", "-O", str(opt), "-C", args.cc,
                "-o", str(binary), str(SOURCE),
            ])
            runtime_ns = [timed([str(binary)], timeout=60) for _ in range(args.samples)]
            generated_text = generated.read_text(encoding="utf-8", errors="strict")
            rows.append({
                "opt": opt,
                "codegen_ns": codegen_ns,
                "object_compile_ns": object_compile_ns,
                "native_build_ns": native_build_ns,
                "generated_c_bytes": generated.stat().st_size,
                "generated_c_sha256": digest(generated),
                "object_bytes": object_file.stat().st_size,
                "object_sha256": digest(object_file),
                "binary_bytes": binary.stat().st_size,
                "binary_sha256": digest(binary),
                "runtime_samples_ns": runtime_ns,
                "runtime_median_ns": int(statistics.median(runtime_ns)),
                "runtime_p99_ns": max(runtime_ns),
                "sort_dispatch_mentions": generated_text.count("xrt_method_discard_0"),
                "retired_private_sort_mentions": sum(
                    generated_text.count(symbol)
                    for symbol in ("xrt_introsort_", "xrt_vintrosort", "TYPED_SORT")
                ),
            })

    version = subprocess.run([xray, "--version", "--json"], cwd=ROOT, check=True, text=True,
                             encoding="utf-8", errors="strict",
                             stdout=subprocess.PIPE).stdout.strip()
    cc_version = subprocess.run([args.cc, "--version"], cwd=ROOT, check=True, text=True,
                                encoding="utf-8", errors="strict",
                                stdout=subprocess.PIPE).stdout.splitlines()[0]
    payload = {
        "schema": 1,
        "task": 262,
        "source": str(SOURCE.relative_to(ROOT)),
        "source_sha256": digest(SOURCE),
        "host": platform.platform(),
        "machine": platform.machine(),
        "xray": json.loads(version),
        "cc": cc_version,
        "samples": args.samples,
        "canonical_consumer_mentions": sum(
            (ROOT / path).read_text(encoding="utf-8").count("xr_sort_core_")
            for path in ("src/runtime/object/xarray_vm.c", "src/aot/xrt_sort.inc.c")
        ),
        "profiles": rows,
    }
    args.output.resolve().write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(f"semantic performance evidence: PASS output={args.output.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

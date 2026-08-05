#!/usr/bin/env python3
"""Xray AOT benchmarks against C references, and distance from the C90 target.

Default mode reports where AOT currently stands without failing on performance
or generated-code shape. `--gate` turns ratio and code-shape expectations into
failures. Build, run, and output mismatches always fail: a benchmark whose
output disagrees with its C reference is not slow, it is wrong, and its timing
means nothing.

Each benchmark is measured by the median of N samples, alternating nothing --
the C and AOT binaries are timed back to back inside one sample so a machine
that drifts during the run drifts for both.

With `--rust`, benchmarks shipping a .rs reference are also compared against
safe std-only Rust at the same optimization tier. Manifests gate that column
via min_rust_ratio; without it the Rust column is report-only.

Usage: run_aot_c90_benchmarks.py [options]
"""

from __future__ import annotations

import argparse
import json
import os
import platform as host_platform
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Sequence


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parent.parent / "tests" / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, workspace  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent
BENCH_ROOT = REPO_ROOT / "tests" / "benchmarks" / "aot_c90"
AUDIT_SCRIPT = REPO_ROOT / "scripts" / "check_aot_codegen_invariants.py"

DEFAULT_MIN_RATIO = "0.90"
NAN = "nan"


@dataclass
class Result:
    name: str
    category: str
    status: str
    c_median: str
    aot_median: str
    ratio: str
    min_ratio: str
    audit_pass: int
    c_size: int
    aot_size: int
    rust_median: str
    rust_ratio: str
    checksum: str


def median_of(values: Sequence[float]) -> str:
    return NAN if not values else f"{statistics.median(values):.6f}"


def ratio_of(numerator: str, denominator: str) -> str:
    try:
        c, a = float(numerator), float(denominator)
    except ValueError:
        return NAN
    return NAN if a <= 0 else f"{c / a:.6f}"


def as_number(text: str) -> "float | int | None":
    """A JSON number, or null when the value is nan/empty, as the shell did."""
    try:
        value = float(text)
    except (TypeError, ValueError):
        return None
    if value != value:  # nan
        return None
    return int(value) if value.is_integer() and "." not in str(text) else value


def measure_ms(out_file: Path, argv: Sequence) -> float | None:
    """Wall time of one run in ms, or None when it exited non-zero.

    perf_counter_ns around a plain subprocess: the point is to compare two
    binaries under identical measurement, not to isolate CPU time.
    """
    start = time.perf_counter_ns()
    with open(out_file, "wb") as handle:
        code = subprocess.run([str(a) for a in argv], stdout=handle).returncode
    end = time.perf_counter_ns()
    return None if code != 0 else (end - start) / 1_000_000


def print_diff(expected: Path, actual: Path, limit: int = 20) -> None:
    """A unified diff of two output files, indented, first `limit` lines.

    An output mismatch is the one failure where the numbers are worthless and
    only the difference explains anything, so it is shown rather than just
    named.
    """
    import difflib

    lines = list(difflib.unified_diff(
        expected.read_text(encoding="utf-8").splitlines(True),
        actual.read_text(encoding="utf-8").splitlines(True),
        fromfile=str(expected), tofile=str(actual)))
    for line in lines[:limit]:
        print(f"  {line.rstrip()}")


def manifest_value(path: Path, key: str) -> str:
    """`key = value` from a .expect manifest, comments stripped."""
    if not path.is_file():
        return ""
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.lstrip().startswith("#") or "=" not in line:
            continue
        name, _, value = line.partition("=")
        if name.strip() != key:
            continue
        return value.split("#")[0].strip()
    return ""


def metric_from_audit(text: str, key: str) -> str:
    for line in text.splitlines():
        name, _, value = line.partition("=")
        if name == key:
            return value
    return ""


def cc_cpu_arg(cpu: str) -> str:
    if not cpu:
        return ""
    machine = host_platform.machine().lower()
    prefix = "-mcpu=" if machine.startswith(("arm", "aarch64")) else "-march="
    return f"{prefix}{cpu}"


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run Xray AOT benchmarks against C references.")
    parser.add_argument("--xray-bin", type=Path,
                        default=Path(os.environ.get("XRAY_BIN",
                                                    str(REPO_ROOT / "build" / "xray"))))
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--rust", action="store_true",
                        help="also benchmark Rust references (*.rs)")
    parser.add_argument("--rustc", default=os.environ.get("RUSTC", "rustc"))
    parser.add_argument("--opt", default="3", help="xray optimization level")
    parser.add_argument("--cpu", default="",
                        help="tune host builds (xray --cpu, C -march/-mcpu)")
    parser.add_argument("--samples", type=int, default=31)
    parser.add_argument("--quick", action="store_true",
                        help="one sample, useful for smoke tests")
    parser.add_argument("--bench", default="",
                        help="comma-separated benchmark basenames or relative names")
    parser.add_argument("--json", type=Path, default=None,
                        help="write machine-readable JSON")
    parser.add_argument("--gate", action="store_true",
                        help="fail when ratio/audit expectations are not met")
    parser.add_argument("--keep", action="store_true",
                        help="keep the temporary build directory")
    args = parser.parse_args(argv[1:])
    if args.quick:
        args.samples = 1
    return args


def matches_filter(spec: str, relative: str, base: str) -> bool:
    if not spec:
        return True
    return any(item in (relative, base) for item in spec.split(","))


def run_one(args: argparse.Namespace, xr_file: Path, work: Path,
            results: list[Result]) -> str:
    """Run one benchmark. Returns "pass", "report_only_fail", or "fail"."""
    relative = xr_file.relative_to(BENCH_ROOT)
    rel_no_ext = str(relative.with_suffix(""))
    base = xr_file.stem
    category = str(relative.parent)
    c_file = BENCH_ROOT / f"{rel_no_ext}.c"
    rs_file = BENCH_ROOT / f"{rel_no_ext}.rs"
    expect_file = BENCH_ROOT / "manifests" / f"{base}.expect"

    print(f"--- {rel_no_ext} ---")

    if not c_file.is_file():
        print(f"FAIL: missing C reference: {c_file}")
        return "fail"

    bench_work = work / rel_no_ext.replace("/", "_")
    bench_work.mkdir(parents=True, exist_ok=True)
    gen_c = bench_work / f"{base}.generated.c"
    aot_bin = bench_work / platform.exe_name(f"{base}.aot")
    c_bin = bench_work / platform.exe_name(f"{base}.c")

    cpu_args = ["--cpu", args.cpu] if args.cpu else []
    generate = proc.run([args.xray_bin, "build", "--native", "-c",
                         "-O", args.opt, "-C", args.cc, "-o", gen_c,
                         *cpu_args, xr_file])
    if not generate.ok:
        print("FAIL: AOT C generation failed")
        for line in generate.combined_text().splitlines()[:20]:
            print(f"  {line}")
        return "fail"

    build = proc.run([args.xray_bin, "build", "--native", "-O", args.opt,
                      "-C", args.cc, "-o", aot_bin, *cpu_args, xr_file])
    if not build.ok:
        print("FAIL: AOT binary build failed")
        for line in build.combined_text().splitlines()[:20]:
            print(f"  {line}")
        return "fail"

    cpu_flag = cc_cpu_arg(args.cpu)
    c_build = proc.run([args.cc, f"-O{args.opt}", *( [cpu_flag] if cpu_flag else []),
                        c_file, "-o", c_bin, "-lm"])
    if not c_build.ok:
        print("FAIL: C reference build failed")
        for line in c_build.combined_text().splitlines()[:20]:
            print(f"  {line}")
        return "fail"

    rust_bin: Path | None = None
    if args.rust and rs_file.is_file():
        rust_bin = bench_work / platform.exe_name(f"{base}.rust")
        rust_build = proc.run([args.rustc, "-C", f"opt-level={args.opt}",
                               "-C", "lto=fat", "-C", "panic=abort",
                               "--edition", "2021",
                               *(["-C", f"target-cpu={args.cpu}"] if args.cpu else []),
                               rs_file, "-o", rust_bin])
        if not rust_build.ok:
            print("FAIL: Rust reference build failed")
            for line in rust_build.combined_text().splitlines()[:20]:
                print(f"  {line}")
            return "fail"

    audit = proc.run([sys.executable, AUDIT_SCRIPT, *(["--strict"] if args.gate else []),
                      *(["--expect", str(expect_file)] if expect_file.is_file() else []),
                      gen_c])
    audit_text = audit.stdout.decode("utf-8", "replace")
    if not audit.ok:
        print("FAIL: AOT generated-code audit failed")
        for line in audit_text.splitlines():
            if line.startswith("expectation_failure="):
                print(f"  {line}")
        return "fail"

    c_times: list[float] = []
    aot_times: list[float] = []
    rust_times: list[float] = []
    c_out_ref = bench_work / "c.ref.out"

    for i in range(1, args.samples + 1):
        c_out = bench_work / f"c.{i}.out"
        aot_out = bench_work / f"aot.{i}.out"
        c_ms = measure_ms(c_out, [c_bin])
        if c_ms is None:
            print("FAIL: C reference run failed")
            return "fail"
        aot_ms = measure_ms(aot_out, [aot_bin])
        if aot_ms is None:
            print("FAIL: AOT run failed")
            return "fail"
        c_times.append(c_ms)
        aot_times.append(aot_ms)

        c_bytes = c_out.read_bytes()
        if rust_bin is not None:
            rust_out = bench_work / f"rust.{i}.out"
            rust_ms = measure_ms(rust_out, [rust_bin])
            if rust_ms is None:
                print("FAIL: Rust reference run failed")
                return "fail"
            rust_times.append(rust_ms)
            if rust_out.read_bytes() != c_bytes:
                print("FAIL: C/Rust output mismatch")
                print_diff(c_out, rust_out)
                return "fail"
        if i == 1:
            c_out_ref.write_bytes(c_bytes)
        if aot_out.read_bytes() != c_bytes:
            print("FAIL: C/AOT output mismatch")
            print_diff(c_out, aot_out)
            return "fail"

    c_median = median_of(c_times)
    aot_median = median_of(aot_times)
    ratio = ratio_of(c_median, aot_median)
    min_ratio = manifest_value(expect_file, "min_ratio") or DEFAULT_MIN_RATIO
    ratio_pass = _at_least(ratio, min_ratio)

    rust_median = rust_ratio = ""
    rust_ratio_pass = True
    min_rust_ratio = manifest_value(expect_file, "min_rust_ratio")
    if rust_bin is not None and rust_times:
        rust_median = median_of(rust_times)
        rust_ratio = ratio_of(rust_median, aot_median)
        if min_rust_ratio:
            rust_ratio_pass = _at_least(rust_ratio, min_rust_ratio)

    audit_pass = int(metric_from_audit(audit_text, "audit_pass") or 1)
    aot_size = aot_bin.stat().st_size
    c_size = c_bin.stat().st_size

    print(f"  C median:   {c_median} ms")
    print(f"  AOT median: {aot_median} ms")
    print(f"  ratio:      {ratio} (target >= {min_ratio})")
    if rust_median:
        print(f"  Rust median: {rust_median} ms")
        if min_rust_ratio:
            print(f"  rust ratio: {rust_ratio} (target >= {min_rust_ratio})")
        else:
            print(f"  rust ratio: {rust_ratio} (report only)")
    print(f"  audit:      {'pass' if audit_pass == 1 else 'fail'}")
    print(f"  size:       C={c_size} AOT={aot_size} bytes")

    if audit_pass != 1:
        for line in audit_text.splitlines():
            if line.startswith("expectation_failure="):
                print(f"  {line}")

    status = "pass"
    if not ratio_pass or audit_pass != 1 or not rust_ratio_pass:
        status = "report_only_fail"

    results.append(Result(
        name=rel_no_ext, category=category, status=status,
        c_median=c_median, aot_median=aot_median, ratio=ratio,
        min_ratio=min_ratio, audit_pass=audit_pass, c_size=c_size,
        aot_size=aot_size, rust_median=rust_median, rust_ratio=rust_ratio,
        checksum=c_out_ref.read_text(encoding="utf-8").replace("\n", "|")))
    return status


def _at_least(value: str, threshold: str) -> bool:
    try:
        return float(value) >= float(threshold)
    except ValueError:
        return False


def write_json(path: Path, args: argparse.Namespace,
               results: Sequence[Result]) -> None:
    payload: Dict = {
        "xray": str(args.xray_bin),
        "cc": args.cc,
        "cpu": args.cpu or None,
    }
    if args.rust:
        probe = proc.run([args.rustc, "--version"])
        payload["rustc"] = (probe.stdout.decode("utf-8", "replace").strip()
                            if probe.ok else "unknown")
    payload["samples"] = args.samples
    payload["gate"] = args.gate
    payload["benchmarks"] = [
        {
            "name": r.name,
            "category": r.category,
            "status": r.status,
            "c_median_ms": as_number(r.c_median),
            "aot_median_ms": as_number(r.aot_median),
            "ratio": as_number(r.ratio),
            "min_ratio": as_number(r.min_ratio),
            "rust_median_ms": as_number(r.rust_median),
            "rust_ratio": as_number(r.rust_ratio),
            "audit_pass": r.audit_pass == 1,
            "c_size_bytes": r.c_size,
            "aot_size_bytes": r.aot_size,
            "checksum": r.checksum,
        }
        for r in results
    ]
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8", newline="\n")


def main(argv: list[str]) -> int:
    args = parse_args(argv)

    if not BENCH_ROOT.is_dir():
        sys.stderr.write(f"Missing benchmark directory: {BENCH_ROOT}\n")
        return 2

    print("=== Xray AOT C90 Benchmarks ===")
    print(f"xray={args.xray_bin}")
    print(f"cc={args.cc}")
    print(f"samples={args.samples}")
    print(f"gate={'true' if args.gate else 'false'}")
    if args.cpu:
        print(f"cpu={args.cpu}")
    if args.rust:
        probe = proc.run([args.rustc, "--version"])
        version = (probe.stdout.decode("utf-8", "replace").strip()
                   if probe.ok else "unknown")
        print(f"rustc={args.rustc} ({version})")
    print("")

    cases = sorted(p for p in BENCH_ROOT.rglob("*.xr")
                   if len(p.relative_to(BENCH_ROOT).parts) >= 2)
    selected = [p for p in cases
                if matches_filter(args.bench,
                                  str(p.relative_to(BENCH_ROOT).with_suffix("")),
                                  p.stem)]
    if not selected:
        print("No benchmarks matched.")
        return 2

    passed = failed = report_only = 0
    results: list[Result] = []
    with workspace.Workspace("xray_aot_c90", keep=args.keep) as ws:
        if args.keep:
            print(f"Work dir: {ws.root}")
        for xr_file in selected:
            status = run_one(args, xr_file, ws.root, results)
            if status == "fail":
                # No trailing blank line: a hard failure ends the benchmark
                # early, and the report reads as one block per completed case.
                failed += 1
                continue
            if status == "report_only_fail":
                report_only += 1
                if args.gate:
                    failed += 1
                else:
                    passed += 1
            else:
                passed += 1
            print("")

        if args.json:
            write_json(args.json, args, results)
            print(f"JSON: {args.json}")

    print(f"=== Results: {passed} completed, {failed} failed, "
          f"{report_only} below target/audit ===")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

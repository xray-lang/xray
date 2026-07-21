#!/usr/bin/env python3
"""Generate and measure task-210 enum static-domain fixtures."""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import statistics
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[4]
DEFAULT_XRAY = ROOT / "build/xray"
MODES = (
    "no_enum", "unused", "length", "manual", "value", "descriptor", "name",
    "payload", "payload_name", "escape",
)
HOT_MODES = {"manual", "value", "descriptor"}
FORBIDDEN_C_SYMBOLS = (
    "xrt_array_new(",
    "xrt_getprop_name(",
    "memberCount",
    "getMember",
    "Reflect",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xray", type=Path, default=DEFAULT_XRAY)
    parser.add_argument("--counts", default="8,64,1024")
    parser.add_argument("--modes", default=",".join(MODES))
    parser.add_argument("--samples", type=int, default=3)
    parser.add_argument("--target-iterations", type=int, default=1_000_000)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--keep-work", action="store_true")
    return parser.parse_args()


def variant_name(index: int) -> str:
    return f"Variant{index:04d}ColdNameSentinel"


def enum_decl(count: int, *, payload: bool = False) -> str:
    members = []
    for index in range(count):
        name = variant_name(index)
        members.append(f"    {name}(field{index:04d}: int)" if payload else f"    {name}")
    return "enum BenchEnum {\n" + ",\n".join(members) + "\n}\n"


def program_for(mode: str, count: int, target_iterations: int) -> str:
    repetitions = max(1, target_iterations // count)
    if mode == "no_enum":
        return "print(1)\n"
    source = enum_decl(count, payload=mode in {"payload", "payload_name"})
    if mode == "unused":
        return source + "print(1)\n"
    if mode == "length":
        return source + "print(BenchEnum.variants.length)\n"
    if mode == "manual":
        return (
            source
            + f"var sum = 0\nfor (var round = 0; round < {repetitions}; round++) {{\n"
            + f"    for (var ordinal = 0; ordinal < {count}; ordinal++) {{ sum += ordinal }}\n"
            + "}\nprint(sum)\n"
        )
    if mode == "value":
        return (
            source
            + f"var sum = 0\nfor (round in 0..{repetitions}) {{\n"
            + "    for (value in BenchEnum) { sum += value.ordinal }\n}\nprint(sum)\n"
        )
    if mode == "descriptor":
        return (
            source
            + f"var sum = 0\nfor (round in 0..{repetitions}) {{\n"
            + "    for (variant in BenchEnum.variants) { sum += variant.ordinal }\n}\nprint(sum)\n"
        )
    if mode == "name":
        return (
            source
            + "var sum = 0\nfor (variant in BenchEnum.variants) {\n"
            + "    sum += len(variant.name)\n}\nprint(sum)\n"
        )
    if mode == "payload":
        return (
            source
            + "var sum = 0\nfor (variant in BenchEnum.variants) {\n"
            + "    sum += variant.payloadCount\n"
            + "    for (field in variant.payloads) { sum += field.type }\n}\nprint(sum)\n"
        )
    if mode == "payload_name":
        return (
            source
            + "var sum = 0\nfor (variant in BenchEnum.variants) {\n"
            + "    for (field in variant.payloads) { sum += len(field.name) }\n}\nprint(sum)\n"
        )
    if mode == "escape":
        return (
            source
            + "type ErasedDescriptor = EnumVariant<BenchEnum> | int\n"
            + "var values: Array<ErasedDescriptor> = []\n"
            + "for (variant in BenchEnum.variants) { values.push(variant) }\n"
            + "print(len(values))\n"
        )
    raise ValueError(f"unknown mode: {mode}")


def parse_peak_rss(stderr: str) -> int | None:
    patterns = (
        r"(?m)^\s*(\d+)\s+maximum resident set size\s*$",
        r"(?m)^\s*Maximum resident set size \(kbytes\):\s*(\d+)\s*$",
    )
    for index, pattern in enumerate(patterns):
        match = re.search(pattern, stderr)
        if match:
            value = int(match.group(1))
            return value if index == 0 else value * 1024
    return None


def run_timed(command: list[str], *, cwd: Path) -> tuple[float, int | None, str, str]:
    time_bin = Path("/usr/bin/time")
    wrapped = command
    if time_bin.is_file():
        wrapped = [str(time_bin), "-l" if platform.system() == "Darwin" else "-v", *command]
    started = time.perf_counter_ns()
    proc = subprocess.run(wrapped, cwd=cwd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    elapsed_ms = (time.perf_counter_ns() - started) / 1_000_000.0
    if proc.returncode != 0:
        raise RuntimeError(
            f"command failed ({proc.returncode}): {' '.join(command)}\n{proc.stdout}\n{proc.stderr}"
        )
    return elapsed_ms, parse_peak_rss(proc.stderr), proc.stdout, proc.stderr


def macho_sections(binary: Path) -> dict[str, int]:
    try:
        output = subprocess.check_output(["/usr/bin/size", "-m", str(binary)], text=True)
    except (FileNotFoundError, subprocess.CalledProcessError):
        return {}
    sections: dict[str, int] = {}
    for name in ("__text", "__const", "__cstring"):
        match = re.search(rf"(?m)^\s*Section {re.escape(name)}:\s*(\d+)\s*$", output)
        if match:
            sections[name.removeprefix("__") + "_bytes"] = int(match.group(1))
    return sections


def run_hot(binary: Path, samples: int) -> dict[str, Any]:
    elapsed: list[int] = []
    output = None
    for _ in range(samples):
        started = time.perf_counter_ns()
        proc = subprocess.run([str(binary)], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        duration = time.perf_counter_ns() - started
        if proc.returncode != 0:
            raise RuntimeError(f"benchmark binary failed: {binary}\n{proc.stderr}")
        if output is None:
            output = proc.stdout
        elif proc.stdout != output:
            raise RuntimeError(f"unstable benchmark output: {binary}")
        elapsed.append(duration)
    return {
        "samples": samples,
        "mean_ns": int(statistics.fmean(elapsed)),
        "median_ns": int(statistics.median(elapsed)),
        "stdout": output.strip() if output else "",
    }


def git_sha() -> str:
    return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()


def measure_case(args: argparse.Namespace, work: Path, count: int, mode: str) -> dict[str, Any]:
    case = work / f"{count}-{mode}"
    case.mkdir(parents=True)
    source = case / "case.xr"
    bytecode = case / "case.xrb"
    c_file = case / "case.c"
    binary = case / "case"
    source.write_text(program_for(mode, count, args.target_iterations), encoding="utf-8")
    cache = case / "cache"

    compile_command = [
        str(args.xray), "build", "--native", "-O", "3", "-S", "--rebuild",
        "--cache-dir", str(cache), str(source), "-o", str(binary),
    ]
    compile_ms, peak_rss, _, _ = run_timed(compile_command, cwd=ROOT)
    c_command = [
        str(args.xray), "build", "--native", "-O", "3", "-c", "--rebuild",
        "--cache-dir", str(cache), str(source), "-o", str(c_file),
    ]
    subprocess.run(c_command, cwd=ROOT, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    bytecode_command = [
        str(args.xray), "compile", "-f", "bytecode", "-s", "-S",
        str(source), "-o", str(bytecode),
    ]
    subprocess.run(bytecode_command, cwd=ROOT, check=True, stdout=subprocess.PIPE,
                   stderr=subprocess.PIPE)
    c_text = c_file.read_text(encoding="utf-8", errors="replace")

    forbidden = [symbol for symbol in FORBIDDEN_C_SYMBOLS if symbol in c_text]
    if mode in {"manual", "value", "descriptor", "length"} and forbidden:
        raise RuntimeError(f"forbidden C symbols in {count}/{mode}: {forbidden}")
    box_calls = c_text.count("xrt_enum_descriptor_box_new(")
    scalar_box_calls = c_text.count("xrt_enum_scalar_box(")
    if mode == "escape" and box_calls == 0:
        raise RuntimeError(f"erased descriptor fixture has no box conversion: {count}/{mode}")
    if mode in {"value", "descriptor"} and box_calls != 0:
        raise RuntimeError(f"scalar descriptor fixture unexpectedly boxes: {count}/{mode}")
    if mode in {"value", "descriptor"} and scalar_box_calls != 0:
        raise RuntimeError(f"hot enum fixture unexpectedly crosses a tagged boundary: {count}/{mode}")

    entry: dict[str, Any] = {
        "count": count,
        "mode": mode,
        "source_bytes": source.stat().st_size,
        "bytecode_bytes": bytecode.stat().st_size,
        "generated_c_bytes": c_file.stat().st_size,
        "binary_bytes": binary.stat().st_size,
        "compile_wall_ms": round(compile_ms, 3),
        "compile_peak_rss_bytes": peak_rss,
        "descriptor_box_call_sites": box_calls,
        "enum_scalar_box_call_sites": scalar_box_calls,
        "descriptor_box_allocations_per_run": count if mode == "escape" else 0,
        "forbidden_c_symbols": forbidden,
        **macho_sections(binary),
    }
    if mode in HOT_MODES:
        entry["execution"] = run_hot(binary, args.samples)
    return entry


def main() -> int:
    args = parse_args()
    args.xray = args.xray.resolve()
    if not args.xray.is_file():
        raise SystemExit(f"xray executable not found: {args.xray}")
    if args.samples < 1:
        raise SystemExit("--samples must be >= 1")
    counts = [int(part) for part in args.counts.split(",") if part.strip()]
    if any(count < 1 for count in counts):
        raise SystemExit("all --counts values must be positive")

    modes = [part.strip() for part in args.modes.split(",") if part.strip()]
    unknown_modes = [mode for mode in modes if mode not in MODES]
    if unknown_modes:
        raise SystemExit(f"unknown --modes: {','.join(unknown_modes)}")
    temp = None
    if args.keep_work:
        work = Path(tempfile.mkdtemp(prefix="xray-enum-static-iteration-bench."))
    else:
        temp = tempfile.TemporaryDirectory(prefix="xray-enum-static-iteration-bench.")
        work = Path(temp.name)
    entries = []
    try:
        for count in counts:
            for mode in modes:
                print(f"==> variants={count} mode={mode}", flush=True)
                entries.append(measure_case(args, work, count, mode))
        snapshot = {
            "schema": "xray.enum_static_iteration_benchmark.v1",
            "git_sha": git_sha(),
            "platform": platform.platform(),
            "machine": platform.machine(),
            "xray": str(args.xray),
            "target_iterations": args.target_iterations,
            "entries": entries,
        }
        rendered = json.dumps(snapshot, indent=2, ensure_ascii=False) + "\n"
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(rendered, encoding="utf-8")
            print(f"wrote {args.output}")
        else:
            print(rendered)
        if args.keep_work:
            print(f"kept workdir: {work}")
        return 0
    finally:
        if temp is not None:
            temp.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Task 260 paired W0/candidate benchmark gate for Windows native images."""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import random
import statistics
import struct
import subprocess
import time


CASES = {
    "exact_dot": "6000000",
    "exact_static_index": "6000000",
    "construct_destroy": "2000000",
    "json_dynamic_lookup": "1",
    "json_parse_typed": "3500000",
    "json_encode_stringify": "18500000",
}

CASE_ARGS = {
    "exact_dot": ["2000000"],
    "exact_static_index": ["2000000"],
    "construct_destroy": ["2000000"],
    "json_dynamic_lookup": ["20000000"],
    "json_parse_typed": ["500000"],
    "json_encode_stringify": ["500000"],
}


def percentile(values: list[int], fraction: float) -> float:
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, int(len(ordered) * fraction + 0.999999) - 1))
    return float(ordered[index])


class ProcessMemoryCounters(ctypes.Structure):
    _fields_ = [
        ("cb", wintypes.DWORD),
        ("PageFaultCount", wintypes.DWORD),
        ("PeakWorkingSetSize", ctypes.c_size_t),
        ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t),
        ("PeakPagefileUsage", ctypes.c_size_t),
    ]


def peak_working_set(process: subprocess.Popen[bytes]) -> int | None:
    if not hasattr(process, "_handle") or not hasattr(ctypes, "WinDLL"):
        return None
    counters = ProcessMemoryCounters()
    counters.cb = ctypes.sizeof(counters)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    get_memory = psapi.GetProcessMemoryInfo
    get_memory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD]
    get_memory.restype = wintypes.BOOL
    if not get_memory(wintypes.HANDLE(process._handle), ctypes.byref(counters), counters.cb):
        return None
    return int(counters.PeakWorkingSetSize)


def run_once(binary: Path, expected: str, args: list[str]) -> tuple[int, int | None]:
    start = time.perf_counter_ns()
    process = subprocess.Popen(
        [str(binary), *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    stdout, stderr = process.communicate()
    elapsed = time.perf_counter_ns() - start
    peak = peak_working_set(process)
    actual = stdout.decode("utf-8", "replace").strip()
    if process.returncode != 0 or actual != expected:
        raise RuntimeError(
            f"{binary} failed: rc={process.returncode} stdout={actual!r} stderr={stderr!r}"
        )
    return elapsed, peak


def bootstrap_ratio_interval(
    pairs: list[tuple[int, int]], statistic, seed: int
) -> tuple[float, float]:
    rng = random.Random(seed)
    estimates: list[float] = []
    for _ in range(10_000):
        draw = [pairs[rng.randrange(len(pairs))] for _ in pairs]
        baseline = [sample[0] for sample in draw]
        candidate = [sample[1] for sample in draw]
        estimates.append(statistic(candidate) / statistic(baseline))
    estimates.sort()
    return estimates[249], estimates[9749]


def pe_sections(binary: Path) -> dict[str, int]:
    data = binary.read_bytes()
    if len(data) < 0x40 or data[:2] != b"MZ":
        return {}
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_offset + 24 > len(data) or data[pe_offset : pe_offset + 4] != b"PE\0\0":
        return {}
    section_count = struct.unpack_from("<H", data, pe_offset + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe_offset + 20)[0]
    section_offset = pe_offset + 24 + optional_size
    result: dict[str, int] = {}
    for index in range(section_count):
        offset = section_offset + index * 40
        if offset + 40 > len(data):
            break
        name = data[offset : offset + 8].split(b"\0", 1)[0].decode("ascii", "replace")
        result[name] = struct.unpack_from("<I", data, offset + 16)[0]
    return result


def classify(interval: tuple[float, float], limit: float) -> str:
    if interval[1] <= limit:
        return "pass"
    if interval[0] > limit:
        return "fail"
    return "insufficient"


def pin_runner(cpu: int) -> None:
    if not hasattr(ctypes, "WinDLL"):
        return
    if cpu < 0 or cpu >= ctypes.sizeof(ctypes.c_size_t) * 8:
        raise ValueError(f"CPU index {cpu} is outside the current Windows processor group")
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    get_current_process = kernel32.GetCurrentProcess
    get_current_process.restype = wintypes.HANDLE
    set_affinity = kernel32.SetProcessAffinityMask
    set_affinity.argtypes = [wintypes.HANDLE, ctypes.c_size_t]
    set_affinity.restype = wintypes.BOOL
    set_priority = kernel32.SetPriorityClass
    set_priority.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    set_priority.restype = wintypes.BOOL
    current = get_current_process()
    if not set_affinity(current, ctypes.c_size_t(1 << cpu)):
        raise OSError(ctypes.get_last_error(), "SetProcessAffinityMask failed")
    # Children inherit both affinity and priority. Isolating the runner on one
    # logical CPU removes scheduler migration from these short process samples.
    high_priority_class = 0x00000080
    if not set_priority(current, high_priority_class):
        raise OSError(ctypes.get_last_error(), "SetPriorityClass failed")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline-dir", type=Path, required=True)
    parser.add_argument("--candidate-dir", type=Path, required=True)
    parser.add_argument("--warmups", type=int, default=3)
    parser.add_argument("--samples", type=int, default=100)
    parser.add_argument("--batch", type=int, default=1)
    parser.add_argument("--cpu", type=int, default=0)
    parser.add_argument(
        "--cases", default=",".join(CASES), help="comma-separated case names"
    )
    parser.add_argument("--max-regression-percent", type=float, default=1.0)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.warmups < 3 or args.samples < 10 or args.batch < 1:
        parser.error("Task 260 requires at least 3 warmups and 10 paired samples")
    selected = [name.strip() for name in args.cases.split(",") if name.strip()]
    unknown = sorted(set(selected) - set(CASES))
    if not selected or unknown:
        parser.error(f"unknown or empty case selection: {unknown}")
    pin_runner(args.cpu)

    limit = 1.0 + args.max_regression_percent / 100.0
    payload: dict[str, object] = {
        "schema": 1,
        "warmups": args.warmups,
        "pairedSamples": args.samples,
        "executionsPerSample": args.batch,
        "cpu": args.cpu,
        "selectedCases": selected,
        "maxRegressionPercent": args.max_regression_percent,
        "cases": {},
    }
    overall = "pass"
    for case_index, name in enumerate(selected):
        expected = CASES[name]
        case_args = CASE_ARGS.get(name, [])
        binaries = {
            "baseline": args.baseline_dir / f"{name}.exe",
            "candidate": args.candidate_dir / f"{name}.exe",
        }
        for binary in binaries.values():
            if not binary.is_file():
                raise FileNotFoundError(binary)
        for lane in ("baseline", "candidate"):
            for _ in range(args.warmups):
                run_once(binaries[lane], expected, case_args)

        elapsed: dict[str, list[int]] = {"baseline": [], "candidate": []}
        peaks: dict[str, list[int]] = {"baseline": [], "candidate": []}
        for pair_index in range(args.samples):
            order = ("baseline", "candidate") if pair_index % 2 == 0 else (
                "candidate",
                "baseline",
            )
            for lane in order:
                duration_total = 0
                batch_peaks: list[int] = []
                for _ in range(args.batch):
                    duration, peak = run_once(binaries[lane], expected, case_args)
                    duration_total += duration
                    if peak is not None:
                        batch_peaks.append(peak)
                elapsed[lane].append(duration_total)
                if batch_peaks:
                    peaks[lane].append(max(batch_peaks))

        pairs = list(zip(elapsed["baseline"], elapsed["candidate"]))
        median_ci = bootstrap_ratio_interval(pairs, statistics.median, 26001 + case_index)
        p95_ci = bootstrap_ratio_interval(
            pairs, lambda values: percentile(values, 0.95), 26101 + case_index
        )
        median_status = classify(median_ci, limit)
        p95_status = classify(p95_ci, limit)
        status = "pass"
        if "fail" in (median_status, p95_status):
            status = "fail"
        elif "insufficient" in (median_status, p95_status):
            status = "insufficient"
        if status == "fail" or (status == "insufficient" and overall == "pass"):
            overall = status

        case_result = {
            "expectedOutput": expected,
            "args": case_args,
            "baselineNs": elapsed["baseline"],
            "candidateNs": elapsed["candidate"],
            "baselineMedianNs": statistics.median(elapsed["baseline"]),
            "candidateMedianNs": statistics.median(elapsed["candidate"]),
            "baselineP95Ns": percentile(elapsed["baseline"], 0.95),
            "candidateP95Ns": percentile(elapsed["candidate"], 0.95),
            "medianRatio95Ci": median_ci,
            "p95Ratio95Ci": p95_ci,
            "medianStatus": median_status,
            "p95Status": p95_status,
            "status": status,
            "baselinePeakWorkingSetBytes": peaks["baseline"],
            "candidatePeakWorkingSetBytes": peaks["candidate"],
            "baselineImageBytes": binaries["baseline"].stat().st_size,
            "candidateImageBytes": binaries["candidate"].stat().st_size,
            "baselinePeSections": pe_sections(binaries["baseline"]),
            "candidatePeSections": pe_sections(binaries["candidate"]),
        }
        payload["cases"][name] = case_result
        print(
            f"{name}: {status} median={case_result['baselineMedianNs']:.0f}/"
            f"{case_result['candidateMedianNs']:.0f}ns ci={median_ci} "
            f"p95={case_result['baselineP95Ns']:.0f}/"
            f"{case_result['candidateP95Ns']:.0f}ns ci={p95_ci}"
        )

    payload["status"] = overall
    encoded = json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded, encoding="utf-8")
    else:
        print(encoded, end="")
    print(f"Task 260 paired performance gate: {overall.upper()}")
    return 0 if overall == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Task 254: reject linear RSS growth from first-class mutable capture cells."""

from __future__ import annotations

import argparse
import ctypes
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def windows_peak_rss(proc: subprocess.Popen[str]) -> int:
    from ctypes import wintypes

    class Counters(ctypes.Structure):
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
            ("PrivateUsage", ctypes.c_size_t),
        ]

    counters = Counters()
    counters.cb = ctypes.sizeof(counters)
    ok = ctypes.windll.psapi.GetProcessMemoryInfo(
        wintypes.HANDLE(int(proc._handle)),  # type: ignore[attr-defined]
        ctypes.byref(counters),
        counters.cb,
    )
    return int(counters.PeakWorkingSetSize) if ok else 0


def unix_rss(pid: int) -> int:
    status = Path(f"/proc/{pid}/status")
    if status.is_file():
        try:
            for line in status.read_text(encoding="ascii").splitlines():
                if line.startswith(("VmHWM:", "VmRSS:")):
                    return int(line.split()[1]) * 1024
        except (OSError, ValueError, IndexError):
            return 0
    return 0


def measured(command: list[str], cwd: Path) -> tuple[str, int]:
    proc = subprocess.Popen(
        command,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    peak = 0
    while proc.poll() is None:
        sample = windows_peak_rss(proc) if os.name == "nt" else unix_rss(proc.pid)
        peak = max(peak, sample)
        time.sleep(0.01)
    output = proc.stdout.read() if proc.stdout else ""
    if proc.returncode != 0:
        raise RuntimeError(f"workload failed ({' '.join(command)}):\n{output.rstrip()}")
    if "cell_churn_ok" not in output:
        raise RuntimeError(f"workload did not publish its completion marker:\n{output.rstrip()}")
    if peak <= 0:
        raise RuntimeError("peak RSS could not be measured")
    return output, peak


def assert_bounded(name: str, prefix: list[str], cwd: Path, small: int, large: int,
                   max_growth: int) -> None:
    _, small_peak = measured([*prefix, str(small)], cwd)
    _, large_peak = measured([*prefix, str(large)], cwd)
    growth = large_peak - small_peak
    print(
        f"{name}: small={small_peak / 1048576:.1f} MiB "
        f"large={large_peak / 1048576:.1f} MiB growth={growth / 1048576:.1f} MiB"
    )
    if growth > max_growth:
        raise RuntimeError(
            f"{name} mutable-capture residue grows with iteration count: "
            f"{growth / 1048576:.1f} MiB > {max_growth / 1048576:.1f} MiB"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--xray", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--small", type=int, default=20000)
    parser.add_argument("--large", type=int, default=2000000)
    parser.add_argument("--max-growth-mib", type=int, default=64)
    parser.add_argument("--require-aot", action="store_true")
    args = parser.parse_args()

    xray = args.xray.resolve()
    source = args.source.resolve()
    cwd = source.parents[3]
    max_growth = args.max_growth_mib * 1024 * 1024
    assert_bounded("VM", [str(xray), "run", str(source), "--"], cwd, args.small, args.large,
                   max_growth)

    with tempfile.TemporaryDirectory(prefix="xray-task254-") as temp:
        native = Path(temp) / ("cell-rss.exe" if os.name == "nt" else "cell-rss")
        built = subprocess.run(
            [str(xray), "build", "-o", str(native), str(source)],
            cwd=cwd,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if built.returncode != 0:
            if args.require_aot:
                raise RuntimeError(f"hosted AOT provider is required:\n{built.stdout.rstrip()}")
            print("AOT: SKIP (hosted provider unavailable on this machine)")
            return 0
        assert_bounded("AOT", [str(native)], cwd, args.small, args.large, max_growth)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)

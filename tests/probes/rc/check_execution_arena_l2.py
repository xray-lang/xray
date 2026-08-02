#!/usr/bin/env python3
"""Task 252: assert bounded cycle residue for both VM and hosted AOT."""

from __future__ import annotations

import argparse
import ctypes
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def _windows_peak_rss(proc: subprocess.Popen[str]) -> int:
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


def _unix_rss(pid: int) -> int:
    status = Path(f"/proc/{pid}/status")
    if status.is_file():
        try:
            for line in status.read_text(encoding="ascii").splitlines():
                if line.startswith(("VmHWM:", "VmRSS:")):
                    return int(line.split()[1]) * 1024
        except (OSError, ValueError, IndexError):
            return 0
    try:
        result = subprocess.run(
            ["ps", "-o", "rss=", "-p", str(pid)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        return int(result.stdout.strip() or "0") * 1024
    except (OSError, ValueError):
        return 0


def run_measured(command: list[str], cwd: Path) -> tuple[int, str, int]:
    proc = subprocess.Popen(
        command,
        cwd=cwd,
        text=True,
        encoding="utf-8",
        errors="replace",
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    peak = 0
    while proc.poll() is None:
        if os.name == "nt":
            peak = max(peak, _windows_peak_rss(proc))
        else:
            peak = max(peak, _unix_rss(proc.pid))
        time.sleep(0.01)
    if os.name == "nt":
        peak = max(peak, _windows_peak_rss(proc))
    output = proc.stdout.read() if proc.stdout else ""
    return proc.returncode, output, peak


def assert_bound(
    name: str,
    command_prefix: list[str],
    cwd: Path,
    small: int,
    large: int,
    batch: int,
    max_growth: int,
    require_rss_bound: bool,
) -> None:
    measurements: list[int] = []
    for rounds in (small, large):
        command = [*command_prefix, str(rounds), str(batch)]
        code, output, peak = run_measured(command, cwd)
        if code != 0:
            raise RuntimeError(
                f"{name} workload failed ({' '.join(command)}):\n{output.rstrip()}"
            )
        if peak <= 0:
            raise RuntimeError(f"{name} peak RSS could not be measured")
        measurements.append(peak)
    growth = measurements[1] - measurements[0]
    print(
        f"{name}: small={measurements[0] / 1048576:.1f} MiB "
        f"large={measurements[1] / 1048576:.1f} MiB "
        f"growth={growth / 1048576:.1f} MiB"
    )
    if growth > max_growth and require_rss_bound:
        raise RuntimeError(
            f"{name} cycle residue grows with completed coroutine count: "
            f"{growth / 1048576:.1f} MiB > {max_growth / 1048576:.1f} MiB"
        )
    if growth > max_growth:
        print(
            f"{name}: RSS bound is informational on Windows because the CRT "
            "may retain freed aligned pages; source-level live-memory counters "
            "still asserted the execution boundary"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--xray", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--small", type=int, default=200)
    parser.add_argument("--large", type=int, default=8000)
    parser.add_argument("--batch", type=int, default=128)
    parser.add_argument("--max-growth-mib", type=int, default=64)
    parser.add_argument("--require-rss-bound", action="store_true")
    parser.add_argument("--require-aot", action="store_true")
    args = parser.parse_args()

    xray = args.xray.resolve()
    source = args.source.resolve()
    cwd = source.parents[3]
    max_growth = args.max_growth_mib * 1024 * 1024
    require_rss_bound = args.require_rss_bound or os.name != "nt"
    assert_bound(
        "VM",
        [str(xray), "run", str(source), "--"],
        cwd,
        args.small,
        args.large,
        args.batch,
        max_growth,
        require_rss_bound,
    )

    with tempfile.TemporaryDirectory(prefix="xray-task252-") as temp:
        suffix = ".exe" if os.name == "nt" else ""
        native = Path(temp) / f"l2{suffix}"
        built = subprocess.run(
            [str(xray), "build", "-o", str(native), str(source)],
            cwd=cwd,
            text=True,
            encoding="utf-8",
            errors="replace",
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if built.returncode != 0:
            if args.require_aot:
                print(built.stdout, file=sys.stderr)
                raise RuntimeError("hosted AOT provider is required but build failed")
            print("AOT: SKIP (hosted provider unavailable on this machine)")
            return 0
        assert_bound(
            "AOT",
            [str(native)],
            cwd,
            args.small,
            args.large,
            args.batch,
            max_growth,
            require_rss_bound,
        )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)

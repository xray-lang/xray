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


def windows_peak_rss(proc: subprocess.Popen[bytes]) -> int:
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


def linux_rss(pid: int) -> int:
    status = Path(f"/proc/{pid}/status")
    if not status.is_file():
        return 0
    try:
        for line in status.read_text(encoding="ascii").splitlines():
            if line.startswith(("VmHWM:", "VmRSS:")):
                return int(line.split()[1]) * 1024
    except (OSError, ValueError, IndexError):
        return 0
    return 0


class _ProcTaskInfo(ctypes.Structure):
    """Prefix of struct proc_taskinfo; only the first two fields are read."""

    _fields_ = [
        ("pti_virtual_size", ctypes.c_uint64),
        ("pti_resident_size", ctypes.c_uint64),
        ("pti_total_user", ctypes.c_uint64),
        ("pti_total_system", ctypes.c_uint64),
        ("pti_threads_user", ctypes.c_uint64),
        ("pti_threads_system", ctypes.c_uint64),
        ("pti_policy", ctypes.c_int32),
        ("pti_faults", ctypes.c_int32),
        ("pti_pageins", ctypes.c_int32),
        ("pti_cow_faults", ctypes.c_int32),
        ("pti_messages_sent", ctypes.c_int32),
        ("pti_messages_received", ctypes.c_int32),
        ("pti_syscalls_mach", ctypes.c_int32),
        ("pti_syscalls_unix", ctypes.c_int32),
        ("pti_csw", ctypes.c_int32),
        ("pti_threadnum", ctypes.c_int32),
        ("pti_numrunning", ctypes.c_int32),
        ("pti_priority", ctypes.c_int32),
    ]


PROC_PIDTASKINFO = 4


def macos_rss(pid: int) -> int:
    """Current resident size via libproc.

    There is no /proc on macOS, so the Linux reader returned 0 for every
    sample and the caller could only conclude that memory was unmeasurable.
    libproc reports the CURRENT resident size rather than a high-water mark,
    which is why the caller samples in a loop and keeps the maximum.
    """
    libc = ctypes.CDLL("libc.dylib", use_errno=True)
    info = _ProcTaskInfo()
    written = libc.proc_pidinfo(
        ctypes.c_int(pid),
        ctypes.c_int(PROC_PIDTASKINFO),
        ctypes.c_uint64(0),
        ctypes.byref(info),
        ctypes.c_int(ctypes.sizeof(info)),
    )
    return int(info.pti_resident_size) if written == ctypes.sizeof(info) else 0


def unix_rss(pid: int) -> int:
    return macos_rss(pid) if sys.platform == "darwin" else linux_rss(pid)


def measured(command: list[str], cwd: Path) -> tuple[bytes, int]:
    proc = subprocess.Popen(
        command,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    peak = 0
    while proc.poll() is None:
        sample = windows_peak_rss(proc) if os.name == "nt" else unix_rss(proc.pid)
        peak = max(peak, sample)
        time.sleep(0.01)
    output = proc.stdout.read() if proc.stdout else b""
    if proc.returncode != 0:
        raise RuntimeError(f"workload failed ({' '.join(command)}): bytes={output.rstrip()!r}")
    if b"cell_churn_ok" not in output:
        raise RuntimeError(f"workload did not publish its completion marker: bytes={output!r}")
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
            [str(xray), "build", "--native", "-o", str(native), str(source)],
            cwd=cwd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if built.returncode != 0:
            if args.require_aot:
                raise RuntimeError(f"hosted AOT provider is required: bytes={built.stdout!r}")
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

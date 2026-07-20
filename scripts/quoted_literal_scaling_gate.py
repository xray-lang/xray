#!/usr/bin/env python3
"""Verify that large fixed-byte literals stay structurally compact."""

from __future__ import annotations

import argparse
import re
import resource
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Optional


SIZES = (1024, 64 * 1024, 1024 * 1024)


def child_peak_rss_kib() -> int:
    value = int(resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)
    return value // 1024 if sys.platform == "darwin" else value


def run_timed(
    command: list[str], cwd: Path, timeout: Optional[float] = None
) -> tuple[subprocess.CompletedProcess[str], float, int]:
    started = time.perf_counter()
    try:
        result = subprocess.run(
            command, cwd=cwd, text=True, capture_output=True, check=False, timeout=timeout
        )
    except subprocess.TimeoutExpired as error:
        stdout = error.stdout.decode() if isinstance(error.stdout, bytes) else (error.stdout or "")
        stderr = error.stderr.decode() if isinstance(error.stderr, bytes) else (error.stderr or "")
        result = subprocess.CompletedProcess(command, 124, stdout, stderr)
    elapsed_ms = (time.perf_counter() - started) * 1000.0
    return result, elapsed_ms, child_peak_rss_kib()


def source_for(size: int) -> str:
    return "var payload = b\"\"\"\n" + ("A" * size) + "\n\"\"\"\nprint(len(payload))\n"


def legacy_source_for(size: int) -> str:
    return "var payload = b\"" + ("A" * size) + "\"\nprint(len(payload))\n"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def verify_case(root: Path, xray: Path, work: Path, size: int) -> tuple[float, int, int]:
    source = work / f"quoted_{size}.xr"
    generated = work / f"quoted_{size}.c"
    binary = work / f"quoted_{size}"
    source.write_text(source_for(size), encoding="utf-8")
    result, elapsed_ms, peak_rss_kib = run_timed(
        [
            str(xray),
            "build",
            "--native",
            "--c-only",
            "--dump-xaot-plan",
            "-o",
            str(generated),
            str(source),
        ],
        root,
    )
    output = result.stdout + result.stderr
    require(result.returncode == 0, f"{size}: compiler failed\n{output}")
    require(output.count("fixed-bytes-plan ") == 1, f"{size}: expected one fixed-byte plan")
    require(output.count("fixed-bytes-blob ") == 1, f"{size}: expected one canonical blob")
    require("fixed-bytes-stats values=1 blobs=1 dedup-hits=0" in output, f"{size}: bad plan stats")

    c_source = generated.read_text(encoding="utf-8")
    require(c_source.count("static const char _xbytes_") == 1, f"{size}: expected one static blob")
    require(c_source.count("memcpy(_fa") == 1, f"{size}: expected one bulk copy")
    store_pattern = r"_fa\d+\[(?:\d+|INT64_C\(\d+\))\]\s*="
    require(re.search(store_pattern, c_source) is None, f"{size}: per-byte stores leaked into C")

    expected = f"{size}\n"
    vm_result, _, _ = run_timed([str(xray), "run", str(source)], root, timeout=60.0)
    require(
        vm_result.returncode == 0 and vm_result.stdout == expected,
        f"{size}: VM value mismatch\n{vm_result.stdout}{vm_result.stderr}",
    )
    native_result, _, _ = run_timed(
        [str(xray), "build", "--native", "-O", "0", "-o", str(binary), str(source)],
        root,
        timeout=60.0,
    )
    require(
        native_result.returncode == 0,
        f"{size}: native build failed\n{native_result.stdout}{native_result.stderr}",
    )
    run_result, _, _ = run_timed([str(binary)], root, timeout=60.0)
    require(
        run_result.returncode == 0 and run_result.stdout == expected,
        f"{size}: native value mismatch\n{run_result.stdout}{run_result.stderr}",
    )
    return elapsed_ms, peak_rss_kib, len(c_source)


def measure_legacy_case(
    root: Path, xray: Path, work: Path, size: int, timeout: float
) -> tuple[int, float, int, int, int]:
    source = work / f"legacy_quoted_{size}.xr"
    generated = work / f"legacy_quoted_{size}.c"
    source.write_text(legacy_source_for(size), encoding="utf-8")
    result, elapsed_ms, peak_rss_kib = run_timed(
        [str(xray), "build", "--native", "--c-only", "-o", str(generated), str(source)],
        root,
        timeout,
    )
    if result.returncode != 0 or not generated.is_file():
        return result.returncode, elapsed_ms, peak_rss_kib, 0, 0
    c_source = generated.read_text(encoding="utf-8")
    stores = len(re.findall(r"_fa\d+\[(?:\d+|INT64_C\(\d+\))\]\s*=", c_source))
    return result.returncode, elapsed_ms, peak_rss_kib, len(c_source), stores


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--xray", type=Path)
    parser.add_argument(
        "--legacy-baseline",
        action="store_true",
        help="measure the pre-214 inline b-literal expansion instead of enforcing the compact gate",
    )
    parser.add_argument("--legacy-timeout", type=float, default=30.0)
    parser.add_argument("--sizes", default=",".join(str(size) for size in SIZES))
    args = parser.parse_args()
    root = args.root.resolve()
    xray = (args.xray or (root / "build" / "xray")).resolve()
    if not xray.is_file():
        parser.error(f"xray binary not found: {xray}")
    try:
        sizes = tuple(int(raw) for raw in args.sizes.split(",") if raw)
    except ValueError:
        parser.error("--sizes must be a comma-separated list of integers")

    try:
        with tempfile.TemporaryDirectory(prefix="xray-quoted-scaling-") as raw_work:
            work = Path(raw_work)
            for size in sizes:
                if args.legacy_baseline:
                    returncode, elapsed_ms, peak_rss_kib, c_bytes, stores = measure_legacy_case(
                        root, xray, work, size, args.legacy_timeout
                    )
                    print(
                        f"size={size} legacy_returncode={returncode} per_byte_stores={stores} "
                        f"generated_c_bytes={c_bytes} elapsed_ms={elapsed_ms:.1f} "
                        f"peak_child_rss_kib={peak_rss_kib}"
                    )
                    continue
                elapsed_ms, peak_rss_kib, c_bytes = verify_case(root, xray, work, size)
                print(
                    f"size={size} plan=1 blob=1 memcpy=1 vm=ok native=ok "
                    f"generated_c_bytes={c_bytes} elapsed_ms={elapsed_ms:.1f} "
                    f"peak_child_rss_kib={peak_rss_kib}"
                )
    except RuntimeError as error:
        print(f"quoted literal scaling gate failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

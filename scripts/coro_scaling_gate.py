#!/usr/bin/env python3
"""Run coroutine benchmark scaling gates for Xray and Go."""

from __future__ import annotations

import argparse
import json
import math
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent
BENCHMARK_DIR = PROJECT_DIR / "tests" / "benchmarks" / "coro"
if "XRAY_BIN" in os.environ:
    DEFAULT_XRAY_BIN = Path(os.environ["XRAY_BIN"])
else:
    DEFAULT_XRAY_BIN = Path(os.environ.get("XRAY_BUILD_DIR", str(PROJECT_DIR / "build"))) / "xray"

DEFAULT_TESTS = [
    "spawn",
    "chain_spawn",
    "pingpong",
    "ring",
    "skynet",
    "fanout",
    "producer_consumer",
    "parallel_sum",
    "work_pool",
    "work_pool_queue",
    "pipeline",
    "select_multiplex",
    "thundering_herd",
    "sleep_storm",
    "timeout_storm",
    "cancel_storm",
    "concurrent_sieve",
    "chameneos",
    "dining_philosophers",
    "starvation",
]

SCHED_METRIC_PREFIXES = (
    "Dispatch mix:",
    "Spawn:",
    "Steal:",
    "Runnable wait:",
    "LIFO gate:",
    "Fast dispatch:",
    "VM fast path:",
    "Coro pool:",
    "Channel wake commands:",
    "Channel wake diagnostics:",
    "Select:",
    "Channel hot path:",
    "Channel logical shape transitions:",
    "Channel worker shape transitions:",
    "Channel logical ops:",
    "Channel logical send ops:",
    "Channel logical recv ops:",
    "Channel block send waiters:",
    "Channel block recv waiters:",
    "Channel ready wake send waiters:",
    "Channel ready wake recv waiters:",
    "Channel ready wake retarget send waiters:",
    "Channel ready wake retarget recv waiters:",
    "Channel worker-shape ops:",
    "Channel lock:",
    "Channel lock wait:",
    "Channel buffered fast path:",
    "Channel ops:",
    "Channel waitq:",
    "Channel ownership:",
    "Channel close fanout:",
    "Task lifecycle:",
    "WorkQueue:",
    "ResultGroup:",
    "Timeout:",
    "Timer:",
    "Timer cancel queue:",
    "Handoff:",
    "Inject:",
    "Inject diagnostics:",
    "Async:",
    "Runtime teardown:",
    "Multicore teardown:",
    "Isolate teardown:",
)


@dataclass(frozen=True)
class RunTarget:
    test: str
    runtime: str
    workers: int
    size_label: str
    args: tuple[str, ...]
    repeat: int


@dataclass
class PreparedRuntime:
    runtime: str
    command_prefix: list[str] | None
    source_available: bool
    prepare_status: str = "ok"
    prepare_stderr: str = ""


XRAY_RUN_RUNTIMES = {"xray", "xray-vm", "xray-jit"}
XRAY_RUNTIMES = XRAY_RUN_RUNTIMES | {"xray-aot"}


def parse_csv_ints(raw: str | None, *, default: list[int]) -> list[int]:
    if not raw:
        return default
    values: list[int] = []
    for part in raw.split(","):
        part = part.strip()
        if not part:
            continue
        values.append(int(part))
    return unique_preserve(values)


def unique_preserve(values: list[int]) -> list[int]:
    seen: set[int] = set()
    result: list[int] = []
    for value in values:
        if value not in seen:
            seen.add(value)
            result.append(value)
    return result


def physical_core_count() -> int:
    if platform.system() == "Darwin":
        try:
            out = subprocess.check_output(["sysctl", "-n", "hw.physicalcpu"], text=True).strip()
            return max(1, int(out))
        except Exception:
            pass
    if platform.system() == "Linux":
        try:
            out = subprocess.check_output(["lscpu", "-p=Core,Socket"], text=True)
            pairs = {
                line.strip()
                for line in out.splitlines()
                if line.strip() and not line.startswith("#")
            }
            if pairs:
                return len(pairs)
        except Exception:
            pass
    return max(1, os.cpu_count() or 1)


def default_workers() -> list[int]:
    return unique_preserve([1, 2, 4, 8, 16, physical_core_count()])


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run coroutine scaling and correctness gates.")
    runtime = parser.add_mutually_exclusive_group()
    runtime.add_argument("--xray-only", action="store_true", help="Run only Xray VM benchmarks.")
    runtime.add_argument("--vm-only", action="store_true", help="Alias for --xray-only.")
    runtime.add_argument("--jit-only", action="store_true", help="Run only Xray JIT benchmarks with --jit-force.")
    runtime.add_argument("--vm-jit", action="store_true", help="Run Xray VM and Xray JIT benchmarks.")
    runtime.add_argument("--vm-jit-go", action="store_true", help="Run Xray VM, Xray JIT, and Go benchmarks.")
    runtime.add_argument("--go", action="store_true", help="Run only Go benchmarks.")
    runtime.add_argument("--aot-only", action="store_true", help="Run only Xray AOT benchmarks.")
    runtime.add_argument("--vm-go", action="store_true", help="Run Xray VM and Go benchmarks.")
    runtime.add_argument("--all", action="store_true", help="Alias for --vm-go.")
    runtime.add_argument("--all-backends", action="store_true", help="Run Xray VM, Xray JIT, Xray AOT, and Go benchmarks.")
    parser.add_argument("--include-aot-correctness", action="store_true", help="Append Xray AOT as a correctness appendix to the selected VM/JIT matrix.")
    parser.add_argument("--tests", help="Comma-separated benchmark names.")
    parser.add_argument("--repeats", type=int, default=5, help="Run count for each matrix point.")
    parser.add_argument("--workers", help="Comma-separated workers/procs list. Defaults to 1,2,4,8,16,physical.")
    parser.add_argument("--cardinality", help="Comma-separated N values, such as 1000,10000,100000,1000000.")
    parser.add_argument("--args", help="Space-separated benchmark args. Overrides cardinality mapping.")
    parser.add_argument("--json", help="Machine-readable JSON output path.")
    parser.add_argument("--markdown", help="Human-readable Markdown report path.")
    parser.add_argument("--xray-bin", default=str(DEFAULT_XRAY_BIN), help="Path to xray executable.")
    parser.add_argument("--sched-stats", action="store_true", help="Enable XRAY_SCHED_STATS for Xray runs.")
    parser.add_argument("--timeout", type=float, default=120.0, help="Per-run wall timeout in seconds.")
    parser.add_argument("--cleanup-tail-threshold", type=float, default=1.15, help="wall/reported ratio that marks cleanup-tail.")
    parser.add_argument("--cleanup-tail-min-ms", type=float, default=50.0, help="Minimum wall-reported delta before cleanup-tail is enforced.")
    parser.add_argument("--keep-going", action="store_true", help="Accepted for compatibility; the gate always completes the matrix.")
    parser.add_argument("--list", action="store_true", help="List known benchmark names and exit.")
    args = parser.parse_args()
    if args.list:
        for name in DEFAULT_TESTS:
            print(name)
        raise SystemExit(0)
    if not args.json:
        parser.error("--json is required unless --list is used")
    if args.repeats < 1:
        parser.error("--repeats must be >= 1")
    return args


def selected_runtimes(args: argparse.Namespace) -> list[str]:
    if args.go:
        return ["go"]
    if args.aot_only:
        return ["xray-aot"]
    if args.jit_only:
        runtimes = ["xray-jit"]
    elif args.vm_jit:
        runtimes = ["xray-vm", "xray-jit"]
    elif args.vm_jit_go:
        runtimes = ["xray-vm", "xray-jit", "go"]
    elif args.vm_go or args.all:
        runtimes = ["xray-vm", "go"]
    elif args.all_backends:
        runtimes = ["xray-vm", "xray-jit", "xray-aot", "go"]
    elif args.xray_only or args.vm_only:
        runtimes = ["xray-vm"]
    else:
        runtimes = ["xray-vm"]

    if args.include_aot_correctness and "xray-aot" not in runtimes:
        if "go" in runtimes:
            insert_at = runtimes.index("go")
            runtimes.insert(insert_at, "xray-aot")
        else:
            runtimes.append("xray-aot")
    return runtimes


def selected_tests(args: argparse.Namespace) -> list[str]:
    if args.tests:
        return [item.strip() for item in args.tests.split(",") if item.strip()]
    return [name for name in DEFAULT_TESTS if (BENCHMARK_DIR / name).is_dir()]


def cardinality_args(test: str, n: int) -> tuple[str, ...]:
    if test in {"spawn", "pingpong", "cancel_storm", "sleep_storm", "timeout_storm"}:
        if test in {"sleep_storm", "timeout_storm"}:
            return (str(n), "10")
        return (str(n),)
    if test == "chain_spawn":
        return (str(n),)
    if test in {"skynet"}:
        depth = max(1, min(6, int(round(math.log10(max(10, n))))))
        return (str(depth),)
    if test == "ring":
        nodes = min(max(1, n), 1000)
        rounds = max(1, n // nodes)
        return (str(nodes), str(rounds))
    if test in {
        "fanout",
        "work_pool",
        "work_pool_queue",
        "work_pool_result_group",
        "work_pool_queue_result_group",
    }:
        workers = min(100, max(1, int(math.sqrt(n))))
        return (str(workers), str(n))
    if test == "producer_consumer":
        producers = 4
        consumers = 4
        return (str(producers), str(consumers), str(n))
    if test == "parallel_sum":
        workers = min(16, max(1, physical_core_count()))
        return (str(workers), str(n))
    if test == "pipeline":
        return ("5", str(n))
    if test == "select_multiplex":
        return (str(max(1, n // 4)),)
    if test == "thundering_herd":
        waiters = min(max(1, n), 10000)
        rounds = max(1, n // waiters)
        return (str(waiters), str(rounds))
    if test == "concurrent_sieve":
        return (str(max(1, min(n, 10000))),)
    if test == "chameneos":
        return (str(n), "10")
    if test == "dining_philosophers":
        philosophers = 5
        meals = max(1, n // philosophers)
        return (str(philosophers), str(meals))
    if test == "starvation":
        return (str(n), "2000")
    if test == "blocking_storm":
        return (str(n), "10")
    return (str(n),)


def matrix_args(args: argparse.Namespace, test: str) -> list[tuple[str, tuple[str, ...]]]:
    if args.args:
        return [("custom", tuple(args.args.split()))]
    if args.cardinality:
        values = parse_csv_ints(args.cardinality, default=[])
        return [(f"n{value}", cardinality_args(test, value)) for value in values]
    return [("default", ())]


def prepare_runtime(
    runtime: str,
    test: str,
    tmp_dir: Path,
    xray_bin: Path,
) -> PreparedRuntime:
    test_dir = BENCHMARK_DIR / test
    if runtime in XRAY_RUN_RUNTIMES:
        source = test_dir / f"{test}.xr"
        if not source.exists():
            return PreparedRuntime(runtime, None, False, "skipped-missing-source")
        flags: list[str] = []
        if runtime == "xray-vm":
            flags.append("--no-jit")
        elif runtime == "xray-jit":
            flags.extend(["--jit-force", "--jit-stats"])
        return PreparedRuntime(runtime, [str(xray_bin), "run", *flags, str(source), "--"], True)
    if runtime == "xray-aot":
        source = test_dir / f"{test}.xr"
        if not source.exists():
            return PreparedRuntime(runtime, None, False, "skipped-missing-source")
        out_bin = tmp_dir / f"{test}_xray_aot"
        proc = subprocess.run(
            [str(xray_bin), "build", "--native", str(source), "-o", str(out_bin)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if proc.returncode != 0:
            return PreparedRuntime(runtime, None, True, "build-fail", summarize(proc.stdout + proc.stderr))
        return PreparedRuntime(runtime, [str(out_bin)], True)
    if runtime == "go":
        source = test_dir / f"{test}.go"
        if not source.exists():
            return PreparedRuntime(runtime, None, False, "skipped-missing-source")
        if shutil.which("go") is None:
            return PreparedRuntime(runtime, None, True, "skipped-go-missing")
        out_bin = tmp_dir / f"{test}_go"
        proc = subprocess.run(
            ["go", "build", "-o", str(out_bin), source.name],
            cwd=str(test_dir),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if proc.returncode != 0:
            return PreparedRuntime(runtime, None, True, "build-fail", summarize(proc.stdout + proc.stderr))
        return PreparedRuntime(runtime, [str(out_bin)], True)
    raise ValueError(f"unknown runtime: {runtime}")


def command_for(prepared: PreparedRuntime, bench_args: tuple[str, ...]) -> list[str]:
    if prepared.command_prefix is None:
        return []
    if prepared.runtime in XRAY_RUN_RUNTIMES:
        return prepared.command_prefix + list(bench_args)
    return prepared.command_prefix + list(bench_args)


def env_for(runtime: str, workers: int, sched_stats: bool) -> dict[str, str]:
    env = dict(os.environ)
    if runtime in XRAY_RUNTIMES:
        env["XRAY_WORKERS"] = str(workers)
        if sched_stats:
            env["XRAY_SCHED_STATS"] = "1"
    elif runtime == "go":
        env["GOMAXPROCS"] = str(workers)
    return env


def warmup_prepared_runtime(prep: PreparedRuntime, bench_args: tuple[str, ...], workers: int, timeout: float) -> None:
    if prep.runtime != "go" or prep.prepare_status != "ok":
        return
    cmd = command_for(prep, bench_args)
    if not cmd:
        return
    try:
        subprocess.run(
            cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            env=env_for(prep.runtime, workers, False),
            timeout=timeout,
        )
    except (OSError, subprocess.TimeoutExpired):
        pass


def run_measured(cmd: list[str], env: dict[str, str], timeout: float) -> tuple[int, float, int | None, str, str, str]:
    time_file = tempfile.NamedTemporaryFile(delete=False)
    time_file.close()
    time_cmd: list[str]
    if platform.system() == "Darwin":
        time_cmd = ["/usr/bin/time", "-l", "-o", time_file.name]
    else:
        time_cmd = ["/usr/bin/time", "-v", "-o", time_file.name]
    start = time.perf_counter()
    try:
        proc = subprocess.run(
            time_cmd + cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
            timeout=timeout,
        )
        exit_code = proc.returncode
        stdout = proc.stdout
        stderr = proc.stderr
    except subprocess.TimeoutExpired as exc:
        exit_code = 124
        stdout = exc.stdout if isinstance(exc.stdout, str) else (exc.stdout or b"").decode("utf-8", "replace")
        stderr = exc.stderr if isinstance(exc.stderr, str) else (exc.stderr or b"").decode("utf-8", "replace")
        stderr += f"\nTimeout after {timeout:.1f}s"
    wall_ms = (time.perf_counter() - start) * 1000.0
    try:
        time_text = Path(time_file.name).read_text(encoding="utf-8", errors="replace")
    finally:
        Path(time_file.name).unlink(missing_ok=True)
    return exit_code, wall_ms, parse_rss_bytes(time_text), stdout, stderr, time_text


def parse_rss_bytes(time_text: str) -> int | None:
    match = re.search(r"^\s*(\d+)\s+maximum resident set size", time_text, re.MULTILINE)
    if match:
        return int(match.group(1))
    match = re.search(r"Maximum resident set size \(kbytes\):\s*(\d+)", time_text)
    if match:
        return int(match.group(1)) * 1024
    return None


def first_number_after(patterns: tuple[str, ...], text: str) -> float | None:
    for pattern in patterns:
        match = re.search(pattern, text, re.IGNORECASE | re.MULTILINE)
        if match:
            return float(match.group(1))
    return None


def all_ints_after(pattern: str, text: str) -> tuple[int, ...] | None:
    match = re.search(pattern, text, re.IGNORECASE | re.MULTILINE)
    if not match:
        return None
    return tuple(int(group) for group in match.groups())


def parse_bool_label(label: str, text: str) -> bool | None:
    match = re.search(rf"^{re.escape(label)}\s*:\s*(true|false|TRUE|FALSE|True|False)", text, re.MULTILINE)
    if match:
        return match.group(1).lower() == "true"
    return None


def parse_load_balance(text: str) -> dict[str, float | int | None]:
    match = re.search(
        r"^(?:负载均衡|load balance)\s*:\s*min=\s*([0-9]+)\s+max=\s*([0-9]+)\s+ratio=\s*([0-9]+(?:\.[0-9]+)?)",
        text,
        re.IGNORECASE | re.MULTILINE,
    )
    if not match:
        return {"min": None, "max": None, "ratio": None}
    return {
        "min": int(match.group(1)),
        "max": int(match.group(2)),
        "ratio": float(match.group(3)),
    }


def parse_output(test: str, stdout: str, stderr: str, wall_ms: float, cleanup_threshold: float, cleanup_min_delta_ms: float) -> dict[str, Any]:
    combined = stdout + "\n" + stderr
    load_balance = parse_load_balance(combined)
    reported_time_ms = first_number_after(
        (
            r"^reported_time_ms\s*:\s*([0-9]+(?:\.[0-9]+)?)",
            r"^(?:总时间|Total time|Total Time)\s*:\s*([0-9]+(?:\.[0-9]+)?)",
            r"^(?:时间|Time|elapsed|elapsed_ms)\s*:\s*([0-9]+(?:\.[0-9]+)?)",
        ),
        combined,
    )
    if reported_time_ms is None:
        recv_wall = first_number_after((r"^recv wall\s*:\s*([0-9]+(?:\.[0-9]+)?)",), combined)
        send_wall = first_number_after((r"^send wall\s*:\s*([0-9]+(?:\.[0-9]+)?)",), combined)
        if recv_wall is not None and send_wall is not None:
            reported_time_ms = recv_wall + send_wall

    throughput = first_number_after(
        (
            r"^(?:吞吐量|throughput|Throughput)\s*:\s*([0-9]+(?:\.[0-9]+)?)",
            r"^(?:速度|Speed)\s*:\s*([0-9]+(?:\.[0-9]+)?)",
        ),
        combined,
    )

    completed_ops = first_number_after((r"^completed_ops\s*:\s*([0-9]+)",), combined)
    expected_ops = first_number_after((r"^expected_ops\s*:\s*([0-9]+)",), combined)
    checksum = first_number_after((r"^checksum\s*:\s*(-?[0-9]+)",), combined)
    explicit_correct = parse_bool_label("correctness", combined)

    if completed_ops is None or expected_ops is None:
        completed_ops, expected_ops = infer_completed_expected(test, combined)

    if checksum is None:
        checksum = first_number_after(
            (
                r"^(?:结果校验和|结果|最终结果|cpu_result|total)\s*:\s*(-?[0-9]+)",
            ),
            combined,
        )

    legacy_bools = [
        parse_bool_label("正确", combined),
        parse_bool_label("公平调度", combined),
        parse_bool_label("high_before_normal", combined),
    ]
    if "结果: PASS" in combined:
        legacy_bools.append(True)
    if "结果: FAIL" in combined:
        legacy_bools.append(False)
    explicit_values = [value for value in [explicit_correct, *legacy_bools] if value is not None]
    if explicit_values:
        correctness = all(explicit_values)
    elif completed_ops is not None and expected_ops is not None:
        correctness = int(completed_ops) == int(expected_ops)
    else:
        correctness = False

    cleanup_tail = False
    wall_report_ratio = None
    if reported_time_ms is not None and reported_time_ms > 0:
        wall_report_ratio = wall_ms / reported_time_ms
        cleanup_tail = wall_report_ratio > cleanup_threshold and (wall_ms - reported_time_ms) >= cleanup_min_delta_ms

    return {
        "reported_time_ms": reported_time_ms,
        "throughput": throughput,
        "completed_ops": int(completed_ops) if completed_ops is not None else None,
        "expected_ops": int(expected_ops) if expected_ops is not None else None,
        "checksum": int(checksum) if checksum is not None else None,
        "correctness": correctness,
        "wall_report_ratio": wall_report_ratio,
        "cleanup_tail": cleanup_tail,
        "load_balance_min": load_balance["min"],
        "load_balance_max": load_balance["max"],
        "load_balance_ratio": load_balance["ratio"],
        "sched_metrics": parse_sched_metrics(combined),
        "stdout_summary": summarize(stdout),
        "stderr_summary": summarize(stderr),
    }


def infer_completed_expected(test: str, text: str) -> tuple[float | None, float | None]:
    pair_patterns = (
        r"^成功取消\s*:\s*([0-9]+)\s*/\s*([0-9]+)",
        r"^短任务完成\s*:\s*([0-9]+)\s*/\s*([0-9]+)",
    )
    for pattern in pair_patterns:
        values = all_ints_after(pattern, text)
        if values:
            return float(values[0]), float(values[1])

    if test == "timeout_storm":
        recv_completed = first_number_after((r"^recv completed\s*:\s*([0-9]+)",), text)
        send_completed = first_number_after((r"^send completed\s*:\s*([0-9]+)",), text)
        recv_timeouts = first_number_after((r"^recv timeouts\s*:\s*([0-9]+)",), text)
        send_timeouts = first_number_after((r"^send timeouts\s*:\s*([0-9]+)",), text)
        if None not in (recv_completed, send_completed, recv_timeouts, send_timeouts):
            completed = recv_completed + send_completed
            expected = recv_timeouts + send_timeouts
            return completed, expected

    label_pairs = (
        ("接收数量", "数据项数"),
        ("总会合次数", "预期会合次数"),
        ("总进餐次数", "预期"),
        ("消费总数", "预期"),
        ("接收总数", "预期"),
        ("总唤醒数", "预期"),
        ("找到素数", "预期"),
        ("结果校验和", "预期"),
        ("结果", "预期"),
        ("最终结果", "预期"),
    )
    for completed_label, expected_label in label_pairs:
        completed = first_number_after((rf"^{re.escape(completed_label)}\s*:\s*(-?[0-9]+)",), text)
        expected = first_number_after((rf"^{re.escape(expected_label)}\s*:\s*(-?[0-9]+)",), text)
        if completed is not None and expected is not None:
            return completed, expected

    single_total_patterns = (
        r"^消息总数\s*:\s*([0-9]+)",
        r"^总消息数\s*:\s*([0-9]+)",
        r"^协程数量\s*:\s*([0-9]+)",
        r"^任务总数\s*:\s*([0-9]+)",
        r"^任务数量\s*:\s*([0-9]+)",
        r"^数据项数\s*:\s*([0-9]+)",
        r"^coroutines\s*:\s*([0-9]+)",
        r"^tasks\s*:\s*([0-9]+)",
        r"^low_tasks\s*:\s*([0-9]+)",
    )
    for pattern in single_total_patterns:
        value = first_number_after((pattern,), text)
        if value is not None:
            return value, value
    return None, None


def parse_sched_metrics(text: str) -> dict[str, float | None]:
    metrics: dict[str, float | None] = {}
    for line in text.splitlines():
        prefix = next((item for item in SCHED_METRIC_PREFIXES if line.startswith(item)), None)
        if prefix is None:
            continue
        section = re.sub(r"[^A-Za-z0-9]+", "_", prefix.strip(":").strip()).strip("_").lower()
        if section == "workqueue":
            section = "work_queue"
        if section == "resultgroup":
            section = "result_group"
        for key, value in re.findall(r"([A-Za-z_][A-Za-z0-9_]*)=(-?[0-9]+(?:\.[0-9]+)?)", line):
            metrics[f"{section}_{key}"] = float(value)
    return metrics


def parse_jit_metrics(text: str) -> dict[str, float | int | None]:
    metrics: dict[str, float | int | None] = {
        "functions_compiled": None,
        "tier1": None,
        "tier2": None,
        "conservative": None,
        "compile_ms": None,
        "compile_avg_ms": None,
        "code_cache_used_kb": None,
        "code_cache_allocated_kb": None,
        "code_cache_budget_mb": None,
        "deopts": None,
        "disabled": None,
        "evicted": None,
        "suspend_count": None,
        "resume_count": None,
        "helper_count": None,
        "chan_try_hit": None,
        "chan_try_miss": None,
        "chan_block": None,
        "await_done_fast": None,
        "await_block": None,
    }
    match = re.search(
        r"Functions compiled:\s*(\d+)\s*\(Tier1:\s*(\d+),\s*Tier2:\s*(\d+),\s*Conservative:\s*(\d+)\)",
        text,
    )
    if match:
        metrics["functions_compiled"] = int(match.group(1))
        metrics["tier1"] = int(match.group(2))
        metrics["tier2"] = int(match.group(3))
        metrics["conservative"] = int(match.group(4))
    match = re.search(
        r"Total compile time:\s*(\d+)ms\s*\(avg\s*([0-9]+(?:\.[0-9]+)?)ms/func\)",
        text,
    )
    if match:
        metrics["compile_ms"] = int(match.group(1))
        metrics["compile_avg_ms"] = float(match.group(2))
    match = re.search(
        r"Code cache:\s*(\d+)KB used\s*/\s*(\d+)KB allocated\s*\(budget\s*(\d+)MB\)",
        text,
    )
    if match:
        metrics["code_cache_used_kb"] = int(match.group(1))
        metrics["code_cache_allocated_kb"] = int(match.group(2))
        metrics["code_cache_budget_mb"] = int(match.group(3))
    match = re.search(r"Deopts:\s*(\d+)\s+Permanently disabled:\s*(\d+)\s+Evicted:\s*(\d+)", text)
    if match:
        metrics["deopts"] = int(match.group(1))
        metrics["disabled"] = int(match.group(2))
        metrics["evicted"] = int(match.group(3))
    match = re.search(
        r"(?:Suspend/resume|Suspends?)[:\s]+(?:suspends?=)?(\d+)[,\s]+(?:resumes?=)?(\d+)",
        text,
        re.IGNORECASE,
    )
    if match:
        metrics["suspend_count"] = int(match.group(1))
        metrics["resume_count"] = int(match.group(2))
    match = re.search(
        r"(?:JIT coroutine helpers|JIT helpers|Helpers|helper_count)[:=\s]+(\d+)",
        text,
        re.IGNORECASE,
    )
    if match:
        metrics["helper_count"] = int(match.group(1))
    match = re.search(
        r"Coroutine runtime:\s*chan_try_hit=(\d+)\s+chan_try_miss=(\d+)\s+"
        r"chan_block=(\d+)\s+await_done_fast=(\d+)\s+await_block=(\d+)",
        text,
    )
    if match:
        metrics["chan_try_hit"] = int(match.group(1))
        metrics["chan_try_miss"] = int(match.group(2))
        metrics["chan_block"] = int(match.group(3))
        metrics["await_done_fast"] = int(match.group(4))
        metrics["await_block"] = int(match.group(5))
    return metrics


def runtime_time_ms(runtime: str, reported_time_ms: float | None, jit_metrics: dict[str, Any]) -> float | None:
    if reported_time_ms is None:
        return None
    if runtime != "xray-jit":
        return reported_time_ms
    compile_ms = jit_metrics.get("compile_ms")
    if not isinstance(compile_ms, (int, float)):
        return reported_time_ms
    return max(0.0, reported_time_ms - float(compile_ms))


def summarize(text: str, max_lines: int = 12) -> str:
    lines = [line for line in text.strip().splitlines() if line.strip()]
    if len(lines) <= max_lines:
        return "\n".join(lines)
    head = lines[: max_lines // 2]
    tail = lines[-(max_lines - len(head)) :]
    return "\n".join(head + ["..."] + tail)


def numeric_stats(values: list[float | int | None]) -> dict[str, float | int | None]:
    clean = [float(value) for value in values if value is not None]
    if not clean:
        return {"median": None, "min": None, "max": None}
    return {
        "median": statistics.median(clean),
        "min": min(clean),
        "max": max(clean),
    }


def summarize_results(results: list[dict[str, Any]]) -> list[dict[str, Any]]:
    groups: dict[tuple[str, str, int, str, tuple[str, ...]], list[dict[str, Any]]] = {}
    for result in results:
        if result["status"].startswith("skipped"):
            continue
        key = (
            result["test"],
            result["runtime"],
            result["workers"],
            result["size_label"],
            tuple(result["args"]),
        )
        groups.setdefault(key, []).append(result)

    summaries: list[dict[str, Any]] = []
    for key, items in sorted(groups.items()):
        test, runtime, workers, size_label, bench_args = key
        valid_items = [item for item in items if item.get("valid") is True]
        sched_metric_keys = sorted(
            {
                key
                for item in valid_items
                for key in (item.get("sched_metrics") or {}).keys()
            }
        )
        sched_metrics = {
            key: numeric_stats([(item.get("sched_metrics") or {}).get(key) for item in valid_items])
            for key in sched_metric_keys
        }
        summaries.append(
            {
                "test": test,
                "runtime": runtime,
                "workers": workers,
                "size_label": size_label,
                "args": list(bench_args),
                "repeats": len(items),
                "valid_repeats": len(valid_items),
                "status_counts": count_by(items, "status"),
                "wall_ms": numeric_stats([item.get("wall_time_ms") for item in valid_items]),
                "reported_time_ms": numeric_stats([item.get("reported_time_ms") for item in valid_items]),
                "rss_peak_bytes": numeric_stats([item.get("rss_peak_bytes") for item in valid_items]),
                "throughput": numeric_stats([item.get("throughput") for item in valid_items]),
                "load_balance_min": numeric_stats([item.get("load_balance_min") for item in valid_items]),
                "load_balance_max": numeric_stats([item.get("load_balance_max") for item in valid_items]),
                "load_balance_ratio": numeric_stats([item.get("load_balance_ratio") for item in valid_items]),
                "runtime_time_ms": numeric_stats([item.get("runtime_time_ms") for item in valid_items]),
                "jit_compile_ms": numeric_stats([item.get("jit_compile_ms") for item in valid_items]),
                "jit_deopts": numeric_stats([item.get("jit_metrics", {}).get("deopts") for item in valid_items]),
                "jit_suspend_count": numeric_stats([item.get("jit_metrics", {}).get("suspend_count") for item in valid_items]),
                "jit_resume_count": numeric_stats([item.get("jit_metrics", {}).get("resume_count") for item in valid_items]),
                "jit_helper_count": numeric_stats([item.get("jit_metrics", {}).get("helper_count") for item in valid_items]),
                "jit_chan_try_hit": numeric_stats([item.get("jit_metrics", {}).get("chan_try_hit") for item in valid_items]),
                "jit_chan_try_miss": numeric_stats([item.get("jit_metrics", {}).get("chan_try_miss") for item in valid_items]),
                "jit_chan_block": numeric_stats([item.get("jit_metrics", {}).get("chan_block") for item in valid_items]),
                "jit_await_done_fast": numeric_stats([item.get("jit_metrics", {}).get("await_done_fast") for item in valid_items]),
                "jit_await_block": numeric_stats([item.get("jit_metrics", {}).get("await_block") for item in valid_items]),
                "sched_metrics": sched_metrics,
                "cleanup_tail": any(item.get("cleanup_tail") for item in items),
                "correctness": all(item.get("correctness") is True for item in items),
                "performance_blocking": all(item.get("performance_blocking", True) for item in items),
                "speedup_vs_base": None,
                "scaling_efficiency": None,
                "scaling_base_workers": None,
            }
        )
    annotate_scaling_efficiency(summaries)
    return summaries


def annotate_scaling_efficiency(summaries: list[dict[str, Any]]) -> None:
    groups: dict[tuple[str, str, str, tuple[str, ...]], list[dict[str, Any]]] = {}
    for summary in summaries:
        key = (
            summary["test"],
            summary["runtime"],
            summary["size_label"],
            tuple(summary["args"]),
        )
        groups.setdefault(key, []).append(summary)

    for items in groups.values():
        candidates = [
            item
            for item in items
            if item["runtime_time_ms"]["median"] is not None
            and item["runtime_time_ms"]["median"] > 0
            and item["workers"] > 0
        ]
        if not candidates:
            continue
        base = min(candidates, key=lambda item: item["workers"])
        base_workers = int(base["workers"])
        base_ms = float(base["runtime_time_ms"]["median"])
        if base_ms <= 0:
            continue
        for item in items:
            current_ms = item["runtime_time_ms"]["median"]
            workers = int(item["workers"])
            item["scaling_base_workers"] = base_workers
            if current_ms is None or current_ms <= 0 or workers <= 0:
                continue
            speedup = base_ms / float(current_ms)
            item["speedup_vs_base"] = speedup
            item["scaling_efficiency"] = speedup / (workers / base_workers)


def summarize_vm_go_comparisons(summaries: list[dict[str, Any]]) -> list[dict[str, Any]]:
    by_key: dict[tuple[str, int, str, tuple[str, ...]], dict[str, dict[str, Any]]] = {}
    for summary in summaries:
        if summary["runtime"] not in {"xray-vm", "go"}:
            continue
        key = (
            summary["test"],
            int(summary["workers"]),
            summary["size_label"],
            tuple(summary["args"]),
        )
        by_key.setdefault(key, {})[summary["runtime"]] = summary

    comparisons: list[dict[str, Any]] = []
    for key, runtimes in sorted(by_key.items()):
        vm = runtimes.get("xray-vm")
        go = runtimes.get("go")
        if not vm or not go:
            continue
        vm_runtime_ms = vm["runtime_time_ms"]["median"]
        go_runtime_ms = go["runtime_time_ms"]["median"]
        vm_wall_ms = vm["wall_ms"]["median"]
        go_wall_ms = go["wall_ms"]["median"]
        comparisons.append(
            {
                "test": key[0],
                "workers": key[1],
                "size_label": key[2],
                "args": list(key[3]),
                "vm_runtime_ms": vm_runtime_ms,
                "go_runtime_ms": go_runtime_ms,
                "vm_go_runtime_ratio": ratio_or_none(vm_runtime_ms, go_runtime_ms),
                "vm_wall_ms": vm_wall_ms,
                "go_wall_ms": go_wall_ms,
                "vm_go_wall_ratio": ratio_or_none(vm_wall_ms, go_wall_ms),
                "vm_scaling_efficiency": vm.get("scaling_efficiency"),
                "go_scaling_efficiency": go.get("scaling_efficiency"),
                "vm_valid_repeats": vm["valid_repeats"],
                "go_valid_repeats": go["valid_repeats"],
                "vm_repeats": vm["repeats"],
                "go_repeats": go["repeats"],
                "correctness": vm["correctness"] and go["correctness"],
            }
        )
    return comparisons


def ratio_or_none(numerator: Any, denominator: Any) -> float | None:
    if not isinstance(numerator, (int, float)) or not isinstance(denominator, (int, float)):
        return None
    if numerator <= 0 or denominator <= 0:
        return None
    return float(numerator) / float(denominator)


def count_by(items: list[dict[str, Any]], key: str) -> dict[str, int]:
    counts: dict[str, int] = {}
    for item in items:
        value = str(item.get(key))
        counts[value] = counts.get(value, 0) + 1
    return counts


def write_markdown(path: Path, payload: dict[str, Any]) -> None:
    lines: list[str] = []
    lines.append("# Coroutine Scaling Gate Report")
    lines.append("")
    lines.append(f"- generated_at: `{payload['generated_at']}`")
    lines.append(f"- runtimes: `{', '.join(payload['runtimes'])}`")
    lines.append(f"- workers: `{', '.join(str(w) for w in payload['workers'])}`")
    lines.append(f"- repeats: `{payload['repeats']}`")
    lines.append("")
    invalid = [item for item in payload["results"] if item.get("valid") is False]
    cleanup = [item for item in payload["results"] if item.get("cleanup_tail")]
    appendix = [item for item in payload["results"] if item.get("correctness_appendix")]
    lines.append(f"- invalid_runs: `{len(invalid)}`")
    lines.append(f"- cleanup_tail_runs: `{len(cleanup)}`")
    if appendix:
        lines.append(f"- aot_correctness_appendix_runs: `{len(appendix)}`")
    lines.append("")
    lines.append("| test | runtime | workers | size | bench args | valid | reported median ms | runtime median ms | wall median ms | speedup vs base | scaling efficiency | jit compile median ms | jit deopts median | jit suspend median | jit resume median | jit helper median | rss median bytes | cleanup-tail |")
    lines.append("|------|---------|---------|------|------------|-------|--------------------|-------------------|----------------|-----------------|--------------------|-----------------------|------------------|--------------------|-------------------|-------------------|------------------|--------------|")
    for summary in payload["summaries"]:
        lines.append(
            "| {test} | {runtime} | {workers} | {size_label} | {bench_args} | {valid}/{repeats} | {reported} | {runtime_ms} | {wall} | {speedup} | {efficiency} | {jit_compile} | {jit_deopts} | {jit_suspend} | {jit_resume} | {jit_helper} | {rss} | {tail} |".format(
                test=summary["test"],
                runtime=summary["runtime"],
                workers=summary["workers"],
                size_label=summary["size_label"],
                bench_args=fmt_args(summary.get("args")),
                valid=summary["valid_repeats"],
                repeats=summary["repeats"],
                reported=fmt_num(summary["reported_time_ms"]["median"]),
                runtime_ms=fmt_num(summary["runtime_time_ms"]["median"]),
                wall=fmt_num(summary["wall_ms"]["median"]),
                speedup=fmt_num(summary["speedup_vs_base"]),
                efficiency=fmt_num(summary["scaling_efficiency"]),
                jit_compile=fmt_num(summary["jit_compile_ms"]["median"]),
                jit_deopts=fmt_num(summary["jit_deopts"]["median"]),
                jit_suspend=fmt_num(summary["jit_suspend_count"]["median"]),
                jit_resume=fmt_num(summary["jit_resume_count"]["median"]),
                jit_helper=fmt_num(summary["jit_helper_count"]["median"]),
                rss=fmt_num(summary["rss_peak_bytes"]["median"]),
                tail="yes" if summary["cleanup_tail"] else "no",
            )
        )
    comparisons = payload.get("vm_go_comparisons") or []
    if comparisons:
        lines.append("")
        lines.append("## VM/Go Comparisons")
        lines.append("")
        lines.append("| test | workers | size | bench args | VM runtime ms | Go runtime ms | VM/Go runtime ratio | VM wall ms | Go wall ms | VM/Go wall ratio | VM scaling efficiency | Go scaling efficiency |")
        lines.append("|------|---------|------|------------|---------------|---------------|---------------------|------------|------------|------------------|-----------------------|-----------------------|")
        for item in comparisons:
            lines.append(
                "| {test} | {workers} | {size_label} | {bench_args} | {vm_runtime} | {go_runtime} | {runtime_ratio} | {vm_wall} | {go_wall} | {wall_ratio} | {vm_efficiency} | {go_efficiency} |".format(
                    test=item["test"],
                    workers=item["workers"],
                    size_label=item["size_label"],
                    bench_args=fmt_args(item.get("args")),
                    vm_runtime=fmt_num(item["vm_runtime_ms"]),
                    go_runtime=fmt_num(item["go_runtime_ms"]),
                    runtime_ratio=fmt_num(item["vm_go_runtime_ratio"]),
                    vm_wall=fmt_num(item["vm_wall_ms"]),
                    go_wall=fmt_num(item["go_wall_ms"]),
                    wall_ratio=fmt_num(item["vm_go_wall_ratio"]),
                    vm_efficiency=fmt_num(item["vm_scaling_efficiency"]),
                    go_efficiency=fmt_num(item["go_scaling_efficiency"]),
                )
            )
    load_rows = [
        summary
        for summary in payload["summaries"]
        if summary.get("load_balance_ratio", {}).get("median") is not None
    ]
    if load_rows:
        lines.append("")
        lines.append("## Load Balance")
        lines.append("")
        lines.append("| test | runtime | workers | size | bench args | min median | max median | ratio median |")
        lines.append("|------|---------|---------|------|------------|------------|------------|--------------|")
        for summary in load_rows:
            lines.append(
                "| {test} | {runtime} | {workers} | {size_label} | {bench_args} | {load_min} | {load_max} | {load_ratio} |".format(
                    test=summary["test"],
                    runtime=summary["runtime"],
                    workers=summary["workers"],
                    size_label=summary["size_label"],
                    bench_args=fmt_args(summary.get("args")),
                    load_min=fmt_num(summary["load_balance_min"]["median"]),
                    load_max=fmt_num(summary["load_balance_max"]["median"]),
                    load_ratio=fmt_num(summary["load_balance_ratio"]["median"]),
                )
            )
    metric_rows = [
        summary
        for summary in payload["summaries"]
        if summary["runtime"].startswith("xray") and summary.get("sched_metrics")
    ]
    if metric_rows:
        selected_metrics = (
            "vm_fast_path_chan_send_fast_no_ext",
            "vm_fast_path_chan_recv_fast_no_ext",
            "vm_fast_path_await_done_fast",
            "vm_fast_path_select_probe_hit",
            "steal_success_ratio",
            "spawn_shared_ratio",
            "spawn_burst_share_ratio",
            "steal_items_per_success",
            "steal_batch_empty",
            "steal_batch_fail_ratio",
            "steal_fresh_reject",
            "steal_throttle_wait_ms",
            "inject_spill",
            "inject_diagnostics_worker_pull",
            "channel_ops_send_block",
            "channel_ops_recv_block",
            "channel_waitq_ready_wake",
            "channel_buffered_fast_path_hit_rate",
            "channel_ownership_send_deep_copy",
            "channel_ownership_recv_deep_copy",
        )
        lines.append("")
        lines.append("## Runtime Metrics")
        lines.append("")
        lines.append("| test | runtime | workers | size | send no-ext | recv no-ext | await done fast | select probe hit | steal success % | spawn shared % | spawn burst % | steal items/success | batch empty | batch fail % | steal fresh reject | steal wait ms | inject spill | inject pull | send block | recv block | ready wake | buffered hit % | send deep copy | recv deep copy |")
        lines.append("|------|---------|---------|------|-------------|-------------|-----------------|------------------|-----------------|----------------|---------------|---------------------|-------------|--------------|--------------------|---------------|--------------|-------------|------------|------------|------------|----------------|----------------|----------------|")
        for summary in metric_rows:
            metrics = summary.get("sched_metrics") or {}
            values = {
                key: (metrics.get(key) or {}).get("median")
                for key in selected_metrics
            }
            lines.append(
                "| {test} | {runtime} | {workers} | {size_label} | {send_no_ext} | {recv_no_ext} | {await_done} | {select_probe} | {steal_success} | {spawn_shared} | {spawn_burst} | {steal_items} | {batch_empty} | {batch_fail} | {steal_fresh} | {steal_wait} | {inject_spill} | {inject_pull} | {send_block} | {recv_block} | {ready_wake} | {buffer_hit} | {send_copy} | {recv_copy} |".format(
                    test=summary["test"],
                    runtime=summary["runtime"],
                    workers=summary["workers"],
                    size_label=summary["size_label"],
                    send_no_ext=fmt_num(values["vm_fast_path_chan_send_fast_no_ext"]),
                    recv_no_ext=fmt_num(values["vm_fast_path_chan_recv_fast_no_ext"]),
                    await_done=fmt_num(values["vm_fast_path_await_done_fast"]),
                    select_probe=fmt_num(values["vm_fast_path_select_probe_hit"]),
                    steal_success=fmt_num(values["steal_success_ratio"]),
                    spawn_shared=fmt_num(values["spawn_shared_ratio"]),
                    spawn_burst=fmt_num(values["spawn_burst_share_ratio"]),
                    steal_items=fmt_num(values["steal_items_per_success"]),
                    batch_empty=fmt_num(values["steal_batch_empty"]),
                    batch_fail=fmt_num(values["steal_batch_fail_ratio"]),
                    steal_fresh=fmt_num(values["steal_fresh_reject"]),
                    steal_wait=fmt_num(values["steal_throttle_wait_ms"]),
                    inject_spill=fmt_num(values["inject_spill"]),
                    inject_pull=fmt_num(values["inject_diagnostics_worker_pull"]),
                    send_block=fmt_num(values["channel_ops_send_block"]),
                    recv_block=fmt_num(values["channel_ops_recv_block"]),
                    ready_wake=fmt_num(values["channel_waitq_ready_wake"]),
                    buffer_hit=fmt_num(values["channel_buffered_fast_path_hit_rate"]),
                    send_copy=fmt_num(values["channel_ownership_send_deep_copy"]),
                    recv_copy=fmt_num(values["channel_ownership_recv_deep_copy"]),
                )
            )
        result_group_rows = [
            summary
            for summary in metric_rows
            if any(
                ((summary.get("sched_metrics") or {}).get(key) or {}).get("median")
                for key in (
                    "result_group_add",
                    "result_group_flush",
                    "result_group_recv",
                    "result_group_block",
                    "result_group_wake",
                )
            )
        ]
        if result_group_rows:
            result_group_metrics = (
                "result_group_add",
                "result_group_flush",
                "result_group_flush_items",
                "result_group_items_per_flush",
                "result_group_recv",
                "result_group_recv_empty",
                "result_group_block",
                "result_group_wake",
                "result_group_close",
                "result_group_close_wake",
            )
            lines.append("")
            lines.append("## ResultGroup Metrics")
            lines.append("")
            lines.append("| test | runtime | workers | size | bench args | add | flush | flush items | items/flush | recv | recv empty | block | wake | close | close wake |")
            lines.append("|------|---------|---------|------|------------|-----|-------|-------------|-------------|------|------------|-------|------|-------|------------|")
            for summary in result_group_rows:
                metrics = summary.get("sched_metrics") or {}
                values = {
                    key: (metrics.get(key) or {}).get("median")
                    for key in result_group_metrics
                }
                lines.append(
                    "| {test} | {runtime} | {workers} | {size_label} | {bench_args} | {add} | {flush} | {flush_items} | {items_per_flush} | {recv} | {recv_empty} | {block} | {wake} | {close} | {close_wake} |".format(
                        test=summary["test"],
                        runtime=summary["runtime"],
                        workers=summary["workers"],
                        size_label=summary["size_label"],
                        bench_args=fmt_args(summary.get("args")),
                        add=fmt_num(values["result_group_add"]),
                        flush=fmt_num(values["result_group_flush"]),
                        flush_items=fmt_num(values["result_group_flush_items"]),
                        items_per_flush=fmt_num(values["result_group_items_per_flush"]),
                        recv=fmt_num(values["result_group_recv"]),
                        recv_empty=fmt_num(values["result_group_recv_empty"]),
                        block=fmt_num(values["result_group_block"]),
                        wake=fmt_num(values["result_group_wake"]),
                        close=fmt_num(values["result_group_close"]),
                        close_wake=fmt_num(values["result_group_close_wake"]),
                    )
                )
        channel_shape_metrics = (
            "channel_logical_ops_generic",
            "channel_logical_ops_rendezvous",
            "channel_logical_ops_spsc",
            "channel_logical_ops_mpsc",
            "channel_logical_ops_work_queue",
            "channel_logical_ops_mpmc",
            "channel_worker_shape_ops_generic",
            "channel_worker_shape_ops_rendezvous",
            "channel_worker_shape_ops_spsc",
            "channel_worker_shape_ops_mpsc",
            "channel_worker_shape_ops_work_queue",
            "channel_worker_shape_ops_mpmc",
            "channel_lock_contention",
            "channel_buffered_fast_path_hit_rate",
        )
        lines.append("")
        lines.append("## Channel Shape Metrics")
        lines.append("")
        lines.append("| test | runtime | workers | size | bench args | logical generic | logical rendezvous | logical spsc | logical mpsc | logical work queue | logical mpmc | worker generic | worker rendezvous | worker spsc | worker mpsc | worker work queue | worker mpmc | lock contention % | buffered hit % |")
        lines.append("|------|---------|---------|------|------------|-----------------|--------------------|--------------|--------------|--------------------|--------------|----------------|-------------------|-------------|-------------|-------------------|-------------|-------------------|----------------|")
        for summary in metric_rows:
            metrics = summary.get("sched_metrics") or {}
            values = {
                key: (metrics.get(key) or {}).get("median")
                for key in channel_shape_metrics
            }
            lines.append(
                "| {test} | {runtime} | {workers} | {size_label} | {bench_args} | {logical_generic} | {logical_rendezvous} | {logical_spsc} | {logical_mpsc} | {logical_work_queue} | {logical_mpmc} | {worker_generic} | {worker_rendezvous} | {worker_spsc} | {worker_mpsc} | {worker_work_queue} | {worker_mpmc} | {lock_contention} | {buffer_hit} |".format(
                    test=summary["test"],
                    runtime=summary["runtime"],
                    workers=summary["workers"],
                    size_label=summary["size_label"],
                    bench_args=fmt_args(summary.get("args")),
                    logical_generic=fmt_num(values["channel_logical_ops_generic"]),
                    logical_rendezvous=fmt_num(values["channel_logical_ops_rendezvous"]),
                    logical_spsc=fmt_num(values["channel_logical_ops_spsc"]),
                    logical_mpsc=fmt_num(values["channel_logical_ops_mpsc"]),
                    logical_work_queue=fmt_num(values["channel_logical_ops_work_queue"]),
                    logical_mpmc=fmt_num(values["channel_logical_ops_mpmc"]),
                    worker_generic=fmt_num(values["channel_worker_shape_ops_generic"]),
                    worker_rendezvous=fmt_num(values["channel_worker_shape_ops_rendezvous"]),
                    worker_spsc=fmt_num(values["channel_worker_shape_ops_spsc"]),
                    worker_mpsc=fmt_num(values["channel_worker_shape_ops_mpsc"]),
                    worker_work_queue=fmt_num(values["channel_worker_shape_ops_work_queue"]),
                    worker_mpmc=fmt_num(values["channel_worker_shape_ops_mpmc"]),
                    lock_contention=fmt_num(values["channel_lock_contention"]),
                    buffer_hit=fmt_num(values["channel_buffered_fast_path_hit_rate"]),
                )
            )
        channel_direction_metrics = (
            "channel_logical_send_ops_generic",
            "channel_logical_recv_ops_generic",
            "channel_logical_send_ops_spsc",
            "channel_logical_recv_ops_spsc",
            "channel_logical_send_ops_mpsc",
            "channel_logical_recv_ops_mpsc",
            "channel_logical_send_ops_work_queue",
            "channel_logical_recv_ops_work_queue",
            "channel_logical_send_ops_mpmc",
            "channel_logical_recv_ops_mpmc",
        )
        lines.append("")
        lines.append("## Channel Direction Metrics")
        lines.append("")
        lines.append("| test | runtime | workers | size | bench args | generic send | generic recv | spsc send | spsc recv | mpsc send | mpsc recv | work queue send | work queue recv | mpmc send | mpmc recv |")
        lines.append("|------|---------|---------|------|------------|--------------|--------------|-----------|-----------|-----------|-----------|-----------------|-----------------|-----------|-----------|")
        for summary in metric_rows:
            metrics = summary.get("sched_metrics") or {}
            values = {
                key: (metrics.get(key) or {}).get("median")
                for key in channel_direction_metrics
            }
            lines.append(
                "| {test} | {runtime} | {workers} | {size_label} | {bench_args} | {generic_send} | {generic_recv} | {spsc_send} | {spsc_recv} | {mpsc_send} | {mpsc_recv} | {work_queue_send} | {work_queue_recv} | {mpmc_send} | {mpmc_recv} |".format(
                    test=summary["test"],
                    runtime=summary["runtime"],
                    workers=summary["workers"],
                    size_label=summary["size_label"],
                    bench_args=fmt_args(summary.get("args")),
                    generic_send=fmt_num(values["channel_logical_send_ops_generic"]),
                    generic_recv=fmt_num(values["channel_logical_recv_ops_generic"]),
                    spsc_send=fmt_num(values["channel_logical_send_ops_spsc"]),
                    spsc_recv=fmt_num(values["channel_logical_recv_ops_spsc"]),
                    mpsc_send=fmt_num(values["channel_logical_send_ops_mpsc"]),
                    mpsc_recv=fmt_num(values["channel_logical_recv_ops_mpsc"]),
                    work_queue_send=fmt_num(values["channel_logical_send_ops_work_queue"]),
                    work_queue_recv=fmt_num(values["channel_logical_recv_ops_work_queue"]),
                    mpmc_send=fmt_num(values["channel_logical_send_ops_mpmc"]),
                    mpmc_recv=fmt_num(values["channel_logical_recv_ops_mpmc"]),
                )
            )
        channel_ready_wake_metrics = (
            "channel_ready_wake_send_waiters_generic",
            "channel_ready_wake_recv_waiters_generic",
            "channel_ready_wake_send_waiters_rendezvous",
            "channel_ready_wake_recv_waiters_rendezvous",
            "channel_ready_wake_send_waiters_spsc",
            "channel_ready_wake_recv_waiters_spsc",
            "channel_ready_wake_send_waiters_mpsc",
            "channel_ready_wake_recv_waiters_mpsc",
            "channel_ready_wake_send_waiters_work_queue",
            "channel_ready_wake_recv_waiters_work_queue",
            "channel_ready_wake_send_waiters_mpmc",
            "channel_ready_wake_recv_waiters_mpmc",
        )
        channel_block_metrics = (
            "channel_block_send_waiters_generic",
            "channel_block_recv_waiters_generic",
            "channel_block_send_waiters_rendezvous",
            "channel_block_recv_waiters_rendezvous",
            "channel_block_send_waiters_spsc",
            "channel_block_recv_waiters_spsc",
            "channel_block_send_waiters_mpsc",
            "channel_block_recv_waiters_mpsc",
            "channel_block_send_waiters_work_queue",
            "channel_block_recv_waiters_work_queue",
            "channel_block_send_waiters_mpmc",
            "channel_block_recv_waiters_mpmc",
        )
        lines.append("")
        lines.append("## Channel Block Metrics")
        lines.append("")
        lines.append("| test | runtime | workers | size | bench args | generic send waiter | generic recv waiter | rendezvous send waiter | rendezvous recv waiter | spsc send waiter | spsc recv waiter | mpsc send waiter | mpsc recv waiter | work queue send waiter | work queue recv waiter | mpmc send waiter | mpmc recv waiter |")
        lines.append("|------|---------|---------|------|------------|---------------------|---------------------|------------------------|------------------------|------------------|------------------|------------------|------------------|------------------------|------------------------|------------------|------------------|")
        for summary in metric_rows:
            metrics = summary.get("sched_metrics") or {}
            values = {
                key: (metrics.get(key) or {}).get("median")
                for key in channel_block_metrics
            }
            lines.append(
                "| {test} | {runtime} | {workers} | {size_label} | {bench_args} | {generic_send} | {generic_recv} | {rendezvous_send} | {rendezvous_recv} | {spsc_send} | {spsc_recv} | {mpsc_send} | {mpsc_recv} | {work_queue_send} | {work_queue_recv} | {mpmc_send} | {mpmc_recv} |".format(
                    test=summary["test"],
                    runtime=summary["runtime"],
                    workers=summary["workers"],
                    size_label=summary["size_label"],
                    bench_args=fmt_args(summary.get("args")),
                    generic_send=fmt_num(values["channel_block_send_waiters_generic"]),
                    generic_recv=fmt_num(values["channel_block_recv_waiters_generic"]),
                    rendezvous_send=fmt_num(
                        values["channel_block_send_waiters_rendezvous"]
                    ),
                    rendezvous_recv=fmt_num(
                        values["channel_block_recv_waiters_rendezvous"]
                    ),
                    spsc_send=fmt_num(values["channel_block_send_waiters_spsc"]),
                    spsc_recv=fmt_num(values["channel_block_recv_waiters_spsc"]),
                    mpsc_send=fmt_num(values["channel_block_send_waiters_mpsc"]),
                    mpsc_recv=fmt_num(values["channel_block_recv_waiters_mpsc"]),
                    work_queue_send=fmt_num(
                        values["channel_block_send_waiters_work_queue"]
                    ),
                    work_queue_recv=fmt_num(
                        values["channel_block_recv_waiters_work_queue"]
                    ),
                    mpmc_send=fmt_num(values["channel_block_send_waiters_mpmc"]),
                    mpmc_recv=fmt_num(values["channel_block_recv_waiters_mpmc"]),
                )
            )
        lines.append("")
        lines.append("## Channel Ready Wake Metrics")
        lines.append("")
        lines.append("| test | runtime | workers | size | bench args | generic send waiter | generic recv waiter | rendezvous send waiter | rendezvous recv waiter | spsc send waiter | spsc recv waiter | mpsc send waiter | mpsc recv waiter | work queue send waiter | work queue recv waiter | mpmc send waiter | mpmc recv waiter |")
        lines.append("|------|---------|---------|------|------------|---------------------|---------------------|------------------------|------------------------|------------------|------------------|------------------|------------------|------------------------|------------------------|------------------|------------------|")
        for summary in metric_rows:
            metrics = summary.get("sched_metrics") or {}
            values = {
                key: (metrics.get(key) or {}).get("median")
                for key in channel_ready_wake_metrics
            }
            lines.append(
                "| {test} | {runtime} | {workers} | {size_label} | {bench_args} | {generic_send} | {generic_recv} | {rendezvous_send} | {rendezvous_recv} | {spsc_send} | {spsc_recv} | {mpsc_send} | {mpsc_recv} | {work_queue_send} | {work_queue_recv} | {mpmc_send} | {mpmc_recv} |".format(
                    test=summary["test"],
                    runtime=summary["runtime"],
                    workers=summary["workers"],
                    size_label=summary["size_label"],
                    bench_args=fmt_args(summary.get("args")),
                    generic_send=fmt_num(values["channel_ready_wake_send_waiters_generic"]),
                    generic_recv=fmt_num(values["channel_ready_wake_recv_waiters_generic"]),
                    rendezvous_send=fmt_num(
                        values["channel_ready_wake_send_waiters_rendezvous"]
                    ),
                    rendezvous_recv=fmt_num(
                        values["channel_ready_wake_recv_waiters_rendezvous"]
                    ),
                    spsc_send=fmt_num(values["channel_ready_wake_send_waiters_spsc"]),
                    spsc_recv=fmt_num(values["channel_ready_wake_recv_waiters_spsc"]),
                    mpsc_send=fmt_num(values["channel_ready_wake_send_waiters_mpsc"]),
                    mpsc_recv=fmt_num(values["channel_ready_wake_recv_waiters_mpsc"]),
                    work_queue_send=fmt_num(
                        values["channel_ready_wake_send_waiters_work_queue"]
                    ),
                    work_queue_recv=fmt_num(
                        values["channel_ready_wake_recv_waiters_work_queue"]
                    ),
                    mpmc_send=fmt_num(values["channel_ready_wake_send_waiters_mpmc"]),
                    mpmc_recv=fmt_num(values["channel_ready_wake_recv_waiters_mpmc"]),
                )
            )
        channel_retarget_metrics = (
            "channel_ready_wake_retarget_send_waiters_generic",
            "channel_ready_wake_retarget_recv_waiters_generic",
            "channel_ready_wake_retarget_send_waiters_rendezvous",
            "channel_ready_wake_retarget_recv_waiters_rendezvous",
            "channel_ready_wake_retarget_send_waiters_spsc",
            "channel_ready_wake_retarget_recv_waiters_spsc",
            "channel_ready_wake_retarget_send_waiters_mpsc",
            "channel_ready_wake_retarget_recv_waiters_mpsc",
            "channel_ready_wake_retarget_send_waiters_work_queue",
            "channel_ready_wake_retarget_recv_waiters_work_queue",
            "channel_ready_wake_retarget_send_waiters_mpmc",
            "channel_ready_wake_retarget_recv_waiters_mpmc",
        )
        lines.append("")
        lines.append("## Channel Ready Wake Retarget Metrics")
        lines.append("")
        lines.append("| test | runtime | workers | size | bench args | generic send waiter | generic recv waiter | rendezvous send waiter | rendezvous recv waiter | spsc send waiter | spsc recv waiter | mpsc send waiter | mpsc recv waiter | work queue send waiter | work queue recv waiter | mpmc send waiter | mpmc recv waiter |")
        lines.append("|------|---------|---------|------|------------|---------------------|---------------------|------------------------|------------------------|------------------|------------------|------------------|------------------|------------------------|------------------------|------------------|------------------|")
        for summary in metric_rows:
            metrics = summary.get("sched_metrics") or {}
            values = {
                key: (metrics.get(key) or {}).get("median")
                for key in channel_retarget_metrics
            }
            lines.append(
                "| {test} | {runtime} | {workers} | {size_label} | {bench_args} | {generic_send} | {generic_recv} | {rendezvous_send} | {rendezvous_recv} | {spsc_send} | {spsc_recv} | {mpsc_send} | {mpsc_recv} | {work_queue_send} | {work_queue_recv} | {mpmc_send} | {mpmc_recv} |".format(
                    test=summary["test"],
                    runtime=summary["runtime"],
                    workers=summary["workers"],
                    size_label=summary["size_label"],
                    bench_args=fmt_args(summary.get("args")),
                    generic_send=fmt_num(
                        values["channel_ready_wake_retarget_send_waiters_generic"]
                    ),
                    generic_recv=fmt_num(
                        values["channel_ready_wake_retarget_recv_waiters_generic"]
                    ),
                    rendezvous_send=fmt_num(
                        values["channel_ready_wake_retarget_send_waiters_rendezvous"]
                    ),
                    rendezvous_recv=fmt_num(
                        values["channel_ready_wake_retarget_recv_waiters_rendezvous"]
                    ),
                    spsc_send=fmt_num(values["channel_ready_wake_retarget_send_waiters_spsc"]),
                    spsc_recv=fmt_num(values["channel_ready_wake_retarget_recv_waiters_spsc"]),
                    mpsc_send=fmt_num(values["channel_ready_wake_retarget_send_waiters_mpsc"]),
                    mpsc_recv=fmt_num(values["channel_ready_wake_retarget_recv_waiters_mpsc"]),
                    work_queue_send=fmt_num(
                        values["channel_ready_wake_retarget_send_waiters_work_queue"]
                    ),
                    work_queue_recv=fmt_num(
                        values["channel_ready_wake_retarget_recv_waiters_work_queue"]
                    ),
                    mpmc_send=fmt_num(values["channel_ready_wake_retarget_send_waiters_mpmc"]),
                    mpmc_recv=fmt_num(values["channel_ready_wake_retarget_recv_waiters_mpmc"]),
                )
            )
    teardown_rows = [
        summary
        for summary in payload["summaries"]
        if summary["runtime"].startswith("xray")
        and any(
            key.startswith(("runtime_teardown_", "multicore_teardown_", "isolate_teardown_"))
            for key in (summary.get("sched_metrics") or {}).keys()
        )
    ]
    if teardown_rows:
        teardown_metrics = (
            "multicore_teardown_total_ms",
            "multicore_teardown_stop_ms",
            "runtime_teardown_total_ms",
            "runtime_teardown_stop_ms",
            "runtime_teardown_stats_print_ms",
            "runtime_teardown_task_defer_ms",
            "runtime_teardown_deferred_tasks",
            "runtime_teardown_worker_destroy_ms",
            "isolate_teardown_total_ms",
            "isolate_teardown_runtime_ms",
            "isolate_teardown_main_coro_ms",
            "isolate_teardown_coro_storage_ms",
            "isolate_teardown_gc_cleanup_ms",
            "isolate_teardown_deferred_tasks_ms",
        )
        lines.append("")
        lines.append("## Teardown Metrics")
        lines.append("")
        lines.append("| test | runtime | workers | size | multicore total ms | multicore stop ms | runtime total ms | runtime stop ms | stats print ms | task defer ms | deferred tasks | worker destroy ms | isolate total ms | isolate runtime ms | main coro ms | coro storage ms | gc cleanup ms | deferred task free ms |")
        lines.append("|------|---------|---------|------|--------------------|-------------------|------------------|-----------------|----------------|---------------|----------------|-------------------|------------------|--------------------|--------------|-----------------|---------------|-----------------------|")
        for summary in teardown_rows:
            metrics = summary.get("sched_metrics") or {}
            values = {key: (metrics.get(key) or {}).get("median") for key in teardown_metrics}
            lines.append(
                "| {test} | {runtime} | {workers} | {size_label} | {multi_total} | {multi_stop} | {rt_total} | {rt_stop} | {stats_print} | {task_defer} | {deferred_tasks} | {worker_destroy} | {iso_total} | {iso_runtime} | {main_coro} | {coro_storage} | {gc_cleanup} | {deferred_free} |".format(
                    test=summary["test"],
                    runtime=summary["runtime"],
                    workers=summary["workers"],
                    size_label=summary["size_label"],
                    multi_total=fmt_num(values["multicore_teardown_total_ms"]),
                    multi_stop=fmt_num(values["multicore_teardown_stop_ms"]),
                    rt_total=fmt_num(values["runtime_teardown_total_ms"]),
                    rt_stop=fmt_num(values["runtime_teardown_stop_ms"]),
                    stats_print=fmt_num(values["runtime_teardown_stats_print_ms"]),
                    task_defer=fmt_num(values["runtime_teardown_task_defer_ms"]),
                    deferred_tasks=fmt_num(values["runtime_teardown_deferred_tasks"]),
                    worker_destroy=fmt_num(values["runtime_teardown_worker_destroy_ms"]),
                    iso_total=fmt_num(values["isolate_teardown_total_ms"]),
                    iso_runtime=fmt_num(values["isolate_teardown_runtime_ms"]),
                    main_coro=fmt_num(values["isolate_teardown_main_coro_ms"]),
                    coro_storage=fmt_num(values["isolate_teardown_coro_storage_ms"]),
                    gc_cleanup=fmt_num(values["isolate_teardown_gc_cleanup_ms"]),
                    deferred_free=fmt_num(values["isolate_teardown_deferred_tasks_ms"]),
                )
            )
    if invalid:
        lines.append("")
        lines.append("## Invalid Runs")
        lines.append("")
        for item in invalid[:50]:
            lines.append(f"- `{item['test']}` `{item['runtime']}` workers={item['workers']} repeat={item['repeat']} status={item['status']}: {item.get('invalid_reason', '')}")
    if cleanup:
        lines.append("")
        lines.append("## Cleanup Tail")
        lines.append("")
        for item in cleanup[:50]:
            ratio = item.get("wall_report_ratio")
            ratio_text = f"{ratio:.3f}" if isinstance(ratio, (int, float)) else "n/a"
            lines.append(f"- `{item['test']}` `{item['runtime']}` workers={item['workers']} repeat={item['repeat']} wall/report={ratio_text}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def fmt_num(value: Any) -> str:
    if value is None:
        return ""
    if isinstance(value, float):
        return f"{value:.3f}"
    return str(value)


def fmt_args(value: Any) -> str:
    if not value:
        return ""
    if isinstance(value, (list, tuple)):
        return " ".join(str(item) for item in value)
    return str(value)


def main() -> int:
    args = parse_args()
    runtimes = selected_runtimes(args)
    tests = selected_tests(args)
    workers = parse_csv_ints(args.workers, default=default_workers())
    xray_bin = Path(args.xray_bin)
    if any(runtime in XRAY_RUNTIMES for runtime in runtimes) and not xray_bin.exists():
        print(f"error: xray executable does not exist: {xray_bin}", file=sys.stderr)
        return 2

    results: list[dict[str, Any]] = []
    tmp_root = Path(tempfile.mkdtemp(prefix="xray-coro-gate-"))
    try:
        prepared: dict[tuple[str, str], PreparedRuntime] = {}
        for test in tests:
            for runtime in runtimes:
                prepared[(test, runtime)] = prepare_runtime(runtime, test, tmp_root, xray_bin)

        warmed: set[tuple[str, str]] = set()
        for test in tests:
            for size_label, bench_args in matrix_args(args, test):
                for workers_value in workers:
                    for runtime in runtimes:
                        prep = prepared[(test, runtime)]
                        for repeat in range(1, args.repeats + 1):
                            target = RunTarget(test, runtime, workers_value, size_label, bench_args, repeat)
                            if prep.prepare_status != "ok":
                                results.append(skipped_result(target, prep, args))
                                continue
                            warm_key = (test, runtime)
                            if warm_key not in warmed:
                                warmup_prepared_runtime(prep, bench_args, workers_value, args.timeout)
                                warmed.add(warm_key)
                            print(f"[{runtime}] {test} workers={workers_value} size={size_label} repeat={repeat}/{args.repeats}", flush=True)
                            result = run_target(target, prep, args)
                            results.append(result)
    finally:
        shutil.rmtree(tmp_root, ignore_errors=True)

    summaries = summarize_results(results)
    payload = {
        "schema": "xray.coro_scaling_gate.v1",
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "project_dir": str(PROJECT_DIR),
        "xray_bin": str(xray_bin),
        "runtimes": runtimes,
        "tests": tests,
        "workers": workers,
        "repeats": args.repeats,
        "cardinality": args.cardinality.split(",") if args.cardinality else None,
        "custom_args": args.args,
        "sched_stats": args.sched_stats,
        "include_aot_correctness": args.include_aot_correctness,
        "cleanup_tail_threshold": args.cleanup_tail_threshold,
        "cleanup_tail_min_ms": args.cleanup_tail_min_ms,
        "results": results,
        "summaries": summaries,
        "vm_go_comparisons": summarize_vm_go_comparisons(summaries),
    }
    json_path = Path(args.json)
    json_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if args.markdown:
        write_markdown(Path(args.markdown), payload)

    bad = [
        item
        for item in results
        if item.get("performance_blocking", True)
        and (item.get("valid") is False or item.get("cleanup_tail") is True)
    ]
    skipped_build_fail = [
        item
        for item in results
        if item.get("performance_blocking", True) and str(item.get("status", "")).endswith("fail")
    ]
    appendix_bad = [
        item
        for item in results
        if not item.get("performance_blocking", True)
        and (item.get("valid") is False or item.get("cleanup_tail") is True)
    ]
    print(f"JSON: {json_path}")
    if args.markdown:
        print(f"Markdown: {args.markdown}")
    print(
        f"runs={len(results)} bad={len(bad)} build_fail={len(skipped_build_fail)} "
        f"aot_appendix_bad={len(appendix_bad)}"
    )
    return 1 if bad or skipped_build_fail else 0


def is_correctness_appendix(target: RunTarget, args: argparse.Namespace) -> bool:
    if target.runtime != "xray-aot":
        return False
    if args.aot_only or args.all_backends:
        return False
    return bool(args.include_aot_correctness)


def run_target(target: RunTarget, prep: PreparedRuntime, args: argparse.Namespace) -> dict[str, Any]:
    cmd = command_for(prep, target.args)
    exit_code, wall_ms, rss_peak_bytes, stdout, stderr, time_text = run_measured(
        cmd,
        env_for(target.runtime, target.workers, args.sched_stats),
        args.timeout,
    )
    parsed = parse_output(
        target.test,
        stdout,
        stderr,
        wall_ms,
        args.cleanup_tail_threshold,
        args.cleanup_tail_min_ms,
    )
    combined = stdout + "\n" + stderr
    jit_metrics = parse_jit_metrics(combined) if target.runtime == "xray-jit" else {}
    jit_compile_ms = jit_metrics.get("compile_ms") if target.runtime == "xray-jit" else None
    runtime_ms = runtime_time_ms(target.runtime, parsed["reported_time_ms"], jit_metrics)
    status = "ok" if exit_code == 0 else ("timeout" if exit_code == 124 else "fail")
    valid = status == "ok" and parsed["correctness"] is True and parsed["completed_ops"] == parsed["expected_ops"]
    invalid_reason = ""
    if status != "ok":
        invalid_reason = f"exit_code={exit_code}"
    elif parsed["correctness"] is not True:
        invalid_reason = "correctness check failed"
    elif parsed["completed_ops"] != parsed["expected_ops"]:
        invalid_reason = f"completed_ops={parsed['completed_ops']} expected_ops={parsed['expected_ops']}"
    correctness_appendix = is_correctness_appendix(target, args)
    return {
        "test": target.test,
        "runtime": target.runtime,
        "workers": target.workers,
        "size_label": target.size_label,
        "args": list(target.args),
        "repeat": target.repeat,
        "status": status,
        "exit_code": exit_code,
        "wall_time_ms": wall_ms,
        "wall_ms": wall_ms,
        "runtime_time_ms": runtime_ms,
        "jit_compile_ms": jit_compile_ms,
        "jit_metrics": jit_metrics,
        "rss_peak_bytes": rss_peak_bytes,
        "valid": valid,
        "invalid_reason": invalid_reason,
        "performance_blocking": not correctness_appendix,
        "correctness_appendix": correctness_appendix,
        "time_tool_summary": summarize(time_text, max_lines=8),
        **parsed,
    }


def skipped_result(target: RunTarget, prep: PreparedRuntime, args: argparse.Namespace) -> dict[str, Any]:
    correctness_appendix = is_correctness_appendix(target, args)
    return {
        "test": target.test,
        "runtime": target.runtime,
        "workers": target.workers,
        "size_label": target.size_label,
        "args": list(target.args),
        "repeat": target.repeat,
        "status": prep.prepare_status,
        "exit_code": 125 if prep.prepare_status == "build-fail" else 0,
        "wall_time_ms": None,
        "wall_ms": None,
        "runtime_time_ms": None,
        "jit_compile_ms": None,
        "jit_metrics": {},
        "rss_peak_bytes": None,
        "reported_time_ms": None,
        "throughput": None,
        "completed_ops": None,
        "expected_ops": None,
        "checksum": None,
        "correctness": None,
        "wall_report_ratio": None,
        "cleanup_tail": False,
        "sched_metrics": {},
        "stdout_summary": "",
        "stderr_summary": prep.prepare_stderr,
        "valid": None if prep.prepare_status.startswith("skipped") else False,
        "invalid_reason": prep.prepare_status,
        "performance_blocking": not correctness_appendix,
        "correctness_appendix": correctness_appendix,
    }


if __name__ == "__main__":
    raise SystemExit(main())

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
    "priority_latency",
]

SCHED_METRIC_PREFIXES = (
    "Dispatch mix:",
    "Steal:",
    "Runnable wait:",
    "LIFO gate:",
    "Fast dispatch:",
    "Channel wake commands:",
    "Channel wake diagnostics:",
    "Channel hot path:",
    "Channel logical shape transitions:",
    "Channel worker shape transitions:",
    "Channel logical ops:",
    "Channel worker-shape ops:",
    "Channel lock:",
    "Channel lock wait:",
    "Channel buffered fast path:",
    "Channel ops:",
    "Channel waitq:",
    "Channel close fanout:",
    "WorkQueue:",
    "Timeout:",
    "Timer:",
    "Timer cancel queue:",
    "Handoff:",
    "Inject:",
    "Inject diagnostics:",
    "Async:",
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
    runtime.add_argument("--go", action="store_true", help="Run only Go benchmarks.")
    runtime.add_argument("--aot-only", action="store_true", help="Run only Xray AOT benchmarks.")
    runtime.add_argument("--all", action="store_true", help="Run Xray VM and Go benchmarks.")
    runtime.add_argument("--all-backends", action="store_true", help="Run Xray VM, Xray AOT, and Go benchmarks.")
    parser.add_argument("--tests", help="Comma-separated benchmark names.")
    parser.add_argument("--repeats", type=int, default=5, help="Run count for each matrix point.")
    parser.add_argument("--workers", help="Comma-separated workers/procs list. Defaults to 1,2,4,8,16,physical.")
    parser.add_argument("--cardinality", help="Comma-separated N values, such as 1000,10000,100000,1000000.")
    parser.add_argument("--args", help="Space-separated benchmark args. Overrides cardinality mapping.")
    parser.add_argument("--json", required=True, help="Machine-readable JSON output path.")
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
    if args.repeats < 1:
        parser.error("--repeats must be >= 1")
    return args


def selected_runtimes(args: argparse.Namespace) -> list[str]:
    if args.go:
        return ["go"]
    if args.aot_only:
        return ["xray-aot"]
    if args.all:
        return ["xray", "go"]
    if args.all_backends:
        return ["xray", "xray-aot", "go"]
    return ["xray"]


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
    if test in {"fanout", "work_pool", "work_pool_queue"}:
        workers = min(100, max(1, int(math.sqrt(n))))
        return (str(workers), str(n))
    if test == "producer_consumer":
        producers = 4
        consumers = 4
        per_producer = max(1, n // producers)
        return (str(producers), str(consumers), str(per_producer))
    if test == "parallel_sum":
        workers = min(16, max(1, physical_core_count()))
        return (str(workers), str(n))
    if test == "pipeline":
        return ("5", str(n))
    if test == "select_multiplex":
        return (str(max(1, n // 3)),)
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
    if test == "priority_latency":
        low_tasks = min(max(1, n), 10000)
        probes = min(100, max(1, n // 100))
        return (str(low_tasks), str(probes), "100")
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
    if runtime == "xray":
        source = test_dir / f"{test}.xr"
        if not source.exists():
            return PreparedRuntime(runtime, None, False, "skipped-missing-source")
        return PreparedRuntime(runtime, [str(xray_bin), "run", str(source), "--"], True)
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
    if prepared.runtime == "xray":
        return prepared.command_prefix + list(bench_args)
    return prepared.command_prefix + list(bench_args)


def env_for(runtime: str, workers: int, sched_stats: bool) -> dict[str, str]:
    env = dict(os.environ)
    if runtime in {"xray", "xray-aot"}:
        env["XRAY_WORKERS"] = str(workers)
        if sched_stats:
            env["XRAY_SCHED_STATS"] = "1"
    elif runtime == "go":
        env["GOMAXPROCS"] = str(workers)
    return env


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


def parse_output(test: str, stdout: str, stderr: str, wall_ms: float, cleanup_threshold: float, cleanup_min_delta_ms: float) -> dict[str, Any]:
    combined = stdout + "\n" + stderr
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
        for key, value in re.findall(r"([A-Za-z_][A-Za-z0-9_]*)=(-?[0-9]+(?:\.[0-9]+)?)", line):
            metrics[f"{section}_{key}"] = float(value)
    return metrics


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
                "wall_ms": numeric_stats([item.get("wall_ms") for item in valid_items]),
                "reported_time_ms": numeric_stats([item.get("reported_time_ms") for item in valid_items]),
                "rss_peak_bytes": numeric_stats([item.get("rss_peak_bytes") for item in valid_items]),
                "throughput": numeric_stats([item.get("throughput") for item in valid_items]),
                "cleanup_tail": any(item.get("cleanup_tail") for item in items),
                "correctness": all(item.get("correctness") is True for item in items),
            }
        )
    return summaries


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
    lines.append(f"- invalid_runs: `{len(invalid)}`")
    lines.append(f"- cleanup_tail_runs: `{len(cleanup)}`")
    lines.append("")
    lines.append("| test | runtime | workers | size | valid | reported median ms | wall median ms | rss median bytes | cleanup-tail |")
    lines.append("|------|---------|---------|------|-------|--------------------|----------------|------------------|--------------|")
    for summary in payload["summaries"]:
        lines.append(
            "| {test} | {runtime} | {workers} | {size_label} | {valid}/{repeats} | {reported} | {wall} | {rss} | {tail} |".format(
                test=summary["test"],
                runtime=summary["runtime"],
                workers=summary["workers"],
                size_label=summary["size_label"],
                valid=summary["valid_repeats"],
                repeats=summary["repeats"],
                reported=fmt_num(summary["reported_time_ms"]["median"]),
                wall=fmt_num(summary["wall_ms"]["median"]),
                rss=fmt_num(summary["rss_peak_bytes"]["median"]),
                tail="yes" if summary["cleanup_tail"] else "no",
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


def main() -> int:
    args = parse_args()
    runtimes = selected_runtimes(args)
    tests = selected_tests(args)
    workers = parse_csv_ints(args.workers, default=default_workers())
    xray_bin = Path(args.xray_bin)
    if any(runtime in {"xray", "xray-aot"} for runtime in runtimes) and not xray_bin.exists():
        print(f"error: xray executable does not exist: {xray_bin}", file=sys.stderr)
        return 2

    results: list[dict[str, Any]] = []
    tmp_root = Path(tempfile.mkdtemp(prefix="xray-coro-gate-"))
    try:
        prepared: dict[tuple[str, str], PreparedRuntime] = {}
        for test in tests:
            for runtime in runtimes:
                prepared[(test, runtime)] = prepare_runtime(runtime, test, tmp_root, xray_bin)

        for test in tests:
            for size_label, bench_args in matrix_args(args, test):
                for workers_value in workers:
                    for runtime in runtimes:
                        prep = prepared[(test, runtime)]
                        for repeat in range(1, args.repeats + 1):
                            target = RunTarget(test, runtime, workers_value, size_label, bench_args, repeat)
                            if prep.prepare_status != "ok":
                                results.append(skipped_result(target, prep))
                                continue
                            print(f"[{runtime}] {test} workers={workers_value} size={size_label} repeat={repeat}/{args.repeats}", flush=True)
                            result = run_target(target, prep, args)
                            results.append(result)
    finally:
        shutil.rmtree(tmp_root, ignore_errors=True)

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
        "cleanup_tail_threshold": args.cleanup_tail_threshold,
        "cleanup_tail_min_ms": args.cleanup_tail_min_ms,
        "results": results,
        "summaries": summarize_results(results),
    }
    json_path = Path(args.json)
    json_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if args.markdown:
        write_markdown(Path(args.markdown), payload)

    bad = [
        item
        for item in results
        if item.get("valid") is False or item.get("cleanup_tail") is True
    ]
    skipped_build_fail = [
        item
        for item in results
        if str(item.get("status", "")).endswith("fail")
    ]
    print(f"JSON: {json_path}")
    if args.markdown:
        print(f"Markdown: {args.markdown}")
    print(f"runs={len(results)} bad={len(bad)} build_fail={len(skipped_build_fail)}")
    return 1 if bad or skipped_build_fail else 0


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
    status = "ok" if exit_code == 0 else ("timeout" if exit_code == 124 else "fail")
    valid = status == "ok" and parsed["correctness"] is True and parsed["completed_ops"] == parsed["expected_ops"]
    invalid_reason = ""
    if status != "ok":
        invalid_reason = f"exit_code={exit_code}"
    elif parsed["correctness"] is not True:
        invalid_reason = "correctness check failed"
    elif parsed["completed_ops"] != parsed["expected_ops"]:
        invalid_reason = f"completed_ops={parsed['completed_ops']} expected_ops={parsed['expected_ops']}"
    return {
        "test": target.test,
        "runtime": target.runtime,
        "workers": target.workers,
        "size_label": target.size_label,
        "args": list(target.args),
        "repeat": target.repeat,
        "status": status,
        "exit_code": exit_code,
        "wall_ms": wall_ms,
        "rss_peak_bytes": rss_peak_bytes,
        "valid": valid,
        "invalid_reason": invalid_reason,
        "time_tool_summary": summarize(time_text, max_lines=8),
        **parsed,
    }


def skipped_result(target: RunTarget, prep: PreparedRuntime) -> dict[str, Any]:
    return {
        "test": target.test,
        "runtime": target.runtime,
        "workers": target.workers,
        "size_label": target.size_label,
        "args": list(target.args),
        "repeat": target.repeat,
        "status": prep.prepare_status,
        "exit_code": 125 if prep.prepare_status == "build-fail" else 0,
        "wall_ms": None,
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
    }


if __name__ == "__main__":
    raise SystemExit(main())

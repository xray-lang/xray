#!/usr/bin/env python3
"""
ws_bench.py - Unified WebSocket benchmark client

Tests a WebSocket echo server with standardized workloads and produces
comparable metrics across different server implementations.

Test suite:
  1. Latency       - Single-connection round-trip time (various payload sizes)
  2. Throughput     - Single-connection max messages/sec (various payload sizes)
  3. Concurrency   - Multi-connection throughput scaling (1 to 50 connections)
  4. Large messages - Throughput for 64KB to 1MB payloads
  5. Connection churn - Rapid connect/disconnect cycles

Usage:
  python3 ws_bench.py [OPTIONS]

Options:
  --url URL       Server URL (default: ws://127.0.0.1:9001)
  --quick         Reduced iterations for fast smoke test
  --json FILE     Write raw results as JSON
  --label NAME    Label for this run (e.g. "xray", "go")

Requirements:
  pip3 install websockets
"""

import argparse
import asyncio
import json
import os
import statistics
import sys
import time

try:
    import websockets
except ImportError:
    print("ERROR: pip3 install websockets", file=sys.stderr)
    sys.exit(1)

# ─────────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────────

def fmt_size(n):
    if n >= 1048576:
        return f"{n / 1048576:.0f}MB"
    if n >= 1024:
        return f"{n / 1024:.0f}KB"
    return f"{n}B"


def fmt_rate(msg_per_sec):
    if msg_per_sec >= 1000:
        return f"{msg_per_sec / 1000:.1f}K"
    return f"{msg_per_sec:.0f}"


def make_payload(size):
    return b"X" * size


# ─────────────────────────────────────────────────────────────────────────────
# Test 1: Latency
# ─────────────────────────────────────────────────────────────────────────────

async def test_latency(url, size, count, warmup=50):
    """Measure single-connection round-trip latency."""
    payload = make_payload(size)
    latencies = []

    async with websockets.connect(url, max_size=2 ** 21, ping_interval=None) as ws:
        # warmup
        for _ in range(min(warmup, count)):
            await ws.send(payload)
            await ws.recv()

        for _ in range(count):
            t0 = time.perf_counter()
            await ws.send(payload)
            resp = await ws.recv()
            dt = (time.perf_counter() - t0) * 1000  # ms
            latencies.append(dt)
            assert len(resp) == size

    latencies.sort()
    return {
        "size": size,
        "count": count,
        "min": latencies[0],
        "avg": statistics.mean(latencies),
        "median": statistics.median(latencies),
        "p95": latencies[int(len(latencies) * 0.95)],
        "p99": latencies[int(len(latencies) * 0.99)],
        "max": latencies[-1],
    }


# ─────────────────────────────────────────────────────────────────────────────
# Test 2: Throughput (single connection)
# ─────────────────────────────────────────────────────────────────────────────

async def test_throughput(url, size, duration_sec=3.0, warmup=100):
    """Measure single-connection throughput over a fixed duration."""
    payload = make_payload(size)

    async with websockets.connect(url, max_size=2 ** 21, ping_interval=None) as ws:
        # warmup
        for _ in range(warmup):
            await ws.send(payload)
            await ws.recv()

        count = 0
        t0 = time.perf_counter()
        deadline = t0 + duration_sec
        while time.perf_counter() < deadline:
            await ws.send(payload)
            await ws.recv()
            count += 1
        elapsed = time.perf_counter() - t0

    msg_per_sec = count / elapsed
    bytes_per_sec = msg_per_sec * size
    return {
        "size": size,
        "duration": elapsed,
        "messages": count,
        "msg_per_sec": msg_per_sec,
        "mb_per_sec": bytes_per_sec / 1048576,
    }


# ─────────────────────────────────────────────────────────────────────────────
# Test 3: Concurrency
# ─────────────────────────────────────────────────────────────────────────────

async def _concurrent_worker(url, payload, count, results, wid):
    try:
        async with websockets.connect(url, max_size=2 ** 21, ping_interval=None) as ws:
            t0 = time.perf_counter()
            for _ in range(count):
                await ws.send(payload)
                await ws.recv()
            elapsed = time.perf_counter() - t0
            results[wid] = {"ok": True, "messages": count, "time": elapsed}
    except Exception as e:
        results[wid] = {"ok": False, "error": str(e)}


async def test_concurrency(url, num_conns, msg_size=128, msgs_per_conn=500):
    """Measure aggregate throughput with N concurrent connections."""
    payload = make_payload(msg_size)
    results = {}

    t0 = time.perf_counter()
    tasks = [
        _concurrent_worker(url, payload, msgs_per_conn, results, i)
        for i in range(num_conns)
    ]
    await asyncio.gather(*tasks)
    wall_time = time.perf_counter() - t0

    ok = [r for r in results.values() if r.get("ok")]
    fail = [r for r in results.values() if not r.get("ok")]
    total_msgs = sum(r["messages"] for r in ok)
    agg_msg_sec = total_msgs / wall_time if wall_time > 0 else 0

    return {
        "connections": num_conns,
        "msg_size": msg_size,
        "msgs_per_conn": msgs_per_conn,
        "total_messages": total_msgs,
        "wall_time": wall_time,
        "agg_msg_sec": agg_msg_sec,
        "agg_mb_sec": agg_msg_sec * msg_size / 1048576,
        "ok_conns": len(ok),
        "fail_conns": len(fail),
        "errors": [r.get("error") for r in fail[:3]],
    }


# ─────────────────────────────────────────────────────────────────────────────
# Test 4: Large messages
# ─────────────────────────────────────────────────────────────────────────────

async def test_large_message(url, size, count=100, warmup=10):
    """Throughput for large payloads."""
    payload = make_payload(size)

    async with websockets.connect(url, max_size=2 ** 21, ping_interval=None) as ws:
        for _ in range(warmup):
            await ws.send(payload)
            await ws.recv()

        t0 = time.perf_counter()
        for _ in range(count):
            await ws.send(payload)
            await ws.recv()
        elapsed = time.perf_counter() - t0

    msg_per_sec = count / elapsed
    return {
        "size": size,
        "count": count,
        "elapsed": elapsed,
        "msg_per_sec": msg_per_sec,
        "mb_per_sec": msg_per_sec * size / 1048576,
    }


# ─────────────────────────────────────────────────────────────────────────────
# Test 5: Connection churn
# ─────────────────────────────────────────────────────────────────────────────

async def test_connection_churn(url, cycles=200):
    """Measure connect → send → recv → close cycle time."""
    payload = b"ping"
    times = []

    for _ in range(cycles):
        t0 = time.perf_counter()
        async with websockets.connect(url, ping_interval=None) as ws:
            await ws.send(payload)
            await ws.recv()
        dt = (time.perf_counter() - t0) * 1000
        times.append(dt)

    times.sort()
    return {
        "cycles": cycles,
        "avg_ms": statistics.mean(times),
        "median_ms": statistics.median(times),
        "p95_ms": times[int(len(times) * 0.95)],
        "p99_ms": times[int(len(times) * 0.99)],
        "min_ms": times[0],
        "max_ms": times[-1],
        "conns_per_sec": 1000.0 / statistics.mean(times),
    }


# ─────────────────────────────────────────────────────────────────────────────
# Runner
# ─────────────────────────────────────────────────────────────────────────────

async def run_all(url, quick=False):
    all_results = {}

    # ── Test 1: Latency ──────────────────────────────────────────────────
    print("━━━ Test 1: Round-trip Latency (single connection) ━━━")
    print()
    sizes = [64, 256, 1024, 4096, 16384, 65536]
    count_map = {64: 1000, 256: 1000, 1024: 500, 4096: 500, 16384: 200, 65536: 100}
    if quick:
        count_map = {s: max(c // 5, 20) for s, c in count_map.items()}

    hdr = f"  {'Size':>8s}  {'Min':>8s}  {'Avg':>8s}  {'Median':>8s}  {'P95':>8s}  {'P99':>8s}  {'Max':>8s}"
    sep = f"  {'─'*8}  {'─'*8}  {'─'*8}  {'─'*8}  {'─'*8}  {'─'*8}  {'─'*8}"
    print(hdr)
    print(sep)

    lat_results = []
    for size in sizes:
        try:
            r = await test_latency(url, size, count_map[size])
            lat_results.append(r)
            print(
                f"  {fmt_size(size):>8s}"
                f"  {r['min']:>7.3f}ms"
                f"  {r['avg']:>7.3f}ms"
                f"  {r['median']:>7.3f}ms"
                f"  {r['p95']:>7.3f}ms"
                f"  {r['p99']:>7.3f}ms"
                f"  {r['max']:>7.3f}ms"
            )
        except Exception as e:
            print(f"  {fmt_size(size):>8s}  ERROR: {e}")
    all_results["latency"] = lat_results
    print()

    # ── Test 2: Throughput ───────────────────────────────────────────────
    print("━━━ Test 2: Single-Connection Throughput ━━━")
    print()
    tp_sizes = [64, 256, 1024, 4096, 16384, 65536]
    duration = 1.5 if quick else 3.0

    hdr = f"  {'Size':>8s}  {'Msgs':>8s}  {'Time':>7s}  {'Msg/s':>10s}  {'MB/s':>8s}"
    sep = f"  {'─'*8}  {'─'*8}  {'─'*7}  {'─'*10}  {'─'*8}"
    print(hdr)
    print(sep)

    tp_results = []
    for size in tp_sizes:
        try:
            r = await test_throughput(url, size, duration)
            tp_results.append(r)
            print(
                f"  {fmt_size(size):>8s}"
                f"  {r['messages']:>8d}"
                f"  {r['duration']:>6.2f}s"
                f"  {fmt_rate(r['msg_per_sec']):>10s}"
                f"  {r['mb_per_sec']:>7.2f}"
            )
        except Exception as e:
            print(f"  {fmt_size(size):>8s}  ERROR: {e}")
    all_results["throughput"] = tp_results
    print()

    # ── Test 3: Concurrency ──────────────────────────────────────────────
    print("━━━ Test 3: Concurrent Connections ━━━")
    print()
    conns_list = [1, 5, 10, 20, 50]
    msgs_per = 200 if quick else 500

    hdr = f"  {'Conns':>6s}  {'Total':>8s}  {'Time':>7s}  {'Agg Msg/s':>10s}  {'MB/s':>8s}  {'OK':>6s}"
    sep = f"  {'─'*6}  {'─'*8}  {'─'*7}  {'─'*10}  {'─'*8}  {'─'*6}"
    print(hdr)
    print(sep)

    conc_results = []
    for n in conns_list:
        try:
            r = await test_concurrency(url, n, msg_size=128, msgs_per_conn=msgs_per)
            conc_results.append(r)
            print(
                f"  {n:>6d}"
                f"  {r['total_messages']:>8d}"
                f"  {r['wall_time']:>6.2f}s"
                f"  {fmt_rate(r['agg_msg_sec']):>10s}"
                f"  {r['agg_mb_sec']:>7.2f}"
                f"  {r['ok_conns']:>3d}/{n}"
            )
            if r["errors"]:
                for err in r["errors"]:
                    print(f"         ERROR: {err}")
        except Exception as e:
            print(f"  {n:>6d}  ERROR: {e}")
    all_results["concurrency"] = conc_results
    print()

    # ── Test 4: Large Messages ───────────────────────────────────────────
    print("━━━ Test 4: Large Message Throughput ━━━")
    print()
    large_sizes = [65536, 262144, 1048576]
    large_counts = {65536: 200, 262144: 80, 1048576: 30}
    if quick:
        large_counts = {s: max(c // 4, 10) for s, c in large_counts.items()}

    hdr = f"  {'Size':>8s}  {'Count':>6s}  {'Time':>7s}  {'Msg/s':>10s}  {'MB/s':>8s}"
    sep = f"  {'─'*8}  {'─'*6}  {'─'*7}  {'─'*10}  {'─'*8}"
    print(hdr)
    print(sep)

    large_results = []
    for size in large_sizes:
        try:
            r = await test_large_message(url, size, large_counts[size])
            large_results.append(r)
            print(
                f"  {fmt_size(size):>8s}"
                f"  {r['count']:>6d}"
                f"  {r['elapsed']:>6.2f}s"
                f"  {fmt_rate(r['msg_per_sec']):>10s}"
                f"  {r['mb_per_sec']:>7.2f}"
            )
        except Exception as e:
            print(f"  {fmt_size(size):>8s}  ERROR: {e}")
    all_results["large_message"] = large_results
    print()

    # ── Test 5: Connection Churn ─────────────────────────────────────────
    print("━━━ Test 5: Connection Churn ━━━")
    print()
    cycles = 50 if quick else 200
    try:
        r = await test_connection_churn(url, cycles)
        all_results["connection_churn"] = r
        print(f"  Cycles       : {r['cycles']}")
        print(f"  Avg          : {r['avg_ms']:.2f} ms/cycle")
        print(f"  Median       : {r['median_ms']:.2f} ms")
        print(f"  P95          : {r['p95_ms']:.2f} ms")
        print(f"  P99          : {r['p99_ms']:.2f} ms")
        print(f"  Conns/sec    : {r['conns_per_sec']:.0f}")
    except Exception as e:
        print(f"  ERROR: {e}")
    print()

    return all_results


def main():
    parser = argparse.ArgumentParser(description="WebSocket Server Benchmark")
    parser.add_argument("--url", default="ws://127.0.0.1:9001", help="Server URL")
    parser.add_argument("--quick", action="store_true", help="Quick mode (fewer iterations)")
    parser.add_argument("--json", metavar="FILE", help="Save raw results as JSON")
    parser.add_argument("--label", default="", help="Label for this run")
    args = parser.parse_args()

    label = args.label or args.url
    print()
    print(f"╔══════════════════════════════════════════════════════╗")
    print(f"║          WebSocket Server Benchmark                  ║")
    print(f"╠══════════════════════════════════════════════════════╣")
    print(f"║  Target : {label:<42s} ║")
    print(f"║  URL    : {args.url:<42s} ║")
    print(f"║  Mode   : {'quick' if args.quick else 'full':<42s} ║")
    print(f"╚══════════════════════════════════════════════════════╝")
    print()

    results = asyncio.run(run_all(args.url, args.quick))
    results["_meta"] = {"label": label, "url": args.url, "quick": args.quick}

    if args.json:
        with open(args.json, "w") as f:
            json.dump(results, f, indent=2)
        print(f"Results saved to {args.json}")

    print("━━━ Done ━━━")


if __name__ == "__main__":
    main()

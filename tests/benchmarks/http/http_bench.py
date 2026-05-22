#!/usr/bin/env python3
"""
http_bench.py - Unified HTTP benchmark client

Tests an HTTP server with standardized workloads and produces
comparable metrics across different server implementations.

Test suite:
  1. Latency       - Single-connection round-trip time (plaintext, JSON)
  2. Throughput     - Single-connection keep-alive max req/s
  3. Concurrency   - Multi-connection throughput scaling (1 to 100 connections)
  4. POST echo     - Echo throughput with various body sizes
  5. Connection churn - New connection per request (no keep-alive)
  6. Large response pipeline - Sustained transfer rate

Usage:
  python3 http_bench.py [OPTIONS]

Options:
  --url URL       Server base URL (default: http://127.0.0.1:8080)
  --quick         Reduced iterations for fast smoke test
  --json FILE     Write raw results as JSON
  --label NAME    Label for this run (e.g. "xray", "go")

Requirements:
  pip3 install aiohttp
"""

import argparse
import asyncio
import json
import statistics
import sys
import time

try:
    import aiohttp
except ImportError:
    print("ERROR: pip3 install aiohttp", file=sys.stderr)
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


def fmt_rate(req_per_sec):
    if req_per_sec >= 1000:
        return f"{req_per_sec / 1000:.1f}K"
    return f"{req_per_sec:.0f}"


# ─────────────────────────────────────────────────────────────────────────────
# Test 1: Latency
# ─────────────────────────────────────────────────────────────────────────────

async def test_latency(base_url, path, count, warmup=50):
    """Measure single-connection round-trip latency with keep-alive."""
    url = base_url + path
    latencies = []

    conn = aiohttp.TCPConnector(limit=1, force_close=False)
    async with aiohttp.ClientSession(connector=conn) as session:
        # warmup
        for _ in range(min(warmup, count)):
            async with session.get(url) as resp:
                await resp.read()

        for _ in range(count):
            t0 = time.perf_counter()
            async with session.get(url) as resp:
                await resp.read()
            dt = (time.perf_counter() - t0) * 1000  # ms
            latencies.append(dt)

    latencies.sort()
    return {
        "path": path,
        "count": count,
        "min": latencies[0],
        "avg": statistics.mean(latencies),
        "median": statistics.median(latencies),
        "p95": latencies[int(len(latencies) * 0.95)],
        "p99": latencies[int(len(latencies) * 0.99)],
        "max": latencies[-1],
    }


# ─────────────────────────────────────────────────────────────────────────────
# Test 2: Throughput (single connection, keep-alive)
# ─────────────────────────────────────────────────────────────────────────────

async def test_throughput(base_url, path, duration_sec=3.0, warmup=100):
    """Measure single-connection throughput over a fixed duration."""
    url = base_url + path

    conn = aiohttp.TCPConnector(limit=1, force_close=False)
    async with aiohttp.ClientSession(connector=conn) as session:
        # warmup
        for _ in range(warmup):
            async with session.get(url) as resp:
                await resp.read()

        count = 0
        total_bytes = 0
        t0 = time.perf_counter()
        deadline = t0 + duration_sec
        while time.perf_counter() < deadline:
            async with session.get(url) as resp:
                body = await resp.read()
                total_bytes += len(body)
                count += 1
        elapsed = time.perf_counter() - t0

    req_per_sec = count / elapsed
    return {
        "path": path,
        "duration": elapsed,
        "requests": count,
        "req_per_sec": req_per_sec,
        "total_bytes": total_bytes,
        "mb_per_sec": total_bytes / elapsed / 1048576,
    }


# ─────────────────────────────────────────────────────────────────────────────
# Test 3: Concurrency
# ─────────────────────────────────────────────────────────────────────────────

async def _concurrent_worker(session, url, count, results, wid):
    try:
        t0 = time.perf_counter()
        for _ in range(count):
            async with session.get(url) as resp:
                await resp.read()
        elapsed = time.perf_counter() - t0
        results[wid] = {"ok": True, "requests": count, "time": elapsed}
    except Exception as e:
        results[wid] = {"ok": False, "error": str(e)}


async def test_concurrency(base_url, num_conns, reqs_per_conn=500):
    """Measure aggregate throughput with N concurrent connections."""
    url = base_url + "/plaintext"
    results = {}

    conn = aiohttp.TCPConnector(limit=num_conns, force_close=False)
    async with aiohttp.ClientSession(connector=conn) as session:
        t0 = time.perf_counter()
        tasks = [
            _concurrent_worker(session, url, reqs_per_conn, results, i)
            for i in range(num_conns)
        ]
        await asyncio.gather(*tasks)
        wall_time = time.perf_counter() - t0

    ok = [r for r in results.values() if r.get("ok")]
    fail = [r for r in results.values() if not r.get("ok")]
    total_reqs = sum(r["requests"] for r in ok)
    agg_req_sec = total_reqs / wall_time if wall_time > 0 else 0

    return {
        "connections": num_conns,
        "reqs_per_conn": reqs_per_conn,
        "total_requests": total_reqs,
        "wall_time": wall_time,
        "agg_req_sec": agg_req_sec,
        "ok_conns": len(ok),
        "fail_conns": len(fail),
        "errors": [r.get("error") for r in fail[:3]],
    }


# ─────────────────────────────────────────────────────────────────────────────
# Test 4: POST echo throughput
# ─────────────────────────────────────────────────────────────────────────────

async def test_echo(base_url, body_size, duration_sec=3.0, warmup=50):
    """Measure POST echo throughput with various body sizes."""
    url = base_url + "/echo"
    payload = b"X" * body_size

    conn = aiohttp.TCPConnector(limit=1, force_close=False)
    async with aiohttp.ClientSession(connector=conn) as session:
        # warmup
        for _ in range(warmup):
            async with session.post(url, data=payload) as resp:
                await resp.read()

        count = 0
        t0 = time.perf_counter()
        deadline = t0 + duration_sec
        while time.perf_counter() < deadline:
            async with session.post(url, data=payload) as resp:
                body = await resp.read()
                assert len(body) == body_size, f"echo mismatch: {len(body)} != {body_size}"
                count += 1
        elapsed = time.perf_counter() - t0

    req_per_sec = count / elapsed
    return {
        "body_size": body_size,
        "duration": elapsed,
        "requests": count,
        "req_per_sec": req_per_sec,
        "mb_per_sec": req_per_sec * body_size * 2 / 1048576,  # *2 for send+recv
    }


# ─────────────────────────────────────────────────────────────────────────────
# Test 5: Connection churn
# ─────────────────────────────────────────────────────────────────────────────

async def test_connection_churn(base_url, cycles=200):
    """Measure new-connection-per-request cycle time."""
    url = base_url + "/plaintext"
    times = []

    for _ in range(cycles):
        conn = aiohttp.TCPConnector(limit=1, force_close=True)
        t0 = time.perf_counter()
        async with aiohttp.ClientSession(connector=conn) as session:
            async with session.get(url) as resp:
                await resp.read()
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
# Test 6: Sustained pipeline (concurrent keep-alive)
# ─────────────────────────────────────────────────────────────────────────────

async def _pipeline_worker(session, url, duration, results, wid):
    try:
        count = 0
        t0 = time.perf_counter()
        deadline = t0 + duration
        while time.perf_counter() < deadline:
            async with session.get(url) as resp:
                await resp.read()
                count += 1
        elapsed = time.perf_counter() - t0
        results[wid] = {"ok": True, "requests": count, "time": elapsed}
    except Exception as e:
        results[wid] = {"ok": False, "error": str(e)}


async def test_pipeline(base_url, num_conns=10, duration_sec=5.0):
    """Sustained throughput with multiple keep-alive connections."""
    url = base_url + "/plaintext"
    results = {}

    conn = aiohttp.TCPConnector(limit=num_conns, force_close=False)
    async with aiohttp.ClientSession(connector=conn) as session:
        t0 = time.perf_counter()
        tasks = [
            _pipeline_worker(session, url, duration_sec, results, i)
            for i in range(num_conns)
        ]
        await asyncio.gather(*tasks)
        wall_time = time.perf_counter() - t0

    ok = [r for r in results.values() if r.get("ok")]
    total_reqs = sum(r["requests"] for r in ok)
    agg_req_sec = total_reqs / wall_time if wall_time > 0 else 0

    return {
        "connections": num_conns,
        "duration": wall_time,
        "total_requests": total_reqs,
        "agg_req_sec": agg_req_sec,
    }


# ─────────────────────────────────────────────────────────────────────────────
# Runner
# ─────────────────────────────────────────────────────────────────────────────

async def run_all(base_url, quick=False):
    all_results = {}

    # ── Test 1: Latency ──────────────────────────────────────────────────
    print("━━━ Test 1: Request Latency (single connection, keep-alive) ━━━")
    print()
    paths = ["/plaintext", "/json"]
    count = 200 if quick else 1000

    hdr = f"  {'Endpoint':>12s}  {'Min':>8s}  {'Avg':>8s}  {'Median':>8s}  {'P95':>8s}  {'P99':>8s}  {'Max':>8s}"
    sep = f"  {'─'*12}  {'─'*8}  {'─'*8}  {'─'*8}  {'─'*8}  {'─'*8}  {'─'*8}"
    print(hdr)
    print(sep)

    lat_results = []
    for path in paths:
        try:
            r = await test_latency(base_url, path, count)
            lat_results.append(r)
            print(
                f"  {path:>12s}"
                f"  {r['min']:>7.3f}ms"
                f"  {r['avg']:>7.3f}ms"
                f"  {r['median']:>7.3f}ms"
                f"  {r['p95']:>7.3f}ms"
                f"  {r['p99']:>7.3f}ms"
                f"  {r['max']:>7.3f}ms"
            )
        except Exception as e:
            print(f"  {path:>12s}  ERROR: {e}")
    all_results["latency"] = lat_results
    print()

    # ── Test 2: Throughput ───────────────────────────────────────────────
    print("━━━ Test 2: Single-Connection Throughput (keep-alive) ━━━")
    print()
    duration = 1.5 if quick else 3.0

    hdr = f"  {'Endpoint':>12s}  {'Reqs':>8s}  {'Time':>7s}  {'Req/s':>10s}  {'MB/s':>8s}"
    sep = f"  {'─'*12}  {'─'*8}  {'─'*7}  {'─'*10}  {'─'*8}"
    print(hdr)
    print(sep)

    tp_results = []
    for path in ["/plaintext", "/json"]:
        try:
            r = await test_throughput(base_url, path, duration)
            tp_results.append(r)
            print(
                f"  {path:>12s}"
                f"  {r['requests']:>8d}"
                f"  {r['duration']:>6.2f}s"
                f"  {fmt_rate(r['req_per_sec']):>10s}"
                f"  {r['mb_per_sec']:>7.2f}"
            )
        except Exception as e:
            print(f"  {path:>12s}  ERROR: {e}")
    all_results["throughput"] = tp_results
    print()

    # ── Test 3: Concurrency ──────────────────────────────────────────────
    print("━━━ Test 3: Concurrent Connections (/plaintext) ━━━")
    print()
    conns_list = [1, 10, 25, 50, 100]
    reqs_per = 100 if quick else 500

    hdr = f"  {'Conns':>6s}  {'Total':>8s}  {'Time':>7s}  {'Agg Req/s':>10s}  {'OK':>6s}"
    sep = f"  {'─'*6}  {'─'*8}  {'─'*7}  {'─'*10}  {'─'*6}"
    print(hdr)
    print(sep)

    conc_results = []
    for n in conns_list:
        try:
            r = await test_concurrency(base_url, n, reqs_per_conn=reqs_per)
            conc_results.append(r)
            print(
                f"  {n:>6d}"
                f"  {r['total_requests']:>8d}"
                f"  {r['wall_time']:>6.2f}s"
                f"  {fmt_rate(r['agg_req_sec']):>10s}"
                f"  {r['ok_conns']:>3d}/{n}"
            )
            if r["errors"]:
                for err in r["errors"]:
                    print(f"         ERROR: {err}")
        except Exception as e:
            print(f"  {n:>6d}  ERROR: {e}")
    all_results["concurrency"] = conc_results
    print()

    # ── Test 4: POST Echo ────────────────────────────────────────────────
    print("━━━ Test 4: POST Echo Throughput ━━━")
    print()
    echo_sizes = [64, 256, 1024, 4096, 16384]
    echo_duration = 1.5 if quick else 3.0

    hdr = f"  {'BodySize':>10s}  {'Reqs':>8s}  {'Time':>7s}  {'Req/s':>10s}  {'MB/s':>8s}"
    sep = f"  {'─'*10}  {'─'*8}  {'─'*7}  {'─'*10}  {'─'*8}"
    print(hdr)
    print(sep)

    echo_results = []
    for size in echo_sizes:
        try:
            r = await test_echo(base_url, size, echo_duration)
            echo_results.append(r)
            print(
                f"  {fmt_size(size):>10s}"
                f"  {r['requests']:>8d}"
                f"  {r['duration']:>6.2f}s"
                f"  {fmt_rate(r['req_per_sec']):>10s}"
                f"  {r['mb_per_sec']:>7.2f}"
            )
        except Exception as e:
            print(f"  {fmt_size(size):>10s}  ERROR: {e}")
    all_results["echo"] = echo_results
    print()

    # ── Test 5: Connection Churn ─────────────────────────────────────────
    print("━━━ Test 5: Connection Churn (new conn per request) ━━━")
    print()
    cycles = 50 if quick else 200
    try:
        r = await test_connection_churn(base_url, cycles)
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

    # ── Test 6: Sustained Pipeline ───────────────────────────────────────
    print("━━━ Test 6: Sustained Pipeline (multi-conn keep-alive) ━━━")
    print()
    pipe_conns = [1, 10, 50]
    pipe_duration = 2.0 if quick else 5.0

    hdr = f"  {'Conns':>6s}  {'Total':>10s}  {'Time':>7s}  {'Agg Req/s':>12s}"
    sep = f"  {'─'*6}  {'─'*10}  {'─'*7}  {'─'*12}"
    print(hdr)
    print(sep)

    pipe_results = []
    for n in pipe_conns:
        try:
            r = await test_pipeline(base_url, n, pipe_duration)
            pipe_results.append(r)
            print(
                f"  {n:>6d}"
                f"  {r['total_requests']:>10d}"
                f"  {r['duration']:>6.2f}s"
                f"  {fmt_rate(r['agg_req_sec']):>12s}"
            )
        except Exception as e:
            print(f"  {n:>6d}  ERROR: {e}")
    all_results["pipeline"] = pipe_results
    print()

    return all_results


def main():
    parser = argparse.ArgumentParser(description="HTTP Server Benchmark")
    parser.add_argument("--url", default="http://127.0.0.1:8080", help="Server base URL")
    parser.add_argument("--quick", action="store_true", help="Quick mode (fewer iterations)")
    parser.add_argument("--json", metavar="FILE", help="Save raw results as JSON")
    parser.add_argument("--label", default="", help="Label for this run")
    args = parser.parse_args()

    label = args.label or args.url
    print()
    print(f"╔══════════════════════════════════════════════════════╗")
    print(f"║            HTTP Server Benchmark                     ║")
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

#!/usr/bin/env python3
"""
TCP Echo Benchmark Client
Unified benchmark tool for TCP echo servers (xray, Go, Node.js, Python).

Tests:
  1. Latency       - Single connection, serial echo, measure RTT
  2. Throughput     - Single connection, bulk echo, measure msg/s and MB/s
  3. Concurrency   - N concurrent connections, each echo M times
  4. Connection Rate - Rapid connect -> echo -> close cycles
  5. Large Message  - Single connection, large payload transfer
  6. Message Sweep  - Various message sizes, measure scaling
"""

import argparse
import json
import os
import socket
import statistics
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed


def tcp_connect(host, port, timeout=5.0):
    """Create a TCP connection with TCP_NODELAY."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.connect((host, port))
    return sock


def tcp_echo(sock, data):
    """Send data and receive full echo response."""
    sock.sendall(data)
    received = b""
    while len(received) < len(data):
        chunk = sock.recv(65536)
        if not chunk:
            raise ConnectionError("Connection closed during echo")
        received += chunk
    return received


# ========== Test 1: Latency ==========

def test_latency(host, port, msg_size=64, iterations=1000):
    """Single connection serial echo - measure per-message RTT."""
    payload = b"X" * msg_size
    sock = tcp_connect(host, port)
    latencies = []

    # warmup
    for _ in range(50):
        tcp_echo(sock, payload)

    for _ in range(iterations):
        t0 = time.perf_counter()
        tcp_echo(sock, payload)
        t1 = time.perf_counter()
        latencies.append((t1 - t0) * 1_000_000)  # microseconds

    sock.close()
    latencies.sort()

    return {
        "test": "latency",
        "msg_size": msg_size,
        "iterations": iterations,
        "unit": "us",
        "avg": round(statistics.mean(latencies), 2),
        "median": round(statistics.median(latencies), 2),
        "p95": round(latencies[int(len(latencies) * 0.95)], 2),
        "p99": round(latencies[int(len(latencies) * 0.99)], 2),
        "min": round(min(latencies), 2),
        "max": round(max(latencies), 2),
        "stdev": round(statistics.stdev(latencies), 2) if len(latencies) > 1 else 0,
    }


# ========== Test 2: Throughput ==========

def test_throughput(host, port, msg_size=1024, iterations=10000):
    """Single connection bulk echo - measure msg/s and MB/s."""
    payload = b"T" * msg_size
    sock = tcp_connect(host, port)

    # warmup
    for _ in range(100):
        tcp_echo(sock, payload)

    t0 = time.perf_counter()
    for _ in range(iterations):
        tcp_echo(sock, payload)
    t1 = time.perf_counter()

    sock.close()
    elapsed = t1 - t0
    msg_per_sec = iterations / elapsed
    bytes_total = msg_size * iterations * 2  # send + recv
    mb_per_sec = bytes_total / elapsed / (1024 * 1024)

    return {
        "test": "throughput",
        "msg_size": msg_size,
        "iterations": iterations,
        "elapsed_sec": round(elapsed, 3),
        "msg_per_sec": round(msg_per_sec, 1),
        "mb_per_sec": round(mb_per_sec, 2),
    }


# ========== Test 3: Concurrency ==========

def _concurrent_worker(host, port, msg_size, echo_count):
    """Worker for concurrent test: connect, echo N times, close."""
    payload = b"C" * msg_size
    latencies = []
    try:
        sock = tcp_connect(host, port)
        for _ in range(echo_count):
            t0 = time.perf_counter()
            tcp_echo(sock, payload)
            t1 = time.perf_counter()
            latencies.append((t1 - t0) * 1_000_000)
        sock.close()
        return latencies, None
    except Exception as e:
        return latencies, str(e)


def test_concurrency(host, port, connections=100, echo_per_conn=100, msg_size=256):
    """N concurrent connections, each echo M times."""
    all_latencies = []
    errors = 0

    t0 = time.perf_counter()
    with ThreadPoolExecutor(max_workers=connections) as executor:
        futures = [
            executor.submit(_concurrent_worker, host, port, msg_size, echo_per_conn)
            for _ in range(connections)
        ]
        for f in as_completed(futures):
            lats, err = f.result()
            all_latencies.extend(lats)
            if err:
                errors += 1
    t1 = time.perf_counter()

    elapsed = t1 - t0
    total_msgs = len(all_latencies)
    all_latencies.sort()

    result = {
        "test": "concurrency",
        "connections": connections,
        "echo_per_conn": echo_per_conn,
        "msg_size": msg_size,
        "total_messages": total_msgs,
        "elapsed_sec": round(elapsed, 3),
        "msg_per_sec": round(total_msgs / elapsed, 1) if elapsed > 0 else 0,
        "errors": errors,
    }

    if all_latencies:
        result.update({
            "avg_us": round(statistics.mean(all_latencies), 2),
            "p95_us": round(all_latencies[int(len(all_latencies) * 0.95)], 2),
            "p99_us": round(all_latencies[int(len(all_latencies) * 0.99)], 2),
        })

    return result


# ========== Test 4: Connection Rate ==========

def test_conn_rate(host, port, iterations=1000, msg_size=64):
    """Rapid connect -> echo -> close cycles."""
    payload = b"R" * msg_size
    latencies = []
    errors = 0

    # warmup
    for _ in range(20):
        try:
            s = tcp_connect(host, port)
            tcp_echo(s, payload)
            s.close()
        except Exception:
            pass

    for _ in range(iterations):
        try:
            t0 = time.perf_counter()
            s = tcp_connect(host, port)
            tcp_echo(s, payload)
            s.close()
            t1 = time.perf_counter()
            latencies.append((t1 - t0) * 1_000_000)
        except Exception:
            errors += 1

    elapsed_total = sum(latencies) / 1_000_000 if latencies else 1
    latencies.sort()

    result = {
        "test": "conn_rate",
        "iterations": iterations,
        "msg_size": msg_size,
        "successful": len(latencies),
        "errors": errors,
        "conn_per_sec": round(len(latencies) / elapsed_total, 1) if latencies else 0,
    }

    if latencies:
        result.update({
            "avg_us": round(statistics.mean(latencies), 2),
            "p95_us": round(latencies[int(len(latencies) * 0.95)], 2),
            "p99_us": round(latencies[int(len(latencies) * 0.99)], 2),
        })

    return result


# ========== Test 5: Large Message ==========

def test_large_message(host, port, size_mb=1):
    """Single connection, send/recv a large payload."""
    size_bytes = size_mb * 1024 * 1024
    payload = b"L" * size_bytes
    sock = tcp_connect(host, port, timeout=30.0)

    t0 = time.perf_counter()
    sock.sendall(payload)

    received = b""
    while len(received) < size_bytes:
        chunk = sock.recv(65536)
        if not chunk:
            break
        received += chunk
    t1 = time.perf_counter()

    sock.close()
    elapsed = t1 - t0
    mb_per_sec = (size_bytes * 2) / elapsed / (1024 * 1024)

    return {
        "test": "large_message",
        "size_mb": size_mb,
        "size_bytes": size_bytes,
        "received_bytes": len(received),
        "complete": len(received) == size_bytes,
        "elapsed_sec": round(elapsed, 4),
        "mb_per_sec": round(mb_per_sec, 2),
    }


# ========== Test 6: Message Size Sweep ==========

def test_msg_sweep(host, port, iterations_per_size=2000):
    """Various message sizes, measure msg/s for each."""
    sizes = [32, 256, 1024, 4096, 16384, 65536]
    results = []

    for size in sizes:
        payload = b"S" * size
        sock = tcp_connect(host, port)

        # warmup
        for _ in range(50):
            tcp_echo(sock, payload)

        t0 = time.perf_counter()
        for _ in range(iterations_per_size):
            tcp_echo(sock, payload)
        t1 = time.perf_counter()

        sock.close()
        elapsed = t1 - t0
        msg_per_sec = iterations_per_size / elapsed
        mb_per_sec = (size * iterations_per_size * 2) / elapsed / (1024 * 1024)

        results.append({
            "size": size,
            "iterations": iterations_per_size,
            "elapsed_sec": round(elapsed, 3),
            "msg_per_sec": round(msg_per_sec, 1),
            "mb_per_sec": round(mb_per_sec, 2),
        })

    return {
        "test": "msg_sweep",
        "sizes": results,
    }


# ========== Main ==========

def wait_for_server(host, port, timeout=10):
    """Wait for server to be ready."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(1)
            s.connect((host, port))
            s.close()
            return True
        except (ConnectionRefusedError, OSError):
            time.sleep(0.1)
    return False


def run_all_tests(host, port):
    """Run all benchmark tests and return results."""
    results = {}

    print("  [1/6] Latency test...", flush=True)
    results["latency"] = test_latency(host, port)

    print("  [2/6] Throughput test...", flush=True)
    results["throughput"] = test_throughput(host, port)

    print("  [3/6] Concurrency test...", flush=True)
    results["concurrency"] = test_concurrency(host, port)

    print("  [4/6] Connection rate test...", flush=True)
    results["conn_rate"] = test_conn_rate(host, port)

    print("  [5/6] Large message test...", flush=True)
    results["large_message"] = test_large_message(host, port)

    print("  [6/6] Message sweep test...", flush=True)
    results["msg_sweep"] = test_msg_sweep(host, port)

    return results


def main():
    parser = argparse.ArgumentParser(description="TCP Echo Benchmark Client")
    parser.add_argument("--host", default="127.0.0.1", help="Server host")
    parser.add_argument("--port", type=int, default=9001, help="Server port")
    parser.add_argument("--output", "-o", help="Output JSON file")
    parser.add_argument("--server", default="unknown", help="Server name for results")
    parser.add_argument("--test", help="Run specific test (latency|throughput|concurrency|conn_rate|large_message|msg_sweep)")
    parser.add_argument("--wait", action="store_true", help="Wait for server to be ready")
    args = parser.parse_args()

    if args.wait:
        print(f"Waiting for server at {args.host}:{args.port}...", flush=True)
        if not wait_for_server(args.host, args.port):
            print("ERROR: Server not available", file=sys.stderr)
            sys.exit(1)

    print(f"Running TCP benchmark against {args.server} ({args.host}:{args.port})", flush=True)

    if args.test:
        test_fn = {
            "latency": lambda: test_latency(args.host, args.port),
            "throughput": lambda: test_throughput(args.host, args.port),
            "concurrency": lambda: test_concurrency(args.host, args.port),
            "conn_rate": lambda: test_conn_rate(args.host, args.port),
            "large_message": lambda: test_large_message(args.host, args.port),
            "msg_sweep": lambda: test_msg_sweep(args.host, args.port),
        }.get(args.test)
        if not test_fn:
            print(f"Unknown test: {args.test}", file=sys.stderr)
            sys.exit(1)
        results = {args.test: test_fn()}
    else:
        results = run_all_tests(args.host, args.port)

    output = {
        "server": args.server,
        "host": args.host,
        "port": args.port,
        "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
        "results": results,
    }

    if args.output:
        os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)
        with open(args.output, "w") as f:
            json.dump(output, f, indent=2)
        print(f"Results saved to {args.output}")
    else:
        print(json.dumps(output, indent=2))


if __name__ == "__main__":
    main()

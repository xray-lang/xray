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
  7. Message Path   - read/write echo path, separate from native stream echo
 8. Upload         - Client -> server one-way transfer
 9. Download       - Server -> client one-way transfer
10. Proxy          - Client -> proxy -> upstream echo
11. Slow Upload    - Client write path under server-side slow reads
12. Slow Download  - Server write path under client-side slow reads
13. Idle Conns     - Many idle connections with a ping after the idle window
"""

import argparse
import copy
import json
import os
import resource
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


def recv_count(sock, expected_bytes):
    """Receive up to expected_bytes and return the number of bytes received."""
    received = 0
    while received < expected_bytes:
        chunk = sock.recv(min(65536, expected_bytes - received))
        if not chunk:
            break
        received += len(chunk)
    return received


def recv_count_slow(sock, expected_bytes, read_chunk=4096, read_delay_ms=1):
    """Receive expected bytes with a delay after each small read."""
    received = 0
    reads = 0
    delay = max(0, read_delay_ms) / 1000.0
    while received < expected_bytes:
        chunk = sock.recv(min(read_chunk, expected_bytes - received))
        if not chunk:
            break
        received += len(chunk)
        reads += 1
        if delay > 0:
            time.sleep(delay)
    return received, reads


def process_metrics():
    """Return lightweight client-side process metrics for result metadata."""
    usage = resource.getrusage(resource.RUSAGE_SELF)
    return {
        "cpu_user_sec": round(usage.ru_utime, 4),
        "cpu_system_sec": round(usage.ru_stime, 4),
        "max_rss_kb": usage.ru_maxrss,
    }


def renamed(result, name):
    result = dict(result)
    result["test"] = name
    return result


def median_result(samples):
    """Aggregate repeated test runs by median for numeric top-level fields."""
    if len(samples) == 1:
        return samples[0]

    result = copy.deepcopy(samples[0])
    result["repeat"] = len(samples)
    result["samples"] = samples
    keys = set()
    for sample in samples:
        keys.update(sample.keys())
    for key in keys:
        vals = [sample.get(key) for sample in samples]
        if vals and all(isinstance(v, (int, float)) and not isinstance(v, bool) for v in vals):
            result[key] = round(statistics.median(vals), 4)
    return result


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


# ========== Phase 0: Message path / one-way / proxy tests ==========

def test_message_latency(host, port):
    """Message API echo latency. Server should run in message mode."""
    return renamed(test_latency(host, port, msg_size=64, iterations=1000), "message_latency")


def test_message_throughput(host, port):
    """Message API echo throughput. Server should run in message mode."""
    return renamed(test_throughput(host, port, msg_size=1024, iterations=10000),
                   "message_throughput")


def test_upload(host, port, total_mb=64, chunk_size=65536):
    """Client -> server one-way transfer. Server should run in discard mode."""
    total_bytes = total_mb * 1024 * 1024
    payload = b"U" * chunk_size
    sock = tcp_connect(host, port, timeout=30.0)

    sent = 0
    t0 = time.perf_counter()
    while sent < total_bytes:
        n = min(chunk_size, total_bytes - sent)
        sock.sendall(payload[:n])
        sent += n
    sock.shutdown(socket.SHUT_WR)
    ack_bytes = recv_count(sock, 2)
    t1 = time.perf_counter()
    sock.close()

    elapsed = t1 - t0
    return {
        "test": "upload",
        "total_mb": total_mb,
        "bytes_sent": sent,
        "ack_bytes": ack_bytes,
        "elapsed_sec": round(elapsed, 4),
        "mb_per_sec": round((sent / elapsed) / (1024 * 1024), 2),
        "complete": sent == total_bytes and ack_bytes == 2,
    }


def test_download(host, port, total_mb=64):
    """Server -> client one-way transfer. Server should run in source mode."""
    total_bytes = total_mb * 1024 * 1024
    sock = tcp_connect(host, port, timeout=30.0)
    t0 = time.perf_counter()
    received = recv_count(sock, total_bytes)
    t1 = time.perf_counter()
    sock.close()

    elapsed = t1 - t0
    return {
        "test": "download",
        "total_mb": total_mb,
        "bytes_received": received,
        "elapsed_sec": round(elapsed, 4),
        "mb_per_sec": round((received / elapsed) / (1024 * 1024), 2),
        "complete": received == total_bytes,
    }


def test_slow_upload(host, port, total_mb=16, chunk_size=65536):
    """Client -> server transfer where the server deliberately reads slowly."""
    total_bytes = total_mb * 1024 * 1024
    payload = b"B" * chunk_size
    sock = tcp_connect(host, port, timeout=120.0)

    sent = 0
    t0 = time.perf_counter()
    while sent < total_bytes:
        n = min(chunk_size, total_bytes - sent)
        sock.sendall(payload[:n])
        sent += n
    sock.shutdown(socket.SHUT_WR)
    ack_bytes = recv_count(sock, 2)
    t1 = time.perf_counter()
    sock.close()

    elapsed = t1 - t0
    return {
        "test": "slow_upload",
        "total_mb": total_mb,
        "bytes_sent": sent,
        "ack_bytes": ack_bytes,
        "elapsed_sec": round(elapsed, 4),
        "mb_per_sec": round((sent / elapsed) / (1024 * 1024), 2),
        "complete": sent == total_bytes and ack_bytes == 2,
    }


def test_slow_download(host, port, total_mb=16, read_chunk=4096, read_delay_ms=1):
    """Server -> client transfer where the client deliberately reads slowly."""
    total_bytes = total_mb * 1024 * 1024
    sock = tcp_connect(host, port, timeout=120.0)
    t0 = time.perf_counter()
    received, reads = recv_count_slow(sock, total_bytes, read_chunk, read_delay_ms)
    t1 = time.perf_counter()
    sock.close()

    elapsed = t1 - t0
    return {
        "test": "slow_download",
        "total_mb": total_mb,
        "read_chunk": read_chunk,
        "read_delay_ms": read_delay_ms,
        "read_calls": reads,
        "bytes_received": received,
        "elapsed_sec": round(elapsed, 4),
        "mb_per_sec": round((received / elapsed) / (1024 * 1024), 2),
        "complete": received == total_bytes,
    }


def test_idle_connections(host, port, connections=1000, idle_ms=1000, ping_size=16):
    """Open many idle connections, then ping each one once."""
    socks = []
    errors = 0
    open_start = time.perf_counter()
    for _ in range(connections):
        try:
            socks.append(tcp_connect(host, port, timeout=10.0))
        except Exception:
            errors += 1
    open_end = time.perf_counter()

    time.sleep(max(0, idle_ms) / 1000.0)

    payload = b"I" * ping_size
    ping_latencies = []
    for sock in socks:
        try:
            t0 = time.perf_counter()
            tcp_echo(sock, payload)
            t1 = time.perf_counter()
            ping_latencies.append((t1 - t0) * 1_000_000)
        except Exception:
            errors += 1

    for sock in socks:
        try:
            sock.close()
        except Exception:
            pass

    ping_latencies.sort()
    result = {
        "test": "idle_connections",
        "connections": connections,
        "opened": len(socks),
        "idle_ms": idle_ms,
        "ping_size": ping_size,
        "ping_success": len(ping_latencies),
        "errors": errors,
        "open_elapsed_sec": round(open_end - open_start, 4),
        "complete": len(socks) == connections and len(ping_latencies) == len(socks) and errors == 0,
    }
    if ping_latencies:
        result.update({
            "avg_ping_us": round(statistics.mean(ping_latencies), 2),
            "p95_ping_us": round(ping_latencies[int(len(ping_latencies) * 0.95)], 2),
            "p99_ping_us": round(ping_latencies[int(len(ping_latencies) * 0.99)], 2),
        })
    return result


def test_proxy_throughput(host, port, msg_size=1024, iterations=5000):
    """End-to-end echo through a TCP proxy."""
    return renamed(test_throughput(host, port, msg_size=msg_size, iterations=iterations),
                   "proxy_throughput")


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
    return run_named_tests(host, port, DEFAULT_TESTS)


TESTS = {
    "latency": lambda host, port: test_latency(host, port),
    "throughput": lambda host, port: test_throughput(host, port),
    "concurrency": lambda host, port: test_concurrency(host, port),
    "conn_rate": lambda host, port: test_conn_rate(host, port),
    "large_message": lambda host, port: test_large_message(host, port),
    "msg_sweep": lambda host, port: test_msg_sweep(host, port),
    "message_latency": lambda host, port: test_message_latency(host, port),
    "message_throughput": lambda host, port: test_message_throughput(host, port),
    "upload": lambda host, port: test_upload(host, port),
    "download": lambda host, port: test_download(host, port),
    "proxy_throughput": lambda host, port: test_proxy_throughput(host, port),
    "slow_upload": lambda host, port: test_slow_upload(host, port),
    "slow_download": lambda host, port: test_slow_download(host, port),
    "idle_connections": lambda host, port: test_idle_connections(host, port),
}


DEFAULT_TESTS = ["latency", "throughput", "concurrency", "conn_rate", "large_message", "msg_sweep"]


def run_named_tests(host, port, names, repeat=1):
    results = {}
    for idx, name in enumerate(names, 1):
        test_fn = TESTS.get(name)
        if not test_fn:
            raise ValueError(f"Unknown test: {name}")
        print(f"  [{idx}/{len(names)}] {name}...", flush=True)
        samples = [test_fn(host, port) for _ in range(repeat)]
        results[name] = median_result(samples)
    return results


def main():
    parser = argparse.ArgumentParser(description="TCP Echo Benchmark Client")
    parser.add_argument("--host", default="127.0.0.1", help="Server host")
    parser.add_argument("--port", type=int, default=9001, help="Server port")
    parser.add_argument("--output", "-o", help="Output JSON file")
    parser.add_argument("--server", default="unknown", help="Server name for results")
    parser.add_argument("--scenario", default="echo", help="Scenario label for result metadata")
    parser.add_argument("--repeat", type=int, default=1, help="Repeat each selected test and report medians")
    parser.add_argument(
        "--test",
        help="Run comma-separated tests (latency|throughput|concurrency|conn_rate|large_message|msg_sweep|message_latency|message_throughput|upload|download|proxy_throughput|slow_upload|slow_download|idle_connections)",
    )
    parser.add_argument("--wait", action="store_true", help="Wait for server to be ready")
    args = parser.parse_args()

    if args.wait:
        print(f"Waiting for server at {args.host}:{args.port}...", flush=True)
        if not wait_for_server(args.host, args.port):
            print("ERROR: Server not available", file=sys.stderr)
            sys.exit(1)

    print(f"Running TCP benchmark against {args.server} ({args.host}:{args.port})", flush=True)

    repeat = max(1, args.repeat)
    if args.test:
        names = [name.strip() for name in args.test.split(",") if name.strip()]
        try:
            results = run_named_tests(args.host, args.port, names, repeat)
        except ValueError as e:
            print(str(e), file=sys.stderr)
            sys.exit(1)
    else:
        results = run_named_tests(args.host, args.port, DEFAULT_TESTS, repeat)

    output = {
        "server": args.server,
        "scenario": args.scenario,
        "host": args.host,
        "port": args.port,
        "repeat": repeat,
        "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
        "client_metrics": process_metrics(),
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

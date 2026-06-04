#!/usr/bin/env python3
"""
TCP Benchmark Result Comparison Tool
Reads JSON result files and outputs a side-by-side comparison table.

Usage:
    python3 compare.py results/xray.json results/go.json results/node.json results/python.json
"""

import json
import sys
import os


def load_result(path):
    """Load a JSON result file."""
    with open(path) as f:
        return json.load(f)


def fmt_num(val, unit=""):
    """Format a number for display."""
    if val is None:
        return "N/A"
    if isinstance(val, float):
        if val >= 10000:
            return f"{val:,.0f}{unit}"
        elif val >= 100:
            return f"{val:,.1f}{unit}"
        else:
            return f"{val:,.2f}{unit}"
    return f"{val}{unit}"


def print_table(headers, rows, col_widths=None):
    """Print a formatted table."""
    if not col_widths:
        col_widths = [max(len(str(row[i])) for row in [headers] + rows) + 2
                      for i in range(len(headers))]

    def fmt_row(row):
        return "│".join(str(row[i]).center(col_widths[i]) for i in range(len(row)))

    separator = "┼".join("─" * w for w in col_widths)

    print(fmt_row(headers))
    print(separator)
    for row in rows:
        print(fmt_row(row))


def has_test(results, name):
    return any(name in r.get("results", {}) for r in results)


def compare_numeric_test(results, test_name, title, metrics):
    print(f"\n╔{'═' * (len(title) + 4)}╗")
    print(f"║  {title}  ║")
    print(f"╚{'═' * (len(title) + 4)}╝\n")

    headers = ["Metric"] + [r["server"] for r in results]
    rows = []
    for label, key in metrics:
        row = [label]
        for r in results:
            val = r.get("results", {}).get(test_name, {}).get(key)
            if isinstance(val, bool):
                row.append("Yes" if val else "No")
            else:
                row.append(fmt_num(val))
        rows.append(row)
    print_table(headers, rows)


def compare_latency(results):
    """Compare latency test results."""
    print("\n╔══════════════════════════════════════════╗")
    print("║  Test 1: Latency (single conn, 64B echo) ║")
    print("╚══════════════════════════════════════════╝\n")

    headers = ["Metric"] + [r["server"] for r in results]
    rows = []
    metrics = [
        ("Avg (µs)", "avg"),
        ("Median (µs)", "median"),
        ("P95 (µs)", "p95"),
        ("P99 (µs)", "p99"),
        ("Min (µs)", "min"),
        ("Max (µs)", "max"),
        ("Stdev (µs)", "stdev"),
    ]
    for label, key in metrics:
        row = [label]
        for r in results:
            val = r.get("results", {}).get("latency", {}).get(key)
            row.append(fmt_num(val))
        rows.append(row)

    print_table(headers, rows)


def compare_throughput(results):
    """Compare throughput test results."""
    print("\n╔═══════════════════════════════════════════════╗")
    print("║  Test 2: Throughput (single conn, 1KB echo)    ║")
    print("╚═══════════════════════════════════════════════╝\n")

    headers = ["Metric"] + [r["server"] for r in results]
    rows = []
    metrics = [
        ("Msg/sec", "msg_per_sec"),
        ("MB/sec", "mb_per_sec"),
        ("Elapsed (s)", "elapsed_sec"),
    ]
    for label, key in metrics:
        row = [label]
        for r in results:
            val = r.get("results", {}).get("throughput", {}).get(key)
            row.append(fmt_num(val))
        rows.append(row)

    print_table(headers, rows)


def compare_concurrency(results):
    """Compare concurrency test results."""
    print("\n╔═══════════════════════════════════════════════════╗")
    print("║  Test 3: Concurrency (100 conns × 100 echoes)     ║")
    print("╚═══════════════════════════════════════════════════╝\n")

    headers = ["Metric"] + [r["server"] for r in results]
    rows = []
    metrics = [
        ("Total Msg/sec", "msg_per_sec"),
        ("Avg Latency (µs)", "avg_us"),
        ("P95 Latency (µs)", "p95_us"),
        ("P99 Latency (µs)", "p99_us"),
        ("Errors", "errors"),
    ]
    for label, key in metrics:
        row = [label]
        for r in results:
            val = r.get("results", {}).get("concurrency", {}).get(key)
            row.append(fmt_num(val))
        rows.append(row)

    print_table(headers, rows)


def compare_conn_rate(results):
    """Compare connection rate test results."""
    print("\n╔════════════════════════════════════════════════╗")
    print("║  Test 4: Connection Rate (connect+echo+close)  ║")
    print("╚════════════════════════════════════════════════╝\n")

    headers = ["Metric"] + [r["server"] for r in results]
    rows = []
    metrics = [
        ("Conn/sec", "conn_per_sec"),
        ("Avg (µs)", "avg_us"),
        ("P95 (µs)", "p95_us"),
        ("P99 (µs)", "p99_us"),
        ("Errors", "errors"),
    ]
    for label, key in metrics:
        row = [label]
        for r in results:
            val = r.get("results", {}).get("conn_rate", {}).get(key)
            row.append(fmt_num(val))
        rows.append(row)

    print_table(headers, rows)


def compare_large_message(results):
    """Compare large message test results."""
    print("\n╔═══════════════════════════════════════╗")
    print("║  Test 5: Large Message (1MB echo)      ║")
    print("╚═══════════════════════════════════════╝\n")

    headers = ["Metric"] + [r["server"] for r in results]
    rows = []
    metrics = [
        ("MB/sec", "mb_per_sec"),
        ("Elapsed (s)", "elapsed_sec"),
        ("Complete", "complete"),
    ]
    for label, key in metrics:
        row = [label]
        for r in results:
            val = r.get("results", {}).get("large_message", {}).get(key)
            if isinstance(val, bool):
                row.append("Yes" if val else "No")
            else:
                row.append(fmt_num(val))
        rows.append(row)

    print_table(headers, rows)


def compare_msg_sweep(results):
    """Compare message sweep test results."""
    print("\n╔═══════════════════════════════════════════════╗")
    print("║  Test 6: Message Size Sweep (msg/s by size)    ║")
    print("╚═══════════════════════════════════════════════╝\n")

    sizes = [32, 256, 1024, 4096, 16384, 65536]
    size_labels = ["32B", "256B", "1KB", "4KB", "16KB", "64KB"]

    # msg/s table
    print("  Messages per second:")
    headers = ["Size"] + [r["server"] for r in results]
    rows = []
    for i, (size, label) in enumerate(zip(sizes, size_labels)):
        row = [label]
        for r in results:
            sweep = r.get("results", {}).get("msg_sweep", {}).get("sizes", [])
            val = None
            for s in sweep:
                if s.get("size") == size:
                    val = s.get("msg_per_sec")
                    break
            row.append(fmt_num(val))
        rows.append(row)

    print_table(headers, rows)

    # MB/s table
    print("\n  Throughput (MB/s):")
    rows2 = []
    for i, (size, label) in enumerate(zip(sizes, size_labels)):
        row = [label]
        for r in results:
            sweep = r.get("results", {}).get("msg_sweep", {}).get("sizes", [])
            val = None
            for s in sweep:
                if s.get("size") == size:
                    val = s.get("mb_per_sec")
                    break
            row.append(fmt_num(val))
        rows2.append(row)

    print_table(headers, rows2)


def print_summary(results):
    """Print a quick winner summary."""
    print("\n╔═══════════════════════════════╗")
    print("║        Summary (Best ★)        ║")
    print("╚═══════════════════════════════╝\n")

    comparisons = [
        ("Latency (avg µs)", "latency", "avg", "lower"),
        ("Throughput (msg/s)", "throughput", "msg_per_sec", "higher"),
        ("Concurrency (msg/s)", "concurrency", "msg_per_sec", "higher"),
        ("Conn Rate (conn/s)", "conn_rate", "conn_per_sec", "higher"),
        ("Large Msg (MB/s)", "large_message", "mb_per_sec", "higher"),
    ]

    for label, test, metric, direction in comparisons:
        best_val = None
        best_server = "N/A"
        for r in results:
            val = r.get("results", {}).get(test, {}).get(metric)
            if val is None:
                continue
            if best_val is None:
                best_val = val
                best_server = r["server"]
            elif direction == "lower" and val < best_val:
                best_val = val
                best_server = r["server"]
            elif direction == "higher" and val > best_val:
                best_val = val
                best_server = r["server"]

        print(f"  {label:30s} ★ {best_server} ({fmt_num(best_val)})")


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 compare.py result1.json result2.json ...")
        print("       python3 compare.py results/")
        sys.exit(1)

    # Support directory as argument
    paths = []
    for arg in sys.argv[1:]:
        if os.path.isdir(arg):
            for f in sorted(os.listdir(arg)):
                if f.endswith(".json"):
                    paths.append(os.path.join(arg, f))
        else:
            paths.append(arg)

    if not paths:
        print("No result files found.", file=sys.stderr)
        sys.exit(1)

    results = [load_result(p) for p in paths]

    print(f"\nComparing {len(results)} servers: {', '.join(r['server'] for r in results)}")
    print(f"Files: {', '.join(os.path.basename(p) for p in paths)}")

    if has_test(results, "latency"):
        compare_latency(results)
    if has_test(results, "throughput"):
        compare_throughput(results)
    if has_test(results, "concurrency"):
        compare_concurrency(results)
    if has_test(results, "conn_rate"):
        compare_conn_rate(results)
    if has_test(results, "large_message"):
        compare_large_message(results)
    if has_test(results, "msg_sweep"):
        compare_msg_sweep(results)
    if has_test(results, "message_latency"):
        compare_numeric_test(results, "message_latency", "Message Path Latency (read/write echo)", [
            ("Avg (µs)", "avg"),
            ("Median (µs)", "median"),
            ("P95 (µs)", "p95"),
            ("P99 (µs)", "p99"),
            ("Stdev (µs)", "stdev"),
        ])
    if has_test(results, "message_throughput"):
        compare_numeric_test(results, "message_throughput", "Message Path Throughput (read/write echo)", [
            ("Msg/sec", "msg_per_sec"),
            ("MB/sec", "mb_per_sec"),
            ("Elapsed (s)", "elapsed_sec"),
        ])
    if has_test(results, "upload"):
        compare_numeric_test(results, "upload", "Upload One-Way (client -> server)", [
            ("MB/sec", "mb_per_sec"),
            ("Elapsed (s)", "elapsed_sec"),
            ("Complete", "complete"),
        ])
    if has_test(results, "download"):
        compare_numeric_test(results, "download", "Download One-Way (server -> client)", [
            ("MB/sec", "mb_per_sec"),
            ("Elapsed (s)", "elapsed_sec"),
            ("Complete", "complete"),
        ])
    if has_test(results, "proxy_throughput"):
        compare_numeric_test(results, "proxy_throughput", "Proxy Throughput (client -> proxy -> upstream)", [
            ("Msg/sec", "msg_per_sec"),
            ("MB/sec", "mb_per_sec"),
            ("Elapsed (s)", "elapsed_sec"),
        ])
    if has_test(results, "slow_download"):
        compare_numeric_test(results, "slow_download", "Slow Download Backpressure (slow client reads)", [
            ("MB/sec", "mb_per_sec"),
            ("Elapsed (s)", "elapsed_sec"),
            ("Read Calls", "read_calls"),
            ("Complete", "complete"),
        ])
    if has_test(results, "slow_upload"):
        compare_numeric_test(results, "slow_upload", "Slow Upload Backpressure (slow server reads)", [
            ("MB/sec", "mb_per_sec"),
            ("Elapsed (s)", "elapsed_sec"),
            ("Complete", "complete"),
        ])
    if has_test(results, "idle_connections"):
        compare_numeric_test(results, "idle_connections", "Idle Connections (idle then ping)", [
            ("Opened", "opened"),
            ("Ping Success", "ping_success"),
            ("Open Elapsed (s)", "open_elapsed_sec"),
            ("Avg Ping (µs)", "avg_ping_us"),
            ("P99 Ping (µs)", "p99_ping_us"),
            ("Errors", "errors"),
            ("Complete", "complete"),
        ])
    if has_test(results, "tls_handshake_rate"):
        compare_numeric_test(results, "tls_handshake_rate", "TLS Handshake Rate", [
            ("Handshakes", "handshakes"),
            ("Handshake/sec", "handshakes_per_sec"),
            ("Elapsed (s)", "elapsed_sec"),
            ("Complete", "complete"),
        ])
    if has_test(results, "tls_latency"):
        compare_numeric_test(results, "tls_latency", "TLS Echo Latency", [
            ("Avg (µs)", "avg"),
            ("Median (µs)", "median"),
            ("P95 (µs)", "p95"),
            ("P99 (µs)", "p99"),
            ("Complete", "complete"),
        ])
    if has_test(results, "tls_throughput"):
        compare_numeric_test(results, "tls_throughput", "TLS Echo Throughput", [
            ("Msg/sec", "msg_per_sec"),
            ("MB/sec", "mb_per_sec"),
            ("Elapsed (s)", "elapsed_sec"),
            ("Complete", "complete"),
        ])
    if has_test(results, "tls_large"):
        compare_numeric_test(results, "tls_large", "TLS Large Stream", [
            ("Size MB", "size_mb"),
            ("MB/sec", "mb_per_sec"),
            ("Elapsed (s)", "elapsed_sec"),
            ("Complete", "complete"),
        ])
    if any(has_test(results, name) for name in ["latency", "throughput", "concurrency",
                                                "conn_rate", "large_message"]):
        print_summary(results)
    print()


if __name__ == "__main__":
    main()

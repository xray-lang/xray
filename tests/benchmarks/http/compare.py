#!/usr/bin/env python3
"""
compare.py - Compare HTTP benchmark results across servers

Reads JSON result files from the results/ directory and prints a
side-by-side comparison table.

Usage:
  python3 compare.py [results_dir]
"""

import json
import os
import sys


def load_results(results_dir):
    data = {}
    for fname in sorted(os.listdir(results_dir)):
        if fname.endswith(".json"):
            path = os.path.join(results_dir, fname)
            with open(path) as f:
                obj = json.load(f)
            label = obj.get("_meta", {}).get("label", fname.replace(".json", ""))
            data[label] = obj
    return data


def fmt_rate(v):
    if v >= 1000:
        return f"{v / 1000:.1f}K"
    return f"{v:.0f}"


def fmt_size(n):
    if n >= 1048576:
        return f"{n / 1048576:.0f}MB"
    if n >= 1024:
        return f"{n / 1024:.0f}KB"
    return f"{n}B"


def print_section(title):
    print()
    print(f"━━━ {title} ━━━")
    print()


def compare(data):
    labels = list(data.keys())
    if not labels:
        print("No result files found.")
        return

    print()
    print("╔══════════════════════════════════════════════════════════════════╗")
    print("║                HTTP Benchmark Comparison                        ║")
    print(f"║  Servers: {', '.join(labels):<54s} ║")
    print("╚══════════════════════════════════════════════════════════════════╝")

    col_w = 12

    # ── Latency comparison ───────────────────────────────────────────────
    print_section("Latency: Avg RTT (ms) — lower is better")

    all_paths = []
    for d in data.values():
        for r in d.get("latency", []):
            if r["path"] not in all_paths:
                all_paths.append(r["path"])

    header = f"  {'Endpoint':>12s}"
    for lb in labels:
        header += f"  {lb:>{col_w}s}"
    print(header)
    print(f"  {'─'*12}" + f"  {'─'*col_w}" * len(labels))

    for path in all_paths:
        row = f"  {path:>12s}"
        vals = []
        for lb in labels:
            found = [r for r in data[lb].get("latency", []) if r["path"] == path]
            vals.append(found[0]["avg"] if found else None)

        best = min((v for v in vals if v is not None), default=None)
        for v in vals:
            if v is None:
                row += f"  {'—':>{col_w}s}"
            else:
                marker = " *" if v == best and len([x for x in vals if x]) > 1 else "  "
                row += f"  {v:>{col_w - 2}.3f}{marker}"
        print(row)

    # ── Throughput comparison ────────────────────────────────────────────
    print_section("Throughput: Req/s (single connection) — higher is better")

    all_paths = []
    for d in data.values():
        for r in d.get("throughput", []):
            if r["path"] not in all_paths:
                all_paths.append(r["path"])

    header = f"  {'Endpoint':>12s}"
    for lb in labels:
        header += f"  {lb:>{col_w}s}"
    print(header)
    print(f"  {'─'*12}" + f"  {'─'*col_w}" * len(labels))

    for path in all_paths:
        row = f"  {path:>12s}"
        vals = []
        for lb in labels:
            found = [r for r in data[lb].get("throughput", []) if r["path"] == path]
            vals.append(found[0]["req_per_sec"] if found else None)

        best = max((v for v in vals if v is not None), default=None)
        for v in vals:
            if v is None:
                row += f"  {'—':>{col_w}s}"
            else:
                marker = " *" if v == best and len([x for x in vals if x]) > 1 else "  "
                row += f"  {fmt_rate(v):>{col_w - 2}s}{marker}"
        print(row)

    # ── Concurrency comparison ───────────────────────────────────────────
    print_section("Concurrency: Aggregate Req/s — higher is better")

    all_conns = set()
    for d in data.values():
        for r in d.get("concurrency", []):
            all_conns.add(r["connections"])
    all_conns = sorted(all_conns)

    header = f"  {'Conns':>8s}"
    for lb in labels:
        header += f"  {lb:>{col_w}s}"
    print(header)
    print(f"  {'─'*8}" + f"  {'─'*col_w}" * len(labels))

    for nc in all_conns:
        row = f"  {nc:>8d}"
        vals = []
        for lb in labels:
            found = [r for r in data[lb].get("concurrency", []) if r["connections"] == nc]
            vals.append(found[0]["agg_req_sec"] if found else None)

        best = max((v for v in vals if v is not None), default=None)
        for v in vals:
            if v is None:
                row += f"  {'—':>{col_w}s}"
            else:
                marker = " *" if v == best and len([x for x in vals if x]) > 1 else "  "
                row += f"  {fmt_rate(v):>{col_w - 2}s}{marker}"
        print(row)

    # ── Echo comparison ──────────────────────────────────────────────────
    print_section("POST Echo: Req/s — higher is better")

    all_sizes = set()
    for d in data.values():
        for r in d.get("echo", []):
            all_sizes.add(r["body_size"])
    all_sizes = sorted(all_sizes)

    header = f"  {'BodySize':>10s}"
    for lb in labels:
        header += f"  {lb:>{col_w}s}"
    print(header)
    print(f"  {'─'*10}" + f"  {'─'*col_w}" * len(labels))

    for size in all_sizes:
        row = f"  {fmt_size(size):>10s}"
        vals = []
        for lb in labels:
            found = [r for r in data[lb].get("echo", []) if r["body_size"] == size]
            vals.append(found[0]["req_per_sec"] if found else None)

        best = max((v for v in vals if v is not None), default=None)
        for v in vals:
            if v is None:
                row += f"  {'—':>{col_w}s}"
            else:
                marker = " *" if v == best and len([x for x in vals if x]) > 1 else "  "
                row += f"  {fmt_rate(v):>{col_w - 2}s}{marker}"
        print(row)

    # ── Connection churn comparison ──────────────────────────────────────
    print_section("Connection Churn — lower latency / higher conns/sec is better")

    header = f"  {'Metric':>16s}"
    for lb in labels:
        header += f"  {lb:>{col_w}s}"
    print(header)
    print(f"  {'─'*16}" + f"  {'─'*col_w}" * len(labels))

    for metric, unit, better in [
        ("avg_ms", "ms", "lower"),
        ("median_ms", "ms", "lower"),
        ("p99_ms", "ms", "lower"),
        ("conns_per_sec", "/s", "higher"),
    ]:
        row = f"  {metric:>16s}"
        vals = []
        for lb in labels:
            churn = data[lb].get("connection_churn", {})
            vals.append(churn.get(metric))

        valid = [v for v in vals if v is not None]
        best = min(valid) if better == "lower" and valid else max(valid) if valid else None
        for v in vals:
            if v is None:
                row += f"  {'—':>{col_w}s}"
            else:
                marker = " *" if v == best and len(valid) > 1 else "  "
                row += f"  {v:>{col_w - 2}.2f}{marker}"
        print(row)

    # ── Pipeline comparison ──────────────────────────────────────────────
    print_section("Sustained Pipeline: Aggregate Req/s — higher is better")

    all_conns = set()
    for d in data.values():
        for r in d.get("pipeline", []):
            all_conns.add(r["connections"])
    all_conns = sorted(all_conns)

    if all_conns:
        header = f"  {'Conns':>8s}"
        for lb in labels:
            header += f"  {lb:>{col_w}s}"
        print(header)
        print(f"  {'─'*8}" + f"  {'─'*col_w}" * len(labels))

        for nc in all_conns:
            row = f"  {nc:>8d}"
            vals = []
            for lb in labels:
                found = [r for r in data[lb].get("pipeline", []) if r["connections"] == nc]
                vals.append(found[0]["agg_req_sec"] if found else None)

            best = max((v for v in vals if v is not None), default=None)
            for v in vals:
                if v is None:
                    row += f"  {'—':>{col_w}s}"
                else:
                    marker = " *" if v == best and len([x for x in vals if x]) > 1 else "  "
                    row += f"  {fmt_rate(v):>{col_w - 2}s}{marker}"
            print(row)

    print()
    print("  (* = best in category)")
    print()


def main():
    results_dir = sys.argv[1] if len(sys.argv) > 1 else "results"
    if not os.path.isdir(results_dir):
        print(f"No results directory: {results_dir}")
        sys.exit(1)
    data = load_results(results_dir)
    compare(data)


if __name__ == "__main__":
    main()

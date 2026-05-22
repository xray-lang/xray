#!/usr/bin/env python3
import argparse
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
XRAY_ROOT = SCRIPT_DIR.parents[2]
RESULTS_DIR = SCRIPT_DIR / "results" / "compare"


def executable_name(name):
    return f"{name}.exe" if os.name == "nt" else name


def command_exists(name):
    return shutil.which(name) is not None


def find_xray(explicit):
    if explicit:
        path = Path(explicit)
        if path.is_file():
            return path.resolve()
        raise FileNotFoundError(f"xray binary not found: {explicit}")
    env = os.environ.get("XRAY_BIN")
    if env:
        path = Path(env)
        if path.is_file():
            return path.resolve()
    for dirname in ("build-release", "build", "build-http-debug", "build-embed", "build-new"):
        for name in ("xray.exe", "xray"):
            path = XRAY_ROOT / dirname / name
            if path.is_file():
                return path.resolve()
    raise FileNotFoundError("xray binary not found; set XRAY_BIN or pass --xray-bin")


def wait_for_server(port, timeout):
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5) as sock:
                sock.sendall(b"GET /plaintext HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n")
                data = sock.recv(256)
                if b"200 OK" in data:
                    return True
        except OSError as exc:
            last_error = exc
        time.sleep(0.2)
    if last_error:
        print(f"server wait failed: {last_error}", file=sys.stderr)
    return False


def terminate_process(proc):
    if proc.poll() is not None:
        return
    if os.name == "nt":
        proc.terminate()
    else:
        proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)


def build_go_server(source, output):
    if not command_exists("go"):
        raise RuntimeError("Go is not installed")
    cmd = ["go", "build", "-o", str(output), str(source)]
    subprocess.run(cmd, cwd=SCRIPT_DIR, check=True)


def server_command(name, port, xray_bin):
    if name == "xray":
        return [str(xray_bin), str(SCRIPT_DIR / "http_server.xr"), "--", str(port)]
    if name == "go":
        output = SCRIPT_DIR / executable_name("http_server_go")
        source = SCRIPT_DIR / "http_server.go"
        if not output.exists() or source.stat().st_mtime > output.stat().st_mtime:
            build_go_server(source, output)
        return [str(output), str(port)]
    if name == "fasthttp":
        output = SCRIPT_DIR / executable_name("http_server_fasthttp_bin")
        source = SCRIPT_DIR / "http_server_fasthttp.go"
        if not output.exists() or source.stat().st_mtime > output.stat().st_mtime:
            build_go_server(source, output)
        return [str(output), str(port)]
    if name == "node":
        if not command_exists("node"):
            raise RuntimeError("Node.js is not installed")
        return ["node", str(SCRIPT_DIR / "http_server.js"), str(port)]
    if name == "python":
        return [sys.executable, str(SCRIPT_DIR / "http_server.py"), str(port)]
    raise ValueError(f"unknown server: {name}")


def validate_results(path):
    data = json.loads(path.read_text(encoding="utf-8"))
    if len(data.get("latency", [])) != 2:
        raise RuntimeError("latency results are incomplete")
    if len(data.get("throughput", [])) != 2:
        raise RuntimeError("throughput results are incomplete")
    if any(item.get("req_per_sec", 0) <= 0 for item in data.get("throughput", [])):
        raise RuntimeError("throughput contains zero req/s")
    concurrency = data.get("concurrency", [])
    if len(concurrency) != 5:
        raise RuntimeError("concurrency results are incomplete")
    if any(item.get("fail_conns", 0) != 0 or item.get("total_requests", 0) <= 0 for item in concurrency):
        raise RuntimeError("concurrency contains failed or zero-request workers")
    if len(data.get("echo", [])) != 5:
        raise RuntimeError("echo results are incomplete")
    churn = data.get("connection_churn", {})
    if churn.get("conns_per_sec", 0) <= 0:
        raise RuntimeError("connection churn result is missing")
    pipeline = data.get("pipeline", [])
    if len(pipeline) != 3:
        raise RuntimeError("pipeline results are incomplete")
    if any(item.get("total_requests", 0) <= 0 for item in pipeline):
        raise RuntimeError("pipeline contains zero-request workers")


def run_benchmark(name, port, quick, xray_bin, keep_logs):
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    json_path = RESULTS_DIR / f"{name}.json"
    log_path = RESULTS_DIR / f"{name}.log"
    cmd = server_command(name, port, xray_bin)
    print(f"\n=== {name} ===")
    print("server:", " ".join(cmd))
    with open(log_path, "w", encoding="utf-8") as log_file:
        proc = subprocess.Popen(cmd, cwd=SCRIPT_DIR, stdout=log_file, stderr=subprocess.STDOUT)
        try:
            if not wait_for_server(port, 20):
                raise RuntimeError(f"{name} server did not start")
            bench_cmd = [
                sys.executable,
                str(SCRIPT_DIR / "http_bench.py"),
                "--url",
                f"http://127.0.0.1:{port}",
                "--label",
                name,
                "--json",
                str(json_path),
            ]
            if quick:
                bench_cmd.append("--quick")
            subprocess.run(bench_cmd, cwd=SCRIPT_DIR, check=True)
            validate_results(json_path)
        finally:
            terminate_process(proc)
            if not keep_logs and log_path.exists() and log_path.stat().st_size == 0:
                log_path.unlink()


def parse_only(values):
    if not values:
        return ["xray", "go", "fasthttp", "node", "python"]
    result = []
    for item in values:
        for name in item.split(","):
            name = name.strip()
            if name:
                result.append(name)
    return result


def main():
    parser = argparse.ArgumentParser(description="Cross-platform HTTP benchmark comparison")
    parser.add_argument("--quick", action="store_true", help="Use reduced benchmark iterations")
    parser.add_argument("--only", action="append", help="Server(s): xray,go,fasthttp,node,python")
    parser.add_argument("--port", type=int, default=8080, help="Base port used for every server")
    parser.add_argument("--xray-bin", help="Path to xray executable")
    parser.add_argument("--keep-logs", action="store_true", help="Keep server stdout/stderr logs")
    parser.add_argument("--compare-only", action="store_true", help="Only compare existing JSON results")
    args = parser.parse_args()

    if args.compare_only:
        subprocess.run([sys.executable, str(SCRIPT_DIR / "compare.py"), str(RESULTS_DIR)], check=True)
        return

    selected = parse_only(args.only)
    valid = {"xray", "go", "fasthttp", "node", "python"}
    unknown = [name for name in selected if name not in valid]
    if unknown:
        raise SystemExit(f"unknown server(s): {', '.join(unknown)}")

    xray_bin = find_xray(args.xray_bin) if "xray" in selected else None
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    for path in RESULTS_DIR.glob("*.json"):
        path.unlink()
    failures = []
    for name in selected:
        try:
            run_benchmark(name, args.port, args.quick, xray_bin, args.keep_logs)
        except Exception as exc:
            failures.append((name, exc))
            print(f"SKIP/FAIL {name}: {exc}", file=sys.stderr)

    subprocess.run([sys.executable, str(SCRIPT_DIR / "compare.py"), str(RESULTS_DIR)], check=True)
    if failures:
        print("\nSome servers failed or were skipped:")
        for name, exc in failures:
            print(f"  {name}: {exc}")


if __name__ == "__main__":
    main()

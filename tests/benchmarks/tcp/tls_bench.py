#!/usr/bin/env python3
"""Local TLS benchmark harness for Xray's net.dialTLS/read/write path."""

import argparse
import json
import os
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent


def find_xray():
    candidates = [
        SCRIPT_DIR / "../../../build-release/xray",
        SCRIPT_DIR / "../../../build/xray",
        shutil.which("xray"),
    ]
    for candidate in candidates:
        if not candidate:
            continue
        path = Path(candidate).resolve()
        if path.exists() and os.access(path, os.X_OK):
            return str(path)
    return None


def ensure_cert(cert_dir):
    cert_dir.mkdir(parents=True, exist_ok=True)
    cert_file = cert_dir / "tls-bench-cert.pem"
    key_file = cert_dir / "tls-bench-key.pem"
    if cert_file.exists() and key_file.exists():
        return cert_file, key_file

    if not shutil.which("openssl"):
        raise RuntimeError("openssl is required to generate the local TLS benchmark certificate")

    with tempfile.NamedTemporaryFile("w", delete=False) as conf:
        conf.write(
            """
[req]
distinguished_name = dn
x509_extensions = v3_req
prompt = no

[dn]
CN = localhost

[v3_req]
subjectAltName = @alt_names
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,digitalSignature,keyEncipherment
extendedKeyUsage = serverAuth

[alt_names]
DNS.1 = localhost
IP.1 = 127.0.0.1
"""
        )
        conf_path = conf.name

    try:
        subprocess.run(
            [
                "openssl",
                "req",
                "-x509",
                "-newkey",
                "rsa:2048",
                "-nodes",
                "-keyout",
                str(key_file),
                "-out",
                str(cert_file),
                "-days",
                "7",
                "-config",
                conf_path,
            ],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    finally:
        os.unlink(conf_path)

    return cert_file, key_file


class TlsEchoServer:
    def __init__(self, cert_file, key_file, host="127.0.0.1", port=0):
        self.cert_file = cert_file
        self.key_file = key_file
        self.host = host
        self.port = port
        self.sock = None
        self.thread = None
        self.stop_event = threading.Event()

    def start(self):
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(self.cert_file, self.key_file)

        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((self.host, self.port))
        sock.listen(1024)
        sock.settimeout(0.2)
        self.sock = sock
        self.port = sock.getsockname()[1]

        def serve():
            while not self.stop_event.is_set():
                try:
                    raw, _ = sock.accept()
                except socket.timeout:
                    continue
                except OSError:
                    break
                threading.Thread(target=self._handle, args=(context, raw), daemon=True).start()

        self.thread = threading.Thread(target=serve, daemon=True)
        self.thread.start()

    def _handle(self, context, raw):
        try:
            with context.wrap_socket(raw, server_side=True) as conn:
                while True:
                    data = conn.recv(65536)
                    if not data:
                        break
                    conn.sendall(data)
        except (OSError, ssl.SSLError):
            try:
                raw.close()
            except OSError:
                pass

    def stop(self):
        self.stop_event.set()
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
        if self.thread:
            self.thread.join(timeout=1.0)


def convert_value(raw):
    if raw == "true":
        return True
    if raw == "false":
        return False
    try:
        if "." in raw:
            return round(float(raw), 4)
        return int(raw)
    except ValueError:
        return raw


def parse_results(stdout):
    results = {}
    for line in stdout.splitlines():
        line = line.strip()
        if not line.startswith("RESULT "):
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        name = parts[1]
        fields = {"test": name}
        for item in parts[2:]:
            if "=" not in item:
                continue
            key, value = item.split("=", 1)
            fields[key] = convert_value(value)
        results[name] = fields
    return results


def run_xray_tls_client(xray_bin, host, port, mode, cert_file, timeout):
    env = os.environ.copy()
    env["SSL_CERT_FILE"] = str(cert_file)
    cmd = [xray_bin, str(SCRIPT_DIR / "tls_client.xr"), "--", host, str(port), mode]
    proc = subprocess.run(cmd, text=True, capture_output=True, env=env, timeout=timeout)
    if proc.returncode != 0:
        raise RuntimeError(
            "xray TLS client failed\n"
            f"command: {' '.join(cmd)}\n"
            f"stdout:\n{proc.stdout}\n"
            f"stderr:\n{proc.stderr}"
        )
    results = parse_results(proc.stdout)
    if not results:
        raise RuntimeError(f"xray TLS client produced no RESULT lines\nstdout:\n{proc.stdout}")
    return results


def main():
    parser = argparse.ArgumentParser(description="Run local TLS benchmarks for Xray")
    parser.add_argument("--xray", help="Path to xray binary")
    parser.add_argument("--host", default="127.0.0.1", help="TLS server host")
    parser.add_argument("--port", type=int, default=0, help="TLS server port (0 = ephemeral)")
    parser.add_argument("--test", default="all", help="handshake|latency|throughput|large|all")
    parser.add_argument("--output", "-o", help="Output JSON file")
    parser.add_argument("--cert-dir", help="Directory for generated TLS cert/key")
    parser.add_argument("--timeout", type=int, default=60, help="Subprocess timeout in seconds")
    args = parser.parse_args()

    xray_bin = args.xray or find_xray()
    if not xray_bin:
        print("ERROR: xray binary not found", file=sys.stderr)
        sys.exit(1)

    output_path = Path(args.output) if args.output else SCRIPT_DIR / "results/xray_tls.json"
    cert_dir = Path(args.cert_dir) if args.cert_dir else output_path.parent / "tls"
    cert_file, key_file = ensure_cert(cert_dir)

    server = TlsEchoServer(cert_file, key_file, args.host, args.port)
    server.start()
    try:
        time.sleep(0.1)
        results = run_xray_tls_client(
            xray_bin, args.host, server.port, args.test, cert_file, args.timeout
        )
    finally:
        server.stop()

    output = {
        "server": "xray_tls",
        "scenario": "tls",
        "host": args.host,
        "port": server.port,
        "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
        "results": results,
    }

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(output, indent=2))
    print(f"Results saved to {output_path}")
    print(json.dumps(output["results"], indent=2))


if __name__ == "__main__":
    main()

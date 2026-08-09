#!/usr/bin/env python3
"""Verify hosted VM/AOT netpoll state is retired before socket-fd reuse."""

from __future__ import annotations

import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "tests" / "aot" / "net_fd_reuse_close.xr"
EXPECTED = b"fd-reuse-ok\n\n"


def reserve_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def trigger_when_ready(port: int, process: subprocess.Popen[bytes]) -> None:
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        if process.poll() is not None:
            stdout, stderr = process.communicate()
            raise RuntimeError(
                "process exited before trigger listener was ready: "
                f"{process.returncode} stdout={stdout!r} stderr={stderr!r}"
            )
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1) as connection:
                connection.sendall(b"x")
                return
        except OSError:
            time.sleep(0.01)
    raise RuntimeError("trigger listener did not become ready")


def run_case(command: list[str]) -> None:
    trigger_port = reserve_port()
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as status:
        status.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        status.bind(("127.0.0.1", 0))
        status.listen(1)
        status.settimeout(5.0)
        status_port = int(status.getsockname()[1])
        environment = os.environ.copy()
        environment["XRAY_TRIGGER_PORT"] = str(trigger_port)
        environment["XRAY_STATUS_PORT"] = str(status_port)
        process = subprocess.Popen(
            command, cwd=ROOT, env=environment,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        try:
            trigger_when_ready(trigger_port, process)
            connection, _ = status.accept()
            with connection:
                connection.sendall(b"fd-reuse-ok\n")
            stdout, stderr = process.communicate(timeout=5.0)
        except BaseException:
            process.kill()
            process.wait()
            raise
    if process.returncode != 0:
        raise RuntimeError(
            f"{' '.join(command)} failed with {process.returncode}: "
            f"{stderr.decode('utf-8', 'replace')}"
        )
    if stdout != EXPECTED:
        raise RuntimeError(f"unexpected output from {' '.join(command)}: {stdout!r}")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: run_aot_net_fd_reuse.py <xray>")
    xray = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="xray-aot-net-fd-reuse-") as raw:
        binary = Path(raw) / "net-fd-reuse"
        subprocess.run(
            [str(xray), "build", "--native", "-O", "2", "--rebuild", "-o", str(binary),
             str(SOURCE)],
            cwd=ROOT, check=True, timeout=120,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        for _ in range(4):
            run_case([str(binary)])
    print("AOT net fd-reuse close regression: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

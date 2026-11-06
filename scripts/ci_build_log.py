#!/usr/bin/env python3
"""Keep byte-exact build logs and publish bounded failure annotations."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import threading

ERROR_LINE = re.compile(
    rb"\b(?:fatal\s+)?error(?:\s+[A-Z]+[0-9]+)?\s*:|"
    rb"^CMake Error\b|^FAILED:|^ninja:\s*(?:error|build stopped)|"
    rb"\b(?:Assertion|CHECK) failed\b", re.IGNORECASE
)
MAX_ANNOTATIONS = 20
MAX_LINE_BYTES = 8192


def escape_message(value: str) -> str:
    return value.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def run(command: list[str], path: Path) -> int:
    path.parent.mkdir(parents=True, exist_ok=True)
    lock = threading.Lock()
    errors: list[bytes] = []
    failures: list[BaseException] = []

    def remember(line: bytes, truncated: bool) -> None:
        if ERROR_LINE.search(line):
            with lock:
                if len(errors) < MAX_ANNOTATIONS:
                    errors.append(line + (b" [annotation truncated; raw log is complete]" if truncated else b""))

    def copy(source, output, raw, combined) -> None:
        pending = b""
        truncated = False
        try:
            while chunk := os.read(source.fileno(), 65536):
                raw.write(chunk)
                with lock:
                    combined.write(chunk)
                output.write(chunk)
                output.flush()
                for part in chunk.splitlines(keepends=True):
                    available = MAX_LINE_BYTES - len(pending)
                    pending += part[:available]
                    truncated |= len(part) > available
                    if part.endswith((b"\n", b"\r")):
                        remember(pending, truncated)
                        pending = b""
                        truncated = False
            if pending:
                remember(pending, truncated)
        except BaseException as error:
            failures.append(error)
        finally:
            source.close()

    with path.open("wb") as combined, path.with_suffix(".stdout").open("wb") as stdout, \
            path.with_suffix(".stderr").open("wb") as stderr:
        child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        threads = [
            threading.Thread(target=copy, args=(child.stdout, sys.stdout.buffer, stdout, combined)),
            threading.Thread(target=copy, args=(child.stderr, sys.stderr.buffer, stderr, combined)),
        ]
        for thread in threads:
            thread.start()
        status = child.wait()
        for thread in threads:
            thread.join()
    if status:
        messages = [line.decode("utf-8", errors="replace") for line in errors]
        if not messages:
            messages = [f"Build exited with status {status}; see {path} for original output."]
        sys.stdout.buffer.write(b"\n")
        for message in messages:
            sys.stdout.buffer.write(("::error::" + escape_message(message) + "\n").encode("utf-8"))
        sys.stdout.buffer.flush()
    if failures:
        raise RuntimeError("Build log capture failed") from failures[0]
    if status < 0 and os.name == "posix":
        # Preserve the child's signal status on POSIX instead of remapping it
        # to a different shell exit code through Python's SystemExit.
        if -status not in (signal.SIGKILL, signal.SIGSTOP):
            signal.signal(-status, signal.SIG_DFL)
        os.kill(os.getpid(), -status)
    return status


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("a build command is required after --")
    return run(command, args.log)


if __name__ == "__main__":
    raise SystemExit(main())

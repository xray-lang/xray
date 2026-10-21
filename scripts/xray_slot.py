#!/usr/bin/env python3
"""FIFO resource leases for local builds; operating-system locks own the slots."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import errno
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid
from xray_process import RecoveryRequired, run_child

if os.name == "nt":
    import ctypes
    from ctypes import wintypes
    import msvcrt
else:
    import fcntl


def save_json(path: Path, value: dict, timeout: float = 5) -> None:
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    deadline = time.monotonic() + timeout
    try:
        temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        while True:
            try:
                temporary.replace(path)
                return
            except PermissionError as error:
                # A Windows reader may omit FILE_SHARE_DELETE. Keep the old
                # complete record visible and retry only within this deadline.
                if os.name != "nt" or getattr(error, "winerror", None) not in (5, 32, 33) or time.monotonic() >= deadline:
                    raise
                time.sleep(0.02)
    finally:
        temporary.unlink(missing_ok=True)


class FileLock:
    def __init__(self, path: Path):
        self.path = path
        self.stream = None

    def acquire(self) -> bool:
        try:
            stream = self._open()
        except OSError as error:
            if getattr(error, "winerror", None) in (32, 33):
                return False
            raise
        try:
            if os.name == "nt":
                if stream.seek(0, 2) == 0:
                    stream.write(b"\0")
                    stream.flush()
                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            stream.close()
            if error.errno in (errno.EACCES, errno.EAGAIN, errno.EDEADLK):
                return False
            raise
        self.stream = stream
        return True

    def _open(self):
        if os.name != "nt":
            return self.path.open("a+b")
        # Preserve the Win32 distinction between access denial and sharing
        # contention; CRT fopen maps both errors to EACCES.
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                      ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        kernel.CreateFileW.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        handle = kernel.CreateFileW(str(self.path), 0xC0000000, 3, None, 4, 0x80, None)
        if handle == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            descriptor = msvcrt.open_osfhandle(handle, os.O_RDWR | os.O_BINARY)
        except BaseException:
            kernel.CloseHandle(handle)
            raise
        try:
            return os.fdopen(descriptor, "r+b")
        except BaseException:
            os.close(descriptor)
            raise

    def close(self) -> None:
        if self.stream is not None:
            self.stream.close()
            self.stream = None


@contextmanager
def mutex(path: Path, deadline: float):
    lock = FileLock(path)
    while not lock.acquire():
        if time.monotonic() >= deadline:
            raise TimeoutError(f"Timed out waiting for {path}")
        time.sleep(0.05)
    try:
        yield
    finally:
        lock.close()


class Lease:
    """A crashed running owner needs manual recovery, never age-based reclamation."""

    def __init__(self, root: Path, kind: str, priority: int = 1,
                 capacity: int | None = None, timeout: float = 7200,
                 payload: dict | None = None):
        self.root = root.resolve()
        self.kind = kind
        self.priority = priority
        self.capacity = capacity or (3 if kind == "build" else 1)
        self.deadline = time.monotonic() + timeout
        self.payload = payload or {}
        self.path = None
        self.live = None
        self.slot = None
        self.record = {}

    def _requests(self) -> list[dict]:
        pending = []
        for path in sorted((self.root / "requests").glob("*.json")):
            record = json.loads(path.read_text(encoding="utf-8"))
            if record["kind"] != self.kind or record["state"] not in (
                    "queued", "running", "NEEDS_RECOVERY"):
                continue
            if path != self.path and record["state"] != "NEEDS_RECOVERY":
                probe = FileLock(path.with_suffix(".live"))
                if probe.acquire():
                    probe.close()
                    record["state"] = ("cancelled" if record["state"] == "queued"
                                       else "NEEDS_RECOVERY")
                    save_json(path, record)
            if record["state"] == "queued":
                pending.append(record)
            elif record["state"] == "NEEDS_RECOVERY":
                raise RuntimeError(f"Resource owner requires recovery: {path}")
        return sorted(pending, key=lambda row: (row["priority"], row["sequence"]))

    def __enter__(self):
        (self.root / "requests").mkdir(parents=True, exist_ok=True)
        try:
            with mutex(self.root / "scheduler.lock", self.deadline):
                sequence_path = self.root / "sequence.json"
                sequence = (json.loads(sequence_path.read_text())["sequence"]
                            if sequence_path.exists() else 0) + 1
                save_json(sequence_path, {"sequence": sequence})
                self.path = self.root / "requests" / f"{sequence:020d}-{uuid.uuid4().hex}.json"
                self.live = FileLock(self.path.with_suffix(".live"))
                if not self.live.acquire():
                    raise RuntimeError("New request already has an owner")
                self.record = {"sequence": sequence, "kind": self.kind, "priority": self.priority,
                               "pid": os.getpid(), "created": time.time(), "state": "queued",
                               "payload": self.payload}
                save_json(self.path, self.record)
            notice = time.monotonic()
            while True:
                with mutex(self.root / "scheduler.lock", self.deadline):
                    pending = self._requests()
                    if pending and pending[0]["sequence"] == sequence:
                        for number in range(self.capacity):
                            slot = FileLock(self.root / f"{self.kind}-{number}.lock")
                            if slot.acquire():
                                self.slot = slot
                                self.record.update(state="running", slot=number, started=time.time())
                                save_json(self.path, self.record)
                                return self
                if time.monotonic() >= self.deadline:
                    raise TimeoutError(f"Timed out waiting for {self.kind} slot")
                if time.monotonic() - notice >= 30:
                    print(f"Waiting for {self.kind} slot; request {sequence}", flush=True)
                    notice = time.monotonic()
                time.sleep(0.1)
        except BaseException:
            self.__exit__(*sys.exc_info())
            raise

    def __exit__(self, exc_type, exc, traceback):
        try:
            if self.path is not None and self.record:
                # Publish completion before liveness can disappear, under the
                # same transaction used by abandoned-owner detection. Cleanup
                # gets its own deadline even when acquisition already expired.
                with mutex(self.root / "scheduler.lock", time.monotonic() + 30):
                    state = "NEEDS_RECOVERY" if isinstance(exc, RecoveryRequired) else ("failed" if exc_type else "finished")
                    self.record.update(state=state, finished=time.time())
                    save_json(self.path, self.record)
                    if self.slot is not None:
                        self.slot.close()
                    if self.live is not None:
                        self.live.close()
        finally:
            if self.slot is not None:
                self.slot.close()
            if self.live is not None:
                self.live.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kind", choices=("build", "heavy"), required=True)
    parser.add_argument("--priority", choices=("normal", "merge"), default="normal")
    parser.add_argument("--root", type=Path, default=Path(r"D:\xray-build\slots"))
    parser.add_argument("--timeout", type=float, default=7200)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("a program and its arguments are required after --")
    environment = os.environ.copy()
    environment["XR_BUILD_JOBS"] = "8" if args.kind == "build" else "4"
    with Lease(args.root, args.kind, 0 if args.priority == "merge" else 1,
               timeout=args.timeout, payload={"command": command}):
        result = run_child(command, env=environment)
        if result:
            raise subprocess.CalledProcessError(result, command)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.returncode)

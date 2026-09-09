"""Crash-safe cross-process ownership for one CMake/Ninja build tree."""

from __future__ import annotations

import errno
import json
import os
import sys
import time
from pathlib import Path
from typing import BinaryIO


DEFAULT_TIMEOUT_SECONDS = 600
POLL_SECONDS = 0.1
OWNER_RECORD_BYTES = 4096


def timeout_from_env() -> int:
    """Return the build-tree wait budget, rejecting ambiguous values."""
    raw = os.environ.get("XR_BUILD_LOCK_TIMEOUT", str(DEFAULT_TIMEOUT_SECONDS))
    try:
        timeout = int(raw)
    except ValueError as error:
        raise ValueError("XR_BUILD_LOCK_TIMEOUT must be an integer") from error
    if timeout < 0:
        raise ValueError("XR_BUILD_LOCK_TIMEOUT must be non-negative")
    return timeout


def lock_path(build_dir: Path) -> Path:
    """Use a canonical sidecar so configure and build resolve to one lock."""
    build = Path(build_dir).resolve()
    return build.parent / f".{build.name}.xray-build.lock"


def owner_path(build_dir: Path) -> Path:
    """Keep diagnostics outside the mandatory Windows byte-range lock."""
    return lock_path(build_dir).with_suffix(".lock.owner")


def _try_lock(handle: BinaryIO) -> bool:
    handle.seek(0)
    try:
        if os.name == "nt":
            import msvcrt

            msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl

            fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        return True
    except OSError as error:
        if error.errno in (errno.EACCES, errno.EAGAIN, errno.EDEADLK):
            return False
        raise


def _unlock(handle: BinaryIO) -> None:
    handle.seek(0)
    if os.name == "nt":
        import msvcrt

        msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
    else:
        import fcntl

        fcntl.flock(handle.fileno(), fcntl.LOCK_UN)


class BuildTreeLock:
    """Exclusive build/test lease released automatically when a process dies."""

    def __init__(self, build_dir: Path, timeout: int | None = None) -> None:
        self.build_dir = Path(build_dir).resolve()
        self.path = lock_path(self.build_dir)
        self.owner_path = owner_path(self.build_dir)
        self.timeout = timeout_from_env() if timeout is None else timeout
        if self.timeout < 0:
            raise ValueError("build lock timeout must be non-negative")
        self.waited_seconds = 0.0
        self._handle: BinaryIO | None = None

    def acquire(self) -> bool:
        if self._handle is not None:
            raise RuntimeError("build-tree lock is already held")
        self.path.parent.mkdir(parents=True, exist_ok=True)
        descriptor = os.open(self.path, os.O_RDWR | os.O_CREAT, 0o600)
        handle = os.fdopen(descriptor, "r+b", buffering=0)
        handle.seek(0, os.SEEK_END)
        if handle.tell() == 0:
            handle.write(b"\0")
            handle.flush()

        started = time.monotonic()
        deadline = started + self.timeout
        try:
            while not _try_lock(handle):
                now = time.monotonic()
                if now >= deadline:
                    self.waited_seconds = now - started
                    handle.close()
                    return False
                time.sleep(min(POLL_SECONDS, max(0.0, deadline - now)))
            self.waited_seconds = time.monotonic() - started
            self._handle = handle
            self._write_owner()
            return True
        except BaseException:
            handle.close()
            raise

    def _write_owner(self) -> None:
        assert self._handle is not None
        owner = {
            "pid": os.getpid(),
            "build_dir": str(self.build_dir),
            "command": sys.argv,
        }
        payload = json.dumps(owner, ensure_ascii=True, separators=(",", ":")).encode("utf-8")
        if len(payload) > OWNER_RECORD_BYTES - 2:
            payload = payload[:OWNER_RECORD_BYTES - 2]
        self.owner_path.write_bytes(payload + b"\n")

    def owner_text(self) -> str:
        try:
            with self.owner_path.open("rb") as handle:
                payload = handle.read(OWNER_RECORD_BYTES)
            return payload.decode("utf-8", errors="replace").strip()
        except OSError:
            return ""

    def release(self) -> None:
        handle, self._handle = self._handle, None
        if handle is None:
            return
        try:
            _unlock(handle)
        finally:
            handle.close()

    def __enter__(self) -> "BuildTreeLock":
        if not self.acquire():
            owner = self.owner_text()
            suffix = f"; last owner: {owner}" if owner else ""
            raise TimeoutError(f"timed out waiting for {self.path}{suffix}")
        return self

    def __exit__(self, exc_type, exc, tb) -> bool:
        self.release()
        return False

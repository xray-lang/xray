#!/usr/bin/env python3
"""Exercise exact identities and method shapes against the actual CLI server."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import queue
import subprocess
import threading
from typing import Any, BinaryIO


def read_exact(stream: BinaryIO, count: int) -> bytes:
    chunks: list[bytes] = []
    remaining = count
    while remaining:
        chunk = stream.read(remaining)
        if not chunk:
            raise EOFError("short LSP response body")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


class Session:
    def __init__(self, executable: Path) -> None:
        self.process = subprocess.Popen(
            [str(executable), "lsp", "--stdio"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            bufsize=0,
        )
        self.messages: queue.Queue[Any] = queue.Queue()
        self.errors: list[bytes] = []
        self.readers = [threading.Thread(target=self.read_stdout, daemon=True),
                        threading.Thread(target=self.read_stderr, daemon=True)]
        for reader in self.readers:
            reader.start()

    def read_stdout(self) -> None:
        assert self.process.stdout is not None
        try:
            while True:
                headers: dict[bytes, bytes] = {}
                while True:
                    line = self.process.stdout.readline()
                    if not line:
                        raise EOFError("server closed stdout")
                    if line == b"\r\n":
                        break
                    key, value = line.rstrip(b"\r\n").split(b":", 1)
                    headers[key.lower()] = value.strip()
                body = read_exact(self.process.stdout, int(headers[b"content-length"]))
                self.messages.put(json.loads(body))
        except BaseException as error:
            self.messages.put(error)

    def read_stderr(self) -> None:
        assert self.process.stderr is not None
        for line in self.process.stderr:
            self.errors.append(line)

    def send(self, *messages: dict[str, Any]) -> None:
        assert self.process.stdin is not None
        frames = []
        for message in messages:
            body = json.dumps(message, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
            frames.append(f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body)
        self.process.stdin.write(b"".join(frames))
        self.process.stdin.flush()

    def response(self, identity: Any, code: int | None = None) -> dict[str, Any]:
        try:
            item = self.messages.get(timeout=5)
        except queue.Empty as error:
            raise AssertionError("missing response; stderr=" + b"".join(self.errors)[-4000:].decode("utf-8", "replace")) from error
        if isinstance(item, BaseException):
            raise AssertionError(f"server response failed: {item}") from item
        assert isinstance(item, dict) and item.get("jsonrpc") == "2.0", item
        assert type(item.get("id")) is type(identity) and item.get("id") == identity, item
        assert ("result" in item) != ("error" in item), item
        if code is None:
            assert "result" in item, item
        else:
            assert item["error"]["code"] == code and isinstance(item["error"]["message"], str), item
        return item

    def close(self) -> None:
        if self.process.stdin:
            self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)
            raise AssertionError("server did not exit after shutdown/exit")
        finally:
            for reader in self.readers:
                reader.join(timeout=1)


def request(method: str, identity: Any) -> dict[str, Any]:
    return {"jsonrpc": "2.0", "id": identity, "method": method, "params": {}}


def run(executable: Path) -> None:
    session = Session(executable)
    try:
        initialize = request("initialize", "init\0😀")
        initialize["params"] = {"processId": None, "rootUri": None, "capabilities": {}}
        session.send(initialize)
        session.response("init\0😀")
        session.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        for identity in (0, -2147483648, 2147483647, "", "0", "a\0b😀"):
            session.send(request("xray/unknownIdentityProbe", identity))
            session.response(identity, -32601)
        session.send(request("xray/unknownIdentityProbe", 1.0))
        session.response(1, -32601)
        for invalid in (None, True, 0.5, 2147483648, -2147483649):
            session.send(request("xray/unknownIdentityProbe", invalid))
            session.response(None, -32600)
        # Every invalid method shape must release admission, even beyond capacity.
        for identity in range(100, 170):
            session.send(request("initialized", identity))
            session.response(identity, -32600)
        session.send({"jsonrpc": "2.0", "method": "textDocument/hover", "params": {}},
                     request("xray/notificationBarrier", "barrier"))
        session.response("barrier", -32601)
        session.send(request("$/cancelRequest", "cancel-as-request"))
        session.response("cancel-as-request", -32600)
        session.send(request("shutdown", "shutdown\0😀"))
        assert session.response("shutdown\0😀")["result"] is None
        session.send({"jsonrpc": "2.0", "method": "exit", "params": {}})
    finally:
        session.close()
    assert session.process.returncode == 0, session.process.returncode


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--xray", required=True, type=Path)
    options = parser.parse_args()
    run(options.xray.resolve(strict=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

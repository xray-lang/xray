#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import queue
import subprocess
import threading
from pathlib import Path
from typing import Any


def frame(message: dict[str, Any]) -> bytes:
    body = json.dumps(message, separators=(",", ":")).encode("utf-8")
    return f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body


class LspSession:
    def __init__(self, xray: Path) -> None:
        self.proc = subprocess.Popen(
            [str(xray), "lsp", "--stdio"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
        )
        if self.proc.stdin is None or self.proc.stdout is None or self.proc.stderr is None:
            raise RuntimeError("failed to open LSP server pipes")
        self._stdout: queue.Queue[dict[str, Any] | BaseException | None] = queue.Queue()
        self._stderr: list[bytes] = []
        self._stdout_thread = threading.Thread(target=self._read_stdout, daemon=True)
        self._stderr_thread = threading.Thread(target=self._read_stderr, daemon=True)
        self._stdout_thread.start()
        self._stderr_thread.start()

    def _read_stdout(self) -> None:
        assert self.proc.stdout is not None
        try:
            while True:
                headers: dict[str, str] = {}
                while True:
                    line = self.proc.stdout.readline()
                    if not line:
                        self._stdout.put(None)
                        return
                    if line in (b"\r\n", b"\n"):
                        break
                    name, value = line.decode("ascii").split(":", 1)
                    headers[name.lower()] = value.strip()
                length = int(headers["content-length"])
                body = self.proc.stdout.read(length)
                if len(body) != length:
                    raise EOFError("short LSP response body")
                self._stdout.put(json.loads(body))
        except BaseException as exc:
            self._stdout.put(exc)

    def _read_stderr(self) -> None:
        assert self.proc.stderr is not None
        for line in self.proc.stderr:
            self._stderr.append(line)

    def send(self, *messages: dict[str, Any]) -> None:
        assert self.proc.stdin is not None
        self.proc.stdin.write(b"".join(frame(message) for message in messages))
        self.proc.stdin.flush()

    def recv_id(self, request_id: int, timeout: float = 5.0) -> dict[str, Any]:
        while True:
            try:
                item = self._stdout.get(timeout=timeout)
            except queue.Empty as exc:
                raise AssertionError(
                    f"timed out waiting for LSP response {request_id}; stderr={self.stderr_text()}"
                ) from exc
            if item is None:
                raise AssertionError(
                    f"LSP server closed stdout before response {request_id}; stderr={self.stderr_text()}"
                )
            if isinstance(item, BaseException):
                raise AssertionError(
                    f"failed to read LSP response: {item}; stderr={self.stderr_text()}"
                ) from item
            if item.get("id") == request_id:
                return item

    def close(self) -> None:
        if self.proc.stdin is not None:
            try:
                self.proc.stdin.close()
            except BrokenPipeError:
                pass
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=5)
        self._stdout_thread.join(timeout=1)
        self._stderr_thread.join(timeout=1)

    def stderr_text(self) -> str:
        return b"".join(self._stderr)[-4000:].decode("utf-8", errors="replace")


def request(method: str, request_id: int, params: dict[str, Any]) -> dict[str, Any]:
    return {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params}


def notification(method: str, params: dict[str, Any]) -> dict[str, Any]:
    return {"jsonrpc": "2.0", "method": method, "params": params}


def test_batched_open_and_completion(xray: Path) -> None:
    session = LspSession(xray)
    try:
        session.send(
            request(
                "initialize",
                1,
                {"processId": None, "rootUri": None, "capabilities": {}},
            )
        )
        initialized = session.recv_id(1)
        completion_provider = initialized["result"]["capabilities"]["completionProvider"]
        assert "." in completion_provider["triggerCharacters"], initialized

        source = "fn main() {\n    var a = 10\n    a.\n}\n"
        uri = "file:///private/tmp/xray_lsp_completion_transcript.xr"

        # Deliberately write all three frames in one syscall.  The server must
        # drain frames already buffered in user space instead of waiting for a
        # second kernel readability event that will never arrive.
        session.send(
            notification("initialized", {}),
            notification(
                "textDocument/didOpen",
                {
                    "textDocument": {
                        "uri": uri,
                        "languageId": "xray",
                        "version": 1,
                        "text": source,
                    }
                },
            ),
            request(
                "textDocument/completion",
                2,
                {
                    "textDocument": {"uri": uri},
                    "position": {"line": 2, "character": 6},
                    "context": {"triggerKind": 2, "triggerCharacter": "."},
                },
            ),
        )

        response = session.recv_id(2)
        result = response["result"]
        items = result["items"] if isinstance(result, dict) else result
        labels = {item["label"] for item in items}
        assert {"abs", "toString", "checkedAdd"} <= labels, labels

        session.send(request("shutdown", 3, {}))
        shutdown = session.recv_id(3)
        assert shutdown.get("result") is None, shutdown
        session.send(notification("exit", {}))
    finally:
        session.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--xray", type=Path, required=True)
    args = parser.parse_args()
    test_batched_open_and_completion(args.xray.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

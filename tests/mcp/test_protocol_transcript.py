#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import queue
import subprocess
import threading
from pathlib import Path
from typing import Any


ERR_PARSE = -32700
ERR_METHOD_NOT_FOUND = -32601
ERR_INVALID_PARAMS = -32602
ERR_NOT_INITIALIZED = -32002
ERR_ALREADY_INITIALIZED = -32003


class McpSession:
    def __init__(self, xray: Path, *, enable_runner: bool = False) -> None:
        cmd = [str(xray), "mcp-server"]
        if enable_runner:
            cmd.append("--enable-runner")
        self.proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        if self.proc.stdin is None or self.proc.stdout is None or self.proc.stderr is None:
            raise RuntimeError("failed to open MCP server pipes")
        self._stdout: queue.Queue[str | None] = queue.Queue()
        self._stderr: list[str] = []
        self._stdout_thread = threading.Thread(target=self._read_stdout, daemon=True)
        self._stderr_thread = threading.Thread(target=self._read_stderr, daemon=True)
        self._stdout_thread.start()
        self._stderr_thread.start()

    def _read_stdout(self) -> None:
        assert self.proc.stdout is not None
        for line in self.proc.stdout:
            self._stdout.put(line)
        self._stdout.put(None)

    def _read_stderr(self) -> None:
        assert self.proc.stderr is not None
        for line in self.proc.stderr:
            self._stderr.append(line)

    def send(self, message: dict[str, Any]) -> None:
        assert self.proc.stdin is not None
        self.proc.stdin.write(json.dumps(message, separators=(",", ":")) + "\n")
        self.proc.stdin.flush()

    def send_raw(self, line: str) -> None:
        assert self.proc.stdin is not None
        self.proc.stdin.write(line)
        self.proc.stdin.flush()

    def recv(self, timeout: float = 5.0) -> dict[str, Any]:
        try:
            line = self._stdout.get(timeout=timeout)
        except queue.Empty as exc:
            raise AssertionError(f"timed out waiting for response; stderr={self.stderr_text()}") from exc
        if line is None:
            raise AssertionError(f"server closed stdout; stderr={self.stderr_text()}")
        try:
            return json.loads(line)
        except json.JSONDecodeError as exc:
            raise AssertionError(f"stdout line is not JSON: {line!r}; stderr={self.stderr_text()}") from exc

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
        if self.proc.returncode != 0:
            raise AssertionError(f"MCP server exited with {self.proc.returncode}; stderr={self.stderr_text()}")

    def stderr_text(self) -> str:
        return "".join(self._stderr)[-4000:]


def request(method: str, req_id: int, params: dict[str, Any] | None = None) -> dict[str, Any]:
    msg: dict[str, Any] = {"jsonrpc": "2.0", "id": req_id, "method": method}
    if params is not None:
        msg["params"] = params
    return msg


def notification(method: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
    msg: dict[str, Any] = {"jsonrpc": "2.0", "method": method}
    if params is not None:
        msg["params"] = params
    return msg


def assert_error(response: dict[str, Any], req_id: int | None, code: int) -> None:
    assert response.get("id") == req_id, response
    err = response.get("error")
    assert isinstance(err, dict), response
    assert err.get("code") == code, response


def initialize(session: McpSession, req_id: int = 1) -> dict[str, Any]:
    session.send(
        request(
            "initialize",
            req_id,
            {
                "protocolVersion": "2025-03-26",
                "capabilities": {},
                "clientInfo": {"name": "xray-test", "version": "1.0"},
            },
        )
    )
    response = session.recv()
    assert response.get("id") == req_id, response
    result = response.get("result")
    assert isinstance(result, dict), response
    assert result.get("protocolVersion") == "2025-03-26", response
    return response


def mark_initialized(session: McpSession) -> None:
    session.send(notification("notifications/initialized"))


def test_lifecycle_and_tools_list(xray: Path) -> None:
    session = McpSession(xray)
    try:
        initialize(session, 1)
        session.send(request("tools/list", 2, {}))
        assert_error(session.recv(), 2, ERR_NOT_INITIALIZED)

        mark_initialized(session)
        session.send(request("tools/list", 3, {}))
        response = session.recv()
        assert response.get("id") == 3, response
        tools = response["result"]["tools"]
        names = [tool["name"] for tool in tools]
        assert "xray_analyze" in names, names
        assert "xray_run" not in names, names

        session.send(request("initialize", 4, {}))
        assert_error(session.recv(), 4, ERR_ALREADY_INITIALIZED)
    finally:
        session.close()


def test_unknown_request_and_notification(xray: Path) -> None:
    session = McpSession(xray)
    try:
        initialize(session, 1)
        mark_initialized(session)

        session.send(request("xray/unknown", 2, {}))
        assert_error(session.recv(), 2, ERR_METHOD_NOT_FOUND)

        session.send(notification("xray/unknown"))
        session.send(request("ping", 3, {}))
        response = session.recv()
        assert response.get("id") == 3, response
        assert response.get("result") == {}, response
    finally:
        session.close()


def test_parse_error_and_content_length_line(xray: Path) -> None:
    session = McpSession(xray)
    try:
        session.send_raw("{not json}\n")
        assert_error(session.recv(), None, ERR_PARSE)

        session.send_raw("Content-Length: 2\n")
        assert_error(session.recv(), None, ERR_PARSE)
    finally:
        session.close()


def test_runner_stdout_is_protocol_isolated(xray: Path) -> None:
    session = McpSession(xray, enable_runner=True)
    try:
        initialize(session, 1)
        mark_initialized(session)
        session.send(
            request(
                "tools/call",
                2,
                {
                    "name": "xray_run",
                    "arguments": {"code": "print(\"runner-out\")\n", "timeoutMs": 1000},
                },
            )
        )
        response = session.recv()
        assert response.get("id") == 2, response
        result = response.get("result")
        assert isinstance(result, dict), response
        assert result.get("isError") in (False, None), response
        structured = result.get("structuredContent")
        assert isinstance(structured, dict), response
        assert structured.get("ok") is True, response
        assert structured.get("stdout") == "runner-out\n", response
    finally:
        session.close()


def test_resources_and_prompts_protocol_paths(xray: Path) -> None:
    session = McpSession(xray)
    try:
        initialize(session, 1)
        mark_initialized(session)

        session.send(request("resources/templates/list", 2, {}))
        templates_response = session.recv()
        assert templates_response.get("id") == 2, templates_response
        templates = templates_response["result"]["resourceTemplates"]
        uri_templates = [template["uriTemplate"] for template in templates]
        assert "xray://spec/topic/{name}" in uri_templates, uri_templates
        assert "xray://stdlib/{module}" in uri_templates, uri_templates

        session.send(request("prompts/list", 3, {}))
        prompts_response = session.recv()
        assert prompts_response.get("id") == 3, prompts_response
        prompts = prompts_response["result"]["prompts"]
        prompt_names = [prompt["name"] for prompt in prompts]
        assert "code-review" in prompt_names, prompt_names

        session.send(
            request(
                "prompts/get",
                4,
                {
                    "name": "code-review",
                    "arguments": {"code": "let x = 1\nprint(x)\n"},
                },
            )
        )
        prompt_response = session.recv()
        assert prompt_response.get("id") == 4, prompt_response
        prompt_result = prompt_response.get("result")
        assert isinstance(prompt_result, dict), prompt_response
        messages = prompt_result.get("messages")
        assert isinstance(messages, list), prompt_response
        assert len(messages) >= 2, prompt_response
    finally:
        session.close()


def test_mixed_ndjson_request_notification_and_error(xray: Path) -> None:
    session = McpSession(xray)
    try:
        initialize(session, 1)
        mark_initialized(session)

        session.send(request("ping", 2, {}))
        session.send(notification("xray/unknown"))
        session.send(request("xray/unknown", 3, {}))
        session.send(request("tools/list", 4, {}))

        ping_response = session.recv()
        assert ping_response.get("id") == 2, ping_response
        assert ping_response.get("result") == {}, ping_response

        unknown_response = session.recv()
        assert_error(unknown_response, 3, ERR_METHOD_NOT_FOUND)

        tools_response = session.recv()
        assert tools_response.get("id") == 4, tools_response
        assert isinstance(tools_response.get("result"), dict), tools_response
    finally:
        session.close()


def test_tools_call_invalid_params_and_structured_content(xray: Path) -> None:
    session = McpSession(xray)
    try:
        initialize(session, 1)
        mark_initialized(session)

        session.send(
            request(
                "tools/call",
                2,
                {
                    "name": "xray_format",
                    "arguments": {"indentSize": 2},
                },
            )
        )
        assert_error(session.recv(), 2, ERR_INVALID_PARAMS)

        session.send(
            request(
                "tools/call",
                3,
                {
                    "name": "xray_format",
                    "arguments": {"code": "let x=1\nprint(x)\n", "indentSize": 2},
                },
            )
        )
        response = session.recv()
        assert response.get("id") == 3, response
        result = response.get("result")
        assert isinstance(result, dict), response
        assert result.get("isError") in (False, None), response
        structured = result.get("structuredContent")
        assert isinstance(structured, dict), response
        assert isinstance(structured.get("formattedCode"), str), response
        assert structured.get("indentSize") == 2, response
    finally:
        session.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--xray", required=True, type=Path)
    args = parser.parse_args()

    tests = [
        test_lifecycle_and_tools_list,
        test_unknown_request_and_notification,
        test_parse_error_and_content_length_line,
        test_runner_stdout_is_protocol_isolated,
        test_resources_and_prompts_protocol_paths,
        test_mixed_ndjson_request_notification_and_error,
        test_tools_call_invalid_params_and_structured_content,
    ]
    for test in tests:
        test(args.xray)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

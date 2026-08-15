#!/usr/bin/env python3
"""Fail-closed execution evidence for the bounded XTP fuzz matrix."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import subprocess
import sys
import time
from pathlib import Path


COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")
FUZZ_RE = re.compile(
    r"typed XTP deterministic mutation matrix passed: "
    r"executed=(?P<executed>\d+) mutations=(?P<mutations>\d+) "
    r"sanitizer=(?P<sanitizer>[a-z0-9-]+)"
)
RESOURCE_RE = re.compile(r"^XTP resource ladder:", re.MULTILINE)
RESOURCE_METRICS_RE = re.compile(
    r"^XTP resource ladder:.*wall-ms=(?P<wall>[0-9]+(?:\.[0-9]+)?) "
    r"peak-bytes=(?P<rss>[0-9]+)$",
    re.MULTILINE,
)


def fail(message: str) -> int:
    print(f"FAIL: {message}", file=sys.stderr)
    return 1


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--fuzzer", type=Path, required=True)
    parser.add_argument("--resource-stress", type=Path, required=True)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--expected-commit", required=True)
    parser.add_argument(
        "--sanitizer", choices=("release", "asan-ubsan", "tsan"), required=True
    )
    parser.add_argument("--timeout", type=float, default=90.0)
    parser.add_argument("--max-resource-wall-ms", type=float, default=30000.0)
    parser.add_argument("--max-resource-rss-bytes", type=int, default=536870912)
    parser.add_argument("--host-os", default=platform.system().lower())
    return parser.parse_args(argv)


def validate_executable(path: Path, label: str) -> str | None:
    if not path.is_file():
        return f"missing {label}: {path}"
    if not os.access(path, os.X_OK):
        return f"non-executable {label}: {path}"
    return None


def run_command(command: list[str], timeout: float) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        capture_output=True,
        check=False,
        encoding="utf-8",
        errors="replace",
        text=True,
        timeout=timeout,
    )


def verify_runtime_identity(runtime: Path, expected_commit: str, timeout: float) -> str | None:
    try:
        completed = run_command([str(runtime), "--version", "--json"], timeout)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return f"runtime identity probe could not execute: {exc}"
    if completed.returncode != 0:
        return f"runtime identity probe exited {completed.returncode}"
    try:
        identity = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        return f"runtime identity is not JSON: {exc}"
    if identity.get("schema") != 1:
        return "runtime identity schema is not 1"
    if identity.get("commit") != expected_commit:
        return (
            "runtime identity mismatch: expected "
            f"{expected_commit}, got {identity.get('commit')!r}"
        )
    if identity.get("dirty") is not False:
        return "runtime identity is dirty"
    return None


def verify_fuzzer_output(output: str, sanitizer: str) -> str | None:
    match = FUZZ_RE.search(output)
    if match is None:
        return "fuzzer did not emit the deterministic mutation evidence"
    executed = int(match.group("executed"))
    mutations = int(match.group("mutations"))
    if executed == 0:
        return "fuzzer executed zero deterministic mutations"
    if executed != 26 or mutations != 26:
        return f"fuzzer evidence must cover 26 mutations, got {executed}/{mutations}"
    if match.group("sanitizer") != sanitizer:
        return (
            "fuzzer sanitizer identity mismatch: expected "
            f"{sanitizer}, got {match.group('sanitizer')}"
        )
    return None


def corpus_identity(corpus: Path) -> tuple[str, int] | str:
    if not corpus.is_dir():
        return f"missing corpus directory: {corpus}"
    files = sorted(path for path in corpus.rglob("*") if path.is_file())
    if not files:
        return f"corpus has zero inputs: {corpus}"
    digest = hashlib.sha256()
    for path in files:
        relative = path.relative_to(corpus).as_posix().encode("utf-8")
        content = path.read_bytes()
        if not content:
            return f"corpus contains an empty input: {path}"
        digest.update(len(relative).to_bytes(4, "little"))
        digest.update(relative)
        digest.update(len(content).to_bytes(8, "little"))
        digest.update(content)
    return digest.hexdigest(), len(files)


def verify_resource_output(output: str, max_wall_ms: float,
                           max_rss_bytes: int) -> str | None:
    ladders = len(RESOURCE_RE.findall(output))
    if ladders == 0:
        return "resource stress executed zero size-ladder cases"
    if ladders != 4:
        return f"resource stress must report four size-ladder cases, got {ladders}"
    if "XTP resource stress tests passed" not in output:
        return "resource stress did not report a complete bounded run"
    metrics = list(RESOURCE_METRICS_RE.finditer(output))
    if len(metrics) != 4:
        return "resource stress omitted wall/RSS metrics for a size-ladder case"
    for match in metrics:
        if float(match.group("wall")) > max_wall_ms:
            return f"resource ladder wall time exceeds {max_wall_ms:.3f}ms"
        if int(match.group("rss")) > max_rss_bytes:
            return f"resource ladder RSS exceeds {max_rss_bytes} bytes"
    return None


def run_evidence(args: argparse.Namespace) -> int:
    expected_commit = args.expected_commit.lower()
    if COMMIT_RE.fullmatch(expected_commit) is None:
        return fail("expected commit must be a 40-character lowercase SHA-1")
    if args.timeout <= 0:
        return fail("timeout must be positive")
    if args.max_resource_wall_ms <= 0 or args.max_resource_rss_bytes <= 0:
        return fail("resource budgets must be positive")
    if args.sanitizer == "tsan" and args.host_os.lower().startswith("windows"):
        return fail("ThreadSanitizer is unsupported on Windows; this lane is red, not skipped")

    for path, label in (
        (args.runtime, "runtime"),
        (args.fuzzer, "fuzzer"),
        (args.resource_stress, "resource stress executable"),
    ):
        error = validate_executable(path, label)
        if error is not None:
            return fail(error)

    corpus = corpus_identity(args.corpus)
    if isinstance(corpus, str):
        return fail(corpus)
    corpus_digest, corpus_inputs = corpus

    error = verify_runtime_identity(args.runtime, expected_commit, args.timeout)
    if error is not None:
        return fail(error)

    fuzzer_started = time.monotonic()
    try:
        fuzzer = run_command([str(args.fuzzer)], args.timeout)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return fail(f"fuzzer could not execute: {exc}")
    fuzzer_seconds = time.monotonic() - fuzzer_started
    if fuzzer.returncode != 0:
        detail = (fuzzer.stdout + fuzzer.stderr).strip()
        return fail(f"fuzzer exited {fuzzer.returncode}: {detail}")
    error = verify_fuzzer_output(fuzzer.stdout, args.sanitizer)
    if error is not None:
        return fail(error)

    resource_started = time.monotonic()
    try:
        resource = run_command([str(args.resource_stress)], args.timeout)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return fail(f"resource stress could not execute: {exc}")
    resource_seconds = time.monotonic() - resource_started
    if resource.returncode != 0:
        return fail(f"resource stress exited {resource.returncode}: {resource.stderr.strip()}")
    error = verify_resource_output(resource.stdout, args.max_resource_wall_ms,
                                   args.max_resource_rss_bytes)
    if error is not None:
        return fail(error)

    print(
        "PASS: XTP fuzz evidence "
        f"commit={expected_commit} sanitizer={args.sanitizer} "
        f"mutations=26 resource_ladders=4 fuzzer_seconds={fuzzer_seconds:.3f} "
        f"resource_seconds={resource_seconds:.3f} corpus_inputs={corpus_inputs} "
        f"corpus_sha256={corpus_digest}"
    )
    return 0


def main(argv: list[str] | None = None) -> int:
    return run_evidence(parse_args(sys.argv[1:] if argv is None else argv))


if __name__ == "__main__":
    raise SystemExit(main())

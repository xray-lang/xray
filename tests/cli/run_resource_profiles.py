"""Qualify unlimited product execution and explicit finite sandbox profiles.

Every case invokes the real CLI or a program it built. Elapsed times identify
slow or timed-out commands; they are not performance benchmark measurements.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from xraytest import proc


RESOURCE_DIAGNOSTIC = b"XR_RUN_6003: canonical execution exceeded its resource budget\n"
FIB_SOURCE = """fn fib(n: i64) -> i64 {
    if (n <= 1) { return n }
    return fib(n - 1) + fib(n - 2)
}
print(fib(30))
"""
PUSH_SOURCE = """var values: Array<i64> = []
for (var i = 0; i < 10000000; i++) {
    values.push(i)
}
print(len(values))
print(values[9999999])
"""
TEST_SOURCE = """@test
fn resourceProfileAcceptsSmallTests() {
    assert(6 * 7 == 42)
}
"""


@dataclass(frozen=True)
class Case:
    name: str
    command: list[str | Path]
    timeout: float
    exit_code: int = 0
    stdout: bytes | None = b"42\n"
    stderr: bytes | None = b""
    usage_line: bytes | None = None
    artifact: Path | None = None


def source(directory: Path, name: str, text: str) -> Path:
    path = directory / name
    path.write_text(text, encoding="utf-8", newline="\n")
    return path


def basic_cases(binary: Path, directory: Path, short_timeout: float,
                build_timeout: float) -> list[Case]:
    simple = source(directory, "simple.xr", "print(42)\n")
    tests = source(directory, "profile_test.xr", TEST_SOURCE)
    executable = directory / ("profile.exe" if os.name == "nt" else "profile")
    usage = f"Usage: {executable} [--sandbox]".encode("utf-8")
    return [
        Case("run-default-small", [binary, "run", simple], short_timeout),
        Case("run-sandbox-small", [binary, "run", "--sandbox", simple], short_timeout),
        Case("test-default-small", [binary, "test", "--quiet", tests], short_timeout,
             stdout=b""),
        Case("test-sandbox-small", [binary, "test", "--sandbox", "--quiet", tests],
             short_timeout, stdout=b""),
        Case("build-native-small", [binary, "build", simple, "-o", executable],
             build_timeout, stdout=None, stderr=None, artifact=executable),
        Case("native-default-small", [executable], short_timeout),
        Case("native-sandbox-small", [executable, "--sandbox"], short_timeout),
        Case("native-unknown-option", [executable, "--unknown-resource-profile"],
             short_timeout, exit_code=3, stdout=b"", stderr=None, usage_line=usage),
        Case("native-duplicate-sandbox", [executable, "--sandbox", "--sandbox"],
             short_timeout, exit_code=3, stdout=b"", stderr=None, usage_line=usage),
    ]


def large_cases(binary: Path, directory: Path, large_timeout: float,
                sandbox_timeout: float) -> list[Case]:
    cases = []
    for name, text, expected in (
        ("fib30", FIB_SOURCE, b"832040\n"),
        ("push10000000", PUSH_SOURCE, b"10000000\n9999999\n"),
    ):
        path = source(directory, name + ".xr", text)
        cases.append(Case(name + "-default", [binary, "run", path], large_timeout,
                          stdout=expected))
        cases.append(Case(name + "-sandbox", [binary, "run", "--sandbox", path],
                          sandbox_timeout, exit_code=1, stdout=b"",
                          stderr=RESOURCE_DIAGNOSTIC))
    return cases


def verdict(case: Case, result: proc.ProcResult) -> list[str]:
    failures = []
    if result.timed_out:
        failures.append(f"command exceeded its {case.timeout:g}-second timeout")
    if result.returncode != case.exit_code:
        failures.append(f"exit {result.returncode}, expected {case.exit_code}")
    if case.stdout is not None and result.stdout != case.stdout:
        failures.append(f"stdout differs: expected {case.stdout!r}, got {result.stdout!r}")
    if case.stderr is not None and result.stderr != case.stderr:
        failures.append(f"stderr differs: expected {case.stderr!r}, got {result.stderr!r}")
    # The native launcher's usage rejection precedes binary stream setup on
    # Windows. Only this diagnostic accepts the platform's CRLF line ending.
    if case.usage_line is not None and result.stderr.splitlines() != [case.usage_line]:
        failures.append(f"usage diagnostic differs: got {result.stderr!r}")
    if case.artifact is not None and (
            not case.artifact.is_file() or case.artifact.stat().st_size == 0):
        failures.append("successful native build did not produce a nonempty executable")
    return failures


def write_report(paths: tuple[Path, ...], report: dict) -> None:
    text = json.dumps(report, indent=2, ensure_ascii=True) + "\n"
    for path in paths:
        path.write_text(text, encoding="utf-8", newline="\n")


def execute(case: Case, root: Path, environment: dict[str, str], logs: Path) -> dict:
    logs.mkdir()
    command = [str(part) for part in case.command]
    metadata = {"id": case.name, "command": command, "cwd": str(root),
                "timeout_seconds": case.timeout, "expected_exit_code": case.exit_code}
    (logs / "command.json").write_text(json.dumps(metadata, indent=2) + "\n",
                                       encoding="utf-8", newline="\n")
    print(f"RUN {case.name}: timeout={case.timeout:g}s", flush=True)
    started = time.monotonic()
    result = proc.run(command, cwd=root, env=environment, timeout=case.timeout)
    seconds = time.monotonic() - started
    (logs / "stdout.bin").write_bytes(result.stdout)
    (logs / "stderr.bin").write_bytes(result.stderr)
    failures = verdict(case, result)
    metadata.update({"status": "fail" if failures else "pass", "executed": True,
                     "exit_code": result.returncode, "timed_out": result.timed_out,
                     "seconds": round(seconds, 3), "failures": failures,
                     "stdout": result.stdout.decode("utf-8", errors="replace"),
                     "stderr": result.stderr.decode("utf-8", errors="replace"),
                     "stdout_file": str(logs / "stdout.bin"),
                     "stderr_file": str(logs / "stderr.bin")})
    (logs / "result.json").write_text(json.dumps(metadata, indent=2) + "\n",
                                      encoding="utf-8", newline="\n")
    print(f"{metadata['status'].upper()} {case.name}: exit={result.returncode} "
          f"elapsed={seconds:.3f}s" + ("; " + "; ".join(failures) if failures else ""),
          flush=True)
    return metadata


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--suite", choices=("basic", "large"), default="basic")
    parser.add_argument("--short-timeout", type=float, default=15)
    parser.add_argument("--build-timeout", type=float, default=120)
    parser.add_argument("--large-timeout", type=float, default=300)
    parser.add_argument("--sandbox-timeout", type=float, default=120)
    args = parser.parse_args()
    for name in ("short_timeout", "build_timeout", "large_timeout", "sandbox_timeout"):
        value = getattr(args, name)
        if not math.isfinite(value) or value <= 0:
            parser.error(f"--{name.replace('_', '-')} must be finite and positive")
    binary, root, output = args.binary.resolve(), args.root.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {"schema": 1, "gate": "running", "suite": args.suite,
              "binary": str(binary), "root": str(root), "cases": [],
              "timing_purpose": "functional timeout observation, not a performance benchmark"}
    report_paths = (output / "results.json",)
    write_report(report_paths, report)
    try:
        directory = Path(tempfile.mkdtemp(prefix="run-", dir=output))
        report_paths += (directory / "results.json",)
        report["run_directory"] = str(directory)
        environment = dict(os.environ, XRAY_STDLIB_PATH=str(root / "stdlib"), NO_COLOR="1")
        cases = basic_cases(binary, directory, args.short_timeout, args.build_timeout) \
            if args.suite == "basic" else large_cases(binary, directory, args.large_timeout,
                                                     args.sandbox_timeout)
        report["planned_cases"] = [case.name for case in cases]
        report["command_timeout_sum_seconds"] = sum(case.timeout for case in cases)
        write_report(report_paths, report)
        for case in cases:
            row = execute(case, root, environment, directory / case.name)
            report["cases"].append(row)
            write_report(report_paths, report)
        failed = sum(row["status"] == "fail" for row in report["cases"])
        report.update(gate="fail" if failed else "pass", passed=len(cases) - failed,
                      failed=failed)
        write_report(report_paths, report)
        print(f"resource profiles ({args.suite}): {len(cases) - failed} passed, "
              f"{failed} failed; evidence: {report_paths[0]}")
        return int(failed != 0)
    except (OSError, ValueError) as error:
        report.update(gate="fail", error=str(error))
        write_report(report_paths, report)
        print(f"resource profiles: FAIL infrastructure: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

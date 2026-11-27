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


# The sandbox is a product option that is temporarily unavailable on every
# execution path. A generated program owns no resource profile, so it rejects
# the option instead of silently running unrestricted.
RUN_SANDBOX_DIAGNOSTIC = b"xray run: sandbox execution is temporarily unavailable\n"
TEST_SANDBOX_DIAGNOSTIC = b"xray test: sandbox execution is temporarily unavailable\n"
TEST_ROLE_REPORT = b"xray test: 1 passed, 0 failed, 0 skipped\n"
NATIVE_SANDBOX_DIAGNOSTIC = b"error: --sandbox is not available in a generated program"
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


# A diagnostic line is either one exact byte string, or the set of byte strings
# the same text can take when the platform decides how a path is encoded.
Line = bytes | tuple[bytes, ...]


@dataclass(frozen=True)
class Case:
    name: str
    command: list[str | Path]
    timeout: float
    exit_code: int = 0
    stdout: bytes | None = b"42\n"
    stderr: bytes | None = b""
    stderr_lines: tuple[Line, ...] | None = None
    artifact: Path | None = None
    test_report: tuple[Path, dict] | None = None


def usage_line(executable: Path) -> tuple[bytes, ...]:
    """The program prints the `argv[0]` it was given. Windows hands a C `main`
    the console code page bytes of that argument, so the same path can arrive in
    another encoding than the one this script holds it in."""
    text = f"Usage: {executable}"
    variants = [text.encode("utf-8")]
    if os.name == "nt":
        try:
            variants.append(text.encode("mbcs"))
        except (LookupError, UnicodeEncodeError):
            pass  # A path outside the code page has no second form.
    return tuple(dict.fromkeys(variants))


def source(directory: Path, name: str, text: str) -> Path:
    path = directory / name
    path.write_text(text, encoding="utf-8", newline="\n")
    return path


def vm_cases(binary: Path, directory: Path, short_timeout: float) -> list[Case]:
    simple = source(directory, "simple.xr", "print(42)\n")
    tests = source(directory, "profile_test.xr", TEST_SOURCE)
    return [
        Case("run-default-small", [binary, "run", simple], short_timeout),
        Case("run-sandbox-small", [binary, "run", "--sandbox", simple], short_timeout,
             exit_code=1, stdout=b"", stderr=RUN_SANDBOX_DIAGNOSTIC),
        Case("test-sandbox-small", [binary, "test", "--sandbox", "--quiet", tests],
             short_timeout, exit_code=1, stdout=b"", stderr=TEST_SANDBOX_DIAGNOSTIC),
    ]


def test_role_cases(binary: Path, directory: Path, root: Path, short_timeout: float) -> list[Case]:
    tests = source(directory, "profile_test.xr", TEST_SOURCE)
    second = source(directory, "second_profile_test.xr", TEST_SOURCE)
    name = "test_" + "a" * 245 + "_after_marker"
    long_test = source(directory, "long_test.xr", f"@test\nfn {name}() {{ assert(true) }}\n")
    report = (f"  test        {name}\nxray test: {long_test}: 1 test(s), 0 skipped, 0 hook(s)\n"
              .encode() + TEST_ROLE_REPORT)
    selected = root / "tests/lir/programs/test-role-session-selected.xr"
    selected_report = directory / "selected-test-report.json"
    return [
        Case("test-run-small", [binary, "test", tests], short_timeout,
             stdout=b"", stderr=TEST_ROLE_REPORT),
        Case("test-run-parallel", [binary, "test", "--jobs", "2", second, tests], short_timeout,
             stdout=b"", stderr=b"xray test: 2 passed, 0 failed, 0 skipped\n"),
        Case("test-filter-full-name", [binary, "test", "--filter", "after_marker", "--verbose", long_test],
             short_timeout, stdout=b"", stderr=report),
        Case("test-run-filtered-state-and-hooks", [binary, "test", "--filter", "kept", selected],
             short_timeout, stdout=b"40\n43\n45\n46\n",
             stderr=b"xray test: 2 passed, 0 failed, 2 skipped\n"),
        Case("test-unmatched-filter-skips-initializer", [binary, "test", "--filter", "absent", selected],
             short_timeout, exit_code=1, stdout=b"",
             stderr=b"xray test: 0 tests executed\nxray test: 0 passed, 0 failed, 4 skipped\n"),
        Case("test-report-filtered-state-and-hooks",
             [binary, "test", "--filter", "kept", "--report", selected_report, selected],
             short_timeout, stdout=b"40\n43\n45\n46\n", test_report=(selected_report, {
                 "path": str(selected), "tests": 4, "passed": 2, "failed": 0, "errors": 0,
                 "timeouts": 0, "skipped": 2, "cases": [
                     {"name": "kept_first", "status": "passed"},
                     {"name": "excluded_deadline", "status": "skipped"},
                     {"name": "kept_explicit_skip", "status": "skipped"},
                     {"name": "kept_second", "status": "passed"},
                 ],
             })),
    ]


def native_cases(binary: Path, directory: Path, short_timeout: float,
                 build_timeout: float) -> list[Case]:
    simple = source(directory, "simple.xr", "print(42)\n")
    executable = directory / ("profile.exe" if os.name == "nt" else "profile")
    usage = usage_line(executable)
    sandbox = (NATIVE_SANDBOX_DIAGNOSTIC, usage)
    unknown = (b"error: unknown option --unknown-resource-profile", usage)
    return [
        Case("build-native-small", [binary, "build", simple, "-o", executable],
             build_timeout, stdout=None, stderr=None, artifact=executable),
        Case("native-default-small", [executable], short_timeout),
        Case("native-sandbox-small", [executable, "--sandbox"], short_timeout,
             exit_code=3, stdout=b"", stderr=None, stderr_lines=sandbox),
        Case("native-sandbox-value", [executable, "--sandbox=true"], short_timeout,
             exit_code=3, stdout=b"", stderr=None, stderr_lines=sandbox),
        # An unknown option is a usage error, not the unavailable feature.
        Case("native-unknown-option", [executable, "--unknown-resource-profile"],
             short_timeout, exit_code=2, stdout=b"", stderr=None, stderr_lines=unknown),
        Case("native-duplicate-sandbox", [executable, "--sandbox", "--sandbox"],
             short_timeout, exit_code=3, stdout=b"", stderr=None, stderr_lines=sandbox),
        # Only options are refused: a plain argument still runs the program.
        Case("native-plain-argument", [executable, "value"], short_timeout),
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
                          stderr=RUN_SANDBOX_DIAGNOSTIC))
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
    # Diagnostic lines are compared line by line, so the platform's own line
    # ending does not decide the verdict while the text of every line does.
    if case.stderr_lines is not None:
        actual = result.stderr.splitlines()
        if len(actual) != len(case.stderr_lines) or any(
                line not in expected if isinstance(expected, tuple) else line != expected
                for line, expected in zip(actual, case.stderr_lines)):
            failures.append(f"stderr lines differ: got {result.stderr!r}")
    if case.artifact is not None and (
            not case.artifact.is_file() or case.artifact.stat().st_size == 0):
        failures.append("successful native build did not produce a nonempty executable")
    if case.test_report is not None:
        path, expected = case.test_report
        try:
            report = json.loads(path.read_bytes())
            duration = report.pop("duration_ms")
            if type(duration) is not int or duration < 0 or report != {"schema": 1, "files": [expected]}:
                failures.append(f"test report differs: {report!r}, duration {duration!r}")
        except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
            failures.append(f"cannot read schema-1 test report: {error}")
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
    parser.add_argument("--suite", choices=("vm", "native", "large", "test-role"), required=True)
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
        if args.suite == "vm":
            cases = vm_cases(binary, directory, args.short_timeout)
        elif args.suite == "native":
            cases = native_cases(binary, directory, args.short_timeout, args.build_timeout)
        elif args.suite == "test-role":
            cases = test_role_cases(binary, directory, root, args.short_timeout)
        else:
            cases = large_cases(binary, directory, args.large_timeout, args.sandbox_timeout)
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

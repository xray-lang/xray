"""Verify test-runner fixtures using independent case names and result counts."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile


CASES = {
    "single_pass": (0, (1, 1, 0, 0), {"test_simple_pass": "passed"}),
    "multi_pass": (0, (3, 3, 0, 0), {
        "test_addition": "passed", "test_string": "passed", "test_array": "passed"}),
    "with_skip": (0, (2, 1, 0, 1), {
        "test_runs": "passed", "test_skipped": "skipped"}),
    "single_fail": (1, (1, 0, 1, 0), {"test_deliberate_failure": "failed"}),
    "mixed_pass_fail": (1, (3, 2, 1, 0), {
        "test_passes_1": "passed", "test_fails": "failed", "test_passes_2": "passed"}),
    "no_tests": (1, (0, 0, 0, 0), {}),
}


def fact(path):
    data = path.read_bytes()
    return {"path": str(path), "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--case", choices=CASES, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = args.fixture.resolve(strict=True)
    assert fixture.name == args.case + ".xr"
    expected_exit, totals, expected_cases = CASES[args.case]
    before = fact(fixture)
    record = {"status": "RUNNING", "input_before": before, "commands": [],
              "expected_exit": expected_exit, "expected_totals": totals,
              "expected_cases": expected_cases}

    def save():
        (args.output / "record.json").write_text(
            json.dumps(record, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    def run(label, command):
        result = subprocess.run(list(map(str, command)), capture_output=True)
        item = {"name": label, "command": list(map(str, command)),
                "exit": result.returncode}
        for name, data in (("stdout", result.stdout), ("stderr", result.stderr)):
            dest = args.output / (label + "." + name)
            dest.write_bytes(data)
            item[name] = fact(dest)
        record["commands"].append(item)
        save()
        return result

    try:
        if args.case == "no_tests":
            checked = run("check", [args.cli, "check", fixture])
            assert (checked.returncode, checked.stdout, checked.stderr) == (0, b"", b""), (
                "The zero-test fixture must be an admitted program", checked.stderr)
        with tempfile.TemporaryDirectory(prefix="xray-harness-report-") as directory:
            report_path = Path(directory) / "report.json"
            result = run("test", [args.cli, "test", fixture, "--report", report_path])
            assert result.returncode == expected_exit, (result.returncode, expected_exit)
            expected_stderr = (b"Error: 0 tests executed across 1 file(s)\n"
                               if args.case == "no_tests" else b"")
            assert result.stderr == expected_stderr, result.stderr
            assert b"\x1b[" not in result.stdout, "Redirected output must be plain"
            assert report_path.is_file(), "The selected fixture did not produce a report"
            raw = report_path.read_bytes()
            (args.output / "report.json").write_bytes(raw)
            report = json.loads(raw)
            assert report["schema"] == 1 and len(report["files"]) == 1, report
            row = report["files"][0]
            assert Path(row["path"]).resolve() == fixture, row["path"]
            assert tuple(row[key] for key in ("tests", "passed", "failed", "skipped")) == totals, row
            assert row["errors"] == 0 and row["timeouts"] == 0 and "error" not in row, row
            actual_cases = {case["name"]: case["status"] for case in row["cases"]}
            assert len(row["cases"]) == len(expected_cases) and actual_cases == expected_cases, row
            for case in row["cases"]:
                if case["status"] == "failed":
                    assert case.get("message") == "[Uncaught Panic] E0445: ", case
                else:
                    assert "message" not in case, case
            assert isinstance(report["duration_ms"], (int, float)) and report["duration_ms"] >= 0
            record["verified_report"] = report
            record["input_after"] = fact(fixture)
            assert record["input_after"] == before
            record["status"] = "PASS"
            save()
            sys.stdout.buffer.write(result.stdout)
            sys.stderr.buffer.write(result.stderr)
        return 0
    except (AssertionError, OSError, ValueError, KeyError, TypeError) as error:
        record["status"] = "FAIL"
        record["failure"] = str(error)
        save()
        print(f"test harness report oracle failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

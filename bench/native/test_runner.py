"""Exercise real references and verify that bad output cannot get a median."""

import argparse
import csv
import json
import statistics
import runpy
from pathlib import Path
import subprocess
import sys


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--reference", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    command = [sys.executable, str(Path(__file__).with_name("run.py")),
               "--reference", args.reference, "--output", str(args.output),
               "--quick", "--samples", "5", "--cpython"]
    process = subprocess.run(command, capture_output=True, text=True)
    if process.returncode:
        raise AssertionError(process.stdout + process.stderr)
    result_path = Path(process.stdout.split("results: ")[-1].strip())
    with result_path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    assert len(rows) == 6, rows
    assert all(row["status"] == "ok" and row["samples"] == "5" and row["median_ms"] for row in rows)
    metadata = json.loads(result_path.with_name("metadata.json").read_text(encoding="utf-8"))
    assert metadata["quick"] and metadata["measurement"] == "coarse"
    assert all(candidate["binary"]["sha256"] for candidate in metadata["candidates"])
    samples = json.loads(result_path.with_name("samples.json").read_text(encoding="utf-8"))
    for row in rows:
        times = [sample["elapsed_ms"] for sample in samples
                 if (sample["case"], sample["candidate"]) == (row["case"], row["candidate"])]
        assert float(row["median_ms"]) == round(statistics.median(times), 6)

    bad_candidate = args.output / "bad-candidate.json"
    bad_candidate.write_text(json.dumps([
        {"name": "wrong", "command": [sys.executable, "-c", "print(0)"]},
        {"name": "quota", "command": [sys.executable, "-c", "import sys;sys.stderr.write('work limit exceeded');sys.exit(1)"]},
        {"name": "failed", "command": [sys.executable, "-c", "import sys;sys.exit(2)"]},
        {"name": "timeout", "command": [sys.executable, "-c", "import time;time.sleep(2)"]},
    ]), encoding="utf-8")
    process = subprocess.run(command + ["--case", "fib", "--candidates", str(bad_candidate), "--timeout", "1"],
                             capture_output=True, text=True)
    assert process.returncode == 1, process.stdout + process.stderr
    result_path = Path(process.stdout.split("results: ")[-1].strip())
    with result_path.open(newline="", encoding="utf-8") as stream:
        rows = {row["candidate"]: row for row in csv.DictReader(stream)}
    assert rows["wrong"]["status"] == "wrong-output" and not rows["wrong"]["median_ms"]
    assert rows["quota"]["status"] == "quota-failed" and not rows["quota"]["median_ms"]
    assert rows["failed"]["status"] == "failed" and not rows["failed"]["median_ms"]
    assert rows["timeout"]["status"] == "timeout" and not rows["timeout"]["median_ms"]
    assert rows["c"]["status"] == "ok" and rows["cpython"]["status"] == "ok"
    samples = json.loads(result_path.with_name("samples.json").read_text(encoding="utf-8"))
    for sample in samples:
        if sample["candidate"] == "quota":
            assert sample["returncode"] == 1
            assert Path(sample["stderr"]).read_bytes() == b"work limit exceeded"
    runner = runpy.run_path(str(Path(__file__).with_name("run.py")))
    failed = runner["run_sample"]([str(args.output / "missing-executable")], b"", 1,
                                  args.output / "launch-failed")
    assert failed["status"] == "launch-failed" and failed["returncode"] is None
    assert Path(failed["stderr"]).read_bytes()
    print("native_bench_runner: checked results, five samples, and retained failures")


if __name__ == "__main__":
    main()

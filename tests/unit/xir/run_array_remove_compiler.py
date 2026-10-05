"""Complete Array removal compiler faults in owned, bounded Windows processes."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "lib"))
from xraytest.windows_owned_job import establish_job

WORKERS = 8
TIMEOUT = 300
MAX_REPORT = 16 * 1024 * 1024
PHYSICAL = ("compiler_blocks", "compiler_bytes", "runtime_blocks", "runtime_bytes")


def checked(condition, message):
    if not condition:
        raise RuntimeError(message)


def records(path):
    checked(path.is_file() and path.stat().st_size <= MAX_REPORT, f"Missing/oversize report: {path}")
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


def zero_physical(record):
    checked(all(type(record.get(key)) is int and record[key] == 0 for key in PHYSICAL),
            f"Residual physical owners: {record}")


def baseline(record, mode):
    checked(record.get("kind") == "baseline" and record.get("mode") == mode, "Wrong fresh baseline")
    sites = record.get("sites")
    checked(type(sites) is int and 0 < sites <= 67108864, "Nonfinite whole-graph site denominator")
    checked(record.get("oom_status") == 7 and record.get("functions", 0) > 0 and
            0 < record.get("c_bytes", 0) <= 1048576, "Complete Program/C-output contract changed")
    checked((record.get("allocated_cap"), record.get("live_cap"), record.get("work_cap")) ==
            (67108864, 8388608, 128000000), "Whole-graph resource caps changed")
    for value, cap in (("allocated", "allocated_cap"), ("peak", "live_cap"), ("work", "work_cap")):
        checked(type(record.get(value)) is int and 1 < record[value] <= record[cap], "Baseline budget changed")
    zero_physical(record)
    return sites


def complete(record, mode, index, workers, sites):
    expected = {"kind": "complete", "mode": mode, "index": index, "workers": workers, "sites": sites,
                **{key: 0 for key in PHYSICAL}}
    checked(record == expected, "Missing exact completion/physical-zero sentinel")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--reports-root", type=Path, required=True)
    parser.add_argument("--mode", type=int, choices=range(4), required=True)
    parser.add_argument("--workers", type=int, required=True)
    parser.add_argument("--component", choices=("transaction", "matrix7"), default="transaction")
    options = parser.parse_args()
    checked(os.name == "nt", "Parallel compiler gate is registered only on Windows")
    checked(1 <= options.workers <= WORKERS, "Worker count must be in 1..8")
    start = time.monotonic()
    deadline = start + TIMEOUT
    executable = options.executable.resolve(strict=True)
    root = options.reports_root / (time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex)
    root.mkdir(parents=True, exist_ok=False)
    manifest = {"qualified": False, "workers": options.workers, "mode": options.mode,
                "timeout": TIMEOUT, "executable": str(executable),
                "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
                "commands": [], "completed_ordinals": []}
    fixture = Path(__file__).resolve().parents[2] / "fixtures/xir_array_remove" / options.component
    inputs = [fixture / "root.xr", fixture / "producer.xr", Path(__file__).resolve()]
    manifest["inputs"] = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}
    active = []

    def publish():
        (root / "qualification.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    def launch(name, arguments):
        checked(time.monotonic() < deadline, "Original overall 300-second deadline exhausted")
        command = [str(executable), *arguments, str(root / (name + ".jsonl"))]
        log = (root / (name + ".log")).open("xb")
        try:
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log,
                                       stderr=subprocess.STDOUT, close_fds=True)
        finally:
            log.close()
        active.append((name, process))
        manifest["commands"].append({"name": name, "argv": command, "pid": process.pid})
        publish()

    def wait_phase():
        while active:
            checked(time.monotonic() < deadline, "Original overall 300-second deadline exhausted")
            for name, process in list(active):
                status = process.poll()
                if status is not None:
                    active.remove((name, process))
                    manifest["commands"][next(i for i, row in enumerate(manifest["commands"])
                                               if row["name"] == name)]["exit_code"] = status
                    checked(status == 0, f"{name} exited {status}; see {root / (name + '.log')}")
            if active:
                time.sleep(0.01)

    try:
        # Assign this parent before the first child. Kernel termination closes
        # its sole non-inherited Job handle, including when CTest kills us.
        manifest["windows_job"] = establish_job()
        publish()
        mode = str(options.mode)
        launch("baseline", ["--compiler-baseline", mode])
        wait_phase()
        rows = records(root / "baseline.jsonl")
        checked(len(rows) == 2, "Incomplete normal baseline")
        sites = baseline(rows[0], options.mode)
        first = rows[0]
        complete(rows[1], options.mode, 0, 1, sites)
        manifest["baseline"] = first
        manifest["sites"] = sites
        for index in range(options.workers):
            launch(f"shard-{index}", ["--compiler-shard", mode, str(index), str(options.workers), str(sites)])
        wait_phase()
        union = set()
        for index in range(options.workers):
            rows = records(root / f"shard-{index}.jsonl")
            checked(len(rows) >= 2 and rows[0] == first, "Worker fresh complete baseline differs")
            baseline(rows[0], options.mode)
            complete(rows[-1], options.mode, index, options.workers, sites)
            observed = []
            for row in rows[1:-1]:
                ordinal = row.get("ordinal")
                checked(row.get("kind") == "point" and row.get("mode") == options.mode and
                        type(ordinal) is int and 0 <= ordinal < sites, "Invalid actual failure ordinal")
                checked(ordinal % options.workers == index and ordinal not in union, "Duplicate/wrong partition")
                checked(row.get("status") == 7 and row.get("injected") is True and row.get("empty_output") is True,
                        "Failure status/output contract changed")
                checked(type(row.get("attempts")) is int and row["attempts"] > ordinal and
                        type(row.get("stage")) is int and 0 <= row["stage"] <= 7, "Fault was not reached")
                zero_physical(row)
                union.add(ordinal)
                observed.append(ordinal)
            checked(observed == list(range(index, sites, options.workers)), "Missing/reordered worker ordinals")
        checked(union == set(range(sites)), "Full original fault denominator is incomplete")
        manifest["completed_ordinals"] = sorted(union)
        publish()
        launch("bounds", ["--compiler-bounds", mode])
        wait_phase()
        rows = records(root / "bounds.jsonl")
        checked(len(rows) == 8 and rows[0] == first, "Bounds normal baseline changed")
        complete(rows[-1], options.mode, 0, 1, sites)
        for axis in range(3):
            for shortfall in range(2):
                row = rows[1 + 2 * axis + shortfall]
                checked(row.get("kind") == "axis" and row.get("mode") == options.mode and
                        row.get("axis") == axis and row.get("shortfall") == shortfall and
                        row.get("status") == (6 if shortfall else 0), "Exact/minus1 resource responsibility missing")
                zero_physical(row)
        checked(not active and time.monotonic() < deadline, "Incomplete process drain/deadline")
        checked(hashlib.sha256(executable.read_bytes()).hexdigest() == manifest["executable_sha256"],
                "Executable changed during complete fault census")
        checked(all(hashlib.sha256(Path(path).read_bytes()).hexdigest() == digest
                    for path, digest in manifest["inputs"].items()), "Source/driver changed during fault census")
        manifest["qualified"] = True
        manifest["seconds"] = time.monotonic() - start
        publish()
        print(f"Array remove mode{options.mode}: all {sites} faults, three exact/minus1 axes, physical0; {root}")
        return 0
    except Exception as error:
        manifest["failure"] = str(error)
        manifest["seconds"] = time.monotonic() - start
        publish()
        print(f"Array remove FAIL: {error}; {root}", file=sys.stderr)
        return 1
    finally:
        for _, process in active:
            if process.poll() is None:
                process.kill()
        for _, process in active:
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                pass
        # Never CloseHandle the Job containing this live parent; process exit
        # closes the sole handle and kills any descendants not yet drained.


if __name__ == "__main__":
    raise SystemExit(main())

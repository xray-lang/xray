"""Bounded owned processes execute the original complete metadata failure set."""
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

MAX_WORKERS = 16
TIMEOUT = 300
MAX_SITES = 20000
MAX_REPORT = 4 * 1024 * 1024
PHYSICAL = ("compiler_blocks", "compiler_bytes", "runtime_blocks", "runtime_bytes")


def checked(condition, message):
    if not condition:
        raise RuntimeError(message)


def records(path):
    checked(path.is_file() and path.stat().st_size <= MAX_REPORT, f"Missing/oversize report: {path}")
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


def zero_physical(record):
    checked(all(record.get(key) == 0 for key in PHYSICAL), f"Residual physical owners: {record}")


def baseline(record, controls):
    checked(record.get("kind") == "baseline" and record.get("controls") is controls, "Incorrect baseline phase")
    sites = record.get("sites")
    checked(type(sites) is int and 0 < sites < MAX_SITES, "Original <20000 site guard failed")
    checked(record.get("functions") == 28 and record.get("oom_status") == 7, "Whole original Program/status changed")
    checked((record.get("allocated_cap"), record.get("live_cap"), record.get("work_cap")) ==
            (67108864, 8388608, 128000000), "Whole-graph resource caps changed")
    zero_physical(record)
    return sites


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--reports-root", type=Path, required=True)
    parser.add_argument("--workers", type=int, required=True)
    options = parser.parse_args()
    workers = options.workers
    checked(1 <= workers <= MAX_WORKERS, "Worker count must be in 1..16")
    checked(os.name == "nt", "Parallel gate is registered only on Windows")
    executable = options.executable.resolve(strict=True)
    start = time.monotonic()
    deadline = start + TIMEOUT
    root = options.reports_root / (time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex)
    root.mkdir(parents=True, exist_ok=False)
    manifest = {"qualified": False, "workers": workers, "outer_timeout": TIMEOUT,
                "executable": str(executable), "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
                "directory": str(root.resolve()), "commands": [], "completed_ordinals": []}
    active = []

    def publish():
        (root / "qualification.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    def launch(name, args):
        checked(time.monotonic() < deadline, "Original overall 300-second deadline exhausted")
        command = [str(executable), *args, str(root / (name + ".jsonl"))]
        manifest["commands"].append({"name": name, "argv": command})
        log = (root / (name + ".log")).open("xb")
        try:
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log,
                                       stderr=subprocess.STDOUT, close_fds=True)
        finally:
            log.close()
        active.append((name, process))
        publish()

    def wait_phase():
        while active:
            checked(time.monotonic() < deadline, "Original overall 300-second deadline exhausted")
            for name, process in list(active):
                status = process.poll()
                if status is not None:
                    active.remove((name, process))
                    checked(status == 0, f"{name} exited {status}; see {root / (name + '.log')}")
            if active:
                time.sleep(0.01)

    try:
        # This is the only point of process authority. Failure starts NO child.
        manifest["windows_job"] = establish_job()
        publish()
        launch("baseline", ["--metadata-baseline"])
        wait_phase()
        first = records(root / "baseline.jsonl")
        checked(len(first) == 1, "Baseline report must contain exactly one complete baseline")
        sites = baseline(first[0], True)
        manifest["sites"] = sites
        for index in range(workers):
            launch(f"shard-{index}", ["--metadata-shard", str(index), str(workers), str(sites)])
        wait_phase()
        union = set()
        for index in range(workers):
            rows = records(root / f"shard-{index}.jsonl")
            checked(len(rows) >= 2 and baseline(rows[0], False) == sites, "Worker fresh baseline differs")
            checked(rows[-1] == {"kind": "complete", "index": index, "workers": workers, "sites": sites},
                    "Missing exact worker completion record")
            observed = []
            for row in rows[1:-1]:
                ordinal = row.get("ordinal")
                checked(row.get("kind") == "point" and type(ordinal) is int and 0 <= ordinal < sites,
                        "Invalid failure ordinal")
                checked(ordinal % workers == index and ordinal not in union, "Duplicate or wrongly assigned ordinal")
                checked(row.get("status") == 7 and row.get("injected") is True and row.get("empty_output") is True,
                        f"Failure status/output mismatch: {row}")
                checked(type(row.get("attempts")) is int and row["attempts"] > ordinal, "Fault was not actually attempted")
                zero_physical(row)
                observed.append(ordinal)
                union.add(ordinal)
            checked(observed == list(range(index, sites, workers)), "Worker skipped/reordered fault points")
        checked(union == set(range(sites)), "Full original failure denominator is incomplete")
        manifest["completed_ordinals"] = sorted(union)
        publish()
        launch("bounds", ["--metadata-bounds"])
        wait_phase()
        bounds = records(root / "bounds.jsonl")
        checked(len(bounds) == 1 and bounds[0].get("kind") == "bounds" and bounds[0].get("exact") is True and
                bounds[0].get("minus1") == ["allocated", "live", "work"], "Original exact/minus1 axes incomplete")
        zero_physical(bounds[0])
        manifest["qualified"] = True
        manifest["seconds"] = time.monotonic() - start
        publish()
        print(f"Source nested metadata: all {sites} ordinals, six rejects/control41, exact/minus1, physical0; {root}")
        return 0
    except Exception as error:
        manifest["failure"] = str(error)
        manifest["seconds"] = time.monotonic() - start
        publish()
        print(f"Source nested metadata FAIL: {error}; records {root}", file=sys.stderr)
        return 1
    finally:
        for _, process in active:
            if process.poll() is None:
                process.kill()
        stop = time.monotonic() + 2
        for _, process in active:
            try:
                process.wait(timeout=max(0.001, stop - time.monotonic()))
            except subprocess.TimeoutExpired:
                pass
        # Do NOT CloseHandle the Job containing this live parent. The kernel
        # closes its non-inherited sole handle at process exit, killing stragglers.


if __name__ == "__main__":
    raise SystemExit(main())

"""Sweep the unchanged REPL allocation denominator in eight owned processes."""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "lib"))
from xraytest.windows_owned_job import establish_job

WORKERS = 8
TIMEOUT = 600
MODES = ("history", "inspection", "rooted", "file", "array", "write")
CAPS = (67108864, 8388608, 128000000)


def checked(value, message):
    if not value:
        raise RuntimeError(message)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def records(path):
    checked(path.is_file() and path.stat().st_size <= 32 * 1024 * 1024,
            "Missing or oversized actual report")
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        value = json.loads(line)
        checked(type(value) is dict, "Invalid report row")
        rows.append(value)
    return rows


def inputs(row, mode):
    checked(set(row) == {"kind", "mode", "stdlib", "rooted", "file",
                         "allocated_cap", "live_cap", "work_cap"}, "Invalid input declaration")
    checked(row["kind"] == "inputs" and row["mode"] == mode, "Wrong actual family")
    checked(all(type(row[key]) is str and row[key] for key in ("stdlib", "rooted", "file")),
            "Missing compiled input roots")
    checked(all(type(row[key]) is int for key in ("allocated_cap", "live_cap", "work_cap")) and
            tuple(row[key] for key in ("allocated_cap", "live_cap", "work_cap")) == CAPS,
            "Original whole-operation limits changed")


def zero(row):
    checked(type(row.get("compiler_blocks")) is int and
            type(row.get("compiler_bytes")) is int and
            row["compiler_blocks"] == row["compiler_bytes"] == 0, "Residual physical ownership")


def baseline(row):
    checked(set(row) == {"kind", "point", "sites", "status", "attempts", "empty_output",
                         "compiler_blocks", "compiler_bytes"}, "Invalid baseline fields")
    checked(row["kind"] == "baseline" and type(row["point"]) is int and row["point"] == 0 and
            type(row["sites"]) is int and 0 < row["sites"] <= CAPS[0] and
            type(row["status"]) is int and row["status"] == 0 and
            type(row["attempts"]) is int and row["attempts"] == row["sites"] and
            row["empty_output"] is False, "Normal path or allocation denominator changed")
    zero(row)
    return row["sites"]


def complete(row, sites, points, index):
    expected = {"kind": "complete", "sites": sites, "points": points,
                "index": index, "workers": WORKERS, "single_point": False,
                "compiler_blocks": 0, "compiler_bytes": 0}
    checked(row == expected and row.get("single_point") is False and all(type(row.get(k)) is int for k in
            ("sites", "points", "index", "workers", "compiler_blocks", "compiler_bytes")),
            "Missing exact process completion")


def verify_baseline(rows, actual_inputs):
    checked(len(rows) == 3 and rows[0] == actual_inputs, "Incomplete normal process")
    sites = baseline(rows[1])
    complete(rows[-1], sites, 0, 0)
    return sites


def verify_partition(rows, actual_inputs, sites, index):
    checked(len(rows) >= 3 and rows[0] == actual_inputs, "Wrong actual worker inputs")
    checked(baseline(rows[1]) == sites, "Worker independently observed a different denominator")
    points = []
    for row in rows[2:-1]:
        checked(set(row) == {"kind", "point", "sites", "status", "attempts", "empty_output",
                             "compiler_blocks", "compiler_bytes"}, "Invalid point fields")
        checked(row["kind"] == "point" and type(row["point"]) is int and
                type(row["sites"]) is int and row["sites"] == sites and
                type(row["status"]) is int and row["status"] == 6 and
                type(row["attempts"]) is int and row["attempts"] >= row["point"] and
                row["empty_output"] is True, "Original reached-OOM or empty-output assertion missing")
        zero(row)
        points.append(row["point"])
    checked(points == list(range(index + 1, sites + 1, WORKERS)),
            "Missing, duplicate, reordered or foreign partition point")
    complete(rows[-1], sites, len(points), index)
    return points


def job_processes(job):
    class Accounting(ctypes.Structure):
        _fields_ = [("user", ctypes.c_longlong), ("kernel", ctypes.c_longlong),
                    ("period_user", ctypes.c_longlong), ("period_kernel", ctypes.c_longlong),
                    ("faults", ctypes.c_uint32), ("total", ctypes.c_uint32),
                    ("active", ctypes.c_uint32), ("terminated", ctypes.c_uint32)]
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.QueryInformationJobObject.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p,
                                                ctypes.c_uint32, ctypes.c_void_p]
    kernel.QueryInformationJobObject.restype = ctypes.c_int
    state = Accounting()
    checked(kernel.QueryInformationJobObject(job, 1, ctypes.byref(state), ctypes.sizeof(state), None),
            "Cannot observe owned Job drain")
    return state.active


def actual_files(actual_inputs):
    files = {Path(__file__).resolve(),
             Path(sys.modules[establish_job.__module__].__file__).resolve()}
    absent = set()
    for key in ("stdlib", "rooted", "file"):
        root = Path(actual_inputs[key]).resolve(strict=True)
        checked(root.is_dir(), "Compiled input root is unavailable")
        files.update(p.resolve() for p in root.rglob("*") if p.is_file())
        # Rooted and FILE intake consult the nearest project and lockfile. Guard
        # both actual files and absence; a new nearer manifest changes authority.
        if key != "stdlib":
            directories = {root, *(p for p in root.rglob("*") if p.is_dir())}
            for directory in list(directories):
                directories.update(directory.parents)
            for directory in directories:
                for name in ("xray.toml", "xray.lock"):
                    path = directory / name
                    if path.exists():
                        checked(path.is_file(), "Non-file authority input")
                        files.add(path.resolve())
                    else:
                        absent.add(path)
    return {str(p): sha(p) for p in sorted(files)}, sorted(str(p) for p in absent)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--mode", choices=MODES, required=True)
    parser.add_argument("--reports-root", type=Path, required=True)
    args = parser.parse_args()
    checked(os.name == "nt", "Owned Windows partition parent required")
    started = time.monotonic()
    deadline = started + TIMEOUT
    executable = args.executable.resolve(strict=True)
    out = args.reports_root / (time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex)
    out.mkdir(parents=True)
    record = {"qualified": False, "mode": args.mode, "workers": WORKERS, "timeout": TIMEOUT,
              "executable": str(executable), "executable_sha256": sha(executable), "commands": []}
    active = []
    job = None

    def publish():
        (out / "qualification.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")

    def launch(name, arguments):
        checked(time.monotonic() < deadline, "Original 600-second overall deadline exhausted")
        report = out / (name + ".jsonl")
        command = [str(executable), *arguments, str(report)]
        with (out / (name + ".log")).open("xb") as log:
            child = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log,
                                     stderr=subprocess.STDOUT, close_fds=True)
        row = {"name": name, "argv": command, "pid": child.pid}
        active.append((child, row))
        record["commands"].append(row)
        publish()

    def drain():
        while active:
            checked(time.monotonic() < deadline, "Original 600-second overall deadline exhausted")
            for child, row in list(active):
                status = child.poll()
                if status is not None:
                    active.remove((child, row))
                    row["exit_code"] = status
                    publish()
                    checked(status == 0, f"{row['name']} exited {status}; see {out}")
            if active:
                time.sleep(.01)

    try:
        job = establish_job()
        record["windows_job"] = job
        publish()
        launch("inputs", ["--repl-fi-inputs", args.mode])
        drain()
        rows = records(out / "inputs.jsonl")
        checked(len(rows) == 1, "Invalid compiled-input handshake")
        inputs(rows[0], args.mode)
        actual_inputs = rows[0]
        record["actual_inputs"] = actual_inputs
        hashes, absent = actual_files(actual_inputs)
        record.update({"input_hashes": hashes, "input_absences": absent})
        publish()
        launch("baseline", ["--repl-fi-baseline", args.mode])
        drain()
        sites = verify_baseline(records(out / "baseline.jsonl"), actual_inputs)
        record["allocation_sites"] = sites
        publish()
        for index in range(WORKERS):
            launch(f"partition-{index}", ["--repl-fi-partition", args.mode, str(index), str(sites)])
        drain()
        union = set()
        for index in range(WORKERS):
            points = verify_partition(records(out / f"partition-{index}.jsonl"), actual_inputs, sites, index)
            checked(not union.intersection(points), "Duplicate fault points across processes")
            union.update(points)
        checked(union == set(range(1, sites + 1)), "Original allocation denominator is incomplete")
        final_hashes, final_absent = actual_files(actual_inputs)
        checked(sha(executable) == record["executable_sha256"] and
                final_hashes == hashes and final_absent == absent, "Actual inputs or authority changed")
        remaining = job_processes(job)
        checked(not active and remaining == 1, "Unobserved descendants remain")
        checked(time.monotonic() < deadline, "Original 600-second overall deadline exhausted")
        record.update({"completed_points": len(union), "job_active_processes": remaining,
                       "qualified": True, "seconds": time.monotonic() - started})
        publish()
        print(f"REPL {args.mode}: all {sites} actual allocation points, sticky/physical0, "
              f"original600s, owned8 processes; {out}")
        return 0
    except Exception as error:
        record.update({"failure": str(error), "seconds": time.monotonic() - started})
        publish()
        print(f"REPL full-prefix FAIL: {error}; {out}", file=sys.stderr)
        return 1
    finally:
        for child, _ in active:
            if child.poll() is None:
                child.kill()
        for child, row in active:
            row["exit_code"] = child.wait()
            row["terminated_on_failure"] = True
        if active:
            record["failure_job_active_processes"] = job_processes(job)
            publish()
            checked(record["failure_job_active_processes"] == 1, "Failure descendants did not drain")


if __name__ == "__main__":
    raise SystemExit(main())

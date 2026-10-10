"""Complete conditional Source faults under one original 600-second deadline."""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "lib"))
from xraytest.windows_owned_job import establish_job

WORKERS, CASES, TIMEOUT = 8, 12, 600
MAX_REPORT = 64 * 1024 * 1024
CAPS = {"allocated_cap": 67108864, "live_cap": 8388608, "work_cap": 128000000}
PHYSICAL = {"compiler_blocks": 0, "compiler_bytes": 0}
TEST_INPUTS = (
    "tests/unit/xir/test_xir_root_conditional_source.c",
    "tests/unit/xir/xir_root_conditional_compiler_parallel.h",
    "tests/unit/xir/xir_root_conditional_oracles.h",
    "tests/unit/xir/xir_library_compile_owner.h",
    "tests/unit/xir/xir_source_program_compile_owner.h",
    "tests/unit/xir/root_conditional.cmake",
    "tests/unit/xir/run_root_conditional_source_compiler.py",
    "tests/lib/xraytest/windows_owned_job.py",
)


def checked(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_inputs(root):
    # Product directories and the actual test closure are source inputs. Build
    # trees, ignored candidates and CTest logs are outside these named roots.
    paths = [root / "CMakeLists.txt", *(root / item for item in TEST_INPUTS)]
    for name in ("src", "include", "stdlib", "cmake"):
        directory = root / name
        checked(directory.is_dir(), f"Missing source directory: {directory}")
        paths.extend(path for path in directory.rglob("*") if path.is_file())
    return {str(path.relative_to(root)): sha(path) for path in sorted(set(paths))}


def process_times(process):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE, *[ctypes.POINTER(wintypes.FILETIME)] * 4]
    kernel.GetProcessTimes.restype = wintypes.BOOL
    values = [wintypes.FILETIME() for _ in range(4)]
    if not kernel.GetProcessTimes(wintypes.HANDLE(int(process._handle)), *[ctypes.byref(value) for value in values]):
        raise ctypes.WinError(ctypes.get_last_error())
    return tuple((value.dwHighDateTime << 32) | value.dwLowDateTime for value in values[:2])


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        checked(key not in result, "Duplicate report field")
        result[key] = value
    return result


def records(path):
    checked(path.is_file() and 0 < path.stat().st_size <= MAX_REPORT, f"Missing/oversize report: {path}")
    return [json.loads(line, object_pairs_hook=unique_object) for line in path.read_text(encoding="utf-8").splitlines()]


def physical(row):
    checked(all(type(row.get(key)) is int and row[key] == value for key, value in PHYSICAL.items()),
            "Residual/missing physical owners")


def digest(value):
    return isinstance(value, str) and len(value) == 64 and all(c in "0123456789abcdef" for c in value)


def baseline(row, fixture):
    keys = {"kind", "case", "name", "sites", "input_bytes", "input_sha256", "template_bytes", "template_sha256",
            "instance_bytes", "instance_sha256", "allocation_count", "allocated", "live", "peak", "work", "oom_status",
            *CAPS, *PHYSICAL}
    checked(set(row) == keys and row["kind"] == "baseline" and type(row["case"]) is int and
            row["case"] == fixture, "Wrong complete baseline shape/order")
    checked(isinstance(row["name"], str) and row["name"], "Missing fixture identity")
    checked(type(row["sites"]) is int and 0 < row["sites"] <= CAPS["allocated_cap"], "Nonfinite actual site denominator")
    checked(type(row["oom_status"]) is int and row["oom_status"] == 7, "Typed OOM identity changed")
    checked(all(type(row[key]) is int and row[key] == value for key, value in CAPS.items()), "Original caps changed")
    for key in ("input", "template", "instance"):
        checked(type(row[key + "_bytes"]) is int and 0 < row[key + "_bytes"] <= CAPS["allocated_cap"] and
                digest(row[key + "_sha256"]), "Missing actual source/Checked wire identity")
    checked(type(row["allocation_count"]) is int and row["allocation_count"] > 0, "Missing actual allocation count")
    for key, cap in (("allocated", "allocated_cap"), ("peak", "live_cap"), ("work", "work_cap")):
        checked(type(row[key]) is int and 0 < row[key] <= row[cap], "Invalid actual baseline ledger")
    checked(type(row["live"]) is int and 0 < row["live"] <= row["peak"], "Missing finite owner baseline")
    physical(row)
    return row["sites"]


def bounded_stats(row, caps=CAPS):
    for key, cap in (("allocated", "allocated_cap"), ("peak", "live_cap"), ("work", "work_cap")):
        checked(type(row.get(key)) is int and 0 <= row[key] <= caps[cap], "Reported ledger exceeds actual cap")


def completion(row, fixture, mode, index, workers, sites, points, axes):
    expected = {"kind": "complete", "mode": mode, "case": fixture, "index": index, "workers": workers,
                "sites": sites, "points": points, "axes": axes, **PHYSICAL}
    checked(row == expected and all(type(row[key]) is int for key in ("case", "index", "workers", "sites", "points", "axes")),
            "Incomplete fixture/physical sentinel")
    physical(row)


def validate_report(rows, mode, index, workers, first=None, union=None):
    position = 0
    baselines, total_sites, total_points, total_axes = [], 0, 0, 0
    for fixture in range(CASES):
        checked(position < len(rows), "Missing fixture baseline")
        row = rows[position]; position += 1
        sites = baseline(row, fixture); baselines.append(row)
        if first is not None:
            checked(row == first[fixture], "Fresh worker source/wire/ledger/site baseline differs")
        points, axes = 0, 0
        if mode == "shard":
            for ordinal in range(index, sites, workers):
                checked(position < len(rows), "Missing actual fault ordinal")
                point = rows[position]; position += 1
                keys = {"kind", "case", "ordinal", "attempts", "phase", "status", "injected", "allocated", "peak", "work", *PHYSICAL}
                checked(set(point) == keys and point["kind"] == "point" and type(point["case"]) is int and
                        point["case"] == fixture and type(point["ordinal"]) is int and point["ordinal"] == ordinal,
                        "Missing/duplicate/reordered/wrong-partition ordinal")
                checked(type(point["status"]) is int and point["status"] == 7 and point["injected"] is True and
                        type(point["attempts"]) is int and point["attempts"] > ordinal and
                        type(point["phase"]) is int and 0 <= point["phase"] <= 8, "Fault not reached with typed OOM")
                bounded_stats(point); physical(point)
                checked(union is not None and ordinal not in union[fixture], "Duplicate global ordinal")
                union[fixture].add(ordinal); points += 1
        elif mode == "bounds":
            for axis, key in enumerate(("allocated", "peak", "work")):
                for shortfall in range(2):
                    checked(position < len(rows), "Missing exact/minus-one axis")
                    point = rows[position]; position += 1
                    keys = {"kind", "case", "axis", "shortfall", "exact", "cap", "status", "allocated", "peak", "work", *PHYSICAL}
                    checked(set(point) == keys and point["kind"] == "axis", "Wrong axis shape")
                    for field, value in {"case": fixture, "axis": axis, "shortfall": shortfall,
                                         "exact": row[key], "cap": row[key] - shortfall, "status": 6 if shortfall else 0}.items():
                        checked(type(point[field]) is int and point[field] == value, "Original exact/minus-one responsibility changed")
                    caps = dict(CAPS); caps[("allocated_cap", "live_cap", "work_cap")[axis]] = point["cap"]
                    bounded_stats(point, caps); physical(point); axes += 1
        checked(position < len(rows), "Missing fixture completion")
        completion(rows[position], fixture, mode, index, workers, sites, points, axes); position += 1
        total_sites += sites; total_points += points; total_axes += axes
    checked(position + 1 == len(rows), "Extra/truncated complete inventory")
    expected = {"kind": "finished", "mode": mode, "cases": CASES, "index": index, "workers": workers,
                "sites": total_sites, "points": total_points, "axes": total_axes, **PHYSICAL}
    checked(rows[position] == expected and all(type(rows[position][key]) is int for key in
            ("cases", "index", "workers", "sites", "points", "axes")), "Missing exact final inventory sentinel")
    physical(rows[position])
    checked(len({row["name"] for row in baselines}) == CASES, "Duplicate fixture identity")
    return baselines


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--reports-root", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--workers", type=int, choices=range(1, WORKERS + 1), required=True)
    options = parser.parse_args()
    checked(os.name == "nt", "This parallel registration requires Windows Job ownership")
    start = time.monotonic(); deadline = start + TIMEOUT
    executable = options.executable.resolve(strict=True)
    source_root = options.source_root.resolve(strict=True)
    checked(Path(__file__).resolve() == source_root / TEST_INPUTS[6], "Runner/source root identity differs")
    reports_root = options.reports_root.resolve()
    checked(not any(reports_root.is_relative_to(source_root / name) for name in ("src", "include", "stdlib", "cmake", "tests")),
            "Reports must remain outside source inputs")
    root = reports_root / (time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex)
    root.mkdir(parents=True, exist_ok=False)
    manifest = {"qualified": False, "qualification_scope": "Source12 complete eight-stage physical FI and three exact/minus-one axes",
                "workers": options.workers, "timeout": TIMEOUT, "executable": str(executable), "source_root": str(source_root),
                "commands": [], "reports": {}}
    active = []

    def publish():
        (root / "qualification.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    def launch(name, arguments):
        checked(time.monotonic() < deadline, "Original overall 600-second deadline exhausted")
        command = [str(executable), *arguments, str(root / (name + ".jsonl"))]
        with (root / (name + ".log")).open("xb") as log:
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log,
                                       stderr=subprocess.STDOUT, close_fds=True)
        active.append((name, process))
        row = {"name": name, "argv": command, "pid": process.pid}
        manifest["commands"].append(row)
        creation, _ = process_times(process)
        checked(creation > 0, "Missing actual child creation time")
        row["creation_filetime"] = creation; publish()

    def wait_phase():
        while active:
            checked(time.monotonic() < deadline, "Original overall 600-second deadline exhausted")
            for name, process in list(active):
                status = process.poll()
                if status is None:
                    continue
                active.remove((name, process))
                row = next(row for row in manifest["commands"] if row["name"] == name)
                creation, exited = process_times(process)
                checked(creation == row["creation_filetime"] and exited >= creation,
                        "Changed child identity or missing actual exit time")
                row.update(exit_code=status, exit_filetime=exited); publish()
                checked(status == 0, f"{name} exited {status}; see {root / (name + '.log')}")
            if active:
                time.sleep(0.01)

    def report(name):
        path = root / (name + ".jsonl")
        rows = records(path); manifest["reports"][str(path)] = sha(path)
        return rows

    try:
        manifest["inputs"] = source_inputs(source_root)
        manifest["executable_sha256"] = sha(executable)
        manifest["windows_job"] = establish_job(); publish()
        launch("baseline", ["--compiler-baseline"]); wait_phase()
        first = validate_report(report("baseline"), "baseline", 0, 1)
        manifest["baseline"] = first; publish()
        for index in range(options.workers):
            launch(f"shard-{index}", ["--compiler-shard", str(index), str(options.workers)])
        wait_phase()
        union = [set() for _ in range(CASES)]
        for index in range(options.workers):
            validate_report(report(f"shard-{index}"), "shard", index, options.workers, first, union)
        checked(all(actual == set(range(row["sites"])) for actual, row in zip(union, first)),
                "Complete physical fault responsibility has omissions")
        manifest["completed_ordinals"] = {row["name"]: sorted(actual) for row, actual in zip(first, union)}
        publish()
        launch("bounds", ["--compiler-bounds"]); wait_phase()
        validate_report(report("bounds"), "bounds", 0, 1, first)
        checked(not active and time.monotonic() < deadline, "Incomplete process drain/deadline")
        declared = {"qualification.json", *(row["name"] + suffix for row in manifest["commands"] for suffix in (".log", ".jsonl"))}
        checked({path.name for path in root.iterdir()} == declared, "Undeclared/missing exclusive output material")
        manifest["declared_outputs"] = sorted(declared)
        manifest["logs"] = {str(root / (row["name"] + ".log")): sha(root / (row["name"] + ".log")) for row in manifest["commands"]}
        checked(sha(executable) == manifest["executable_sha256"], "Consumed executable changed")
        checked(source_inputs(source_root) == manifest["inputs"], "Exact source/test inputs changed")
        checked(all(sha(Path(path)) == value for path, value in manifest["reports"].items()), "Declared child report changed")
        checked(time.monotonic() < deadline, "Original overall 600-second deadline exhausted during final guards")
        manifest["qualified"] = True; manifest["seconds"] = time.monotonic() - start; publish()
        print(f"Source12: all {sum(row['sites'] for row in first)} actual ordinals, all72axes, physical0; {root}")
        return 0
    except Exception as error:
        manifest["failure"] = str(error); manifest["seconds"] = time.monotonic() - start; publish()
        print(f"Source12 compiler FAIL: {error}; {root}", file=sys.stderr)
        return 1
    finally:
        for _, process in active:
            if process.poll() is None:
                process.kill()
        for name, process in active:
            try:
                status = process.wait(timeout=2)
                creation, exited = process_times(process)
                row = next(row for row in manifest["commands"] if row["name"] == name)
                checked(creation == row.get("creation_filetime", creation) and exited >= creation, "Missing failed-child terminal identity")
                row.update(creation_filetime=creation, exit_code=status, exit_filetime=exited)
            except Exception as error:
                manifest.setdefault("cleanup_failures", []).append({"name": name, "reason": str(error)})
                publish()
                raise RuntimeError(f"Owned child cleanup failed: {name}") from error
            publish()
        # Never close the Job containing this live parent. Process termination
        # closes its sole non-inherited handle and kills any remaining children.


if __name__ == "__main__":
    raise SystemExit(main())

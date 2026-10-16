"""Own the complete allocation case union within one original 300-second deadline."""
from __future__ import annotations

import time
STARTED = time.monotonic()

import argparse
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import uuid

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "lib"))
from xraytest.windows_owned_job import establish_job
from xraytest.proc import _terminate_tree

TIMEOUT = 300
WORKERS = 12
SCHEMA = "allocation-case-faults.v1"
EXPECTED = [("Function", 0, n) for n in range(2)] + [("Producer", 0, 0)]
EXPECTED += [(name, 0, n) for name in ("runtime", "direct") for n in range(8)]
EXPECTED += [(name, scalar, n) for scalar in range(2)
             for name in ("capture", "return_capture", "function_cell") for n in range(8)]
CASE = re.compile(r"FI map ([A-Za-z_]+) scalar([0-9]+) mode([0-9]+): whole=([0-9]+) "
                  r"compiler=\[0,([0-9]+)\) runtime=\[([0-9]+),([0-9]+)\) local=\[0,([0-9]+)\)")
TOTAL = re.compile(r"FI map total: cases=([0-9]+) original_ordinals=([0-9]+) compiler=([0-9]+) "
                   r"runtime=([0-9]+) bridge=([0-9]+) axes=([0-9]+)")


def checked(condition, message):
    if not condition:
        raise RuntimeError(message)


def remaining(deadline):
    seconds = deadline - time.monotonic()
    checked(seconds > 0, "Original global 300-second deadline exhausted")
    return seconds


def sha(path, deadline):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        while data := stream.read(1024 * 1024):
            remaining(deadline)
            result.update(data)
    remaining(deadline)
    return result.hexdigest()


def mapping_sha(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def cache_values(build):
    result = {}
    for line in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        if not line.startswith(("#", "//")) and ":" in line and "=" in line:
            key, value = line.split("=", 1)
            result[key.split(":", 1)[0]] = value
    return result


def cmake_flag(value):
    text = (value or "").upper()
    return int(text not in {"", "0", "OFF", "NO", "FALSE", "N", "IGNORE", "NOTFOUND"}
               and not text.endswith("-NOTFOUND"))


def ninja_read(ninja, build, arguments, deadline):
    result = subprocess.run([str(ninja), "-t", *arguments], cwd=build,
                            stdin=subprocess.DEVNULL, capture_output=True,
                            timeout=remaining(deadline), close_fds=True)
    remaining(deadline)
    checked(result.returncode == 0 and not result.stderr, "Actual Ninja dependency query failed")
    return result.stdout.decode("utf-8").replace("\r\n", "\n")


def input_paths(source, build, executable, deadline):
    cache = cache_values(build)
    ninja = Path(cache["CMAKE_MAKE_PROGRAM"]).resolve(strict=True)
    checked(cache.get("CMAKE_GENERATOR") == "Ninja", "Allocation qualification requires Ninja")
    target = executable.relative_to(build).as_posix()
    names = ninja_read(ninja, build, ["inputs", target], deadline).splitlines()
    checked(names, "Missing actual native target inputs")
    files = {ninja, executable}
    objects = []
    phony = []
    for name in names:
        remaining(deadline)
        path = (build / name).resolve()
        if path.is_file():
            files.add(path)
            if path.suffix in {".obj", ".o"}:
                objects.append((name, path))
        else:
            query = ninja_read(ninja, build, ["query", name], deadline)
            checked("\n  input: phony\n" in query, f"Missing physical native input: {name}")
            phony.append(name)
    checked(objects, "Native target has no recorded objects")
    for name, _ in objects:
        text = ninja_read(ninja, build, ["deps", name], deadline)
        match = re.search(r"#deps (\d+)", text)
        checked(match and int(match[1]) > 0, f"Missing real header dependencies for {name}")
        for line in text.splitlines()[1:]:
            if line.startswith("    "):
                path = (build / line.strip()).resolve(strict=True)
                checked(path.is_file(), f"Missing header dependency: {path}")
                files.add(path)
    required = ["tests/unit/xir/test_xir_allocations.c", "tests/unit/xir/xir_allocation_case_workers.h",
                "tests/unit/xir/xir_allocation_phase_cases.h", "tests/unit/xir/xir_allocation_phase_map.h",
                "tests/unit/xir/xir_allocation_request_trace.h", "tests/unit/xir/allocation_feedback.cmake",
                "tests/unit/CMakeLists.txt", "tests/unit/xir/run_allocation_case_faults.py",
                "tests/unit/xir/test_allocation_case_driver.py",
                "tests/lib/xraytest/windows_owned_job.py", "tests/lib/xraytest/proc.py"]
    for name in required:
        path = (source / name).resolve(strict=True)
        checked(path.is_file(), f"Missing real qualification input: {name}")
        files.add(path)
    for path in executable.parent.glob("*.dll"):
        files.add(path.resolve(strict=True))
    return sorted(files), phony, cache


def fingerprint(paths, deadline):
    return {str(path): sha(path, deadline) for path in paths}


def config_paths(build):
    return [build / name for name in ["CMakeCache.txt", "build.ninja", "CMakeFiles/rules.ninja",
                                     "CTestTestfile.cmake", "tests/unit/CTestTestfile.cmake"]]


def json_record(text):
    def pairs(values):
        result = {}
        for key, value in values:
            checked(key not in result, f"Duplicate report key: {key}")
            result[key] = value
        return result
    row = json.loads(text, object_pairs_hook=pairs)
    checked(type(row) is dict, "Child record is not an object")
    return row


def axis_mask(ids):
    mask = 0
    for index in ids:
        if index >= 10 and (index - 3) % 8 == 7:
            mask |= 1 << ((index - 10) // 8)
    return mask


def parse_report(raw, identity, mode, index, workers, census=None):
    checked(mode in {"census", "normal", "producer", "aggregate", "matrix"} and
            type(index) is int and type(workers) is int and 0 <= index < workers <= 67,
            "Invalid report consumer or partition")
    checked(mode != "census" or (index == 0 and workers == 1), "Invalid census consumer")
    checked(len(raw) <= 2 * 1024 * 1024, "Oversized child report")
    lines = raw.decode("utf-8").splitlines()
    checked(lines and lines[0].startswith("ALLOCATION_BEGIN ") and
            lines[-1].startswith("ALLOCATION_END "), "Incomplete child report envelope")
    expected_begin = {"schema": SCHEMA, "mode": mode, "index": index, "workers": workers, **identity}
    begin = json_record(lines[0][len("ALLOCATION_BEGIN "):])
    checked(begin.keys() == expected_begin.keys() and
            all(type(begin[key]) is type(value) and begin[key] == value
                for key, value in expected_begin.items()), "Foreign child identity or configuration")
    rows = []
    totals = []
    for line in lines[1:-1]:
        checked("ALLOCATION_" not in line, "Unexpected additional child envelope")
        if not line.startswith("FI map "):
            continue
        match = CASE.fullmatch(line)
        if match:
            family = match[1]
            scalar, case_mode, whole, prefix, start, end, runtime = map(int, match.groups()[1:])
            checked(0 < prefix < whole < 10000 and whole == prefix + runtime and
                    start == prefix and end == whole, "Invalid actual ordinal partition")
            rows.append({"family": family, "scalar": scalar, "mode": case_mode,
                         "whole": whole, "prefix": prefix, "runtime": runtime})
        else:
            match = TOTAL.fullmatch(line)
            checked(mode == "census" and match, "Malformed or foreign case map")
            totals.append(tuple(map(int, match.groups())))
    ids = list(range(67)) if mode == "census" else list(range(index, 67, workers))
    checked([(row["family"], row["scalar"], row["mode"]) for row in rows] ==
            [EXPECTED[case] for case in ids], "Duplicate, missing, foreign or reordered case")
    if census is not None:
        checked(rows == [census[case] for case in ids], "Actual case census changed in a worker")
    whole = sum(row["whole"] for row in rows)
    compiler = sum(row["prefix"] for row in rows) if mode in {"producer", "aggregate", "matrix"} else 0
    runtime = sum(row["runtime"] for row in rows) if mode in {"aggregate", "matrix"} else 0
    mask = 0 if mode == "census" else axis_mask(ids)
    expected_end = {"cases": len(ids), "whole": whole, "compiler": compiler, "runtime": runtime,
                    "bridge": runtime, "axis_groups": mask, "axes": mask.bit_count() * 11,
                    "remaining_resources": int(mode == "aggregate" and index == 0),
                    "physical_blocks": 0, "physical_bytes": 0}
    end = json_record(lines[-1][len("ALLOCATION_END "):])
    checked(end.keys() == expected_end.keys() and
            all(type(end[key]) is int and end[key] == value for key, value in expected_end.items()),
            "Incomplete original faults, axes, resources or physical release")
    checked(totals == ([(67, whole, 0, 0, 0, 0)] if mode == "census" else []), "Unexpected census summary")
    return rows, end, ids


def job_processes(job):
    class Accounting(ctypes.Structure):
        _fields_ = [("user", ctypes.c_longlong), ("kernel", ctypes.c_longlong),
                    ("period_user", ctypes.c_longlong), ("period_kernel", ctypes.c_longlong),
                    ("faults", ctypes.c_uint32), ("total", ctypes.c_uint32),
                    ("active", ctypes.c_uint32), ("terminated", ctypes.c_uint32)]
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    call = kernel.QueryInformationJobObject
    call.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p]
    call.restype = ctypes.c_int
    state = Accounting()
    checked(call(job, 1, ctypes.byref(state), ctypes.sizeof(state), None), "Cannot inspect owned Job drain")
    return state.active


def owned_drain(owner, deadline):
    remaining(deadline)
    if owner["adapter"] == "windows-job":
        active = job_processes(owner["job"])
        checked(active == 1, "Owned Windows descendants did not drain")
        return {"adapter": "windows-job", "active_processes": active}
    observer = shutil.which("ps")
    checked(observer, "Native process-group inspection is unavailable")
    command = [observer, "-e", "-o", "pid=,pgid="]
    child = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, close_fds=True)
    try:
        stdout, stderr = child.communicate(timeout=remaining(deadline))
    except BaseException:
        child.kill()
        child.wait()
        raise
    checked(child.returncode == 0 and not stderr, "Native process-group inspection failed")
    members = set()
    for line in stdout.splitlines():
        columns = line.split()
        checked(len(columns) == 2 and all(value.isdigit() for value in columns), "Malformed process inventory")
        pid, group = map(int, columns)
        if group == os.getpgrp():
            members.add(pid)
    checked(os.getpid() in members and members <= {os.getpid(), child.pid}, "Unobserved POSIX descendants remain")
    remaining(deadline)
    return {"adapter": "posix-process-group", "active_processes": 1, "group": os.getpgrp(),
            "observer_argv": command, "snapshot_members": sorted(members)}


def main():
    deadline = STARTED + TIMEOUT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--reports-root", type=Path)
    parser.add_argument("--mode", choices=("normal", "producer", "aggregate"), required=True)
    args = parser.parse_args()
    remaining(deadline)
    checked(os.name in {"nt", "posix"}, "Native owned-process capability is required")
    source = args.source_root.resolve(strict=True)
    build = args.build_root.resolve(strict=True)
    executable = args.executable.resolve(strict=True)
    checked(executable.is_relative_to(build) and executable.name in
            {"test_xir_allocations.exe", "test_xir_allocations"}, "Foreign native allocation target")
    reports_root = (args.reports_root if args.reports_root is not None else
                    Path(tempfile.gettempdir()) / "xray-allocation-case-faults").resolve()
    checked(not reports_root.is_relative_to(source) and not reports_root.is_relative_to(build),
            "Reports must be outside Source and Build")
    out = reports_root / (time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex)
    out.mkdir(parents=True, exist_ok=False)
    active = []
    owner = None
    manifest = {"schema": SCHEMA, "qualified": False, "full_aggregate_qualified": False,
                "mode": args.mode, "global_timeout": TIMEOUT, "workers": WORKERS,
                "commands": [], "stage": "initializing", "reports_directory": str(out)}

    def publish():
        manifest["seconds"] = time.monotonic() - STARTED
        with (out / "qualification.json").open("w", encoding="utf-8", newline="\n") as stream:
            json.dump(manifest, stream, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())

    def launch(name, arguments):
        remaining(deadline)
        command = [str(executable), *arguments, identity["nonce"]]
        native = {}
        if os.name == "nt":
            info = subprocess.STARTUPINFO()
            info.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            info.wShowWindow = 0
            native = {"startupinfo": info, "creationflags": subprocess.CREATE_NO_WINDOW}
        with (out / (name + ".stdout.log")).open("xb") as stdout, (out / (name + ".stderr.log")).open("xb") as stderr:
            child = subprocess.Popen(command, cwd=build, stdin=subprocess.DEVNULL, stdout=stdout,
                                     stderr=stderr, env=environment, close_fds=True, **native)
        row = {"name": name, "argv": command, "pid": child.pid, "started_seconds": time.monotonic() - STARTED}
        active.append((child, row))
        manifest["commands"].append(row)
        publish()

    def drain():
        while active:
            remaining(deadline)
            for child, row in list(active):
                status = child.poll()
                if status is not None:
                    active.remove((child, row))
                    row.update(exit_code=status, ended_seconds=time.monotonic() - STARTED)
                    publish()
                    checked(status == 0, f"{row['name']} exited {status}; see {out}")
            if active:
                time.sleep(min(.01, remaining(deadline)))

    def report(name, mode, index, workers, census=None):
        remaining(deadline)
        stdout, stderr = out / (name + ".stdout.log"), out / (name + ".stderr.log")
        checked(stderr.is_file() and stderr.stat().st_size == 0, f"Unexpected native diagnostics: {stderr}")
        checked(stdout.is_file() and stdout.stat().st_size <= 2 * 1024 * 1024, "Missing or oversized native output")
        result = parse_report(stdout.read_bytes(), identity, mode, index, workers, census)
        remaining(deadline)
        return result

    try:
        if os.name == "nt":
            owner = {"adapter": "windows-job", "job": establish_job()}
        else:
            checked(os.getpgrp() == os.getpid(), "Missing owned POSIX session")
            owner = {"adapter": "posix-process-group", "group": os.getpgrp()}
        paths, phony, cache = input_paths(source, build, executable, deadline)
        before = fingerprint(paths, deadline)
        configuration = fingerprint(config_paths(build), deadline)
        identity = {"nonce": uuid.uuid4().hex, "input_sha256": mapping_sha(before),
                    "binary_sha256": sha(executable, deadline), "config_sha256": mapping_sha(configuration),
                    "asan": cmake_flag(cache.get("ENABLE_ASAN")), "ubsan": cmake_flag(cache.get("ENABLE_UBSAN"))}
        environment = os.environ.copy()
        for kind in ("input", "binary", "config"):
            environment[f"XRAY_ALLOCATION_{kind.upper()}_SHA256"] = identity[f"{kind}_sha256"]
        manifest.update(inputs=before, configuration=configuration, identity=identity, native_owner=owner,
                        source_root=str(source), build_root=str(build), executable=str(executable),
                        phony_inputs=phony, stage="census")
        publish()
        launch("census", ["--allocation-census"])
        drain()
        census, _, _ = report("census", "census", 0, 1)
        manifest.update(census=census, stage="launching_partitions")
        publish()
        for index in range(WORKERS):
            launch(f"shard-{index}", ["--allocation-shard", str(index), str(WORKERS), args.mode])
        manifest["stage"] = "partitions_running"
        publish()
        drain()
        union = set()
        masks = 0
        resources = 0
        ends = []
        for index in range(WORKERS):
            _, end, ids = report(f"shard-{index}", args.mode, index, WORKERS, census)
            checked(not union.intersection(ids) and not masks & end["axis_groups"], "Duplicate case or resource group")
            union.update(ids)
            masks |= end["axis_groups"]
            resources += end["remaining_resources"]
            ends.append(end)
        checked(union == set(range(67)) and masks == 255 and resources == int(args.mode == "aggregate"),
                "Incomplete original case, resource-axis or remaining-resource union")
        totals = {key: sum(row[key] for row in ends) for key in ("cases", "whole", "compiler", "runtime", "bridge", "axes")}
        checked(totals["cases"] == 67 and totals["axes"] == 88, "Incomplete original denominator")
        checked(fingerprint(paths, deadline) == before and fingerprint(config_paths(build), deadline) == configuration and
                sha(executable, deadline) == identity["binary_sha256"], "Actual input, executable or configuration changed")
        checked(not active, "Owned native children remain active")
        manifest.update(native_drain=owned_drain(owner, deadline), case_union=sorted(union), totals=totals,
                        resource_group_mask=masks, remaining_resources=resources, stage="collecting_outputs")
        names = ["census", *(f"shard-{index}" for index in range(WORKERS))]
        output_names = {name + suffix for name in names for suffix in (".stdout.log", ".stderr.log")}
        checked({path.name for path in out.iterdir()} == output_names | {"qualification.json"}, "Undeclared qualification output")
        manifest["outputs"] = {name: sha(out / name, deadline) for name in sorted(output_names)}
        manifest.update(qualified=True, full_aggregate_qualified=args.mode == "aggregate", stage="complete")
        publish()
        remaining(deadline)
        print(f"Allocation {args.mode}: complete 67-case union, 88 axes, physical0; {out}", flush=True)
        remaining(deadline)
        return 0
    except Exception as error:
        manifest.update(qualified=False, full_aggregate_qualified=False, failure=str(error), stage="failed")
        publish()
        print(f"Allocation qualification FAIL: {error}; {out}", file=sys.stderr, flush=True)
        return 1
    finally:
        for child, _ in active:
            if child.poll() is None:
                child.kill()
        for child, row in active:
            try:
                row["exit_code"] = child.wait(timeout=max(.01, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                row["exit_pending_native_owner_close"] = True
            row["terminated_on_failure"] = True
        if active:
            publish()


def posix_parent():
    deadline = STARTED + TIMEOUT
    read_fd, write_fd = os.pipe()
    os.set_inheritable(write_fd, False)
    child = None
    try:
        command = [sys.executable, "-B", str(Path(__file__).resolve()), "--posix-owner",
                   str(read_fd), repr(STARTED), *sys.argv[1:]]
        child = subprocess.Popen(command, stdin=subprocess.DEVNULL, close_fds=True,
                                 pass_fds=(read_fd,), start_new_session=True)
        os.close(read_fd)
        read_fd = None
        status = child.wait(timeout=remaining(deadline))
        remaining(deadline)
        return status
    except BaseException as error:
        if child is not None and child.poll() is None:
            _terminate_tree(child)
            child.wait()
        print(f"Allocation POSIX owner FAIL: {error}", file=sys.stderr)
        return 1
    finally:
        if read_fd is not None:
            os.close(read_fd)
        os.close(write_fd)


def posix_owner():
    global STARTED
    checked(os.name == "posix" and len(sys.argv) >= 5 and os.getpgrp() == os.getpid(), "Invalid native owner entry")
    read_fd, parent_started = int(sys.argv[2]), float(sys.argv[3])
    checked(read_fd >= 3 and math.isfinite(parent_started) and 0 < parent_started <= STARTED,
            "Invalid global deadline or parent lifeline")
    STARTED = parent_started
    os.set_inheritable(read_fd, False)
    sys.argv = [sys.argv[0], *sys.argv[4:]]

    def orphan_watch():
        try:
            while os.read(read_fd, 1):
                pass
        finally:
            os.killpg(os.getpgrp(), signal.SIGKILL)

    threading.Thread(target=orphan_watch, name="allocation-owner-lifeline", daemon=True).start()
    try:
        result = main()
        if result:
            os.killpg(os.getpgrp(), signal.SIGKILL)
        return result
    except BaseException:
        os.killpg(os.getpgrp(), signal.SIGKILL)
        raise


def entry():
    if os.name == "posix":
        if len(sys.argv) >= 2 and sys.argv[1] == "--posix-owner":
            return posix_owner()
        return posix_parent()
    return main()


if __name__ == "__main__":
    raise SystemExit(entry())

"""Qualify the unchanged generic Source operation within one owned 600-second deadline."""
from __future__ import annotations

import time
STARTED = time.monotonic()

import argparse
import ast
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
import threading
import tempfile
import sys
import uuid

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "lib"))
from xraytest.windows_owned_job import establish_job
from xraytest.proc import _terminate_tree

WORKERS = 8
TIMEOUT = 600
CAPS = (67108864, 8388608, 128000000)
SCHEMA = "generic-compiler-faults.v1"


def checked(value, message):
    if not value:
        raise RuntimeError(message)


def remaining(deadline):
    value = deadline - time.monotonic()
    checked(value > 0, "Original global 600-second deadline exhausted")
    return value


def sha(path, deadline):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            remaining(deadline)
            digest.update(chunk)
    remaining(deadline)
    return digest.hexdigest()


def snapshot(root, deadline):
    files = {}
    for path in sorted(root.rglob("*")):
        remaining(deadline)
        if ".git" in path.relative_to(root).parts or "__pycache__" in path.parts:
            continue
        checked(not path.is_symlink(), f"Unexpected Source symlink: {path}")
        if path.is_file():
            files[path.relative_to(root).as_posix()] = sha(path, deadline)
    checked(files, "Empty Source inventory")
    return files


def mapping_sha(files):
    encoded = json.dumps(files, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def config_snapshot(build, deadline):
    names = ["CMakeCache.txt", "build.ninja", "CMakeFiles/rules.ninja"]
    files = {name: sha(build / name, deadline) for name in names}
    for path in sorted(build.rglob("CTestTestfile.cmake")):
        remaining(deadline)
        files[path.relative_to(build).as_posix()] = sha(path, deadline)
    checked(any(name.endswith("CTestTestfile.cmake") for name in files), "Missing actual CTest registration")
    return files


def fixture_identity(source):
    path = source / "tests/unit/xir/test_xir_source_generic_callable_context.c"
    match = re.search(r'static const char source_generic_definition_text\[\]\s*=\s*'
                      r'((?:"(?:[^"\\]|\\.)*"\s*)+);', path.read_text(encoding="utf-8"), re.S)
    checked(match, "Missing original compiled Source literal")
    tokens = re.findall(r'"(?:[^"\\]|\\.)*"', match.group(1))
    text = "".join(ast.literal_eval(token) for token in tokens).encode("ascii")
    return len(text), hashlib.sha256(text).hexdigest()


def records(path, deadline):
    remaining(deadline)
    checked(path.is_file() and path.stat().st_size <= 16 * 1024 * 1024, "Missing or oversized child report")
    rows = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
    remaining(deadline)
    return rows


def integer(row, key, minimum=0):
    value = row.get(key)
    checked(type(value) is int and value >= minimum, f"Invalid integer {key}")
    return value


def physical_zero(row):
    checked(integer(row, "compiler_blocks") == integer(row, "compiler_bytes") == 0,
            "Compiler physical ownership did not return to zero")


def process_report(path, identity, mode, index, sites, points, deadline):
    rows = records(path, deadline)
    expected = {"event": "identity", "schema": SCHEMA, "mode": mode, "index": index,
                "sites": sites, "workers": WORKERS, **identity,
                "allocated_cap": CAPS[0], "live_cap": CAPS[1], "work_cap": CAPS[2]}
    checked(rows and rows[0] == expected, "Child executable, Source, configuration or partition identity differs")
    completion = {"event": "complete", "points": points, "compiler_blocks": 0, "compiler_bytes": 0}
    checked(len(rows) >= 2 and rows[-1] == completion, "Missing exact child completion")
    return rows[1:-1]


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


def verify_census(row):
    checked(row.get("event") == "census" and row.get("public_calls") == 8, "Original eight public operations incomplete")
    sites = integer(row, "sites", 1)
    checked(sites <= CAPS[0], "Invalid actual allocation denominator")
    physical_zero(row)
    checked(integer(row, "live") == integer(row, "owner_live", 1), "Census ledger live baseline differs")
    required = [integer(row, key, 1) for key in ("allocated", "peak", "work")]
    checked(all(value <= cap for value, cap in zip(required, CAPS)), "Census exceeded original caps")
    integer(row, "allocations", 1)
    return sites, required


def verify_partition(rows, index, sites, union, deadline):
    expected = range(index, sites, WORKERS)
    checked(len(rows) == len(expected), "Shard does not cover its exact ordinal count")
    for row, ordinal in zip(rows, expected):
        remaining(deadline)
        checked(row.get("event") == "point" and integer(row, "ordinal") == ordinal and ordinal not in union,
                "Duplicate, missing, unordered or foreign fault ordinal")
        checked(row.get("status") == 7 and row.get("injected") is True and integer(row, "attempts", 1) > ordinal,
                "Original physical failure was not reached or did not return OOM")
        checked(integer(row, "live") == integer(row, "owner_live", 1), "Fault owner leaked ledger storage")
        for key in ("allocated", "peak", "work", "allocations"):
            integer(row, key)
        physical_zero(row)
        union.add(ordinal)


def verify_bounds(rows, required):
    checked(len(rows) == 6, "Original three exact/minus-one axes incomplete")
    for row, (axis, less) in zip(rows, ((axis, less) for axis in range(3) for less in range(2))):
        checked(row.get("event") == "axis" and row.get("axis") == axis and row.get("less") == less,
                "Resource axes reordered or duplicated")
        checked(row.get("exact") == required[axis] and row.get("status") == (6 if less else 0), "Original cap boundary differs")
        caps = list(CAPS)
        caps[axis] = required[axis] - less
        checked([row.get(key) for key in ("allocated_cap", "live_cap", "work_cap")] == caps, "Resource cap changed")
        checked(integer(row, "live") == integer(row, "owner_live"), "Boundary owner leaked ledger storage")
        for key in ("allocated", "peak", "work", "allocations"):
            integer(row, key)
        physical_zero(row)



def posix_owned_group(deadline):
    checked(os.name == "posix" and os.getpgrp() == os.getpid(), "Missing native owned POSIX session")
    observer = shutil.which("ps")
    checked(observer, "Native process-group inspection is unavailable")
    remaining(deadline)
    return {"adapter": "posix-process-group", "group": os.getpgrp(), "observer": observer,
            "parent": os.getppid(), "lifeline_writer_owner": "outer-parent-only"}


def owned_drain(owner, deadline):
    remaining(deadline)
    if owner["adapter"] == "windows-job":
        active = job_processes(owner["job"])
        checked(active == 1, "Owned Windows descendants did not drain")
        return {"adapter": owner["adapter"], "active_processes": active}
    command = [owner["observer"], "-e", "-o", "pid=,pgid="]
    # The inspector joins the owned group. It is reaped before qualification;
    # its own snapshot row is the sole allowed transient member besides us.
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
        remaining(deadline)
        columns = line.split()
        checked(len(columns) == 2 and all(value.isdigit() for value in columns), "Invalid native process inventory")
        pid, group = (int(value) for value in columns)
        if group == owner["group"]:
            members.add(pid)
    checked(os.getpid() in members and members <= {os.getpid(), child.pid}, "Unobserved POSIX descendants remain")
    return {"adapter": owner["adapter"], "group": owner["group"], "active_processes": 1,
            "observer_argv": command, "observer_pid": child.pid, "observer_exit": child.returncode,
            "snapshot_owned_members": sorted(members)}


def posix_parent():
    deadline = STARTED + TIMEOUT
    read_fd, write_fd = os.pipe()
    os.set_inheritable(write_fd, False)
    command = [sys.executable, "-B", str(Path(__file__).resolve()), "--posix-owner",
               str(read_fd), repr(STARTED), *sys.argv[1:]]
    child = None
    try:
        remaining(deadline)
        child = subprocess.Popen(command, stdin=subprocess.DEVNULL, close_fds=True,
                                 pass_fds=(read_fd,), start_new_session=True)
        os.close(read_fd)
        read_fd = None
        status = child.wait(timeout=remaining(deadline))
        remaining(deadline)
        return status
    except BaseException as error:
        if child is not None and child.poll() is None:
            _terminate_tree(child)  # Same native group owner as the shared test process layer.
            child.wait()
        print(f"Generic compiler POSIX owner FAIL: {error}", file=sys.stderr)
        return 1
    finally:
        if read_fd is not None:
            os.close(read_fd)
        os.close(write_fd)


def posix_owner():
    global STARTED
    checked(os.name == "posix" and len(sys.argv) >= 5 and os.getpgrp() == os.getpid(), "Invalid POSIX owner entry")
    read_fd = int(sys.argv[2])
    parent_started = float(sys.argv[3])
    checked(read_fd >= 3 and math.isfinite(parent_started) and 0 < parent_started <= STARTED,
            "Invalid shared deadline or parent lifeline")
    STARTED = parent_started
    os.set_inheritable(read_fd, False)
    sys.argv = [sys.argv[0], *sys.argv[4:]]

    def orphan_watch():
        # Only the outer parent holds the non-inherited writer. SIGKILL and
        # CTest termination close it even when the parent's finally never runs.
        try:
            while os.read(read_fd, 1):
                pass
        finally:
            os.killpg(os.getpgrp(), signal.SIGKILL)

    watcher = threading.Thread(target=orphan_watch, name="generic-owner-lifeline", daemon=True)
    watcher.start()
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

def main():
    # The timer precedes parsing, input hashing, census and all process work.
    started = STARTED
    deadline = started + TIMEOUT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--reports-root", type=Path)
    args = parser.parse_args()
    checked(os.name in {"nt", "posix"}, "Native owned process capability is required")
    active = []
    manifest = {"qualified": False, "schema": SCHEMA, "global_timeout": TIMEOUT, "workers": WORKERS,
                "generic_same_owner_retry": False, "commands": []}
    source = args.source_root.resolve(strict=True)
    build = args.build_root.resolve(strict=True)
    report_argument = args.reports_root if args.reports_root is not None else Path(tempfile.gettempdir()) / "xray-generic-callable-compiler"
    reports_root = report_argument.resolve()
    checked(not reports_root.is_relative_to(source) and not reports_root.is_relative_to(build),
            "Reports must belong to the qualification owner outside Source and Build")
    out = reports_root / (time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex)
    out.mkdir(parents=True, exist_ok=False)
    owner = None

    def publish():
        with (out / "qualification.json").open("w", encoding="utf-8", newline="\n") as stream:
            json.dump(manifest, stream, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())

    def launch(name, arguments):
        remaining(deadline)
        command = [str(executable), *arguments, nonce, str(out / (name + ".jsonl"))]
        native = {}
        if os.name == "nt":
            info = subprocess.STARTUPINFO()
            info.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            info.wShowWindow = 0
            native = {"startupinfo": info, "creationflags": subprocess.CREATE_NO_WINDOW}
        with (out / (name + ".stdout.log")).open("xb") as stdout, (out / (name + ".stderr.log")).open("xb") as stderr:
            child = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr, env=environment,
                                     close_fds=True, **native)
        row = {"name": name, "argv": command, "pid": child.pid, "started_seconds": time.monotonic() - started}
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
                    row.update(exit_code=status, ended_seconds=time.monotonic() - started)
                    publish()
                    checked(status == 0, f"{row['name']} exited {status}; see its actual logs in {out}")
            if active:
                time.sleep(min(.01, remaining(deadline)))

    try:
        owner = {"adapter": "windows-job", "job": establish_job()} if os.name == "nt" else posix_owned_group(deadline)
        executable = args.executable.resolve(strict=True)
        inputs = snapshot(source, deadline)
        required_inputs = ["tests/unit/xir/test_xir_source_generic_callable_context.c",
                           "tests/unit/xir/xir_generic_compiler_fault_shards.h",
                           "tests/unit/xir/run_generic_callable_compiler_faults.py",
                           "tests/unit/xir/root_conditional.cmake", "tests/lib/xraytest/windows_owned_job.py",
                           "tests/lib/xraytest/proc.py",
                           "tests/unit/xir/xir_library_compile_owner.h", "tests/unit/xir/xir_source_program_compile_owner.h"]
        checked(all(path in inputs for path in required_inputs), "Missing actual helper, driver, registry or original owner")
        config = config_snapshot(build, deadline)
        nonce = uuid.uuid4().hex
        source_bytes, source_sha = fixture_identity(source)
        binary_sha = sha(executable, deadline)
        identity = {"nonce": nonce, "input_sha256": mapping_sha(inputs), "binary_sha256": binary_sha,
                    "config_sha256": mapping_sha(config), "source_bytes": source_bytes, "source_sha256": source_sha}
        environment = os.environ.copy()
        for key in ("input", "binary", "config"):
            environment[f"XRAY_GENERIC_SHARD_{key.upper()}_SHA256"] = identity[f"{key}_sha256"]
        manifest.update(executable=str(executable), source_root=str(source), build_root=str(build), inputs=inputs,
                        configuration=config, identity=identity, native_owner=owner, reports_root=str(reports_root))
        publish()
        launch("census", ["--compiler-census"])
        drain()
        rows = process_report(out / "census.jsonl", identity, "census", 0, 0, 0, deadline)
        checked(len(rows) == 1, "Census included fault trials or omitted its denominator")
        sites, required = verify_census(rows[0])
        manifest.update(census=rows[0], required_caps=required)
        publish()
        for index in range(WORKERS):
            launch(f"shard-{index}", ["--compiler-shard", str(index), str(sites)])
        drain()
        union = set()
        for index in range(WORKERS):
            points = len(range(index, sites, WORKERS))
            rows = process_report(out / f"shard-{index}.jsonl", identity, "shard", index, sites, points, deadline)
            verify_partition(rows, index, sites, union, deadline)
        checked(union == set(range(sites)), "Original full ordinal union has a gap")
        manifest.update(completed_ordinals=sorted(union), complete_fault_count=len(union))
        publish()
        launch("bounds", ["--compiler-bounds", str(sites), *(str(value) for value in required)])
        drain()
        rows = process_report(out / "bounds.jsonl", identity, "bounds", 0, sites, 6, deadline)
        verify_bounds(rows, required)
        manifest["boundaries"] = rows
        checked(snapshot(source, deadline) == inputs and config_snapshot(build, deadline) == config and
                sha(executable, deadline) == binary_sha, "Actual Source, configuration or binary changed")
        checked(not active, "Original child processes did not drain")
        manifest["native_drain"] = owned_drain(owner, deadline)
        names = ["census", *(f"shard-{index}" for index in range(WORKERS)), "bounds"]
        outputs = {name + suffix for name in names for suffix in (".jsonl", ".stdout.log", ".stderr.log")}
        checked({path.name for path in out.iterdir()} == outputs | {"qualification.json"}, "Undeclared test output")
        manifest["outputs"] = {name: sha(out / name, deadline) for name in sorted(outputs)}
        manifest.update(qualified=True, seconds=time.monotonic() - started)
        publish()
        remaining(deadline)  # Final receipt and fingerprint collection are inside the same deadline.
        print(f"Generic compiler: {sites} exact fresh-owner faults, six axes, physical0; {out}")
        return 0
    except Exception as error:
        manifest.update(qualified=False, failure=str(error), seconds=time.monotonic() - started)
        publish()
        print(f"Generic compiler FAIL: {error}; {out}", file=sys.stderr)
        return 1
    finally:
        for child, _ in active:
            if child.poll() is None:
                child.kill()
        for child, row in active:
            try:
                row["exit_code"] = child.wait(timeout=max(.01, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                row["exit_pending_parent_job_close"] = True
            row["terminated_on_failure"] = True
        if owner is not None and active:
            if owner["adapter"] == "windows-job":
                manifest["failure_job_active_processes"] = job_processes(owner["job"])
            publish()
        # Windows process exit closes the sole kill-on-close handle. POSIX
        # owner failure kills its whole group; outer-parent death closes the
        # lifeline and triggers the same group kill independently of finally.


if __name__ == "__main__":
    raise SystemExit(entry())

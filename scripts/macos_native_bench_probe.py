"""Replay the unchanged native benchmark runner and observe its Darwin children."""

import argparse
from contextlib import closing
import ctypes
import errno
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import select
import shutil
import subprocess
import sys
import time
from types import SimpleNamespace
from xml.etree import ElementTree

from macos_owned_process_probe import json_line, recorded, sample


BASE = "8862e25766664aef7769e1bb6d0ddf9612a564e6"
CANDIDATE = "79368992bad34459d53783d075c165930d78700e"
CONTROL = "native_bench_runner"
TEST = Path("bench/native/test_runner.py")
PYTHON = Path("/opt/homebrew/bin/python3.14")
INPUTS = (
    TEST, TEST.with_name("native_bench.cmake"), TEST.with_name("run.py"),
    TEST.with_name("reference.c"), TEST.with_name("reference.py"), TEST.with_name("timeout_fixture.py"),
    Path("scripts/xray_process.py"), Path("CMakeLists.txt"),
)


class BsdProcessInfo(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint32) for name in ("pid", "ppid", "pgid", "status")] + [
        ("command", ctypes.c_char * 16)
    ] + [(name, ctypes.c_uint32) for name in ("flags", "uid", "gid", "ruid", "rgid", "svuid", "svgid", "reserved")]


def probe_process_state():
    # The public short BSD record includes zombies when proc_pidinfo's arg is 1.
    library = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
    info = library.proc_pidinfo
    info.argtypes = (ctypes.c_int, ctypes.c_int, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_int)
    info.restype = ctypes.c_int

    def snapshot(pid):
        data = BsdProcessInfo()
        ctypes.set_errno(0)
        length = info(pid, 13, 1, ctypes.byref(data), ctypes.sizeof(data))
        valid = length == ctypes.sizeof(data) and data.pid == pid
        return {"pid": pid, "bytes": length, "errno": errno.errorcode.get(ctypes.get_errno()),
                "process": None if not valid else {
                    "status": data.status, "pid": data.pid, "ppid": data.ppid, "pgid": data.pgid}}

    return snapshot


def watch_exit(queue, pid):
    event = select.kevent(pid, filter=select.KQ_FILTER_PROC, fflags=select.KQ_NOTE_EXIT,
                          flags=select.KQ_EV_ADD | select.KQ_EV_ONESHOT)
    queue.control([event], 0, 0)


def exit_events(queue, count, wait):
    return [{"pid": event.ident, "flags": event.flags, "fflags": event.fflags, "data": event.data}
            for event in queue.control(None, count, wait)]


def is_exit(event):
    return not event["flags"] & select.KQ_EV_ERROR and bool(event["fflags"] & select.KQ_NOTE_EXIT)


def calibrate_exit_probe(snapshot, output):
    here = snapshot(os.getpid())["process"]
    assert (here["pid"], here["ppid"], here["pgid"]) == (os.getpid(), os.getppid(), os.getpgrp())
    assert here["status"] != 5
    with closing(select.kqueue()) as queue:
        child = subprocess.Popen([sys.executable, "-c", "import sys; sys.stdin.buffer.read(1)"],
                                 stdin=subprocess.PIPE)
        try:
            watch_exit(queue, child.pid)
            assert not exit_events(queue, 1, 0)
            child.stdin.close()
            event, = exit_events(queue, 1, 1)
            zombie = snapshot(child.pid)
            os.kill(child.pid, 0)
            assert is_exit(event) and event["pid"] == child.pid and zombie["process"]["status"] == 5
            assert zombie["process"]["ppid"] == os.getpid()
        finally:
            if not child.stdin.closed:
                child.stdin.close()
            child.wait(timeout=3)
    json_line(output, {"event": "calibration", "living": here, "unreaped": zombie, "exit": event})


def observe_exits(original, output):
    snapshot = probe_process_state()
    calibrate_exit_probe(snapshot, output)
    with closing(select.kqueue()) as queue:
        pids = []
        original_observe = original.observe_process
        run_path = original.runpy.run_path

        def observe(pid):
            watch_exit(queue, pid)
            pids.append(pid)
            return original_observe(pid)

        def wrap_run_path(*args, **kwargs):
            runner = run_path(*args, **kwargs)
            original_sample = runner["run_sample"]

            def run_sample(argv, *args, **kwargs):
                result = original_sample(argv, *args, **kwargs)
                if not pids:
                    return result
                finished = time.monotonic_ns()
                initial = exit_events(queue, len(pids), 0)
                zero_duration = time.monotonic_ns() - finished
                states = [snapshot(pid) for pid in pids]
                pending = set(pids) - {item["pid"] for item in initial if is_exit(item)}
                following = []
                wait_limit = finished + 100_000_000
                while pending and (remaining := wait_limit - time.monotonic_ns()) > 0:
                    for item in exit_events(queue, len(pending), remaining / 1e9):
                        item["elapsedNs"] = time.monotonic_ns() - finished
                        following.append(item)
                        if is_exit(item):
                            pending.discard(item["pid"])
                json_line(output, {"event": "fixture-exits", "mode": argv[2], "sampleStatus": result["status"],
                                   "pids": pids, "zeroTimeEvents": initial, "zeroDurationNs": zero_duration,
                                   "initialStates": states,
                                   "followingEvents": following, "pending": sorted(pending),
                                   "lateStates": [snapshot(pid) for pid in sorted(pending)]})
                pids.clear()
                return result

            runner["run_sample"] = run_sample
            return runner

        original.observe_process = observe
        original.runpy = SimpleNamespace(run_path=wrap_run_path)
        original.main()


def diagnose(test_args, output, direct_events):
    test, reference, products = test_args
    spec = importlib.util.spec_from_file_location("native_bench_runner_control", test)
    original = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(original)
    sys.argv = [test, "--reference", reference, "--output", products]
    if direct_events:
        observe_exits(original, output)
        return 0
    observer = original.process_exited
    started = time.monotonic()

    def observed_exited(pid):
        observation = sample(pid)
        json_line(output, {"event": "before-cleanup-check", "sinceStart": time.monotonic() - started,
                           **observation})
        result = observer(pid)
        json_line(output, {"event": "original-result", "pid": pid, "exited": result,
                           "sinceStart": time.monotonic() - started})
        return result

    original.process_exited = observed_exited
    original.main()
    return 0


def replay(output, direct_events):
    output.mkdir()
    workspace = output.with_name(output.name + "-workspace")
    workspace.mkdir()
    tooling = Path(__file__).resolve().parents[1]
    current_python = subprocess.check_output(
        [PYTHON, "-c", "import sys; print(sys.version)"], text=True, timeout=15).strip()
    host = {"hostname": platform.node(), "platform": platform.platform(), "runnerImage": os.environ.get("ImageOS"),
            "runnerImageVersion": os.environ.get("ImageVersion"), "workflowSha": os.environ.get("GITHUB_SHA"),
            "run": os.environ.get("GITHUB_RUN_ID"), "attempt": os.environ.get("GITHUB_RUN_ATTEMPT"),
            "ctestPython": str(PYTHON), "ctestPythonVersion": current_python, "toolingPython": sys.version}
    json_line(output / "summary.jsonl", {"host": host, "commits": {"base": BASE, "candidate": CANDIDATE}})
    if not current_python.startswith("3.14.10 "):
        raise ValueError("The original CTest interpreter version is unavailable")
    hashes, registrations = {}, {}
    focused = f"^{CONTROL}$"
    for side, commit in (("base", BASE), ("candidate", CANDIDATE)):
        source, build = workspace / side, workspace / f"{side}-build"
        recorded(output, f"{side}-checkout", ["git", "worktree", "add", "--detach", source, commit],
                 cwd=tooling, timeout=90, check=True)
        actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True, timeout=15).strip()
        if actual != commit:
            raise ValueError(f"{side} is not pinned to {commit}")
        hashes[side] = {str(path): hashlib.sha256((source / path).read_bytes()).hexdigest() for path in INPUTS}
        recorded(output, f"{side}-configure", ["cmake", "--preset", "default", "-B", build],
                 cwd=source, timeout=120, check=True)
        shutil.copyfile(build / "CMakeCache.txt", output / f"{side}-CMakeCache.txt")
        recorded(output, f"{side}-build-reference", ["cmake", "--build", build, "--target", "native_bench_reference"],
                 cwd=source, timeout=120, check=True)
        recorded(output, f"{side}-registry", ["ctest", "--test-dir", build, "-R", focused, "--show-only=json-v1"],
                 cwd=source, timeout=30, check=True)
        row, = json.loads((output / f"{side}-registry.stdout").read_bytes())["tests"]
        props = {item["name"]: item["value"] for item in row["properties"]}
        command = [str(part) for part in (PYTHON, source / TEST, "--reference", build / "native_bench_reference",
                                        "--output", build / "native-bench-runner-test")]
        if row["name"] != CONTROL or row["command"] != command or props["TIMEOUT"] != 30:
            raise ValueError(f"{side} registration differs from the original command and timeout")
        if Path(props["WORKING_DIRECTORY"]).resolve() != build:
            raise ValueError(f"{side} has an unexpected working directory")
        registrations[side] = (build, command)
    if hashes["base"] != hashes["candidate"]:
        raise ValueError("The benchmark control dependencies differ across the frozen revisions")
    json_line(output / "summary.jsonl", {"exactInputsSha256": hashes})

    formal = []
    for index, side in enumerate(() if direct_events else ("base", "candidate") * 4, 1):
        build, _ = registrations[side]
        name = f"formal-{index:02d}-{side}"
        junit = output / f"{name}.xml"
        status = recorded(output, name, ["ctest", "--test-dir", build, "-R", focused, "--no-tests=error",
                                        "--output-on-failure", "--output-junit", junit],
                          cwd=build, timeout=45)
        case, = ElementTree.parse(junit).getroot().iter("testcase")
        if case.get("name") != CONTROL or (status, case.get("status")) not in {(0, "run"), (8, "fail")}:
            raise ValueError(f"{name} did not produce an original CTest verdict")
        formal.append({"side": side, "exit": status, "seconds": float(case.get("time"))})

    diagnosis = []
    for index, side in enumerate(("candidate", "base") * (4 if direct_events else 2), 1):
        build, command = registrations[side]
        name = f"diagnostic-{index:02d}-{side}"
        witness = [PYTHON, "-B", __file__, "--output", output / f"{name}.jsonl",
                   "--observe", command[1], command[3], build / f"{name}-products"]
        if direct_events:
            witness.append("--direct-exit-events")
        status = recorded(output, name, witness, cwd=build, timeout=30)
        diagnosis.append({"side": side, "exit": status})
    json_line(output / "summary.jsonl", {"state": "COMPLETE_DIAGNOSTIC_ONLY", "formal": formal,
                                         "diagnostic": diagnosis})
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--observe", nargs=3)
    parser.add_argument("--direct-exit-events", action="store_true")
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("This replay requires Darwin")
    if args.observe:
        return diagnose(args.observe, args.output, args.direct_exit_events)
    return replay(args.output.resolve(), args.direct_exit_events)


if __name__ == "__main__":
    raise SystemExit(main())

"""Replay one original CTest on Darwin and observe process state separately."""

import argparse
import errno
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import time
import unittest
from xml.etree import ElementTree


BASELINE = "00cbb4924eca495a7f934a78dc2d7e16d66ca2b2"
CANDIDATE = "2b6e92dd345f6506de90d26316817b86f6e85cc4"
CONTROL = "legacy_consumer_owned_process_tree_control"
TEST = Path("tests/unit/xir/legacy_consumers/test_owned_process_tree.py")
SHARED = Path("tests/lib/xraytest/proc.py")
REGISTRATION = TEST.with_name("CMakeLists.txt")
PACKAGE = SHARED.with_name("__init__.py")


def json_line(path, data):
    with path.open("a", encoding="utf-8", newline="\n") as stream:
        stream.write(json.dumps(data, sort_keys=True) + "\n")


def recorded(logs, name, argv, *, cwd, timeout, check=False):
    argv = [str(arg) for arg in argv]
    start = time.monotonic()
    with (logs / f"{name}.stdout").open("xb") as out, (logs / f"{name}.stderr").open("xb") as err:
        try:
            status = subprocess.run(argv, stdout=out, stderr=err, cwd=cwd, timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            status = None
    json_line(logs / "commands.jsonl", {"name": name, "argv": argv, "cwd": str(cwd), "exit": status,
                                        "supervisorTimeout": timeout, "seconds": time.monotonic() - start})
    if status is None or (check and status != 0):
        raise RuntimeError(f"{name} did not complete successfully: {status!r}")
    return status


def sample(pid):
    try:
        os.kill(pid, 0)
    except OSError as error:
        kill = {"success": False, "errno": error.errno, "name": errno.errorcode.get(error.errno)}
    else:
        kill = {"success": True}
    argv = ["/bin/ps", "-o", "state=", "-o", "pid=", "-o", "ppid=", "-o", "pgid=", "-p", str(pid)]
    result = subprocess.run(argv, capture_output=True, text=True, cwd="/", timeout=2)
    fields = result.stdout.split()
    process = None
    if result.returncode == 0 and len(fields) == 4 and int(fields[1]) == pid:
        process = dict(zip(("state", "pid", "ppid", "pgid"), (fields[0], *map(int, fields[1:]))))
    return {"pid": pid, "kill0": kill, "ps": {"argv": argv, "exit": result.returncode, "stdout": result.stdout,
                                           "stderr": result.stderr, "process": process}}


def diagnose(source, output):
    """Observe the original control through pass-through hooks."""
    spec = importlib.util.spec_from_file_location("original_owned_process_control", source / TEST)
    control = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(control)
    control.configure(source)
    pids = {}
    started = time.monotonic()
    original_run, original_running = control.proc.run, control.running

    def run(argv, **kwargs):
        result = original_run(argv, **kwargs)
        pids.update(json.loads(result.stdout))
        json_line(output, {"event": "helper-return", "sinceStart": time.monotonic() - started,
                           "pids": pids, "timeout": kwargs["timeout"], "timedOut": result.timed_out,
                           "returncode": result.returncode, "stderrHex": result.stderr.hex()})
        for role in ("child", "parent"):
            json_line(output, {"event": "after-helper", "role": role, "sinceStart": time.monotonic() - started,
                               **sample(pids[role])})
        return result

    def running(pid, terminate=False):
        result = original_running(pid, terminate)
        if not terminate:
            json_line(output, {"event": "original-liveness-check", "sinceStart": time.monotonic() - started,
                               "originalResult": result, **sample(pid)})
        return result

    control.proc.run = run
    control.running = running
    tests = unittest.defaultTestLoader.loadTestsFromModule(control)
    result = unittest.TextTestRunner(verbosity=2).run(tests)
    for delay in (0, 0.05, 0.2, 1):
        time.sleep(delay)
        for role, pid in pids.items():
            json_line(output, {"event": "after-control", "role": role, "sinceStart": time.monotonic() - started,
                               **sample(pid)})
    return 0 if result.wasSuccessful() and result.testsRun == 1 else 1


def replay(output):
    output.mkdir()
    tool = Path(__file__).resolve().parents[1]
    workspace = output.parent / "task339-owned-process-workspace"
    workspace.mkdir()
    host = {"hostname": platform.node(), "platform": platform.platform(), "python": sys.version,
            "pythonExecutable": sys.executable, "runnerImage": os.environ.get("ImageOS"),
            "runnerImageVersion": os.environ.get("ImageVersion"), "workflowSha": os.environ.get("GITHUB_SHA"),
            "run": os.environ.get("GITHUB_RUN_ID"), "attempt": os.environ.get("GITHUB_RUN_ATTEMPT")}
    json_line(output / "summary.jsonl", {"host": host, "commits": {"base": BASELINE, "candidate": CANDIDATE}})
    digests = {}
    paths = {}
    focused = f"^{CONTROL}$"
    for side, commit in (("base", BASELINE), ("candidate", CANDIDATE)):
        source, build = workspace / side, workspace / f"{side}-build"
        paths[side] = (source, build)
        recorded(output, f"{side}-checkout", ["git", "worktree", "add", "--detach", source, commit],
                 cwd=tool, timeout=60, check=True)
        actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
        if actual != commit:
            raise ValueError(f"{side} is not pinned to {commit}")
        digests[side] = {str(file): hashlib.sha256((source / file).read_bytes()).hexdigest()
                         for file in (TEST, SHARED, REGISTRATION, PACKAGE)}
        recorded(output, f"{side}-configure", ["cmake", "--preset", "default", "-B", build],
                 cwd=source, timeout=120, check=True)
        recorded(output, f"{side}-registry", ["ctest", "--test-dir", build, "-R", focused, "--show-only=json-v1"],
                 cwd=source, timeout=30, check=True)
        row, = json.loads((output / f"{side}-registry.stdout").read_bytes())["tests"]
        properties = {prop["name"]: prop["value"] for prop in row["properties"]}
        if (row["name"] != CONTROL or properties["TIMEOUT"] != 30 or
                row["command"][1:] != ["-X", "utf8", "-B", str(source / TEST), "--source-root", str(source)] or
                Path(row["command"][0]).resolve() != Path(sys.executable).resolve()):
            raise ValueError(f"{side} did not register the exact control with its original limits")
    if digests["base"] != digests["candidate"]:
        raise ValueError("The original control or its execution dependencies differ")
    json_line(output / "summary.jsonl", {"exactInputsSha256": digests})

    formal = []
    for index, side in enumerate(("base", "candidate") * 4, 1):
        source, build = paths[side]
        name = f"formal-{index:02d}-{side}"
        junit = output / f"{name}.xml"
        status = recorded(output, name, ["ctest", "--test-dir", build, "-R", focused, "--no-tests=error",
                                        "--output-on-failure", "--output-junit", junit],
                          cwd=source, timeout=45)
        case, = ElementTree.parse(junit).getroot().iter("testcase")
        if case.get("name") != CONTROL or (status, case.get("status")) not in {(0, "run"), (8, "fail")}:
            raise ValueError(f"{name} did not record a completed observation of the original control")
        formal.append({"side": side, "exit": status})

    diagnostics = []
    for index, side in enumerate(("candidate", "base") * 2, 1):
        source, _ = paths[side]
        name = f"diagnostic-{index:02d}-{side}"
        status = recorded(output, name, [sys.executable, "-X", "utf8", "-B", __file__, "--diagnose",
                                         source, "--output", output / f"{name}.jsonl"], cwd=source, timeout=30)
        diagnostics.append({"side": side, "exit": status})
    json_line(output / "summary.jsonl", {"state": "COMPLETE_DIAGNOSTIC_ONLY", "formal": formal,
                                         "diagnostic": diagnostics})
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--diagnose", type=Path)
    args = parser.parse_args()
    if (sys.platform, sys.version_info[:3]) != ("darwin", (3, 11, 9)):
        parser.error("This replay uses Darwin and the original Python 3.11.9")
    if args.diagnose:
        return diagnose(args.diagnose, args.output)
    return replay(args.output.resolve())


if __name__ == "__main__":
    raise SystemExit(main())

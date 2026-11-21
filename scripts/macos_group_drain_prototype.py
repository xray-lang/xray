"""Compare a Darwin group-drain overlay against its unchanged registered controls."""

import argparse
from contextlib import closing
import errno
import hashlib
import json
import os
from pathlib import Path
import platform
import runpy
import select
import shutil
import subprocess
import sys
import time
from unittest.mock import patch
from xml.etree import ElementTree

from macos_native_bench_probe import (
    BASE, CANDIDATE, CONTROL, INPUTS, PYTHON, TEST, exit_events, is_exit, probe_process_state, watch_exit,
)
from macos_owned_process_probe import json_line, recorded


LEGACY = "legacy_consumer_owned_process_tree_control"
LEGACY_SOURCE = Path("tests/unit/xir/legacy_consumers/test_owned_process_tree.py")
HELPER = Path("scripts/xray_process.py")
WAIT_GROUP = '''
def _wait_darwin_group(group: int, deadline: float) -> None:
    """Require Darwin to report no remaining members of the original group."""
    while True:
        try:
            os.killpg(group, 0)
        except ProcessLookupError:
            return
        except PermissionError:
            pass
        except OSError as error:
            raise RecoveryRequired("Cannot verify command process group termination") from error
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise RecoveryRequired("Command process group did not disappear")
        time.sleep(min(0.01, remaining))


'''


def replacement(source):
    edits = (
        ("import subprocess\nimport time", "import subprocess\nimport sys\nimport time"),
        ("def _kill_group(group: int) -> None:",
         "def _kill_group(group: int, deadline: float | None = None) -> None:"),
        ("    deadline = time.monotonic() + GROUP_EXIT_SECONDS\n    while True:",
         "    if deadline is None:\n        deadline = time.monotonic() + GROUP_EXIT_SECONDS\n    while True:"),
        ("def run_child(", WAIT_GROUP.lstrip("\n") + "def run_child("),
        ("                _kill_group(child.pid)\n                child.wait()",
         "                deadline = time.monotonic() + GROUP_EXIT_SECONDS if sys.platform == \"darwin\" else None\n"
         "                _kill_group(child.pid, deadline)\n"
         "                child.wait()\n"
         "                if deadline is not None:\n"
         "                    _wait_darwin_group(child.pid, deadline)"),
    )
    for old, new in edits:
        if source.count(old) != 1:
            raise ValueError("The exact helper no longer has the expected insertion boundary")
        source = source.replace(old, new)
    return source.encode()


def negative_witness(helper, output):
    original = runpy.run_path(str(helper))
    prove, recovery, budget = (original[key] for key in
                               ("_wait_darwin_group", "RecoveryRequired", "GROUP_EXIT_SECONDS"))
    assert budget == 5
    child = subprocess.Popen([sys.executable, "-c", "import sys; sys.stdin.buffer.read(1)"],
                             stdin=subprocess.PIPE, start_new_session=True)
    real_killpg = os.killpg
    start = time.monotonic()
    queries = []

    def record_query(group, signal):
        assert group == child.pid and signal == 0
        queries.append(time.monotonic() - start)
        return real_killpg(group, signal)

    try:
        assert os.getpgid(child.pid) == child.pid
        with patch.object(os, "killpg", side_effect=record_query):
            try:
                prove(child.pid, start + budget)
            except recovery as error:
                failure = str(error)
            else:
                raise AssertionError("A live process group was declared drained")
        duration = time.monotonic() - start
        assert failure == "Command process group did not disappear" and duration >= budget
        assert child.poll() is None and queries
        real_killpg(child.pid, 0)
    finally:
        child.stdin.close()
        child.wait(timeout=5)
    prove(child.pid, time.monotonic() + budget)
    json_line(output, {"case": "live-group-refused", "budgetSeconds": budget, "elapsedSeconds": duration,
                       "queries": len(queries), "firstQuerySeconds": queries[0], "lastQuerySeconds": queries[-1],
                       "keptAliveThroughout": True, "onlySignalZero": True, "reapedGroupThenAdmitted": True})

    for number in (errno.EPERM, errno.EIO):
        with patch.object(os, "killpg", side_effect=OSError(number, os.strerror(number))) as unavailable:
            try:
                prove(child.pid, time.monotonic())
            except recovery as error:
                assert unavailable.call_args.args == (child.pid, 0)
                json_line(output, {"case": "unavailable-group-proof-refused", "errno": number,
                                   "exception": str(error), "queries": unavailable.call_count})
            else:
                raise AssertionError(f"An unavailable group query was accepted: {number}")

    with closing(select.kqueue()) as queue:
        child = subprocess.Popen([sys.executable, "-c", "import sys; sys.stdin.buffer.read(1)"],
                                 stdin=subprocess.PIPE, start_new_session=True)
        try:
            watch_exit(queue, child.pid)
            child.stdin.close()
            event, = exit_events(queue, 1, 1)
            state = probe_process_state()(child.pid)["process"]
            assert is_exit(event) and state["status"] == 5 and state["pgid"] == child.pid
            try:
                real_killpg(child.pid, 0)
            except OSError as error:
                group = errno.errorcode[error.errno]
            else:
                group = "exists"
            try:
                prove(child.pid, time.monotonic())
            except recovery:
                accepted = False
            else:
                accepted = True
            assert accepted == (group == "ESRCH")
        finally:
            if not child.stdin.closed:
                child.stdin.close()
            child.wait(timeout=5)
    prove(child.pid, time.monotonic() + budget)
    json_line(output, {"case": "known-unreaped-group", "groupQuery": group, "process": state,
                       "acceptedUnreaped": accepted, "admittedAfterReap": True})


def replay(output):
    output.mkdir()
    workspace = output.with_name(output.name + "-workspace")
    workspace.mkdir()
    source, build = workspace / "source", workspace / "build"
    tooling = Path(__file__).resolve().parents[1]
    host = {"hostname": platform.node(), "platform": platform.platform(), "runnerImage": os.environ.get("ImageOS"),
            "runnerImageVersion": os.environ.get("ImageVersion"), "workflowSha": os.environ.get("GITHUB_SHA"),
            "run": os.environ.get("GITHUB_RUN_ID"), "attempt": os.environ.get("GITHUB_RUN_ATTEMPT")}
    json_line(output / "summary.jsonl", {"host": host, "base": BASE, "sourceCommit": CANDIDATE})
    recorded(output, "checkout", ["git", "worktree", "add", "--detach", source, CANDIDATE],
             cwd=tooling, timeout=90, check=True)
    before = (source / HELPER).read_bytes()
    after = replacement(before.decode())
    compile(after, str(source / HELPER), "exec")
    (output / "original-xray_process.py").write_bytes(before)
    (output / "overlay-xray_process.py").write_bytes(after)
    (source / HELPER).write_bytes(after)
    recorded(output, "overlay-diff", ["git", "diff", "--", str(HELPER)], cwd=source, timeout=15, check=True)
    (source / HELPER).write_bytes(before)
    recorded(output, "configure", ["cmake", "--preset", "default", "-B", build],
             cwd=source, timeout=120, check=True)
    recorded(output, "build-reference", ["cmake", "--build", build, "--target", "native_bench_reference"],
             cwd=source, timeout=120, check=True)
    shutil.copyfile(build / "CMakeCache.txt", output / "CMakeCache.txt")
    focused = f"^({CONTROL}|{LEGACY})$"
    recorded(output, "registry", ["ctest", "--test-dir", build, "-R", focused, "--show-only=json-v1"],
             cwd=source, timeout=30, check=True)
    rows = {row["name"]: row for row in json.loads((output / "registry.stdout").read_bytes())["tests"]}
    assert set(rows) == {CONTROL, LEGACY}
    for row in rows.values():
        assert {item["name"]: item["value"] for item in row["properties"]}["TIMEOUT"] == 30
    assert rows[CONTROL]["command"] == [str(part) for part in
                                       (PYTHON, source / TEST, "--reference", build / "native_bench_reference",
                                        "--output", build / "native-bench-runner-test")]
    assert rows[LEGACY]["command"][1:] == ["-X", "utf8", "-B", str(source / LEGACY_SOURCE),
                                          "--source-root", str(source)]
    ref_hash = hashlib.sha256((build / "native_bench_reference").read_bytes()).hexdigest()
    baseline_inputs = {str(path): hashlib.sha256((source / path).read_bytes()).hexdigest()
                       for path in (*INPUTS, LEGACY_SOURCE, Path("tests/lib/xraytest/proc.py")) if path != HELPER}
    observations = []
    for index, arm in enumerate(("original", "overlay", "overlay", "original") * 2, 1):
        name = f"{index:02d}-{arm}"
        patched = arm == "overlay"
        (source / HELPER).write_bytes(after if patched else before)
        dirty = subprocess.check_output(["git", "status", "--porcelain=v1"], cwd=source, text=True, timeout=15)
        assert dirty == (" M scripts/xray_process.py\n" if patched else "")
        junit = output / f"{name}.xml"
        status = recorded(output, name, ["ctest", "--test-dir", build, "-R", focused, "--no-tests=error",
                                        "--output-on-failure", "--output-junit", junit],
                          cwd=build, timeout=75)
        outcomes = {case.get("name"): {"state": case.get("status"), "seconds": float(case.get("time"))}
                    for case in ElementTree.parse(junit).getroot().iter("testcase")}
        assert set(outcomes) == set(rows) and all(c["state"] in ("run", "fail") for c in outcomes.values())
        assert status == (8 if any(c["state"] == "fail" for c in outcomes.values()) else 0)
        observations.append({"arm": arm, "runnerExit": status, "outcomes": outcomes})
    assert hashlib.sha256((build / "native_bench_reference").read_bytes()).hexdigest() == ref_hash
    assert {str(path): hashlib.sha256((source / path).read_bytes()).hexdigest()
            for path in (*INPUTS, LEGACY_SOURCE, Path("tests/lib/xraytest/proc.py")) if path != HELPER} == baseline_inputs
    (source / HELPER).write_bytes(after)
    try:
        negative_witness(source / HELPER, output / "negative.jsonl")
    finally:
        (source / HELPER).write_bytes(before)
    json_line(output / "summary.jsonl", {"state": "COMPLETE_DIAGNOSTIC_ONLY", "referenceSHA256": ref_hash,
                                         "originalHelperSHA256": hashlib.sha256(before).hexdigest(),
                                         "overlayHelperSHA256": hashlib.sha256(after).hexdigest(),
                                         "observations": observations})
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("This diagnostic requires Darwin")
    raise SystemExit(replay(args.output.resolve()))

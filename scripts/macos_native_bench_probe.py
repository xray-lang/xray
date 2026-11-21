"""Replay the unchanged native benchmark runner and observe its Darwin children."""

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time
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


def diagnose(test_args, output):
    test, reference, products = test_args
    spec = importlib.util.spec_from_file_location("native_bench_runner_control", test)
    original = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(original)
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
    sys.argv = [test, "--reference", reference, "--output", products]
    original.main()
    return 0


def replay(output):
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
    for index, side in enumerate(("base", "candidate") * 4, 1):
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
    for index, side in enumerate(("candidate", "base") * 2, 1):
        build, command = registrations[side]
        name = f"diagnostic-{index:02d}-{side}"
        status = recorded(output, name, [PYTHON, "-B", __file__, "--output", output / f"{name}.jsonl", "--observe",
                                         command[1], command[3], build / f"{name}-products"], cwd=build, timeout=30)
        diagnosis.append({"side": side, "exit": status})
    json_line(output / "summary.jsonl", {"state": "COMPLETE_DIAGNOSTIC_ONLY", "formal": formal,
                                         "diagnostic": diagnosis})
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--observe", nargs=3)
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("This replay requires Darwin")
    if args.observe:
        return diagnose(args.observe, args.output)
    return replay(args.output.resolve())


if __name__ == "__main__":
    raise SystemExit(main())

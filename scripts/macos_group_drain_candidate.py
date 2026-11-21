"""Run the unchanged focused macOS controls on the exact Darwin drain candidate."""

import argparse
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess
from xml.etree import ElementTree

from macos_group_drain_prototype import LEGACY, LEGACY_SOURCE
from macos_native_bench_probe import BASE, CONTROL, PYTHON, TEST
from macos_owned_process_probe import json_line, recorded


CLEANUP = "child_process_cleanup"
CANDIDATE = "97246785fb39dd37d75c6d761c506830b10e40ab"
DIFF = ["scripts/xray_process.py", "tests/tools/test_xray_process.py"]


def run(output):
    output.mkdir()
    workspace = output.with_name(output.name + "-workspace")
    workspace.mkdir()
    source, build = workspace / "source", workspace / "build"
    root = Path(__file__).resolve().parents[1]
    report = output / "summary.jsonl"
    json_line(report, {"host": platform.platform(), "hostname": platform.node(), "baseline": BASE, "candidate": CANDIDATE})
    recorded(output, "checkout", ["git", "worktree", "add", "--detach", source, CANDIDATE], cwd=root, timeout=120, check=True)

    def git(*args):
        return subprocess.check_output(["git", *args], cwd=source, text=True, timeout=30).splitlines()

    assert git("rev-parse", "HEAD", "HEAD^") == [CANDIDATE, BASE] and not git("status", "--short")
    assert git("diff", "--name-only", BASE, CANDIDATE) == DIFF
    recorded(output, "configure", ["cmake", "--preset", "default", "-B", build], cwd=source, timeout=120, check=True)
    recorded(output, "build-reference", ["cmake", "--build", build, "--target", "native_bench_reference"],
             cwd=source, timeout=120, check=True)
    shutil.copyfile(build / "CMakeCache.txt", output / "CMakeCache.txt")
    focused = f"^({CLEANUP}|{CONTROL}|{LEGACY})$"
    recorded(output, "registry", ["ctest", "--test-dir", build, "-R", focused, "--show-only=json-v1"],
             cwd=source, timeout=30, check=True)
    actual = {row["name"]: row for row in json.loads((output / "registry.stdout").read_bytes())["tests"]}
    assert set(actual) == {CLEANUP, CONTROL, LEGACY}
    for row in actual.values():
        assert {p["name"]: p["value"] for p in row["properties"]}["TIMEOUT"] == 30
    assert actual[CLEANUP]["command"] == [str(PYTHON), "-B", str(source / DIFF[1])]
    assert actual[CONTROL]["command"] == [str(PYTHON), str(source / TEST),
                                          "--reference", str(build / "native_bench_reference"),
                                          "--output", str(build / "native-bench-runner-test")]
    assert actual[LEGACY]["command"][1:] == ["-X", "utf8", "-B", str(source / LEGACY_SOURCE),
                                            "--source-root", str(source)]
    results = []
    for index in range(1, 5):
        name = f"focused-{index:02d}"
        junit = output / f"{name}.xml"
        status = recorded(output, name, ["ctest", "--test-dir", build, "-R", focused, "--no-tests=error",
                                        "--output-on-failure", "--output-junit", junit],
                          cwd=build, timeout=100)
        cases = {case.get("name"): {"status": case.get("status"), "seconds": float(case.get("time"))}
                 for case in ElementTree.parse(junit).getroot().iter("testcase")}
        assert set(cases) == set(actual) and all(row["status"] in {"run", "fail"} for row in cases.values())
        assert status == (8 if any(row["status"] == "fail" for row in cases.values()) else 0)
        results.append({"exit": status, "outcomes": cases})
    assert not git("status", "--short") and git("rev-parse", "HEAD") == [CANDIDATE]
    passed = all(not row["exit"] for row in results)
    hashes = {path: hashlib.sha256((source / path).read_bytes()).hexdigest() for path in DIFF}
    json_line(report, {"state": "PASS_FOCUSED" if passed else "FAIL_FOCUSED", "sourceClean": True,
                       "inputsSHA256": hashes, "trials": results})
    return int(not passed)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if platform.system() != "Darwin":
        parser.error("This focused verification requires Darwin")
    raise SystemExit(run(args.output.resolve()))

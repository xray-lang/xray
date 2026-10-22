#!/usr/bin/env python3
"""Exercise real Git, Ninja and CTest transitions in isolated temporary repositories."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

QUEUE = Path(__file__).resolve().parents[2] / "scripts/merge_slice.py"
sys.path.insert(0, str(QUEUE.parent))
import merge_slice
from xray_process import RecoveryRequired
GATE = '''import os, pathlib, subprocess, sys, time
root = pathlib.Path(__file__).parent
mode, build = sys.argv[1:]
if mode == 'l1':
    time.sleep(0.15)
    if (root / 'fail').exists(): raise SystemExit(3)
    if (root / 'race').exists():
        old, new = (root / 'race').read_text().split()
        subprocess.run(['git','-C',str(root),'update-ref','refs/heads/main',new,old],check=True)
if mode == 'asan':
    assert os.environ['XR_ASAN_PROFILE'] == 'full'
    assert os.environ['XR_ASAN_JOBS'] == '4'
    assert pathlib.Path(os.environ['XR_ASAN_BUILD_DIR']).is_absolute()
    (pathlib.Path(build) / 'asan-ran').write_text('PASS')
print(mode + ': PASS')
'''
CMAKE = '''cmake_minimum_required(VERSION 3.25)
project(QueueFixture NONE)
enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                OUTPUT_VARIABLE identity OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
file(WRITE "${CMAKE_BINARY_DIR}/identity.json" "{\\\"commit\\\":\\\"${identity}\\\",\\\"dirty\\\":false}")
add_custom_target(xray COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/gate.py" build "${CMAKE_BINARY_DIR}")
add_test(NAME actual COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/gate.py" l1 "${CMAKE_BINARY_DIR}")
add_test(NAME meta_ownership_inventory COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/gate.py" metadata "${CMAKE_BINARY_DIR}")
add_test(NAME asan_focused COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/gate.py" asan "${CMAKE_BINARY_DIR}")
'''


class QueueTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="xray-queue-test-")
        self.root = Path(self.temporary.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        self.children = []
        self.git("init", "-b", "main")
        self.git("config", "user.name", "Queue Fixture")
        self.git("config", "user.email", "fixture@example.com")
        self.git("config", "core.autocrlf", "false")
        self.write("gate.py", GATE)
        self.write("version.py", "from pathlib import Path\nimport sys\nprint(Path(sys.argv[1]).read_text())\n")
        self.write("CMakeLists.txt", CMAKE)
        plan = {"schema": 1, "targets": ["xray"], "tests": ["actual"],
                "versionCommand": [sys.executable, "{source}/version.py", "{build}/identity.json"],
                "smokeStatus": "fixture only"}
        self.write("scripts/l1-v0.json", json.dumps(plan))
        self.git("add", ".")
        self.git("commit", "-m", "Create queue fixture")
        self.base = self.git("rev-parse", "HEAD")
        self.integration = self.root / "int"
        self.git("worktree", "add", "--detach", str(self.integration), self.base)
        self.git("switch", "--detach")
        self.evidence = self.root / "review.txt"
        self.evidence.write_text("Synthetic review for isolated queue tests only.\n")
        self.l0 = self.root / "l0.log"
        self.l0.write_text("fixture L0 passed\n")
        self.asan_log = self.root / "slice-asan.log"
        self.asan_log.write_text("Synthetic slice ASan result for isolated queue tests only.\n")

    def tearDown(self):
        for child in self.children:
            if child.poll() is None:
                child.kill()
            child.wait()
        report = os.environ.get("XR_QUEUE_TEST_REPORT_DIR")
        if report and (self.root / "queue").exists():
            destination = Path(report) / self._testMethodName
            shutil.copytree(self.root / "queue", destination, dirs_exist_ok=True)
        self.temporary.cleanup()

    def git(self, *arguments, root=None):
        return subprocess.check_output(["git", "-C", str(root or self.repo), *arguments],
                                       stderr=subprocess.DEVNULL, text=True).strip()

    def write(self, path, value):
        file = self.repo / path
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(value, encoding="utf-8")

    def branch(self, name, files, invalid=False):
        self.git("switch", "-c", name, self.base)
        for path, text in files.items():
            self.write(path, text)
        self.git("add", "--", *files)
        args = ["-c", "user.email=fixture@xray.invalid"] if invalid else []
        self.git(*args, "commit", "-m", "Change fixture input")
        sha = self.git("rev-parse", "HEAD")
        self.git("switch", "--detach")
        review = self.root / (name.replace("/", "-") + ".json")
        review.write_text(json.dumps({"approved": True, "sourceSHA": sha, "reviewedBase": self.base,
                                     "authorRole": "a", "reviewerRole": "b",
                                     "reviewEvidence": str(self.evidence), "l0Log": str(self.l0),
                                     "sliceAsanLog": str(self.asan_log), "sliceAsanResult": "PASS"}))
        return name, review, sha

    def start(self, branch, extra=()):
        name, review, _ = branch
        args = [sys.executable, str(QUEUE), "--branch", name, "--review", str(review),
                "--repo", str(self.repo), "--integration", str(self.integration),
                "--build", str(self.root / "build"),
                "--temporary", str(self.root / "tmp"), "--slots", str(self.root / "slots"),
                "--queue", str(self.root / "queue"), *extra]
        child = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.children.append(child)
        return child

    def logs(self):
        return [json.loads(p.read_text(encoding="utf-8"))
                for p in sorted((self.root / "queue/logs").glob("*/result.json"))]

    def finish(self, child, code):
        self.assertEqual(child.wait(timeout=60), code, self.logs())
        return self.logs()[-1]

    def test_pass_fast_forwards_and_runs_exact_test(self):
        branch = self.branch("a/pass", {"pass": "yes"})
        log = self.finish(self.start(branch), 0)
        self.assertEqual(log["state"], "MERGED")
        self.assertEqual(log["oldMain"], self.base)
        self.assertEqual(self.git("rev-parse", "main"), log["candidate"])
        self.assertEqual(log["gates"]["L1"], "PASS")
        self.assertEqual(len(self.logs()), 1)

    def test_failure_keeps_main_and_allows_next_request(self):
        bad = self.branch("a/fail", {"fail": "yes"})
        self.finish(self.start(bad), 1)
        self.assertEqual(self.git("rev-parse", "main"), self.base)
        good = self.branch("a/next", {"next": "yes"})
        self.finish(self.start(good), 0)

    def test_concurrent_requests_are_fifo_and_rebuild_on_new_main(self):
        first = self.branch("a/first", {"first": "yes"})
        second = self.branch("a/second", {"second": "yes"})
        child1 = self.start(first)
        deadline = time.monotonic() + 10
        while not list((self.root / "queue/requests").glob("*.json")):
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.01)
        child2 = self.start(second)
        self.finish(child1, 0)
        self.finish(child2, 0)
        one, two = self.logs()
        self.assertLess(one["sequence"], two["sequence"])
        self.assertGreaterEqual(two["processingStarted"], one["finished"])
        self.assertEqual(two["oldMain"], one["newMain"])
        self.assertNotEqual(two["candidate"], second[2])

    def test_compiler_change_uses_reviewed_slice_asan_while_full_lane_is_red(self):
        branch = self.branch("a/aot", {"src/aot/change.c": "/* fixture */"})
        log = self.finish(self.start(branch), 0)
        self.assertEqual(log["gates"]["meta_ownership_inventory"], "PASS")
        self.assertEqual(log["gates"]["slice_asan"], "PASS")
        self.assertIn("NOT_RUN: redlist-2026-10-21", log["gates"]["asan_focused"])
        self.assertFalse((self.root / "build/asan-ran").exists())
        saved = self.root / "queue/logs" / f"{log['sequence']:020d}" / "slice-asan.log"
        self.assertEqual(saved.read_bytes(), self.asan_log.read_bytes())
        slots = [json.loads(p.read_text()) for p in (self.root / "slots/requests").glob("*.json")]
        self.assertEqual([r["kind"] for r in slots], ["build"])

    def test_placeholder_identity_is_rejected(self):
        branch = self.branch("a/invalid", {"invalid": "yes"}, invalid=True)
        log = self.finish(self.start(branch), 1)
        self.assertIn("Placeholder", log["error"])
        self.assertEqual(self.git("rev-parse", "main"), self.base)

    def test_missing_test_and_external_manifest_cannot_pass(self):
        plan = json.loads((self.repo / "scripts/l1-v0.json").read_text())
        plan["tests"].append("missing-test")
        branch = self.branch("a/missing", {"scripts/l1-v0.json": json.dumps(plan)})
        log = self.finish(self.start(branch), 1)
        self.assertIn("requested tests differ", log["error"])
        log = self.finish(self.start(branch, ("--manifest", str(self.repo / "scripts/l1-v0.json"))), 1)
        self.assertIn("tracked path", log["error"])
        self.assertEqual(self.git("rev-parse", "main"), self.base)

    def test_failed_or_unrun_slice_asan_never_advances_main(self):
        branch = self.branch("a/asan-failed", {"src/lir/change.c": "/* fixture */"})
        review = json.loads(branch[1].read_text())
        for result in ("FAIL", "NOT_RUN", "NOT_APPLICABLE"):
            review.update(sliceAsanResult=result, sliceAsanReason="fixture reason")
            branch[1].write_text(json.dumps(review))
            log = self.finish(self.start(branch), 1)
            self.assertEqual(log["gates"]["slice_asan"], "FAIL")
            self.assertEqual(self.git("rev-parse", "main"), self.base)

    def test_non_native_slice_requires_a_reason_for_asan_exemption(self):
        branch = self.branch("a/docs", {"spec/source/entry.md": "fixture"})
        review = json.loads(branch[1].read_text())
        review.update(sliceAsanResult="NOT_APPLICABLE")
        branch[1].write_text(json.dumps(review))
        self.finish(self.start(branch), 1)
        review["sliceAsanReason"] = "Only specification prose changed."
        branch[1].write_text(json.dumps(review))
        log = self.finish(self.start(branch), 0)
        self.assertTrue(log["gates"]["slice_asan"].startswith("NOT_APPLICABLE:"))

    def test_missing_slice_log_is_rejected_before_queueing(self):
        branch = self.branch("a/no-log", {"src/hir/change.c": "/* fixture */"})
        self.asan_log.unlink()
        self.assertEqual(self.start(branch).wait(timeout=60), 1)
        self.assertEqual(self.logs(), [])
        self.assertEqual(self.git("rev-parse", "main"), self.base)

    def test_main_compare_and_swap_does_not_overwrite_a_race(self):
        alternate = self.branch("b/alternate", {"alternate": "yes"})
        branch = self.branch("a/race", {"race": self.base + " " + alternate[2]})
        log = self.finish(self.start(branch), 1)
        self.assertIn("update-ref", log["error"])
        self.assertEqual(self.git("rev-parse", "main"), alternate[2])

    def test_moved_branch_invalidates_review(self):
        branch = self.branch("a/reviewed", {"reviewed": "yes"})
        alternate = self.branch("b/moved", {"moved": "yes"})
        self.git("update-ref", "refs/heads/" + branch[0], alternate[2], branch[2])
        log = self.finish(self.start(branch), 1)
        self.assertIn("moved after", log["error"])
        self.assertEqual(self.git("rev-parse", "main"), self.base)

    def test_rebase_rejects_a_new_placeholder_committer(self):
        first = self.branch("a/first", {"first": "yes"})
        second = self.branch("a/second", {"second": "yes"})
        merged = self.finish(self.start(first), 0)["newMain"]
        self.git("config", "extensions.worktreeConfig", "true")
        self.git("config", "--worktree", "user.email", "unset@xray.invalid", root=self.integration)
        log = self.finish(self.start(second), 1)
        self.assertIn("Rebased candidate", log["error"])
        self.assertEqual(self.git("rev-parse", "main"), merged)

    def test_recovery_exception_blocks_the_queue_itself(self):
        branch = self.branch("a/recovery", {"recovery": "yes"})
        arguments = [str(QUEUE), "--branch", branch[0], "--review", str(branch[1]),
                     "--repo", str(self.repo), "--queue", str(self.root / "queue")]
        with mock.patch.object(sys, "argv", arguments), mock.patch.object(
                merge_slice, "integrate", side_effect=RecoveryRequired("process tree unknown")):
            with self.assertRaises(RecoveryRequired):
                merge_slice.main()
        self.assertEqual(self.logs()[0]["state"], "NEEDS_RECOVERY")
        request = next((self.root / "queue/requests").glob("*.json"))
        self.assertEqual(json.loads(request.read_text())["state"], "NEEDS_RECOVERY")
        self.assertEqual(self.git("rev-parse", "main"), self.base)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Exercise real queue transitions using isolated Git and recorded CTest fixtures."""

import hashlib
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
import xml.etree.ElementTree as ET

QUEUE = Path(__file__).resolve().parents[2] / "scripts/merge_slice.py"
sys.path.insert(0, str(QUEUE.parent))
import merge_slice
import xray_l1
from xray_process import RecoveryRequired
FIXTURE_TESTS = {"actual", "smoke"}
FULL_TESTS = FIXTURE_TESTS | {"meta_ownership_inventory", "historical_failure"}
COMPARISON_HOSTS = ("windows-2022", "macos-latest")
TASK_FILE = "tasks/339-direct-cutover-to-target-architecture-cn.md"


def junit(results):
    suite = ET.Element("testsuite")
    for name, state in sorted(results.items()):
        test = ET.SubElement(suite, "testcase", name=name, status=state)
        if state == "fail":
            ET.SubElement(test, "failure")
        elif state == "notrun":
            ET.SubElement(test, "skipped")
    return ET.tostring(suite, encoding="utf-8")


def registry(tests):
    return {"tests": [{"name": test} for test in sorted(tests)]}


def json_file(path, data):
    path.write_text(json.dumps(data), encoding="utf-8")
    return str(path)


class QueueTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.template_directory = tempfile.TemporaryDirectory(prefix="xray-queue-template-")
        cls.addClassCleanup(cls.template_directory.cleanup)
        cls.template = Path(cls.template_directory.name) / "repo"
        cls.template.mkdir()
        for arguments in [("init", "-b", "main"), ("config", "user.name", "Queue Fixture"),
                          ("config", "user.email", "fixture@example.com"),
                          ("config", "core.autocrlf", "false")]:
            subprocess.run(["git", "-C", str(cls.template), *arguments], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        smoke = {"cases": [{"id": "seed", "expect": "pass"}]}
        files = {
            "version.py": "fixture root",
            "scripts/verify_current_build.py": "REQUIRED = " + repr(tuple(sorted(FIXTURE_TESTS))) + "\n",
            "tests/smoke/cases.json": json.dumps(smoke),
            "tests/smoke/original-baseline.json": "{}",
        }
        for path, content in files.items():
            file = cls.template / path
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text(content, encoding="utf-8")
        for arguments in [("add", "."), ("commit", "-m", "Create queue fixture")]:
            subprocess.run(["git", "-C", str(cls.template), *arguments], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cls.template_base = subprocess.check_output(
            ["git", "-C", str(cls.template), "rev-parse", "HEAD"], text=True).strip()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="xray-queue-test-")
        self.root = Path(self.temporary.name)
        self.repo = self.root / "repo"
        # Independent copies keep work files, refs, configuration and objects
        # private while avoiding repeated creation of the same base commit.
        shutil.copytree(self.template, self.repo)
        self.children = []
        self.git("update-index", "--refresh")
        self.base = self.template_base
        self.integration = self.root / "int"
        self.git("worktree", "add", "--detach", str(self.integration), self.base)
        self.git("switch", "--detach")
        self.evidence = self.root / "review.txt"
        self.evidence.write_text("Synthetic review for isolated queue tests only.\n")
        self.l0 = self.root / "l0.log"
        self.l0.write_text("fixture L0 passed\n")
        self.docs = self.root / "docs"
        (self.docs / "tasks").mkdir(parents=True)
        (self.docs / TASK_FILE).write_text("fixture Task339", encoding="utf-8")

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
        self.write_review(review, sha)
        return name, review, sha

    def write_review(self, path, sha):
        evidence = self.root / (path.stem + "-evidence")
        evidence.mkdir(exist_ok=True)
        gates = {}
        release = {}
        run = {"id": 100, "head_sha": sha, "status": "completed", "conclusion": "success",
               "path": ".github/workflows/native-host-portability.yml"}
        jobs = [{"name": name, "run_id": run["id"], "head_sha": sha, "status": "completed", "conclusion": "success"}
                for name in [*merge_slice.HOST_JOBS.values(), merge_slice.SAN_JOB]]
        for host in merge_slice.HOST_JOBS:
            host_dir = evidence / host
            host_dir.mkdir(exist_ok=True)
            gates[host] = self.current_gate(host_dir, host, sha)
            if host not in COMPARISON_HOSTS:
                continue
            release[host] = {"hostIdentity": host + " fixture"}
            for side, commit in (("base", self.base), ("candidate", sha)):
                directory = host_dir / side
                directory.mkdir(exist_ok=True)
                log = directory / "ctest.stdout"
                log.write_text("fixture terminal CTest output", encoding="utf-8")
                report = directory / "full.xml"
                results = dict.fromkeys(FULL_TESTS, "run")
                results["historical_failure"] = "fail"
                report.write_bytes(junit(results))
                cache = directory / "CMakeCache.txt"
                cache.write_text("CMAKE_GENERATOR:INTERNAL=Ninja\nCMAKE_BUILD_TYPE:STRING=Release\n"
                                 "CMAKE_C_COMPILER:FILEPATH=/fixture/cc\n", encoding="utf-8")
                release[host][side] = {
                    "identity": json_file(directory / "identity.json", self.identity(host, commit)),
                    "registry": json_file(directory / "registry.json", registry(FULL_TESTS)),
                    "junit": str(report), "cache": str(cache), "log": str(log), "exit": 8,
                }
        review = {
            "approved": True, "sourceSHA": sha, "reviewedBase": self.base, "comparisonBase": self.base,
            "author": "Core Fixture", "reviewer": "Separate Reviewer",
            "reviewEvidence": str(self.evidence), "l0Log": str(self.l0),
            "ciRun": json_file(evidence / "run.json", run), "ciJobs": json_file(evidence / "jobs.json", {"jobs": jobs}),
            "current": gates, "release": release,
        }
        json_file(path, review)

    @staticmethod
    def identity(host, sha, profile="Release"):
        return {"commit": sha, "dirty": False, "target": merge_slice.HOST_TARGETS[host][0], "buildProfile": profile}

    def current_gate(self, directory, host, sha, profile="Release"):
        commands = []
        for name, payload in (
            ("identity", json.dumps(self.identity(host, sha, profile))),
            ("ctest-list", json.dumps(registry(FULL_TESTS))),
            ("current-tests", "fixture terminal CTest output"),
        ):
            (directory / f"{name}.stdout").write_text(payload, encoding="utf-8")
            (directory / f"{name}.stderr").write_bytes(b"")
            commands.append({"name": name, "command": [name], "exit": 0, "seconds": 0.1,
                             "stdoutSha256": hashlib.sha256(payload.encode()).hexdigest(),
                             "stderrSha256": hashlib.sha256(b"").hexdigest()})
        json_file(directory / "commands.json", commands)
        (directory / "current.xml").write_bytes(junit(dict.fromkeys(FIXTURE_TESTS, "run")))
        return str(directory)

    def start(self, branch):
        name, review, _ = branch
        args = [sys.executable, "-B", str(QUEUE), "--branch", name, "--review", str(review),
                "--repo", str(self.repo), "--integration", str(self.integration),
                "--docs", str(self.docs), "--queue", str(self.root / "queue")]
        child = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.children.append(child)
        return child

    def logs(self):
        return [json.loads(p.read_text(encoding="utf-8"))
                for p in sorted((self.root / "queue/logs").glob("*/result.json"))]

    def finish(self, child, code):
        self.assertEqual(child.wait(timeout=60), code, self.logs())
        return self.logs()[-1]

    def test_pass_fast_forwards_after_exact_evidence(self):
        branch = self.branch("a/pass", {"pass": "yes"})
        log = self.finish(self.start(branch), 0)
        self.assertEqual(log["state"], "MERGED")
        self.assertEqual(log["oldMain"], self.base)
        self.assertEqual(self.git("rev-parse", "main"), log["candidate"])
        self.assertEqual(log["gates"]["L1"], "PASS")
        self.assertEqual(len(self.logs()), 1)

    def test_repository_copy_keeps_template_files_and_refs_independent(self):
        original = (self.template / "version.py").read_bytes()
        self.write("version.py", "changed independently\n")
        self.git("config", "user.name", "Independent Fixture")
        self.git("update-ref", "refs/heads/private", self.base)
        self.assertEqual((self.template / "version.py").read_bytes(), original)
        self.assertEqual(self.git("config", "user.name", root=self.template), "Queue Fixture")
        self.assertEqual(self.git("for-each-ref", "refs/heads/private", root=self.template), "")
        self.assertNotEqual(self.git("rev-parse", "--path-format=absolute", "--git-common-dir"),
                            self.git("rev-parse", "--path-format=absolute", "--git-common-dir", root=self.template))

    def test_failure_keeps_main_and_allows_next_request(self):
        bad = self.branch("a/fail", {"fail": "yes"})
        evidence = json.loads(bad[1].read_text())
        jobs = Path(evidence["ciJobs"])
        rows = json.loads(jobs.read_text())
        rows["jobs"][0]["conclusion"] = "failure"
        json_file(jobs, rows)
        log = self.finish(self.start(bad), 1)
        self.assertIn("Missing or failed CI job", log["error"])
        self.assertEqual(self.git("rev-parse", "main"), self.base)
        good = self.branch("a/next", {"next": "yes"})
        self.finish(self.start(good), 0)

    def test_warm_checkout_materializes_new_lf_attributes(self):
        self.git("config", "core.autocrlf", "true")
        name, review, first = self.branch("a/eol", {"oracle.out": "42\n", "中文输出.out": "42\n", "untouched": "stable\n"})
        self.git("switch", "--detach", first, root=self.integration)
        oracle = self.integration / "oracle.out"
        unicode_oracle = self.integration / "中文输出.out"
        self.assertEqual(oracle.read_bytes(), b"42\r\n")
        self.assertEqual(unicode_oracle.read_bytes(), b"42\r\n")
        untouched = self.integration / "untouched"
        stamp = untouched.stat().st_mtime_ns
        self.git("switch", name)
        self.write(".gitattributes", "oracle.out text eol=lf\n中文输出.out text eol=lf\n")
        self.git("add", "--", ".gitattributes")
        self.git("commit", "-m", "Pin the output witness checkout bytes")
        final = self.git("rev-parse", "HEAD")
        self.git("switch", "--detach")
        self.write_review(review, final)
        log = self.finish(self.start((name, review, final)), 0)
        self.assertEqual(log["state"], "MERGED")
        self.assertEqual(oracle.read_bytes(), b"42\n")
        self.assertEqual(unicode_oracle.read_bytes(), b"42\n")
        self.assertEqual(untouched.stat().st_mtime_ns, stamp)
        self.assertEqual([row["path"] for row in log["checkoutMaterialization"]], ["oracle.out", "中文输出.out"])
        oracle_stamp = oracle.stat().st_mtime_ns
        unicode_stamp = unicode_oracle.stat().st_mtime_ns
        self.assertEqual(merge_slice.materialize_checkout(self.integration, log["changed"]), [])
        self.assertEqual(oracle.stat().st_mtime_ns, oracle_stamp)
        self.assertEqual(unicode_oracle.stat().st_mtime_ns, unicode_stamp)
        self.assertEqual(untouched.stat().st_mtime_ns, stamp)

    def test_checkout_materialization_rejects_dirty_files_before_writing(self):
        (self.repo / "first.out").write_bytes(b"42\n")
        self.write("second", "stable\n")
        self.write(".gitattributes", "first.out text eol=lf\n")
        self.git("add", "--", "first.out", "second", ".gitattributes")
        self.git("commit", "-m", "Create checkout inputs")
        first, second = self.repo / "first.out", self.repo / "second"
        first.write_bytes(b"42\r\n")
        second.write_bytes(b"dirty\n")
        before = [(path.read_bytes(), path.stat().st_mtime_ns) for path in (first, second)]
        with self.assertRaisesRegex(RuntimeError, "changed content"):
            merge_slice.materialize_checkout(self.repo, ["first.out", "second"])
        self.assertEqual([(path.read_bytes(), path.stat().st_mtime_ns) for path in (first, second)], before)

    def test_concurrent_requests_are_fifo_and_stale_evidence_is_rejected(self):
        first = self.branch("a/first", {"first": "yes"})
        second = self.branch("a/second", {"second": "yes"})
        child1 = self.start(first)
        deadline = time.monotonic() + 10
        while not list((self.root / "queue/requests").glob("*.json")):
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.01)
        child2 = self.start(second)
        self.finish(child1, 0)
        self.finish(child2, 1)
        one, two = self.logs()
        self.assertLess(one["sequence"], two["sequence"])
        self.assertGreaterEqual(two["processingStarted"], one["finished"])
        self.assertEqual(two["oldMain"], one["newMain"])
        self.assertIn("current main", two["error"])
        self.assertEqual(self.git("rev-parse", "main"), first[2])

    def test_old_r3_qualification_does_not_control_stdlib_admission(self):
        branch = self.branch("front/stdlib", {"stdlib/example/example.xr": "// synthetic fixture"})
        log = self.finish(self.start(branch), 0)
        self.assertEqual(log["gates"]["fullCTest"], "PASS")
        self.assertEqual(log["gates"]["affectedSan"], "NOT_APPLICABLE")

    def test_passing_smoke_cannot_be_demoted(self):
        path = "tests/smoke/cases.json"
        branch = self.branch("front/smoke", {path: json.dumps({"cases": [{"id": "seed", "expect": "gap"}]})})
        log = self.finish(self.start(branch), 1)
        self.assertIn("passing case demoted", log["error"])
        self.assertEqual(self.git("rev-parse", "main"), self.base)

    def test_new_full_ctest_failure_never_advances_main(self):
        branch = self.branch("front/compare", {"example": "yes"})
        review = json.loads(branch[1].read_text())
        report = Path(review["release"]["windows-2022"]["candidate"]["junit"])
        outcomes = dict.fromkeys(FULL_TESTS, "run")
        outcomes.update(actual="fail", historical_failure="fail")
        report.write_bytes(junit(outcomes))
        log = self.finish(self.start(branch), 1)
        self.assertIn("new failing or unexecuted tests: actual", log["error"])
        self.assertEqual(self.git("rev-parse", "main"), self.base)

    def test_missing_safety_inventory_never_advances_main(self):
        branch = self.branch("core/metadata", {"src/lir/xlir_test.c": "/* fixture */"})
        review = json.loads(branch[1].read_text())
        for side in ("base", "candidate"):
            evidence = review["release"]["windows-2022"][side]
            tests = FULL_TESTS - {"meta_ownership_inventory"}
            json_file(Path(evidence["registry"]), registry(tests))
            outcomes = dict.fromkeys(tests, "run")
            outcomes["historical_failure"] = "fail"
            Path(evidence["junit"]).write_bytes(junit(outcomes))
        log = self.finish(self.start(branch), 1)
        self.assertIn("misses an applicable safety or smoke test", log["error"])
        self.assertEqual(self.git("rev-parse", "main"), self.base)

    def test_placeholder_identity_is_rejected(self):
        branch = self.branch("a/invalid", {"invalid": "yes"}, invalid=True)
        log = self.finish(self.start(branch), 1)
        self.assertIn("Placeholder", log["error"])
        self.assertEqual(self.git("rev-parse", "main"), self.base)

    def test_changed_current_receipt_is_rejected(self):
        branch = self.branch("core/receipt", {"example": "yes"})
        review = json.loads(branch[1].read_text())
        receipt = Path(review["current"]["ubuntu-24.04"]) / "current-tests.stdout"
        receipt.write_text("replacement", encoding="utf-8")
        log = self.finish(self.start(branch), 1)
        self.assertIn("changed current-tests stdout", log["error"])
        self.assertEqual(self.git("rev-parse", "main"), self.base)

    def test_affected_fail_closed_without_san_and_accepts_current_coverage(self):
        branch = self.branch("core/asan", {"src/runtime/native/source.c": "/* fixture */"})
        review = json.loads(branch[1].read_text())
        log = self.finish(self.start(branch), 1)
        self.assertIn("affected sanitizer evidence", log["error"])
        san = branch[1].with_suffix(".san")
        san.mkdir()
        cache = san / "CMakeCache.txt"
        cache.write_text("ENABLE_ASAN:BOOL=ON\nENABLE_UBSAN:BOOL=ON\nXR_XIR_PARANOID:BOOL=ON\n", encoding="utf-8")
        review["san"] = {"cache": str(cache), "current": self.current_gate(san, "ubuntu-24.04", branch[2], "Debug"),
                         "tests": sorted(FIXTURE_TESTS)}
        json_file(branch[1], review)
        log = self.finish(self.start(branch), 0)
        self.assertEqual(log["gates"]["affectedSan"], "PASS")

    def test_shared_changes_require_distinct_review(self):
        branch = self.branch("core/shared", {"src/shared/file.c": "/* fixture */"})
        review = json.loads(branch[1].read_text())
        review["reviewer"] = review["author"].upper()
        json_file(branch[1], review)
        log = self.finish(self.start(branch), 1)
        self.assertIn("Independent review required", log["error"])

    def test_main_compare_and_swap_does_not_overwrite_a_race(self):
        alternate = self.branch("b/alternate", {"alternate": "yes"})
        branch = self.branch("a/race", {"race": "yes"})
        args = type("Args", (), {"repo": self.repo, "integration": self.integration, "branch": branch[0]})()
        original = merge_slice.verify_task339

        def race(*arguments):
            original(*arguments)
            self.git("update-ref", "refs/heads/main", alternate[2], self.base)

        with mock.patch.object(merge_slice, "verify_task339", side_effect=race):
            with self.assertRaisesRegex(RuntimeError, "update-ref"):
                merge_slice.integrate(args, json.loads(branch[1].read_text()), {"gates": {}}, self.root)
        self.assertEqual(self.git("rev-parse", "main"), alternate[2])

    def test_moved_branch_invalidates_review(self):
        branch = self.branch("a/reviewed", {"reviewed": "yes"})
        alternate = self.branch("b/moved", {"moved": "yes"})
        self.git("update-ref", "refs/heads/" + branch[0], alternate[2], branch[2])
        log = self.finish(self.start(branch), 1)
        self.assertIn("moved after", log["error"])
        self.assertEqual(self.git("rev-parse", "main"), self.base)

    def test_nonlinear_candidate_is_rejected(self):
        first = self.branch("a/first", {"first": "yes"})
        second = self.branch("a/second", {"second": "yes"})
        self.git("switch", second[0])
        self.git("merge", "--no-ff", "-m", "Merge fixtures", first[0])
        head = self.git("rev-parse", "HEAD")
        self.git("switch", "--detach")
        self.write_review(second[1], head)
        log = self.finish(self.start((second[0], second[1], head)), 1)
        self.assertIn("nonempty, linear slice", log["error"])
        self.assertEqual(self.git("rev-parse", "main"), self.base)

    def test_recovery_exception_blocks_the_queue_itself(self):
        branch = self.branch("a/recovery", {"recovery": "yes"})
        arguments = [str(QUEUE), "--branch", branch[0], "--review", str(branch[1]),
                     "--repo", str(self.repo), "--docs", str(self.docs), "--queue", str(self.root / "queue")]
        with mock.patch.object(sys, "argv", arguments), mock.patch.object(
                merge_slice, "integrate", side_effect=RecoveryRequired("process tree unknown")):
            with self.assertRaises(RecoveryRequired):
                merge_slice.main()
        self.assertEqual(self.logs()[0]["state"], "NEEDS_RECOVERY")
        request = next((self.root / "queue/requests").glob("*.json"))
        self.assertEqual(json.loads(request.read_text())["state"], "NEEDS_RECOVERY")
        self.assertEqual(self.git("rev-parse", "main"), self.base)


class GroupedReportTests(unittest.TestCase):
    def run_report(self, cases, *, failed_command=False):
        with tempfile.TemporaryDirectory(prefix="xray-groups-") as directory:
            logs = Path(directory)
            calls = []

            def produce(arguments, directory, environment, log):
                calls.append(arguments)
                report = Path(arguments[arguments.index("--output-junit") + 1])
                root = ET.Element("testsuite")
                for name, status, problem in cases:
                    case = ET.SubElement(root, "testcase", name=name, status=status, time="0.01")
                    if problem:
                        ET.SubElement(case, problem)
                ET.ElementTree(root).write(report)
                log.write_text("fixture command\n", encoding="utf-8")
                if failed_command:
                    raise subprocess.CalledProcessError(8, arguments)

            with mock.patch.object(xray_l1, "command", side_effect=produce):
                result = xray_l1.test_groups(
                    {"l1": ["actual"], "metadata": ["meta_ownership_inventory"]},
                    logs, logs, {})
            self.assertEqual(len(calls), 1)
            return result

    def test_exact_groups(self):
        self.assertEqual(self.run_report([("actual", "run", None),
                                          ("meta_ownership_inventory", "run", None)]),
                         {"l1": True, "metadata": True})

    def test_missing_duplicate_or_unexpected_rows_reject(self):
        valid = [("actual", "run", None), ("meta_ownership_inventory", "run", None)]
        for cases in [valid[:1], valid[1:], valid + valid[:1], valid + valid[1:],
                      valid + [("extra", "run", None)]]:
            with self.subTest(cases=cases), self.assertRaisesRegex(RuntimeError, "requested tests differ"):
                self.run_report(cases)

    def test_failed_skipped_or_unexecuted_rows_reject_only_their_group(self):
        for name, group in [("actual", "l1"), ("meta_ownership_inventory", "metadata")]:
            for status, problem in [("run", "failure"), ("run", "error"),
                                    ("run", "skipped"), ("notrun", None)]:
                cases = [(n, status if n == name else "run", problem if n == name else None)
                         for n in ("actual", "meta_ownership_inventory")]
                with self.subTest(group=group, status=status, problem=problem):
                    result = self.run_report(cases, failed_command=problem == "failure")
                    self.assertFalse(result[group])
                    self.assertTrue(result["metadata" if group == "l1" else "l1"])

    def test_command_failure_cannot_be_hidden_by_passing_xml(self):
        with self.assertRaisesRegex(RuntimeError, "CTest failed"):
            self.run_report([("actual", "run", None), ("meta_ownership_inventory", "run", None)],
                            failed_command=True)

    def test_duplicate_requested_group_rejects_before_execution(self):
        with mock.patch.object(xray_l1, "command") as command:
            with self.assertRaises(ValueError):
                xray_l1.test_groups({"l1": ["actual", "actual"]}, Path(), Path(), {})
            command.assert_not_called()


if __name__ == "__main__":
    unittest.main()

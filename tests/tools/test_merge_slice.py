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
from zipfile import ZipFile

QUEUE = Path(__file__).resolve().parents[2] / "scripts/merge_slice.py"
sys.path.insert(0, str(QUEUE.parent))
import merge_slice
import xray_l1
from xray_process import RecoveryRequired
FIXTURE_TESTS = {"actual"} | merge_slice.CURRENT_CORE
FULL_TESTS = FIXTURE_TESTS | {"meta_ownership_inventory", "historical_failure"}
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
            "scripts/check_meta_ownership.py": "print('ownership inventory passed')\n",
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
            ["git", "-C", str(cls.template), "rev-parse", "HEAD"], text=True, encoding="utf-8", errors="strict").strip()

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
        self.evidence.write_text("Synthetic review for isolated queue tests only.\n", encoding="utf-8")
        self.l0 = self.root / "l0.log"
        self.l0.write_text("fixture L0 passed\n", encoding="utf-8")
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
                                       stderr=subprocess.DEVNULL, text=True, encoding="utf-8", errors="strict").strip()

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
        api = {}
        run = {"id": 100, "run_attempt": 1, "head_sha": sha, "status": "completed", "conclusion": "success",
               "repository": {"id": 20, "full_name": merge_slice.CI_REPO},
               "head_repository": {"full_name": merge_slice.CI_REPO},
               "path": ".github/workflows/native-host-portability.yml"}
        endpoint = "/runs/100/attempts/1"
        api[endpoint] = run
        jobs, gates, artifacts = [], {}, []
        hosts = [*merge_slice.HOST_JOBS.items(), ("san", merge_slice.SAN_JOB)]
        for artifact_id, (host, name) in enumerate(hosts, 50):
            profile = "Debug" if host == "san" else "Release"
            directory = evidence / host
            directory.mkdir(exist_ok=True)
            artifact_name = "current-sanitizers" if host == "san" else "current-" + host
            package = directory / "current.zip"
            base_host = "ubuntu-24.04" if host == "san" else host
            self.current_gate(package, base_host, sha, profile)
            api[f"/artifacts/{artifact_id}"] = {
                "id": artifact_id, "name": artifact_name + "-" + sha, "expired": False,
                "created_at": "2026-11-20T01:00:01Z", "size_in_bytes": package.stat().st_size,
                "digest": "sha256:" + self.digest(package.read_bytes()),
                "workflow_run": {"id": 100, "head_sha": sha, "repository_id": 20, "head_repository_id": 20},
            }
            artifacts.append(api[f"/artifacts/{artifact_id}"])
            kind = "Windows" if host == "windows-2022" else "Unix"
            steps = ([f"Configure {kind} Ninja Release", f"Build {kind} all without exclusions",
                      f"Independently rebuild {kind} CLI", f"Verify {kind} current gates"] if host != "san" else
                     ["Configure combined sanitizers", "Build current products and kernels", "Verify exact CLI and current gates"])
            jobs.append({"name": name, "run_id": 100, "run_attempt": 1, "head_sha": sha,
                         "run_url": merge_slice.CI_API + "/runs/100", "status": "completed", "conclusion": "success",
                         "steps": [{"name": step, "status": "completed", "conclusion": "success",
                                    "started_at": "2026-11-20T01:00:00Z", "completed_at": "2026-11-20T01:00:02Z"}
                                   for step in ["Run actions/checkout@v4", *steps, merge_slice.UPLOAD_STEP]]})
            gates[host] = {"id": artifact_id, "zip": str(package)}
        api[endpoint + "/jobs?per_page=100"] = {"jobs": jobs}
        api["/runs/100/artifacts?per_page=100&page=1"] = {"total_count": len(artifacts), "artifacts": artifacts}
        json_file(path.with_suffix(".api"), api)
        json_file(path, {
            "approved": True, "sourceSHA": sha, "reviewedBase": self.base,
            "author": "Core Fixture", "reviewer": "Separate Reviewer",
            "reviewEvidence": str(self.evidence), "l0Log": str(self.l0),
            "ci": {"runId": 100, "runAttempt": 1, "artifacts": gates},
        })

    @staticmethod
    def digest(content):
        return hashlib.sha256(content).hexdigest()

    @staticmethod
    def identity(host, sha, profile="Release"):
        return {"commit": sha, "dirty": False, "target": merge_slice.HOST_TARGETS[host][0], "buildProfile": profile}

    @staticmethod
    def source_path(host, side):
        return "D:/fixture/" + side if host == "windows-2022" else "/fixture/" + side

    @staticmethod
    def cache(source, profile):
        common = (f"CMAKE_HOME_DIRECTORY:INTERNAL={source}\nCMAKE_CACHEFILE_DIR:INTERNAL={source}/build\n"
                  f"CMAKE_GENERATOR:INTERNAL=Ninja\nCMAKE_BUILD_TYPE:STRING={profile}\n"
                  "CMAKE_C_COMPILER:FILEPATH=/fixture/cc\n")
        return common + "".join(f"{name}:BOOL=ON\n" for name in ("ENABLE_ASAN", "ENABLE_UBSAN", "XR_XIR_PARANOID")
                                if profile == "Debug")

    def current_gate(self, path, host, sha, profile):
        source = self.source_path(host, "hosted")
        build = source + "/build"
        exe = "xray.exe" if host == "windows-2022" else "xray"
        folder = "sanitizer-evidence" if profile == "Debug" else "current-evidence"
        commands = []
        with ZipFile(path, "w") as zipfile:
            zipfile.writestr("build/CMakeCache.txt", self.cache(source, profile))
            for name, argv, payload in (
                ("identity", [build + "/" + exe, "--version", "--json"], json.dumps(self.identity(host, sha, profile))),
                ("ctest-list", ["ctest", "--test-dir", build, "--show-only=json-v1"], json.dumps(registry(FULL_TESTS))),
                ("current-tests", ["ctest", "--test-dir", build, "--output-on-failure", "--no-tests=error",
                                   "-j", "2", "-R", "^(" + "|".join(sorted(FIXTURE_TESTS)) + ")$",
                                   "--output-junit", f"{source}/{folder}/current.xml"], "fixture terminal CTest output"),
            ):
                zipfile.writestr(f"{folder}/{name}.stdout", payload)
                zipfile.writestr(f"{folder}/{name}.stderr", "")
                commands.append({"name": name, "command": argv, "exit": 0, "seconds": 0.1,
                                 "stdoutSha256": self.digest(payload.encode()), "stderrSha256": self.digest(b"")})
            zipfile.writestr(f"{folder}/commands.json", json.dumps(commands))
            zipfile.writestr(f"{folder}/current.xml", junit(dict.fromkeys(FIXTURE_TESTS, "run")))

    def release_evidence(self, directory, side, host, host_identity, sha):
        source = self.source_path(host, side)
        build = source + "/build"
        exe = "xray.exe" if host == "windows-2022" else "xray"
        report = source + "/evidence/full.xml"
        results = dict.fromkeys(FULL_TESTS, "run")
        results["historical_failure"] = "fail"
        payloads = {"identity": json.dumps(self.identity(host, sha)).encode(),
                    "registry": json.dumps(registry(FULL_TESTS)).encode(), "junit": junit(results),
                    "cache": self.cache(source, "Release").encode(), "log": b"fixture CTest output", "stderr": b""}
        paths = {}
        for name, payload in payloads.items():
            file = directory / name
            file.write_bytes(payload)
            paths[name] = str(file)
        commands = []
        for name, argv, exit_code, stdout in (
            ("configure", ["cmake", "--preset", "default", "-B", build], 0, b""),
            ("all", ["cmake", "--build", build, "--parallel", "4", "--", "-k0"], 0, b""),
            ("cli-post-all", ["cmake", "--build", build, "--parallel", "4", "--target", "xray"], 0, b""),
            ("identity", [build + "/" + exe, "--version", "--json"], 0, payloads["identity"]),
            ("registry", ["ctest", "--test-dir", build, "--show-only=json-v1"], 0, payloads["registry"]),
            ("ctest", ["ctest", "--test-dir", build, "--output-on-failure", "--no-tests=error",
                       "-j2", "--output-junit", report], 8, payloads["log"]),
        ):
            commands.append({"name": "candidate-" + name, "argv": argv, "exit": exit_code, "childrenDrained": True,
                             "stdoutSHA256": self.digest(stdout), "stderrSHA256": self.digest(b"")})
        receipt = {
            "head": sha, "source": source, "build": build, "state": "NEEDS_REVIEW",
            "hostIdentity": host_identity, "resourcesReleased": True, "commands": commands,
            "identity": {"head": sha, "status": "", "diffSha256": self.digest(b"")},
            "finalIdentity": {"head": sha, "status": "", "diffSha256": self.digest(b"")},
            "trees": {"release": {"build": build, "cache": build + "/CMakeCache.txt", "cli": build + "/" + exe,
                                  "toolchainIdentity": "fixed fixture compiler and SDK",
                                  "cacheSHA256": self.digest(payloads["cache"]), "cliSHA256": "f" * 64}},
            "release": {"output": report, "outputSHA256": self.digest(payloads["junit"]),
                        "registrySHA256": self.digest(payloads["registry"]), "rawExit": 8},
        }
        paths["receipt"] = json_file(directory / "receipt.json", receipt)
        return paths

    def rewrite_outcomes(self, paths, outcomes, change_inventory=False):
        receipt_path = Path(paths["receipt"])
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        payload = junit(outcomes)
        Path(paths["junit"]).write_bytes(payload)
        receipt["release"]["outputSHA256"] = self.digest(payload)
        if change_inventory:
            payload = json.dumps(registry(outcomes)).encode()
            Path(paths["registry"]).write_bytes(payload)
            receipt["commands"][4]["stdoutSHA256"] = receipt["release"]["registrySHA256"] = self.digest(payload)
        json_file(receipt_path, receipt)

    def start(self, branch):
        name, review, _ = branch
        # A separate Python process uses the real queue; only public CI access
        # is injected from recorded-shaped fixtures, never from production flags.
        program = (
            "import json,sys; from pathlib import Path; "
            "sys.path.insert(0, str(Path(sys.argv.pop(1)).parent)); import merge_slice; "
            "api = json.loads(Path(sys.argv.pop(1)).read_text(encoding='utf-8')); "
            "merge_slice.official_json = lambda endpoint, *_: api[endpoint]; "
            "raise SystemExit(merge_slice.main())"
        )
        args = [sys.executable, "-B", "-c", program, str(QUEUE), str(review.with_suffix(".api")),
                "--branch", name, "--review", str(review), "--repo", str(self.repo),
                "--integration", str(self.integration), "--docs", str(self.docs), "--queue", str(self.root / "queue")]
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
        metadata = bad[1].with_suffix(".api")
        rows = json.loads(metadata.read_text(encoding="utf-8"))
        rows["/runs/100/attempts/1/jobs?per_page=100"]["jobs"][0]["conclusion"] = "failure"
        json_file(metadata, rows)
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
        self.assertEqual(log["gates"]["combinedSan"], "PASS")
        self.assertEqual(log["gates"]["affectedSan"], "NOT_APPLICABLE")

    def test_passing_smoke_cannot_be_demoted(self):
        path = "tests/smoke/cases.json"
        branch = self.branch("front/smoke", {path: json.dumps({"cases": [{"id": "seed", "expect": "gap"}]})})
        log = self.finish(self.start(branch), 1)
        self.assertIn("passing case demoted", log["error"])
        self.assertEqual(self.git("rev-parse", "main"), self.base)

    def test_missing_combined_sanitizer_job_blocks_an_ordinary_candidate(self):
        branch = self.branch("front/no-san", {"example": "yes"})
        metadata = branch[1].with_suffix(".api")
        api = json.loads(metadata.read_text(encoding="utf-8"))
        jobs = api["/runs/100/attempts/1/jobs?per_page=100"]["jobs"]
        jobs[:] = [job for job in jobs if job["name"] != merge_slice.SAN_JOB]
        json_file(metadata, api)
        log = self.finish(self.start(branch), 1)
        self.assertIn(merge_slice.SAN_JOB, log["error"])
        self.assertEqual(log["gates"]["combinedSan"], "NOT_RUN")
        self.assertEqual(self.git("rev-parse", "main"), self.base)

    def test_metadata_inventory_failure_never_advances_main(self):
        branch = self.branch("core/metadata", {"src/lir/xlir_test.c": "/* fixture */",
                             "scripts/check_meta_ownership.py": "raise SystemExit(1)\n"})
        log = self.finish(self.start(branch), 1)
        self.assertIn("meta_ownership_inventory exited", log["error"])
        self.assertEqual(log["gates"]["meta_ownership_inventory"], "FAIL")
        self.assertEqual(self.git("rev-parse", "main"), self.base)

    def test_metadata_inventory_passes_without_full_ctest(self):
        branch = self.branch("core/metadata", {"src/lir/xlir_test.c": "/* fixture */"})
        log = self.finish(self.start(branch), 0)
        self.assertEqual(log["gates"]["meta_ownership_inventory"], "PASS")
        self.assertIn("meta_ownership_inventory.stdout", log["evidence"])
        self.assertNotIn("fullCTest", log["gates"])
        self.assertEqual(self.git("rev-parse", "main"), branch[2])

    def test_metadata_inventory_change_requires_independent_review_and_scan(self):
        path = "scripts/check_meta_ownership.py"
        branch = self.branch("core/metadata-policy", {path: "raise SystemExit(1)\n"})
        review = json.loads(branch[1].read_text(encoding="utf-8"))
        reviewer = review["reviewer"]
        review["reviewer"] = review["author"]
        json_file(branch[1], review)
        log = self.finish(self.start(branch), 1)
        self.assertIn("Independent review required: " + path, log["error"])
        review["reviewer"] = reviewer
        json_file(branch[1], review)
        log = self.finish(self.start(branch), 1)
        self.assertEqual(log["gates"]["combinedSan"], "PASS")
        self.assertEqual(log["gates"]["meta_ownership_inventory"], "FAIL")
        self.assertEqual(self.git("rev-parse", "main"), self.base)

    def test_daily_comparison_still_reports_a_new_full_ctest_failure(self):
        branch = self.branch("front/compare", {"example": "yes"})
        host = "windows-2022"
        identity = "Windows fixture"
        pair = {"hostIdentity": identity}
        for side, sha in (("base", self.base), ("candidate", branch[2])):
            directory = self.root / side
            directory.mkdir()
            pair[side] = self.release_evidence(directory, side, host, identity, sha)
        outcomes = dict.fromkeys(FULL_TESTS, "run")
        outcomes.update(actual="fail", historical_failure="fail")
        self.rewrite_outcomes(pair["candidate"], outcomes)
        with self.assertRaisesRegex(ValueError, "new failing or unexecuted tests: actual"):
            merge_slice.verify_release_pair(pair, host, branch[2], self.base, FIXTURE_TESTS, self.root, {})

    def test_placeholder_identity_is_rejected(self):
        branch = self.branch("a/invalid", {"invalid": "yes"}, invalid=True)
        log = self.finish(self.start(branch), 1)
        self.assertIn("Placeholder", log["error"])
        self.assertEqual(self.git("rev-parse", "main"), self.base)

    def test_changed_ci_archive_is_rejected(self):
        branch = self.branch("core/receipt", {"example": "yes"})
        review = json.loads(branch[1].read_text(encoding="utf-8"))
        artifact = Path(review["ci"]["artifacts"]["ubuntu-24.04"]["zip"])
        with artifact.open("ab") as output:
            output.write(b"replacement bytes")
        log = self.finish(self.start(branch), 1)
        self.assertIn("official artifact digest", log["error"])
        self.assertEqual(self.git("rev-parse", "main"), self.base)

    def test_artifact_from_another_attempt_is_rejected(self):
        branch = self.branch("core/other-attempt", {"example": "yes"})
        metadata = branch[1].with_suffix(".api")
        api = json.loads(metadata.read_text(encoding="utf-8"))
        api["/artifacts/50"]["created_at"] = "2026-11-20T00:59:59Z"
        json_file(metadata, api)
        log = self.finish(self.start(branch), 1)
        self.assertIn("this run and upload step", log["error"])

    def test_upload_window_accepts_only_a_unique_artifact_in_its_attempt(self):
        branch = self.branch("core/upload-window", {"example": "yes"})
        review = json.loads(branch[1].read_bytes())
        requested = review["ci"]["artifacts"]["macos-latest"]
        detail = f"/artifacts/{requested['id']}"
        endpoint = "/runs/100/artifacts?per_page=100&page=1"
        fixture = branch[1].with_suffix(".api").read_bytes()
        for case, created, sibling_time, sibling_expired, listed_id, error in (
            ("exact-start", "01:00:00", None, False, requested["id"], None),
            ("plus-five", "01:00:07", None, False, requested["id"], None),
            ("plus-six", "01:00:08", None, False, requested["id"], "this run and upload step"),
            ("before-start", "00:59:59", None, False, requested["id"], "this run and upload step"),
            ("previous-attempt", "01:00:01", "00:59:55", False, requested["id"], None),
            ("different-listed-id", "01:00:01", None, False, 200, "exactly the requested artifact"),
            ("live-duplicate", "01:00:01", "01:00:07", False, requested["id"], "exactly the requested artifact"),
            ("expired-duplicate", "01:00:01", "01:00:07", True, requested["id"], "exactly the requested artifact"),
            ("duplicate-on-next-page", "01:00:01", "01:00:07", False, requested["id"], "exactly the requested artifact"),
            ("inconsistent-listed-time", "01:00:01", None, False, requested["id"], "exactly the requested artifact"),
        ):
            with self.subTest(case=case):
                api = json.loads(fixture)
                meta = api[detail]
                meta["created_at"] = f"2026-11-20T{created}Z"
                rows = api[endpoint]["artifacts"]
                row = next(item for item in rows if item["id"] == requested["id"])
                row.update(id=listed_id, created_at=meta["created_at"])
                if case == "inconsistent-listed-time":
                    row["created_at"] = "2026-11-20T01:00:02Z"
                if sibling_time is not None:
                    rows.append({**row, "id": 200, "created_at": f"2026-11-20T{sibling_time}Z", "expired": sibling_expired})
                total_count = len(rows)
                if case == "duplicate-on-next-page":
                    duplicate = rows.pop()
                    size = merge_slice.ARTIFACT_PAGE_SIZE
                    rows.extend({**row, "id": 300 + i, "name": f"unrelated-{i}"} for i in range(size - len(rows)))
                    total_count = size + 1
                    api["/runs/100/artifacts?per_page=100&page=2"] = {"total_count": total_count, "artifacts": [duplicate]}
                api[endpoint]["total_count"] = total_count
                run = api["/runs/100/attempts/1"]
                job = next(job for job in api["/runs/100/attempts/1/jobs?per_page=100"]["jobs"]
                           if job["name"] == merge_slice.HOST_JOBS["macos-latest"])
                logs = self.root / case
                logs.mkdir()
                with mock.patch.object(merge_slice, "official_json", side_effect=lambda endpoint, *_: api[endpoint]):
                    catalog = merge_slice.official_run_artifacts(run["id"], logs, {})
                    args = (run, job, requested, "current-macos-latest", [], catalog, logs, {})
                    if error is None:
                        with merge_slice.verified_ci_package(*args) as package:
                            self.assertIn("build/CMakeCache.txt", package.namelist())
                    else:
                        with self.assertRaisesRegex(ValueError, error):
                            merge_slice.verified_ci_package(*args)

    def test_official_artifact_listing_requires_all_pages(self):
        size = merge_slice.ARTIFACT_PAGE_SIZE
        first = [{"id": i + 1, "name": f"artifact-{i + 1}", "created_at": "2026-11-20T01:00:01Z"}
                 for i in range(size)]
        last = {**first[0], "id": size + 1}
        for case, count, rows, error in (
            ("complete", size + 1, [last], None),
            ("short-page", size + 1, [], "Incomplete or inconsistent"),
            ("changed-total", size, [], "Incomplete or inconsistent"),
            ("overlapping-page", size + 1, [first[0]], "Invalid or duplicate"),
        ):
            with self.subTest(case=case):
                pages = [{"total_count": size + 1, "artifacts": first}, {"total_count": count, "artifacts": rows}]
                with mock.patch.object(merge_slice, "official_json", side_effect=pages) as fetch:
                    if error is None:
                        catalog = merge_slice.official_run_artifacts(100, self.root, {})
                        self.assertEqual(set(catalog), set(range(1, size + 2)))
                    else:
                        with self.assertRaisesRegex(ValueError, error):
                            merge_slice.official_run_artifacts(100, self.root, {})
                self.assertEqual([call.args[0] for call in fetch.call_args_list],
                                 [f"/runs/100/artifacts?per_page={size}&page={page}" for page in (1, 2)])

    def test_fabricated_current_argv_is_rejected(self):
        branch = self.branch("core/argv", {"example": "yes"})
        review = json.loads(branch[1].read_text(encoding="utf-8"))
        artifact = review["ci"]["artifacts"]["ubuntu-24.04"]
        path = Path(artifact["zip"])
        with ZipFile(path) as package:
            files = {name: package.read(name) for name in package.namelist()}
        cmd = "current-evidence/commands.json"
        commands = json.loads(files[cmd])
        commands[-1]["command"] = ["current-tests"]
        files[cmd] = json.dumps(commands).encode()
        with ZipFile(path, "w") as package:
            for name, value in files.items():
                package.writestr(name, value)
        metadata = branch[1].with_suffix(".api")
        api = json.loads(metadata.read_text(encoding="utf-8"))
        api[f"/artifacts/{artifact['id']}"].update(size_in_bytes=path.stat().st_size,
                                                   digest="sha256:" + self.digest(path.read_bytes()))
        json_file(metadata, api)
        log = self.finish(self.start(branch), 1)
        self.assertIn("unexpected command or build", log["error"])

    def test_full_receipt_rejects_spliced_commands_and_unrelated_junit(self):
        branch = self.branch("core/full-receipt", {"example": "yes"})
        host = "windows-2022"
        identity = "Windows fixture"
        paths = self.release_evidence(self.root, "candidate", host, identity, branch[2])
        receipt_path = Path(paths["receipt"])
        raw = receipt_path.read_bytes()
        receipt = json.loads(raw)
        receipt["commands"][-1]["argv"][-1] = "D:/different/full.xml"
        json_file(receipt_path, receipt)
        with self.assertRaisesRegex(ValueError, "different JUnit"):
            merge_slice.release_receipt(paths, host, branch[2], identity, self.root, {}, "candidate")
        receipt = json.loads(raw)
        receipt["finalIdentity"]["status"] = " M unrelated"
        json_file(receipt_path, receipt)
        with self.assertRaisesRegex(ValueError, "changed clean source identity"):
            merge_slice.release_receipt(paths, host, branch[2], identity, self.root, {}, "candidate")
        receipt_path.write_bytes(raw)
        Path(paths["junit"]).write_bytes(junit(dict.fromkeys(FULL_TESTS, "run")))
        with self.assertRaisesRegex(ValueError, "unrelated junit bytes"):
            merge_slice.release_receipt(paths, host, branch[2], identity, self.root, {}, "candidate")

    def test_affected_fail_closed_without_exact_san_coverage(self):
        path = "src/base/xsigned_rc.h"
        branch = self.branch("core/asan", {path: "/* fixture */"})
        review = json.loads(branch[1].read_text(encoding="utf-8"))
        log = self.finish(self.start(branch), 1)
        self.assertIn("affected sanitizer evidence", log["error"])
        scope = branch[1].with_suffix(".san")
        proof = {"approved": True, "reviewer": review["reviewer"], "candidate": branch[2], "base": self.base,
                 "paths": [path], "tests": ["smoke"], "coverage": "Fixture declares affected coverage"}
        review["san"] = json_file(scope, proof)
        json_file(branch[1], review)
        log = self.finish(self.start(branch), 1)
        self.assertIn("exact scope and combined suite", log["error"])
        proof["tests"] = sorted(FIXTURE_TESTS)
        json_file(scope, proof)
        log = self.finish(self.start(branch), 0)
        self.assertEqual(log["gates"]["affectedSan"], "PASS")

    def test_shared_changes_require_distinct_review(self):
        branch = self.branch("core/shared", {"src/shared/file.c": "/* fixture */"})
        review = json.loads(branch[1].read_text(encoding="utf-8"))
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

        api = json.loads(branch[1].with_suffix(".api").read_text(encoding="utf-8"))
        with (mock.patch.object(merge_slice, "verify_task339", side_effect=race),
              mock.patch.object(merge_slice, "official_json", side_effect=lambda endpoint, *_: api[endpoint])):
            with self.assertRaisesRegex(RuntimeError, "update-ref"):
                merge_slice.integrate(args, json.loads(branch[1].read_text(encoding="utf-8")), {"gates": {}}, self.root)
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
        self.assertEqual(json.loads(request.read_text(encoding="utf-8"))["state"], "NEEDS_RECOVERY")
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

"""Temporary-directory safety tests; never clean a real build tree."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "clean_build_artifacts.py"
spec = importlib.util.spec_from_file_location("clean_build_artifacts", SCRIPT)
cleanup = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = cleanup
spec.loader.exec_module(cleanup)


class CleanupTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.home = Path(self.temporary.name)
        self.root = self.home / "attempts"
        self.requests = self.home / "requests"
        self.root.mkdir()
        self.requests.mkdir()
        self.policy_path = self.home / "policy.json"
        self.data = {"version": 1, "roots": [{"path": str(self.root), "attempt_glob": "attempt-*",
            "receipt": {"path": "result.json", "key": "state", "accepted": ["PASS"]},
            "native_artifacts": ["products/native"]}], "request_dirs": [str(self.requests)],
            "process_paths": [str(self.root)], "min_age_hours": 48}

    def policy(self):
        self.policy_path.write_text(json.dumps(self.data), encoding="utf-8")
        return cleanup.load_policy(self.policy_path)

    def attempt(self, name, state="PASS", days=4):
        path = self.root / ("attempt-" + str(name))
        (path / "products").mkdir(parents=True)
        (path / "input").mkdir()
        (path / "source").mkdir()
        for item in ("products/test.exe", "products/test.pdb", "products/test.obj", "products/test.o",
                     "products/native", "products/unknown", "input/compiler.exe", "source/example.obj",
                     "main.c", "program.xr", "command.txt", "run.stdout", "run.stderr", "exit.json"):
            (path / item).write_bytes(b"preserved evidence or reproducible product")
        (path / "result.json").write_text(json.dumps({"state": state}), encoding="utf-8")
        stamp = time.time_ns() - days * 86400 * 1_000_000_000
        for item in sorted(path.rglob("*"), key=lambda item: len(item.parts), reverse=True):
            os.utime(item, ns=(stamp, stamp))
        os.utime(path, ns=(stamp, stamp))
        return path

    def run_cleanup(self, apply=False, processes=None):
        report = {"status": "APPLIED" if apply else "DRY_RUN", "attempts": [],
                  "candidateFiles": 0, "candidateLogicalBytes": 0,
                  "deletedFiles": 0, "deletedLogicalBytes": 0}
        events = []
        def emit(event):
            events.append(event)
            report["deletedFiles"] += 1
            report["deletedLogicalBytes"] += event["logicalBytes"]
        cleanup.cleanup(self.policy(), apply, report, emit, [] if processes is None else processes)
        return report, events

    def three(self):
        return [self.attempt(index, days=6-index) for index in range(3)]

    def test_default_dry_run_and_latest_two_successes(self):
        paths = self.three()
        report, events = self.run_cleanup()
        self.assertEqual(report["candidateFiles"], 5)
        self.assertEqual(report["deletedFiles"], 0)
        self.assertFalse(events)
        self.assertTrue(all((path / "products/test.exe").exists() for path in paths))

    def test_apply_preserves_evidence_latest_and_is_idempotent(self):
        paths = self.three()
        report, events = self.run_cleanup(True)
        self.assertEqual(report["deletedFiles"], 5)
        self.assertEqual(report["deletedLogicalBytes"], sum(item["logicalBytes"] for item in events))
        self.assertFalse((paths[0] / "products/test.exe").exists())
        self.assertTrue(all((path / "products/test.exe").exists() for path in paths[1:]))
        for name in ("products/unknown", "input/compiler.exe", "source/example.obj", "main.c", "program.xr",
                     "command.txt", "run.stdout", "run.stderr", "exit.json", "result.json"):
            self.assertTrue((paths[0] / name).exists(), name)
        self.data["min_age_hours"] = 0
        again, _ = self.run_cleanup(True)
        self.assertEqual(again["deletedFiles"], 0)

    def test_failed_unknown_and_nonobject_receipts_stay(self):
        self.three()
        failed = self.attempt("failed", "FAIL", days=9)
        unknown = self.attempt("unknown", "RUNNING", days=9)
        missing = self.attempt("missing", days=9)
        (missing / "result.json").write_text("[]", encoding="utf-8")
        report, _ = self.run_cleanup(True)
        self.assertEqual(report["deletedFiles"], 5)
        for path in (failed, unknown, missing):
            self.assertTrue((path / "products/test.exe").exists())

    def test_recent_evidence_protects_whole_attempt(self):
        paths = self.three()
        os.utime(paths[0] / "run.stderr", None)
        report, _ = self.run_cleanup(True)
        self.assertEqual(report["deletedFiles"], 0)
        self.assertTrue((paths[0] / "products/test.exe").exists())

    def test_old_file_written_after_completion_stays_protected(self):
        paths = self.three()
        marker = (paths[0] / "result.json").stat()
        later = marker.st_mtime_ns + 3600 * 1_000_000_000
        os.utime(paths[0] / "run.stdout", ns=(later, later))
        report, _ = self.run_cleanup(True)
        self.assertEqual(report["deletedFiles"], 0)
        self.assertIn("newer than completion marker", report["attempts"][0]["reason"])
        self.assertTrue((paths[0] / "products/test.exe").exists())

    def test_warm_cmake_cache_is_never_cleaned(self):
        paths = self.three()
        (paths[0] / "CMakeCache.txt").write_text("warm cache", encoding="utf-8")
        report, _ = self.run_cleanup(True)
        self.assertEqual(report["deletedFiles"], 0)

    def test_metadata_change_protects_all_files(self):
        path = self.attempt(0)
        policy = self.policy()
        attempt = cleanup.collect_attempt(self.root, path, policy["roots"][0])
        (path / "run.stdout").write_bytes(b"still writing")
        with self.assertRaisesRegex(cleanup.UnsafeAttempt, "metadata changed"):
            cleanup.delete_attempt(attempt, lambda event: None)
        self.assertTrue((path / "products/test.exe").exists())

    def test_receipt_change_even_with_restored_size_and_mtime(self):
        path = self.attempt(0)
        attempt = cleanup.collect_attempt(self.root, path, self.policy()["roots"][0])
        marker = path / "result.json"
        info = marker.stat()
        marker.write_text(json.dumps({"state": "FAIL"}), encoding="utf-8")
        os.utime(marker, ns=(info.st_atime_ns, info.st_mtime_ns))
        with self.assertRaisesRegex(cleanup.UnsafeAttempt, "receipt changed"):
            cleanup.delete_attempt(attempt, lambda event: None)
        self.assertTrue((path / "products/test.exe").exists())

    def test_each_file_is_rechecked_and_partial_failure_is_visible(self):
        path = self.attempt(0)
        attempt = cleanup.collect_attempt(self.root, path, self.policy()["roots"][0])
        first, second = attempt.candidates[:2]
        events = []
        def mutate(event):
            events.append(event)
            second.write_bytes(b"changed during deletion")
        with self.assertRaisesRegex(cleanup.UnsafeAttempt, "metadata changed"):
            cleanup.delete_attempt(attempt, mutate)
        self.assertEqual(len(events), 1)
        self.assertFalse(first.exists())
        self.assertTrue(second.exists())

    def test_deletion_error_is_reported_without_silencing(self):
        self.three()
        with patch.object(Path, "unlink", side_effect=PermissionError("locked product")):
            report, _ = self.run_cleanup(True)
        self.assertEqual(report["status"], "ERROR")
        self.assertEqual(report["deletedFiles"], 0)
        self.assertIn("locked product", next(item for item in report["attempts"] if item["status"] == "error")["reason"])

    def test_busy_requests_and_scoped_processes(self):
        paths = self.three()
        (self.requests / "recovery.json").write_text(json.dumps({"state": "NEEDS_RECOVERY"}))
        report, _ = self.run_cleanup(True)
        self.assertEqual(report["status"], "BUSY")
        self.assertTrue((paths[0] / "products/test.exe").exists())
        (self.requests / "recovery.json").write_text(json.dumps({"state": "finished"}))
        unrelated = {"ProcessId": 1, "Name": "python.exe", "CommandLine": "C:/DCM/Ghidra/server.py"}
        current = {"ProcessId": os.getpid(), "Name": "python.exe", "CommandLine": str(self.root / "cleanup.py")}
        active = {"ProcessId": 2, "Name": "ctest.exe", "CommandLine": 'ctest --test-dir "' + str(self.root / "build") + '"'}
        report, _ = self.run_cleanup(True, [unrelated, current, active])
        self.assertEqual(report["status"], "BUSY")
        self.assertEqual(len(report["busy"]), 1)
        report, _ = self.run_cleanup(True, [unrelated, current])
        self.assertEqual(report["deletedFiles"], 5)

    def test_policy_rejects_escape_overlap_and_unsafe_retention(self):
        for marker in ("../outside.json", "/outside.json", "C:/outside.json", "folder\\result.json"):
            self.data["roots"][0]["receipt"]["path"] = marker
            with self.assertRaises(ValueError):
                self.policy()
        self.data["roots"][0]["receipt"]["path"] = "result.json"
        self.data["keep_successful"] = 1
        with self.assertRaises(ValueError):
            self.policy()
        self.data["keep_successful"] = 2
        self.data["roots"].append(dict(self.data["roots"][0]))
        with self.assertRaises(ValueError):
            self.policy()

    def make_directory_link(self, link, target):
        try:
            os.symlink(target, link, target_is_directory=True)
        except OSError:
            if os.name != "nt":
                raise
            result = subprocess.run(["cmd", "/d", "/c", "mklink", "/J", str(link), str(target)], capture_output=True)
            if result.returncode:
                self.skipTest("host does not permit a symlink or junction")

    def test_reparse_entry_and_managed_root_are_rejected(self):
        paths = self.three()
        outside = self.home / "outside"
        outside.mkdir()
        (outside / "foreign.exe").write_bytes(b"keep")
        self.make_directory_link(paths[0] / "linked", outside)
        report, _ = self.run_cleanup(True)
        self.assertEqual(report["deletedFiles"], 0)
        self.assertTrue((outside / "foreign.exe").exists())
        alias = self.home / "alias"
        self.make_directory_link(alias, self.root)
        self.data["roots"][0]["path"] = str(alias)
        with self.assertRaises(cleanup.UnsafeAttempt):
            self.policy()

    def test_all_values_requires_complete_keys_and_every_value(self):
        paths = self.three()
        self.data["roots"][0]["receipt"] = {"kind": "all_values", "path": "result.json",
            "required_keys": ["normal", "failure"], "accepted": ["PASS"]}
        for path in paths:
            (path / "result.json").write_text(json.dumps({"normal": "PASS", "failure": "PASS"}))
        self.data["min_age_hours"] = 0
        report, _ = self.run_cleanup(True)
        self.assertEqual(report["deletedFiles"], 5)
        for value in ({}, {"normal": "PASS"}, {"normal": "PASS", "failure": "FAIL"},
                      {"normal": "PASS", "failure": "PASS", "extra": "RUNNING"}):
            self.assertFalse(cleanup.receipt_valid(json.dumps(value).encode(), self.data["roots"][0]["receipt"]))

    def test_sealed_approval_binds_only_discovered_leaf_and_marker(self):
        paths = self.three()
        approvals = {}
        for path in paths:
            marker = path / "result.json"
            approvals[str(path)] = {"marker": "result.json", "sha256": hashlib.sha256(marker.read_bytes()).hexdigest()}
        approvals[str(self.home / "outside")] = {"marker": "result.json", "sha256": "0" * 64}
        manifest = self.home / "validated.json"
        manifest.write_text(json.dumps(approvals))
        self.data["validated_attempts"] = str(manifest)
        self.data["roots"][0]["receipt"] = {"kind": "sealed"}
        policy = self.policy()
        attempt = cleanup.collect_attempt(self.root, paths[0], policy["roots"][0])
        manifest.write_text(json.dumps({}))
        with self.assertRaisesRegex(cleanup.UnsafeAttempt, "approval changed"):
            cleanup.delete_attempt(attempt, lambda event: None)
        manifest.write_text(json.dumps(approvals))
        report, _ = self.run_cleanup(True)
        self.assertEqual(report["deletedFiles"], 5)

    def test_marker_hash_mismatch_stays_protected(self):
        path = self.attempt(0)
        manifest = self.home / "validated.json"
        manifest.write_text(json.dumps({str(path): {"marker": "result.json", "sha256": "0" * 64}}))
        self.data["validated_attempts"] = str(manifest)
        self.data["roots"][0]["receipt"] = {"kind": "sealed"}
        report, _ = self.run_cleanup(True)
        self.assertEqual(report["deletedFiles"], 0)
        self.assertIn("SHA256", report["attempts"][0]["reason"])

    def test_receipt_alternatives_share_retention_and_block_explicit_failure(self):
        paths = self.three()
        new = self.attempt("new", days=1)
        (new / "cleanup-complete.json").write_text(json.dumps({"state": "PASS"}))
        self.data["roots"][0]["receipt"] = [
            {"path": "cleanup-complete.json", "key": "state", "accepted": ["PASS"]},
            {"path": "result.json", "key": "state", "accepted": ["PASS"]}]
        self.data["min_age_hours"] = 0
        report, _ = self.run_cleanup(True)
        self.assertEqual(report["deletedFiles"], 10)
        self.assertTrue((paths[2] / "products/test.exe").exists())
        self.assertTrue((new / "products/test.exe").exists())
        blocked = self.attempt("blocked", days=9)
        (blocked / "cleanup-complete.json").write_text(json.dumps({"state": "FAIL"}))
        report, _ = self.run_cleanup(True)
        self.assertTrue((blocked / "products/test.exe").exists())
        self.assertEqual(next(item for item in report["attempts"] if item["attempt"] == str(blocked))["status"], "protected")

    @unittest.skipUnless(os.name == "nt", "actual Windows process inventory")
    def test_actual_windows_process_inventory_detects_scoped_child(self):
        self.policy()
        child = subprocess.Popen([sys.executable, "-B", "-c",
            "import sys;print('ready',flush=True);sys.stdin.read()", str(self.root / "build")],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            self.assertEqual(child.stdout.readline().strip(), b"ready")
            reasons = cleanup.busy_reasons(self.policy())
            self.assertTrue(any(f"process {child.pid}:" in reason for reason in reasons), reasons)
        finally:
            child.stdin.close()
            child.wait(timeout=10)
            child.stdout.close()
            child.stderr.close()
        self.assertEqual(child.returncode, 0)

    def test_os_lock_blocks_a_second_real_process(self):
        paths = self.three()
        self.policy()
        command = ("import sys;from pathlib import Path;sys.path.insert(0,sys.argv[1]);"
                   "from clean_build_artifacts import RunLock;lock=RunLock(Path(sys.argv[2]));"
                   "assert lock.acquire();print('locked',flush=True);sys.stdin.read();lock.close()")
        holder = subprocess.Popen([sys.executable, "-B", "-c", command, str(SCRIPT.parent),
            str(self.policy_path.with_suffix(".cleanup.lock"))], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            self.assertEqual(holder.stdout.readline().strip(), b"locked")
            report = self.home / "busy.json"
            contender = subprocess.run([sys.executable, str(SCRIPT), "--policy", str(self.policy_path),
                "--apply", "--report", str(report)], capture_output=True, timeout=20)
            self.assertEqual(contender.returncode, 2, contender.stderr)
            self.assertEqual(json.loads(report.read_text())["status"], "BUSY")
            self.assertTrue((paths[0] / "products/test.exe").exists())
        finally:
            holder.stdin.close()
            holder.wait(timeout=10)
            holder.stdout.close()
            holder.stderr.close()
        self.assertEqual(holder.returncode, 0)
        self.assertTrue(self.policy_path.with_suffix(".cleanup.lock").exists())

    def test_cli_dry_run_records_disk_free_and_does_not_delete(self):
        paths = self.three()
        self.policy()
        report_path = self.home / "dry.json"
        with patch.object(cleanup, "windows_processes", return_value=[]):
            result = cleanup.main(["--policy", str(self.policy_path), "--report", str(report_path)])
        report = json.loads(report_path.read_text())
        self.assertEqual(result, 0)
        self.assertEqual(report["status"], "DRY_RUN")
        self.assertEqual(report["candidateFiles"], 5)
        self.assertIn(str(self.root), report["diskFreeBefore"])
        self.assertIn(str(self.root), report["diskFreeAfter"])
        self.assertTrue((paths[0] / "products/test.exe").exists())

    def test_cli_apply_records_actual_file_events_and_totals(self):
        self.three()
        self.policy()
        report_path = self.home / "apply.json"
        with patch.object(cleanup, "windows_processes", return_value=[]):
            result = cleanup.main(["--policy", str(self.policy_path), "--report", str(report_path), "--apply"])
        report = json.loads(report_path.read_text())
        self.assertEqual(result, 0)
        self.assertEqual(report["status"], "APPLIED")
        events = [json.loads(line) for line in Path(report["filesLog"]).read_text().splitlines()]
        self.assertEqual(report["deletedFiles"], len(events))
        self.assertEqual(len(events), 5)
        self.assertEqual(report["deletedLogicalBytes"], sum(item["logicalBytes"] for item in events))

    def test_cli_never_overwrites_existing_report_or_file_log(self):
        paths = self.three()
        self.policy()
        for existing in ("old.json", "old.files.jsonl"):
            path = self.home / existing
            path.write_bytes(b"original report must survive")
            with patch.object(cleanup, "windows_processes") as reader:
                code = cleanup.main(["--policy", str(self.policy_path), "--report", str(self.home / "old.json"), "--apply"])
            reader.assert_not_called()
            self.assertEqual(code, 1)
            self.assertEqual(path.read_bytes(), b"original report must survive")
            self.assertTrue((paths[0] / "products/test.exe").exists())
            self.assertFalse(self.policy_path.with_suffix(".cleanup.lock").exists())
            path.unlink()


if __name__ == "__main__":
    unittest.main()

"""Verify the public corpus update cannot erase a recorded regression."""
from __future__ import annotations

from contextlib import redirect_stdout
from io import StringIO
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))
import check_regression_corpus as gate


class RegressionCorpusRatchet(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="xray_corpus_ratchet_test_")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.corpus = self.root / "corpus"
        self.corpus.mkdir()
        self.baseline = self.corpus / "xir_passing.txt"
        self.original = b"# Preserve this historical evidence verbatim.\r\n\r\ncase.xr::stable\r\n"
        self.baseline.write_bytes(self.original)

    def invoke(self, files: list[dict], update: bool) -> tuple[int, str]:
        def run_report(xray: Path, jobs: int, report: Path, timeout: int):
            report.write_text(json.dumps({"schema": 1, "files": files}), encoding="utf-8")
            return subprocess.CompletedProcess([str(xray)], 1, b"", b"")

        argv = ["check_regression_corpus.py", "--xray", str(self.root / "xray"),
                "--baseline", str(self.baseline)]
        if update:
            argv.append("--update")
        output = StringIO()
        with mock.patch.object(gate, "CORPUS", self.corpus), \
                mock.patch.object(gate, "run_corpus", side_effect=run_report), redirect_stdout(output):
            status = gate.main(argv)
        return status, output.getvalue()

    def entry(self, name: str, cases: list[dict], error: str | None = None) -> dict:
        result = {"path": str(self.corpus / name), "cases": cases}
        if error is not None:
            result["error"] = error
        return result

    def test_update_refuses_failed_skipped_missing_and_uncompiled_recorded_tests(self) -> None:
        newly_passing = self.entry("new.xr", [{"name": "new", "status": "passed"}])
        cases = (
            (self.entry("case.xr", [{"name": "stable", "status": "failed", "message": "wrong value"}]),
             "wrong value"),
            (self.entry("case.xr", [{"name": "stable", "status": "skipped"}]), "not executed"),
            (None, "not executed"),
            (self.entry("case.xr", [], "source check failed"), "source check failed"),
        )
        for recorded_file, reason in cases:
            with self.subTest(reason=reason, recorded_file=recorded_file):
                self.baseline.write_bytes(self.original)
                files = [newly_passing] + ([recorded_file] if recorded_file is not None else [])
                status, output = self.invoke(files, update=True)
                self.assertEqual(status, 1)
                self.assertIn("refusing --update", output)
                self.assertIn(f"case.xr::stable: {reason}", output)
                self.assertEqual(self.baseline.read_bytes(), self.original)

    def test_update_adds_new_passes_and_keeps_failing_files_in_the_denominator(self) -> None:
        files = [self.entry("case.xr", [{"name": "stable", "status": "passed"},
                                         {"name": "new", "status": "passed"}]),
                 self.entry("unimplemented.xr", [], "unsupported syntax")]
        status, output = self.invoke(files, update=True)
        self.assertEqual(status, 0)
        self.assertIn("corpus: 2 files, 2 tests pass, 1 fail or error", output)
        self.assertEqual(gate.read_baseline(self.baseline), ["case.xr::new", "case.xr::stable"])
        updated = self.baseline.read_bytes()
        status, output = self.invoke(files, update=False)
        self.assertEqual(status, 0)
        self.assertIn("regression corpus matches its baseline", output)
        self.assertEqual(self.baseline.read_bytes(), updated)

    def test_verification_rejects_unrecorded_passes_without_writing(self) -> None:
        files = [self.entry("case.xr", [{"name": "stable", "status": "passed"},
                                         {"name": "new", "status": "passed"}])]
        status, output = self.invoke(files, update=False)
        self.assertEqual(status, 1)
        self.assertIn("test(s) pass but are not recorded", output)
        self.assertEqual(self.baseline.read_bytes(), self.original)


if __name__ == "__main__":
    unittest.main()

"""Keep legal concurrency traces from hiding incomplete or divergent output."""
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import run_smoke as smoke


class TraceOracleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = smoke.Source("demos/05-concurrency/shared_data.xr", "demos", "run")
        source = self.root / self.source.path
        source.parent.mkdir(parents=True)
        source.write_bytes(b"scope { go alpha(); go beta() }\n")
        self.catalog = {"schema": 1, "source": self.source.path,
                        "sourceCanonicalSha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                        "traces": ["alpha\nbeta\n", "beta\nalpha\n"]}
        (self.root / "expected.txt").write_bytes(b"alpha\nbeta\n")
        self.case = {"id": self.source.path, "path": self.source.path, "mode": "run",
                     "expect": "pass", "exit_code": 0, "stdout": "expected.txt", "stdout_traces": "traces.json"}
        self.native = dict(self.case, id="native:" + self.source.path, mode="build-run")
        self.write_catalog()

    def write_catalog(self):
        (self.root / "traces.json").write_text(json.dumps(self.catalog), encoding="utf-8")

    def load(self, cases=None):
        (self.root / "cases.json").write_text(json.dumps({"schema": 1, "cases": cases or [self.case, self.native]}))
        return smoke.load_cases(self.root / "cases.json", self.root, {self.source.path: self.source})

    def rows(self, outputs):
        rows = []
        for i, (case, output) in enumerate(zip([self.case, self.native], outputs)):
            logs = self.root / str(i)
            logs.mkdir()
            (logs / "run.stdout").write_bytes(output)
            rows.append({"id": case["id"], "status": "pass", "log_directory": str(logs)})
        return rows

    def test_source_identity_and_both_companions_are_required(self):
        self.assertEqual(len(self.load()), 2)
        (self.root / self.source.path).write_bytes(b"different program\n")
        with self.assertRaisesRegex(ValueError, "source identity"):
            self.load()

    def test_line_endings_only_are_canonical_for_source(self):
        source = self.root / self.source.path
        source.write_bytes(source.read_bytes().replace(b"\n", b"\r\n"))
        self.assertEqual(len(self.load()), 2)
        self.native.pop("stdout_traces")
        with self.assertRaisesRegex(ValueError, "same complete trace set|share the complete trace set"):
            self.load()

    def test_rejects_unreviewed_shape_and_incomplete_traces(self):
        original = deepcopy(self.catalog)
        for mutation in ({"schema": True}, {"traces": []}, {"traces": ["alpha"]},
                         {"traces": ["alpha\r\n"]}, {"traces": ["alpha\n", "alpha\n"]},
                         {"traces": [3]}, {"extra": "unreviewed"}):
            with self.subTest(mutation=mutation):
                self.catalog = dict(original, **mutation)
                self.write_catalog()
                with self.assertRaises(ValueError):
                    self.load()

    def test_gaps_and_unrelated_sources_cannot_use_traces(self):
        self.case.update(expect="gap", exit_code=1, gap={"feature": "F19", "first_diagnostic": "closed"})
        with self.assertRaisesRegex(ValueError, "restricted"):
            self.load()
        self.case.update(expect="pass", path="demos/hello.xr")
        with self.assertRaisesRegex(ValueError, "restricted"):
            smoke.expected_traces(self.case, self.root)

    def test_default_oracle_cannot_disagree_with_trace_set(self):
        (self.root / "expected.txt").write_bytes(b"a new observed output\n")
        with self.assertRaisesRegex(ValueError, "absent from the complete trace set"):
            self.load()

    def test_real_execute_path_checks_whole_bytes_without_sorting(self):
        for output, status in ((b"beta\nalpha\n", "pass"), (b"alpha\n", "fail"),
                               (b"beta\r\nalpha\r\n", "fail"), (b"alpha\nalpha\n", "fail")):
            result = smoke.proc.ProcResult(("unit protocol stub",), 0, output, b"", False)
            with self.subTest(output=output), patch.object(smoke.proc, "run", return_value=result):
                row = smoke.execute(self.case, self.source, self.root / "xray", self.root,
                                    self.root, self.root, 1)
                self.assertEqual(row["status"], status)
                self.assertEqual((self.root / "run.stdout").read_bytes(), output)

    def test_two_legal_but_different_engine_traces_fail_both(self):
        rows = self.rows([b"alpha\nbeta\n", b"beta\nalpha\n"])
        changed = smoke.verify_pairs([self.case, self.native], rows)
        self.assertEqual(len(changed), 2)
        self.assertTrue(all(row["status"] == "fail" for row in rows))
        self.assertTrue(all("bytes differ" in row["reason"] for row in rows))

    def test_identical_raw_traces_have_one_shared_hash(self):
        rows = self.rows([b"beta\nalpha\n"] * 2)
        self.assertEqual(smoke.verify_pairs([self.case, self.native], rows), [])
        self.assertEqual(rows[0]["trace_pair"], rows[1]["trace_pair"])
        self.assertEqual(rows[0]["trace_pair"]["raw_sha256"], hashlib.sha256(b"beta\nalpha\n").hexdigest())

    def test_missing_raw_log_or_failed_companion_cannot_score_one_side(self):
        rows = self.rows([b"alpha\nbeta\n"] * 2)
        (Path(rows[1]["log_directory"]) / "run.stdout").unlink()
        self.assertEqual(len(smoke.verify_pairs([self.case, self.native], rows)), 2)
        rows[0]["status"] = "pass"
        rows[1].update(status="fail", reason="native build failed")
        self.assertEqual(len(smoke.verify_pairs([self.case, self.native], rows)), 1)
        self.assertEqual(rows[1]["reason"], "native build failed")

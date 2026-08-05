"""workspace and report tests: cleanup fires on the exception path, KEEP_TMP
preserves, and a non-passing case without a reason is rejected at construction.
"""

import io
import os
import unittest
from pathlib import Path

from _support import bootstrap_xraytest

bootstrap_xraytest()
from xraytest import report, workspace  # noqa: E402


class WorkspaceTest(unittest.TestCase):
    def test_directory_removed_on_normal_exit(self):
        seen = {}
        with workspace.Workspace("t") as ws:
            seen["root"] = ws.root
            self.assertTrue(ws.root.is_dir())
        self.assertFalse(seen["root"].exists())

    def test_directory_removed_on_exception(self):
        seen = {}
        with self.assertRaises(RuntimeError):
            with workspace.Workspace("t") as ws:
                seen["root"] = ws.root
                raise RuntimeError("boom")
        self.assertFalse(seen["root"].exists())

    def test_keep_preserves_directory(self):
        import shutil

        seen = {}
        try:
            with workspace.Workspace("t", keep=True) as ws:
                seen["root"] = ws.root
            self.assertTrue(seen["root"].is_dir())
        finally:
            # keep=True intentionally leaves the tree; the test owns cleanup so
            # it does not litter the temp dir on the way out.
            if "root" in seen:
                shutil.rmtree(seen["root"], ignore_errors=True)

    def test_write_and_read_back(self):
        with workspace.Workspace("t") as ws:
            p = ws.write("a/b.txt", "content")
            self.assertEqual(p.read_text(), "content")
            self.assertTrue(str(p).endswith("b.txt"))

    def test_use_outside_context_raises(self):
        ws = workspace.Workspace("t")
        with self.assertRaises(RuntimeError):
            _ = ws.root


class ReportTest(unittest.TestCase):
    def test_fail_without_reason_is_rejected(self):
        with self.assertRaises(ValueError):
            report.CaseResult(name="x", status=report.Status.FAIL)

    def test_pass_needs_no_reason(self):
        r = report.CaseResult(name="x", status=report.Status.PASS)
        self.assertIs(r.status, report.Status.PASS)

    def test_counts_and_exit_code(self):
        rep = report.Report("suite")
        rep.record("a", report.Status.PASS)
        rep.record("b", report.Status.FAIL, detail="bad")
        rep.record("c", report.Status.SKIP, detail="no toolchain")
        self.assertEqual(len(rep.passed), 1)
        self.assertEqual(len(rep.failed), 1)
        self.assertEqual(len(rep.skipped), 1)
        self.assertEqual(rep.exit_code, 1)
        self.assertEqual(rep.failed_names, ["b"])
        self.assertEqual(rep.skipped_names, ["c"])

    def test_all_pass_is_exit_zero(self):
        rep = report.Report("suite")
        rep.record("a", report.Status.PASS)
        self.assertEqual(rep.exit_code, 0)

    def test_skip_alone_does_not_fail_suite(self):
        rep = report.Report("suite")
        rep.record("a", report.Status.SKIP, detail="skipped")
        self.assertEqual(rep.exit_code, 0)

    def test_write_summary_shape(self):
        rep = report.Report("mysuite")
        rep.record("a", report.Status.PASS)
        rep.record("b", report.Status.FAIL, detail="reason")
        buf = io.StringIO()
        rep.write(buf)
        text = buf.getvalue()
        self.assertIn("mysuite: 1 passed, 1 failed, 0 skipped", text)
        self.assertIn("(reason)", text)


if __name__ == "__main__":
    unittest.main()

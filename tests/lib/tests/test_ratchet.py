"""Ratchet tests. The subtle case is skip: a skipped baseline entry is neither
a regression nor a fix, so it must not be reported as a line to delete.
"""

import unittest
from pathlib import Path

from _support import bootstrap_xraytest

bootstrap_xraytest()
from xraytest import ratchet  # noqa: E402


class EvaluateTest(unittest.TestCase):
    def test_failure_outside_baseline_is_a_new_failure(self):
        r = ratchet.evaluate(failed={"a", "b"}, baseline={"a"})
        self.assertEqual(r.new_failures, ["b"])
        self.assertEqual(r.now_passing, [])
        self.assertFalse(r.ok)

    def test_baselined_case_that_now_passes_must_be_deleted(self):
        r = ratchet.evaluate(failed={"a"}, baseline={"a", "b"})
        self.assertEqual(r.new_failures, [])
        self.assertEqual(r.now_passing, ["b"])
        self.assertFalse(r.ok)

    def test_skipped_baseline_entry_is_not_reported_as_fixed(self):
        # b is baselined and did not run; it has not been shown to pass.
        r = ratchet.evaluate(failed={"a"}, baseline={"a", "b"}, skipped={"b"})
        self.assertEqual(r.now_passing, [])
        self.assertTrue(r.ok)

    def test_all_baselined_and_failing_is_green(self):
        r = ratchet.evaluate(failed={"a", "b"}, baseline={"a", "b"})
        self.assertTrue(r.ok)
        self.assertEqual(r.failing_count, 2)
        self.assertEqual(r.baseline_count, 2)

    def test_empty_everything_is_green(self):
        r = ratchet.evaluate(failed=set(), baseline=set())
        self.assertTrue(r.ok)

    def test_outputs_are_sorted(self):
        r = ratchet.evaluate(failed={"z", "y", "x"}, baseline=set())
        self.assertEqual(r.new_failures, ["x", "y", "z"])


class ReadBaselineTest(unittest.TestCase):
    def setUp(self):
        import tempfile

        self.tmp = tempfile.mkdtemp(prefix="xt_ratchet.")
        self.path = Path(self.tmp) / "baseline.txt"

    def tearDown(self):
        import shutil

        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_comments_blanks_and_trailing_space_are_stripped(self):
        self.path.write_text(
            "# header comment\n"
            "case_one.xr\n"
            "\n"
            "case_two.xr   # inline comment\n"
            "  case_three.xr  \n",
            encoding="utf-8",
        )
        self.assertEqual(
            ratchet.read_baseline(self.path),
            {"case_one.xr", "case_two.xr", "case_three.xr"},
        )

    def test_missing_file_is_empty_baseline(self):
        self.assertEqual(ratchet.read_baseline(self.path.parent / "nope.txt"), set())


if __name__ == "__main__":
    unittest.main()

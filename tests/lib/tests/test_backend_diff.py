"""Unit tests for the backend-diff runner's own logic.

Covers the three things the migration contract requires: argument/env parsing,
command construction, and the ratchet decision. The command-construction test
in particular pins that the case-directory key includes .xr.expected, so a
changed oracle changes the key -- the miss that would otherwise diff a stale
binary against a new expectation and pass by accident.
"""

import unittest
from pathlib import Path

from _support import bootstrap_xraytest, load_module

bootstrap_xraytest()

_RUNNER = Path(__file__).resolve().parents[2] / "diff" / "run_backend_diff.py"


rbd = load_module("rbd_under_test", _RUNNER)


class JobConfigTest(unittest.TestCase):
    def test_explicit_job_count_is_not_auto(self):
        jobs, auto = rbd.configure_jobs("4")
        self.assertEqual(jobs, 4)
        self.assertFalse(auto)

    def test_auto_is_capped_and_flagged(self):
        jobs, auto = rbd.configure_jobs("auto")
        self.assertTrue(auto)
        self.assertLessEqual(jobs, 16)
        self.assertGreaterEqual(jobs, 1)

    def test_garbage_falls_back_to_serial(self):
        jobs, auto = rbd.configure_jobs("not-a-number")
        self.assertEqual(jobs, 1)


class ShardValidationTest(unittest.TestCase):
    def test_valid_shard(self):
        self.assertEqual(rbd.validate_shard("4", "2"), (4, 2))

    def test_index_past_total_rejected(self):
        with self.assertRaises(ValueError):
            rbd.validate_shard("4", "4")

    def test_non_numeric_rejected(self):
        with self.assertRaises(ValueError):
            rbd.validate_shard("x", "0")


class CaseKeyTest(unittest.TestCase):
    def setUp(self):
        import tempfile

        self.tmp = tempfile.mkdtemp(prefix="xt_bd.")
        self.dir = Path(self.tmp)

    def tearDown(self):
        import shutil

        shutil.rmtree(self.tmp, ignore_errors=True)

    def _case_dir_key(self):
        from xraytest import cache

        return cache.dir_key(self.dir, rbd.CASE_DIR_GLOBS)

    def test_expected_oracle_is_part_of_the_key(self):
        # The load-bearing invariant: change .xr.expected, the key must change,
        # or a cached binary would be diffed against a new oracle and pass.
        (self.dir / "c.xr").write_text("print(1)\n")
        (self.dir / "c.xr.expected").write_text("1\n")
        before = self._case_dir_key()
        (self.dir / "c.xr.expected").write_text("2\n")
        after = self._case_dir_key()
        self.assertNotEqual(before, after)

    def test_args_sidecar_is_part_of_the_key(self):
        (self.dir / "c.xr").write_text("print(1)\n")
        (self.dir / "c.args").write_text("--foo\n")
        before = self._case_dir_key()
        (self.dir / "c.args").write_text("--bar\n")
        self.assertNotEqual(before, self._case_dir_key())

    def test_cache_name_is_bounded_and_readable(self):
        name = rbd.case_cache_name("some/dir/a_very_long_case_name_that_exceeds.xr", "key")
        self.assertLessEqual(len(name), 32 + 1 + 24)
        self.assertTrue(name.startswith("a_very_long_case_name"))


class SidecarTest(unittest.TestCase):
    def setUp(self):
        import tempfile

        self.tmp = tempfile.mkdtemp(prefix="xt_bd_sc.")
        self.dir = Path(self.tmp)
        self.case = self.dir / "c.xr"
        self.case.write_text("// anchor: my-anchor\n// diff-backends: vm,aot\nprint(1)\n")

    def tearDown(self):
        import shutil

        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_reads_anchor_directive(self):
        self.assertEqual(rbd.read_first_directive(self.case, "// anchor: ", 1), "my-anchor")

    def test_diff_backends_only_scanned_in_head(self):
        self.assertEqual(
            rbd.read_first_directive(self.case, "// diff-backends: ", 5), "vm,aot"
        )

    def test_args_first_line_split(self):
        (self.dir / "c.args").write_text("--a --b c\n")
        self.assertEqual(rbd.read_args(self.case), ["--a", "--b", "c"])

    def test_missing_stdin_is_empty_bytes(self):
        self.assertEqual(rbd.read_stdin(self.case), b"")

    def test_expected_sidecar_read_as_bytes(self):
        Path(str(self.case) + ".expected").write_bytes(b"out\n")
        self.assertEqual(rbd.read_expected_stdout(self.case), b"out\n")


class RatchetWiringTest(unittest.TestCase):
    """The runner delegates to xraytest.ratchet; verify the wiring, including
    the partial-run suppression of the now-passing check."""

    def test_new_failure_flagged(self):
        from xraytest import ratchet

        v = ratchet.evaluate(failed={"x.xr"}, baseline=set())
        self.assertEqual(v.new_failures, ["x.xr"])
        self.assertFalse(v.ok)

    def test_partial_run_never_reports_now_passing(self):
        # Mirrors main()'s partial-run call: unseen baseline entries are fed as
        # skipped, so none can be declared fixed.
        from xraytest import ratchet

        baseline = {"a.xr", "b.xr", "c.xr"}
        failed = {"a.xr"}
        v = ratchet.evaluate(failed=failed, baseline=baseline, skipped=baseline - failed)
        self.assertEqual(v.now_passing, [])
        self.assertTrue(v.ok)


if __name__ == "__main__":
    unittest.main()

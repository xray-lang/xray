"""Unit tests for the AOT equivalence suite's own logic.

The load-bearing one is the tombstone gate: it decides the exit code of
aot_standalone_suite, so a mistake there either hides a regression or blocks a
clean tree. Its policy is the shared only-shrink ratchet plus a size ceiling.
"""

import tempfile
import unittest
from pathlib import Path

from _support import bootstrap_xraytest, load_module

bootstrap_xraytest()

_AOT_DIR = Path(__file__).resolve().parents[2] / "aot"
runner = load_module("run_aot_tests_under_test", _AOT_DIR / "run_aot_tests.py")
Status = runner.Status


def _verdict(name, status, detail=""):
    return runner.CaseVerdict(name=name, status=status, detail=detail)


class TombstoneParseTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="xt_tomb.")
        self.path = Path(self.tmp) / "TOMBSTONES.tsv"

    def tearDown(self):
        import shutil

        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_skips_comments_blanks_and_header(self):
        self.path.write_text(
            "# a comment\n"
            "\n"
            "case\tfailure_class\n"
            "coro/alpha\tcoro-diff\n"
            "basic/beta\tfloat-diff\n",
            encoding="utf-8",
        )
        self.assertEqual(runner.read_tombstones(self.path), {"coro/alpha", "basic/beta"})

    def test_empty_inventory(self):
        self.path.write_text("# only comments\n", encoding="utf-8")
        self.assertEqual(runner.read_tombstones(self.path), set())


class TombstoneGateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="xt_gate.")
        self.path = Path(self.tmp) / "TOMBSTONES.tsv"

    def tearDown(self):
        import shutil

        shutil.rmtree(self.tmp, ignore_errors=True)

    def _write(self, *names):
        body = "case\tfailure_class\n" + "".join(f"{n}\tother\n" for n in names)
        self.path.write_text(body, encoding="utf-8")

    def test_only_tombstoned_failures_passes(self):
        self._write("coro/alpha")
        verdicts = [_verdict("coro/alpha", Status.FAIL.value, "x"),
                    _verdict("basic/ok", Status.PASS.value)]
        self.assertEqual(runner.tombstone_gate(self.path, verdicts, 10), 0)

    def test_untombstoned_failure_fails_closed(self):
        self._write("coro/alpha")
        verdicts = [_verdict("coro/alpha", Status.FAIL.value, "x"),
                    _verdict("coro/beta", Status.FAIL.value, "y")]
        self.assertEqual(runner.tombstone_gate(self.path, verdicts, 10), 1)

    def test_empty_inventory_means_no_failure_allowed(self):
        self._write()
        verdicts = [_verdict("coro/alpha", Status.FAIL.value, "x")]
        self.assertEqual(runner.tombstone_gate(self.path, verdicts, 10), 1)

    def test_all_green_with_empty_inventory_passes(self):
        self._write()
        self.assertEqual(runner.tombstone_gate(self.path, [_verdict("a", Status.PASS.value)], 10), 0)

    def test_resolved_tombstone_reported_but_not_fatal_by_itself(self):
        # A tombstoned case that now passes is reported for pruning; on its own
        # it does not fail the gate (the shell gate behaved the same way).
        self._write("coro/alpha")
        verdicts = [_verdict("coro/alpha", Status.PASS.value)]
        self.assertEqual(runner.tombstone_gate(self.path, verdicts, 10), 0)

    def test_inventory_larger_than_ceiling_fails(self):
        self._write("a", "b", "c")
        self.assertEqual(runner.tombstone_gate(self.path, [], 2), 1)

    def test_no_ceiling_allows_any_size(self):
        self._write("a", "b", "c")
        self.assertEqual(runner.tombstone_gate(self.path, [], None), 0)

    def test_skipped_tombstone_is_not_declared_resolved(self):
        # A tombstoned case that did not run has not been shown to pass.
        self._write("coro/alpha")
        verdicts = [_verdict("coro/alpha", Status.SKIP.value, "no toolchain")]
        self.assertEqual(runner.tombstone_gate(self.path, verdicts, 10), 0)


class CaseHelpersTest(unittest.TestCase):
    def test_sections(self):
        self.assertEqual(runner.POSITIVE_SECTIONS, ("basic", "modules", "coro"))
        self.assertEqual(runner.NEGATIVE_SECTION, "negative")

    def test_args_matched_by_stem_prefix(self):
        self.assertEqual(runner.case_args(Path("x/process_args.xr")), ("100000", "abc"))
        self.assertEqual(runner.case_args(Path("x/process_args_extra.xr")), ("100000", "abc"))
        self.assertEqual(runner.case_args(Path("x/other.xr")), ())

    def test_safe_case_name_sanitizes(self):
        self.assertEqual(runner.safe_case_name("coro/a b.xr"), "coro_a_b")

    def test_build_retries_are_bounded(self):
        self.assertGreaterEqual(runner.BUILD_ATTEMPTS, 1)

    def test_negative_rejection_patterns_present(self):
        # A negative case must be rejected for a recognized reason; an empty or
        # over-broad list would turn any compiler crash into a pass.
        self.assertIn(": error: ", runner.NEGATIVE_REJECTION_PATTERNS)
        self.assertGreater(len(runner.NEGATIVE_REJECTION_PATTERNS), 3)

    def test_configure_jobs(self):
        self.assertEqual(runner.configure_jobs("4"), 4)
        self.assertGreaterEqual(runner.configure_jobs("auto"), 1)
        self.assertEqual(runner.configure_jobs("junk"), 1)


if __name__ == "__main__":
    unittest.main()

"""Tests for exact compile-error corpus selection."""

import tempfile
import unittest
from pathlib import Path
from unittest import mock

from _support import bootstrap_xraytest, load_module

bootstrap_xraytest()

ROOT = Path(__file__).resolve().parents[3]
runner = load_module(
    "compile_error_runner_under_test",
    ROOT / "tests" / "compile_errors" / "run_compile_error_tests.py")
census = load_module(
    "diagnostic_position_census_under_test",
    ROOT / "tests" / "compile_errors" / "check_diagnostic_positions.py")
blesser = load_module(
    "compile_error_blesser_under_test",
    ROOT / "tests" / "compile_errors" / "bless_expected.py")


class ExactSelectionTest(unittest.TestCase):
    def test_t0_inventory_is_exact_distinct_and_has_expectations(self):
        manifest = ROOT / "tests" / "compile_errors" / "t0_cases.txt"
        selected = runner.load_case_list(manifest)
        cases = runner.collect_cases(selected)
        self.assertEqual(len(cases), 13)
        self.assertEqual(len(cases), len(set(cases)))

    def test_selection_rejects_missing_traversal_and_duplicates(self):
        for name in ("missing/no.xr", "../outside.xr", "syntax", "/absolute.xr"):
            with self.subTest(name=name), self.assertRaises(runner.SelectionError):
                runner.collect_cases([name])
        with self.assertRaisesRegex(runner.SelectionError, "duplicate"):
            runner.collect_cases([
                "syntax/012_range_inclusive_missing_end.xr",
                "syntax/012_range_inclusive_missing_end.xr",
            ])

    def test_empty_explicit_list_stays_empty_instead_of_running_every_case(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest = Path(directory) / "empty.txt"
            manifest.write_text("# no cases\n", encoding="utf-8")
            self.assertEqual(runner.load_case_list(manifest), [])
            self.assertEqual(runner.collect_cases([]), [])

    def test_timeout_is_an_explicit_red_verdict(self):
        with tempfile.TemporaryDirectory() as directory:
            category = Path(directory) / "semantic"
            category.mkdir()
            case = category / "stuck.xr"
            case.write_text("missing\n", encoding="utf-8")
            Path(str(case) + ".expected").write_text(
                "--> 1:1\nmissing\n", encoding="utf-8")
            timed_out = runner.proc.ProcResult(
                argv=("xray", str(case)), returncode=1, stdout=b"",
                stderr=b"partial diagnostic", timed_out=True)
            with mock.patch.object(runner.proc, "run", return_value=timed_out):
                result = runner.run_one_case(Path("xray"), case, 2.5)
            self.assertEqual(result.verdict, runner.FAIL)
            self.assertIn("timed out after 2.5 seconds", result.report)
            self.assertIn("process tree was terminated", result.report)
            self.assertIn("partial diagnostic", result.report)

    def test_position_census_preserves_timeout_state_and_output(self):
        case = census.SCRIPT_DIR / "semantic" / "stuck.xr"
        timed_out = census.proc.ProcResult(
            argv=("xray", "check", str(case)), returncode=1, stdout=b"",
            stderr=b"partial census diagnostic", timed_out=True)
        with mock.patch.object(census.proc, "run", return_value=timed_out):
            result = census.check_one(Path("xray"), case, 2.5)
        self.assertTrue(result.timed_out)
        self.assertEqual(result.rows, [])
        self.assertIn("partial census diagnostic", result.output)

    def test_bless_timeout_is_red_and_never_proposes_a_write(self):
        with tempfile.TemporaryDirectory() as directory:
            case = Path(directory) / "stuck.xr"
            case.write_text("missing\n", encoding="utf-8")
            Path(str(case) + ".expected").write_text(
                "--> 1:1\nmissing\n", encoding="utf-8")
            timed_out = blesser.proc.ProcResult(
                argv=("xray", str(case)), returncode=1, stdout=b"",
                stderr=b"partial bless diagnostic", timed_out=True)
            with mock.patch.object(blesser.proc, "run", return_value=timed_out):
                result = blesser.bless_one(Path("xray"), case, 2.5, False)
            self.assertTrue(result.failed)
            self.assertFalse(result.changed)
            self.assertIsNone(result.text)
            self.assertIn("expected file was not changed", result.notes[0])
            self.assertIn("partial bless diagnostic", result.notes[0])


if __name__ == "__main__":
    unittest.main()

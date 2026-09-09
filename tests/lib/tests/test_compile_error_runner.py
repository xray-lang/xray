"""Tests for exact compile-error corpus selection."""

import tempfile
import unittest
from pathlib import Path

from _support import bootstrap_xraytest, load_module

bootstrap_xraytest()

ROOT = Path(__file__).resolve().parents[3]
runner = load_module(
    "compile_error_runner_under_test",
    ROOT / "tests" / "compile_errors" / "run_compile_error_tests.py")


class ExactSelectionTest(unittest.TestCase):
    def test_t0_inventory_is_exact_distinct_and_has_expectations(self):
        manifest = ROOT / "tests" / "compile_errors" / "t0_cases.txt"
        selected = runner.load_case_list(manifest)
        cases = runner.collect_cases(selected)
        self.assertEqual(len(cases), 12)
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


if __name__ == "__main__":
    unittest.main()

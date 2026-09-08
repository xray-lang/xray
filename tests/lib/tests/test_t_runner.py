"""Tests for focused selection in the tiered test runner."""

import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from _support import bootstrap_xraytest, load_module

bootstrap_xraytest()

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))
runner = load_module("tiered_test_runner_under_test", ROOT / "scripts" / "t.py")


class FocusedSelectionTest(unittest.TestCase):
    def test_broad_options_do_not_select_out_auxiliary_corpora(self):
        self.assertFalse(runner.has_explicit_ctest_selection([]))
        self.assertFalse(runner.has_explicit_ctest_selection(["--output-on-failure"]))
        self.assertFalse(runner.has_explicit_ctest_selection(["-E", "slow"]))

    def test_name_and_label_selectors_are_focused(self):
        for arguments in (["-R", "parser"], ["-Rparser"],
                          ["--tests-regex=parser"], ["-L", "unit"], ["-Lunit"],
                          ["--label-regex", "unit"]):
            with self.subTest(arguments=arguments):
                self.assertTrue(runner.has_explicit_ctest_selection(arguments))

    def test_ninja_manifest_is_refreshed_before_test_selection(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            (build / "build.ninja").write_text("# ninja\n", encoding="utf-8")
            completed = SimpleNamespace(ok=True, combined_text=lambda: "")
            with mock.patch.object(runner.proc, "run", return_value=completed) as run:
                self.assertTrue(runner.refresh_cmake_manifest(build, 7))
            run.assert_called_once_with([
                "cmake", "--build", str(build), "-j", "7", "--target", "build.ninja"
            ])

    def test_non_ninja_manifest_needs_no_refresh(self):
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.object(runner.proc, "run") as run:
                self.assertTrue(runner.refresh_cmake_manifest(Path(directory), 3))
            run.assert_not_called()

    def test_stateful_ctest_selectors_are_focused(self):
        for arguments in (["-I", "1,1"], ["--tests-information", "1,1"],
                          ["--rerun-failed"], ["--tests-from-file", "tests.txt"]):
            with self.subTest(arguments=arguments):
                self.assertTrue(runner.has_explicit_ctest_selection(arguments))


if __name__ == "__main__":
    unittest.main()

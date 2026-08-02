#!/usr/bin/env python3
"""Canonical read/ref/move syntax rejection diagnostics."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def clean_output(text: str) -> str:
    return ANSI_RE.sub("", text)


class ParamModeDiagnosticsTest(unittest.TestCase):
    xray: Path

    def run_case(self, rel: str) -> str:
        env = os.environ.copy()
        env["NO_COLOR"] = "1"
        proc = subprocess.run(
            [str(self.xray), str(ROOT / rel)],
            cwd=ROOT,
            text=True,
            encoding="utf-8",
            errors="strict",
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=env,
            timeout=10,
        )
        self.assertNotEqual(0, proc.returncode, rel)
        return clean_output(proc.stdout)

    def assert_contains_all(self, output: str, needles: list[str]) -> None:
        for needle in needles:
            self.assertIn(needle, output)

    def assert_contains_none(self, output: str, needles: list[str]) -> None:
        for needle in needles:
            self.assertNotIn(needle, output)

    def test_prefix_param_modes_are_rejected(self) -> None:
        for rel in (
            "tests/compile_errors/syntax/028_param_mode_prefix_removed.xr",
            "tests/compile_errors/syntax/030_param_move_prefix_removed.xr",
        ):
            with self.subTest(rel=rel):
                output = self.run_case(rel)
                self.assert_contains_all(
                    output, ["parameter mode must appear after the parameter name and ':'"]
                )

    def test_postfix_param_mode_is_rejected(self) -> None:
        output = self.run_case("tests/compile_errors/syntax/031_param_mode_postfix_removed.xr")
        self.assert_contains_all(output, ["parameter mode must appear immediately after ':'"])

    def test_removed_in_out_forms_have_no_compatibility_path(self) -> None:
        cases = {
            "tests/compile_errors/syntax/032_param_in_mode_removed.xr":
                "'in' is a keyword and cannot be used as an identifier",
            "tests/compile_errors/syntax/033_param_out_mode_removed.xr":
                "expected ')' after parameter list",
            "tests/compile_errors/syntax/034_in_call_marker_removed.xr": "expected expression",
            "tests/compile_errors/syntax/035_out_call_marker_removed.xr":
                "expected ')' after argument list",
        }
        all_output = ""
        for rel, expected in cases.items():
            with self.subTest(rel=rel):
                output = self.run_case(rel)
                self.assert_contains_all(output, [expected])
                all_output += output
        self.assert_contains_none(all_output, ["deprecated", "compatibility alias"])


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--xray", type=Path, default=ROOT / "build/xray")
    args, unittest_args = parser.parse_known_args()
    ParamModeDiagnosticsTest.xray = args.xray.resolve()
    unittest.main(argv=[__file__, *unittest_args])


if __name__ == "__main__":
    main()

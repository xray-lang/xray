#!/usr/bin/env python3
"""ParamMode removed-syntax diagnostics and repair hints."""

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

    def test_prefix_param_mode_suggests_colon_position(self) -> None:
        output = self.run_case("tests/compile_errors/syntax/028_param_mode_prefix_removed.xr")
        self.assert_contains_all(
            output,
            [
                "error[E0805]",
                "parameter mode 'ref' before parameter name was removed",
                "note: Write parameter modes after the colon, for example `name: ref T`.",
            ],
        )
        self.assert_contains_none(output, ["write x: ref T", "use `ref x: T`"])

    def test_move_param_mode_suggests_call_site_move(self) -> None:
        for rel in (
            "tests/compile_errors/syntax/029_param_move_mode_removed.xr",
            "tests/compile_errors/syntax/030_param_move_prefix_removed.xr",
        ):
            with self.subTest(rel=rel):
                output = self.run_case(rel)
                self.assert_contains_all(
                    output,
                    [
                        "error[E0806]",
                        "`move` is not a parameter mode",
                        "note: Use a value parameter and write `move value` at the call site "
                        "when transferring ownership.",
                    ],
                )
                self.assert_contains_none(output, ["name: move T", "move name: T"])

    def test_postfix_or_combined_modes_suggest_single_colon_mode(self) -> None:
        cases = (
            (
                "tests/compile_errors/syntax/031_param_mode_postfix_removed.xr",
                "error[E0808]",
                "parameter mode 'ref' after parameter type was removed",
                "note: Write parameter modes immediately after the colon, for example `name: ref T`.",
            ),
            (
                "tests/compile_errors/syntax/032_param_mode_combined_removed.xr",
                "error[E0807]",
                "parameter modes cannot be combined",
                "note: Use exactly one parameter mode after the colon, for example `name: ref T`.",
            ),
        )
        for rel, code, title, note in cases:
            with self.subTest(rel=rel):
                output = self.run_case(rel)
                self.assert_contains_all(output, [code, title, note])
                self.assert_contains_none(output, ["int ref T", "ref out T"])

    def test_call_site_in_marker_suggests_plain_argument(self) -> None:
        output = self.run_case("tests/compile_errors/syntax/034_in_call_marker_removed.xr")
        self.assert_contains_all(
            output,
            [
                "error[E0809]",
                "call-site `in` marker was removed",
                "note: Pass the argument directly, for example `f(value)`; `in` is a "
                "declaration-side parameter mode only.",
            ],
        )
        self.assert_contains_none(output, ["for example `f(in value)`", "XR_CALL_ARG_IN"])


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--xray", type=Path, default=ROOT / "build/xray")
    args, unittest_args = parser.parse_known_args()
    ParamModeDiagnosticsTest.xray = args.xray.resolve()
    unittest.main(argv=[__file__, *unittest_args])


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Focused read/ref/move frontend contract gate."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")

NEGATIVE_CASES = (
    "tests/compile_errors/ownership/096_ref_call_requires_marker.xr",
    "tests/compile_errors/ownership/097_ref_marker_rejects_value_param.xr",
    "tests/compile_errors/ownership/159_generic_method_ref_contract_retained.xr",
    "tests/compile_errors/ffi/041_extern_ref_param_mode_rejected.xr",
)

POSITIVE_CASES = (
    "tests/regression/08_oop/0914_ref_constructor_call_authorization.xr",
    "tests/regression/13_types/1422_param_mode_nonoverlap_projection_alias.xr",
    "tests/regression/13_types/1433_param_mode_move_source_action.xr",
    "tests/regression/13_types/1435_param_mode_place_kind_authorization.xr",
    "tests/regression/13_types/1437_generic_method_param_contract.xr",
)


class ParamContractFrontendTest(unittest.TestCase):
    xray: Path

    def run_xray(self, args: list[str]) -> subprocess.CompletedProcess[str]:
        env = os.environ.copy()
        env["NO_COLOR"] = "1"
        return subprocess.run(
            [str(self.xray), *args],
            cwd=ROOT,
            text=True,
            encoding="utf-8",
            errors="strict",
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=env,
            timeout=15,
        )

    def test_call_contract_negative_matrix(self) -> None:
        for rel in NEGATIVE_CASES:
            with self.subTest(rel=rel):
                proc = self.run_xray([str(ROOT / rel)])
                output = ANSI_RE.sub("", proc.stdout)
                self.assertNotEqual(0, proc.returncode, rel)
                expected_path = ROOT / f"{rel}.expected"
                expected = expected_path.read_text(encoding="utf-8").strip()
                self.assertIn(expected, output, rel)

    def test_call_contract_positive_matrix(self) -> None:
        for rel in POSITIVE_CASES:
            with self.subTest(rel=rel):
                proc = self.run_xray(["test", str(ROOT / rel)])
                self.assertEqual(0, proc.returncode, f"{rel}\n{proc.stdout}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--xray", type=Path, default=ROOT / "build/xray")
    args, unittest_args = parser.parse_known_args()
    ParamContractFrontendTest.xray = args.xray.resolve()
    unittest.main(argv=[__file__, *unittest_args])


if __name__ == "__main__":
    main()

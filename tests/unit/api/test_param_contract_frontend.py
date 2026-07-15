#!/usr/bin/env python3
"""Focused task-206 call authorization and out-DA frontend gate."""

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
    "tests/compile_errors/ownership/101_out_call_requires_marker.xr",
    "tests/compile_errors/ownership/102_out_marker_rejects_value_param.xr",
    "tests/compile_errors/ownership/150_ref_marker_rejects_in_param.xr",
    "tests/compile_errors/ownership/151_out_marker_rejects_in_param.xr",
    "tests/compile_errors/ownership/152_out_marker_rejects_ref_param.xr",
    "tests/compile_errors/ownership/153_ref_marker_rejects_out_param.xr",
    "tests/compile_errors/ownership/105_out_param_read_before_write.xr",
    "tests/compile_errors/ownership/106_out_param_return_without_assignment.xr",
    "tests/compile_errors/ownership/108_out_param_method_return_without_assignment.xr",
    "tests/compile_errors/ownership/120_out_param_if_branch_missing_assignment.xr",
    "tests/compile_errors/ownership/124_out_param_try_assign_catch_missing.xr",
    "tests/compile_errors/ownership/126_out_param_field_assignment_not_whole_init.xr",
    "tests/compile_errors/ownership/159_generic_method_ref_contract_retained.xr",
)

POSITIVE_CASES = (
    "tests/regression/08_oop/0914_ref_constructor_call_authorization.xr",
    "tests/regression/13_types/1424_out_param_if_branches_assign.xr",
    "tests/regression/13_types/1427_out_param_try_catch_assign.xr",
    "tests/regression/13_types/1430_super_out_param_forward_initializes_caller.xr",
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
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=env,
            timeout=15,
        )

    def test_call_and_da_negative_matrix(self) -> None:
        for rel in NEGATIVE_CASES:
            with self.subTest(rel=rel):
                proc = self.run_xray([str(ROOT / rel)])
                output = ANSI_RE.sub("", proc.stdout)
                self.assertNotEqual(0, proc.returncode, rel)
                expected_path = ROOT / f"{rel}.expected"
                expected = expected_path.read_text(encoding="utf-8").strip()
                self.assertIn(expected, output, rel)

    def test_call_and_da_positive_matrix(self) -> None:
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

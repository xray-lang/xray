#!/usr/bin/env python3
"""Focused task-206 AOT ParamContract VM/native parity gate."""

from __future__ import annotations

import argparse
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]

POSITIVE_AOT_CASES = (
    "tests/aot/basic/ref_out_call_plan.xr",
    "tests/aot/basic/dynamic_function_value_ref_out_call_plan.xr",
    "tests/aot/modules/import_param_contract.xr",
    "tests/aot/filetests/cgen/fixed_array_ref_param_abi.xr",
)

EXPECTED_OUTPUTS = {
    "tests/aot/basic/ref_out_call_plan.xr": b"2\n2\n9\n9\n5\n5\n9\n9\n",
    "tests/aot/basic/dynamic_function_value_ref_out_call_plan.xr": b"2\n2\n4\n4\n10\n10\n20\n20\n",
    "tests/aot/modules/import_param_contract.xr": b"2\n2\n9\n9\n4\n4\n9\n9\n6\n6\n11\n11\n7\n7\n11\n11\n",
    "tests/aot/filetests/cgen/fixed_array_ref_param_abi.xr": b"10\n",
}


class ParamContractAotTest(unittest.TestCase):
    xray: Path

    def run_checked(self, args: list[str], *, stdout: int = subprocess.PIPE) -> subprocess.CompletedProcess[bytes]:
        return subprocess.run(
            args,
            cwd=ROOT,
            stdout=stdout,
            stderr=subprocess.STDOUT,
            check=True,
            timeout=30,
        )

    def test_param_contract_vm_aot_parity(self) -> None:
        for rel in POSITIVE_AOT_CASES:
            with self.subTest(rel=rel):
                src = ROOT / rel
                vm = self.run_checked([str(self.xray), str(src)]).stdout
                self.assertEqual(EXPECTED_OUTPUTS[rel], vm, rel)

                with tempfile.TemporaryDirectory(prefix="xray-param-contract-aot-") as tmp:
                    native = Path(tmp) / "case"
                    self.run_checked(
                        [
                            str(self.xray),
                            "build",
                            "--native",
                            "-O",
                            "0",
                            str(src),
                            "-o",
                            str(native),
                        ],
                        stdout=subprocess.DEVNULL,
                    )
                    aot = self.run_checked([str(native)]).stdout

                self.assertEqual(vm, aot, rel)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--xray", type=Path, default=ROOT / "build/xray")
    args, unittest_args = parser.parse_known_args()
    ParamContractAotTest.xray = args.xray.resolve()
    unittest.main(argv=[__file__, *unittest_args])


if __name__ == "__main__":
    main()

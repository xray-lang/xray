#!/usr/bin/env python3
"""Focused task-206 AOT ParamContract write-back gate."""

from __future__ import annotations

import argparse
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]

POSITIVE_AOT_CASES = (
    "tests/aot/basic/dynamic_function_value_ref_out_call_plan.xr",
)


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

    def test_dynamic_function_value_ref_out_writeback(self) -> None:
        for rel in POSITIVE_AOT_CASES:
            with self.subTest(rel=rel):
                src = ROOT / rel
                vm = self.run_checked([str(self.xray), str(src)]).stdout

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

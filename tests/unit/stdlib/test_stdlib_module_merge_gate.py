#!/usr/bin/env python3
"""Focused tests for the task-196 S0 module intake gate."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))

from check_stdlib_module_merge import (  # noqa: E402
    infer_modules,
    load_intake,
    owned_collisions,
    path_matches,
    validate_intake,
)


class StdlibModuleMergeGateTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.intake = load_intake(ROOT)

    def test_intake_manifest_has_unique_s1_s2_s3_lanes(self) -> None:
        self.assertEqual([], validate_intake(self.intake))
        self.assertEqual(
            [("S1", 198), ("S2", 199), ("S3", 200)],
            [(lane["slot"], lane["task"]) for lane in self.intake["lane"]],
        )

    def test_s0_owned_and_generated_paths_block_worker_edits(self) -> None:
        paths = [
            "stdlib/base64/base64.xr",
            "tests/stdlib/contracts/base64/contract.toml",
            "stdlib/stdlib_boundary.toml",
            "src/app/lsp/xlsp_stdlib_generated.inc",
            "tools/stdlibgen/stdlibgen.py",
        ]
        collisions = owned_collisions(self.intake, paths)
        self.assertEqual(
            [
                "stdlib/stdlib_boundary.toml",
                "src/app/lsp/xlsp_stdlib_generated.inc",
                "tools/stdlibgen/stdlibgen.py",
            ],
            [entry["path"] for entry in collisions],
        )

    def test_module_inference_uses_source_contract_and_benchmark_paths(self) -> None:
        self.assertEqual(
            ["base64", "encoding", "text"],
            infer_modules(
                [
                    "stdlib/base64/base64.xr",
                    "stdlib/defs/core.def",
                    "tests/stdlib/contracts/encoding/contract.toml",
                    "tests/benchmarks/stdlib/text/corpus.txt",
                ]
            ),
        )

    def test_recursive_patterns_cover_directory_root_and_children(self) -> None:
        self.assertTrue(path_matches("tools/stdlibgen/**", "tools/stdlibgen"))
        self.assertTrue(path_matches("tools/stdlibgen/**", "tools/stdlibgen/stdlibgen.py"))
        self.assertFalse(path_matches("tools/stdlibgen/**", "tools/other.py"))


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Focused unit coverage for task-200 public-native readiness blockers."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))

from check_binary_public_native_readiness import (  # noqa: E402
    NATIVE_TYPED_ERROR_ABI_BLOCKERS,
    check_dependency_markers,
    check_native_typed_error_abi_blockers,
)


class BinaryPublicNativeReadinessTest(unittest.TestCase):
    def test_typed_native_error_blockers_are_module_level(self) -> None:
        results = check_native_typed_error_abi_blockers(ROOT)
        by_subject = {result.subject: result for result in results}
        self.assertEqual(set(NATIVE_TYPED_ERROR_ABI_BLOCKERS), set(by_subject))
        for module in ("compress", "crypto", "io", "net"):
            with self.subTest(module=module):
                result = by_subject[module]
                self.assertTrue(result.ok, result.detail)
                self.assertEqual("NATIVE_TYPED_ERROR_ABI_BLOCKER", result.category)
                self.assertIn("typed native error ABI remains blocked", result.detail)

    def test_public_switch_markers_remain_absent(self) -> None:
        results = check_dependency_markers(ROOT)
        blockers = {
            result.subject: result
            for result in results
            if result.category == "PUBLIC_SWITCH_DEPENDENCY_BLOCKER"
        }
        self.assertEqual(
            {"TASK_197_SLICE_PROVENANCE_READY", "TASK_198_TYPED_NATIVE_ERRORS_READY"},
            set(blockers),
        )
        for marker, result in blockers.items():
            with self.subTest(marker=marker):
                self.assertTrue(result.ok, result.detail)
                self.assertIn("public-native switch remains blocked", result.detail)


if __name__ == "__main__":
    unittest.main()

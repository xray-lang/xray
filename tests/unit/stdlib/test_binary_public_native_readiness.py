#!/usr/bin/env python3
"""Focused fail-closed coverage for task-256's built-in stdlib cutover."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))

import check_binary_public_native_readiness as readiness  # noqa: E402


class BinaryPublicNativeReadinessTest(unittest.TestCase):
    def test_live_cutover_is_complete(self) -> None:
        failures = [item for item in readiness.build_results(ROOT) if not item.ok]
        self.assertEqual([], failures, "\n".join(f"{item.category}: {item.detail}" for item in failures))

    def test_retained_stdlib_set_is_exact(self) -> None:
        modules = readiness.load_boundary_modules(ROOT)
        self.assertTrue(readiness.RETAINED_STDLIB_MODULES <= set(modules))
        self.assertEqual(readiness.TERMINAL_STDLIB_MODULE_COUNT, len(modules))

        partial = dict(modules)
        partial.pop("http2")
        result = readiness.check_boundary(ROOT, stdlib_modules=partial)[0]
        self.assertFalse(result.ok)
        self.assertIn("http2", result.detail)

    def test_module_cannot_leave_stdlib(self) -> None:
        modules = readiness.load_boundary_modules(ROOT)
        mutated = dict(modules)
        mutated.pop("crypto")
        result = readiness.check_boundary(ROOT, stdlib_modules=mutated)[0]
        self.assertFalse(result.ok)
        self.assertIn("crypto", result.detail)

    def test_l2_public_native_surface_is_exact(self) -> None:
        modules = readiness.load_boundary_modules(ROOT)
        mutated = {name: dict(entry) for name, entry in modules.items()}
        mutated["net"]["public_native"] = [*mutated["net"]["public_native"], "dial"]
        result = next(
            item
            for item in readiness.check_boundary(ROOT, stdlib_modules=mutated)
            if item.category == "L2_THIN_PUBLIC_SURFACE" and item.subject == "net"
        )
        self.assertFalse(result.ok)
        self.assertIn("dial", result.detail)

    def test_contract_and_perf_ownership(self) -> None:
        self.assertTrue(all(item.ok for item in readiness.check_contracts(ROOT)))
        self.assertTrue(all(item.ok for item in readiness.check_perf_manifest(ROOT)))

    def test_api_import_and_physical_classification_is_canonical(self) -> None:
        self.assertTrue(all(item.ok for item in readiness.check_api_classification(ROOT)))
        self.assertTrue(all(item.ok for item in readiness.check_import_cutover(ROOT)))
        self.assertTrue(all(item.ok for item in readiness.check_physical_cutover(ROOT)))


if __name__ == "__main__":
    unittest.main()

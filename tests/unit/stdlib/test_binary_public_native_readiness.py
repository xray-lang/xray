#!/usr/bin/env python3
"""Fail-closed coverage for binary-stdlib and L2 native-provider readiness."""

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

    def test_native_provider_set_is_exact(self) -> None:
        modules = readiness.load_boundary_modules(ROOT)
        self.assertEqual({"crypto"}, readiness.NATIVE_PROVIDER_MODULES)
        self.assertTrue(readiness.NATIVE_PROVIDER_MODULES <= set(modules))

    def test_source_only_modules_are_not_native_readiness_subjects(self) -> None:
        source_only = {"cluster", "http2", "compress"}
        checks = (
            readiness.check_module_payloads(ROOT),
            readiness.check_contracts(ROOT),
            readiness.check_perf_manifest(ROOT),
            readiness.check_api_classification(ROOT),
        )
        for results in checks:
            self.assertEqual({"crypto"}, {item.subject for item in results})
        self.assertTrue(source_only.isdisjoint(readiness.NATIVE_PROVIDER_MODULES))
        without_source_only = {
            name: entry
            for name, entry in readiness.load_boundary_modules(ROOT).items()
            if name not in source_only
        }
        self.assertTrue(
            all(
                item.ok
                for item in readiness.check_boundary(
                    ROOT, stdlib_modules=without_source_only
                )
            )
        )
        self.assertFalse(
            any("compress" in path for path in readiness.ABI_EVIDENCE),
            "source-only compress evidence must not be classified as native-provider ABI",
        )

    def test_module_cannot_leave_stdlib(self) -> None:
        modules = readiness.load_boundary_modules(ROOT)
        mutated = dict(modules)
        mutated.pop("crypto")
        results = readiness.check_boundary(ROOT, stdlib_modules=mutated)
        provider_set = next(item for item in results if item.category == "NATIVE_PROVIDER_SET")
        self.assertFalse(provider_set.ok)
        self.assertIn("crypto", provider_set.detail)

    def test_l2_public_native_surface_is_exact(self) -> None:
        modules = readiness.load_boundary_modules(ROOT)
        self.assertEqual(set(), readiness.L2_PUBLIC_NATIVE["os"])
        self.assertEqual([], modules["os"]["public_native"])

        mutated = {name: dict(entry) for name, entry in modules.items()}
        mutated["os"]["public_native"] = ["platform"]
        result = next(
            item
            for item in readiness.check_boundary(ROOT, stdlib_modules=mutated)
            if item.category == "L2_THIN_PUBLIC_SURFACE" and item.subject == "os"
        )
        self.assertFalse(result.ok)
        self.assertIn("platform", result.detail)

    def test_contract_and_perf_ownership(self) -> None:
        self.assertTrue(all(item.ok for item in readiness.check_contracts(ROOT)))
        self.assertTrue(all(item.ok for item in readiness.check_perf_manifest(ROOT)))

    def test_api_classification_is_canonical(self) -> None:
        self.assertTrue(all(item.ok for item in readiness.check_api_classification(ROOT)))


if __name__ == "__main__":
    unittest.main()

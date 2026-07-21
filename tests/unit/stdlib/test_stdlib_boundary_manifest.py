#!/usr/bin/env python3
"""Focused unit coverage for the task-196 stdlib boundary manifest."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))

from check_stdlib_boundary import (  # noqa: E402
    check_dynamic,
    check_error_model_policy,
    check_fastpaths,
    check_manifest,
    check_semantic_owners,
)
from stdlib_manifest import load_manifest, registry_modules  # noqa: E402
from report_stdlib_self_hosting import build_report  # noqa: E402
from check_stdlib_aot_helper_residue import check_forbidden_tokens  # noqa: E402


class StdlibBoundaryManifestTest(unittest.TestCase):
    def test_registered_modules_have_one_boundary_entry(self) -> None:
        manifest = load_manifest(ROOT)
        self.assertEqual(set(registry_modules(ROOT)), set(manifest.by_name))
        self.assertEqual([], check_manifest(ROOT))

    def test_semantic_native_and_fastpath_contracts_are_source_derived(self) -> None:
        self.assertEqual([], check_semantic_owners(ROOT))
        self.assertEqual([], check_fastpaths(ROOT))

    def test_exact_aot_runtime_adapters_do_not_allow_module_helper_families(self) -> None:
        manifest = load_manifest(ROOT)
        allowed = {
            name: set(module.get("aot_runtime_adapters", ()))
            for name, module in manifest.by_name.items()
            if module.get("aot_runtime_adapters")
        }
        self.assertEqual(
            [],
            check_forbidden_tokens(ROOT, manifest.aot_helper_forbidden_modules, allowed),
        )

    def test_superseded_global_result_cannot_reenter_prelude(self) -> None:
        self.assertEqual([], check_error_model_policy(ROOT))

    def test_dynamic_surface_is_fully_classified(self) -> None:
        errors, report = check_dynamic(ROOT)
        self.assertEqual([], errors)
        self.assertEqual(report["allowed_count"], len(report["allowlist"]))
        for entry in report["allowlist"]:
            self.assertEqual(
                {"symbol", "direction", "domain", "reason", "owner", "review_task"},
                set(entry),
            )
        self.assertEqual(0, report["migration_debt_count"])

    def test_consistency_is_not_reported_as_completion(self) -> None:
        errors, report = build_report(ROOT)
        self.assertEqual([], errors)
        self.assertTrue(report["status"]["consistent"])
        self.assertFalse(report["status"]["complete"])
        kinds = {item["kind"] for item in report["status"]["completion_blockers"]}
        # Source consistency must never be reported as completion while blockers
        # remain. Task 221 cleared the benchmark and dynamic-migration blockers;
        # correctness coverage and legacy-oracle execution still gate completion.
        self.assertTrue(kinds, "consistency must not imply completion while blockers remain")
        self.assertNotIn("dynamic_migration_debt", kinds)
        self.assertIn("missing_correctness_contracts", kinds)
        self.assertIn("non_executable_legacy_oracles", kinds)


if __name__ == "__main__":
    unittest.main()

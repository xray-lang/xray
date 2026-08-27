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
from stdlib_manifest import (  # noqa: E402
    load_manifest,
    loadable_modules,
    registry_modules,
    source_modules,
)
from report_stdlib_self_hosting import build_report  # noqa: E402
from check_stdlib_aot_helper_residue import check_forbidden_tokens  # noqa: E402


class StdlibBoundaryManifestTest(unittest.TestCase):
    def test_loadable_modules_have_one_boundary_entry(self) -> None:
        manifest = load_manifest(ROOT)
        self.assertEqual(loadable_modules(ROOT), set(manifest.by_name))
        self.assertEqual([], check_manifest(ROOT))

    def test_source_only_module_needs_no_native_factory(self) -> None:
        manifest = load_manifest(ROOT)
        csv = manifest.by_name["csv"]
        self.assertIn("csv", source_modules(ROOT))
        self.assertNotIn("csv", registry_modules(ROOT))
        self.assertNotIn("factory_source", csv)
        self.assertEqual([], csv.get("public_native", []))

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

    def test_completion_is_reported_after_all_blockers_clear(self) -> None:
        errors, report = build_report(ROOT)
        self.assertEqual([], errors)
        self.assertTrue(report["status"]["consistent"])
        self.assertTrue(report["status"]["complete"])
        self.assertEqual([], report["status"]["completion_blockers"])


if __name__ == "__main__":
    unittest.main()

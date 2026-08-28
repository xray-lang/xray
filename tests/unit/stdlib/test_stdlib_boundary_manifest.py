#!/usr/bin/env python3
"""Focused unit coverage for the task-196 stdlib boundary manifest."""

from __future__ import annotations

import copy
import dataclasses
import sys
import unittest
from pathlib import Path
from unittest import mock


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
    BoundaryManifest,
    load_manifest,
    loadable_modules,
    load_stdlibgen,
    private_leaf_binder_modules,
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
        for name in ("base64", "csv"):
            module = manifest.by_name[name]
            self.assertIn(name, source_modules(ROOT))
            self.assertNotIn(name, registry_modules(ROOT))
            self.assertNotIn(name, private_leaf_binder_modules(ROOT))
            self.assertNotIn("factory_source", module)
            self.assertEqual([], module.get("public_native", []))

    def test_source_module_private_leaves_use_generic_binder(self) -> None:
        manifest = load_manifest(ROOT)
        os_module = manifest.by_name["os"]
        binders = private_leaf_binder_modules(ROOT)
        self.assertEqual({"io", "net", "os"}, set(binders))
        self.assertIn("os", source_modules(ROOT))
        self.assertNotIn("os", registry_modules(ROOT))
        self.assertEqual(
            "xr_stdlib_vm_bind_os_generated",
            binders.get("os"),
        )
        self.assertNotIn("factory_source", os_module)
        self.assertTrue(os_module.get("private_native_sources"))

    def test_generic_private_leaf_module_rejects_stale_factory_source(self) -> None:
        manifest = load_manifest(ROOT)
        raw = copy.deepcopy(manifest.raw)
        os_module = next(module for module in raw["module"] if module["name"] == "os")
        os_module["factory_source"] = "stdlib/os/os.c"
        mutated = BoundaryManifest(
            root=ROOT,
            raw=raw,
            modules=tuple(raw["module"]),
            vm_fastpaths=tuple(raw.get("vm_fastpath", ())),
        )
        with mock.patch("check_stdlib_boundary.load_manifest", return_value=mutated):
            errors = check_manifest(ROOT)
        self.assertIn(
            "module os: module without native factory must not declare factory_source",
            errors,
        )

    def test_duplicate_private_leaf_provider_identity_fails_generation(self) -> None:
        stdlibgen = load_stdlibgen(ROOT)
        entries, *_ = stdlibgen.parse_def_metadata(ROOT)
        original = next(entry for entry in entries if entry.vm)
        divergent = dataclasses.replace(original, vm=f"{original.vm}_divergent")
        with self.assertRaisesRegex(SystemExit, "duplicate VM binding rows disagree"):
            stdlibgen.unique_vm_binding_entries([original, divergent])

    def test_target_leaf_source_owner_is_private_unique_and_canonical(self) -> None:
        stdlibgen = load_stdlibgen(ROOT)
        owners: dict[str, str] = {}
        stdlibgen.validate_target_leaf_source_owner(
            ROOT, "os", "__getpid", "i64-getpid", "internal", "nothrow", "no_heap", owners
        )
        self.assertEqual({"i64-getpid": "os.__getpid"}, owners)
        with self.assertRaisesRegex(SystemExit, "must have internal visibility"):
            stdlibgen.validate_target_leaf_source_owner(
                ROOT, "os", "publicLeaf", "i64-public", "public", "nothrow", "no_heap", {}
            )
        with self.assertRaisesRegex(SystemExit, "must declare effect = nothrow"):
            stdlibgen.validate_target_leaf_source_owner(
                ROOT, "os", "__missingEffect", "i64-missing-effect", "internal", "", "no_heap", {}
            )
        with self.assertRaisesRegex(SystemExit, "must declare allocation = no_heap"):
            stdlibgen.validate_target_leaf_source_owner(
                ROOT, "os", "__missingAllocation", "i64-missing-allocation", "internal", "nothrow", "", {}
            )
        with self.assertRaisesRegex(SystemExit, "duplicate providers"):
            stdlibgen.validate_target_leaf_source_owner(
                ROOT, "os", "anotherPrivateLeaf", "i64-getpid", "internal", "nothrow", "no_heap", owners
            )
        with self.assertRaisesRegex(SystemExit, "requires canonical source module"):
            stdlibgen.validate_target_leaf_source_owner(
                ROOT, "missing_source_module", "__leaf", "i64-missing", "internal", "nothrow", "no_heap", {}
            )

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

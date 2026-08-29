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
    native_entry_binder_modules,
    source_modules,
)
from report_stdlib_self_hosting import build_report  # noqa: E402
from check_stdlib_aot_helper_residue import check_forbidden_tokens  # noqa: E402


class StdlibBoundaryManifestTest(unittest.TestCase):
    def test_loadable_modules_have_one_boundary_entry(self) -> None:
        manifest = load_manifest(ROOT)
        self.assertEqual(loadable_modules(ROOT), set(manifest.by_name))
        self.assertEqual([], check_manifest(ROOT))

    def test_source_only_module_declares_no_native_entries(self) -> None:
        manifest = load_manifest(ROOT)
        for name in ("base64", "csv"):
            module = manifest.by_name[name]
            self.assertIn(name, source_modules(ROOT))
            self.assertNotIn(name, native_entry_binder_modules(ROOT))
            self.assertEqual([], module.get("public_native", []))

    def test_module_native_entries_are_bound_by_a_generated_binder(self) -> None:
        manifest = load_manifest(ROOT)
        os_module = manifest.by_name["os"]
        binders = native_entry_binder_modules(ROOT)
        # Every module with generated native entries belongs here. Keep the
        # complete set explicit so a newly generated binder cannot silently
        # bypass the generic loader's boundary inventory.
        self.assertEqual(
            {
                "cluster",
                "compress",
                "crypto",
                "http2",
                "io",
                "math",
                "mem",
                "net",
                "os",
                "regex",
                "runtime",
                "sync",
                "sys",
                "test_yield",
                "time",
            },
            set(binders),
        )
        self.assertIn("os", source_modules(ROOT))
        self.assertEqual(
            "xr_stdlib_vm_bind_os_generated",
            binders.get("os"),
        )
        self.assertTrue(os_module.get("private_native_sources"))

    def test_no_module_is_loaded_by_a_c_factory(self) -> None:
        """The loader is generic: no module may have a C factory written for it."""
        loader = (ROOT / "src/module/xmodule.c").read_text(encoding="utf-8")
        self.assertNotIn("xr_native_module_create_", loader)
        self.assertFalse(list(ROOT.glob("stdlib/*/*.c*")) and [
            path
            for path in ROOT.glob("stdlib/**/*.c")
            if "xr_native_module_create_" in path.read_text(encoding="utf-8")
        ])
        for module in load_manifest(ROOT).modules:
            self.assertNotIn("factory_source", module)
            self.assertNotIn("factory_symbol", module)

    def test_loader_declarations_must_name_something_real(self) -> None:
        manifest = load_manifest(ROOT)
        raw = copy.deepcopy(manifest.raw)
        sync_module = next(module for module in raw["module"] if module["name"] == "sync")
        sync_module["native_type_exports"] = [
            {"name": "Semaphore", "builtin_type": "XR_TNOSUCHTYPE"}
        ]
        mutated = BoundaryManifest(
            root=ROOT,
            raw=raw,
            modules=tuple(raw["module"]),
            vm_fastpaths=tuple(raw.get("vm_fastpath", ())),
        )
        with mock.patch("check_stdlib_boundary.load_manifest", return_value=mutated):
            errors = check_manifest(ROOT)
        self.assertIn(
            "module sync: native_type_exports 'Semaphore' names an unknown builtin type "
            "XR_TNOSUCHTYPE",
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

    def test_policy_agreement_is_reported_after_all_blockers_clear(self) -> None:
        errors, report = build_report(ROOT)
        self.assertEqual([], errors)
        self.assertTrue(report["status"]["consistent"])
        self.assertTrue(report["status"]["policy_agreement"])
        self.assertEqual([], report["status"]["policy_agreement_blockers"])


if __name__ == "__main__":
    unittest.main()

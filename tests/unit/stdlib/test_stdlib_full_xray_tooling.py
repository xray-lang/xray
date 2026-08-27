#!/usr/bin/env python3
"""Focused tests for source-derived stdlib ownership and plan evidence."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from dataclasses import asdict, replace
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))

import check_stdlib_full_xray_completion as completion  # noqa: E402
import probe_stdlib_backends as probe  # noqa: E402
import stdlib_symbol_inventory as inventory  # noqa: E402


COMMIT = "a" * 40


def valid_authority() -> dict[str, object]:
    return {
        "schemas": dict(completion.CURRENT_PLAN_SCHEMAS),
        "schema_errors": [],
        "source": {"commit": COMMIT, "tree": "b" * 40, "dirty": False},
        "compiler": {
            "schema": 1,
            "commit": COMMIT,
            "dirty": False,
            "features": ["vm", "aot"],
        },
    }


def valid_plan() -> completion.PlanIdentity:
    identity, errors = completion.PlanIdentity.parse(
        {
            "psc_schema": 8,
            "semantic_schema": 44,
            "target_schema": 54,
            "xtp_schema": 54,
            "module_count": 3,
            "dependency_count": 2,
            "program_fingerprint": "1" * 64,
            "module_set_fingerprint": "2" * 64,
            "generation_closure_id": "3" * 32,
            "semantic_fingerprint": "4" * 64,
            "target_fingerprint": "5" * 64,
            "xtp_sha256": "6" * 64,
            "xtp_verified": True,
        }
    )
    if identity is None or errors:
        raise AssertionError(errors)
    return identity


class SchemaAndPlanEvidenceTest(unittest.TestCase):
    def test_live_source_uses_current_authority_schemas(self) -> None:
        schemas, errors = probe.source_schema_authority(ROOT)
        self.assertEqual([], errors)
        self.assertEqual(
            (8, 44, 54, 54),
            (
                schemas.psc_schema,
                schemas.semantic_schema,
                schemas.target_schema,
                schemas.xtp_schema,
            ),
        )

    def test_aot_observation_does_not_invent_an_xtp(self) -> None:
        target = "5" * 64
        semantic = "4" * 64
        stdout = "\n".join(
            [
                f"[module-summary] modules=3 graph={'2' * 64} xsm-hits=0",
                (
                    f"[program-closure] modules=3 dependencies=2 "
                    f"psc={'1' * 64} gci={'3' * 32}"
                ),
                *(
                    f"target-plan module={index} schema=54 fingerprint={target} "
                    f"semantic={semantic} profile={'7' * 64}"
                    for index in range(3)
                ),
            ]
        )
        observation = probe.parse_aot_plan_observation(
            stdout, probe.SchemaAuthority(8, 44, 54, 54)
        )
        self.assertEqual(3, observation.module_count)
        self.assertEqual(2, observation.dependency_count)
        self.assertEqual(target, observation.target_fingerprint)
        self.assertFalse(observation.xtp_verified)
        self.assertFalse(observation.complete)
        self.assertTrue(any("no exact verified XTP" in item for item in observation.errors))

    def test_both_backends_accepting_without_identity_is_unrun(self) -> None:
        verdict, reason = completion.ProbeModuleEvidence(
            name="time", vm_ok=True, aot_ok=True
        ).plan_verdict()
        self.assertIsNone(verdict)
        self.assertIn("no complete verified plan identity", reason)

    def test_exact_identity_tuple_can_pass(self) -> None:
        identity = valid_plan()
        verdict, _ = completion.ProbeModuleEvidence(
            name="csv", vm_ok=True, aot_ok=True, vm_plan=identity, aot_plan=identity
        ).plan_verdict()
        self.assertTrue(verdict)

    def test_gci_or_xtp_difference_fails(self) -> None:
        identity = valid_plan()
        different = replace(identity, generation_closure_id="7" * 32)
        verdict, reason = completion.ProbeModuleEvidence(
            name="csv", vm_ok=True, aot_ok=True, vm_plan=identity, aot_plan=different
        ).plan_verdict()
        self.assertFalse(verdict)
        self.assertIn("tuples differ", reason)

    def test_completion_gate_keeps_missing_identity_unrun(self) -> None:
        module = inventory.ModuleRow(
            name="time",
            layer="L1",
            policy="xray_semantic",
            audience="production",
            semantic_source="stdlib/time/time.xr",
            factory_source="",
        )
        evidence = completion.ProbeEvidence(
            available=True,
            source="fixture",
            modules={
                "time": completion.ProbeModuleEvidence(
                    name="time", vm_ok=True, aot_ok=True
                )
            },
        )
        result = completion.gate_unified_target_plan_coverage([module], evidence)
        self.assertEqual(completion.UNRUN, result.status)

    def test_completion_gate_rejects_different_identity(self) -> None:
        module = inventory.ModuleRow(
            name="csv",
            layer="L4",
            policy="xray_semantic",
            audience="production",
            semantic_source="stdlib/csv/csv.xr",
            factory_source="",
        )
        identity = valid_plan()
        evidence = completion.ProbeEvidence(
            available=True,
            source="fixture",
            modules={
                "csv": completion.ProbeModuleEvidence(
                    name="csv",
                    vm_ok=True,
                    aot_ok=True,
                    vm_plan=identity,
                    aot_plan=replace(identity, xtp_sha256="7" * 64),
                )
            },
        )
        result = completion.gate_unified_target_plan_coverage([module], evidence)
        self.assertEqual(completion.FAIL, result.status)

    def test_old_or_unverified_plan_identity_is_invalid(self) -> None:
        payload = asdict(valid_plan())
        payload["psc_schema"] = 7
        payload["xtp_verified"] = False
        _identity, errors = completion.PlanIdentity.parse(payload)
        self.assertTrue(any("expected 8" in item for item in errors))
        self.assertTrue(any("not independently verified" in item for item in errors))


class StrictProbeReaderTest(unittest.TestCase):
    def load(self, payload: dict[str, object]) -> completion.ProbeEvidence:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        path = Path(temporary.name) / "probe.json"
        path.write_text(json.dumps(payload), encoding="utf-8")
        return completion.load_probe_evidence(str(path), COMMIT)

    def test_legacy_unified_flag_cannot_create_a_pass(self) -> None:
        evidence = self.load(
            {
                "schema": 2,
                "authority": valid_authority(),
                "modules": [
                    {
                        "module": "time",
                        "vm": {"returncode": 0, "timed_out": False},
                        "aot": {"returncode": 0, "timed_out": False},
                        "plan_unified": True,
                        "vm_plan": None,
                        "aot_plan": None,
                    }
                ],
            }
        )
        self.assertTrue(evidence.available)
        verdict, _ = evidence.modules["time"].plan_verdict()
        self.assertIsNone(verdict)

    def test_current_complete_plan_identity_survives_strict_reader(self) -> None:
        identity = asdict(valid_plan())
        evidence = self.load(
            {
                "schema": 2,
                "authority": valid_authority(),
                "modules": [
                    {
                        "module": "csv",
                        "vm": {"returncode": 0, "timed_out": False},
                        "aot": {"returncode": 0, "timed_out": False},
                        "vm_plan": identity,
                        "aot_plan": identity,
                    }
                ],
            }
        )
        self.assertTrue(evidence.available)
        verdict, _ = evidence.modules["csv"].plan_verdict()
        self.assertTrue(verdict)

    def test_old_probe_schema_is_unavailable(self) -> None:
        evidence = self.load(
            {"schema": 1, "authority": valid_authority(), "modules": [{"module": "time"}]}
        )
        self.assertFalse(evidence.available)
        self.assertIn("not current schema 2", evidence.error)

    def test_mismatched_compiler_commit_is_unavailable(self) -> None:
        authority = valid_authority()
        compiler = dict(authority["compiler"])
        compiler["commit"] = "c" * 40
        authority["compiler"] = compiler
        evidence = self.load(
            {"schema": 2, "authority": authority, "modules": [{"module": "time"}]}
        )
        self.assertFalse(evidence.available)
        self.assertIn("source and compiler commits differ", evidence.error)


class InventoryTraceTest(unittest.TestCase):
    def test_sync_manual_native_classes_are_traceable(self) -> None:
        traced = inventory.manual_native_class_exports(ROOT, "stdlib/sync/sync.c")
        self.assertEqual(
            {"CountdownLatch", "EventCount", "ResultGroup", "Semaphore", "WorkQueue"},
            set(traced),
        )
        modules, rows, defects = inventory.build_rows(ROOT)
        self.assertFalse(any("sync." in item for item in defects))
        sync_rows = {
            row.symbol: row
            for row in rows
            if row.module == "sync" and row.kind == "manual-native-class"
        }
        self.assertEqual(set(traced), set(sync_rows))
        self.assertTrue(
            all(
                row.semantic_source == "stdlib/sync/sync.c"
                for row in sync_rows.values()
            )
        )
        self.assertTrue(any(module.name == "sync" for module in modules))

    def test_comments_and_bare_strings_are_not_exports(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        source = root / "stdlib" / "fake" / "fake.c"
        source.parent.mkdir(parents=True)
        source.write_text(
            '// fake_export_native_class(vm, module, "Commented", TYPE);\n'
            'const char *name = "BareString";\n',
            encoding="utf-8",
        )
        self.assertEqual(
            {}, inventory.manual_native_class_exports(root, "stdlib/fake/fake.c")
        )


class StableReportTest(unittest.TestCase):
    def test_machine_paths_are_redacted(self) -> None:
        workspace = Path("C:/temp/probe")
        root = Path("C:/repo")
        xray = root / "build" / "xray.exe"
        payload = {
            "argv": [str(xray), str(root / "file.xr")],
            "scratch": str(workspace / "module.c"),
        }
        redacted = probe.redact_paths(payload, workspace, root, xray)
        encoded = json.dumps(redacted)
        self.assertNotIn(str(root), encoded)
        self.assertNotIn(str(workspace), encoded)
        self.assertIn("<xray-binary>", encoded)
        self.assertIn("<probe-workspace>", encoded)


if __name__ == "__main__":
    unittest.main()

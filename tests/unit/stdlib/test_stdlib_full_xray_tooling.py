#!/usr/bin/env python3
"""Focused tests for source-derived stdlib ownership and plan evidence."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from dataclasses import asdict, replace
from pathlib import Path
from types import SimpleNamespace


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
            "psc_schema": 9,
            "semantic_schema": 45,
            "target_schema": 55,
            "xtp_schema": 55,
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
            (9, 45, 55, 55),
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
                    f"target-plan module={index} schema=55 fingerprint={target} "
                    f"semantic={semantic} profile={'7' * 64}"
                    for index in range(3)
                ),
            ]
        )
        observation = probe.parse_aot_plan_observation(
            stdout, probe.SchemaAuthority(9, 45, 55, 55)
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
        self.assertTrue(any("expected 9" in item for item in errors))
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
        root = Path(temporary.name).resolve()
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


GOOD_LEAF_RECORD = """schema = 1

[[leaf]]
module = "os"
symbol = "__getpid"
class = "host_abi_leaf"
abi = "getpid(2)"
ownership = "none"
effect = "nothrow"
provider = "the kernel via src/shared/xr_os_core.h"
deletion_trigger = "when Xray source can issue the getpid(2) call itself"
"""


def def_entry(
    return_ownership: str = "", effect: str = "", vm_binding: str = "normal"
) -> SimpleNamespace:
    """The three `.def` fields the allowlist has to agree with."""
    return SimpleNamespace(
        return_ownership=return_ownership, effect=effect, vm_binding=vm_binding
    )


class NativeLeafAllowlistTest(unittest.TestCase):
    """The three fail-closed directions of the native-leaf allowlist."""

    def load(self, body: str) -> tuple[dict, list[str]]:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        (root / "stdlib").mkdir()
        (root / inventory.NATIVE_LEAF_ALLOWLIST_PATH).write_text(body, encoding="utf-8")
        return inventory.load_leaf_allowlist(root)

    def test_a_well_formed_record_approves_its_leaf(self) -> None:
        records, defects = self.load(GOOD_LEAF_RECORD)
        self.assertEqual([], defects)
        leaf_class, _reason, row_defects = inventory.classify_leaf(
            "os", "__getpid", def_entry(), records
        )
        self.assertEqual("host_abi_leaf", leaf_class)
        self.assertEqual([], row_defects)

    def test_missing_allowlist_file_is_a_defect_not_an_empty_allowlist(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        records, defects = inventory.load_leaf_allowlist(Path(temporary.name))
        self.assertEqual({}, records)
        self.assertTrue(any("is missing" in item for item in defects))

    # Direction 1: a record for a leaf the repository no longer declares.
    def test_a_record_no_leaf_reached_is_reported_as_stale(self) -> None:
        defects = inventory.stale_leaf_record_defects(
            {("os", "__gone"): None, ("os", "__getpid"): None},
            {("os", "__getpid")},
        )
        self.assertEqual(1, len(defects))
        self.assertIn("os::__gone", defects[0])
        self.assertIn("no declared native leaf reached", defects[0])

    # Direction 2: a leaf the allowlist does not name stays unclassified.
    def test_a_leaf_with_no_record_stays_unclassified(self) -> None:
        leaf_class, reason, defects = inventory.classify_leaf(
            "os", "__nosuch", def_entry(), {}
        )
        self.assertEqual("unclassified", leaf_class)
        self.assertIn("no record in", reason)
        # Unfinished migration work is the gate's subject, not a broken input.
        self.assertEqual([], defects)

    # Direction 3a: a class outside the closed set approves nothing.
    def test_a_class_outside_the_closed_set_is_rejected(self) -> None:
        records, defects = self.load(
            GOOD_LEAF_RECORD.replace("host_abi_leaf", "host_api_leaf")
        )
        self.assertTrue(any("is not one of" in item for item in defects))
        leaf_class, _reason, _defects = inventory.classify_leaf(
            "os", "__getpid", def_entry(), records
        )
        self.assertEqual("unclassified", leaf_class)

    def test_an_unapproved_class_never_counts_as_approved(self) -> None:
        row = inventory.SymbolRow(
            module="os",
            symbol="__getpid",
            kind="function",
            audience="production",
            semantic_source="stdlib/defs/core.def",
            xray_body=False,
            handwritten_c_body="stdlib/os/os.c",
            generated_c_only=False,
            native_leaf=True,
            leaf_class="host_api_leaf",
            leaf_reason="",
            factory_loader="",
            plan_coverage="native_binding",
            vm_binding="os_getpid",
            aot_binding="xrt_os_getpid",
            covered_c_deletion="",
            blocker="",
        )
        # Neither "" nor "unclassified", but still not a class anyone defined.
        self.assertFalse(inventory.leaf_is_approved(row))
        gate = completion.gate_native_leaf_allowlist([row], {})
        self.assertEqual(completion.FAIL, gate.status)

    # Direction 3b: a record that contradicts its own `.def` entry.
    def test_ownership_contradicting_the_def_entry_is_a_defect(self) -> None:
        records, _ = self.load(GOOD_LEAF_RECORD)
        leaf_class, reason, defects = inventory.classify_leaf(
            "os", "__getpid", def_entry(return_ownership="fresh"), records
        )
        self.assertEqual("unclassified", leaf_class)
        self.assertIn("contradicts the .def return_ownership", reason)
        self.assertEqual(1, len(defects))

    def test_a_yieldable_leaf_must_declare_that_it_suspends(self) -> None:
        records, _ = self.load(GOOD_LEAF_RECORD)
        leaf_class, reason, _defects = inventory.classify_leaf(
            "os", "__getpid", def_entry(vm_binding="yieldable"), records
        )
        self.assertEqual("unclassified", leaf_class)
        self.assertIn("omits 'suspends'", reason)

    def test_dropping_a_declared_effect_is_a_defect(self) -> None:
        records, _ = self.load(GOOD_LEAF_RECORD)
        leaf_class, reason, _defects = inventory.classify_leaf(
            "os", "__getpid", def_entry(effect="NetError.Io"), records
        )
        self.assertEqual("unclassified", leaf_class)
        self.assertIn("drops the .def-declared effect", reason)

    def test_a_misspelled_key_does_not_silently_empty_a_column(self) -> None:
        records, defects = self.load(
            GOOD_LEAF_RECORD.replace("deletion_trigger =", "deletion_triger =")
        )
        self.assertTrue(any("unknown key" in item for item in defects))
        self.assertTrue(any("deletion_trigger is empty" in item for item in defects))
        self.assertFalse(records[("os", "__getpid")].valid)

    def test_duplicate_records_approve_nothing(self) -> None:
        body = GOOD_LEAF_RECORD + GOOD_LEAF_RECORD.split("schema = 1\n", 1)[1]
        records, defects = self.load(body)
        self.assertTrue(any("more than one" in item for item in defects))
        self.assertFalse(records[("os", "__getpid")].valid)

    def test_effect_vocabulary_is_closed(self) -> None:
        records, defects = self.load(
            GOOD_LEAF_RECORD.replace('effect = "nothrow"', 'effect = "blocking"')
        )
        self.assertTrue(any("effect token" in item for item in defects))
        self.assertFalse(records[("os", "__getpid")].valid)

    def test_nothrow_cannot_be_combined(self) -> None:
        _records, defects = self.load(
            GOOD_LEAF_RECORD.replace('effect = "nothrow"', 'effect = "nothrow, suspends"')
        )
        self.assertTrue(any("exclusive" in item for item in defects))

    def test_every_repository_leaf_is_approved(self) -> None:
        _modules, rows, _defects = inventory.build_rows(ROOT)
        leaves = [row for row in rows if row.native_leaf]
        self.assertTrue(leaves)
        unapproved = [
            f"{row.module}::{row.symbol}"
            for row in leaves
            if not inventory.leaf_is_approved(row)
        ]
        self.assertEqual([], unapproved)

    def test_the_repository_allowlist_is_well_formed(self) -> None:
        """The checked-in allowlist parses and every record is usable.

        Stale records are deliberately not asserted on here. The allowlist can
        legitimately run ahead of the repository while a module's leaves are
        being renamed in a parallel branch, and a record with no leaf yet is
        reported by `--check` rather than by this test. What must always hold
        is that every record present is well-formed: the stale case is covered
        by `test_a_record_no_leaf_reached_is_reported_as_stale`.
        """
        records, defects = inventory.load_leaf_allowlist(ROOT)
        self.assertTrue(records)
        self.assertEqual([], defects)
        self.assertTrue(all(record.valid for record in records.values()))


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

import copy
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from _support import load_module


ROOT = Path(__file__).resolve().parents[3]
gate = load_module(
    "h2_backend_differential_under_test",
    ROOT / "scripts/check_xr_program_h2_backend_differential.py",
)


class H2BackendDifferentialTests(unittest.TestCase):
    def manifest(self):
        return gate.load_json(gate.MANIFEST)

    def registry(self):
        return gate.load_json(gate.REGISTRY)

    @staticmethod
    def record(identifier, route):
        return {
            "schema": 1,
            "executor": identifier,
            "route": route,
            "oracle": gate.EXPECTED_ORACLE,
        }

    @staticmethod
    def encoded(record):
        return (json.dumps(record, separators=(",", ":")) + "\n").encode("utf-8")

    def test_repository_manifest_is_honest_pending_and_reference_is_real(self):
        document = gate.validate_manifest(self.manifest(), self.registry())
        self.assertEqual(
            [executor["id"] for executor in document["executors"]],
            list(gate.EXECUTOR_IDS),
        )
        self.assertEqual(
            {executor["state"] for executor in document["executors"]},
            {"expected-unsupported"},
        )
        self.assertEqual(gate.check(), (0, 4))

    def test_operation_and_oracle_drift_fail_closed(self):
        for mutate, message in (
            (lambda data: data["operations"].pop(), "operation inventory"),
            (lambda data: data["operations"][0].update(stable_id=999), "operation inventory"),
            (lambda data: data["oracle"]["value"].update(data=41), "oracle drifted"),
            (lambda data: data["required_observables"].remove("drop-events"), "observable"),
        ):
            with self.subTest(message=message):
                document = self.manifest()
                mutate(document)
                with self.assertRaisesRegex(gate.GateError, message):
                    gate.validate_manifest(document, self.registry())

    def test_registry_identity_and_coverage_cannot_be_self_reported_by_manifest(self):
        registry = self.registry()
        operation = next(item for item in registry["operations"]
                         if item["stable_id"] == 142)
        operation["spelling"] = "core.class.compat"
        with self.assertRaisesRegex(gate.GateError, "identity mismatch"):
            gate.validate_manifest(self.manifest(), registry)

        registry = self.registry()
        operation = next(item for item in registry["operations"]
                         if item["stable_id"] == 142)
        operation["coverage"]["vm"]["status"] = "COMPLETE"
        with self.assertRaisesRegex(gate.GateError, "disagrees with CoreSpec"):
            gate.validate_manifest(self.manifest(), registry)

    def test_pending_witness_cannot_be_missing_or_weakened(self):
        for mutate, message in (
            (lambda executor: executor.update(pending_witness=None), "lacks exact rejection"),
            (lambda executor: executor["pending_witness"]["ordered_tokens"].pop(),
             "may not weaken"),
            (lambda executor: executor["pending_witness"].update(function="not_a_test"),
             "lacks function"),
        ):
            with self.subTest(message=message):
                document = self.manifest()
                mutate(document["executors"][0])
                with self.assertRaisesRegex(gate.GateError, message):
                    gate.validate_manifest(document, self.registry())

    def test_reference_evidence_tokens_are_a_non_weakenable_minimum(self):
        document = self.manifest()
        document["reference_evidence"]["ordered_tokens"].remove(
            "XR_REFERENCE_EVENT_CLASS_RECLAIM"
        )
        with self.assertRaisesRegex(gate.GateError, "may not weaken"):
            gate.validate_manifest(document, self.registry())

        document = self.manifest()
        document["reference_program_evidence"]["ordered_tokens"].remove(
            ".value.i64 = 7"
        )
        with self.assertRaisesRegex(gate.GateError, "may not weaken"):
            gate.validate_manifest(document, self.registry())

    def test_qualification_is_unique_and_keeps_all_existing_owners(self):
        self.assertEqual(
            gate.qualification_ctest_names(),
            tuple(self.manifest()["qualification"]["ctests"]),
        )
        self.assertEqual(
            gate.qualification_build_targets(),
            tuple(self.manifest()["qualification"]["targets"]),
        )
        document = self.manifest()
        document["qualification"]["ctests"].remove("test_xr_program_vm_runtime")
        with self.assertRaisesRegex(gate.GateError, "omits a required existing CTest"):
            gate.validate_manifest(document, self.registry())
        document = self.manifest()
        document["qualification"]["targets"].append("test_xr_program_aot")
        with self.assertRaisesRegex(gate.GateError, "duplicates"):
            gate.validate_manifest(document, self.registry())

    def test_probe_record_requires_exact_schema_route_and_oracle(self):
        identifier = "vm-baseline"
        route = "vm-baseline-view"
        record = self.record(identifier, route)
        self.assertEqual(gate.parse_probe_record(self.encoded(record), identifier, route), record)
        mutations = (
            lambda value: value.pop("oracle"),
            lambda value: value.update(extra=True),
            lambda value: value.update(executor="vm-fixed"),
            lambda value: value["oracle"]["events"].pop(),
        )
        for mutate in mutations:
            with self.subTest(mutate=mutate):
                bad = copy.deepcopy(record)
                mutate(bad)
                with self.assertRaises(gate.GateError):
                    gate.parse_probe_record(self.encoded(bad), identifier, route)

    def test_active_command_and_route_cannot_drift(self):
        document = self.manifest()
        document["executors"][0]["active_command"]["arguments"][-1] = "fixed"
        with self.assertRaisesRegex(gate.GateError, "command/provenance drifted"):
            gate.validate_manifest(document, self.registry())

    def test_false_extra_output_duplicate_key_and_non_utf8_fail(self):
        inputs = (
            b"false\n",
            self.encoded(self.record("vm-baseline", "vm-baseline-view")) + b"false\n",
            b'{"schema":1,"schema":1,"executor":"vm-baseline",'
            b'"route":"vm-baseline-view","oracle":{}}\n',
            b"\xff\n",
        )
        for stdout in inputs:
            with self.subTest(stdout=stdout), self.assertRaises(gate.GateError):
                gate.parse_probe_record(stdout, "vm-baseline", "vm-baseline-view")

    def test_active_mode_executes_every_backend_route_exactly_once(self):
        document = self.manifest()
        for executor in document["executors"]:
            executor["state"] = "execute"
        with tempfile.TemporaryDirectory(prefix="xr-h2-differential-") as directory:
            root = Path(directory)
            binaries = {name: root / name for name in ("vm", "aot")}
            for binary in binaries.values():
                binary.write_bytes(b"probe")
            results = [
                subprocess.CompletedProcess([], 0, self.encoded(self.record(
                    executor["id"], executor["active_command"]["route"])), b"")
                for executor in document["executors"]
            ]
            with mock.patch.object(gate.subprocess, "run", side_effect=results) as run:
                self.assertEqual(gate.run_active(document, binaries), 4)
            self.assertEqual(run.call_count, 4)
            self.assertEqual(
                [call.args[0][1:] for call in run.call_args_list],
                [executor["active_command"]["arguments"]
                 for executor in document["executors"]],
            )

    def test_missing_binary_nonzero_stderr_and_common_rejection_are_failures(self):
        document = self.manifest()
        document["executors"] = [document["executors"][0]]
        document["executors"][0]["state"] = "execute"
        with self.assertRaisesRegex(gate.GateError, "binary is missing"):
            gate.run_active(document, {})

        with tempfile.TemporaryDirectory(prefix="xr-h2-differential-") as directory:
            binary = Path(directory) / "vm"
            binary.write_bytes(b"probe")
            for result in (
                subprocess.CompletedProcess([], 7, b"", b"unsupported"),
                subprocess.CompletedProcess([], 0, b"false\n", b""),
                subprocess.CompletedProcess([], 0, self.encoded(self.record(
                    "vm-baseline", "vm-baseline-view")), b"warning"),
            ):
                with self.subTest(result=result), mock.patch.object(
                    gate.subprocess, "run", return_value=result
                ), self.assertRaises(gate.GateError):
                    gate.run_active(document, {"vm": binary})


if __name__ == "__main__":
    unittest.main()

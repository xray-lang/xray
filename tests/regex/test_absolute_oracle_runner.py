#!/usr/bin/env python3
"""Self-tests for the regex absolute oracle manifest validator."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock
from pathlib import Path


SCRIPT = Path(__file__).resolve().parent / "run_absolute_oracle.py"
SPEC = importlib.util.spec_from_file_location("run_absolute_oracle", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
ORACLE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ORACLE)


class AbsoluteOracleValidatorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="xray-regex-oracle-test-")
        self.root = Path(self.temp.name) / "v1"
        shutil.copytree(ORACLE.ORACLE_ROOT, self.root)
        self.manifest = self.root / "manifest.json"

    def tearDown(self) -> None:
        self.temp.cleanup()

    def load_manifest(self) -> dict:
        return json.loads(self.manifest.read_text(encoding="utf-8"))

    def store_manifest(self, data: dict) -> None:
        self.manifest.write_text(
            json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )

    def toolchain_identity(self) -> dict:
        return {
            "probe": {"cache": "hit", "fingerprint": "probe-fingerprint"},
            "request": {
                "normalizedTarget": "aarch64-apple-darwin",
                "profile": "hosted",
                "target": "native",
            },
            "selection": {
                "compiler": "/usr/bin/clang",
                "fallbackUsed": False,
                "ownership": "external",
                "provider": "apple-clang",
                "ready": True,
                "runtimeArtifact": "/runtime/libxray_runtime.a",
                "targetAbi": "aarch64-apple-darwin",
            },
            "xray": {
                "build": "0" * 40,
                "sdkDigest": "sdk-digest",
                "version": "0.9.1",
            },
        }

    def toolchain_plan(self, compiler: str = "/usr/bin/clang") -> bytes:
        return (
            "Toolchain plan: provider=apple-clang ownership=external "
            f"target=aarch64-apple-darwin compiler={compiler} "
            "runtime=/runtime/libxray_runtime.a sdk=sdk-digest "
            "probe=probe-fingerprint cache=hit\n"
        ).encode("utf-8")

    def test_repository_oracle_is_valid(self) -> None:
        self.assertEqual(ORACLE.validate_oracle(), [])

    def test_duplicate_case_id_is_rejected(self) -> None:
        data = self.load_manifest()
        data["cases"][1]["id"] = data["cases"][0]["id"]
        self.store_manifest(data)
        self.assertIn(
            "case ids do not match the frozen set",
            ORACLE.validate_oracle(self.manifest),
        )

    def test_removed_case_is_rejected(self) -> None:
        data = self.load_manifest()
        data["cases"].pop()
        self.store_manifest(data)
        self.assertIn(
            "case ids do not match the frozen set",
            ORACLE.validate_oracle(self.manifest),
        )

    def test_source_content_drift_is_rejected(self) -> None:
        source = self.root / "programs/literal_source_owner.xr"
        source.write_text(source.read_text(encoding="utf-8") + "print(1)\n", encoding="utf-8")
        errors = ORACLE.validate_oracle(self.manifest)
        self.assertTrue(any("source_sha256" in error for error in errors))

    def test_expected_content_drift_is_rejected(self) -> None:
        expected = self.root / "expected/literal_source_owner.stdout"
        expected.write_text("changed\n", encoding="utf-8")
        errors = ORACLE.validate_oracle(self.manifest)
        self.assertTrue(any("expected_sha256" in error for error in errors))

    def test_duplicate_json_key_is_rejected(self) -> None:
        payload = self.manifest.read_text(encoding="utf-8")
        self.manifest.write_text(
            payload.replace('"schema": 1', '"schema": 1,\n  "schema": 1'),
            encoding="utf-8",
        )
        errors = ORACLE.validate_oracle(self.manifest)
        self.assertTrue(any("duplicate JSON key" in error for error in errors))

    def test_boolean_schema_is_rejected(self) -> None:
        data = self.load_manifest()
        data["schema"] = True
        self.store_manifest(data)
        self.assertIn(
            "oracle schema version is not exact",
            ORACLE.validate_oracle(self.manifest),
        )

    def test_provider_identity_maps_to_supported_selector(self) -> None:
        self.assertEqual(ORACLE._provider_selector("apple-clang"), "clang")
        self.assertEqual(ORACLE._provider_selector("llvm-clang"), "clang")
        self.assertEqual(ORACLE._provider_selector("gcc"), "gcc")
        self.assertEqual(ORACLE._provider_selector("msvc"), "msvc")
        self.assertEqual(ORACLE._provider_selector("zig"), "zig")
        with self.assertRaises(RuntimeError):
            ORACLE._provider_selector("unknown")

    def test_binary_identity_contract_rejects_mismatch(self) -> None:
        identity = {
            "buildProfile": "Release",
            "commit": "0" * 40,
            "dirty": False,
            "features": ["vm", "aot"],
            "product": "xray-lang",
            "schema": 1,
            "target": "aarch64-apple-darwin",
            "version": "0.9.1",
        }
        ORACLE._validate_binary_identity(identity, "0" * 40)
        changed = dict(identity)
        changed["commit"] = "1" * 40
        with self.assertRaises(RuntimeError):
            ORACLE._validate_binary_identity(changed, "0" * 40)
        changed = dict(identity)
        changed["features"] = ["vm"]
        with self.assertRaises(RuntimeError):
            ORACLE._validate_binary_identity(changed, "0" * 40)

    def test_toolchain_plan_requires_full_doctor_identity(self) -> None:
        toolchain = self.toolchain_identity()
        plan = ORACLE._verify_toolchain_plan(self.toolchain_plan(), toolchain)
        self.assertEqual(plan["provider"], "apple-clang")
        toolchain["probe"]["cache"] = "miss"
        self.assertEqual(
            ORACLE._verify_toolchain_plan(self.toolchain_plan(), toolchain)["cache"],
            "hit",
        )
        with self.assertRaises(RuntimeError):
            ORACLE._verify_toolchain_plan(self.toolchain_plan("/wrong/clang"), toolchain)
        invalid_cache = self.toolchain_plan().replace(b"cache=hit", b"cache=unknown")
        with self.assertRaises(RuntimeError):
            ORACLE._verify_toolchain_plan(invalid_cache, toolchain)

    def test_aot_command_pins_profile_cache_and_provider(self) -> None:
        failed = subprocess.CompletedProcess([], 1, b"", b"failed")
        with mock.patch.object(ORACLE, "_run", return_value=failed) as run:
            ORACLE._execute_aot(
                Path("/fake/xray"),
                Path("/tmp/case.xr"),
                Path("/tmp/case"),
                Path("/tmp/cache"),
                "apple-clang",
                1,
            )
        command = run.call_args.args[0]
        self.assertEqual(command[command.index("--profile") + 1], "hosted")
        self.assertEqual(command[command.index("--cache-dir") + 1], "/tmp/cache")
        self.assertEqual(command[command.index("--toolchain") + 1], "clang")

    def test_path_escape_is_rejected(self) -> None:
        data = self.load_manifest()
        data["cases"][0]["source"] = "../outside.xr"
        self.store_manifest(data)
        errors = ORACLE.validate_oracle(self.manifest)
        self.assertTrue(any("escapes the oracle root" in error for error in errors))

    def test_missing_mutation_is_rejected(self) -> None:
        mutation = self.root / "mutation_inventory.jsonl"
        lines = mutation.read_text(encoding="utf-8").splitlines()
        mutation.write_text("\n".join(lines[1:]) + "\n", encoding="utf-8")
        self.assertIn(
            "mutation inventory ids do not match the frozen set",
            ORACLE.validate_oracle(self.manifest),
        )

    def test_negative_row_drift_is_rejected_by_digest(self) -> None:
        negative = self.root / "negative_inventory.jsonl"
        negative.write_text(
            negative.read_text(encoding="utf-8").replace(
                "regex.invalid_flag", "regex.changed", 1
            ),
            encoding="utf-8",
        )
        self.assertIn(
            "negative_inventory content does not match its manifest digest",
            ORACLE.validate_oracle(self.manifest),
        )

    def test_mutation_row_drift_is_rejected_by_digest(self) -> None:
        mutation = self.root / "mutation_inventory.jsonl"
        mutation.write_text(
            mutation.read_text(encoding="utf-8").replace(
                "regex-plan.bad-opcode", "regex-plan.changed", 1
            ),
            encoding="utf-8",
        )
        self.assertIn(
            "mutation_inventory content does not match its manifest digest",
            ORACLE.validate_oracle(self.manifest),
        )

    def test_hazard_row_drift_is_rejected_by_digest(self) -> None:
        hazard = self.root / "hazard_inventory.jsonl"
        hazard.write_text(
            hazard.read_text(encoding="utf-8").replace(
                "scalar-boundary-valid-utf8", "changed", 1
            ),
            encoding="utf-8",
        )
        self.assertIn(
            "hazard_inventory content does not match its manifest digest",
            ORACLE.validate_oracle(self.manifest),
        )

    def test_inventory_semantic_drift_is_rejected_after_manifest_update(self) -> None:
        drifts = {
            "negative_inventory": ("regex.invalid_flag", "regex.changed"),
            "mutation_inventory": ("regex-plan.bad-opcode", "regex-plan.changed"),
            "hazard_inventory": ("scalar-boundary-valid-utf8", "changed"),
        }
        for field, (before, after) in drifts.items():
            with self.subTest(field=field):
                path = self.root / f"{field}.jsonl"
                original = path.read_bytes()
                path.write_bytes(original.replace(before.encode(), after.encode(), 1))
                data = self.load_manifest()
                data[f"{field}_sha256"] = ORACLE._sha256(path.read_bytes())
                self.store_manifest(data)
                self.assertIn(
                    f"{field} content does not match its frozen semantic digest",
                    ORACLE.validate_oracle(self.manifest),
                )
                path.write_bytes(original)
                data[f"{field}_sha256"] = ORACLE._sha256(original)
                self.store_manifest(data)

    def test_target_plan_mutation_owner_drift_is_rejected(self) -> None:
        mutation = self.root / "mutation_inventory.jsonl"
        original = mutation.read_bytes()
        original_digest = ORACLE._sha256(original)
        changes = (
            ("scratch_over_profile", "activation_owner", "regex_plan_verifier"),
            ("scratch_over_profile", "expected_rejection", "regex-plan.resource-limit"),
            ("unexpected_dynamic_operation", "activation_owner", "regex_plan_verifier"),
            (
                "unexpected_dynamic_operation",
                "expected_rejection",
                "regex-plan.capability-mismatch",
            ),
        )
        for mutation_id, field, replacement in changes:
            with self.subTest(mutation_id=mutation_id, field=field):
                rows = [json.loads(line) for line in original.decode().splitlines()]
                next(row for row in rows if row["id"] == mutation_id)[field] = replacement
                changed = (
                    "\n".join(
                        json.dumps(row, separators=(",", ":"), sort_keys=True)
                        for row in rows
                    )
                    + "\n"
                ).encode()
                mutation.write_bytes(changed)
                changed_digest = ORACLE._sha256(changed)
                data = self.load_manifest()
                data["mutation_inventory_sha256"] = changed_digest
                self.store_manifest(data)
                with mock.patch.dict(
                    ORACLE.REQUIRED_INVENTORY_DIGESTS,
                    {"mutation_inventory": changed_digest},
                ):
                    errors = ORACLE.validate_oracle(self.manifest)
                self.assertTrue(
                    any(
                        "TargetPlan admission route is not exact" in error
                        for error in errors
                    )
                )
                mutation.write_bytes(original)
                data["mutation_inventory_sha256"] = original_digest
                self.store_manifest(data)

    def test_utf8_continuation_offset_is_rejected(self) -> None:
        negative = self.root / "negative_inventory.jsonl"
        lines = negative.read_text(encoding="utf-8").splitlines()
        row = json.loads(lines[3])
        row["pattern_utf8_hex"] = "c3a9"
        row["expected_offset"] = 1
        lines[3] = json.dumps(row, separators=(",", ":"), sort_keys=True)
        negative.write_text("\n".join(lines) + "\n", encoding="utf-8")
        data = self.load_manifest()
        data["negative_inventory_sha256"] = ORACLE._sha256(negative.read_bytes())
        self.store_manifest(data)
        errors = ORACLE.validate_oracle(self.manifest)
        self.assertTrue(any("offset splits a UTF-8 scalar" in error for error in errors))

    def test_unlisted_program_is_rejected(self) -> None:
        (self.root / "programs/unlisted.xr").write_text(
            'print("unlisted")\n', encoding="utf-8"
        )
        self.assertIn(
            "program fixture inventory does not exactly match the manifest",
            ORACLE.validate_oracle(self.manifest),
        )

    def test_carriage_return_in_expected_output_is_rejected(self) -> None:
        expected = self.root / "expected/matching_core.stdout"
        expected.write_bytes(expected.read_bytes().replace(b"\n", b"\r\n", 1))
        errors = ORACLE.validate_oracle(self.manifest)
        self.assertTrue(any("LF-only" in error for error in errors))

    def test_runner_executes_every_case_on_both_backends(self) -> None:
        manifest, errors = ORACLE._load_validated_oracle()
        self.assertEqual(errors, [])
        assert manifest is not None
        expected = {
            case["id"]: (ORACLE.ORACLE_ROOT / case["expected_stdout"]).read_bytes()
            for case in manifest["cases"]
        }
        vm_seen: list[str] = []
        aot_seen: list[str] = []

        def execute_vm(binary: Path, source: Path, timeout: int) -> subprocess.CompletedProcess:
            del binary, timeout
            vm_seen.append(source.stem)
            return subprocess.CompletedProcess([], 0, expected[source.stem], b"")

        def execute_aot(
            binary: Path,
            source: Path,
            output: Path,
            cache: Path,
            provider: str,
            timeout: int,
            env: dict[str, str],
        ) -> tuple[subprocess.CompletedProcess, subprocess.CompletedProcess]:
            del binary, output, timeout
            self.assertEqual(provider, "apple-clang")
            self.assertEqual(cache.name, "aot-cache")
            cache_variable = "LOCALAPPDATA" if ORACLE.os.name == "nt" else "XDG_CACHE_HOME"
            self.assertEqual(Path(env[cache_variable]).name, "provider-cache")
            aot_seen.append(source.stem)
            build = subprocess.CompletedProcess([], 0, self.toolchain_plan(), b"")
            run = subprocess.CompletedProcess([], 0, expected[source.stem], b"")
            return build, run

        identity = {
            "buildProfile": "Release",
            "commit": "0" * 40,
            "target": "aarch64-apple-darwin",
            "version": "0.9.1",
        }
        provider = self.toolchain_identity()
        with (
            mock.patch.object(ORACLE, "verify_matching_binary", return_value=identity),
            mock.patch.object(
                ORACLE, "verify_native_provider", return_value=provider
            ) as verify_provider,
            mock.patch.object(ORACLE, "_execute_vm", side_effect=execute_vm),
            mock.patch.object(ORACLE, "_execute_aot", side_effect=execute_aot),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(ORACLE.run_oracle(Path("/fake/xray"), 1, manifest), 0)
        case_ids = sorted(ORACLE.REQUIRED_CASE_SHAPES)
        self.assertEqual(sorted(vm_seen), case_ids)
        self.assertEqual(sorted(aot_seen), case_ids)
        provider_env = verify_provider.call_args.args[2]
        cache_variable = "LOCALAPPDATA" if ORACLE.os.name == "nt" else "XDG_CACHE_HOME"
        self.assertEqual(Path(provider_env[cache_variable]).name, "provider-cache")

    def test_successful_aot_build_stderr_is_rejected(self) -> None:
        manifest, errors = ORACLE._load_validated_oracle()
        self.assertEqual(errors, [])
        assert manifest is not None
        expected = {
            case["id"]: (ORACLE.ORACLE_ROOT / case["expected_stdout"]).read_bytes()
            for case in manifest["cases"]
        }

        def execute_vm(binary: Path, source: Path, timeout: int) -> subprocess.CompletedProcess:
            del binary, timeout
            return subprocess.CompletedProcess([], 0, expected[source.stem], b"")

        def execute_aot(
            binary: Path,
            source: Path,
            output: Path,
            cache: Path,
            provider: str,
            timeout: int,
            env: dict[str, str],
        ) -> tuple[subprocess.CompletedProcess, subprocess.CompletedProcess]:
            del binary, output, cache, provider, timeout, env
            build = subprocess.CompletedProcess(
                [], 0, self.toolchain_plan(), b"unexpected diagnostic\n"
            )
            run = subprocess.CompletedProcess([], 0, expected[source.stem], b"")
            return build, run

        identity = {
            "buildProfile": "Release",
            "commit": "0" * 40,
            "target": "aarch64-apple-darwin",
            "version": "0.9.1",
        }
        provider = self.toolchain_identity()
        with (
            mock.patch.object(ORACLE, "verify_matching_binary", return_value=identity),
            mock.patch.object(ORACLE, "verify_native_provider", return_value=provider),
            mock.patch.object(ORACLE, "_execute_vm", side_effect=execute_vm),
            mock.patch.object(ORACLE, "_execute_aot", side_effect=execute_aot),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            self.assertEqual(ORACLE.run_oracle(Path("/fake/xray"), 1, manifest), 1)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Focused unit coverage for task-200 public-native readiness blockers."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))

import check_binary_public_native_readiness as readiness  # noqa: E402
from check_binary_public_native_readiness import (  # noqa: E402
    NATIVE_TYPED_ERROR_ABI_BLOCKERS,
    PRE_SWITCH_NATIVE_PUBLIC_SURFACE,
    SINGLE_FUNCTION_PUBLIC_SURFACE_PROBES,
    check_boundary,
    check_dependency_markers,
    check_native_typed_error_abi_blockers,
    check_single_function_public_surface_probes,
    check_single_function_native_typed_error_probes,
    load_boundary_modules,
)


class BinaryPublicNativeReadinessTest(unittest.TestCase):
    def run_fast_single_function_probe(
        self,
        subject: str = "compress.gunzip",
        modules: dict[str, dict[str, object]] | None = None,
    ):
        original_modules = readiness.load_boundary_modules
        original_probes = readiness.SINGLE_FUNCTION_NATIVE_TYPED_ERROR_PROBES
        probe = dict(original_probes[subject])
        probe["required"] = {}
        if modules is not None:
            readiness.load_boundary_modules = lambda root: modules
        readiness.SINGLE_FUNCTION_NATIVE_TYPED_ERROR_PROBES = {subject: probe}
        try:
            return {
                result.subject: result
                for result in check_single_function_native_typed_error_probes(ROOT)
            }[subject]
        finally:
            readiness.load_boundary_modules = original_modules
            readiness.SINGLE_FUNCTION_NATIVE_TYPED_ERROR_PROBES = original_probes

    def run_fast_public_surface_probe(
        self,
        subject: str = "crypto.sha256",
        modules: dict[str, dict[str, object]] | None = None,
    ):
        original_modules = readiness.load_boundary_modules
        original_probes = readiness.SINGLE_FUNCTION_PUBLIC_SURFACE_PROBES
        probe = dict(original_probes[subject])
        probe["required"] = {}
        if modules is not None:
            readiness.load_boundary_modules = lambda root: modules
        readiness.SINGLE_FUNCTION_PUBLIC_SURFACE_PROBES = {subject: probe}
        try:
            return {
                result.subject: result
                for result in check_single_function_public_surface_probes(ROOT)
            }[subject]
        finally:
            readiness.load_boundary_modules = original_modules
            readiness.SINGLE_FUNCTION_PUBLIC_SURFACE_PROBES = original_probes

    def test_typed_native_error_blockers_are_module_level(self) -> None:
        results = check_native_typed_error_abi_blockers(ROOT)
        by_subject = {result.subject: result for result in results}
        self.assertEqual(set(NATIVE_TYPED_ERROR_ABI_BLOCKERS), set(by_subject))
        for module in ("compress", "crypto", "io", "net"):
            with self.subTest(module=module):
                result = by_subject[module]
                self.assertTrue(result.ok, result.detail)
                self.assertEqual("NATIVE_TYPED_ERROR_ABI_BLOCKER", result.category)
                self.assertIn("typed native error ABI remains blocked", result.detail)

    def test_compress_gunzip_typed_error_abi_is_a_focused_function_slice(self) -> None:
        spec = readiness.SINGLE_FUNCTION_NATIVE_TYPED_ERROR_PROBES["compress.gunzip"]
        module = load_boundary_modules(ROOT)["compress"]
        result = self.run_fast_single_function_probe()

        self.assertTrue(result.ok, result.detail)
        self.assertEqual("NATIVE_TYPED_ERROR_ABI_FUNCTION_BLOCKER", result.category)
        self.assertIn("focused VM/native-AOT typed CompressionError ABI slice", result.detail)
        self.assertIn("gunzip", module["public_native"])
        self.assertIsNot(module.get("def_migration_complete"), True)
        for path_text, anchors in spec["required"].items():
            with self.subTest(path=path_text):
                text = (ROOT / path_text).read_text(encoding="utf-8")
                for anchor in anchors:
                    self.assertIn(anchor, text)

        blocks = readiness.def_function_blocks(ROOT, spec["module"], spec["function"])
        self.assertTrue(blocks)
        for block in blocks:
            self.assertNotIn("@errors(", block)
            self.assertIn('effect: "CompressionError.InvalidData"', block)

        self.assertTrue(spec.get("allow_typed_error_before_full_marker"))
        self.assertIn("focused VM/native-AOT typed CompressionError ABI slice", spec["detail"])
        self.assertIn("full task-198 readiness remains blocked", spec["detail"])
        self.assertFalse(
            (ROOT / "tests" / "stdlib" / "contracts" / "TASK_198_TYPED_NATIVE_ERRORS_READY").exists(),
            "task-198 full readiness marker must remain absent for this probe",
        )

    def test_compress_inflate_typed_error_abi_is_a_focused_function_slice(self) -> None:
        spec = readiness.SINGLE_FUNCTION_NATIVE_TYPED_ERROR_PROBES["compress.inflate"]
        module = load_boundary_modules(ROOT)["compress"]
        result = self.run_fast_single_function_probe("compress.inflate")

        self.assertTrue(result.ok, result.detail)
        self.assertEqual("NATIVE_TYPED_ERROR_ABI_FUNCTION_BLOCKER", result.category)
        self.assertIn("focused VM/native-AOT typed CompressionError ABI slice", result.detail)
        self.assertIn("inflate", module["public_native"])
        self.assertIsNot(module.get("def_migration_complete"), True)
        for path_text, anchors in spec["required"].items():
            with self.subTest(path=path_text):
                text = (ROOT / path_text).read_text(encoding="utf-8")
                for anchor in anchors:
                    self.assertIn(anchor, text)

        blocks = readiness.def_function_blocks(ROOT, spec["module"], spec["function"])
        self.assertTrue(blocks)
        for block in blocks:
            self.assertNotIn("@errors(", block)
            self.assertIn('effect: "CompressionError.InvalidData"', block)

        self.assertTrue(spec.get("allow_typed_error_before_full_marker"))
        self.assertIn("focused VM/native-AOT typed CompressionError ABI slice", spec["detail"])
        self.assertIn("full task-198 readiness remains blocked", spec["detail"])
        self.assertFalse(
            (ROOT / "tests" / "stdlib" / "contracts" / "TASK_198_TYPED_NATIVE_ERRORS_READY").exists(),
            "task-198 full readiness marker must remain absent for this probe",
        )

    def test_compress_zlib_decompress_typed_error_abi_is_a_focused_function_slice(self) -> None:
        spec = readiness.SINGLE_FUNCTION_NATIVE_TYPED_ERROR_PROBES["compress.zlibDecompress"]
        module = load_boundary_modules(ROOT)["compress"]
        result = self.run_fast_single_function_probe("compress.zlibDecompress")

        self.assertTrue(result.ok, result.detail)
        self.assertEqual("NATIVE_TYPED_ERROR_ABI_FUNCTION_BLOCKER", result.category)
        self.assertIn("focused VM/native-AOT typed CompressionError ABI slice", result.detail)
        self.assertIn("zlibDecompress", module["public_native"])
        self.assertIsNot(module.get("def_migration_complete"), True)
        for path_text, anchors in spec["required"].items():
            with self.subTest(path=path_text):
                text = (ROOT / path_text).read_text(encoding="utf-8")
                for anchor in anchors:
                    self.assertIn(anchor, text)

        blocks = readiness.def_function_blocks(ROOT, spec["module"], spec["function"])
        self.assertTrue(blocks)
        for block in blocks:
            self.assertNotIn("@errors(", block)
            self.assertIn('effect: "CompressionError.InvalidData"', block)

        self.assertTrue(spec.get("allow_typed_error_before_full_marker"))
        self.assertIn("focused VM/native-AOT typed CompressionError ABI slice", spec["detail"])
        self.assertIn("full task-198 readiness remains blocked", spec["detail"])
        self.assertFalse(
            (ROOT / "tests" / "stdlib" / "contracts" / "TASK_198_TYPED_NATIVE_ERRORS_READY").exists(),
            "task-198 full readiness marker must remain absent for this probe",
        )

    def test_crypto_random_bytes_typed_error_abi_is_a_single_function_blocker(self) -> None:
        spec = readiness.SINGLE_FUNCTION_NATIVE_TYPED_ERROR_PROBES["crypto.randomBytes"]
        module = load_boundary_modules(ROOT)["crypto"]
        result = self.run_fast_single_function_probe("crypto.randomBytes")

        self.assertTrue(result.ok, result.detail)
        self.assertEqual("NATIVE_TYPED_ERROR_ABI_FUNCTION_BLOCKER", result.category)
        self.assertIn("focused VM/native-AOT typed CryptoError ABI slice", result.detail)
        self.assertIn("randomBytes", module["public_native"])
        self.assertIsNot(module.get("def_migration_complete"), True)
        for path_text, anchors in spec["required"].items():
            with self.subTest(path=path_text):
                text = (ROOT / path_text).read_text(encoding="utf-8")
                for anchor in anchors:
                    self.assertIn(anchor, text)

        blocks = readiness.def_function_blocks(ROOT, spec["module"], spec["function"])
        self.assertTrue(blocks)
        for block in blocks:
            self.assertNotIn("@errors(", block)
            self.assertIn('effect: "CryptoError.InvalidLength"', block)

        self.assertTrue(spec.get("allow_typed_error_before_full_marker"))
        self.assertIn("Array<byte>", spec["detail"])
        self.assertIn("full task-198/task-200 readiness remains blocked", spec["detail"])
        self.assertFalse(
            (ROOT / "tests" / "stdlib" / "contracts" / "TASK_198_TYPED_NATIVE_ERRORS_READY").exists(),
            "task-198 full readiness marker must remain absent for this probe",
        )

    def test_crypto_sha256_fixed_digest_surface_is_a_single_function_blocker(self) -> None:
        spec = SINGLE_FUNCTION_PUBLIC_SURFACE_PROBES["crypto.sha256"]
        module = load_boundary_modules(ROOT)["crypto"]
        result = self.run_fast_public_surface_probe("crypto.sha256")

        self.assertTrue(result.ok, result.detail)
        self.assertEqual("PUBLIC_NATIVE_FUNCTION_SURFACE_BLOCKER", result.category)
        self.assertIn("hex-string fixed-digest native path", result.detail)
        self.assertIn("sha256", module["public_native"])
        self.assertIsNot(module.get("def_migration_complete"), True)
        for path_text, anchors in spec["required"].items():
            with self.subTest(path=path_text):
                text = (ROOT / path_text).read_text(encoding="utf-8")
                for anchor in anchors:
                    self.assertIn(anchor, text)

        blocks = readiness.def_function_blocks(ROOT, spec["module"], spec["function"])
        self.assertTrue(blocks)
        for block in blocks:
            self.assertNotIn("Slice<byte>", block)
            self.assertNotIn("[byte;32]", block)
            self.assertNotIn("encoding.hexEncode", block)

        self.assertIn("[byte;32]", spec["detail"])
        self.assertIn("Slice<byte>", spec["detail"])
        self.assertIn("encoding.hexEncode", spec["detail"])

    def test_net_read_string_null_surface_is_a_single_function_blocker(self) -> None:
        spec = SINGLE_FUNCTION_PUBLIC_SURFACE_PROBES["net.read"]
        module = load_boundary_modules(ROOT)["net"]
        result = self.run_fast_public_surface_probe("net.read")

        self.assertTrue(result.ok, result.detail)
        self.assertEqual("PUBLIC_NATIVE_FUNCTION_SURFACE_BLOCKER", result.category)
        self.assertIn("string?/null-sentinel native path", result.detail)
        self.assertIn("read", module["public_native"])
        self.assertIsNot(module.get("def_migration_complete"), True)
        for path_text, anchors in spec["required"].items():
            with self.subTest(path=path_text):
                text = (ROOT / path_text).read_text(encoding="utf-8")
                for anchor in anchors:
                    self.assertIn(anchor, text)

        blocks = readiness.def_function_blocks(ROOT, spec["module"], spec["function"])
        self.assertEqual(2, len(blocks))
        for block in blocks:
            for anchor in spec["forbidden_def_anchors"]:
                self.assertNotIn(anchor, block)

        self.assertIn("Array<byte>", spec["detail"])
        self.assertIn("NetError", spec["detail"])
        self.assertIn("typed byte boundary", spec["detail"])

    def test_compress_gunzip_probe_fails_closed_on_partial_public_switch(self) -> None:
        modules = {
            name: dict(module)
            for name, module in load_boundary_modules(ROOT).items()
        }
        modules["compress"] = dict(modules["compress"])
        modules["compress"]["public_native"] = ["crc32"]

        result = self.run_fast_single_function_probe(modules=modules)

        self.assertFalse(result.ok)
        self.assertIn("compress.gunzip left pre-switch public_native", result.detail)

    def test_compress_inflate_probe_fails_closed_on_partial_public_switch(self) -> None:
        modules = {
            name: dict(module)
            for name, module in load_boundary_modules(ROOT).items()
        }
        modules["compress"] = dict(modules["compress"])
        modules["compress"]["public_native"] = ["crc32"]

        result = self.run_fast_single_function_probe("compress.inflate", modules)

        self.assertFalse(result.ok)
        self.assertIn("compress.inflate left pre-switch public_native", result.detail)

    def test_compress_zlib_decompress_probe_fails_closed_on_partial_public_switch(self) -> None:
        modules = {
            name: dict(module)
            for name, module in load_boundary_modules(ROOT).items()
        }
        modules["compress"] = dict(modules["compress"])
        modules["compress"]["public_native"] = ["crc32"]

        result = self.run_fast_single_function_probe("compress.zlibDecompress", modules)

        self.assertFalse(result.ok)
        self.assertIn("compress.zlibDecompress left pre-switch public_native", result.detail)

    def test_crypto_random_bytes_probe_fails_closed_on_partial_public_switch(self) -> None:
        modules = {
            name: dict(module)
            for name, module in load_boundary_modules(ROOT).items()
        }
        modules["crypto"] = dict(modules["crypto"])
        modules["crypto"]["public_native"] = ["uuid"]

        result = self.run_fast_single_function_probe("crypto.randomBytes", modules)

        self.assertFalse(result.ok)
        self.assertIn("crypto.randomBytes left pre-switch public_native", result.detail)

    def test_crypto_sha256_probe_fails_closed_on_partial_public_switch(self) -> None:
        modules = {
            name: dict(module)
            for name, module in load_boundary_modules(ROOT).items()
        }
        modules["crypto"] = dict(modules["crypto"])
        modules["crypto"]["public_native"] = ["uuid"]

        result = self.run_fast_public_surface_probe("crypto.sha256", modules)

        self.assertFalse(result.ok)
        self.assertIn("crypto.sha256 left pre-switch public_native", result.detail)

    def test_net_read_probe_fails_closed_on_partial_public_switch(self) -> None:
        modules = {
            name: dict(module)
            for name, module in load_boundary_modules(ROOT).items()
        }
        modules["net"] = dict(modules["net"])
        modules["net"]["public_native"] = ["write"]

        result = self.run_fast_public_surface_probe("net.read", modules)

        self.assertFalse(result.ok)
        self.assertIn("net.read left pre-switch public_native", result.detail)

    def test_public_switch_markers_remain_absent(self) -> None:
        results = check_dependency_markers(ROOT)
        blockers = {
            result.subject: result
            for result in results
            if result.category == "PUBLIC_SWITCH_DEPENDENCY_BLOCKER"
        }
        self.assertEqual(
            {"TASK_197_SLICE_PROVENANCE_READY", "TASK_198_TYPED_NATIVE_ERRORS_READY"},
            set(blockers),
        )
        for marker, result in blockers.items():
            with self.subTest(marker=marker):
                self.assertTrue(result.ok, result.detail)
                self.assertIn("public-native switch remains blocked", result.detail)

    def test_task_198_string_native_error_marker_is_scoped(self) -> None:
        marker = ROOT / "tests" / "stdlib" / "contracts" / "TASK_198_STRING_NATIVE_ERROR_ABI_READY"
        text = marker.read_text(encoding="utf-8")
        self.assertIn("scope: task-198 string native error ABI only", text)
        self.assertIn(
            "string.fromUtf8(Slice<byte>) -> string throws Utf8Error.InvalidUtf8",
            text,
        )
        self.assertIn("public-native switch remains blocked", text)
        self.assertFalse(
            (ROOT / "tests" / "stdlib" / "contracts" / "TASK_198_TYPED_NATIVE_ERRORS_READY").exists(),
            "scoped string ABI marker must not imply full task-198 readiness",
        )

        result = {
            item.subject: item for item in readiness.check_partial_dependency_evidence(ROOT)
        }["TASK_198_STRING_RUNTIME_ONLY"]
        self.assertTrue(result.ok, result.detail)
        self.assertIn("scoped marker is present", result.detail)

    def test_task_197_slice_provenance_is_partial_evidence_only(self) -> None:
        spec = readiness.PARTIAL_DEPENDENCY_EVIDENCE["TASK_197_VERIFIER_ONLY"]
        for path_text, anchors in spec["required"].items():
            with self.subTest(path=path_text):
                text = (ROOT / path_text).read_text(encoding="utf-8")
                for anchor in anchors:
                    self.assertIn(anchor, text)

        marker = spec["full_marker"]
        self.assertFalse((ROOT / "tests" / "stdlib" / "contracts" / marker).exists())
        self.assertIn("borrow/provenance verifier evidence exists", spec["detail"])
        self.assertIn("full Slice public-switch provenance is not marked ready", spec["detail"])

    def test_pre_switch_native_surface_is_exact_until_dependencies_close(self) -> None:
        results = check_boundary(ROOT)
        exact = {
            result.subject: result
            for result in results
            if result.category == "PRE_SWITCH_NATIVE_EXACT_SURFACE"
        }
        self.assertEqual(set(PRE_SWITCH_NATIVE_PUBLIC_SURFACE), set(exact))

        modules = load_boundary_modules(ROOT)
        for module, expected in PRE_SWITCH_NATIVE_PUBLIC_SURFACE.items():
            with self.subTest(module=module):
                self.assertTrue(exact[module].ok, exact[module].detail)
                self.assertEqual(expected, tuple(modules[module]["public_native"]))

    def test_pre_switch_native_surface_rejects_partial_drift(self) -> None:
        modules = {
            name: dict(module)
            for name, module in load_boundary_modules(ROOT).items()
        }
        modules["compress"] = dict(modules["compress"])
        modules["compress"]["public_native"] = ["crc32"]

        original = readiness.load_boundary_modules
        readiness.load_boundary_modules = lambda root: modules
        try:
            exact = {
                result.subject: result
                for result in readiness.check_boundary(ROOT)
                if result.category == "PRE_SWITCH_NATIVE_EXACT_SURFACE"
            }
        finally:
            readiness.load_boundary_modules = original

        self.assertFalse(exact["compress"].ok)
        self.assertIn("missing pre-switch entries", exact["compress"].detail)
        self.assertTrue(exact["crypto"].ok, exact["crypto"].detail)


if __name__ == "__main__":
    unittest.main()

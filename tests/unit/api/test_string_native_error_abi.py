#!/usr/bin/env python3
"""Focused task-198 VM/native-AOT typed string error ABI gate."""

from __future__ import annotations

import argparse
import ast
import os
import re
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]

def _cache_root() -> Path:
    """Shared native-build cache. Lives outside build/ so a clean reconfigure
    does not throw away objects that cost ~30s each to recreate."""
    return Path(os.environ.get("XRAY_TEST_CACHE_ROOT", str(ROOT / ".cache" / "xray-test")))

FIXTURE = ROOT / "tests/aot/basic/string_utf8_conversion.xr"
STRING_DECL = ROOT / "stdlib/types/string.xr"
NATIVE_DEFS = ROOT / "src/frontend/analyzer/xnative_type_defs.inc.c"
VM_RUNTIME = ROOT / "src/runtime/object/xstring_methods.c"
AOT_RUNTIME = ROOT / "src/aot/xrt_method.h"
EXPECTED_OUTPUT = (
    "hi\n"
    "hé\n"
    "true\n"
    "Utf8Error.InvalidUtf8\n"
    "true\n"
    "true\n"
    "true\n"
    "true\n"
    "true\n"
    "A�B\n"
    "�(�\n"
    "�\n"
    "A€����B\n"
    "��������A\n"
    "���A\n"
    "����A\n"
    "[104, 105]\n"
    "é\n"
    "🙂\n"
    "a|β|中\n"
    "aβ中\n"
    "A\n"
    "é\n"
    "中\n"
    "true\n"
    "true\n"
    "true\n"
    "StringSliceError.InvalidByteRange\n"
    "true\n"
    "StringSliceError.InvalidByteRange\n"
).encode()


def embedded_native_def(name: str) -> str:
    source = NATIVE_DEFS.read_text(encoding="utf-8")
    pattern = rf"static const char xr_native_def_{re.escape(name)}\[\] =(?P<body>.*?);\n"
    match = re.search(pattern, source, re.S)
    if not match:
        raise AssertionError(f"missing embedded native type definition: {name}")
    return "".join(ast.literal_eval(part) for part in re.findall(r'"(?:\\.|[^"\\])*"', match["body"]))


class StringNativeErrorAbiTest(unittest.TestCase):
    xray: Path

    @staticmethod
    def normalized_output(output: bytes) -> bytes:
        return output.replace(b"\r\n", b"\n")

    def run_checked(
        self, args: list[str], *, stdout: int = subprocess.PIPE
    ) -> subprocess.CompletedProcess[bytes]:
        return subprocess.run(
            args,
            cwd=ROOT,
            stdout=stdout,
            stderr=subprocess.STDOUT,
            check=True,
            timeout=60,
        )

    def run_raw(self, args: list[str]) -> subprocess.CompletedProcess[bytes]:
        return subprocess.run(
            args,
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
            timeout=60,
        )

    def test_public_declaration_is_typed_and_non_nullable(self) -> None:
        declaration = STRING_DECL.read_text(encoding="utf-8")
        generated = embedded_native_def("string")
        self.assertIn("static fromUtf8(bytes: Slice<u8>) -> string", declaration)
        self.assertIn("sliceBytes(start: i64, end: i64) -> string", declaration)
        self.assertNotIn("fromUtf8(bytes: Slice<u8>) -> string?", declaration)
        self.assertIn("enum Utf8Error", generated)
        self.assertIn("enum StringSliceError", generated)
        self.assertIn("static fromUtf8(bytes: Slice<u8>) -> string", generated)
        self.assertIn("sliceBytes(start: i64, end: i64) -> string", generated)
        self.assertNotIn("@" + "native", generated)
        self.assertNotIn("@" + "errors", generated)
        self.assertNotIn("fromUtf8(bytes: Slice<u8>) -> string?", generated)

    def test_runtime_sources_route_failures_to_typed_error_channel(self) -> None:
        vm_runtime = VM_RUNTIME.read_text(encoding="utf-8")
        aot_runtime = AOT_RUNTIME.read_text(encoding="utf-8")

        self.assertIn("xr_utf8_scan_strict(data, len)", vm_runtime)
        self.assertIn(
            "string_publish_builtin_enum_error(iso, XR_GLOBAL_VAR_UTF8_ERROR, 0",
            vm_runtime,
        )
        self.assertIn(
            "string_publish_builtin_enum_error(iso, XR_GLOBAL_VAR_STRING_SLICE_ERROR, 0",
            vm_runtime,
        )
        self.assertIn("xr_builtin_enum_error_construct", vm_runtime)
        # The consuming half of this channel moved into the execution context and
        # its VM-side rediscovery was deleted with it, so the runtime source read
        # here no longer carries either. test_execution_error_channel covers it.
        self.assertIn("xr_utf8_core_scan_strict(data, len)", aot_runtime)
        self.assertIn(
            'xrt_set_builtin_enum_error("Utf8Error", "InvalidUtf8", 0);',
            aot_runtime,
        )
        self.assertIn(
            'xrt_set_builtin_enum_error("StringSliceError", "InvalidByteRange", 0);',
            aot_runtime,
        )
        self.assertIn("xrt_pending_error = xrt_enum_aggregate_box(err);", aot_runtime)

    def assert_uncaught_pending_error(
        self, source_text: str, native_stem: str, forbidden_fragments: list[bytes]
    ) -> None:
        output_dir = ROOT / "build" / ".xray-test-tmp"
        output_dir.mkdir(parents=True, exist_ok=True)
        source = output_dir / f"{native_stem}_{os.getpid()}.xr"
        native = output_dir / f"{native_stem}_{os.getpid()}"
        cache = _cache_root() / native_stem
        source.write_text(source_text, encoding="utf-8")
        try:
            vm = self.run_raw([str(self.xray), str(source)])
            self.assertNotEqual(vm.returncode, 0, vm.stdout.decode("utf-8", "replace"))
            for fragment in forbidden_fragments:
                self.assertNotIn(fragment, vm.stdout)

            self.run_checked(
                [
                    str(self.xray),
                    "build",
                    "--native",
                    "-O",
                    "0",
                    str(source),
                    "-o",
                    str(native),
                    "--cache-dir",
                    str(cache),
                ],
                stdout=subprocess.DEVNULL,
            )
            aot = self.run_raw([str(native)])
            self.assertNotEqual(aot.returncode, 0, aot.stdout.decode("utf-8", "replace"))
            for fragment in forbidden_fragments:
                self.assertNotIn(fragment, aot.stdout)
        finally:
            native.unlink(missing_ok=True)
            source.unlink(missing_ok=True)

    def test_uncaught_invalid_utf8_fails_instead_of_returning_null(self) -> None:
        valid_source = "var valid: Array<u8> = [111, 107]\nprint(string.fromUtf8(valid[:]))\n"
        output_dir = ROOT / "build" / ".xray-test-tmp"
        output_dir.mkdir(parents=True, exist_ok=True)
        valid = output_dir / f"task-198-valid-utf8_{os.getpid()}.xr"
        valid.write_text(valid_source, encoding="utf-8")
        try:
            output = self.run_checked([str(self.xray), str(valid)]).stdout
        finally:
            valid.unlink(missing_ok=True)
        self.assertEqual(self.normalized_output(output), b"ok\n")

        self.assert_uncaught_pending_error(
            "var invalid: Array<u8> = [255]\nprint(string.fromUtf8(invalid[:]))\n",
            "task-198-uncaught-invalid-utf8",
            [b"null\n", b"string.fromUtf8 invalid UTF-8"],
        )

    def test_uncaught_invalid_slice_byte_range_fails_instead_of_returning_null(self) -> None:
        self.assert_uncaught_pending_error(
            'var s = "Aé中"\nprint(s.sliceBytes(2, 3))\n',
            "task-198-uncaught-invalid-slice",
            [b"null\n", b"string.sliceBytes invalid byte range"],
        )

    def test_vm_native_aot_typed_catch_parity(self) -> None:
        vm = self.normalized_output(
            self.run_checked([str(self.xray), str(FIXTURE)]).stdout
        )
        self.assertEqual(EXPECTED_OUTPUT, vm)

        output_dir = ROOT / "build" / ".xray-test-tmp"
        output_dir.mkdir(parents=True, exist_ok=True)
        native = output_dir / f"string_native_error_{os.getpid()}"
        cache = _cache_root() / "task-198-native-error"
        try:
            self.run_checked(
                [
                    str(self.xray),
                    "build",
                    "--native",
                    "-O",
                    "0",
                    str(FIXTURE),
                    "-o",
                    str(native),
                    "--cache-dir",
                    str(cache),
                ],
                stdout=subprocess.DEVNULL,
            )
            aot = self.normalized_output(self.run_checked([str(native)]).stdout)
        finally:
            native.unlink(missing_ok=True)

        self.assertEqual(vm, aot)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--xray", type=Path, default=ROOT / "build/xray")
    args, unittest_args = parser.parse_known_args()
    StringNativeErrorAbiTest.xray = args.xray.resolve()
    unittest.main(argv=[__file__, *unittest_args])


if __name__ == "__main__":
    main()

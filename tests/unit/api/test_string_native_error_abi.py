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

    def test_public_declaration_is_typed_and_non_nullable(self) -> None:
        declaration = STRING_DECL.read_text(encoding="utf-8")
        generated = embedded_native_def("string")
        self.assertIn(
            "static fromUtf8(bytes: Slice<byte>) -> string "
            "@errors(Utf8Error.InvalidUtf8)",
            declaration,
        )
        self.assertIn(
            "sliceBytes(start: int, end: int) -> string "
            "@errors(StringSliceError.InvalidByteRange)",
            declaration,
        )
        self.assertNotIn("fromUtf8(bytes: Slice<byte>) -> string?", declaration)
        self.assertIn("@native\nenum Utf8Error", generated)
        self.assertIn("@native\nenum StringSliceError", generated)
        self.assertIn(
            "static fromUtf8(bytes: Slice<byte>) -> string "
            "@errors(Utf8Error.InvalidUtf8)",
            generated,
        )
        self.assertIn(
            "sliceBytes(start: int, end: int) -> string "
            "@errors(StringSliceError.InvalidByteRange)",
            generated,
        )
        self.assertNotIn("fromUtf8(bytes: Slice<byte>) -> string?", generated)

    def test_runtime_sources_route_failures_to_typed_error_channel(self) -> None:
        vm_runtime = VM_RUNTIME.read_text(encoding="utf-8")
        aot_runtime = AOT_RUNTIME.read_text(encoding="utf-8")

        self.assertIn("xr_utf8_scan_strict(data, len)", vm_runtime)
        self.assertIn(
            "string_set_builtin_enum_error(iso, XR_GLOBAL_VAR_UTF8_ERROR, 0",
            vm_runtime,
        )
        self.assertIn(
            "string_set_builtin_enum_error(iso, XR_GLOBAL_VAR_STRING_SLICE_ERROR, 0",
            vm_runtime,
        )
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

    def test_vm_native_aot_typed_catch_parity(self) -> None:
        vm = self.run_checked([str(self.xray), str(FIXTURE)]).stdout
        self.assertEqual(EXPECTED_OUTPUT, vm)

        output_dir = ROOT / "build" / ".xray-test-tmp"
        output_dir.mkdir(parents=True, exist_ok=True)
        native = output_dir / f"string_native_error_{os.getpid()}"
        cache = ROOT / "build" / ".xray-test-cache" / "task-198-native-error"
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
            aot = self.run_checked([str(native)]).stdout
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

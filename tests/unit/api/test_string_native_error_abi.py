#!/usr/bin/env python3
"""Focused task-198 VM/native-AOT typed string error ABI gate."""

from __future__ import annotations

import argparse
import os
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
FIXTURE = ROOT / "tests/aot/basic/string_utf8_conversion.xr"
STRING_DECL = ROOT / "stdlib/types/string.xr"
EXPECTED_OUTPUT = (
    "hi\n"
    "hé\n"
    "true\n"
    "A�B\n"
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
).encode()


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

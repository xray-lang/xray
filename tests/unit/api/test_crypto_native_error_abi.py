#!/usr/bin/env python3
"""Focused task-198 crypto.randomBytes typed native error ABI gate."""

from __future__ import annotations

import argparse
import os
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
FIXTURE = ROOT / "tests/aot/basic/crypto_random_bytes_typed_error.xr"
CORE_DEF = ROOT / "stdlib/defs/core.def"
VM_RUNTIME = ROOT / "stdlib/crypto/crypto.c"
AOT_RUNTIME = ROOT / "src/aot/xrt_crypto.h"
GENERATED = ROOT / "src/frontend/analyzer/xanalyzer_builtins_generated.h"
EXPECTED_OUTPUT = (
    b"true\n"
    b"CryptoError.InvalidLength\n"
    b"CryptoError.InvalidLength\n"
    b"CryptoError.InvalidLength\n"
)


class CryptoNativeErrorAbiTest(unittest.TestCase):
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

    def test_public_contract_is_typed_and_preswitch_string_return(self) -> None:
        core_def = CORE_DEF.read_text(encoding="utf-8")
        generated = GENERATED.read_text(encoding="utf-8")
        random_bytes_def = core_def[
            core_def.index("fn randomBytes {") : core_def.index("fn uuid {")
        ]

        self.assertIn('fn randomBytes {\n    signature: "(n: int): string"', core_def)
        self.assertIn('effect: "CryptoError.InvalidLength"', random_bytes_def)
        self.assertNotIn("@errors(", random_bytes_def)
        self.assertNotIn("Array<byte>", random_bytes_def)
        self.assertIn('"randomBytes", "(n: int): string"', generated)
        self.assertIn("XA_EFFECT_CONTRACT_ERRORS", generated)
        self.assertIn('"CryptoError.InvalidLength"', generated)

    def test_runtime_sources_route_invalid_length_to_typed_error_channel(self) -> None:
        vm_runtime = VM_RUNTIME.read_text(encoding="utf-8")
        aot_runtime = AOT_RUNTIME.read_text(encoding="utf-8")
        vm_random_bytes = vm_runtime[
            vm_runtime.index("static XrValue crypto_random_bytes") :
            vm_runtime.index("static XrValue crypto_uuid")
        ]
        aot_random_bytes = aot_runtime[
            aot_runtime.index("static inline XrValue xrt_crypto_random_bytes") :
            aot_runtime.index("static inline XrValue xrt_crypto_uuid")
        ]

        self.assertIn("XR_GLOBAL_VAR_CRYPTO_ERROR", vm_random_bytes)
        self.assertIn("crypto_set_builtin_enum_error", vm_random_bytes)
        self.assertIn('"CryptoError", "InvalidLength"', aot_random_bytes)
        self.assertIn("xrt_pending_error = xrt_enum_aggregate_box(err);", aot_runtime)
        self.assertNotIn("if (len <= 0 || len > 1024)\n        return xr_null();", vm_random_bytes)
        self.assertNotIn(
            "if (len64 <= 0 || len64 > 1024)\n        return XR_NULL_VAL;",
            aot_random_bytes,
        )

    def test_vm_native_aot_typed_catch_parity(self) -> None:
        vm = self.run_checked([str(self.xray), str(FIXTURE)]).stdout
        self.assertEqual(EXPECTED_OUTPUT, vm)

        output_dir = ROOT / "build" / ".xray-test-tmp"
        output_dir.mkdir(parents=True, exist_ok=True)
        native = output_dir / f"crypto_native_error_{os.getpid()}"
        cache = ROOT / "build" / ".xray-test-cache" / "task-198-crypto-native-error"
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
    CryptoNativeErrorAbiTest.xray = args.xray
    unittest.main(argv=[__file__, *unittest_args])


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Focused task-198 crypto.randomBytes typed native error ABI gate."""

from __future__ import annotations

import argparse
import os
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]

def _cache_root() -> Path:
    """Shared native-build cache. Lives outside build/ so a clean reconfigure
    does not throw away objects that cost ~30s each to recreate."""
    return Path(os.environ.get("XRAY_TEST_CACHE_ROOT", str(ROOT / ".cache" / "xray-test")))

RANDOM_BYTES_FIXTURE = ROOT / "tests/aot/basic/crypto_random_bytes_typed_error.xr"
DECRYPT_FIXTURE = ROOT / "tests/aot/basic/crypto_decrypt_typed_error.xr"
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
DECRYPT_EXPECTED_OUTPUT = (
    b"true\n"
    b"CryptoError.InvalidLength\n"
    b"CryptoError.InvalidLength\n"
    b"CryptoError.InvalidLength\n"
    b"true\n"
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
        decrypt_def = core_def[
            core_def.index("fn decrypt {") : core_def.index("fn timingSafeEqual {")
        ]

        self.assertIn('fn randomBytes {\n    signature: "(n: int): string"', core_def)
        self.assertIn('effect: "CryptoError.InvalidLength"', random_bytes_def)
        self.assertNotIn("@errors(", random_bytes_def)
        self.assertNotIn("Array<byte>", random_bytes_def)
        self.assertIn('fn decrypt {\n    signature: "(key: string, ciphertext: string): string?"', core_def)
        self.assertIn('effect: "CryptoError.InvalidLength"', decrypt_def)
        self.assertNotIn("@errors(", decrypt_def)
        self.assertIn('"randomBytes", "(n: int): string"', generated)
        self.assertIn('"decrypt", "(key: string, ciphertext: string): string?"', generated)
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
        vm_decrypt = vm_runtime[
            vm_runtime.index("static XrValue crypto_decrypt") :
            vm_runtime.index("// Constant-time comparison")
        ]
        aot_decrypt = aot_runtime[
            aot_runtime.index("static inline XrValue xrt_crypto_decrypt") :
            aot_runtime.index("static inline XrValue xrt_crypto_hmac")
        ]

        self.assertIn("XR_GLOBAL_VAR_CRYPTO_ERROR", vm_random_bytes)
        self.assertIn("crypto_set_builtin_enum_error", vm_random_bytes)
        self.assertIn("XR_GLOBAL_VAR_CRYPTO_ERROR", vm_decrypt)
        self.assertIn("crypto_set_builtin_enum_error", vm_decrypt)
        self.assertIn("crypto.decrypt invalid ciphertext length", vm_decrypt)
        self.assertIn('"CryptoError", "InvalidLength"', aot_random_bytes)
        self.assertIn('"CryptoError", "InvalidLength"', aot_decrypt)
        self.assertIn("xrt_pending_error = xrt_enum_aggregate_box(err);", aot_runtime)
        self.assertNotIn("if (len <= 0 || len > 1024)\n        return xr_null();", vm_random_bytes)
        self.assertNotIn(
            "if (len64 <= 0 || len64 > 1024)\n        return XR_NULL_VAL;",
            aot_random_bytes,
        )

    def assert_vm_native_aot_parity(self, fixture: Path, expected: bytes, native_stem: str) -> None:
        vm = self.run_checked([str(self.xray), str(fixture)]).stdout
        self.assertEqual(expected, vm)

        output_dir = ROOT / "build" / ".xray-test-tmp"
        output_dir.mkdir(parents=True, exist_ok=True)
        native = output_dir / f"{native_stem}_{os.getpid()}"
        cache = _cache_root() / native_stem
        try:
            self.run_checked(
                [
                    str(self.xray),
                    "build",
                    "--native",
                    "-O",
                    "0",
                    str(fixture),
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

    def test_vm_native_aot_typed_catch_parity(self) -> None:
        self.assert_vm_native_aot_parity(
            RANDOM_BYTES_FIXTURE, EXPECTED_OUTPUT, "task-198-crypto-random-native-error"
        )

    def test_decrypt_invalid_length_vm_native_aot_typed_catch_parity(self) -> None:
        self.assert_vm_native_aot_parity(
            DECRYPT_FIXTURE, DECRYPT_EXPECTED_OUTPUT, "task-198-crypto-decrypt-native-error"
        )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--xray", type=Path, default=ROOT / "build/xray")
    args, unittest_args = parser.parse_known_args()
    CryptoNativeErrorAbiTest.xray = args.xray
    unittest.main(argv=[__file__, *unittest_args])


if __name__ == "__main__":
    main()

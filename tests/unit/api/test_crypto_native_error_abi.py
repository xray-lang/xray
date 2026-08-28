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
CRYPTO_SOURCE = ROOT / "stdlib/crypto/crypto.xr"
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
        """randomBytes answers a hex string and refuses a bad length as a typed
        CryptoError.  Its contract moved to crypto.xr when the module's digests
        and CSPRNG surface became Xray; decrypt is still declared in core.def,
        so the two are checked against the source that owns each."""
        core_def = CORE_DEF.read_text(encoding="utf-8")
        generated = GENERATED.read_text(encoding="utf-8")
        crypto_source = CRYPTO_SOURCE.read_text(encoding="utf-8")
        random_bytes_body = crypto_source[
            crypto_source.index("export fn randomBytes(") : crypto_source.index("export fn uuid(")
        ]
        decrypt_def = core_def[
            core_def.index("fn decrypt {") : core_def.index("\n}\n", core_def.index("fn decrypt {"))
        ]

        self.assertIn("export fn randomBytes(n: i64) -> string", crypto_source)
        self.assertIn("throw CryptoError.InvalidLength", random_bytes_body)
        self.assertNotIn("Array<u8>", random_bytes_body)
        self.assertNotIn("fn randomBytes {", core_def)
        self.assertIn('fn decrypt {\n    signature: "(key: string, ciphertext: string): string?"', core_def)
        self.assertIn('effect: "CryptoError.InvalidLength"', decrypt_def)
        self.assertNotIn("@errors(", decrypt_def)
        self.assertIn('"decrypt", "(key: string, ciphertext: string): string?"', generated)
        self.assertIn("XA_EFFECT_CONTRACT_ERRORS", generated)
        self.assertIn('"CryptoError.InvalidLength"', generated)

    def test_runtime_sources_route_invalid_length_to_typed_error_channel(self) -> None:
        """decrypt is the remaining native entry point that reports a typed
        length error, so the VM and AOT runtimes must still publish it through
        the builtin-enum channel rather than answering a bare null.  The
        randomBytes half of this check moved to crypto.xr with the function."""
        vm_runtime = VM_RUNTIME.read_text(encoding="utf-8")
        aot_runtime = AOT_RUNTIME.read_text(encoding="utf-8")
        vm_decrypt = vm_runtime[
            vm_runtime.index("static XrValue crypto_decrypt") :
            vm_runtime.index("\n}\n", vm_runtime.index("static XrValue crypto_decrypt"))
        ]
        aot_decrypt = aot_runtime[
            aot_runtime.index("static inline XrValue xrt_crypto_decrypt") :
            aot_runtime.index("\n}\n", aot_runtime.index("static inline XrValue xrt_crypto_decrypt"))
        ]

        self.assertIn("XR_GLOBAL_VAR_CRYPTO_ERROR", vm_decrypt)
        self.assertIn("crypto_publish_builtin_enum_error", vm_decrypt)
        self.assertIn("crypto.decrypt invalid ciphertext length", vm_decrypt)
        self.assertIn("xr_builtin_enum_error_construct", vm_runtime)
        self.assertIn("if (ctx && !XR_IS_NULL(ctx->pending_error))", vm_runtime)
        self.assertIn("ctx->pending_error.ptr == result.value.ptr", vm_runtime)
        self.assertIn('"CryptoError", "InvalidLength"', aot_decrypt)
        self.assertIn("xrt_pending_error = xrt_enum_aggregate_box(err);", aot_runtime)

    def assert_vm_native_aot_parity(self, fixture: Path, expected: bytes, native_stem: str) -> None:
        vm = self.run_checked([str(self.xray), str(fixture)]).stdout.replace(b"\r\n", b"\n")
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
            aot = self.run_checked([str(native)]).stdout.replace(b"\r\n", b"\n")
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

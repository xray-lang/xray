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
        """randomBytes and decrypt each answer a typed CryptoError.  Both
        contracts now live in crypto.xr, randomBytes since the module's digests
        and CSPRNG surface became Xray and decrypt since its cipher surface
        followed, so both are checked against the source that owns them and
        core.def is checked for their absence."""
        core_def = CORE_DEF.read_text(encoding="utf-8")
        crypto_source = CRYPTO_SOURCE.read_text(encoding="utf-8")
        random_bytes_body = crypto_source[
            crypto_source.index("export fn randomBytes(") : crypto_source.index("export fn uuid(")
        ]
        decrypt_body = crypto_source[
            crypto_source.index("export fn decrypt(") :
            crypto_source.index("\n}\n", crypto_source.index("export fn decrypt("))
        ]

        self.assertIn("export fn randomBytes(n: i64) -> string", crypto_source)
        self.assertIn("throw CryptoError.InvalidLength", random_bytes_body)
        self.assertNotIn("Array<u8>", random_bytes_body)
        self.assertNotIn("fn randomBytes {", core_def)
        self.assertIn(
            "export fn decrypt(key: string, ciphertext: string) -> string?", crypto_source
        )
        self.assertIn("throw CryptoError.InvalidLength", decrypt_body)
        self.assertNotIn("fn decrypt {", core_def)

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

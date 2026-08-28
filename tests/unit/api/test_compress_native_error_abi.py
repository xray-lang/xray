#!/usr/bin/env python3
"""Focused task-198 compress decompressor typed native error ABI gate."""

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

FIXTURE = ROOT / "tests/aot/basic/compress_gunzip_typed_error.xr"
CORE_DEF = ROOT / "stdlib/defs/core.def"
VM_RUNTIME = ROOT / "stdlib/compress/compress.c"
AOT_RUNTIME = ROOT / "src/aot/xrt_compress.h"
GENERATED = ROOT / "src/frontend/analyzer/xanalyzer_builtins_generated.h"
EXPECTED_OUTPUT = (
    b"true\nCompressionError.InvalidData\n"
    b"true\nCompressionError.InvalidData\n"
    b"true\nCompressionError.InvalidData\n"
)


class CompressNativeErrorAbiTest(unittest.TestCase):
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

    def test_public_contract_is_typed_and_non_nullable(self) -> None:
        core_def = CORE_DEF.read_text(encoding="utf-8")
        generated = GENERATED.read_text(encoding="utf-8")
        gunzip_def = core_def[core_def.index("fn gunzip {") : core_def.index("fn deflate {")]
        inflate_def = core_def[core_def.index("fn inflate {") : core_def.index("fn zlibCompress {")]
        zlib_decompress_def = core_def[
            core_def.index("fn zlibDecompress {") : core_def.index("fn isGzip {")
        ]

        self.assertIn('fn gunzip {\n    signature: "(data: string): string"', core_def)
        self.assertIn('fn inflate {\n    signature: "(data: string): string"', core_def)
        self.assertIn('fn zlibDecompress {\n    signature: "(data: string): string"', core_def)
        self.assertIn('effect: "CompressionError.InvalidData"', gunzip_def)
        self.assertIn('effect: "CompressionError.InvalidData"', inflate_def)
        self.assertIn('effect: "CompressionError.InvalidData"', zlib_decompress_def)
        self.assertNotIn('fn gunzip {\n    signature: "(data: string): string?"', core_def)
        self.assertNotIn('fn inflate {\n    signature: "(data: string): string?"', core_def)
        self.assertNotIn('fn zlibDecompress {\n    signature: "(data: string): string?"', core_def)
        self.assertIn('"gunzip", "(data: string): string"', generated)
        self.assertIn('"inflate", "(data: string): string"', generated)
        self.assertIn('"zlibDecompress", "(data: string): string"', generated)
        self.assertIn("XA_EFFECT_CONTRACT_ERRORS", generated)
        self.assertIn('"CompressionError.InvalidData"', generated)

    def test_runtime_sources_route_failure_to_typed_error_channel(self) -> None:
        vm_runtime = VM_RUNTIME.read_text(encoding="utf-8")
        aot_runtime = AOT_RUNTIME.read_text(encoding="utf-8")
        vm_gunzip = vm_runtime[
            vm_runtime.index("static XrValue compress_gunzip") :
            vm_runtime.index("static XrValue compress_deflate")
        ]
        vm_inflate = vm_runtime[
            vm_runtime.index("static XrValue compress_inflate") :
            vm_runtime.index("static XrValue compress_zlib_compress")
        ]
        vm_zlib_decompress = vm_runtime[
            vm_runtime.index("static XrValue compress_zlib_decompress") :
            vm_runtime.index("static XrValue compress_is_gzip")
        ]
        aot_gunzip = aot_runtime[
            aot_runtime.index("static inline XrValue xrt_compress_gunzip") :
            aot_runtime.index("static inline XrValue xrt_compress_deflate")
        ]
        aot_inflate = aot_runtime[
            aot_runtime.index("static inline XrValue xrt_compress_inflate") :
            aot_runtime.index("static inline XrValue xrt_compress_zlib_compress")
        ]
        aot_zlib_decompress = aot_runtime[
            aot_runtime.index("static inline XrValue xrt_compress_zlib_decompress") :
            aot_runtime.index("static inline XrValue xrt_compress_is_gzip")
        ]

        self.assertIn("XR_GLOBAL_VAR_COMPRESSION_ERROR", vm_gunzip)
        self.assertIn("compress_publish_builtin_enum_error", vm_gunzip)
        self.assertIn("XR_GLOBAL_VAR_COMPRESSION_ERROR", vm_inflate)
        self.assertIn("compress_publish_builtin_enum_error", vm_inflate)
        self.assertIn("XR_GLOBAL_VAR_COMPRESSION_ERROR", vm_zlib_decompress)
        self.assertIn("compress_publish_builtin_enum_error", vm_zlib_decompress)
        self.assertIn("xr_builtin_enum_error_construct", vm_runtime)
        # The consuming half of this channel moved into the execution context and
        # its VM-side rediscovery was deleted with it, so the runtime source read
        # here no longer carries either. test_execution_error_channel covers it.
        self.assertIn('"CompressionError", "InvalidData"', aot_gunzip)
        self.assertIn('"CompressionError", "InvalidData"', aot_inflate)
        self.assertIn('"CompressionError", "InvalidData"', aot_zlib_decompress)
        self.assertIn("xrt_pending_error = xrt_enum_aggregate_box(err);", aot_runtime)
        self.assertNotIn("if (!output)\n        return xr_null();", vm_gunzip)
        self.assertNotIn("if (!output)\n        return xr_null();", vm_inflate)
        self.assertNotIn("if (!output)\n        return xr_null();", vm_zlib_decompress)
        self.assertNotIn("if (!buf)\n        return XR_NULL_VAL;", aot_gunzip)
        self.assertNotIn("if (!buf)\n        return XR_NULL_VAL;", aot_inflate)
        self.assertNotIn("if (!buf)\n        return XR_NULL_VAL;", aot_zlib_decompress)

    def test_vm_native_aot_typed_catch_parity(self) -> None:
        vm = self.run_checked([str(self.xray), str(FIXTURE)]).stdout.replace(b"\r\n", b"\n")
        self.assertEqual(EXPECTED_OUTPUT, vm)

        output_dir = ROOT / "build" / ".xray-test-tmp"
        output_dir.mkdir(parents=True, exist_ok=True)
        native = output_dir / f"compress_native_error_{os.getpid()}"
        cache = _cache_root() / "task-198-compress-native-error"
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
            aot = self.run_checked([str(native)]).stdout.replace(b"\r\n", b"\n")
        finally:
            native.unlink(missing_ok=True)

        self.assertEqual(vm, aot)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--xray", type=Path, default=ROOT / "build/xray")
    args, unittest_args = parser.parse_known_args()
    CompressNativeErrorAbiTest.xray = args.xray
    unittest.main(argv=[__file__, *unittest_args])


if __name__ == "__main__":
    main()

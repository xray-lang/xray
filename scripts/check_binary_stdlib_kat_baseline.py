#!/usr/bin/env python3
"""Check task-200 binary stdlib KAT and AOT baseline coverage.

This is a P0 coverage gate. It does not claim the legacy string-binary surface
is correct or final; it only makes sure the current RFC/base64, compression,
crypto, and AOT link-command evidence remains present while later task-200
phases replace that public surface with Slice/Array/fixed byte APIs.
"""

from __future__ import annotations

import argparse
import json
import re
from dataclasses import asdict, dataclass
from pathlib import Path


@dataclass(frozen=True)
class Baseline:
    category: str
    path: str
    contains: tuple[str, ...]
    regex: tuple[str, ...] = ()


@dataclass(frozen=True)
class CheckResult:
    category: str
    path: str
    ok: bool
    detail: str


BASELINES = (
    Baseline(
        category="BASE64_RFC4648_KAT",
        path="tests/regression/10_stdlib/1180_base64_basic.xr",
        contains=(
            'assert_eq(result, "SGVsbG8sIFdvcmxkIQ==")',
            'assert_eq(result, "QQ==")',
            'assert_eq(result, "QUI=")',
            'assert_eq(result, "QUJD")',
            'assert_eq(base64.isValid("A"), false)',
            "base64.encode(bytes[:])",
            'base64.decode("SGVs", optionalDecodeOptions())',
            'assert(decodeFails("!!!!"))',
        ),
    ),
    Baseline(
        category="BASE64_CONTRACT_CORPUS",
        path="tests/stdlib/contracts/base64/contract.toml",
        contains=(
            'id = "rfc4648-roundtrip"',
            'id = "typed-error-invalid-input"',
            'diff_cases_manifest = "tests/stdlib/contracts/base64/diff_cases.txt"',
        ),
    ),
    Baseline(
        category="BASE64_DIFF_CORPUS",
        path="tests/stdlib/contracts/base64/diff_cases.txt",
        contains=(
            "tests/diff/cases/semantics/stdlib/base64_module.xr",
            "tests/diff/cases/semantics/stdlib/base64_isvalid_direct.xr",
            "tests/diff/cases/semantics/stdlib/base64_property_direct.xr",
        ),
    ),
    Baseline(
        category="BASE64_PROPERTY_FUZZ",
        path="tests/regression/10_stdlib/1181_binary_codec_properties.xr",
        contains=(
            "test_base64_full_rfc4648_vectors",
            "test_base64_independent_all_byte_aggregate",
            "assert_eq(aggregate, 544432900)",
            "test_base64_deterministic_roundtrip_fuzz",
        ),
    ),
    Baseline(
        category="BASE64_TYPED_ERROR_DIFF",
        path="tests/diff/cases/semantics/stdlib/base64_property_direct.xr",
        contains=(
            "fn errorCode(text: string) -> int",
            "aggregate == 544432900",
            'print(errorCode("A"))',
            'print(errorCode("QR=="))',
            'print(errorCode("AA=A"))',
        ),
    ),
    Baseline(
        category="ENCODING_HEX_KAT",
        path="tests/regression/10_stdlib/1300_encoding.xr",
        contains=(
            'assert_eq(hexEncodeText("Hello"), "48656c6c6f")',
            'assert_eq(hexDecodeText("48656c6c6f"), "Hello")',
            'assert_eq(hexDecodeFails("gg"), true)',
            'assert_eq(hexDecodeFails("abc"), true)',
            "encoding.hexDecode(\"48656c6c6f\")",
        ),
    ),
    Baseline(
        category="ENCODING_UTF8_VALID_KAT",
        path="tests/regression/10_stdlib/1300_encoding.xr",
        contains=(
            'assert_eq(utf8ValidText("Hello"), true)',
            'assert_eq(utf8ValidText("你好"), true)',
            "overlong[0] = 0xC0",
            "surrogate[0] = 0xED",
            "encoding.utf8Valid(surrogate[:])",
        ),
    ),
    Baseline(
        category="ENCODING_UTF16_TYPED_ERROR_KAT",
        path="tests/regression/10_stdlib/1300_encoding.xr",
        contains=(
            "Utf16EncodeOptions",
            "Utf16DecodeOptions",
            "Utf16Error",
            "utf16DecodeFails(low[:])",
            "utf16DecodeFails(missing[:])",
            "utf16DecodeFails(invalid[:])",
            "consumeBom",
        ),
    ),
    Baseline(
        category="ENCODING_CONTRACT_CORPUS",
        path="tests/stdlib/contracts/encoding/contract.toml",
        contains=(
            'module = "encoding"',
            'id = "hex-byte-roundtrip"',
            'id = "strict-utf8-validation"',
            'id = "utf16-endian-bom-roundtrip"',
            'id = "utf16-typed-errors"',
            'diff_cases_manifest = "tests/stdlib/contracts/encoding/diff_cases.txt"',
        ),
    ),
    Baseline(
        category="ENCODING_DIFF_CORPUS",
        path="tests/stdlib/contracts/encoding/diff_cases.txt",
        contains=(
            "tests/diff/cases/semantics/stdlib/encoding_module.xr",
            "tests/diff/cases/semantics/stdlib/encoding_core_direct.xr",
            "tests/diff/cases/semantics/stdlib/encoding_property_direct.xr",
        ),
    ),
    Baseline(
        category="ENCODING_PROPERTY_FUZZ",
        path="tests/regression/10_stdlib/1181_binary_codec_properties.xr",
        contains=(
            "test_hex_full_byte_kat",
            "test_hex_deterministic_roundtrip_fuzz",
            "test_utf8_exhaustive_single_and_two_byte_space",
            "test_utf8_scalar_boundaries",
            "test_utf16_supplementary_kat_and_roundtrip",
        ),
    ),
    Baseline(
        category="ENCODING_TYPED_ERROR_DIFF",
        path="tests/diff/cases/semantics/stdlib/encoding_property_direct.xr",
        contains=(
            "fn hexErrorCode(text: string) -> int",
            "fn utf16ErrorCode(data: Slice<byte>) -> int",
            'print(hexErrorCode("abc"))',
            "print(utf16ErrorCode(invalidPair[:]))",
        ),
    ),
    Baseline(
        category="BINARY_CODEC_RANDOMIZED_FUZZ_HARNESS",
        path="tests/diff/fuzz_binary_stdlib.py",
        contains=(
            "Seeded cross-oracle fuzzing for task-200 pure binary codecs.",
            "base64.b64encode(data)",
            "base64.urlsafe_b64encode(data)",
            'data.decode("utf-8", "strict")',
            '[str(xray), "run", str(source_path)]',
            '[str(xray), "build", "--native", str(source_path), "-o", str(binary)]',
        ),
    ),
    Baseline(
        category="BINARY_NATIVE_RANDOMIZED_FUZZ_HARNESS",
        path="tests/diff/fuzz_binary_native_stdlib.py",
        contains=(
            "Seeded cross-oracle fuzzing for task-200 native binary stdlib baselines.",
            "zlib.crc32(data)",
            "zlib.adler32(data)",
            "hashlib.sha256(data).hexdigest()",
            "hmac.new(key, data, getattr(hashlib, algo)).hexdigest()",
            "compress.gzip(text",
            "crypto.sha512(text",
            '[str(xray), "run", str(source_path)]',
            '[str(xray), "build", "--native", str(source_path), "-o", str(binary)]',
        ),
    ),
    Baseline(
        category="COMPRESS_CHECKSUM_KAT",
        path="tests/regression/10_stdlib/1301_compress.xr",
        contains=(
            'compress.crc32("123456789")',
            "assert_eq(crc, 3421780262)",
            'compress.adler32("Wikipedia")',
            "assert_eq(adler, 300286872)",
            "compress.gzip(data)",
            "compress.deflate(data)",
            "compress.zlibCompress(data)",
        ),
    ),
    Baseline(
        category="COMPRESS_CONTRACT_CORPUS",
        path="tests/stdlib/contracts/compress/contract.toml",
        contains=(
            'module = "compress"',
            'id = "checksum-known-values"',
            'id = "roundtrip-formats"',
            'id = "nullable-decompression-failure"',
            'id = "string-binary-payloads"',
            'diff_cases_manifest = "tests/stdlib/contracts/compress/diff_cases.txt"',
        ),
    ),
    Baseline(
        category="COMPRESS_DIFF_CORPUS",
        path="tests/stdlib/contracts/compress/diff_cases.txt",
        contains=(
            "tests/diff/cases/semantics/stdlib/compress_checksum_direct.xr",
            "tests/diff/cases/semantics/stdlib/compress_roundtrip_direct.xr",
        ),
    ),
    Baseline(
        category="CRYPTO_HASH_KAT",
        path="tests/regression/10_stdlib/1400_crypto_hash.xr",
        contains=(
            '"d41d8cd98f00b204e9800998ecf8427e"',
            '"da39a3ee5e6b4b0d3255bfef95601890afd80709"',
            '"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"',
        ),
    ),
    Baseline(
        category="CRYPTO_SHA512_HMAC_KAT",
        path="tests/regression/10_stdlib/1403_crypto_sha512.xr",
        contains=(
            '"cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"',
            '"164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea2505549758bf75c05a994a6d034f65f8f0e6fdcaeab1a34d4a6b4b636e070a38bce737"',
        ),
    ),
    Baseline(
        category="CRYPTO_HMAC_KAT",
        path="tests/regression/10_stdlib/1401_crypto_hmac.xr",
        contains=(
            '"6e9ef29b75fffc5b7abae527d58fdadb2fe42e7219011976917343065f58ed4a"',
            '"2088df74d5f2146b48146caf4965377e9d0be3a4"',
            '"4e4748e62b463521f6775fbf921234b5"',
        ),
    ),
    Baseline(
        category="CRYPTO_ENCRYPTION_BASELINE",
        path="tests/regression/10_stdlib/1404_crypto_aes.xr",
        contains=(
            "crypto.encrypt(key, plaintext)",
            "crypto.decrypt(key, encrypted)",
            'crypto.decrypt("wrong-key", encrypted)',
            "assert_eq(result, null)",
        ),
    ),
    Baseline(
        category="CRYPTO_TIMING_BASELINE",
        path="tests/regression/10_stdlib/1405_crypto_utils.xr",
        contains=(
            'crypto.timingSafeEqual("hello", "hello")',
            'crypto.timingSafeEqual("short", "longer string")',
            'crypto.hmac("sha256", "secret", "data")',
        ),
    ),
    Baseline(
        category="CRYPTO_CONTRACT_CORPUS",
        path="tests/stdlib/contracts/crypto/contract.toml",
        contains=(
            'module = "crypto"',
            'id = "hash-and-hmac-kat"',
            'id = "timing-safe-equal"',
            'id = "random-shape"',
            'id = "hex-digest-presentation"',
            'id = "ambiguous-encrypt-decrypt"',
            'diff_cases_manifest = "tests/stdlib/contracts/crypto/diff_cases.txt"',
        ),
    ),
    Baseline(
        category="CRYPTO_DIFF_CORPUS",
        path="tests/stdlib/contracts/crypto/diff_cases.txt",
        contains=(
            "tests/diff/cases/semantics/stdlib/crypto_timing_safe_equal_direct.xr",
            "tests/diff/cases/semantics/stdlib/crypto_random_system_direct.xr",
        ),
    ),
    Baseline(
        category="AOT_BASE64_LINK_BASELINE",
        path="tests/aot/filetests/link/core_base64.expect",
        contains=(
            '"stdlib_symbols": ["base64.Base64Alphabet", "base64.Base64DecodeOptions", "base64.Base64EncodeOptions", "base64.Base64Error", "base64.Base64PaddingPolicy", "base64.encode", "base64.decode", "base64.isValid"]',
            'not_contains="runtime_objects": ["xray_core"]',
            "c_not_contains=xrt_base64_",
        ),
    ),
    Baseline(
        category="AOT_ENCODING_LINK_BASELINE",
        path="tests/aot/filetests/link/core_encoding.expect",
        contains=(
            '"stdlib_symbols": ["encoding.HexError", "encoding.Utf16DecodeOptions", "encoding.Utf16EncodeOptions", "encoding.Utf16Error", "encoding.hexEncode", "encoding.hexDecode", "encoding.utf8Valid", "encoding.utf16Decode", "encoding.hexValid", "encoding.utf16Encode"]',
            'not_contains="runtime_objects": ["xray_core"]',
            'not_contains="stdlib_objects": ["encoding"]',
            "c_not_contains=xrt_encoding_",
        ),
    ),
    Baseline(
        category="AOT_COMPRESS_LINK_BASELINE",
        path="tests/aot/filetests/link/core_compress.expect",
        contains=(
            '"stdlib_symbols": ["compress.crc32", "compress.adler32", "compress.gzip", "compress.isGzip", "compress.gunzip", "compress.deflate", "compress.inflate", "compress.zlibCompress", "compress.isZlib", "compress.zlibDecompress"]',
            "c_contains=xrt_compress_crc32(",
            "c_contains=xrt_compress_zlib_decompress(",
            "c_not_contains=xrt_method_",
        ),
    ),
    Baseline(
        category="AOT_CRYPTO_LINK_BASELINE",
        path="tests/aot/filetests/link/core_crypto.expect",
        contains=(
            '"stdlib_symbols": ["crypto.timingSafeEqual", "crypto.encrypt", "crypto.decrypt", "crypto.md5", "crypto.sha1", "crypto.sha256", "crypto.sha512", "crypto.hmac"]',
            "c_contains=xrt_crypto_sha512(",
            "c_contains=xrt_crypto_hmac(",
            "c_not_contains=xrt_method_",
        ),
    ),
    Baseline(
        category="AOT_CRYPTO_RANDOM_LINK_BASELINE",
        path="tests/aot/filetests/link/system_crypto_random.expect",
        contains=(
            '"stdlib_symbols": ["crypto.randomBytes", "crypto.uuid"]',
            "c_contains=xrt_crypto_random_bytes(",
            "c_contains=xrt_crypto_uuid(",
            "c_not_contains=xrt_method_",
        ),
    ),
    Baseline(
        category="STDLIB_BASE64_BENCH_BASELINE",
        path="tests/benchmarks/stdlib/manifest.toml",
        contains=(
            'id = "base64.contract"',
            'module = "base64"',
            'source = "tests/diff/cases/semantics/stdlib/base64_module.xr"',
            'compare = ["vm", "aot"]',
        ),
    ),
)


def check_contains(text: str, needles: tuple[str, ...]) -> list[str]:
    return [needle for needle in needles if needle not in text]


def check_regex(text: str, patterns: tuple[str, ...]) -> list[str]:
    missing: list[str] = []
    for pattern in patterns:
        if not re.search(pattern, text, re.MULTILINE):
            missing.append(pattern)
    return missing


def check_baseline(root: Path, baseline: Baseline) -> CheckResult:
    path = root / baseline.path
    if not path.is_file():
        return CheckResult(baseline.category, baseline.path, False, "missing fixture")

    text = path.read_text(encoding="utf-8")
    missing_contains = check_contains(text, baseline.contains)
    missing_regex = check_regex(text, baseline.regex)
    if missing_contains or missing_regex:
        detail_parts = []
        if missing_contains:
            detail_parts.append("missing anchors: " + ", ".join(missing_contains))
        if missing_regex:
            detail_parts.append("missing regex: " + ", ".join(missing_regex))
        return CheckResult(baseline.category, baseline.path, False, "; ".join(detail_parts))
    return CheckResult(baseline.category, baseline.path, True, "anchors ok")


def build_results(root: Path) -> list[CheckResult]:
    return [check_baseline(root, baseline) for baseline in BASELINES]


def print_text(results: list[CheckResult]) -> None:
    print("Task 200 binary stdlib KAT/AOT baseline coverage")
    for result in results:
        status = "ok" if result.ok else "missing"
        print(f"{result.category}: {status}: {result.path}: {result.detail}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--json", action="store_true", help="print machine-readable JSON")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    results = build_results(root)

    if args.json:
        print(json.dumps([asdict(result) for result in results], indent=2, sort_keys=True))
    else:
        print_text(results)
    return 0 if all(result.ok for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())

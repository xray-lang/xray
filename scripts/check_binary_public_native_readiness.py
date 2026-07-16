#!/usr/bin/env python3
"""Check task-200 public-native switch readiness without cutting the switch.

This gate is intentionally conservative.  Task 200 still depends on task 197
Slice provenance and task 198 typed native error ABI before compress/crypto/io
and net can expose final public byte APIs.  The gate therefore proves that the
current branch has the inventory, contract, fuzz, AOT, generated-residue, and
owner metadata needed for a later switch, while also failing if that switch is
silently cut early.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any


BINARY_MODULES = ("base64", "encoding", "compress", "crypto", "io", "net", "http", "ws")
PRE_SWITCH_NATIVE_MODULES = {
    "compress": "native_library",
    "crypto": "native_library",
    "io": "native_primitive",
    "net": "native_primitive",
}
PRE_SWITCH_NATIVE_PUBLIC_SURFACE = {
    "compress": (
        "adler32",
        "crc32",
        "deflate",
        "gunzip",
        "gzip",
        "inflate",
        "isGzip",
        "isZlib",
        "zlibCompress",
        "zlibDecompress",
    ),
    "crypto": (
        "decrypt",
        "encrypt",
        "hmac",
        "md5",
        "randomBytes",
        "sha1",
        "sha256",
        "sha512",
        "timingSafeEqual",
        "uuid",
    ),
    "io": (
        "FileStat",
        "appendFile",
        "chdir",
        "chmod",
        "copyFile",
        "cwd",
        "exists",
        "fileSize",
        "isDir",
        "isFile",
        "isSymlink",
        "mkdir",
        "mkdirp",
        "readDir",
        "readDirRecursive",
        "readFile",
        "readFileBytes",
        "readLines",
        "readStdin",
        "readlink",
        "realpath",
        "remove",
        "removeAll",
        "rename",
        "stat",
        "symlink",
        "tempDir",
        "tempFile",
        "touch",
        "writeFile",
        "writeFileBytes",
    ),
    "net": (
        "NetConn",
        "NetConn.close",
        "NetConn.fd",
        "NetConn.isClosed",
        "NetConn.isTLS",
        "NetListener",
        "NetListener.close",
        "NetListener.fd",
        "NetListener.isClosed",
        "NetListener.port",
        "UdpPacket",
        "accept",
        "close",
        "copy",
        "copyBidirectional",
        "dial",
        "dialTLS",
        "fd",
        "hasTLS",
        "lastErrno",
        "lastError",
        "listen",
        "lookup",
        "read",
        "readInto",
        "recvFrom",
        "sendTo",
        "setAcceptDeadline",
        "setDeadline",
        "setReadDeadline",
        "setWriteDeadline",
        "shutdown",
        "shutdownRead",
        "shutdownWrite",
        "udpBind",
        "upgradeTLS",
        "write",
        "writeBytes",
    ),
}
PURE_BYTE_MODULES = ("base64", "encoding")
REQUIRED_CONTRACT_MODULES = ("base64", "encoding", "compress", "crypto", "io", "net", "http", "ws")
REQUIRED_SURFACE_CATEGORIES = (
    "PUBLIC_BINARY_STRING_SIGNATURE",
    "PUBLIC_ARBITRARY_STRING_CREATOR",
    "PUBLIC_NULL_SENTINEL",
    "CONSUMER_OLD_BINARY_API_CALL",
    "NATIVE_ARBITRARY_STRING_CREATOR",
    "GENERATED_METADATA_STALE_BINARY_SURFACE",
)
DEPENDENCY_MARKERS = (
    "TASK_197_SLICE_PROVENANCE_READY",
    "TASK_198_TYPED_NATIVE_ERRORS_READY",
)
GENERATED_METADATA_FILES = (
    "src/frontend/analyzer/xanalyzer_builtins_generated.h",
    "src/app/lsp/xlsp_stdlib_generated.inc",
    "src/app/mcp/xmcp_knowledge_generated.c",
)
REQUIRED_BENCHMARKS = {
    "base64.contract": ("base64", "stdlib/base64", "tests/diff/cases/semantics/stdlib/base64_module.xr"),
    "encoding.contract": ("encoding", "stdlib/encoding", "tests/diff/cases/semantics/stdlib/encoding_module.xr"),
    "compress.contract": (
        "compress",
        "stdlib/compress",
        "tests/diff/cases/semantics/stdlib/compress_roundtrip_direct.xr",
    ),
    "crypto.contract": (
        "crypto",
        "stdlib/crypto",
        "tests/diff/cases/semantics/stdlib/crypto_timing_safe_equal_direct.xr",
    ),
}
REQUIRED_ORACLE_CASES = {
    "compress": {
        "checksum-kat": "tests/diff/cases/semantics/stdlib/compress_checksum_direct.xr",
        "format-roundtrip-and-invalid": "tests/diff/cases/semantics/stdlib/compress_roundtrip_direct.xr",
        "truncated-input-classification": "tests/diff/cases/semantics/stdlib/compress_truncated_direct.xr",
        "seeded-cross-oracle-fuzz": "tests/diff/fuzz_binary_native_stdlib.py",
    },
    "crypto": {
        "hash-hmac-aes-and-timing": "tests/diff/cases/semantics/stdlib/crypto_timing_safe_equal_direct.xr",
        "hex-digest-presentation": "tests/regression/10_stdlib/1403_crypto_sha512.xr",
        "random-shape": "tests/diff/cases/semantics/stdlib/crypto_random_system_direct.xr",
        "seeded-cross-oracle-fuzz": "tests/diff/fuzz_binary_native_stdlib.py",
    },
    "io": {
        "filesystem-text-and-bytes": "tests/diff/cases/semantics/stdlib/io_system_direct.xr",
        "all-byte-file-boundary": "tests/diff/cases/semantics/stdlib/io_binary_file_boundary_direct.xr",
    },
    "net": {
        "high-byte-loopback": "tests/regression/10_stdlib/1433_net_loopback.xr",
        "native-copy-byte-boundary": "tests/regression/10_stdlib/1433_net_loopback.xr",
    },
    "http": {
        "request-body-framing": "tests/diff/cases/semantics/stdlib/http_request_message_pure_direct.xr",
        "response-body-framing": "tests/diff/cases/semantics/stdlib/http_response_text_pure_direct.xr",
    },
    "ws": {
        "websocket-protocol-bytes": "tests/diff/cases/semantics/stdlib/ws_pure_protocol_direct.xr",
    },
}
REQUIRED_TEXT_ANCHORS = {
    "tests/regression/10_stdlib/1433_net_loopback.xr": (
        "test_loopback_binary_high_bytes",
        "assert_eq(resp.bytes()[0]!, 195)",
        "assert_eq(resp.bytes()[4]!, 172)",
        "test_native_copy_loopback",
    ),
    "tests/regression/10_stdlib/1181_binary_codec_properties.xr": (
        "test_base64_independent_all_byte_aggregate",
        "test_base64_deterministic_roundtrip_fuzz",
        "test_hex_full_byte_kat",
        "test_utf8_exhaustive_single_and_two_byte_space",
    ),
}
PARTIAL_DEPENDENCY_EVIDENCE = {
    "TASK_198_ANALYZER_ONLY": {
        "required": {
            "src/frontend/analyzer/xanalyzer_errorset.c": (
                "es_apply_native_call_contract",
                "contract->errors[i]",
            ),
            "tests/unit/analyzer/test_analyzer.c": (
                "analyzer_error_effect_consumes_xrd_native_contracts",
                "@errors(NativeErr.Boom)",
                "analyzer_xrd_native_typed_byte_contracts_reject_legacy_aliases",
            ),
        },
        "full_marker": "TASK_198_TYPED_NATIVE_ERRORS_READY",
        "detail": "analyzer/XRD typed-error evidence exists, but runtime/AOT typed native ABI is not marked ready",
    },
    "TASK_198_STRING_RUNTIME_ONLY": {
        "required": {
            "tests/stdlib/contracts/TASK_198_STRING_NATIVE_ERROR_ABI_READY": (
                "scope: task-198 string native error ABI only",
                "string.fromUtf8(Slice<byte>) -> string throws Utf8Error.InvalidUtf8",
                "public-native switch remains blocked",
            ),
            "tests/unit/api/test_string_native_error_abi.py": (
                "Focused task-198 VM/native-AOT typed string error ABI gate.",
                "def test_vm_native_aot_typed_catch_parity",
                "Utf8Error.InvalidUtf8",
                "StringSliceError.InvalidByteRange",
                "self.assertEqual(vm, aot)",
            ),
            "tests/unit/CMakeLists.txt": (
                "NAME test_string_native_error_abi",
                'LABELS "vm;aot;frontend;task-198"',
            ),
            "stdlib/types/string.xr": (
                "static fromUtf8(bytes: Slice<byte>) -> string @errors(Utf8Error.InvalidUtf8)",
                "sliceBytes(start: int, end: int) -> string @errors(StringSliceError.InvalidByteRange)",
            ),
            "tests/aot/basic/string_utf8_conversion.xr": (
                "catch (e: Utf8Error)",
                "catch (e: StringSliceError)",
                'print(invalid_utf8_error)',
                'print(oob_error)',
            ),
            "src/runtime/object/xstring_methods.c": (
                "string_set_builtin_enum_error(iso, XR_GLOBAL_VAR_UTF8_ERROR, 0,",
                "string_set_builtin_enum_error(iso, XR_GLOBAL_VAR_STRING_SLICE_ERROR, 0,",
                '{"fromUtf8", m_from_utf8, 1}',
                '{"sliceBytes", m_slice_bytes, 2}',
            ),
            "src/aot/xrt_method.h": (
                'xrt_set_builtin_enum_error("Utf8Error", "InvalidUtf8", 0);',
                'xrt_set_builtin_enum_error("StringSliceError", "InvalidByteRange", 0);',
            ),
        },
        "full_marker": "TASK_198_TYPED_NATIVE_ERRORS_READY",
        "detail": "string VM/AOT typed-error runtime probe exists and scoped marker is present, but compress/crypto/io/net native typed-error ABI is not marked ready",
    },
    "TASK_197_VERIFIER_ONLY": {
        "required": {
            "src/aot/xaot_storage_plan.c": (
                "AOT address provenance permits a lifetime escape",
                "XR_POINTER_ESCAPE_CALL_BOUND",
            ),
            "tests/compile_errors/type/span_active_borrow_owner_mutation.xr.expected": (
                "cannot mutate owner 'bytes' while Slice view 'view' is active",
            ),
        },
        "full_marker": "TASK_197_SLICE_PROVENANCE_READY",
        "detail": "borrow/provenance verifier evidence exists, but full Slice public-switch provenance is not marked ready",
    },
}
NATIVE_TYPED_ERROR_ABI_BLOCKERS = {
    "compress": {
        "required": {
            "tests/stdlib/contracts/compress/contract.toml": (
                "decompression failures must become typed CompressionError values",
                "legacy decompression failure is reported through nullable output",
                "future converged decompression APIs throw CompressionError",
            ),
            "stdlib/defs/core.def": (
                'signature: "(data: string, level?: int): string?"',
                "xrt_compress_gzip_default",
                "xrt_compress_gunzip",
                "xrt_compress_zlib_decompress",
            ),
        },
        "detail": "compress typed native error ABI remains blocked by nullable/string compression signatures",
    },
    "crypto": {
        "required": {
            "tests/stdlib/contracts/crypto/contract.toml": (
                "randomBytes returns Array<byte> and reports native failures through a typed error",
                "legacy randomBytes returns hex text rather than owned random bytes",
            ),
            "stdlib/defs/core.def": (
                'signature: "(n: int): string"',
                "crypto_random_bytes",
                "xrt_crypto_random_bytes",
            ),
        },
        "detail": "crypto typed native error ABI remains blocked by string randomBytes output",
    },
    "io": {
        "required": {
            "tests/stdlib/contracts/io/contract.toml": (
                "readFileBytes and writeFileBytes preserve arbitrary byte values",
                "future converged binary I/O must not add string fallbacks",
            ),
            "stdlib/defs/core.def": (
                'signature: "(path: Path): Array<byte>?"',
                'signature: "(path: Path, data: Array<byte>): bool"',
                "io_readFileBytes",
                "io_writeFileBytes",
            ),
        },
        "detail": "io typed native error ABI remains blocked by nullable/bool file sentinels",
    },
    "net": {
        "required": {
            "stdlib/defs/core.def": (
                'signature: "(host: string, port: int, timeout?: int): NetConn?"',
                'signature: "(conn: NetConn, maxlen?: int): string?"',
                'signature: "(handle: NetConn, data: string, host: string, port: int): int"',
                'signature: "(handle: NetConn, maxlen?: int): UdpPacket?"',
            ),
            "stdlib/net/xneterror.h": (
                "} XrNetError;",
                "XR_NERR_",
            ),
            "tests/regression/10_stdlib/1433_net_loopback.xr": (
                "test_loopback_binary_high_bytes",
                "test_native_copy_loopback",
            ),
        },
        "detail": "net typed native error ABI remains blocked by nullable/string/status sentinels",
    },
}
SINGLE_FUNCTION_NATIVE_TYPED_ERROR_PROBES = {
    "compress.gunzip": {
        "module": "compress",
        "function": "gunzip",
        "typed_error": "CompressionError",
        "required": {
            "tests/stdlib/contracts/compress/contract.toml": (
                "decompression failures must become typed CompressionError values",
                "nullable-decompression-failure",
                "future converged decompression APIs throw CompressionError",
            ),
            "stdlib/defs/core.def": (
                "fn gunzip {",
                'signature: "(data: string): string"',
                'effect: "CompressionError.InvalidData"',
                'vm: "compress_gunzip"',
                'aot: "xrt_compress_gunzip"',
            ),
            "stdlib/compress/compress.c": (
                "static XrValue compress_gunzip",
                "uint8_t *output = xr_gunzip_alloc",
                "compress_set_builtin_enum_error(X, XR_GLOBAL_VAR_COMPRESSION_ERROR, 0",
            ),
            "src/aot/xrt_compress.h": (
                "static inline XrValue xrt_compress_gunzip",
                "uint8_t *buf = xr_compress_core_gunzip_alloc",
                'xrt_compress_set_builtin_enum_error("CompressionError", "InvalidData", 0);',
            ),
            "src/shared/xr_compress_core.h": (
                "XrCompressError err = fn(input, in_len, output, cap, out_len);",
                "if (err != XR_COMPRESS_ERR_BUFFER)\n            return NULL;",
            ),
            "tests/unit/api/test_compress_native_error_abi.py": (
                "Focused task-198 compress decompressor typed native error ABI gate.",
                "def test_vm_native_aot_typed_catch_parity",
                "CompressionError.InvalidData",
            ),
        },
        "allow_typed_error_before_full_marker": True,
        "detail": (
            "compress.gunzip has a focused VM/native-AOT typed CompressionError ABI slice; "
            "full task-198 readiness remains blocked by the rest of compress/crypto/io/net"
        ),
    },
    "compress.inflate": {
        "module": "compress",
        "function": "inflate",
        "typed_error": "CompressionError",
        "required": {
            "tests/stdlib/contracts/compress/contract.toml": (
                "decompression failures must become typed CompressionError values",
                "nullable-decompression-failure",
                "future converged decompression APIs throw CompressionError",
            ),
            "stdlib/defs/core.def": (
                "fn inflate {",
                'signature: "(data: string): string"',
                'effect: "CompressionError.InvalidData"',
                'vm: "compress_inflate"',
                'aot: "xrt_compress_inflate"',
            ),
            "stdlib/compress/compress.c": (
                "static XrValue compress_inflate",
                "uint8_t *output = xr_compress_core_inflate_alloc",
                "compress_set_builtin_enum_error(X, XR_GLOBAL_VAR_COMPRESSION_ERROR, 0",
            ),
            "src/aot/xrt_compress.h": (
                "static inline XrValue xrt_compress_inflate",
                "uint8_t *buf = xr_compress_core_inflate_alloc",
                'xrt_compress_set_builtin_enum_error("CompressionError", "InvalidData", 0);',
            ),
            "tests/unit/api/test_compress_native_error_abi.py": (
                "Focused task-198 compress decompressor typed native error ABI gate.",
                "def test_vm_native_aot_typed_catch_parity",
                "CompressionError.InvalidData",
            ),
        },
        "allow_typed_error_before_full_marker": True,
        "detail": (
            "compress.inflate has a focused VM/native-AOT typed CompressionError ABI slice; "
            "full task-198 readiness remains blocked by the rest of compress/crypto/io/net"
        ),
    },
    "compress.zlibDecompress": {
        "module": "compress",
        "function": "zlibDecompress",
        "typed_error": "CompressionError",
        "required": {
            "tests/stdlib/contracts/compress/contract.toml": (
                "decompression failures must become typed CompressionError values",
                "nullable-decompression-failure",
                "future converged decompression APIs throw CompressionError",
            ),
            "stdlib/defs/core.def": (
                "fn zlibDecompress {",
                'signature: "(data: string): string"',
                'effect: "CompressionError.InvalidData"',
                'vm: "compress_zlib_decompress"',
                'aot: "xrt_compress_zlib_decompress"',
            ),
            "stdlib/compress/compress.c": (
                "static XrValue compress_zlib_decompress",
                "uint8_t *output = xr_compress_core_zlib_decompress_alloc",
                "compress_set_builtin_enum_error(X, XR_GLOBAL_VAR_COMPRESSION_ERROR, 0",
            ),
            "src/aot/xrt_compress.h": (
                "static inline XrValue xrt_compress_zlib_decompress",
                "uint8_t *buf = xr_compress_core_zlib_decompress_alloc",
                'xrt_compress_set_builtin_enum_error("CompressionError", "InvalidData", 0);',
            ),
            "tests/unit/api/test_compress_native_error_abi.py": (
                "Focused task-198 compress decompressor typed native error ABI gate.",
                "def test_vm_native_aot_typed_catch_parity",
                "zlibDecompress",
                "CompressionError.InvalidData",
            ),
        },
        "allow_typed_error_before_full_marker": True,
        "detail": (
            "compress.zlibDecompress has a focused VM/native-AOT typed CompressionError ABI slice; "
            "full task-198 readiness remains blocked by the rest of compress/crypto/io/net"
        ),
    },
    "crypto.randomBytes": {
        "module": "crypto",
        "function": "randomBytes",
        "typed_error": "CryptoError",
        "required": {
            "tests/stdlib/contracts/crypto/contract.toml": (
                "randomBytes returns Array<byte> and reports native failures through a typed error",
                "legacy randomBytes returns hex text rather than owned random bytes",
            ),
            "stdlib/defs/core.def": (
                "fn randomBytes {",
                'signature: "(n: int): string"',
                'effect: "CryptoError.InvalidLength"',
                'vm: "crypto_random_bytes"',
                'aot: "xrt_crypto_random_bytes"',
            ),
            "stdlib/crypto/crypto.c": (
                "static XrValue crypto_random_bytes",
                "crypto_set_builtin_enum_error(isolate, XR_GLOBAL_VAR_CRYPTO_ERROR, 0",
                "char hex[2049];",
                "return xr_string_value(xr_string_new(isolate, hex, len * 2));",
            ),
            "src/aot/xrt_crypto.h": (
                "static inline XrValue xrt_crypto_random_bytes",
                'xrt_crypto_set_builtin_enum_error("CryptoError", "InvalidLength", 0);',
                "char hex[2049];",
                "XrValue result = xrt_str_alloc(len * 2);",
                "memcpy(xr_str_buf(result), hex, len * 2);",
            ),
            "tests/unit/api/test_crypto_native_error_abi.py": (
                "Focused task-198 crypto.randomBytes typed native error ABI gate.",
                "def test_vm_native_aot_typed_catch_parity",
                "CryptoError.InvalidLength",
            ),
            "tests/aot/basic/crypto_random_bytes_typed_error.xr": (
                "catch (e: CryptoError)",
                "CryptoError.InvalidLength",
            ),
            "tests/diff/cases/semantics/stdlib/crypto_random_system_direct.xr": (
                "var r8 = crypto.randomBytes(8)",
                "print(len(r8) == 16)",
                "print(len(r16) == 32)",
            ),
        },
        "allow_typed_error_before_full_marker": True,
        "detail": (
            "crypto.randomBytes has a focused VM/native-AOT typed CryptoError ABI slice while "
            "remaining a pre-switch hex-string native path; full task-198/task-200 readiness "
            "remains blocked by the future Array<byte> public switch"
        ),
    },
}
SINGLE_FUNCTION_PUBLIC_SURFACE_PROBES = {
    "crypto.sha1": {
        "module": "crypto",
        "function": "sha1",
        "required": {
            "tests/stdlib/contracts/crypto/contract.toml": (
                "fixed digest APIs return fixed byte arrays and use encoding.hexEncode only for presentation",
                "legacy hash and HMAC APIs conflate digest bytes with lowercase hex presentation",
                "future converged digest APIs return fixed byte arrays; hex strings are produced by encoding.hexEncode at call sites",
            ),
            "tests/stdlib/contracts/crypto/cases.jsonl": (
                "hash-hmac-aes-and-timing",
                "published digest/HMAC vectors including RFC 4231",
            ),
            "stdlib/defs/core.def": (
                "fn sha1 {",
                'signature: "(data: string): string"',
                'vm: "crypto_sha1"',
                'aot: "xrt_crypto_sha1"',
            ),
            "stdlib/crypto/crypto.c": (
                "static XrValue crypto_sha1",
                "return crypto_hash_value(isolate, args, nargs, XR_CRYPTO_CORE_HASH_SHA1);",
                "return crypto_hex_string_result(isolate, digest, digest_len);",
            ),
            "src/aot/xrt_crypto.h": (
                "static inline XrValue xrt_crypto_sha1",
                "return xrt_crypto_hash_string(data, len, XR_CRYPTO_CORE_HASH_SHA1);",
                "return xrt_crypto_hex_result(digest, digest_len);",
            ),
            "tests/diff/fuzz_binary_native_stdlib.py": (
                "hashlib.sha1(data).hexdigest()",
                "print(crypto.sha1(text{index}))",
            ),
            "tests/regression/10_stdlib/1400_crypto_hash.xr": (
                'var hash = crypto.sha1("hello")',
                'assert_eq(hash, "aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d")',
                'assert_eq(len(crypto.sha1("test")), 40)',
            ),
        },
        "forbidden_def_anchors": (
            "Slice<byte>",
            "[byte;20]",
            "encoding.hexEncode",
        ),
        "detail": (
            "crypto.sha1 remains a hex-string fixed-digest native path; future switch "
            "must return [byte;20] from Slice<byte> input and move presentation to "
            "encoding.hexEncode before task-200 public-native cutover"
        ),
    },
    "crypto.sha256": {
        "module": "crypto",
        "function": "sha256",
        "required": {
            "tests/stdlib/contracts/crypto/contract.toml": (
                "fixed digest APIs return fixed byte arrays and use encoding.hexEncode only for presentation",
                "legacy hash and HMAC APIs conflate digest bytes with lowercase hex presentation",
                "future converged digest APIs return fixed byte arrays; hex strings are produced by encoding.hexEncode at call sites",
            ),
            "stdlib/defs/core.def": (
                "fn sha256 {",
                'signature: "(data: string): string"',
                'vm: "crypto_sha256"',
                'aot: "xrt_crypto_sha256"',
            ),
            "stdlib/crypto/crypto.c": (
                "static XrValue crypto_sha256",
                "return crypto_hash_value(isolate, args, nargs, XR_CRYPTO_CORE_HASH_SHA256);",
                "return crypto_hex_string_result(isolate, digest, digest_len);",
            ),
            "src/aot/xrt_crypto.h": (
                "static inline XrValue xrt_crypto_sha256",
                "return xrt_crypto_hash_string(data, len, XR_CRYPTO_CORE_HASH_SHA256);",
                "return xrt_crypto_hex_result(digest, digest_len);",
            ),
            "tests/diff/fuzz_binary_native_stdlib.py": (
                "hashlib.sha256(data).hexdigest()",
                "print(crypto.sha256(text{index}))",
            ),
            "tests/regression/10_stdlib/1400_crypto_hash.xr": (
                'var hash = crypto.sha256("hello")',
                'assert_eq(hash, "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824")',
                'assert_eq(len(crypto.sha256("test")), 64)',
            ),
        },
        "forbidden_def_anchors": (
            "Slice<byte>",
            "[byte;32]",
            "encoding.hexEncode",
        ),
        "detail": (
            "crypto.sha256 remains a hex-string fixed-digest native path; future switch "
            "must return [byte;32] from Slice<byte> input and move presentation to "
            "encoding.hexEncode before task-200 public-native cutover"
        ),
    },
    "crypto.sha512": {
        "module": "crypto",
        "function": "sha512",
        "required": {
            "tests/stdlib/contracts/crypto/contract.toml": (
                "fixed digest APIs return fixed byte arrays and use encoding.hexEncode only for presentation",
                "legacy hash and HMAC APIs conflate digest bytes with lowercase hex presentation",
                "future converged digest APIs return fixed byte arrays; hex strings are produced by encoding.hexEncode at call sites",
            ),
            "tests/stdlib/contracts/crypto/cases.jsonl": (
                "hex-digest-presentation",
                "SHA-512 KAT anchors the current 128-character lowercase hex presentation",
            ),
            "stdlib/defs/core.def": (
                "fn sha512 {",
                'signature: "(data: string): string"',
                'vm: "crypto_sha512"',
                'aot: "xrt_crypto_sha512"',
            ),
            "stdlib/crypto/crypto.c": (
                "static XrValue crypto_sha512",
                "return crypto_hash_value(isolate, args, nargs, XR_CRYPTO_CORE_HASH_SHA512);",
                "return crypto_hex_string_result(isolate, digest, digest_len);",
            ),
            "src/aot/xrt_crypto.h": (
                "static inline XrValue xrt_crypto_sha512",
                "return xrt_crypto_hash_string(data, len, XR_CRYPTO_CORE_HASH_SHA512);",
                "return xrt_crypto_hex_result(digest, digest_len);",
            ),
            "tests/diff/fuzz_binary_native_stdlib.py": (
                "hashlib.sha512(data).hexdigest()",
                "print(crypto.sha512(text{index}))",
            ),
            "tests/regression/10_stdlib/1403_crypto_sha512.xr": (
                'var hash = crypto.sha512("hello")',
                'assert_eq(hash, "9b71d224bd62f3785d96d46ad3ea3d73319bfbc2890caadae2dff72519673ca72323c3d99ba5c11d7c7acc6e14b8c5da0c4663475c2e5c3adef46f73bcdec043")',
                'assert_eq(len(crypto.sha512("test")), 128)',
            ),
        },
        "forbidden_def_anchors": (
            "Slice<byte>",
            "[byte;64]",
            "encoding.hexEncode",
        ),
        "detail": (
            "crypto.sha512 remains a hex-string fixed-digest native path; future switch "
            "must return [byte;64] from Slice<byte> input and move presentation to "
            "encoding.hexEncode before task-200 public-native cutover"
        ),
    },
    "net.read": {
        "module": "net",
        "function": "read",
        "required": {
            "tests/stdlib/contracts/net/contract.toml": (
                "future converged net.read returns Array<byte> instead of string storage for arbitrary bytes",
                "legacy net.read exposes arbitrary socket bytes as string?",
                "loopback-string-status-sentinels",
                "future converged read/write APIs use typed byte values and typed native errors instead of string? and status sentinels",
            ),
            "stdlib/defs/core.def": (
                "fn read {",
                'signature: "(conn: NetConn, maxlen?: int): string?"',
                'vm: "net_read_handle_yieldable"',
                'aot: "xrt_net_read_default"',
                'aot: "xrt_net_read"',
            ),
            "stdlib/net/net.c": (
                "net.read(conn_handle, maxlen?) -> string | null",
                "static XrCFuncResult net_read_handle_yieldable",
                "*result = xr_string_value(xr_string_new(X, state->buf, n));",
                "*result = XR_NULL_VAL;",
                "net_conn_set_error",
            ),
            "src/aot/xrt_net.h": (
                "static inline XrValue xrt_net_read",
                "XrValue out = xrt_str_alloc((size_t) n);",
                "memcpy(xr_str_buf(out), buf, (size_t) n);",
                "return XR_NULL_VAL;",
                "xrt_net_set_error_base",
            ),
            "tests/regression/10_stdlib/1433_net_loopback.xr": (
                "test_loopback_binary_high_bytes",
                "var resp = net.read(c)!",
                "assert_eq(resp.bytes()[0]!, 195)",
                "assert_eq(resp.bytes()[4]!, 172)",
            ),
            "tests/stdlib/contracts/net/cases.jsonl": (
                "high-byte-loopback",
                "TCP loopback preserves high bytes through the current pre-switch net.read/net.write path",
            ),
        },
        "forbidden_def_anchors": (
            "Array<byte>",
            "Slice<byte>",
            "NetError",
            "@errors(",
        ),
        "detail": (
            "net.read remains a string?/null-sentinel native path; future switch "
            "must return Array<byte> from a typed byte boundary and emit NetError "
            "enum payloads before task-200 public-native cutover"
        ),
    },
}


@dataclass(frozen=True)
class CheckResult:
    category: str
    subject: str
    ok: bool
    detail: str


def load_toml(root: Path, path: Path) -> dict[str, Any]:
    sys.path.insert(0, str(root / "scripts"))
    try:
        import stdlib_manifest  # type: ignore[import-not-found]
    finally:
        try:
            sys.path.remove(str(root / "scripts"))
        except ValueError:
            pass
    return stdlib_manifest.load_toml(path)


def load_boundary_modules(root: Path) -> dict[str, dict[str, Any]]:
    data = load_toml(root, root / "stdlib" / "stdlib_boundary.toml")
    return {str(module.get("name", "")): module for module in data.get("module", ())}


def load_surface_inventory(root: Path) -> dict[str, list[Any]]:
    sys.path.insert(0, str(root / "scripts"))
    try:
        import check_binary_stdlib_surface  # type: ignore[import-not-found]
    finally:
        try:
            sys.path.remove(str(root / "scripts"))
        except ValueError:
            pass
    return check_binary_stdlib_surface.build_inventory(root)


def read_text(root: Path, path_text: str) -> str | None:
    path = root / path_text
    if not path.is_file():
        return None
    return path.read_text(encoding="utf-8")


def missing_anchors(root: Path, path_text: str, anchors: tuple[str, ...]) -> list[str]:
    text = read_text(root, path_text)
    if text is None:
        return [f"missing file {path_text}"]
    return [anchor for anchor in anchors if anchor not in text]


def source_marker_hits(root: Path, marker: str) -> list[Path]:
    proc = subprocess.run(
        [
            "git",
            "-C",
            str(root),
            "ls-files",
            "-co",
            "--exclude-standard",
            "--",
            marker,
            f":(glob)**/{marker}",
        ],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if proc.returncode == 0:
        return [root / line for line in proc.stdout.splitlines() if line]

    ignored_dirs = {".git", "build", "build-make", "build-ninja", "cmake-build-debug"}
    hits: list[Path] = []
    for path in root.rglob(marker):
        if any(part in ignored_dirs for part in path.relative_to(root).parts):
            continue
        hits.append(path)
    return hits


def def_module_block(root: Path, module: str) -> str | None:
    text = read_text(root, "stdlib/defs/core.def")
    if text is None:
        return None
    marker = f"module {module} {{"
    start = text.find(marker)
    if start < 0:
        return None
    next_module = text.find("\nmodule ", start + len(marker))
    return text[start:] if next_module < 0 else text[start:next_module]


def def_function_blocks(root: Path, module: str, function: str) -> list[str]:
    block = def_module_block(root, module)
    if block is None:
        return []
    blocks: list[str] = []
    marker = f"\n  fn {function} {{"
    search_from = 0
    while True:
        start = block.find(marker, search_from)
        if start < 0:
            break
        next_fn = block.find("\n  fn ", start + len(marker))
        blocks.append(block[start:] if next_fn < 0 else block[start:next_fn])
        search_from = start + len(marker)
    return blocks


def check_boundary(root: Path) -> list[CheckResult]:
    modules = load_boundary_modules(root)
    results: list[CheckResult] = []

    for name in BINARY_MODULES:
        module = modules.get(name)
        if module is None:
            results.append(CheckResult("BOUNDARY_BINARY_MODULE", name, False, "missing stdlib boundary entry"))
            continue

        semantic_source = str(module.get("semantic_source", ""))
        perf_suite = str(module.get("perf_suite", ""))
        public_native = module.get("public_native", None)
        failures: list[str] = []
        if not semantic_source:
            failures.append("missing semantic_source")
        if not perf_suite:
            failures.append("missing perf_suite")
        if not isinstance(public_native, list):
            failures.append("public_native must be a list")

        results.append(
            CheckResult(
                "BOUNDARY_BINARY_MODULE",
                name,
                not failures,
                "; ".join(failures) if failures else f"{semantic_source}, {perf_suite}",
            )
        )

    for name in PURE_BYTE_MODULES:
        module = modules.get(name, {})
        failures = []
        if module.get("policy") != "xray_semantic":
            failures.append(f"policy={module.get('policy')!r}, expected xray_semantic")
        if module.get("public_native") != []:
            failures.append("public_native must stay empty for pure byte module")
        if module.get("def_migration_complete") is not True:
            failures.append("def_migration_complete must be true")
        results.append(
            CheckResult(
                "PURE_BYTE_MODULE_READY",
                name,
                not failures,
                "; ".join(failures) if failures else "pure byte owner is already cut over",
            )
        )

    for name, expected_policy in PRE_SWITCH_NATIVE_MODULES.items():
        module = modules.get(name, {})
        public_native = module.get("public_native", [])
        failures = []
        if module.get("policy") != expected_policy:
            failures.append(f"policy={module.get('policy')!r}, expected {expected_policy}")
        if not isinstance(public_native, list) or not public_native:
            failures.append("public_native must remain explicit until dependencies close")
        if module.get("def_migration_complete") is True:
            failures.append("def_migration_complete was set before task-197/198 dependency closure")
        results.append(
            CheckResult(
                "PRE_SWITCH_NATIVE_BLOCKED",
                name,
                not failures,
                "; ".join(failures) if failures else "native public surface still gated",
            )
        )

    for name, expected in PRE_SWITCH_NATIVE_PUBLIC_SURFACE.items():
        module = modules.get(name, {})
        public_native = module.get("public_native", [])
        failures: list[str] = []
        if not isinstance(public_native, list):
            failures.append("public_native must be a list")
        else:
            actual = tuple(str(item) for item in public_native)
            if actual != expected:
                missing = sorted(set(expected) - set(actual))
                extra = sorted(set(actual) - set(expected))
                if missing:
                    failures.append("missing pre-switch entries: " + ", ".join(missing))
                if extra:
                    failures.append("unexpected pre-switch entries: " + ", ".join(extra))
                if not missing and not extra:
                    failures.append("pre-switch public_native order changed")

        results.append(
            CheckResult(
                "PRE_SWITCH_NATIVE_EXACT_SURFACE",
                name,
                not failures,
                "exact pre-switch public_native surface locked"
                if not failures
                else "; ".join(failures),
            )
        )

    return results


def check_contracts(root: Path) -> list[CheckResult]:
    results: list[CheckResult] = []
    for name in REQUIRED_CONTRACT_MODULES:
        contract = root / "tests" / "stdlib" / "contracts" / name / "contract.toml"
        cases = contract.with_name("cases.jsonl")
        diff_cases = contract.with_name("diff_cases.txt")
        missing = [str(path.relative_to(root)) for path in (contract, cases, diff_cases) if not path.is_file()]
        results.append(
            CheckResult(
                "BINARY_CONTRACT_CORPUS",
                name,
                not missing,
                "files present" if not missing else "missing " + ", ".join(missing),
            )
        )
    return results


def check_surface(root: Path) -> list[CheckResult]:
    inventory = load_surface_inventory(root)
    results: list[CheckResult] = []
    for category in REQUIRED_SURFACE_CATEGORIES:
        count = len(inventory.get(category, ()))
        results.append(
            CheckResult(
                "PRE_SWITCH_RESIDUE_TRACKED",
                category,
                count > 0,
                f"{count} tracked hits" if count > 0 else "expected tracked pre-switch residue",
            )
        )
    return results


def check_generated_residue(root: Path) -> list[CheckResult]:
    inventory = load_surface_inventory(root)
    by_file = {path: 0 for path in GENERATED_METADATA_FILES}
    for hit in inventory.get("GENERATED_METADATA_STALE_BINARY_SURFACE", ()):
        path = str(getattr(hit, "path", ""))
        if path in by_file:
            by_file[path] += 1

    results: list[CheckResult] = []
    for path, count in by_file.items():
        results.append(
            CheckResult(
                "GENERATED_BINARY_RESIDUE_TRACKED",
                path,
                count > 0,
                f"{count} stale generated-surface hits tracked"
                if count > 0
                else "expected generated metadata residue before public switch",
            )
        )
    return results


def check_perf_manifest(root: Path) -> list[CheckResult]:
    manifest_path = root / "tests" / "benchmarks" / "stdlib" / "manifest.toml"
    results: list[CheckResult] = []
    if not manifest_path.is_file():
        return [CheckResult("BINARY_PERF_MANIFEST", str(manifest_path), False, "missing manifest")]

    data = load_toml(root, manifest_path)
    governed = set(data.get("governed_suites", ()))
    missing_suites = [f"stdlib/{module}" for module in BINARY_MODULES if f"stdlib/{module}" not in governed]
    results.append(
        CheckResult(
            "BINARY_PERF_GOVERNED_SUITE",
            "task-200 binary modules",
            not missing_suites,
            "all binary suites governed" if not missing_suites else "missing " + ", ".join(missing_suites),
        )
    )

    by_id = {str(entry.get("id", "")): entry for entry in data.get("benchmark", ())}
    for bench_id, (module, suite, source) in REQUIRED_BENCHMARKS.items():
        entry = by_id.get(bench_id)
        failures: list[str] = []
        if entry is None:
            failures.append("missing benchmark entry")
        else:
            if entry.get("module") != module:
                failures.append(f"module={entry.get('module')!r}, expected {module}")
            if entry.get("suite") != suite:
                failures.append(f"suite={entry.get('suite')!r}, expected {suite}")
            if entry.get("source") != source:
                failures.append(f"source={entry.get('source')!r}, expected {source}")
            if entry.get("compare") != ["vm", "aot"]:
                failures.append("compare must be ['vm', 'aot']")
            if entry.get("metrics") != ["wall_ns"]:
                failures.append("metrics must be ['wall_ns']")
        if not (root / source).is_file():
            failures.append(f"source missing: {source}")
        results.append(
            CheckResult(
                "BINARY_PERF_BENCHMARK",
                bench_id,
                not failures,
                "VM/AOT wall_ns benchmark anchored" if not failures else "; ".join(failures),
            )
        )
    return results


def load_jsonl(root: Path, path_text: str) -> list[dict[str, Any]]:
    path = root / path_text
    if not path.is_file():
        return []
    records: list[dict[str, Any]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        records.append(json.loads(line))
    return records


def check_contract_oracles(root: Path) -> list[CheckResult]:
    results: list[CheckResult] = []
    for module, cases in REQUIRED_ORACLE_CASES.items():
        path_text = f"tests/stdlib/contracts/{module}/cases.jsonl"
        records = load_jsonl(root, path_text)
        by_case = {str(record.get("case", "")): record for record in records}
        for case, probe in cases.items():
            record = by_case.get(case)
            failures: list[str] = []
            if record is None:
                failures.append("missing case")
            elif record.get("probe") != probe:
                failures.append(f"probe={record.get('probe')!r}, expected {probe}")
            if not (root / probe).is_file():
                failures.append(f"probe file missing: {probe}")
            results.append(
                CheckResult(
                    "BINARY_CONTRACT_ORACLE",
                    f"{module}:{case}",
                    not failures,
                    "oracle probe anchored" if not failures else "; ".join(failures),
                )
            )

    for path_text, anchors in REQUIRED_TEXT_ANCHORS.items():
        missing = missing_anchors(root, path_text, anchors)
        results.append(
            CheckResult(
                "BINARY_KAT_PROPERTY_ORACLE",
                path_text,
                not missing,
                "anchors ok" if not missing else "missing anchors: " + ", ".join(missing),
            )
        )
    return results


def check_dependency_markers(root: Path) -> list[CheckResult]:
    marker_dir = root / "tests" / "stdlib" / "contracts"
    results: list[CheckResult] = []
    for marker in DEPENDENCY_MARKERS:
        hits = source_marker_hits(root, marker)
        results.append(
            CheckResult(
                "PUBLIC_SWITCH_DEPENDENCY_BLOCKER",
                marker,
                not hits,
                "not present; public-native switch remains blocked"
                if not hits
                else "unexpected readiness marker present: "
                + ", ".join(str(path.relative_to(root)) for path in hits[:5]),
            )
        )
    if not marker_dir.is_dir():
        results.append(CheckResult("PUBLIC_SWITCH_DEPENDENCY_BLOCKER", str(marker_dir), False, "missing contracts dir"))
    return results


def check_partial_dependency_evidence(root: Path) -> list[CheckResult]:
    results: list[CheckResult] = []
    for name, spec in PARTIAL_DEPENDENCY_EVIDENCE.items():
        missing: list[str] = []
        required = spec["required"]
        assert isinstance(required, dict)
        for path_text, anchors in required.items():
            missing.extend(missing_anchors(root, str(path_text), anchors))
        marker = str(spec["full_marker"])
        marker_hits = source_marker_hits(root, marker)
        failures = list(missing)
        if marker_hits:
            failures.append(
                "unexpected full-readiness marker present: "
                + ", ".join(str(path.relative_to(root)) for path in marker_hits[:5])
            )
        results.append(
            CheckResult(
                "PUBLIC_SWITCH_PARTIAL_DEPENDENCY",
                name,
                not failures,
                str(spec["detail"]) if not failures else "; ".join(failures),
            )
        )
    return results


def check_native_typed_error_abi_blockers(root: Path) -> list[CheckResult]:
    modules = load_boundary_modules(root)
    results: list[CheckResult] = []
    for name, spec in NATIVE_TYPED_ERROR_ABI_BLOCKERS.items():
        failures: list[str] = []
        module = modules.get(name)
        if module is None:
            failures.append("missing stdlib boundary entry")
        else:
            public_native = module.get("public_native")
            if module.get("policy") != PRE_SWITCH_NATIVE_MODULES.get(name):
                failures.append(
                    f"policy={module.get('policy')!r}, expected {PRE_SWITCH_NATIVE_MODULES.get(name)}"
                )
            if not isinstance(public_native, list) or not public_native:
                failures.append("public_native surface changed before typed native error ABI closure")
            if module.get("def_migration_complete") is True:
                failures.append("def_migration_complete set before typed native error ABI closure")

        required = spec["required"]
        assert isinstance(required, dict)
        for path_text, anchors in required.items():
            missing = missing_anchors(root, str(path_text), anchors)
            failures.extend(f"{path_text}: missing {anchor}" for anchor in missing)

        block = def_module_block(root, name)
        if block is None:
            failures.append(f"stdlib/defs/core.def: missing module {name}")
        elif "@errors(" in block:
            failures.append(f"stdlib/defs/core.def: module {name} has @errors before readiness marker")

        results.append(
            CheckResult(
                "NATIVE_TYPED_ERROR_ABI_BLOCKER",
                name,
                not failures,
                str(spec["detail"]) if not failures else "; ".join(failures),
            )
        )
    return results


def check_single_function_native_typed_error_probes(root: Path) -> list[CheckResult]:
    modules = load_boundary_modules(root)
    results: list[CheckResult] = []
    for subject, spec in SINGLE_FUNCTION_NATIVE_TYPED_ERROR_PROBES.items():
        module_name = str(spec["module"])
        function_name = str(spec["function"])
        failures: list[str] = []

        module = modules.get(module_name)
        if module is None:
            failures.append("missing stdlib boundary entry")
        else:
            public_native = module.get("public_native")
            if not isinstance(public_native, list) or function_name not in public_native:
                failures.append(f"{module_name}.{function_name} left pre-switch public_native")
            if module.get("def_migration_complete") is True:
                failures.append("def_migration_complete set before typed native error ABI closure")

        required = spec["required"]
        assert isinstance(required, dict)
        for path_text, anchors in required.items():
            missing = missing_anchors(root, str(path_text), anchors)
            failures.extend(f"{path_text}: missing {anchor}" for anchor in missing)

        function_blocks = def_function_blocks(root, module_name, function_name)
        if not function_blocks:
            failures.append(f"stdlib/defs/core.def: missing {subject} function block")
        for function_block in function_blocks:
            if "@errors(" in function_block:
                failures.append(f"stdlib/defs/core.def: {subject} has @errors before readiness marker")
            typed_error = str(spec.get("typed_error", ""))
            if (not spec.get("allow_typed_error_before_full_marker")) and typed_error and typed_error in function_block:
                failures.append(
                    f"stdlib/defs/core.def: {subject} mentions {typed_error} before readiness marker"
                )

        marker_hits = source_marker_hits(root, "TASK_198_TYPED_NATIVE_ERRORS_READY")
        if marker_hits:
            failures.append(
                "unexpected full-readiness marker present: "
                + ", ".join(str(path.relative_to(root)) for path in marker_hits[:5])
            )

        results.append(
            CheckResult(
                "NATIVE_TYPED_ERROR_ABI_FUNCTION_BLOCKER",
                subject,
                not failures,
                str(spec["detail"]) if not failures else "; ".join(failures),
            )
        )
    return results


def check_single_function_public_surface_probes(root: Path) -> list[CheckResult]:
    modules = load_boundary_modules(root)
    results: list[CheckResult] = []
    for subject, spec in SINGLE_FUNCTION_PUBLIC_SURFACE_PROBES.items():
        module_name = str(spec["module"])
        function_name = str(spec["function"])
        failures: list[str] = []

        module = modules.get(module_name)
        if module is None:
            failures.append("missing stdlib boundary entry")
        else:
            public_native = module.get("public_native")
            if not isinstance(public_native, list) or function_name not in public_native:
                failures.append(f"{module_name}.{function_name} left pre-switch public_native")
            if module.get("def_migration_complete") is True:
                failures.append("def_migration_complete set before public surface closure")

        required = spec["required"]
        assert isinstance(required, dict)
        for path_text, anchors in required.items():
            missing = missing_anchors(root, str(path_text), anchors)
            failures.extend(f"{path_text}: missing {anchor}" for anchor in missing)

        function_blocks = def_function_blocks(root, module_name, function_name)
        if not function_blocks:
            failures.append(f"stdlib/defs/core.def: missing {subject} function block")
        forbidden_def_anchors = tuple(str(anchor) for anchor in spec.get("forbidden_def_anchors", ()))
        for function_block in function_blocks:
            for anchor in forbidden_def_anchors:
                if anchor in function_block:
                    failures.append(
                        f"stdlib/defs/core.def: {subject} mentions {anchor} before public switch"
                    )

        results.append(
            CheckResult(
                "PUBLIC_NATIVE_FUNCTION_SURFACE_BLOCKER",
                subject,
                not failures,
                str(spec["detail"]) if not failures else "; ".join(failures),
            )
        )
    return results


def check_harness_anchors(root: Path) -> list[CheckResult]:
    anchors = {
        "tests/diff/fuzz_binary_stdlib.py": (
            "base64.b64encode(data)",
            'data.decode("utf-8", "strict")',
            '[str(xray), "build", "--native"',
        ),
        "tests/diff/fuzz_binary_native_stdlib.py": (
            "zlib.crc32(data)",
            "hashlib.sha256(data).hexdigest()",
            "hmac.new(key, data, getattr(hashlib, algo)).hexdigest()",
            '[str(xray), "build", "--native"',
        ),
        "tests/aot/filetests/link/core_compress.expect": (
            "c_contains=xrt_compress_crc32(",
            "c_not_contains=xrt_method_",
        ),
        "tests/aot/filetests/link/core_crypto.expect": (
            "c_contains=xrt_crypto_sha512(",
            "c_not_contains=xrt_method_",
        ),
        "tests/benchmarks/stdlib/manifest.toml": (
            'id = "compress.contract"',
            'id = "crypto.contract"',
            'compare = ["vm", "aot"]',
        ),
    }
    results: list[CheckResult] = []
    for path_text, needles in anchors.items():
        path = root / path_text
        if not path.is_file():
            results.append(CheckResult("BINARY_ORACLE_HARNESS", path_text, False, "missing file"))
            continue
        text = path.read_text(encoding="utf-8")
        missing = [needle for needle in needles if needle not in text]
        results.append(
            CheckResult(
                "BINARY_ORACLE_HARNESS",
                path_text,
                not missing,
                "anchors ok" if not missing else "missing anchors: " + ", ".join(missing),
            )
        )
    return results


def build_results(root: Path) -> list[CheckResult]:
    results: list[CheckResult] = []
    results.extend(check_boundary(root))
    results.extend(check_contracts(root))
    results.extend(check_surface(root))
    results.extend(check_generated_residue(root))
    results.extend(check_perf_manifest(root))
    results.extend(check_contract_oracles(root))
    results.extend(check_harness_anchors(root))
    results.extend(check_dependency_markers(root))
    results.extend(check_partial_dependency_evidence(root))
    results.extend(check_native_typed_error_abi_blockers(root))
    results.extend(check_single_function_native_typed_error_probes(root))
    results.extend(check_single_function_public_surface_probes(root))
    return results


def print_text(results: list[CheckResult]) -> None:
    print("Task 200 binary public-native switch readiness")
    for result in results:
        status = "ok" if result.ok else "blocked"
        print(f"{result.category}: {status}: {result.subject}: {result.detail}")


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

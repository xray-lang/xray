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
REQUIRED_CONTRACT_MODULES = ("base64", "encoding", "compress", "crypto", "io", "http", "ws")
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
        "random-shape": "tests/diff/cases/semantics/stdlib/crypto_random_system_direct.xr",
        "seeded-cross-oracle-fuzz": "tests/diff/fuzz_binary_native_stdlib.py",
    },
    "io": {
        "filesystem-text-and-bytes": "tests/diff/cases/semantics/stdlib/io_system_direct.xr",
        "all-byte-file-boundary": "tests/diff/cases/semantics/stdlib/io_binary_file_boundary_direct.xr",
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
        "assert_eq(resp.bytes()[0]!.toUInt32(), 195)",
        "assert_eq(resp.bytes()[4]!.toUInt32(), 172)",
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
        "detail": "string VM/AOT typed-error runtime probe exists, but compress/crypto native typed-error ABI is not marked ready",
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
                'signature: "(data: string): string?"',
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
        hits = list(root.rglob(marker))
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
        marker_hits = list(root.rglob(marker))
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

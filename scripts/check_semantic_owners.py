#!/usr/bin/env python3
"""Verify shared semantic-core ownership and production semantic-owner ratchets."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import re
import sys
import tempfile
import tomllib
from pathlib import Path


XISAGEN_DIR = Path(__file__).resolve().parent.parent / "tools" / "xisagen"
if str(XISAGEN_DIR) not in sys.path:
    sys.path.insert(0, str(XISAGEN_DIR))

from xisagen import parse_xi_ops_def, parse_xi_semantic_owners  # noqa: E402


SOURCE_SUFFIXES = (".c", ".h")
SIGNATURE_RE = re.compile(
    r"\b(?:(?:static\s+)?inline|XR_BYTE_SLICE_SCALAR_INLINE|XR_BYTE_ARRAY_COPY_INLINE|"
    r"XR_NULL_TEST_INLINE)\s+[^;{}]*?"
    r"\b(xr_[A-Za-z0-9_]+)\s*\(")
SORT_OLD_SYMBOLS = (
    "xr_array_hybrid_sort",
    "xr_sort_merge",
    "TYPED_SORT",
    "xrt_introsort_",
    "xrt_vintrosort",
    "XRT_SORT_DEF",
)
SORT_CONSUMERS = (
    "src/runtime/object/xarray_vm.c",
    "src/aot/xrt_sort.inc.c",
)
TRUTHINESS_CONSUMERS = (
    ("src/runtime/value/xvalue_truthy.c", "xr_value_is_truthy"),
    ("src/vm/xvm_internal.h", "vm_is_truthy"),
    ("src/aot/xrt_value.h", "xr_truthy"),
    ("src/aot/xrt_method.h", "xrt_to_bool"),
    ("src/aot/xrt_core_freestanding.h", "xrt_to_bool"),
    ("src/aot/xrt_core_freestanding.h", "xr_truthy"),
)
TRUTHINESS_RETIRED_DECISIONS = (
    "return XR_TO_BOOL(value) != 0",
    "return XR_TO_INT(value) != 0",
    "return XR_TO_FLOAT(value) != 0.0",
    "return value.i == 0",
    "return XR_TO_INT(value) == 0",
    "return XR_TO_FLOAT(value) == 0.0",
    "return XR_FROM_BOOL(v.i != 0)",
    "return XR_FROM_BOOL(v.f != 0.0)",
    "return XR_FROM_BOOL(XR_TO_INT(val) != 0)",
    "return XR_FROM_BOOL(XR_TO_FLOAT(val) != 0.0)",
)
TRUTHINESS_SURROGATE_OWNER_TOKENS = (
    "xg_global_evidence",
    "global_evidence_plan",
    "canonical_name",
    '"xi.not"',
    '"shared.truthiness"',
)
CGEN_TRUTHINESS_CONSUMERS = (
    ("src/aot/xi_cgen.c", "emit_condition_expr_ctx"),
    ("src/aot/xi_cgen_dispatch_helpers.inc.c", "xicgen_emit_assert_condition"),
)
TYPE_IDENTITY_CORE_CONSUMERS = (
    ("src/runtime/value/xvalue_typeid.c", "xr_value_typeid"),
    ("src/runtime/value/xvalue_typeid.c", "xr_value_typeid_vm"),
    ("src/aot/xrt_arith.h", "xrt_typeof_id"),
    ("src/aot/xrt_core_freestanding.h", "xrt_typeof_id"),
)
TYPE_IDENTITY_SURROGATE_OWNER_TOKENS = (
    "xg_global_evidence",
    "global_evidence_plan",
    "canonical_name",
    '"xi.typeid"',
    '"primitive.type-identity"',
)
EXACT_BITS_OPERATIONS = {
    "xi.bit.rotl",
    "xi.bit.rotr",
    "xi.bit.bswap",
    "xi.bit.popcount",
    "xi.bit.clz",
    "xi.bit.ctz",
    "xi.bit.mul-high",
}
EXACT_BITS_AOT_BINDINGS = (
    "src/aot/xrt.h",
    "src/aot/xrt_core_freestanding.h",
)
BITS_NOT_OPERATIONS = {"xi.bnot"}
BITS_NOT_AOT_BINDINGS = (
    "src/aot/xrt.h",
    "src/aot/xrt_core_freestanding.h",
)
BITWISE_BINARY_OPERATIONS = {"xi.band", "xi.bor", "xi.bxor"}
BITWISE_BINARY_AOT_BINDINGS = (
    "src/aot/xrt.h",
    "src/aot/xrt_core_freestanding.h",
)
SHIFT_OPERATIONS = {"xi.shl", "xi.shr"}
SHIFT_AOT_BINDINGS = (
    "src/aot/xrt.h",
    "src/aot/xrt_core_freestanding.h",
)
NUMERIC_NEG_OPERATIONS = {"xi.neg"}
NUMERIC_NEG_AOT_BINDINGS = (
    "src/aot/xrt.h",
    "src/aot/xrt_core_freestanding.h",
)
NUMERIC_WIDTH_OPERATIONS = {
    "xi.narrow.i8",
    "xi.narrow.u8",
    "xi.narrow.i16",
    "xi.narrow.u16",
    "xi.narrow.i32",
    "xi.narrow.u32",
    "xi.narrow.f32",
    "xi.widen.i8",
    "xi.widen.u8",
    "xi.widen.i16",
    "xi.widen.u16",
    "xi.widen.i32",
    "xi.widen.u32",
    "xi.widen.f32",
}
NUMERIC_WIDTH_KERNELS = {
    "xr_numeric_narrow_i8",
    "xr_numeric_narrow_u8",
    "xr_numeric_narrow_i16",
    "xr_numeric_narrow_u16",
    "xr_numeric_narrow_i32",
    "xr_numeric_narrow_u32",
    "xr_numeric_narrow_f32",
    "xr_numeric_widen_i8",
    "xr_numeric_widen_u8",
    "xr_numeric_widen_i16",
    "xr_numeric_widen_u16",
    "xr_numeric_widen_i32",
    "xr_numeric_widen_u32",
    "xr_numeric_widen_f32",
}
BYTE_SLICE_SCALAR_OPERATIONS = {
    f"xi.byte.slice.{direction}.{scalar}"
    for direction in ("load", "store")
    for scalar in ("u16", "u32", "u64", "f32", "f64")
}
BYTE_SLICE_COMPARE_OPERATIONS = {"xi.byte.slice.compare"}
BYTE_SLICE_COMMON_PREFIX_OPERATIONS = {"xi.byte.slice.common.prefix"}
BYTE_SLICE_FILL_OPERATIONS = {"xi.byte.slice.fill"}
BYTE_SLICE_COPY_OPERATIONS = {"xi.byte.slice.copy"}
BYTE_SLICE_REPEAT_OPERATIONS = {"xi.byte.slice.repeat"}
POD_SLICE_COPY_OPERATIONS = {"xi.slice.copy"}
POD_SLICE_COMPARE_OPERATIONS = {"xi.slice.compare"}
POD_SLICE_VIEW_OPERATIONS = {"xi.slice.as.bytes", "xi.slice.reinterpret"}
RAW_MEMORY_COPY_OPERATIONS = {"xi.ptr.copy.nonoverlap"}
RAW_SCALAR_ACCESS_OPERATIONS = {"xi.ptr.load", "xi.ptr.store"}
ENUM_METADATA_ACCESS_OPERATIONS = {"xi.enum.variant.at", "xi.enum.payload.at"}
CELL_ACCESS_OPERATIONS = {"xi.cell.get", "xi.cell.set"}
NULL_TEST_OPERATIONS = {"xi.isnull"}
DATA_POINTER_OPERATIONS = {"xi.array.data.ptr", "xi.static.bytes.ptr"}
BYTE_ARRAY_COPY_OPERATIONS = {"xi.byte.array.copy.within", "xi.byte.array.copy.from"}
TARGET_LAYOUT_QUERY_OPERATIONS = {"xi.target.sizeof", "xi.target.alignof"}
RANGE_OPERATIONS = {"xi.range"}
RANGE_AOT_BINDINGS = (
    "src/aot/xrt.h",
    "src/aot/xrt_core_freestanding.h",
)
BYTE_SLICE_SCALAR_KERNELS = {
    f"xr_array_core_bytes_{direction}_{scalar}"
    for direction in ("load", "store")
    for scalar in ("u16", "u32", "u64", "f32", "f64")
}
OWNER_ID_RE = re.compile(r"^[0-9a-f]{32}$")
OWNER_HALF_RE = re.compile(r"^0x[0-9a-f]{16}$")
CONSUMER_BITS_RE = re.compile(r"^0x[0-9a-f]{8}$")
MIGRATED_OWNER_PREFIX = "XR_SEM_OWNER_ID_"


def canonical_registry_fingerprint(registry: dict) -> str:
    payload = {key: value for key, value in registry.items() if key != "canonical_fingerprint"}
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":"),
                         ensure_ascii=False).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def owner_macro_prefix(owner: str) -> str:
    suffix = re.sub(r"[^A-Za-z0-9]", "_", owner).upper()
    return f"{MIGRATED_OWNER_PREFIX}{suffix}"


def semantic_stable_id(canonical_name: str) -> str:
    digest = hashlib.sha256(b"xray-semantic-id-v1\0" + canonical_name.encode("utf-8")).digest()
    return digest[:16].hex()


def _validate_stable_id(row: dict, name_key: str, id_key: str,
                        errors: list[str], label: str) -> None:
    stable_id = row.get(id_key)
    hi = row.get(f"{id_key}_hi")
    lo = row.get(f"{id_key}_lo")
    if not isinstance(stable_id, str) or not OWNER_ID_RE.fullmatch(stable_id):
        errors.append(f"{label}: {id_key} is not a 128-bit lowercase hex ID")
        return
    if not isinstance(hi, str) or not OWNER_HALF_RE.fullmatch(hi):
        errors.append(f"{label}: {id_key}_hi is not a canonical u64 hex value")
    if not isinstance(lo, str) or not OWNER_HALF_RE.fullmatch(lo):
        errors.append(f"{label}: {id_key}_lo is not a canonical u64 hex value")
    if hi != f"0x{stable_id[:16]}" or lo != f"0x{stable_id[16:]}":
        errors.append(f"{label}: {id_key} halves do not match the serialized ID")
    if not isinstance(row.get(name_key), str) or not row[name_key]:
        errors.append(f"{label}: missing canonical {name_key}")
    elif stable_id != semantic_stable_id(row[name_key]):
        errors.append(f"{label}: {id_key} does not match its canonical semantic name")


def validate_owner_registry(registry: dict, operation_categories: dict[str, str],
                            root: Path | None = None) -> list[str]:
    errors: list[str] = []
    if registry.get("schema") != 1:
        errors.append("semantic owner registry schema must be exactly 1")
    if canonical_registry_fingerprint(registry) != registry.get("canonical_fingerprint"):
        errors.append("semantic owner registry canonical fingerprint mismatch")

    consumer_table = registry.get("consumers")
    if not isinstance(consumer_table, dict) or not consumer_table:
        errors.append("semantic owner registry has no consumer bit table")
        consumer_table = {}
    parsed_consumer_bits: dict[str, int] = {}
    used_bits = 0
    for consumer, encoded_bit in consumer_table.items():
        if not isinstance(consumer, str) or not isinstance(encoded_bit, str) or not \
                CONSUMER_BITS_RE.fullmatch(encoded_bit):
            errors.append(f"semantic owner registry consumer {consumer!r} has invalid bit")
            continue
        bit = int(encoded_bit, 16)
        if bit == 0 or bit & (bit - 1) or used_bits & bit:
            errors.append(f"semantic owner registry consumer {consumer!r} has duplicate/non-bit value")
            continue
        parsed_consumer_bits[consumer] = bit
        used_bits |= bit

    operations = registry.get("operations")
    if not isinstance(operations, list):
        errors.append("semantic owner registry operations must be an array")
        operations = []
    operation_rows: dict[str, dict] = {}
    operation_ids: dict[str, str] = {}
    owner_ids: dict[str, str] = {}
    owner_names_by_id: dict[str, str] = {}
    owners_from_operations: dict[str, list[dict]] = {}

    for index, row in enumerate(operations):
        if not isinstance(row, dict):
            errors.append(f"semantic owner registry operation row {index} is not an object")
            continue
        operation = row.get("operation")
        label = f"operation row {index} ({operation!r})"
        if operation in operation_rows:
            errors.append(f"{operation}: operation has multiple owner rows")
        elif isinstance(operation, str):
            operation_rows[operation] = row
        _validate_stable_id(row, "operation", "operation_id", errors, label)
        _validate_stable_id(row, "owner", "owner_id", errors, label)
        if isinstance(row.get("operation_id"), str):
            previous = operation_ids.get(row["operation_id"])
            if previous is not None and previous != operation:
                errors.append(f"{operation}: duplicate stable operation ID owned by {previous}")
            operation_ids[row["operation_id"]] = operation
        owner = row.get("owner")
        owner_id = row.get("owner_id")
        if isinstance(owner, str) and isinstance(owner_id, str):
            previous_id = owner_ids.get(owner)
            if previous_id is not None and previous_id != owner_id:
                errors.append(f"{owner}: owner name maps to multiple stable IDs")
            previous_owner = owner_names_by_id.get(owner_id)
            if previous_owner is not None and previous_owner != owner:
                errors.append(f"{owner}: duplicate stable owner ID owned by {previous_owner}")
            owner_ids[owner] = owner_id
            owner_names_by_id[owner_id] = owner
            owners_from_operations.setdefault(owner, []).append(row)

        expected_category = operation_categories.get(operation)
        if expected_category is None:
            errors.append(f"{operation}: generated registry names unknown operation")
        elif row.get("category") != expected_category:
            errors.append(f"{operation}: generated owner category differs from xisa/xi/ops.def")

        consumers = row.get("consumers")
        if not isinstance(consumers, list) or not consumers:
            errors.append(f"{operation}: semantic owner has no production consumer")
            consumers = []
        if len(consumers) != len(set(consumers)):
            errors.append(f"{operation}: duplicate production consumer")
        unknown_consumers = sorted(set(consumers) - parsed_consumer_bits.keys())
        if unknown_consumers:
            errors.append(f"{operation}: unknown production consumers {unknown_consumers!r}")
        expected_bits = 0
        for consumer in consumers:
            expected_bits |= parsed_consumer_bits.get(consumer, 0)
        encoded_bits = row.get("consumer_bits")
        if not isinstance(encoded_bits, str) or not CONSUMER_BITS_RE.fullmatch(encoded_bits) or \
                int(encoded_bits, 16) != expected_bits:
            errors.append(f"{operation}: consumer bitset differs from declared consumers")

        bindings = row.get("production_bindings")
        if not isinstance(bindings, list):
            errors.append(f"{operation}: production bindings must be an array")
            bindings = []
        binding_consumers: list[str] = []
        for binding in bindings:
            if not isinstance(binding, dict):
                errors.append(f"{operation}: malformed production binding")
                continue
            consumer = binding.get("consumer")
            path = binding.get("path")
            symbol = binding.get("symbol")
            if not all(isinstance(value, str) and value for value in (consumer, path, symbol)):
                errors.append(f"{operation}: incomplete production binding")
                continue
            binding_consumers.append(consumer)
            if root is None:
                continue
            source = root / path
            if not source.is_file():
                errors.append(f"{operation}: production consumer source is missing: {path}")
                continue
            text = source.read_text(encoding="utf-8", errors="strict")
            if symbol not in text:
                errors.append(f"{operation}: production binding symbol {symbol} is missing from {path}")
            if owner != operation:
                prefix = owner_macro_prefix(owner)
                marker_text = text
                if consumer == "semantic-plan":
                    generated_path = root / "src/plan/semantic/xr_semantic_ops_gen.h"
                    marker_text = generated_path.read_text(encoding="utf-8", errors="strict") \
                        if generated_path.is_file() else ""
                if f"{prefix}_HI" not in marker_text or f"{prefix}_LO" not in marker_text:
                    errors.append(f"{operation}: {path} does not consume stable owner ID {owner}")
        if sorted(binding_consumers) != sorted(consumers):
            errors.append(f"{operation}: production bindings do not cover declared consumers exactly once")
        if "cgen" in consumers and not row.get("cgen_adapter"):
            errors.append(f"{operation}: CGen consumer has no generated adapter binding")
        if "cgen" not in consumers and row.get("cgen_adapter") is not None:
            errors.append(f"{operation}: non-CGen owner unexpectedly declares a CGen adapter")

    expected_operations = set(operation_categories)
    actual_operations = set(operation_rows)
    for operation in sorted(expected_operations - actual_operations):
        errors.append(f"{operation}: operation is unowned in generated registry")
    for operation in sorted(actual_operations - expected_operations):
        errors.append(f"{operation}: generated registry contains unknown operation")

    owners = registry.get("owners")
    if not isinstance(owners, list):
        errors.append("semantic owner registry owners must be an array")
        owners = []
    owner_rows: dict[str, dict] = {}
    for index, row in enumerate(owners):
        if not isinstance(row, dict):
            errors.append(f"semantic owner registry owner row {index} is not an object")
            continue
        owner = row.get("owner")
        label = f"owner row {index} ({owner!r})"
        _validate_stable_id(row, "owner", "owner_id", errors, label)
        if owner in owner_rows:
            errors.append(f"{owner}: duplicate semantic owner row")
        elif isinstance(owner, str):
            owner_rows[owner] = row
        member_rows = owners_from_operations.get(owner, [])
        expected_members = sorted(member["operation"] for member in member_rows)
        if not isinstance(row.get("operations"), list) or \
                sorted(row["operations"]) != expected_members:
            errors.append(f"{owner}: owner operation list differs from operation rows")
        if not expected_members:
            errors.append(f"{owner}: dead semantic owner has no operations")
        expected_consumers = sorted({consumer for member in member_rows
                                     for consumer in member.get("consumers", [])})
        if not isinstance(row.get("consumers"), list) or \
                len(row["consumers"]) != len(set(row["consumers"])) or \
                sorted(row["consumers"]) != expected_consumers:
            errors.append(f"{owner}: owner consumer list differs from operation rows")
        if row.get("owner_id") != owner_ids.get(owner):
            errors.append(f"{owner}: owner stable ID differs from operation rows")
        expected_categories = {member.get("category") for member in member_rows}
        if len(expected_categories) != 1 or row.get("category") not in expected_categories:
            errors.append(f"{owner}: owner category differs from operation rows")
        expected_bits = 0
        for member in member_rows:
            encoded = member.get("consumer_bits")
            if isinstance(encoded, str) and CONSUMER_BITS_RE.fullmatch(encoded):
                expected_bits |= int(encoded, 16)
        if row.get("consumer_bits") != f"0x{expected_bits:08x}":
            errors.append(f"{owner}: owner consumer bitset differs from operation rows")
        expected_adapters = {member.get("cgen_adapter") for member in member_rows}
        if len(expected_adapters) != 1 or row.get("cgen_adapter") not in expected_adapters:
            errors.append(f"{owner}: owner CGen adapter differs from operation rows")
        expected_bindings = {
            (binding.get("consumer"), binding.get("path"), binding.get("symbol"))
            for member in member_rows for binding in member.get("production_bindings", [])
            if isinstance(binding, dict)
        }
        summary_bindings = row.get("production_bindings")
        actual_bindings = {
            (binding.get("consumer"), binding.get("path"), binding.get("symbol"))
            for binding in summary_bindings
            if isinstance(binding, dict)
        } if isinstance(summary_bindings, list) else set()
        if actual_bindings != expected_bindings or \
                not isinstance(summary_bindings, list) or \
                len(summary_bindings) != len(actual_bindings):
            errors.append(f"{owner}: owner production bindings differ from operation rows")
    if set(owner_rows) != set(owners_from_operations):
        errors.append("semantic owner summary differs from operation owner set")
    return errors


def verify_generated_owner_header(root: Path, registry: dict) -> list[str]:
    errors: list[str] = []
    path = root / "src/shared/xr_semantic_owner_ids_gen.h"
    if not path.is_file():
        return ["generated semantic owner ID header is missing"]
    text = path.read_text(encoding="utf-8", errors="strict")
    fingerprint = registry.get("canonical_fingerprint")
    fingerprint_line = f'#define XR_SEMANTIC_OWNER_REGISTRY_FINGERPRINT "{fingerprint}"'
    if fingerprint_line not in text:
        errors.append("generated owner header fingerprint differs from machine-readable registry")
    for owner in registry.get("owners", []):
        operations = owner.get("operations")
        if not isinstance(operations, list) or \
                (len(operations) == 1 and operations[0] == owner.get("owner")):
            continue
        prefix = owner_macro_prefix(owner["owner"])
        expected = (
            f"#define {prefix}_HI UINT64_C({owner.get('owner_id_hi')})",
            f"#define {prefix}_LO UINT64_C({owner.get('owner_id_lo')})",
            f"#define {prefix}_CONSUMERS UINT32_C({owner.get('consumer_bits')})",
        )
        for line in expected:
            if line not in text:
                errors.append(f"generated owner header binding drifted: {line}")
    return errors


def source_files(root: Path) -> list[Path]:
    files: list[Path] = []
    for base in ("src", "stdlib", "include", "tests"):
        directory = root / base
        for path in directory.rglob("*"):
            if path.is_file() and (path.name.endswith(".inc.c") or path.suffix in SOURCE_SUFFIXES):
                files.append(path)
    return sorted(files)


def callers_for(root: Path, header: Path, symbols: list[str], files: list[Path]) -> tuple[list[str], list[str]]:
    production: set[str] = set()
    tests: set[str] = set()
    needles = (header.name, *symbols)
    for path in files:
        if path == header:
            continue
        text = path.read_text(encoding="utf-8", errors="strict")
        if not any(needle in text for needle in needles):
            continue
        relative = path.relative_to(root).as_posix()
        (tests if relative.startswith("tests/") else production).add(relative)
    return sorted(production), sorted(tests)


def build_inventory(root: Path, manifest: dict) -> list[dict]:
    files = source_files(root)
    rows: list[dict] = []
    for entry in sorted(manifest["core"], key=lambda item: item["header"]):
        header = root / entry["header"]
        text = header.read_text(encoding="utf-8")
        symbols = sorted(set(SIGNATURE_RE.findall(text)))
        production, tests = callers_for(root, header, symbols, files)
        status = "production" if production else ("test-only" if tests else "dead")
        rows.append(
            {
                "header": entry["header"],
                "id": entry["id"],
                "representation": entry["representation"],
                "owner": entry["owner"],
                "profiles": entry["profiles"],
                "status": status,
                "signatures": symbols,
                "production_callers": production,
                "test_callers": tests,
                "contains_tagged_value": "XrValue" in text,
            }
        )
    return rows


def verify_sort_ratchet(root: Path) -> list[str]:
    errors: list[str] = []
    for relative in SORT_CONSUMERS:
        text = (root / relative).read_text(encoding="utf-8")
        if "xr_sort_core.h" not in text or "xr_sort_core_" not in text:
            errors.append(f"{relative}: Array.sort no longer consumes the canonical shared kernel")
        for symbol in SORT_OLD_SYMBOLS:
            if symbol in text:
                errors.append(f"{relative}: retired private sort symbol revived: {symbol}")
    return errors


def extract_c_function(text: str, symbol: str) -> str | None:
    pattern = re.compile(rf"\b{re.escape(symbol)}\s*\([^;{{}}]*\)\s*\{{")
    match = pattern.search(text)
    if match is None:
        return None
    start = text.find("{", match.start())
    depth = 0
    for index in range(start, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start:index + 1]
    return None


def truthiness_surrogate_owner(body: str) -> str | None:
    for token in TRUTHINESS_SURROGATE_OWNER_TOKENS:
        if token in body:
            return token
    return None


def type_identity_surrogate_owner(body: str) -> str | None:
    for token in TYPE_IDENTITY_SURROGATE_OWNER_TOKENS:
        if token in body:
            return token
    return None


def verify_truthiness_ratchet(root: Path) -> list[str]:
    errors: list[str] = []
    marker = owner_macro_prefix("shared.truthiness")
    for relative, symbol in TRUTHINESS_CONSUMERS:
        text = (root / relative).read_text(encoding="utf-8", errors="strict")
        body = extract_c_function(text, symbol)
        if body is None:
            errors.append(f"{relative}: truthiness consumer {symbol} is missing")
            continue
        if f"{marker}_HI" not in body or f"{marker}_LO" not in body:
            errors.append(f"{relative}: {symbol} no longer consumes the stable truthiness owner ID")
        if "xr_truthy_core_eval" not in body and "xrt_to_bool" not in body:
            errors.append(f"{relative}: {symbol} no longer delegates to the truthiness owner")
        if "xr_semantic_owner_has_consumer" in body:
            errors.append(f"{relative}: {symbol} revived a runtime consumer fallback")
        for retired in TRUTHINESS_RETIRED_DECISIONS:
            if retired in body:
                errors.append(f"{relative}: {symbol} revived private truthiness rule: {retired}")
        surrogate = truthiness_surrogate_owner(body)
        if surrogate:
            errors.append(f"{relative}: {symbol} revived surrogate truthiness owner: {surrogate}")

    adapter_text = (root / "src/aot/xi_cgen.c").read_text(encoding="utf-8", errors="strict")
    adapter_body = extract_c_function(adapter_text, "cg_truthiness_adapter_name")
    if adapter_body is None:
        errors.append("src/aot/xi_cgen.c: stable-ID truthiness adapter resolver is missing")
    else:
        if f"{marker}_HI" not in adapter_body or f"{marker}_LO" not in adapter_body or \
                "xr_semantic_owner_cgen_adapter" not in adapter_body:
            errors.append("src/aot/xi_cgen.c: CGen no longer resolves truthiness by stable owner ID")
        surrogate = truthiness_surrogate_owner(adapter_body)
        if surrogate:
            errors.append(f"src/aot/xi_cgen.c: CGen revived surrogate truthiness owner: {surrogate}")

    for relative, symbol in CGEN_TRUTHINESS_CONSUMERS:
        text = (root / relative).read_text(encoding="utf-8", errors="strict")
        body = extract_c_function(text, symbol)
        if body is None:
            errors.append(f"{relative}: CGen truthiness consumer {symbol} is missing")
            continue
        if "cg_truthiness_adapter_name" not in body:
            errors.append(f"{relative}: {symbol} bypasses the stable owner adapter")
        if " != 0" in body:
            errors.append(f"{relative}: {symbol} revived a representation truthiness fallback")
        surrogate = truthiness_surrogate_owner(body)
        if surrogate:
            errors.append(f"{relative}: {symbol} revived surrogate truthiness owner: {surrogate}")

    for relative in ("src/aot/xi_cgen.c", "src/aot/xi_cgen_dispatch_helpers.inc.c"):
        text = (root / relative).read_text(encoding="utf-8", errors="strict")
        if '"xr_truthy(' in text:
            errors.append(f"{relative}: CGen revived a source-name truthiness binding")
    return errors


def verify_type_identity_ratchet(root: Path) -> list[str]:
    errors: list[str] = []
    marker = owner_macro_prefix("primitive.type-identity")
    for relative, symbol in TYPE_IDENTITY_CORE_CONSUMERS:
        text = (root / relative).read_text(encoding="utf-8", errors="strict")
        body = extract_c_function(text, symbol)
        if body is None:
            errors.append(f"{relative}: type-identity consumer {symbol} is missing")
            continue
        if f"{marker}_HI" not in body or f"{marker}_LO" not in body:
            errors.append(f"{relative}: {symbol} no longer consumes the stable type-identity owner ID")
        if "xr_type_identity_core_eval" not in body:
            errors.append(f"{relative}: {symbol} no longer delegates to the type-identity owner")
        if "xr_semantic_owner_has_consumer" in body:
            errors.append(f"{relative}: {symbol} revived a runtime consumer fallback")
        surrogate = type_identity_surrogate_owner(body)
        if surrogate:
            errors.append(f"{relative}: {symbol} revived surrogate type-identity owner: {surrogate}")

    vm_path = root / "src/vm/xvm_dispatch_convert.inc.c"
    vm_text = vm_path.read_text(encoding="utf-8", errors="strict")
    vm_start = vm_text.find("vmcase(OP_TYPEOF)")
    vm_end = vm_text.find("vmcase(", vm_start + 1) if vm_start >= 0 else -1
    vm_body = vm_text[vm_start:vm_end if vm_end >= 0 else len(vm_text)] if vm_start >= 0 else ""
    if f"{marker}_HI" not in vm_body or f"{marker}_LO" not in vm_body or \
            "xr_value_typeid_vm" not in vm_body:
        errors.append("src/vm/xvm_dispatch_convert.inc.c: OP_TYPEOF bypasses its stable owner binding")
    if "xr_value_typeid(val)" in vm_body:
        errors.append("src/vm/xvm_dispatch_convert.inc.c: OP_TYPEOF revived the runtime-consumer fallback")
    surrogate = type_identity_surrogate_owner(vm_body)
    if surrogate:
        errors.append(f"src/vm/xvm_dispatch_convert.inc.c: OP_TYPEOF revived surrogate owner: {surrogate}")

    cgen_path = root / "src/aot/xi_cgen.c"
    cgen_text = cgen_path.read_text(encoding="utf-8", errors="strict")
    adapter_body = extract_c_function(cgen_text, "cg_type_identity_adapter_name")
    if adapter_body is None:
        errors.append("src/aot/xi_cgen.c: stable-ID type-identity adapter resolver is missing")
    else:
        if f"{marker}_HI" not in adapter_body or f"{marker}_LO" not in adapter_body or \
                "xr_semantic_owner_cgen_adapter" not in adapter_body:
            errors.append("src/aot/xi_cgen.c: CGen no longer resolves type identity by stable owner ID")
        surrogate = type_identity_surrogate_owner(adapter_body)
        if surrogate:
            errors.append(f"src/aot/xi_cgen.c: CGen revived surrogate type-identity owner: {surrogate}")

    dispatch_path = root / "src/aot/xi_cgen_dispatch_helpers.inc.c"
    dispatch_text = dispatch_path.read_text(encoding="utf-8", errors="strict")
    typeid_body = extract_c_function(dispatch_text, "xicgen_typeid")
    if typeid_body is None or "cg_type_identity_adapter_name" not in typeid_body:
        errors.append("src/aot/xi_cgen_dispatch_helpers.inc.c: XI_TYPEID bypasses the stable owner adapter")
    if 'strcmp(bn, "typeOf")' in dispatch_text:
        errors.append("src/aot/xi_cgen_dispatch_helpers.inc.c: source-name typeOf fallback revived")
    for relative, text in (("src/aot/xi_cgen.c", cgen_text),
                           ("src/aot/xi_cgen_dispatch_helpers.inc.c", dispatch_text)):
        if '"xrt_typeof_id(' in text:
            errors.append(f"{relative}: CGen revived a literal type-identity adapter binding")
    return errors


def verify_exact_bits_ratchet(root: Path, registry: dict) -> list[str]:
    errors: list[str] = []
    marker = owner_macro_prefix("shared.bits")
    owner = next((row for row in registry.get("owners", [])
                  if row.get("owner") == "shared.bits"), None)
    if owner is None or set(owner.get("operations", [])) != EXACT_BITS_OPERATIONS:
        errors.append("semantic owner registry has no exact shared.bits operation family")

    core_text = (root / "src/shared/xr_bits_core.h").read_text(encoding="utf-8", errors="strict")
    if (f"{marker}_HI" not in core_text or f"{marker}_LO" not in core_text or
            "XR_BITS_EXACT_OWNER_APPLY" not in core_text):
        errors.append("src/shared/xr_bits_core.h: exact bit family lacks its stable owner guard")

    vm_text = (root / "src/vm/xvm_dispatch_bitwise.inc.c").read_text(
        encoding="utf-8", errors="strict")
    exact_start = vm_text.find("/* Exact-width bit intrinsics")
    exact_body = vm_text[exact_start:] if exact_start >= 0 else ""
    if (f"{marker}_HI" not in exact_body or f"{marker}_LO" not in exact_body or
            "XR_BITS_EXACT_OWNER_APPLY" not in exact_body):
        errors.append("src/vm/xvm_dispatch_bitwise.inc.c: VM exact bits bypass stable owner")
    for retired in ("xr_u64_mul_high", "__builtin_", "high = ((uint"):
        if retired in exact_body:
            errors.append(
                f"src/vm/xvm_dispatch_bitwise.inc.c: VM revived private exact-bit rule: {retired}")

    for relative in EXACT_BITS_AOT_BINDINGS:
        text = (root / relative).read_text(encoding="utf-8", errors="strict")
        if (f"{marker}_HI" not in text or f"{marker}_LO" not in text or
                "XR_BITS_EXACT_OWNER_APPLY" not in text or "xrt_bits_exact_eval" not in text):
            errors.append(f"{relative}: AOT exact-bit adapter bypasses stable owner")

    cgen_text = (root / "src/aot/xi_cgen.c").read_text(encoding="utf-8", errors="strict")
    adapter_body = extract_c_function(cgen_text, "cg_exact_bits_adapter_name")
    if (adapter_body is None or f"{marker}_HI" not in adapter_body or
            f"{marker}_LO" not in adapter_body or
            "xr_semantic_owner_cgen_adapter" not in adapter_body):
        errors.append("src/aot/xi_cgen.c: CGen exact bits do not resolve by stable owner ID")

    dispatch_text = (root / "src/aot/xi_cgen_dispatch_helpers.inc.c").read_text(
        encoding="utf-8", errors="strict")
    emitter_body = extract_c_function(dispatch_text, "xicgen_exact_bit")
    if (emitter_body is None or "cg_exact_bits_adapter_name" not in emitter_body or
            "cg_exact_bit_kernel_name" not in emitter_body):
        errors.append("src/aot/xi_cgen_dispatch_helpers.inc.c: exact bits bypass owner adapter")
    elif any(token in emitter_body for token in
             ("__builtin_", "XR_BITS_ROT", "xr_u64_mul_high", "sizeof(uintptr_t)")):
        errors.append("src/aot/xi_cgen_dispatch_helpers.inc.c: exact bits revived CGen semantics")
    return errors


def verify_bits_not_ratchet(root: Path, registry: dict) -> list[str]:
    errors: list[str] = []
    marker = owner_macro_prefix("shared.bits-not")
    owner = next((row for row in registry.get("owners", [])
                  if row.get("owner") == "shared.bits-not"), None)
    if owner is None or set(owner.get("operations", [])) != BITS_NOT_OPERATIONS:
        errors.append("semantic owner registry has no exact shared.bits-not operation family")

    core_text = (root / "src/shared/xr_bits_core.h").read_text(
        encoding="utf-8", errors="strict")
    if (f"{marker}_HI" not in core_text or f"{marker}_LO" not in core_text or
            "XR_BITS_NOT_OWNER_APPLY" not in core_text or "xr_bits_not_i64" not in core_text):
        errors.append("src/shared/xr_bits_core.h: bitwise-not lacks its stable owner kernel")

    vm_text = (root / "src/vm/xvm_dispatch_bitwise.inc.c").read_text(
        encoding="utf-8", errors="strict")
    vm_start = vm_text.find("#define XVM_TEMPLATE_BITWISE_UNARY_CASE")
    vm_end = vm_text.find("#undef XVM_TEMPLATE_BITWISE_UNARY_CASE", vm_start)
    vm_body = vm_text[vm_start:vm_end] if vm_start >= 0 and vm_end >= 0 else ""
    if (f"{marker}_HI" not in vm_body or f"{marker}_LO" not in vm_body or
            "XR_BITS_NOT_OWNER_APPLY" not in vm_body):
        errors.append("src/vm/xvm_dispatch_bitwise.inc.c: VM bitwise-not bypasses stable owner")
    if any(token in vm_body for token in ("VM_TRY_UNARY_OP_OVERLOAD", "SYMBOL_OP_BNOT", "~")):
        errors.append("src/vm/xvm_dispatch_bitwise.inc.c: retired VM bitwise-not semantics revived")

    vm_gen_text = (root / "src/vm/xvm_template_bitwise_unary_gen.inc.c").read_text(
        encoding="utf-8", errors="strict")
    if ("XVM_TEMPLATE_BITWISE_UNARY_CASE(OP_BNOT," not in vm_gen_text or
            any(token in vm_gen_text for token in ("SYMBOL_OP_BNOT", "~"))):
        errors.append("src/vm/xvm_template_bitwise_unary_gen.inc.c: generated VM semantics revived")

    for relative in BITS_NOT_AOT_BINDINGS:
        text = (root / relative).read_text(encoding="utf-8", errors="strict")
        if (f"{marker}_HI" not in text or f"{marker}_LO" not in text or
                "XR_BITS_NOT_OWNER_APPLY" not in text or "xrt_bits_not_eval" not in text):
            errors.append(f"{relative}: AOT bitwise-not adapter bypasses stable owner")

    cgen_text = (root / "src/aot/xi_cgen.c").read_text(encoding="utf-8", errors="strict")
    adapter_body = extract_c_function(cgen_text, "cg_bits_not_adapter_name")
    if (adapter_body is None or f"{marker}_HI" not in adapter_body or
            f"{marker}_LO" not in adapter_body or
            "xr_semantic_owner_cgen_adapter" not in adapter_body):
        errors.append("src/aot/xi_cgen.c: CGen bitwise-not does not resolve by stable owner ID")
    if "emit_bitwise_unop_ctx" in cgen_text:
        errors.append("src/aot/xi_cgen.c: retired raw bitwise-not emitter revived")

    dispatch_text = (root / "src/aot/xi_cgen_dispatch_helpers.inc.c").read_text(
        encoding="utf-8", errors="strict")
    emitter_body = extract_c_function(dispatch_text, "xicgen_template_bitwise_unary")
    if emitter_body is None or "cg_bits_not_adapter_name" not in emitter_body:
        errors.append("src/aot/xi_cgen_dispatch_helpers.inc.c: bitwise-not bypasses owner adapter")
    elif any(token in emitter_body for token in
             ("~", "xi_to_c_template_bitwise_unary_op", "emit_bitwise_unop_ctx")):
        errors.append("src/aot/xi_cgen_dispatch_helpers.inc.c: CGen bitwise-not semantics revived")

    dispatch_gen_text = (root / "src/aot/xi_to_c_dispatch_gen.h").read_text(
        encoding="utf-8", errors="strict")
    if "xi_to_c_template_bitwise_unary_op" in dispatch_gen_text:
        errors.append("src/aot/xi_to_c_dispatch_gen.h: generated raw bitwise-not operator revived")

    optimizer_text = (root / "src/ir/xi_opt.c").read_text(encoding="utf-8", errors="strict")
    optimizer_body = extract_c_function(optimizer_text, "xi_opt_const_fold")
    raw_fold = (optimizer_body is not None and
                re.search(r"rewrite_to_const_int\s*\(\s*v\s*,\s*~", optimizer_body))
    if optimizer_body is None or "xr_bits_not_i64(unary_i)" not in optimizer_body:
        errors.append("src/ir/xi_opt.c: constant-folded bitwise-not bypasses shared owner kernel")
    if raw_fold:
        errors.append("src/ir/xi_opt.c: retired raw constant-folded bitwise-not revived")
    return errors


def verify_range_ratchet(root: Path, registry: dict) -> list[str]:
    errors: list[str] = []
    marker = owner_macro_prefix("shared.range")
    owner = next((row for row in registry.get("owners", [])
                  if row.get("owner") == "shared.range"), None)
    if owner is None or set(owner.get("operations", [])) != RANGE_OPERATIONS:
        errors.append("semantic owner registry has no exact shared.range operation family")

    core_text = (root / "src/shared/xr_range_core.h").read_text(
        encoding="utf-8", errors="strict")
    for token in (f"{marker}_HI", f"{marker}_LO", "XR_RANGE_OWNER_APPLY",
                  "xr_range_core_make_with_bound", "xr_range_core_length",
                  "xr_range_core_contains", "xr_range_core_index",
                  "xr_range_core_format_buf"):
        if token not in core_text:
            errors.append(f"src/shared/xr_range_core.h: shared Range owner lacks {token}")

    vm_text = (root / "src/vm/xvm_dispatch_collection.inc.c").read_text(
        encoding="utf-8", errors="strict")
    for token in (f"{marker}_HI", f"{marker}_LO", "XR_RANGE_OWNER_APPLY",
                  "xr_range_from_core"):
        if token not in vm_text:
            errors.append(f"src/vm/xvm_dispatch_collection.inc.c: VM Range lacks {token}")

    runtime_header = (root / "src/runtime/object/xrange.h").read_text(
        encoding="utf-8", errors="strict")
    runtime_source = (root / "src/runtime/object/xrange.c").read_text(
        encoding="utf-8", errors="strict")
    value_format = (root / "src/runtime/value/xvalue_format.c").read_text(
        encoding="utf-8", errors="strict")
    hosted_range = (root / "src/aot/xrt_range.h").read_text(
        encoding="utf-8", errors="strict")
    retired = (
        "xr_range_new(", "xr_range_new_with_step(", "xr_range_len_from_distance(",
        "xrt_range_new_raw(", "xrt_range_from_i64(", "xrt_range_len_from_distance(",
    )
    for relative, text in (
            ("src/runtime/object/xrange.h", runtime_header),
            ("src/runtime/object/xrange.c", runtime_source),
            ("src/aot/xrt_range.h", hosted_range)):
        for token in retired:
            if token in text:
                errors.append(f"{relative}: retired private Range path revived: {token}")

    runtime_format = extract_c_function(runtime_source, "m_range_to_string")
    if runtime_format is None or "xr_range_core_format_buf" not in runtime_format:
        errors.append("src/runtime/object/xrange.c: VM Range formatting bypasses shared core")
    if "xr_range_core_format_buf(xr_range_core_view(rng)" not in value_format:
        errors.append("src/runtime/value/xvalue_format.c: generic Range formatting bypasses core")
    if re.search(r"rng->start\s*\+.*rng->step", vm_text):
        errors.append("src/vm/xvm_dispatch_collection.inc.c: raw Range indexing revived")
    for helper in ("xrt_range_length_ptr", "xrt_range_contains_ptr",
                   "xrt_range_index_ptr", "xrt_range_format_buf"):
        body = extract_c_function(hosted_range, helper)
        if body is None or "xr_range_core_" not in body:
            errors.append(f"src/aot/xrt_range.h: {helper} bypasses shared Range core")

    for relative in RANGE_AOT_BINDINGS:
        text = (root / relative).read_text(encoding="utf-8", errors="strict")
        if (f"{marker}_HI" not in text or f"{marker}_LO" not in text or
                "XR_RANGE_OWNER_APPLY" not in text or "xrt_range_semantics" not in text):
            errors.append(f"{relative}: AOT Range adapter bypasses stable owner")

    cgen_text = (root / "src/aot/xi_cgen.c").read_text(
        encoding="utf-8", errors="strict")
    adapter_body = extract_c_function(cgen_text, "cg_range_adapter_name")
    if (adapter_body is None or f"{marker}_HI" not in adapter_body or
            f"{marker}_LO" not in adapter_body or
            "xr_semantic_owner_cgen_adapter" not in adapter_body):
        errors.append("src/aot/xi_cgen.c: CGen Range does not resolve by stable owner ID")

    dispatch_text = (root / "src/aot/xi_cgen_dispatch_helpers.inc.c").read_text(
        encoding="utf-8", errors="strict")
    emitter_body = extract_c_function(dispatch_text, "xicgen_range")
    if (emitter_body is None or "cg_range_adapter_name" not in emitter_body or
            "xrt_range_from_core" not in emitter_body):
        errors.append("src/aot/xi_cgen_dispatch_helpers.inc.c: Range bypasses owner adapter")
    elif any(token in emitter_body for token in ("xrt_range_from_i64", "xrt_range_new_raw")):
        errors.append("src/aot/xi_cgen_dispatch_helpers.inc.c: retired Range semantics revived")
    return errors


def verify_shift_ratchet(root: Path, registry: dict) -> list[str]:
    errors: list[str] = []
    marker = owner_macro_prefix("shared.shift")
    owner = next((row for row in registry.get("owners", [])
                  if row.get("owner") == "shared.shift"), None)
    if owner is None or set(owner.get("operations", [])) != SHIFT_OPERATIONS:
        errors.append("semantic owner registry has no exact shared.shift operation family")

    core_text = (root / "src/shared/xr_bits_core.h").read_text(
        encoding="utf-8", errors="strict")
    for token in (f"{marker}_HI", f"{marker}_LO", "XR_SHIFT_OWNER_APPLY",
                  "XR_SHIFT_BIGINT_OWNER_PLAN", "XR_SHIFT_BIGINT_OWNER_APPLY",
                  "xr_shift_i64", "xr_shift_bigint_plan", "xr_shift_bigint_apply"):
        if token not in core_text:
            errors.append(f"src/shared/xr_bits_core.h: shared shift owner lacks {token}")

    vm_text = (root / "src/vm/xvm_dispatch_bitwise.inc.c").read_text(
        encoding="utf-8", errors="strict")
    vm_start = vm_text.find("#define XVM_TEMPLATE_SHIFT_CASE")
    vm_end = vm_text.find("#undef XVM_TEMPLATE_SHIFT_CASE", vm_start)
    vm_body = vm_text[vm_start:vm_end] if vm_start >= 0 and vm_end > vm_start else ""
    if "XR_SHIFT_OWNER_APPLY" not in vm_body or "xr_bigint_shift" not in vm_body:
        errors.append("src/vm/xvm_dispatch_bitwise.inc.c: VM shift bypasses shared owner adapters")
    for retired in ("xr_int_shl_wrap", "xr_int_shr_wrap", "xr_int_shr_u_wrap",
                    "xr_bigint_shl", "xr_bigint_shr"):
        if retired in vm_body:
            errors.append(f"src/vm/xvm_dispatch_bitwise.inc.c: retired shift path revived: {retired}")

    runtime_text = (root / "src/runtime/object/xbigint.c").read_text(
        encoding="utf-8", errors="strict")
    runtime_body = extract_c_function(runtime_text, "xr_bigint_shift")
    if (runtime_body is None or "XR_SHIFT_BIGINT_OWNER_PLAN" not in runtime_body or
            "XR_SHIFT_BIGINT_OWNER_APPLY" not in runtime_body):
        errors.append("src/runtime/object/xbigint.c: BigInt representation adapter bypasses owner")
    if runtime_body and ("<<" in runtime_body or ">>" in runtime_body):
        errors.append("src/runtime/object/xbigint.c: raw BigInt shift semantics revived")

    for rel in SHIFT_AOT_BINDINGS:
        text = (root / rel).read_text(encoding="utf-8", errors="strict")
        if ("xrt_shift_eval" not in text or f"{marker}_HI" not in text or
                f"{marker}_LO" not in text or "XR_SHIFT_OWNER_APPLY" not in text):
            errors.append(f"{rel}: AOT shift adapter bypasses shared owner")
        for retired in ("xrt_i64_shl", "xrt_i64_shr", "xrt_i64_shr_u"):
            if retired in text:
                errors.append(f"{rel}: retired scalar shift adapter revived: {retired}")

    hosted_text = (root / "src/aot/xrt_arith.h").read_text(encoding="utf-8", errors="strict")
    hosted_body = extract_c_function(hosted_text, "xrt_bigint_shift_val")
    if (hosted_body is None or "XR_SHIFT_BIGINT_OWNER_PLAN" not in hosted_body or
            "XR_SHIFT_BIGINT_OWNER_APPLY" not in hosted_body):
        errors.append("src/aot/xrt_arith.h: hosted BigInt shift bypasses shared owner")
    if hosted_body and ("<<" in hosted_body or ">>" in hosted_body):
        errors.append("src/aot/xrt_arith.h: raw BigInt shift semantics revived")

    cgen_text = (root / "src/aot/xi_cgen.c").read_text(encoding="utf-8", errors="strict")
    adapter_body = extract_c_function(cgen_text, "cg_shift_adapter_name")
    emitter_body = extract_c_function(cgen_text, "emit_shift_binop_ctx")
    if (adapter_body is None or f"{marker}_HI" not in adapter_body or
            f"{marker}_LO" not in adapter_body or
            "xr_semantic_owner_cgen_adapter" not in adapter_body):
        errors.append("src/aot/xi_cgen.c: CGen shift does not resolve by stable owner ID")
    if (emitter_body is None or "cg_shift_adapter_name" not in emitter_body or
            "xrt_bigint_shift_val" not in emitter_body):
        errors.append("src/aot/xi_cgen.c: CGen shift bypasses mechanical owner adapters")
    if emitter_body and ("<<" in emitter_body or ">>" in emitter_body):
        errors.append("src/aot/xi_cgen.c: raw C shift semantics revived in shift emitter")
    if re.search(r"emit_native_[A-Za-z0-9_]*shift", cgen_text):
        errors.append("src/aot/xi_cgen.c: retired native shift fastpath revived")

    optimizer_text = (root / "src/ir/xi_opt.c").read_text(encoding="utf-8", errors="strict")
    optimizer_body = extract_c_function(optimizer_text, "fold_int_binary")
    sccp_text = (root / "src/ir/xi_opt_sccp.c").read_text(encoding="utf-8", errors="strict")
    sccp_body = extract_c_function(sccp_text, "eval_bitwise")
    if optimizer_body is None or optimizer_body.count("xr_shift_i64") < 2:
        errors.append("src/ir/xi_opt.c: shift constant folding bypasses shared kernel")
    if sccp_body is None or sccp_body.count("xr_shift_i64") < 2:
        errors.append("src/ir/xi_opt_sccp.c: SCCP shift folding bypasses shared kernel")

    retired_sources = (
        "src/shared/xr_int_arith.h",
        "src/shared/xr_numeric_core.h",
        "src/runtime/value/xvalue.h",
        "src/runtime/object/xbigint.h",
        "src/runtime/object/xbigint.c",
        "src/aot/xrt_arith.h",
        "src/aot/xrt_core_freestanding.h",
        "tools/xisagen/xisagen.py",
        "src/vm/xvm_template_shift_gen.inc.c",
        "src/aot/xi_to_c_dispatch_gen.h",
    )
    retired_tokens = (
        "xr_i64_shl_wrap", "xr_i64_shr_wrap", "xr_i64_shr_u_wrap",
        "xr_int_shl_wrap", "xr_int_shr_wrap", "xr_int_shr_u_wrap",
        "xr_numeric_core_i64_shl_wrap", "xr_numeric_core_i64_shr_wrap",
        "xr_bigint_shl", "xr_bigint_shr", "xrt_bigint_shl_val", "xrt_bigint_shr_val",
        "xrt_i64_shl", "xrt_i64_shr", "xrt_i64_shr_u",
    )
    for rel in retired_sources:
        text = (root / rel).read_text(encoding="utf-8", errors="strict")
        for token in retired_tokens:
            if token in text:
                errors.append(f"{rel}: retired shift semantic source remains: {token}")
    return errors


def verify_numeric_neg_ratchet(root: Path, registry: dict) -> list[str]:
    errors: list[str] = []
    marker = owner_macro_prefix("shared.numeric-neg")
    owner = next((row for row in registry.get("owners", [])
                  if row.get("owner") == "shared.numeric-neg"), None)
    if owner is None or set(owner.get("operations", [])) != NUMERIC_NEG_OPERATIONS:
        errors.append("semantic owner registry has no exact shared.numeric-neg operation family")

    core_text = (root / "src/shared/xr_numeric_core.h").read_text(
        encoding="utf-8", errors="strict")
    for token in (f"{marker}_HI", f"{marker}_LO", "XR_NUMERIC_NEG_OWNER_APPLY",
                  "XR_NUMERIC_NEG_BIGINT_OWNER_PLAN", "XR_NUMERIC_NEG_KIND_GUARD",
                  "xr_numeric_neg_eval",
                  "xr_numeric_neg_bigint_plan"):
        if token not in core_text:
            errors.append(f"src/shared/xr_numeric_core.h: shared numeric-neg owner lacks {token}")

    vm_text = (root / "src/vm/xvm_dispatch_arith.inc.c").read_text(
        encoding="utf-8", errors="strict")
    vm_start = vm_text.find("#define XVM_TEMPLATE_UNARY_NEG_CASE")
    vm_end = vm_text.find("#define XVM_TEMPLATE_UNARY_NOT_CASE", vm_start)
    vm_body = vm_text[vm_start:vm_end] if vm_start >= 0 and vm_end > vm_start else ""
    for token in (f"{marker}_HI", f"{marker}_LO", "XR_NUMERIC_NEG_OWNER_APPLY",
                  "xr_bigint_neg"):
        if token not in vm_body:
            errors.append(f"src/vm/xvm_dispatch_arith.inc.c: VM numeric neg lacks {token}")
    for token in ("XVM_TRY_UNARY_OP_OVERLOAD", "-(uint64_t)", "-XR_TO_FLOAT"):
        if token in vm_body:
            errors.append(
                f"src/vm/xvm_dispatch_arith.inc.c: retired numeric-neg semantics revived: {token}")

    vm_generated = (root / "src/vm/xvm_template_unary_gen.inc.c").read_text(
        encoding="utf-8", errors="strict")
    if "XVM_TEMPLATE_UNARY_NEG_CASE(OP_UNM)" not in vm_generated or \
            'XVM_TEMPLATE_UNARY_NEG_CASE(OP_UNM, "-")' in vm_generated:
        errors.append("src/vm/xvm_template_unary_gen.inc.c: numeric-neg operator alias revived")

    runtime_text = (root / "src/runtime/object/xbigint.c").read_text(
        encoding="utf-8", errors="strict")
    runtime_body = extract_c_function(runtime_text, "xr_bigint_neg")
    if runtime_body is None or "XR_NUMERIC_NEG_BIGINT_OWNER_PLAN" not in runtime_body:
        errors.append("src/runtime/object/xbigint.c: BigInt neg bypasses shared owner")
    elif "result->sign = -" in runtime_body or "result->sign = -result->sign" in runtime_body:
        errors.append("src/runtime/object/xbigint.c: private BigInt sign negation revived")

    hosted_arith = (root / "src/aot/xrt_arith.h").read_text(
        encoding="utf-8", errors="strict")
    hosted_neg = extract_c_function(hosted_arith, "xrt_neg")
    hosted_bigint_neg = extract_c_function(hosted_arith, "xrt_bigint_neg_val")
    if hosted_neg is None or hosted_neg.count("XR_NUMERIC_NEG_OWNER_APPLY") != 2:
        errors.append("src/aot/xrt_arith.h: hosted scalar neg bypasses shared owner")
    elif any(token in hosted_neg for token in ("xr_i64_neg_wrap", "-a.f",
                                                "return XR_FROM_INT(0)")):
        errors.append("src/aot/xrt_arith.h: retired hosted scalar neg semantics revived")
    if hosted_bigint_neg is None or "XR_NUMERIC_NEG_BIGINT_OWNER_PLAN" not in hosted_bigint_neg:
        errors.append("src/aot/xrt_arith.h: hosted BigInt neg bypasses shared owner")
    elif "r->sign = -" in hosted_bigint_neg or "xrt_bigint_is_zero_v" in hosted_bigint_neg:
        errors.append("src/aot/xrt_arith.h: private hosted BigInt neg semantics revived")

    for relative in NUMERIC_NEG_AOT_BINDINGS:
        text = (root / relative).read_text(encoding="utf-8", errors="strict")
        if (f"{marker}_HI" not in text or f"{marker}_LO" not in text or
                "XR_NUMERIC_NEG_OWNER_APPLY" not in text or
                "xrt_numeric_neg_eval" not in text):
            errors.append(f"{relative}: AOT numeric-neg adapter bypasses stable owner")

    cgen_text = (root / "src/aot/xi_cgen.c").read_text(
        encoding="utf-8", errors="strict")
    adapter_body = extract_c_function(cgen_text, "cg_numeric_neg_adapter_name")
    if (adapter_body is None or f"{marker}_HI" not in adapter_body or
            f"{marker}_LO" not in adapter_body or
            "xr_semantic_owner_cgen_adapter" not in adapter_body):
        errors.append("src/aot/xi_cgen.c: CGen numeric-neg does not resolve by stable owner ID")
    for function_name in ("cg_const_int_value_matches_bits",
                          "cg_const_float_value_matches_literal"):
        body = extract_c_function(cgen_text, function_name)
        if body is None or "xr_numeric_neg_eval" not in body:
            errors.append(f"src/aot/xi_cgen.c: {function_name} bypasses shared numeric-neg core")

    dispatch_text = (root / "src/aot/xi_cgen_dispatch_helpers.inc.c").read_text(
        encoding="utf-8", errors="strict")
    emitter_body = extract_c_function(dispatch_text, "xicgen_neg")
    if emitter_body is None or "cg_numeric_neg_adapter_name" not in emitter_body or \
            "xrt_neg" not in emitter_body:
        errors.append("src/aot/xi_cgen_dispatch_helpers.inc.c: numeric-neg bypasses owner adapters")
    elif any(token in emitter_body for token in
             ("-(uint64_t)", 'fprintf(out, "-")', "xicgen_emit_bigint_literal_value")):
        errors.append("src/aot/xi_cgen_dispatch_helpers.inc.c: raw numeric-neg semantics revived")

    optimizer_text = (root / "src/ir/xi_opt.c").read_text(encoding="utf-8", errors="strict")
    optimizer_body = extract_c_function(optimizer_text, "xi_opt_const_fold")
    if optimizer_body is None or optimizer_body.count("xr_numeric_neg_eval") < 2:
        errors.append("src/ir/xi_opt.c: numeric-neg constant folding bypasses shared core")
    sccp_text = (root / "src/ir/xi_opt_sccp.c").read_text(encoding="utf-8", errors="strict")
    sccp_body = extract_c_function(sccp_text, "eval_unary")
    if sccp_body is None or sccp_body.count("xr_numeric_neg_eval") < 2:
        errors.append("src/ir/xi_opt_sccp.c: SCCP numeric-neg bypasses shared core")
    return errors


def verify_bitwise_binary_ratchet(root: Path, registry: dict) -> list[str]:
    errors: list[str] = []
    marker = owner_macro_prefix("shared.bitwise-binary")
    owner = next((row for row in registry.get("owners", [])
                  if row.get("owner") == "shared.bitwise-binary"), None)
    if owner is None or set(owner.get("operations", [])) != BITWISE_BINARY_OPERATIONS:
        errors.append(
            "semantic owner registry has no exact shared.bitwise-binary operation family")

    core_text = (root / "src/shared/xr_bits_core.h").read_text(
        encoding="utf-8", errors="strict")
    for token in (f"{marker}_HI", f"{marker}_LO", "XR_BITWISE_BINARY_OWNER_APPLY",
                  "XR_BITWISE_BINARY_BIGINT_OWNER_PLAN",
                  "XR_BITWISE_BINARY_BIGINT_OWNER_APPLY", "xr_bitwise_binary_i64",
                  "xr_bitwise_binary_bigint_plan", "xr_bitwise_binary_bigint_apply"):
        if token not in core_text:
            errors.append(
                f"src/shared/xr_bits_core.h: shared bitwise-binary owner lacks {token}")

    vm_text = (root / "src/vm/xvm_dispatch_bitwise.inc.c").read_text(
        encoding="utf-8", errors="strict")
    vm_start = vm_text.find("#define XVM_TEMPLATE_BITWISE_BINARY_CASE")
    vm_end = vm_text.find("#undef XVM_TEMPLATE_BITWISE_BINARY_CASE", vm_start)
    vm_body = vm_text[vm_start:vm_end] if vm_start >= 0 and vm_end > vm_start else ""
    if "XR_BITWISE_BINARY_OWNER_APPLY" not in vm_body or "xr_bigint_bitwise" not in vm_body:
        errors.append("src/vm/xvm_dispatch_bitwise.inc.c: VM bitwise binary bypasses owner")
    if any(token in vm_body for token in
           ("VM_TRY_BINARY_OP_OVERLOAD", "SYMBOL_OP_BAND", "SYMBOL_OP_BOR", "SYMBOL_OP_BXOR",
            "xr_bigint_and", "xr_bigint_or", "xr_bigint_xor")):
        errors.append("src/vm/xvm_dispatch_bitwise.inc.c: retired VM bitwise semantics revived")

    vm_gen_text = (root / "src/vm/xvm_template_bitwise_binary_gen.inc.c").read_text(
        encoding="utf-8", errors="strict")
    if (vm_gen_text.count("XVM_TEMPLATE_BITWISE_BINARY_CASE(") != 3 or
            any(token in vm_gen_text for token in
                ("XVM_TEMPLATE_BITWISE_BINARY_BOOL_CASE", "xr_bigint_and", "xr_bigint_or",
                 "xr_bigint_xor", "SYMBOL_OP_BAND", "SYMBOL_OP_BOR", "SYMBOL_OP_BXOR"))):
        errors.append("src/vm/xvm_template_bitwise_binary_gen.inc.c: generated semantics revived")

    runtime_text = (root / "src/runtime/object/xbigint.c").read_text(
        encoding="utf-8", errors="strict")
    runtime_body = extract_c_function(runtime_text, "xr_bigint_bitwise")
    if (runtime_body is None or "XR_BITWISE_BINARY_BIGINT_OWNER_PLAN" not in runtime_body or
            "XR_BITWISE_BINARY_BIGINT_OWNER_APPLY" not in runtime_body):
        errors.append("src/runtime/object/xbigint.c: BigInt bitwise adapter bypasses owner")

    for relative in BITWISE_BINARY_AOT_BINDINGS:
        text = (root / relative).read_text(encoding="utf-8", errors="strict")
        if (f"{marker}_HI" not in text or f"{marker}_LO" not in text or
                "XR_BITWISE_BINARY_OWNER_APPLY" not in text or
                "xrt_bitwise_binary_eval" not in text):
            errors.append(f"{relative}: AOT bitwise-binary adapter bypasses shared owner")

    hosted_text = (root / "src/aot/xrt_arith.h").read_text(
        encoding="utf-8", errors="strict")
    hosted_body = extract_c_function(hosted_text, "xrt_bigint_bitwise_val")
    if (hosted_body is None or "XR_BITWISE_BINARY_BIGINT_OWNER_PLAN" not in hosted_body or
            "XR_BITWISE_BINARY_BIGINT_OWNER_APPLY" not in hosted_body):
        errors.append("src/aot/xrt_arith.h: hosted BigInt bitwise bypasses shared owner")

    cgen_text = (root / "src/aot/xi_cgen.c").read_text(encoding="utf-8", errors="strict")
    adapter_body = extract_c_function(cgen_text, "cg_bitwise_binary_adapter_name")
    emitter_body = extract_c_function(cgen_text, "emit_bitwise_binop_ctx")
    if (adapter_body is None or f"{marker}_HI" not in adapter_body or
            f"{marker}_LO" not in adapter_body or
            "xr_semantic_owner_cgen_adapter" not in adapter_body):
        errors.append("src/aot/xi_cgen.c: CGen bitwise binary does not resolve by owner ID")
    if (emitter_body is None or "cg_bitwise_binary_adapter_name" not in emitter_body or
            "xrt_bigint_bitwise_val" not in emitter_body):
        errors.append("src/aot/xi_cgen.c: CGen bitwise binary bypasses mechanical adapters")
    if emitter_body and re.search(r"\)\s*[&|^]\s*\(", emitter_body):
        errors.append("src/aot/xi_cgen.c: raw C bitwise-binary semantics revived")

    optimizer_text = (root / "src/ir/xi_opt.c").read_text(encoding="utf-8", errors="strict")
    optimizer_body = extract_c_function(optimizer_text, "fold_int_binary")
    sccp_text = (root / "src/ir/xi_opt_sccp.c").read_text(encoding="utf-8", errors="strict")
    sccp_body = extract_c_function(sccp_text, "eval_bitwise")
    if optimizer_body is None or optimizer_body.count("xr_bitwise_binary_i64") != 3:
        errors.append("src/ir/xi_opt.c: bitwise-binary folding bypasses shared owner")
    if sccp_body is None or sccp_body.count("xr_bitwise_binary_i64") != 3:
        errors.append("src/ir/xi_opt_sccp.c: bitwise-binary SCCP bypasses shared owner")

    retired_sources = (
        "src/runtime/object/xbigint.h", "src/runtime/object/xbigint.c",
        "src/aot/xrt_arith.h", "src/aot/xi_cgen.c",
        "src/vm/xvm_template_bitwise_binary_gen.inc.c", "src/aot/xi_to_c_dispatch_gen.h",
    )
    retired_tokens = (
        "xr_bigint_and", "xr_bigint_or", "xr_bigint_xor", "xrt_bigint_and_val",
        "xrt_bigint_or_val", "xrt_bigint_xor_val", "xrt_bi_to_twos", "xrt_bi_from_twos",
        "xi_to_c_template_bitwise_binary_op", "XVM_TEMPLATE_BITWISE_BINARY_BOOL_CASE",
    )
    for rel in retired_sources:
        text = (root / rel).read_text(encoding="utf-8", errors="strict")
        for token in retired_tokens:
            if token in text:
                errors.append(f"{rel}: retired bitwise-binary semantic source remains: {token}")
    return errors


def verify_numeric_width_ratchet(root: Path, registry: dict) -> list[str]:
    errors: list[str] = []
    marker = owner_macro_prefix("shared.numeric-conversion")
    owner = next((row for row in registry.get("owners", [])
                  if row.get("owner") == "shared.numeric-conversion"), None)
    if owner is None or set(owner.get("operations", [])) != NUMERIC_WIDTH_OPERATIONS:
        errors.append(
            "semantic owner registry has no exact shared.numeric-conversion width family")

    core_text = (root / "src/shared/xr_numeric_conversion_core.h").read_text(
        encoding="utf-8", errors="strict")
    if (f"{marker}_HI" not in core_text or f"{marker}_LO" not in core_text or
            "XR_NUMERIC_WIDTH_OWNER_APPLY" not in core_text or
            not NUMERIC_WIDTH_KERNELS.issubset(set(SIGNATURE_RE.findall(core_text)))):
        errors.append(
            "src/shared/xr_numeric_conversion_core.h: numeric width family lacks stable owner kernels")

    vm_text = (root / "src/vm/xvm_template_width_gen.inc.c").read_text(
        encoding="utf-8", errors="strict")
    if (f"{marker}_HI" not in vm_text or f"{marker}_LO" not in vm_text or
            "XR_NUMERIC_WIDTH_OWNER_APPLY" not in vm_text or
            not NUMERIC_WIDTH_KERNELS.issubset(set(re.findall(
                r"\b(xr_numeric_(?:narrow|widen)_[a-z0-9]+)\b", vm_text)))):
        errors.append(
            "src/vm/xvm_template_width_gen.inc.c: VM numeric width bypasses stable owner")
    if "xr_numeric_int_convert_i64" in vm_text or "xr_numeric_f64_to_f32" in vm_text:
        errors.append(
            "src/vm/xvm_template_width_gen.inc.c: VM revived private numeric width semantics")

    for relative in EXACT_BITS_AOT_BINDINGS:
        text = (root / relative).read_text(encoding="utf-8", errors="strict")
        if (f"{marker}_HI" not in text or f"{marker}_LO" not in text or
                "XR_NUMERIC_WIDTH_OWNER_APPLY" not in text or
                "xrt_numeric_width_eval" not in text):
            errors.append(f"{relative}: AOT numeric width adapter bypasses stable owner")

    cgen_text = (root / "src/aot/xi_cgen.c").read_text(encoding="utf-8", errors="strict")
    adapter_body = extract_c_function(cgen_text, "cg_numeric_width_adapter_name")
    if (adapter_body is None or f"{marker}_HI" not in adapter_body or
            f"{marker}_LO" not in adapter_body or
            "xr_semantic_owner_cgen_adapter" not in adapter_body):
        errors.append("src/aot/xi_cgen.c: CGen numeric width does not resolve by stable owner ID")

    dispatch_text = (root / "src/aot/xi_cgen_dispatch_helpers.inc.c").read_text(
        encoding="utf-8", errors="strict")
    abi_text = (root / "src/aot/xi_cgen_abi_helpers.inc.c").read_text(
        encoding="utf-8", errors="strict")
    array_text = (root / "src/aot/xi_cgen_array_helpers.inc.c").read_text(
        encoding="utf-8", errors="strict")
    emitter_body = extract_c_function(dispatch_text, "xicgen_numeric_width")
    if (emitter_body is None or "cg_numeric_width_adapter_name" not in emitter_body or
            "xi_to_c_template_width_numeric_kernel" not in emitter_body):
        errors.append(
            "src/aot/xi_cgen_dispatch_helpers.inc.c: numeric width bypasses owner adapter")
    elif any(token in emitter_body for token in
             ("xr_numeric_int_convert_i64", "xr_numeric_f64_to_f32", "(uint8_t)",
              "(uint16_t)", "(uint32_t)", "(int8_t)", "(int16_t)", "(int32_t)")):
        errors.append(
            "src/aot/xi_cgen_dispatch_helpers.inc.c: numeric width revived CGen semantics")
    retired = (
        "xicgen_unsigned_narrow_lowbits_binop",
        "cg_unsigned_narrow_lowbits_binop_arg",
        "cg_unsigned_narrow_cast_ctype",
        "cg_lowbits_binop_elided_into_unsigned_narrow",
        "xicgen_convert_i64_width",
        "xicgen_f32_roundtrip",
        "cg_int_widen_source_rep",
        "cg_int_widen_inner_value_rep",
        "cg_int_widen_can_use_inner_for_slot",
        "cg_int_widen_use_consumes_inner",
        "cg_array_index_get_reads_f32_storage",
        "XR_NUMERIC_NARROW_OWNER_APPLY",
        "xrt_numeric_narrow_eval",
        "cg_numeric_narrow_adapter_name",
        "xicgen_numeric_narrow",
        "xi_to_c_template_width_narrow_kernel",
    )
    for symbol in retired:
        if (symbol in cgen_text or symbol in dispatch_text or symbol in abi_text or
                symbol in array_text or symbol in core_text or symbol in vm_text):
            errors.append(f"retired private numeric width path remains: {symbol}")
    return errors


def verify_byte_slice_scalar_ratchet(root: Path, registry: dict) -> list[str]:
    errors: list[str] = []
    marker = owner_macro_prefix("shared.byte-slice-scalar")
    owner = next((row for row in registry.get("owners", [])
                  if row.get("owner") == "shared.byte-slice-scalar"), None)
    if owner is None or set(owner.get("operations", [])) != BYTE_SLICE_SCALAR_OPERATIONS:
        errors.append(
            "semantic owner registry has no exact shared.byte-slice-scalar operation family")

    core_text = (root / "src/shared/xr_byte_slice_scalar_core.h").read_text(
        encoding="utf-8", errors="strict")
    if (f"{marker}_HI" not in core_text or f"{marker}_LO" not in core_text or
            "XR_BYTE_SLICE_SCALAR_OWNER_APPLY" not in core_text or
            not BYTE_SLICE_SCALAR_KERNELS.issubset(set(SIGNATURE_RE.findall(core_text)))):
        errors.append(
            "src/shared/xr_byte_slice_scalar_core.h: scalar I/O family lacks stable owner kernels")

    array_text = (root / "src/shared/xr_array_core.h").read_text(
        encoding="utf-8", errors="strict")
    freestanding_text = (root / "src/aot/xrt_core_freestanding.h").read_text(
        encoding="utf-8", errors="strict")
    for relative, text in (("src/shared/xr_array_core.h", array_text),
                           ("src/aot/xrt_core_freestanding.h", freestanding_text)):
        duplicate_kernels = BYTE_SLICE_SCALAR_KERNELS.intersection(set(SIGNATURE_RE.findall(text)))
        if duplicate_kernels:
            errors.append(
                f"{relative}: revived private byte-slice scalar kernels: " +
                ", ".join(sorted(duplicate_kernels)))

    vm_text = (root / "src/vm/xvm_dispatch_collection.inc.c").read_text(
        encoding="utf-8", errors="strict")
    vm_start = vm_text.find("#define VM_BYTE_SLICE_LOAD_CASE")
    vm_end = vm_text.find("#undef VM_BYTE_SLICE_LOAD_CASE", vm_start)
    vm_body = vm_text[vm_start:vm_end] if vm_start >= 0 and vm_end >= 0 else ""
    if (f"{marker}_HI" not in vm_body or f"{marker}_LO" not in vm_body or
            "XR_BYTE_SLICE_SCALAR_OWNER_APPLY" not in vm_body or
            not BYTE_SLICE_SCALAR_KERNELS.issubset(set(re.findall(
                r"\b(xr_array_core_bytes_(?:load|store)_(?:u16|u32|u64|f32|f64))\b",
                vm_body)))):
        errors.append(
            "src/vm/xvm_dispatch_collection.inc.c: VM byte-slice scalar I/O bypasses stable owner")

    hosted_header = (root / "src/aot/xrt.h").read_text(encoding="utf-8", errors="strict")
    hosted_runtime = (root / "src/aot/xrt_byte_array.inc.c").read_text(
        encoding="utf-8", errors="strict")
    c90_runtime = (root / "src/aot/xrt_c90.h").read_text(
        encoding="utf-8", errors="strict")
    for relative, text in (("src/aot/xrt.h", hosted_header),
                           ("src/aot/xrt_core_freestanding.h", freestanding_text)):
        if (f"{marker}_HI" not in text or f"{marker}_LO" not in text or
                "XR_BYTE_SLICE_SCALAR_OWNER_APPLY" not in text or
                "xrt_byte_slice_scalar_eval" not in text):
            errors.append(f"{relative}: AOT byte-slice scalar adapter bypasses stable owner")
    if "xrt_byte_slice_scalar_eval" not in hosted_runtime:
        errors.append(
            "src/aot/xrt_byte_array.inc.c: hosted byte-slice scalar runtime bypasses stable owner")
    if ("../shared/xr_byte_slice_scalar_core.h" not in c90_runtime or
            "xrt_byte_slice_scalar_eval" not in c90_runtime or
            "xr_byte_slice_scalar_load_u32_unchecked" not in c90_runtime or
            "xr_byte_slice_scalar_load_u64_unchecked" not in c90_runtime or
            "xr_byte_slice_scalar_store_u32_unchecked" not in c90_runtime or
            "xr_byte_slice_scalar_store_u64_unchecked" not in c90_runtime):
        errors.append(
            "src/aot/xrt_c90.h: restricted C90 byte-slice adapters bypass shared owner")

    retired_semantics = (
        "xrt_freestanding_endian_matches_host",
        "xrt_freestanding_bytes_range_ok",
        "xrt_freestanding_bswap16",
        "xrt_freestanding_bswap32",
        "xrt_freestanding_bswap64",
        "xr_raw_load_u16_unaligned",
        "xr_raw_load_u32_unaligned",
        "xr_raw_load_u64_unaligned",
        "xr_raw_store_u16_unaligned",
        "xr_raw_store_u32_unaligned",
        "xr_raw_store_u64_unaligned",
        "xr_raw_u16_from_le",
        "xr_raw_u32_from_le",
        "xr_raw_u64_from_le",
    )
    for symbol in retired_semantics:
        if symbol in hosted_runtime or symbol in freestanding_text or symbol in c90_runtime:
            errors.append(f"retired private byte-slice scalar path remains: {symbol}")
    for symbol in ("xrt_c90_load_u32", "xrt_c90_load_u64", "xrt_c90_store_u32",
                   "xrt_c90_store_u64", "xrt_c90_host_is_little_endian",
                   "xrt_c90_bswap32", "xrt_c90_bswap64", "xrt_c90_endian_matches_host"):
        if symbol in c90_runtime:
            errors.append(f"retired restricted-C90 byte-slice semantic path remains: {symbol}")

    cgen_text = (root / "src/aot/xi_cgen.c").read_text(encoding="utf-8", errors="strict")
    adapter_body = extract_c_function(cgen_text, "cg_byte_slice_scalar_adapter_name")
    if (adapter_body is None or f"{marker}_HI" not in adapter_body or
            f"{marker}_LO" not in adapter_body or
            "xr_semantic_owner_cgen_adapter" not in adapter_body):
        errors.append(
            "src/aot/xi_cgen.c: CGen byte-slice scalar I/O does not resolve by stable owner ID")

    dispatch_text = (root / "src/aot/xi_cgen_dispatch_helpers.inc.c").read_text(
        encoding="utf-8", errors="strict")
    for emitter in ("xicgen_emit_byte_slice_load", "xicgen_emit_byte_slice_float_load",
                    "xicgen_emit_byte_slice_store", "xicgen_emit_byte_slice_float_store"):
        body = extract_c_function(dispatch_text, emitter)
        if body is None or "cg_byte_slice_scalar_adapter_name" not in body:
            errors.append(
                f"src/aot/xi_cgen_dispatch_helpers.inc.c: {emitter} bypasses owner adapter")
    return errors


def verify_byte_slice_compare_ratchet(root: Path, registry: dict) -> list[str]:
    errors: list[str] = []
    marker = owner_macro_prefix("shared.byte-slice-compare")
    owner = next((row for row in registry.get("owners", [])
                  if row.get("owner") == "shared.byte-slice-compare"), None)
    if owner is None or set(owner.get("operations", [])) != BYTE_SLICE_COMPARE_OPERATIONS:
        errors.append(
            "semantic owner registry has no exact shared.byte-slice-compare operation family")

    core_text = (root / "src/shared/xr_byte_slice_scalar_core.h").read_text(
        encoding="utf-8", errors="strict")
    if (f"{marker}_HI" not in core_text or f"{marker}_LO" not in core_text or
            "XR_BYTE_SLICE_COMPARE_OWNER_APPLY" not in core_text or
            "xr_byte_slice_compare_core" not in core_text):
        errors.append(
            "src/shared/xr_byte_slice_scalar_core.h: byte compare lacks stable owner kernel")

    vm_text = (root / "src/vm/xvm_dispatch_collection.inc.c").read_text(
        encoding="utf-8", errors="strict")
    vm_body = extract_c_function(vm_text, "vmcase(OP_BYTE_SLICE_COMPARE)")
    if vm_body is None:
        start = vm_text.find("vmcase(OP_BYTE_SLICE_COMPARE)")
        end = vm_text.find("vmbreak;", start)
        vm_body = vm_text[start:end] if start >= 0 and end >= 0 else ""
    if (f"{marker}_HI" not in vm_body or f"{marker}_LO" not in vm_body or
            "XR_BYTE_SLICE_COMPARE_OWNER_APPLY" not in vm_body or
            "xr_byte_slice_compare_core" not in vm_body):
        errors.append(
            "src/vm/xvm_dispatch_collection.inc.c: VM byte compare bypasses stable owner")
    if "memcmp(" in vm_body or re.search(r"\b(?:left|right)_length\s*[<>]", vm_body):
        errors.append(
            "src/vm/xvm_dispatch_collection.inc.c: VM revived private byte compare semantics")

    hosted_header = (root / "src/aot/xrt.h").read_text(encoding="utf-8", errors="strict")
    freestanding_text = (root / "src/aot/xrt_core_freestanding.h").read_text(
        encoding="utf-8", errors="strict")
    c90_text = (root / "src/aot/xrt_c90.h").read_text(
        encoding="utf-8", errors="strict")
    hosted_runtime = (root / "src/aot/xrt_byte_array.inc.c").read_text(
        encoding="utf-8", errors="strict")
    for relative, text in (("src/aot/xrt.h", hosted_header),
                           ("src/aot/xrt_core_freestanding.h", freestanding_text)):
        if (f"{marker}_HI" not in text or f"{marker}_LO" not in text or
                "XR_BYTE_SLICE_COMPARE_OWNER_APPLY" not in text or
                "xrt_byte_slice_compare_semantics" not in text):
            errors.append(f"{relative}: AOT byte compare adapter bypasses stable owner")
    hosted_body = extract_c_function(hosted_runtime, "xrt_byte_slice_compare_checked_raw") or ""
    if (f"{marker}_HI" not in hosted_runtime or f"{marker}_LO" not in hosted_runtime or
            "#ifndef xrt_byte_slice_compare_semantics" not in hosted_runtime):
        errors.append(
            "src/aot/xrt_byte_array.inc.c: direct hosted collection include lacks owner binding")
    if ("xrt_byte_slice_compare_semantics" not in hosted_body or "memcmp(" in hosted_body or
            re.search(r"\b(?:left|right)\.length\s*[<>]", hosted_body)):
        errors.append("src/aot/xrt_byte_array.inc.c: hosted byte compare owns semantics")
    freestanding_body = extract_c_function(
        freestanding_text, "xrt_byte_slice_compare_checked_raw") or ""
    if ("xrt_byte_slice_compare_semantics" not in freestanding_body or
            "memcmp(" in freestanding_body or
            re.search(r"\b(?:left|right)\.length\s*[<>]", freestanding_body)):
        errors.append(
            "src/aot/xrt_core_freestanding.h: freestanding byte compare owns semantics")
    c90_body = extract_c_function(c90_text, "xrt_byte_slice_compare_checked_raw") or ""
    if ("xr_byte_slice_compare_core" not in c90_body or "memcmp(" in c90_body or
            re.search(r"\b(?:left|right)\.length\s*[<>]", c90_body)):
        errors.append("src/aot/xrt_c90.h: C90 byte compare owns semantics")

    cgen_text = (root / "src/aot/xi_cgen.c").read_text(encoding="utf-8", errors="strict")
    adapter_body = extract_c_function(cgen_text, "cg_byte_slice_compare_adapter_name")
    if (adapter_body is None or f"{marker}_HI" not in adapter_body or
            f"{marker}_LO" not in adapter_body or
            "xr_semantic_owner_cgen_adapter" not in adapter_body):
        errors.append(
            "src/aot/xi_cgen.c: CGen byte compare does not resolve by stable owner ID")
    dispatch_text = (root / "src/aot/xi_cgen_dispatch_helpers.inc.c").read_text(
        encoding="utf-8", errors="strict")
    emitter_body = extract_c_function(dispatch_text, "xicgen_byte_slice_compare")
    if (emitter_body is None or "cg_byte_slice_compare_adapter_name" not in emitter_body or
            "memcmp(" in emitter_body or "_left.length < _right.length" in emitter_body):
        errors.append(
            "src/aot/xi_cgen_dispatch_helpers.inc.c: CGen byte compare bypasses owner adapter")
    return errors


def verify_byte_slice_fill_ratchet(root: Path, registry: dict) -> list[str]:
    errors: list[str] = []
    marker = owner_macro_prefix("shared.byte-slice-fill")
    owner = next((row for row in registry.get("owners", [])
                  if row.get("owner") == "shared.byte-slice-fill"), None)
    if owner is None or set(owner.get("operations", [])) != BYTE_SLICE_FILL_OPERATIONS:
        errors.append("semantic owner registry has no exact shared.byte-slice-fill family")

    core_text = (root / "src/shared/xr_byte_slice_scalar_core.h").read_text(
        encoding="utf-8", errors="strict")
    if (f"{marker}_HI" not in core_text or f"{marker}_LO" not in core_text or
            "XR_BYTE_SLICE_FILL_OWNER_APPLY" not in core_text or
            "xr_byte_slice_fill_core" not in core_text):
        errors.append(
            "src/shared/xr_byte_slice_scalar_core.h: byte fill lacks stable owner kernel")

    vm_text = (root / "src/vm/xvm_dispatch_collection.inc.c").read_text(
        encoding="utf-8", errors="strict")
    start = vm_text.find("vmcase(OP_BYTE_SLICE_FILL)")
    end = vm_text.find("vmbreak;", start)
    vm_body = vm_text[start:end] if start >= 0 and end >= 0 else ""
    if (f"{marker}_HI" not in vm_body or f"{marker}_LO" not in vm_body or
            "XR_BYTE_SLICE_FILL_OWNER_APPLY" not in vm_body or
            "xr_byte_slice_fill_core" not in vm_body):
        errors.append("src/vm/xvm_dispatch_collection.inc.c: VM byte fill bypasses stable owner")
    if "memset(" in vm_body or "xr_array_core_bytes_fill_value" in vm_text:
        errors.append("src/vm/xvm_dispatch_collection.inc.c: VM revived private byte fill")

    hosted_header = (root / "src/aot/xrt.h").read_text(encoding="utf-8", errors="strict")
    hosted_runtime = (root / "src/aot/xrt_byte_array.inc.c").read_text(
        encoding="utf-8", errors="strict")
    freestanding_text = (root / "src/aot/xrt_core_freestanding.h").read_text(
        encoding="utf-8", errors="strict")
    c90_text = (root / "src/aot/xrt_c90.h").read_text(encoding="utf-8", errors="strict")
    for relative, text in (("src/aot/xrt.h", hosted_header),
                           ("src/aot/xrt_core_freestanding.h", freestanding_text)):
        if (f"{marker}_HI" not in text or f"{marker}_LO" not in text or
                "XR_BYTE_SLICE_FILL_OWNER_APPLY" not in text or
                "xrt_byte_slice_fill_semantics" not in text):
            errors.append(f"{relative}: AOT byte fill adapter bypasses stable owner")
    hosted_body = extract_c_function(hosted_runtime, "xrt_byte_slice_fill_checked_raw") or ""
    freestanding_body = extract_c_function(
        freestanding_text, "xrt_byte_slice_fill_checked_raw") or ""
    c90_body = extract_c_function(c90_text, "xrt_byte_slice_fill_checked_raw") or ""
    for relative, body, required in (
            ("src/aot/xrt_byte_array.inc.c", hosted_body, "xrt_byte_slice_fill_semantics"),
            ("src/aot/xrt_core_freestanding.h", freestanding_body,
             "xrt_byte_slice_fill_semantics"),
            ("src/aot/xrt_c90.h", c90_body, "xr_byte_slice_fill_core")):
        if required not in body or "memset(" in body or re.search(r"\.length\s*[<>]", body):
            errors.append(f"{relative}: byte fill adapter owns semantics")

    cgen_text = (root / "src/aot/xi_cgen.c").read_text(encoding="utf-8", errors="strict")
    adapter_body = extract_c_function(cgen_text, "cg_byte_slice_fill_adapter_name")
    if (adapter_body is None or f"{marker}_HI" not in adapter_body or
            f"{marker}_LO" not in adapter_body or
            "xr_semantic_owner_cgen_adapter" not in adapter_body):
        errors.append("src/aot/xi_cgen.c: CGen byte fill does not resolve by stable owner ID")
    dispatch_text = (root / "src/aot/xi_cgen_dispatch_helpers.inc.c").read_text(
        encoding="utf-8", errors="strict")
    emitter_body = extract_c_function(dispatch_text, "xicgen_byte_slice_fill")
    if (emitter_body is None or "cg_byte_slice_fill_adapter_name" not in emitter_body or
            "memset(" in emitter_body or "((uint8_t*)" in emitter_body or "({" in emitter_body):
        errors.append("src/aot/xi_cgen_dispatch_helpers.inc.c: CGen byte fill bypasses owner")
    return errors


def verify_byte_slice_mutation_ratchet(root: Path, registry: dict) -> list[str]:
    errors: list[str] = []
    families = (
        ("shared.byte-slice-copy", BYTE_SLICE_COPY_OPERATIONS, "COPY",
         "xr_byte_slice_copy_core", "xrt_byte_slice_copy_semantics",
         "xrt_byte_slice_copy_checked_raw", "cg_byte_slice_copy_adapter_name"),
        ("shared.byte-slice-repeat", BYTE_SLICE_REPEAT_OPERATIONS, "REPEAT",
         "xr_byte_slice_repeat_core", "xrt_byte_slice_repeat_semantics",
         "xrt_byte_slice_repeat_from_checked_raw", "cg_byte_slice_repeat_adapter_name"),
    )
    core_text = (root / "src/shared/xr_byte_slice_scalar_core.h").read_text(
        encoding="utf-8", errors="strict")
    vm_text = (root / "src/vm/xvm_dispatch_collection.inc.c").read_text(
        encoding="utf-8", errors="strict")
    hosted_header = (root / "src/aot/xrt.h").read_text(encoding="utf-8", errors="strict")
    hosted_runtime = (root / "src/aot/xrt_byte_array.inc.c").read_text(
        encoding="utf-8", errors="strict")
    freestanding_text = (root / "src/aot/xrt_core_freestanding.h").read_text(
        encoding="utf-8", errors="strict")
    c90_text = (root / "src/aot/xrt_c90.h").read_text(encoding="utf-8", errors="strict")
    cgen_text = (root / "src/aot/xi_cgen.c").read_text(encoding="utf-8", errors="strict")
    dispatch_text = (root / "src/aot/xi_cgen_dispatch_helpers.inc.c").read_text(
        encoding="utf-8", errors="strict")
    forbidden_cgen = ("memcpy(_dst.data", "memmove(_dst.data", "((uint8_t*)_dst.data)",
                      "xr_array_core_copy_or_move_bytes(_dst.data",
                      "xr_array_core_bytes_repeat_copy(_span.data", "({ xr_span_t")

    for owner_name, operations, tag, kernel, semantics, adapter, resolver in families:
        marker = owner_macro_prefix(owner_name)
        owner = next((row for row in registry.get("owners", [])
                      if row.get("owner") == owner_name), None)
        if owner is None or set(owner.get("operations", [])) != operations:
            errors.append(f"semantic owner registry has no exact {owner_name} family")
        if (f"{marker}_HI" not in core_text or f"{marker}_LO" not in core_text or
                f"XR_BYTE_SLICE_{tag}_OWNER_APPLY" not in core_text or kernel not in core_text):
            errors.append(f"src/shared/xr_byte_slice_scalar_core.h: {owner_name} lacks kernel")

        start = vm_text.find(f"vmcase(OP_BYTE_SLICE_{tag})")
        end = vm_text.find("vmbreak;", start)
        vm_body = vm_text[start:end] if start >= 0 and end >= 0 else ""
        if (f"{marker}_HI" not in vm_body or f"{marker}_LO" not in vm_body or
                f"XR_BYTE_SLICE_{tag}_OWNER_APPLY" not in vm_body or kernel not in vm_body):
            errors.append(f"src/vm/xvm_dispatch_collection.inc.c: {owner_name} bypasses owner")

        for relative, text in (("src/aot/xrt.h", hosted_header),
                               ("src/aot/xrt_core_freestanding.h", freestanding_text)):
            if (f"{marker}_HI" not in text or f"{marker}_LO" not in text or
                    f"XR_BYTE_SLICE_{tag}_OWNER_APPLY" not in text or semantics not in text):
                errors.append(f"{relative}: {owner_name} adapter bypasses owner")
        for relative, text, required in (
                ("src/aot/xrt_byte_array.inc.c", hosted_runtime, semantics),
                ("src/aot/xrt_core_freestanding.h", freestanding_text, semantics),
                ("src/aot/xrt_c90.h", c90_text, kernel)):
            body = extract_c_function(text, adapter) or ""
            if required not in body or re.search(r"\.length\s*[<>]", body):
                errors.append(f"{relative}: {owner_name} adapter owns semantics")

        resolver_body = extract_c_function(cgen_text, resolver)
        if (resolver_body is None or f"{marker}_HI" not in resolver_body or
                f"{marker}_LO" not in resolver_body or
                "xr_semantic_owner_cgen_adapter" not in resolver_body):
            errors.append(f"src/aot/xi_cgen.c: {owner_name} does not resolve stable adapter")
        emitter = extract_c_function(dispatch_text, f"xicgen_byte_slice_{tag.lower()}") or ""
        if resolver not in emitter or any(token in emitter for token in forbidden_cgen):
            errors.append(f"src/aot/xi_cgen_dispatch_helpers.inc.c: {owner_name} revived semantics")
    return errors


def verify_byte_slice_common_prefix_ratchet(root: Path, registry: dict) -> list[str]:
    errors: list[str] = []
    marker = owner_macro_prefix("shared.byte-slice-common-prefix")
    owner = next((row for row in registry.get("owners", [])
                  if row.get("owner") == "shared.byte-slice-common-prefix"), None)
    if owner is None or set(owner.get("operations", [])) != BYTE_SLICE_COMMON_PREFIX_OPERATIONS:
        errors.append(
            "semantic owner registry has no exact shared.byte-slice-common-prefix family")

    core_text = (root / "src/shared/xr_byte_slice_scalar_core.h").read_text(
        encoding="utf-8", errors="strict")
    if (f"{marker}_HI" not in core_text or f"{marker}_LO" not in core_text or
            "XR_BYTE_SLICE_COMMON_PREFIX_OWNER_APPLY" not in core_text or
            "xr_byte_slice_common_prefix_core" not in core_text):
        errors.append(
            "src/shared/xr_byte_slice_scalar_core.h: common-prefix lacks stable owner kernel")

    retired = (
        "xr_array_core_bytes_common_prefix_raw",
        "xr_array_core_bytes_common_prefix",
        "xr_array_core_common_prefix_diff_byte64",
    )
    for relative in ("src/shared/xr_array_core.h", "src/aot/xrt_core_freestanding.h"):
        text = (root / relative).read_text(encoding="utf-8", errors="strict")
        for symbol in retired:
            if symbol in text:
                errors.append(f"{relative}: retired common-prefix semantic source remains: {symbol}")

    vm_text = (root / "src/vm/xvm_dispatch_collection.inc.c").read_text(
        encoding="utf-8", errors="strict")
    start = vm_text.find("vmcase(OP_BYTE_SLICE_COMMON_PREFIX)")
    end = vm_text.find("vmbreak;", start)
    vm_body = vm_text[start:end] if start >= 0 and end >= 0 else ""
    if (f"{marker}_HI" not in vm_body or f"{marker}_LO" not in vm_body or
            "XR_BYTE_SLICE_COMMON_PREFIX_OWNER_APPLY" not in vm_body or
            "xr_byte_slice_common_prefix_core" not in vm_body):
        errors.append(
            "src/vm/xvm_dispatch_collection.inc.c: VM common-prefix bypasses stable owner")
    if any(symbol in vm_body for symbol in retired) or "while (" in vm_body:
        errors.append(
            "src/vm/xvm_dispatch_collection.inc.c: VM revived private common-prefix semantics")

    hosted_header = (root / "src/aot/xrt_coll.h").read_text(encoding="utf-8", errors="strict")
    freestanding_text = (root / "src/aot/xrt_core_freestanding.h").read_text(
        encoding="utf-8", errors="strict")
    hosted_runtime = (root / "src/aot/xrt_byte_array.inc.c").read_text(
        encoding="utf-8", errors="strict")
    c90_text = (root / "src/aot/xrt_c90.h").read_text(encoding="utf-8", errors="strict")
    for relative, text in (("src/aot/xrt_coll.h", hosted_header),
                           ("src/aot/xrt_core_freestanding.h", freestanding_text)):
        if (f"{marker}_HI" not in text or f"{marker}_LO" not in text or
                "XR_BYTE_SLICE_COMMON_PREFIX_OWNER_APPLY" not in text or
                "xrt_byte_slice_common_prefix_semantics" not in text):
            errors.append(f"{relative}: AOT common-prefix adapter bypasses stable owner")

    hosted_body = extract_c_function(
        hosted_runtime, "xrt_byte_slice_common_prefix_checked_raw") or ""
    if "#ifndef xrt_byte_slice_common_prefix_semantics" in hosted_runtime or \
            "#define xrt_byte_slice_common_prefix_semantics" in hosted_runtime:
        errors.append(
            "src/aot/xrt_byte_array.inc.c: common-prefix owner fallback or alias revived")
    if "xrt_byte_slice_common_prefix_semantics" not in hosted_body or any(
            symbol in hosted_body for symbol in retired):
        errors.append("src/aot/xrt_byte_array.inc.c: hosted common-prefix owns semantics")
    freestanding_body = extract_c_function(
        freestanding_text, "xrt_byte_slice_common_prefix_checked_raw") or ""
    if "xrt_byte_slice_common_prefix_semantics" not in freestanding_body or any(
            symbol in freestanding_body for symbol in retired):
        errors.append(
            "src/aot/xrt_core_freestanding.h: freestanding common-prefix owns semantics")
    c90_body = extract_c_function(c90_text, "xrt_byte_slice_common_prefix_checked_raw") or ""
    if "xr_byte_slice_common_prefix_core" not in c90_body or any(
            symbol in c90_body for symbol in retired):
        errors.append("src/aot/xrt_c90.h: C90 common-prefix owns semantics")

    cgen_text = (root / "src/aot/xi_cgen.c").read_text(encoding="utf-8", errors="strict")
    adapter_body = extract_c_function(cgen_text, "cg_byte_slice_common_prefix_adapter_name")
    if (adapter_body is None or f"{marker}_HI" not in adapter_body or
            f"{marker}_LO" not in adapter_body or
            "xr_semantic_owner_cgen_adapter" not in adapter_body):
        errors.append(
            "src/aot/xi_cgen.c: CGen common-prefix does not resolve by stable owner ID")
    dispatch_text = (root / "src/aot/xi_cgen_dispatch_helpers.inc.c").read_text(
        encoding="utf-8", errors="strict")
    emitter_body = extract_c_function(dispatch_text, "xicgen_byte_slice_common_prefix") or ""
    if ("cg_byte_slice_common_prefix_adapter_name" not in emitter_body or
            "xrt_byte_slice_common_prefix_checked_raw" in emitter_body or
            "xr_byte_slice_common_prefix_core" in emitter_body or
            any(symbol in emitter_body for symbol in retired) or
            "_left_len" in emitter_body or "_right_len" in emitter_body):
        errors.append(
            "src/aot/xi_cgen_dispatch_helpers.inc.c: CGen common-prefix bypasses owner adapter")
    return errors


def verify_raw_memory_copy_ratchet(root: Path, registry: dict) -> list[str]:
    errors: list[str] = []
    marker = owner_macro_prefix("shared.raw-memory-copy")
    owner = next((row for row in registry.get("owners", [])
                  if row.get("owner") == "shared.raw-memory-copy"), None)
    if owner is None or set(owner.get("operations", [])) != RAW_MEMORY_COPY_OPERATIONS:
        errors.append("semantic owner registry has no exact shared.raw-memory-copy family")

    core_text = (root / "src/shared/xr_raw_memory_core.h").read_text(
        encoding="utf-8", errors="strict")
    if (f"{marker}_HI" not in core_text or f"{marker}_LO" not in core_text or
            "XR_RAW_MEMORY_COPY_OWNER_APPLY" not in core_text or
            "xr_raw_memory_copy_nonoverlap" not in core_text or
            "if (count <= 0)" not in core_text):
        errors.append("src/shared/xr_raw_memory_core.h: raw copy lacks stable owner kernel")

    retired = "xr_array_core_copy_nonoverlap_bytes"
    for relative in ("src/shared/xr_array_core.h", "src/aot/xrt_core_freestanding.h",
                     "src/aot/xi_cgen_array_helpers.inc.c",
                     "src/vm/xvm_dispatch_collection.inc.c",
                     "src/aot/xi_cgen_dispatch_helpers.inc.c"):
        text = (root / relative).read_text(encoding="utf-8", errors="strict")
        if retired in text:
            errors.append(f"{relative}: retired raw-memory copy semantic source remains")

    vm_text = (root / "src/vm/xvm_dispatch_collection.inc.c").read_text(
        encoding="utf-8", errors="strict")
    start = vm_text.find("vmcase(OP_PTR_COPY_NONOVERLAP)")
    end = vm_text.find("vmbreak;", start)
    vm_body = vm_text[start:end] if start >= 0 and end >= 0 else ""
    if (f"{marker}_HI" not in vm_body or f"{marker}_LO" not in vm_body or
            "XR_RAW_MEMORY_COPY_OWNER_APPLY" not in vm_body or "memcpy(" in vm_body):
        errors.append("src/vm/xvm_dispatch_collection.inc.c: VM raw copy bypasses stable owner")

    hosted_text = (root / "src/aot/xrt.h").read_text(encoding="utf-8", errors="strict")
    freestanding_text = (root / "src/aot/xrt_core_freestanding.h").read_text(
        encoding="utf-8", errors="strict")
    for relative, text in (("src/aot/xrt.h", hosted_text),
                           ("src/aot/xrt_core_freestanding.h", freestanding_text)):
        if (f"{marker}_HI" not in text or f"{marker}_LO" not in text or
                "XR_RAW_MEMORY_COPY_OWNER_APPLY" not in text or
                "xrt_raw_memory_copy_nonoverlap" not in text):
            errors.append(f"{relative}: AOT raw-memory copy adapter bypasses stable owner")
    if ("#ifndef xrt_raw_memory_copy_nonoverlap" in hosted_text or
            "#ifndef xrt_raw_memory_copy_nonoverlap" in freestanding_text):
        errors.append("AOT raw-memory copy fallback or alias revived")

    c90_text = (root / "src/aot/xrt_c90.h").read_text(encoding="utf-8", errors="strict")
    c90_body = extract_c_function(c90_text, "xrt_raw_memory_copy_nonoverlap") or ""
    if "xr_raw_memory_copy_nonoverlap" not in c90_body or "memcpy(" in c90_body:
        errors.append("src/aot/xrt_c90.h: C90 raw-memory copy owns semantics")

    cgen_text = (root / "src/aot/xi_cgen.c").read_text(encoding="utf-8", errors="strict")
    adapter_body = extract_c_function(cgen_text, "cg_raw_memory_copy_adapter_name")
    if (adapter_body is None or f"{marker}_HI" not in adapter_body or
            f"{marker}_LO" not in adapter_body or
            "xr_semantic_owner_cgen_adapter" not in adapter_body):
        errors.append("src/aot/xi_cgen.c: CGen raw copy does not resolve by stable owner ID")
    dispatch_text = (root / "src/aot/xi_cgen_dispatch_helpers.inc.c").read_text(
        encoding="utf-8", errors="strict")
    emitter_body = extract_c_function(dispatch_text, "xicgen_ptr_copy_nonoverlap") or ""
    if ("cg_raw_memory_copy_adapter_name" not in emitter_body or
            "xrt_raw_memory_copy_nonoverlap" in emitter_body or
            "xr_raw_memory_copy_nonoverlap" in emitter_body or "memcpy(" in emitter_body or
            "XR_ASSUME" in emitter_body or "size_t" in emitter_body):
        errors.append("src/aot/xi_cgen_dispatch_helpers.inc.c: CGen raw copy owns semantics")
    return errors


def verify_raw_scalar_access_ratchet(root: Path, registry: dict) -> list[str]:
    errors: list[str] = []
    marker = owner_macro_prefix("shared.raw-scalar-access")
    owner = next((row for row in registry.get("owners", [])
                  if row.get("owner") == "shared.raw-scalar-access"), None)
    if owner is None or set(owner.get("operations", [])) != RAW_SCALAR_ACCESS_OPERATIONS:
        errors.append("semantic owner registry has no exact shared.raw-scalar-access family")

    core_text = (root / "src/shared/xr_raw_scalar_core.h").read_text(
        encoding="utf-8", errors="strict")
    required_core = (
        f"{marker}_HI", f"{marker}_LO", "XR_RAW_SCALAR_ACCESS_OWNER_APPLY",
        "xr_raw_scalar_load", "xr_raw_scalar_store", "xr_raw_scalar_width",
        "xr_raw_scalar_sign_extend",
    )
    if any(token not in core_text for token in required_core):
        errors.append("src/shared/xr_raw_scalar_core.h: raw scalar access lacks stable owner kernel")

    vm_text = (root / "src/vm/xvm_ffi.c").read_text(encoding="utf-8", errors="strict")
    for symbol, delegate in (("xr_ffi_ptr_load", "xr_raw_scalar_load"),
                             ("xr_ffi_ptr_store", "xr_raw_scalar_store")):
        body = extract_c_function(vm_text, symbol) or ""
        if (f"{marker}_HI" not in body or f"{marker}_LO" not in body or
                "XR_RAW_SCALAR_ACCESS_OWNER_APPLY" not in body or delegate not in body):
            errors.append(f"src/vm/xvm_ffi.c: {symbol} bypasses stable raw scalar owner")
        if any(token in body for token in
               ("xr_raw_load_u", "xr_raw_store_u", "xr_raw_u16_from_endian",
                "xr_raw_u32_from_endian", "xr_raw_u64_from_endian",
                "xr_raw_f32_from_bits", "xr_raw_f64_from_bits")):
            errors.append(f"src/vm/xvm_ffi.c: {symbol} revived private raw scalar semantics")
    for retired in ("ffi_load_integer_bits", "ffi_store_integer_bits", "ffi_sign_extend_integer"):
        if retired in vm_text:
            errors.append(f"src/vm/xvm_ffi.c: retired raw scalar source remains: {retired}")

    for relative, consumer in (("src/aot/xrt.h", "XR_SEM_CONSUMER_AOT_HOSTED"),
                               ("src/aot/xrt_core_freestanding.h",
                                "XR_SEM_CONSUMER_AOT_FREESTANDING")):
        text = (root / relative).read_text(encoding="utf-8", errors="strict")
        for token in (f"{marker}_HI", f"{marker}_LO", consumer,
                      "XR_RAW_SCALAR_ACCESS_OWNER_APPLY",
                      "xrt_raw_scalar_access_load_i64", "xrt_raw_scalar_access_store_i64"):
            if token not in text:
                errors.append(f"{relative}: AOT raw scalar adapter bypasses stable owner")
                break

    # The restricted C90 surface has no Xi raw-pointer lowering; generated C90
    # therefore cannot consume xi.ptr.load/store and needs no compatibility path.

    cgen_text = (root / "src/aot/xi_cgen.c").read_text(encoding="utf-8", errors="strict")
    resolver = extract_c_function(cgen_text, "cg_raw_scalar_access_adapter_name") or ""
    if (f"{marker}_HI" not in resolver or f"{marker}_LO" not in resolver or
            "xr_semantic_owner_cgen_adapter" not in resolver):
        errors.append("src/aot/xi_cgen.c: raw scalar access does not resolve stable adapter")
    dispatch_text = (root / "src/aot/xi_cgen_dispatch_helpers.inc.c").read_text(
        encoding="utf-8", errors="strict")
    cgen_required = {
        "xicgen_ptr_load": (
            'fprintf(out, "%s(%s, ", owner_adapter, value_plan->rep.c_type)',
            'fprintf(out, "%s_load_%s(", owner_adapter,',
        ),
        "xicgen_ptr_store": (
            'fprintf(out, "%s_store(%s, ", owner_adapter, value_plan->rep.c_type)',
            'fprintf(out, "%s_store_%s(", owner_adapter,',
        ),
    }
    cgen_forbidden = {
        "xicgen_ptr_load": ('fprintf(out, "%s_store',),
        "xicgen_ptr_store": ('fprintf(out, "%s(%s, ", owner_adapter,',
                              'fprintf(out, "%s_load_'),
    }
    for symbol in ("xicgen_ptr_load", "xicgen_ptr_store"):
        body = extract_c_function(dispatch_text, symbol) or ""
        if "cg_raw_scalar_access_adapter_name" not in body:
            errors.append(f"src/aot/xi_cgen_dispatch_helpers.inc.c: {symbol} bypasses owner adapter")
        if any(token not in body for token in cgen_required[symbol]) or any(
                token in body for token in cgen_forbidden[symbol]):
            errors.append(
                f"src/aot/xi_cgen_dispatch_helpers.inc.c: {symbol} emits a wrong owner adapter")
        if any(token in body for token in
               ("xr_raw_load_u", "xr_raw_store_u", "xr_raw_load_ptr_unaligned",
                "xr_raw_store_ptr_unaligned", "xr_raw_u16_from_", "xr_raw_u32_from_",
                "xr_raw_u64_from_", "xr_raw_f32_", "xr_raw_f64_")):
            errors.append(f"src/aot/xi_cgen_dispatch_helpers.inc.c: {symbol} owns raw semantics")
    return errors


def verify_enum_metadata_access_ratchet(root: Path, registry: dict) -> list[str]:
    errors: list[str] = []
    owner_name = "shared.enum-metadata-access"
    marker = owner_macro_prefix(owner_name)
    owner = next((row for row in registry.get("owners", [])
                  if row.get("owner") == owner_name), None)
    if owner is None or set(owner.get("operations", [])) != ENUM_METADATA_ACCESS_OPERATIONS:
        errors.append("semantic owner registry has no exact shared.enum-metadata-access family")

    core_text = (root / "src/shared/xr_enum_metadata_core.h").read_text(
        encoding="utf-8", errors="strict")
    for token in (f"{marker}_HI", f"{marker}_LO", "XR_ENUM_METADATA_ACCESS_OWNER_APPLY",
                  "xr_enum_metadata_variant_at_core", "xr_enum_metadata_payload_at_core"):
        if token not in core_text:
            errors.append("src/shared/xr_enum_metadata_core.h: enum metadata lacks stable owner")
            break

    vm_text = (root / "src/vm/xvm_dispatch_enum.inc.c").read_text(
        encoding="utf-8", errors="strict")
    for opcode, kernel in (("OP_ENUM_VARIANT_AT", "xr_enum_metadata_variant_at_core"),
                           ("OP_ENUM_PAYLOAD_AT", "xr_enum_metadata_payload_at_core")):
        start = vm_text.find(f"vmcase({opcode})")
        end = vm_text.find("vmbreak;", start)
        body = vm_text[start:end] if start >= 0 and end >= 0 else ""
        if (f"{marker}_HI" not in body or f"{marker}_LO" not in body or
                "XR_ENUM_METADATA_ACCESS_OWNER_APPLY" not in body or kernel not in body or
                "index < 0" in body or ">= count" in body or "<< 32" in body):
            errors.append(f"src/vm/xvm_dispatch_enum.inc.c: {opcode} bypasses enum metadata owner")

    for relative, consumer in (("src/aot/xrt_coll.h", "XR_SEM_CONSUMER_AOT_HOSTED"),
                               ("src/aot/xrt_core_freestanding.h",
                                "XR_SEM_CONSUMER_AOT_FREESTANDING")):
        text = (root / relative).read_text(encoding="utf-8", errors="strict")
        for function, kernel in (
                ("xrt_enum_metadata_access_variant_at", "xr_enum_metadata_variant_at_core"),
                ("xrt_enum_metadata_access_payload_at", "xr_enum_metadata_payload_at_core")):
            body = extract_c_function(text, function) or ""
            if (f"{marker}_HI" not in body or f"{marker}_LO" not in body or consumer not in body or
                    "XR_ENUM_METADATA_ACCESS_OWNER_APPLY" not in body or kernel not in body):
                errors.append(f"{relative}: {function} bypasses enum metadata owner")
            if "index < 0" in body or ">= count" in body or "<< 32" in body:
                errors.append(f"{relative}: {function} owns enum metadata semantics")
        if "#ifndef xrt_enum_metadata_access" in text:
            errors.append(f"{relative}: enum metadata fallback or alias revived")

    cgen_text = (root / "src/aot/xi_cgen.c").read_text(encoding="utf-8", errors="strict")
    resolver = extract_c_function(cgen_text, "cg_enum_metadata_access_adapter_name") or ""
    if (f"{marker}_HI" not in resolver or f"{marker}_LO" not in resolver or
            "xr_semantic_owner_cgen_adapter" not in resolver):
        errors.append("src/aot/xi_cgen.c: enum metadata does not resolve stable adapter")
    dispatch = (root / "src/aot/xi_cgen_dispatch_helpers.inc.c").read_text(
        encoding="utf-8", errors="strict")
    for emitter, suffix in (("xicgen_enum_variant_at", "_variant_at("),
                            ("xicgen_enum_payload_at", "_payload_at(")):
        body = extract_c_function(dispatch, emitter) or ""
        if ("cg_enum_metadata_access_adapter_name" not in body or suffix not in body or
                "XI_CGEN_C_DIALECT_C90" not in body):
            errors.append(f"src/aot/xi_cgen_dispatch_helpers.inc.c: {emitter} bypasses owner adapter")
        if ("index out of bounds" in body or "_i < 0" in body or "_i >=" in body or
                "<< 32" in body or "xrt_throw_error" in body):
            errors.append(f"src/aot/xi_cgen_dispatch_helpers.inc.c: {emitter} owns semantics")
    return errors


def verify_cell_access_ratchet(root: Path, registry: dict) -> list[str]:
    errors: list[str] = []
    owner_name = "shared.cell-access"
    marker = owner_macro_prefix(owner_name)
    owner = next((row for row in registry.get("owners", [])
                  if row.get("owner") == owner_name), None)
    if owner is None or set(owner.get("operations", [])) != CELL_ACCESS_OPERATIONS:
        errors.append("semantic owner registry has no exact shared.cell-access family")

    core_text = (root / "src/shared/xr_cell_access_core.h").read_text(
        encoding="utf-8", errors="strict")
    for token in (f"{marker}_HI", f"{marker}_LO", "XR_CELL_ACCESS_OWNER_APPLY",
                  "xr_cell_access_load_core", "xr_cell_access_replace_core"):
        if token not in core_text:
            errors.append("src/shared/xr_cell_access_core.h: cell access lacks stable owner")
            break

    vm_text = (root / "src/vm/xvm_dispatch_closure.inc.c").read_text(
        encoding="utf-8", errors="strict")
    for opcode, kernel in (("OP_CELL_GET", "xr_cell_access_load_core"),
                           ("OP_CELL_SET", "xr_cell_access_replace_core")):
        start = vm_text.find(f"vmcase({opcode})")
        end = vm_text.find("vmbreak;", start)
        body = vm_text[start:end] if start >= 0 and end >= 0 else ""
        if (f"{marker}_HI" not in body or f"{marker}_LO" not in body or
                "XR_SEM_CONSUMER_VM" not in body or
                "XR_CELL_ACCESS_OWNER_APPLY" not in body or kernel not in body):
            errors.append(f"src/vm/xvm_dispatch_closure.inc.c: {opcode} bypasses cell owner")
        if opcode == "OP_CELL_SET" and "xr_rc_release_value" not in body:
            errors.append("src/vm/xvm_dispatch_closure.inc.c: OP_CELL_SET lost old-value release")
        if re.search(r"cell->value\s*=", body):
            errors.append(f"src/vm/xvm_dispatch_closure.inc.c: {opcode} owns slot semantics")

    hosted_text = (root / "src/aot/xrt_coll.h").read_text(encoding="utf-8", errors="strict")
    for function, kernel in (("xrt_cell_access_get", "xr_cell_access_load_core"),
                             ("xrt_cell_access_set", "xr_cell_access_replace_core")):
        body = extract_c_function(hosted_text, function) or ""
        if (f"{marker}_HI" not in body or f"{marker}_LO" not in body or
                "XR_SEM_CONSUMER_AOT_HOSTED" not in body or
                "XR_CELL_ACCESS_OWNER_APPLY" not in body or kernel not in body):
            errors.append(f"src/aot/xrt_coll.h: {function} bypasses cell owner")
        if function.endswith("_set") and "xrt_release(old)" not in body:
            errors.append("src/aot/xrt_coll.h: cell set lost old-value release")
        if re.search(r"cell->value\s*=", body):
            errors.append(f"src/aot/xrt_coll.h: {function} owns slot semantics")
    if re.search(r"\bxrt_cell_(?:get|set)\s*\(", hosted_text):
        errors.append("src/aot/xrt_coll.h: retired cell access alias revived")

    cgen_text = (root / "src/aot/xi_cgen.c").read_text(encoding="utf-8", errors="strict")
    resolver = extract_c_function(cgen_text, "cg_cell_access_adapter_name") or ""
    if (f"{marker}_HI" not in resolver or f"{marker}_LO" not in resolver or
            "xr_semantic_owner_cgen_adapter" not in resolver):
        errors.append("src/aot/xi_cgen.c: cell access does not resolve stable adapter")
    dispatch = (root / "src/aot/xi_cgen_dispatch_helpers.inc.c").read_text(
        encoding="utf-8", errors="strict")
    for emitter, suffix in (("xicgen_cell_get", "_get("), ("xicgen_cell_set", "_set(")):
        body = extract_c_function(dispatch, emitter) or ""
        if ("cg_cell_access_adapter_name" not in body or suffix not in body or
                "freestanding_profile" not in body or "XI_CGEN_C_DIALECT_C90" not in body):
            errors.append(f"src/aot/xi_cgen_dispatch_helpers.inc.c: {emitter} bypasses owner")
        if "->value" in body or re.search(r"\bxrt_cell_(?:get|set)\s*\(", body):
            errors.append(f"src/aot/xi_cgen_dispatch_helpers.inc.c: {emitter} owns cell semantics")

    freestanding_text = (root / "src/aot/xrt_core_freestanding.h").read_text(
        encoding="utf-8", errors="strict")
    for function, kernel in (("xrt_cell_access_get", "xr_cell_access_load_core"),
                             ("xrt_cell_access_set", "xr_cell_access_replace_core")):
        body = extract_c_function(freestanding_text, function) or ""
        if (f"{marker}_HI" not in body or f"{marker}_LO" not in body or
                "XR_SEM_CONSUMER_AOT_FREESTANDING" not in body or
                "XR_CELL_ACCESS_OWNER_APPLY" not in body or kernel not in body):
            errors.append(f"src/aot/xrt_core_freestanding.h: {function} bypasses cell owner")
        if function.endswith("_set") and "xrt_release(old)" not in body:
            errors.append("src/aot/xrt_core_freestanding.h: cell set lost old-value release")
    c90_text = (root / "src/aot/xrt_c90.h").read_text(encoding="utf-8", errors="strict")
    if "xrt_cell_access_" in c90_text:
        errors.append("src/aot/xrt_c90.h: unsupported cell adapter surface revived")
    return errors


def verify_null_test_ratchet(root: Path, registry: dict) -> list[str]:
    errors: list[str] = []
    owner_name = "shared.null-test"
    marker = owner_macro_prefix(owner_name)
    owner = next((row for row in registry.get("owners", [])
                  if row.get("owner") == owner_name), None)
    if owner is None or set(owner.get("operations", [])) != NULL_TEST_OPERATIONS:
        errors.append("semantic owner registry has no exact shared.null-test family")

    core_text = (root / "src/shared/xr_null_test_core.h").read_text(
        encoding="utf-8", errors="strict")
    for token in (f"{marker}_HI", f"{marker}_LO", "XR_NULL_TEST_OWNER_APPLY",
                  "xr_null_test_tagged_core", "xr_null_test_pointer_is_null_core"):
        if token not in core_text:
            errors.append("src/shared/xr_null_test_core.h: null test lacks stable owner")
            break

    vm_text = (root / "src/vm/xvm_dispatch_compare.inc.c").read_text(
        encoding="utf-8", errors="strict")
    for opcode in ("OP_ISNULL", "OP_ISNULL_SET"):
        start = vm_text.find(f"vmcase({opcode})")
        end = vm_text.find("vmbreak;", start)
        body = vm_text[start:end] if start >= 0 and end >= 0 else ""
        if (f"{marker}_HI" not in body or f"{marker}_LO" not in body or
                "XR_SEM_CONSUMER_VM" not in body or
                "XR_NULL_TEST_OWNER_APPLY" not in body or
                "xr_null_test_tagged_core" not in body):
            errors.append(f"src/vm/xvm_dispatch_compare.inc.c: {opcode} bypasses null-test owner")
        if "XR_IS_NULL" in body or ".tag == XR_TAG_NULL" in body:
            errors.append(f"src/vm/xvm_dispatch_compare.inc.c: {opcode} owns null semantics")

    for relative, consumer in (("src/aot/xrt.h", "XR_SEM_CONSUMER_AOT_HOSTED"),
                               ("src/aot/xrt_core_freestanding.h",
                                "XR_SEM_CONSUMER_AOT_FREESTANDING")):
        text = (root / relative).read_text(encoding="utf-8", errors="strict")
        for adapter, kernel in (("xrt_null_test_tagged", "xr_null_test_tagged_core"),
                                ("xrt_null_test_pointer", "xr_null_test_pointer_is_null_core")):
            if (adapter not in text or kernel not in text or f"{marker}_HI" not in text or
                    f"{marker}_LO" not in text or consumer not in text or
                    "XR_NULL_TEST_OWNER_APPLY" not in text):
                errors.append(f"{relative}: {adapter} bypasses null-test owner")

    c90_text = (root / "src/aot/xrt_c90.h").read_text(encoding="utf-8", errors="strict")
    for token in ("XR_NULL_TEST_C90", "#undef XR_NULL_TEST_C90",
                  "xrt_null_test_tagged", "xrt_null_test_pointer",
                  "xr_null_test_tagged_core", "xr_null_test_pointer_is_null_core"):
        if token not in c90_text:
            errors.append("src/aot/xrt_c90.h: null-test mechanical adapter is incomplete")
            break

    cgen_text = (root / "src/aot/xi_cgen.c").read_text(encoding="utf-8", errors="strict")
    resolver = extract_c_function(cgen_text, "cg_null_test_adapter_name") or ""
    if (f"{marker}_HI" not in resolver or f"{marker}_LO" not in resolver or
            "xr_semantic_owner_cgen_adapter" not in resolver):
        errors.append("src/aot/xi_cgen.c: null test does not resolve stable adapter")
    dispatch = (root / "src/aot/xi_cgen_dispatch_helpers.inc.c").read_text(
        encoding="utf-8", errors="strict")
    body = extract_c_function(dispatch, "xicgen_isnull") or ""
    if ("cg_null_test_adapter_name" not in body or "_tagged(" not in body or
            "_pointer(" not in body):
        errors.append("src/aot/xi_cgen_dispatch_helpers.inc.c: xicgen_isnull bypasses owner")
    if ".tag == XR_TAG_NULL" in body or " == NULL" in body:
        errors.append("src/aot/xi_cgen_dispatch_helpers.inc.c: xicgen_isnull owns null semantics")

    for relative in ("src/ir/xi_opt.c", "src/ir/xi_opt_sccp.c"):
        text = (root / relative).read_text(encoding="utf-8", errors="strict")
        if "xr_null_test_tagged_core" not in text:
            errors.append(f"{relative}: constant null fold bypasses null-test core")
    if "aot/test_xrt_null_test_owner_c90.c" not in (
            root / "tests/unit/CMakeLists.txt").read_text(encoding="utf-8", errors="strict"):
        errors.append("tests/unit/CMakeLists.txt: C90 null-test KAT is not registered")
    return errors


def verify_data_pointer_ratchet(root: Path, registry: dict) -> list[str]:
    errors: list[str] = []
    owner_name = "shared.data-pointer"
    marker = owner_macro_prefix(owner_name)
    owner = next((row for row in registry.get("owners", [])
                  if row.get("owner") == owner_name), None)
    if owner is None or set(owner.get("operations", [])) != DATA_POINTER_OPERATIONS:
        errors.append("semantic owner registry has no exact shared.data-pointer family")

    core_text = (root / "src/shared/xr_data_pointer_core.h").read_text(
        encoding="utf-8", errors="strict")
    for token in (f"{marker}_HI", f"{marker}_LO", "XR_DATA_POINTER_OWNER_APPLY",
                  "xr_data_pointer_project_core", "XR_DATA_POINTER_OWNER_BORROW",
                  "XR_DATA_POINTER_STATIC"):
        if token not in core_text:
            errors.append("src/shared/xr_data_pointer_core.h: data pointer lacks stable owner")
            break
    core_body = extract_c_function(core_text, "xr_data_pointer_project_core") or ""
    if ("result.address = address" not in core_body or
            "result.lifetime = lifetime" not in core_body or
            any(token in core_body for token in ("malloc(", "memcpy(", "retain", "release"))):
        errors.append("src/shared/xr_data_pointer_core.h: projection contract drifted")

    vm_text = (root / "src/vm/xvm_dispatch_collection.inc.c").read_text(
        encoding="utf-8", errors="strict")
    start = vm_text.find("vmcase(OP_ARRAY_DATA_PTR)")
    end = vm_text.find("vmbreak;", start)
    body = vm_text[start:end] if start >= 0 and end >= 0 else ""
    for token in (f"{marker}_HI", f"{marker}_LO", "XR_SEM_CONSUMER_VM",
                  "XR_DATA_POINTER_OWNER_APPLY", "XR_DATA_POINTER_STATIC",
                  "XR_DATA_POINTER_OWNER_BORROW", "projection.address"):
        if token not in body:
            errors.append("src/vm/xvm_dispatch_collection.inc.c: OP_ARRAY_DATA_PTR bypasses owner")
            break
    if re.search(r"R\(a\)\s*=.*(?:str->data|arr->data|span->data|R\(b\)\.ptr)", body):
        errors.append("src/vm/xvm_dispatch_collection.inc.c: VM revived direct pointer semantics")

    for relative, consumer in (("src/aot/xrt.h", "XR_SEM_CONSUMER_AOT_HOSTED"),
                               ("src/aot/xrt_core_freestanding.h",
                                "XR_SEM_CONSUMER_AOT_FREESTANDING")):
        text = (root / relative).read_text(encoding="utf-8", errors="strict")
        for token in (f"{marker}_HI", f"{marker}_LO", consumer,
                      "XR_DATA_POINTER_OWNER_APPLY", "xrt_data_pointer_project"):
            if token not in text:
                errors.append(f"{relative}: AOT data-pointer adapter bypasses stable owner")
                break
        if "#ifndef xrt_data_pointer_project" in text:
            errors.append(f"{relative}: data-pointer fallback or alias revived")

    c90_text = (root / "src/aot/xrt_c90.h").read_text(encoding="utf-8", errors="strict")
    c90_body = extract_c_function(c90_text, "xrt_data_pointer_project") or ""
    if ("xr_data_pointer_project_core" not in c90_body or
            any(token in c90_body for token in ("malloc(", "memcpy(", "retain", "release"))):
        errors.append("src/aot/xrt_c90.h: C90 data-pointer adapter owns semantics")

    cgen_text = (root / "src/aot/xi_cgen.c").read_text(encoding="utf-8", errors="strict")
    resolver = extract_c_function(cgen_text, "cg_data_pointer_adapter_name") or ""
    if (f"{marker}_HI" not in resolver or f"{marker}_LO" not in resolver or
            "xr_semantic_owner_cgen_adapter" not in resolver):
        errors.append("src/aot/xi_cgen.c: data pointer does not resolve stable adapter")
    dispatch = (root / "src/aot/xi_cgen_dispatch_helpers.inc.c").read_text(
        encoding="utf-8", errors="strict")
    for emitter, lifetime in (("xicgen_array_data_ptr", "XR_DATA_POINTER_OWNER_BORROW"),
                              ("xicgen_static_bytes_ptr", "XR_DATA_POINTER_STATIC")):
        emitter_body = extract_c_function(dispatch, emitter) or ""
        if ("cg_data_pointer_adapter_name" not in emitter_body or
                ").address" not in emitter_body or lifetime not in emitter_body):
            errors.append(f"src/aot/xi_cgen_dispatch_helpers.inc.c: {emitter} bypasses owner")
        if ("xr_data_pointer_project_core" in emitter_body or
                "xrt_data_pointer_project(" in emitter_body):
            errors.append(f"src/aot/xi_cgen_dispatch_helpers.inc.c: {emitter} owns semantics")

    opt_text = (root / "src/ir/xi_opt.c").read_text(encoding="utf-8", errors="strict")
    if ("case XI_ARRAY_DATA_PTR:" not in opt_text or
            "*out = XR_REP_RAWPTR;" not in opt_text):
        errors.append("src/ir/xi_opt.c: data-pointer optimizer adapter drifted")
    if "case XI_STATIC_BYTES_PTR:" in opt_text:
        errors.append("src/ir/xi_opt.c: optimizer revived private static-pointer semantics")
    return errors


def verify_byte_array_copy_ratchet(root: Path, registry: dict) -> list[str]:
    errors: list[str] = []
    owner_name = "shared.byte-array-copy"
    marker = owner_macro_prefix(owner_name)
    owner = next((row for row in registry.get("owners", [])
                  if row.get("owner") == owner_name), None)
    if owner is None or set(owner.get("operations", [])) != BYTE_ARRAY_COPY_OPERATIONS:
        errors.append("semantic owner registry has no exact shared.byte-array-copy family")

    core_text = (root / "src/shared/xr_byte_array_copy_core.h").read_text(
        encoding="utf-8", errors="strict")
    for token in (f"{marker}_HI", f"{marker}_LO", "XR_BYTE_ARRAY_COPY_OWNER_APPLY",
                  "xr_byte_array_copy_core", "xr_byte_array_copy_range_ok", "memmove(",
                  "count <= length - offset"):
        if token not in core_text:
            errors.append("src/shared/xr_byte_array_copy_core.h: copy contract is incomplete")
            break

    vm_text = (root / "src/vm/xvm_dispatch_collection.inc.c").read_text(
        encoding="utf-8", errors="strict")
    for opcode, kind in (("OP_BYTE_ARRAY_COPY_WITHIN", "XR_BYTE_ARRAY_COPY_WITHIN"),
                         ("OP_BYTE_ARRAY_COPY_FROM", "XR_BYTE_ARRAY_COPY_FROM")):
        start = vm_text.find(f"vmcase({opcode})")
        end = vm_text.find("vmbreak;", start)
        body = vm_text[start:end] if start >= 0 and end >= 0 else ""
        for token in (f"{marker}_HI", f"{marker}_LO", "XR_SEM_CONSUMER_VM",
                      "XR_BYTE_ARRAY_COPY_OWNER_APPLY", "xr_byte_array_copy_core", kind,
                      "XR_ARRAY_MARK_MUTATED"):
            if token not in body:
                errors.append(
                    f"src/vm/xvm_dispatch_collection.inc.c: {opcode} bypasses copy owner")
                break
        if "(int32_t) XR_TO_INT" in body or "memmove(" in body or "range_ok" in body:
            errors.append(
                f"src/vm/xvm_dispatch_collection.inc.c: {opcode} owns or truncates semantics")

    retired = (
        "xr_array_core_bytes_copy_within", "xr_array_core_bytes_copy_from",
        "xr_byte_array_copy_within", "xr_byte_array_copy_from",
        "xrt_byte_array_copy_within_raw", "xrt_byte_array_copy_from_raw",
        "xrt_byte_array_copy_within_checked_raw", "xrt_byte_array_copy_from_checked_raw",
        "xrt_byte_array_copy_within_value", "xrt_byte_array_copy_from_value",
    )
    retired_surfaces = "\n".join(
        (root / relative).read_text(encoding="utf-8", errors="strict")
        for relative in ("src/runtime/object/xarray.c", "src/runtime/object/xarray.h",
                         "src/shared/xr_array_core.h", "src/aot/xrt_byte_array.inc.c")
    )
    for symbol in retired:
        if re.search(rf"\b{re.escape(symbol)}\s*\(", retired_surfaces):
            errors.append(f"retired byte-array copy surface revived: {symbol}")

    for relative, consumer in (("src/aot/xrt_coll.h", "XR_SEM_CONSUMER_AOT_HOSTED"),
                               ("src/aot/xrt_core_freestanding.h",
                                "XR_SEM_CONSUMER_AOT_FREESTANDING")):
        text = (root / relative).read_text(encoding="utf-8", errors="strict")
        for token in (f"{marker}_HI", f"{marker}_LO", consumer,
                      "XR_BYTE_ARRAY_COPY_OWNER_APPLY", "xrt_byte_array_copy_semantics",
                      "xr_byte_array_copy_core"):
            if token not in text:
                errors.append(f"{relative}: byte-array copy adapter bypasses stable owner")
                break
        if "#ifndef xrt_byte_array_copy_semantics" in text:
            errors.append(f"{relative}: byte-array copy fallback or alias revived")

    hosted_text = (root / "src/aot/xrt_byte_array.inc.c").read_text(
        encoding="utf-8", errors="strict")
    hosted_body = extract_c_function(hosted_text, "xrt_byte_array_copy_checked_raw") or ""
    for token in ("xrt_byte_array_copy_semantics", "XR_ARRAY_MARK_MUTATED", "result.changed",
                  "result.status != XR_BYTE_ARRAY_COPY_OK"):
        if token not in hosted_body:
            errors.append("src/aot/xrt_byte_array.inc.c: checked copy adapter is incomplete")
            break
    if "memmove(" in hosted_body or "xrt_byte_array_range_ok" in hosted_body:
        errors.append("src/aot/xrt_byte_array.inc.c: checked copy adapter owns semantics")

    c90_text = (root / "src/aot/xrt_c90.h").read_text(encoding="utf-8", errors="strict")
    for token in ("XR_BYTE_ARRAY_COPY_C90", "#undef XR_BYTE_ARRAY_COPY_C90",
                  "xrt_byte_array_copy_semantics", "xr_byte_array_copy_core"):
        if token not in c90_text:
            errors.append("src/aot/xrt_c90.h: byte-array copy mechanical adapter is incomplete")
            break
    if "memmove(" in c90_text or "xr_byte_array_copy_range_ok(" in c90_text:
        errors.append("src/aot/xrt_c90.h: C90 byte-array copy adapter owns semantics")

    cgen_text = (root / "src/aot/xi_cgen.c").read_text(encoding="utf-8", errors="strict")
    resolver = extract_c_function(cgen_text, "cg_byte_array_copy_adapter_name") or ""
    if (f"{marker}_HI" not in resolver or f"{marker}_LO" not in resolver or
            "xr_semantic_owner_cgen_adapter" not in resolver):
        errors.append("src/aot/xi_cgen.c: byte-array copy does not resolve stable adapter")
    dispatch = (root / "src/aot/xi_cgen_dispatch_helpers.inc.c").read_text(
        encoding="utf-8", errors="strict")
    for emitter, kind in (("xicgen_byte_array_copy_within", "XR_BYTE_ARRAY_COPY_WITHIN"),
                          ("xicgen_byte_array_copy_from", "XR_BYTE_ARRAY_COPY_FROM")):
        body = extract_c_function(dispatch, emitter) or ""
        if ("cg_byte_array_copy_adapter_name" not in body or kind not in body or
                "xicgen_byte_array_i64_arg" not in body):
            errors.append(
                f"src/aot/xi_cgen_dispatch_helpers.inc.c: {emitter} bypasses copy adapter")
        if ("xr_byte_array_copy_core" in body or "memmove(" in body or "range_ok" in body or
                any(symbol in body for symbol in retired)):
            errors.append(
                f"src/aot/xi_cgen_dispatch_helpers.inc.c: {emitter} owns copy semantics")

    for relative in ("src/ir/xi_opt.c", "src/aot/xaot_prepare.c"):
        text = (root / relative).read_text(encoding="utf-8", errors="strict")
        if ("XI_BYTE_ARRAY_COPY_WITHIN" not in text or "XI_BYTE_ARRAY_COPY_FROM" not in text):
            errors.append(f"{relative}: byte-array copy mechanical adapter is missing")
        if "xr_byte_array_copy_core" in text:
            errors.append(f"{relative}: optimizer or prepare path owns copy semantics")

    cmake_text = (root / "tests/unit/CMakeLists.txt").read_text(
        encoding="utf-8", errors="strict")
    for fixture in ("test_byte_array_copy_core", "test_xrt_byte_array_copy_owner_freestanding",
                    "test_xrt_byte_array_copy_owner_c90"):
        if fixture not in cmake_text:
            errors.append(f"tests/unit/CMakeLists.txt: {fixture} is not registered")
    return errors


def verify_target_layout_query_ratchet(root: Path, registry: dict) -> list[str]:
    errors: list[str] = []
    owner_name = "shared.target-layout-query"
    marker = owner_macro_prefix(owner_name)
    owner = next((row for row in registry.get("owners", [])
                  if row.get("owner") == owner_name), None)
    if owner is None or set(owner.get("operations", [])) != TARGET_LAYOUT_QUERY_OPERATIONS:
        errors.append("semantic owner registry has no exact shared.target-layout-query family")

    core_text = (root / "src/shared/xr_native_type_core.h").read_text(
        encoding="utf-8", errors="strict")
    core_body = extract_c_function(core_text, "xr_target_layout_query_core") or ""
    for token in (f"{marker}_HI", f"{marker}_LO", "XR_TARGET_LAYOUT_QUERY_OWNER_APPLY",
                  "XR_TARGET_LAYOUT_QUERY_INVALID_LAYOUT",
                  "XR_TARGET_LAYOUT_QUERY_INVALID_NATIVE_TYPE", "xr_target_data_layout_validate",
                  "xr_native_type_size",
                  "xr_native_type_align"):
        if token not in core_text:
            errors.append("src/shared/xr_native_type_core.h: target-layout contract is incomplete")
            break
    if any(token in core_body for token in ("sizeof(", "_Alignof(", "malloc(", "free(")):
        errors.append("src/shared/xr_native_type_core.h: target-layout core revived host semantics")

    vm_text = (root / "src/ir/xi_emit.c").read_text(encoding="utf-8", errors="strict")
    vm_body = extract_c_function(vm_text, "xi_emit_target_layout_query") or ""
    for token in (f"{marker}_HI", f"{marker}_LO", "XR_SEM_CONSUMER_VM",
                  "XR_TARGET_LAYOUT_QUERY_OWNER_APPLY", "xr_target_layout_query_core"):
        if token not in vm_body:
            errors.append("src/ir/xi_emit.c: VM target-layout query bypasses stable owner")
            break
    if "xr_native_type_size(" in vm_body or "xr_native_type_align(" in vm_body:
        errors.append("src/ir/xi_emit.c: VM target-layout adapter owns query semantics")

    cgen_text = (root / "src/aot/xi_cgen.c").read_text(encoding="utf-8", errors="strict")
    resolver = extract_c_function(cgen_text, "cg_target_layout_query_adapter_name") or ""
    if (f"{marker}_HI" not in resolver or f"{marker}_LO" not in resolver or
            "xr_semantic_owner_cgen_adapter" not in resolver):
        errors.append("src/aot/xi_cgen.c: target-layout query does not resolve stable adapter")
    dispatch = (root / "src/aot/xi_cgen_dispatch_helpers.inc.c").read_text(
        encoding="utf-8", errors="strict")
    emitter = extract_c_function(dispatch, "xicgen_target_layout_expr") or ""
    for token in ("cg_target_layout_query_adapter_name", f"{marker}_HI", f"{marker}_LO",
                  "XR_SEM_CONSUMER_CGEN", "XR_TARGET_LAYOUT_QUERY_OWNER_APPLY",
                  "xr_target_layout_query_core", "bundle->target_data_layout", "INT64_C"):
        if token not in emitter:
            errors.append("src/aot/xi_cgen_dispatch_helpers.inc.c: CGen target-layout query bypasses owner")
            break
    if "sizeof(" in emitter or "_Alignof(" in emitter or "xicgen_native_layout_c_type" in dispatch:
        errors.append("src/aot/xi_cgen_dispatch_helpers.inc.c: host layout semantics revived")
    return errors


def verify_pod_slice_ratchet(root: Path, registry: dict) -> list[str]:
    errors: list[str] = []
    families = (
        ("shared.pod-slice-copy", POD_SLICE_COPY_OPERATIONS, "COPY", "OP_SLICE_COPY",
         "xr_pod_slice_copy_core", "xrt_pod_slice_copy_semantics",
         "xrt_span_copy_checked_raw", "cg_pod_slice_copy_adapter_name", "xicgen_span_copy",
         ("memcpy(", "memmove(")),
        ("shared.pod-slice-compare", POD_SLICE_COMPARE_OPERATIONS, "COMPARE", "OP_SLICE_COMPARE",
         "xr_pod_slice_compare_core", "xrt_pod_slice_compare_semantics",
         "xrt_span_compare_checked_raw", "cg_pod_slice_compare_adapter_name",
         "xicgen_span_compare", ("memcmp(", "_left.length <", "_right.length <")),
    )
    core_text = (root / "src/shared/xr_pod_slice_core.h").read_text(
        encoding="utf-8", errors="strict")
    vm_text = (root / "src/vm/xvm_dispatch_collection.inc.c").read_text(
        encoding="utf-8", errors="strict")
    hosted_text = (root / "src/aot/xrt_coll.h").read_text(encoding="utf-8", errors="strict")
    freestanding_text = (root / "src/aot/xrt_core_freestanding.h").read_text(
        encoding="utf-8", errors="strict")
    c90_text = (root / "src/aot/xrt_c90.h").read_text(encoding="utf-8", errors="strict")
    cgen_text = (root / "src/aot/xi_cgen.c").read_text(encoding="utf-8", errors="strict")
    dispatch_text = (root / "src/aot/xi_cgen_dispatch_helpers.inc.c").read_text(
        encoding="utf-8", errors="strict")

    for (owner_name, operations, tag, opcode, kernel, semantics, adapter, resolver, emitter,
         forbidden) in families:
        marker = owner_macro_prefix(owner_name)
        owner = next((row for row in registry.get("owners", [])
                      if row.get("owner") == owner_name), None)
        if owner is None or set(owner.get("operations", [])) != operations:
            errors.append(f"semantic owner registry has no exact {owner_name} family")
        if (f"{marker}_HI" not in core_text or f"{marker}_LO" not in core_text or
                f"XR_POD_SLICE_{tag}_OWNER_APPLY" not in core_text or kernel not in core_text):
            errors.append(f"src/shared/xr_pod_slice_core.h: {owner_name} lacks stable kernel")

        start = vm_text.find(f"vmcase({opcode})")
        end = vm_text.find("vmbreak;", start)
        vm_body = vm_text[start:end] if start >= 0 and end >= 0 else ""
        if (f"{marker}_HI" not in vm_body or f"{marker}_LO" not in vm_body or
                f"XR_POD_SLICE_{tag}_OWNER_APPLY" not in vm_body or kernel not in vm_body or
                any(token in vm_body for token in forbidden)):
            errors.append(f"src/vm/xvm_dispatch_collection.inc.c: {owner_name} bypasses owner")

        for relative, text in (("src/aot/xrt_coll.h", hosted_text),
                               ("src/aot/xrt_core_freestanding.h", freestanding_text)):
            if (f"{marker}_HI" not in text or f"{marker}_LO" not in text or
                    f"XR_POD_SLICE_{tag}_OWNER_APPLY" not in text or semantics not in text):
                errors.append(f"{relative}: {owner_name} adapter bypasses owner")
            body = extract_c_function(text, adapter) or ""
            if semantics not in body or any(token in body for token in forbidden):
                errors.append(f"{relative}: {owner_name} adapter owns semantics")
        if f"#ifndef {semantics}" in hosted_text or f"#ifndef {semantics}" in freestanding_text:
            errors.append(f"AOT {owner_name} fallback or alias revived")

        c90_body = extract_c_function(c90_text, adapter) or ""
        if kernel not in c90_body or any(token in c90_body for token in forbidden):
            errors.append(f"src/aot/xrt_c90.h: {owner_name} adapter owns semantics")

        resolver_body = extract_c_function(cgen_text, resolver) or ""
        if (f"{marker}_HI" not in resolver_body or f"{marker}_LO" not in resolver_body or
                "xr_semantic_owner_cgen_adapter" not in resolver_body):
            errors.append(f"src/aot/xi_cgen.c: {owner_name} does not resolve stable adapter")
        emitter_body = extract_c_function(dispatch_text, emitter) or ""
        if (resolver not in emitter_body or adapter in emitter_body or kernel in emitter_body or
                any(token in emitter_body for token in forbidden) or "({ xr_span_t" in emitter_body or
                "((uint" in emitter_body):
            errors.append(
                f"src/aot/xi_cgen_dispatch_helpers.inc.c: {owner_name} revived semantics")
    return errors


def verify_pod_slice_view_ratchet(root: Path, registry: dict) -> list[str]:
    errors: list[str] = []
    owner_name = "shared.pod-slice-view"
    marker = owner_macro_prefix(owner_name)
    owner = next((row for row in registry.get("owners", [])
                  if row.get("owner") == owner_name), None)
    if owner is None or set(owner.get("operations", [])) != POD_SLICE_VIEW_OPERATIONS:
        errors.append("semantic owner registry has no exact shared.pod-slice-view family")
    core_text = (root / "src/shared/xr_pod_slice_core.h").read_text(
        encoding="utf-8", errors="strict")
    if (f"{marker}_HI" not in core_text or f"{marker}_LO" not in core_text or
            "XR_POD_SLICE_VIEW_OWNER_APPLY" not in core_text or
            "xr_pod_slice_view_core" not in core_text):
        errors.append("src/shared/xr_pod_slice_core.h: POD view family lacks stable kernel")

    vm_text = (root / "src/vm/xvm_dispatch_collection.inc.c").read_text(
        encoding="utf-8", errors="strict")
    for opcode in ("OP_SLICE_AS_BYTES", "OP_SLICE_REINTERPRET"):
        start = vm_text.find(f"vmcase({opcode})")
        end = vm_text.find("vmbreak;", start)
        body = vm_text[start:end] if start >= 0 and end >= 0 else ""
        if (f"{marker}_HI" not in body or f"{marker}_LO" not in body or
                "XR_POD_SLICE_VIEW_OWNER_APPLY" not in body or
                "xr_pod_slice_view_core" not in body or
                "length * (int64_t) elem_size" in body or
                "length % (int64_t) target_elem_size" in body or
                "length / (int64_t) target_elem_size" in body or
                "(uintptr_t) data %" in body):
            errors.append(f"src/vm/xvm_dispatch_collection.inc.c: {opcode} bypasses POD view owner")

    hosted_text = (root / "src/aot/xrt_coll.h").read_text(encoding="utf-8", errors="strict")
    freestanding_text = (root / "src/aot/xrt_core_freestanding.h").read_text(
        encoding="utf-8", errors="strict")
    for relative, text in (("src/aot/xrt_coll.h", hosted_text),
                           ("src/aot/xrt_core_freestanding.h", freestanding_text)):
        body = extract_c_function(text, "xrt_pod_slice_view_checked_raw") or ""
        if (f"{marker}_HI" not in text or f"{marker}_LO" not in text or
                "XR_POD_SLICE_VIEW_OWNER_APPLY" not in text or
                "xrt_pod_slice_view_semantics" not in body or
                "% (int64_t)" in body or "/ (int64_t)" in body or
                "* (int64_t)" in body or "(uintptr_t)" in body):
            errors.append(f"{relative}: POD view adapter owns semantics")
        if "#ifndef xrt_pod_slice_view_semantics" in text:
            errors.append(f"{relative}: POD view fallback or alias revived")

    c90_text = (root / "src/aot/xrt_c90.h").read_text(encoding="utf-8", errors="strict")
    c90_body = extract_c_function(c90_text, "xrt_pod_slice_view_checked_raw") or ""
    if "xr_pod_slice_view_core" not in c90_body or "% (int64_t)" in c90_body or \
            "/ (int64_t)" in c90_body or "* (int64_t)" in c90_body:
        errors.append("src/aot/xrt_c90.h: C90 POD view adapter owns semantics")

    cgen_text = (root / "src/aot/xi_cgen.c").read_text(encoding="utf-8", errors="strict")
    resolver = extract_c_function(cgen_text, "cg_pod_slice_view_adapter_name") or ""
    if (f"{marker}_HI" not in resolver or f"{marker}_LO" not in resolver or
            "xr_semantic_owner_cgen_adapter" not in resolver):
        errors.append("src/aot/xi_cgen.c: POD view CGen adapter is not owner-resolved")
    dispatch_text = (root / "src/aot/xi_cgen_dispatch_helpers.inc.c").read_text(
        encoding="utf-8", errors="strict")
    forbidden = ("xrt_pod_slice_view_checked_raw", "xr_pod_slice_view_core", "({ xr_span_t",
                 "_s.length *", "_s.length /", "_s.length %", "(uintptr_t)_s.data %")
    for emitter in ("xicgen_span_as_bytes", "xicgen_span_reinterpret"):
        body = extract_c_function(dispatch_text, emitter) or ""
        if "cg_pod_slice_view_adapter_name" not in body or any(token in body for token in forbidden):
            errors.append(f"src/aot/xi_cgen_dispatch_helpers.inc.c: {emitter} revived semantics")
    return errors


def verify_operation_registry(root: Path) -> tuple[list[str], int]:
    errors: list[str] = []
    source_path = root / "xisa/xi/ops.def"
    try:
        source = source_path.read_text(encoding="utf-8")
        operations = parse_xi_ops_def(source, source_path.as_posix())
        owners = parse_xi_semantic_owners(source, operations, source_path.as_posix())
    except SystemExit:
        return ["xisa/xi/ops.def does not define a complete unique semantic owner registry"], 0

    operation_names = {operation.name for operation in operations}
    inventory_path = root / "contracts/target-machine/semantic-owner-inventory.json"
    if not inventory_path.is_file():
        errors.append("target-machine semantic owner inventory is missing")
        return errors, len(operations)
    inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
    inventory_rows = {
        row["operation_id"]: row
        for row in inventory.get("operations", [])
        if row.get("operation_id", "").startswith("xi.")
    }
    if set(inventory_rows) != operation_names:
        errors.append("target-machine Xi operation inventory differs from xisa/xi/ops.def")
    registry_path = root / "contracts/semantic-owner-registry.json"
    registry: dict = {}
    if not registry_path.is_file():
        errors.append("generated semantic owner registry is missing")
    else:
        try:
            registry = json.loads(registry_path.read_text(encoding="utf-8"))
            errors.extend(validate_owner_registry(registry, owners, root))
            errors.extend(verify_generated_owner_header(root, registry))
        except (OSError, UnicodeError, json.JSONDecodeError) as exc:
            errors.append(f"generated semantic owner registry is unreadable: {exc}")
    explicit_owner_by_operation = {
        operation: row.get("owner")
        for row in registry.get("owners", [])
        if set(row.get("consumers", [])) != {"semantic-plan"}
        for operation in row.get("operations", [])
    }
    for name, category in owners.items():
        row = inventory_rows.get(name)
        expected_owner = explicit_owner_by_operation.get(name, "SemanticPlan.operation_registry")
        if row and row.get("future_semantic_owner") != expected_owner:
            errors.append(f"{name}: target-machine inventory does not point at {expected_owner}")
        if category not in {
            "declarative-primitive",
            "shared-semantic-kernel",
            "capability-provider",
            "generated-specialization",
        }:
            errors.append(f"{name}: invalid semantic owner category {category}")
    return errors, len(operations)


def verify(root: Path, write: bool) -> list[str]:
    manifest_path = root / "contracts/semantic-owners.toml"
    snapshot_path = root / "contracts/shared-core-inventory.json"
    registry_path = root / "contracts/semantic-owner-registry.json"
    manifest = tomllib.loads(manifest_path.read_text(encoding="utf-8"))
    errors: list[str] = []

    declared = [entry["header"] for entry in manifest.get("core", [])]
    actual = sorted(path.relative_to(root).as_posix() for path in (root / "src/shared").glob("xr_*_core.h"))
    if len(declared) != len(set(declared)):
        errors.append("semantic-owners.toml contains duplicate core headers")
    if sorted(declared) != actual:
        errors.append(f"core manifest mismatch: declared={sorted(declared)!r} actual={actual!r}")
    if len(actual) != 34:
        errors.append(f"shared-core inventory must contain exactly 34 headers, found {len(actual)}")

    for entry in manifest.get("core", []):
        if entry.get("owner") != "shared-kernel":
            errors.append(f"{entry.get('header')}: observable owner must be shared-kernel")

    inventory = build_inventory(root, manifest)
    for row in inventory:
        if row["representation"] == "native" and row["contains_tagged_value"]:
            errors.append(f"{row['header']}: native kernel contains XrValue")

    rendered = json.dumps({"schema": 1, "cores": inventory}, indent=2, sort_keys=True) + "\n"
    if write:
        snapshot_path.write_text(rendered, encoding="utf-8")
    elif not snapshot_path.is_file():
        errors.append("contracts/shared-core-inventory.json is missing; run with --write")
    elif snapshot_path.read_text(encoding="utf-8") != rendered:
        errors.append("shared-core caller inventory drifted; review it and run with --write")

    errors.extend(verify_sort_ratchet(root))
    errors.extend(verify_truthiness_ratchet(root))
    errors.extend(verify_type_identity_ratchet(root))
    if registry_path.is_file():
        registry = json.loads(registry_path.read_text(encoding="utf-8", errors="strict"))
        errors.extend(verify_exact_bits_ratchet(root, registry))
        errors.extend(verify_bits_not_ratchet(root, registry))
        errors.extend(verify_range_ratchet(root, registry))
        errors.extend(verify_bitwise_binary_ratchet(root, registry))
        errors.extend(verify_shift_ratchet(root, registry))
        errors.extend(verify_numeric_neg_ratchet(root, registry))
        errors.extend(verify_numeric_width_ratchet(root, registry))
        errors.extend(verify_byte_slice_scalar_ratchet(root, registry))
        errors.extend(verify_byte_slice_fill_ratchet(root, registry))
        errors.extend(verify_byte_slice_mutation_ratchet(root, registry))
        errors.extend(verify_byte_slice_compare_ratchet(root, registry))
        errors.extend(verify_byte_slice_common_prefix_ratchet(root, registry))
        errors.extend(verify_raw_memory_copy_ratchet(root, registry))
        errors.extend(verify_raw_scalar_access_ratchet(root, registry))
        errors.extend(verify_enum_metadata_access_ratchet(root, registry))
        errors.extend(verify_cell_access_ratchet(root, registry))
        errors.extend(verify_null_test_ratchet(root, registry))
        errors.extend(verify_data_pointer_ratchet(root, registry))
        errors.extend(verify_byte_array_copy_ratchet(root, registry))
        errors.extend(verify_target_layout_query_ratchet(root, registry))
        errors.extend(verify_pod_slice_ratchet(root, registry))
        errors.extend(verify_pod_slice_view_ratchet(root, registry))
    registry_errors, _ = verify_operation_registry(root)
    errors.extend(registry_errors)
    return errors


def self_test() -> int:
    assert re.search(r"\)\s*[&|^]\s*\(", "(lhs) & (rhs)")
    assert not re.search(r"\)\s*[&|^]\s*\(",
                         "xrt_bitwise_binary_eval(XR_BITWISE_BINARY_AND, lhs, rhs)")
    assert re.search(r"emit_native_[A-Za-z0-9_]*shift", "emit_native_const_shift")
    assert not re.search(r"emit_native_[A-Za-z0-9_]*shift", "emit_shift_binop_ctx")
    assert re.search(r"rewrite_to_const_int\s*\(\s*v\s*,\s*~",
                     "rewrite_to_const_int(v, ~unary_i);")
    assert not re.search(r"rewrite_to_const_int\s*\(\s*v\s*,\s*~",
                         "rewrite_to_const_int(v, xr_bits_not_i64(unary_i));")
    assert SIGNATURE_RE.findall("static inline int xr_demo_core(int x) {") == ["xr_demo_core"]
    assert SIGNATURE_RE.findall(
        "XR_BYTE_SLICE_SCALAR_INLINE int xr_demo_c90_core(int x) {") == ["xr_demo_c90_core"]
    assert "xrt_introsort_foo".startswith(SORT_OLD_SYMBOLS[3])
    assert extract_c_function("static int owner(int x) { return x != 0; }", "owner") == \
        "{ return x != 0; }"
    assert TRUTHINESS_RETIRED_DECISIONS[0] in \
        "bool owner(XrValue value) { return XR_TO_BOOL(value) != 0; }"
    assert truthiness_surrogate_owner(
        "bool owner(void) { return xg_global_evidence != NULL; }") == "xg_global_evidence"
    assert truthiness_surrogate_owner(
        'bool owner(void) { return lookup("xi.not"); }') == '"xi.not"'
    assert truthiness_surrogate_owner("bool owner(void) { return true; }") is None
    assert type_identity_surrogate_owner(
        "int owner(void) { return xg_global_evidence != NULL; }") == "xg_global_evidence"
    assert type_identity_surrogate_owner(
        'int owner(void) { return lookup("xi.typeid"); }') == '"xi.typeid"'
    assert type_identity_surrogate_owner("int owner(void) { return 8; }") is None

    def stable_fields(name: str, key: str) -> dict[str, str]:
        stable_id = semantic_stable_id(name)
        return {
            key: stable_id,
            f"{key}_hi": f"0x{stable_id[:16]}",
            f"{key}_lo": f"0x{stable_id[16:]}",
        }

    semantic_plan_binding = {
        "consumer": "semantic-plan",
        "path": "src/plan/semantic/xr_semantic_ops.c",
        "symbol": "xr_semantic_op_contract",
    }
    runtime_binding = {
        "consumer": "runtime",
        "path": "src/runtime/value/xvalue_truthy.c",
        "symbol": "xr_value_is_truthy",
    }
    add_row = {
        "operation": "xi.add",
        **stable_fields("xi.add", "operation_id"),
        "owner": "xi.add",
        **stable_fields("xi.add", "owner_id"),
        "category": "declarative-primitive",
        "consumers": ["semantic-plan"],
        "consumer_bits": "0x00000001",
        "cgen_adapter": None,
        "production_bindings": [semantic_plan_binding],
    }
    truthy_row = {
        "operation": "xi.not",
        **stable_fields("xi.not", "operation_id"),
        "owner": "shared.truthiness",
        **stable_fields("shared.truthiness", "owner_id"),
        "category": "shared-semantic-kernel",
        "consumers": ["runtime"],
        "consumer_bits": "0x00000002",
        "cgen_adapter": None,
        "production_bindings": [runtime_binding],
    }
    registry = {
        "schema": 1,
        "source": "fixture",
        "canonical_fingerprint": "",
        "consumers": {"semantic-plan": "0x00000001", "runtime": "0x00000002"},
        "owners": [
            {
                "owner": "shared.truthiness",
                **stable_fields("shared.truthiness", "owner_id"),
                "category": "shared-semantic-kernel",
                "operations": ["xi.not"],
                "consumers": ["runtime"],
                "consumer_bits": "0x00000002",
                "cgen_adapter": None,
                "production_bindings": [runtime_binding],
            },
            {
                "owner": "xi.add",
                **stable_fields("xi.add", "owner_id"),
                "category": "declarative-primitive",
                "operations": ["xi.add"],
                "consumers": ["semantic-plan"],
                "consumer_bits": "0x00000001",
                "cgen_adapter": None,
                "production_bindings": [semantic_plan_binding],
            },
        ],
        "operations": [add_row, truthy_row],
    }
    registry["canonical_fingerprint"] = canonical_registry_fingerprint(registry)
    categories = {"xi.add": "declarative-primitive", "xi.not": "shared-semantic-kernel"}

    def mutated(change, refresh: bool = True) -> dict:
        candidate = copy.deepcopy(registry)
        change(candidate)
        if refresh:
            candidate["canonical_fingerprint"] = canonical_registry_fingerprint(candidate)
        return candidate

    def require_error(candidate: dict, needle: str, root: Path) -> None:
        found = validate_owner_registry(candidate, categories, root)
        assert any(needle in error for error in found), (needle, found)

    with tempfile.TemporaryDirectory(prefix="xray-semantic-owner-selftest-") as directory:
        root = Path(directory)
        plan_source = root / semantic_plan_binding["path"]
        plan_source.parent.mkdir(parents=True, exist_ok=True)
        plan_source.write_text("xr_semantic_op_contract\n", encoding="utf-8")
        runtime_source = root / runtime_binding["path"]
        runtime_source.parent.mkdir(parents=True, exist_ok=True)
        runtime_source.write_text(
            "XR_SEM_OWNER_ID_SHARED_TRUTHINESS_HI\n"
            "XR_SEM_OWNER_ID_SHARED_TRUTHINESS_LO\n"
            "xr_value_is_truthy\n",
            encoding="utf-8",
        )
        assert validate_owner_registry(registry, categories, root) == []

        require_error(mutated(lambda value: value["operations"].pop()),
                      "operation is unowned", root)
        require_error(mutated(lambda value: value["operations"].append(
            copy.deepcopy(value["operations"][1]))), "multiple owner rows", root)

        def add_unknown(value: dict) -> None:
            row = copy.deepcopy(value["operations"][0])
            row["operation"] = "xi.ghost"
            row.update(stable_fields("xi.ghost", "operation_id"))
            row["owner"] = "xi.ghost"
            row.update(stable_fields("xi.ghost", "owner_id"))
            value["operations"].append(row)
            summary = copy.deepcopy(value["owners"][1])
            summary["owner"] = "xi.ghost"
            summary.update(stable_fields("xi.ghost", "owner_id"))
            summary["operations"] = ["xi.ghost"]
            value["owners"].append(summary)

        require_error(mutated(add_unknown), "unknown operation", root)

        def duplicate_owner_id(value: dict) -> None:
            duplicate = stable_fields("xi.add", "owner_id")
            value["operations"][1].update(duplicate)
            value["owners"][0].update(duplicate)

        require_error(mutated(duplicate_owner_id), "duplicate stable owner ID", root)

        def remove_consumers(value: dict) -> None:
            value["operations"][1]["consumers"] = []
            value["operations"][1]["consumer_bits"] = "0x00000000"
            value["operations"][1]["production_bindings"] = []
            value["owners"][0]["consumers"] = []
            value["owners"][0]["consumer_bits"] = "0x00000000"
            value["owners"][0]["production_bindings"] = []

        require_error(mutated(remove_consumers), "no production consumer", root)
        require_error(mutated(lambda value: value["operations"][1]["consumers"].append("ghost")),
                      "unknown production consumers", root)
        require_error(mutated(lambda value: value["operations"][1]["production_bindings"][0]
                              .update(path="src/runtime/value/dead.c")),
                      "production consumer source is missing", root)
        bad_fingerprint = mutated(lambda value: value.update(canonical_fingerprint="0" * 64),
                                  refresh=False)
        require_error(bad_fingerprint, "canonical fingerprint mismatch", root)

        runtime_source.write_text("xr_value_is_truthy\n", encoding="utf-8")
        require_error(registry, "does not consume stable owner ID", root)
    print("semantic-owner verifier self-test: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".")
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    root = Path(args.root).resolve()
    errors = verify(root, args.write)
    if errors:
        print("semantic-owner gate failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    _, operation_count = verify_operation_registry(root)
    manifest = tomllib.loads((root / "contracts/semantic-owners.toml").read_text(encoding="utf-8"))
    print(f"semantic-owner gate: PASS ({len(manifest['core'])} cores, {operation_count} Xi ops, "
          "stable owner IDs, production consumer bindings, owner ratchets)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

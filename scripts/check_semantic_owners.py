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
    r"\b(?:(?:static\s+)?inline|XR_BYTE_SLICE_SCALAR_INLINE)\s+[^;{}]*?"
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
    if len(actual) != 27:
        errors.append(f"shared-core inventory must contain exactly 27 headers, found {len(actual)}")

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
        errors.extend(verify_numeric_width_ratchet(root, registry))
        errors.extend(verify_byte_slice_scalar_ratchet(root, registry))
        errors.extend(verify_byte_slice_compare_ratchet(root, registry))
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

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
SIGNATURE_RE = re.compile(r"\b(?:static\s+)?inline\s+[^;{}]*?\b(xr_[A-Za-z0-9_]+)\s*\(")
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
NUMERIC_NARROW_OPERATIONS = {
    "xi.narrow.i8",
    "xi.narrow.u8",
    "xi.narrow.i16",
    "xi.narrow.u16",
    "xi.narrow.i32",
    "xi.narrow.u32",
    "xi.narrow.f32",
}
NUMERIC_NARROW_KERNELS = {
    "xr_numeric_narrow_i8",
    "xr_numeric_narrow_u8",
    "xr_numeric_narrow_i16",
    "xr_numeric_narrow_u16",
    "xr_numeric_narrow_i32",
    "xr_numeric_narrow_u32",
    "xr_numeric_narrow_f32",
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


def verify_numeric_narrow_ratchet(root: Path, registry: dict) -> list[str]:
    errors: list[str] = []
    marker = owner_macro_prefix("shared.numeric-conversion")
    owner = next((row for row in registry.get("owners", [])
                  if row.get("owner") == "shared.numeric-conversion"), None)
    if owner is None or set(owner.get("operations", [])) != NUMERIC_NARROW_OPERATIONS:
        errors.append(
            "semantic owner registry has no exact shared.numeric-conversion narrow family")

    core_text = (root / "src/shared/xr_numeric_conversion_core.h").read_text(
        encoding="utf-8", errors="strict")
    if (f"{marker}_HI" not in core_text or f"{marker}_LO" not in core_text or
            "XR_NUMERIC_NARROW_OWNER_APPLY" not in core_text or
            not NUMERIC_NARROW_KERNELS.issubset(set(SIGNATURE_RE.findall(core_text)))):
        errors.append(
            "src/shared/xr_numeric_conversion_core.h: numeric narrow family lacks its stable owner kernels")

    vm_text = (root / "src/vm/xvm_template_width_gen.inc.c").read_text(
        encoding="utf-8", errors="strict")
    narrow_end = vm_text.find("#define XVM_TEMPLATE_WIDTH_INT_CASE")
    narrow_body = vm_text[:narrow_end] if narrow_end >= 0 else ""
    if (f"{marker}_HI" not in narrow_body or f"{marker}_LO" not in narrow_body or
            "XR_NUMERIC_NARROW_OWNER_APPLY" not in narrow_body or
            not NUMERIC_NARROW_KERNELS.issubset(set(re.findall(
                r"\b(xr_numeric_narrow_[a-z0-9]+)\b", vm_text)))):
        errors.append(
            "src/vm/xvm_template_width_gen.inc.c: VM numeric narrowing bypasses stable owner")
    if "xr_numeric_int_convert_i64" in narrow_body or "xr_numeric_f64_to_f32" in narrow_body:
        errors.append(
            "src/vm/xvm_template_width_gen.inc.c: VM revived private numeric narrow semantics")

    for relative in EXACT_BITS_AOT_BINDINGS:
        text = (root / relative).read_text(encoding="utf-8", errors="strict")
        if (f"{marker}_HI" not in text or f"{marker}_LO" not in text or
                "XR_NUMERIC_NARROW_OWNER_APPLY" not in text or
                "xrt_numeric_narrow_eval" not in text):
            errors.append(f"{relative}: AOT numeric narrow adapter bypasses stable owner")

    cgen_text = (root / "src/aot/xi_cgen.c").read_text(encoding="utf-8", errors="strict")
    adapter_body = extract_c_function(cgen_text, "cg_numeric_narrow_adapter_name")
    if (adapter_body is None or f"{marker}_HI" not in adapter_body or
            f"{marker}_LO" not in adapter_body or
            "xr_semantic_owner_cgen_adapter" not in adapter_body):
        errors.append("src/aot/xi_cgen.c: CGen numeric narrow does not resolve by stable owner ID")

    dispatch_text = (root / "src/aot/xi_cgen_dispatch_helpers.inc.c").read_text(
        encoding="utf-8", errors="strict")
    emitter_body = extract_c_function(dispatch_text, "xicgen_numeric_narrow")
    if (emitter_body is None or "cg_numeric_narrow_adapter_name" not in emitter_body or
            "xi_to_c_template_width_narrow_kernel" not in emitter_body):
        errors.append(
            "src/aot/xi_cgen_dispatch_helpers.inc.c: numeric narrow bypasses owner adapter")
    elif any(token in emitter_body for token in
             ("xr_numeric_int_convert_i64", "xr_numeric_f64_to_f32", "(uint8_t)",
              "(uint16_t)", "(uint32_t)", "(int8_t)", "(int16_t)", "(int32_t)")):
        errors.append(
            "src/aot/xi_cgen_dispatch_helpers.inc.c: numeric narrow revived CGen semantics")
    retired = (
        "xicgen_unsigned_narrow_lowbits_binop",
        "cg_unsigned_narrow_lowbits_binop_arg",
        "cg_unsigned_narrow_cast_ctype",
        "cg_lowbits_binop_elided_into_unsigned_narrow",
    )
    for symbol in retired:
        if symbol in cgen_text or symbol in dispatch_text:
            errors.append(f"CGen revived retired private numeric narrow path: {symbol}")
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
    if len(actual) != 26:
        errors.append(f"shared-core inventory must contain exactly 26 headers, found {len(actual)}")

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
        errors.extend(verify_numeric_narrow_ratchet(root, registry))
    registry_errors, _ = verify_operation_registry(root)
    errors.extend(registry_errors)
    return errors


def self_test() -> int:
    assert SIGNATURE_RE.findall("static inline int xr_demo_core(int x) {") == ["xr_demo_core"]
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

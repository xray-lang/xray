#!/usr/bin/env python3
"""Validate and project the canonical XrProgram wire-format schema."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


SCHEMA_PATH = Path("xisa/program/schema.json")
SOURCE_PROJECTION_PATH = Path("xisa/program/xi-source-projection.json")
CORE_REGISTRY_PATH = Path("xisa/core/registry.json")
XI_REGISTRY_PATH = Path("xisa/xi/ops.def")
HEADER_PATH = Path("src/program/xr_program_schema_gen.h")
SOURCE_PROJECTION_HEADER_PATH = Path("src/program/xr_program_xi_projection_gen.h")
SOURCE_PROJECTION_SOURCE_PATH = Path("src/program/xr_program_xi_projection_gen.c")
SPEC_PATH = Path("contracts/canonical-program/xrprogram-format-v1.md")
COVERAGE_PATH = Path("contracts/canonical-program/xrprogram-format-coverage.json")
TOP_KEYS = {"schema", "format", "encoding", "type_system", "value_system", "sections", "limits"}
FORMAT_KEYS = {"major", "minor", "magic_hex", "program_id_domain"}
ENCODING_KEYS = {
    "fixed_integers",
    "variable_integers",
    "signed_integers",
    "section_offsets",
    "section_order",
    "unknown_section_policy",
}
LIMIT_KEYS = {
    "artifact_bytes",
    "sections",
    "features",
    "types",
    "constants",
    "functions",
    "blocks_per_function",
    "values_per_function",
    "operations",
    "operands_per_operation",
    "successors_per_operation",
}
TYPE_SYSTEM_KEYS = {
    "builtin_rows",
    "dynamic_type_base",
    "dynamic_kinds",
    "identity_order",
    "aggregate_shape",
    "variant_shape",
    "recursive_value_shape",
    "physical_layout",
}
VALUE_SYSTEM_KEYS = {
    "function_parameter_contract",
    "parameter_modes",
    "value_categories",
    "place_physical_layout",
}
SOURCE_PROJECTION_KEYS = {
    "schema",
    "source_stage",
    "migration_policy",
    "value_mappings",
    "structural_mappings",
}
SOURCE_POLICY = {
    "semantic_authority": "CoreSpec",
    "unlisted_xi_operation": "reject",
    "new_pipeline_legacy_dependencies": [],
    "old_product_route": "frozen-not-consumed",
    "physical_route_deletion": "atomic-task-302",
    "compatibility_bridge": "forbidden",
}
VALUE_MAPPING_KEYS = {
    "xi_operation",
    "result_type",
    "projection_kind",
    "core_operation",
    "immediate_u32",
}
PROJECTION_KINDS = {
    "constant": "XR_PROGRAM_XI_PROJECTION_CONSTANT",
    "binary-arithmetic": "XR_PROGRAM_XI_PROJECTION_BINARY_ARITHMETIC",
    "compare": "XR_PROGRAM_XI_PROJECTION_COMPARE",
    "sealed-direct-call": "XR_PROGRAM_XI_PROJECTION_SEALED_DIRECT_CALL",
    "aggregate-construct": "XR_PROGRAM_XI_PROJECTION_AGGREGATE_CONSTRUCT",
    "aggregate-project": "XR_PROGRAM_XI_PROJECTION_AGGREGATE_PROJECT",
    "aggregate-update": "XR_PROGRAM_XI_PROJECTION_AGGREGATE_UPDATE",
    "variant-construct": "XR_PROGRAM_XI_PROJECTION_VARIANT_CONSTRUCT",
    "variant-test": "XR_PROGRAM_XI_PROJECTION_VARIANT_TEST",
    "variant-project": "XR_PROGRAM_XI_PROJECTION_VARIANT_PROJECT",
    "owner-move": "XR_PROGRAM_XI_PROJECTION_OWNER_MOVE",
    "place-local": "XR_PROGRAM_XI_PROJECTION_PLACE_LOCAL",
    "place-load": "XR_PROGRAM_XI_PROJECTION_PLACE_LOAD",
    "place-store": "XR_PROGRAM_XI_PROJECTION_PLACE_STORE",
}
CORE_TYPE_NAMES = {
    "any": "XR_PROGRAM_XI_ANY_RESULT_TYPE",
    "void": "XR_CORE_TYPE_VOID",
    "bool": "XR_CORE_TYPE_BOOL",
    "i64": "XR_CORE_TYPE_I64",
    "u32": "XR_CORE_TYPE_U32",
    "error": "XR_CORE_TYPE_ERROR",
}


class ProgramSchemaError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ProgramSchemaError(message)


def reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, child in pairs:
        require(key not in value, f"duplicate JSON key: {key}")
        value[key] = child
    return value


def canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2) + "\n"


def read_schema(path: Path) -> dict[str, Any]:
    raw = path.read_text(encoding="utf-8", errors="strict")
    try:
        value = json.loads(raw, object_pairs_hook=reject_duplicate_keys)
    except (json.JSONDecodeError, ProgramSchemaError) as exc:
        raise ProgramSchemaError(f"invalid JSON in {path}: {exc}") from exc
    require(isinstance(value, dict), "program schema must be an object")
    require(raw == canonical_json(value), "program schema must be canonical two-space JSON")
    return value


def read_json(path: Path, label: str) -> dict[str, Any]:
    raw = path.read_text(encoding="utf-8", errors="strict")
    try:
        value = json.loads(raw, object_pairs_hook=reject_duplicate_keys)
    except (json.JSONDecodeError, ProgramSchemaError) as exc:
        raise ProgramSchemaError(f"invalid JSON in {path}: {exc}") from exc
    require(isinstance(value, dict), f"{label} must be an object")
    require(raw == canonical_json(value), f"{label} must be canonical two-space JSON")
    return value


def c_identifier(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9]+", "_", name).strip("_").upper()


def validate(schema: dict[str, Any]) -> None:
    require(set(schema) == TOP_KEYS, "program schema top-level fields drifted")
    require(schema["schema"] == "xray-program-format-schema/1", "program schema version drifted")
    fmt = schema["format"]
    require(isinstance(fmt, dict) and set(fmt) == FORMAT_KEYS, "format fields drifted")
    require(fmt["major"] == 1 and fmt["minor"] == 0, "W2 must define XrProgram format 1.0")
    require(isinstance(fmt["magic_hex"], str) and re.fullmatch(r"[0-9a-f]{16}", fmt["magic_hex"]),
            "program magic must be eight canonical lowercase hex bytes")
    require(fmt["program_id_domain"] == "xray-program-id-v1", "ProgramId domain drifted")

    encoding = schema["encoding"]
    require(isinstance(encoding, dict) and set(encoding) == ENCODING_KEYS,
            "encoding fields drifted")
    expected_encoding = {
        "fixed_integers": "little-endian",
        "variable_integers": "minimal-uleb128",
        "signed_integers": "zigzag-then-minimal-uleb128",
        "section_offsets": "relative-to-payload-start",
        "section_order": "ascending-stable-id",
        "unknown_section_policy": "reject-until-declared-optional",
    }
    require(encoding == expected_encoding, "wire encoding policy drifted")

    type_system = schema["type_system"]
    require(isinstance(type_system, dict) and set(type_system) == TYPE_SYSTEM_KEYS,
            "type-system contract fields drifted")
    require(type_system == {
        "builtin_rows": ["0:void", "1:bool", "2:i64", "3:u32", "4:error"],
        "dynamic_type_base": 16,
        "dynamic_kinds": ["aggregate", "variant"],
        "identity_order": "ascending-semantic-key",
        "aggregate_shape": "declaration-ordered-field-type-ids",
        "variant_shape": "declaration-ordered-variants-and-payload-type-ids",
        "recursive_value_shape": "reject",
        "physical_layout": "forbidden",
    }, "type-system semantic policy drifted")

    value_system = schema["value_system"]
    require(isinstance(value_system, dict) and set(value_system) == VALUE_SYSTEM_KEYS,
            "value-system contract fields drifted")
    require(value_system == {
        "function_parameter_contract": "ordered-(TypeId,ParamMode)",
        "parameter_modes": ["0:read", "1:ref", "2:move"],
        "value_categories": ["0:value", "1:place"],
        "place_physical_layout": "forbidden",
    }, "value-system semantic policy drifted")

    sections = schema["sections"]
    require(isinstance(sections, list) and sections, "sections must be a non-empty array")
    ids: list[int] = []
    names: list[str] = []
    for section in sections:
        require(isinstance(section, dict) and set(section) == {"stable_id", "name", "required"},
                "section fields drifted")
        require(isinstance(section["stable_id"], int) and 1 <= section["stable_id"] <= 65535,
                "section stable id is invalid")
        require(isinstance(section["name"], str) and section["name"], "section name is invalid")
        require(isinstance(section["required"], bool), "section required marker is invalid")
        ids.append(section["stable_id"])
        names.append(section["name"])
    require(ids == sorted(ids) and len(ids) == len(set(ids)), "section ids must be unique and sorted")
    require(len(names) == len(set(names)), "section names must be unique")
    required = [section["name"] for section in sections if section["required"]]
    require(required == ["types", "constants", "functions", "code", "imports", "boundaries",
                         "semantic-metadata"], "required section set drifted")
    require(sections[-1] == {"stable_id": 8, "name": "debug-sidecar-binding", "required": False},
            "debug sidecar must be the sole optional W2 section")

    limits = schema["limits"]
    require(isinstance(limits, dict) and set(limits) == LIMIT_KEYS, "resource-limit fields drifted")
    for name, value in limits.items():
        require(isinstance(value, int) and value > 0 and value <= (1 << 31),
                f"resource limit {name} is invalid")
    require(limits["sections"] == len(sections), "section limit must equal declared section count")
    require(limits["successors_per_operation"] <= 32, "successor bound must remain small")


def validate_source_projection(projection: dict[str, Any], core: dict[str, Any],
                               xi_source: str) -> None:
    require(set(projection) == SOURCE_PROJECTION_KEYS,
            "Xi source projection top-level fields drifted")
    require(projection["schema"] == "xray-program-xi-source-projection/1",
            "Xi source projection schema drifted")
    require(projection["source_stage"] == "XI_STAGE_OPTIMIZED",
            "source projection must consume target-neutral optimized Xi")
    require(projection["migration_policy"] == SOURCE_POLICY,
            "Xi source projection migration policy drifted")

    core_operations = {
        row["spelling"]: row for row in core.get("operations", [])
        if isinstance(row, dict) and isinstance(row.get("spelling"), str)
    }
    core_types = {
        row["name"] for row in core.get("types", [])
        if isinstance(row, dict) and isinstance(row.get("name"), str)
    }
    xi_operations = set(re.findall(r"\(define-xi-op\s+(xi\.[a-z0-9.-]+)", xi_source))
    require(core_operations, "CoreSpec has no operations")
    require(xi_operations, "Xi registry has no operations")

    rows = projection["value_mappings"]
    require(isinstance(rows, list) and rows, "Xi source projection has no value mappings")
    identities: set[tuple[str, str]] = set()
    for row in rows:
        require(isinstance(row, dict) and set(row) == VALUE_MAPPING_KEYS,
                "Xi source projection value mapping fields drifted")
        identity = (row["xi_operation"], row["result_type"])
        require(identity not in identities, f"duplicate Xi source projection {identity}")
        identities.add(identity)
        require(row["xi_operation"] in xi_operations,
                f"unknown Xi operation {row['xi_operation']}")
        require(row["result_type"] == "any" or row["result_type"] in core_types,
                f"unknown projection result type {row['result_type']}")
        require(row["projection_kind"] in PROJECTION_KINDS,
                f"unknown projection kind {row['projection_kind']}")
        require(row["core_operation"] in core_operations,
                f"unknown CoreSpec operation {row['core_operation']}")
        require(isinstance(row["immediate_u32"], int)
                and 0 <= row["immediate_u32"] <= 0xffffffff,
                f"invalid immediate for {identity}")

    structural = projection["structural_mappings"]
    require(isinstance(structural, list) and structural,
            "Xi source projection has no structural mappings")
    sources: set[str] = set()
    for row in structural:
        require(isinstance(row, dict) and set(row) == {"source", "core_operation"},
                "structural projection fields drifted")
        require(isinstance(row["source"], str) and row["source"] not in sources,
                "structural projection source is empty or duplicated")
        sources.add(row["source"])
        require(row["core_operation"] in core_operations,
                f"unknown structural CoreSpec operation {row['core_operation']}")
    require(sources == {
        "block-argument", "unconditional-edge", "conditional-edge", "function-return"
    }, "structural projection set drifted")


def xi_c_identifier(name: str) -> str:
    return "XI_" + c_identifier(name.removeprefix("xi."))


def core_c_identifier(name: str) -> str:
    return "XR_CORE_OP_" + c_identifier(name)


def generate_source_projection_header() -> str:
    return "\n".join([
        "/* AUTO-GENERATED by programgen - DO NOT EDIT */",
        "/* Source: xisa/program/xi-source-projection.json */",
        "#ifndef XR_PROGRAM_XI_PROJECTION_GEN_H",
        "#define XR_PROGRAM_XI_PROJECTION_GEN_H",
        "",
        '#include "core/xr_core_spec_gen.h"',
        "#include <stdbool.h>",
        "#include <stdint.h>",
        "",
        "#define XR_PROGRAM_XI_ANY_RESULT_TYPE UINT16_MAX",
        "",
        "typedef enum XrProgramXiProjectionKind {",
        "    XR_PROGRAM_XI_PROJECTION_CONSTANT = 1,",
        "    XR_PROGRAM_XI_PROJECTION_BINARY_ARITHMETIC = 2,",
        "    XR_PROGRAM_XI_PROJECTION_COMPARE = 3,",
        "    XR_PROGRAM_XI_PROJECTION_SEALED_DIRECT_CALL = 4,",
        "    XR_PROGRAM_XI_PROJECTION_AGGREGATE_CONSTRUCT = 5,",
        "    XR_PROGRAM_XI_PROJECTION_AGGREGATE_PROJECT = 6,",
        "    XR_PROGRAM_XI_PROJECTION_AGGREGATE_UPDATE = 7,",
        "    XR_PROGRAM_XI_PROJECTION_VARIANT_CONSTRUCT = 8,",
        "    XR_PROGRAM_XI_PROJECTION_VARIANT_TEST = 9,",
        "    XR_PROGRAM_XI_PROJECTION_VARIANT_PROJECT = 10,",
        "    XR_PROGRAM_XI_PROJECTION_OWNER_MOVE = 11,",
        "    XR_PROGRAM_XI_PROJECTION_PLACE_LOCAL = 12,",
        "    XR_PROGRAM_XI_PROJECTION_PLACE_LOAD = 13,",
        "    XR_PROGRAM_XI_PROJECTION_PLACE_STORE = 14,",
        "} XrProgramXiProjectionKind;",
        "",
        "typedef struct XrProgramXiProjection {",
        "    uint16_t core_operation_id;",
        "    uint16_t result_type_id;",
        "    uint32_t immediate_u32;",
        "    XrProgramXiProjectionKind kind;",
        "} XrProgramXiProjection;",
        "",
        "bool xr_program_xi_projection(uint16_t xi_operation, uint16_t result_type_id,",
        "                              XrProgramXiProjection *projection_out);",
        "bool xr_program_xi_value_is_materialized(uint16_t xi_operation);",
        "",
        "#endif /* XR_PROGRAM_XI_PROJECTION_GEN_H */",
        "",
    ])


def generate_source_projection_source(projection: dict[str, Any],
                                      core: dict[str, Any]) -> str:
    core_ids = {row["spelling"]: row["stable_id"] for row in core["operations"]}
    lines = [
        "/* AUTO-GENERATED by programgen - DO NOT EDIT */",
        "/* Source: xisa/program/xi-source-projection.json */",
        '#include "program/xr_program_xi_projection_gen.h"',
        '#include "core/xr_core_spec_gen.h"',
        '#include "ir/xi.h"',
        "",
        "typedef struct XrProgramXiProjectionRow {",
        "    uint16_t xi_operation;",
        "    uint16_t result_type_id;",
        "    uint16_t core_operation_id;",
        "    uint32_t immediate_u32;",
        "    XrProgramXiProjectionKind kind;",
        "} XrProgramXiProjectionRow;",
        "",
        "static const XrProgramXiProjectionRow xr_program_xi_projection_rows[] = {",
    ]
    for row in projection["value_mappings"]:
        core_name = row["core_operation"]
        require(core_ids[core_name] <= 65535, f"CoreSpec id too wide for {core_name}")
        lines.extend([
            "    {",
            f"        {xi_c_identifier(row['xi_operation'])},",
            f"        {CORE_TYPE_NAMES[row['result_type']]},",
            f"        {core_c_identifier(core_name)},",
            f"        UINT32_C({row['immediate_u32']}),",
            f"        {PROJECTION_KINDS[row['projection_kind']]},",
            "    },",
        ])
    lines.extend([
        "};",
        "",
        "bool xr_program_xi_projection(uint16_t xi_operation, uint16_t result_type_id,",
        "                              XrProgramXiProjection *projection_out) {",
        "    if (!projection_out)",
        "        return false;",
        "    for (uint32_t index = 0;",
        "         index < sizeof(xr_program_xi_projection_rows) / sizeof(xr_program_xi_projection_rows[0]);",
        "         ++index) {",
        "        const XrProgramXiProjectionRow *row = &xr_program_xi_projection_rows[index];",
        "        if (row->xi_operation != xi_operation ||",
        "            (row->result_type_id != XR_PROGRAM_XI_ANY_RESULT_TYPE &&",
        "             row->result_type_id != result_type_id))",
        "            continue;",
        "        projection_out->core_operation_id = row->core_operation_id;",
        "        projection_out->result_type_id = result_type_id;",
        "        projection_out->immediate_u32 = row->immediate_u32;",
        "        projection_out->kind = row->kind;",
        "        return true;",
        "    }",
        "    return false;",
        "}",
        "",
        "bool xr_program_xi_value_is_materialized(uint16_t xi_operation) {",
        "    for (uint32_t index = 0;",
        "         index < sizeof(xr_program_xi_projection_rows) / sizeof(xr_program_xi_projection_rows[0]);",
        "         ++index) {",
        "        if (xr_program_xi_projection_rows[index].xi_operation == xi_operation)",
        "            return true;",
        "    }",
        "    return xi_operation == XI_PARAM || xi_operation == XI_PHI;",
        "}",
        "",
    ])
    return "\n".join(lines)


def generate_header(schema: dict[str, Any], digest: str) -> str:
    fmt = schema["format"]
    magic = bytes.fromhex(fmt["magic_hex"])
    lines = [
        "/* AUTO-GENERATED by programgen - DO NOT EDIT */",
        "/* Source: xisa/program/schema.json */",
        "#ifndef XR_PROGRAM_SCHEMA_GEN_H",
        "#define XR_PROGRAM_SCHEMA_GEN_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define XR_PROGRAM_FORMAT_MAJOR {fmt['major']}u",
        f"#define XR_PROGRAM_FORMAT_MINOR {fmt['minor']}u",
        f"#define XR_PROGRAM_SCHEMA_SHA256 \"{digest}\"",
        f"#define XR_PROGRAM_ID_DOMAIN \"{fmt['program_id_domain']}\"",
        f"#define XR_PROGRAM_MAGIC_SIZE {len(magic)}u",
        "/* clang-format off */",
        "#define XR_PROGRAM_MAGIC_BYTES {" + ", ".join(f"UINT8_C(0x{byte:02x})" for byte in magic) + "}",
        "/* clang-format on */",
        "",
        "typedef enum XrProgramSectionId {",
    ]
    for section in schema["sections"]:
        lines.append(
            f"    XR_PROGRAM_SECTION_{c_identifier(section['name'])} = {section['stable_id']},")
    lines.extend([
        "} XrProgramSectionId;",
        "",
        f"#define XR_PROGRAM_REQUIRED_SECTION_COUNT {sum(1 for row in schema['sections'] if row['required'])}u",
        f"#define XR_PROGRAM_SECTION_COUNT {len(schema['sections'])}u",
        "",
    ])
    for name, value in schema["limits"].items():
        lines.append(f"#define XR_PROGRAM_LIMIT_{c_identifier(name)} UINT32_C({value})")
    lines.extend(["", "#endif /* XR_PROGRAM_SCHEMA_GEN_H */", ""])
    return "\n".join(lines)


def generate_spec(schema: dict[str, Any], digest: str) -> str:
    lines = [
        "# XrProgram format v1",
        "",
        "> AUTO-GENERATED by `tools/programgen/programgen.py`; do not edit.",
        f"> Schema SHA-256: `{digest}`.",
        "",
        "The header carries the format version, CoreSpec epoch/fingerprint, semantic-profile fingerprint, required feature IDs, and a canonical section directory. Directory offsets are minimal ULEB128 values relative to the payload start, so directory size is not self-referential.",
        "",
        "| ID | Section | Required |",
        "|---:|---|---|",
    ]
    for section in schema["sections"]:
        lines.append(f"| {section['stable_id']} | `{section['name']}` | {'yes' if section['required'] else 'no'} |")
    type_system = schema["type_system"]
    value_system = schema["value_system"]
    lines.extend([
        "",
        "## Logical type rows",
        "",
        "The type section starts with the five fixed builtin rows, followed by dynamic rows whose IDs start at `16`. Dynamic rows are sorted by semantic key and encode only logical declaration order.",
        "",
        f"- Builtins: `{', '.join(type_system['builtin_rows'])}`",
        f"- Dynamic kinds: `{', '.join(type_system['dynamic_kinds'])}`",
        f"- Aggregate shape: `{type_system['aggregate_shape']}`",
        f"- Variant shape: `{type_system['variant_shape']}`",
        f"- Recursive value shape: `{type_system['recursive_value_shape']}`",
        f"- Physical layout: `{type_system['physical_layout']}`",
        "",
        "Offsets, alignment, register classes, VM slots, native C layout, and target ABI facts are not encodable in XrProgram. Executors derive private realizations after profile binding.",
        "",
        "## Function and value contracts",
        "",
        f"- Function parameters: `{value_system['function_parameter_contract']}`",
        f"- Parameter modes: `{', '.join(value_system['parameter_modes'])}`",
        f"- Value categories: `{', '.join(value_system['value_categories'])}`",
        f"- Place physical layout: `{value_system['place_physical_layout']}`",
        "",
        "A `place` is a verifier-confined SSA capability with a pointee `TypeId`; it is not a language type and never enters the type table. `REF` entry arguments and call operands are places, while `READ` and `MOVE` use values.",
    ])
    lines.extend(["", "## Resource ceilings", ""])
    for name, value in schema["limits"].items():
        lines.append(f"- `{name}`: `{value}`")
    lines.append("")
    return "\n".join(lines)


def generate_coverage(schema: dict[str, Any], digest: str) -> str:
    value = {
        "schema": "xray-program-format-coverage/1",
        "format": "1.0",
        "schema_sha256": digest,
        "product_status": "OFF_PRODUCT_UNTIL_TASK_302",
        "structural_decoder": "COMPLETE",
        "semantic_admission": "COMPLETE_TASK_297",
        "vm_consumer": "COMPLETE_TASK_299",
        "aot_consumer": "COMPLETE_TASK_300",
        "sections": [
            {
                "stable_id": section["stable_id"],
                "name": section["name"],
                "required": section["required"],
                "w2_writer": "COMPLETE" if section["required"] else "NOT_YET_EMITTED",
                "w2_decoder": "ZERO_ROW_ONLY" if section["name"] in {
                    "imports", "boundaries", "semantic-metadata"
                } else ("OPTIONAL_DIGEST_ONLY" if not section["required"] else "COMPLETE"),
            }
            for section in schema["sections"]
        ],
    }
    return canonical_json(value)


def outputs(schema: dict[str, Any], projection: dict[str, Any], core: dict[str, Any],
            xi_source: str) -> dict[Path, str]:
    validate(schema)
    validate_source_projection(projection, core, xi_source)
    digest = hashlib.sha256(canonical_json(schema).encode("utf-8")).hexdigest()
    return {
        HEADER_PATH: generate_header(schema, digest),
        SOURCE_PROJECTION_HEADER_PATH: generate_source_projection_header(),
        SOURCE_PROJECTION_SOURCE_PATH: generate_source_projection_source(projection, core),
        SPEC_PATH: generate_spec(schema, digest),
        COVERAGE_PATH: generate_coverage(schema, digest),
    }


def self_test(schema: dict[str, Any], projection: dict[str, Any], core: dict[str, Any],
              xi_source: str) -> None:
    require(outputs(schema, projection, core, xi_source)
            == outputs(copy.deepcopy(schema), copy.deepcopy(projection),
                       copy.deepcopy(core), xi_source),
            "program schema generation is nondeterministic")
    mutations: list[tuple[str, dict[str, Any]]] = []
    mutation = copy.deepcopy(schema)
    mutation["sections"][1]["stable_id"] = mutation["sections"][0]["stable_id"]
    mutations.append(("duplicate section id", mutation))
    mutation = copy.deepcopy(schema)
    mutation["encoding"]["variable_integers"] = "host-sized"
    mutations.append(("host-sized integer policy", mutation))
    mutation = copy.deepcopy(schema)
    mutation["sections"][-1]["required"] = True
    mutations.append(("debug sidecar made required", mutation))
    mutation = copy.deepcopy(schema)
    mutation["limits"]["successors_per_operation"] = 1024
    mutations.append(("unbounded successor policy", mutation))
    for label, value in mutations:
        try:
            outputs(value, projection, core, xi_source)
        except ProgramSchemaError:
            continue
        raise ProgramSchemaError(f"self-test mutation was accepted: {label}")
    mutation = copy.deepcopy(projection)
    mutation["value_mappings"][0]["core_operation"] = "core.missing"
    try:
        outputs(schema, mutation, core, xi_source)
    except ProgramSchemaError:
        return
    raise ProgramSchemaError("self-test mutation was accepted: unknown CoreSpec projection")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--generate", action="store_true")
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        schema = read_schema(root / SCHEMA_PATH)
        projection = read_json(root / SOURCE_PROJECTION_PATH, "Xi source projection")
        core = read_json(root / CORE_REGISTRY_PATH, "CoreSpec registry")
        xi_source = (root / XI_REGISTRY_PATH).read_text(encoding="utf-8", errors="strict")
        generated = outputs(schema, projection, core, xi_source)
        if args.generate:
            for relative, content in generated.items():
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                if not path.exists() or path.read_text(encoding="utf-8") != content:
                    path.write_text(content, encoding="utf-8")
                    print(f"programgen: generated {relative}")
        elif args.check:
            for relative, expected in generated.items():
                path = root / relative
                require(path.is_file(), f"generated output is missing: {relative}")
                require(path.read_text(encoding="utf-8", errors="strict") == expected,
                        f"generated output is stale: {relative}; run programgen.py --generate")
            print(f"XrProgram schema: PASS (sha256={hashlib.sha256(canonical_json(schema).encode()).hexdigest()})")
        else:
            self_test(schema, projection, core, xi_source)
            print("XrProgram schema self-test: PASS")
    except (OSError, UnicodeError, ProgramSchemaError) as exc:
        print(f"programgen: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

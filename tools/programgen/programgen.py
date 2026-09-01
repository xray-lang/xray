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
HEADER_PATH = Path("src/program/xr_program_schema_gen.h")
SPEC_PATH = Path("contracts/canonical-program/xrprogram-format-v1.md")
COVERAGE_PATH = Path("contracts/canonical-program/xrprogram-format-coverage.json")
TOP_KEYS = {"schema", "format", "encoding", "sections", "limits"}
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
        "semantic_admission": "NOT_YET_ACTIVE_TASK_297",
        "vm_consumer": "NOT_YET_ACTIVE_TASK_299",
        "aot_consumer": "NOT_YET_ACTIVE_TASK_300",
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


def outputs(schema: dict[str, Any]) -> dict[Path, str]:
    validate(schema)
    digest = hashlib.sha256(canonical_json(schema).encode("utf-8")).hexdigest()
    return {
        HEADER_PATH: generate_header(schema, digest),
        SPEC_PATH: generate_spec(schema, digest),
        COVERAGE_PATH: generate_coverage(schema, digest),
    }


def self_test(schema: dict[str, Any]) -> None:
    require(outputs(schema) == outputs(copy.deepcopy(schema)), "program schema generation is nondeterministic")
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
            outputs(value)
        except ProgramSchemaError:
            continue
        raise ProgramSchemaError(f"self-test mutation was accepted: {label}")


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
        generated = outputs(schema)
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
            self_test(schema)
            print("XrProgram schema self-test: PASS")
    except (OSError, UnicodeError, ProgramSchemaError) as exc:
        print(f"programgen: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

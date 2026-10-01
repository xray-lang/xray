"""Independently derive fixed semantic and Program identity vectors.

This tool hashes explicit field frames and literal legacy wire. It neither
loads compiler libraries nor reads hashes printed by the implementation.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import re
import struct
from pathlib import Path


def digest(data: bytes) -> bytes:
    return hashlib.sha256(data).digest()


def u32(value: int) -> bytes:
    return struct.pack("<I", value)


def u64(value: int) -> bytes:
    return struct.pack("<Q", value)


def frame(value: bytes) -> bytes:
    return u64(len(value)) + value


def source_begin(domain: str) -> bytes:
    return frame(domain.encode()) + u32(1)


def stable_id(key: str) -> bytes:
    return digest(b"xray-entity-id-v2\0" + frame(key.encode()))[:16]


def module(identity: str, source: str) -> dict[str, bytes]:
    authority = digest(source_begin("xray-source-module-authority-v1") + frame(identity.encode()))
    identifier = digest(source_begin("xray-source-module-identity-v1") + frame(authority))[:16]
    exports = digest(source_begin("xray-source-module-empty-exports-v1") + frame(identifier) + u32(0))
    return {"id": identifier, "authority": authority, "source": digest(source.encode()), "exports": exports}


def enum_values(text: str) -> dict[str, int]:
    text = text[text.index("    AST_LITERAL_INT"):]
    text = text[:text.index("} AstNodeType")]
    result: dict[str, int] = {}
    value = 0
    for match in re.finditer(r"^\s*(AST_[A-Z0-9_]+)(?:\s*=\s*(\d+))?\s*[,\n]", text, re.M):
        if match[2]:
            value = int(match[2])
        result[match[1]] = value
        value += 1
    return result


def fixed_array(text: str, name: str) -> bytes:
    body = re.search(r"\b" + re.escape(name) + r"\s*\[[^]]*\]\s*=\s*\{([^}]+)\}", text, re.S)
    if body is None:
        raise ValueError(f"missing fixed byte vector {name}")
    return bytes(int(token, 16) for token in re.findall(r"0x([0-9a-fA-F]{2})", body[1]))


def closure_vector(schema: int, enums: dict[str, int]) -> dict[str, str]:
    policy = digest(b"policy-v1")
    app = module("memory-module-v1:id=7:psc-app", "app-source")
    library = module("memory-module-v1:id=11:psc-library", "library-source")
    signature = digest(b"fn():i64")
    effect = digest(b"effect:no-suspend:no-throw")
    pair_declaration = stable_id("declaration:psc-app:Pair")
    pair_instance = stable_id("instance:psc-app:Pair<>")
    pair_shape = digest(b"Pair{left:i64,right:i64}")
    pair_owner = digest(b"Pair:inline-copy-drop-none")
    pair = digest(b"xray-program-semantic-type-v1\0" + u32(1) + policy + app["id"] +
                  pair_declaration + pair_instance + pair_shape + pair_owner)[:16]
    functions = []
    function_ids = []
    for owner, declaration, instance, locator, flags in (
        (app, "declaration:psc-app:main", "instance:psc-app:main<>", (enums["AST_FUNCTION_DECL"], 1, 1, 3, 2), 1),
        (library, "declaration:psc-library:helper", "instance:psc-library:helper<>", (enums["AST_FUNCTION_DECL"], 5, 1, 7, 2), 2),
    ):
        declaration_id = stable_id(declaration)
        instance_id = stable_id(instance)
        identifier = digest(b"xray-program-semantic-function-v1\0" + u32(1) + policy +
                            owner["id"] + declaration_id + instance_id + signature + effect + u64(1))[:16]
        function_ids.append(identifier)
        functions.append((identifier, identifier + owner["id"] + declaration_id + instance_id +
                          b"".join(u32(part) for part in locator) + signature + effect + bytes(16) +
                          u32(0) + u32(0) + u64(1) + bytes([flags])))
    entry, helper = function_ids
    locator = (enums["AST_CALL_EXPR"], 2, 5, 2, 20)
    callsite = digest(source_begin("xray-source-program-callsite-v2") + frame(app["source"]) +
                      frame(app["id"]) + frame(stable_id("declaration:psc-app:main")) +
                      b"".join(u32(part) for part in locator))[:16]
    contract = digest(b"direct-call:fn():i64:no-suspend")
    call = digest(b"xray-program-semantic-call-v2\0" + u32(2) + policy + callsite + entry +
                  helper + bytes(16) + contract)[:16]
    payload = b"xray-program-semantic-closure-v9\0" + u32(schema) + u32(1) + policy
    payload += b"".join(u32(count) for count in (2, 1, 1, 0, 2, 0, 1))
    for row in sorted((app, library), key=lambda row: row["id"]):
        payload += row["id"] + row["authority"] + row["source"] + row["exports"]
    payload += app["id"] + library["id"] + bytes(20) + bytes(48) + digest(b"app-to-library-contract") + u32(0)
    payload += pair + app["id"] + pair_declaration + pair_instance + bytes(20) + pair_shape + pair_owner + bytes(20)
    payload += b"".join(row[1] for row in sorted(functions))
    payload += call + callsite + b"".join(u32(part) for part in locator) + entry + helper + bytes(16) + contract
    fingerprint = digest(payload)
    generation = digest(b"xray-generation-closure-id-v1\0" + u32(schema) + fingerprint)[:16]
    return {"call": call.hex(), "entry": entry.hex(), "helper": helper.hex(),
            "fingerprint": fingerprint.hex(), "generation": generation.hex(),
            "framed_bytes": payload.hex()}


def program_id(wire: bytes) -> str:
    return digest(b"xray-program-id-v3\0" + wire).hex()


def uvar(value: int) -> bytes:
    result = bytearray()
    while value >= 128:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return bytes(result)


def numbers(*values: int) -> bytes:
    return b"".join(uvar(value) for value in values)


def core_key(value: str) -> bytes:
    return digest(b"xray-core-ir-semantic-key-v1\0" + value.encode())


def core_projection(root: Path) -> tuple[bytes, dict]:
    registry = json.loads((root / "xisa/core/registry.json").read_text(encoding="utf-8"))
    projected = copy.deepcopy(registry)
    for family in ("types", "effects", "capabilities", "traps"):
        for row in projected[family]:
            row.pop("description", None)
    for row in projected["retired_operation_ids"]:
        row.pop("reason", None)
    for row in projected["operations"]:
        row.pop("kat_validator", None)
        row.pop("coverage", None)
    encoded = (json.dumps(projected, ensure_ascii=False, indent=2) + "\n").encode()
    identifier = digest(encoded)
    assert identifier.hex() == "69075c19fb8d9b74075be1c9c48e9bbcf8b8cc09dbc4118b5efa3332b6b6776a"
    return identifier, projected


def builtin_types(builder: bool, legacy_base: bool = False) -> bytes:
    identifiers = [*range(11)] if legacy_base else [*range(11), *range(12, 20 + int(builder))]
    rows = []
    for identifier in identifiers:
        ownership = int(identifier in (5, 12, 20))
        copy_contract = 2 if identifier in (0, 5, 20) else int(identifier == 12)
        rows.append(numbers(identifier, identifier, ownership, copy_contract))
    return numbers(len(rows)) + b"".join(rows)


def wire(sections: list[bytes], minor: int, core: bytes, profile: bytes, major: int = 3) -> bytes:
    header = bytes.fromhex("585250524f470d0a") + struct.pack("<HH", major, minor)
    header += numbers(1) + core + profile + numbers(1, 1, 7)
    offset = 0
    for identifier, section in enumerate(sections, 1):
        header += numbers(identifier, offset, len(section))
        offset += len(section)
    return header + b"".join(sections)


def minimal_vm_wire(minor: int, core: bytes, builder: bool) -> bytes:
    # One nullary i64 function, one constant i64(42), const and return ops.
    signatures = numbers(1, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0)
    functions = signatures + numbers(1, 0, 0, 0, 1, 1, 1)
    code = numbers(1, 0, 1, 0, 0, 2)
    code += numbers(1, 1, 2, 0, 0, 0, 4, 0, 0)
    code += numbers(35, 0, 0, 0, 0, 1, 0, 0, 0)
    metadata = numbers(0, 0, 1, 0, 0, 0, 0, 0, 0)
    return wire([builtin_types(builder), numbers(1, 0, 2, 1, 84), functions,
                 code, numbers(0), numbers(0), metadata], minor, core,
                core_key("task-299:embedded:semantic-profile"))


def execution_wire(minor: int, core: bytes, builder: bool) -> bytes:
    signatures = numbers(1, 0, 0, 0, 0, 2, 0, 0, 0, 0, 69, 32)
    functions = signatures + numbers(1, 0, 0, 0, 1, 2, 1)
    code = numbers(1, 0, 1, 0, 0, 3)
    code += numbers(1, 1, 2, 0, 0, 0, 4, 0, 0)
    code += numbers(136, 2, 2, 0, 0, 1, 0, 10, 0, 0, 0)
    code += numbers(35, 0, 0, 0, 0, 1, 1, 0, 0)
    # Logical v1: reads-clock, all platforms, hosted, in i64 -> i64, no error.
    logical = u32(1) + u32(8) + u32(31) + bytes([1, 1, 3, 0, 1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 1])
    imports = numbers(1) + bytes(range(202, 218)) + numbers(1)
    imports += bytes(range(21, 37)) + numbers(len(logical)) + logical
    metadata = numbers(0, 0, 1, 0, 0, 0, 0, 0, 0)
    return wire([builtin_types(builder), numbers(1, 0, 2, 1, 84), functions, code,
                 imports, numbers(0), metadata], minor, core, core_key("execution:semantic-profile"))


def boundary_wire(minor: int, core: bytes, builder: bool, major: int = 3) -> tuple[bytes, dict[str, int]]:
    legacy = major == 1
    rows = sorted([(core_key("boundary:type:aggregate"), "aggregate"),
                   (core_key("boundary:type:variant"), "variant")])
    dynamic_base = 16 if legacy else 32
    identifiers = {name: dynamic_base + index for index, (_, name) in enumerate(rows)}
    type_base = builtin_types(builder, legacy)
    types = numbers(type_base[0] + 2) + type_base[1:]
    for key, name in rows:
        types += numbers(identifiers[name], dynamic_base + int(name == "variant"), 0, 0) + key
        if name == "aggregate":
            types += numbers(0, 3, 1, 2, 3)
        else:
            types += numbers(0, 3, 0, 2, 1, 2, 1, 3)
        if minor == 11:
            types += numbers(0) * (1 if name == "aggregate" else 4)
    parameters = [identifiers["aggregate"], identifiers["variant"], 6]
    signatures = numbers(1, 0, 3) + b"".join(numbers(parameter, 0) for parameter in parameters)
    signatures += numbers(0, 0, 6, 0, 0, 0, 0, 1, 0)
    functions = signatures + numbers(1, 0, 0, 0, 1, 3, 1)
    code = numbers(1, 0, 1, 0, 3)
    code += b"".join(numbers(index, parameter, 0, 0) for index, parameter in enumerate(parameters))
    code += numbers(1, 35, 0, 0, 0, 0, 1, 2, 0, 0)
    metadata = numbers(0, 0, 1, 0, 0, 0, 0, 0) + (b"" if legacy else numbers(0))
    return wire([types, numbers(0), functions, code, numbers(0), numbers(0), metadata],
                minor, core, core_key("boundary:semantic-profile"), major), identifiers


PROFILE_IDENTITIES = {
    "native": ("378857327ac945ad3254e6ed808095ae2344aa860d038818e3d3d0354cbdf96e",
               "1f77211f058a7a6464d002a86dc4121160f3fcedfa12b0dbee47e6bdbd5d7464",
               "b4154c1d6fdfc6e54c2ae30b6873e11ec308b1cbf4148ef7428fed0c05f4ec55"),
    "foreign": ("091656d302eea37d9a09a5e19569841744dd795602948ad47c68f686676702d7",
                "afccc033804cac95af572d15ba7735271bab46e47cc53cb14256d32d130ad0ed",
                "1fe7176670e77c0a0fa15ad3ca430320eff7e15a0854a3a76e6b16f67a69ad11"),
    "legacy-native": ("745631840895bfe714599f8686cf82d549da59c68b07a2bb9c5cc8859b96bb84",
                      "1f77211f058a7a6464d002a86dc4121160f3fcedfa12b0dbee47e6bdbd5d7464",
                      "0321f092cacc6245be6ac87aed998c66d50dc9b095d9206efd6ae1380e9597d6"),
    "legacy-foreign": ("d18add7bfa26da93088cc72d1caeeba28dfb9a3facc362a4a0577d7ae4695b1d",
                       "afccc033804cac95af572d15ba7735271bab46e47cc53cb14256d32d130ad0ed",
                       "06f4d3f767462fa702cc5b84540d5845c362b64791aab8c0eb1ec1e820f7259f"),
}


def execution_id(identifier: str, identities: tuple[str, str, str]) -> str:
    return digest(b"xray-execution-id-v1\0" + bytes.fromhex(identifier) +
                  b"".join(bytes.fromhex(part) for part in identities)).hex()


def layout_ids(identifier: str, types: dict[str, int], profile: str) -> dict[str, str]:
    identities = PROFILE_IDENTITIES[profile]
    prefix = bytes.fromhex(identifier) + bytes.fromhex(identities[1])

    def fields(*values: int) -> bytes:
        return b"".join(u64(value) for value in values)

    def begin(type_id: int, kind: int) -> bytes:
        return b"xray-boundary-type-layout-v1\0" + prefix + fields(1, type_id, kind)

    def scalar(type_id: int, representation: int, size: int, alignment: int) -> bytes:
        return digest(begin(type_id, 2) + fields(representation, 1, size, alignment))

    scalar_rows = {1: (1, 1, 1), 2: (2, 8, 8), 3: (3, 4, 4), 6: (3, 2, 2)}
    scalars = {type_id: scalar(type_id, *row) for type_id, row in scalar_rows.items()}
    aggregate = begin(types["aggregate"], 3) + fields(3)
    for ordinal, (type_id, offset) in enumerate(((1, 0), (2, 8), (3, 16))):
        _, size, alignment = scalar_rows[type_id]
        aggregate += fields(ordinal, type_id, offset, size, alignment) + scalars[type_id]
    aggregate = digest(aggregate + fields(24, 8))
    variant = begin(types["variant"], 4) + fields(3, 0, 4, 8, 3)
    for ordinal, row in enumerate(((), ((1, 8), (2, 16)), ((3, 8),))):
        size, alignment = ((0, 1), (16, 8), (4, 4))[ordinal]
        variant += fields(ordinal, len(row), size, alignment)
        for field, (type_id, offset) in enumerate(row):
            _, size, alignment = scalar_rows[type_id]
            variant += fields(field, type_id, offset, size, alignment) + scalars[type_id]
    variant = digest(variant + fields(24, 8))
    call = b"xray-boundary-call-layout-v1\0" + bytes.fromhex(execution_id(identifier, identities))
    call += bytes.fromhex(identities[1]) + fields(1, 0, 1, 1, 3, 56, 8)
    for type_id, offset, size, alignment, child in (
        (types["aggregate"], 0, 24, 8, aggregate),
        (types["variant"], 24, 24, 8, variant), (6, 48, 2, 2, scalars[6])
    ):
        call += fields(type_id, 1, 1, offset, size, alignment) + child
    call += fields(6, 2, 1, 2, 2) + scalars[6]
    return {"u16": scalars[6].hex(), "aggregate": aggregate.hex(), "variant": variant.hex(),
            "call": digest(call).hex()}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--write-vm-fixture", action="store_true")
    parser.add_argument("--write-wire-fixtures", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    schema = json.loads((root / "xisa/program/schema.json").read_text(encoding="utf-8"))
    assert schema["format"] == {"major": 3, "minor": 11, "magic_hex": "585250524f470d0a",
                               "program_id_domain": "xray-program-id-v3"}
    assert schema["type_system"]["builtin_rows"] == [
        "0:void", "1:bool", "2:i64", "3:u32", "4:error", "5:panic-info", "6:u16",
        "7:TargetOs", "8:TargetArch", "9:TargetAbi", "10:TargetEndian", "12:string",
        "13:rune", "14:i8", "15:u8", "16:i16", "17:i32", "18:u64", "19:f64", "20:string-builder"]
    enums = enum_values((root / "src/frontend/parser/xast_types.h").read_text(encoding="utf-8"))
    old = closure_vector(9, enums)
    assert old["call"] == "19fd962d3ab6171421b6c2f58a246087"
    assert old["entry"] == "c67e74497fd36d81750ccc8fc b448ee7".replace(" ", "")
    assert old["helper"] == "2e0e44b93b0e6ae1c76d377a0b973a1a"
    assert old["fingerprint"] == "75321eaa494f8544857938eb69ed00be583296f4800ecc4a06231f8b2c4a37a1"
    assert old["generation"] == "a85cbf5f7d470356849942f3361a5fb9"
    print("PASS legacy closure v9: call, entry, helper, all framed rows, fingerprint, generation")
    program_text = (root / "tests/unit/program/test_xr_program.c").read_text(encoding="utf-8")
    old_program = bytearray(fixed_array(program_text, "builder_vector"))
    old_program[13:45] = fixed_array(program_text, "byte_compare_core")
    assert program_id(old_program) == "1bc457be305a4ce40fd7c37814e57f06ea46f4333d1fc2d41738014c6476f411"
    print("PASS legacy Program byte-compare wire: fixed complete literal and ProgramId")
    current_program = bytearray(old_program)
    current_program[13:45] = bytes.fromhex("69075c19fb8d9b74075be1c9c48e9bbcf8b8cc09dbc4118b5efa3332b6b6776a")
    current_core, projected_core = core_projection(root)
    vm_text = (root / "tests/unit/vm/xr_program_vm_embedded_fixture.h").read_text(encoding="utf-8")
    legacy_name = "xr_program_vm_embedded_fixture_v3_9"
    if legacy_name not in vm_text:
        legacy_name = "xr_program_vm_embedded_fixture"
    vm_legacy = fixed_array(vm_text, legacy_name)
    assert minimal_vm_wire(9, vm_legacy[13:45], False) == vm_legacy
    print("PASS legacy VM format 3.9: all 237 literal wire bytes reproduced independently")
    execution_legacy = execution_wire(9, vm_legacy[13:45], False)
    assert program_id(execution_legacy) == "3cbcea9370fb5d06fc10aa641a58dedf1980b991060a830c952d3b2cfd355076"
    print("PASS legacy execution format 3.9: provider signature, imports, logical contract, ProgramId")
    result = {"closure9": old, "closure10": closure_vector(10, enums),
              "ast_function_decl": enums["AST_FUNCTION_DECL"], "ast_call_expr": enums["AST_CALL_EXPR"],
              "program_byte_compare_id": program_id(old_program),
              "program_current_id": program_id(current_program),
              "program_current_wire": current_program.hex(),
              "core_projection": projected_core,
              "vm_legacy_id": program_id(vm_legacy),
              "vm_current_id": program_id(minimal_vm_wire(11, current_core, True)),
              "vm_current_wire": minimal_vm_wire(11, current_core, True).hex()}
    result["execution_legacy_id"] = program_id(execution_legacy)
    result["execution_current_id"] = program_id(execution_wire(11, current_core, True))
    result["execution_legacy_wire"] = execution_legacy.hex()
    result["execution_current_wire"] = execution_wire(11, current_core, True).hex()
    boundary_current, boundary_types = boundary_wire(11, current_core, True)
    boundary_legacy, boundary_old_types = boundary_wire(0, bytes.fromhex("229dbce42ce7276fa1052fd042d48abae285a4bb589819b85837be22e8754312"), False, 1)
    result["boundary_current_id"] = program_id(boundary_current)
    result["boundary_current_wire"] = boundary_current.hex()
    result["boundary_types"] = boundary_types
    result["boundary_legacy_id"] = digest(b"xray-program-id-v1\0" + boundary_legacy).hex()
    result["boundary_legacy_wire"] = boundary_legacy.hex()
    boundary_old_expected = {
        "native": ["936b366c0f1cc567edae7fd50028cbf409463a60f1f4dbb1e7e65766ddbb2b1c",
                   "d1d9fa6ccc28edfadb7acb59c84548b19f6509f1e4646e4dad1b64d74582d3d8",
                   "9c85e2ca71233c9ef5876ceb1811463facd825b9cd21d17cbc865c5fe601adf1",
                   "19e294b70dbe7cd9963f4cf1c8d68f879ca734739ebce81b746a5967011df8d9"],
        "foreign": ["e89cdc3a3cec8bd1ec34bddceac088fe0f0e90aa945224a75d32961d7880f673",
                    "b98225baa3c9a5de1d7b5b96cd88e23ebf1d4073e34628700d4478118a6be067",
                    "7b6fa36b491c9a0568d743c020bbcbed4f76142689f85e014c53e73fcd83966a",
                    "dbb6ad70e23a6c026d920a1d1b28d6ee290a4143186d9714a28111bba76d4a8b"],
    }
    for profile in ("native", "foreign"):
        legacy_layout = layout_ids(result["boundary_legacy_id"], boundary_old_types, "legacy-" + profile)
        assert list(legacy_layout.values()) == boundary_old_expected[profile]
        for name, identifier in legacy_layout.items():
            print(f"PASS legacy boundary {profile} {name}: {identifier}")
        result["boundary_legacy_layout_" + profile] = legacy_layout
        result["boundary_current_layout_" + profile] = layout_ids(result["boundary_current_id"], boundary_types, profile)
        identities = PROFILE_IDENTITIES[profile]
        scalar_profile = ("2484cd1d386a6d2b8d98be381bf9effe558961efb05455afd1dfdd62e994df4a"
                          if profile == "native" else
                          "64f2f74782b9273b0f7d0863a94c5b2e8e4ef223dca48b38ff6cce04ff50ea66")
        scalar_identities = (scalar_profile, identities[1], identities[2])
        legacy_execution = execution_id(result["execution_legacy_id"], scalar_identities)
        assert legacy_execution == ("d37a9f35b874a861458d2837c8dff39c818a21982fa9323b8dad8e64116cd2bb"
                                    if profile == "native" else
                                    "44899dfb37d5562ddf9cc6cdb718a2d9090ce634c22cdf84be94c80a7ee27aec")
        print(f"PASS legacy ExecutionId {profile}: {legacy_execution}")
        result["execution_current_instance_" + profile] = execution_id(result["execution_current_id"], scalar_identities)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if args.write_wire_fixtures:
        header = """/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_identity_vectors.h - Independently framed canonical wire vectors
 *
 * KEY CONCEPT:
 *   Literal current wires pin every logical field and section offset.
 *   Obsolete wires remain rejection vectors for the strict decoder.
 */
#ifndef XR_PROGRAM_IDENTITY_VECTORS_H
#define XR_PROGRAM_IDENTITY_VECTORS_H

"""
        for name in ("execution_legacy", "execution_current", "boundary_legacy", "boundary_current"):
            payload = bytes.fromhex(result[name + "_wire"])
            header += f"static const unsigned char xr_identity_{name}_wire[] = {{\n"
            for offset in range(0, len(payload), 16):
                header += "    " + ", ".join(f"0x{value:02x}" for value in payload[offset:offset + 16]) + ",\n"
            header += "};\n\n"
        header += "#endif  // XR_PROGRAM_IDENTITY_VECTORS_H\n"
        (root / "tests/unit/program/xr_program_identity_vectors.h").write_text(header, encoding="utf-8")
    if args.write_vm_fixture:
        header = vm_text[:vm_text.index("static const unsigned char")]
        for name, payload in (("xr_program_vm_embedded_fixture_v3_9", vm_legacy),
                              ("xr_program_vm_embedded_fixture", minimal_vm_wire(11, current_core, True))):
            header += f"static const unsigned char {name}[] = {{\n"
            for offset in range(0, len(payload), 16):
                header += "    " + ", ".join(f"0x{value:02x}" for value in payload[offset:offset + 16]) + ",\n"
            header += "};\n"
        header += f"static const unsigned long xr_program_vm_embedded_fixture_size = {len(payload)}UL;\n\n"
        header += "#endif  // XR_PROGRAM_VM_EMBEDDED_FIXTURE_H\n"
        (root / "tests/unit/vm/xr_program_vm_embedded_fixture.h").write_text(header, encoding="utf-8")
    else:
        assert fixed_array(vm_text, "xr_program_vm_embedded_fixture") == minimal_vm_wire(11, current_core, True)
        current_vm = minimal_vm_wire(11, current_core, True)
        assert len(current_vm) == 244
        size = re.search(r"xr_program_vm_embedded_fixture_size\s*=\s*(\d+)UL", vm_text)
        assert size and int(size.group(1)) == len(current_vm)
    fixture = (root / "tests/unit/program/xr_program_identity_vectors.h").read_text(encoding="utf-8")
    for name in ("execution_legacy", "execution_current", "boundary_legacy", "boundary_current"):
        assert fixed_array(fixture, "xr_identity_" + name + "_wire").hex() == result[name + "_wire"]
    print("PASS current fixed C wire fixtures: every byte matches independent field encoder")
    print(json.dumps({key: value for key, value in result.items() if not isinstance(value, dict) and not key.endswith("wire")}, indent=2))


if __name__ == "__main__":
    main()

"""Generate typed provider identities and canonical logical contract bytes."""

from __future__ import annotations

import hashlib
import json
import re
import struct
from typing import Sequence

from provider_declarations import ProviderDeclaration, admission_reasons
from provider_types import logical_type, native_resource_id


ADAPTERS = {
    "typed": "XR_STDLIB_PROVIDER_TYPED",
    "i64-nullary-u64": "XR_STDLIB_PROVIDER_I64_NULLARY_U64",
    "i64-nullary-i64": "XR_STDLIB_PROVIDER_I64_NULLARY_I64",
    "i64-unary-status-out": "XR_STDLIB_PROVIDER_I64_UNARY_STATUS_OUT",
    "optional-i64-pair-pipe-create": "XR_STDLIB_PROVIDER_OPTIONAL_I64_PAIR_PIPE_CREATE",
    "bool-i64-pipe-close": "XR_STDLIB_PROVIDER_BOOL_I64_PIPE_CLOSE",
}
EFFECTS = {"none": 0, "reads-clock": 8, "reads-process": 16,
           "reads-environment": 32, "io": 64,
           "managed-allocation": 128, "managed-deallocation": 256}
PLATFORMS = {"linux": 1, "macos": 2, "windows": 4}
HOST_PROTOTYPES = {
    "typed": "XrProviderCallStatus (*)(void *, const XrProviderValuePack *, XrProviderValuePack *)",
    "i64-nullary-u64": "uint64_t (*)(void)",
    "i64-nullary-i64": "int64_t (*)(void)",
    "i64-unary-status-out": "bool (*)(int64_t, int64_t *)",
    "bool-i64-pipe-close": "int (*)(XrPipeHandle)",
    "optional-i64-pair-pipe-create": "int (*)(XrPipe *, const XrPipeOptions *)",
}


def stable_id(key: str) -> bytes:
    encoded = key.encode("utf-8")
    return hashlib.sha256(b"xray-entity-id-v2\0" + struct.pack("<Q", len(encoded))
                          + encoded).digest()[:16]


def logical_bytes(declaration: ProviderDeclaration) -> bytes:
    reasons = admission_reasons(declaration)
    if reasons:
        raise ValueError("provider declaration is not admitted: " + ", ".join(reasons))
    logical = declaration.logical
    types = b"".join(logical_type(parameter.type)[0] for parameter in logical.parameters)
    types += logical_type(logical.result_type)[0] + b"\x01"  # The typed error result is unit.
    effects = sum(EFFECTS[effect] for effect in logical.effects)
    platforms = sum(PLATFORMS[platform] for platform in logical.platforms)
    result = bytearray(struct.pack("<III10B", 1, effects, platforms, 1,
                                  len(logical.parameters), len(types), len(logical.resources),
                                  4 if logical.result_owner == "owned" else 1, 1, 1,
                                  2 if logical.reentry == "forbidden" else 1, 1, 1))
    for parameter in logical.parameters:
        result.extend((2 if parameter.mode == "ref" else 1, 2 if parameter.owner == "borrow" else 1))
    result.extend(types)
    for resource in logical.resources:
        result.extend(stable_id(resource.resource))
        if resource.value.startswith("parameter."):
            result.extend((1, int(resource.value.split(".")[1]), 2, 1, 0))
        else:
            result.extend((2, 0, 1, 2, 2, 0, int(resource.value.split(".")[2])))
    return bytes(result)


def logical_fingerprint(declaration: ProviderDeclaration) -> bytes:
    return hashlib.sha256(b"xray-provider-logical-contract-v1\0"
                          + logical_bytes(declaration)).digest()


def admitted_entries(entries: Sequence[object]) -> list[object]:
    declared = [(entry.provider_declaration.contract, entry.provider_declaration.operation)
                for entry in entries if entry.provider_declaration]
    if len(declared) != len(set(declared)):
        raise ValueError("multiple leaves declare the same provider operation identity")
    result = [entry for entry in entries if entry.provider_declaration
              and not admission_reasons(entry.provider_declaration)]
    result.sort(key=lambda entry: (stable_id(entry.provider_declaration.contract),
                                    stable_id(entry.provider_declaration.operation)))
    identities = [(entry.provider_declaration.contract, entry.provider_declaration.operation)
                  for entry in result]
    encoded = [(stable_id(contract), stable_id(operation)) for contract, operation in identities]
    if len(encoded) != len(set(encoded)):
        raise ValueError("distinct provider operations have colliding stable identities")
    return result


def byte_initializer(value: bytes) -> str:
    return "{ " + ", ".join(f"0x{byte:02x}" for byte in value) + " }"


def header(filename: str, purpose: str) -> list[str]:
    return ["/*", " * xray - Lightweight typed scripting with native concurrency",
            " * https://www.xray-lang.org", " *",
            " * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>",
            " * Licensed under the MIT License", " *", f" * {filename} - {purpose}",
            " *", " * Generated from explicit provider declarations. Do not edit.", " */", ""]


def emit_provider_keys(entries: Sequence[object]) -> str:
    declarations = [entry.provider_declaration for entry in admitted_entries(entries)]
    lines = header("xr_stdlib_provider_keys_gen.h", "Provider identity spellings")
    lines += ["#ifndef XR_STDLIB_PROVIDER_KEYS_GEN_H", "#define XR_STDLIB_PROVIDER_KEYS_GEN_H", ""]
    for contract in sorted({declaration.contract for declaration in declarations}):
        name = contract.rsplit("/", 1)[1].upper().replace("-", "_")
        lines.append(f'#define XR_PROVIDER_{name}_CONTRACT_KEY "{contract}"')
    lines.append("")
    for declaration in sorted(declarations, key=lambda row: row.operation):
        name = declaration.operation.split("/", 1)[1].upper().replace("-", "_").replace("/", "_")
        lines.append(f'#define XR_PROVIDER_{name}_OPERATION_KEY "{declaration.operation}"')
    resources = set()
    for declaration in declarations:
        for text in [declaration.logical.result_type, *(p.type for p in declaration.logical.parameters)]:
            resources.update(re.findall(r"resource<([A-Za-z_][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)>", text))
    resource_macros = set()
    for module, name in sorted(resources):
        macro = "XR_PROVIDER_RESOURCE_" + module.upper() + "_" + name.upper() + "_ID"
        if macro in resource_macros:
            raise ValueError("distinct resource identities have colliding C macro names")
        resource_macros.add(macro)
        lines.append(f"#define {macro} {{ {byte_initializer(native_resource_id(module, name))} }}")
    lines += ["", "#endif  // XR_STDLIB_PROVIDER_KEYS_GEN_H", ""]
    return "\n".join(lines)


def emit_provider_descriptors(entries: Sequence[object]) -> str:
    selected = admitted_entries(entries)
    lines = header("xr_stdlib_provider_descriptors_gen.inc.c", "Explicit provider contracts")
    lines += ["/* clang-format off */", ""]
    for index, entry in enumerate(selected):
        encoded = logical_bytes(entry.provider_declaration)
        lines += [f"static const uint8_t xr_stdlib_provider_logical_{index}[] =",
                  f"    {byte_initializer(encoded)};", ""]
    lines.append("static const XrStdlibProviderDescriptor xr_stdlib_provider_descriptors[] = {")
    for index, entry in enumerate(selected):
        declaration = entry.provider_declaration
        lines += ["    {", f'        .symbol = "{entry.symbol}",',
                  f'        .contract_key = "{declaration.contract}",',
                  f'        .operation_key = "{declaration.operation}",',
                  f"        .contract_id = {{ {byte_initializer(stable_id(declaration.contract))} }},",
                  f"        .operation_id = {{ {byte_initializer(stable_id(declaration.operation))} }},",
                  f"        .logical_fingerprint = {{ {byte_initializer(logical_fingerprint(declaration))} }},",
                  f"        .logical_bytes = xr_stdlib_provider_logical_{index},",
                  f"        .logical_size = sizeof(xr_stdlib_provider_logical_{index}),",
                  f"        .adapter = {ADAPTERS[declaration.host.adapter]},",
                  f'        .host_header = "{declaration.host.header}",',
                  f'        .host_symbol = "{declaration.host.symbol}",', "    },"]
    if not selected:
        lines.append("    {0},")
    lines += ["};", f"#define XR_STDLIB_PROVIDER_DESCRIPTOR_COUNT {len(selected)}u", "",
              "/* clang-format on */", ""]
    return "\n".join(lines)


def host_adapter_body(declaration: ProviderDeclaration) -> tuple[str, str, list[str]]:
    host = declaration.host
    if host.adapter == "typed":
        return "TYPED", "const XrProviderValuePack *arguments, XrProviderValuePack *result", [
            f"    return {host.symbol}(context, arguments, result);"]
    if host.adapter in {"i64-nullary-u64", "i64-nullary-i64"}:
        body = ["    if (!result_out) return XR_PROVIDER_CALL_FAILED;"]
        if host.adapter == "i64-nullary-u64":
            body += [f"    uint64_t raw = {host.symbol}();",
                     "    *result_out = raw <= INT64_MAX ? (int64_t)raw :",
                     "                  -INT64_C(1) - (int64_t)(UINT64_MAX - raw);"]
        else:
            body += [f"    *result_out = {host.symbol}();"]
        body += ["    return XR_PROVIDER_CALL_OK;"]
        return "I64_NULLARY", "int64_t *result_out", body
    if host.adapter == "i64-unary-status-out":
        return "I64_UNARY", "int64_t argument, int64_t *result_out", [
            f"    return {host.symbol}(argument, result_out) ?",
            "           XR_PROVIDER_CALL_OK : XR_PROVIDER_CALL_FAILED;"]
    if host.adapter == "bool-i64-pipe-close":
        return "BOOL_I64_UNARY", "int64_t argument, bool *result_out", [
            "    if (!result_out) return XR_PROVIDER_CALL_FAILED;",
            "    XrPipeHandle handle = (XrPipeHandle)argument;",
            "    if ((int64_t)handle != argument) {",
            "        *result_out = false;",
            "        return XR_PROVIDER_CALL_OK;",
            "    }",
            f"    *result_out = {host.symbol}(handle) == 0;",
            "    return XR_PROVIDER_CALL_OK;"]
    if host.adapter == "optional-i64-pair-pipe-create":
        return ("OPTIONAL_I64_PAIR_NULLARY",
                "bool *present_out, int64_t *first_out, int64_t *second_out", [
                    "    if (!present_out || !first_out || !second_out)",
                    "        return XR_PROVIDER_CALL_FAILED;",
                    "    XrPipe pipe = {XR_PIPE_INVALID, XR_PIPE_INVALID};",
                    f"    bool present = {host.symbol}(&pipe, NULL) == 0;",
                    "    *present_out = present;",
                    "    *first_out = present ? (int64_t)pipe.read : 0;",
                    "    *second_out = present ? (int64_t)pipe.write : 0;",
                    "    return XR_PROVIDER_CALL_OK;"])
    raise ValueError("provider declaration has no admitted host adapter")


def emit_provider_bindings(entries: Sequence[object]) -> str:
    selected = admitted_entries(entries)
    lines = header("xr_stdlib_provider_bindings_gen.inc.c", "Typed host operation bindings")
    lines += [f'#include "{name}"' for name in sorted({entry.provider_declaration.host.header
                                                       for entry in selected})]
    lines += ["", "/* clang-format off */", ""]
    kinds = []
    for index, entry in enumerate(selected):
        declaration = entry.provider_declaration
        kind, parameters, body = host_adapter_body(declaration)
        kinds.append(kind)
        lines += [f"_Static_assert(_Generic(&{declaration.host.symbol},",
                  f"    {HOST_PROTOTYPES[declaration.host.adapter]}: 1, default: 0),",
                  '    "Provider host signature does not match its explicit adapter");',
                  f"static XrProviderCallStatus xr_stdlib_provider_call_{index}(",
                  f"    void *context, {parameters}) {{", "    (void)context;", *body, "}", ""]
    lines.append("static const XrProviderOperationBinding xr_stdlib_provider_bindings[] = {")
    for index, entry in enumerate(selected):
        lines += ["    {",
                  f"        .operation_id = {{ {byte_initializer(stable_id(entry.provider_declaration.operation))} }},",
                  f"        .trampoline_kind = XR_PROVIDER_TRAMPOLINE_{kinds[index]},",
                  f"        .entry.{kinds[index].lower()} = xr_stdlib_provider_call_{index},",
                  "    },"]
    if not selected:
        lines.append("    {0},")
    lines += ["};", f"#define XR_STDLIB_PROVIDER_BINDING_COUNT {len(selected)}u", "",
              "/* clang-format on */", ""]
    return "\n".join(lines)


def emit_provider_aot_sources(entries: Sequence[object]) -> str:
    """Project the same typed adapter bodies into standalone native C text."""
    selected = admitted_entries(entries)
    lines = header("xr_provider_aot_sources_gen.inc.c", "Declared native adapter source")
    lines += ["/* clang-format off */", "",
              "static const XrAotNativeProviderSource xr_aot_native_provider_sources[] = {"]
    for index, entry in enumerate(selected):
        declaration = entry.provider_declaration
        kind, parameters, body = host_adapter_body(declaration)
        definition = [
            f"_Static_assert(_Generic(&{declaration.host.symbol},",
            f"    {HOST_PROTOTYPES[declaration.host.adapter]}: 1, default: 0),",
            '    "Provider host signature does not match its explicit adapter");',
            f"static int xr_aot_native_provider_{index}(void *context, {parameters}) {{",
            "    (void)context;",
            *[line.replace("XR_PROVIDER_CALL_FAILED", "1").replace("XR_PROVIDER_CALL_OK", "0")
              for line in body],
            "}", "",
        ]
        lines += ["    {",
                  f"        .contract_id = {{ {byte_initializer(stable_id(declaration.contract))} }},",
                  f"        .operation_id = {{ {byte_initializer(stable_id(declaration.operation))} }},",
                  f"        .kind = XR_AOT_NATIVE_{kind},",
                  f"        .header = {json.dumps(declaration.host.header)},",
                  "        .definition ="]
        lines += ["            " + json.dumps(line + "\n") for line in definition]
        lines += ["        ,", "    },"]
    if not selected:
        lines.append("    {0},")
    lines += ["};", f"#define XR_AOT_NATIVE_PROVIDER_SOURCE_COUNT {len(selected)}u", "",
              "/* clang-format on */", ""]
    return "\n".join(lines)

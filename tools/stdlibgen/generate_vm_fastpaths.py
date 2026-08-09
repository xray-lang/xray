#!/usr/bin/env python3
"""Generate the source harness and VM adapters for stdlib native fastpaths.

The boundary manifest is the only registration source.  This generator never
copies an algorithm: it emits Xray adapters that call the canonical exported
stdlib functions, then the stage-1 compiler lowers those adapters to C.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import re
import sys
import tomllib
from pathlib import Path
from typing import Any


VALUE_TYPES = {
    "bool": ("bool", "uint8_t", "XR_IS_BOOL", "XR_TO_BOOL", "xr_bool"),
    "int": ("i64", "int64_t", "XR_IS_INT", "XR_TO_INT", "xr_int"),
    "float": ("f64", "double", "XR_IS_FLOAT", "XR_TO_FLOAT", "xr_float"),
    "string": ("string", "XrValue", "XR_IS_STRING", "", ""),
    "bool?": ("nullable-bool", "XrValue", "", "", ""),
    "int?": ("nullable-i64", "XrValue", "", "", ""),
    "float?": ("nullable-f64", "XrValue", "", "", ""),
    "string?": ("nullable-string", "XrValue", "", "", ""),
    "Array<byte>": ("array-u8", "XrValue", "XR_IS_ARRAY", "", ""),
    "Array<byte>?": ("nullable-array-u8", "XrValue", "", "", ""),
    "Array<float>": ("array-f64", "XrValue", "XR_IS_ARRAY", "", ""),
    "Array<string>": ("array-string", "XrValue", "XR_IS_ARRAY", "", ""),
    "Array<string>?": ("nullable-array-string", "XrValue", "", "", ""),
    "Slice<byte>": ("slice-u8", "XrValue", "XR_IS_ARRAY", "", ""),
    "()": ("unit", "void", "", "", "xr_null"),
}

for _integer_type in (
    "byte", "char", "rune", "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64",
):
    VALUE_TYPES[_integer_type] = (
        f"i64:{_integer_type}", "int64_t", "XR_IS_INT", "XR_TO_INT", "xr_int"
    )
    VALUE_TYPES[_integer_type + "?"] = (
        f"nullable-i64:{_integer_type}", "XrValue", "", "", ""
    )


def fail(message: str) -> None:
    raise ValueError(message)


def c_ident(value: str) -> str:
    ident = re.sub(r"[^A-Za-z0-9_]", "_", value)
    if not ident or ident[0].isdigit():
        ident = "_" + ident
    return ident


def xr_ident(module: str, member: str) -> str:
    words = re.findall(r"[A-Za-z0-9]+", module + "_" + member)
    return "vmFastpath" + "".join(word[:1].upper() + word[1:] for word in words)


def native_ident(module: str, member: str) -> str:
    snake = re.sub(r"(?<!^)(?=[A-Z])", "_", member).lower()
    return f"xr_generated_{c_ident(module).lower()}_{c_ident(snake).lower()}"


def harness_type(module: str, kind: str) -> str:
    del module
    return kind


def load_manifest_toml(root: Path, path: Path) -> dict[str, Any]:
    """Parse a repository TOML manifest."""
    with path.open("rb") as handle:
        return tomllib.load(handle)


def load_source_inventory(root: Path) -> list[dict[str, Any]]:
    inventory_path = root / "scripts" / "gen_api_inventory.py"
    spec = importlib.util.spec_from_file_location("xray_gen_api_inventory", inventory_path)
    if not spec or not spec.loader:
        fail(f"cannot load source inventory from {inventory_path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return list(module.collect_pure_stdlib(root))


def hosted_signature(
    signature: str, value_types: dict[str, tuple[str, str, str, str, str]]
) -> tuple[list[tuple[str, str, str | None]], str] | None:
    match = re.fullmatch(r"\((.*)\):\s*(.+)", signature.strip())
    if not match:
        return None
    raw_params, result = match.groups()
    if result not in value_types:
        return None
    params: list[tuple[str, str, str | None]] = []
    saw_default = False
    if raw_params.strip():
        raw_items = split_top_level(raw_params, ",")
        for index, raw in enumerate(raw_items):
            item = raw.strip()
            if item.startswith("..."):
                return None
            param = re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*:\s*(.+)", item)
            if not param:
                return None
            param_type, default = split_top_level_default(param.group(1).strip())
            if param_type not in value_types:
                return None
            if default is not None:
                saw_default = True
            elif saw_default:
                return None
            params.append((f"p{index}", param_type, default))
    return params, result


def split_top_level(value: str, delimiter: str) -> list[str]:
    """Split a source fragment without cutting nested types, tuples or literals."""
    items: list[str] = []
    depth = 0
    start = 0
    quote = ""
    escaped = False
    for offset, char in enumerate(value + delimiter):
        if quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = ""
            continue
        if char in {'"', "'"}:
            quote = char
        elif char in "<([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
        elif char == ">" and (offset == 0 or value[offset - 1] != "-"):
            depth -= 1
        elif char == delimiter and depth == 0:
            items.append(value[start:offset])
            start = offset + 1
    return items


def split_top_level_default(value: str) -> tuple[str, str | None]:
    parts = split_top_level(value, "=")
    if len(parts) == 1:
        return parts[0].strip(), None
    if len(parts) != 2:
        return value.strip(), None
    param_type = parts[0].strip()
    default = parts[1].strip()
    return (param_type, default) if param_type and default else (value.strip(), None)


def module_name_from_source(source: str) -> str:
    path = Path(source)
    return path.parent.name


def hosted_field_type(signature: str) -> str | None:
    match = re.fullmatch(r":\s*(.+?)(?:\s+\{.*)?", signature.strip())
    return match.group(1).strip() if match else None


def class_declares_constructor(text: str, class_name: str) -> bool:
    declaration = re.search(
        rf"(?m)^export\s+class\s+{re.escape(class_name)}(?:<[^>{{]+>)?\s*{{", text
    )
    if not declaration:
        return False
    depth = 1
    cursor = declaration.end()
    while cursor < len(text) and depth:
        depth += (text[cursor] == "{") - (text[cursor] == "}")
        cursor += 1
    body = text[declaration.end() : cursor - 1]
    return re.search(r"(?m)^\s*(?:private\s+)?constructor\s*\(", body) is not None


def hosted_value_types(
    root: Path, inventory: list[dict[str, Any]]
) -> tuple[
    dict[str, tuple[str, str, str, str, str]],
    dict[str, str],
    dict[str, dict[str, Any]],
]:
    value_types = dict(VALUE_TYPES)
    type_owners: dict[str, str] = {}
    for source in sorted((root / "stdlib").glob("*/*.xr")):
        text = source.read_text(encoding="utf-8")
        owner = source.parent.name
        for declaration in re.finditer(
            r"(?m)^export\s+enum\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{", text
        ):
            depth = 1
            end = declaration.end()
            while end < len(text) and depth:
                depth += (text[end] == "{") - (text[end] == "}")
                end += 1
            body = text[declaration.end() : end - 1]
            # First enum batch is nominal scalar/unit variants. Payload-bearing
            # recursive ADTs join the class/container batch once their nested
            # value view and ownership rules are available.
            if re.search(r"(?m)^\s*[A-Za-z_][A-Za-z0-9_]*\s*\(", body):
                continue
            name = declaration.group(1)
            value_types[name] = (f"enum:{name}", "XrValue", "", "", "")
            value_types[name + "?"] = (f"nullable-enum:{name}", "XrValue", "", "", "")
            type_owners[name] = owner

    class_infos: dict[str, dict[str, Any]] = {}
    members_by_class: dict[str, list[dict[str, Any]]] = {}
    for item in inventory:
        if item.get("category") != "stdlib-module":
            continue
        if item.get("kind") == "type":
            name = str(item["name"])
            source = str(item["source"])
            text = (root / source).read_text(encoding="utf-8")
            declaration = re.search(
                rf"(?m)^export\s+class\s+{re.escape(name)}"
                r"(?:\s*<([^>{}]+)>)?\s*{",
                text,
            )
            if not declaration:
                continue
            owner = str(item.get("doc_module") or module_name_from_source(source))
            if name in class_infos and class_infos[name]["module"] != owner:
                fail(f"hosted class name is ambiguous across modules: {name}")
            class_infos[name] = {
                "name": name,
                "module": owner,
                "source": source,
                "members": [],
                "has_constructor": class_declares_constructor(text, name),
                "generic_arity": (
                    len([part for part in declaration.group(1).split(",") if part.strip()])
                    if declaration.group(1)
                    else 0
                ),
            }
        else:
            members_by_class.setdefault(str(item.get("namespace", "")), []).append(item)

    # Class values cross the VM/AOT boundary as opaque nominal proxies, so the
    # proxy type itself does not depend on every member signature already being
    # in the same ABI batch.  Member roots are still emitted only when their
    # complete signature is supported; the terminal coverage gate compares
    # that generated set with the source inventory before bytecode removal.
    for name, info in class_infos.items():
        if info["generic_arity"]:
            continue
        owner = str(info["module"])
        value_types[name] = (f"object:{owner}:{name}", "XrValue", "", "", "")
        value_types[name + "?"] = (
            f"nullable-object:{owner}:{name}", "XrValue", "", "", ""
        )
    for name, info in class_infos.items():
        if info["generic_arity"]:
            continue
        info["members"] = sorted(
            members_by_class.get(name, []),
            key=lambda item: (int(item.get("line", 0)), str(item.get("kind", ""))),
        )
        type_owners[name] = str(info["module"])
    return value_types, type_owners, class_infos


def hosted_entry(
    *,
    value_types: dict[str, tuple[str, str, str, str, str]],
    type_owners: dict[str, str],
    module: str,
    member: str,
    source: str,
    params: list[tuple[str, str, str | None]],
    result: str,
    kind: str = "function",
    class_name: str = "",
) -> dict[str, Any]:
    arg_abi = ",".join(value_types[param_type][0] for _, param_type, _ in params) or "unit"
    kinds = [param_type for _, param_type, _ in params] + [result]
    has_array = any(
        value in {
            "Array<byte>", "Array<byte>?", "Array<float>",
            "Array<string>", "Array<string>?", "Slice<byte>",
        }
        for value in kinds
    )
    has_object = any(value_types[value][0].startswith(("object:", "nullable-object:"))
                     for value in kinds)
    batch = (
        "object_rc"
        if has_object
        else "mutable_rc"
        if has_array
        else "string_rc"
        if any(value in {"string", "string?"} for value in kinds)
        else "scalar"
    )
    logical = member if not class_name else f"{class_name}.{kind}.{member}"
    imports: list[tuple[str, str]] = []
    for value in kinds:
        base = value[:-1] if value.endswith("?") else value
        if base in VALUE_TYPES or base.startswith("Array<") or base.startswith("Slice<"):
            continue
        owner = type_owners.get(base)
        if owner and (owner, base) not in imports:
            imports.append((owner, base))
    owned_results = {
        "string", "string?", "Array<byte>", "Array<byte>?", "Array<float>",
        "Array<string>", "Array<string>?",
    }
    result_abi = value_types[result][0]
    ownership = (
        "owned-or-null"
        if result.endswith("?") and (result in owned_results or result_abi.startswith("nullable-object:"))
        else "owned"
        if result in owned_results or result_abi.startswith("object:")
        else "immediate-or-null"
        if result in {"bool?", "int?", "float?"}
        else "immediate"
    )
    return {
        "symbol": f"{module}.{logical}",
        "module": module,
        "member": member,
        "class_name": class_name,
        "kind": kind,
        "reference": f"{source}::{class_name + '.' if class_name else ''}{member}",
        "native": native_ident(module, logical),
        "abi": f"{arg_abi}->{result_abi}",
        "batch": batch,
        "params": params,
        "result": result,
        "ownership": ownership,
        "effect": "compiler-verified",
        "imports": imports,
    }


def hosted_bridge_params(entry: dict[str, Any]) -> list[tuple[str, str, str | None]]:
    """Return the generated wrapper ABI while preserving source-owned defaults.

    A default expression is never copied into C.  The adapter supplies the
    public argument count plus nullable placeholders, and the Xray wrapper
    performs an arity-specific source call so the canonical declaration owns
    evaluation of every default expression.
    """
    params = list(entry["params"])
    if not any(default is not None for _, _, default in params):
        return params
    bridge = [("_provided", "int", None)]
    for name, param_type, default in params:
        bridge_type = (
            param_type
            if default is None or param_type.endswith("?")
            else param_type + "?"
        )
        bridge.append((name, bridge_type, None))
    return bridge


def derive_hosted_entries(root: Path) -> list[dict[str, Any]]:
    entries: list[dict[str, Any]] = []
    inventory = load_source_inventory(root)
    value_types, type_owners, class_infos = hosted_value_types(root, inventory)
    for item in inventory:
        if item.get("category") != "stdlib-module" or item.get("kind") != "function":
            continue
        parsed = hosted_signature(str(item.get("signature", "")), value_types)
        if not parsed:
            continue
        params, result = parsed
        module = str(item["namespace"])
        member = str(item["name"])
        source = str(item["source"])
        entries.append(hosted_entry(
            value_types=value_types, type_owners=type_owners, module=module, member=member,
            source=source, params=params, result=result,
        ))

    for class_name, info in sorted(class_infos.items()):
        if info["generic_arity"]:
            continue
        module = str(info["module"])
        source = str(info["source"])
        emitted_constructor = False
        for item in info["members"]:
            item_kind = str(item.get("kind", ""))
            member = str(item.get("name", ""))
            if member.startswith("_"):
                continue
            if item_kind == "field":
                field_type = hosted_field_type(str(item.get("signature", "")))
                if not field_type or field_type not in value_types:
                    continue
                entries.append(hosted_entry(
                    value_types=value_types, type_owners=type_owners, module=module,
                    member=member, source=source, params=[("p0", class_name, None)],
                    result=field_type, kind="getter", class_name=class_name,
                ))
                if "{" not in str(item.get("signature", "")):
                    entries.append(hosted_entry(
                        value_types=value_types, type_owners=type_owners, module=module,
                        member=member, source=source,
                        params=[("p0", class_name, None), ("p1", field_type, None)],
                        result="()", kind="setter", class_name=class_name,
                    ))
                continue
            if item_kind not in {"method", "static-method"}:
                continue
            parsed = hosted_signature(str(item.get("signature", "")), value_types)
            if not parsed:
                continue
            params, result = parsed
            if member == "constructor":
                emitted_constructor = True
                entries.append(hosted_entry(
                    value_types=value_types, type_owners=type_owners, module=module,
                    member=member, source=source, params=params, result=class_name,
                    kind="constructor", class_name=class_name,
                ))
            elif item_kind == "static-method":
                entries.append(hosted_entry(
                    value_types=value_types, type_owners=type_owners, module=module,
                    member=member, source=source, params=params, result=result,
                    kind="static", class_name=class_name,
                ))
            else:
                receiver_params = [("p0", class_name, None)] + [
                    (f"p{index + 1}", param_type, default)
                    for index, (_name, param_type, default) in enumerate(params)
                ]
                entries.append(hosted_entry(
                    value_types=value_types, type_owners=type_owners, module=module,
                    member=member, source=source, params=receiver_params, result=result,
                    kind="method", class_name=class_name,
                ))
        if not info.get("has_constructor") and not emitted_constructor:
            entries.append(hosted_entry(
                value_types=value_types, type_owners=type_owners, module=module,
                member="constructor", source=source, params=[], result=class_name,
                kind="constructor", class_name=class_name,
            ))
    return sorted(entries, key=lambda item: (
        str(item["module"]), str(item.get("class_name", "")),
        str(item["kind"]), str(item["member"]),
    ))


def source_runtime_exports(root: Path) -> list[dict[str, str]]:
    """Return every callable/accessor that a VM stdlib module publishes.

    Type declarations themselves are compile-time namespace values.  Runtime
    coverage is measured over module functions plus public class methods and
    field accessors.  Private fields are implementation details and never
    become hosted entry roots.
    """
    exports: dict[str, dict[str, str]] = {}
    for item in load_source_inventory(root):
        if item.get("category") != "stdlib-module":
            continue
        kind = str(item.get("kind", ""))
        member = str(item.get("name", ""))
        source = str(item.get("source", ""))
        signature = str(item.get("signature", ""))
        if not member or not source:
            continue
        if kind == "function":
            module = str(item.get("namespace") or module_name_from_source(source))
            symbols = [(f"{module}.{member}", "function", "")]
        elif kind in {"method", "static-method", "field"}:
            if member.startswith("_"):
                continue
            module = str(item.get("doc_module") or module_name_from_source(source))
            class_name = str(item.get("namespace", ""))
            if not class_name:
                continue
            if kind == "field":
                symbols = [
                    (f"{module}.{class_name}.getter.{member}", "getter", class_name)
                ]
                if "{" not in signature:
                    symbols.append(
                        (f"{module}.{class_name}.setter.{member}", "setter", class_name)
                    )
            else:
                member_kind = (
                    "constructor"
                    if member == "constructor"
                    else "static"
                    if kind == "static-method"
                    else "method"
                )
                symbols = [
                    (
                        f"{module}.{class_name}.{member_kind}.{member}",
                        member_kind,
                        class_name,
                    )
                ]
        else:
            continue
        for symbol, member_kind, class_name in symbols:
            if symbol in exports:
                fail(f"duplicate source runtime export: {symbol}")
            exports[symbol] = {
                "symbol": symbol,
                "module": module,
                "class_name": class_name,
                "kind": member_kind,
                "member": member,
                "signature": signature,
                "source": source,
            }
    return [exports[symbol] for symbol in sorted(exports)]


def enforce_atomic_class_coverage(
    entries: list[dict[str, Any]], source_exports: list[dict[str, str]]
) -> tuple[list[dict[str, Any]], list[dict[str, str]]]:
    """Never publish a partial proxy class or a function that depends on one.

    A hosted object is only valid when its nominal proxy owns the complete
    public surface.  This is a transitive graph rule: if one method of class A
    crosses unsupported class B, A is incomplete too, and free functions that
    consume or return either class must remain on the source implementation.
    """
    generated = {str(entry["symbol"]): entry for entry in entries}
    missing = [row for row in source_exports if row["symbol"] not in generated]
    incomplete_classes = {
        (row["module"], row["class_name"])
        for row in missing
        if row["class_name"]
    }

    def object_dependencies(entry: dict[str, Any]) -> set[tuple[str, str]]:
        return {
            (match.group(1), match.group(2))
            for match in re.finditer(
                r"(?:nullable-)?object:([A-Za-z_][A-Za-z0-9_]*):"
                r"([A-Za-z_][A-Za-z0-9_]*)",
                str(entry.get("abi", "")),
            )
        }

    changed = True
    while changed:
        changed = False
        for entry in entries:
            class_key = (str(entry["module"]), str(entry.get("class_name", "")))
            if not class_key[1] or class_key in incomplete_classes:
                continue
            if object_dependencies(entry) & incomplete_classes:
                incomplete_classes.add(class_key)
                changed = True

    filtered: list[dict[str, Any]] = []
    removed: list[dict[str, str]] = []
    for entry in entries:
        class_key = (str(entry["module"]), str(entry.get("class_name", "")))
        incomplete_dependency = bool(object_dependencies(entry) & incomplete_classes)
        if (class_key[1] and class_key in incomplete_classes) or incomplete_dependency:
            removed.append(
                {
                    "symbol": str(entry["symbol"]),
                    "source": str(entry["reference"]).split("::", 1)[0],
                    "signature": str(entry["abi"]),
                    "reason": (
                        "incomplete-class-atomicity"
                        if class_key[1] and class_key in incomplete_classes
                        else "incomplete-object-dependency"
                    ),
                }
            )
        else:
            filtered.append(entry)
    unsupported = [
        {
            "symbol": row["symbol"],
            "source": row["source"],
            "signature": row["signature"],
            "reason": "unsupported-hosted-signature",
        }
        for row in missing
    ]
    unsupported.extend(removed)
    unsupported.sort(key=lambda row: row["symbol"])
    return filtered, unsupported


def load_entries(
    root: Path,
) -> tuple[
    int,
    list[dict[str, Any]],
    list[dict[str, str]],
    list[dict[str, str]],
    str,
]:
    manifest_path = root / "stdlib" / "stdlib_boundary.toml"
    raw_bytes = manifest_path.read_bytes()
    manifest = load_manifest_toml(root, manifest_path)
    object_abi = manifest.get("object_abi", {})
    version = int(object_abi.get("version", 0))
    if version <= 0:
        fail("stdlib_boundary.toml: object_abi.version must be positive")
    if object_abi.get("value_layout") != "include/xray_value_abi.h":
        fail("object_abi.value_layout must use include/xray_value_abi.h")
    if object_abi.get("entry_abi") != (
        "XrValue(XrHostedFragmentContext*,XrValue*,uint32_t;signal)"
    ):
        fail("object_abi.entry_abi is not the hosted VM CFunction ABI")
    if object_abi.get("unsupported_policy") != "fail_closed":
        fail("object_abi.unsupported_policy must be fail_closed")

    baselines = {str(item.get("symbol", "")): dict(item) for item in manifest.get("vm_fastpath", [])}
    entries, unsupported = enforce_atomic_class_coverage(
        derive_hosted_entries(root), source_runtime_exports(root)
    )
    derived_by_symbol = {str(entry["symbol"]): entry for entry in entries}
    deferred: list[dict[str, str]] = []
    deferred_symbols: set[str] = set()
    for item in manifest.get("hosted_fragment_deferred", []):
        symbol = str(item.get("symbol", ""))
        reason = str(item.get("reason", ""))
        if not symbol or not reason:
            fail("hosted_fragment_deferred rows require symbol and reason")
        if symbol in deferred_symbols:
            fail(f"duplicate hosted_fragment_deferred symbol: {symbol}")
        if symbol not in derived_by_symbol:
            fail(f"hosted_fragment_deferred symbol is not source-derived: {symbol}")
        deferred_symbols.add(symbol)
        deferred.append({"symbol": symbol, "reason": reason})
    entries = [entry for entry in entries if str(entry["symbol"]) not in deferred_symbols]
    seen_symbols: set[str] = set()
    seen_native: set[str] = set()
    for entry in entries:
        symbol = str(entry["symbol"])
        module = str(entry["module"])
        member = str(entry["member"])
        native = str(entry["native"])
        abi = str(entry["abi"])
        kind = str(entry.get("kind", "function"))
        class_name = str(entry.get("class_name", ""))
        if kind == "function" and symbol != f"{module}.{member}":
            fail(f"{symbol}: symbol must equal module.member")
        if symbol in seen_symbols or native in seen_native:
            fail(f"{symbol}: duplicate symbol or generated native name")
        seen_symbols.add(symbol)
        seen_native.add(native)
        if entry["batch"] not in {"scalar", "string_rc", "mutable_rc", "object_rc"}:
            fail(f"{symbol}: ABI {abi!r} has an unsupported hosted batch")
        reference_path, sep, reference_member = str(entry["reference"]).partition("::")
        expected_reference = member if not class_name else f"{class_name}.{member}"
        if not sep or reference_member != expected_reference:
            fail(f"{symbol}: reference must end in ::{expected_reference}")
        source_path = root / reference_path
        if not source_path.is_file():
            fail(f"{symbol}: reference source does not exist: {reference_path}")
        source = source_path.read_text(encoding="utf-8")
        if kind == "function" and not re.search(
            rf"\bexport\s+fn\s+{re.escape(member)}\s*\(", source
        ):
            fail(f"{symbol}: reference source has no exported function {member}")
        if kind != "function" and not re.search(
            rf"\bexport\s+class\s+{re.escape(class_name)}\b", source
        ):
            fail(f"{symbol}: reference source has no exported class {class_name}")
        if kind == "function" and symbol in baselines:
            baseline = baselines.pop(symbol)
            for key in ("module", "member", "reference", "native", "abi", "batch"):
                if key in baseline and baseline[key] != entry[key]:
                    fail(f"{symbol}: benchmark baseline {key} disagrees with source-derived entry")
            entry.update(baseline)

    if baselines:
        fail("benchmark baseline is not source-derived scalar export: " + ", ".join(sorted(baselines)))

    fingerprint = hashlib.sha256(raw_bytes)
    for source_path in sorted((root / "stdlib").glob("*/*.xr")):
        reference_path = source_path.relative_to(root).as_posix()
        fingerprint.update(reference_path.encode("utf-8"))
        fingerprint.update(source_path.read_bytes())
    return version, entries, deferred, unsupported, fingerprint.hexdigest()


def render_harness(entries: list[dict[str, Any]], fingerprint: str) -> tuple[str, str]:
    xr_lines = [
        "// AUTO-GENERATED - DO NOT EDIT.",
        f"// source-fingerprint: {fingerprint}",
        "// Algorithms remain in stdlib/<module>/<module>.xr.",
        "",
    ]
    for module in sorted({str(entry["module"]) for entry in entries}):
        xr_lines.append(f"import {module}")
    type_imports: dict[str, set[str]] = {}
    for entry in entries:
        for owner, name in entry.get("imports", []):
            type_imports.setdefault(str(owner), set()).add(str(name))
    for module in sorted(type_imports):
        names = ", ".join(sorted(type_imports[module]))
        xr_lines.append(f"import {{ {names} }} from {module}")
    xr_lines.append("")
    toml_lines = [
        "# AUTO-GENERATED - DO NOT EDIT.",
        f"# source-fingerprint: {fingerprint}",
        "[package]",
        'name = "xray-stdlib-vm-native-fastpaths"',
        'version = "1.0.0"',
        'license = "MIT"',
        'main = "main.xr"',
        "",
    ]
    for entry in entries:
        module = str(entry["module"])
        member = str(entry["member"])
        kind = str(entry.get("kind", "function"))
        class_name = str(entry.get("class_name", ""))
        wrapper = xr_ident(module, f"{class_name}_{kind}_{member}")
        params = list(entry["params"])
        result = str(entry["result"])
        bridge_params = hosted_bridge_params(entry)
        declaration = ", ".join(
            f"{name}: {harness_type(module, param_type)}"
            for name, param_type, _ in bridge_params
        )
        return_decl = "" if result == "()" else f" -> {harness_type(module, result)}"
        xr_lines.append(f"export fn {wrapper}({declaration}){return_decl} {{")
        if kind in {"method", "setter"}:
            xr_lines.append(f"    var target = {params[0][0]}")

        has_defaults = any(default is not None for _, _, default in params)
        receiver_count = 1 if kind in {"method", "getter", "setter"} else 0
        user_params = params[receiver_count:]
        required = sum(1 for _, _, default in user_params if default is None)
        arities = range(required, len(user_params) + 1) if has_defaults else [len(user_params)]
        for arity_index, arity in enumerate(arities):
            call_names: list[str] = []
            for name, param_type, default in user_params[:arity]:
                call_names.append(name + "!" if default is not None and not param_type.endswith("?") else name)
            call_args = ", ".join(call_names)
            if kind == "constructor":
                call = f"{class_name}({call_args})"
            elif kind == "method":
                call = f"target.{member}({call_args})"
            elif kind == "static":
                call = f"{class_name}.{member}({call_args})"
            elif kind == "getter":
                call = f"{params[0][0]}.{member}"
            elif kind == "setter":
                call = f"target.{member} = {params[1][0]}"
            else:
                call = f"{module}.{member}({call_args})"

            if has_defaults and arity_index < len(arities) - 1:
                xr_lines.append(f"    if (_provided == {arity}) {{")
                xr_lines.append(
                    f"        {call}" if result == "()" else f"        return {call}"
                )
                if result == "()":
                    xr_lines.append("        return")
                xr_lines.append("    }")
            else:
                xr_lines.append(f"    {call}" if result == "()" else f"    return {call}")
        xr_lines.extend(["}", ""])
        toml_lines.extend(
            [
                "[[export.c]]",
                f'xray = "{wrapper}"',
                f'symbol = "{entry["native"]}"',
                'visibility = "hidden"',
                'abi = "hosted-vm-v1"',
                "header = true",
                "",
            ]
        )
    xr_lines.extend(["fn main() {}", "main()", ""])
    return "\n".join(xr_lines), "\n".join(toml_lines)


def render_registry(version: int, entries: list[dict[str, Any]], fingerprint: str, target: str) -> str:
    lines = [
        "/* AUTO-GENERATED - DO NOT EDIT. */",
        f"/* source-fingerprint: {fingerprint} */",
        f"#define XR_STDLIB_VM_FASTPATH_GENERATED_ABI_VERSION UINT32_C({version})",
        f'#define XR_STDLIB_VM_FASTPATH_GENERATED_TARGET "{target}"',
        f'#define XR_STDLIB_VM_FASTPATH_GENERATED_FINGERPRINT "{fingerprint}"',
        "",
        "#ifndef XR_HOSTED_FRAGMENT_SUSPENDABILITY_DECLARED",
        '#error "include the generated hosted-fragment header before this registry"',
        "#endif",
        "",
        "#if defined(XR_STDLIB_VM_FASTPATH_EMIT_ADAPTERS)",
        "typedef struct XrVMRuntime XrVMRuntime;",
        "extern const XrHostedFragmentHostOps xr_hosted_fragment_host_ops;",
        "extern XrValue xr_hosted_fragment_handle_signal(",
        "    XrVMRuntime *, const char *, const XrHostedFragmentSignal *);",
        "extern XrCFuncResult xr_stdlib_vm_fastpath_handle_yieldable_signal(",
        "    XrVMRuntime *, const char *, const XrHostedFragmentSignal *,",
        "    XrContinuation, XrValue, XrValue *);",
        "extern const void *xr_hosted_fragment_runtime_ops(void);",
        "extern void *xr_hosted_fragment_current_coroutine(XrVMRuntime *);",
        "",
    ]
    for index, entry in enumerate(entries):
        native = c_ident(str(entry["native"]))
        adapter = f"xr_stdlib_vm_fastpath_adapter_{index}"
        symbol = str(entry["symbol"])
        params = list(entry["params"])
        bridge_params = hosted_bridge_params(entry)
        kind = str(entry.get("kind", "function"))
        has_receiver = kind in {"method", "getter", "setter"}
        user_params = params[1:] if has_receiver else params
        expected = len(user_params)
        required = sum(1 for _, _, default in user_params if default is None)
        has_defaults = required != expected
        sync_signature = (
            f"XRT_INTERNAL XrValue {adapter}(XrVMRuntime *isolate, XrValue self, "
            "XrValue *args, int nargs) {"
            if kind != "function"
            else f"XRT_INTERNAL XrValue {adapter}(XrVMRuntime *isolate, XrValue *args, int nargs) {{"
        )
        yield_signature = (
            f"XRT_INTERNAL XrCFuncResult {adapter}(XrVMRuntime *isolate, XrValue self, "
            "XrValue *args, int nargs, XrValue *result) {"
            if kind != "function"
            else f"XRT_INTERNAL XrCFuncResult {adapter}(XrVMRuntime *isolate, XrValue *args, "
                 "int nargs, XrValue *result) {"
        )
        suspend_macro = f"XR_HOSTED_FRAGMENT_SUSPENDABLE_{native}"
        lines.extend(
            [
                f"extern XrValue {native}(",
                "    const XrHostedFragmentContext *, const XrValue *, uint32_t);",
                f"#if {suspend_macro}",
                f"static XrCFuncResult {adapter}_resume(",
                "    XrVMRuntime *isolate, int status, XrValue resume_value,",
                "    void *opaque, XrValue *result) {",
                "    (void)status;",
                "    (void)resume_value;",
                "    XrHostedFragmentSignal signal = {0};",
                "    XrHostedFragmentContext context = {0};",
                "    context.ops = &xr_hosted_fragment_host_ops;",
                "    context.host = isolate;",
                "    context.coroutine = xr_hosted_fragment_current_coroutine(isolate);",
                "    context.runtime_ops = xr_hosted_fragment_runtime_ops();",
                "    context.continuation = opaque;",
                f'    context.module_name = "{entry["module"]}";',
                "    context.signal = &signal;",
                f"    XrValue value = {native}(&context, NULL, UINT32_C(0));",
                "    return xr_stdlib_vm_fastpath_handle_yieldable_signal(",
                f'        isolate, "{symbol}", &signal, {adapter}_resume, value, result);',
                "}",
                yield_signature,
                "#else",
                sync_signature,
                "#endif",
                "    XrHostedFragmentSignal signal = {0};",
                "    XrHostedFragmentContext context = {0};",
                "    context.ops = &xr_hosted_fragment_host_ops;",
                "    context.host = isolate;",
                "    context.coroutine = xr_hosted_fragment_current_coroutine(isolate);",
                "    context.runtime_ops = xr_hosted_fragment_runtime_ops();",
                f'    context.module_name = "{entry["module"]}";',
                "    context.signal = &signal;",
            ]
        )
        if kind in {"constructor", "static"}:
            lines.append("    (void)self;")
        if has_defaults or has_receiver:
            lines.extend(
                [
                    f"    if (nargs < {required} || nargs > {expected}" + (" || (nargs > 0 && !args)" if expected else "") + ") {",
                    "        signal.status = XR_HOSTED_FRAGMENT_INVALID_CALL;",
                    "        signal.argument_index = 0;",
                    f"#if {suspend_macro}",
                    f'        (void)xr_hosted_fragment_handle_signal(isolate, "{symbol}", &signal);',
                    "        return XR_CFUNC_ERROR;",
                    "#else",
                    f'        return xr_hosted_fragment_handle_signal(isolate, "{symbol}", &signal);',
                    "#endif",
                    "    }",
                ]
            )
        if has_defaults or has_receiver:
            lines.append(f"    XrValue normalized[{len(bridge_params)}];")
        if has_defaults:
            lines.append("    normalized[0] = XR_FROM_INT(nargs);")
        if has_receiver:
            receiver_index = 1 if has_defaults else 0
            lines.append(f"    normalized[{receiver_index}] = self;")
        if has_defaults:
            for user_index, (_, param_type, default) in enumerate(user_params):
                param_index = user_index + 1 + (1 if has_receiver else 0)
                lines.append(
                    f"    normalized[{param_index}] = nargs > {user_index} ? args[{user_index}] : XR_NULL_VAL;"
                )
        elif has_receiver:
            for user_index, _ in enumerate(user_params):
                lines.append(f"    normalized[{user_index + 1}] = args[{user_index}];")
        call_arguments = "normalized" if (has_defaults or has_receiver) and params else "args"
        call_count = (
            f"UINT32_C({len(bridge_params)})"
            if has_defaults or has_receiver
            else "(nargs < 0 ? UINT32_MAX : (uint32_t)nargs)"
        )
        lines.extend(
            [
                f"    XrValue value = {native}(&context, "
                + call_arguments
                + f", {call_count});",
                f"#if {suspend_macro}",
                "    return xr_stdlib_vm_fastpath_handle_yieldable_signal(",
                f'        isolate, "{symbol}", &signal, {adapter}_resume, value, result);',
                "#else",
                "    if (signal.status == XR_HOSTED_FRAGMENT_RETURN)",
                "        return value;",
                f'    return xr_hosted_fragment_handle_signal(isolate, "{symbol}", &signal);',
                "#endif",
            ]
        )
        lines.extend(["}", ""])
    lines.extend(["#else", ""])
    for index, entry in enumerate(entries):
        native = c_ident(str(entry["native"]))
        macro = f"XR_HOSTED_FRAGMENT_SUSPENDABLE_{native}"
        lines.append(f"#if {macro}")
        if str(entry.get("kind", "function")) == "function":
            lines.append(
                f"extern XrCFuncResult xr_stdlib_vm_fastpath_adapter_{index}("
                "XrVMRuntime *, XrValue *, int, XrValue *);"
            )
        else:
            lines.append(
                f"extern XrCFuncResult xr_stdlib_vm_fastpath_adapter_{index}("
                "XrVMRuntime *, XrValue, XrValue *, int, XrValue *);"
            )
        lines.append("#else")
        if str(entry.get("kind", "function")) == "function":
            lines.append(
                f"extern XrValue xr_stdlib_vm_fastpath_adapter_{index}("
                "XrVMRuntime *, XrValue *, int);"
            )
        else:
            lines.append(
                f"extern XrValue xr_stdlib_vm_fastpath_adapter_{index}("
                "XrVMRuntime *, XrValue, XrValue *, int);"
            )
        lines.append("#endif")
    lines.append("")
    lines.append("static const XrStdlibVmFastpathEntry g_stdlib_vm_fastpaths[] = {")
    for index, entry in enumerate(entries):
        if str(entry.get("kind", "function")) != "function":
            continue
        native = c_ident(str(entry["native"]))
        macro = f"XR_HOSTED_FRAGMENT_SUSPENDABLE_{native}"
        lines.append(f"#if {macro}")
        lines.append(
            f'    {{"{entry["module"]}", "{entry["member"]}", "{entry["abi"]}", '
            f'"{entry["effect"]}", "{entry["ownership"]}", '
            f"NULL, xr_stdlib_vm_fastpath_adapter_{index}}},"
        )
        lines.append("#else")
        lines.append(
            f'    {{"{entry["module"]}", "{entry["member"]}", "{entry["abi"]}", '
            f'"{entry["effect"]}", "{entry["ownership"]}", '
            f"xr_stdlib_vm_fastpath_adapter_{index}, NULL}},"
        )
        lines.append("#endif")
    lines.extend(
        [
            "};",
            "#define XR_STDLIB_VM_FASTPATH_GENERATED_COUNT \\",
            "    (sizeof(g_stdlib_vm_fastpaths) / sizeof(g_stdlib_vm_fastpaths[0]))",
            "",
            "static const XrStdlibVmFastpathClassEntry g_stdlib_vm_fastpath_classes[] = {",
        ]
    )
    classes = sorted({
        (str(entry["module"]), str(entry.get("class_name", "")))
        for entry in entries if entry.get("class_name")
    })
    for module, class_name in classes:
        lines.append(f'    {{"{module}", "{class_name}"}},')
    lines.extend(
        [
            "};",
            "#define XR_STDLIB_VM_FASTPATH_GENERATED_CLASS_COUNT \\",
            "    (sizeof(g_stdlib_vm_fastpath_classes) / sizeof(g_stdlib_vm_fastpath_classes[0]))",
            "",
            "static const XrStdlibVmFastpathMethodEntry g_stdlib_vm_fastpath_methods[] = {",
        ]
    )
    kind_constants = {
        "constructor": "XR_STDLIB_VM_MEMBER_CONSTRUCTOR",
        "method": "XR_STDLIB_VM_MEMBER_METHOD",
        "static": "XR_STDLIB_VM_MEMBER_STATIC",
        "getter": "XR_STDLIB_VM_MEMBER_GETTER",
        "setter": "XR_STDLIB_VM_MEMBER_SETTER",
    }
    for index, entry in enumerate(entries):
        kind = str(entry.get("kind", "function"))
        if kind == "function":
            continue
        user_count = len(entry["params"]) - (1 if kind in {"method", "getter", "setter"} else 0)
        native = c_ident(str(entry["native"]))
        macro = f"XR_HOSTED_FRAGMENT_SUSPENDABLE_{native}"
        lines.append(f"#if {macro}")
        lines.append(
            f'    {{"{entry["module"]}", "{entry["class_name"]}", "{entry["member"]}", '
            f'{kind_constants[kind]}, UINT16_C({user_count}), NULL, '
            f'xr_stdlib_vm_fastpath_adapter_{index}}},'
        )
        lines.append("#else")
        lines.append(
            f'    {{"{entry["module"]}", "{entry["class_name"]}", "{entry["member"]}", '
            f'{kind_constants[kind]}, UINT16_C({user_count}), '
            f'xr_stdlib_vm_fastpath_adapter_{index}, NULL}},'
        )
        lines.append("#endif")
    lines.extend(
        [
            "};",
            "#define XR_STDLIB_VM_FASTPATH_GENERATED_METHOD_COUNT \\",
            "    (sizeof(g_stdlib_vm_fastpath_methods) / sizeof(g_stdlib_vm_fastpath_methods[0]))",
            "#endif",
            "",
        ]
    )
    return "\n".join(lines)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def render_fragment_manifest(
    root: Path,
    version: int,
    entries: list[dict[str, Any]],
    deferred: list[dict[str, str]],
    unsupported: list[dict[str, str]],
    fingerprint: str,
    target: str,
    compiler: Path,
) -> str:
    payload = {
        "schema": 1,
        "artifact_kind": "hosted_fragment",
        "linkage": "static",
        "target": target,
        "object_abi_version": version,
        "entry_abi": "XrValue(XrHostedFragmentContext*,XrValue*,uint32_t;signal)",
        "source_fingerprint": fingerprint,
        "compiler": {
            "path": compiler.name,
            "sha256": sha256_file(compiler),
        },
        "coverage_complete": not deferred and not unsupported,
        "coverage": {
            "generated": len(entries),
            "unsupported": len(unsupported),
            "total": len(entries) + len(unsupported),
        },
        "deferred_exports": deferred,
        "unsupported_exports": unsupported,
        "exports": [],
    }
    for entry in entries:
        source = str(entry["reference"]).split("::", 1)[0]
        payload["exports"].append(
            {
                "symbol": entry["symbol"],
                "native": entry["native"],
                "abi": entry["abi"],
                "effect": entry["effect"],
                "ownership": entry["ownership"],
                "source": source,
                "source_sha256": sha256_file(root / source),
            }
        )
    return json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True) + "\n"


def write_if_changed(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text(encoding="utf-8") == content:
        return
    # Path.write_text gained a newline parameter in 3.10; open() has always
    # taken one, and generated sources must keep LF endings on every host.
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write(content)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--harness-dir", type=Path)
    parser.add_argument("--registry-output", type=Path)
    parser.add_argument("--manifest-output", type=Path)
    parser.add_argument("--target")
    parser.add_argument("--compiler", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        version, entries, deferred, unsupported, fingerprint = load_entries(root)
        if not entries:
            fail("no [[vm_fastpath]] entries are registered")
        if args.check:
            print(
                f"stdlib VM fastpaths: {len(entries)} entries, object ABI v{version}, "
                f"{len(deferred)} deferred, {len(unsupported)} unsupported, "
                f"fingerprint {fingerprint}"
            )
            return 0
        if not args.harness_dir or not args.registry_output:
            fail("--harness-dir and --registry-output are required unless --check is used")
        if not args.manifest_output or not args.target or not args.compiler:
            fail("--manifest-output, --target and --compiler are required unless --check is used")
        compiler = args.compiler.resolve()
        if not compiler.is_file():
            fail(f"stage compiler does not exist: {compiler}")
        harness, manifest = render_harness(entries, fingerprint)
        write_if_changed(args.harness_dir / "main.xr", harness)
        write_if_changed(args.harness_dir / "xray.toml", manifest)
        write_if_changed(
            args.registry_output, render_registry(version, entries, fingerprint, args.target)
        )
        write_if_changed(
            args.manifest_output,
            render_fragment_manifest(
                root, version, entries, deferred, unsupported, fingerprint, args.target, compiler
            ),
        )
        print(
            f"generated {len(entries)} stdlib VM fastpaths "
            f"({len(deferred)} deferred, {len(unsupported)} unsupported, "
            f"object ABI v{version})"
        )
        return 0
    # TOMLDecodeError derives from ValueError; the dependency-free fallback
    # parser reports malformed manifests as RuntimeError.
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"generate_vm_fastpaths: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

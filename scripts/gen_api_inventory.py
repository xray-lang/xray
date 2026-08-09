#!/usr/bin/env python3
"""Generate a source-derived Xray API inventory.

The inventory is intentionally broader than `xray builtin-dump`: it merges
analyzer-visible stdlib modules, pure-Xray stdlib exports, native type
declarations, global builtins, prelude names, builtin interfaces, keywords, and
IR intrinsics into one machine-readable list.  The same JSON can drive a human
HTML explorer and documentation coverage checks.
"""

from __future__ import annotations

import argparse
import ast
import html
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import builtin_symbols  # noqa: E402  (repo-local helper, needs the path above)


ITEM_SORT_KEY = lambda item: (
    item.get("category", ""),
    item.get("namespace", ""),
    item.get("name", ""),
    item.get("kind", ""),
)
API_ID_FIELDS = ("category", "namespace", "name", "kind")


GLOBAL_SIGNATURES = {
    "assert": "(value: any, ...): ()",
    "assert_eq": "(left: any, right: any, ...): ()",
    "assert_ne": "(left: any, right: any, ...): ()",
    "assert_true": "(value: any): ()",
    "assert_false": "(value: any): ()",
    "assert_throws": "(fn: function, ...): ()",
    "likely": "(value: bool): bool",
    "unlikely": "(value: bool): bool",
    "int": "(value: any): int",
    "float": "(value: any): float",
    "string": "(value: any): string",
    "bool": "(value: any): bool",
    "char": "(value: any): char",
    "Array": "(...items: any): Array<any>",
    "Map": "(...items: any): Map<any, any>",
    "Set": "(...items: any): Set<any>",
    "WeakMap": "(...items: any): Map<any, any>",
    "WeakSet": "(...items: any): Set<any>",
    "typeOf": "(value: any): Type",
    "typeName": "(value: any): string | <T>(): string",
    "chr": "(codepoint: int): string",
    "copy": "(value: any): any",
    "dump": "(value: any, ...): ()",
    "print": "(...values: any): ()",
}

GLOBAL_SUMMARIES = {
    "assert": "Assert that a value is truthy.",
    "assert_eq": "Assert equality in tests.",
    "assert_ne": "Assert inequality in tests.",
    "assert_true": "Assert that a value is true.",
    "assert_false": "Assert that a value is false.",
    "assert_throws": "Assert that a callable throws.",
    "likely": "Branch-probability hint for true-biased conditions.",
    "unlikely": "Branch-probability hint for false-biased conditions.",
    "int": "Convert a value to int.",
    "float": "Convert a value to float.",
    "string": "Convert a value to string.",
    "bool": "Convert a value to bool.",
    "char": "Construct a char from a codepoint.",
    "Array": "Construct an Array.",
    "Map": "Construct a Map.",
    "Set": "Construct a Set.",
    "WeakMap": "Construct a weak map.",
    "WeakSet": "Construct a weak set.",
    "typeOf": "Return the stable TypeId for a value.",
    "typeName": "Return the debug/display type name for a value or static type.",
    "chr": "Construct a one-character string from a codepoint.",
    "copy": "Deep-copy a runtime value where supported.",
    "dump": "Print debug representation of values.",
    "print": "Print values.",
    "process": "Runtime process metadata.",
    "__file__": "Current source file path.",
    "__dir__": "Current source directory.",
}



def rel(root: Path, path: Path) -> str:
    try:
        # Inventory paths are serialized contract data, not host-native paths.
        # Keep them byte-stable across Windows and POSIX so source ownership
        # checks can match the canonical repository prefixes.
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return path.as_posix()


def line_for_offset(text: str, offset: int) -> int:
    return text.count("\n", 0, max(offset, 0)) + 1


def item(
    *,
    category: str,
    namespace: str,
    name: str,
    kind: str,
    signature: str = "",
    summary: str = "",
    source: str,
    line: int = 1,
    doc_surface: str = "",
    doc_module: str = "",
    profile: str = "",
    effect: str = "",
    allocation: str = "",
) -> dict[str, Any]:
    qualified = f"{namespace}.{name}" if namespace and name and name != namespace else name
    entry = {
        "category": category,
        "namespace": namespace,
        "name": name,
        "qualified": qualified,
        "kind": kind,
        "signature": signature,
        "summary": summary,
        "source": source,
        "line": line,
        "doc_surface": doc_surface,
        "doc_module": doc_module,
    }
    if profile:
        entry["profile"] = profile
    if effect:
        entry["effect"] = effect
    if allocation:
        entry["allocation"] = allocation
    return entry


def stdlib_doc_surface_for_name(doc_surface: str, doc_module: str, name: str) -> tuple[str, str]:
    if doc_surface != "stdlib" or not name.startswith("_"):
        return doc_surface, doc_module
    return "", ""


def strip_line_comment(line: str) -> str:
    in_string = False
    escaped = False
    out: list[str] = []
    for ch in line:
        if escaped:
            out.append(ch)
            escaped = False
            continue
        if ch == "\\" and in_string:
            out.append(ch)
            escaped = True
            continue
        if ch == '"':
            in_string = not in_string
            out.append(ch)
            continue
        if ch == "/" and not in_string:
            # Caller passes one line; keep this simple and conservative.
            pass
        out.append(ch)
    text = "".join(out)
    return re.sub(r"\s*//.*$", "", text)


def split_top_level_commas(text: str) -> list[str]:
    parts: list[str] = []
    start = 0
    angle = paren = bracket = brace = 0
    in_string = False
    escaped = False
    for i, ch in enumerate(text):
        if escaped:
            escaped = False
            continue
        if ch == "\\" and in_string:
            escaped = True
            continue
        if ch == '"':
            in_string = not in_string
            continue
        if in_string:
            continue
        if ch == "<":
            angle += 1
        elif ch == ">" and angle:
            angle -= 1
        elif ch == "(":
            paren += 1
        elif ch == ")" and paren:
            paren -= 1
        elif ch == "[":
            bracket += 1
        elif ch == "]" and bracket:
            bracket -= 1
        elif ch == "{":
            brace += 1
        elif ch == "}" and brace:
            brace -= 1
        elif ch == "," and not (angle or paren or bracket or brace):
            part = text[start:i].strip()
            if part:
                parts.append(part)
            start = i + 1
    tail = text[start:].strip()
    if tail:
        parts.append(tail)
    return parts


def matching_brace_index(text: str, open_index: int) -> int:
    depth = 0
    in_string = False
    escaped = False
    for i in range(open_index, len(text)):
        ch = text[i]
        if escaped:
            escaped = False
            continue
        if ch == "\\" and in_string:
            escaped = True
            continue
        if ch == '"':
            in_string = not in_string
            continue
        if in_string:
            continue
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return i
    return -1


def matching_paren_index(text: str, open_index: int) -> int:
    depth = 0
    in_string = False
    escaped = False
    for i in range(open_index, len(text)):
        ch = text[i]
        if escaped:
            escaped = False
            continue
        if ch == "\\" and in_string:
            escaped = True
            continue
        if ch == '"':
            in_string = not in_string
            continue
        if in_string:
            continue
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return i
    return -1


def brace_delta(text: str) -> int:
    delta = 0
    in_string = False
    escaped = False
    for ch in text:
        if escaped:
            escaped = False
            continue
        if ch == "\\" and in_string:
            escaped = True
            continue
        if ch == '"':
            in_string = not in_string
            continue
        if in_string:
            continue
        if ch == "{":
            delta += 1
        elif ch == "}":
            delta -= 1
    return delta


def normalize_signature(params: str, ret: str | None) -> str:
    params = " ".join(params.strip().split())
    ret = " ".join((ret or "()").strip().split())
    return f"({params}): {ret}"


def infer_const_type(value: str, annotation: str | None) -> str:
    if annotation:
        return annotation.strip()
    value = value.strip()
    if value.startswith('"'):
        return "string"
    if value in {"true", "false"}:
        return "bool"
    if re.fullmatch(r"-?(?:0x[0-9A-Fa-f]+|\d+)", value):
        return "int"
    if re.fullmatch(r"-?\d+\.\d+", value):
        return "float"
    return "unknown"


def parse_exported_names(text: str) -> list[str]:
    """Return names whose own top-level declaration carries `export`.

    Selective re-exports intentionally do not enter the source inventory for
    the defining module, and the removed local `export { name }` form is not
    recognized.
    """
    pattern = re.compile(
        r"(?m)^export\s+(?:(?:final|packed)\s+)?"
        r"(?:fn|class|struct|union|interface|enum|type|const|shared)\s+"
        r"([A-Za-z_][A-Za-z0-9_]*)"
    )
    return [match.group(1) for match in pattern.finditer(text)]


def decode_c_string(raw: str) -> str:
    try:
        return ast.literal_eval(f'"{raw}"')
    except (SyntaxError, ValueError):
        return raw


CLASS_RE = re.compile(
    r"(?m)^(?:@[A-Za-z_][^\n]*\n)*(?:export\s+)?(?:(?:final|packed)\s+)?"
    r"(?:class|struct|union|interface)\s+([A-Za-z_][A-Za-z0-9_]*)(?:<[^>]+>)?"
    r"(?:\s+align\s*\([^\n)]*\))?\s*\{"
)
TOP_FN_RE = re.compile(
    r"(?m)^(?:export\s+)?fn\s+([A-Za-z_][A-Za-z0-9_]*)\s*"
    r"(?:<[^>]+>)?\("
)
TOP_CONST_RE = re.compile(
    r"(?m)^(?:export\s+)?const\s+([A-Za-z_][A-Za-z0-9_]*)\s*"
    r"(?::\s*([^=\n]+))?\s*=\s*([^\n]+)"
)
TYPE_ALIAS_RE = re.compile(
    r"(?m)^(?:export\s+)?type\s+([A-Za-z_][A-Za-z0-9_]*)(?:<[^>{]+>)?\s*=\s*"
)
MEMBER_METHOD_RE = re.compile(
    r"^\s*(static\s+)?([A-Za-z_][A-Za-z0-9_]*)(?:<[^>]+>)?\s*\("
)
MEMBER_SIGNATURE_START_RE = re.compile(r"^\s*(?:static\s+)?[A-Za-z_][A-Za-z0-9_]*(?:<[^>]+>)?\s*\(")
MEMBER_FIELD_RE = re.compile(r"^\s*(?:const\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*:\s*([^=\n]+)$")


def parse_signature_tail(line: str, open_index: int) -> tuple[str, str | None] | None:
    close_index = matching_paren_index(line, open_index)
    if close_index < 0:
        return None
    params = line[open_index + 1 : close_index]
    tail_start = close_index + 1
    while tail_start < len(line) and line[tail_start].isspace():
        tail_start += 1
    ret: str | None = None
    if line.startswith("->", tail_start):
        ret_start = tail_start + 2
        ret_end = line.find("{", ret_start)
        if ret_end < 0:
            ret_end = len(line)
        ret = line[ret_start:ret_end].strip()
    return params, ret


def parse_member_method_line(line: str) -> tuple[bool, str, str, str | None] | None:
    match = MEMBER_METHOD_RE.match(line)
    if not match:
        return None
    open_index = line.find("(", match.start())
    parsed = parse_signature_tail(line, open_index)
    if parsed is None:
        return None
    params, ret = parsed
    return bool(match.group(1)), match.group(2), params, ret


def parse_top_functions(text: str) -> list[tuple[re.Match[str], str, str, str | None]]:
    """Return complete top-level function signatures, including multiline ones."""
    out: list[tuple[re.Match[str], str, str, str | None]] = []
    for match in TOP_FN_RE.finditer(text):
        open_index = text.find("(", match.start(), match.end())
        close_index = matching_paren_index(text, open_index)
        if close_index < 0:
            continue
        body_index = text.find("{", close_index + 1)
        if body_index < 0:
            signature_end = text.find("\n", close_index + 1)
            signature_end = len(text) if signature_end < 0 else signature_end
        else:
            signature_end = body_index + 1
        parsed = parse_signature_tail(text[:signature_end], open_index)
        if parsed is None:
            continue
        params, ret = parsed
        out.append((match, match.group(1), params, ret))
    return out


def parse_class_body(
    root: Path,
    path: Path,
    text: str,
    class_name: str,
    body: str,
    body_offset: int,
    category: str,
    doc_surface: str = "",
    doc_module: str = "",
) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    depth = 0
    lines = body.splitlines()
    line_index = 0
    while line_index < len(lines):
        local_lineno = line_index + 1
        raw = lines[line_index]
        line = strip_line_comment(raw).rstrip()
        if not line.strip() or line.strip().startswith("@"):
            depth = max(0, depth + brace_delta(line))
            line_index += 1
            continue
        if depth != 0:
            depth = max(0, depth + brace_delta(line))
            line_index += 1
            continue

        # Class member signatures may span any number of physical lines and
        # may themselves contain nested function-type parentheses.  Parse one
        # complete logical signature from the source instead of dropping the
        # continuation lines; the inventory is an input to generated ABI
        # wrappers, so silently turning a six-argument constructor into a
        # zero-argument constructor is not an acceptable approximation.
        signature_delta = brace_delta(line)
        if MEMBER_SIGNATURE_START_RE.match(line):
            signature_parts = [line.strip()]
            while parse_member_method_line(" ".join(signature_parts)) is None:
                line_index += 1
                if line_index >= len(lines):
                    break
                continuation = strip_line_comment(lines[line_index]).strip()
                signature_parts.append(continuation)
                signature_delta += brace_delta(continuation)
            line = " ".join(signature_parts)

        method = parse_member_method_line(line)
        if method:
            static, name, params, ret = method
            kind = "static-method" if static else "method"
            surface, module = stdlib_doc_surface_for_name(doc_surface, doc_module, name)
            out.append(
                item(
                    category=category,
                    namespace=class_name,
                    name=name,
                    kind=kind,
                    signature=normalize_signature(params, ret),
                    source=rel(root, path),
                    line=line_for_offset(text, body_offset) + local_lineno - 1,
                    doc_surface=surface,
                    doc_module=module,
                )
            )
            depth = max(0, depth + signature_delta)
            line_index += 1
            continue
        field = MEMBER_FIELD_RE.match(line)
        if field:
            name = field.group(1)
            surface, module = stdlib_doc_surface_for_name(doc_surface, doc_module, name)
            out.append(
                item(
                    category=category,
                    namespace=class_name,
                    name=name,
                    kind="field",
                    signature=f": {' '.join(field.group(2).strip().split())}",
                    source=rel(root, path),
                    line=line_for_offset(text, body_offset) + local_lineno - 1,
                    doc_surface=surface,
                    doc_module=module,
                )
            )
        depth = max(0, depth + brace_delta(line))
        line_index += 1
    return out


def parse_xray_classes(
    root: Path,
    path: Path,
    category: str,
    exported: set[str] | None = None,
    doc_surface: str = "",
    doc_module: str = "",
) -> list[dict[str, Any]]:
    text = path.read_text(encoding="utf-8")
    out: list[dict[str, Any]] = []
    for match in CLASS_RE.finditer(text):
        class_name = match.group(1)
        if exported is not None and class_name not in exported:
            continue
        open_index = text.find("{", match.end() - 1)
        close_index = matching_brace_index(text, open_index)
        if close_index < 0:
            continue
        out.append(
            item(
                category=category,
                namespace=class_name,
                name=class_name,
                kind="type",
                signature=class_name,
                source=rel(root, path),
                line=line_for_offset(text, match.start()),
                doc_surface=doc_surface,
                doc_module=doc_module,
            )
        )
        out.extend(
            parse_class_body(
                root,
                path,
                text,
                class_name,
                text[open_index + 1 : close_index],
                open_index + 1,
                category,
                doc_surface,
                doc_module,
            )
        )
    return out


def parse_xray_type_aliases(
    root: Path,
    path: Path,
    category: str,
    exported: set[str],
    doc_surface: str,
    doc_module: str,
) -> list[dict[str, Any]]:
    text = path.read_text(encoding="utf-8")
    out: list[dict[str, Any]] = []
    for match in TYPE_ALIAS_RE.finditer(text):
        name = match.group(1)
        if name not in exported:
            continue
        value_start = match.end()
        open_index = value_start if value_start < len(text) and text[value_start] == "{" else -1
        close_index = matching_brace_index(text, open_index) if open_index >= 0 else -1
        if close_index >= 0:
            signature = " ".join(text[value_start : close_index + 1].split())
        else:
            line_end = text.find("\n", value_start)
            if line_end < 0:
                line_end = len(text)
            signature = " ".join(text[value_start:line_end].strip().split())
        out.append(
            item(
                category=category,
                namespace=path.parent.name,
                name=name,
                kind="type",
                signature=signature,
                source=rel(root, path),
                line=line_for_offset(text, match.start()),
                doc_surface=doc_surface,
                doc_module=doc_module,
            )
        )
        if close_index < 0:
            continue
        body = text[open_index + 1 : close_index]
        for raw_field in split_top_level_commas(body):
            field = " ".join(raw_field.strip().split())
            field_match = re.fullmatch(
                r"(?:const\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*:\s*(.+)", field
            )
            if not field_match:
                continue
            out.append(
                item(
                    category=category,
                    namespace=path.parent.name,
                    name=f"{name}.{field_match.group(1)}",
                    kind="field",
                    signature=field_match.group(2).strip(),
                    summary="Type alias field",
                    source=rel(root, path),
                    line=line_for_offset(text, open_index + 1 + body.find(raw_field)),
                    doc_surface=doc_surface,
                    doc_module=doc_module,
                )
            )
    return out


def load_builtin_dump(root: Path, xray: Path | None, builtin_dump: Path | None) -> dict[str, Any]:
    if builtin_dump:
        return json.loads(builtin_dump.read_text(encoding="utf-8"))
    if xray is None:
        candidate = root / "build" / "xray"
        if candidate.exists():
            xray = candidate
    if xray is None:
        return {"modules": []}
    proc = subprocess.run(
        [str(xray), "builtin-dump"],
        cwd=str(root),
        text=True,
        encoding="utf-8",
        errors="strict",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=20,
    )
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr or proc.stdout)
    return json.loads(proc.stdout)


def load_stdlibgen(root: Path):
    tools_dir = root / "tools" / "stdlibgen"
    sys.path.insert(0, str(tools_dir))
    try:
        import stdlibgen  # type: ignore[import-not-found]
    finally:
        try:
            sys.path.remove(str(tools_dir))
        except ValueError:
            pass
    return stdlibgen


def build_def_location_index(root: Path) -> dict[tuple[str, str, str], tuple[str, int]]:
    defs_dir = root / "stdlib" / "defs"
    index: dict[tuple[str, str, str], tuple[str, int]] = {}
    current_module = ""
    block_patterns = (
        ("fn", re.compile(r"fn\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{")),
        ("const", re.compile(r"const\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{")),
        ("handle", re.compile(r"handle\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{")),
        ("object", re.compile(r"object\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{")),
        ("enum", re.compile(r"enum\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{")),
    )
    for path in sorted(defs_dir.glob("*.def")):
        for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            module_match = re.fullmatch(r"module\s+([A-Za-z_][A-Za-z0-9_/]*)\s*\{", line)
            if module_match:
                current_module = module_match.group(1)
                continue
            if line == "}" and current_module:
                continue
            if not current_module:
                continue
            for kind, pattern in block_patterns:
                match = pattern.fullmatch(line)
                if not match:
                    continue
                if len(match.groups()) == 1:
                    name = match.group(1)
                else:
                    name = f"{match.group(1)}.{match.group(2)}"
                index[(kind, current_module, name)] = (rel(root, path), lineno)
                break
    return index


def module_inventory_surface(module: str) -> tuple[str, str, str]:
    return "stdlib-module", "stdlib", module


def collect_def_stdlib(root: Path) -> list[dict[str, Any]]:
    stdlibgen = load_stdlibgen(root)
    (
        entries,
        constants,
        handles,
        object_shapes,
        enums,
        _type_methods,
        _native_classes,
        _classes,
        _class_methods,
        _class_fields,
    ) = stdlibgen.parse_def_metadata(root)
    locations = build_def_location_index(root)

    def source_for(kind: str, module: str, name: str) -> tuple[str, int]:
        return locations.get((kind, module, name), ("stdlib/defs/core.def", 1))

    out: list[dict[str, Any]] = []
    for entry in entries:
        if entry.is_internal:
            continue
        source, line = source_for("fn", entry.module, entry.name)
        category, surface, doc_module = module_inventory_surface(entry.module)
        surface, doc_module = stdlib_doc_surface_for_name(surface, doc_module, entry.name)
        out.append(
            item(
                category=category,
                namespace=entry.module,
                name=entry.name,
                kind="function",
                signature=entry.signature,
                summary=entry.doc,
                source=source,
                line=line,
                doc_surface=surface,
                doc_module=doc_module,
            )
        )
    for const in constants:
        if getattr(const, "is_internal", False):
            continue
        source, line = source_for("const", const.module, const.name)
        category, surface, doc_module = module_inventory_surface(const.module)
        surface, doc_module = stdlib_doc_surface_for_name(surface, doc_module, const.name)
        out.append(
            item(
                category=category,
                namespace=const.module,
                name=const.name,
                kind="const",
                signature=const.signature,
                summary=const.doc,
                source=source,
                line=line,
                doc_surface=surface,
                doc_module=doc_module,
            )
        )
    for handle in handles:
        if getattr(handle, "is_internal", False):
            continue
        source, line = source_for("handle", handle.module, handle.name)
        category, surface, doc_module = module_inventory_surface(handle.module)
        out.append(
            item(
                category=category,
                namespace=handle.module,
                name=handle.name,
                kind="handle",
                signature=handle.name,
                summary=handle.doc,
                source=source,
                line=line,
                doc_surface=surface,
                doc_module=doc_module,
            )
        )
        for field in handle.fields:
            out.append(
                item(
                    category=category,
                    namespace=handle.module,
                    name=f"{handle.name}.{field.name}",
                    kind="field",
                    signature=f"{'const ' if field.is_const else ''}{field.type}",
                    summary="Handle field",
                    source=source,
                    line=line,
                    doc_surface=surface,
                    doc_module=doc_module,
                )
            )
    for object_shape in object_shapes:
        if getattr(object_shape, "is_internal", False):
            continue
        source, line = source_for("object", object_shape.module, object_shape.name)
        category, surface, doc_module = module_inventory_surface(object_shape.module)
        out.append(
            item(
                category=category,
                namespace=object_shape.module,
                name=object_shape.name,
                kind="type",
                signature="{ "
                + ", ".join(f"{field.name}: {field.type}" for field in object_shape.fields)
                + " }",
                summary=object_shape.doc,
                source=source,
                line=line,
                doc_surface=surface,
                doc_module=doc_module,
            )
        )
        for field in object_shape.fields:
            out.append(
                item(
                    category=category,
                    namespace=object_shape.module,
                    name=f"{object_shape.name}.{field.name}",
                    kind="field",
                    signature=f"{'const ' if field.is_const else ''}{field.type}",
                    summary="Object field",
                    source=source,
                    line=line,
                    doc_surface=surface,
                    doc_module=doc_module,
                )
            )
    for enum in enums:
        if getattr(enum, "is_internal", False):
            continue
        source, line = source_for("enum", enum.module, enum.name)
        category, surface, doc_module = module_inventory_surface(enum.module)
        out.append(
            item(
                category=category,
                namespace=enum.module,
                name=enum.name,
                kind="enum",
                signature="enum " + enum.name,
                summary=enum.doc,
                source=source,
                line=line,
                doc_surface=surface,
                doc_module=doc_module,
            )
        )
        for variant in enum.variants:
            payload = (
                "(" + ", ".join(variant.payload_types) + ")"
                if variant.payload_types
                else ""
            )
            out.append(
                item(
                    category=category,
                    namespace=enum.module,
                    name=f"{enum.name}.{variant.name}",
                    kind="enum-variant",
                    signature=f"{enum.name}.{variant.name}{payload}",
                    summary="Enum variant",
                    source=source,
                    line=line,
                    doc_surface=surface,
                    doc_module=doc_module,
                )
            )
    return out


def collect_runtime_intrinsic_modules(root: Path) -> list[dict[str, Any]]:
    path = root / "src/frontend/analyzer/xanalyzer_builtins.c"
    text = path.read_text(encoding="utf-8")
    member_arrays: dict[str, list[dict[str, Any]]] = {}
    member_array_re = re.compile(
        r"static\s+const\s+XaBuiltinMember\s+"
        r"(?P<array>g_rt_[A-Za-z0-9_]+_functions)\[\]\s*=\s*\{(?P<body>.*?)\};",
        re.S,
    )
    member_re = re.compile(
        r"\{\s*\"(?P<name>(?:\\.|[^\"\\])*)\"\s*,\s*"
        r"\"(?P<signature>(?:\\.|[^\"\\])*)\"\s*,\s*"
        r"\"(?P<summary>(?:\\.|[^\"\\])*)\"\s*,\s*"
        r"(?P<is_method>true|false)\s*,\s*"
        r"(?P<is_static>true|false)\s*,\s*"
        r"(?P<is_internal>true|false)\s*,\s*"
        r"(?P<is_lowered_only>true|false)"
        r"(?:\s*,\s*(?:true|false))*\s*\}",
        re.S,
    )
    for array_match in member_array_re.finditer(text):
        symbols: list[dict[str, Any]] = []
        body = array_match.group("body")
        body_offset = array_match.start("body")
        for member_match in member_re.finditer(body):
            if member_match.group("is_internal") == "true":
                continue
            symbols.append(
                {
                    "name": decode_c_string(member_match.group("name")),
                    "signature": decode_c_string(member_match.group("signature")),
                    "summary": decode_c_string(member_match.group("summary")),
                    "line": line_for_offset(text, body_offset + member_match.start()),
                }
            )
        member_arrays[array_match.group("array")] = symbols

    module_match = re.search(
        r"static\s+const\s+XaBuiltinModule\s+g_rt_builtin_modules\[\]\s*=\s*\{(?P<body>.*?)\};",
        text,
        re.S,
    )
    if not module_match:
        return []

    out: list[dict[str, Any]] = []
    for match in re.finditer(
        r"\{\s*\"(?P<module>(?:\\.|[^\"\\])*)\"\s*,\s*"
        r"(?P<array>g_rt_[A-Za-z0-9_]+_functions)\s*,",
        module_match.group("body"),
    ):
        module = decode_c_string(match.group("module"))
        for symbol in member_arrays.get(match.group("array"), []):
            surface, doc_module = stdlib_doc_surface_for_name("stdlib", module, symbol["name"])
            out.append(
                item(
                    category="stdlib-module",
                    namespace=module,
                    name=symbol["name"],
                    kind="function",
                    signature=symbol["signature"],
                    summary=symbol["summary"],
                    source=rel(root, path),
                    line=symbol["line"],
                    doc_surface=surface,
                    doc_module=doc_module,
                )
            )
    return out


def collect_builtin_modules(root: Path, data: dict[str, Any]) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    source = "xray builtin-dump"
    for mod in data.get("modules", []):
        module = mod.get("name", "")
        for sym in mod.get("symbols", []):
            name = sym.get("name", "")
            surface, doc_module = stdlib_doc_surface_for_name("stdlib", module, name)
            out.append(
                item(
                    category="stdlib-module",
                    namespace=module,
                    name=name,
                    kind=sym.get("kind", "function"),
                    signature=sym.get("signature", ""),
                    summary=sym.get("summary", ""),
                    source=source,
                    line=1,
                    doc_surface=surface,
                    doc_module=doc_module,
                )
            )
    return out


def collect_pure_stdlib(root: Path) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    stdlib = root / "stdlib"
    for path in sorted(stdlib.glob("*/*.xr")):
        module = path.parent.name
        if module == "types" or module.startswith("_") or path.stem != module:
            continue
        text = path.read_text(encoding="utf-8")
        exports = parse_exported_names(text)
        if not exports:
            continue
        exported = set(exports)
        for match in TOP_CONST_RE.finditer(text):
            name = match.group(1)
            if name not in exported:
                continue
            surface, doc_module = stdlib_doc_surface_for_name("stdlib", module, name)
            out.append(
                item(
                    category="stdlib-module",
                    namespace=module,
                    name=name,
                    kind="const",
                    signature=f": {infer_const_type(match.group(3), match.group(2))}",
                    source=rel(root, path),
                    line=line_for_offset(text, match.start()),
                    doc_surface=surface,
                    doc_module=doc_module,
                )
            )
        for match, name, params, ret in parse_top_functions(text):
            if name not in exported:
                continue
            surface, doc_module = stdlib_doc_surface_for_name("stdlib", module, name)
            out.append(
                item(
                    category="stdlib-module",
                    namespace=module,
                    name=name,
                    kind="function",
                    signature=normalize_signature(params, ret),
                    source=rel(root, path),
                    line=line_for_offset(text, match.start()),
                    doc_surface=surface,
                    doc_module=doc_module,
                )
            )
        out.extend(
            parse_xray_classes(
                root,
                path,
                "stdlib-module",
                exported=exported,
                doc_surface="stdlib",
                doc_module=module,
            )
        )
        out.extend(
            parse_xray_type_aliases(
                root,
                path,
                "stdlib-module",
                exported,
                "stdlib",
                module,
            )
        )
    return out


def collect_native_types(root: Path) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    cards = {path.stem for path in (root / "spec/source/cards/stdlib").glob("*.json")}
    for path in sorted((root / "stdlib/types").glob("*.xr")):
        text = path.read_text(encoding="utf-8")
        for match in CLASS_RE.finditer(text):
            class_name = match.group(1)
            doc_module = class_name.lower() if class_name.lower() in cards else ""
            doc_surface = "stdlib" if doc_module else ""
            open_index = text.find("{", match.end() - 1)
            close_index = matching_brace_index(text, open_index)
            if close_index < 0:
                continue
            out.append(
                item(
                    category="native-type",
                    namespace=class_name,
                    name=class_name,
                    kind="type",
                    signature=class_name,
                    source=rel(root, path),
                    line=line_for_offset(text, match.start()),
                    doc_surface=doc_surface,
                    doc_module=doc_module,
                )
            )
            out.extend(
                parse_class_body(
                    root,
                    path,
                    text,
                    class_name,
                    text[open_index + 1 : close_index],
                    open_index + 1,
                    "native-type",
                    doc_surface,
                    doc_module,
                )
            )
    return out


def collect_globals(root: Path) -> list[dict[str, Any]]:
    path = root / "src/frontend/analyzer/xanalyzer.c"
    text = path.read_text(encoding="utf-8")
    out: list[dict[str, Any]] = []
    seen: set[tuple[str, str]] = set()
    for kind, pattern in (
        ("function", r'register_builtin_func\s*\(\s*analyzer\s*,\s*"([^"]+)"'),
        ("module", r'register_builtin_module\s*\(\s*analyzer\s*,\s*"([^"]+)"'),
        ("variable", r'register_builtin_var\s*\(\s*analyzer\s*,\s*"([^"]+)"'),
    ):
        for match in re.finditer(pattern, text):
            name = match.group(1)
            key = (kind, name)
            if key in seen:
                continue
            seen.add(key)
            out.append(
                item(
                    category="global-builtin",
                    namespace="global",
                    name=name,
                    kind=kind,
                    signature=GLOBAL_SIGNATURES.get(name, ""),
                    summary=GLOBAL_SUMMARIES.get(name, ""),
                    source=rel(root, path),
                    line=line_for_offset(text, match.start()),
                )
            )
    return out


def collect_prelude(root: Path) -> list[dict[str, Any]]:
    """Prelude types and enums, both read from builtin_symbols.def."""
    out: list[dict[str, Any]] = []
    registry = builtin_symbols.load(root)
    source = rel(root, registry.path)
    for symbol in registry.by_category("prelude_type"):
        out.append(
            item(
                category="prelude",
                namespace="prelude",
                name=symbol.name,
                kind="type",
                signature=symbol.prelude_kind or "",
                summary=f"Visible without import; native type {symbol.native_type}",
                source=source,
                line=symbol.line,
            )
        )
    for symbol in registry.by_category("enum"):
        out.append(
            item(
                category="prelude",
                namespace="prelude",
                name=symbol.name,
                kind="enum",
                signature=" | ".join(name for name, _payload in symbol.variants),
                summary="Built-in prelude enum.",
                source=source,
                line=symbol.line,
            )
        )
    return out


def collect_interfaces(root: Path) -> list[dict[str, Any]]:
    path = root / "src/frontend/analyzer/xanalyzer_builtin_interfaces.c"
    text = path.read_text(encoding="utf-8")
    method_counts: dict[str, int] = {}
    method_names: dict[str, list[str]] = {}
    for match in re.finditer(r"static\s+XaInterfaceMethod\s+(\w+)_methods\[\]\s*=\s*\{(.*?)\};", text, re.S):
        key = match.group(1)
        names = re.findall(r'\{\s*"([^"]+)"', match.group(2))
        method_names[key] = names
        method_counts[key] = len(names)
    out: list[dict[str, Any]] = []
    for match in re.finditer(r'\[XA_IFACE_[^\]]+\]\s*=\s*\{"([^"]+)",\s*(\w+)_methods,\s*(\d+)\}', text):
        iface = match.group(1)
        method_key = match.group(2)
        out.append(
            item(
                category="interface",
                namespace=iface,
                name=iface,
                kind="interface",
                signature=f"{method_counts.get(method_key, int(match.group(3)))} method(s)",
                source=rel(root, path),
                line=line_for_offset(text, match.start()),
            )
        )
        for method in method_names.get(method_key, []):
            out.append(
                item(
                    category="interface",
                    namespace=iface,
                    name=method,
                    kind="method",
                    source=rel(root, path),
                    line=line_for_offset(text, match.start()),
                )
            )
    return out


def collect_keywords(root: Path) -> list[dict[str, Any]]:
    path = root / "src/frontend/lexer/xkeywords.def"
    text = path.read_text(encoding="utf-8")
    out: list[dict[str, Any]] = []
    for match in re.finditer(r'XR_KW\(\s*"([^"]+)"\s*,\s*(\d+)\s*,\s*([A-Z_]+)\s*\)', text):
        out.append(
            item(
                category="language-keyword",
                namespace="keyword",
                name=match.group(1),
                kind="keyword",
                signature=match.group(3),
                source=rel(root, path),
                line=line_for_offset(text, match.start()),
            )
        )
    return out


def collect_intrinsics(root: Path) -> list[dict[str, Any]]:
    path = root / "src/ir/xi_intrinsic.def"
    text = path.read_text(encoding="utf-8")
    out: list[dict[str, Any]] = []
    for match in re.finditer(
        r"XI_INTRINSIC\(\s*([A-Z0-9_]+)\s*,\s*([0-9]+)\s*,\s*(-?[0-9]+)\s*,\s*([A-Za-z0-9_]+)\s*,\s*([^,]+),\s*([A-Z0-9_]+)\s*\)",
        text,
    ):
        out.append(
            item(
                category="ir-intrinsic",
                namespace="intrinsic",
                name=match.group(1),
                kind="intrinsic",
                signature=f"id={match.group(2)}, arity={match.group(3)}, ret={match.group(6)}",
                summary=f"Helper {match.group(4)}; effects {match.group(5).strip()}",
                source=rel(root, path),
                line=line_for_offset(text, match.start()),
            )
        )
    return out


def collect_enum_metadata_api(root: Path) -> list[dict[str, Any]]:
    """Collect the compiler-owned, non-reflective enum descriptor surface."""
    path = root / "src/runtime/value/xenum_metadata_api.def"
    text = path.read_text(encoding="utf-8")
    quoted = r'"(?:\\.|[^"\\])*"'
    common = (
        rf"\s*,\s*({quoted})\s*,\s*({quoted})\s*,\s*({quoted})\s*,\s*({quoted})"
    )
    out: list[dict[str, Any]] = []

    type_pattern = re.compile(
        rf"XR_ENUM_METADATA_TYPE\(\s*([A-Z_]+)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)"
        rf"{common}\s*\)",
        re.S,
    )
    for match in type_pattern.finditer(text):
        source_name = match.group(2)
        summary, profile, effect, allocation = (
            ast.literal_eval(match.group(i)) for i in range(3, 7)
        )
        out.append(
            item(
                category="language-intrinsic",
                namespace=source_name,
                name=source_name,
                kind="type",
                signature=f"{source_name}<E>",
                summary=summary,
                source=rel(root, path),
                line=line_for_offset(text, match.start()),
                profile=profile,
                effect=effect,
                allocation=allocation,
            )
        )

    property_pattern = re.compile(
        rf"XR_ENUM_METADATA_PROPERTY\(\s*([A-Z_]+)\s*,\s*"
        rf"([A-Za-z_][A-Za-z0-9_]*)\s*,\s*({quoted}){common}\s*\)",
        re.S,
    )
    owner_names = {
        "VARIANTS": "EnumVariants",
        "VARIANT": "EnumVariant",
        "PAYLOADS": "EnumPayloads",
        "PAYLOAD_FIELD": "EnumPayloadField",
    }
    for match in property_pattern.finditer(text):
        owner = owner_names[match.group(1)]
        source_name = match.group(2)
        result_type, summary, profile, effect, allocation = (
            ast.literal_eval(match.group(i)) for i in range(3, 8)
        )
        out.append(
            item(
                category="language-intrinsic",
                namespace=owner,
                name=source_name,
                kind="property",
                signature=f": {result_type}",
                summary=summary,
                source=rel(root, path),
                line=line_for_offset(text, match.start()),
                profile=profile,
                effect=effect,
                allocation=allocation,
            )
        )

    static_pattern = re.compile(
        rf"XR_ENUM_STATIC_PROPERTY\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*"
        rf"({quoted}){common}\s*\)",
        re.S,
    )
    for match in static_pattern.finditer(text):
        source_name = match.group(1)
        result_type, summary, profile, effect, allocation = (
            ast.literal_eval(match.group(i)) for i in range(2, 7)
        )
        out.append(
            item(
                category="language-intrinsic",
                namespace="enum",
                name=source_name,
                kind="static-property",
                signature=f": {result_type}",
                summary=summary,
                source=rel(root, path),
                line=line_for_offset(text, match.start()),
                profile=profile,
                effect=effect,
                allocation=allocation,
            )
        )
    return out


def dedupe(items: list[dict[str, Any]]) -> list[dict[str, Any]]:
    seen: dict[tuple[str, str, str, str, str], dict[str, Any]] = {}
    for entry in items:
        key = (
            entry.get("category", ""),
            entry.get("namespace", ""),
            entry.get("name", ""),
            entry.get("kind", ""),
            entry.get("signature", ""),
        )
        if key not in seen:
            seen[key] = entry
    return sorted(seen.values(), key=ITEM_SORT_KEY)


def api_identity(entry: dict[str, Any]) -> tuple[str, str, str, str]:
    return tuple(str(entry.get(field, "")) for field in API_ID_FIELDS)


def api_identity_label(entry: dict[str, Any]) -> str:
    namespace = str(entry.get("namespace", ""))
    name = str(entry.get("name", ""))
    kind = str(entry.get("kind", ""))
    category = str(entry.get("category", ""))
    qualified = str(entry.get("qualified") or (f"{namespace}.{name}" if namespace else name))
    return f"{category}:{kind}:{qualified}"


def api_diff_change(
    *,
    change: str,
    severity: str,
    entry: dict[str, Any],
    old_signature: str = "",
    new_signature: str = "",
    reason: str,
) -> dict[str, Any]:
    return {
        "change": change,
        "severity": severity,
        "category": entry.get("category", ""),
        "namespace": entry.get("namespace", ""),
        "name": entry.get("name", ""),
        "qualified": entry.get("qualified") or (
            f"{entry.get('namespace', '')}.{entry.get('name', '')}"
            if entry.get("namespace")
            else entry.get("name", "")
        ),
        "kind": entry.get("kind", ""),
        "old_signature": old_signature,
        "new_signature": new_signature,
        "reason": reason,
    }


def group_api_items(inventory: dict[str, Any]) -> dict[tuple[str, str, str, str], list[dict[str, Any]]]:
    grouped: dict[tuple[str, str, str, str], list[dict[str, Any]]] = {}
    for entry in inventory.get("items", []):
        grouped.setdefault(api_identity(entry), []).append(entry)
    for entries in grouped.values():
        entries.sort(key=lambda entry: str(entry.get("signature", "")))
    return grouped


def compare_api_inventories(
    old_inventory: dict[str, Any], new_inventory: dict[str, Any]
) -> dict[str, Any]:
    old_items = group_api_items(old_inventory)
    new_items = group_api_items(new_inventory)
    changes: list[dict[str, Any]] = []

    for key in sorted(set(old_items) | set(new_items)):
        old_group = old_items.get(key, [])
        new_group = new_items.get(key, [])
        if not old_group:
            for entry in new_group:
                changes.append(
                    api_diff_change(
                        change="added",
                        severity="additive",
                        entry=entry,
                        new_signature=str(entry.get("signature", "")),
                        reason="API symbol added",
                    )
                )
            continue
        if not new_group:
            for entry in old_group:
                changes.append(
                    api_diff_change(
                        change="removed",
                        severity="breaking",
                        entry=entry,
                        old_signature=str(entry.get("signature", "")),
                        reason="API symbol removed",
                    )
                )
            continue

        if len(old_group) == 1 and len(new_group) == 1:
            old_entry = old_group[0]
            new_entry = new_group[0]
            old_signature = str(old_entry.get("signature", ""))
            new_signature = str(new_entry.get("signature", ""))
            if old_signature != new_signature:
                changes.append(
                    api_diff_change(
                        change="signature_changed",
                        severity="breaking",
                        entry=new_entry,
                        old_signature=old_signature,
                        new_signature=new_signature,
                        reason="API signature changed",
                    )
                )
            continue

        old_by_signature = {str(entry.get("signature", "")): entry for entry in old_group}
        new_by_signature = {str(entry.get("signature", "")): entry for entry in new_group}
        for signature in sorted(set(old_by_signature) - set(new_by_signature)):
            entry = old_by_signature[signature]
            changes.append(
                api_diff_change(
                    change="overload_removed",
                    severity="breaking",
                    entry=entry,
                    old_signature=signature,
                    reason=f"API overload removed from {api_identity_label(entry)}",
                )
            )
        for signature in sorted(set(new_by_signature) - set(old_by_signature)):
            entry = new_by_signature[signature]
            changes.append(
                api_diff_change(
                    change="overload_added",
                    severity="additive",
                    entry=entry,
                    new_signature=signature,
                    reason=f"API overload added to {api_identity_label(entry)}",
                )
            )

    breaking = sum(1 for change in changes if change["severity"] == "breaking")
    additive = sum(1 for change in changes if change["severity"] == "additive")
    return {
        "schema": 1,
        "counts": {
            "changes": len(changes),
            "breaking": breaking,
            "additive": additive,
        },
        "changes": sorted(
            changes,
            key=lambda change: (
                change.get("severity", ""),
                change.get("category", ""),
                change.get("namespace", ""),
                change.get("name", ""),
                change.get("kind", ""),
                change.get("old_signature", ""),
                change.get("new_signature", ""),
            ),
        ),
    }


def build_inventory(root: Path, xray: Path | None, builtin_dump: Path | None) -> dict[str, Any]:
    builtin_data = load_builtin_dump(root, xray, builtin_dump)
    items: list[dict[str, Any]] = []
    def_items = collect_def_stdlib(root)
    runtime_intrinsic_items = collect_runtime_intrinsic_modules(root)
    source_module_symbols = {
        (entry.get("namespace", ""), entry.get("name", ""))
        for entry in [*def_items, *runtime_intrinsic_items]
        if entry.get("category") == "stdlib-module"
    }
    items.extend(def_items)
    items.extend(runtime_intrinsic_items)
    items.extend(
        entry
        for entry in collect_builtin_modules(root, builtin_data)
        if (entry.get("namespace", ""), entry.get("name", "")) not in source_module_symbols
    )
    items.extend(collect_pure_stdlib(root))
    items.extend(collect_native_types(root))
    items.extend(collect_globals(root))
    items.extend(collect_prelude(root))
    items.extend(collect_interfaces(root))
    items.extend(collect_keywords(root))
    items.extend(collect_intrinsics(root))
    items.extend(collect_enum_metadata_api(root))
    items = dedupe(items)
    return {
        "schema": 1,
        "root": str(root),
        "counts": {
            "items": len(items),
            "stdlib_symbols": sum(1 for i in items if i.get("doc_surface") == "stdlib"),
            "categories": len({i["category"] for i in items}),
            "namespaces": len({i["namespace"] for i in items}),
        },
        "items": items,
    }


def stdlib_symbol_index(inventory: dict[str, Any]) -> dict[str, set[str]]:
    index: dict[str, set[str]] = {}
    for entry in inventory.get("items", []):
        if entry.get("doc_surface") != "stdlib":
            continue
        module = entry.get("doc_module") or entry.get("namespace")
        name = entry.get("name", "")
        kind = entry.get("kind", "")
        if not module or not name or kind in {"module"}:
            continue
        namespace = entry.get("namespace", "")
        qualified = entry.get("qualified", name)
        symbol_name = qualified if namespace and namespace != module else name
        index.setdefault(module, set()).add(symbol_name)
    return index


def check_docs(root: Path, inventory: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    cards_dir = root / "spec/source/cards/stdlib"
    docs_dir = root / "docs/knowledge/stdlib"
    required_modules = sorted(stdlib_symbol_index(inventory))
    for module in required_modules:
        if not (cards_dir / f"{module}.json").exists():
            errors.append(f"missing stdlib knowledge card for source API module `{module}`")
        if docs_dir.exists() and not (docs_dir / f"{module}.md").exists():
            errors.append(f"missing generated stdlib knowledge markdown for `{module}`")
    errors.extend(check_def_stdlib_source(root, inventory))
    errors.extend(check_stdlib_builtin_dump_residue(inventory))
    errors.extend(check_language_spec_stdlib_residue(root))
    errors.extend(check_boundary_module_inventory(root, inventory))
    return errors


def check_boundary_module_inventory(root: Path, inventory: dict[str, Any]) -> list[str]:
    """Keep API inventory and task-196 module ownership on one module set."""
    scripts_dir = str((root / "scripts").resolve())
    if scripts_dir not in sys.path:
        sys.path.insert(0, scripts_dir)
    from stdlib_manifest import load_manifest

    manifest = load_manifest(root)
    inventoried = {
        entry.get("doc_module") or entry.get("namespace")
        for entry in inventory.get("items", [])
        if entry.get("category") == "stdlib-module"
    }
    return [
        f"stdlib boundary module `{module['name']}` has no source-derived API inventory entries"
        for module in manifest.modules
        if module.get("public", True) and module["name"] not in inventoried
    ]


def check_def_stdlib_source(root: Path, inventory: dict[str, Any]) -> list[str]:
    """Ensure .def-owned module APIs do not silently fall back to builtin-dump."""
    actual_by_key = {
        (
            entry.get("category", ""),
            entry.get("namespace", ""),
            entry.get("name", ""),
            entry.get("kind", ""),
            entry.get("signature", ""),
        ): entry
        for entry in inventory.get("items", [])
    }
    errors: list[str] = []
    for expected in collect_def_stdlib(root):
        key = (
            expected.get("category", ""),
            expected.get("namespace", ""),
            expected.get("name", ""),
            expected.get("kind", ""),
            expected.get("signature", ""),
        )
        actual = actual_by_key.get(key)
        label = f"{expected.get('namespace')}.{expected.get('name')}"
        if actual is None:
            errors.append(f".def API symbol missing from source inventory: {label}")
            continue
        source = actual.get("source", "")
        if not source.startswith("stdlib/defs/"):
            errors.append(
                f".def API symbol must use .def source, not {source or '<missing>'}: {label}"
            )
    return errors


def check_stdlib_builtin_dump_residue(inventory: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    for entry in inventory.get("items", []):
        if entry.get("doc_surface") != "stdlib" or entry.get("source") != "xray builtin-dump":
            continue
        module = entry.get("doc_module") or entry.get("namespace")
        name = entry.get("name", "")
        errors.append(
            f"stdlib API symbol must have a source declaration, not builtin-dump: {module}.{name}"
        )
    return errors


def check_language_spec_stdlib_residue(root: Path) -> list[str]:
    stale_patterns = (
        (
            "MCP knowledge fetches API signatures via `xray builtin-dump`",
            "MCP stdlib docs must describe source-derived inventory, not builtin-dump only",
        ),
        (
            "MCP knowledge 通过 `xray builtin-dump` 获取 API 签名",
            "MCP stdlib docs must describe source-derived inventory, not builtin-dump only",
        ),
        (
            "**Authoritative native module list**",
            "stdlib module docs must not call pure-Xray migrated modules native-only",
        ),
        (
            "**真实 native 模块清单**",
            "stdlib module docs must not call pure-Xray migrated modules native-only",
        ),
        (
            "`http` | HTTP / HTTPS client + server + HTTP/2 | `get` `post` `request` `Server`",
            "LANGUAGE_SPEC still lists removed HTTP convenience/server facade APIs",
        ),
        (
            "`http` | HTTP / HTTPS 客户端 + 服务端 + HTTP/2 | `get` `post` `request` `Server`",
            "LANGUAGE_SPEC still lists removed HTTP convenience/server facade APIs",
        ),
    )
    checked_paths = (
        root / "spec/source/sections/016-15-standard-library.md",
        root / "LANGUAGE_SPEC.md",
        root / "LANGUAGE_SPEC_CN.md",
    )
    errors: list[str] = []
    for path in checked_paths:
        if not path.exists():
            continue
        text = path.read_text(encoding="utf-8")
        for pattern, message in stale_patterns:
            if pattern in text:
                errors.append(f"{rel(root, path)}: {message}: {pattern!r}")
    return errors


def render_html(inventory: dict[str, Any]) -> str:
    data = json.dumps(inventory["items"], ensure_ascii=False)
    count = inventory["counts"]["items"]
    stdlib_count = inventory["counts"]["stdlib_symbols"]
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Xray API Inventory</title>
<style>
:root {{
  color-scheme: light;
  --bg: #f7f8fb;
  --panel: #ffffff;
  --text: #172033;
  --muted: #667085;
  --line: #d8dee9;
  --accent: #147d64;
  --accent-soft: #e5f4ef;
  --warn: #9a5b00;
}}
* {{ box-sizing: border-box; }}
body {{
  margin: 0;
  background: var(--bg);
  color: var(--text);
  font-family: ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
  font-size: 14px;
}}
header {{
  padding: 22px 28px 14px;
  border-bottom: 1px solid var(--line);
  background: var(--panel);
}}
h1 {{
  margin: 0 0 8px;
  font-size: 22px;
  font-weight: 700;
  letter-spacing: 0;
}}
.meta {{
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  color: var(--muted);
}}
.pill {{
  border: 1px solid var(--line);
  border-radius: 6px;
  padding: 4px 8px;
  background: #fff;
}}
main {{ padding: 16px 28px 28px; }}
.toolbar {{
  display: grid;
  grid-template-columns: minmax(240px, 1fr) 180px 180px 150px;
  gap: 10px;
  margin-bottom: 14px;
}}
input, select {{
  width: 100%;
  border: 1px solid var(--line);
  border-radius: 6px;
  padding: 9px 10px;
  background: #fff;
  color: var(--text);
  font: inherit;
}}
.table-wrap {{
  overflow: auto;
  border: 1px solid var(--line);
  border-radius: 8px;
  background: var(--panel);
  max-height: calc(100vh - 170px);
}}
table {{
  width: 100%;
  border-collapse: separate;
  border-spacing: 0;
  min-width: 1180px;
}}
th, td {{
  padding: 8px 10px;
  border-bottom: 1px solid #edf0f4;
  vertical-align: top;
  text-align: left;
}}
th {{
  position: sticky;
  top: 0;
  z-index: 1;
  background: #f0f3f7;
  color: #344054;
  font-weight: 650;
}}
tbody tr:hover {{ background: #f8fbfa; }}
code {{
  font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, "Liberation Mono", monospace;
  font-size: 12px;
}}
.kind {{
  display: inline-block;
  border-radius: 5px;
  padding: 2px 6px;
  background: var(--accent-soft);
  color: var(--accent);
  white-space: nowrap;
}}
.undoc {{ color: var(--warn); }}
.source {{ color: var(--muted); font-size: 12px; }}
.empty {{
  padding: 28px;
  color: var(--muted);
  text-align: center;
}}
@media (max-width: 900px) {{
  header, main {{ padding-left: 16px; padding-right: 16px; }}
  .toolbar {{ grid-template-columns: 1fr; }}
  .table-wrap {{ max-height: none; }}
}}
</style>
</head>
<body>
<header>
  <h1>Xray API Inventory</h1>
  <div class="meta">
    <span class="pill">{count} source-derived items</span>
    <span class="pill">{stdlib_count} stdlib/documented-surface symbols</span>
    <span class="pill">Generated by scripts/gen_api_inventory.py</span>
  </div>
</header>
<main>
  <div class="toolbar">
    <input id="search" type="search" placeholder="Search name, signature, summary, source">
    <select id="category"><option value="">All categories</option></select>
    <select id="namespace"><option value="">All namespaces</option></select>
    <select id="docs"><option value="">All doc states</option><option value="stdlib">Stdlib documented surface</option><option value="none">No stdlib doc surface</option></select>
  </div>
  <div class="table-wrap">
    <table>
      <thead>
        <tr>
          <th>Category</th>
          <th>Namespace</th>
          <th>Name</th>
          <th>Kind</th>
          <th>Signature</th>
          <th>Summary</th>
          <th>Docs</th>
          <th>Source</th>
        </tr>
      </thead>
      <tbody id="rows"></tbody>
    </table>
    <div id="empty" class="empty" hidden>No matching API items.</div>
  </div>
</main>
<script>
const ITEMS = {data};
const rows = document.getElementById('rows');
const empty = document.getElementById('empty');
const search = document.getElementById('search');
const category = document.getElementById('category');
const namespace = document.getElementById('namespace');
const docs = document.getElementById('docs');

function esc(value) {{
  return String(value ?? '').replace(/[&<>"']/g, ch => ({{
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
  }}[ch]));
}}

function fillSelect(select, values) {{
  for (const value of values) {{
    const option = document.createElement('option');
    option.value = value;
    option.textContent = value;
    select.appendChild(option);
  }}
}}

fillSelect(category, [...new Set(ITEMS.map(i => i.category))].sort());
fillSelect(namespace, [...new Set(ITEMS.map(i => i.namespace).filter(Boolean))].sort());

function matches(item) {{
  const q = search.value.trim().toLowerCase();
  if (category.value && item.category !== category.value) return false;
  if (namespace.value && item.namespace !== namespace.value) return false;
  if (docs.value === 'stdlib' && item.doc_surface !== 'stdlib') return false;
  if (docs.value === 'none' && item.doc_surface === 'stdlib') return false;
  if (!q) return true;
  const haystack = [
    item.category, item.namespace, item.name, item.qualified, item.kind,
    item.signature, item.summary, item.source, item.doc_module
  ].join(' ').toLowerCase();
  return haystack.includes(q);
}}

function render() {{
  const shown = ITEMS.filter(matches);
  rows.innerHTML = shown.map(item => `
    <tr>
      <td>${{esc(item.category)}}</td>
      <td><code>${{esc(item.namespace)}}</code></td>
      <td><code>${{esc(item.qualified)}}</code></td>
      <td><span class="kind">${{esc(item.kind)}}</span></td>
      <td><code>${{esc(item.signature)}}</code></td>
      <td>${{esc(item.summary)}}</td>
      <td>${{item.doc_surface === 'stdlib' ? esc(item.doc_module) : '<span class="undoc">not stdlib docs</span>'}}</td>
      <td class="source">${{esc(item.source)}}:${{esc(item.line)}}</td>
    </tr>
  `).join('');
  empty.hidden = shown.length !== 0;
}}

[search, category, namespace, docs].forEach(el => el.addEventListener('input', render));
render();
</script>
</body>
</html>
"""


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--xray", type=Path, default=None, help="xray executable for builtin-dump")
    parser.add_argument("--builtin-dump", type=Path, default=None, help="JSON from `xray builtin-dump`")
    parser.add_argument("--json", type=Path, default=None, help="write inventory JSON")
    parser.add_argument("--html", type=Path, default=None, help="write interactive HTML inventory")
    parser.add_argument("--compare-json", type=Path, default=None, help="baseline inventory JSON to diff")
    parser.add_argument("--diff-json", type=Path, default=None, help="write API diff JSON report")
    parser.add_argument("--fail-on-breaking", action="store_true", help="return nonzero on breaking API diff")
    parser.add_argument("--check-docs", action="store_true", help="fail if source API modules lack docs")
    args = parser.parse_args(argv)

    root = args.root.resolve()
    inventory = build_inventory(root, args.xray, args.builtin_dump)

    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(inventory, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    if args.html:
        args.html.parent.mkdir(parents=True, exist_ok=True)
        args.html.write_text(render_html(inventory), encoding="utf-8")

    diff_report: dict[str, Any] | None = None
    if args.compare_json:
        old_inventory = json.loads(args.compare_json.read_text(encoding="utf-8"))
        diff_report = compare_api_inventories(old_inventory, inventory)
        if args.diff_json:
            args.diff_json.parent.mkdir(parents=True, exist_ok=True)
            args.diff_json.write_text(
                json.dumps(diff_report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
            )

    errors: list[str] = []
    if args.check_docs:
        errors.extend(check_docs(root, inventory))
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    if diff_report is not None and args.fail_on_breaking and diff_report["counts"]["breaking"]:
        for change in diff_report["changes"]:
            if change["severity"] != "breaking":
                continue
            print(
                f"{change['change']}: {change['qualified']} "
                f"{change['old_signature']!r} -> {change['new_signature']!r}",
                file=sys.stderr,
            )
        return 1
    if diff_report is not None and not args.diff_json and not args.json and not args.html:
        print(json.dumps(diff_report, indent=2, ensure_ascii=False))
    elif diff_report is None and not args.json and not args.html:
        print(json.dumps(inventory, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

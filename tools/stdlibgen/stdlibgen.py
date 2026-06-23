#!/usr/bin/env python3
"""
Generate stdlib metadata from declarative .def files.

The .def files are the source of truth for AOT direct-call, module-constant,
handle declaration, type-method, and link-manifest metadata. The parser
intentionally stays small: module/function/constant/handle/type-method blocks
with key: value properties, no embedded code, and generated C artifacts that
are checked into the repository.
"""

from __future__ import annotations

import argparse
import ast
import dataclasses
import difflib
import re
import sys
from pathlib import Path


CAP_BITS = {
    "coro": "XAOT_STDLIB_CAP_CORO",
    "timer": "XAOT_STDLIB_CAP_TIMER",
    "channel": "XAOT_STDLIB_CAP_CHANNEL",
    "netpoll": "XAOT_STDLIB_CAP_NETPOLL",
    "task": "XAOT_STDLIB_CAP_TASK",
    "work_queue": "XAOT_STDLIB_CAP_WORK_QUEUE",
    "result_group": "XAOT_STDLIB_CAP_RESULT_GROUP",
    "objects": "XAOT_STDLIB_CAP_OBJECTS",
    "deep_copy": "XAOT_STDLIB_CAP_DEEP_COPY",
    "exception": "XAOT_STDLIB_CAP_EXCEPTION",
    "reflection": "XAOT_STDLIB_CAP_REFLECTION",
    "stacktrace": "XAOT_STDLIB_CAP_STACKTRACE",
    "instanceof": "XAOT_STDLIB_CAP_INSTANCEOF",
    "scope": "XAOT_STDLIB_CAP_SCOPE",
}


@dataclasses.dataclass(frozen=True)
class StdlibEntry:
    module: str
    name: str
    signature: str
    doc: str
    vm: str
    vm_binding: str
    vm_ifdef: str
    aot: str
    argc: str
    arg_spec: str
    ret: str
    aot_direct: bool
    aot_kind: str
    link_object: str
    define: str
    layer: str
    caps: tuple[str, ...]

    @property
    def symbol(self) -> str:
        return f"{self.module}.{self.name}"


@dataclasses.dataclass(frozen=True)
class StdlibConstEntry:
    module: str
    name: str
    signature: str
    doc: str
    vm: str
    vm_value: str
    aot: str
    aot_const_kind: str
    value: int
    f64_value: str
    link_object: str
    define: str
    layer: str
    caps: tuple[str, ...]

    @property
    def symbol(self) -> str:
        return f"{self.module}.{self.name}"


@dataclasses.dataclass(frozen=True)
class StdlibHandleFieldEntry:
    name: str
    type: str
    is_const: bool


@dataclasses.dataclass(frozen=True)
class StdlibHandleEntry:
    module: str
    name: str
    doc: str
    fields: tuple[StdlibHandleFieldEntry, ...]

    @property
    def symbol(self) -> str:
        return f"{self.module}.{self.name}"


@dataclasses.dataclass(frozen=True)
class StdlibTypeMethodEntry:
    module: str
    type_name: str
    name: str
    signature: str
    doc: str

    @property
    def symbol(self) -> str:
        return f"{self.type_name}.{self.name}"


def strip_comment(line: str) -> str:
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
        if ch == "#" and not in_string:
            break
        out.append(ch)
    return "".join(out).strip()


def parse_scalar(raw: str):
    raw = raw.strip()
    if not raw:
        return ""
    if raw in {"true", "false"}:
        return raw == "true"
    if raw[0] in {'"', "'"}:
        return ast.literal_eval(raw)
    if re.fullmatch(r"-?[0-9]+", raw):
        return int(raw)
    return raw


HANDLE_FIELD_RE = re.compile(
    r"(const\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*:\s*"
    r"([A-Za-z_][A-Za-z0-9_]*(?:<[^,{}]+>)?\??)"
)


def parse_handle_fields(raw: str, context: str) -> tuple[StdlibHandleFieldEntry, ...]:
    fields: list[StdlibHandleFieldEntry] = []
    covered: list[tuple[int, int]] = []
    for match in HANDLE_FIELD_RE.finditer(raw):
        fields.append(
            StdlibHandleFieldEntry(
                name=match.group(2),
                type=match.group(3),
                is_const=match.group(1) is not None,
            )
        )
        covered.append(match.span())
    if not fields:
        raise SystemExit(f"{context}: handle requires at least one field")

    # Validate that the regex did not silently skip malformed fragments.
    cursor = 0
    for start, end in covered:
        skipped = raw[cursor:start].strip()
        if skipped and skipped != ",":
            raise SystemExit(f"{context}: malformed handle field fragment: {skipped!r}")
        cursor = end
    tail = raw[cursor:].strip()
    if tail and tail != ",":
        raise SystemExit(f"{context}: malformed handle field fragment: {tail!r}")
    return tuple(fields)


def parse_def_metadata(
    root: Path,
) -> tuple[
    list[StdlibEntry],
    list[StdlibConstEntry],
    list[StdlibHandleEntry],
    list[StdlibTypeMethodEntry],
]:
    defs_dir = root / "stdlib" / "defs"
    entries: list[StdlibEntry] = []
    constants: list[StdlibConstEntry] = []
    handles: list[StdlibHandleEntry] = []
    type_methods: list[StdlibTypeMethodEntry] = []
    if not defs_dir.exists():
        raise SystemExit(f"missing stdlib defs directory: {defs_dir}")

    current_module: str | None = None
    current_kind: str | None = None
    current_name: str | None = None
    props: dict[str, object] = {}

    def finish_entry(path: Path, line_no: int) -> None:
        nonlocal current_kind, current_name, props
        if current_module is None or current_kind is None or current_name is None:
            return
        caps_raw = str(props.get("caps", ""))
        caps = tuple(c for c in (s.strip() for s in caps_raw.split(",")) if c)
        link_raw = props.get("link_object", False)
        if link_raw is True:
            link_object = f"{current_module}.{current_name}"
        elif link_raw is False:
            link_object = ""
        else:
            link_object = str(link_raw)

        if current_kind == "fn":
            missing = [k for k in ("signature", "doc", "vm", "argc") if k not in props]
            if missing:
                names = ", ".join(missing)
                raise SystemExit(f"{path}:{line_no}: {current_module}.{current_name} missing {names}")
            aot_direct = bool(props.get("aot_direct", False))
            aot_kind = str(props.get("aot_kind", "method" if aot_direct else ""))
            if aot_kind and aot_kind not in {"method", "builtin"}:
                raise SystemExit(
                    f"{path}:{line_no}: unsupported aot_kind for {current_module}.{current_name}: {aot_kind}"
                )
            if aot_kind and not aot_direct:
                raise SystemExit(
                    f"{path}:{line_no}: {current_module}.{current_name} aot_kind requires aot_direct: true"
                )
            vm_binding = str(props.get("vm_binding", "normal"))
            if vm_binding not in {"normal", "yieldable", "slow"}:
                raise SystemExit(
                    f"{path}:{line_no}: unsupported vm_binding for "
                    f"{current_module}.{current_name}: {vm_binding}"
                )
            vm_ifdef = str(props.get("vm_ifdef", ""))
            if vm_ifdef and not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", vm_ifdef):
                raise SystemExit(
                    f"{path}:{line_no}: unsupported vm_ifdef for "
                    f"{current_module}.{current_name}: {vm_ifdef}"
                )

            entries.append(
                StdlibEntry(
                    module=current_module,
                    name=current_name,
                    signature=str(props["signature"]),
                    doc=str(props["doc"]),
                    vm=str(props["vm"]),
                    vm_binding=vm_binding,
                    vm_ifdef=vm_ifdef,
                    aot=str(props.get("aot", "")),
                    argc=str(props["argc"]),
                    arg_spec=str(props.get("arg_spec", "")),
                    ret=str(props.get("ret", "value")),
                    aot_direct=aot_direct,
                    aot_kind=aot_kind,
                    link_object=link_object,
                    define=str(props.get("define", "")),
                    layer=str(props.get("layer", "")),
                    caps=caps,
                )
            )
        elif current_kind == "const":
            missing = [k for k in ("signature", "doc", "vm", "aot_const") if k not in props]
            if missing:
                names = ", ".join(missing)
                raise SystemExit(f"{path}:{line_no}: {current_module}.{current_name} missing {names}")
            aot_const_kind = str(props["aot_const"])
            if aot_const_kind not in {"helper_value", "int64", "float64"}:
                raise SystemExit(
                    f"{path}:{line_no}: unsupported aot_const for {current_module}.{current_name}: {aot_const_kind}"
                )
            if aot_const_kind == "helper_value" and not str(props.get("aot", "")):
                raise SystemExit(
                    f"{path}:{line_no}: {current_module}.{current_name} helper_value requires aot"
                )
            value = props.get("value", 0)
            if aot_const_kind == "int64" and not isinstance(value, int):
                raise SystemExit(
                    f"{path}:{line_no}: {current_module}.{current_name} int64 constant requires integer value"
                )
            f64_value = "0.0"
            if aot_const_kind == "float64":
                f64_value = c_float_const_expr(
                    str(value), f"{current_module}.{current_name} float64 constant value"
                )
            constants.append(
                StdlibConstEntry(
                    module=current_module,
                    name=current_name,
                    signature=str(props["signature"]),
                    doc=str(props["doc"]),
                    vm=str(props["vm"]),
                    vm_value=c_vm_value_expr(str(props.get("vm_value", "")), f"{current_module}.{current_name}"),
                    aot=str(props.get("aot", "")),
                    aot_const_kind=aot_const_kind,
                    value=int(value) if isinstance(value, int) else 0,
                    f64_value=f64_value,
                    link_object=link_object,
                    define=str(props.get("define", "")),
                    layer=str(props.get("layer", "")),
                    caps=caps,
                )
            )
        elif current_kind == "handle":
            missing = [k for k in ("fields",) if k not in props]
            if missing:
                names = ", ".join(missing)
                raise SystemExit(f"{path}:{line_no}: {current_module}.{current_name} missing {names}")
            fields = parse_handle_fields(
                str(props["fields"]), f"{path}:{line_no}: {current_module}.{current_name}"
            )
            handles.append(
                StdlibHandleEntry(
                    module=current_module,
                    name=current_name,
                    doc=str(props.get("doc", "Native handle type")),
                    fields=fields,
                )
            )
        elif current_kind == "type_method":
            missing = [k for k in ("signature", "doc") if k not in props]
            if missing:
                names = ", ".join(missing)
                raise SystemExit(f"{path}:{line_no}: {current_module}.{current_name} missing {names}")
            type_name, method_name = str(current_name).split(".", 1)
            type_methods.append(
                StdlibTypeMethodEntry(
                    module=current_module,
                    type_name=type_name,
                    name=method_name,
                    signature=str(props["signature"]),
                    doc=str(props["doc"]),
                )
            )
        else:
            raise SystemExit(f"{path}:{line_no}: unsupported block kind: {current_kind}")
        current_kind = None
        current_name = None
        props = {}

    for path in sorted(defs_dir.glob("*.def")):
        for line_no, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            line = strip_comment(raw_line)
            if not line:
                continue
            m = re.fullmatch(r"module\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{", line)
            if m:
                if current_module is not None or current_kind is not None:
                    raise SystemExit(f"{path}:{line_no}: nested module block")
                current_module = m.group(1)
                continue
            m = re.fullmatch(r"fn\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{", line)
            if m:
                if current_module is None or current_kind is not None:
                    raise SystemExit(f"{path}:{line_no}: fn outside module or nested fn")
                current_kind = "fn"
                current_name = m.group(1)
                props = {}
                continue
            m = re.fullmatch(r"const\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{", line)
            if m:
                if current_module is None or current_kind is not None:
                    raise SystemExit(f"{path}:{line_no}: const outside module or nested const")
                current_kind = "const"
                current_name = m.group(1)
                props = {}
                continue
            m = re.fullmatch(r"handle\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{", line)
            if m:
                if current_module is None or current_kind is not None:
                    raise SystemExit(f"{path}:{line_no}: handle outside module or nested handle")
                current_kind = "handle"
                current_name = m.group(1)
                props = {}
                continue
            m = re.fullmatch(
                r"type_method\s+([A-Za-z_][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)\s*\{",
                line,
            )
            if m:
                if current_module is None or current_kind is not None:
                    raise SystemExit(
                        f"{path}:{line_no}: type_method outside module or nested type_method"
                    )
                current_kind = "type_method"
                current_name = f"{m.group(1)}.{m.group(2)}"
                props = {}
                continue
            if line == "}":
                if current_kind is not None:
                    finish_entry(path, line_no)
                elif current_module is not None:
                    current_module = None
                else:
                    raise SystemExit(f"{path}:{line_no}: stray closing brace")
                continue
            if current_kind is None:
                raise SystemExit(f"{path}:{line_no}: property outside function/constant block")
            if ":" not in line:
                raise SystemExit(f"{path}:{line_no}: expected key: value")
            key, value = line.split(":", 1)
            props[key.strip()] = parse_scalar(value)

    if current_module is not None or current_kind is not None:
        raise SystemExit("unterminated stdlib .def block")
    return entries, constants, handles, type_methods


def parse_defs(root: Path) -> list[StdlibEntry]:
    entries, _, _, _ = parse_def_metadata(root)
    return entries


def parse_constants(root: Path) -> list[StdlibConstEntry]:
    _, constants, _, _ = parse_def_metadata(root)
    return constants


def parse_handles(root: Path) -> list[StdlibHandleEntry]:
    _, _, handles, _ = parse_def_metadata(root)
    return handles


def parse_type_methods(root: Path) -> list[StdlibTypeMethodEntry]:
    _, _, _, type_methods = parse_def_metadata(root)
    return type_methods


def c_string(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def c_ident(value: str, context: str) -> str:
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", value):
        raise SystemExit(f"{context}: expected C identifier, got {value!r}")
    return value


_C_IDENT_RE = r"[A-Za-z_][A-Za-z0-9_]*"
_C_NUM_RE = r"(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?"
_C_FLOAT_ATOM_RE = rf"(?:{_C_IDENT_RE}|{_C_NUM_RE})"
_C_FLOAT_EXPR_RE = rf"-?{_C_FLOAT_ATOM_RE}(?:\s*[*+/-]\s*-?{_C_FLOAT_ATOM_RE})?"


def c_float_const_expr(value: str, context: str) -> str:
    value = value.strip()
    if not value or not re.fullmatch(_C_FLOAT_EXPR_RE, value):
        raise SystemExit(f"{context}: unsupported C float constant expression: {value!r}")
    return value


def c_vm_value_expr(value: str, context: str) -> str:
    if not value:
        return ""
    allowed = (
        r"xr_int\([A-Za-z_][A-Za-z0-9_]*\)"
        rf"|xr_float\({_C_FLOAT_EXPR_RE}\)"
        r"|xrs_string_value_c\(isolate, [A-Za-z_][A-Za-z0-9_]*(?:\(\))?\)"
    )
    if not re.fullmatch(allowed, value):
        raise SystemExit(f"{context}: unsupported VM constant value expression: {value!r}")
    return value


def c_i64_literal(value: int) -> str:
    if value == -(2**63):
        return "INT64_MIN"
    if value == 2**63 - 1:
        return "INT64_MAX"
    return f"INT64_C({value})"


def generated_header(title: str) -> list[str]:
    return [
        "/*",
        " * AUTO-GENERATED FILE - DO NOT EDIT",
        " * Generated by tools/stdlibgen/stdlibgen.py",
        " *",
        f" * {title}",
        " */",
        "",
        "/* clang-format off */",
        "",
    ]


def argc_expr(entry: StdlibEntry) -> str:
    return "CG_AOT_STDLIB_VARIADIC" if entry.argc == "variadic" else entry.argc


def ret_expr(entry: StdlibEntry) -> str:
    if entry.ret == "value":
        return "CG_AOT_RET_VALUE"
    if entry.ret == "str_borrowed":
        return "CG_AOT_RET_STR_BORROWED"
    raise SystemExit(f"unsupported ret kind for {entry.symbol}: {entry.ret}")


def const_kind_expr(entry: StdlibConstEntry) -> str:
    if entry.aot_const_kind == "helper_value":
        return "CG_AOT_STDLIB_CONST_HELPER_VALUE"
    if entry.aot_const_kind == "int64":
        return "CG_AOT_STDLIB_CONST_I64"
    if entry.aot_const_kind == "float64":
        return "CG_AOT_STDLIB_CONST_F64"
    raise SystemExit(f"unsupported aot_const kind for {entry.symbol}: {entry.aot_const_kind}")


def emit_aot_methods(entries: list[StdlibEntry], constants: list[StdlibConstEntry]) -> str:
    rows = [e for e in entries if e.aot_direct and e.aot_kind == "method"]
    builtin_rows = [e for e in entries if e.aot_direct and e.aot_kind == "builtin"]
    const_rows = [c for c in constants if c.aot_const_kind]
    lines = generated_header("xstdlib_aot_methods_generated.inc.c - AOT stdlib direct-call table")
    lines.append("static const CgAotStdlibMethod g_aot_stdlib_generated_methods[] = {")
    for e in rows:
        if not e.aot:
            raise SystemExit(f"{e.symbol}: aot_direct requires aot symbol")
        lines.append(
            "    {"
            f"{c_string(e.module)}, {c_string(e.name)}, {argc_expr(e)}, {c_string(e.aot)}, "
            f"{c_string(e.arg_spec)}, {ret_expr(e)}, NULL"
            "},"
        )
    lines.append("};")
    lines.append(
        "#define CG_AOT_STDLIB_GENERATED_METHOD_COUNT "
        "((int) (sizeof(g_aot_stdlib_generated_methods) / "
        "sizeof(g_aot_stdlib_generated_methods[0])))"
    )
    lines.append("")
    lines.extend(
        [
            "typedef enum CgAotStdlibConstKind {",
            "    CG_AOT_STDLIB_CONST_I64,",
            "    CG_AOT_STDLIB_CONST_F64,",
            "    CG_AOT_STDLIB_CONST_HELPER_VALUE,",
            "} CgAotStdlibConstKind;",
            "",
            "typedef struct CgAotStdlibConst {",
            "    const char *module;",
            "    const char *name;",
            "    CgAotStdlibConstKind kind;",
            "    const char *helper;",
            "    const char *f64_expr;",
            "    int64_t i64_value;",
            "} CgAotStdlibConst;",
            "",
            "static const CgAotStdlibConst g_aot_stdlib_generated_consts[] = {",
        ]
    )
    for c in const_rows:
        lines.append(
            "    {"
            f"{c_string(c.module)}, {c_string(c.name)}, {const_kind_expr(c)}, "
            f"{c_string(c.aot)}, {c_string(c.f64_value)}, {c_i64_literal(c.value)}"
            "},"
        )
    lines.append("};")
    lines.append(
        "#define CG_AOT_STDLIB_GENERATED_CONST_COUNT "
        "((int) (sizeof(g_aot_stdlib_generated_consts) / "
        "sizeof(g_aot_stdlib_generated_consts[0])))"
    )
    lines.append("")
    lines.append("static const CgAotStdlibConst *cg_aot_stdlib_generated_const_at(int index) {")
    lines.append("    if (index < 0 || index >= CG_AOT_STDLIB_GENERATED_CONST_COUNT)")
    lines.append("        return NULL;")
    lines.append("    return &g_aot_stdlib_generated_consts[index];")
    lines.append("}")
    lines.append("")
    lines.append("static bool cg_aot_stdlib_generated_module_has_constants(const char *module) {")
    lines.append("    if (!module)")
    lines.append("        return false;")
    lines.append("    for (int i = 0; i < CG_AOT_STDLIB_GENERATED_CONST_COUNT; i++) {")
    lines.append("        const CgAotStdlibConst *c = &g_aot_stdlib_generated_consts[i];")
    lines.append("        if (strcmp(module, c->module) == 0)")
    lines.append("            return true;")
    lines.append("    }")
    lines.append("    return false;")
    lines.append("}")
    lines.append("")
    lines.append(
        "static const CgAotStdlibConst *cg_aot_stdlib_generated_const_for_member("
        "const char *module, const char *name) {"
    )
    lines.append("    if (!module || !name)")
    lines.append("        return NULL;")
    lines.append("    for (int i = 0; i < CG_AOT_STDLIB_GENERATED_CONST_COUNT; i++) {")
    lines.append("        const CgAotStdlibConst *c = &g_aot_stdlib_generated_consts[i];")
    lines.append("        if (strcmp(module, c->module) == 0 && strcmp(name, c->name) == 0)")
    lines.append("            return c;")
    lines.append("    }")
    lines.append("    return NULL;")
    lines.append("}")
    lines.append("")
    lines.append(
        "static bool cg_aot_stdlib_generated_has_builtin_direct_call(const char *module, "
        "const char *name) {"
    )
    lines.append("    if (!module || !name)")
    lines.append("        return false;")
    for e in builtin_rows:
        lines.append(
            f"    if (strcmp(module, {c_string(e.module)}) == 0 && "
            f"strcmp(name, {c_string(e.name)}) == 0)\n"
            "        return true;"
        )
    lines.append("    return false;")
    lines.append("}")
    lines.append("")
    lines.append("/* clang-format on */")
    lines.append("")
    return "\n".join(lines)


def emit_driver_metadata(entries: list[StdlibEntry], constants: list[StdlibConstEntry]) -> str:
    symbol_entries = [*entries, *constants]
    object_rows = list({e.symbol: e for e in symbol_entries if e.link_object}.values())
    define_rows = list({e.symbol: e for e in symbol_entries if e.define}.values())
    cap_rows = list({e.symbol: e for e in symbol_entries if e.caps}.values())
    builtin_rows = list(
        {e.symbol: e for e in entries if e.aot_direct and e.aot_kind == "builtin"}.values()
    )
    const_rows = list({c.symbol: c for c in constants if c.aot_const_kind}.values())
    lines = generated_header("xaot_stdlib_generated.inc.c - AOT stdlib driver metadata")
    lines.extend(
        [
            "enum {",
            "    XAOT_STDLIB_CAP_CORO = 1u << 0,",
            "    XAOT_STDLIB_CAP_TIMER = 1u << 1,",
            "    XAOT_STDLIB_CAP_CHANNEL = 1u << 2,",
            "    XAOT_STDLIB_CAP_NETPOLL = 1u << 3,",
            "    XAOT_STDLIB_CAP_TASK = 1u << 4,",
            "    XAOT_STDLIB_CAP_WORK_QUEUE = 1u << 5,",
            "    XAOT_STDLIB_CAP_RESULT_GROUP = 1u << 6,",
            "    XAOT_STDLIB_CAP_OBJECTS = 1u << 7,",
            "    XAOT_STDLIB_CAP_DEEP_COPY = 1u << 8,",
            "    XAOT_STDLIB_CAP_EXCEPTION = 1u << 9,",
            "    XAOT_STDLIB_CAP_REFLECTION = 1u << 10,",
            "    XAOT_STDLIB_CAP_STACKTRACE = 1u << 11,",
            "    XAOT_STDLIB_CAP_INSTANCEOF = 1u << 12,",
            "    XAOT_STDLIB_CAP_SCOPE = 1u << 13,",
            "};",
            "",
        ]
    )
    lines.append("static const char *xaot_stdlib_generated_object_for_symbol(const char *symbol) {")
    lines.append("    if (!symbol)")
    lines.append("        return NULL;")
    for e in object_rows:
        lines.append(
            f"    if (strcmp(symbol, {c_string(e.symbol)}) == 0)\n"
            f"        return {c_string(e.link_object)};"
        )
    lines.append("    return NULL;")
    lines.append("}")
    lines.append("")
    lines.append("static const char *xaot_stdlib_generated_define_for_symbol(const char *symbol) {")
    lines.append("    if (!symbol)")
    lines.append("        return NULL;")
    for e in define_rows:
        lines.append(
            f"    if (strcmp(symbol, {c_string(e.symbol)}) == 0)\n"
            f"        return {c_string(e.define)};"
        )
    lines.append("    return NULL;")
    lines.append("}")
    lines.append("")
    lines.append("static uint32_t xaot_stdlib_generated_caps_for_symbol(const char *symbol) {")
    lines.append("    if (!symbol)")
    lines.append("        return 0;")
    for e in cap_rows:
        unknown = [cap for cap in e.caps if cap not in CAP_BITS]
        if unknown:
            raise SystemExit(f"{e.symbol}: unknown caps: {', '.join(unknown)}")
        cap_expr = " | ".join(CAP_BITS[cap] for cap in e.caps) if e.caps else "0"
        lines.append(f"    if (strcmp(symbol, {c_string(e.symbol)}) == 0)\n        return {cap_expr};")
    lines.append("    return 0;")
    lines.append("}")
    lines.append("")
    lines.append("static bool xaot_stdlib_generated_symbol_is_builtin_direct(const char *symbol) {")
    lines.append("    if (!symbol)")
    lines.append("        return false;")
    for e in builtin_rows:
        lines.append(f"    if (strcmp(symbol, {c_string(e.symbol)}) == 0)\n        return true;")
    lines.append("    return false;")
    lines.append("}")
    lines.append("")
    lines.append("static bool xaot_stdlib_generated_symbol_is_constant(const char *symbol) {")
    lines.append("    if (!symbol)")
    lines.append("        return false;")
    for c in const_rows:
        lines.append(f"    if (strcmp(symbol, {c_string(c.symbol)}) == 0)\n        return true;")
    lines.append("    return false;")
    lines.append("}")
    lines.append("")
    lines.append("/* clang-format on */")
    lines.append("")
    return "\n".join(lines)


def emit_vm_bindings(entries: list[StdlibEntry], constants: list[StdlibConstEntry]) -> str:
    rows_by_module: dict[str, list[StdlibEntry]] = {}
    consts_by_module: dict[str, list[StdlibConstEntry]] = {}
    seen: dict[tuple[str, str, str], tuple[str, str]] = {}
    for e in entries:
        key = (e.module, e.name, e.vm)
        existing = seen.get(key)
        if existing is not None:
            existing_binding, existing_ifdef = existing
            if existing_binding != e.vm_binding or existing_ifdef != e.vm_ifdef:
                raise SystemExit(
                    f"{e.symbol}: duplicate VM binding rows disagree: "
                    f"{existing_binding}/{existing_ifdef} vs {e.vm_binding}/{e.vm_ifdef}"
                )
            continue
        seen[key] = (e.vm_binding, e.vm_ifdef)
        c_ident(e.vm, e.symbol)
        rows_by_module.setdefault(e.module, []).append(e)
    for c in constants:
        if c.vm_value:
            consts_by_module.setdefault(c.module, []).append(c)

    lines = generated_header("xstdlib_vm_bindings_generated.inc.c - VM stdlib binding shell")
    lines.extend(
        [
            "/*",
            " * Include this file from a stdlib module TU after stdlib/common.h",
            " * and after the module's static",
            " * C functions have been declared, then define exactly one",
            " * XR_STDLIB_VM_BIND_MODULE_<MODULE> macro before including it.",
            " */",
            "",
        ]
    )
    for module in sorted(set(rows_by_module) | set(consts_by_module)):
        macro = f"XR_STDLIB_VM_BIND_MODULE_{module.upper()}"
        func = f"xr_stdlib_vm_bind_{module}_generated"
        lines.append(f"#ifdef {macro}")
        lines.append(f"static void {func}(XrVMRuntime *isolate, XrModule *module) {{")
        for e in rows_by_module.get(module, []):
            export_macro = {
                "normal": "XRS_EXPORT",
                "yieldable": "XRS_EXPORT_YIELDABLE",
                "slow": "XRS_EXPORT_SLOW",
            }[e.vm_binding]
            if e.vm_ifdef:
                lines.append(f"#ifdef {e.vm_ifdef}")
            lines.append(
                f"    {export_macro}(module, isolate, {c_string(e.name)}, "
                f"{c_ident(e.vm, e.symbol)});"
            )
            if e.vm_ifdef:
                lines.append(f"#endif  /* {e.vm_ifdef} */")
        for c in consts_by_module.get(module, []):
            lines.append(
                f"    xr_module_add_export(isolate, module, {c_string(c.name)}, {c.vm_value});"
            )
        lines.append("}")
        lines.append(f"#endif  /* {macro} */")
        lines.append("")
    lines.append("/* clang-format on */")
    lines.append("")
    return "\n".join(lines)


def emit_defs_header(
    entries: list[StdlibEntry],
    constants: list[StdlibConstEntry],
    handles: list[StdlibHandleEntry],
    type_methods: list[StdlibTypeMethodEntry],
) -> str:
    lines = generated_header("xstdlib_defs_generated.h - stdlib declarative metadata")
    lines.extend(
        [
            "#ifndef XSTDLIB_DEFS_GENERATED_H",
            "#define XSTDLIB_DEFS_GENERATED_H",
            "",
            "#include <stdbool.h>",
            "#include <math.h>",
            "#include <stdint.h>",
            "",
            "typedef struct XrStdlibDefEntry {",
            "    const char *module;",
            "    const char *name;",
            "    const char *signature;",
            "    const char *doc;",
            "    const char *vm;",
            "    const char *vm_binding;",
            "    const char *vm_ifdef;",
            "    const char *aot;",
            "    const char *arg_spec;",
            "    const char *ret;",
            "    const char *link_object;",
            "    const char *define;",
            "    const char *layer;",
            "    const char *aot_kind;",
            "    uint16_t argc;",
            "    bool aot_direct;",
            "} XrStdlibDefEntry;",
            "",
            "typedef struct XrStdlibConstDefEntry {",
            "    const char *module;",
            "    const char *name;",
            "    const char *signature;",
            "    const char *doc;",
            "    const char *vm;",
            "    const char *vm_value;",
            "    const char *aot;",
            "    const char *aot_const_kind;",
            "    const char *link_object;",
            "    const char *define;",
            "    const char *layer;",
            "    int64_t value;",
            "    double f64_value;",
            "} XrStdlibConstDefEntry;",
            "",
            "typedef struct XrStdlibHandleFieldDefEntry {",
            "    const char *module;",
            "    const char *handle;",
            "    const char *name;",
            "    const char *type;",
            "    bool is_const;",
            "} XrStdlibHandleFieldDefEntry;",
            "",
            "typedef struct XrStdlibHandleDefEntry {",
            "    const char *module;",
            "    const char *name;",
            "    const char *doc;",
            "    const XrStdlibHandleFieldDefEntry *fields;",
            "    uint16_t field_count;",
            "} XrStdlibHandleDefEntry;",
            "",
            "typedef struct XrStdlibTypeMethodDefEntry {",
            "    const char *module;",
            "    const char *type_name;",
            "    const char *name;",
            "    const char *signature;",
            "    const char *doc;",
            "} XrStdlibTypeMethodDefEntry;",
            "",
            "static const XrStdlibDefEntry xr_stdlib_def_entries[] = {",
        ]
    )
    for e in entries:
        if e.argc == "variadic":
            argc = "UINT16_MAX"
        else:
            argc = e.argc
        lines.append(
            "    {"
            f"{c_string(e.module)}, {c_string(e.name)}, {c_string(e.signature)}, "
            f"{c_string(e.doc)}, {c_string(e.vm)}, {c_string(e.vm_binding)}, "
            f"{c_string(e.vm_ifdef)}, "
            f"{c_string(e.aot)}, {c_string(e.arg_spec)}, {c_string(e.ret)}, "
            f"{c_string(e.link_object)}, {c_string(e.define)}, {c_string(e.layer)}, "
            f"{c_string(e.aot_kind)}, {argc}, "
            f"{'true' if e.aot_direct else 'false'}"
            "},"
        )
    lines.extend(
        [
            "};",
            "#define XR_STDLIB_DEF_ENTRY_COUNT "
            "((uint32_t) (sizeof(xr_stdlib_def_entries) / sizeof(xr_stdlib_def_entries[0])))",
            "",
            "static const XrStdlibConstDefEntry xr_stdlib_const_def_entries[] = {",
        ]
    )
    for c in constants:
        lines.append(
            "    {"
            f"{c_string(c.module)}, {c_string(c.name)}, {c_string(c.signature)}, "
            f"{c_string(c.doc)}, {c_string(c.vm)}, {c_string(c.vm_value)}, {c_string(c.aot)}, "
            f"{c_string(c.aot_const_kind)}, {c_string(c.link_object)}, "
            f"{c_string(c.define)}, {c_string(c.layer)}, {c_i64_literal(c.value)}, {c.f64_value}"
            "},"
        )
    lines.extend(
        [
            "};",
            "#define XR_STDLIB_CONST_DEF_ENTRY_COUNT "
            "((uint32_t) (sizeof(xr_stdlib_const_def_entries) / "
            "sizeof(xr_stdlib_const_def_entries[0])))",
            "",
        ]
    )
    for h in handles:
        field_array = f"xr_stdlib_handle_fields_{h.module}_{h.name}"
        lines.append(f"static const XrStdlibHandleFieldDefEntry {field_array}[] = {{")
        for field in h.fields:
            lines.append(
                "    {"
                f"{c_string(h.module)}, {c_string(h.name)}, {c_string(field.name)}, "
                f"{c_string(field.type)}, {'true' if field.is_const else 'false'}"
                "},"
            )
        lines.append("};")
        lines.append("")
    lines.append("static const XrStdlibHandleDefEntry xr_stdlib_handle_def_entries[] = {")
    for h in handles:
        field_array = f"xr_stdlib_handle_fields_{h.module}_{h.name}"
        lines.append(
            "    {"
            f"{c_string(h.module)}, {c_string(h.name)}, {c_string(h.doc)}, "
            f"{field_array}, {len(h.fields)}"
            "},"
        )
    lines.extend(
        [
            "};",
            "#define XR_STDLIB_HANDLE_DEF_ENTRY_COUNT "
            "((uint32_t) (sizeof(xr_stdlib_handle_def_entries) / "
            "sizeof(xr_stdlib_handle_def_entries[0])))",
            "",
            "static const XrStdlibTypeMethodDefEntry xr_stdlib_type_method_def_entries[] = {",
        ]
    )
    for tm in type_methods:
        lines.append(
            "    {"
            f"{c_string(tm.module)}, {c_string(tm.type_name)}, {c_string(tm.name)}, "
            f"{c_string(tm.signature)}, {c_string(tm.doc)}"
            "},"
        )
    lines.extend(
        [
            "};",
            "#define XR_STDLIB_TYPE_METHOD_DEF_ENTRY_COUNT "
            "((uint32_t) (sizeof(xr_stdlib_type_method_def_entries) / "
            "sizeof(xr_stdlib_type_method_def_entries[0])))",
            "",
            "#endif  /* XSTDLIB_DEFS_GENERATED_H */",
            "",
            "/* clang-format on */",
            "",
        ]
    )
    return "\n".join(lines)


def output_paths(root: Path) -> dict[Path, str]:
    entries, constants, handles, type_methods = parse_def_metadata(root)
    return {
        root / "src" / "aot" / "xstdlib_aot_methods_generated.inc.c": emit_aot_methods(
            entries, constants
        ),
        root / "src" / "aot" / "xaot_stdlib_generated.inc.c": emit_driver_metadata(
            entries, constants
        ),
        root / "src" / "stdlib" / "xstdlib_vm_bindings_generated.inc.c": emit_vm_bindings(
            entries, constants
        ),
        root / "src" / "stdlib" / "xstdlib_defs_generated.h": emit_defs_header(
            entries, constants, handles, type_methods
        ),
    }


def write_outputs(root: Path) -> None:
    for path, content in output_paths(root).items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")


def check_outputs(root: Path) -> int:
    failed = False
    for path, content in output_paths(root).items():
        if not path.exists():
            print(f"missing generated stdlib metadata: {path}", file=sys.stderr)
            failed = True
            continue
        current = path.read_text(encoding="utf-8")
        if current != content:
            print(f"stale generated stdlib metadata: {path}", file=sys.stderr)
            diff = difflib.unified_diff(
                current.splitlines(),
                content.splitlines(),
                fromfile=str(path),
                tofile=f"{path} (regenerated)",
                lineterm="",
            )
            for line in list(diff)[:120]:
                print(line, file=sys.stderr)
            failed = True
    return 1 if failed else 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--check", action="store_true", help="verify generated files are current")
    args = parser.parse_args(argv)

    root = Path(args.root).resolve()
    if args.check:
        return check_outputs(root)
    write_outputs(root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

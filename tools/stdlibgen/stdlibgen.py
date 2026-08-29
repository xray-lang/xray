#!/usr/bin/env python3
"""
Generate stdlib metadata from declarative .def files.

The .def files are the source of truth for AOT direct-call, module-constant,
handle declaration, type-method, native-class binding, and link-manifest
metadata. The parser intentionally stays small: flat blocks with key: value
properties, no embedded code, and generated C artifacts that are checked into
the repository.

Visibility
----------
Every declaration kind carries a visibility, and `is_internal` is the single
question the surface gates ask of an entry. An internal entry keeps its whole
C-side binding -- `vm:`, `aot:`, `argc`, `core_slot`, `native_body` and the
rest still name the same C -- but stops counting as part of the module's
public surface, so an `.xr` semantic source can publish over it.

Three spellings decide it, and they differ per kind on purpose:

* `fn`, `const`, `handle`, `object` and `enum` default to internal when the
  name starts with `__`, because a leaf reached only from the module's own
  `.xr` body wants a name a caller cannot spell anyway. An explicit
  `visibility:` key overrides that default, which is how a shape that must
  keep a spellable name can still leave the public surface.
* `class` and `native_class` have no prefix rule and take an explicit
  `visibility: "internal"` key only. A class name is a user-visible type name
  that reaches diagnostics, LSP completion and error messages, so `__Buffer`
  would be a worse name rather than a private one.
* `type_method`, `class_method` and `class_field` inherit. A member that
  declares no visibility of its own takes the visibility of the `class` or
  `native_class` with the same name in the same module. That way marking a
  class internal is one line rather than one line per member, and a member
  cannot be left publicly reachable on a class nothing can name. A member may
  still say `visibility: "public"` explicitly to opt out. A member whose owner
  is not a declared class -- `Coro.CoroLocal` has `type_method` rows and no
  class row -- stays public.

Because the declaring class may appear after its members in the file, member
inheritance is resolved once, after the whole `.def` set is parsed.
"""

from __future__ import annotations

import argparse
import ast
import dataclasses
import difflib
import hashlib
import re
import sys
import tomllib
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
    "stacktrace": "XAOT_STDLIB_CAP_STACKTRACE",
    "instanceof": "XAOT_STDLIB_CAP_INSTANCEOF",
    "scope": "XAOT_STDLIB_CAP_SCOPE",
}

RUNTIME_CAP_BITS = {
    "coro": "XR_CAP_COROUTINE",
    "timer": "XR_CAP_TIMER",
    "channel": "XR_CAP_CHANNEL",
    "netpoll": "XR_CAP_NETPOLL",
    "task": "XR_CAP_TASK",
    "work_queue": "XR_CAP_WORK_QUEUE",
    "result_group": "XR_CAP_RESULT_GROUP",
    "objects": "XR_CAP_OBJECTS",
    "deep_copy": "XR_CAP_DEEP_COPY",
    "exception": "XR_CAP_EXCEPTION",
    "stacktrace": "XR_CAP_STACKTRACE",
    "instanceof": "XR_CAP_INSTANCEOF",
    "scope": "XR_CAP_SCOPE",
}


FREESTANDING_DIRECT_BUILTINS: set[str] = set()

FREESTANDING_HEADER_ONLY_SYMBOLS = {
    "mem.__fence",
    "mem.__prefetch",
    "mem.__cacheFlush",
    "mem.__cacheInvalidate",
    "mem.__nontemporalStore",
    "mem.__cacheLineSize",
    "mem.__alloc",
    "mem.__allocZeroed",
    "mem.__allocAligned",
    "mem.__copy",
    "mem.__move",
    "mem.__set",
    "mem.__compare",
    "mem.__volatileLoad",
    "mem.__volatileStore",
}

TARGET_LEAF_KINDS = {
    "": "XR_STDLIB_TARGET_LEAF_NONE",
    "i64-getpid": "XR_STDLIB_TARGET_LEAF_I64_GETPID",
}


def validate_target_leaf_source_owner(
    root: Path,
    module: str,
    name: str,
    target_leaf: str,
    visibility: str,
    effect: str,
    allocation: str,
    owners: dict[str, str],
) -> None:
    """Require each target leaf to be one audited private source-owned row."""
    if not target_leaf:
        return
    symbol = f"{module}.{name}"
    if visibility != "internal":
        raise SystemExit(f"{symbol} target_leaf must have internal visibility")
    if effect != "nothrow":
        raise SystemExit(f"{symbol} target_leaf must declare effect = nothrow")
    if allocation != "no_heap":
        raise SystemExit(f"{symbol} target_leaf must declare allocation = no_heap")
    canonical_source = root / "stdlib" / module / f"{module}.xr"
    if not canonical_source.is_file():
        raise SystemExit(
            f"{symbol} target_leaf requires canonical source module {canonical_source}"
        )
    previous = owners.get(target_leaf)
    if previous is not None:
        raise SystemExit(
            f"target_leaf {target_leaf} has duplicate providers: {previous}, {symbol}"
        )
    owners[target_leaf] = symbol


def resolve_visibility(props: dict[str, object], default: str, context: str) -> str:
    """Read an entry's own visibility, falling back to the kind's default."""
    visibility = str(props.get("visibility", default))
    if visibility not in {"public", "internal"}:
        raise SystemExit(f"{context}: unsupported visibility: {visibility}")
    return visibility


def resolve_member_visibility(props: dict[str, object], context: str) -> str:
    """Read a class member's visibility, where empty means inherit.

    The declaring class may appear after the member in the .def file, so the
    empty answer is resolved once the whole file is parsed.
    """
    visibility = str(props.get("visibility", ""))
    if visibility not in {"", "public", "internal"}:
        raise SystemExit(f"{context}: unsupported visibility: {visibility}")
    return visibility


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
    aot_enum: str
    aot_direct: bool
    aot_kind: str
    link_object: str
    define: str
    layer: str
    visibility: str
    effect: str
    allocation: str
    return_ownership: str
    semantic_intrinsic: bool
    target_leaf: str
    caps: tuple[str, ...]

    @property
    def symbol(self) -> str:
        return f"{self.module}.{self.name}"

    @property
    def is_internal(self) -> bool:
        return self.visibility == "internal"


VM_BINDING_KINDS = ("normal", "yieldable", "slow")


@dataclasses.dataclass(frozen=True)
class ModuleNativeTypeExport:
    name: str
    builtin_type: str


@dataclasses.dataclass(frozen=True)
class ModuleNativeFnExport:
    name: str
    vm: str
    vm_binding: str


@dataclasses.dataclass(frozen=True)
class ModuleDeclaration:
    """The parts of one stdlib_boundary.toml module the loader is built from.

    A module used to be created by a C factory written for it. The three fields
    below are what those factories did beyond binding .def rows, restated as
    declarations so that one generic load path can carry every module.
    """

    name: str
    native_type_exports: tuple[ModuleNativeTypeExport, ...]
    native_fn_exports: tuple[ModuleNativeFnExport, ...]
    load_time_classes: tuple[str, ...]

    @property
    def is_empty(self) -> bool:
        return not (self.native_type_exports or self.native_fn_exports or self.load_time_classes)


def parse_module_declarations(root: Path) -> dict[str, ModuleDeclaration]:
    """Read the loader-facing declarations out of stdlib_boundary.toml."""
    manifest_path = root / "stdlib" / "stdlib_boundary.toml"
    if not manifest_path.is_file():
        raise SystemExit(f"missing stdlib boundary manifest: {manifest_path}")
    with manifest_path.open("rb") as handle:
        manifest = tomllib.load(handle)

    declarations: dict[str, ModuleDeclaration] = {}
    for module in manifest.get("module", ()):
        name = str(module.get("name", ""))
        if not name:
            raise SystemExit(f"{manifest_path}: module entry without a name")

        type_exports: list[ModuleNativeTypeExport] = []
        for row in module.get("native_type_exports", ()):
            missing = [key for key in ("name", "builtin_type") if not row.get(key)]
            if missing:
                raise SystemExit(
                    f"{manifest_path}: {name}.native_type_exports missing {', '.join(missing)}"
                )
            type_exports.append(ModuleNativeTypeExport(str(row["name"]), str(row["builtin_type"])))

        fn_exports: list[ModuleNativeFnExport] = []
        for row in module.get("native_fn_exports", ()):
            missing = [key for key in ("name", "vm", "vm_binding") if not row.get(key)]
            if missing:
                raise SystemExit(
                    f"{manifest_path}: {name}.native_fn_exports missing {', '.join(missing)}"
                )
            binding = str(row["vm_binding"])
            if binding not in VM_BINDING_KINDS:
                raise SystemExit(
                    f"{manifest_path}: {name}.native_fn_exports {row['name']}: "
                    f"unsupported vm_binding {binding!r}"
                )
            fn_exports.append(ModuleNativeFnExport(str(row["name"]), str(row["vm"]), binding))

        load_time_classes = tuple(str(cls) for cls in module.get("load_time_classes", ()))
        declarations[name] = ModuleDeclaration(
            name=name,
            native_type_exports=tuple(type_exports),
            native_fn_exports=tuple(fn_exports),
            load_time_classes=load_time_classes,
        )
    return declarations


def derive_binder_modules(root: Path) -> set[str]:
    """Return every module stdlibgen emits a VM binder for.

    The generic module loader looks a module up in this set to decide whether it
    has native entries to install, so the answer has to come from the same
    declarations the binder itself is generated from: .def rows and constants,
    plus the boundary manifest's loader declarations.
    """
    entries, constants, *_ = parse_def_metadata(root)
    modules = {entry.module for entry in unique_vm_binding_entries(entries)}
    modules.update(constant.module for constant in constants if constant.vm_value)
    modules.update(
        name
        for name, declaration in parse_module_declarations(root).items()
        if not declaration.is_empty
    )
    return modules


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
    visibility: str

    @property
    def symbol(self) -> str:
        return f"{self.module}.{self.name}"

    @property
    def is_internal(self) -> bool:
        return self.visibility == "internal"


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
    visibility: str

    @property
    def symbol(self) -> str:
        return f"{self.module}.{self.name}"

    @property
    def is_internal(self) -> bool:
        return self.visibility == "internal"


@dataclasses.dataclass(frozen=True)
class StdlibObjectShapeEntry:
    module: str
    name: str
    doc: str
    fields: tuple[StdlibHandleFieldEntry, ...]
    exact: bool
    visibility: str

    @property
    def symbol(self) -> str:
        return f"{self.module}.{self.name}"

    @property
    def is_internal(self) -> bool:
        return self.visibility == "internal"


@dataclasses.dataclass(frozen=True)
class StdlibEnumVariantEntry:
    name: str
    payload_types: tuple[str, ...]


@dataclasses.dataclass(frozen=True)
class StdlibEnumEntry:
    module: str
    name: str
    doc: str
    variants: tuple[StdlibEnumVariantEntry, ...]
    visibility: str

    @property
    def symbol(self) -> str:
        return f"{self.module}.{self.name}"

    @property
    def is_internal(self) -> bool:
        return self.visibility == "internal"


def stable_enum_layout_id(module: str, enum: StdlibEnumEntry) -> int:
    """Mirror xr_enum_layout_nominal_id for source/native enum parity."""

    mask = (1 << 64) - 1

    def byte(hash_value: int, value: int) -> int:
        return ((hash_value ^ value) * 1099511628211) & mask

    def u32(hash_value: int, value: int) -> int:
        for shift in (24, 16, 8, 0):
            hash_value = byte(hash_value, (value >> shift) & 0xFF)
        return hash_value

    def string(hash_value: int, value: str) -> int:
        encoded = value.encode("utf-8")
        hash_value = u32(hash_value, len(encoded))
        for item in encoded:
            hash_value = byte(hash_value, item)
        return hash_value

    hash_value = 1469598103934665603
    hash_value = string(hash_value, "xray.enum.nominal.v1")
    hash_value = string(hash_value, module)
    hash_value = string(hash_value, enum.name)
    hash_value = u32(hash_value, len(enum.variants))
    for variant in enum.variants:
        hash_value = string(hash_value, variant.name)
        hash_value = u32(hash_value, len(variant.payload_types))

    hash_value ^= hash_value >> 30
    hash_value = (hash_value * 0xBF58476D1CE4E5B9) & mask
    hash_value ^= hash_value >> 27
    hash_value = (hash_value * 0x94D049BB133111EB) & mask
    hash_value ^= hash_value >> 31
    return (hash_value & 0xFFFFFFFF) | 0x80000000


@dataclasses.dataclass(frozen=True)
class StdlibTypeMethodEntry:
    module: str
    type_name: str
    name: str
    signature: str
    doc: str
    allocation: str
    visibility: str

    @property
    def symbol(self) -> str:
        return f"{self.type_name}.{self.name}"

    @property
    def is_internal(self) -> bool:
        return self.visibility == "internal"


@dataclasses.dataclass(frozen=True)
class StdlibNativeClassEntry:
    module: str
    name: str
    super_slot: str
    core_slot: str
    native_body_expr: str
    flags: str
    builtin_kind: str
    visibility: str

    @property
    def symbol(self) -> str:
        return f"{self.module}.{self.name}"

    @property
    def is_internal(self) -> bool:
        return self.visibility == "internal"


@dataclasses.dataclass(frozen=True)
class StdlibClassEntry:
    module: str
    name: str
    super_slot: str
    core_slot: str
    flags: str
    builtin_kind: str
    visibility: str

    @property
    def symbol(self) -> str:
        return f"{self.module}.{self.name}"

    @property
    def is_internal(self) -> bool:
        return self.visibility == "internal"


@dataclasses.dataclass(frozen=True)
class StdlibClassMethodEntry:
    module: str
    class_name: str
    name: str
    vm: str
    argc: str
    flags: str
    visibility: str

    @property
    def symbol(self) -> str:
        return f"{self.class_name}.{self.name}"

    @property
    def is_internal(self) -> bool:
        return self.visibility == "internal"


@dataclasses.dataclass(frozen=True)
class StdlibClassFieldEntry:
    module: str
    class_name: str
    name: str
    flags: str
    visibility: str

    @property
    def symbol(self) -> str:
        return f"{self.class_name}.{self.name}"

    @property
    def is_internal(self) -> bool:
        return self.visibility == "internal"


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
    r"^(const\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*:\s*(.+)$"
)


def split_top_level_csv(raw: str) -> list[str]:
    parts: list[str] = []
    start = 0
    depth = 0
    for i, ch in enumerate(raw):
        if ch in "<([":
            depth += 1
        elif ch in ">)]":
            depth = max(depth - 1, 0)
        elif ch == "," and depth == 0:
            part = raw[start:i].strip()
            if part:
                parts.append(part)
            start = i + 1
    tail = raw[start:].strip()
    if tail:
        parts.append(tail)
    return parts


def parse_function_signature_shape(signature: str) -> tuple[list[str], str]:
    """Return top-level parameter fragments and return type from an Xray signature."""
    if not signature.startswith("("):
        raise ValueError(f"function signature must start with '(': {signature!r}")
    depth = 0
    close = -1
    for i, ch in enumerate(signature):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                close = i
                break
    if close < 0:
        raise ValueError(f"function signature has no matching ')': {signature!r}")
    tail = signature[close + 1 :].strip()
    if not tail.startswith(":"):
        raise ValueError(f"function signature has no return type: {signature!r}")
    return split_top_level_csv(signature[1:close]), tail[1:].strip()


_NON_RC_RETURN_RE = re.compile(
    r"^(?:\(\)|unit|bool|rune|i(?:8|16|32|64)|u(?:8|16|32|64)|f(?:32|64)|isize|usize)(?:\?)?$"
)


def return_type_requires_ownership_contract(return_type: str) -> bool:
    """Conservative manifest gate: false only for proven non-RC return shapes."""
    value = return_type.strip()
    if _NON_RC_RETURN_RE.fullmatch(value):
        return False
    bare = value[:-1].strip() if value.endswith("?") else value
    return not (bare.startswith("Ptr<") or bare.startswith("MutPtr<") or bare.startswith("Slice<"))


def parse_handle_fields(raw: str, context: str) -> tuple[StdlibHandleFieldEntry, ...]:
    fields: list[StdlibHandleFieldEntry] = []
    for fragment in split_top_level_csv(raw):
        match = HANDLE_FIELD_RE.fullmatch(fragment)
        if not match:
            raise SystemExit(f"{context}: malformed handle field fragment: {fragment!r}")
        type_str = match.group(3).strip()
        if not type_str:
            raise SystemExit(f"{context}: handle field {match.group(2)!r} requires a type")
        fields.append(
            StdlibHandleFieldEntry(
                name=match.group(2),
                type=type_str,
                is_const=match.group(1) is not None,
            )
        )
    if not fields:
        raise SystemExit(f"{context}: handle requires at least one field")
    return tuple(fields)


ENUM_VARIANT_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)(?:\((.*)\))?$")


def parse_enum_variants(raw: str, context: str) -> tuple[StdlibEnumVariantEntry, ...]:
    variants: list[StdlibEnumVariantEntry] = []
    seen: set[str] = set()
    for fragment in split_top_level_csv(raw):
        match = ENUM_VARIANT_RE.fullmatch(fragment.strip())
        if not match:
            raise SystemExit(f"{context}: malformed enum variant: {fragment!r}")
        name = match.group(1)
        if name in seen:
            raise SystemExit(f"{context}: duplicate enum variant: {name}")
        seen.add(name)
        payload_raw = match.group(2)
        payload_types = tuple(split_top_level_csv(payload_raw)) if payload_raw is not None else ()
        if payload_raw is not None and not payload_types:
            raise SystemExit(f"{context}: enum variant {name!r} requires payload types")
        variants.append(StdlibEnumVariantEntry(name=name, payload_types=payload_types))
    if not variants:
        raise SystemExit(f"{context}: enum requires at least one variant")
    return tuple(variants)


def parse_def_metadata(
    root: Path,
) -> tuple[
    list[StdlibEntry],
    list[StdlibConstEntry],
    list[StdlibHandleEntry],
    list[StdlibObjectShapeEntry],
    list[StdlibEnumEntry],
    list[StdlibTypeMethodEntry],
    list[StdlibNativeClassEntry],
    list[StdlibClassEntry],
    list[StdlibClassMethodEntry],
    list[StdlibClassFieldEntry],
]:
    defs_dir = root / "stdlib" / "defs"
    entries: list[StdlibEntry] = []
    constants: list[StdlibConstEntry] = []
    handles: list[StdlibHandleEntry] = []
    object_shapes: list[StdlibObjectShapeEntry] = []
    enums: list[StdlibEnumEntry] = []
    type_methods: list[StdlibTypeMethodEntry] = []
    native_classes: list[StdlibNativeClassEntry] = []
    classes: list[StdlibClassEntry] = []
    class_methods: list[StdlibClassMethodEntry] = []
    class_fields: list[StdlibClassFieldEntry] = []
    target_leaf_owners: dict[str, str] = {}
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
            ret = str(props.get("ret", "value"))
            aot_enum = str(props.get("aot_enum", ""))
            if ret not in {"value", "i64", "enum_i64", "str_borrowed", "i64_pair_result"}:
                raise SystemExit(
                    f"{path}:{line_no}: unsupported ret kind for "
                    f"{current_module}.{current_name}: {ret}"
                )
            if aot_kind and aot_kind not in {"method", "builtin"}:
                raise SystemExit(
                    f"{path}:{line_no}: unsupported aot_kind for {current_module}.{current_name}: {aot_kind}"
                )
            if aot_kind and not aot_direct:
                raise SystemExit(
                    f"{path}:{line_no}: {current_module}.{current_name} aot_kind requires aot_direct: true"
                )
            if ret in {"enum_i64", "i64_pair_result"} and (not aot_direct or not aot_enum):
                raise SystemExit(
                    f"{path}:{line_no}: {current_module}.{current_name} {ret} "
                    "requires aot_direct: true and aot_enum"
                )
            if aot_enum and ret not in {"enum_i64", "i64_pair_result"}:
                raise SystemExit(
                    f"{path}:{line_no}: {current_module}.{current_name} aot_enum "
                    "requires ret: enum_i64 or i64_pair_result"
                )
            argc_raw = str(props["argc"])
            arg_spec = str(props.get("arg_spec", ""))
            if argc_raw == "variadic":
                variadic_spec = "*" if aot_direct and aot_kind == "method" else ""
                if arg_spec != variadic_spec:
                    raise SystemExit(
                        f"{path}:{line_no}: {current_module}.{current_name} variadic arg_spec "
                        f"must be {variadic_spec!r}, got {arg_spec!r}"
                    )
            else:
                try:
                    argc_count = int(argc_raw)
                except ValueError:
                    raise SystemExit(
                        f"{path}:{line_no}: {current_module}.{current_name} argc must be an "
                        f"integer or variadic, got {argc_raw!r}"
                    ) from None
                if argc_count < 0:
                    raise SystemExit(
                        f"{path}:{line_no}: {current_module}.{current_name} argc must not be "
                        f"negative, got {argc_count}"
                    )
                if aot_kind == "builtin":
                    if arg_spec:
                        raise SystemExit(
                            f"{path}:{line_no}: {current_module}.{current_name} builtin rows "
                            "have dedicated lowerings and do not consume arg_spec; omit it"
                        )
                elif len(arg_spec) != argc_count:
                    raise SystemExit(
                        f"{path}:{line_no}: {current_module}.{current_name} arg_spec length "
                        f"{len(arg_spec)} does not match argc {argc_count}; the AOT emitter "
                        "consumes one spec character per argument"
                    )
                else:
                    unknown = sorted(set(arg_spec) - set("ipsv"))
                    if unknown:
                        raise SystemExit(
                            f"{path}:{line_no}: {current_module}.{current_name} arg_spec has "
                            f"unsupported characters {''.join(unknown)!r} (allowed: i, p, s, v)"
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
            visibility = resolve_visibility(
                props,
                "internal" if current_name.startswith("__") else "public",
                f"{path}:{line_no}: {current_module}.{current_name}",
            )
            allocation = str(props.get("allocation", ""))
            if allocation not in {"", "no_heap", "may_heap"}:
                raise SystemExit(
                    f"{path}:{line_no}: unsupported allocation contract for "
                    f"{current_module}.{current_name}: {allocation}"
                )
            signature = str(props["signature"])
            try:
                signature_params, signature_return = parse_function_signature_shape(signature)
            except ValueError as exc:
                raise SystemExit(f"{path}:{line_no}: {current_module}.{current_name}: {exc}") from exc
            return_ownership = str(props.get("return_ownership", ""))
            ownership_values = {"fresh", "borrowed_static"}
            borrowed_param = re.fullmatch(r"borrowed_param:([0-9]+)", return_ownership)
            if return_ownership and return_ownership not in ownership_values and not borrowed_param:
                raise SystemExit(
                    f"{path}:{line_no}: unsupported return ownership contract for "
                    f"{current_module}.{current_name}: {return_ownership}"
                )
            if borrowed_param and int(borrowed_param.group(1)) >= len(signature_params):
                raise SystemExit(
                    f"{path}:{line_no}: return ownership for {current_module}.{current_name} "
                    f"references missing parameter {borrowed_param.group(1)}"
                )
            semantic_intrinsic_value = props.get("semantic_intrinsic", False)
            if not isinstance(semantic_intrinsic_value, bool):
                raise SystemExit(
                    f"{path}:{line_no}: {current_module}.{current_name} semantic_intrinsic "
                    "must be a boolean"
                )
            semantic_intrinsic = semantic_intrinsic_value
            if semantic_intrinsic and return_ownership:
                raise SystemExit(
                    f"{path}:{line_no}: {current_module}.{current_name} semantic intrinsic "
                    "must specialize return ownership during lowering, not declare a generic contract"
                )
            if (return_type_requires_ownership_contract(signature_return) and
                    not return_ownership and not semantic_intrinsic):
                raise SystemExit(
                    f"{path}:{line_no}: {current_module}.{current_name} returns reference-capable "
                    f"type {signature_return!r} and requires explicit return_ownership"
                )
            target_leaf = str(props.get("target_leaf", ""))
            effect = str(props.get("effect", ""))
            if target_leaf not in TARGET_LEAF_KINDS:
                raise SystemExit(
                    f"{path}:{line_no}: unsupported target_leaf for "
                    f"{current_module}.{current_name}: {target_leaf}"
                )
            if target_leaf and (
                signature != "(): i64"
                or argc_raw != "0"
                or arg_spec
                or not aot_direct
                or aot_kind != "method"
                or ret != "value"
                or vm_binding != "normal"
                or vm_ifdef
                or aot_enum
                or str(props.get("define", ""))
                or caps
            ):
                raise SystemExit(
                    f"{path}:{line_no}: {current_module}.{current_name} target_leaf "
                    "requires one unconditional direct `(): i64` scalar member"
                )
            validate_target_leaf_source_owner(
                root,
                current_module,
                current_name,
                target_leaf,
                visibility,
                effect,
                allocation,
                target_leaf_owners,
            )

            entries.append(
                StdlibEntry(
                    module=current_module,
                    name=current_name,
                    signature=signature,
                    doc=str(props["doc"]),
                    vm=str(props["vm"]),
                    vm_binding=vm_binding,
                    vm_ifdef=vm_ifdef,
                    aot=str(props.get("aot", "")),
                    argc=argc_raw,
                    arg_spec=arg_spec,
                    ret=ret,
                    aot_enum=aot_enum,
                    aot_direct=aot_direct,
                    aot_kind=aot_kind,
                    link_object=link_object,
                    define=str(props.get("define", "")),
                    layer=str(props.get("layer", "")),
                    visibility=visibility,
                    effect=effect,
                    allocation=allocation,
                    return_ownership=return_ownership,
                    semantic_intrinsic=semantic_intrinsic,
                    target_leaf=target_leaf,
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
                    visibility=resolve_visibility(
                        props,
                        "internal" if current_name.startswith("__") else "public",
                        f"{path}:{line_no}: {current_module}.{current_name}",
                    ),
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
                    visibility=resolve_visibility(
                        props,
                        "internal" if current_name.startswith("__") else "public",
                        f"{path}:{line_no}: {current_module}.{current_name}",
                    ),
                )
            )
        elif current_kind == "object":
            missing = [k for k in ("fields",) if k not in props]
            if missing:
                names = ", ".join(missing)
                raise SystemExit(f"{path}:{line_no}: {current_module}.{current_name} missing {names}")
            fields = parse_handle_fields(
                str(props["fields"]), f"{path}:{line_no}: {current_module}.{current_name}"
            )
            exact = props.get("exact", True)
            if not isinstance(exact, bool):
                raise SystemExit(
                    f"{path}:{line_no}: {current_module}.{current_name} exact must be boolean"
                )
            object_shapes.append(
                StdlibObjectShapeEntry(
                    module=current_module,
                    name=current_name,
                    doc=str(props.get("doc", "Native exact object shape")),
                    fields=fields,
                    exact=exact,
                    visibility=resolve_visibility(
                        props,
                        "internal" if current_name.startswith("__") else "public",
                        f"{path}:{line_no}: {current_module}.{current_name}",
                    ),
                )
            )
        elif current_kind == "enum":
            missing = [k for k in ("variants",) if k not in props]
            if missing:
                names = ", ".join(missing)
                raise SystemExit(f"{path}:{line_no}: {current_module}.{current_name} missing {names}")
            enums.append(
                StdlibEnumEntry(
                    module=current_module,
                    name=current_name,
                    doc=str(props.get("doc", "Native enum type")),
                    variants=parse_enum_variants(
                        str(props["variants"]),
                        f"{path}:{line_no}: {current_module}.{current_name}",
                    ),
                    visibility=resolve_visibility(
                        props,
                        "internal" if current_name.startswith("__") else "public",
                        f"{path}:{line_no}: {current_module}.{current_name}",
                    ),
                )
            )
        elif current_kind == "type_method":
            missing = [k for k in ("signature", "doc") if k not in props]
            if missing:
                names = ", ".join(missing)
                raise SystemExit(f"{path}:{line_no}: {current_module}.{current_name} missing {names}")
            type_name, method_name = str(current_name).split(".", 1)
            allocation = str(props.get("allocation", ""))
            if allocation not in {"", "no_heap", "may_heap"}:
                raise SystemExit(
                    f"{path}:{line_no}: unsupported allocation contract for "
                    f"{current_module}.{current_name}: {allocation}"
                )
            type_methods.append(
                StdlibTypeMethodEntry(
                    module=current_module,
                    type_name=type_name,
                    name=method_name,
                    signature=str(props["signature"]),
                    doc=str(props["doc"]),
                    allocation=allocation,
                    visibility=resolve_member_visibility(
                        props, f"{path}:{line_no}: {current_module}.{current_name}"
                    ),
                )
            )
        elif current_kind == "native_class":
            missing = [k for k in ("core_slot",) if k not in props]
            if missing:
                names = ", ".join(missing)
                raise SystemExit(f"{path}:{line_no}: {current_module}.{current_name} missing {names}")
            body_symbol = str(props.get("native_body", "")).strip()
            body_expr = str(props.get("native_body_expr", "")).strip()
            if bool(body_symbol) == bool(body_expr):
                raise SystemExit(
                    f"{path}:{line_no}: {current_module}.{current_name} requires exactly one "
                    "of native_body or native_body_expr"
                )
            native_body_expr = f"&{body_symbol}" if body_symbol else body_expr
            native_classes.append(
                StdlibNativeClassEntry(
                    module=current_module,
                    name=current_name,
                    super_slot=str(props.get("super_slot", "")),
                    core_slot=str(props["core_slot"]),
                    native_body_expr=native_body_expr,
                    flags=str(props.get("flags", "XR_CLASS_BUILTIN | XR_CLASS_HAS_NATIVE_BODY")),
                    builtin_kind=str(props.get("builtin_kind", "")),
                    visibility=resolve_visibility(
                        props, "public", f"{path}:{line_no}: {current_module}.{current_name}"
                    ),
                )
            )
        elif current_kind == "class":
            missing = [k for k in ("core_slot",) if k not in props]
            if missing:
                names = ", ".join(missing)
                raise SystemExit(f"{path}:{line_no}: {current_module}.{current_name} missing {names}")
            classes.append(
                StdlibClassEntry(
                    module=current_module,
                    name=current_name,
                    super_slot=str(props.get("super_slot", "")),
                    core_slot=str(props["core_slot"]),
                    flags=str(props.get("flags", "XR_CLASS_BUILTIN")),
                    builtin_kind=str(props.get("builtin_kind", "")),
                    visibility=resolve_visibility(
                        props, "public", f"{path}:{line_no}: {current_module}.{current_name}"
                    ),
                )
            )
        elif current_kind == "class_method":
            missing = [k for k in ("vm", "argc") if k not in props]
            if missing:
                names = ", ".join(missing)
                raise SystemExit(f"{path}:{line_no}: {current_module}.{current_name} missing {names}")
            class_name, method_name = str(current_name).split(".", 1)
            class_methods.append(
                StdlibClassMethodEntry(
                    module=current_module,
                    class_name=class_name,
                    name=method_name,
                    vm=str(props["vm"]),
                    argc=str(props["argc"]),
                    flags=str(props.get("flags", "0")),
                    visibility=resolve_member_visibility(
                        props, f"{path}:{line_no}: {current_module}.{current_name}"
                    ),
                )
            )
        elif current_kind == "class_field":
            class_name, field_name = str(current_name).split(".", 1)
            class_fields.append(
                StdlibClassFieldEntry(
                    module=current_module,
                    class_name=class_name,
                    name=field_name,
                    flags=str(props.get("flags", "0")),
                    visibility=resolve_member_visibility(
                        props, f"{path}:{line_no}: {current_module}.{current_name}"
                    ),
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
            m = re.fullmatch(
                r"module\s+([A-Za-z_][A-Za-z0-9_]*(?:/[A-Za-z_][A-Za-z0-9_]*)?)\s*\{",
                line,
            )
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
            m = re.fullmatch(r"object\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{", line)
            if m:
                if current_module is None or current_kind is not None:
                    raise SystemExit(f"{path}:{line_no}: object declaration outside module or nested object declaration")
                current_kind = "object"
                current_name = m.group(1)
                props = {}
                continue
            m = re.fullmatch(r"enum\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{", line)
            if m:
                if current_module is None or current_kind is not None:
                    raise SystemExit(f"{path}:{line_no}: enum outside module or nested enum")
                current_kind = "enum"
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
            m = re.fullmatch(r"native_class\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{", line)
            if m:
                if current_module is None or current_kind is not None:
                    raise SystemExit(
                        f"{path}:{line_no}: native_class outside module or nested native_class"
                    )
                current_kind = "native_class"
                current_name = m.group(1)
                props = {}
                continue
            m = re.fullmatch(r"class\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{", line)
            if m:
                if current_module is None or current_kind is not None:
                    raise SystemExit(f"{path}:{line_no}: class outside module or nested class")
                current_kind = "class"
                current_name = m.group(1)
                props = {}
                continue
            m = re.fullmatch(
                r"class_method\s+([A-Za-z_][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_:]*)\s*\{",
                line,
            )
            if m:
                if current_module is None or current_kind is not None:
                    raise SystemExit(
                        f"{path}:{line_no}: class_method outside module or nested class_method"
                    )
                current_kind = "class_method"
                current_name = f"{m.group(1)}.{m.group(2)}"
                props = {}
                continue
            m = re.fullmatch(
                r"class_field\s+([A-Za-z_][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)\s*\{",
                line,
            )
            if m:
                if current_module is None or current_kind is not None:
                    raise SystemExit(
                        f"{path}:{line_no}: class_field outside module or nested class_field"
                    )
                current_kind = "class_field"
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

    # A member that declared no visibility of its own takes the visibility of
    # the class it hangs off, so marking a class internal is one line rather
    # than one line per member. A member whose owner is not a declared class --
    # Coro.CoroLocal has type_method rows and no class row -- stays public.
    class_visibility = {
        (entry.module, entry.name): entry.visibility
        for entry in (*native_classes, *classes)
    }

    def inherit(entry, owner: str):
        if entry.visibility:
            return entry
        return dataclasses.replace(
            entry, visibility=class_visibility.get((entry.module, owner), "public")
        )

    type_methods = [inherit(e, e.type_name) for e in type_methods]
    class_methods = [inherit(e, e.class_name) for e in class_methods]
    class_fields = [inherit(e, e.class_name) for e in class_fields]

    return (
        entries,
        constants,
        handles,
        object_shapes,
        enums,
        type_methods,
        native_classes,
        classes,
        class_methods,
        class_fields,
    )


def parse_defs(root: Path) -> list[StdlibEntry]:
    entries, _, _, _, _, _, _, _, _, _ = parse_def_metadata(root)
    return entries


def parse_constants(root: Path) -> list[StdlibConstEntry]:
    _, constants, _, _, _, _, _, _, _, _ = parse_def_metadata(root)
    return constants


def parse_handles(root: Path) -> list[StdlibHandleEntry]:
    _, _, handles, _, _, _, _, _, _, _ = parse_def_metadata(root)
    return handles


def parse_object_shapes(root: Path) -> list[StdlibObjectShapeEntry]:
    _, _, _, object_shapes, _, _, _, _, _, _ = parse_def_metadata(root)
    return object_shapes


def parse_enums(root: Path) -> list[StdlibEnumEntry]:
    _, _, _, _, enums, _, _, _, _, _ = parse_def_metadata(root)
    return enums


def parse_type_methods(root: Path) -> list[StdlibTypeMethodEntry]:
    _, _, _, _, _, type_methods, _, _, _, _ = parse_def_metadata(root)
    return type_methods


def parse_native_classes(root: Path) -> list[StdlibNativeClassEntry]:
    _, _, _, _, _, _, native_classes, _, _, _ = parse_def_metadata(root)
    return native_classes


def parse_classes(root: Path) -> list[StdlibClassEntry]:
    _, _, _, _, _, _, _, classes, _, _ = parse_def_metadata(root)
    return classes


def parse_class_methods(root: Path) -> list[StdlibClassMethodEntry]:
    _, _, _, _, _, _, _, _, class_methods, _ = parse_def_metadata(root)
    return class_methods


def c_string(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def c_ident(value: str, context: str) -> str:
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", value):
        raise SystemExit(f"{context}: expected C identifier, got {value!r}")
    return value


def c_module_ident(value: str) -> str:
    """Map a canonical owner/name module path to a generated C identifier."""
    return c_ident(value.replace("/", "_"), f"module {value}")


def c_int_expr(value: str, context: str) -> str:
    value = value.strip()
    if not re.fullmatch(r"-?[0-9]+", value):
        raise SystemExit(f"{context}: expected integer expression, got {value!r}")
    return value


def c_flag_expr(value: str, context: str) -> str:
    value = value.strip()
    if not value or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*(?:\s*\|\s*[A-Za-z_][A-Za-z0-9_]*)*|0", value):
        raise SystemExit(f"{context}: unsupported flag expression: {value!r}")
    return value


def c_native_body_expr(value: str, context: str) -> str:
    value = value.strip()
    ident = r"[A-Za-z_][A-Za-z0-9_]*"
    if not re.fullmatch(rf"&?{ident}|{ident}\(\)", value):
        raise SystemExit(f"{context}: unsupported native body expression: {value!r}")
    return value


def c_macro_suffix(value: str, context: str) -> str:
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", value):
        raise SystemExit(f"{context}: expected macro suffix identifier, got {value!r}")
    return c_snake(value).upper()


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
    if entry.ret == "i64":
        return "CG_AOT_RET_I64"
    if entry.ret == "enum_i64":
        return "CG_AOT_RET_ENUM_I64"
    if entry.ret == "str_borrowed":
        return "CG_AOT_RET_STR_BORROWED"
    if entry.ret == "i64_pair_result":
        return "CG_AOT_RET_I64_PAIR_RESULT"
    raise SystemExit(f"unsupported ret kind for {entry.symbol}: {entry.ret}")


def const_kind_expr(entry: StdlibConstEntry) -> str:
    if entry.aot_const_kind == "helper_value":
        return "CG_AOT_STDLIB_CONST_HELPER_VALUE"
    if entry.aot_const_kind == "int64":
        return "CG_AOT_STDLIB_CONST_I64"
    if entry.aot_const_kind == "float64":
        return "CG_AOT_STDLIB_CONST_F64"
    raise SystemExit(f"unsupported aot_const kind for {entry.symbol}: {entry.aot_const_kind}")


def emit_aot_methods(
    entries: list[StdlibEntry],
    constants: list[StdlibConstEntry],
    enums: list[StdlibEnumEntry],
) -> str:
    rows = [e for e in entries if e.aot_direct and e.aot_kind == "method"]
    builtin_rows = [e for e in entries if e.aot_direct and e.aot_kind == "builtin"]
    const_rows = [c for c in constants if c.aot_const_kind]
    enum_by_symbol = {enum.symbol: enum for enum in enums}
    lines = generated_header("xstdlib_aot_methods_generated.inc.c - AOT stdlib direct-call table")
    for e in rows:
        if not e.aot_enum:
            continue
        enum = enum_by_symbol.get(f"{e.module}.{e.aot_enum}")
        if not enum:
            raise SystemExit(f"{e.symbol}: unknown aot_enum {e.module}.{e.aot_enum}")
        variants_name = f"g_aot_stdlib_{c_module_ident(e.module)}_{e.name}_enum_variants"
        lines.append(f"static const char *const {variants_name}[] = {{")
        for variant in enum.variants:
            lines.append(f"    {c_string(variant.name)},")
        lines.append("};")
        lines.append("")
    lines.append("static const CgAotStdlibMethod g_aot_stdlib_generated_methods[] = {")
    for e in rows:
        if not e.aot:
            raise SystemExit(f"{e.symbol}: aot_direct requires aot symbol")
        enum = (
            enum_by_symbol.get(f"{e.module}.{e.aot_enum}") if e.aot_enum else None
        )
        variants_ref = (
            f"g_aot_stdlib_{c_module_ident(e.module)}_{e.name}_enum_variants"
            if enum
            else "NULL"
        )
        layout_id = stable_enum_layout_id(e.module, enum) if enum else 0
        enum_name = c_string(enum.name) if enum else "NULL"
        variant_count = len(enum.variants) if enum else 0
        lines.append(
            "    {"
            f"{c_string(e.module)}, {c_string(e.name)}, {argc_expr(e)}, {c_string(e.aot)}, "
            f"{c_string(e.arg_spec)}, {ret_expr(e)}, NULL, UINT32_C({layout_id}), "
            f"{enum_name}, {variants_ref}, {variant_count}"
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
    freestanding_header_only_const_rows = list(
        {
            c.symbol: c
            for c in constants
            if c.aot_const_kind in ("int64", "float64") and not c.link_object
        }.values()
    )
    freestanding_direct_builtin_rows = list(
        {
            e.symbol: e
            for e in entries
            if e.symbol in FREESTANDING_DIRECT_BUILTINS
            and e.aot_direct
            and e.aot_kind == "builtin"
            and not e.link_object
        }.values()
    )
    known_symbols = {e.symbol for e in symbol_entries}
    unknown_header_only = sorted(FREESTANDING_HEADER_ONLY_SYMBOLS - known_symbols)
    if unknown_header_only:
        raise SystemExit(
            "unknown freestanding header-only symbols: " + ", ".join(unknown_header_only)
        )
    freestanding_header_only_symbol_rows = list(
        {
            e.symbol: e
            for e in symbol_entries
            if e.symbol in FREESTANDING_HEADER_ONLY_SYMBOLS and not e.link_object
        }.values()
    )
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
            "    XAOT_STDLIB_CAP_STACKTRACE = 1u << 10,",
            "    XAOT_STDLIB_CAP_INSTANCEOF = 1u << 11,",
            "    XAOT_STDLIB_CAP_SCOPE = 1u << 12,",
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
    lines.append("static bool xaot_stdlib_generated_symbol_is_freestanding_header_only(")
    lines.append("    const char *symbol) {")
    lines.append("    if (!symbol)")
    lines.append("        return false;")
    freestanding_header_only_rows = {
        row.symbol: row
        for row in [
            *freestanding_header_only_const_rows,
            *freestanding_direct_builtin_rows,
            *freestanding_header_only_symbol_rows,
        ]
    }
    for row in freestanding_header_only_rows.values():
        lines.append(f"    if (strcmp(symbol, {c_string(row.symbol)}) == 0)\n        return true;")
    lines.append("    return false;")
    lines.append("}")
    lines.append("")
    lines.append("/* clang-format on */")
    lines.append("")
    return "\n".join(lines)


def unique_vm_binding_entries(entries: list[StdlibEntry]) -> list[StdlibEntry]:
    seen: dict[tuple[str, str], tuple[str, str, str]] = {}
    unique: list[StdlibEntry] = []
    for e in entries:
        key = (e.module, e.name)
        existing = seen.get(key)
        if existing is not None:
            existing_vm, existing_binding, existing_ifdef = existing
            if (
                existing_vm != e.vm
                or existing_binding != e.vm_binding
                or existing_ifdef != e.vm_ifdef
            ):
                raise SystemExit(
                    f"{e.symbol}: duplicate VM binding rows disagree: "
                    f"{existing_vm}/{existing_binding}/{existing_ifdef} vs "
                    f"{e.vm}/{e.vm_binding}/{e.vm_ifdef}"
                )
            continue
        seen[key] = (e.vm, e.vm_binding, e.vm_ifdef)
        c_ident(e.vm, e.symbol)
        unique.append(e)
    return unique


def emit_vm_bindings(
    entries: list[StdlibEntry],
    constants: list[StdlibConstEntry],
    declarations: dict[str, ModuleDeclaration],
) -> str:
    rows_by_module: dict[str, list[StdlibEntry]] = {}
    consts_by_module: dict[str, list[StdlibConstEntry]] = {}
    for e in unique_vm_binding_entries(entries):
        rows_by_module.setdefault(e.module, []).append(e)
    for c in constants:
        if c.vm_value:
            consts_by_module.setdefault(c.module, []).append(c)

    declared = {name for name, decl in declarations.items() if not decl.is_empty}

    lines = generated_header("xstdlib_vm_bindings_generated.inc.c - VM stdlib binding shell")
    lines.extend(
        [
            "/*",
            " * Include this file from a stdlib module TU after stdlib/common.h",
            " * and after the module's static",
            " * C functions have been declared, then define exactly one",
            " * XR_STDLIB_VM_BIND_MODULE_<MODULE> macro before including it.",
            " *",
            " * Every binder has one shape: it installs the module's whole native",
            " * entry set and answers whether the installed count is the declared",
            " * one, so the generic loader can fail closed without knowing which",
            " * module it is loading.",
            " */",
            "",
        ]
    )
    for module in sorted(set(rows_by_module) | set(consts_by_module) | declared):
        module_ident = c_module_ident(module)
        macro = f"XR_STDLIB_VM_BIND_MODULE_{module_ident.upper()}"
        declaration = declarations.get(module)
        func = f"xr_stdlib_vm_bind_{module_ident}_generated"
        lines.append(f"#ifdef {macro}")
        lines.append(f"XR_FUNC bool {func}(XrVMRuntime *isolate, XrModule *module) {{")
        lines.append(
            "    if (!isolate || !module || xr_module_state(module) != XR_MODULE_NEW || "
            "module->export_count != 0)"
        )
        lines.append("        return false;")
        lines.append("    size_t expected_count = 0;")
        lines.append("    (void) expected_count;")
        for class_name in declaration.load_time_classes if declaration else ():
            helper = f"xr_stdlib_vm_register_{c_snake(class_name)}_class_generated"
            lines.append(f"    {helper}(isolate);")
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
            lines.append("    expected_count++;")
            if e.vm_ifdef:
                lines.append(f"#endif  /* {e.vm_ifdef} */")
        for c in consts_by_module.get(module, []):
            lines.append(
                f"    xr_module_add_export(isolate, module, {c_string(c.name)}, {c.vm_value});"
            )
            lines.append("    expected_count++;")
        for type_export in declaration.native_type_exports if declaration else ():
            lines.append(
                "    xr_module_export_native_type_class(isolate, module, "
                f"{c_string(type_export.name)}, {type_export.builtin_type});"
            )
            lines.append("    expected_count++;")
        for fn_export in declaration.native_fn_exports if declaration else ():
            export_macro = {
                "normal": "XRS_EXPORT",
                "yieldable": "XRS_EXPORT_YIELDABLE",
                "slow": "XRS_EXPORT_SLOW",
            }[fn_export.vm_binding]
            lines.append(
                f"    {export_macro}(module, isolate, {c_string(fn_export.name)}, "
                f"{fn_export.vm});"
            )
            lines.append("    expected_count++;")
        lines.append("    return module->export_count == expected_count;")
        lines.append("}")
        lines.append(f"#endif  /* {macro} */")
        lines.append("")
    lines.append("/* clang-format on */")
    lines.append("")
    return "\n".join(lines)


def c_snake(value: str) -> str:
    pieces: list[str] = []
    prev_lower = False
    for ch in value:
        if ch.isupper() and prev_lower:
            pieces.append("_")
        pieces.append(ch.lower())
        prev_lower = ch.islower() or ch.isdigit()
    snake = "".join(pieces)
    if not re.fullmatch(r"[a-z_][a-z0-9_]*", snake):
        raise SystemExit(f"{value}: cannot convert class name to C helper name")
    return snake


def emit_class_bindings(
    native_classes: list[StdlibNativeClassEntry],
    classes: list[StdlibClassEntry],
    class_methods: list[StdlibClassMethodEntry],
    class_fields: list[StdlibClassFieldEntry],
) -> str:
    class_keys = {(c.module, c.name) for c in native_classes}
    class_keys.update((c.module, c.name) for c in classes)
    methods_by_class: dict[tuple[str, str], list[StdlibClassMethodEntry]] = {}
    for method in class_methods:
        key = (method.module, method.class_name)
        if key not in class_keys:
            raise SystemExit(f"{method.symbol}: class_method has no class entry")
        c_ident(method.vm, method.symbol)
        c_int_expr(method.argc, method.symbol)
        c_flag_expr(method.flags, method.symbol)
        methods_by_class.setdefault(key, []).append(method)

    fields_by_class: dict[tuple[str, str], list[StdlibClassFieldEntry]] = {}
    for field in class_fields:
        key = (field.module, field.class_name)
        if key not in class_keys:
            raise SystemExit(f"{field.symbol}: class_field has no class entry")
        c_flag_expr(field.flags, field.symbol)
        fields_by_class.setdefault(key, []).append(field)

    seen_slots: dict[str, str] = {}
    all_classes = [*native_classes, *classes]
    native_keys = {(c.module, c.name) for c in native_classes}
    for cls in all_classes:
        c_ident(cls.core_slot, cls.symbol)
        if cls.super_slot:
            c_ident(cls.super_slot, cls.symbol)
        c_flag_expr(cls.flags, cls.symbol)
        if cls.builtin_kind:
            c_ident(cls.builtin_kind, cls.symbol)
        if (cls.module, cls.name) in native_keys:
            c_native_body_expr(cls.native_body_expr, cls.symbol)
        other = seen_slots.get(cls.core_slot)
        if other is not None:
            raise SystemExit(f"{cls.symbol}: duplicate class core slot with {other}")
        seen_slots[cls.core_slot] = cls.symbol

    lines = generated_header("xstdlib_class_bindings_generated.inc.c - VM stdlib class binding shell")
    lines.extend(
        [
            "/*",
            " * Include this file from a stdlib module TU after the class method",
            " * functions and native body descriptor have been declared, then define",
            " * one or more XR_STDLIB_VM_BIND_CLASS_<CLASS> macros before including it.",
            " */",
            "",
        ]
    )
    for cls in all_classes:
        suffix = c_macro_suffix(cls.name, cls.symbol)
        helper = f"xr_stdlib_vm_register_{c_snake(cls.name)}_class_generated"
        super_expr = f"core->{cls.super_slot}" if cls.super_slot else "NULL"
        is_native = (cls.module, cls.name) in native_keys
        lines.append(f"#ifdef XR_STDLIB_VM_BIND_CLASS_{suffix}")
        lines.append(f"static void {helper}(XrVMRuntime *X) {{")
        lines.append(f"    XR_DCHECK(X != NULL, {c_string(helper + ': NULL isolate')});")
        lines.append("    XrayCoreClasses *core = xr_isolate_get_core_classes(X);")
        lines.append(f"    XR_DCHECK(core != NULL, {c_string(helper + ': core not initialised')});")
        if cls.super_slot:
            lines.append(
                f"    XR_DCHECK(core->{cls.super_slot} != NULL, "
                f"{c_string(helper + ': super class not registered')});"
            )
        lines.append(
            f"    XR_DCHECK(core->{cls.core_slot} == NULL, "
            f"{c_string(helper + ': already registered')});"
        )
        lines.append(
            f"    XrClassBuilder *builder = xr_class_builder_new(X, "
            f"{c_string(cls.name)}, {super_expr});"
        )
        lines.append(f"    XR_CHECK(builder != NULL, {c_string(helper + ': builder alloc failed')});")
        if is_native:
            lines.append(f"    xr_class_builder_set_native_body(builder, {cls.native_body_expr});")
        for field in fields_by_class.get((cls.module, cls.name), []):
            lines.append(
                f"    xr_class_builder_add_field(builder, {c_string(field.name)}, {field.flags});"
            )
        for method in methods_by_class.get((cls.module, cls.name), []):
            lines.append(
                f"    xr_class_builder_add_method(builder, {c_string(method.name)}, "
                f"{method.vm}, {method.argc}, {method.flags});"
            )
        lines.append("    XrClass *cls = xr_class_builder_finalize(builder);")
        lines.append(f"    XR_CHECK(cls != NULL, {c_string(helper + ': finalize failed')});")
        lines.append(f"    cls->flags |= {cls.flags};")
        if cls.builtin_kind:
            lines.append(f"    cls->builtin_kind = {cls.builtin_kind};")
        lines.append(f"    core->{cls.core_slot} = cls;")
        lines.append("}")
        lines.append(f"#endif  /* XR_STDLIB_VM_BIND_CLASS_{suffix} */")
        lines.append("")
    lines.append("/* clang-format on */")
    lines.append("")
    return "\n".join(lines)


def emit_defs_header(
    entries: list[StdlibEntry],
    constants: list[StdlibConstEntry],
    handles: list[StdlibHandleEntry],
    object_shapes: list[StdlibObjectShapeEntry],
    enums: list[StdlibEnumEntry],
    type_methods: list[StdlibTypeMethodEntry],
    native_classes: list[StdlibNativeClassEntry],
    classes: list[StdlibClassEntry],
    class_methods: list[StdlibClassMethodEntry],
    class_fields: list[StdlibClassFieldEntry],
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
            '#include "../base/xentry_plan.h"',
            "",
            "typedef enum XrStdlibTargetLeafKind {",
            "    XR_STDLIB_TARGET_LEAF_NONE = 0,",
            "    XR_STDLIB_TARGET_LEAF_I64_GETPID = 1,",
            "    XR_STDLIB_TARGET_LEAF_COUNT,",
            "} XrStdlibTargetLeafKind;",
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
            "    const char *aot_enum;",
            "    const char *link_object;",
            "    const char *define;",
            "    const char *layer;",
            "    const char *aot_kind;",
            "    uint32_t runtime_capabilities;",
            "    uint16_t argc;",
            "    uint16_t target_leaf;",
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
            "    uint32_t runtime_capabilities;",
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
            "typedef struct XrStdlibObjectShapeDefEntry {",
            "    const char *module;",
            "    const char *name;",
            "    const char *doc;",
            "    const XrStdlibHandleFieldDefEntry *fields;",
            "    uint16_t field_count;",
            "    bool exact;",
            "} XrStdlibObjectShapeDefEntry;",
            "",
            "typedef struct XrStdlibEnumVariantDefEntry {",
            "    const char *name;",
            "    const char *const *payload_types;",
            "    uint16_t payload_count;",
            "} XrStdlibEnumVariantDefEntry;",
            "",
            "typedef struct XrStdlibEnumDefEntry {",
            "    const char *module;",
            "    const char *name;",
            "    const char *doc;",
            "    const XrStdlibEnumVariantDefEntry *variants;",
            "    uint16_t variant_count;",
            "    uint32_t layout_id;",
            "} XrStdlibEnumDefEntry;",
            "",
            "typedef struct XrStdlibTypeMethodDefEntry {",
            "    const char *module;",
            "    const char *type_name;",
            "    const char *name;",
            "    const char *signature;",
            "    const char *doc;",
            "} XrStdlibTypeMethodDefEntry;",
            "",
            "typedef struct XrStdlibNativeClassDefEntry {",
            "    const char *module;",
            "    const char *name;",
            "    const char *super_slot;",
            "    const char *core_slot;",
            "    const char *native_body_expr;",
            "    const char *flags;",
            "    const char *builtin_kind;",
            "} XrStdlibNativeClassDefEntry;",
            "",
            "typedef struct XrStdlibClassDefEntry {",
            "    const char *module;",
            "    const char *name;",
            "    const char *super_slot;",
            "    const char *core_slot;",
            "    const char *flags;",
            "    const char *builtin_kind;",
            "} XrStdlibClassDefEntry;",
            "",
            "typedef struct XrStdlibClassMethodDefEntry {",
            "    const char *module;",
            "    const char *class_name;",
            "    const char *name;",
            "    const char *vm;",
            "    int16_t argc;",
            "    const char *flags;",
            "} XrStdlibClassMethodDefEntry;",
            "",
            "typedef struct XrStdlibClassFieldDefEntry {",
            "    const char *module;",
            "    const char *class_name;",
            "    const char *name;",
            "    const char *flags;",
            "} XrStdlibClassFieldDefEntry;",
            "",
            "static const XrStdlibDefEntry xr_stdlib_def_entries[] = {",
        ]
    )
    for e in entries:
        if e.argc == "variadic":
            argc = "UINT16_MAX"
        else:
            argc = e.argc
        unknown_caps = [cap for cap in e.caps if cap not in RUNTIME_CAP_BITS]
        if unknown_caps:
            raise SystemExit(f"{e.symbol}: unknown runtime caps: {', '.join(unknown_caps)}")
        runtime_caps = (
            " | ".join(RUNTIME_CAP_BITS[cap] for cap in e.caps) if e.caps else "0"
        )
        lines.append(
            "    {"
            f"{c_string(e.module)}, {c_string(e.name)}, {c_string(e.signature)}, "
            f"{c_string(e.doc)}, {c_string(e.vm)}, {c_string(e.vm_binding)}, "
            f"{c_string(e.vm_ifdef)}, "
            f"{c_string(e.aot)}, {c_string(e.arg_spec)}, {c_string(e.ret)}, "
            f"{c_string(e.aot_enum)}, "
            f"{c_string(e.link_object)}, {c_string(e.define)}, {c_string(e.layer)}, "
            f"{c_string(e.aot_kind)}, {runtime_caps}, {argc}, {TARGET_LEAF_KINDS[e.target_leaf]}, "
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
        unknown_caps = [cap for cap in c.caps if cap not in RUNTIME_CAP_BITS]
        if unknown_caps:
            raise SystemExit(f"{c.symbol}: unknown runtime caps: {', '.join(unknown_caps)}")
        runtime_caps = (
            " | ".join(RUNTIME_CAP_BITS[cap] for cap in c.caps) if c.caps else "0"
        )
        lines.append(
            "    {"
            f"{c_string(c.module)}, {c_string(c.name)}, {c_string(c.signature)}, "
            f"{c_string(c.doc)}, {c_string(c.vm)}, {c_string(c.vm_value)}, {c_string(c.aot)}, "
            f"{c_string(c.aot_const_kind)}, {c_string(c.link_object)}, "
            f"{c_string(c.define)}, {c_string(c.layer)}, {runtime_caps}, "
            f"{c_i64_literal(c.value)}, {c.f64_value}"
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
    for object_shape in object_shapes:
        field_array = f"xr_stdlib_object_fields_{c_module_ident(object_shape.module)}_{object_shape.name}"
        lines.append(f"static const XrStdlibHandleFieldDefEntry {field_array}[] = {{")
        for field in object_shape.fields:
            lines.append(
                "    {"
                f"{c_string(object_shape.module)}, {c_string(object_shape.name)}, {c_string(field.name)}, "
                f"{c_string(field.type)}, {'true' if field.is_const else 'false'}"
                "},"
            )
        lines.append("};")
        lines.append("")
    lines.append("static const XrStdlibObjectShapeDefEntry xr_stdlib_object_shape_def_entries[] = {")
    for object_shape in object_shapes:
        field_array = f"xr_stdlib_object_fields_{c_module_ident(object_shape.module)}_{object_shape.name}"
        lines.append(
            "    {"
            f"{c_string(object_shape.module)}, {c_string(object_shape.name)}, {c_string(object_shape.doc)}, "
            f"{field_array}, {len(object_shape.fields)}, {'true' if object_shape.exact else 'false'}"
            "},"
        )
    lines.extend(
        [
            "};",
            "#define XR_STDLIB_OBJECT_SHAPE_DEF_ENTRY_COUNT "
            "((uint32_t) (sizeof(xr_stdlib_object_shape_def_entries) / "
            "sizeof(xr_stdlib_object_shape_def_entries[0])))",
            "",
        ]
    )
    for enum in enums:
        enum_prefix = f"xr_stdlib_enum_{c_module_ident(enum.module)}_{enum.name}"
        for index, variant in enumerate(enum.variants):
            if not variant.payload_types:
                continue
            payload_array = f"{enum_prefix}_variant_{index}_payloads"
            lines.append(f"static const char *const {payload_array}[] = {{")
            for payload_type in variant.payload_types:
                lines.append(f"    {c_string(payload_type)},")
            lines.append("};")
            lines.append("")
        variant_array = f"{enum_prefix}_variants"
        lines.append(f"static const XrStdlibEnumVariantDefEntry {variant_array}[] = {{")
        for index, variant in enumerate(enum.variants):
            payload_ref = (
                f"{enum_prefix}_variant_{index}_payloads" if variant.payload_types else "NULL"
            )
            lines.append(
                "    {"
                f"{c_string(variant.name)}, {payload_ref}, {len(variant.payload_types)}"
                "},"
            )
        lines.append("};")
        lines.append("")
    lines.append("static const XrStdlibEnumDefEntry xr_stdlib_enum_def_entries[] = {")
    for enum in enums:
        variant_array = f"xr_stdlib_enum_{c_module_ident(enum.module)}_{enum.name}_variants"
        lines.append(
            "    {"
            f"{c_string(enum.module)}, {c_string(enum.name)}, {c_string(enum.doc)}, "
            f"{variant_array}, {len(enum.variants)}, "
            f"UINT32_C({stable_enum_layout_id(enum.module, enum)})"
            "},"
        )
    lines.extend(
        [
            "};",
            "#define XR_STDLIB_ENUM_DEF_ENTRY_COUNT "
            "((uint32_t) (sizeof(xr_stdlib_enum_def_entries) / "
            "sizeof(xr_stdlib_enum_def_entries[0])))",
            "",
        ]
    )
    for h in handles:
        field_array = f"xr_stdlib_handle_fields_{c_module_ident(h.module)}_{h.name}"
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
        field_array = f"xr_stdlib_handle_fields_{c_module_ident(h.module)}_{h.name}"
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
            "static const XrStdlibNativeClassDefEntry xr_stdlib_native_class_def_entries[] = {",
        ]
    )
    for cls in native_classes:
        lines.append(
            "    {"
            f"{c_string(cls.module)}, {c_string(cls.name)}, {c_string(cls.super_slot)}, "
            f"{c_string(cls.core_slot)}, {c_string(cls.native_body_expr)}, {c_string(cls.flags)}, "
            f"{c_string(cls.builtin_kind)}"
            "},"
        )
    lines.extend(
        [
            "};",
            "#define XR_STDLIB_NATIVE_CLASS_DEF_ENTRY_COUNT "
            "((uint32_t) (sizeof(xr_stdlib_native_class_def_entries) / "
            "sizeof(xr_stdlib_native_class_def_entries[0])))",
            "",
            "static const XrStdlibClassDefEntry xr_stdlib_class_def_entries[] = {",
        ]
    )
    for cls in classes:
        lines.append(
            "    {"
            f"{c_string(cls.module)}, {c_string(cls.name)}, {c_string(cls.super_slot)}, "
            f"{c_string(cls.core_slot)}, {c_string(cls.flags)}, {c_string(cls.builtin_kind)}"
            "},"
        )
    lines.extend(
        [
            "};",
            "#define XR_STDLIB_CLASS_DEF_ENTRY_COUNT "
            "((uint32_t) (sizeof(xr_stdlib_class_def_entries) / "
            "sizeof(xr_stdlib_class_def_entries[0])))",
            "",
            "static const XrStdlibClassMethodDefEntry xr_stdlib_class_method_def_entries[] = {",
        ]
    )
    for method in class_methods:
        lines.append(
            "    {"
            f"{c_string(method.module)}, {c_string(method.class_name)}, {c_string(method.name)}, "
            f"{c_string(method.vm)}, {c_int_expr(method.argc, method.symbol)}, "
            f"{c_string(method.flags)}"
            "},"
        )
    lines.extend(
        [
            "};",
            "#define XR_STDLIB_CLASS_METHOD_DEF_ENTRY_COUNT "
            "((uint32_t) (sizeof(xr_stdlib_class_method_def_entries) / "
            "sizeof(xr_stdlib_class_method_def_entries[0])))",
            "",
            "static const XrStdlibClassFieldDefEntry xr_stdlib_class_field_def_entries[] = {",
        ]
    )
    for field in class_fields:
        lines.append(
            "    {"
            f"{c_string(field.module)}, {c_string(field.class_name)}, "
            f"{c_string(field.name)}, {c_string(field.flags)}"
            "},"
        )
    lines.extend(
        [
            "};",
            "#define XR_STDLIB_CLASS_FIELD_DEF_ENTRY_COUNT "
            "((uint32_t) (sizeof(xr_stdlib_class_field_def_entries) / "
            "sizeof(xr_stdlib_class_field_def_entries[0])))",
            "",
            "#endif  /* XSTDLIB_DEFS_GENERATED_H */",
            "",
            "/* clang-format on */",
            "",
        ]
    )
    return "\n".join(lines)


def output_paths(root: Path) -> dict[Path, str]:
    (
        entries,
        constants,
        handles,
        object_shapes,
        enums,
        type_methods,
        native_classes,
        classes,
        class_methods,
        class_fields,
    ) = parse_def_metadata(root)
    declarations = parse_module_declarations(root)
    return {
        root / "src" / "aot" / "xstdlib_aot_methods_generated.inc.c": emit_aot_methods(
            entries, constants, enums
        ),
        root / "src" / "aot" / "xaot_stdlib_generated.inc.c": emit_driver_metadata(
            entries, constants
        ),
        root / "src" / "stdlib" / "xstdlib_vm_bindings_generated.inc.c": emit_vm_bindings(
            entries, constants, declarations
        ),
        root / "src" / "stdlib" / "xstdlib_class_bindings_generated.inc.c": emit_class_bindings(
            native_classes, classes, class_methods, class_fields
        ),
        root / "src" / "stdlib" / "xstdlib_defs_generated.h": emit_defs_header(
            entries,
            constants,
            handles,
            object_shapes,
            enums,
            type_methods,
            native_classes,
            classes,
            class_methods,
            class_fields,
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

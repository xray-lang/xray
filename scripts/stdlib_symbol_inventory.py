#!/usr/bin/env python3
"""Per-symbol ownership inventory for the standard library.

The legacy governance report answers a module-level question: does every
module agree with the policy recorded for it in the boundary manifest. That
question cannot say which individual symbols still have a handwritten C
semantic owner, so it cannot measure progress toward an all-Xray standard
library.

This inventory answers the symbol-level question instead. Every row is derived
from source -- the boundary manifest, the `.def` declarations, the `.xr`
sources and the C translation units -- and never from a policy label. The two
directions are both closed: every public symbol reaches a row, and every C
function under `stdlib/` is attributed to a row, so an unclassified item is a
reported defect rather than a silent omission.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

from stdlib_manifest import api_inventory, load_manifest, load_stdlibgen  # noqa: E402


SCHEMA = 1

# A non-public module is test-only unless it is the prelude, which is
# unimportable because the language installs it implicitly, not because it is
# a test fixture.
PRODUCTION_EXCEPT_NON_PUBLIC = {"prelude"}

# Function-like C constructs that the definition scanner must not mistake for
# a definition when they start a line.
C_CONTROL_KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "else", "do"}

C_FUNC_RE = re.compile(
    r"^(?P<decl>(?:[A-Za-z_][A-Za-z0-9_]*|\*)(?:[ \t\n]+(?:[A-Za-z_][A-Za-z0-9_]*|\*+))*[ \t\n*]+)"
    r"(?P<name>[A-Za-z_][A-Za-z0-9_]*)[ \t]*\((?P<args>[^;{}]*?)\)[ \t\n]*\{",
    re.MULTILINE | re.DOTALL,
)

XR_LEAF_CALL_RE = re.compile(r"\b(?P<name>__[A-Za-z_][A-Za-z0-9_]*)\s*\(")

XR_MODULE_SOURCE_RE = re.compile(r"^stdlib/(?P<module>[A-Za-z_][A-Za-z0-9_]*)/[^/]+\.xr$")

MODULE_FACTORY_RE = re.compile(r"^xr_native_module_create_(?P<module>[A-Za-z_][A-Za-z0-9_]*)$")


@dataclass
class SymbolRow:
    """One standard-library symbol or C function, with its current owner."""

    module: str
    symbol: str
    kind: str
    audience: str
    semantic_source: str
    xray_body: bool
    handwritten_c_body: str
    generated_c_only: bool
    native_leaf: bool
    leaf_class: str
    leaf_reason: str
    factory_loader: str
    plan_coverage: str
    vm_binding: str
    aot_binding: str
    covered_c_deletion: str
    blocker: str


@dataclass
class ModuleRow:
    """Module-level facts the per-symbol rows are grouped under."""

    name: str
    layer: str
    policy: str
    audience: str
    semantic_source: str
    factory_source: str
    public_native: list[str] = field(default_factory=list)
    xr_sources: list[str] = field(default_factory=list)
    c_sources: list[str] = field(default_factory=list)
    c_function_count: int = 0
    # An `.xr` file under the module directory makes the module a source module
    # in any importing program's graph, whether or not the manifest names it as
    # the module's semantic source.
    enters_module_graph: bool = False
    symbol_count: int = 0
    xray_body_symbols: int = 0
    handwritten_c_symbols: int = 0
    native_leaf_symbols: int = 0
    queue: str = ""
    queue_reason: str = ""
    probe: dict[str, Any] = field(default_factory=dict)


def rel(root: Path, path: Path) -> str:
    return path.resolve().relative_to(root).as_posix()


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def c_functions(root: Path) -> dict[str, list[tuple[str, str]]]:
    """Map a module directory to the (function, source file) pairs it defines.

    `stdlib_cache.c` sits directly under `stdlib/` and belongs to no module; it
    is reported under the synthetic `_stdlib` owner so the C side still closes.
    """
    out: dict[str, list[tuple[str, str]]] = {}
    for path in sorted((root / "stdlib").rglob("*.c")):
        owner = path.parent.name if path.parent != root / "stdlib" else "_stdlib"
        text = read(path)
        for match in C_FUNC_RE.finditer(text):
            name = match.group("name")
            if name in C_CONTROL_KEYWORDS:
                continue
            out.setdefault(owner, []).append((name, rel(root, path)))
    return out


def xr_sources(root: Path, module: str) -> list[Path]:
    directory = root / "stdlib" / module
    if not directory.is_dir():
        return []
    return sorted(directory.glob("*.xr"))


def c_sources(root: Path, module: str) -> list[Path]:
    directory = root / "stdlib" / module
    if not directory.is_dir():
        return []
    return sorted(directory.glob("*.c"))


def xray_public_symbols(root: Path) -> dict[str, dict[str, dict[str, str]]]:
    """Map each module to the public symbols its `.xr` source declares.

    The repository's API inventory is the authority here rather than a private
    scan of `export` lines: it expands class members and type aliases the same
    way the documentation and metadata surfaces do, so a class method declared
    in Xray is counted exactly like a class method declared in a `.def` file.
    Counting only top-level `export` lines would understate the Xray side and
    make the two sides of the coverage check incomparable.
    """
    out: dict[str, dict[str, dict[str, str]]] = {}
    for item in api_inventory(root).get("items", ()):
        if item.get("category") != "stdlib-module":
            continue
        source = str(item.get("source", ""))
        match = XR_MODULE_SOURCE_RE.match(source)
        if not match:
            continue
        module = match.group("module")
        namespace = str(item.get("namespace", ""))
        name = str(item.get("name", ""))
        # A class member is inventoried under the class as its namespace; the
        # module-qualified name has to carry the class so it stays unique.
        symbol = name if namespace == module else f"{namespace}.{name}"
        out.setdefault(module, {})[symbol] = {
            "kind": str(item.get("kind", "")),
            "source": source,
            "signature": str(item.get("signature", "")),
        }
    return out


def def_entries_by_module(root: Path) -> dict[str, list[Any]]:
    stdlibgen = load_stdlibgen(root)
    out: dict[str, list[Any]] = {}
    for part in stdlibgen.parse_def_metadata(root):
        for entry in part:
            module = str(getattr(entry, "module", "") or "")
            if not module:
                continue
            out.setdefault(module, []).append(entry)
    return out


def entry_symbol_name(entry: Any) -> str:
    if hasattr(entry, "type_name"):
        return f"{entry.type_name}.{entry.name}"
    if type(entry).__name__ in {"StdlibClassMethodEntry", "StdlibClassFieldEntry"}:
        return f"{entry.class_name}.{entry.name}"
    name = getattr(entry, "name", None) or getattr(entry, "class_name", None)
    return str(name or "")


def entry_kind(entry: Any) -> str:
    return {
        "StdlibEntry": "function",
        "StdlibConstEntry": "const",
        "StdlibHandleEntry": "handle",
        "StdlibObjectShapeEntry": "object-shape",
        "StdlibEnumEntry": "enum",
        "StdlibClassEntry": "class",
        "StdlibClassMethodEntry": "class-method",
        "StdlibClassFieldEntry": "class-field",
        "StdlibNativeClassEntry": "native-class",
        "StdlibTypeMethodEntry": "type-method",
    }.get(type(entry).__name__, type(entry).__name__)


def leaf_class_proposal(module: str, entry: Any) -> tuple[str, str]:
    """Propose an allowlist class for a private native leaf.

    The proposal orders the migration queue. It is deliberately not written
    into the authoritative `leaf_class` column: an approved class needs a
    per-symbol record carrying ABI, ownership, effect, provider and deletion
    trigger, and no such record exists in the current manifest schema.
    """
    layer = str(getattr(entry, "layer", "") or "")
    if module in {"crypto"}:
        return "security_provider_leaf", "cryptographic provider boundary"
    if layer == "alloc":
        return "runtime_leaf", "allocator and object representation primitive"
    if layer == "runtime":
        return "runtime_leaf", "scheduler, netpoll or coroutine primitive"
    if layer == "system":
        return "host_abi_leaf", "operating-system ABI boundary"
    if module == "math":
        return "machine_intrinsic_leaf", "hardware or libm intrinsic"
    return "unclassified", f"no allowlist class derivable from layer {layer!r}"


def build_rows(root: Path) -> tuple[list[ModuleRow], list[SymbolRow], list[str]]:
    manifest = load_manifest(root)
    defs = def_entries_by_module(root)
    xray_symbols = xray_public_symbols(root)
    cfuncs = c_functions(root)
    defects: list[str] = []

    modules: list[ModuleRow] = []
    rows: list[SymbolRow] = []
    attributed_c: dict[str, set[str]] = {}

    manifest_names = {str(m["name"]) for m in manifest.modules}

    for module in manifest.modules:
        name = str(module["name"])
        public = bool(module.get("public", False))
        audience = (
            "production"
            if public or name in PRODUCTION_EXCEPT_NON_PUBLIC
            else "test-only"
        )
        policy = str(module.get("policy", ""))
        semantic_source = str(module.get("semantic_source", ""))
        factory_source = str(module.get("factory_source", ""))
        public_native = [str(x) for x in module.get("public_native", ())]

        xr_paths = xr_sources(root, name)
        c_paths = c_sources(root, name)
        module_c = cfuncs.get(name, [])
        attributed_c.setdefault(name, set())

        mrow = ModuleRow(
            name=name,
            layer=str(module.get("layer", "")),
            policy=policy,
            audience=audience,
            semantic_source=semantic_source,
            factory_source=factory_source,
            public_native=public_native,
            xr_sources=[rel(root, p) for p in xr_paths],
            c_sources=[rel(root, p) for p in c_paths],
            c_function_count=len(module_c),
            enters_module_graph=bool(xr_paths),
        )

        # Every function the module's C defines, indexed for attribution.
        c_by_name: dict[str, str] = {}
        for func, source in module_c:
            c_by_name.setdefault(func, source)

        xr_exports = xray_symbols.get(name, {})
        xr_leaf_calls: set[str] = set()
        for path in xr_paths:
            xr_leaf_calls.update(
                m.group("name") for m in XR_LEAF_CALL_RE.finditer(read(path))
            )

        # `.def`-declared symbols: the C side owns the semantics. The row
        # names the declaration file, which is not the module's `.xr` source
        # even when the manifest points the module at one.
        def_source = (
            semantic_source
            if semantic_source.endswith(".def")
            else "stdlib/defs/core.def"
        )
        for entry in defs.get(name, ()):
            symbol = entry_symbol_name(entry)
            if not symbol:
                continue
            vm = str(getattr(entry, "vm", "") or "")
            aot = str(getattr(entry, "aot", "") or "")
            is_leaf = symbol.startswith("__") or str(
                getattr(entry, "visibility", "")
            ) == "internal"
            c_body = c_by_name.get(vm, "")
            if vm and vm in c_by_name:
                attributed_c[name].add(vm)
            if aot and aot in c_by_name:
                attributed_c[name].add(aot)
            leaf_class, leaf_reason = (
                leaf_class_proposal(name, entry) if is_leaf else ("", "")
            )
            rows.append(
                SymbolRow(
                    module=name,
                    symbol=symbol,
                    kind=entry_kind(entry),
                    audience=audience,
                    semantic_source=def_source,
                    xray_body=False,
                    handwritten_c_body=c_body or ("external" if vm else ""),
                    generated_c_only=False,
                    native_leaf=is_leaf,
                    # Authoritative class stays unclassified until a per-symbol
                    # leaf record exists; the proposal only orders the queue.
                    leaf_class="unclassified" if is_leaf else "",
                    leaf_reason=(
                        f"proposed {leaf_class}: {leaf_reason}" if is_leaf else ""
                    ),
                    factory_loader="",
                    plan_coverage="native_binding",
                    vm_binding=vm,
                    aot_binding=aot,
                    covered_c_deletion=c_body,
                    blocker="",
                )
            )

        # `.xr` exports: the Xray source owns the semantics.
        for symbol, meta in sorted(xr_exports.items()):
            rows.append(
                SymbolRow(
                    module=name,
                    symbol=symbol,
                    kind=meta["kind"],
                    audience=audience,
                    semantic_source=meta["source"],
                    xray_body=True,
                    handwritten_c_body="",
                    generated_c_only=False,
                    native_leaf=False,
                    leaf_class="",
                    leaf_reason="",
                    factory_loader="",
                    plan_coverage="xray_source",
                    vm_binding="",
                    aot_binding="",
                    covered_c_deletion="",
                    blocker="",
                )
            )

        # Remaining C functions: loaders, helpers and unattributed bodies.
        for func, source in module_c:
            if func in attributed_c[name]:
                continue
            factory = MODULE_FACTORY_RE.match(func)
            if factory:
                attributed_c[name].add(func)
                rows.append(
                    SymbolRow(
                        module=name,
                        symbol=func,
                        kind="module-factory",
                        audience=audience,
                        semantic_source=source,
                        xray_body=False,
                        handwritten_c_body=source,
                        generated_c_only=False,
                        native_leaf=False,
                        leaf_class="",
                        leaf_reason="",
                        factory_loader=source,
                        plan_coverage="c_loader",
                        vm_binding="",
                        aot_binding="",
                        covered_c_deletion=source,
                        blocker="module-specific C loader; needs a generic source-derived loader",
                    )
                )
                continue
            attributed_c[name].add(func)
            rows.append(
                SymbolRow(
                    module=name,
                    symbol=func,
                    kind="c-function",
                    audience=audience,
                    semantic_source=source,
                    xray_body=False,
                    handwritten_c_body=source,
                    generated_c_only=False,
                    native_leaf=False,
                    leaf_class="",
                    leaf_reason="",
                    factory_loader="",
                    plan_coverage="c_internal",
                    vm_binding="",
                    aot_binding="",
                    covered_c_deletion=source,
                    blocker="",
                )
            )

        module_rows = [r for r in rows if r.module == name]
        mrow.symbol_count = len(module_rows)
        mrow.xray_body_symbols = sum(1 for r in module_rows if r.xray_body)
        mrow.handwritten_c_symbols = sum(
            1 for r in module_rows if r.handwritten_c_body and not r.xray_body
        )
        mrow.native_leaf_symbols = sum(1 for r in module_rows if r.native_leaf)
        modules.append(mrow)

    # C functions living under stdlib/ but outside any manifest module.
    for owner, funcs in sorted(cfuncs.items()):
        if owner in manifest_names:
            continue
        for func, source in funcs:
            rows.append(
                SymbolRow(
                    module=owner,
                    symbol=func,
                    kind="c-function",
                    audience="production",
                    semantic_source=source,
                    xray_body=False,
                    handwritten_c_body=source,
                    generated_c_only=False,
                    native_leaf=False,
                    leaf_class="",
                    leaf_reason="",
                    factory_loader="",
                    plan_coverage="c_internal",
                    vm_binding="",
                    aot_binding="",
                    covered_c_deletion=source,
                    blocker="C source under stdlib/ with no owning manifest module",
                )
            )

    # `.def` modules that no manifest module claims.
    for module in sorted(defs):
        if module in manifest_names:
            continue
        for entry in defs[module]:
            symbol = entry_symbol_name(entry)
            vm = str(getattr(entry, "vm", "") or "")
            rows.append(
                SymbolRow(
                    module=module,
                    symbol=symbol,
                    kind=entry_kind(entry),
                    audience="production",
                    semantic_source="stdlib/defs/core.def",
                    xray_body=False,
                    handwritten_c_body="external",
                    generated_c_only=False,
                    native_leaf=symbol.startswith("__"),
                    leaf_class="unclassified" if symbol.startswith("__") else "",
                    leaf_reason="",
                    factory_loader="",
                    plan_coverage="native_binding",
                    vm_binding=vm,
                    aot_binding=str(getattr(entry, "aot", "") or ""),
                    covered_c_deletion="",
                    blocker="declared module is outside the stdlib boundary manifest",
                )
            )

    classify_queue(modules, rows)
    return modules, rows, defects


QUEUE_ORDER = (
    "deepen-existing-xray",
    "establish-xray-source",
    "needs-generic-loader",
    "native-leaf-only",
    "test-only",
)


def classify_queue(modules: list[ModuleRow], rows: list[SymbolRow]) -> None:
    """Assign each module the migration queue it belongs to.

    Two questions separate the queues. Can the standard-library owner start
    work on this module now: a module whose only residue is the per-module C
    loader cannot, because deleting that loader needs a generic source-derived
    load path the standard library does not own. And does the module already
    have an Xray semantic source: deleting residue behind an existing `.xr`
    surface is a different job, at a different risk, from writing the first
    `.xr` body for a module that has never had one.
    """
    by_module: dict[str, list[SymbolRow]] = {}
    for row in rows:
        by_module.setdefault(row.module, []).append(row)

    for mrow in modules:
        module_rows = by_module.get(mrow.name, [])
        semantic_c = [
            r
            for r in module_rows
            if r.handwritten_c_body
            and not r.xray_body
            and r.kind != "module-factory"
            and not r.native_leaf
        ]
        if mrow.audience == "test-only":
            mrow.queue = "test-only"
            mrow.queue_reason = "not part of the production standard-library surface"
            continue
        if not mrow.semantic_source.endswith(".xr"):
            mrow.queue = "establish-xray-source"
            mrow.queue_reason = (
                f"{mrow.policy} module declares {mrow.semantic_source} as its semantic "
                f"source, so an .xr body has to exist before any C owner can be deleted"
            )
            continue
        if mrow.public_native:
            mrow.queue = "deepen-existing-xray"
            mrow.queue_reason = (
                f"{len(mrow.public_native)} public native symbols still bypass the .xr surface"
            )
            continue
        if semantic_c:
            mrow.queue = "deepen-existing-xray"
            mrow.queue_reason = (
                f"{len(semantic_c)} handwritten C functions remain behind the .xr surface"
            )
            continue
        loaders = [r for r in module_rows if r.kind == "module-factory"]
        if loaders:
            mrow.queue = "needs-generic-loader"
            mrow.queue_reason = (
                "only residue is the per-module C loader, which a generic "
                "source-derived load path has to replace"
            )
            continue
        mrow.queue = "native-leaf-only"
        mrow.queue_reason = "no handwritten C owner and no per-module loader"


def is_semantic_c_owner(row: SymbolRow) -> bool:
    """Report whether a row is a handwritten C owner of standard-library meaning.

    A per-module loader and an approved private leaf are residue of their own
    kinds with their own gates. Everything else with a C body is a semantic
    owner, which is the quantity the completion definition drives to zero.
    """
    if row.xray_body or not row.handwritten_c_body:
        return False
    if row.kind == "module-factory" or row.native_leaf:
        return False
    return True


def summarize(modules: list[ModuleRow], rows: list[SymbolRow]) -> dict[str, Any]:
    production = [m for m in modules if m.audience == "production"]
    # Rows are selected by their own audience, not by manifest membership.
    # Selecting by membership would drop the rows that have no owning module --
    # the C under `stdlib/` outside any module, and the declared modules the
    # manifest never claims -- which are exactly the rows that most need
    # counting, because nothing else governs them.
    production_rows = [r for r in rows if r.audience == "production"]
    return {
        "modules": len(modules),
        "production_modules": len(production),
        "test_only_modules": len(modules) - len(production),
        "symbols": len(rows),
        "production_symbols": len(production_rows),
        "xray_body_symbols": sum(1 for r in rows if r.xray_body),
        "handwritten_c_semantic_owner": sum(
            1 for r in production_rows if is_semantic_c_owner(r)
        ),
        "native_leaf_symbols": sum(1 for r in rows if r.native_leaf),
        "production_native_leaf_symbols": sum(
            1 for r in production_rows if r.native_leaf
        ),
        "unclassified_native_leaf": sum(
            1 for r in rows if r.native_leaf and r.leaf_class == "unclassified"
        ),
        "whole_module_native_policy": sum(
            1
            for m in production
            if m.policy in {"native_primitive", "native_library"}
        ),
        "public_native_symbols": sum(len(m.public_native) for m in production),
        "module_specific_c_loaders": sum(
            1 for r in rows if r.kind == "module-factory"
        ),
        "production_module_specific_c_loaders": sum(
            1 for r in production_rows if r.kind == "module-factory"
        ),
        "production_modules_without_xray_source": sum(
            1 for m in production if not m.semantic_source.endswith(".xr")
        ),
        "modules_entering_module_graph": sum(
            1 for m in modules if m.enters_module_graph
        ),
        "c_functions_without_manifest_module": sum(
            1 for r in rows if r.blocker.startswith("C source under stdlib/")
        ),
        "def_symbols_without_manifest_module": sum(
            1 for r in rows if r.blocker.startswith("declared module is outside")
        ),
        "queue": {key: sum(1 for m in modules if m.queue == key) for key in QUEUE_ORDER},
    }


def markdown(modules: list[ModuleRow], rows: list[SymbolRow], base: str) -> str:
    counts = summarize(modules, rows)
    out: list[str] = []
    out.append("# Standard-library per-symbol ownership inventory")
    out.append("")
    out.append(f"Source commit: `{base}`")
    out.append("")
    out.append("Generated by `scripts/stdlib_symbol_inventory.py`. Do not hand-edit.")
    out.append("")
    out.append("## Totals")
    out.append("")
    out.append("| metric | value |")
    out.append("|---|---:|")
    for key in (
        "modules",
        "production_modules",
        "test_only_modules",
        "symbols",
        "production_symbols",
        "xray_body_symbols",
        "handwritten_c_semantic_owner",
        "native_leaf_symbols",
        "unclassified_native_leaf",
        "whole_module_native_policy",
        "public_native_symbols",
        "module_specific_c_loaders",
        "production_module_specific_c_loaders",
        "production_modules_without_xray_source",
        "modules_entering_module_graph",
        "c_functions_without_manifest_module",
        "def_symbols_without_manifest_module",
    ):
        out.append(f"| {key} | {counts[key]} |")
    out.append("")
    out.append("## Modules")
    out.append("")
    out.append(
        "| module | layer | policy | audience | symbols | xray body | handwritten C | native leaf | C functions | queue |"
    )
    out.append("|---|---|---|---|---:|---:|---:|---:|---:|---|")
    for m in sorted(modules, key=lambda x: x.name):
        out.append(
            f"| {m.name} | {m.layer} | {m.policy} | {m.audience} | {m.symbol_count} | "
            f"{m.xray_body_symbols} | {m.handwritten_c_symbols} | {m.native_leaf_symbols} | "
            f"{m.c_function_count} | {m.queue} |"
        )
    out.append("")
    out.append("## Queue")
    out.append("")
    for key in QUEUE_ORDER:
        members = [m for m in modules if m.queue == key]
        out.append(f"### {key} ({len(members)})")
        out.append("")
        for m in sorted(members, key=lambda x: x.name):
            out.append(f"- `{m.name}` — {m.queue_reason}")
        out.append("")
    out.append("## Symbols")
    out.append("")
    out.append(
        "| module | symbol | kind | audience | semantic source | xray body | handwritten C | native leaf | leaf class | vm binding | aot binding |"
    )
    out.append("|---|---|---|---|---|---|---|---|---|---|---|")
    for r in sorted(rows, key=lambda x: (x.module, x.kind, x.symbol)):
        out.append(
            f"| {r.module} | `{r.symbol}` | {r.kind} | {r.audience} | {r.semantic_source} | "
            f"{'yes' if r.xray_body else 'no'} | {r.handwritten_c_body or '-'} | "
            f"{'yes' if r.native_leaf else 'no'} | {r.leaf_class or '-'} | "
            f"{r.vm_binding or '-'} | {r.aot_binding or '-'} |"
        )
    out.append("")
    return "\n".join(out)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--json", dest="json_path", help="write the machine-readable inventory")
    parser.add_argument("--markdown", dest="md_path", help="write the human-readable matrix")
    parser.add_argument("--base", default="", help="source commit recorded in the output")
    parser.add_argument(
        "--check",
        action="store_true",
        help="fail when a symbol or C function is left unattributed",
    )
    args = parser.parse_args(argv)

    root = Path(args.root).resolve()
    modules, rows, defects = build_rows(root)
    counts = summarize(modules, rows)

    payload = {
        "schema": SCHEMA,
        "base": args.base,
        "counts": counts,
        "modules": [asdict(m) for m in modules],
        # Empty strings and false flags are dropped from symbol rows: with a
        # row per symbol and per C function they would triple the artifact for
        # no information, and a missing key reads the same as an empty one.
        "symbols": [
            {key: value for key, value in asdict(r).items() if value not in ("", False)}
            for r in rows
        ],
    }

    if args.json_path:
        Path(args.json_path).write_text(
            json.dumps(payload, indent=2, sort_keys=False) + "\n", encoding="utf-8"
        )
    if args.md_path:
        Path(args.md_path).write_text(markdown(modules, rows, args.base), encoding="utf-8")

    if args.check:
        if defects:
            for defect in defects:
                print(f"FAIL: {defect}", file=sys.stderr)
            return 1
        print(
            f"OK: {counts['symbols']} stdlib symbols attributed across "
            f"{counts['modules']} modules"
        )
        return 0

    print(json.dumps(counts, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

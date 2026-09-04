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

from stdlib_manifest import api_inventory, load_manifest, load_stdlibgen, load_toml  # noqa: E402


SCHEMA = 1

# The per-symbol allowlist of accepted native leaves. The `leaf_class` column
# is authoritative, so it may only be written from a record that names the
# whole boundary: which ABI the leaf crosses, what it does with ownership,
# which effects it has, who provides the implementation, and the condition
# under which the leaf can be deleted. A leaf with no such record stays
# `unclassified` and keeps failing its gate.
NATIVE_LEAF_ALLOWLIST_PATH = Path("stdlib/native_leaf_allowlist.toml")

NATIVE_LEAF_ALLOWLIST_SCHEMA = 1

# The closed set of approved classes, each with the judgement that admits a
# leaf to it. The set is closed on purpose: `leaf_class` is a free string in
# the row, so an open column would let a misspelled class approve a leaf
# silently -- the gate treats anything that is neither empty nor
# `unclassified` as approved.
#
# A class names *which boundary* the leaf crosses and nothing else. Whether
# that boundary is minimal, and what would have to become true for the leaf to
# be deleted, is the record's `deletion_trigger` -- which is why the schema
# carries one at all. Folding the two questions into the class name would
# force a leaf that crosses the host ABI through a short-write drain loop to be
# either misfiled or unclassified, when the honest reading is that it crosses
# the host ABI and has a deletion trigger a filed capability gap controls.
LEAF_CLASSES = {
    "host_abi_leaf": (
        "the leaf's business is crossing the operating system's ABI: a syscall, "
        "a libc entry point or a platform API that Xray cannot issue itself"
    ),
    "runtime_leaf": (
        "the leaf reads or drives a runtime-owned primitive -- scheduler, "
        "netpoll, channel, coroutine, reclamation domain, or the representation "
        "of a native handle -- that Xray has no surface for"
    ),
    "machine_intrinsic_leaf": (
        "the leaf is a hardware instruction or a libm routine whose accuracy "
        "contract Xray arithmetic cannot restate"
    ),
    "security_provider_leaf": (
        "the leaf is the boundary to a cryptographic provider, and reimplementing "
        "it in Xray would move a security guarantee into unaudited code"
    ),
    "alloc_leaf": (
        "the leaf is an allocator or object-representation primitive that the "
        "language's own object model is built on"
    ),
    "native_record_shape_leaf": (
        "the declaration is not a call at all: it is the record layout a native "
        "leaf's return value needs so both backends share one field schema"
    ),
    "build_identity_leaf": (
        "the leaf answers a fact about the build rather than about the running "
        "machine -- the target platform, the architecture, whether an optional "
        "provider was linked -- fixed by the preprocessor and unobservable from "
        "Xray source"
    ),
    "native_engine_leaf": (
        "the leaf is an entry point into a self-contained algorithm engine "
        "Xray ships in C -- a codec, a parser, a matcher -- that crosses no "
        "external boundary at all. Exactly three things keep such an engine in "
        "C: the language cannot represent the data it consumes or produces, "
        "rewriting the entry point would fork a definition the engine's other "
        "entry points share, or the engine holds runtime-owned state. The "
        "deletion trigger has to name which of the three; a leaf kept in C for "
        "throughput alone does not belong in this class"
    ),
}

# Ownership is `return_ownership` from the `.def` entry when it declares one,
# so the closed set is exactly the vocabulary that column uses plus the value
# it omits: a leaf whose return carries no ownership at all.
LEAF_OWNERSHIP = {
    "none": "the return transfers no ownership (a scalar, a unit, or a bool)",
    "fresh": "the return is newly allocated and the caller owns it",
    "borrowed_static": "the return borrows storage with static lifetime",
}

# Effect is a comma-separated token sequence. `nothrow` is exclusive: a leaf
# that cannot throw and cannot suspend has nothing else to state.
LEAF_EFFECT_TOKENS = {
    "nothrow": "cannot throw and cannot suspend",
    "may_throw": "can raise an Xray exception",
    "suspends": "can park the coroutine and return the worker to the scheduler",
}

# A `.def` entry may state its effect as the set of error variants it raises;
# those variants are legal effect tokens so the allowlist can restate the
# declaration without losing which errors it names.
THROWN_VARIANT_RE = re.compile(r"^(?:__)?[A-Z][A-Za-z0-9_]*\.[A-Za-z_][A-Za-z0-9_]*$")

LEAF_RECORD_KEYS = {
    "module",
    "symbol",
    "class",
    "abi",
    "ownership",
    "effect",
    "provider",
    "deletion_trigger",
}

# A non-public module is test-only unless it is the prelude, which is
# unimportable because the language installs it implicitly, not because it is
# a test fixture.
PRODUCTION_EXCEPT_NON_PUBLIC = {"prelude"}

# Modules whose semantic source is the compiler's own declaration input rather
# than a module body, so asking them for an `.xr` source asks for something
# that cannot exist.
#
# The prelude is the only one. `stdlib/prelude/builtin_symbols.def` is expanded
# by the C preprocessor into eight compiler translation units -- xanalyzer.c,
# xtype_ref_resolve.c, xanalyzer_builtin_interfaces.c, xanalyzer_builtins.c and
# xlsp_keywords.c -- so it has to exist before any `.xr` can be parsed at all,
# and a file resolved at run time cannot feed a table built at C compile time.
# The module exports nothing (stdlib/prelude/prelude.c: "No exports yet"),
# while check_stdlib_boundary.py requires an `xray_semantic` module's `.xr` to
# export at least one item, so the two requirements are mutually exclusive.
# Re-declaring its enums in Xray would mint a second nominal identity for each
# -- blockers/a-stdlib-enum-identity-blocks-type-migration.md -- which is a
# silent wrong answer rather than progress.
#
# The exception covers only "must have an `.xr` source" and "must not declare a
# whole-module native policy". Every other property is still counted, so the
# prelude's handwritten C semantic owners and its module-specific C loader stay
# on the ledger.
COMPILER_OWNED_SEMANTIC_SOURCE = {"prelude"}


def has_compiler_owned_semantic_source(module: "ModuleRow", xray_owned_symbols: int) -> bool:
    """Whether the exception above applies to this module right now.

    Re-checked rather than asserted: the exception holds only while the module
    still has the shape its reason describes, so a module that grows a public
    Xray symbol or a `.xr` semantic source loses it automatically.
    """
    return (
        module.name in COMPILER_OWNED_SEMANTIC_SOURCE
        and module.semantic_source.endswith(".def")
        and xray_owned_symbols == 0
    )

# Function-like C constructs that the definition scanner must not mistake for
# a definition when they start a line.
C_CONTROL_KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "else", "do"}

C_BODY_START_RE = re.compile(r"\)[ \t\n]*\{")

C_IDENTIFIER_CHARS = set(
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"
)


def c_definition_names(text: str) -> list[tuple[str, int]]:
    """Return the (name, offset) of every function defined in a C source.

    A declaration can carry an attribute macro that is itself written as a
    call -- `static intptr_t XR_ACQUIRE("tag") open_handle(const char *p) {` --
    and a parameter can be a function pointer. Both put more than one
    parenthesis group ahead of the body, so taking the first identifier
    followed by `(` names the macro instead of the function and drops the
    function entirely. Match the body brace first and walk left through
    balanced parentheses instead, which names the group the body belongs to
    whatever precedes it.
    """
    out: list[tuple[str, int]] = []
    for match in C_BODY_START_RE.finditer(text):
        depth = 0
        index = match.start()
        while index >= 0:
            char = text[index]
            if char == ")":
                depth += 1
            elif char == "(":
                depth -= 1
                if depth == 0:
                    break
            elif char in ";}":
                # A statement or block boundary inside the scan means this
                # brace closes something that is not a function definition.
                index = -1
                break
            index -= 1
        if index < 0:
            continue
        end = index
        while end > 0 and text[end - 1] in " \t":
            end -= 1
        start = end
        while start > 0 and text[start - 1] in C_IDENTIFIER_CHARS:
            start -= 1
        if start == end:
            continue
        name = text[start:end]
        if name in C_CONTROL_KEYWORDS or name[0].isdigit():
            continue
        out.append((name, start))
    return out

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
    loader_declarations: list[str] = field(default_factory=list)
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


def c_without_comments(text: str) -> str:
    """Mask C comments while preserving strings, escapes and line numbers."""
    out = list(text)
    index = 0
    state = "code"
    while index < len(text):
        char = text[index]
        nxt = text[index + 1] if index + 1 < len(text) else ""
        if state == "code":
            if char == '"':
                state = "string"
            elif char == "'":
                state = "char"
            elif char == "/" and nxt == "/":
                out[index] = out[index + 1] = " "
                state = "line-comment"
                index += 1
            elif char == "/" and nxt == "*":
                out[index] = out[index + 1] = " "
                state = "block-comment"
                index += 1
        elif state in {"string", "char"}:
            terminator = '"' if state == "string" else "'"
            if char == "\\":
                index += 1
            elif char == terminator:
                state = "code"
        elif state == "line-comment":
            if char == "\n":
                state = "code"
            else:
                out[index] = " "
        elif state == "block-comment":
            if char == "*" and nxt == "/":
                out[index] = out[index + 1] = " "
                state = "code"
                index += 1
            elif char != "\n":
                out[index] = " "
        index += 1
    return "".join(out)


def c_functions(root: Path) -> dict[str, list[tuple[str, str]]]:
    """Map a module directory to the (function, source file) pairs it defines.

    `stdlib_cache.c` sits directly under `stdlib/` and belongs to no module; it
    is reported under the synthetic `_stdlib` owner so the C side still closes.
    """
    out: dict[str, list[tuple[str, str]]] = {}
    for path in sorted((root / "stdlib").rglob("*.c")):
        owner = path.parent.name if path.parent != root / "stdlib" else "_stdlib"
        for name, _offset in c_definition_names(read(path)):
            out.setdefault(owner, []).append((name, rel(root, path)))
    return out


DEBUG_OUTPUT_RE = re.compile(
    r"^(?![ \t]*(?://|\*)).*\b(?:fprintf[ \t]*\([ \t]*stderr|printf[ \t]*\()[^;]*\[DBG",
    re.MULTILINE,
)


def debug_output_defects(root: Path) -> list[str]:
    """Report standard-library C that writes debug output on a normal path.

    Such a write is observable behaviour that no declaration accounts for: the
    module's `.xr` surface does not describe it, no contract case covers it,
    and every caller of the affected function pays for it. It is reported with
    the unattributed items because it is one -- output belonging to no symbol's
    stated meaning.
    """
    defects: list[str] = []
    for path in sorted((root / "stdlib").rglob("*.[ch]")):
        text = read(path)
        for match in DEBUG_OUTPUT_RE.finditer(text):
            line = text.count("\n", 0, match.start()) + 1
            defects.append(
                f"{rel(root, path)}:{line}: debug output on a normal execution path"
            )
    return defects


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


def manual_native_class_exports(module: dict) -> dict[str, tuple[str, str]]:
    """Trace manifest-declared manual native classes to what publishes them.

    Some runtime-owned classes reach a module through neither a ``.def``
    declaration nor an ``.xr`` export: the VM registers the class and the module
    publishes it under its own name. That used to be a hand-written factory
    call, which had to be traced by scanning C. It is a ``native_type_exports``
    row now, so the declaration is the registration and is its own authority.
    """
    return {
        str(row["name"]): ("stdlib/stdlib_boundary.toml", str(row["builtin_type"]))
        for row in module.get("native_type_exports", ())
        if row.get("name") and row.get("builtin_type")
    }


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

    The proposal orders the migration queue and is the starting point for
    writing a leaf's allowlist record, but it never approves one by itself: it
    is derived from the entry's layer, which says where the leaf sits and not
    what it does. Approval is `stdlib/native_leaf_allowlist.toml`, where the
    record has to name ABI, ownership, effect, provider and deletion trigger.
    """
    layer = str(getattr(entry, "layer", "") or "")
    if module in {"crypto"}:
        return "security_provider_leaf", "cryptographic provider boundary"
    if layer == "runtime":
        return "runtime_leaf", "scheduler, netpoll or coroutine primitive"
    if layer == "system":
        return "host_abi_leaf", "operating-system ABI boundary"
    if module == "math":
        return "machine_intrinsic_leaf", "hardware or libm intrinsic"
    if layer == "alloc":
        # `alloc` used to imply an allocator primitive, and no leaf carried it.
        # It now covers the compress coder and the regex engine as well, which
        # allocate but are not allocators, so the layer no longer picks a class
        # on its own and saying otherwise would propose the wrong one.
        return (
            "unclassified",
            "layer 'alloc' spans allocators, coders and engines; read the module",
        )
    return "unclassified", f"no allowlist class derivable from layer {layer!r}"


@dataclass
class LeafRecord:
    """One approved-leaf record, and whether it is well-formed enough to use."""

    module: str
    symbol: str
    leaf_class: str
    abi: str
    ownership: str
    effect: str
    provider: str
    deletion_trigger: str
    errors: list[str] = field(default_factory=list)

    @property
    def valid(self) -> bool:
        return not self.errors

    @property
    def effect_tokens(self) -> list[str]:
        return [token.strip() for token in self.effect.split(",") if token.strip()]

    def reason(self) -> str:
        """Render the record as the row's `leaf_reason`.

        The five approval facts travel with the row rather than staying in the
        manifest, so a reader of the inventory or of a gate failure sees why
        the boundary was accepted without opening a second file.
        """
        return (
            f"abi={self.abi}; ownership={self.ownership}; effect={self.effect}; "
            f"provider={self.provider}; deletion_trigger={self.deletion_trigger}"
        )


def effect_token_error(token: str) -> str:
    """Report why an effect token is not a legal one, or the empty string."""
    if token in LEAF_EFFECT_TOKENS:
        return ""
    if THROWN_VARIANT_RE.fullmatch(token):
        return ""
    return (
        f"effect token {token!r} is neither one of "
        f"{sorted(LEAF_EFFECT_TOKENS)} nor an Enum.Variant an entry can raise"
    )


def leaf_record_errors(record: LeafRecord, raw: dict[str, Any]) -> list[str]:
    """Validate one allowlist record against the closed vocabularies.

    Every column is checked here rather than at the point of use so a record
    that cannot be trusted is rejected once, whole, and cannot approve the leaf
    it names through some path that skipped a check.
    """
    errors: list[str] = []
    unknown = sorted(set(raw) - LEAF_RECORD_KEYS)
    if unknown:
        # An unrecognised key is a misspelled one far more often than it is a
        # new fact, and a misspelled `deletion_trigger` would otherwise read as
        # a record with an empty trigger that still approves its leaf.
        errors.append(f"unknown key(s) {unknown}; allowed keys are {sorted(LEAF_RECORD_KEYS)}")
    for name in ("abi", "provider", "deletion_trigger"):
        if not getattr(record, name):
            errors.append(f"{name} is empty; an approved class needs it stated")
    if record.leaf_class not in LEAF_CLASSES:
        errors.append(
            f"class {record.leaf_class or '<empty>'!r} is not one of "
            f"{sorted(LEAF_CLASSES)}"
        )
    if record.ownership not in LEAF_OWNERSHIP:
        errors.append(
            f"ownership {record.ownership or '<empty>'!r} is not one of "
            f"{sorted(LEAF_OWNERSHIP)}"
        )
    tokens = record.effect_tokens
    if not tokens:
        errors.append("effect is empty; an approved class needs it stated")
    for token in tokens:
        problem = effect_token_error(token)
        if problem:
            errors.append(problem)
    if "nothrow" in tokens and len(tokens) > 1:
        errors.append("effect 'nothrow' is exclusive; it cannot be combined with other tokens")
    return errors


def load_leaf_allowlist(root: Path) -> tuple[dict[tuple[str, str], LeafRecord], list[str]]:
    """Read the per-symbol native-leaf allowlist.

    A missing or malformed file is a defect rather than an empty allowlist: an
    unreadable manifest would otherwise quietly demote every leaf to
    `unclassified`, which reads as unfinished migration work instead of as a
    broken governance input.
    """
    path = root / NATIVE_LEAF_ALLOWLIST_PATH
    key = NATIVE_LEAF_ALLOWLIST_PATH.as_posix()
    if not path.is_file():
        return {}, [f"{key}: the native-leaf allowlist is missing"]
    try:
        raw = load_toml(path)
    except Exception as error:  # noqa: BLE001 - the parse error is the defect
        return {}, [f"{key}: cannot parse the native-leaf allowlist: {error}"]

    defects: list[str] = []
    if raw.get("schema") != NATIVE_LEAF_ALLOWLIST_SCHEMA:
        defects.append(
            f"{key}: schema {raw.get('schema')!r} is not the current schema "
            f"{NATIVE_LEAF_ALLOWLIST_SCHEMA}"
        )
    records: dict[tuple[str, str], LeafRecord] = {}
    for index, item in enumerate(raw.get("leaf", ())):
        if not isinstance(item, dict):
            defects.append(f"{key}: [[leaf]] #{index + 1} is not a table")
            continue
        module = str(item.get("module", "")).strip()
        symbol = str(item.get("symbol", "")).strip()
        if not module or not symbol:
            defects.append(
                f"{key}: [[leaf]] #{index + 1} has no module/symbol pair to attach to"
            )
            continue
        # Values are stripped so a column holding only whitespace reads as the
        # empty column it is, rather than passing the "is it stated" checks.
        record = LeafRecord(
            module=module,
            symbol=symbol,
            leaf_class=str(item.get("class", "")).strip(),
            abi=str(item.get("abi", "")).strip(),
            ownership=str(item.get("ownership", "")).strip(),
            effect=str(item.get("effect", "")).strip(),
            provider=str(item.get("provider", "")).strip(),
            deletion_trigger=str(item.get("deletion_trigger", "")).strip(),
        )
        record.errors = leaf_record_errors(record, item)
        if (module, symbol) in records:
            # Two records for one leaf leave which of them approves it up to
            # file order. Marking the surviving record invalid is enough to
            # refuse the leaf, since only the surviving record is ever read.
            defects.append(f"{key}: {module}::{symbol} has more than one [[leaf]] record")
            record.errors.append("duplicates an earlier record for the same leaf")
        for error in record.errors:
            defects.append(f"{key}: {module}::{symbol}: {error}")
        records[(module, symbol)] = record
    return records, defects


def leaf_record_disagreements(entry: Any, record: LeafRecord) -> list[str]:
    """Report where a record contradicts the `.def` entry it describes.

    The `.def` declaration is the authority for ownership and effect, so the
    allowlist restates those facts rather than asserting new ones. A record
    that disagrees is describing a leaf other than the one it names.
    """
    problems: list[str] = []
    declared_ownership = str(getattr(entry, "return_ownership", "") or "")
    if declared_ownership and record.ownership != declared_ownership:
        problems.append(
            f"ownership {record.ownership!r} contradicts the .def "
            f"return_ownership {declared_ownership!r}"
        )
    tokens = set(record.effect_tokens)
    declared_effect = str(getattr(entry, "effect", "") or "")
    missing = [
        token
        for token in (part.strip() for part in declared_effect.split(","))
        if token and token not in tokens
    ]
    if missing:
        problems.append(
            f"effect drops the .def-declared effect(s) {missing}"
        )
    if str(getattr(entry, "vm_binding", "") or "") == "yieldable" and "suspends" not in tokens:
        problems.append(
            "effect omits 'suspends' for a leaf the .def binds as yieldable"
        )
    return problems


def stale_leaf_record_defects(
    records: dict[tuple[str, str], LeafRecord],
    used: set[tuple[str, str]],
) -> list[str]:
    """Report allowlist records that no declared native leaf reached.

    Direction 1 of the fail-closed contract. An approval nothing stands behind
    is worse than a missing one: it reads as a reviewed boundary while there is
    no leaf there, and the next leaf to take that name would inherit an
    approval nobody wrote for it.

    The wording stays neutral about how the record got here. It covers the
    record left behind by a deleted leaf and the record written ahead of a
    rename that has not landed yet, and neither is safe to leave unreported --
    from the manifest alone the two are the same thing.
    """
    return [
        f"{NATIVE_LEAF_ALLOWLIST_PATH.as_posix()}: {module}::{symbol} has an "
        f"allowlist record that no declared native leaf reached"
        for module, symbol in sorted(set(records) - used)
    ]


def classify_leaf(
    module: str,
    symbol: str,
    entry: Any,
    records: dict[tuple[str, str], LeafRecord],
) -> tuple[str, str, list[str]]:
    """Decide a leaf's authoritative class from the allowlist.

    Three ways to fail, all closed toward `unclassified`, because the class
    column is what the completion gate reads and an approval it cannot justify
    is worse than an admitted gap:

    1. No record for a leaf the repository has -- the leaf keeps failing its
       gate. This is unfinished work, not a defect, so nothing is reported
       here; the gate names it.
    2. A record the loader rejected -- a malformed record must approve nothing.
    3. A record that contradicts the `.def` entry -- reported as a defect,
       because a manifest describing a different leaf is a broken input rather
       than missing work.
    """
    record = records.get((module, symbol))
    if record is None:
        return (
            "unclassified",
            f"no record in {NATIVE_LEAF_ALLOWLIST_PATH.as_posix()}",
            [],
        )
    if not record.valid:
        return (
            "unclassified",
            f"allowlist record rejected: {'; '.join(record.errors)}",
            [],
        )
    disagreements = leaf_record_disagreements(entry, record)
    if disagreements:
        return (
            "unclassified",
            f"allowlist record contradicts the declaration: {'; '.join(disagreements)}",
            [
                f"{NATIVE_LEAF_ALLOWLIST_PATH.as_posix()}: {module}::{symbol}: {item}"
                for item in disagreements
            ],
        )
    return record.leaf_class, record.reason(), []


def build_rows(root: Path) -> tuple[list[ModuleRow], list[SymbolRow], list[str]]:
    manifest = load_manifest(root)
    defs = def_entries_by_module(root)
    xray_symbols = xray_public_symbols(root)
    cfuncs = c_functions(root)
    defects: list[str] = debug_output_defects(root)
    leaf_records, leaf_defects = load_leaf_allowlist(root)
    defects.extend(leaf_defects)
    # Which allowlist records an actual leaf reached. Anything left over is a
    # record for a leaf the repository no longer has.
    leaf_records_used: set[tuple[str, str]] = set()

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
        loader_declarations = [
            field_name
            for field_name in ("native_type_exports", "native_fn_exports", "load_time_classes")
            if module.get(field_name)
        ]
        public_native = [str(x) for x in module.get("public_native", ())]
        manual_public_native = {
            str(x) for x in module.get("manual_public_native", ())
        }

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
            loader_declarations=loader_declarations,
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
            # A leaf is a `fn` row that reaches C with an ownership and an
            # effect to record. Every declaration kind carries a visibility
            # now, but a class declaration, a method binding or a field is not
            # a call into C, and the allowlist schema has no shape for one:
            # classify_leaf checks a record's ownership against the row's
            # return_ownership, which only a fn row declares. Reading
            # visibility off any entry would file each internal class and
            # method as an unclassified leaf and turn the allowlist gate red
            # for rows it cannot describe.
            is_leaf = symbol.startswith("__") or (
                type(entry).__name__ == "StdlibEntry" and entry.is_internal
            )
            c_body = c_by_name.get(vm, "")
            if vm and vm in c_by_name:
                attributed_c[name].add(vm)
            if aot and aot in c_by_name:
                attributed_c[name].add(aot)
            if is_leaf:
                leaf_records_used.add((name, symbol))
                leaf_class, leaf_reason, leaf_row_defects = classify_leaf(
                    name, symbol, entry, leaf_records
                )
                defects.extend(leaf_row_defects)
                if leaf_class == "unclassified":
                    # Keep the queue-ordering proposal visible on a leaf that
                    # has no usable record, so the failing gate still says
                    # which class the record would most likely carry.
                    proposal, why = leaf_class_proposal(name, entry)
                    leaf_reason = f"{leaf_reason}; proposed {proposal}: {why}"
            else:
                leaf_class, leaf_reason = "", ""
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
                    leaf_class=leaf_class,
                    leaf_reason=leaf_reason,
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

        # A declared public native symbol that none of the three sources
        # reached. `sync` exposes five such names: they have no `.def` block
        # and are not `.xr` exports, because the VM installs them as builtin
        # globals. Without this pass the inventory would report the module as
        # having no public native surface while the manifest declares five,
        # and every gate reading the inventory would be blind to them.
        seen = {r.symbol for r in rows if r.module == name}
        manual_exports = manual_native_class_exports(module)
        for symbol in public_native:
            if symbol in seen:
                continue
            if symbol in manual_public_native and symbol in manual_exports:
                source, builtin_type = manual_exports[symbol]
                rows.append(
                    SymbolRow(
                        module=name,
                        symbol=symbol,
                        kind="manual-native-class",
                        audience=audience,
                        semantic_source=source,
                        xray_body=False,
                        handwritten_c_body=source,
                        generated_c_only=False,
                        native_leaf=False,
                        leaf_class="",
                        leaf_reason="",
                        factory_loader="",
                        plan_coverage="native_binding",
                        vm_binding=f"{builtin_type}:{symbol}",
                        aot_binding="",
                        covered_c_deletion=source,
                        blocker="runtime native class is still a public C-owned surface",
                    )
                )
                continue
            rows.append(
                SymbolRow(
                    module=name,
                    symbol=symbol,
                    kind="undeclared-public-native",
                    audience=audience,
                    semantic_source="",
                    xray_body=False,
                    handwritten_c_body="external",
                    generated_c_only=False,
                    native_leaf=False,
                    leaf_class="",
                    leaf_reason="",
                    factory_loader="",
                    plan_coverage="unknown",
                    vm_binding="",
                    aot_binding="",
                    covered_c_deletion="",
                    blocker=(
                        "declared public native with no .def entry, .xr export "
                        "or stdlib C definition to trace it to"
                    ),
                )
            )
            defects.append(
                f"stdlib/stdlib_boundary.toml: {name}.{symbol} is declared public "
                f"native but has no traceable declaration source"
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
            # A leaf outside the manifest is classified through the same
            # allowlist as any other. Skipping it here would let a leaf escape
            # the approval requirement precisely by being ungoverned.
            is_leaf = symbol.startswith("__")
            if is_leaf:
                leaf_records_used.add((module, symbol))
                leaf_class, leaf_reason, leaf_row_defects = classify_leaf(
                    module, symbol, entry, leaf_records
                )
                defects.extend(leaf_row_defects)
            else:
                leaf_class, leaf_reason = "", ""
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
                    native_leaf=is_leaf,
                    leaf_class=leaf_class,
                    leaf_reason=leaf_reason,
                    factory_loader="",
                    plan_coverage="native_binding",
                    vm_binding=vm,
                    aot_binding=str(getattr(entry, "aot", "") or ""),
                    covered_c_deletion="",
                    blocker="declared module is outside the stdlib boundary manifest",
                )
            )

    defects.extend(stale_leaf_record_defects(leaf_records, leaf_records_used))

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


def leaf_is_approved(row: SymbolRow) -> bool:
    """Report whether a native leaf carries an approved allowlist class.

    Membership of the closed class set is the test, not "is not
    `unclassified`". A row whose class is neither empty nor `unclassified` but
    is also not a class anyone defined would otherwise read as approved, which
    is exactly how a misspelling would let a leaf through.
    """
    return row.native_leaf and row.leaf_class in LEAF_CLASSES


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


def attach_probe_results(modules: list[ModuleRow], path: Path) -> None:
    """Record each module's measured backend outcome on its row.

    Whether a module's public surface actually runs is a measurement, not a
    property of the source, so it is read from a probe report rather than
    derived here. A module the report does not mention keeps an empty record,
    which reads as unmeasured rather than as passing.
    """
    try:
        report = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return
    entries = report.get("modules") or report.get("results") or []
    if isinstance(entries, dict):
        entries = list(entries.values())
    measured: dict[str, dict[str, Any]] = {}
    for entry in entries:
        if not isinstance(entry, dict):
            continue
        name = str(entry.get("module", ""))
        if not name:
            continue
        vm = entry.get("vm") or {}
        aot = entry.get("aot") or {}
        refusal = entry.get("first_refusal") or {}
        generated = entry.get("generated_c") or {}
        measured[name] = {
            "vm_returncode": vm.get("returncode"),
            "aot_returncode": aot.get("returncode"),
            "first_refusal_code": refusal.get("code") or "",
            "first_refusal_stage": refusal.get("stage") or "",
            "generated_c_reproducible": generated.get("identical"),
        }
    for module in modules:
        module.probe = measured.get(module.name, {})


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
            1 for r in rows if r.native_leaf and not leaf_is_approved(r)
        ),
        "whole_module_native_policy": sum(
            1
            for m in production
            if m.policy in {"native_primitive", "native_library"}
            and not has_compiler_owned_semantic_source(m, m.xray_body_symbols)
        ),
        "public_native_symbols": sum(len(m.public_native) for m in production),
        "module_specific_c_loaders": sum(
            1 for r in rows if r.kind == "module-factory"
        ),
        "production_module_specific_c_loaders": sum(
            1 for r in production_rows if r.kind == "module-factory"
        ),
        "production_modules_without_xray_source": sum(
            1
            for m in production
            if not m.semantic_source.endswith(".xr")
            and not has_compiler_owned_semantic_source(m, m.xray_body_symbols)
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
        "| module | layer | policy | audience | symbols | xray body | handwritten C | native leaf | C functions | vm | aot | queue |"
    )
    out.append("|---|---|---|---|---:|---:|---:|---:|---:|---|---|---|")
    for m in sorted(modules, key=lambda x: x.name):
        vm = m.probe.get("vm_returncode")
        aot = m.probe.get("aot_returncode")
        code = m.probe.get("first_refusal_code") or ""
        vm_cell = "-" if vm is None else ("ok" if vm == 0 else "fail")
        if aot is None:
            aot_cell = "-"
        elif aot == 0:
            aot_cell = "ok"
        else:
            aot_cell = code or "fail"
        out.append(
            f"| {m.name} | {m.layer} | {m.policy} | {m.audience} | {m.symbol_count} | "
            f"{m.xray_body_symbols} | {m.handwritten_c_symbols} | {m.native_leaf_symbols} | "
            f"{m.c_function_count} | {vm_cell} | {aot_cell} | {m.queue} |"
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
        "--probe-json",
        dest="probe_json",
        help="attach measured VM and AOT outcomes from a backend probe report",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="fail when a symbol or C function is left unattributed",
    )
    args = parser.parse_args(argv)

    root = Path(args.root).resolve()
    modules, rows, defects = build_rows(root)
    if args.probe_json:
        attach_probe_results(modules, Path(args.probe_json))
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

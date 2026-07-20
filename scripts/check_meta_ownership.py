#!/usr/bin/env python3
"""Task-218 meta-ownership inventory: cross-lifetime borrow audit.

The Xray compiler is a hand-written C "lifetime-safety prover" that has
repeatedly shipped its own dangling pointers and state contamination
(see task 209 / 213 / 190 accident list). This script is the machine
audit for defense line 1: it makes "cross-lifetime borrows" countable,
classifiable, and regression-guardable, exactly the way 205's
``check_error_effect_convergence.py`` did for the error-effect graph.

Ownership rules being audited (R-OWN-1..3):

  R-OWN-1  Every ``const char *`` reachable from XiFunc / XiModule /
           XaotBundle / global-evidence structs must point at its owning
           arena / pool / symbol-table intern -- never at AST, analyzer
           session memory, or a caller stack frame.
  R-OWN-2  Never cache a dynamic-array element pointer across an append
           that may ``realloc``; snapshot by value or use an index.
  R-OWN-3  Aggregates crossing a stage boundary (plan / evidence /
           metadata) are deep-copied or ownership-transferred; no shared
           mutable backing store.

Categories emitted:

  A  AST_PTR_INTO_IR    -- an ``Xi*`` / ``Xaot*`` field store whose RHS is
                          an AST-derived name pointer (``node->name`` etc.)
                          with no ``arena_strdup`` / ``intern`` wrapper.
  B  PTR_ACROSS_GROWTH  -- ``&arr[i]`` / ``arr + i`` taken, then the same
                          array is ``push``/``append``/``grow``/``realloc``-ed
                          later in the same function body.
  C  CGEN_BORROWED_NAME -- a CGen ctx borrows a ``const char *`` from a
                          non-ctx-arena source (ctx field store from an
                          AST pointer, or a raw ``const char *`` field in
                          the ctx struct).

This is a P0 inventory gate. By default it runs in RECORD mode: it prints
the classified counts, optionally diffs against a committed baseline, and
ALWAYS exits 0. P1 (borrow clean-up, owned by another agent) drives each
category to zero and then flips the gate fail-closed via ``--max-category``.

A borrow that is provably safe may be exempted inline with an
``owned:`` annotation, e.g. ``/* owned: cg arena */`` on (or immediately
above) the offending line; annotated lines are never counted.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from dataclasses import asdict, dataclass
from pathlib import Path

# ---------------------------------------------------------------------------
# Scan configuration
# ---------------------------------------------------------------------------

# Category A is about IR/plan/evidence structs borrowing AST strings.
A_SCAN_DIRS = ("src/ir", "src/aot", "src/analysis")
# Category B (pointer-across-growth) is a general UAF shape; scan the same
# compiler-meta layers where realloc-backed dynamic arrays live.
B_SCAN_DIRS = ("src/ir", "src/aot", "src/analysis")
# Category C is specific to the CGen context family.
C_FILE_GLOBS = ("src/aot/xi_cgen",)

TEXT_SUFFIXES = (".c", ".inc.c", ".h")
SKIP_DIR_NAMES = {
    ".git",
    "__pycache__",
    "build",
    "build-asan",
    "build-ubsan",
    "build-msan",
    "build-tsan",
    "build-release",
    "build-fuzz",
    "cmake-build-debug",
    "cmake-build-release",
    "node_modules",
}

SELF_NAME = "scripts/check_meta_ownership.py"

CATEGORIES = (
    "AST_PTR_INTO_IR",
    "PTR_ACROSS_GROWTH",
    "CGEN_BORROWED_NAME",
)

# ---------------------------------------------------------------------------
# Regexes
# ---------------------------------------------------------------------------

# Ownership wrappers / intern helpers that neutralize a borrow.
OWNERSHIP_WRAPPER_RE = re.compile(
    r"\b(?:"
    r"arena_strdup|xr_arena_strdup|xi_arena_strdup|xaot_arena_strdup|"
    r"arena_strndup|xr_arena_strndup|"
    r"strdup|xr_strdup|xr_strndup|"
    r"intern|xr_symbol_intern|xr_str_intern|xstr_intern|pool_intern|"
    r"type_pool|xtype_pool|xr_type_pool|"
    r"xr_string_new|xr_str_new|xstrbuf|snprintf|asprintf"
    r")\b"
)

# Inline exemption for a proven-safe borrow.
OWNED_ANNOTATION_RE = re.compile(r"owned\s*:")

# AST-derived name/string pointer on an assignment RHS.
#   node->name, decl->ident, expr->spelling, tn->text, ...
AST_BASE = (
    r"(?:node|decl|ast|expr|stmt|param|params|field|fields|member|members|"
    r"arg|args|elem|item|child|children|tn|pnode|cnode|dnode|enode|snode|"
    r"type_node|sym_node|ident_node|name_node|ast_node|astnode)"
)
AST_NAME_FIELD = (
    r"(?:name|names|ident|identifier|spelling|text|str|string|lexeme|chars|"
    r"cstr|value_str|str_val|raw|src|source_name|type_name|field_name|"
    r"method_name|module_name)"
)
AST_PTR_RE = re.compile(rf"\b{AST_BASE}\s*->\s*{AST_NAME_FIELD}\b")

# A field store: LHS is `something->field` or `something.field` then `=`
# (single `=`, not `==`/`<=`/`>=`/`!=`/`+=` ...).
FIELD_STORE_RE = re.compile(r"(?:->\s*\w+|\.\w+)\s*=(?![=])")

# A function call in an expression: `hash_name32(...)`, `strdup(...)` etc.
# When the borrowed AST pointer only appears *inside* a call, the value is
# consumed/copied, not stored as a raw borrow, so it is not category A.
FUNC_CALL_RE = re.compile(r"\b\w+\s*\(")

# Category C c1: a store into a cgen ctx field.
CTX_STORE_RE = re.compile(r"\b(?:ctx|cg|cgen|self)\s*->\s*\w+\s*=(?![=])")

# Category C c2: a raw `const char *` field declaration.
CONST_CHAR_FIELD_RE = re.compile(r"^\s*const\s+char\s*\*\s*\w+\s*;")
STRUCT_HEADER_RE = re.compile(r"\b(?:struct|typedef struct)\s+(\w+)?")

# Category B: taking the address of a dynamic-array element, capturing base.
#   p = &arr[i];  p = &obj->items[n];  p = arr + i;  p = base->data + k;
ADDR_ELEM_RE = re.compile(
    r"=\s*&\s*([A-Za-z_]\w*(?:\s*->\s*\w+|\s*\.\s*\w+)*)\s*\[",
)
ADDR_PLUS_RE = re.compile(
    r"=\s*([A-Za-z_]\w*(?:\s*->\s*\w+|\s*\.\s*\w+)*)\s*\+\s*\w+\s*;",
)
# Growth of a dynamic array, capturing the base being grown.
GROWTH_CALL_RE = re.compile(
    r"\b(?:\w*(?:push|append|add|grow|reserve|resize|emplace|insert|extend)\w*)"
    r"\s*\(\s*&?\s*([A-Za-z_]\w*(?:\s*->\s*\w+|\s*\.\s*\w+)*)",
    re.IGNORECASE,
)
REALLOC_RE = re.compile(
    r"([A-Za-z_]\w*(?:\s*->\s*\w+|\s*\.\s*\w+)*)\s*=\s*(?:\w*realloc\w*)\s*\(",
)
# A plausible top-level function definition opener at column 0.
FUNC_OPEN_RE = re.compile(r"^[A-Za-z_].*\)\s*\{\s*$")


def _norm_base(base: str) -> str:
    """Collapse whitespace inside a captured `a -> b . c` base expression."""
    return re.sub(r"\s+", "", base)


@dataclass(frozen=True)
class Hit:
    category: str
    path: str
    line: int
    text: str


def rel(root: Path, path: Path) -> str:
    try:
        return str(path.relative_to(root))
    except ValueError:
        return str(path.resolve().relative_to(root))


def _iter_files(root: Path, dirs: tuple[str, ...]):
    for dirname in dirs:
        base = root / dirname
        if not base.exists():
            continue
        for path in sorted(base.rglob("*")):
            rp = rel(root, path)
            if any(part in SKIP_DIR_NAMES for part in Path(rp).parts):
                continue
            if path.is_file() and any(rp.endswith(sfx) for sfx in TEXT_SUFFIXES):
                yield path


def _read_lines(path: Path) -> list[str] | None:
    try:
        return path.read_text(encoding="utf-8").splitlines()
    except (UnicodeDecodeError, OSError):
        return None


def _is_cgen_file(rp: str) -> bool:
    return any(rp.startswith(g) or f"/{g.split('/')[-1]}" in rp for g in C_FILE_GLOBS) or (
        "xi_cgen" in rp
    )


def _exempt(line: str, prev: str) -> bool:
    """A borrow is exempt if the line (or the comment right above) is annotated
    ``owned:`` or already wrapped by an ownership helper."""
    if OWNED_ANNOTATION_RE.search(line) or OWNED_ANNOTATION_RE.search(prev):
        return True
    if OWNERSHIP_WRAPPER_RE.search(line):
        return True
    return False


# ---------------------------------------------------------------------------
# Category A -- AST pointer stored into an IR/aot struct
# ---------------------------------------------------------------------------

def scan_category_a(root: Path) -> list[Hit]:
    hits: list[Hit] = []
    for path in _iter_files(root, A_SCAN_DIRS):
        rp = rel(root, path)
        if rp == SELF_NAME or _is_cgen_file(rp):
            continue
        lines = _read_lines(path)
        if lines is None:
            continue
        for i, line in enumerate(lines):
            if "=" not in line or "->" not in line:
                continue
            store = FIELD_STORE_RE.search(line)
            if not store:
                continue
            # Only the right-hand side matters: `dst->field = <rhs>`.
            rhs = line[store.end():]
            if not AST_PTR_RE.search(rhs):
                continue
            # A bare borrow store, not `hash_name32(node->name)` / `strdup(...)`.
            if FUNC_CALL_RE.search(rhs):
                continue
            prev = lines[i - 1] if i > 0 else ""
            if _exempt(line, prev):
                continue
            hits.append(Hit("AST_PTR_INTO_IR", rp, i + 1, line.strip()))
    return hits


# ---------------------------------------------------------------------------
# Category C -- CGen ctx borrows a const char *
# ---------------------------------------------------------------------------

def scan_category_c(root: Path) -> list[Hit]:
    hits: list[Hit] = []
    for path in _iter_files(root, ("src/aot",)):
        rp = rel(root, path)
        if not _is_cgen_file(rp) or rp == SELF_NAME:
            continue
        lines = _read_lines(path)
        if lines is None:
            continue

        # Track enclosing struct name via a tiny brace-depth stack so that a
        # `const char *` field is only flagged inside a *Ctx*-shaped struct.
        struct_stack: list[tuple[str, int]] = []
        depth = 0
        for i, line in enumerate(lines):
            prev = lines[i - 1] if i > 0 else ""

            # c1: store into a cgen ctx field from an AST-derived pointer.
            if CTX_STORE_RE.search(line) and AST_PTR_RE.search(line):
                if not _exempt(line, prev):
                    hits.append(Hit("CGEN_BORROWED_NAME", rp, i + 1, line.strip()))

            # c2: raw `const char *` field inside any CGen struct. Every
            # struct in the xi_cgen* family is ctx-owned emission state, so a
            # bare `const char *` field is a borrow surface until it is proven
            # arena-owned and annotated `owned:`.
            if "struct" in line and "{" in line:
                m = STRUCT_HEADER_RE.search(line)
                struct_name = m.group(1) if (m and m.group(1)) else "?"
                struct_stack.append((struct_name, depth))
            if (
                CONST_CHAR_FIELD_RE.match(line)
                and struct_stack
                and not _exempt(line, prev)
            ):
                hits.append(Hit("CGEN_BORROWED_NAME", rp, i + 1, line.strip()))

            depth += line.count("{") - line.count("}")
            while struct_stack and depth <= struct_stack[-1][1]:
                struct_stack.pop()
    return hits


# ---------------------------------------------------------------------------
# Category B -- element pointer cached across array growth
# ---------------------------------------------------------------------------

def _scan_function_for_b(rp: str, start_line: int, body: list[str]) -> list[Hit]:
    """Within a single function body, flag every address-of-element whose base
    array is grown later in the same body."""
    taken: dict[str, tuple[int, str]] = {}  # base -> (lineno, text) first seen
    grown: set[str] = set()
    hits: list[Hit] = []
    for off, line in enumerate(body):
        # Growth first: an address taken *before* this growth is at risk.
        for gm in GROWTH_CALL_RE.finditer(line):
            grown.add(_norm_base(gm.group(1)))
        rm = REALLOC_RE.search(line)
        if rm:
            grown.add(_norm_base(rm.group(1)))

        for am in ADDR_ELEM_RE.finditer(line):
            base = _norm_base(am.group(1))
            taken.setdefault(base, (start_line + off, line.strip()))
        pm = ADDR_PLUS_RE.search(line)
        if pm:
            base = _norm_base(pm.group(1))
            taken.setdefault(base, (start_line + off, line.strip()))

    for base, (lineno, text) in taken.items():
        if base in grown and not OWNED_ANNOTATION_RE.search(text):
            hits.append(Hit("PTR_ACROSS_GROWTH", rp, lineno, text))
    return hits


def scan_category_b(root: Path) -> list[Hit]:
    hits: list[Hit] = []
    for path in _iter_files(root, B_SCAN_DIRS):
        rp = rel(root, path)
        if rp == SELF_NAME:
            continue
        lines = _read_lines(path)
        if lines is None:
            continue
        i = 0
        n = len(lines)
        while i < n:
            if FUNC_OPEN_RE.match(lines[i]):
                depth = lines[i].count("{") - lines[i].count("}")
                body_start = i + 1
                j = i + 1
                while j < n and depth > 0:
                    depth += lines[j].count("{") - lines[j].count("}")
                    j += 1
                body = lines[body_start:j]
                hits.extend(_scan_function_for_b(rp, body_start + 1, body))
                i = j
            else:
                i += 1
    return hits


# ---------------------------------------------------------------------------
# Inventory assembly / reporting
# ---------------------------------------------------------------------------

def build_inventory(root: Path) -> dict[str, list[Hit]]:
    by_category: dict[str, list[Hit]] = defaultdict(list)
    for c in CATEGORIES:
        by_category[c] = []
    for hit in scan_category_a(root):
        by_category[hit.category].append(hit)
    for hit in scan_category_b(root):
        by_category[hit.category].append(hit)
    for hit in scan_category_c(root):
        by_category[hit.category].append(hit)
    return {c: by_category[c] for c in CATEGORIES}


def counts_of(inventory: dict[str, list[Hit]]) -> dict[str, int]:
    return {c: len(inventory[c]) for c in CATEGORIES}


def print_text(inventory: dict[str, list[Hit]], max_per_category: int) -> None:
    print("Task 218 meta-ownership cross-lifetime borrow inventory")
    for c in CATEGORIES:
        hits = inventory[c]
        print(f"{c}: {len(hits)}")
        shown = hits if max_per_category <= 0 else hits[:max_per_category]
        for h in shown:
            print(f"  {h.path}:{h.line}: {h.text}")
        if max_per_category > 0 and len(hits) > max_per_category:
            print(f"  ... {len(hits) - max_per_category} more")


def parse_max_category(values: list[str]) -> dict[str, int]:
    out: dict[str, int] = {}
    for raw in values:
        if "=" not in raw:
            raise SystemExit(f"--max-category expects NAME=N, got {raw!r}")
        name, _, num = raw.partition("=")
        name = name.strip()
        if name not in CATEGORIES:
            raise SystemExit(f"unknown category {name!r}; valid: {', '.join(CATEGORIES)}")
        out[name] = int(num)
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--json", action="store_true", help="print full JSON (hits)")
    parser.add_argument(
        "--counts-json", action="store_true", help="print machine-readable counts JSON"
    )
    parser.add_argument(
        "--max-per-category",
        type=int,
        default=15,
        help="text output limit per category; 0 prints all hits",
    )
    parser.add_argument(
        "--baseline",
        default=None,
        help="baseline counts JSON; RECORD mode diffs but never fails",
    )
    parser.add_argument(
        "--write-baseline",
        default=None,
        help="write current counts to this path and exit 0",
    )
    parser.add_argument(
        "--max-category",
        action="append",
        default=[],
        metavar="NAME=N",
        help="fail (exit 1) if category NAME exceeds N (P1 fail-closed mode)",
    )
    args = parser.parse_args()

    root = Path(args.root).resolve()
    inventory = build_inventory(root)
    counts = counts_of(inventory)

    if args.write_baseline:
        Path(args.write_baseline).write_text(
            json.dumps(counts, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        print(f"wrote baseline counts to {args.write_baseline}: {counts}")
        return 0

    if args.json:
        print(
            json.dumps(
                {c: [asdict(h) for h in inventory[c]] for c in CATEGORIES},
                indent=2,
                sort_keys=True,
            )
        )
    elif args.counts_json:
        print(json.dumps(counts, indent=2, sort_keys=True))
    else:
        print_text(inventory, args.max_per_category)

    # RECORD mode baseline diff (informational; never fails the build).
    if args.baseline:
        try:
            base = json.loads(Path(args.baseline).read_text(encoding="utf-8"))
        except OSError:
            base = None
        if base is not None:
            print("\nBaseline diff (record mode; does not fail the build):")
            for c in CATEGORIES:
                b = int(base.get(c, 0))
                cur = counts[c]
                delta = cur - b
                sign = "+" if delta > 0 else ""
                flag = "  <-- NEW BORROWS" if delta > 0 else ""
                print(f"  {c}: baseline={b} current={cur} ({sign}{delta}){flag}")

    # Fail-closed mode (opt-in; used by P1 once categories reach zero).
    limits = parse_max_category(args.max_category)
    if limits:
        failed = False
        for name, limit in limits.items():
            if counts[name] > limit:
                print(
                    f"FAIL: {name}={counts[name]} exceeds max {limit}",
                    file=sys.stderr,
                )
                failed = True
        if failed:
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

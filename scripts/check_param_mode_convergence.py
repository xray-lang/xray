#!/usr/bin/env python3
"""Fail-closed convergence gate for the READ / REF / MOVE parameter model.

The public Xray parameter surface is intentionally small:

    value: T          # READ (default)
    value: ref T      # exclusive write loan
    value: move T     # unique-owner transfer

The historical ``in`` and ``out`` declaration/call spellings and their enum
members are deleted.  This check rejects their return in active source while
allowing only the compact parser-negative corpus listed below.  It also keeps
the declaration AST on one XrParamNode representation.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import asdict, dataclass
from pathlib import Path

SCAN_DIRS = ("src", "stdlib", "tests", "spec", "demos", "tools")
EXTRA_FILES = ("LANGUAGE_SPEC.md", "LANGUAGE_SPEC_CN.md")
TEXT_SUFFIXES = (
    ".c",
    ".h",
    ".inc",
    ".inc.c",
    ".def",
    ".xr",
    ".md",
    ".toml",
    ".json",
    ".py",
    ".sh",
    ".yml",
    ".yaml",
    ".expect",
    ".expected",
)
SKIP_DIR_NAMES = {
    ".git",
    ".mypy_cache",
    "__pycache__",
    "build",
    "build-fuzz",
    "cmake-build-debug",
    "cmake-build-release",
    "node_modules",
}

# One fixture per rejected grammar shape.  These are parser proofs, not a
# compatibility layer and not an item-by-item archive of the old semantics.
REMOVED_SYNTAX_FIXTURES = {
    "tests/compile_errors/syntax/032_param_in_mode_removed.xr",
    "tests/compile_errors/syntax/033_param_out_mode_removed.xr",
    "tests/compile_errors/syntax/034_in_call_marker_removed.xr",
    "tests/compile_errors/syntax/035_out_call_marker_removed.xr",
}

IDENT = r"[A-Za-z_][A-Za-z0-9_]*"
TYPE_HEAD = (
    r"(?:[A-Z][A-Za-z0-9_]*|int(?:8|16|32|64|size)?|uint(?:8|16|32|64|size)?|"
    r"float(?:32|64)?|bool|byte|rune|char|string|void|never|\[|\()"
)
REMOVED_ENUM_RE = re.compile(
    r"\b(?:XR_PARAM_(?:VALUE|IN|OUT)|XR_CALL_ARG_(?:VALUE|OUT))\b"
)
REMOVED_NAMED_FORMAL_RE = re.compile(
    rf"(?:^|[(,])\s*{IDENT}\s*:\s*(?:in|out)\s+{TYPE_HEAD}"
)
REMOVED_PREFIX_FORMAL_RE = re.compile(
    rf"(?:^|[(,])\s*(?:in|out)\s+{IDENT}\s*:"
)
REMOVED_FUNCTION_TYPE_RE = re.compile(
    rf"(?:^|[(,=])\s*(?:in|out)\s+(?:const\s+)?(?:{TYPE_HEAD}|\[[^\]]+\])\s*\)\s*->"
)
REMOVED_CALL_MARKER_RE = re.compile(
    rf"(?:^|[(,])\s*(?:in|out)\s+(?:{IDENT}|this)(?:\b|[.[])"
)
REMOVED_PUBLIC_SURFACE_RE = re.compile(
    rf"(?:{IDENT}\s*:\s*(?:in|out)\s+{TYPE_HEAD}"
    rf"|(?:^|[(,=])\s*(?:in|out)\s+(?:const\s+)?(?:{TYPE_HEAD}|\[[^\]]+\])\s*\)\s*->)"
)
OUT_KEYWORD_ROW_RE = re.compile(r"^\s*\|\s*`out`\s*\|")
DECL_AST_LEGACY_FIELD_RE = re.compile(
    r"\b(?:char\s*\*\*parameters|XrTypeRef\s*\*\*param_types|"
    r"XrParamMode\s*\*param_passing_modes|AstNode\s*\*\*default_values)\b"
)

PUBLIC_PATH_PREFIXES = (
    "LANGUAGE_SPEC",
    "spec/",
    "demos/",
    "stdlib/",
    "src/app/mcp/xmcp_knowledge_generated.c",
)


@dataclass(frozen=True)
class Hit:
    category: str
    path: str
    line: int
    text: str


def rel(root: Path, path: Path) -> str:
    return path.resolve().relative_to(root.resolve()).as_posix()


def iter_text_files(root: Path):
    seen: set[Path] = set()
    for dirname in SCAN_DIRS:
        base = root / dirname
        if not base.exists():
            continue
        for path in sorted(base.rglob("*")):
            relative = Path(rel(root, path))
            if any(part in SKIP_DIR_NAMES for part in relative.parts):
                continue
            if path.is_file() and any(str(path).endswith(suffix) for suffix in TEXT_SUFFIXES):
                seen.add(path)
                yield path
    for name in EXTRA_FILES:
        path = root / name
        if path.exists() and path not in seen:
            yield path


def is_public_path(path: str) -> bool:
    return path.startswith(PUBLIC_PATH_PREFIXES)


def classify_line(path: str, line: str) -> list[str]:
    categories: list[str] = []
    if REMOVED_ENUM_RE.search(line):
        categories.append("REMOVED_ENUM_MEMBER")

    is_xray_source = path.endswith(".xr")
    if is_xray_source and not line.lstrip().startswith("//"):
        removed_syntax = (
            REMOVED_NAMED_FORMAL_RE.search(line)
            or REMOVED_PREFIX_FORMAL_RE.search(line)
            or REMOVED_FUNCTION_TYPE_RE.search(line)
            or REMOVED_CALL_MARKER_RE.search(line)
        )
        if removed_syntax:
            categories.append(
                "ALLOWED_REMOVED_SYNTAX_FIXTURE"
                if path in REMOVED_SYNTAX_FIXTURES
                else "REMOVED_SOURCE_SYNTAX"
            )

    if is_public_path(path):
        if REMOVED_PUBLIC_SURFACE_RE.search(line) or OUT_KEYWORD_ROW_RE.search(line):
            categories.append("REMOVED_PUBLIC_SURFACE")
    return categories


def build_inventory(root: Path) -> dict[str, list[Hit]]:
    inventory = {
        "REMOVED_ENUM_MEMBER": [],
        "REMOVED_SOURCE_SYNTAX": [],
        "REMOVED_PUBLIC_SURFACE": [],
        "ALLOWED_REMOVED_SYNTAX_FIXTURE": [],
    }
    for path in iter_text_files(root):
        relative = rel(root, path)
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except UnicodeDecodeError:
            continue
        for lineno, line in enumerate(lines, 1):
            for category in classify_line(relative, line):
                inventory[category].append(Hit(category, relative, lineno, line.strip()))
    return inventory


def declaration_ast_contract_residue(root: Path) -> list[str]:
    failures: list[str] = []
    ast_header = root / "src/frontend/parser/xast_nodes_decl.h"
    oop_parser = root / "src/frontend/parser/xparse_oop.c"

    header_text = ast_header.read_text(encoding="utf-8")
    if header_text.count("XrParamNode **params;") < 3:
        failures.append(
            "src/frontend/parser/xast_nodes_decl.h: function, method, and interface "
            "declarations must all store XrParamNode **params"
        )
    for lineno, line in enumerate(header_text.splitlines(), 1):
        if DECL_AST_LEGACY_FIELD_RE.search(line):
            failures.append(f"src/frontend/parser/xast_nodes_decl.h:{lineno}: {line.strip()}")

    parser_text = oop_parser.read_text(encoding="utf-8")
    for lineno, line in enumerate(parser_text.splitlines(), 1):
        if re.search(r"\b(?:param_passing_modes|default_values)\b", line):
            failures.append(f"src/frontend/parser/xparse_oop.c:{lineno}: {line.strip()}")
    return failures


def print_inventory(inventory: dict[str, list[Hit]], max_per_category: int) -> None:
    print("READ / REF / MOVE parameter convergence")
    for category, hits in inventory.items():
        print(f"{category}: {len(hits)}")
        shown = hits if max_per_category <= 0 else hits[:max_per_category]
        for hit in shown:
            print(f"  {hit.path}:{hit.line}: {hit.text}")
        if max_per_category > 0 and len(hits) > max_per_category:
            print(f"  ... {len(hits) - max_per_category} more")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--json", action="store_true", help="print machine-readable JSON")
    parser.add_argument("--max-per-category", type=int, default=20)
    args = parser.parse_args()

    root = Path(args.root).resolve()
    inventory = build_inventory(root)
    ast_failures = declaration_ast_contract_residue(root)

    if args.json:
        payload = {category: [asdict(hit) for hit in hits] for category, hits in inventory.items()}
        payload["DECL_AST_CONTRACT_RESIDUE"] = ast_failures
        print(json.dumps(payload, indent=2, sort_keys=True))
    else:
        print_inventory(inventory, args.max_per_category)
        if ast_failures:
            print("DECL_AST_CONTRACT_RESIDUE:", file=sys.stderr)
            for failure in ast_failures:
                print(f"  {failure}", file=sys.stderr)

    blockers = sum(
        len(inventory[name])
        for name in ("REMOVED_ENUM_MEMBER", "REMOVED_SOURCE_SYNTAX", "REMOVED_PUBLIC_SURFACE")
    )
    return 1 if blockers or ast_failures else 0


if __name__ == "__main__":
    sys.exit(main())

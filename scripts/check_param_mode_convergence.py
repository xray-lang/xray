#!/usr/bin/env python3
"""Inventory task-206 parameter-mode convergence residue.

This is a P0 inventory gate, not a final residue blocker. By default it prints
classified hits and exits successfully. Later 206 phases can add stricter
category-specific failure modes as legacy representation and syntax are removed.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
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

IDENT = r"[A-Za-z_][A-Za-z0-9_]*"
PARAM_BOUNDARY = r"(?:^|[(,\[{|])"
MODE_WORDS = r"(?:in|ref|out)"
MODE_STOPWORDS = r"(?:of|or|and|the|then|memory|range|max|min|force|turn|use|case|cases)\b"

DECL_MODE_RE = re.compile(rf"{PARAM_BOUNDARY}\s*{MODE_WORDS}\s+{IDENT}\s*:")
CANON_DECL_MODE_RE = re.compile(
    rf"{PARAM_BOUNDARY}\s*{IDENT}\s*:\s*{MODE_WORDS}\s+(?!{MODE_STOPWORDS}){IDENT}"
)
CALL_MARKER_RE = re.compile(
    rf"[(,]\s*(?:ref|out)\s+(?!{MODE_STOPWORDS}){IDENT}(?:\b|[.[])"
)
FUNCTION_TYPE_PARAM_MODE_RE = re.compile(r"\([^)]*\b(?:in|ref|out)\s+[^)]*\)\s*->")
MOVE_PARAM_RE = re.compile(
    rf"{PARAM_BOUNDARY}\s*(?:move\s+{IDENT}\s*:|{IDENT}\s*:\s*move\b)"
)
STALE_EBNF_RE = re.compile(r"\b(?:Param\s*::=.*Modifier|Modifier\s*::=.*(?:'in'|'ref'|\bin\b|\bref\b))")
XR_PARAM_RE = re.compile(r"\bXR_PARAM_(?:VALUE|IN|REF|OUT)\b")
PASSING_MODE_RE = re.compile(r"\b(?:param_passing_modes|passing_mode)\b")
FUNCTION_TYPE_RE = re.compile(r"\b(?:xr_type_new_function|XR_TYPE_IS_FUNCTION|function\.param_)")
BACKEND_ABI_RE = re.compile(r"\b(?:xi_func_param_passing_mode|param_passing_mode|XrAotAbi)\b")
ESCAPE_SUSPEND_RE = re.compile(
    r"\b(?:borrowed_root|borrow|escape|suspend|await|yield|noescape|lifetime)\b", re.IGNORECASE
)

ACTIVE_SPEC_PREFIXES = ("LANGUAGE_SPEC", "spec/", "demos/", "stdlib/")
REMOVED_SYNTAX_NEGATIVE_FIXTURES = {
    "tests/compile_errors/syntax/028_param_mode_prefix_removed.xr",
    "tests/compile_errors/syntax/029_param_move_mode_removed.xr",
    "tests/compile_errors/syntax/030_param_move_prefix_removed.xr",
}
CATEGORIES = (
    "MOVE_AS_PARAM_MODE_RESIDUE",
    "PREFIX_DECL_MODE_SPELLING",
    "REMOVED_SYNTAX_NEGATIVE_FIXTURE",
    "CANON_DECL_MODE_SPELLING",
    "CALL_SITE_REF_OUT_MARKER",
    "STALE_MODIFIER_EBNF",
    "XR_PARAM_MACRO_RESIDUE",
    "PASSING_MODE_FIELD_OR_ARRAY",
    "FUNCTION_TYPE_MODE_CONSUMER",
    "BACKEND_ABI_MODE_CONSUMER",
    "BORROW_ESCAPE_SUSPEND_CONSUMER",
    "ACTIVE_PUBLIC_SURFACE_PARAM_MODE_HIT",
)


@dataclass(frozen=True)
class Hit:
    category: str
    path: str
    line: int
    text: str


def rel(root: Path, path: Path) -> Path:
    try:
        return path.relative_to(root)
    except ValueError:
        return path.resolve().relative_to(root)


def iter_text_files(root: Path):
    seen: set[Path] = set()
    for dirname in SCAN_DIRS:
        base = root / dirname
        if not base.exists():
            continue
        for path in sorted(base.rglob("*")):
            rel_path = rel(root, path)
            if any(part in SKIP_DIR_NAMES for part in rel_path.parts):
                continue
            if path.is_file() and any(str(path).endswith(suffix) for suffix in TEXT_SUFFIXES):
                seen.add(path)
                yield path
    for name in EXTRA_FILES:
        path = root / name
        if path.exists() and path not in seen:
            yield path


def active_public_path(rel_path: str) -> bool:
    return rel_path.startswith(ACTIVE_SPEC_PREFIXES)


def classify_line(rel_path: str, line: str) -> list[str]:
    categories: list[str] = []
    if not line.strip():
        return categories

    removed_syntax_fixture = rel_path in REMOVED_SYNTAX_NEGATIVE_FIXTURES
    if MOVE_PARAM_RE.search(line):
        categories.append(
            "REMOVED_SYNTAX_NEGATIVE_FIXTURE"
            if removed_syntax_fixture
            else "MOVE_AS_PARAM_MODE_RESIDUE"
        )
    if DECL_MODE_RE.search(line):
        categories.append(
            "REMOVED_SYNTAX_NEGATIVE_FIXTURE"
            if removed_syntax_fixture
            else "PREFIX_DECL_MODE_SPELLING"
        )
    if CANON_DECL_MODE_RE.search(line):
        categories.append("CANON_DECL_MODE_SPELLING")
    if CALL_MARKER_RE.search(line) and not FUNCTION_TYPE_PARAM_MODE_RE.search(line):
        categories.append("CALL_SITE_REF_OUT_MARKER")
    if STALE_EBNF_RE.search(line):
        categories.append("STALE_MODIFIER_EBNF")
    if XR_PARAM_RE.search(line):
        categories.append("XR_PARAM_MACRO_RESIDUE")
    if PASSING_MODE_RE.search(line):
        categories.append("PASSING_MODE_FIELD_OR_ARRAY")
    if FUNCTION_TYPE_RE.search(line):
        categories.append("FUNCTION_TYPE_MODE_CONSUMER")
    if BACKEND_ABI_RE.search(line):
        categories.append("BACKEND_ABI_MODE_CONSUMER")
    if ESCAPE_SUSPEND_RE.search(line) and ("param" in line or "passing_mode" in line):
        categories.append("BORROW_ESCAPE_SUSPEND_CONSUMER")

    if categories and active_public_path(rel_path):
        categories.append("ACTIVE_PUBLIC_SURFACE_PARAM_MODE_HIT")
    return categories


def scan_file(root: Path, path: Path) -> list[Hit]:
    hits: list[Hit] = []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except UnicodeDecodeError:
        return hits
    rel_path = str(rel(root, path))
    for lineno, line in enumerate(lines, 1):
        for category in classify_line(rel_path, line):
            hits.append(Hit(category, rel_path, lineno, line.strip()))
    return hits


def build_inventory(root: Path) -> dict[str, list[Hit]]:
    by_category: dict[str, list[Hit]] = defaultdict(list)
    for category in CATEGORIES:
        by_category[category]
    for path in iter_text_files(root):
        for hit in scan_file(root, path):
            by_category[hit.category].append(hit)
    return {category: by_category[category] for category in CATEGORIES}


def print_text_inventory(inventory: dict[str, list[Hit]], max_per_category: int) -> None:
    print("Task 206 parameter mode convergence inventory")
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
    parser.add_argument(
        "--max-per-category",
        type=int,
        default=20,
        help="text output limit per category; 0 prints all hits",
    )
    args = parser.parse_args()

    root = Path(args.root).resolve()
    inventory = build_inventory(root)

    if args.json:
        print(
            json.dumps(
                {category: [asdict(hit) for hit in hits] for category, hits in inventory.items()},
                indent=2,
                sort_keys=True,
            )
        )
    else:
        print_text_inventory(inventory, args.max_per_category)
    return 0


if __name__ == "__main__":
    sys.exit(main())

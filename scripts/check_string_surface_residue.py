#!/usr/bin/env python3
"""Inventory task-191 string/byte/rune public-surface residue."""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from dataclasses import asdict, dataclass
from pathlib import Path

SCAN_DIRS = ("src", "stdlib", "tests", "spec", "docs", "demos", "tools", "scripts")
EXTRA_FILES = ("LANGUAGE_SPEC.md", "LANGUAGE_SPEC_CN.md")
TEXT_SUFFIXES = (
    ".c",
    ".h",
    ".inc",
    ".inc.c",
    ".def",
    ".xr",
    ".md",
    ".json",
    ".py",
    ".sh",
    ".toml",
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
SKIP_FILES = {
    Path("scripts/check_string_surface_residue.py"),
    Path("scripts/README.md"),
}
ALLOWED_REMOVED_INDEX_FILES = {
    Path("tests/compile_errors/type/string_index_removed.xr"),
    Path("tests/compile_errors/type/string_index_removed.xr.expected"),
    Path("tests/compile_errors/type/string_slice_operator_removed.xr"),
    Path("tests/compile_errors/type/string_slice_operator_removed.xr.expected"),
    Path("tests/compile_errors/type/string_negative_slice_operator_removed.xr"),
    Path("tests/compile_errors/type/string_negative_slice_operator_removed.xr.expected"),
}
PUBLIC_DOC_PREFIXES = ("spec/", "docs/")
PUBLIC_BLOCKING_CATEGORIES = {
    "PUBLIC_SPAN_VIEW_DIAGNOSTIC",
    "PUBLIC_STRING_INDEX_EXAMPLE",
    "PUBLIC_BACKEND_STRING_INDEX_DIAGNOSTIC",
}
BACKEND_BLOCKING_CATEGORIES = {
    "BACKEND_STRING_INDEX_SUPPORT",
}
CATEGORIES = (
    "PUBLIC_SPAN_VIEW_DIAGNOSTIC",
    "PUBLIC_STRING_INDEX_EXAMPLE",
    "PUBLIC_BACKEND_STRING_INDEX_DIAGNOSTIC",
    "BACKEND_STRING_INDEX_SUPPORT",
    "ALLOWED_REMOVED_STRING_INDEX_NEGATIVE_TEST",
    "CANONICAL_STRING_INDEX_REJECTION_TEXT",
)

PUBLIC_SPAN_VIEW_DIAGNOSTIC_RE = re.compile(r"\bSpan view\b")
STALE_INDEX_EXAMPLE_RE = re.compile(
    r"\b(?:str|s|string|text)\s*\[[^\]]+\].*(?:returns?\s+rune|返回\s*rune|//\s*[\"'][^\"']+[\"'])",
    re.IGNORECASE,
)
BACKEND_STRING_INDEX_RE = re.compile(
    r"\bxrt_string_index_get\b|"
    r"String indexing support|Fast path: String \(Unicode character index\)|"
    r"index by int, returns string|var c = str\[0\]"
)
BACKEND_PUBLIC_DIAGNOSTIC_RE = re.compile(
    r"only Array[^\n\"]*String[^\n\"]*index|String support constant indexing"
)
CANONICAL_REJECTION_RE = re.compile(
    r"string does not support integer indexing or slice syntax|"
    r"Integer indexing a `string` is a compile error|"
    r"`string` 整数索引：编译错误|"
    r"Strings do not support integer indexing"
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
            if (
                len(rel_path.parts) >= 2
                and rel_path.parts[0] == "docs"
                and rel_path.parts[1] == "tasks"
            ):
                continue
            if rel_path in SKIP_FILES:
                continue
            if path.is_file() and any(str(path).endswith(suffix) for suffix in TEXT_SUFFIXES):
                seen.add(path)
                yield path
    for name in EXTRA_FILES:
        path = root / name
        if path.exists() and path not in seen:
            yield path


def is_public_doc(rel_path: Path) -> bool:
    rel_str = rel_path.as_posix()
    return rel_str in EXTRA_FILES or any(rel_str.startswith(prefix) for prefix in PUBLIC_DOC_PREFIXES)


def is_public_diagnostic_surface(rel_path: Path) -> bool:
    rel_str = rel_path.as_posix()
    return (
        rel_str.startswith("src/frontend/")
        or rel_str.startswith("tests/compile_errors/")
        or rel_str.startswith("tests/aot/filetests/")
    )


def classify_line(root: Path, path: Path, lineno: int, line: str) -> list[Hit]:
    rel_path = rel(root, path)
    rel_str = rel_path.as_posix()
    stripped = line.strip()
    hits: list[Hit] = []

    if rel_path in ALLOWED_REMOVED_INDEX_FILES:
        if rel_path.suffix == ".expected" or "value[" in line or "str[" in line:
            hits.append(Hit("ALLOWED_REMOVED_STRING_INDEX_NEGATIVE_TEST", rel_str, lineno, stripped))
            return hits

    if STALE_INDEX_EXAMPLE_RE.search(line) and is_public_doc(rel_path):
        hits.append(Hit("PUBLIC_STRING_INDEX_EXAMPLE", rel_str, lineno, stripped))
        return hits

    if PUBLIC_SPAN_VIEW_DIAGNOSTIC_RE.search(line) and is_public_diagnostic_surface(rel_path):
        hits.append(Hit("PUBLIC_SPAN_VIEW_DIAGNOSTIC", rel_str, lineno, stripped))

    if BACKEND_PUBLIC_DIAGNOSTIC_RE.search(line):
        hits.append(Hit("PUBLIC_BACKEND_STRING_INDEX_DIAGNOSTIC", rel_str, lineno, stripped))

    if BACKEND_STRING_INDEX_RE.search(line):
        hits.append(Hit("BACKEND_STRING_INDEX_SUPPORT", rel_str, lineno, stripped))

    if CANONICAL_REJECTION_RE.search(line):
        hits.append(Hit("CANONICAL_STRING_INDEX_REJECTION_TEXT", rel_str, lineno, stripped))

    return hits


def scan_file(root: Path, path: Path) -> list[Hit]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except UnicodeDecodeError:
        return []
    hits: list[Hit] = []
    for lineno, line in enumerate(lines, 1):
        hits.extend(classify_line(root, path, lineno, line))
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
    print("Task 191 string surface residue inventory")
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
    parser.add_argument(
        "--fail-on-public",
        action="store_true",
        help="fail if public string-index residue categories are non-empty",
    )
    parser.add_argument(
        "--fail-on-backend-legacy",
        action="store_true",
        help="fail if backend string-index support residue categories are non-empty",
    )
    args = parser.parse_args()

    root = Path(args.root).resolve()
    inventory = build_inventory(root)

    if args.json:
        print(
            json.dumps(
                {category: [asdict(hit) for hit in hits] for category, hits in inventory.items()},
                ensure_ascii=False,
                indent=2,
            )
        )
    else:
        print_text_inventory(inventory, args.max_per_category)

    failed: list[str] = []
    if args.fail_on_public:
        failed.extend(
            f"{category}={len(inventory.get(category, []))}"
            for category in sorted(PUBLIC_BLOCKING_CATEGORIES)
            if inventory.get(category)
        )
    if args.fail_on_backend_legacy:
        failed.extend(
            f"{category}={len(inventory.get(category, []))}"
            for category in sorted(BACKEND_BLOCKING_CATEGORIES)
            if inventory.get(category)
        )
    if failed:
        print("blocking residue found: " + ", ".join(failed), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

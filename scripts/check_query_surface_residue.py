#!/usr/bin/env python3
"""Inventory task-192 query-surface convergence residue.

The final public surface is `len(value)`, canonical container membership names,
and `typeOf` / `typeName`.  This script separates public alias residue from
internal lowering/runtime names such as `OP_TYPEOF`, `XI_TYPENAME`, and
`xrt_typename`, which are implementation details rather than source aliases.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from dataclasses import asdict, dataclass
from pathlib import Path

SCAN_DIRS = ("src", "stdlib", "tests", "spec", "demos", "tools", "scripts")
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

SKIP_FILES = {
    Path("scripts/check_query_surface_residue.py"),
    Path("scripts/README.md"),
}

ALLOWED_REMOVED_TYPE_QUERY_FILES = {
    Path("tests/compile_errors/type/091_reflect_typeof_removed.xr"),
    Path("tests/compile_errors/type/091_reflect_typeof_removed.xr.expected"),
    Path("tests/compile_errors/type/092_typename_removed.xr"),
    Path("tests/compile_errors/type/092_typename_removed.xr.expected"),
    Path("tests/compile_errors/type/093_typeof_string_compare_removed.xr"),
    Path("tests/compile_errors/type/093_typeof_string_compare_removed.xr.expected"),
}

PUBLIC_TYPE_QUERY_IMPLEMENTATION_FILES = {
    Path("src/api/xrepl.c"),
    Path("src/api/xrepl.h"),
    Path("src/analysis/xglobal_producer.c"),
    Path("src/app/cli/xcmd_repl.c"),
    Path("src/app/lsp/xlsp_analysis.c"),
    Path("src/app/lsp/xlsp_keywords.c"),
    Path("scripts/gen_api_inventory.py"),
}

PUBLIC_CONTAINER_SURFACE_PREFIXES = (
    "stdlib/types/",
    "src/frontend/analyzer/xnative_type_defs.inc.c",
    "src/runtime/object/builtins/",
)

TYPE_QUERY_TEXT_RE = re.compile(r"\b(?:typeof|typename)\b|Reflect\.typeOf")
TYPE_QUERY_CALL_RE = re.compile(r"\b(?:typeof|typename)\s*\(|Reflect\.typeOf")
TYPE_QUERY_SUPPORT_RE = re.compile(
    r'register_builtin_func\s*\([^)]*"(?:typeof|typename)"|'
    r'strcmp\s*\([^)]*"(?:typeof|typename)"|'
    r'print\s*\(\s*(?:typeof|typename)\s*\(|'
    r'"(?:typeof|typename)"'
)
INTERNAL_TYPE_QUERY_RE = re.compile(
    r"\b(?:OP_TYPEOF|OP_TYPENAME|XI_TYPEOF|XI_TYPENAME|xrt_typeof_id|xrt_typename|"
    r"XG_METADATA_TYPENAME|TYPEOF|TYPENAME|xi\.typename|metadata 0 name=typename)\b"
)
CONTAINER_METHOD_ALIAS_RE = re.compile(r"\.(?:isEmpty|includes|include|has)\s*\(")
JSON_STATIC_ALIAS_RE = re.compile(r"\bJson\.(?:size|isEmpty|has)\s*\(")
CONTAINER_DECL_ALIAS_RE = re.compile(
    r"(?:^|\\n)\s*(?:length|size|isEmpty|includes|include|has)\s*(?:[:(]|->)"
)
CONTAINER_BUILDER_ALIAS_RE = re.compile(
    r'xr_class_builder_add_(?:static_)?(?:method|property)\s*\([^)]*'
    r'"(?:length|size|isEmpty|includes|include|has)"'
)
CONTAINER_ALIAS_TEST_TEXT_RE = re.compile(
    r"\btest_[A-Za-z0-9_]*includes[A-Za-z0-9_]*\b|"
    r"\b(?:Array|Range|Set|Slice|string)\.includes\b|"
    r"\bindexOf\s*/\s*includes\b|"
    r"\bincludes\s*(?:[,，]|——|与)"
)
TOOLING_JS_INCLUDES_RE = re.compile(r"\.includes\s*\(")

PUBLIC_BLOCKING_CATEGORIES = {
    "PUBLIC_TYPE_QUERY_ALIAS",
    "PUBLIC_TYPE_QUERY_ALIAS_SUPPORT",
    "PUBLIC_CONTAINER_QUERY_ALIAS",
    "PUBLIC_CONTAINER_QUERY_ALIAS_SUPPORT",
    "PUBLIC_CONTAINER_QUERY_ALIAS_TEST_TEXT",
}

CATEGORIES = (
    "PUBLIC_TYPE_QUERY_ALIAS",
    "PUBLIC_TYPE_QUERY_ALIAS_SUPPORT",
    "ALLOWED_REMOVED_TYPE_QUERY_NEGATIVE_TEST",
    "TYPE_QUERY_REMOVED_SURFACE_ASSERTION",
    "INTERNAL_TYPE_QUERY_BACKEND_NAME",
    "HISTORICAL_TYPE_QUERY_TEXT",
    "PUBLIC_CONTAINER_QUERY_ALIAS",
    "PUBLIC_CONTAINER_QUERY_ALIAS_SUPPORT",
    "PUBLIC_CONTAINER_QUERY_ALIAS_TEST_TEXT",
    "TOOLING_INTERNAL_JS_INCLUDES",
    "DOMAIN_OR_LAYOUT_LENGTH_SIZE_TEXT",
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
            if rel_path in SKIP_FILES:
                continue
            if path.is_file() and any(str(path).endswith(suffix) for suffix in TEXT_SUFFIXES):
                seen.add(path)
                yield path
    for name in EXTRA_FILES:
        path = root / name
        if path.exists() and path not in seen:
            yield path


def strip_xray_line_comment(line: str) -> str:
    return line.split("//", 1)[0]


def is_public_container_surface_file(rel_path: Path) -> bool:
    path_str = rel_path.as_posix()
    return any(path_str.startswith(prefix) for prefix in PUBLIC_CONTAINER_SURFACE_PREFIXES)


def is_public_container_alias_test_file(rel_path: Path) -> bool:
    path_str = rel_path.as_posix()
    return (
        path_str.startswith("tests/regression/")
        or path_str.startswith("tests/aot/basic/")
        or path_str.startswith("tests/aot/filetests/")
    )


def classify_type_query(root: Path, path: Path, lineno: int, line: str) -> list[Hit]:
    rel_path = rel(root, path)
    rel_str = rel_path.as_posix()
    stripped = line.strip()
    hits: list[Hit] = []
    if not TYPE_QUERY_TEXT_RE.search(line):
        return hits

    if rel_path in ALLOWED_REMOVED_TYPE_QUERY_FILES:
        return [Hit("ALLOWED_REMOVED_TYPE_QUERY_NEGATIVE_TEST", rel_str, lineno, stripped)]

    code_part = strip_xray_line_comment(line) if rel_path.suffix == ".xr" else line
    if rel_path.suffix == ".xr" and TYPE_QUERY_CALL_RE.search(code_part):
        return [Hit("PUBLIC_TYPE_QUERY_ALIAS", rel_str, lineno, stripped)]

    if rel_path in PUBLIC_TYPE_QUERY_IMPLEMENTATION_FILES and TYPE_QUERY_SUPPORT_RE.search(line):
        return [Hit("PUBLIC_TYPE_QUERY_ALIAS_SUPPORT", rel_str, lineno, stripped)]

    if rel_str.startswith("tests/unit/lsp/") and (
        "== NULL" in line or "json_array_contains_label" in line
    ):
        return [Hit("TYPE_QUERY_REMOVED_SURFACE_ASSERTION", rel_str, lineno, stripped)]

    if INTERNAL_TYPE_QUERY_RE.search(line) or rel_str.startswith(
        ("src/aot/", "src/ir/", "src/runtime/", "src/shared/", "src/analysis/")
    ):
        return [Hit("INTERNAL_TYPE_QUERY_BACKEND_NAME", rel_str, lineno, stripped)]

    return [Hit("HISTORICAL_TYPE_QUERY_TEXT", rel_str, lineno, stripped)]


def classify_container_query(root: Path, path: Path, lineno: int, line: str) -> list[Hit]:
    rel_path = rel(root, path)
    rel_str = rel_path.as_posix()
    stripped = line.strip()
    hits: list[Hit] = []

    if rel_path == Path("scripts/gen_api_inventory.py") and TOOLING_JS_INCLUDES_RE.search(line):
        hits.append(Hit("TOOLING_INTERNAL_JS_INCLUDES", rel_str, lineno, stripped))
        return hits

    if is_public_container_alias_test_file(rel_path) and CONTAINER_ALIAS_TEST_TEXT_RE.search(line):
        hits.append(Hit("PUBLIC_CONTAINER_QUERY_ALIAS_TEST_TEXT", rel_str, lineno, stripped))
        return hits

    public_surface = is_public_container_surface_file(rel_path)
    if public_surface and (
        CONTAINER_DECL_ALIAS_RE.search(line) or CONTAINER_BUILDER_ALIAS_RE.search(line)
    ):
        hits.append(Hit("PUBLIC_CONTAINER_QUERY_ALIAS_SUPPORT", rel_str, lineno, stripped))
        return hits

    code_part = strip_xray_line_comment(line) if rel_path.suffix == ".xr" else line
    if rel_path.suffix == ".xr" and (
        CONTAINER_METHOD_ALIAS_RE.search(code_part) or JSON_STATIC_ALIAS_RE.search(code_part)
    ):
        hits.append(Hit("PUBLIC_CONTAINER_QUERY_ALIAS", rel_str, lineno, stripped))
        return hits

    if re.search(r"\.(?:length|size)\b|\b(?:length|size|isEmpty|includes|has)\b", line):
        hits.append(Hit("DOMAIN_OR_LAYOUT_LENGTH_SIZE_TEXT", rel_str, lineno, stripped))
    return hits


def scan_file(root: Path, path: Path) -> list[Hit]:
    hits: list[Hit] = []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except UnicodeDecodeError:
        return hits
    for lineno, line in enumerate(lines, 1):
        hits.extend(classify_type_query(root, path, lineno, line))
        hits.extend(classify_container_query(root, path, lineno, line))
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
    print("Task 192 query surface residue inventory")
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
        help="fail if public query alias categories are non-empty",
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

    if args.fail_on_public:
        blocking = {
            category: hits
            for category, hits in inventory.items()
            if category in PUBLIC_BLOCKING_CATEGORIES and hits
        }
        if blocking:
            print("task-192 public query surface residue gate failed:", file=sys.stderr)
            for category, hits in blocking.items():
                print(f"  {category}: {len(hits)}", file=sys.stderr)
            return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())

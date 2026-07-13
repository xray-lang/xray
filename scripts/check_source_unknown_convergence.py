#!/usr/bin/env python3
"""Inventory task-202 source-unknown and erasure-boundary convergence residue.

This is a P0 inventory gate, not a final blocker. By default it prints
classified hits and exits successfully. Later 202 phases can add stricter
category-specific failure modes as source-level `unknown`, runtime unknown
type singletons, and backend erasure fallbacks are replaced by recovery-only
ErrorType/inference facts and explicit typed boundaries.
"""

from __future__ import annotations

import argparse
import json
import re
from collections import defaultdict
from dataclasses import asdict, dataclass
from pathlib import Path

SCAN_DIRS = ("src", "stdlib", "tests", "spec", "demos", "tools", "scripts")
EXTRA_FILES = ("CMakeLists.txt", "LANGUAGE_SPEC.md", "LANGUAGE_SPEC_CN.md")
TEXT_SUFFIXES = (
    ".c",
    ".h",
    ".inc",
    ".inc.c",
    ".def",
    ".xr",
    ".xrd",
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
    "tmp",
}

SOURCE_UNKNOWN_TYPE_RE = re.compile(
    r"(?:['`]?unknown['`]?\s+(?:type|in type position)|"
    r"(?:type position|payload type|type name|public unknown|source unknown).*\bunknown\b|"
    r"\b(?:Array|Task|Channel|Slice|Map|Set)<[^>\n]*\bunknown\b|"
    r"\b(?:Success|Failed)\(unknown\)|"
    r"(?::|->|<|\()\s*unknown\b)"
)
REMOVED_UNKNOWN_DIAGNOSTIC_RE = re.compile(r"'unknown' type has been removed")
UNKNOWN_IDENTIFIER_GUARD_RE = re.compile(
    r"\b(?:var|const|let)\s+unknown\b|scan_single\(\"unknown\"\)"
)
TREF_UNKNOWN_RE = re.compile(r"\b(?:XR_TREF_UNKNOWN|xr_tref_unknown)\b")
RUNTIME_UNKNOWN_RE = re.compile(
    r"\b(?:XR_KIND_UNKNOWN|XR_TYPE_IS_UNKNOWN|xr_type_new_unknown|g_type_unknown|"
    r"TYPE_NAME_UNKNOWN|unknown_type_count)\b"
)
XR_TYPE_IS_UNKNOWN_RE = re.compile(r"\bXR_TYPE_IS_UNKNOWN\s*\(")
ASSIGNABILITY_GENERIC_RE = re.compile(
    r"\b(?:XR_TYPE_IS_UNKNOWN|unknown fallback|unknown element|unknown \|)\b",
    re.IGNORECASE,
)
ERASURE_FALLBACK_RE = re.compile(r"\b(?:type_any|XR_SLOT_ANY|XR_ELEM_ANY)\b")
AOT_UNKNOWN_RE = re.compile(
    r"\b(?:XR_KIND_UNKNOWN|TYPE_NAME_UNKNOWN|type_any|XR_ELEM_ANY|"
    r"unknown\.toString|unknown boundary|unknown direct callee)\b"
)
IR_UNKNOWN_RE = re.compile(r"\b(?:XR_KIND_UNKNOWN|XR_TREF_UNKNOWN|type_any|XR_ELEM_ANY)\b")
FORMATTER_LSP_RE = re.compile(
    r"\b(?:TYPE_NAME_UNKNOWN|XLSP_TYPE_UNKNOWN|unknown\?)\b|<unknown>|"
    r"xfmt_write_str\([^,\n]*,\s*\"unknown\""
)
TASK_ERASURE_RE = re.compile(
    r"\b(?:TaskOutcome|TaskResult|Task<[^>\n]*,\s*[^>\n]*>|Failed\(unknown\))\b"
)
STDLIB_UNKNOWN_API_RE = re.compile(
    r"\b(?:fixed-shape Json/unknown|TaskOutcome|TaskResult|Failed\(unknown\)|"
    r"unknown APIs|Array<TaskOutcome>)\b"
)

SPEC_OR_DOC_PREFIXES = ("LANGUAGE_SPEC", "spec/")
CATEGORIES = (
    "SOURCE_UNKNOWN_TYPE_SURFACE",
    "UNKNOWN_IDENTIFIER_ALLOWED_GUARD",
    "REMOVED_SOURCE_UNKNOWN_DIAGNOSTIC",
    "TREF_UNKNOWN_RECOVERY",
    "RUNTIME_UNKNOWN_TYPE_SINGLETON_OR_FACTORY",
    "XR_TYPE_IS_UNKNOWN_CONSUMER",
    "ASSIGNABILITY_OR_GENERIC_UNKNOWN_COMPAT",
    "TYPE_ANY_OR_DYNAMIC_SLOT_FALLBACK",
    "IR_UNKNOWN_ERASURE_CONSUMER",
    "AOT_UNKNOWN_ERASURE_CONSUMER",
    "FORMATTER_LSP_UNKNOWN_SURFACE",
    "TASK_ERASED_RESULT_RESIDUE",
    "STDLIB_DYNAMIC_UNKNOWN_API",
    "PUBLIC_SPEC_UNKNOWN_RESIDUE",
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


def is_spec_or_doc(rel_path: str) -> bool:
    return rel_path.startswith(SPEC_OR_DOC_PREFIXES)


def is_source_unknown_surface_path(rel_path: str) -> bool:
    return rel_path.endswith((".xr", ".md", ".expect", ".expected", ".xrd")) or is_spec_or_doc(
        rel_path
    )


def is_unknown_identifier_guard_path(rel_path: str) -> bool:
    return rel_path in {
        "tests/regression/02_variables/0210_unknown_identifier.xr",
        "tests/unit/frontend/test_lexer.c",
        "tests/unit/frontend/test_lexer_keywords.c",
    }


def classify_line(rel_path: str, line: str) -> list[str]:
    categories: list[str] = []
    if not line.strip():
        return categories

    if is_source_unknown_surface_path(rel_path) and SOURCE_UNKNOWN_TYPE_RE.search(line):
        categories.append("SOURCE_UNKNOWN_TYPE_SURFACE")
    if is_unknown_identifier_guard_path(rel_path) and UNKNOWN_IDENTIFIER_GUARD_RE.search(line):
        categories.append("UNKNOWN_IDENTIFIER_ALLOWED_GUARD")
    if REMOVED_UNKNOWN_DIAGNOSTIC_RE.search(line):
        categories.append("REMOVED_SOURCE_UNKNOWN_DIAGNOSTIC")
    if TREF_UNKNOWN_RE.search(line):
        categories.append("TREF_UNKNOWN_RECOVERY")
    if RUNTIME_UNKNOWN_RE.search(line):
        categories.append("RUNTIME_UNKNOWN_TYPE_SINGLETON_OR_FACTORY")
    if XR_TYPE_IS_UNKNOWN_RE.search(line):
        categories.append("XR_TYPE_IS_UNKNOWN_CONSUMER")
    if (
        rel_path.startswith("src/runtime/value/xtype")
        or rel_path.startswith("src/frontend/analyzer/")
    ) and ASSIGNABILITY_GENERIC_RE.search(line):
        categories.append("ASSIGNABILITY_OR_GENERIC_UNKNOWN_COMPAT")
    if ERASURE_FALLBACK_RE.search(line):
        categories.append("TYPE_ANY_OR_DYNAMIC_SLOT_FALLBACK")
    if rel_path.startswith("src/ir/") and IR_UNKNOWN_RE.search(line):
        categories.append("IR_UNKNOWN_ERASURE_CONSUMER")
    if rel_path.startswith("src/aot/") and AOT_UNKNOWN_RE.search(line):
        categories.append("AOT_UNKNOWN_ERASURE_CONSUMER")
    if (
        rel_path.startswith("src/app/lsp/")
        or rel_path.startswith("src/frontend/format/")
        or rel_path.startswith("src/runtime/value/xtype_format")
    ) and FORMATTER_LSP_RE.search(line):
        categories.append("FORMATTER_LSP_UNKNOWN_SURFACE")
    if TASK_ERASURE_RE.search(line):
        categories.append("TASK_ERASED_RESULT_RESIDUE")
    if rel_path.startswith("stdlib/") and STDLIB_UNKNOWN_API_RE.search(line):
        categories.append("STDLIB_DYNAMIC_UNKNOWN_API")
    if is_spec_or_doc(rel_path) and (
        SOURCE_UNKNOWN_TYPE_RE.search(line) or TASK_ERASURE_RE.search(line)
    ):
        categories.append("PUBLIC_SPEC_UNKNOWN_RESIDUE")
    return categories


def scan_file(root: Path, path: Path) -> list[Hit]:
    hits: list[Hit] = []
    rel_path = str(rel(root, path))
    if rel_path == "scripts/check_source_unknown_convergence.py":
        return hits
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except UnicodeDecodeError:
        return hits
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
    print("Task 202 source-unknown convergence inventory")
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
    raise SystemExit(main())

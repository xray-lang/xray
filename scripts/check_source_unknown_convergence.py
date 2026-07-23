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
import os
import re
import subprocess
import sys
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
    r"\b(?:var|const|let)\s+unknown\b|scan_single\(\"unknown\"\)|"
    r"\bassert_eq\s*\(\s*unknown\b"
)
ERROR_TYPE_RECOVERY_RE = re.compile(
    r"\b(?:XR_TREF_ERROR|xr_tref_error|XR_KIND_ERROR|xr_type_new_error)\b"
)
RUNTIME_UNKNOWN_RE = re.compile(
    r"\b(?:XR_KIND_UNKNOWN|XR_TYPE_IS_UNKNOWN|xr_type_new_unknown|g_type_unknown|"
    r"TYPE_NAME_UNKNOWN)\b"
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
IR_UNKNOWN_RE = re.compile(r"\b(?:XR_KIND_UNKNOWN|type_any|XR_ELEM_ANY)\b")
FORMATTER_LSP_RE = re.compile(
    r"\b(?:TYPE_NAME_UNKNOWN|XLSP_TYPE_UNKNOWN|unknown\?)\b|<unknown>|"
    r"xfmt_write_str\([^,\n]*,\s*\"unknown\""
)
TASK_ERASURE_RE = re.compile(
    r"\b(?:TaskOutcome|Failed\(unknown\)|TaskResult[^\n]*\bunknown\b)"
)
STDLIB_UNKNOWN_API_RE = re.compile(
    r"(?:\bfixed-shape Json/unknown\b|\bFailed\(unknown\)|\bunknown APIs\b|"
    r"\b(?:Success|Failed)\(Json\))"
)

SPEC_OR_DOC_PREFIXES = ("LANGUAGE_SPEC", "spec/")
CATEGORIES = (
    "SOURCE_UNKNOWN_TYPE_SURFACE",
    "UNKNOWN_IDENTIFIER_ALLOWED_GUARD",
    "REMOVED_SOURCE_UNKNOWN_DIAGNOSTIC",
    "ALLOWED_REMOVED_SOURCE_UNKNOWN_NEGATIVE_TEST",
    "ALLOWED_RUNTIME_UNKNOWN_OUTPUT_FIXTURE",
    "ERROR_TYPE_RECOVERY",
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

INVENTORY_SCRIPT_SELF_NOISE = {
    "scripts/check_source_unknown_convergence.py",
    "scripts/check_error_effect_convergence.py",
    "scripts/README.md",
}

ALLOWED_REMOVED_SOURCE_UNKNOWN_NEGATIVE_TESTS = {
    "tests/compile_errors/type/source_unknown_cast_removed.xr",
    "tests/compile_errors/type/source_unknown_member_removed.xr",
    "tests/compile_errors/type/source_unknown_param_removed.xr",
    "tests/compile_errors/type/span_as_unknown_escape.xr",
    "tests/compile_errors/type/span_unknown_return_escape.xr",
}

ALLOWED_RUNTIME_UNKNOWN_OUTPUT_FIXTURES = {
    "tests/regression/11_coroutine/1123_channel_timeout.xr.expected",
}

ALLOWED_NON_TYPE_UNKNOWN_DIAGNOSTICS = {
    "tests/compile_errors/ffi/016_extern_attribute_removed.xr.expected",
    "tests/compile_errors/ffi/017_dylib_attribute_removed.xr.expected",
}


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


def is_scanned_text_path(path: Path) -> bool:
    if any(part in SKIP_DIR_NAMES for part in path.parts):
        return False
    root = path.parts[0] if path.parts else ""
    if root not in SCAN_DIRS and path.name not in EXTRA_FILES:
        return False
    return any(str(path).endswith(suffix) for suffix in TEXT_SUFFIXES)


def iter_git_text_files(root: Path):
    try:
        proc = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z", "--cached"],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
    except (OSError, subprocess.CalledProcessError):
        return None

    files: list[Path] = []
    for raw in proc.stdout.split(b"\0"):
        if not raw:
            continue
        rel_path = Path(raw.decode("utf-8", errors="surrogateescape"))
        if is_scanned_text_path(rel_path):
            files.append(root / rel_path)
    return sorted(files)


def iter_text_files(root: Path):
    git_files = iter_git_text_files(root)
    if git_files is not None:
        yield from git_files
        return

    seen: set[Path] = set()
    for dirname in SCAN_DIRS:
        base = root / dirname
        if not base.exists():
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIR_NAMES)
            for filename in sorted(filenames):
                path = Path(dirpath) / filename
                if any(str(path).endswith(suffix) for suffix in TEXT_SUFFIXES):
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

    has_unknown = "unknown" in line or "UNKNOWN" in line
    has_error = "error" in line or "ERROR" in line
    has_erasure_slot = "type_any" in line or "XR_SLOT_ANY" in line or "XR_ELEM_ANY" in line
    has_task_result = "Task" in line or "Failed(unknown)" in line
    has_stdlib_unknown_api = (
        "unknown APIs" in line
        or "Json/unknown" in line
        or "Failed(unknown)" in line
        or "Success(Json)" in line
        or "Failed(Json)" in line
    )

    if has_unknown and rel_path in ALLOWED_REMOVED_SOURCE_UNKNOWN_NEGATIVE_TESTS:
        categories.append("ALLOWED_REMOVED_SOURCE_UNKNOWN_NEGATIVE_TEST")
        return categories
    if (
        has_unknown
        and rel_path in ALLOWED_RUNTIME_UNKNOWN_OUTPUT_FIXTURES
        and "<unknown>" in line
    ):
        categories.append("ALLOWED_RUNTIME_UNKNOWN_OUTPUT_FIXTURE")
        return categories
    if (
        has_unknown
        and rel_path in ALLOWED_NON_TYPE_UNKNOWN_DIAGNOSTICS
        and "unknown attribute name" in line
    ):
        return categories
    if has_unknown and REMOVED_UNKNOWN_DIAGNOSTIC_RE.search(line):
        categories.append("REMOVED_SOURCE_UNKNOWN_DIAGNOSTIC")
    if (
        has_unknown
        and is_unknown_identifier_guard_path(rel_path)
        and UNKNOWN_IDENTIFIER_GUARD_RE.search(line)
    ):
        categories.append("UNKNOWN_IDENTIFIER_ALLOWED_GUARD")
        return categories
    if (
        has_unknown
        and is_source_unknown_surface_path(rel_path)
        and SOURCE_UNKNOWN_TYPE_RE.search(line)
        and "REMOVED_SOURCE_UNKNOWN_DIAGNOSTIC" not in categories
    ):
        categories.append("SOURCE_UNKNOWN_TYPE_SURFACE")
    if has_error and ERROR_TYPE_RECOVERY_RE.search(line):
        categories.append("ERROR_TYPE_RECOVERY")
    if has_unknown and RUNTIME_UNKNOWN_RE.search(line):
        categories.append("RUNTIME_UNKNOWN_TYPE_SINGLETON_OR_FACTORY")
    if "XR_TYPE_IS_UNKNOWN" in line and XR_TYPE_IS_UNKNOWN_RE.search(line):
        categories.append("XR_TYPE_IS_UNKNOWN_CONSUMER")
    if (
        has_unknown
        and (
            rel_path.startswith("src/runtime/value/xtype")
            or rel_path.startswith("src/frontend/analyzer/")
        )
        and ASSIGNABILITY_GENERIC_RE.search(line)
    ):
        categories.append("ASSIGNABILITY_OR_GENERIC_UNKNOWN_COMPAT")
    if has_erasure_slot and ERASURE_FALLBACK_RE.search(line):
        categories.append("TYPE_ANY_OR_DYNAMIC_SLOT_FALLBACK")
    if (
        rel_path.startswith("src/ir/")
        and (has_unknown or has_erasure_slot)
        and IR_UNKNOWN_RE.search(line)
    ):
        categories.append("IR_UNKNOWN_ERASURE_CONSUMER")
    if (
        rel_path.startswith("src/aot/")
        and (has_unknown or has_erasure_slot)
        and AOT_UNKNOWN_RE.search(line)
    ):
        categories.append("AOT_UNKNOWN_ERASURE_CONSUMER")
    if (
        has_unknown
        and (
            rel_path.startswith("src/app/lsp/")
            or rel_path.startswith("src/frontend/format/")
            or rel_path.startswith("src/runtime/value/xtype_format")
        )
        and FORMATTER_LSP_RE.search(line)
    ):
        categories.append("FORMATTER_LSP_UNKNOWN_SURFACE")
    if has_task_result and TASK_ERASURE_RE.search(line):
        categories.append("TASK_ERASED_RESULT_RESIDUE")
    if (
        rel_path.startswith("stdlib/")
        and has_stdlib_unknown_api
        and STDLIB_UNKNOWN_API_RE.search(line)
    ):
        categories.append("STDLIB_DYNAMIC_UNKNOWN_API")
    if is_spec_or_doc(rel_path) and (
        (has_unknown and SOURCE_UNKNOWN_TYPE_RE.search(line))
        or (has_task_result and TASK_ERASURE_RE.search(line))
    ):
        categories.append("PUBLIC_SPEC_UNKNOWN_RESIDUE")
    return categories


def scan_file(root: Path, path: Path) -> list[Hit]:
    hits: list[Hit] = []
    rel_path = str(rel(root, path))
    if rel_path in INVENTORY_SCRIPT_SELF_NOISE:
        return hits
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (UnicodeDecodeError, FileNotFoundError):
        # A tracked path may be deliberately deleted in the current worktree
        # during a clean-slate surface cutover. `git ls-files` still reports it
        # until the deletion is committed; inventories must scan the worktree
        # that will be committed, not fail on the stale index entry.
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


def parse_category_max(raw: str) -> tuple[str, int]:
    if "=" not in raw:
        raise argparse.ArgumentTypeError("expected CATEGORY=COUNT")
    category, limit_text = raw.split("=", 1)
    category = category.strip()
    if category not in CATEGORIES:
        raise argparse.ArgumentTypeError(f"unknown category '{category}'")
    try:
        limit = int(limit_text)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid max count '{limit_text}'") from exc
    if limit < 0:
        raise argparse.ArgumentTypeError("max count must be non-negative")
    return category, limit


def enforce_category_maxima(
    inventory: dict[str, list[Hit]], maxima: list[tuple[str, int]]
) -> list[str]:
    errors: list[str] = []
    for category, limit in maxima:
        count = len(inventory.get(category, []))
        if count > limit:
            errors.append(f"{category}: {count} exceeds max {limit}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--json", action="store_true", help="print machine-readable JSON")
    parser.add_argument(
        "--max-category",
        action="append",
        default=[],
        metavar="CATEGORY=COUNT",
        type=parse_category_max,
        help="fail if a category count exceeds COUNT; may be repeated",
    )
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

    errors = enforce_category_maxima(inventory, args.max_category)
    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

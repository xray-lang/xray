#!/usr/bin/env python3
"""Inventory task-205 unchecked error-effect convergence residue.

This is a P0 inventory gate, not a final blocker. By default it prints
classified hits and exits successfully. Later 205 phases can add stricter
category-specific failure modes as XrErrorSet, function-type-owned error sets,
and MAY_THROW naming are replaced by XaEffectDatabase.
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
}

XR_ERROR_SET_RE = re.compile(r"\bXrErrorSet\b|\bxr_error_set_[A-Za-z0-9_]+\b")
FUNCTION_ERROR_SET_RE = re.compile(
    r"\bfunction\.error_set\b|\bxr_type_(?:set|get|has)_error_set\b|"
    r"\bXrType\.function\.error_set\b"
)
SYMBOL_LINK_ERROR_SET_RE = re.compile(r"\bXaSymbolLinks\.error_set\b|\blinks\.error_set\b")
XI_MAY_THROW_RE = re.compile(r"\b(?:XI_FLAG_MAY_THROW|XI_EFFECT_MAY_THROW)\b")
SUMMARY_MAY_THROW_RE = re.compile(r"\b(?:XG_BODY_MAY_THROW|XR_EFFECT_MAY_THROW)\b")
MAY_THROW_RE = re.compile(r"\b(?:XI_FLAG_MAY_THROW|XI_EFFECT_MAY_THROW|XG_BODY_MAY_THROW|XR_EFFECT_MAY_THROW|MAY_THROW)\b")
ERROR_CHANNEL_RE = re.compile(
    r"\b(?:pending_error|XI_ERR_CHECK|XI_ERR_CATCH|XI_THROW|XI_CATCH|throw|catch)\b"
)
LSP_TOOLING_RE = re.compile(
    r"\b(?:hover|inlay|CodeLens|codeLens|codeAction|code_action|"
    r"xlsp_analyze_hover|xlsp_analyze_inlay_hints|xlsp_handle_td_code_lens|"
    r"xlsp_handle_code_action|hoverProvider|inlayHintProvider|codeLensProvider|"
    r"codeActionProvider)\b"
)
XRD_RE = re.compile(r"\b(?:xrd|XRD|\.xrd|--xrd|generate_xrd|xanalyzer_xrd)\b")
NATIVE_ERROR_RE = re.compile(
    r"@errors\b|\bbodyless\b.*\berror\b|\berror\b.*\bbodyless\b|"
    r"\bnative\b.*\b(?:effect|error_set|@errors)\b|"
    r"\b(?:effect|error_set|@errors)\b.*\bnative\b",
    re.IGNORECASE,
)
TASK_ERROR_RE = re.compile(r"\b(?:TaskOutcome|TaskResult|Failed\(unknown\)|Task<[^>]*,\s*[^>]*>)\b")

XR_ERROR_SET_NEEDLES = ("XrErrorSet", "xr_error_set_")
FUNCTION_ERROR_SET_NEEDLES = ("function.error_set", "xr_type_", "XrType.function.error_set")
SYMBOL_LINK_ERROR_SET_NEEDLES = ("XaSymbolLinks.error_set", "links.error_set")
MAY_THROW_NEEDLE = "MAY_THROW"
ERROR_CHANNEL_NEEDLES = ("pending_error", "XI_ERR_CHECK", "XI_ERR_CATCH", "XI_THROW", "XI_CATCH", "throw", "catch")
LSP_TOOLING_NEEDLES = (
    "hover",
    "inlay",
    "CodeLens",
    "codeLens",
    "codeAction",
    "code_action",
    "xlsp_analyze_hover",
    "xlsp_analyze_inlay_hints",
    "xlsp_handle_td_code_lens",
    "xlsp_handle_code_action",
    "hoverProvider",
    "inlayHintProvider",
    "codeLensProvider",
    "codeActionProvider",
)
XRD_NEEDLES = ("xrd", "XRD", ".xrd", "--xrd", "generate_xrd", "xanalyzer_xrd")
TASK_ERROR_NEEDLES = ("TaskOutcome", "TaskResult", "Failed(unknown)", "Task<")

CATEGORIES = (
    "XR_ERROR_SET_RUNTIME_OR_API",
    "FUNCTION_TYPE_ERROR_SET_FIELD",
    "SYMBOL_LINK_ERROR_SET_FIELD",
    "XI_MAY_THROW_FLAG_OR_EFFECT",
    "GLOBAL_SUMMARY_MAY_THROW_EFFECT",
    "AOT_MAY_THROW_CONSUMER",
    "IR_ERROR_CHANNEL_CONSUMER",
    "VM_RUNTIME_PENDING_ERROR_CHANNEL",
    "LSP_ERROR_TOOLING_ENTRY",
    "XRD_METADATA_GENERATOR_OR_LOADER",
    "NATIVE_ERROR_CONTRACT_SURFACE",
    "TASK_TYPED_ERROR_RESIDUE",
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


def classify_line(rel_path: str, line: str) -> list[str]:
    categories: list[str] = []
    if not line.strip():
        return categories

    if any(needle in line for needle in XR_ERROR_SET_NEEDLES) and XR_ERROR_SET_RE.search(line):
        categories.append("XR_ERROR_SET_RUNTIME_OR_API")
    if any(needle in line for needle in FUNCTION_ERROR_SET_NEEDLES) and FUNCTION_ERROR_SET_RE.search(line):
        categories.append("FUNCTION_TYPE_ERROR_SET_FIELD")
    if any(needle in line for needle in SYMBOL_LINK_ERROR_SET_NEEDLES) and SYMBOL_LINK_ERROR_SET_RE.search(line):
        categories.append("SYMBOL_LINK_ERROR_SET_FIELD")
    if MAY_THROW_NEEDLE in line and XI_MAY_THROW_RE.search(line):
        categories.append("XI_MAY_THROW_FLAG_OR_EFFECT")
    if MAY_THROW_NEEDLE in line and SUMMARY_MAY_THROW_RE.search(line):
        categories.append("GLOBAL_SUMMARY_MAY_THROW_EFFECT")
    if rel_path.startswith("src/aot/") and MAY_THROW_NEEDLE in line and MAY_THROW_RE.search(line):
        categories.append("AOT_MAY_THROW_CONSUMER")
    if rel_path.startswith("src/ir/") and any(needle in line for needle in ERROR_CHANNEL_NEEDLES) and ERROR_CHANNEL_RE.search(line):
        categories.append("IR_ERROR_CHANNEL_CONSUMER")
    if rel_path.startswith("src/vm/") and ("pending_error" in line or "throw" in line or "catch" in line):
        categories.append("VM_RUNTIME_PENDING_ERROR_CHANNEL")
    if (
        (rel_path.startswith("src/app/lsp/") or rel_path.startswith("tests/unit/lsp/"))
        and any(needle in line for needle in LSP_TOOLING_NEEDLES)
        and LSP_TOOLING_RE.search(line)
    ):
        categories.append("LSP_ERROR_TOOLING_ENTRY")
    if (
        rel_path.endswith(".xrd")
        or "xanalyzer_xrd" in rel_path
        or (any(needle in line for needle in XRD_NEEDLES) and XRD_RE.search(line))
    ):
        categories.append("XRD_METADATA_GENERATOR_OR_LOADER")
    lower_line = line.lower()
    if (
        "@errors" in line
        or ("bodyless" in lower_line and "error" in lower_line)
        or ("native" in lower_line and ("effect" in lower_line or "error_set" in lower_line))
    ) and NATIVE_ERROR_RE.search(line):
        categories.append("NATIVE_ERROR_CONTRACT_SURFACE")
    if any(needle in line for needle in TASK_ERROR_NEEDLES) and TASK_ERROR_RE.search(line):
        categories.append("TASK_TYPED_ERROR_RESIDUE")
    return categories


def scan_file(root: Path, path: Path) -> list[Hit]:
    hits: list[Hit] = []
    rel_path = str(rel(root, path))
    if rel_path == "scripts/check_error_effect_convergence.py":
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
    print("Task 205 unchecked error-effect convergence inventory")
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

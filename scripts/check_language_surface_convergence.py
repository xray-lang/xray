#!/usr/bin/env python3
"""Fail-closed gate for the var/const/comptime/move/copy language surface.

The retired ``shared``/``owned`` declaration grammar and its hidden
``to_shared`` copy/promotion machinery must not return.  English uses of the
words "shared" and "owned" are intentionally allowed; this check only matches
compiler identities, Xray declaration shapes, and public syntax descriptions.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import asdict, dataclass
from pathlib import Path

SCAN_DIRS = ("src", "stdlib", "tests", "spec", "docs", "demos", "scripts", "tools")
EXTRA_FILES = ("README.md", "README_CN.md", "LANGUAGE_SPEC.md", "LANGUAGE_SPEC_CN.md")
TEXT_SUFFIXES = (
    ".c", ".h", ".inc", ".inc.c", ".def", ".xr", ".md", ".json", ".py", ".sh",
    ".toml", ".yml", ".yaml", ".expect", ".expected",
)
SKIP_DIR_NAMES = {
    ".git", "__pycache__", ".mypy_cache", "build", "build-asan", "build-fuzz",
    "cmake-build-debug", "cmake-build-release", "node_modules",
}
PUBLIC_PREFIXES = (
    "README", "LANGUAGE_SPEC", "spec/", "docs/", "demos/", "src/app/mcp/",
    "src/app/lsp/",
)

RETIRED_COMPILER_IDENTITY_RE = re.compile(
    r"\b(?:TK_SHARED|TK_OWNED|AST_SHARED_DECL|AST_OWNED_DECL|"
    r"XR_STORAGE_(?:SHARED|OWNED)|XI_TO_SHARED_KIND_[A-Z0-9_]+|OP_TO_SHARED|"
    r"XR_NATIVE_BODY_COPY_SHARED|XrObjToSharedFn|xr_to_shared(?:_[A-Za-z0-9_]+)?|"
    r"copy_shared|copy_owned|to_shared|is_shared_provenance)\b"
)
RETIRED_SYMBOL_FIELD_RE = re.compile(r"(?:->|\.)is_(?:shared|owned)\b")
RETIRED_DECL_RE = re.compile(
    r"^\s*(?:export\s+)?(?:shared|owned)\s+[A-Za-z_][A-Za-z0-9_]*\s*(?::|=)"
)
RETIRED_ESCAPED_DECL_RE = re.compile(
    r"\\n\s*(?:export\s+)?(?:shared|owned)\s+[A-Za-z_][A-Za-z0-9_]*\s*(?::|=)"
)
RETIRED_PUBLIC_GRAMMAR_RE = re.compile(
    r"\b(?:SharedDecl|OwnedDecl)\b|^\s*\|\s*`(?:shared|owned)`\s*\||"
    r"`(?:shared|owned)\s+[A-Za-z_][A-Za-z0-9_]*\s*(?::|=)"
)
RETIRED_PUBLIC_CLAIM_RE = re.compile(
    r"(?:must|MUST|必须)[^\n]{0,48}(?:declared|created|声明|创建)[^\n]{0,32}`shared`|"
    r"(?:declared|created|声明|创建)[^\n]{0,32}(?:with|为|成)\s*`shared`"
)
RETIRED_FREEZE_SURFACE_RE = re.compile(r"\b(?:freeze|Frozen)\s*(?:\(|<)")


@dataclass(frozen=True)
class Hit:
    category: str
    path: str
    line: int
    text: str


def relative(root: Path, path: Path) -> str:
    return str(path.resolve().relative_to(root.resolve()))


def iter_text_files(root: Path):
    seen: set[Path] = set()
    for dirname in SCAN_DIRS:
        base = root / dirname
        if not base.exists():
            continue
        for path in sorted(base.rglob("*")):
            rel = Path(relative(root, path))
            if any(part in SKIP_DIR_NAMES for part in rel.parts):
                continue
            if path.is_file() and any(str(path).endswith(suffix) for suffix in TEXT_SUFFIXES):
                seen.add(path)
                yield path
    for name in EXTRA_FILES:
        path = root / name
        if path.exists() and path not in seen:
            yield path


def is_public(path: str) -> bool:
    return path.startswith(PUBLIC_PREFIXES)


def classify(path: str, line: str) -> list[str]:
    categories: list[str] = []
    if RETIRED_COMPILER_IDENTITY_RE.search(line) or RETIRED_SYMBOL_FIELD_RE.search(line):
        categories.append("RETIRED_COMPILER_IDENTITY")
    if path.endswith(".xr") and not line.lstrip().startswith("//") and RETIRED_DECL_RE.search(line):
        categories.append("RETIRED_SOURCE_DECLARATION")
    if is_public(path):
        if (
            RETIRED_DECL_RE.search(line)
            or RETIRED_ESCAPED_DECL_RE.search(line)
            or RETIRED_PUBLIC_GRAMMAR_RE.search(line)
            or RETIRED_PUBLIC_CLAIM_RE.search(line)
        ):
            categories.append("RETIRED_PUBLIC_SURFACE")
        if RETIRED_FREEZE_SURFACE_RE.search(line):
            categories.append("RETIRED_FREEZE_SURFACE")
    return categories


def build_inventory(root: Path) -> dict[str, list[Hit]]:
    inventory: dict[str, list[Hit]] = {
        "RETIRED_COMPILER_IDENTITY": [],
        "RETIRED_SOURCE_DECLARATION": [],
        "RETIRED_PUBLIC_SURFACE": [],
        "RETIRED_FREEZE_SURFACE": [],
    }
    for path in iter_text_files(root):
        rel = relative(root, path)
        if rel == "scripts/check_language_surface_convergence.py":
            continue
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except UnicodeDecodeError:
            continue
        for lineno, line in enumerate(lines, 1):
            for category in classify(rel, line):
                inventory[category].append(Hit(category, rel, lineno, line.strip()))
    return inventory


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--json", action="store_true", help="print machine-readable JSON")
    parser.add_argument("--max-per-category", type=int, default=20)
    args = parser.parse_args()

    inventory = build_inventory(Path(args.root).resolve())
    if args.json:
        print(json.dumps({k: [asdict(hit) for hit in v] for k, v in inventory.items()},
                         indent=2, sort_keys=True))
    else:
        print("var/const language surface convergence")
        for category, hits in inventory.items():
            print(f"{category}: {len(hits)}")
            shown = hits if args.max_per_category <= 0 else hits[: args.max_per_category]
            for hit in shown:
                print(f"  {hit.path}:{hit.line}: {hit.text}")
            if args.max_per_category > 0 and len(hits) > args.max_per_category:
                print(f"  ... {len(hits) - args.max_per_category} more")
    return 1 if any(inventory.values()) else 0


if __name__ == "__main__":
    sys.exit(main())

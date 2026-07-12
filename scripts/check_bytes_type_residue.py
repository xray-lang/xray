#!/usr/bin/env python3
"""Inventory task-204 Bytes/ByteSpan/ByteView removal residue.

By default this is an inventory tool and exits successfully. Use
`--fail-on-public` once the public-surface categories are expected to be zero.
Internal `XI_BYTES_*`/`OP_BYTES_*`-style names are reported separately because
task 204 allows them to be retired in later backend-focused slices.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from dataclasses import asdict, dataclass
from pathlib import Path

REMOVED_TYPES = ("Bytes", "ByteSpan", "ByteView")
REMOVED_TYPE_RE = re.compile(r"\b(?:Bytes|ByteSpan|ByteView)\b")
SIGNATURE_RE = re.compile(
    r"(?::|->|<|\||\()\s*(?:Bytes|ByteSpan|ByteView)\b|"
    r",\s*(?:Bytes|ByteSpan|ByteView)\b(?!\s*:)"
)
DIAGNOSTIC_RE = re.compile(
    r"(undefined type|member|diagnostic|error|warning).*\b(?:Bytes|ByteSpan|ByteView)\b",
    re.IGNORECASE,
)
METHOD_RECEIVER_RE = re.compile(r"\b(?:Bytes|ByteSpan|ByteView)\s*(?:\.|\()")
PRELUDE_RE = re.compile(r"\bXR_PRELUDE_TYPE\(\"(?:Bytes|ByteSpan|ByteView)\"")
CONSTRUCTOR_RE = re.compile(r"\b(?:OP_BYTES_NEW|XI_BYTES_NEW)\b")
INTERNAL_LEGACY_RE = re.compile(
    r"\b(?:XI_BYTES_[A-Z0-9_]+|OP_BYTES_[A-Z0-9_]+|xr_array_bytes_[A-Za-z0-9_]+|"
    r"xrt_array_bytes|bytes_typed_[A-Za-z0-9_]+|lower_bytes_[A-Za-z0-9_]+|"
    r"emit_bytes_[A-Za-z0-9_]+|cg_bytes_[A-Za-z0-9_]+)\b"
)

SCAN_DIRS = ("src", "stdlib", "tests", "spec", "demos", "tools")
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
    "cmake-build-debug",
    "cmake-build-release",
    "node_modules",
}
ALLOWED_REMOVED_TYPE_FILES = {
    Path("tests/compile_errors/type/bytes_public_types_removed.xr"),
    Path("tests/compile_errors/type/bytes_public_types_removed.xr.expected"),
}
PUBLIC_BLOCKING_CATEGORIES = {
    "PUBLIC_TYPE_BYTES",
    "PUBLIC_TYPE_BYTESPAN",
    "PUBLIC_TYPE_BYTEVIEW",
    "PUBLIC_SIGNATURE_BYTES",
    "PUBLIC_DIAGNOSTIC_BYTES",
    "PRELUDE_OR_RESOLVER_ALIAS",
    "CONSTRUCTOR_OPCODE_BYTES",
    "METHOD_RECEIVER_BYTES",
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


def iter_text_files(root: Path):
    for dirname in SCAN_DIRS:
        base = root / dirname
        if not base.exists():
            continue
        for path in sorted(base.rglob("*")):
            if any(part in SKIP_DIR_NAMES for part in path.parts):
                continue
            if path.is_file() and any(str(path).endswith(suffix) for suffix in TEXT_SUFFIXES):
                yield path


def removed_type_category(line: str) -> str:
    if "ByteSpan" in line:
        return "PUBLIC_TYPE_BYTESPAN"
    if "ByteView" in line:
        return "PUBLIC_TYPE_BYTEVIEW"
    return "PUBLIC_TYPE_BYTES"


def classify_removed_type(root: Path, path: Path, line: str) -> str | None:
    rel_path = rel(root, path)
    if rel_path in ALLOWED_REMOVED_TYPE_FILES:
        return "ALLOWED_REMOVED_TYPE_NEGATIVE_TEST"
    if PRELUDE_RE.search(line) or (
        rel_path == Path("src/frontend/analyzer/xtype_ref_resolve.c")
        and REMOVED_TYPE_RE.search(line)
    ):
        return "PRELUDE_OR_RESOLVER_ALIAS"
    if CONSTRUCTOR_RE.search(line):
        return "CONSTRUCTOR_OPCODE_BYTES"
    if METHOD_RECEIVER_RE.search(line):
        return "METHOD_RECEIVER_BYTES"
    if SIGNATURE_RE.search(line):
        return "PUBLIC_SIGNATURE_BYTES"
    if DIAGNOSTIC_RE.search(line):
        return "PUBLIC_DIAGNOSTIC_BYTES"
    return removed_type_category(line)


def scan_file(root: Path, path: Path) -> list[Hit]:
    hits: list[Hit] = []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except UnicodeDecodeError:
        return hits
    rel_path = str(rel(root, path))
    for lineno, line in enumerate(lines, 1):
        internal_match = INTERNAL_LEGACY_RE.search(line)
        if internal_match:
            hits.append(
                Hit("INTERNAL_LEGACY_BYTES_NAMING", rel_path, lineno, line.strip())
            )
        if REMOVED_TYPE_RE.search(line):
            category = classify_removed_type(root, path, line)
            if category:
                hits.append(Hit(category, rel_path, lineno, line.strip()))
    return hits


def build_inventory(root: Path) -> dict[str, list[Hit]]:
    by_category: dict[str, list[Hit]] = defaultdict(list)
    for path in iter_text_files(root):
        for hit in scan_file(root, path):
            by_category[hit.category].append(hit)
    return dict(sorted(by_category.items()))


def print_text_inventory(inventory: dict[str, list[Hit]], max_per_category: int) -> None:
    print("Task 204 Bytes type residue inventory")
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
        help="fail if public-surface residue categories are non-empty",
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
            print("task-204 public Bytes type residue gate failed:", file=sys.stderr)
            for category, hits in blocking.items():
                print(f"  {category}: {len(hits)}", file=sys.stderr)
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

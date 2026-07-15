#!/usr/bin/env python3
"""Inventory task-190 C interop surface convergence residue.

The final public surface uses Ptr<T>/MutPtr<T>, mem.ptr/mem.mutPtr/mem.addr,
and mem.load/mem.store.  Removed RawPtr/RawMut names, mem.fromAddress /
mem.addressOf, raw-pointer loadLE/storeLE methods, and Buffer.ptrUnchecked must
only survive as compile-error negative fixtures.
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
    "tmp",
}
SKIP_SCAN_FILES = {
    Path("scripts/check_c_interop_surface.py"),
    Path("scripts/README.md"),
}

REMOVED_RAW_POINTER_TYPE_RE = re.compile(r"\b(?:RawPtr|RawMut)\b")
REMOVED_MEM_BRIDGE_RE = re.compile(r"\b(?:fromAddress|addressOf)\b")
REMOVED_POINTER_METHOD_RE = re.compile(r"\b(?:loadLE|loadLEUnchecked|storeLE|storeLEUnchecked)\b")
REMOVED_BUFFER_PTR_RE = re.compile(r"\bptrUnchecked\b")
CANON_PTR_TYPE_RE = re.compile(r"\b(?:Ptr|MutPtr)<")
CANON_MEM_API_RE = re.compile(r"\bmem\.(?:ptr|mutPtr|addr|load|store)\b")

ALLOWED_RAW_POINTER_TYPE_FILES = {
    Path("tests/compile_errors/ffi/007_removed_raw_pointer_type_names.xr"),
    Path("tests/compile_errors/ffi/007_removed_raw_pointer_type_names.xr.expected"),
}
ALLOWED_MEM_BRIDGE_FILES = {
    Path("tests/compile_errors/type/mem_removed_address_bridge_names.xr"),
    Path("tests/compile_errors/type/mem_removed_address_bridge_names.xr.expected"),
}
ALLOWED_POINTER_METHOD_FILES = {
    Path("tests/compile_errors/type/byte_slice_load_le_removed.xr"),
    Path("tests/compile_errors/type/byte_slice_load_le_removed.xr.expected"),
    Path("tests/compile_errors/type/rawptr_load_le_requires_type_arg.xr"),
    Path("tests/compile_errors/type/rawptr_load_le_requires_type_arg.xr.expected"),
    Path("tests/compile_errors/type/rawptr_load_le_requires_u16_u32_or_u64.xr"),
    Path("tests/compile_errors/type/rawptr_load_le_requires_u16_u32_or_u64.xr.expected"),
    Path("tests/compile_errors/type/rawptr_load_le_unchecked_removed.xr"),
    Path("tests/compile_errors/type/rawptr_load_le_unchecked_removed.xr.expected"),
    Path("tests/compile_errors/type/rawptr_load_le_unchecked_requires_unsafe.xr"),
    Path("tests/compile_errors/type/rawptr_load_le_unchecked_requires_unsafe.xr.expected"),
    Path("tests/compile_errors/type/rawmut_store_le_requires_rawmut.xr"),
    Path("tests/compile_errors/type/rawmut_store_le_requires_rawmut.xr.expected"),
    Path("tests/compile_errors/type/rawmut_store_le_requires_type_arg.xr"),
    Path("tests/compile_errors/type/rawmut_store_le_requires_type_arg.xr.expected"),
    Path("tests/compile_errors/type/rawmut_store_le_requires_u16_u32_or_u64.xr"),
    Path("tests/compile_errors/type/rawmut_store_le_requires_u16_u32_or_u64.xr.expected"),
    Path("tests/compile_errors/type/rawmut_store_le_unchecked_removed.xr"),
    Path("tests/compile_errors/type/rawmut_store_le_unchecked_removed.xr.expected"),
    Path("tests/compile_errors/type/rawmut_store_le_unchecked_requires_unsafe.xr"),
    Path("tests/compile_errors/type/rawmut_store_le_unchecked_requires_unsafe.xr.expected"),
}
ALLOWED_BUFFER_PTR_FILES = {
    Path("tests/compile_errors/type/buffer_ptr_unchecked_removed.xr"),
    Path("tests/compile_errors/type/buffer_ptr_unchecked_removed.xr.expected"),
}

ACTIVE_REMOVED_CATEGORIES = {
    "ACTIVE_REMOVED_RAW_POINTER_TYPE",
    "ACTIVE_REMOVED_MEM_BRIDGE",
    "ACTIVE_REMOVED_POINTER_METHOD",
    "ACTIVE_REMOVED_BUFFER_PTR_UNCHECKED",
}

LAYOUT_GATE_REQUIRED_FILES = {
    Path("tests/diff/task190_layout_cases.txt"),
    Path("tests/diff/cases/semantics/ffi/extern_layout_introspection.xr"),
    Path("tests/diff/cases/semantics/ffi/mem_view_extern_layout.xr"),
    Path("tests/diff/cases/semantics/stdlib/mem_layout_introspection.xr"),
    Path("tests/compile_errors/ffi/020_extern_layout_rejects_managed_field.xr"),
    Path("tests/compile_errors/ffi/023_extern_layout_rejects_safe_struct_field.xr"),
    Path("tests/compile_errors/ffi/025_extern_flex_must_be_last.xr"),
    Path("tests/compile_errors/ffi/026_extern_union_rejects_flex.xr"),
    Path("tests/compile_errors/ffi/027_safe_struct_rejects_flex.xr"),
    Path("tests/compile_errors/ffi/028_extern_flex_rejects_managed_element.xr"),
    Path("tests/compile_errors/ffi/029_extern_flex_requires_prefix.xr"),
    Path("tests/compile_errors/ffi/035_mem_view_flexible_tail_requires_length.xr"),
}

LAYOUT_GATE_REQUIRED_MANIFEST_ENTRIES = {
    "tests/diff/cases/semantics/ffi/extern_layout_introspection.xr",
    "tests/diff/cases/semantics/ffi/mem_view_extern_layout.xr",
    "tests/diff/cases/semantics/stdlib/mem_layout_introspection.xr",
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
    seen: set[Path] = set()
    for dirname in SCAN_DIRS:
        base = root / dirname
        if not base.exists():
            continue
        for path in sorted(base.rglob("*")):
            rel_path = rel(root, path)
            if any(part in SKIP_DIR_NAMES for part in rel_path.parts):
                continue
            if rel_path in SKIP_SCAN_FILES:
                continue
            if path.is_file() and any(str(path).endswith(suffix) for suffix in TEXT_SUFFIXES):
                seen.add(path)
                yield path
    for name in EXTRA_FILES:
        path = root / name
        if path.exists() and path not in seen:
            yield path


def classify_removed(root: Path, path: Path, category: str) -> str:
    rel_path = rel(root, path)
    if category == "raw-pointer-type" and rel_path in ALLOWED_RAW_POINTER_TYPE_FILES:
        return "ALLOWED_REMOVED_RAW_POINTER_TYPE_NEGATIVE_TEST"
    if category == "mem-bridge" and rel_path in ALLOWED_MEM_BRIDGE_FILES:
        return "ALLOWED_REMOVED_MEM_BRIDGE_NEGATIVE_TEST"
    if category == "pointer-method" and rel_path in ALLOWED_POINTER_METHOD_FILES:
        return "ALLOWED_REMOVED_POINTER_METHOD_NEGATIVE_TEST"
    if category == "buffer-ptr" and rel_path in ALLOWED_BUFFER_PTR_FILES:
        return "ALLOWED_REMOVED_BUFFER_PTR_NEGATIVE_TEST"
    if category == "raw-pointer-type":
        return "ACTIVE_REMOVED_RAW_POINTER_TYPE"
    if category == "mem-bridge":
        return "ACTIVE_REMOVED_MEM_BRIDGE"
    if category == "pointer-method":
        return "ACTIVE_REMOVED_POINTER_METHOD"
    return "ACTIVE_REMOVED_BUFFER_PTR_UNCHECKED"


def scan_file(root: Path, path: Path) -> list[Hit]:
    hits: list[Hit] = []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except UnicodeDecodeError:
        return hits
    rel_path = str(rel(root, path))
    for lineno, line in enumerate(lines, 1):
        stripped = line.strip()
        checks = (
            (REMOVED_RAW_POINTER_TYPE_RE, "raw-pointer-type"),
            (REMOVED_MEM_BRIDGE_RE, "mem-bridge"),
            (REMOVED_POINTER_METHOD_RE, "pointer-method"),
            (REMOVED_BUFFER_PTR_RE, "buffer-ptr"),
        )
        for regex, kind in checks:
            if regex.search(line):
                hits.append(Hit(classify_removed(root, path, kind), rel_path, lineno, stripped))
        if CANON_PTR_TYPE_RE.search(line):
            hits.append(Hit("CANON_PTR_MUTPTR_SURFACE", rel_path, lineno, stripped))
        if CANON_MEM_API_RE.search(line):
            hits.append(Hit("CANON_MEM_POINTER_API", rel_path, lineno, stripped))
    return hits


def build_inventory(root: Path) -> dict[str, list[Hit]]:
    by_category: dict[str, list[Hit]] = defaultdict(list)
    for path in iter_text_files(root):
        for hit in scan_file(root, path):
            by_category[hit.category].append(hit)
    return dict(sorted(by_category.items()))


def check_layout_gate(root: Path) -> list[str]:
    """Validate the task-190 layout/flexible-tail focused fixture set."""
    missing = [
        str(path)
        for path in sorted(LAYOUT_GATE_REQUIRED_FILES)
        if not (root / path).is_file()
    ]
    manifest_path = root / "tests/diff/task190_layout_cases.txt"
    if manifest_path.is_file():
        entries = {
            line.strip()
            for line in manifest_path.read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.lstrip().startswith("#")
        }
        for entry in sorted(LAYOUT_GATE_REQUIRED_MANIFEST_ENTRIES - entries):
            missing.append(f"tests/diff/task190_layout_cases.txt entry: {entry}")
    return missing


def print_text_inventory(inventory: dict[str, list[Hit]], max_per_category: int) -> None:
    print("Task 190 C interop surface inventory")
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
        "--fail-on-active-removed",
        action="store_true",
        help="fail if removed task-190 public surface appears outside negative fixtures",
    )
    args = parser.parse_args()

    root = Path(args.root).resolve()
    inventory = build_inventory(root)

    if args.json:
        print(
            json.dumps(
                {
                    "inventory": {
                        category: [asdict(hit) for hit in hits]
                        for category, hits in inventory.items()
                    },
                    "layout_gate_missing": check_layout_gate(root),
                },
                indent=2,
                sort_keys=True,
            )
        )
    else:
        print_text_inventory(inventory, args.max_per_category)
        missing_layout_gate = check_layout_gate(root)
        print(f"TASK190_LAYOUT_FLEX_GATE: {'FAIL' if missing_layout_gate else 'PASS'}")
        for missing in missing_layout_gate:
            print(f"  missing {missing}")

    if args.fail_on_active_removed:
        blocking = {
            category: hits
            for category, hits in inventory.items()
            if category in ACTIVE_REMOVED_CATEGORIES and hits
        }
        missing_layout_gate = check_layout_gate(root)
        if blocking:
            print("task-190 removed C interop surface gate failed:", file=sys.stderr)
            for category, hits in blocking.items():
                print(f"  {category}: {len(hits)}", file=sys.stderr)
            return 1
        if missing_layout_gate:
            print("task-190 layout/flexible-tail focused gate failed:", file=sys.stderr)
            for missing in missing_layout_gate:
                print(f"  missing {missing}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

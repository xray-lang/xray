#!/usr/bin/env python3
"""Audit task-204 direct U8 width predicates.

The canonical byte/u8 identity must be tested through shared helpers such as
`xr_type_is_exact_u8`, `xr_type_is_u8_array`, and `xr_type_is_u8_slice`.
This gate allows only low-level numeric/schema verifier predicates to mention
`XR_NATIVE_U8` directly, and rejects string-based `XR_ELEM_U8` selection.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from dataclasses import asdict, dataclass
from pathlib import Path


SCAN_DIRS = (
    Path("src/frontend/analyzer"),
    Path("src/ir"),
    Path("src/aot"),
)
TEXT_SUFFIXES = (".c", ".h", ".inc", ".inc.c")

DIRECT_NATIVE_U8_RE = re.compile(
    r"\bnative_width\s*(?:==|!=)\s*XR_NATIVE_U8\b|"
    r"\bXR_NATIVE_U8\s*(?:==|!=)\s*[^;\n]*\bnative_width\b"
)
TYPE_KEY_U8_RE = re.compile(r"\bxg_synthetic_width_type_key\([^;\n]*XR_TREF_NW_U8")
ELEM_NAME_U8_RE = re.compile(
    r"\b(?:elem_name|strcmp)\b[^;\n]*\"XR_ELEM_U8\"|"
    r"\"XR_ELEM_U8\"[^;\n]*\b(?:elem_name|strcmp)\b"
)
FUNCTION_RE = re.compile(
    r"^\s*(?:static\s+)?(?:XR_FUNC\s+)?(?:inline\s+)?"
    r"[A-Za-z_][A-Za-z0-9_\s\*]*\s+([A-Za-z_][A-Za-z0-9_]*)\s*\([^;]*\)"
)

ALLOWED = {
    (
        "DIRECT_NATIVE_WIDTH_U8",
        "src/ir/xi_value_query.c",
        "xi_value_is_unsigned_i64_safe_width",
    ): "numeric unsigned-width lattice, not receiver/method selection",
    (
        "DIRECT_NATIVE_WIDTH_U8",
        "src/aot/xaot_verify.c",
        "verify_class_field_native_width",
    ): "serialized class-field schema verifier",
    (
        "TYPE_KEY_U8",
        "src/aot/xaot_verify.c",
        "verify_bulk_elem_type_is_memset_byte",
    ): "serialized bulk memset byte-pattern verifier",
    (
        "TYPE_KEY_U8",
        "src/aot/xaot_bundle.c",
        "bulk_elem_type_is_memset_byte",
    ): "bulk memset byte-pattern planner over serialized type keys",
}


@dataclass(frozen=True)
class Hit:
    category: str
    path: str
    line: int
    function: str
    text: str
    reason: str = ""


def rel(root: Path, path: Path) -> str:
    try:
        return str(path.relative_to(root))
    except ValueError:
        return str(path.resolve().relative_to(root))


def iter_text_files(root: Path):
    for scan_dir in SCAN_DIRS:
        base = root / scan_dir
        if not base.exists():
            continue
        for path in sorted(base.rglob("*")):
            if path.is_file() and any(str(path).endswith(suffix) for suffix in TEXT_SUFFIXES):
                yield path


def update_function_context(line: str, current: str) -> str:
    match = FUNCTION_RE.match(line)
    if match and not line.rstrip().endswith(";"):
        return match.group(1)
    return current


def classify(category: str, path: str, function: str) -> str | None:
    return ALLOWED.get((category, path, function))


def scan_file(root: Path, path: Path) -> tuple[list[Hit], list[Hit]]:
    allowed: list[Hit] = []
    disallowed: list[Hit] = []
    function = "<file-scope>"
    rel_path = rel(root, path)
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except UnicodeDecodeError:
        return allowed, disallowed
    for lineno, line in enumerate(lines, 1):
        function = update_function_context(line, function)
        categories = []
        if DIRECT_NATIVE_U8_RE.search(line):
            categories.append("DIRECT_NATIVE_WIDTH_U8")
        if TYPE_KEY_U8_RE.search(line):
            categories.append("TYPE_KEY_U8")
        if ELEM_NAME_U8_RE.search(line):
            categories.append("ELEM_NAME_U8_STRING_PREDICATE")
        for category in categories:
            reason = classify(category, rel_path, function)
            hit = Hit(category, rel_path, lineno, function, line.strip(), reason or "")
            if reason:
                allowed.append(hit)
            else:
                disallowed.append(hit)
    return allowed, disallowed


def build_inventory(root: Path) -> tuple[dict[str, list[Hit]], dict[str, list[Hit]]]:
    allowed_by_category: dict[str, list[Hit]] = defaultdict(list)
    disallowed_by_category: dict[str, list[Hit]] = defaultdict(list)
    for path in iter_text_files(root):
        allowed, disallowed = scan_file(root, path)
        for hit in allowed:
            allowed_by_category[hit.category].append(hit)
        for hit in disallowed:
            disallowed_by_category[hit.category].append(hit)
    return dict(sorted(allowed_by_category.items())), dict(sorted(disallowed_by_category.items()))


def print_inventory(title: str, inventory: dict[str, list[Hit]]) -> None:
    print(title)
    if not inventory:
        print("  <none>")
        return
    for category, hits in inventory.items():
        print(f"  {category}: {len(hits)}")
        for hit in hits:
            reason = f" ({hit.reason})" if hit.reason else ""
            print(f"    {hit.path}:{hit.line}:{hit.function}: {hit.text}{reason}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--json", action="store_true", help="print machine-readable JSON")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    allowed, disallowed = build_inventory(root)

    if args.json:
        print(
            json.dumps(
                {
                    "allowed": {
                        category: [asdict(hit) for hit in hits]
                        for category, hits in allowed.items()
                    },
                    "disallowed": {
                        category: [asdict(hit) for hit in hits]
                        for category, hits in disallowed.items()
                    },
                },
                indent=2,
                sort_keys=True,
            )
        )
    else:
        print("Task 204 U8 width predicate audit")
        print_inventory("Allowed low-level predicates", allowed)
        print_inventory("Disallowed duplicated predicates", disallowed)

    if disallowed:
        print("task-204 U8 width predicate gate failed:", file=sys.stderr)
        for category, hits in disallowed.items():
            print(f"  {category}: {len(hits)}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Verify shared semantic-core ownership, callers, and the Array.sort ratchet."""

from __future__ import annotations

import argparse
import json
import re
import sys
import tomllib
from pathlib import Path


XISAGEN_DIR = Path(__file__).resolve().parent.parent / "tools" / "xisagen"
if str(XISAGEN_DIR) not in sys.path:
    sys.path.insert(0, str(XISAGEN_DIR))

from xisagen import parse_xi_ops_def, parse_xi_semantic_owners  # noqa: E402


SOURCE_SUFFIXES = (".c", ".h")
SIGNATURE_RE = re.compile(r"\b(?:static\s+)?inline\s+[^;{}]*?\b(xr_[A-Za-z0-9_]+)\s*\(")
SORT_OLD_SYMBOLS = (
    "xr_array_hybrid_sort",
    "xr_sort_merge",
    "TYPED_SORT",
    "xrt_introsort_",
    "xrt_vintrosort",
    "XRT_SORT_DEF",
)
SORT_CONSUMERS = (
    "src/runtime/object/xarray_vm.c",
    "src/aot/xrt_sort.inc.c",
)


def source_files(root: Path) -> list[Path]:
    files: list[Path] = []
    for base in ("src", "stdlib", "include", "tests"):
        directory = root / base
        for path in directory.rglob("*"):
            if path.is_file() and (path.name.endswith(".inc.c") or path.suffix in SOURCE_SUFFIXES):
                files.append(path)
    return sorted(files)


def callers_for(root: Path, header: Path, symbols: list[str], files: list[Path]) -> tuple[list[str], list[str]]:
    production: set[str] = set()
    tests: set[str] = set()
    needles = (header.name, *symbols)
    for path in files:
        if path == header:
            continue
        text = path.read_text(encoding="utf-8", errors="strict")
        if not any(needle in text for needle in needles):
            continue
        relative = path.relative_to(root).as_posix()
        (tests if relative.startswith("tests/") else production).add(relative)
    return sorted(production), sorted(tests)


def build_inventory(root: Path, manifest: dict) -> list[dict]:
    files = source_files(root)
    rows: list[dict] = []
    for entry in sorted(manifest["core"], key=lambda item: item["header"]):
        header = root / entry["header"]
        text = header.read_text(encoding="utf-8")
        symbols = sorted(set(SIGNATURE_RE.findall(text)))
        production, tests = callers_for(root, header, symbols, files)
        status = "production" if production else ("test-only" if tests else "dead")
        rows.append(
            {
                "header": entry["header"],
                "id": entry["id"],
                "representation": entry["representation"],
                "owner": entry["owner"],
                "profiles": entry["profiles"],
                "status": status,
                "signatures": symbols,
                "production_callers": production,
                "test_callers": tests,
                "contains_tagged_value": "XrValue" in text,
            }
        )
    return rows


def verify_sort_ratchet(root: Path) -> list[str]:
    errors: list[str] = []
    for relative in SORT_CONSUMERS:
        text = (root / relative).read_text(encoding="utf-8")
        if "xr_sort_core.h" not in text or "xr_sort_core_" not in text:
            errors.append(f"{relative}: Array.sort no longer consumes the canonical shared kernel")
        for symbol in SORT_OLD_SYMBOLS:
            if symbol in text:
                errors.append(f"{relative}: retired private sort symbol revived: {symbol}")
    return errors


def verify_operation_registry(root: Path) -> tuple[list[str], int]:
    errors: list[str] = []
    source_path = root / "xisa/xi/ops.def"
    try:
        source = source_path.read_text(encoding="utf-8")
        operations = parse_xi_ops_def(source, source_path.as_posix())
        owners = parse_xi_semantic_owners(source, operations, source_path.as_posix())
    except SystemExit:
        return ["xisa/xi/ops.def does not define a complete unique semantic owner registry"], 0

    operation_names = {operation.name for operation in operations}
    inventory_path = root / "contracts/target-machine/semantic-owner-inventory.json"
    if not inventory_path.is_file():
        errors.append("target-machine semantic owner inventory is missing")
        return errors, len(operations)
    inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
    inventory_rows = {
        row["operation_id"]: row
        for row in inventory.get("operations", [])
        if row.get("operation_id", "").startswith("xi.")
    }
    if set(inventory_rows) != operation_names:
        errors.append("target-machine Xi operation inventory differs from xisa/xi/ops.def")
    for name, category in owners.items():
        row = inventory_rows.get(name)
        if row and row.get("future_semantic_owner") != "SemanticPlan.operation_registry":
            errors.append(f"{name}: phase-zero inventory does not point at the operation registry")
        if category not in {
            "declarative-primitive",
            "shared-semantic-kernel",
            "capability-provider",
            "generated-specialization",
        }:
            errors.append(f"{name}: invalid semantic owner category {category}")
    return errors, len(operations)


def verify(root: Path, write: bool) -> list[str]:
    manifest_path = root / "contracts/semantic-owners.toml"
    snapshot_path = root / "contracts/shared-core-inventory.json"
    manifest = tomllib.loads(manifest_path.read_text(encoding="utf-8"))
    errors: list[str] = []

    declared = [entry["header"] for entry in manifest.get("core", [])]
    actual = sorted(path.relative_to(root).as_posix() for path in (root / "src/shared").glob("xr_*_core.h"))
    if len(declared) != len(set(declared)):
        errors.append("semantic-owners.toml contains duplicate core headers")
    if sorted(declared) != actual:
        errors.append(f"core manifest mismatch: declared={sorted(declared)!r} actual={actual!r}")
    if len(actual) != 25:
        errors.append(f"shared-core inventory must contain exactly 25 headers, found {len(actual)}")

    for entry in manifest.get("core", []):
        if entry.get("owner") != "shared-kernel":
            errors.append(f"{entry.get('header')}: observable owner must be shared-kernel")

    inventory = build_inventory(root, manifest)
    for row in inventory:
        if row["representation"] == "native" and row["contains_tagged_value"]:
            errors.append(f"{row['header']}: native kernel contains XrValue")

    rendered = json.dumps({"schema": 1, "cores": inventory}, indent=2, sort_keys=True) + "\n"
    if write:
        snapshot_path.write_text(rendered, encoding="utf-8")
    elif not snapshot_path.is_file():
        errors.append("contracts/shared-core-inventory.json is missing; run with --write")
    elif snapshot_path.read_text(encoding="utf-8") != rendered:
        errors.append("shared-core caller inventory drifted; review it and run with --write")

    errors.extend(verify_sort_ratchet(root))
    registry_errors, _ = verify_operation_registry(root)
    errors.extend(registry_errors)
    return errors


def self_test() -> int:
    assert SIGNATURE_RE.findall("static inline int xr_demo_core(int x) {") == ["xr_demo_core"]
    assert "xrt_introsort_foo".startswith(SORT_OLD_SYMBOLS[3])
    print("semantic-owner verifier self-test: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".")
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    root = Path(args.root).resolve()
    errors = verify(root, args.write)
    if errors:
        print("semantic-owner gate failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    _, operation_count = verify_operation_registry(root)
    print(f"semantic-owner gate: PASS (25 cores, {operation_count} Xi ops, "
          "unique owner categories, Array.sort production ratchet)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

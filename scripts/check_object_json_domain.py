#!/usr/bin/env python3
"""Inventory the object/Json domain surface and fail closed after convergence."""

from __future__ import annotations

import argparse
import json
import re
from dataclasses import dataclass
from pathlib import Path


TEXT_SUFFIXES = {
    ".c",
    ".h",
    ".def",
    ".inc",
    ".json",
    ".md",
    ".py",
    ".sh",
    ".toml",
    ".tsv",
    ".xr",
}

SKIP_PARTS = {
    ".cache",
    ".evidence",
    ".git",
    "build",
    "build-asan",
    "build-release",
}

SKIP_FILES = {
    Path("scripts/check_object_json_domain.py"),
    Path("contracts/object-json-domain.md"),
}


@dataclass(frozen=True)
class Rule:
    name: str
    pattern: re.Pattern[str]
    roots: tuple[str, ...]


RULES = (
    Rule(
        "PUBLIC_RECORD_SURFACE",
        re.compile(r"\bRecord\b|\bType\.record\b|\bTYPE_NAME_RECORD\b"),
        ("spec", "stdlib", "src/app", "src/frontend/analyzer", "demos", "tools"),
    ),
    Rule(
        "INTERNAL_RECORD_IDENTITY",
        re.compile(r"\bXR_KIND_STRUCT_OBJECT\b|\bXR_BK_STRUCT_OBJECT\b|\bXRT_OBJECT_STRUCT\b"),
        ("src", "stdlib", "tests", "xisa"),
    ),
    Rule(
        "LEGACY_RECORD_TYPE_API",
        re.compile(
            r"\bXR_TYPE_IS_STRUCT_OBJECT\b|\bxr_type_new_struct_object(?:_with_fields)?\b|"
            r"\bXR_TID_RECORD\b|\bTYPE_NAME_RECORD\b"
        ),
        ("src", "stdlib", "tests", "xisa"),
    ),
    Rule(
        "LEGACY_FIXED_FIELD_OP",
        re.compile(r"\bXI_JSON_(?:GET|SET|INIT)_F\b"),
        ("src", "tests", "xisa"),
    ),
    Rule(
        "LEGACY_RECORD_EVIDENCE",
        re.compile(r"\b(?:Xg|Xaot)Record(?:Shape|Field|Access|Merge|Options)"),
        ("src/analysis", "src/aot", "src/ir", "tests", "xisa"),
    ),
)


def iter_text_files(root: Path):
    for path in root.rglob("*"):
        if not path.is_file() or path.suffix.lower() not in TEXT_SUFFIXES:
            continue
        rel = path.relative_to(root)
        if rel in SKIP_FILES or any(part in SKIP_PARTS for part in rel.parts):
            continue
        yield rel, path


def under_any_root(rel: Path, roots: tuple[str, ...]) -> bool:
    value = rel.as_posix()
    return any(value == root or value.startswith(root + "/") for root in roots)


def collect(root: Path) -> dict[str, list[dict[str, object]]]:
    hits: dict[str, list[dict[str, object]]] = {rule.name: [] for rule in RULES}
    for rel, path in iter_text_files(root):
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except UnicodeDecodeError:
            continue
        for rule in RULES:
            if not under_any_root(rel, rule.roots):
                continue
            for line_number, line in enumerate(lines, 1):
                if rule.pattern.search(line):
                    hits[rule.name].append(
                        {"path": rel.as_posix(), "line": line_number, "text": line.strip()}
                    )
    return hits


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--fail-on-legacy", action="store_true")
    args = parser.parse_args()

    root = args.root.resolve()
    hits = collect(root)
    counts = {name: len(rows) for name, rows in hits.items()}
    payload = {"schema": 1, "counts": counts, "hits": hits}

    if args.json:
        print(json.dumps(payload, indent=2, sort_keys=True))
    else:
        for name in sorted(counts):
            print(f"{name}={counts[name]}")

    if args.fail_on_legacy and any(counts.values()):
        for name, rows in hits.items():
            for row in rows[:20]:
                print(f"{name}: {row['path']}:{row['line']}: {row['text']}")
            if len(rows) > 20:
                print(f"{name}: ... {len(rows) - 20} more")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Guard the exact structural-object and Map-backed JSON boundary."""

from __future__ import annotations

import argparse
import json
import os
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
}

# Any directory whose name starts with one of these is a build tree, not source
# under governance. Naming them one by one meant a newly configured tree was
# walked in full: a build directory holds far more files than the sources do,
# so the walk cost, not the file count, is what this avoids.
SKIP_PREFIXES = ("build",)


def _is_skipped_dir(name: str) -> bool:
    return name in SKIP_PARTS or name.startswith(SKIP_PREFIXES)

SKIP_FILES = {
    Path("scripts/check_structural_object_json_map_boundary.py"),
    Path("contracts/structural-object-json-map-boundary.md"),
}


@dataclass(frozen=True)
class Rule:
    name: str
    pattern: re.Pattern[str]
    roots: tuple[str, ...]


RULES = (
    Rule(
        "PUBLIC_LEGACY_JSON_TYPE",
        re.compile(r"(?<![A-Za-z0-9_.])Json(?![A-Za-z0-9_])|@derive\(Json\)"),
        ("spec", "stdlib", "src/app", "demos"),
    ),
    Rule(
        "PUBLIC_LEGACY_JSON_NAMESPACE",
        re.compile(r"\bJson\.(?:parse|decode|encode|stringify|value|kindOf|isValid)\b"),
        ("spec", "stdlib", "src/app", "demos"),
    ),
    Rule(
        "PUBLIC_RECORD_SURFACE",
        re.compile(r"\bRecord\b|\bType\.record\b|\bTYPE_NAME_RECORD\b"),
        ("spec", "stdlib", "src/app", "src/frontend/analyzer", "demos", "tools"),
    ),
    Rule(
        "INTERNAL_RECORD_IDENTITY",
        re.compile(r"\bXR_KIND_RECORD\b|\bXR_BK_RECORD\b|\bXRT_OBJECT_RECORD\b"),
        ("src", "stdlib", "tests", "xisa"),
    ),
    Rule(
        "LEGACY_RECORD_TYPE_API",
        re.compile(
            r"\bXR_TYPE_IS_RECORD\b|\bxr_type_new_record(?:_with_fields)?\b|"
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
    Rule(
        "LEGACY_OBJECT_ROW_METADATA",
        re.compile(
            r"\bXrObjectRowMode\b|\bXR_OBJECT_ROW_(?:EXACT|OPEN)\b|"
            r"\bfield_optional\b|\bobject_row_mode\b"
        ),
        ("src", "stdlib", "tests", "xisa"),
    ),
    Rule(
        "DYNAMIC_STRUCTURAL_OBJECT_PATH",
        re.compile(
            r"\bXG_OBJECT_DOMAIN_JSON\b|\bXG_OBJECT_SHAPE_EXTENSIBLE\b|"
            r"\bXAOT_OBJECT_ACCESS_DYNAMIC_JSON_LOOKUP\b|"
            r"\bxrt_object_(?:get|set)_dynamic\b|\bxrt_object_extension_map\b"
        ),
        ("src", "stdlib", "tests", "xisa"),
    ),
    Rule(
        "SECOND_JSON_OBJECT_STORAGE",
        re.compile(r"\bXrJsonObject\b|\bxrt_json_object_t\b"),
        ("src", "stdlib", "tests", "xisa"),
    ),
)


def iter_text_files(root: Path):
    # Prune skipped directories while walking rather than filtering afterwards:
    # rglob would still descend into every build tree and cache before the
    # filter ran, which is where the time went.
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if not _is_skipped_dir(d)]
        base = Path(dirpath)
        for name in filenames:
            path = base / name
            if path.suffix.lower() not in TEXT_SUFFIXES:
                continue
            rel = path.relative_to(root)
            if rel in SKIP_FILES:
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
                stripped = line.lstrip()
                if (
                    rule.name
                    in {
                        "PUBLIC_RECORD_SURFACE",
                        "PUBLIC_LEGACY_JSON_TYPE",
                        "PUBLIC_LEGACY_JSON_NAMESPACE",
                    }
                    and path.suffix.lower() in {".c", ".h", ".inc", ".py", ".sh"}
                    and stripped.startswith(("//", "/*", "*", "#"))
                ):
                    continue
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

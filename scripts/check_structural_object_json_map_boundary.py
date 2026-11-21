#!/usr/bin/env python3
"""Reject retired object identities while preserving nominal names and Map JSON."""

from __future__ import annotations

import argparse
import json
import os
import re
from dataclasses import dataclass
from pathlib import Path

from xray_scan_patterns import ScanPattern, literal_alternatives


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
}

RETIRED_JSON_NOTICE_PATH = Path("spec/source/accepted/338-recursive-json-value-and-typed-codec-cn.md")
RETIRED_JSON_NOTICE = re.compile(
    r"不得保旧 (?P<names>Json／JsonValue) 别名、旧 raw fallback、双签名或迁移开关。"
)


@dataclass(frozen=True)
class Rule:
    name: str
    pattern: ScanPattern
    roots: tuple[str, ...]


RULES = (
    Rule(
        "DELETED_STRUCTURAL_OBJECT_IDENTITY",
        literal_alternatives(
            ("STRUCT_OBJ", r"\b[A-Za-z_][A-Za-z_0-9]*", r"[A-Za-z_0-9]*\b"),
            "AST_OBJECT_LITERAL", "AST_OBJECT_PATTERN", "AST_SET_LITERAL",
            "TK_HASH_LBRACE", "TK_HASH_LBRACKET", "XR_XIR_TYPE_RECORD",
        ),
        ("src", "include", "xisa"),
    ),
    Rule(
        "PUBLIC_LEGACY_JSON_TYPE",
        literal_alternatives(
            ("Json", r"(?<![A-Za-z0-9_.])", r"(?:Value)?(?![A-Za-z0-9_])"),
            ("Json", r"@derive\(", r"\)"),
        ),
        ("spec", "stdlib", "src/app", "demos"),
    ),
    Rule(
        "PUBLIC_LEGACY_JSON_NAMESPACE",
        literal_alternatives(("Json.", r"\b", r"(?:parse|decode|encode|stringify|value|kindOf|isValid)\b")),
        ("spec", "stdlib", "src/app", "demos"),
    ),
    Rule(
        "PUBLIC_RECORD_SURFACE",
        literal_alternatives("Type.record", "TYPE_NAME_RECORD"),
        ("spec", "stdlib", "src/app", "src/frontend/analyzer", "demos", "tools"),
    ),
    Rule(
        "INTERNAL_RECORD_IDENTITY",
        literal_alternatives("XR_KIND_RECORD", "XR_BK_RECORD", "XRT_OBJECT_RECORD"),
        ("src", "stdlib", "tests", "xisa"),
    ),
    Rule(
        "LEGACY_RECORD_TYPE_API",
        literal_alternatives(
            "XR_TYPE_IS_RECORD", ("xr_type_new_record", r"\b", r"(?:_with_fields)?\b"),
            "XR_TID_RECORD", "TYPE_NAME_RECORD",
        ),
        ("src", "stdlib", "tests", "xisa"),
    ),
    Rule(
        "LEGACY_FIXED_FIELD_OP",
        literal_alternatives(("XI_JSON_", r"\b", r"(?:GET|SET|INIT)_F\b")),
        ("src", "tests", "xisa"),
    ),
    Rule(
        "LEGACY_RECORD_EVIDENCE",
        literal_alternatives(("Record", r"\b(?:Xg|Xaot)", r"(?:Shape|Field|Access|Merge|Options)")),
        ("src/analysis", "src/aot", "src/ir", "tests", "xisa"),
    ),
    Rule(
        "LEGACY_OBJECT_ROW_METADATA",
        literal_alternatives(
            "XrObjectRowMode", ("XR_OBJECT_ROW_", r"\b", r"(?:EXACT|OPEN)\b"),
            "field_optional", "object_row_mode",
        ),
        ("src", "stdlib", "tests", "xisa"),
    ),
    Rule(
        "DYNAMIC_STRUCTURAL_OBJECT_PATH",
        literal_alternatives(
            "XG_OBJECT_DOMAIN_JSON", "XG_OBJECT_SHAPE_EXTENSIBLE", "XAOT_OBJECT_ACCESS_DYNAMIC_JSON_LOOKUP",
            ("xrt_object_", r"\b", r"(?:get|set)_dynamic\b"), "xrt_object_extension_map",
        ),
        ("src", "stdlib", "tests", "xisa"),
    ),
    Rule(
        "SECOND_JSON_OBJECT_STORAGE",
        literal_alternatives("XrJsonObject", "xrt_json_object_t"),
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


def has_active_match(rule: Rule, rel: Path, line: str) -> bool:
    # The canonical prohibition names what was retired. Exempt only those name
    # spans: active types or calls elsewhere on the same line still fail.
    retired_spans = ()
    if rule.name == "PUBLIC_LEGACY_JSON_TYPE" and rel == RETIRED_JSON_NOTICE_PATH:
        retired_spans = tuple(match.span("names") for match in RETIRED_JSON_NOTICE.finditer(line))
    if not retired_spans:
        return rule.pattern.regex.search(line) is not None
    return any(
        not any(start <= match.start() and match.end() <= end for start, end in retired_spans)
        for match in rule.pattern.regex.finditer(line)
    )


def collect(root: Path) -> dict[str, list[dict[str, object]]]:
    hits: dict[str, list[dict[str, object]]] = {rule.name: [] for rule in RULES}
    for rel, path in iter_text_files(root):
        applicable_rules = [rule for rule in RULES if under_any_root(rel, rule.roots)]
        if not applicable_rules:
            continue
        try:
            source = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        applicable_rules = [rule for rule in applicable_rules if rule.pattern.may_match(source)]
        if not applicable_rules:
            continue
        lines = source.splitlines()
        for rule in applicable_rules:
            skip_comments = (
                rule.name in {"PUBLIC_RECORD_SURFACE", "PUBLIC_LEGACY_JSON_TYPE", "PUBLIC_LEGACY_JSON_NAMESPACE"}
                and path.suffix.lower() in {".c", ".h", ".inc", ".py", ".sh"}
            )
            for line_number, line in enumerate(lines, 1):
                if skip_comments and line.lstrip().startswith(("//", "/*", "*", "#")):
                    continue
                if has_active_match(rule, rel, line):
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

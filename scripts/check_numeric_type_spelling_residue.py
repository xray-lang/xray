#!/usr/bin/env python3
"""Fail-closed public-surface residue gate for task 239.

Executable Xray source is scanned as tokens so comments and literals remain
valid historical evidence. Public documentation and generated tooling assets
are scanned as text. Internal stdlib ``aot_const`` payload kinds deliberately
remain ``int64``/``float64`` and are excluded by semantic field name.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

from numeric_type_spelling import OLD_TO_NEW, scan_identifiers


OLD_WORD = re.compile(r"\b(" + "|".join(map(re.escape, OLD_TO_NEW)) + r")\b")
XRAY_ROOTS = ("stdlib", "tests", "demos", "bench")
PUBLIC_FILES = (
    "README.md",
    "README_CN.md",
    "LANGUAGE_SPEC.md",
    "LANGUAGE_SPEC_CN.md",
    "src/app/mcp/xmcp_knowledge_generated.c",
)
PUBLIC_TREES = (
    ("docs/knowledge", {".md", ".json"}),
    ("spec/source", {".md", ".json"}),
    ("src/app/lsp", {".c", ".h", ".inc"}),
    ("tests", {".expected", ".expect"}),
    ("stdlib/defs", {".def"}),
)


def _line_hits(path: Path, root: Path) -> list[dict[str, object]]:
    hits: list[dict[str, object]] = []
    for line_no, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if path.parent.name == "defs" and "aot_const:" in line:
            continue
        for match in OLD_WORD.finditer(line):
            hits.append(
                {
                    "path": str(path.relative_to(root)),
                    "line": line_no,
                    "spelling": match.group(1),
                    "bucket": "public-text",
                }
            )
    return hits


def _xray_hits(path: Path, root: Path) -> list[dict[str, object]]:
    source = path.read_text(encoding="utf-8")
    return [
        {
            "path": str(path.relative_to(root)),
            "line": source.count("\n", 0, start) + 1,
            "spelling": spelling,
            "bucket": "xray-token",
        }
        for start, _end, spelling in scan_identifiers(source)
        if spelling in OLD_TO_NEW
    ]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--fail-on-public", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    hits: list[dict[str, object]] = []

    for rel in XRAY_ROOTS:
        tree = root / rel
        if tree.is_dir():
            for path in sorted(tree.rglob("*.xr")):
                hits.extend(_xray_hits(path, root))

    public_paths: set[Path] = set()
    for rel in PUBLIC_FILES:
        path = root / rel
        if path.is_file():
            public_paths.add(path)
    for rel, suffixes in PUBLIC_TREES:
        tree = root / rel
        if not tree.is_dir():
            continue
        public_paths.update(path for path in tree.rglob("*") if path.is_file() and path.suffix in suffixes)
    for path in sorted(public_paths):
        hits.extend(_line_hits(path, root))

    print(json.dumps({"schema": 1, "hits": hits, "hit_count": len(hits)}, indent=2, sort_keys=True))
    return 1 if args.fail_on_public and hits else 0


if __name__ == "__main__":
    raise SystemExit(main())

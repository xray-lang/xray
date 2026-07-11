#!/usr/bin/env python3
"""Guard the stdlib `parallel` API from legacy syntax/API backsliding.

Task 193 removes the dedicated `parallel for/range/reduce/collect` grammar.
The only user surface is `import parallel` plus the stdlib functions/classes.
This checker is intentionally mechanical: it blocks old user-source syntax,
active spec/demo/API-doc text, old parser/AST entry points, and the old
`tests/aot/coro/parallel_*.xr` migration bucket from reappearing.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


USER_XR_SOURCE_DIRS = ("stdlib", "tests", "demos")
PUBLIC_TEXT_DIRS = (
    "spec",
    "demos",
    "docs/spec",
    "docs/language",
    "docs/knowledge",
    "docs/rules",
)
ALLOWED_LEGACY_XR = {
    Path("tests/compile_errors/stdlib/parallel_keyword_for_removed.xr"),
}

LEGACY_XR_PATTERNS = (
    (
        re.compile(r"\bparallel\s+(for|range|reduce|collect)\b"),
        "legacy parallel grammar",
    ),
    (
        re.compile(r"\bparallel\b.*\b(local|final)\b"),
        "legacy parallel local/final grammar",
    ),
)

LEGACY_SOURCE_PATTERNS = (
    (re.compile(r"\bTK_PARALLEL\b"), "legacy parallel keyword token"),
    (re.compile(r"\bAST_PARALLEL_[A-Z0-9_]+\b"), "legacy parallel AST node"),
    (re.compile(r"\bXrParallelLocalBinding\b"), "legacy parallel local/final AST binding"),
    (re.compile(r"\bxr_parse_parallel_[A-Za-z0-9_]+\b"), "legacy parallel parser entry"),
    (re.compile(r"\blower_parallel_[A-Za-z0-9_]+_(expr|stmt)\b"), "legacy AST parallel lowering"),
    (re.compile(r"\bXI_PAR_COLLECT\b"), "legacy collect IR opcode"),
    (re.compile(r"\bXiParallelCollect(Data)?\b"), "legacy collect IR data"),
)

PUBLIC_TEXT_SUFFIXES = {".md", ".xr"}
SOURCE_SUFFIXES = {".c", ".h", ".inc.c", ".def"}


def rel(root: Path, path: Path) -> Path:
    try:
        return path.relative_to(root)
    except ValueError:
        return path.resolve().relative_to(root)


def iter_files(root: Path, dirs: tuple[str, ...], suffixes: set[str]):
    for directory in dirs:
        base = root / directory
        if not base.exists():
            continue
        for path in sorted(base.rglob("*")):
            if path.is_file() and any(str(path).endswith(suffix) for suffix in suffixes):
                yield path


def scan_text_file(root: Path, path: Path, patterns) -> list[str]:
    errors: list[str] = []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except UnicodeDecodeError:
        return errors
    for lineno, line in enumerate(lines, 1):
        for pattern, label in patterns:
            if pattern.search(line):
                errors.append(f"{rel(root, path)}:{lineno}: {label}: {line.strip()}")
    return errors


def check_legacy_xr_surface(root: Path) -> list[str]:
    errors: list[str] = []
    for path in iter_files(root, USER_XR_SOURCE_DIRS, {".xr"}):
        if rel(root, path) in ALLOWED_LEGACY_XR:
            continue
        errors.extend(scan_text_file(root, path, LEGACY_XR_PATTERNS))
    return errors


def check_legacy_public_text(root: Path) -> list[str]:
    # `docs/` is a symlink to the docs repo and intentionally contains
    # historical task/export/review records. Gate only active public-doc
    # subtrees; task-history docs remain evidence, not current API surface.
    errors: list[str] = []
    seen: set[Path] = set()
    for path in iter_files(root, PUBLIC_TEXT_DIRS, PUBLIC_TEXT_SUFFIXES):
        rel_path = rel(root, path)
        if rel_path in seen or rel_path in ALLOWED_LEGACY_XR:
            continue
        seen.add(rel_path)
        errors.extend(scan_text_file(root, path, LEGACY_XR_PATTERNS))
    return errors


def check_legacy_source_symbols(root: Path) -> list[str]:
    return [
        error
        for path in iter_files(root, ("src", "tests", "scripts"), SOURCE_SUFFIXES)
        for error in scan_text_file(root, path, LEGACY_SOURCE_PATTERNS)
    ]


def check_migrated_aot_coro_bucket(root: Path) -> list[str]:
    bucket = root / "tests" / "aot" / "coro"
    if not bucket.exists():
        return []
    return [
        f"{rel(root, path)}: legacy parallel_* AOT coro fixture bucket must stay empty"
        for path in sorted(bucket.glob("parallel_*.xr"))
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    errors = (
        check_legacy_xr_surface(root)
        + check_legacy_public_text(root)
        + check_legacy_source_symbols(root)
        + check_migrated_aot_coro_bucket(root)
    )
    if errors:
        print("parallel surface convergence gate failed:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        print(
            "\nUse the stdlib `parallel` module API only; do not restore parser aliases, "
            "legacy AST nodes, or old AOT coro fixtures.",
            file=sys.stderr,
        )
        return 1

    print("OK: parallel surface has no legacy grammar/parser/spec-demo-doc/AOT-coro residue")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""
Ensure in-tree stdlib declarations stay in stdlib/defs/*.def.

Third-party modules may still use xbuiltin_decl.h with gen_stdlib_types.py
--xrd, but checked-in stdlib C modules should not carry declaration metadata.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


DISALLOWED = [
    (re.compile(r"\bXR_DEFINE_BUILTIN\s*\("), "XR_DEFINE_BUILTIN"),
    (re.compile(r"//\s*@module\b"), "// @module"),
    (re.compile(r"//\s*@type\b"), "// @type"),
    (re.compile(r"//\s*@handle\b"), "// @handle"),
    (re.compile(r"src/module/xbuiltin_decl\.h"), "xbuiltin_decl.h include"),
]


def check_file(path: Path) -> list[str]:
    errors: list[str] = []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except UnicodeDecodeError:
        return errors
    for lineno, line in enumerate(lines, 1):
        for pattern, label in DISALLOWED:
            if pattern.search(line):
                errors.append(f"{path}:{lineno}: in-tree stdlib C uses {label}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".", help="project root")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    stdlib = root / "stdlib"
    errors: list[str] = []
    for path in sorted(stdlib.rglob("*.c")):
        errors.extend(check_file(path))

    if errors:
        print("stdlib declaration source gate failed:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        print("Move in-tree stdlib declarations to stdlib/defs/*.def.", file=sys.stderr)
        return 1

    print("OK: in-tree stdlib declarations are sourced from stdlib/defs/*.def")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

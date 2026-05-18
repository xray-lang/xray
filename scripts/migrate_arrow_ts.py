#!/usr/bin/env python3
"""Migrate xray code embedded in TypeScript / JavaScript template literals.

TS source uses `=>` for its own arrow functions, so we cannot blindly
apply the .xr migration rules to the whole file. Instead, this script
extracts the contents of each backtick template literal, runs the xray
migration core on them, and writes the result back.

Run: python3 scripts/migrate_arrow_ts.py <file.ts> [<file.ts> ...]
"""
import sys
import re
import pathlib

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from migrate_arrow_xr import migrate_code  # noqa: E402


def migrate_template_literals(src: str) -> str:
    """Find backtick template literals and apply migrate_code to each
    one's body. Embedded `${...}` expressions are kept verbatim (we
    don't re-parse them; their bytes are passed through to migrate_code
    which treats `$` as ordinary code — safe because no rewrite rule
    matches `${`).
    """
    out: list[str] = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        if c == "`":
            # Find matching closing backtick, respecting escape sequences
            # and nested `${ ... }` expressions (which may themselves
            # contain template literals — but that's a rare edge case we
            # treat by counting `${` / `}` depth).
            j = i + 1
            depth = 0
            while j < n:
                ch = src[j]
                if ch == "\\" and j + 1 < n:
                    j += 2
                    continue
                if ch == "$" and j + 1 < n and src[j + 1] == "{":
                    depth += 1
                    j += 2
                    continue
                if depth > 0 and ch == "}":
                    depth -= 1
                    j += 1
                    continue
                if depth == 0 and ch == "`":
                    break
                j += 1
            body = src[i + 1 : j]
            migrated = migrate_code(body)
            out.append("`")
            out.append(migrated)
            if j < n:
                out.append("`")
                i = j + 1
            else:
                i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def migrate_file(path: pathlib.Path) -> bool:
    text = path.read_text(encoding="utf-8")
    new_text = migrate_template_literals(text)
    if new_text == text:
        return False
    path.write_text(new_text, encoding="utf-8")
    return True


def main():
    if len(sys.argv) < 2:
        print("Usage: migrate_arrow_ts.py <file.ts> ...", file=sys.stderr)
        sys.exit(2)
    total = 0
    changed = 0
    for arg in sys.argv[1:]:
        p = pathlib.Path(arg)
        if not p.is_file():
            print(f"  skip: {p}", file=sys.stderr)
            continue
        total += 1
        if migrate_file(p):
            changed += 1
            print(f"  migrated: {p}", file=sys.stderr)
    print(f"Done: {changed}/{total} files changed", file=sys.stderr)


if __name__ == "__main__":
    main()

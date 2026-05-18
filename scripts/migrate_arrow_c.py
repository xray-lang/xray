#!/usr/bin/env python3
"""Migrate xray source strings embedded in C unit-test files.

The .xr code appears inside C string literals (`"fn foo(): int {\n"...`).
Since C syntax never produces patterns like `): TypeName {` outside of such
literals, we apply the xr migration rules directly to the whole file.

This is a thin wrapper around scripts/migrate_arrow_xr.py's core regexes.

Run: python3 scripts/migrate_arrow_c.py <file.c> [<file.c> ...]
"""
import sys
import pathlib

# Reuse rewrite functions from the .xr migration script.
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from migrate_arrow_xr import migrate_code  # noqa: E402


def migrate_file(path: pathlib.Path) -> bool:
    # Use latin-1 as a byte-preserving codec: any byte 0-255 round-trips
    # cleanly. This handles C files with mixed encodings in comments.
    text = path.read_text(encoding="latin-1")
    # Apply migrate_code directly (no mask/unmask). C source outside string
    # literals never contains `): TypeName {` or `=>` patterns that match our
    # rules, so collateral damage is negligible. The handful of in-prose
    # comments that mention `=>` are acceptable to update.
    new_text = migrate_code(text)
    if new_text == text:
        return False
    path.write_text(new_text, encoding="latin-1")
    return True


def main():
    if len(sys.argv) < 2:
        print("Usage: migrate_arrow_c.py <file.c> ...", file=sys.stderr)
        sys.exit(2)
    changed = 0
    total = 0
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

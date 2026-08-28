#!/usr/bin/env python3
"""Splice one module block into core.def or the boundary manifest.

Both files are edited by several lanes at once, so a migration hands over the
replacement block for one module and this script places it. Replacing a named
region keeps every other module's text byte-identical, which is what makes two
concurrent migrations merge without a conflict.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


def splice_core_def(text: str, module: str, block: str) -> str:
    lines = text.splitlines(keepends=True)
    start = None
    for index, line in enumerate(lines):
        if line.rstrip("\n") == f"module {module} {{":
            start = index
            break
    if start is None:
        raise SystemExit(f"core.def has no `module {module}` block")
    end = None
    for index in range(start + 1, len(lines)):
        if lines[index].rstrip("\n") == "}":
            end = index
            break
    if end is None:
        raise SystemExit(f"core.def `module {module}` block is unterminated")
    # A deletion also takes the blank line that separated the block from its
    # successor, so removing a module leaves no double blank behind.
    stop = end + 1
    if not block.strip() and stop < len(lines) and lines[stop].strip() == "":
        stop += 1
    replacement = [] if not block.strip() else [block if block.endswith("\n") else block + "\n"]
    return "".join(lines[:start] + replacement + lines[stop:])


def splice_boundary(text: str, module: str, block: str) -> str:
    lines = text.splitlines(keepends=True)
    start = None
    for index, line in enumerate(lines):
        if line.rstrip("\n") == f'name = "{module}"':
            # The `[[module]]` header sits directly above the name line.
            start = index - 1
            break
    if start is None or lines[start].strip() != "[[module]]":
        raise SystemExit(f"boundary manifest has no `[[module]] name = \"{module}\"` entry")
    end = len(lines)
    for index in range(start + 1, len(lines)):
        if lines[index].strip() == "[[module]]":
            end = index
            break
    # Trailing blank lines belong to the separator, not to the entry, so the
    # replacement keeps the one blank line the next entry expects above it.
    while end > start and lines[end - 1].strip() == "":
        end -= 1
    body = block.rstrip("\n") + "\n"
    tail = lines[end:]
    if tail and tail[0].strip() != "":
        body += "\n"
    return "".join(lines[:start] + [body] + tail)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--module", required=True)
    parser.add_argument("--core-def-block", help="replacement `module X { ... }` block file")
    parser.add_argument("--boundary-block", help="replacement `[[module]]` entry file")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    if args.core_def_block:
        path = root / "stdlib" / "defs" / "core.def"
        block = Path(args.core_def_block).read_text()
        path.write_text(splice_core_def(path.read_text(), args.module, block))
        print(f"core.def: spliced module {args.module}")
    if args.boundary_block:
        path = root / "stdlib" / "stdlib_boundary.toml"
        block = Path(args.boundary_block).read_text()
        path.write_text(splice_boundary(path.read_text(), args.module, block))
        print(f"stdlib_boundary.toml: spliced module {args.module}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

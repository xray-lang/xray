#!/usr/bin/env python3
"""Token-aware inventory, one-shot rewrite and residue gate for task 239.

Only identifier tokens in Xray source are considered. Comments, quoted/raw/
byte/C strings, rune literals and template strings are preserved byte-for-byte.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

OLD_TO_NEW = {
    "int8": "i8",
    "int16": "i16",
    "int32": "i32",
    "int64": "i64",
    "uint8": "u8",
    "uint16": "u16",
    "uint32": "u32",
    "uint64": "u64",
    "float32": "f32",
    "float64": "f64",
    "intsize": "isize",
    "uintsize": "usize",
}


def _ident_start(ch: str) -> bool:
    return ch == "_" or ch.isalpha() or ord(ch) >= 0x80


def _ident_continue(ch: str) -> bool:
    return _ident_start(ch) or ch.isdigit()


def scan_identifiers(source: str):
    """Yield (start, end, spelling) for identifiers in executable source."""
    i = 0
    n = len(source)
    while i < n:
        ch = source[i]
        if ch == "/" and i + 1 < n and source[i + 1] == "/":
            i += 2
            while i < n and source[i] not in "\r\n":
                i += 1
            continue
        if ch == "/" and i + 1 < n and source[i + 1] == "*":
            i += 2
            depth = 1
            while i < n and depth:
                if i + 1 < n and source[i : i + 2] == "/*":
                    depth += 1
                    i += 2
                elif i + 1 < n and source[i : i + 2] == "*/":
                    depth -= 1
                    i += 2
                else:
                    i += 1
            continue
        if ch in ('"', "'", "`"):
            quote = ch
            i += 1
            while i < n:
                if source[i] == "\\" and quote != "`":
                    i += 2
                elif source[i] == quote:
                    i += 1
                    break
                else:
                    i += 1
            continue
        if _ident_start(ch):
            start = i
            i += 1
            while i < n and _ident_continue(source[i]):
                i += 1
            yield start, i, source[start:i]
            continue
        i += 1


def rewrite_source(source: str) -> tuple[str, dict[str, int]]:
    edits = []
    counts: dict[str, int] = {}
    for start, end, word in scan_identifiers(source):
        replacement = OLD_TO_NEW.get(word)
        if replacement:
            edits.append((start, end, replacement))
            counts[word] = counts.get(word, 0) + 1
    for start, end, replacement in reversed(edits):
        source = source[:start] + replacement + source[end:]
    return source, counts


def xr_files(roots: list[Path]):
    seen = set()
    for root in roots:
        candidates = [root] if root.is_file() else root.rglob("*.xr")
        for path in candidates:
            resolved = path.resolve()
            if resolved not in seen and path.suffix == ".xr":
                seen.add(resolved)
                yield path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("inventory", "rewrite", "check"))
    parser.add_argument("roots", nargs="+", type=Path)
    args = parser.parse_args()

    records = []
    total: dict[str, int] = {}
    for path in sorted(xr_files(args.roots), key=lambda p: str(p)):
        source = path.read_text(encoding="utf-8")
        rewritten, counts = rewrite_source(source)
        for word, count in counts.items():
            total[word] = total.get(word, 0) + count
        if counts:
            records.append(
                {
                    "path": str(path),
                    "sha256": hashlib.sha256(source.encode()).hexdigest(),
                    "tokens": counts,
                }
            )
            if args.mode == "rewrite":
                path.write_text(rewritten, encoding="utf-8")

    result = {"files": records, "totals": dict(sorted(total.items()))}
    print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True))
    if args.mode == "check" and total:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Migrate xray code embedded in Markdown documentation files.

Targets only:
  - Fenced code blocks tagged as xray / xr   (```xray ... ```)
  - Untagged fenced code blocks that look like xray (heuristic: contain
    `fn ` or `let ` or `=> ` near the start)
  - Inline code spans `...` containing xray-specific patterns

Other text (prose) is left untouched.

Run: python3 scripts/migrate_arrow_md.py <file.md> [<file.md> ...]
"""
import sys
import re
import pathlib

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from migrate_arrow_xr import migrate_code  # noqa: E402


# Match a fenced code block: ```LANG ... ``` (or ~~~LANG ... ~~~).
# Group 1: the opening fence + lang line, group 2: the body, group 3: closing fence.
FENCE_RE = re.compile(
    r"(?P<open>```|~~~)(?P<lang>[A-Za-z0-9_+\-]*)\n(?P<body>.*?)(?P<close>(?P=open))",
    re.DOTALL,
)


XRAY_LANGS = {"xray", "xr"}


def looks_like_xray(body: str) -> bool:
    head = body.lstrip()[:200]
    if "fn " in head or "let " in head or "shared " in head:
        return True
    if " => " in head and ("\"" in head or "{" in head):
        # Map literal or match arm — likely xray
        return True
    return False


def migrate_fenced(text: str) -> str:
    def repl(m: re.Match) -> str:
        lang = m.group("lang").lower()
        body = m.group("body")
        if lang in XRAY_LANGS or (lang == "" and looks_like_xray(body)):
            new_body = migrate_code(body)
        else:
            new_body = body
        return f"{m.group('open')}{m.group('lang')}\n{new_body}{m.group('close')}"

    return FENCE_RE.sub(repl, text)


# Inline code spans `...` that look like xray fragments. Conservative:
# only touch spans containing `=>` AND xray-shaped tokens (so we don't
# rewrite generic prose like `key=>value`).
INLINE_CODE_RE = re.compile(r"`([^`\n]+)`")


def looks_xray_inline(span: str) -> bool:
    return ("=>" in span or "): " in span or "fn(" in span) and (
        "=>" in span or ": " in span
    )


def migrate_inline(text: str) -> str:
    def repl(m: re.Match) -> str:
        body = m.group(1)
        if looks_xray_inline(body):
            return "`" + migrate_code(body) + "`"
        return m.group(0)

    return INLINE_CODE_RE.sub(repl, text)


def migrate_file(path: pathlib.Path) -> bool:
    text = path.read_text(encoding="utf-8")
    new_text = migrate_fenced(text)
    new_text = migrate_inline(new_text)
    if new_text == text:
        return False
    path.write_text(new_text, encoding="utf-8")
    return True


def main():
    if len(sys.argv) < 2:
        print("Usage: migrate_arrow_md.py <file.md> ...", file=sys.stderr)
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

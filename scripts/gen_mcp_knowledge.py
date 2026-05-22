#!/usr/bin/env python3
"""Regenerate src/app/mcp/xmcp_knowledge_generated.c from docs/knowledge/.

Reads docs/knowledge/{topics,stdlib,resources}/*.md and emits a single
C source file consumed by xmcp_knowledge.c at build time. The output is
committed so the build does not depend on Python; CI re-runs this script
and fails when the working tree is dirty.

Frontmatter format (between two lines of `---`):

    topics/<id>.md       id, title, aliases (list)
    stdlib/<module>.md   module, summary
    resources/<id>.md    id  (one of cheatsheet | concurrency | stdlib_list)

The body is the rest of the file, embedded verbatim as a multi-line C
string literal. Emission is deterministic — entries are sorted by id /
module before output so review diffs reflect only real changes.

Optional --builtins points to the JSON produced by `xray builtin-dump`;
when present, per-symbol metadata (signature, summary) is merged into
the matching stdlib entry. Without --builtins the symbol arrays are
empty and only the human-authored prose round-trips.

Usage:
    python3 scripts/gen_mcp_knowledge.py \\
        --docs   docs/knowledge \\
        --out    src/app/mcp/xmcp_knowledge_generated.c \\
        [--builtins build/builtin_dump.json]
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path
from typing import Any


# ---------------------------------------------------------------------------
# Frontmatter / body parsing
# ---------------------------------------------------------------------------

_FRONT_RE = re.compile(r"\A---\s*\n(.*?)\n---\s*\n", re.DOTALL)


def parse_frontmatter(text: str) -> tuple[dict[str, Any], str]:
    """Strip the leading `--- ... ---` block and return (meta, body).

    Supported value forms:
        key: scalar              -> str
        key: "scalar"            -> str
        key: [a, b, c]           -> list[str]   (single-line only)
        key: 'scalar with : col' -> str
    Comments (`#`) and blank lines are ignored.
    """
    m = _FRONT_RE.match(text)
    if not m:
        raise ValueError("missing frontmatter block (expected leading `---`)")
    meta: dict[str, Any] = {}
    for raw in m.group(1).splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if ":" not in line:
            raise ValueError(f"frontmatter line missing `:` -> {raw!r}")
        key, _, value = line.partition(":")
        key = key.strip()
        value = value.strip()
        if value.startswith("[") and value.endswith("]"):
            inner = value[1:-1].strip()
            items = [
                item.strip().strip("\"'")
                for item in inner.split(",")
                if item.strip()
            ]
            meta[key] = items
        else:
            meta[key] = value.strip("\"'")
    return meta, text[m.end():]


# ---------------------------------------------------------------------------
# C string emission
# ---------------------------------------------------------------------------

def c_escape(s: str) -> str:
    """Escape a single text line for inclusion in a C string literal."""
    out: list[str] = []
    for ch in s:
        code = ord(ch)
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            # Should never occur — caller splits on newline first.
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif ch == "\r":
            continue  # discard CR; we only honour LF endings
        elif 0x20 <= code < 0x7F:
            out.append(ch)
        elif code < 0x20:
            out.append(f"\\x{code:02x}")
        else:
            # UTF-8 encode and emit each byte as \xHH so source stays ASCII.
            for byte in ch.encode("utf-8"):
                out.append(f"\\x{byte:02x}")
    return "".join(out)


def to_c_string_block(s: str, indent: str = "        ") -> str:
    """Emit `s` as a sequence of `"line\\n"` literals, one per source line.

    Adjacent string literals concatenate in C, so the result behaves as a
    single string but stays diff-friendly: each Markdown line maps to one
    C source line.
    """
    if s == "":
        return '""'
    lines = s.split("\n")
    pieces: list[str] = []
    for i, line in enumerate(lines):
        suffix = "\\n" if i < len(lines) - 1 else ""
        pieces.append(f'"{c_escape(line)}{suffix}"')
    return ("\n" + indent).join(pieces)


# ---------------------------------------------------------------------------
# Loaders — one per docs subdirectory
# ---------------------------------------------------------------------------

def _read(path: Path) -> tuple[dict[str, Any], str]:
    text = path.read_text(encoding="utf-8")
    try:
        return parse_frontmatter(text)
    except ValueError as e:
        raise ValueError(f"{path}: {e}") from None


def load_topics(docs: Path) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    topics_dir = docs / "topics"
    for path in sorted(topics_dir.glob("*.md")):
        meta, body = _read(path)
        if "id" not in meta:
            raise ValueError(f"{path}: missing `id`")
        if path.stem != meta["id"]:
            raise ValueError(
                f"{path}: filename stem {path.stem!r} does not match id {meta['id']!r}"
            )
        out.append({
            "id": meta["id"],
            "title": meta.get("title", meta["id"]),
            "aliases": meta.get("aliases", []) or [],
            "body": body.rstrip("\n") + "\n",
        })
    out.sort(key=lambda t: t["id"])
    return out


def load_stdlib(docs: Path, builtins_index: dict[str, list[dict[str, str]]]) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    stdlib_dir = docs / "stdlib"
    for path in sorted(stdlib_dir.glob("*.md")):
        meta, body = _read(path)
        if "module" not in meta:
            raise ValueError(f"{path}: missing `module`")
        module = meta["module"]
        if path.stem != module:
            raise ValueError(
                f"{path}: filename stem {path.stem!r} does not match module {module!r}"
            )
        out.append({
            "module": module,
            "summary": meta.get("summary", ""),
            "body": body.rstrip("\n") + "\n",
            "symbols": builtins_index.get(module, []),
        })
    out.sort(key=lambda m: m["module"])
    return out


def load_resources(docs: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    res_dir = docs / "resources"
    for path in sorted(res_dir.glob("*.md")):
        meta, body = _read(path)
        if "id" not in meta:
            raise ValueError(f"{path}: missing `id`")
        if path.stem != meta["id"]:
            raise ValueError(
                f"{path}: filename stem {path.stem!r} does not match id {meta['id']!r}"
            )
        out[meta["id"]] = body.rstrip("\n") + "\n"
    return out


def load_builtins(path: Path | None) -> dict[str, list[dict[str, str]]]:
    """Read xray builtin-dump JSON, return {module: [{name, signature, summary}, ...]}."""
    if path is None or not path.exists():
        return {}
    data = json.loads(path.read_text(encoding="utf-8"))
    index: dict[str, list[dict[str, str]]] = {}
    # Accepted shapes:
    #   {"modules": [{"name": "json", "symbols": [{...}, ...]}, ...]}
    #   {"json": [{...}, ...]}
    if isinstance(data, dict) and "modules" in data:
        for mod in data["modules"]:
            module = mod["name"]
            symbols = mod.get("symbols", [])
            index[module] = [
                {
                    "name": s.get("name", ""),
                    "signature": s.get("signature", ""),
                    "summary": s.get("summary", ""),
                }
                for s in symbols
            ]
    elif isinstance(data, dict):
        for module, symbols in data.items():
            index[module] = [
                {
                    "name": s.get("name", ""),
                    "signature": s.get("signature", ""),
                    "summary": s.get("summary", ""),
                }
                for s in symbols
            ]
    else:
        raise ValueError(f"{path}: unrecognised builtin-dump shape")
    for module, symbols in index.items():
        symbols.sort(key=lambda s: s["name"])
    return index


# ---------------------------------------------------------------------------
# Emission
# ---------------------------------------------------------------------------

BANNER = (
    "/* @generated by scripts/gen_mcp_knowledge.py — DO NOT EDIT.\n"
    " *\n"
    " * The generator embeds the authoritative MCP knowledge source files\n"
    " * and optional analyzer builtin metadata as static C tables. */\n"
)


def emit_topic(t: dict[str, Any]) -> list[str]:
    aliases_csv = ",".join(t["aliases"])
    return [
        "    {",
        f'        .id = "{c_escape(t["id"])}",',
        f'        .title = "{c_escape(t["title"])}",',
        f'        .aliases_csv = "{c_escape(aliases_csv)}",',
        "        .body =",
        "            " + to_c_string_block(t["body"], indent="            ") + ",",
        "    },",
    ]


def emit_stdlib_symbols_array(module: str, symbols: list[dict[str, str]]) -> list[str]:
    if not symbols:
        return []
    lines = [f"static const XmcpGeneratedStdlibSymbol _symbols_{module}[] = {{"]
    for s in symbols:
        lines.append("    {")
        lines.append(f'        .name = "{c_escape(s["name"])}",')
        lines.append(f'        .signature = "{c_escape(s["signature"])}",')
        lines.append(f'        .summary = "{c_escape(s["summary"])}",')
        lines.append("    },")
    lines.append("};")
    lines.append("")
    return lines


def emit_stdlib_entry(m: dict[str, Any]) -> list[str]:
    if m["symbols"]:
        symbols_ref = f'_symbols_{m["module"]}'
        symbol_count_ref = (
            f'(int)(sizeof({symbols_ref}) / sizeof({symbols_ref}[0]))'
        )
    else:
        symbols_ref = "NULL"
        symbol_count_ref = "0"
    return [
        "    {",
        f'        .module = "{c_escape(m["module"])}",',
        f'        .summary = "{c_escape(m["summary"])}",',
        "        .body =",
        "            " + to_c_string_block(m["body"], indent="            ") + ",",
        f"        .symbols = {symbols_ref},",
        f"        .symbol_count = {symbol_count_ref},",
        "    },",
    ]


def emit_resource(name: str, body: str) -> list[str]:
    return [
        f"XR_DATADEF const char xmcp_generated_{name}[] =",
        "    " + to_c_string_block(body, indent="    ") + ";",
        "",
    ]


def render(
    topics: list[dict[str, Any]],
    stdlib: list[dict[str, Any]],
    resources: dict[str, str],
) -> str:
    lines: list[str] = []
    lines.append(BANNER)
    lines.append('#include "xmcp_knowledge_generated.h"')
    lines.append("")
    lines.append("#include <stddef.h> /* NULL */")
    lines.append("")
    lines.append("// clang-format off")
    lines.append("")

    # Per-module symbol tables first so their addresses are visible below.
    for m in stdlib:
        lines.extend(emit_stdlib_symbols_array(m["module"], m["symbols"]))

    # Topics
    lines.append("XR_DATADEF const XmcpGeneratedTopic xmcp_generated_topics[] = {")
    if topics:
        for t in topics:
            lines.extend(emit_topic(t))
    else:
        # C requires at least one initialiser; sentinel is never visited
        # because xmcp_generated_topic_count == 0.
        lines.append("    {0}")
    lines.append("};")
    lines.append("XR_DATADEF const int xmcp_generated_topic_count = " f"{len(topics)};")
    lines.append("")

    # Stdlib
    lines.append("XR_DATADEF const XmcpGeneratedStdlibEntry xmcp_generated_stdlib[] = {")
    if stdlib:
        for m in stdlib:
            lines.extend(emit_stdlib_entry(m))
    else:
        lines.append("    {0}")
    lines.append("};")
    lines.append("XR_DATADEF const int xmcp_generated_stdlib_count = " f"{len(stdlib)};")
    lines.append("")

    # Resources (always emit all three, default empty so consumer never NULL-derefs).
    for rid in ("cheatsheet", "concurrency", "stdlib_list"):
        body = resources.get(rid, "")
        lines.extend(emit_resource(rid, body))

    lines.append("// clang-format on")
    return "\n".join(lines).rstrip() + "\n"


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0] if __doc__ else "")
    p.add_argument("--docs", required=True, type=Path)
    p.add_argument("--out", required=True, type=Path)
    p.add_argument("--builtins", type=Path, default=None)
    p.add_argument(
        "--check",
        action="store_true",
        help="Exit non-zero if --out would change (CI staleness gate).",
    )
    args = p.parse_args(argv)

    if not args.docs.is_dir():
        print(f"error: {args.docs}: not a directory", file=sys.stderr)
        return 2

    builtins_index = load_builtins(args.builtins)
    topics = load_topics(args.docs)
    stdlib = load_stdlib(args.docs, builtins_index)
    resources = load_resources(args.docs)

    rendered = render(topics, stdlib, resources)

    if args.check:
        if not args.out.exists():
            print(f"error: {args.out}: missing; run without --check to create", file=sys.stderr)
            return 1
        existing = args.out.read_text(encoding="utf-8")
        if existing != rendered:
            builtins_arg = f" --builtins {args.builtins}" if args.builtins else ""
            print(
                f"error: {args.out}: stale; rerun "
                f"`python3 scripts/gen_mcp_knowledge.py --docs {args.docs}"
                f"{builtins_arg} --out {args.out}`",
                file=sys.stderr,
            )
            return 1
        return 0

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(rendered, encoding="utf-8")
    print(
        f"wrote {args.out} "
        f"({len(topics)} topics, {len(stdlib)} stdlib modules, "
        f"{sum(len(m['symbols']) for m in stdlib)} symbols, "
        f"{len(resources)} resources)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

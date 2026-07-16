#!/usr/bin/env python3
"""Keep task 199 P3 internal until scoped grapheme provenance is available."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]


PUBLIC_SURFACE_FORBIDDEN = [
    (
        "stdlib/types/string.xr",
        "graphemes",
        "stdlib/types/string.xr publishes graphemes before provenance integration",
    ),
    (
        "src/frontend/analyzer/xnative_type_defs.inc.c",
        "graphemes",
        "native type snapshot publishes graphemes before provenance integration",
    ),
    (
        "src/runtime/object/xstring_methods.c",
        '"graphemes"',
        "xstring_methods.c registers public graphemes before provenance integration",
    ),
    (
        "src/aot/xrt_method.h",
        "graphemes",
        "xrt_method.h exposes graphemes before direct-loop lowering",
    ),
    (
        "src/app/lsp/xlsp_stdlib_generated.inc",
        "graphemes",
        "LSP stdlib surface exposes graphemes before provenance integration",
    ),
    (
        "src/app/mcp/xmcp_knowledge_generated.c",
        "graphemes",
        "MCP knowledge surface exposes graphemes before provenance integration",
    ),
]

GENERIC_ITERATOR_FORBIDDEN = [
    "src/runtime/object/xiterator.h",
    "src/runtime/object/xiterator.c",
]


def main() -> int:
    failures = []

    for relpath, needle, message in PUBLIC_SURFACE_FORBIDDEN:
        text = (ROOT / relpath).read_text(encoding="utf-8")
        if needle in text:
            failures.append(message)

    generic_iterator_leaked = any(
        "grapheme" in (ROOT / relpath).read_text(encoding="utf-8")
        for relpath in GENERIC_ITERATOR_FORBIDDEN
    )
    if generic_iterator_leaked:
        failures.append("generic XrIterator contains grapheme fallback instead of stack cursor")

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}")
        return 1
    print("OK: task 199 grapheme iterator remains VM-internal")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

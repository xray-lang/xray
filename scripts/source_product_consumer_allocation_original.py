#!/usr/bin/env python3
"""Verify that all eight original allocation inputs remain byte-exact."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re

LEGACY_SHA256 = "6fa8b7fb17e51d584098709ac6ce01394172dfa0915e4c516bcdd7a0a29c7d62"
HEADER = '/*\n * xray - Lightweight typed scripting with native concurrency\n * https://www.xray-lang.org\n * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>\n * Licensed under the MIT License\n *\n * allocation_original_sources.h - Exact legacy allocation source bodies\n *\n * KEY CONCEPT:\n *   Complete original bytes are the preflight oracle; no wrapper is inserted.\n */'
TOKEN = re.compile(r'"(?:\\.|[^"\\])*"|,')


def verify(root: Path) -> None:
    legacy = (root / "tests/unit/program/test_xr_program_source_allocations.c").read_bytes()
    if hashlib.sha256(legacy).hexdigest() != LEGACY_SHA256:
        raise ValueError("original allocation consumer changed; review its complete responsibility set")
    body = legacy.decode("utf-8").split("static const char *const sources[] = {", 1)[1].split("\n    };", 1)[0]
    if TOKEN.sub("", body).strip():
        raise ValueError("unexpected syntax in original source array")
    sources, parts = [], []
    for token in TOKEN.findall(body):
        if token == ",":
            if not parts:
                raise ValueError("empty original source")
            sources.append("".join(parts)); parts = []
        else:
            parts.append(json.loads(token))
    if parts or len(sources) != 8:
        raise ValueError("original eight-source denominator changed")
    directory = root / "tests/unit/xir/product_consumers"
    header = [HEADER,
              "static const char *const allocation_original_sources[] = {"]
    for index, source in enumerate(sources):
        fixture = directory / f"allocation_original/scenario{index}/root.xr"
        if fixture.read_bytes() != source.encode("utf-8"):
            raise ValueError(f"scenario {index} no longer preserves the complete original source")
        header.append("    " + json.dumps(source) + ",")
    header.append("};\n")
    if (directory / "allocation_original_sources.h").read_text(encoding="utf-8") != "\n".join(header):
        raise ValueError("compiled source preflight differs from the exact original sources")
    print("allocation-original source census: 8 exact fixtures and compiled preflight; retired=0")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    verify(args.root)


if __name__ == "__main__":
    main()

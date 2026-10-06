#!/usr/bin/env python3
"""Preserve the complete deep class input and compare its original C constructor."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import uuid

DEEP_TEST = "source_owner_deep_class_chain_has_no_nominal_recursion_limit"


def construct_deep_source(body, decode, string_pattern):
    constructor = body[body.index("{") + 1:body.index("    SourceBuildFixture fixture;")]
    constants = {name: int(value) for name, value in re.findall(
        r'\b(CLASS_COUNT|SOURCE_CAPACITY)\s*=\s*(\d+)', constructor)}
    if constants != {"CLASS_COUNT": 80, "SOURCE_CAPACITY": 32768}:
        raise ValueError("the original eighty classes and source capacity must be retained")
    if not re.search(r'for\s*\(int32_t index = CLASS_COUNT - 1; index >= 0; --index\)', constructor):
        raise ValueError("the original descending class constructor is missing")
    literals = re.findall(r'snprintf\(\s*source\s*\+\s*used,\s*SOURCE_CAPACITY\s*-\s*used,\s*'
        r'((?:' + string_pattern + r'\s*)+)(?:,|\))', constructor)
    formats = [b"".join(decode(literal) for literal in re.findall(string_pattern, group)).decode("ascii")
        for group in literals]
    if len(formats) != 3 or [text.count("%d") for text in formats] != [1, 3, 0]:
        raise ValueError("the original class and answer format strings differ")
    if any("%" in re.sub(r'%d', '', text) for text in formats):
        raise ValueError("unexpected original class source formatting")
    source = []
    for index in range(constants["CLASS_COUNT"] - 1, -1, -1):
        source.append(formats[0] % index if index == constants["CLASS_COUNT"] - 1 else
            formats[1] % (index, index + 1, index + 1))
    source.append(formats[2])
    data = "".join(source).encode("ascii")
    names = [int(value) for value in re.findall(rb'^class C(\d+) \{', data, re.M)]
    if names != list(range(79, -1, -1)) or len(data) + 1 > constants["SOURCE_CAPACITY"]:
        raise ValueError("the original complete class order or bounded source buffer differs")
    if data.count(b"  next: C") != 79 or data.count(b"  value: i64\n") != 1:
        raise ValueError("the original nullable chain and terminal field differ")
    metadata = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
        "constructor_lf_sha256": hashlib.sha256(constructor.encode()).hexdigest(),
        "classes": 80, "nullable_links": 79, "terminal_i64_fields": 1,
        "declaration_order": names, "original_buffer_capacity": constants["SOURCE_CAPACITY"]}
    return data, metadata, constructor


def constructor_oracle(constructor):
    header = ("/*\n * xray - Lightweight typed scripting with native concurrency\n"
        " * https://www.xray-lang.org\n *\n"
        " * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>\n"
        " * Licensed under the MIT License\n *\n"
        " * source_product_nominal_constructors.c - Original deep class source producer\n *\n"
        " * KEY CONCEPT:\n *   The exact original C loop independently produces all eighty class declarations.\n */\n"
        '#include "base/xmalloc.h"\n#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n'
        '#define ASSERT_TRUE(c) do { if (!(c)) { fprintf(stderr, "%d: %s\\n", __LINE__, #c); exit(1); } } while (0)\n'
        '#define ASSERT_NOT_NULL(c) ASSERT_TRUE((c) != NULL)\n')
    code = header + "static void emit_deep_class_graph(FILE *output) {\n" + constructor
    code += "    ASSERT_TRUE(fwrite(source, 1, strlen(source), output) == strlen(source));\n    xr_free(source);\n}\n"
    code += ('int main(int argc, char **argv) {\n    ASSERT_TRUE(argc == 3);\n'
        '    ASSERT_TRUE(!strcmp(argv[1], "deep_class_graph"));\n'
        '    FILE *output = fopen(argv[2], "wb");\n    ASSERT_NOT_NULL(output);\n'
        '    emit_deep_class_graph(output);\n    ASSERT_TRUE(!fclose(output));\n    return 0;\n}\n')
    return code.encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    spec = importlib.util.spec_from_file_location("inventory", root / "scripts/source_product_consumer_inventory.py")
    inventory = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(inventory)
    census, files = inventory.inventory()
    binary = args.binary.resolve(strict=True)
    binary_sha = hashlib.sha256(binary.read_bytes()).hexdigest()
    scratch = args.scratch.resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    if args.scratch.is_symlink() or getattr(os.lstat(args.scratch), "st_file_attributes", 0) & 0x400:
        raise ValueError("source constructor scratch must not be a link")
    evidence = scratch / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + uuid.uuid4().hex[:8])
    evidence.mkdir(exist_ok=False)
    original = root / "tests/unit/xir/product_consumers/fixtures/deep_class_graph/root.xr"
    expected = files[original]
    if original.read_bytes() != expected:
        raise ValueError("tracked original deep class fixture is stale")
    output = evidence / "deep_class_graph.xr"
    command = [str(binary), "deep_class_graph", str(output)]
    with (evidence / "constructor.log").open("wb") as stream:
        process = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT)
        code = process.wait(timeout=90)
    if code or output.read_bytes() != expected or hashlib.sha256(binary.read_bytes()).hexdigest() != binary_sha:
        raise ValueError("native original constructor differs from the complete Python fixture")
    row = next(row for row in census["source"] if row["name"] == DEEP_TEST)
    result = {"status": "PASS", "binary_sha256": binary_sha, "pid": process.pid,
        "argv": command, "returncode": code, "bytes": len(expected),
        "sha256": hashlib.sha256(expected).hexdigest(), "body_lf_sha256": row["body_lf_sha256"],
        "original_source_constructor": row["initial_projection"]["original_source_constructor"],
        "evidence": str(evidence),
        "scope": "Exact original source construction; descriptor and failure qualification are separate."}
    (evidence / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result))


if __name__ == "__main__":
    main()

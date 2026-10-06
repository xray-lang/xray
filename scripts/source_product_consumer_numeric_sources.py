#!/usr/bin/env python3
"""Reconstruct full integer inputs and compare the original C constructors."""
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

MATRICES = {
    "source_owner_integer_wrapping_arithmetic_uses_shared_primitives": "integer_arithmetic",
    "source_owner_integer_comparisons_preserve_signedness_and_order": "integer_comparisons",
    "source_owner_integer_division_and_remainder_preserve_value_rules": "integer_division",
}


def construct_source(body, case, decode, strings, block_end, string_pattern):
    if case not in MATRICES.values():
        raise ValueError("unknown original integer matrix")
    constructor = body[body.index("{") + 1:body.index("    SourceBuildFixture fixture;")]
    table_name = "bounds" if case == "integer_comparisons" else "cases"
    table = re.search(r'\}\s+' + table_name + r'\[\]\s*=\s*\{', body)
    if not table:
        raise ValueError("original integer table is missing")
    start = body.index("{", table.start())
    end = block_end(body, start)
    groups = re.findall(r'\{\s*((?:' + string_pattern + r'\s*,?\s*)+)\}', body[start:end])
    bounds = [[decode(literal).decode("ascii") for literal in re.findall(string_pattern, group)]
              for group in groups]
    if len(bounds) != 8 or {row[0] for row in bounds} != {"i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64"}:
        raise ValueError("the original eight integer widths must be retained")
    formats = re.findall(r'snprintf\(\s*source\s*\+\s*used,\s*sizeof\(source\)\s*-\s*used,\s*((?:' + string_pattern + r'\s*)+),', body)
    formats = [b"".join(decode(literal) for literal in re.findall(string_pattern, item)).decode("ascii")
               for item in formats]
    if len(formats) != 2 or any("%" in re.sub(r'%%|%s|%u', '', text) for text in formats):
        raise ValueError("unexpected original matrix formatting")
    literals = strings(body)
    source = []
    if case == "integer_comparisons":
        predicate_block = re.search(r'predicates\[\]\s*=\s*\{(.*?)\n    \};', body, re.S)
        if not predicate_block:
            raise ValueError("original comparison predicates are missing")
        predicates = re.findall(r'\{\s*(' + string_pattern + r'),\s*\{(true|false),\s*(true|false),\s*(true|false)\}\s*\}', predicate_block[1])
        if len(predicates) != 6 or {decode(row[0]).decode("ascii") for row in predicates} != {"==", "!=", "<", "<=", ">", ">="}:
            raise ValueError("all six original comparison predicates must be retained")
        for type_name, _, _ in bounds:
            for predicate, (symbol, *_) in enumerate(predicates):
                source.append(formats[0] % (predicate, type_name, type_name, type_name, decode(symbol).decode("ascii")))
        source.append(literals["prefix"].decode("ascii"))
        for type_index, (type_name, low, high) in enumerate(bounds):
            for predicate, (_, *expected) in enumerate(predicates):
                for order in range(3):
                    left, right = (low if order == 0 else high), (low if order == 1 else high)
                    source.append(formats[1] % ("!" if expected[order] == "true" else "", predicate,
                        type_name, left, right, 1 + type_index * 18 + predicate * 3 + order))
        checks, functions = 148, 51
    else:
        for row in bounds:
            source.append(formats[0] % ((row[0],) * (12 if case == "integer_arithmetic" else 8)))
        source.append(literals["prefix"].decode("ascii"))
        for index, row in enumerate(bounds):
            if case == "integer_arithmetic":
                type_name, minimum, maximum, added, subtracted, multiplied = row
                arguments = (type_name, maximum, added, index * 3 + 1,
                    type_name, minimum, subtracted, index * 3 + 2,
                    type_name, maximum, multiplied, index * 3 + 3)
            else:
                type_name, left, right, quotient, remainder = row
                arguments = (type_name, left, right, quotient, index * 2 + 1,
                    type_name, left, right, remainder, index * 2 + 2)
            source.append(formats[1] % arguments)
        checks, functions = (25, 26) if case == "integer_arithmetic" else (20, 17)
    source.append(literals["suffix"].decode("ascii"))
    data = "".join(source).encode("ascii")
    failures = [int(value) for value in re.findall(rb'\{ return (\d+) \}', data)]
    capacity = int(re.search(r'char source\[(\d+)\];', body)[1])
    if failures != list(range(1, checks + 1)) or len(re.findall(rb'^fn ', data, re.M)) != functions:
        raise ValueError("the complete original numeric check matrix differs")
    if len(data) + 1 > capacity:
        raise ValueError("the original bounded source buffer is too small")
    metadata = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
        "constructor_lf_sha256": hashlib.sha256(constructor.encode()).hexdigest(),
        "types": [row[0] for row in bounds], "functions": functions, "checks": checks,
        "failure_codes": failures, "original_buffer_capacity": capacity}
    return data, metadata, constructor


def constructor_oracle(constructors):
    header = ("/*\n * xray - Lightweight typed scripting with native concurrency\n"
        " * https://www.xray-lang.org\n *\n"
        " * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>\n"
        " * Licensed under the MIT License\n *\n"
        " * source_product_integer_constructors.c - Original integer source producers\n *\n"
        " * KEY CONCEPT:\n *   Exact original C loops independently produce fixture bytes without retired runtime APIs.\n */\n"
        "#include <stdbool.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
        '#define ASSERT_TRUE(c) do { if (!(c)) { fprintf(stderr, "%d: %s\\n", __LINE__, #c); exit(1); } } while (0)\n')
    code = [header]
    if set(constructors) != set(MATRICES.values()):
        raise ValueError("all original integer constructors are required")
    for name, constructor in constructors.items():
        code.append("static void emit_" + name + "(FILE *output) {\n" + constructor +
            "    ASSERT_TRUE(fwrite(source, 1, strlen(source), output) == strlen(source));\n}\n")
    code.append('int main(int argc, char **argv) {\n    ASSERT_TRUE(argc == 3);\n'
        '    FILE *output = fopen(argv[2], "wb");\n    ASSERT_TRUE(output);\n')
    for index, name in enumerate(constructors):
        code.append(('    if' if index == 0 else '    else if') + ' (!strcmp(argv[1], "' + name + '")) emit_' + name + '(output);\n')
    code.append('    else ASSERT_TRUE(false);\n    ASSERT_TRUE(!fclose(output));\n    return 0;\n}\n')
    return "".join(code).encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    inventory_spec = importlib.util.spec_from_file_location("inventory", root / "scripts/source_product_consumer_inventory.py")
    inventory = importlib.util.module_from_spec(inventory_spec)
    inventory_spec.loader.exec_module(inventory)
    census, files = inventory.inventory()
    binary = args.binary.resolve(strict=True)
    binary_sha = hashlib.sha256(binary.read_bytes()).hexdigest()
    scratch = args.scratch.resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    if args.scratch.is_symlink() or getattr(os.lstat(args.scratch), "st_file_attributes", 0) & 0x400:
        raise ValueError("source constructor scratch must not be a link")
    evidence = scratch / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + uuid.uuid4().hex[:8])
    evidence.mkdir(exist_ok=False)
    rows = []
    for name, case in MATRICES.items():
        original = root / "tests/unit/xir/product_consumers/fixtures" / case / "original.xr"
        expected = files[original]
        if original.read_bytes() != expected:
            raise ValueError("tracked original numeric fixture is stale")
        output = evidence / (case + ".xr")
        command = [str(binary), case, str(output)]
        with (evidence / (case + ".log")).open("wb") as stream:
            process = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT)
            code = process.wait(timeout=90)
        if code or output.read_bytes() != expected or hashlib.sha256(binary.read_bytes()).hexdigest() != binary_sha:
            raise ValueError("native original constructor differs from the complete Python fixture")
        row = next(row for row in census["source"] if row["name"] == name)
        rows.append({"case": case, "pid": process.pid, "argv": command, "returncode": code,
            "bytes": len(expected), "sha256": hashlib.sha256(expected).hexdigest(),
            "checks": row["initial_projection"]["original_source_constructor"]["checks"]})
    result = {"status": "PASS", "binary_sha256": binary_sha, "rows": rows,
        "original_checks": sum(row["checks"] for row in rows), "evidence": str(evidence),
        "scope": "Original source construction; executable Xray and failure qualification are separate."}
    (evidence / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Check exact initializer-failure inputs without substituting runtime oracles."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re

import source_product_consumer_inventory as inventory

CASES = {
    "error": "source_owner_module_initializer_error_cleans_owned_state",
    "panic": "source_owner_module_initializer_panic_cleans_owned_state",
    "panic_message": "source_owner_module_initializer_panic_message_retains_failed_instance",
}


def original_inputs(root: Path) -> tuple[dict, str]:
    text = (root / "tests/unit/program/test_xr_program_source_build.c").read_text(encoding="utf-8")
    signature = "static void assert_module_initializer_failure(bool panic, XrSourceFixtureId fixture_id) {"
    if text.count(signature) != 1:
        raise ValueError("original initializer constructor is missing or duplicated")
    start = text.index(signature)
    body = text[start:inventory.function_end(text, text.index("{", start))]
    match = re.search(r"snprintf\(library, sizeof\(library\),\s*((?:" + inventory.STRING + r"\s*)+),", body)
    if match is None:
        raise ValueError("original library format changed")
    template = b"".join(inventory.c_literal(s) for s in re.findall(inventory.STRING, match[1]))
    if template.count(b"%s") != 1 or template.count(b"%") != 1:
        raise ValueError("library constructor is no longer a single string substitution")
    choices = re.search(r"has_message\s*\?\s*(" + inventory.STRING + r")\s*:\s*panic\s*\?\s*(" + inventory.STRING + r")\s*:\s*(" + inventory.STRING + r")", body)
    if choices is None:
        raise ValueError("original error/panic selection changed")
    replacements = dict(zip(("panic_message", "panic", "error"), map(inventory.c_literal, choices.groups())))
    entry = re.search(r"source_build_fixture_init\(\s*&fixture,\s*(" + inventory.STRING + r"),\s*library\)", body)
    if entry is None:
        raise ValueError("original entry module changed")
    sources = {case: {"root.xr": inventory.c_literal(entry[1]),
                      "library.xr": template.replace(b"%s", replacements[case])} for case in CASES}
    if any(len(files["library.xr"]) >= 768 for files in sources.values()):
        raise ValueError("original snprintf capacity would be exceeded")
    return sources, hashlib.sha256(body.encode("utf-8")).hexdigest()


def verify(root: Path) -> None:
    directory = root / "tests/unit/xir/product_consumers"
    mapping = json.loads((directory / "source_module_initializer_admission_obligations.json").read_text(encoding="utf-8"))
    sources, helper = original_inputs(root)
    if helper != mapping["original_helper_body_lf_sha256"]:
        raise ValueError("original lifecycle/failure assertions changed")
    census = json.loads((directory / "legacy_responsibilities.json").read_text(encoding="utf-8"))
    rows = {row["name"]: row for row in census["source"]}
    legacy = (root / "tests/unit/program/test_xr_program_source_build.c").read_text(encoding="utf-8")
    for case, name in CASES.items():
        row = mapping["originals"][case]
        if row["legacy_name"] != name or row["wrapper_body_lf_sha256"] != rows[name]["body_lf_sha256"]:
            raise ValueError(f"original wrapper differs: {case}")
        wrappers = list(re.finditer(r"^TEST\(" + re.escape(name) + r"\)\s*\{", legacy, re.M))
        if len(wrappers) != 1:
            raise ValueError(f"original wrapper is missing or duplicated: {case}")
        start = wrappers[0].start()
        body = legacy[start:inventory.function_end(legacy, legacy.index("{", start))]
        if hashlib.sha256(body.encode("utf-8")).hexdigest() != row["wrapper_body_lf_sha256"]:
            raise ValueError(f"original wrapper body changed: {case}")
        if row["status"] != "OPEN" or row["replacement_gates"]:
            raise ValueError("source admission cannot retire runtime failure obligations")
        actual = directory / "module_initializer_admission" / case
        if {p.name for p in actual.glob("*.xr")} != set(sources[case]):
            raise ValueError(f"original module graph changed: {case}")
        for name, data in sources[case].items():
            if (actual / name).read_bytes() != data:
                raise ValueError(f"original input changed: {case}/{name}")
            if row["files"][name] != hashlib.sha256(data).hexdigest():
                raise ValueError(f"input mapping differs: {case}/{name}")
    print("initializer failure inputs: 3 original responsibilities, 6 exact files, no adapters; runtime obligations OPEN, retired=0")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    verify(args.root)


if __name__ == "__main__":
    main()

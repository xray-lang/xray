#!/usr/bin/env python3
"""Preserve original request-budget obligations separately from admission probes."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re

import source_product_consumer_inventory as inventory

CASES = {
    "module_count": "source_owner_rejects_module_budget_before_analysis",
    "invalid_request": "source_owner_rejects_invalid_or_expanded_budget_request",
    "mono_depth": "source_owner_reports_exact_monomorphization_depth_budget",
    "mono_instances": "source_owner_reports_exact_monomorphization_instance_budget",
    "graph_instances": "source_owner_applies_instance_budget_across_module_graph",
    "program_bytes": "source_owner_rejects_program_bytes_over_request_budget",
}
ADAPTER = b"export fn consumerAnswer() -> i64 { return answer() }\n"


def inputs(root: Path) -> dict:
    source = root / "tests/unit/program/test_xr_program_source_build.c"
    text = source.read_text(encoding="utf-8")
    census = json.loads((root / "tests/unit/xir/product_consumers/legacy_responsibilities.json").read_text(encoding="utf-8"))
    rows = {row["name"]: row for row in census["source"]}
    result = {}
    for case, name in CASES.items():
        matches = list(re.finditer(r"^TEST\(" + re.escape(name) + r"\)\s*\{", text, re.M))
        if len(matches) != 1:
            raise ValueError(f"original responsibility is missing or duplicated: {name}")
        match = matches[0]
        body = text[match.start():inventory.function_end(text, text.index("{", match.start()))]
        digest = hashlib.sha256(body.encode("utf-8")).hexdigest()
        if digest != rows[name]["body_lf_sha256"] or rows[name]["original_fixture"] is not None:
            raise ValueError(f"original nonfixture responsibility changed: {name}")
        literals = inventory.static_strings(body)
        data = literals.get("source", literals.get("entry_source"))
        if data is None:
            raise ValueError(f"original source missing: {name}")
        result[case] = {"name": name, "body_lf_sha256": digest,
                        "files": {"root.xr": data + ADAPTER}, "original": data}
        if "library_source" in literals:
            result[case]["files"]["library.xr"] = literals["library_source"]
    return result


def verify(root: Path) -> None:
    directory = root / "tests/unit/xir/product_consumers"
    mapping = json.loads((directory / "source_request_budget_obligations.json").read_text(encoding="utf-8"))
    expected = inputs(root)
    if set(mapping["responsibilities"]) != set(expected):
        raise ValueError("six original budget responsibilities changed")
    for case, value in expected.items():
        row = mapping["responsibilities"][case]
        if row["legacy_name"] != value["name"] or row["body_lf_sha256"] != value["body_lf_sha256"]:
            raise ValueError(f"original body identity changed: {case}")
        if row["status"] != "OPEN" or row["replacement_gates"]:
            raise ValueError(f"normal probes cannot retire the request-budget responsibility: {case}")
        fixture_dir = directory / "request_budget_inputs" / case
        if {p.name for p in fixture_dir.glob("*.xr")} != set(value["files"]):
            raise ValueError(f"original module graph changed: {case}")
        for name, data in value["files"].items():
            if (fixture_dir / name).read_bytes() != data:
                raise ValueError(f"original bytes or sole admission adapter changed: {case}/{name}")
            if row["probe_files"][name] != hashlib.sha256(data).hexdigest():
                raise ValueError(f"fixture mapping differs: {case}/{name}")
    print("request-budget census: 6 original bodies, 8 exact source files with root adapters; budget gates OPEN, retired=0")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    verify(args.root)


if __name__ == "__main__":
    main()

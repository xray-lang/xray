#!/usr/bin/env python3
"""Bind every preserved consumer responsibility to exact existing mapping references."""
from __future__ import annotations

import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / "tests/unit/xir/product_consumers"
OUTPUT = DEST / "legacy_responsibility_trace.json"
SPEC = importlib.util.spec_from_file_location(
    "consumer_inventory", ROOT / "scripts/source_product_consumer_inventory.py")
INVENTORY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(INVENTORY)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def json_bytes(value: object) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode()


def documents() -> dict:
    result = {}
    for path in sorted(DEST.glob("*.json")):
        if path.name in (OUTPUT.name, "legacy_responsibilities.json"):
            continue
        data = path.read_bytes().replace(b"\r\n", b"\n")
        result[path.relative_to(ROOT).as_posix()] = (digest(data), json.loads(data))
    return result


def nodes(value: object, pointer: str = ""):
    """Yield dictionaries at exact JSON pointers without interpreting their prose."""
    if isinstance(value, dict):
        yield pointer, value
        for key, child in value.items():
            escaped = key.replace("~", "~0").replace("/", "~1")
            yield from nodes(child, pointer + "/" + escaped)
    elif isinstance(value, list):
        for index, child in enumerate(value):
            yield from nodes(child, pointer + "/" + str(index))


def require_equal(actual: object, expected: object, description: str) -> None:
    if actual != expected:
        raise ValueError(description)


def trace(census: dict, maps: dict) -> dict:
    counts = (census["source_responsibilities"], census["source_native_fixtures"],
              census["source_non_fixture_responsibilities"], len(census["other_consumer_files"]),
              sum(len(x["functions"]) for x in census["other_consumer_files"].values()),
              census["source_allocation_scenarios"]["count"],
              len(census["shared_emission_owner_consumers"]))
    require_equal(counts, (108, 88, 20, 26, 160, 8, 7), "legacy denominator changed")
    source = census["source"]
    other = [row for file in census["other_consumer_files"].values() for row in file["functions"]]
    require_equal((len(source), len(other)), (108, 160), "legacy rows missing")
    rows = source + other
    require_equal(sum(row["original_fixture"] is not None for row in source), 88,
                  "legacy fixture rows missing")
    scenarios = census["source_allocation_scenarios"]["scenarios"]
    require_equal([row["ordinal"] for row in scenarios], list(range(8)),
                  "legacy allocation scenarios missing or reordered")
    identity = {(row["legacy_path"], row["name"]): row for row in rows}
    by_digest = {row["body_lf_sha256"]: row for row in rows}
    require_equal((len(identity), len(by_digest)), (268, 268), "ambiguous legacy identity")
    for row in rows:
        require_equal((row["status"], row["replacement_gates"]), ("OPEN", []),
                      "inventory retirement requires separate replacement qualification")
    references = {sha: [] for sha in by_digest}
    for path, (_, document) in sorted(maps.items()):
        for pointer, node in nodes(document):
            explicit = {"name", "legacy_path", "body_lf_sha256"} <= node.keys()
            if explicit:
                key = (node["legacy_path"], node["name"])
                if key not in identity or identity[key]["body_lf_sha256"] != node["body_lf_sha256"]:
                    raise ValueError(f"stale named legacy reference: {path}#{pointer}")
            for key, value in node.items():
                if not isinstance(value, str) or not key.lower().endswith("sha256"):
                    continue
                if value in references:
                    field = key.replace("~", "~0").replace("/", "~1")
                    references[value].append({"mapping": path, "pointer": pointer + "/" + field,
                        "binding": "exact_path_name_body" if explicit and key == "body_lf_sha256"
                                   else "exact_body_digest_value_only"})
    def group(items: list) -> list:
        return [{"legacy_path": row["legacy_path"], "name": row["name"],
                 "body_lf_sha256": row["body_lf_sha256"], "status": "OPEN",
                 "inventory_projection": row.get("initial_projection"),
                 "mapping_references": references[row["body_lf_sha256"]]}
                for row in items]
    source_referenced = sum(bool(references[row["body_lf_sha256"]]) for row in source)
    other_referenced = sum(bool(references[row["body_lf_sha256"]]) for row in other)
    return {"schema": 1, "legacy_base_commit": census["base_commit"],
        "inventory_lf_sha256": digest(json_bytes(census)),
        "completion": "OPEN", "retired": 0,
        "reference_policy": "References prove exact body identity only, not assertion coverage, "
            "equivalence, execution, current producer qualification or deletion permission. "
            "No digest reference does not prove that a replacement is absent; "
            "alternative mapping formats and prose are not interpreted.",
        "denominators": {"source": 108, "source_fixtures": 88, "source_non_fixtures": 20,
            "other_files": 26, "other_functions": 160, "allocation_scenarios": 8,
            "shared_emission_owner_consumers": 7},
        "reference_counts": {"source_with_digest_reference": source_referenced,
            "source_without_digest_reference": 108 - source_referenced,
            "other_with_digest_reference": other_referenced,
            "other_without_digest_reference": 160 - other_referenced,
            "digest_reference_values": sum(len(x) for x in references.values()),
            "source_inventory_projections": sum("initial_projection" in row for row in source)},
        "mapping_inputs": [{"path": path, "lf_sha256": sha} for path, (sha, _) in sorted(maps.items())],
        "source": group(source), "other_functions": group(other),
        "other_files": [{"path": path, "lf_sha256": value["lf_sha256"],
                         "status": value["whole_file_responsibility"]}
                        for path, value in census["other_consumer_files"].items()],
        "allocation_scenarios": [{"ordinal": row["ordinal"], "input_bytes": row["input_bytes"],
            "input_sha256": row["input_sha256"], "status": row["status"]}
            for row in census["source_allocation_scenarios"]["scenarios"]],
        "shared_emission_owner_consumers": census["shared_emission_owner_consumers"],
        "shared_emission_owner_policy": census["shared_emission_owner_policy"]}


def self_test(census: dict, maps: dict, expected: dict) -> int:
    """Exercise responsibility loss and forged references on private copies only."""
    cases = []
    missing = copy.deepcopy(census)
    missing["source"].pop()
    cases.append(("missing_source_row", lambda: trace(missing, maps)))
    duplicate = copy.deepcopy(census)
    duplicate["source"][1] = copy.deepcopy(duplicate["source"][0])
    cases.append(("duplicate_identity", lambda: trace(duplicate, maps)))
    retired = copy.deepcopy(census)
    retired["source"][0]["status"] = "RETIRED"
    cases.append(("forged_census_retirement", lambda: trace(retired, maps)))
    wrong_count = copy.deepcopy(census)
    wrong_count["source_allocation_scenarios"]["count"] = 7
    cases.append(("allocation_denominator_loss", lambda: trace(wrong_count, maps)))
    missing_scenario = copy.deepcopy(census)
    missing_scenario["source_allocation_scenarios"]["scenarios"].pop()
    cases.append(("missing_allocation_row", lambda: trace(missing_scenario, maps)))
    anchor = next((path, pointer) for path, (_, document) in maps.items()
                  for pointer, node in nodes(document)
                  if {"name", "legacy_path", "body_lf_sha256"} <= node.keys())
    for field in ("body_lf_sha256", "legacy_path"):
        altered = copy.deepcopy(maps)
        node = next(node for pointer, node in nodes(altered[anchor[0]][1]) if pointer == anchor[1])
        node[field] = "0" * 64 if field == "body_lf_sha256" else "unknown/consumer.c"
        cases.append(("forged_" + field, lambda altered=altered: trace(census, altered)))
    smaller = dict(maps)
    smaller.pop(next(iter(smaller)))
    cases.append(("lost_mapping_input", lambda: require_equal(
        trace(census, smaller), expected, "trace lost a mapping input")))
    forged = copy.deepcopy(expected)
    forged["retired"] = 1
    cases.append(("forged_trace_retirement", lambda: require_equal(
        forged, expected, "trace retirement was forged")))
    for name, operation in cases:
        try:
            operation()
        except ValueError:
            print("trace-self-test " + name + " REJECTED")
        else:
            raise AssertionError("trace accepted " + name)
    return len(cases)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("write", "check", "self-test"))
    args = parser.parse_args()
    census, inputs = INVENTORY.inventory()
    inputs[DEST / "legacy_responsibilities.json"] = json_bytes(census)
    for path, data in inputs.items():
        if not path.is_file() or path.read_bytes() != data:
            raise ValueError("stale legacy inventory input: " + str(path.relative_to(ROOT)))
    maps = documents()
    result = trace(census, maps)
    data = json_bytes(result)
    if args.action == "write":
        if not OUTPUT.is_file() or OUTPUT.read_bytes() != data:
            OUTPUT.write_bytes(data)
    elif not OUTPUT.is_file() or OUTPUT.read_bytes() != data:
        raise ValueError("stale legacy responsibility trace")
    attacks = self_test(census, maps, result) if args.action == "self-test" else 0
    print(json.dumps({"status": "PASS", "kind": "STATIC_TRACE_ONLY", "completion": "OPEN",
        "retired": 0, "denominators": result["denominators"], "mapping_files": len(maps),
        "reference_counts": result["reference_counts"], "private_copy_attacks": attacks}))


if __name__ == "__main__":
    main()

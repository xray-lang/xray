#!/usr/bin/env python3
"""Validate the Task 304 capability denominator and product cutover manifests.

These manifests are governance inputs, not runtime admission schemas.  The
checker fails closed on missing declared capabilities, invented evidence,
unknown profiles/providers/legacy owners, premature closure, or a partial
product cutover claim.
"""

from __future__ import annotations

import argparse
import json
import sys
import tempfile
from copy import deepcopy
from pathlib import Path
from typing import Any, Callable


CONTRACT_DIR = Path("contracts/canonical-program")
CAPABILITY_FILE = "capability-denominator.json"
PRODUCT_FILE = "product-cutover-manifest.json"
MIGRATION_FILE = "legacy-owner-migration.json"

REQUIRED_LAYERS = {
    "source_owner",
    "producer",
    "verifier",
    "independent_oracle",
    "vm",
    "aot",
    "product",
}
LAYER_STATES = {"OPEN", "CURRENT_ROUTE_ONLY", "PARTIAL_CANONICAL", "PASS"}
CAPABILITY_STATES = {"OPEN", "CLOSED"}
QUALIFICATION_STATES = {"OPEN", "PARTIAL", "NOT_QUALIFIED", "QUALIFIED"}
ROUTE_STATES = {"OPEN", "LEGACY_REACHABLE", "CANONICAL_INCOMPLETE", "CANONICAL_ONLY"}
DELETION_STATES = {"REACHABLE", "ZERO"}
OBSERVATION_AXES = {
    "value",
    "stdout",
    "typed-error",
    "panic",
    "cleanup",
    "provider-effect",
    "allowed-trace",
    "diagnostic",
}
REQUIRED_ROUTE_IDS = {
    "cli-run-source",
    "cli-test-source",
    "cli-build-native",
    "cli-emit-c",
    "public-source-embed",
    "runtime-only-program-embed",
}
RETAINED_LEGACY_OWNER_IDS = {"aot-generated-c-verifier"}
AOT_HEADER = Path("src/aot/program/xr_backend_ir.h")
AOT_LOWERING = Path("src/aot/program/xr_backend_ir.c")


class ManifestError(ValueError):
    """Raised for a fail-closed cutover-manifest violation."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ManifestError(message)


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="strict")
    except (OSError, UnicodeError) as exc:
        raise ManifestError(f"cannot read {path}: {exc}") from exc


def load_json(path: Path) -> dict[str, Any]:
    raw = read_text(path)
    try:
        value = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise ManifestError(f"invalid JSON in {path}: {exc}") from exc
    require(isinstance(value, dict), f"{path} must contain an object")
    canonical = json.dumps(value, ensure_ascii=False, indent=2) + "\n"
    require(raw == canonical, f"{path} is not canonical two-space JSON with final newline")
    return value


def require_rows(value: Any, owner: str) -> list[dict[str, Any]]:
    require(isinstance(value, list) and value, f"{owner} must be a non-empty array")
    require(all(isinstance(row, dict) for row in value), f"{owner} contains a non-object row")
    return value


def require_unique(rows: list[dict[str, Any]], field: str, owner: str) -> set[str]:
    values = [row.get(field) for row in rows]
    require(all(isinstance(value, str) and value for value in values),
            f"{owner} has an empty {field}")
    require(len(values) == len(set(values)), f"{owner} has duplicate {field} values")
    return set(values)


def require_anchor_list(root: Path, value: Any, owner: str, *, files_only: bool = False) -> None:
    require(isinstance(value, list) and value, f"{owner} must have evidence anchors")
    for relative in value:
        require(isinstance(relative, str) and relative, f"{owner} has an empty anchor")
        target = root / relative
        require(target.is_file() if files_only else target.exists(),
                f"{owner} anchor does not exist: {relative}")


def validate_qualification_rows(root: Path, rows: Any, owner: str) -> set[str]:
    entries = require_rows(rows, owner)
    identifiers = require_unique(entries, "id", owner)
    for row in entries:
        identifier = row["id"]
        require(isinstance(row.get("kind"), str) and row["kind"],
                f"{owner} {identifier} lacks kind")
        require(row.get("required_for_293") is True,
                f"{owner} {identifier} must be an explicit required denominator")
        require(row.get("status") in QUALIFICATION_STATES,
                f"{owner} {identifier} has invalid status")
        require_anchor_list(root, row.get("evidence_anchors"),
                            f"{owner} {identifier}", files_only=True)
        if row["status"] == "QUALIFIED":
            require(len(row["evidence_anchors"]) >= 2,
                    f"qualified {owner} {identifier} needs replayable evidence")
    return identifiers


def validate_capabilities(root: Path, data: dict[str, Any], legacy_ids: set[str]) -> int:
    require(data.get("schema") == "xray-canonical-capability-denominator/2",
            "capability denominator schema must be v2")
    require(data.get("architecture_task") == 293, "capability architecture task drifted")
    require(set(data.get("required_layers", [])) == REQUIRED_LAYERS,
            "capability required layer set drifted")
    require(set(data.get("layer_states", [])) == LAYER_STATES,
            "capability layer state set drifted")
    require(set(data.get("observation_axes", [])) == OBSERVATION_AXES,
            "capability observation axis set drifted")

    profile_ids = validate_qualification_rows(root, data.get("profiles"), "profiles")
    provider_ids = validate_qualification_rows(root, data.get("providers"), "providers")
    rows = require_rows(data.get("capabilities"), "capabilities")
    require_unique(rows, "id", "capabilities")
    for row in rows:
        identifier = row["id"]
        require_anchor_list(root, row.get("spec_anchors"),
                            f"capability {identifier} spec", files_only=True)
        require_anchor_list(root, row.get("source_test_anchors"),
                            f"capability {identifier} source tests", files_only=True)
        observations = row.get("required_observations")
        require(isinstance(observations, list) and observations,
                f"capability {identifier} lacks observations")
        require(len(observations) == len(set(observations)) and
                set(observations) <= OBSERVATION_AXES,
                f"capability {identifier} has duplicate or unknown observations")
        profiles = row.get("required_profiles")
        providers = row.get("required_providers")
        require(isinstance(profiles, list) and profiles and set(profiles) <= profile_ids,
                f"capability {identifier} has unknown or empty profiles")
        require(isinstance(providers, list) and set(providers) <= provider_ids,
                f"capability {identifier} has unknown providers")
        owners = row.get("legacy_owner_ids")
        require(isinstance(owners, list) and owners and set(owners) <= legacy_ids,
                f"capability {identifier} has unknown or empty legacy owners")
        layers = row.get("layers")
        require(isinstance(layers, dict) and set(layers) == REQUIRED_LAYERS,
                f"capability {identifier} layer set drifted")
        require(set(layers.values()) <= LAYER_STATES,
                f"capability {identifier} has invalid layer state")
        require(row.get("state") in CAPABILITY_STATES,
                f"capability {identifier} has invalid state")
        reason = row.get("open_reason")
        if row["state"] == "CLOSED":
            require(set(layers.values()) == {"PASS"},
                    f"closed capability {identifier} has a non-PASS layer")
            require(reason == "", f"closed capability {identifier} retains an open reason")
        else:
            require(isinstance(reason, str) and reason,
                    f"open capability {identifier} lacks an open reason")

    excluded = require_rows(data.get("excluded_from_293_core"), "excluded capabilities")
    require_unique(excluded, "id", "excluded capabilities")
    for row in excluded:
        require(isinstance(row.get("owner_task"), int) and row["owner_task"] >= 287,
                f"excluded capability {row['id']} lacks a numbered owner")
        require(isinstance(row.get("reason"), str) and row["reason"],
                f"excluded capability {row['id']} lacks a reason")
    return len(rows)


def validate_product(root: Path, data: dict[str, Any], legacy_ids: set[str],
                     all_capabilities_closed: bool) -> tuple[int, int]:
    require(data.get("schema") == "xray-canonical-product-cutover/1",
            "product cutover schema must be v1")
    require(data.get("architecture_task") == 293, "product architecture task drifted")
    declared_routes = set(data.get("required_route_ids", []))
    require(declared_routes == REQUIRED_ROUTE_IDS, "required product route set drifted")

    routes = require_rows(data.get("routes"), "product routes")
    route_ids = require_unique(routes, "id", "product routes")
    require(route_ids == REQUIRED_ROUTE_IDS, "product route rows do not match required routes")
    for row in routes:
        identifier = row["id"]
        require(isinstance(row.get("public_surface"), str) and row["public_surface"],
                f"route {identifier} lacks public surface")
        require_anchor_list(root, row.get("current_owner_anchors"),
                            f"route {identifier} current owner", files_only=True)
        for field in ("target_contract", "required_capability_scope"):
            require(isinstance(row.get(field), str) and row[field],
                    f"route {identifier} lacks {field}")
        owners = row.get("legacy_owner_ids")
        require(isinstance(owners, list) and owners and set(owners) <= legacy_ids,
                f"route {identifier} has unknown or empty legacy owners")
        require(row.get("state") in ROUTE_STATES, f"route {identifier} has invalid state")
        evidence = row.get("completion_evidence")
        require(isinstance(evidence, list), f"route {identifier} evidence must be an array")
        if row["state"] == "CANONICAL_INCOMPLETE":
            require_anchor_list(root, evidence, f"route {identifier} in-progress evidence",
                                files_only=True)
        if row["state"] == "CANONICAL_ONLY":
            require(all_capabilities_closed,
                    f"route {identifier} cut over before all capabilities closed")
            require_anchor_list(root, evidence, f"route {identifier} completion", files_only=True)

    nodes = require_rows(data.get("deletion_nodes"), "deletion nodes")
    require_unique(nodes, "id", "deletion nodes")
    covered_legacy_ids: set[str] = set()
    for row in nodes:
        identifier = row["id"]
        owners = row.get("legacy_owner_ids")
        require(isinstance(owners, list) and owners and set(owners) <= legacy_ids,
                f"deletion node {identifier} has unknown or empty legacy owners")
        covered_legacy_ids.update(owners)
        require_anchor_list(root, row.get("source_anchors"),
                            f"deletion node {identifier} source")
        patterns = row.get("symbol_patterns")
        require(isinstance(patterns, list) and patterns and
                all(isinstance(pattern, str) and pattern for pattern in patterns),
                f"deletion node {identifier} lacks symbol patterns")
        require(row.get("final_reachable_count") == 0,
                f"deletion node {identifier} final count must be zero")
        require(row.get("state") in DELETION_STATES,
                f"deletion node {identifier} has invalid state")
        evidence = row.get("evidence")
        require(isinstance(evidence, list),
                f"deletion node {identifier} evidence must be an array")
        if row["state"] == "ZERO":
            require_anchor_list(root, evidence, f"deletion node {identifier} zero evidence",
                                files_only=True)
    require(legacy_ids - RETAINED_LEGACY_OWNER_IDS <= covered_legacy_ids,
            "deletion nodes do not cover every non-retained legacy owner")

    gate = data.get("cutover_gate")
    require(isinstance(gate, dict), "cutover gate must be an object")
    booleans = {
        "all_capabilities_closed",
        "all_routes_canonical_only",
        "all_deletion_nodes_zero",
        "aot_compile_input_pure",
        "runtime_only_embed_closed",
        "matching_source_binary_qualified",
    }
    require(set(gate) == booleans | {"state"}, "cutover gate field set drifted")
    require(all(isinstance(gate[field], bool) for field in booleans),
            "cutover gate facts must be booleans")
    aot_compiler = read_text(root / AOT_HEADER) + read_text(root / AOT_LOWERING)
    forbidden_aot_inputs = (
        "XrInstance",
        "XrExecutionLease",
        "xr_execution_instance_",
        "xr_execution_lease_",
    )
    aot_compile_input_pure = (
        "const XrValidatedProgram *program" in aot_compiler and
        "const XrTargetProfile *profile" in aot_compiler and
        "const XrBackendOptions *options" in aot_compiler and
        all(token not in aot_compiler for token in forbidden_aot_inputs)
    )
    derived = {
        "all_capabilities_closed": all_capabilities_closed,
        "all_routes_canonical_only": all(row["state"] == "CANONICAL_ONLY" for row in routes),
        "all_deletion_nodes_zero": all(row["state"] == "ZERO" for row in nodes),
        "aot_compile_input_pure": aot_compile_input_pure,
    }
    for field, expected in derived.items():
        require(gate[field] == expected, f"cutover gate {field} is not derived from rows")
    complete = all(gate[field] for field in booleans)
    require(gate["state"] == ("PASS" if complete else "BLOCKED"),
            "cutover gate state disagrees with its facts")
    if complete:
        require(all(row["state"] == "ZERO" for row in nodes),
                "cutover PASS retained a reachable deletion node")
    return len(routes), len(nodes)


def validate_all(root: Path, capability: dict[str, Any] | None = None,
                 product: dict[str, Any] | None = None) -> tuple[int, int, int]:
    directory = root / CONTRACT_DIR
    require(directory.is_dir(), f"missing contract directory {directory}")
    migration = load_json(directory / MIGRATION_FILE)
    owners = require_rows(migration.get("owners"), "legacy owners")
    legacy_ids = require_unique(owners, "id", "legacy owners")
    capability_data = capability or load_json(directory / CAPABILITY_FILE)
    product_data = product or load_json(directory / PRODUCT_FILE)
    capability_count = validate_capabilities(root, capability_data, legacy_ids)
    all_closed = all(row.get("state") == "CLOSED"
                     for row in capability_data["capabilities"])
    route_count, deletion_count = validate_product(root, product_data, legacy_ids, all_closed)
    return capability_count, route_count, deletion_count


def expect_rejected(label: str, action: Callable[[], Any]) -> None:
    try:
        action()
    except ManifestError:
        return
    raise ManifestError(f"self-test mutation was accepted: {label}")


def self_test(root: Path) -> None:
    validate_all(root)
    directory = root / CONTRACT_DIR
    capability = load_json(directory / CAPABILITY_FILE)
    product = load_json(directory / PRODUCT_FILE)

    duplicate = deepcopy(capability)
    duplicate["capabilities"].append(deepcopy(duplicate["capabilities"][0]))
    expect_rejected("duplicate capability", lambda: validate_all(root, duplicate, product))

    premature = deepcopy(capability)
    premature["capabilities"][0]["state"] = "CLOSED"
    premature["capabilities"][0]["open_reason"] = ""
    expect_rejected("premature capability closure",
                    lambda: validate_all(root, premature, product))

    missing_oracle = deepcopy(capability)
    del missing_oracle["capabilities"][0]["layers"]["independent_oracle"]
    expect_rejected("missing independent correctness basis",
                    lambda: validate_all(root, missing_oracle, product))

    reference_only = deepcopy(capability)
    row = reference_only["capabilities"][0]
    row["layers"]["reference"] = row["layers"].pop("independent_oracle")
    expect_rejected("full reference requirement replacing correctness basis",
                    lambda: validate_all(root, reference_only, product))

    missing_route = deepcopy(product)
    missing_route["routes"] = missing_route["routes"][1:]
    expect_rejected("missing retained product route",
                    lambda: validate_all(root, capability, missing_route))

    missing_incomplete_evidence = deepcopy(product)
    incomplete = next(row for row in missing_incomplete_evidence["routes"]
                      if row["state"] == "CANONICAL_INCOMPLETE")
    incomplete["completion_evidence"] = []
    expect_rejected("canonical incomplete route without evidence",
                    lambda: validate_all(root, capability, missing_incomplete_evidence))

    nonzero_terminal = deepcopy(product)
    nonzero_terminal["deletion_nodes"][0]["final_reachable_count"] = 1
    expect_rejected("nonzero deletion target",
                    lambda: validate_all(root, capability, nonzero_terminal))

    stale_pure_aot = deepcopy(product)
    stale_pure_aot["cutover_gate"]["aot_compile_input_pure"] = False
    expect_rejected("stale pure-AOT input fact",
                    lambda: validate_all(root, capability, stale_pure_aot))

    with tempfile.TemporaryDirectory(prefix="xray-cutover-manifest-") as raw:
        malformed = Path(raw) / "noncanonical.json"
        malformed.write_text(
            read_text(directory / CAPABILITY_FILE).rstrip() + "\n\n", encoding="utf-8")
        expect_rejected("noncanonical JSON", lambda: load_json(malformed))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        if args.self_test:
            self_test(root)
            print("canonical cutover manifest self-test: PASS")
        else:
            capabilities, routes, deletions = validate_all(root)
            print("canonical cutover manifests: PASS")
            print(f"  capabilities: {capabilities}")
            print(f"  retained product routes: {routes}")
            print(f"  deletion nodes: {deletions}")
    except ManifestError as exc:
        print(f"canonical cutover manifests: FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

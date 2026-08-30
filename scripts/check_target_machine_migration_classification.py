#!/usr/bin/env python3
"""Validate source-backed target-machine migration classifications."""

from __future__ import annotations

import argparse
import copy
import json
import os
import re
import sys
from pathlib import Path
from typing import Any


CHECKER = "target-machine-migration-source-classification/1"
DEFAULT_MANIFEST = "contracts/target-machine/migration-source-classification.json"
EXPECTED_CATEGORIES = {
    "SURVIVOR_AUTHORITY",
    "BRIDGE_TEST_ONLY",
    "LEGACY_TOMBSTONE",
    "FIXTURE_ONLY",
}
EXPECTED_POLICY = {
    "broad_name_prefix_is_classification": "forbidden",
    "bridge_or_fixture_install_reachability": "forbidden",
    "legacy_new_feature_or_schema": "forbidden",
    "multiple_classification": "error",
    "unclassified_governed_path": "error",
}
SURVIVOR_NAMESPACE_RE = re.compile(r"xr_vm_[a-z0-9]+(?:_[a-z0-9]+)*")


def read_json(path: Path) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError(f"{path}: top level must be an object")
    return data


def is_exact_relative_path(value: Any) -> bool:
    path = Path(value) if isinstance(value, str) else None
    return (
        isinstance(value, str)
        and value != ""
        and "\\" not in value
        and path is not None
        and not path.is_absolute()
        and ".." not in path.parts
        and not any(token in value for token in ("*", "?", "[", "]"))
    )


def semantic_owner_counts(root: Path, semantic: dict[str, Any], errors: list[str]) -> tuple[int, int]:
    """Validate target applicability without conflating adapters with owners."""
    operations = semantic.get("operations")
    if not isinstance(operations, list) or semantic.get("operation_count") != len(operations):
        errors.append("semantic owner inventory is malformed")
        return 0, 0
    adapter = "representation adapter"
    dual = 0
    shared = 0
    for row in operations:
        operation_id = row.get("operation_id", "<missing>")
        if not operation_id or not row.get("migration_task"):
            errors.append(f"{operation_id}: semantic owner row lacks identity or migration task")
        targets = set(row.get("targets", []))
        is_shared = row.get("family") == "shared-kernel"
        if is_shared:
            shared += 1
        source_backed = is_shared or row.get("future_semantic_owner") != "SemanticPlan.operation_registry"
        vm_applicable = ("vm" if is_shared else "vm-bytecode") in targets
        aot_applicable = ("aot" if is_shared else "aot-c") in targets
        for label, applicable in (("current_vm_owner", vm_applicable),
                                  ("current_aot_owner", aot_applicable)):
            value = row.get(label)
            if not applicable:
                if value is not None:
                    errors.append(f"{operation_id}: inapplicable {label} must be null")
            elif source_backed:
                if value != adapter:
                    errors.append(f"{operation_id}: applicable {label} is not the governed adapter")
            elif not isinstance(value, str) or not value or value == adapter:
                errors.append(f"{operation_id}: generic {label} lacks its live executor owner")
        if not source_backed and (vm_applicable or aot_applicable):
            dual += 1
        owner = row.get("current_shared_owner")
        if not is_exact_relative_path(owner) or not (root / owner).is_file():
            errors.append(f"{operation_id}: canonical source owner is not a current file")
    return dual, shared


def validate_inventory_rows(root: Path, data: dict[str, Any], errors: list[str]) -> dict[str, int]:
    discovery = data.get("discovery", {})
    counts: dict[str, int] = {}

    legacy = read_json(root / discovery.get("legacy_vm_inventory", "<missing>"))
    opcodes = legacy.get("opcodes")
    frames = legacy.get("tagged_frame_sites")
    public_api = legacy.get("vm_public_api_symbols")
    if not isinstance(opcodes, list) or legacy.get("opcode_count") != len(opcodes):
        errors.append("legacy VM opcode inventory is malformed")
    if not isinstance(frames, list) or not isinstance(public_api, list):
        errors.append("legacy VM frame or public API inventory is malformed")
    if isinstance(opcodes, list) and isinstance(frames, list) and isinstance(public_api, list):
        counts["legacy_vm"] = len(opcodes) + len(frames) + len(public_api) + 1

    aot = read_json(root / discovery.get("aot_plan_inventory", "<missing>"))
    rows = aot.get("rows")
    mixed = aot.get("mixed_representation_types")
    if not isinstance(rows, list) or aot.get("row_count") != len(rows):
        errors.append("AOT private plan inventory is malformed")
    elif any(row.get("category") not in {"semantic", "target", "refinement", "emission-link", "obsolete"}
             or not row.get("destination") or not row.get("deletion_task") for row in rows):
        errors.append("AOT private plan inventory has an unresolved destination")
    if isinstance(rows, list) and isinstance(mixed, dict):
        counts["aot_private_plan"] = len(rows) + len(mixed)

    semantic = read_json(root / discovery.get("semantic_owner_inventory", "<missing>"))
    dual, shared = semantic_owner_counts(root, semantic, errors)
    counts["semantic_dual_owner"] = dual
    counts["shared_kernel"] = shared
    return counts


# A configured build tree carries its own copies of these files, and there can
# be many such trees beside the sources. They are generated output, never the
# install declarations this check governs, so the walk skips them outright
# instead of reading each one back.
_SKIP_DIRS = {".cache", ".evidence", ".git"}


def install_blocks(root: Path) -> list[str]:
    blocks: list[str] = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [
            d for d in dirnames if d not in _SKIP_DIRS and not d.startswith("build")
        ]
        if "CMakeLists.txt" not in filenames:
            continue
        text = (Path(dirpath) / "CMakeLists.txt").read_text(encoding="utf-8", errors="strict")
        blocks.extend(re.findall(r"\binstall\s*\((.*?)\)", text, flags=re.IGNORECASE | re.DOTALL))
    return blocks


def validate(root: Path, data: dict[str, Any]) -> tuple[list[str], dict[str, int]]:
    errors: list[str] = []
    if data.get("schema") != 1 or data.get("checker") != CHECKER:
        errors.append("manifest schema or checker identity is not exact")
    if set(data.get("categories", [])) != EXPECTED_CATEGORIES:
        errors.append("manifest categories are not the exact closed set")
    if data.get("policy") != EXPECTED_POLICY:
        errors.append("manifest policy is not exact")

    items = data.get("items")
    if not isinstance(items, list) or not items:
        return errors + ["manifest items must be a nonempty list"], {}

    ids: set[str] = set()
    path_owner: dict[str, str] = {}
    namespace_owner: dict[str, str] = {}
    target_install_blocks = install_blocks(root)
    for item in items:
        if not isinstance(item, dict):
            errors.append("manifest item is not an object")
            continue
        item_id = item.get("id")
        category = item.get("category")
        if not isinstance(item_id, str) or not item_id or item_id in ids:
            errors.append(f"invalid or duplicate item id {item_id!r}")
            continue
        ids.add(item_id)
        if category not in EXPECTED_CATEGORIES:
            errors.append(f"{item_id}: invalid category {category!r}")
        for field in ("completion_limit", "final_disposition", "independent_checker",
                      "serialization_cache_impact", "typed_consumers"):
            if field not in item or item[field] in (None, ""):
                errors.append(f"{item_id}: missing {field}")
        checker = item.get("independent_checker")
        if is_exact_relative_path(checker) and not (root / checker).is_file():
            errors.append(f"{item_id}: independent checker does not exist: {checker}")

        paths = item.get("paths", [])
        if not isinstance(paths, list):
            errors.append(f"{item_id}: paths must be a list")
            paths = []
        for relative in paths:
            if not is_exact_relative_path(relative):
                errors.append(f"{item_id}: governed paths must be exact: {relative!r}")
                continue
            if not (root / relative).is_file():
                errors.append(f"{item_id}: governed path does not exist: {relative}")
            previous = path_owner.get(relative)
            if previous is not None:
                errors.append(f"{relative}: classified by both {previous} and {item_id}")
            path_owner[relative] = item_id

        residue_scan = item.get("legacy_residue_scan")
        if residue_scan is not None:
            if category != "SURVIVOR_AUTHORITY":
                errors.append(f"{item_id}: legacy residue scan requires survivor authority")
            if not isinstance(residue_scan, dict) or set(residue_scan) != {
                "consumers", "namespaces"
            }:
                errors.append(f"{item_id}: legacy residue scan shape is not exact")
            else:
                namespaces = residue_scan.get("namespaces")
                consumers = residue_scan.get("consumers")
                if (
                    not isinstance(namespaces, list)
                    or not namespaces
                    or any(
                        not isinstance(namespace, str)
                        or SURVIVOR_NAMESPACE_RE.fullmatch(namespace) is None
                        for namespace in namespaces
                    )
                    or len(set(namespaces)) != len(namespaces)
                ):
                    errors.append(f"{item_id}: survivor namespaces are not exact and unique")
                else:
                    for namespace in namespaces:
                        previous = namespace_owner.get(namespace)
                        if previous is not None:
                            errors.append(
                                f"{namespace}: residue namespace owned by both "
                                f"{previous} and {item_id}"
                            )
                        namespace_owner[namespace] = item_id
                if (
                    not isinstance(consumers, list)
                    or any(not is_exact_relative_path(value) for value in consumers)
                    or len(set(consumers)) != len(consumers)
                ):
                    errors.append(f"{item_id}: residue consumers are not exact and unique")
                else:
                    for relative in consumers:
                        if not (root / relative).is_file():
                            errors.append(
                                f"{item_id}: residue consumer does not exist: {relative}"
                            )
                        if relative in paths:
                            errors.append(
                                f"{item_id}: residue consumer duplicates an owner path: {relative}"
                            )

        markers = item.get("markers", [])
        if not isinstance(markers, list):
            errors.append(f"{item_id}: markers must be a list")
        else:
            for marker in markers:
                relative = marker.get("path") if isinstance(marker, dict) else None
                text = marker.get("text") if isinstance(marker, dict) else None
                count = marker.get("count") if isinstance(marker, dict) else None
                if not is_exact_relative_path(relative) or not isinstance(text, str) or not text or not isinstance(count, int):
                    errors.append(f"{item_id}: malformed marker")
                    continue
                path = root / relative
                if not path.is_file() or path.read_text(encoding="utf-8").count(text) != count:
                    errors.append(f"{item_id}: marker count drifted for {relative}:{text}")

        if category in {"BRIDGE_TEST_ONLY", "FIXTURE_ONLY"}:
            if item.get("installed") is not False:
                errors.append(f"{item_id}: test-only classification must be non-installed")
            if any(not relative.startswith("tests/") for relative in paths):
                errors.append(f"{item_id}: test-only path escaped tests/")
        if category == "BRIDGE_TEST_ONLY":
            for target in item.get("build_targets", []):
                if any(re.search(rf"\b{re.escape(target)}\b", block) for block in target_install_blocks):
                    errors.append(f"{item_id}: bridge target is install-reachable: {target}")
        if category == "LEGACY_TOMBSTONE":
            if item.get("new_feature") is not False or not item.get("tombstone_deadline"):
                errors.append(f"{item_id}: legacy tombstone lacks the no-feature rule or deadline")

    namespaces = sorted(namespace_owner)
    for index, namespace in enumerate(namespaces):
        for other in namespaces[index + 1:]:
            if other.startswith(namespace + "_"):
                errors.append(
                    f"survivor namespace boundary overlaps: {namespace} and {other}"
                )

    discovery = data.get("discovery", {})
    globs = discovery.get("typed_source_globs") if isinstance(discovery, dict) else None
    if not isinstance(globs, list) or not globs:
        errors.append("typed source discovery globs are missing")
    else:
        for pattern in globs:
            for path in root.glob(pattern):
                if path.is_file():
                    relative = path.relative_to(root).as_posix()
                    if relative not in path_owner:
                        errors.append(f"unclassified governed typed source: {relative}")

    generation = next((item for item in items
                       if isinstance(item, dict) and item.get("id") == "runtime-target-and-generation-lifecycle"), None)
    if not generation or generation.get("completion_limit") != "lifecycle-primitive-not-generation-closure-id" or set(
            generation.get("missing_authority", [])) != {
                "ProgramSemanticClosure", "GenerationClosureId", "module dependency graph"}:
        errors.append("runtime generation lifecycle overclaims the future generation closure")

    try:
        counts = validate_inventory_rows(root, data, errors)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        errors.append(str(error))
        counts = {}
    return errors, counts


def self_test(root: Path, data: dict[str, Any]) -> int:
    errors, _ = validate(root, data)
    if errors:
        print("migration classification self-test: live manifest is invalid", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1

    semantic = read_json(root / data["discovery"]["semantic_owner_inventory"])
    semantic_errors: list[str] = []
    semantic_owner_counts(root, semantic, semantic_errors)
    if semantic_errors:
        print("migration classification self-test: semantic inventory is invalid", file=sys.stderr)
        return 1

    def require_semantic_error(name: str, mutate) -> None:
        candidate = copy.deepcopy(semantic)
        mutate(candidate["operations"])
        found: list[str] = []
        semantic_owner_counts(root, candidate, found)
        if not found:
            raise AssertionError(f"semantic owner mutation escaped: {name}")

    def bind_inapplicable(rows: list[dict[str, Any]]) -> None:
        row = next(item for item in rows
                   if item.get("operation_id", "").startswith("xi.") and
                   "vm-bytecode" not in item.get("targets", []))
        row["current_vm_owner"] = "representation adapter"

    def erase_explicit_adapter(rows: list[dict[str, Any]]) -> None:
        row = next(item for item in rows
                   if item.get("future_semantic_owner") != "SemanticPlan.operation_registry" and
                   "vm-bytecode" in item.get("targets", []))
        row["current_vm_owner"] = None

    def conflate_generic_adapter(rows: list[dict[str, Any]]) -> None:
        row = next(item for item in rows
                   if item.get("future_semantic_owner") == "SemanticPlan.operation_registry" and
                   "aot-c" in item.get("targets", []))
        row["current_aot_owner"] = "representation adapter"

    require_semantic_error("inapplicable-binding", bind_inapplicable)
    require_semantic_error("missing-explicit-adapter", erase_explicit_adapter)
    require_semantic_error("generic-adapter-conflation", conflate_generic_adapter)

    mutations: list[tuple[str, Any]] = []
    wrong_category = copy.deepcopy(data)
    wrong_category["items"][0]["category"] = "LEGACY_OR_MAYBE"
    mutations.append(("closed-category", wrong_category))
    broad_path = copy.deepcopy(data)
    broad_path["items"][0]["paths"][0] = "src/vm/xr_*"
    mutations.append(("exact-path", broad_path))
    duplicate_path = copy.deepcopy(data)
    duplicate_path["items"][1]["paths"].append(duplicate_path["items"][0]["paths"][0])
    mutations.append(("single-classification", duplicate_path))
    marker_drift = copy.deepcopy(data)
    marker_drift["items"][0].setdefault("markers", []).append({
        "count": 1,
        "path": "src/plan/target/xr_target_builder.c",
        "text": "__XRAY_MIGRATION_CLASSIFICATION_SELF_TEST_MISSING_MARKER__",
    })
    mutations.append(("marker-drift", marker_drift))
    generation_overclaim = copy.deepcopy(data)
    for item in generation_overclaim["items"]:
        if item.get("id") == "runtime-target-and-generation-lifecycle":
            item["completion_limit"] = "generation-closure-complete"
    mutations.append(("generation-overclaim", generation_overclaim))
    residue_scan_items = [
        item for item in data["items"]
        if isinstance(item, dict) and "legacy_residue_scan" in item
    ]
    if residue_scan_items:
        broad_namespace = copy.deepcopy(data)
        for item in broad_namespace["items"]:
            if "legacy_residue_scan" in item:
                item["legacy_residue_scan"]["namespaces"][0] = "xr_vm_*"
                break
        mutations.append(("exact-survivor-namespace", broad_namespace))
        broad_consumer = copy.deepcopy(data)
        for item in broad_consumer["items"]:
            if "legacy_residue_scan" in item:
                item["legacy_residue_scan"]["consumers"][0] = "tests/unit/*"
                break
        mutations.append(("exact-residue-consumer", broad_consumer))
        missing_consumer = copy.deepcopy(data)
        for item in missing_consumer["items"]:
            if "legacy_residue_scan" in item:
                item["legacy_residue_scan"]["consumers"][0] = (
                    "tests/__missing_residue_consumer__.c"
                )
                break
        mutations.append(("existing-residue-consumer", missing_consumer))
        duplicate_consumer = copy.deepcopy(data)
        for item in duplicate_consumer["items"]:
            if "legacy_residue_scan" in item:
                consumers = item["legacy_residue_scan"]["consumers"]
                consumers.append(consumers[0])
                break
        mutations.append(("unique-residue-consumer", duplicate_consumer))
    if len(residue_scan_items) >= 2:
        duplicate_namespace = copy.deepcopy(data)
        scans = [
            item["legacy_residue_scan"] for item in duplicate_namespace["items"]
            if "legacy_residue_scan" in item
        ]
        scans[1]["namespaces"].append(scans[0]["namespaces"][0])
        mutations.append(("unique-residue-namespace-owner", duplicate_namespace))
    non_survivor = next(
        (item for item in data["items"]
         if isinstance(item, dict) and item.get("category") != "SURVIVOR_AUTHORITY"),
        None,
    )
    if non_survivor is not None and residue_scan_items:
        scan_on_legacy = copy.deepcopy(data)
        for item in scan_on_legacy["items"]:
            if item.get("id") == non_survivor.get("id"):
                item["legacy_residue_scan"] = copy.deepcopy(
                    residue_scan_items[0]["legacy_residue_scan"]
                )
                break
        mutations.append(("survivor-only-residue-scan", scan_on_legacy))
    for name, mutation in mutations:
        mutation_errors, _ = validate(root, mutation)
        if not mutation_errors:
            print(f"migration classification self-test missed {name}", file=sys.stderr)
            return 1
    print(f"migration classification self-test: PASS ({len(mutations)} mutations)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".")
    parser.add_argument("--manifest", default=DEFAULT_MANIFEST)
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    root = Path(args.root).resolve()
    try:
        data = read_json(root / args.manifest)
        if args.self_test:
            return self_test(root, data)
        errors, counts = validate(root, data)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        errors, counts = [str(error)], {}
    if args.json:
        print(json.dumps({"checker": CHECKER, "counts": counts, "errors": errors,
                          "ok": not errors}, sort_keys=True))
    elif errors:
        print("migration source classification: FAIL", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
    else:
        summary = ", ".join(f"{key}={value}" for key, value in sorted(counts.items()))
        print(f"migration source classification: PASS ({summary})")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())

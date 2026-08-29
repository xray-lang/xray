#!/usr/bin/env python3
"""Keep coroutine lifecycle projection free of nested table rescans."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


def extract_function(source: str, name: str) -> str:
    match = re.search(rf"\b{re.escape(name)}\s*\(", source)
    if not match:
        raise ValueError(f"missing {name}")
    opening = source.find("{", match.end())
    if opening < 0:
        raise ValueError(f"missing {name} body")
    depth = 0
    for offset in range(opening, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                return source[match.start():offset + 1]
    raise ValueError(f"unterminated {name}")


def loop_bodies(source: str) -> list[tuple[str, str]]:
    loops: list[tuple[str, str]] = []
    for match in re.finditer(r"\b(?:for|while)\s*\([^)]*\)", source):
        opening = source.find("{", match.end())
        if opening < 0:
            continue
        depth = 0
        for offset in range(opening, len(source)):
            if source[offset] == "{":
                depth += 1
            elif source[offset] == "}":
                depth -= 1
                if depth == 0:
                    loops.append((match.group(0), source[opening:offset + 1]))
                    break
    return loops


def verify_target_function(source: str, name: str) -> list[str]:
    errors: list[str] = []
    body = extract_function(source, name)
    required = (
        "release_index.rows",
        "next_projection",
        "xr_semantic_lifecycle_work_charge",
    )
    for token in required:
        if token not in body:
            errors.append(f"{name} lacks indexed lifecycle token: {token}")
    forbidden = (
        "xr_semantic_owned_string_coroutine_lifecycle_is_exact",
        "xr_semantic_string_concat_release_is_exact",
    )
    for token in forbidden:
        if token in body:
            errors.append(f"{name} resurrects per-row semantic scan: {token}")
    for header, nested in loop_bodies(body):
        if "operation" in header and "entity < entity_count" in nested:
            errors.append(f"{name} nests the entity table below operation traversal")
    return errors


def verify_semantic_builder(source: str) -> list[str]:
    errors: list[str] = []
    builder_body = extract_function(source, "build_coroutine_entities")
    for token in (
        "xr_semantic_coroutine_lifecycle_projection_build",
        "projection_cursor",
        "projection.indexed_work",
        "state_entity_by_operation",
        "mark_coroutine_value_membership",
        "xr_semantic_lifecycle_work_charge",
    ):
        if token not in builder_body:
            errors.append(f"semantic lifecycle builder lacks indexed token: {token}")
    for token in (
        "xr_semantic_owned_string_coroutine_lifecycle_is_exact",
        "xr_semantic_owned_string_coroutine_lifecycle_from_release_is_exact",
        "xr_semantic_string_concat_release_is_exact",
        "xr_semantic_coroutine_lifecycle_owner_entity",
        "release_index.rows",
    ):
        if token in builder_body:
            errors.append(f"semantic lifecycle builder resurrects scan: {token}")
    for header, nested in loop_bodies(builder_body):
        if ("state" in header or "entity" in header) and (
            "release_index.count" in nested or
            re.search(r"for\s*\([^)]*release\s*=", nested)
        ):
            errors.append("semantic lifecycle builder nests release traversal below state/entity")
    return errors


def verify_semantic_projection_header(source: str) -> list[str]:
    errors: list[str] = []
    body = extract_function(
        source, "xr_semantic_coroutine_lifecycle_projection_build"
    )
    for token in (
        "xr_semantic_lifecycle_compare_state_function_operation",
        "xr_semantic_lifecycle_compare_state_block_operation",
        "xr_semantic_lifecycle_compare_projection",
        "xr_semantic_lifecycle_state_range_lower_bound",
        "xr_semantic_string_concat_release_index_build",
        "node_begin",
        "tree_base",
    ):
        if token not in source or token not in body and token in (
            "node_begin", "tree_base"
        ):
            errors.append(f"semantic lifecycle projection lacks sorted index token: {token}")
    for token in (
        "xr_semantic_owned_string_coroutine_lifecycle_is_exact",
        "xr_semantic_owned_string_coroutine_lifecycle_for_release_is_exact",
        "xr_semantic_owned_string_coroutine_lifecycle_from_release_is_exact",
        "xr_semantic_coroutine_lifecycle_owner_entity",
        "xr_semantic_coroutine_lifecycle_work_budget_is_valid",
    ):
        if token in source:
            errors.append(f"semantic lifecycle header retains full-table helper: {token}")
    return errors


def verify_semantic_verifier(source: str) -> list[str]:
    errors: list[str] = []
    body = extract_function(source, "verify_coroutine_lifecycle_entities")
    for token in (
        "xr_semantic_coroutine_lifecycle_projection_build",
        "compare_coroutine_lifecycle_entity_projection",
        "projection.indexed_work",
    ):
        if token not in body:
            errors.append(f"semantic lifecycle verifier lacks sorted projection token: {token}")
    for token in (
        "find_expected_coroutine_lifecycle",
        "xr_semantic_string_concat_release_index_build",
        "xr_semantic_owned_string_coroutine_lifecycle_from_release_is_exact",
        "release_index.rows",
    ):
        if token in body:
            errors.append(f"semantic lifecycle verifier resurrects release scan: {token}")
    for header, nested in loop_bodies(body):
        if ("state" in header or "entity" in header) and (
            "release_index.count" in nested or
            re.search(r"for\s*\([^)]*release\s*=", nested)
        ):
            errors.append("semantic lifecycle verifier nests release traversal below state/entity")
    return errors


def verify_frame_partitions(frame: str) -> list[str]:
    errors: list[str] = []
    partition_body = extract_function(frame, "function_cleanup_partition")
    for token in ("record->cleanup_begin", "record->cleanup_count"):
        if token not in partition_body:
            errors.append(f"typed frame cleanup partition lacks exact token: {token}")
    for name in ("slot_lifecycle_contract_is_exact",
                 "xr_typed_frame_execute_cleanups"):
        body = extract_function(frame, name)
        if "function_cleanup_partition" not in body:
            errors.append(f"{name} does not consume the function-local cleanup partition")
        if "xr_target_plan_cleanups" in body:
            errors.append(f"{name} scans the global cleanup table")
    root_partition_body = extract_function(frame, "function_root_partition")
    for token in ("record->root_begin", "record->root_count"):
        if token not in root_partition_body:
            errors.append(f"typed frame root partition lacks exact token: {token}")
    for name in ("slot_has_root_map_entry",
                 "xr_typed_frame_bind_coroutine_state",
                 "frame_state_root_map"):
        body = extract_function(frame, name)
        if "function_root_partition" not in body:
            errors.append(f"{name} does not consume the function-local root partition")
        if "xr_target_plan_root_maps" in body:
            errors.append(f"{name} scans the global root table")
    return errors


def verify(root: Path) -> list[str]:
    builder = (root / "src/plan/target/xr_target_builder.c").read_text(
        encoding="utf-8"
    )
    verifier = (root / "src/plan/target/xr_target_verify.c").read_text(
        encoding="utf-8"
    )
    semantic = (root / "src/plan/semantic/xr_semantic_verify.c").read_text(
        encoding="utf-8"
    )
    frame = (root / "src/vm/xr_typed_frame.c").read_text(encoding="utf-8")
    errors = verify_target_function(
        builder, "materialize_coroutine_roots_and_string_cleanups"
    )
    errors.extend(
        verify_target_function(verifier, "verify_roots_and_cleanups_partition")
    )
    semantic_builder = (
        root / "src/plan/semantic/xr_semantic_builder.c"
    ).read_text(encoding="utf-8")
    errors.extend(verify_semantic_builder(semantic_builder))
    lifecycle_header = (
        root / "src/plan/semantic/xr_semantic_coroutine_lifecycle_shape.h"
    ).read_text(encoding="utf-8")
    errors.extend(verify_semantic_projection_header(lifecycle_header))
    key_body = extract_function(semantic, "coroutine_lifecycle_entity_key_is_exact")
    if loop_bodies(key_body):
        errors.append("semantic lifecycle entity lookup must not scan releases or owners")
    errors.extend(verify_semantic_verifier(semantic))
    errors.extend(verify_frame_partitions(frame))
    return errors


def self_test() -> int:
    valid = """
static bool materialize_coroutine_roots_and_string_cleanups(void) {
  charge();
  for (entity = 0; entity < entity_count; entity++) { projection[next_projection++] = entity; }
  for (operation = 0; operation < operation_count; operation++) {
    while (next_projection < projection_count) { next_projection++; }
    use(release_index.rows[next_release]);
  }
  xr_semantic_lifecycle_work_charge();
}
"""
    if verify_target_function(
        valid, "materialize_coroutine_roots_and_string_cleanups"
    ):
        raise RuntimeError("valid indexed fixture was rejected")
    invalid = valid.replace(
        "while (next_projection < projection_count) { next_projection++; }",
        "for (entity = 0; entity < entity_count; entity++) { use(entity); }",
    )
    if not verify_target_function(
        invalid, "materialize_coroutine_roots_and_string_cleanups"
    ):
        raise RuntimeError("nested entity scan mutation was accepted")
    semantic_builder = """
static bool build_coroutine_entities(void) {
  use(xr_semantic_coroutine_lifecycle_projection_build());
  use(projection_cursor);
  use(projection.indexed_work);
  use(state_entity_by_operation);
  use(mark_coroutine_value_membership());
  use(xr_semantic_lifecycle_work_charge());
}
"""
    if verify_semantic_builder(semantic_builder):
        raise RuntimeError("valid semantic projection fixture was rejected")
    for mutation in (
        "xr_semantic_owned_string_coroutine_lifecycle_is_exact();",
        "xr_semantic_owned_string_coroutine_lifecycle_from_release_is_exact();",
        "xr_semantic_string_concat_release_is_exact();",
        "xr_semantic_coroutine_lifecycle_owner_entity();",
        "use(release_index.rows);",
    ):
        mutated = semantic_builder.replace("\n}", f"\n  {mutation}\n}}")
        if not verify_semantic_builder(mutated):
            raise RuntimeError(f"semantic scan mutation was accepted: {mutation}")
    nested_release = semantic_builder.replace(
        "\n}",
        "\n  for (state = 0; state < state_count; state++) {\n"
        "    for (release = 0; release < release_index.count; release++) {}\n"
        "  }\n}",
    )
    if not verify_semantic_builder(nested_release):
        raise RuntimeError("semantic state-by-release mutation was accepted")

    projection_header = """
static int xr_semantic_lifecycle_compare_state_function_operation(void) { return 0; }
static int xr_semantic_lifecycle_compare_state_block_operation(void) { return 0; }
static int xr_semantic_lifecycle_compare_projection(void) { return 0; }
static int xr_semantic_lifecycle_state_range_lower_bound(void) { return 0; }
static int xr_semantic_string_concat_release_index_build(void) { return 0; }
static bool xr_semantic_coroutine_lifecycle_projection_build(void) {
  use(node_begin); use(tree_base); return true;
}
"""
    if verify_semantic_projection_header(projection_header):
        raise RuntimeError("valid semantic projection header fixture was rejected")
    stale_helper = projection_header + "\nstatic bool " \
        "xr_semantic_coroutine_lifecycle_owner_entity(void) { return false; }\n"
    if not verify_semantic_projection_header(stale_helper):
        raise RuntimeError("full-entity helper mutation was accepted")

    semantic_verifier = """
static int compare_coroutine_lifecycle_entity_projection(void) { return 0; }
static bool verify_coroutine_lifecycle_entities(void) {
  use(xr_semantic_coroutine_lifecycle_projection_build());
  use(compare_coroutine_lifecycle_entity_projection);
  use(projection.indexed_work);
  return true;
}
"""
    if verify_semantic_verifier(semantic_verifier):
        raise RuntimeError("valid semantic verifier fixture was rejected")
    verifier_scan = semantic_verifier.replace(
        "return true;",
        "for (entity = 0; entity < entity_count; entity++) {\n"
        "    for (release = 0; release < release_index.count; release++) {}\n"
        "  } return true;",
    )
    if not verify_semantic_verifier(verifier_scan):
        raise RuntimeError("semantic verifier entity-by-release mutation was accepted")
    frame = """
static bool function_cleanup_partition(void) {
  use(record->cleanup_begin); use(record->cleanup_count); return true;
}
static bool slot_lifecycle_contract_is_exact(void) {
  consume(function_cleanup_partition()); return true;
}
static bool xr_typed_frame_execute_cleanups(void) {
  consume(function_cleanup_partition()); return true;
}
static bool function_root_partition(void) {
  use(record->root_begin); use(record->root_count); return true;
}
static bool slot_has_root_map_entry(void) {
  consume(function_root_partition()); return true;
}
static bool xr_typed_frame_bind_coroutine_state(void) {
  consume(function_root_partition()); return true;
}
static bool frame_state_root_map(void) {
  consume(function_root_partition()); return true;
}
"""
    if verify_frame_partitions(frame):
        raise RuntimeError("valid function-local frame fixture was rejected")
    global_cleanup = frame.replace(
        "consume(function_cleanup_partition()); return true;",
        "consume(xr_target_plan_cleanups()); return true;", 1)
    if not verify_frame_partitions(global_cleanup):
        raise RuntimeError("global cleanup-table mutation was accepted")
    global_root = frame.replace(
        "consume(function_root_partition()); return true;",
        "consume(xr_target_plan_root_maps()); return true;", 1)
    if not verify_frame_partitions(global_root):
        raise RuntimeError("global root-table mutation was accepted")
    print("coroutine lifecycle projection self-test: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    errors = verify(Path(args.root).resolve())
    if errors:
        for error in errors:
            print(f"coroutine lifecycle projection: FAIL: {error}")
        return 1
    print("coroutine lifecycle projection gate: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

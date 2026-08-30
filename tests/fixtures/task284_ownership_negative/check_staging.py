#!/usr/bin/env python3
"""Validate staged ownership source contracts and constructed negative oracles."""

from __future__ import annotations

import copy
import hashlib
import json
import subprocess
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
INVENTORY_PATH = HERE / "boundary_inventory.json"
ORACLES_PATH = HERE / "negative_oracles.json"

FROZEN_COMMIT = "49f71d1ea0f8e12aa2af8e64fb5d8d1c6f3ee79c"
FROZEN_TREE = "478e72a21e54ae5cc182caaa77cf9e526725a0a6"
TRAIN_COMMIT = "5f5df6c5778bcf9d54b3b0e9cf29cabd5e27822a"
TRAIN_TREE = "366fc89b4ebe12b5cf700021f441679e3e2b19b2"

EXPECTED_SURFACES = {
    "parser-ast": "LOCK-FRONTEND",
    "parameter-function-type": "LOCK-FRONTEND and LOCK-SCHEMA",
    "receiver": "LOCK-FRONTEND and LOCK-SCHEMA",
    "view-return": "LOCK-FRONTEND and LOCK-SCHEMA",
    "alias-loan": "LOCK-SCHEMA",
    "program-semantic-closure": "LOCK-SCHEMA",
    "storage-domain": "LOCK-SCHEMA and LOCK-RUNTIME",
    "xi-arc": "LOCK-SCHEMA",
    "semantic-plan": "LOCK-SCHEMA",
    "target-plan": "LOCK-SCHEMA",
    "vm": "LOCK-SCHEMA",
    "aot": "LOCK-BUILD-GEN and LOCK-SCHEMA",
    "runtime": "LOCK-RUNTIME",
    "lsp": "LOCK-FRONTEND and LOCK-BUILD-GEN",
    "docs": "frozen docs task",
    "ports": "integration train coordinated downstream lane",
}

EXPECTED_ROWS = {
    "recoverable-root-alias": ("P3", {"LOCK-SCHEMA", "LOCK-RESIDUE"}),
    "source-view-last-use": ("P4", {"LOCK-SCHEMA", "LOCK-RESIDUE"}),
    "body-inferred-receiver": (
        "P5",
        {"LOCK-FRONTEND", "LOCK-SCHEMA", "LOCK-RESIDUE"},
    ),
    "body-inferred-borrow-origin": (
        "P5",
        {"LOCK-FRONTEND", "LOCK-SCHEMA", "LOCK-RESIDUE"},
    ),
    "read-retain-return-alias": (
        "P2",
        {"LOCK-FRONTEND", "LOCK-SCHEMA", "LOCK-RESIDUE"},
    ),
    "read-ref-suspend-authority": (
        "P2",
        {"LOCK-FRONTEND", "LOCK-SCHEMA", "LOCK-RESIDUE"},
    ),
    "typed-domain-edge-authority": (
        "P3",
        {"LOCK-SCHEMA", "LOCK-RUNTIME", "LOCK-RESIDUE"},
    ),
    "move-boundary-graph-retag": (
        "P7",
        {"LOCK-SCHEMA", "LOCK-RUNTIME", "LOCK-BUILD-GEN", "LOCK-RESIDUE"},
    ),
    "name-driven-ownership-permission": (
        "P6",
        {"LOCK-SCHEMA", "LOCK-BUILD-GEN", "LOCK-RESIDUE"},
    ),
}

EXPECTED_CASES = {
    "receiver-read-write": (
        "source", "receiver_read_write.xr", "OWN-E-RECEIVER-READ-WRITE", {"LOCK-FRONTEND"}
    ),
    "receiver-move-plain-lvalue": (
        "source", "receiver_move_plain_lvalue.xr", "OWN-E-RECEIVER-MOVE", {"LOCK-FRONTEND"}
    ),
    "borrow-origin-ambiguous": (
        "source", "borrow_origin_ambiguous.xr", "OWN-E-VIEW-ORIGIN-AMBIGUOUS",
        {"LOCK-FRONTEND", "LOCK-SCHEMA"},
    ),
    "borrow-origin-invalid": (
        "source", "borrow_origin_invalid.xr", "OWN-E-VIEW-ORIGIN-INVALID",
        {"LOCK-FRONTEND", "LOCK-SCHEMA"},
    ),
    "read-escape": (
        "source", "read_escape.xr", "OWN-E-READ-ESCAPE", {"LOCK-FRONTEND", "LOCK-SCHEMA"}
    ),
    "read-suspend": (
        "source", "read_suspend.xr", "OWN-E-READ-SUSPEND", {"LOCK-FRONTEND", "LOCK-SCHEMA"}
    ),
    "ref-escape": (
        "source", "ref_escape.xr", "OWN-E-REF-ESCAPE", {"LOCK-FRONTEND", "LOCK-SCHEMA"}
    ),
    "ref-suspend": (
        "source", "ref_suspend.xr", "OWN-E-REF-SUSPEND", {"LOCK-FRONTEND", "LOCK-SCHEMA"}
    ),
    "external-alias-after-scope": (
        "source", "external_alias_after_scope.xr", "OWN-E-EXTERNAL-ALIAS", {"LOCK-SCHEMA"}
    ),
    "view-after-owner-invalidation": (
        "source", "view_after_owner_invalidation.xr", "OWN-E-VIEW-INVALIDATED", {"LOCK-SCHEMA"}
    ),
    "view-active-conflict": (
        "source", "view_active_conflict.xr", "OWN-E-VIEW-ACTIVE-CONFLICT", {"LOCK-SCHEMA"}
    ),
    "view-escaped": (
        "source", "view_escaped.xr", "OWN-E-VIEW-ESCAPED", {"LOCK-SCHEMA"}
    ),
    "view-mutable-return": (
        "source", "view_mutable_return.xr", "OWN-E-VIEW-MUTABLE",
        {"LOCK-FRONTEND", "LOCK-SCHEMA"},
    ),
    "domain-edge": (
        "semantic-plan-mutation", "domain_edge.mutation.json", "OWN-E-DOMAIN-EDGE",
        {"LOCK-SCHEMA"},
    ),
    "unknown-domain-edge": (
        "semantic-plan-mutation", "unknown_domain_edge.mutation.json", "OWN-E-UNKNOWN-EDGE",
        {"LOCK-SCHEMA"},
    ),
}

EXPECTED_HARNESSES = {
    "tests/compile_errors/run_compile_error_tests.py",
    "tests/unit/frontend/test_xa_program_semantic_closure.c",
    "tests/unit/plan/test_semantic_plan.c",
    "tests/unit/plan/test_target_plan.c",
    "tests/unit/ir/test_xi_program_semantic.c",
    "tests/unit/ir/test_xi_source_move_verify.c",
    "tests/unit/runtime/test_ownership_audit.c",
    "tests/unit/runtime/test_runtime_target_plan_load_archive.c",
    "tests/unit/aot/test_xr_aot_refinement.c",
}

EXPECTED_FIXTURE_SHA256 = {
    "borrow_origin_ambiguous.xr": "d5a33d60c7d41e1e0fbc1a31117d19c51a73e6734ef70db652d43081ca566ca5",
    "borrow_origin_invalid.xr": "0432334f4d2383338cbd089c0978ba888e36f4100737c9fb2de3274347f1f713",
    "domain_edge.mutation.json": "6af7508e525cefc04da64dc64abac255257599e20184e7d45e1cb1943836812b",
    "external_alias_after_scope.xr": "a51654da5e778a2e58370b5753155bfaf939a55728fcf2fa0e55726d949d3072",
    "read_escape.xr": "ad3da40171848dbadfcc61ee49d772125a3f96babc2e94fe28ab233ca0805e87",
    "read_suspend.xr": "e4b8fadd1026f42d7183aac70749e0a6783055b6c22aaeae4ca238a8142c16c9",
    "receiver_move_plain_lvalue.xr": "cc069bf9fde1b301ee7bce65a8cdb0c26e1ad1737fa8575e5769b6c2cc9e89fa",
    "receiver_read_write.xr": "bfe0832a49d11663b82048c8353180aef74da5ad4f6a648b9d56bbcdd77cf6f1",
    "ref_escape.xr": "0d4a4a643667aaae4e0fde3d4d952b6d219b39aeb94aea3a888bf0cb60324c15",
    "ref_suspend.xr": "66079eee7654b99786887762a338ca0b2d112d203440c2a859cfd04ed039f990",
    "unknown_domain_edge.mutation.json": "c1040dbf137f3507ce8adc6a7df7ace98a9e8ebb40ed571539c3398eca9613e6",
    "view_active_conflict.xr": "d5544b27ca5d9dab7a0b274e51f9d66889e587ef009ff6e8729c835d9ab9adb4",
    "view_after_owner_invalidation.xr": "831d391efefb2a06dc3c7ff9d76df89c92f42c633a8f16b5583228b0cd009f9d",
    "view_escaped.xr": "a730cddcf4bc466d74ff39eb24557f1dcb2fdd1b9266feb405beebe6ccd03496",
    "view_mutable_return.xr": "0d8013c86b2d99a50ca2ff8bc06d8237a320dc9cc1bd78999eb05544585bd9bf",
}

REQUIRED_ROW_FIELDS = {
    "id", "current_owner", "replacement_owner", "deletion_phase", "deletion_boundary",
    "required_locks", "prerequisite_gates", "producers", "consumers", "residue", "witnesses",
}
REQUIRED_CASE_FIELDS = {
    "id", "oracle_kind", "fixture", "sha256", "baseline", "activation_reason", "diagnostic_site",
    "help", "required_locks",
}
EXPECTED_SHORTCUTS = {
    "expected-parser-error", "expected-runtime-gap", "skip", "allowlist", "fallback",
}
FORBIDDEN_MARKERS = (
    "xfail", "skip", "allowlist", "expected-runtime", "expected-parser-error", "fallback",
)
ALLOWED_LOCKS = {
    "LOCK-FRONTEND", "LOCK-SCHEMA", "LOCK-RUNTIME", "LOCK-BUILD-GEN", "LOCK-RESIDUE",
}


class ValidationError(Exception):
    """A staged fact is incomplete, stale, or attempts to weaken the oracle."""


def load_json(path: Path) -> dict[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValidationError(f"cannot read {path.relative_to(ROOT)}: {exc}") from exc
    if not isinstance(value, dict):
        raise ValidationError(f"{path.relative_to(ROOT)} must contain one object")
    return value


def git_output(*args: str) -> str:
    result = subprocess.run(
        ["git", *args], cwd=ROOT, check=False, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True,
    )
    if result.returncode != 0:
        raise ValidationError(f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout.strip()


def validate_checkout(require_clean: bool) -> None:
    if git_output("rev-parse", f"{FROZEN_COMMIT}^{{tree}}") != FROZEN_TREE:
        raise ValidationError("frozen base commit no longer names the recorded tree")
    if git_output("rev-parse", f"{TRAIN_COMMIT}^{{tree}}") != TRAIN_TREE:
        raise ValidationError("train base commit no longer names the recorded tree")
    result = subprocess.run(
        ["git", "merge-base", "--is-ancestor", TRAIN_COMMIT, "HEAD"], cwd=ROOT,
        check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    if result.returncode != 0:
        raise ValidationError("train base is not an ancestor of the current checkout")
    if require_clean and git_output("status", "--porcelain", "--untracked-files=all"):
        raise ValidationError("current checkout is dirty")


def require_exact_ids(items: object, expected: set[str], label: str) -> list[dict[str, object]]:
    if not isinstance(items, list) or not all(isinstance(item, dict) for item in items):
        raise ValidationError(f"{label} must be an object list")
    typed_items = list(items)
    ids = [item.get("id") for item in typed_items]
    if any(not isinstance(item_id, str) or not item_id for item_id in ids):
        raise ValidationError(f"{label} contains an empty id")
    if len(ids) != len(set(ids)):
        raise ValidationError(f"{label} contains a duplicate id")
    actual = set(ids)
    if actual != expected:
        raise ValidationError(
            f"{label} id mismatch: missing={sorted(expected - actual)} extra={sorted(actual - expected)}"
        )
    return typed_items


def require_text(record: dict[str, object], field: str, label: str) -> str:
    value = record.get(field)
    if not isinstance(value, str) or not value.strip():
        raise ValidationError(f"{label}.{field} must be non-empty text")
    return value


def require_text_list(record: dict[str, object], field: str, label: str) -> list[str]:
    value = record.get(field)
    if not isinstance(value, list) or not value or not all(isinstance(x, str) and x for x in value):
        raise ValidationError(f"{label}.{field} must be a non-empty text list")
    return value


def require_locks(record: dict[str, object], expected: set[str], label: str) -> None:
    locks = record.get("required_locks")
    if not isinstance(locks, list) or not all(isinstance(lock, str) for lock in locks):
        raise ValidationError(f"{label} has invalid shared locks")
    actual = set(locks)
    if len(locks) != len(actual) or not actual <= ALLOWED_LOCKS or actual != expected:
        raise ValidationError(
            f"{label} shared lock mismatch: expected={sorted(expected)} actual={sorted(actual)}"
        )


def validate_anchor(record: object, label: str) -> None:
    if not isinstance(record, dict):
        raise ValidationError(f"{label} must be an object")
    path_text = require_text(record, "path", label)
    anchor = require_text(record, "anchor", label)
    occurrences = record.get("occurrences")
    if not isinstance(occurrences, int) or occurrences <= 0:
        raise ValidationError(f"{label}.occurrences must be a positive integer")
    path = ROOT / path_text
    if not path.is_file():
        raise ValidationError(f"{label} path does not exist: {path_text}")
    actual = path.read_text(encoding="utf-8", errors="replace").count(anchor)
    if actual != occurrences:
        raise ValidationError(
            f"{label} anchor count changed: {path_text}: expected={occurrences} actual={actual}: {anchor}"
        )


def validate_residue(record: object, label: str) -> None:
    validate_anchor(record, label)
    assert isinstance(record, dict)
    disposition = require_text(record, "post_cut_disposition", label)
    if disposition not in {"delete-zero", "replace-with-new-owner", "retain-bounded"}:
        raise ValidationError(f"{label} has invalid post-cut disposition: {disposition}")
    require_text(record, "post_cut_expectation", label)
    if disposition in {"delete-zero", "replace-with-new-owner"}:
        if record.get("post_cut_occurrences") != 0:
            raise ValidationError(f"{label} must require zero old-anchor occurrences")
    else:
        bound = record.get("post_cut_occurrences_max")
        if not isinstance(bound, int) or bound < 0:
            raise ValidationError(f"{label} retained residue needs a non-negative bound")


def validate_witness(record: object, label: str) -> None:
    validate_anchor(record, label)
    assert isinstance(record, dict)
    require_text(record, "baseline_expected", label)
    require_text(record, "post_cut_expected", label)


def validate_inventory(data: dict[str, object]) -> None:
    if data.get("schema") != "ownership-source-contract-boundary/1":
        raise ValidationError("inventory schema is not exact")
    if data.get("activation") != "staging-only-before-i1-r4":
        raise ValidationError("inventory activation boundary changed")
    base = data.get("frozen_base")
    if not isinstance(base, dict) or base.get("commit") != FROZEN_COMMIT:
        raise ValidationError("inventory base commit changed")
    if base.get("tree") != FROZEN_TREE:
        raise ValidationError("inventory base tree changed")
    train_base = data.get("current_train_base")
    if not isinstance(train_base, dict) or train_base.get("commit") != TRAIN_COMMIT:
        raise ValidationError("inventory train base commit changed")
    if train_base.get("tree") != TRAIN_TREE:
        raise ValidationError("inventory train base tree changed")
    require_text(data, "owned_fact", "inventory")
    for field in ("allowed_layers", "shared_hotspots", "explicitly_unowned"):
        require_text_list(data, field, "inventory")

    surfaces = require_exact_ids(
        data.get("boundary_surfaces"), set(EXPECTED_SURFACES), "inventory.boundary_surfaces"
    )
    for surface in surfaces:
        surface_id = require_text(surface, "id", "inventory surface")
        for field in ("current_owner", "replacement_owner", "single_writer", "activation_boundary"):
            require_text(surface, field, f"inventory surface {surface_id}")
        if surface.get("single_writer") != EXPECTED_SURFACES[surface_id]:
            raise ValidationError(f"inventory surface {surface_id} single-writer mapping changed")

    rows = require_exact_ids(data.get("rows"), set(EXPECTED_ROWS), "inventory.rows")
    for row in rows:
        row_id = require_text(row, "id", "inventory row")
        missing = REQUIRED_ROW_FIELDS - row.keys()
        if missing:
            raise ValidationError(f"inventory row {row_id} missing {sorted(missing)}")
        phase, expected_locks = EXPECTED_ROWS[row_id]
        if row.get("deletion_phase") != phase:
            raise ValidationError(f"inventory row {row_id} deletion phase changed")
        require_locks(row, expected_locks, f"inventory row {row_id}")
        for field in ("current_owner", "replacement_owner", "deletion_boundary"):
            require_text(row, field, f"inventory row {row_id}")
        require_text_list(row, "prerequisite_gates", f"inventory row {row_id}")
        for group in ("producers", "consumers"):
            anchors = row.get(group)
            if not isinstance(anchors, list) or not anchors:
                raise ValidationError(f"inventory row {row_id} has no {group}")
            for index, anchor in enumerate(anchors):
                validate_anchor(anchor, f"inventory row {row_id}.{group}[{index}]")
        residue = row.get("residue")
        if not isinstance(residue, list) or not residue:
            raise ValidationError(f"inventory row {row_id} has no residue")
        for index, anchor in enumerate(residue):
            validate_residue(anchor, f"inventory row {row_id}.residue[{index}]")
        witnesses = row.get("witnesses")
        if not isinstance(witnesses, list) or not witnesses:
            raise ValidationError(f"inventory row {row_id} has no witnesses")
        for index, witness in enumerate(witnesses):
            validate_witness(witness, f"inventory row {row_id}.witnesses[{index}]")


def validate_mutation_fixture(path: Path, reason: str, label: str) -> None:
    mutation = load_json(path)
    if mutation.get("schema") != "ownership-semantic-plan-mutation/1":
        raise ValidationError(f"{label} mutation schema changed")
    if mutation.get("expected_reason") != reason:
        raise ValidationError(f"{label} mutation reason changed")
    require_text(mutation, "source_precondition", label)
    require_text_list(mutation, "mutations", label)
    require_text_list(mutation, "required_verifier_checks", label)
    mutation_text = json.dumps(mutation, sort_keys=True).lower()
    for marker in FORBIDDEN_MARKERS:
        if marker in mutation_text:
            raise ValidationError(f"{label} mutation weakens the gate with {marker}")


def validate_oracles(data: dict[str, object]) -> None:
    if data.get("schema") != "ownership-negative-oracle-staging/1":
        raise ValidationError("oracle schema is not exact")
    if data.get("activation") != "not-runnable-before-public-contract-cut":
        raise ValidationError("oracle activation boundary changed")
    shortcuts = data.get("forbidden_activation_shortcuts")
    if not isinstance(shortcuts, list) or set(shortcuts) != EXPECTED_SHORTCUTS:
        raise ValidationError("oracle forbidden activation shortcuts changed")
    cases = require_exact_ids(data.get("cases"), set(EXPECTED_CASES), "oracle.cases")
    fixture_names: set[str] = set()
    for case in cases:
        case_id = require_text(case, "id", "oracle case")
        missing = REQUIRED_CASE_FIELDS - case.keys()
        if missing:
            raise ValidationError(f"oracle case {case_id} missing {sorted(missing)}")
        expected_kind, expected_fixture, expected_reason, expected_locks = EXPECTED_CASES[case_id]
        kind = require_text(case, "oracle_kind", f"oracle case {case_id}")
        fixture_name = require_text(case, "fixture", f"oracle case {case_id}")
        reason = require_text(case, "activation_reason", f"oracle case {case_id}")
        if (kind, fixture_name, reason) != (expected_kind, expected_fixture, expected_reason):
            raise ValidationError(f"oracle case {case_id} binding changed")
        require_locks(case, expected_locks, f"oracle case {case_id}")
        if fixture_name in fixture_names:
            raise ValidationError(f"oracle fixture reused: {fixture_name}")
        fixture_names.add(fixture_name)
        fixture = HERE / fixture_name
        if fixture.parent != HERE or not fixture.is_file():
            raise ValidationError(f"oracle fixture is missing or escapes staging: {fixture_name}")
        expected_sha256 = EXPECTED_FIXTURE_SHA256.get(fixture_name)
        recorded_sha256 = require_text(case, "sha256", f"oracle case {case_id}")
        actual_sha256 = hashlib.sha256(fixture.read_bytes()).hexdigest()
        if recorded_sha256 != expected_sha256 or actual_sha256 != expected_sha256:
            raise ValidationError(f"oracle fixture content changed: {fixture_name}")
        if kind == "source":
            source = fixture.read_text(encoding="utf-8")
            if not source.strip() or "fn main()" not in source or not source.rstrip().endswith("main()"):
                raise ValidationError(f"oracle fixture is not a complete source case: {fixture_name}")
            lowered = source.lower()
            for marker in FORBIDDEN_MARKERS:
                if marker in lowered:
                    raise ValidationError(f"oracle fixture weakens the gate with {marker}: {fixture_name}")
        elif kind == "semantic-plan-mutation":
            validate_mutation_fixture(fixture, reason, f"oracle case {case_id}")
        else:
            raise ValidationError(f"oracle case {case_id} has unknown kind: {kind}")
        for field in ("baseline", "diagnostic_site", "help"):
            require_text(case, field, f"oracle case {case_id}")
        case_text = json.dumps(case, sort_keys=True).lower()
        for marker in FORBIDDEN_MARKERS:
            if marker in case_text:
                raise ValidationError(f"oracle metadata weakens the gate with {marker}: {case_id}")

    actual_sources = {path.name for path in HERE.glob("*.xr")}
    actual_mutations = {path.name for path in HERE.glob("*.mutation.json")}
    if actual_sources | actual_mutations != fixture_names:
        raise ValidationError(
            "staged fixture registration mismatch: "
            f"missing={sorted((actual_sources | actual_mutations) - fixture_names)} "
            f"stale={sorted(fixture_names - (actual_sources | actual_mutations))}"
        )
    harnesses = data.get("existing_harnesses")
    if not isinstance(harnesses, list) or set(harnesses) != EXPECTED_HARNESSES:
        raise ValidationError("existing harness inventory changed")
    for harness in harnesses:
        if not isinstance(harness, str) or not (ROOT / harness).is_file():
            raise ValidationError(f"stale existing harness: {harness}")


def validate(inventory: dict[str, object], oracles: dict[str, object]) -> None:
    validate_inventory(inventory)
    validate_oracles(oracles)


def self_test(inventory: dict[str, object], oracles: dict[str, object]) -> None:
    mutations: list[tuple[str, dict[str, object], dict[str, object]]] = []
    missing_row = copy.deepcopy(inventory)
    missing_row["rows"] = missing_row["rows"][:-1]
    mutations.append(("missing inventory row", missing_row, copy.deepcopy(oracles)))
    stale_anchor = copy.deepcopy(inventory)
    stale_anchor["rows"][0]["producers"][0]["anchor"] = "definitely-not-a-source-anchor"
    mutations.append(("stale source anchor", stale_anchor, copy.deepcopy(oracles)))
    weak_lock = copy.deepcopy(inventory)
    weak_lock["rows"][2]["required_locks"] = ["LOCK-SCHEMA"]
    mutations.append(("weakened inventory lock", weak_lock, copy.deepcopy(oracles)))
    missing_case = copy.deepcopy(oracles)
    missing_case["cases"] = missing_case["cases"][:-1]
    mutations.append(("missing oracle case", copy.deepcopy(inventory), missing_case))
    wrong_reason = copy.deepcopy(oracles)
    wrong_reason["cases"][0]["activation_reason"] = "OWN-E-WRONG"
    mutations.append(("wrong oracle reason", copy.deepcopy(inventory), wrong_reason))
    swapped_fixture = copy.deepcopy(oracles)
    swapped_fixture["cases"][0]["fixture"] = swapped_fixture["cases"][1]["fixture"]
    mutations.append(("swapped oracle fixture", copy.deepcopy(inventory), swapped_fixture))
    weakened_case = copy.deepcopy(oracles)
    weakened_case["cases"][0]["baseline"] = "expected-parser-error with fallback"
    mutations.append(("weakened oracle", copy.deepcopy(inventory), weakened_case))
    weakened_fixture = copy.deepcopy(oracles)
    weakened_fixture["cases"][0]["sha256"] = "0" * 64
    mutations.append(("weakened fixture content", copy.deepcopy(inventory), weakened_fixture))
    missing_harnesses = copy.deepcopy(oracles)
    missing_harnesses["existing_harnesses"] = []
    mutations.append(("missing harness inventory", copy.deepcopy(inventory), missing_harnesses))
    for label, mutated_inventory, mutated_oracles in mutations:
        try:
            validate(mutated_inventory, mutated_oracles)
        except ValidationError:
            continue
        raise ValidationError(f"self-test mutation was accepted: {label}")


def main(argv: list[str]) -> int:
    try:
        validate_checkout("--require-clean" in argv)
        inventory = load_json(INVENTORY_PATH)
        oracles = load_json(ORACLES_PATH)
        validate(inventory, oracles)
        if "--self-test" in argv:
            self_test(inventory, oracles)
    except ValidationError as exc:
        print(f"ownership source-contract staging: FAIL: {exc}", file=sys.stderr)
        return 1
    suffix = " with mutation self-test" if "--self-test" in argv else ""
    print(
        f"ownership source-contract staging: PASS: {len(EXPECTED_ROWS)} owner rows, "
        f"{len(EXPECTED_CASES)} negative oracles{suffix}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

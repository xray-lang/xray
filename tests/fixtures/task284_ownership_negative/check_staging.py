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
TYPED_CONTRACT_PATH = HERE / "typed_contract_staging.json"
ATOMIC_CUT_PATH = HERE / "atomic_cut_staging.json"

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
EXPECTED_STAGING_SHA256 = {
    "typed_contract_staging.json": "5376adda1d97fd5d2056e91ce0e7afd423685770356ab1a71e64382e612cbb32",
    "atomic_cut_staging.json": "268c9b468a303a128423e4c14534b029830f33ea2c298e4c30241ca18754cad8",
}
EXPECTED_STAGING_FILES = {TYPED_CONTRACT_PATH.name, ATOMIC_CUT_PATH.name}
EXPECTED_TYPED_AUTHORITY = (
    "non-authoritative executable-oracle input; the integration train owns public activation "
    "after granting every required lock"
)
EXPECTED_CUT_AUTHORITY = (
    "non-authoritative dependency and deletion ledger for one integration-train activation batch"
)
EXPECTED_BORROW_ELISION = {
    "explicit_set": "bind and validate the declared set without running elision",
    "one_signature_candidate": "normalize to the sole candidate",
    "multiple_signature_candidates": "reject with OWN-E-VIEW-ORIGIN-AMBIGUOUS",
    "zero_signature_candidates": "reject with OWN-E-VIEW-ORIGIN-INVALID unless static is explicit",
}
EXPECTED_AXES = {
    "binding_use": ["UNINITIALIZED", "LIVE", "MOVED", "MAYBE_MOVED", "UNKNOWN"],
    "domain_transfer": ["CANDIDATE", "EXTERNAL_ALIASED", "ESCAPED", "UNKNOWN"],
    "capability": ["MUTABLE", "CONST", "SYNC", "BORROW_READ", "BORROW_WRITE", "UNKNOWN"],
    "loan": ["READ", "WRITE", "RAW", "VIEW", "CAPTURE", "CLEANUP"],
    "view_validity": ["LIVE", "INVALIDATED", "MAYBE_INVALIDATED", "UNKNOWN"],
}
EXPECTED_PARAMETER_MODES = [
    {"symbol": "READ", "value": 0, "maximum_capability": "call-bound readonly use"},
    {
        "symbol": "REF", "value": 1,
        "maximum_capability": "call-bound exclusive read/write place loan",
    },
    {"symbol": "MOVE", "value": 2, "maximum_capability": "unique owner transfer"},
]
EXPECTED_ORIGIN_KINDS = ["PARAM", "RECEIVER", "STATIC"]
EXPECTED_DOMAIN_STATES = [
    ("CANDIDATE", 0), ("EXTERNAL_ALIASED", 1), ("ESCAPED", 2), ("UNKNOWN", 3),
]
EXPECTED_DOMAIN_TRANSITIONS = {
    "CANDIDATE->EXTERNAL_ALIASED",
    "EXTERNAL_ALIASED->ESCAPED",
    "CANDIDATE->UNKNOWN",
    "EXTERNAL_ALIASED->UNKNOWN",
    "ESCAPED->UNKNOWN",
}
EXPECTED_EDGE_KINDS = [
    "INTERNAL_OWNED", "EXTERNAL_STRONG", "BORROW_READ", "BORROW_WRITE", "CONST_SHARED",
    "SYNC_SHARED", "WEAK", "FOREIGN_OWNED", "FOREIGN_BORROWED", "UNKNOWN",
]
EXPECTED_VIEW_STATES = [
    ("LIVE", 0), ("INVALIDATED", 1), ("MAYBE_INVALIDATED", 2), ("UNKNOWN", 3),
]
EXPECTED_VIEW_JOIN_TABLE = [
    {"left": "LIVE", "right": "LIVE", "result": "LIVE"},
    {"left": "LIVE", "right": "INVALIDATED", "result": "MAYBE_INVALIDATED"},
    {"left": "LIVE", "right": "MAYBE_INVALIDATED", "result": "MAYBE_INVALIDATED"},
    {"left": "LIVE", "right": "UNKNOWN", "result": "UNKNOWN"},
    {"left": "INVALIDATED", "right": "LIVE", "result": "MAYBE_INVALIDATED"},
    {"left": "INVALIDATED", "right": "INVALIDATED", "result": "INVALIDATED"},
    {"left": "INVALIDATED", "right": "MAYBE_INVALIDATED", "result": "MAYBE_INVALIDATED"},
    {"left": "INVALIDATED", "right": "UNKNOWN", "result": "UNKNOWN"},
    {"left": "MAYBE_INVALIDATED", "right": "LIVE", "result": "MAYBE_INVALIDATED"},
    {"left": "MAYBE_INVALIDATED", "right": "INVALIDATED", "result": "MAYBE_INVALIDATED"},
    {"left": "MAYBE_INVALIDATED", "right": "MAYBE_INVALIDATED", "result": "MAYBE_INVALIDATED"},
    {"left": "MAYBE_INVALIDATED", "right": "UNKNOWN", "result": "UNKNOWN"},
    {"left": "UNKNOWN", "right": "LIVE", "result": "UNKNOWN"},
    {"left": "UNKNOWN", "right": "INVALIDATED", "result": "UNKNOWN"},
    {"left": "UNKNOWN", "right": "MAYBE_INVALIDATED", "result": "UNKNOWN"},
    {"left": "UNKNOWN", "right": "UNKNOWN", "result": "UNKNOWN"},
]
EXPECTED_SEMANTIC_FACTS = {
    "declared parameter/receiver contract",
    "normalized borrowed-return origin set",
    "binding movedness proof",
    "domain transfer proof",
    "view origin/validity/invalidation plan",
    "call-bound loan scopes",
    "ownership edge/domain closure",
    "allocation/storage/drop plan",
    "boundary transfer action",
    "proof/certificate ids",
}
EXPECTED_CUT_ORDER = [
    "source-contract",
    "strict-call-contract",
    "domain-contract",
    "view-contract",
    "signature-validation",
    "plan-and-xi-contract",
    "execution-contract",
    "retirement-contract",
]
EXPECTED_CUT_NODES = {
    "source-contract": (
        "P1", set(), {"LOCK-FRONTEND", "LOCK-SCHEMA", "LOCK-BUILD-GEN"}, set(), set(), {
            "OWN-E-RECEIVER-READ-WRITE", "OWN-E-RECEIVER-MOVE",
            "OWN-E-VIEW-ORIGIN-AMBIGUOUS", "OWN-E-VIEW-ORIGIN-INVALID",
            "OWN-E-VIEW-MUTABLE",
        },
    ),
    "strict-call-contract": (
        "P2", {"source-contract"}, {"LOCK-FRONTEND", "LOCK-SCHEMA"},
        {"read-retain-return-alias", "read-ref-suspend-authority"},
        set(),
        {"OWN-E-READ-ESCAPE", "OWN-E-READ-SUSPEND", "OWN-E-REF-ESCAPE", "OWN-E-REF-SUSPEND"},
    ),
    "domain-contract": (
        "P3", {"strict-call-contract"}, {"LOCK-SCHEMA", "LOCK-RUNTIME", "LOCK-BUILD-GEN"},
        {"recoverable-root-alias", "typed-domain-edge-authority"},
        set(),
        {"OWN-E-EXTERNAL-ALIAS", "OWN-E-DOMAIN-EDGE", "OWN-E-UNKNOWN-EDGE"},
    ),
    "view-contract": (
        "P4", {"domain-contract"}, {"LOCK-SCHEMA"}, {"source-view-last-use"}, set(), {
            "OWN-E-VIEW-INVALIDATED", "OWN-E-VIEW-ACTIVE-CONFLICT", "OWN-E-VIEW-ESCAPED",
        },
    ),
    "signature-validation": (
        "P5", {"source-contract", "strict-call-contract", "view-contract"},
        {"LOCK-FRONTEND", "LOCK-SCHEMA"},
        {"body-inferred-receiver", "body-inferred-borrow-origin"},
        set(),
        {"OWN-E-RECEIVER-READ-WRITE", "OWN-E-VIEW-ORIGIN-INVALID"},
    ),
    "plan-and-xi-contract": (
        "P6", {"signature-validation", "domain-contract", "view-contract"},
        {"LOCK-SCHEMA", "LOCK-BUILD-GEN"}, {"name-driven-ownership-permission"}, set(), {
            "OWN-E-DOMAIN-EDGE", "OWN-E-UNKNOWN-EDGE", "OWN-E-VIEW-INVALIDATED",
        },
    ),
    "execution-contract": (
        "P7", {"plan-and-xi-contract"}, {"LOCK-SCHEMA", "LOCK-RUNTIME", "LOCK-BUILD-GEN"},
        {"move-boundary-graph-retag"}, set(),
        {"OWN-E-DOMAIN-EDGE", "OWN-E-UNKNOWN-EDGE"},
    ),
    "retirement-contract": (
        "P8", set(EXPECTED_CUT_ORDER[:-1]), ALLOWED_LOCKS, set(), set(EXPECTED_ROWS), {
            case[2] for case in EXPECTED_CASES.values()
        },
    ),
}
EXPECTED_CUT_NODE_DETAILS = {
    "source-contract": (
        "receiver modes and BorrowOriginSet are declaration-owned source and type identity",
        [
            "parser and public AST", "function type and PSC",
            "formatter LSP MCP and API projection", "source corpus migration",
        ],
        [
            "receiver parser and formatter KATs", "BorrowOriginSet normalization and roundtrip",
            "interface import and function-value identity",
        ],
    ),
    "strict-call-contract": (
        "declared READ REF MOVE modes bound maximum call capability without body expansion",
        ["call checker", "effect summary", "call-bound loan validation"],
        ["READ non-retaining calls", "REF exclusive place mutation", "MOVE whole-binding transfer"],
    ),
    "domain-contract": (
        "a monotone domain transfer lattice and typed ownership edges decide transfer",
        ["ownership domain solver", "storage plan", "typed native and foreign edge registry"],
        ["candidate domain transfer", "owned DAG adoption", "owned cycle teardown"],
    ),
    "view-contract": (
        "forward view validity and typed owner effects decide source legality",
        ["view facts and owner reverse index", "CFG forward joins", "call-bound loan scopes"],
        [
            "owner action invalidates intersecting local views", "branch and loop forward join",
            "view rebind creates a new version",
        ],
    ),
    "signature-validation": (
        "method bodies and return paths validate but never create receiver or origin authority",
        [
            "method and override validation", "borrowed return path verification",
            "import and dynamic target matching",
        ],
        [
            "READ REF MOVE receiver body validation",
            "every return provenance belongs to its declared origin set",
            "dynamic target sets have identical normalized contracts",
        ],
    ),
    "plan-and-xi-contract": (
        "SemanticPlan publishes complete ownership facts and Xi consumes them without source inference",
        [
            "SemanticPlan builder verifier and codec", "TargetPlan builder verifier and codec",
            "Xi lowering source-move verifier and ARC",
        ],
        [
            "SemanticPlan build check serialize load replay",
            "TargetPlan representation-only selection", "Xi source move and ARC path balance",
        ],
    ),
    "execution-contract": (
        "VM and AOT execute the same verified O(1) domain handoff and exactly-once drop plan",
        ["VM plan execution", "AOT generated C", "portable runtime headers", "artifact identity"],
        [
            "VM AOT ownership differential", "DAG cycle error and cancel teardown",
            "supported generated-C provider matrix",
        ],
    ),
    "retirement-contract": (
        "all covered legacy authorities and compatibility residue are absent after the typed owner is proven",
        [
            "legacy symbol and text inventory", "contract anchors and digests",
            "cache and artifact version", "completion baseline",
        ],
        [
            "all inventory residue reaches its declared zero replacement or bound",
            "contract freeze and hostile artifact checks", "full sanitizer differential and provider gates",
        ],
    ),
}
EXPECTED_ACTIVATION_GATES = [
    (
        "matching-default-release-binary", "before activation",
        "stdlib/sync/sync.xr XR_SEM_0019 blocks the current train binary",
    ),
    (
        "semantic-plan-build-check-replay", "plan-and-xi-contract",
        "typed ownership rows are not publicly activated",
    ),
    (
        "semantic-and-target-plan-hostile-input", "plan-and-xi-contract",
        "typed ownership codecs and loaders are not publicly activated",
    ),
    (
        "vm-aot-same-plan-differential", "execution-contract",
        "VM and AOT typed ownership consumers do not exist",
    ),
    (
        "o1-handoff-no-graph-walk", "execution-contract",
        "visited_node_count=0 and no-copy code shape require the activated runtime path",
    ),
    (
        "handoff-cleanup-drop-once", "execution-contract",
        "channel close full timeout cancel task-failure and drop-once paths are not activated",
    ),
    (
        "meta-ownership-inventory", "retirement-contract",
        "must rerun against the activated final tree",
    ),
    (
        "generated-c-w1-w4", "retirement-contract",
        "generated ownership paths are not activated",
    ),
    (
        "contract-freeze-and-hostile-artifact", "retirement-contract",
        "schema cache artifact and contract identities are not activated",
    ),
    (
        "asan-focused", "retirement-contract",
        "requires the independent asan_focused compiler ASan and UBSan build of the activated tree",
    ),
    (
        "aot-ubsan-generated-output", "retirement-contract",
        "requires aot_ubsan to instrument and execute real generated native output",
    ),
    (
        "lsan-strict", "retirement-contract",
        "requires the independent lsan_strict supported-host leak build of the activated tree",
    ),
    (
        "tsan-focused-if-applicable", "retirement-contract",
        "requires the independent tsan_focused build when the activated runtime path is supported",
    ),
    (
        "provider-appleclang", "retirement-contract",
        "requires real generated C on an AppleClang host",
    ),
    ("provider-clang", "retirement-contract", "requires real generated C on a Clang host"),
    ("provider-gcc", "retirement-contract", "requires real generated C on a GCC host"),
    ("provider-msvc", "retirement-contract", "requires real generated C on an MSVC Windows host"),
    ("provider-zig", "retirement-contract", "requires real generated C on a Zig provider host"),
]
EXPECTED_EXTERNAL_DEPENDENCIES = [
    {
        "id": "i1-r4-checkpoint",
        "owner": "integration train 01a0510c-59b4-7471-9945-a396121e3019",
        "status": "WAITING",
    },
    {
        "id": "docs-sync", "owner": "docs task 01a0510d-17d5-7130-ac01-c45bd1c5dca8",
        "status": "WAITING",
    },
    {
        "id": "ports-downstream", "owner": "integration-train coordinated downstream lane",
        "status": "WAITING",
    },
]


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


def require_exact_keys(record: dict[str, object], expected: set[str], label: str) -> None:
    actual = set(record)
    if actual != expected:
        raise ValidationError(
            f"{label} key mismatch: missing={sorted(expected - actual)} extra={sorted(actual - expected)}"
        )


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


def require_symbol_values(records: object, expected: list[tuple[str, int]], label: str) -> None:
    if not isinstance(records, list) or not all(isinstance(record, dict) for record in records):
        raise ValidationError(f"{label} must be an object list")
    actual = [(record.get("symbol"), record.get("value")) for record in records]
    if actual != expected:
        raise ValidationError(f"{label} symbolic values changed: expected={expected} actual={actual}")


def validate_staging_bytes(name: str, content: bytes, expected: str) -> None:
    if hashlib.sha256(content).hexdigest() != expected:
        raise ValidationError(f"typed staging content changed: {name}")


def validate_staging_hash_coverage(expected: dict[str, str]) -> None:
    if set(expected) != EXPECTED_STAGING_FILES:
        raise ValidationError("typed staging hash coverage changed")


def validate_staging_file_hashes() -> None:
    validate_staging_hash_coverage(EXPECTED_STAGING_SHA256)
    for name, expected in EXPECTED_STAGING_SHA256.items():
        path = HERE / name
        if not path.is_file():
            raise ValidationError(f"typed staging content is missing: {name}")
        validate_staging_bytes(name, path.read_bytes(), expected)


def validate_typed_contract(data: dict[str, object]) -> None:
    require_exact_keys(data, {
        "schema", "activation", "authority", "train_base", "public_activation_forbidden_without",
        "forbidden_compatibility", "axes", "parameter_modes", "receiver_contract",
        "borrow_origin_contract", "domain_transfer_contract", "ownership_edge_contract",
        "view_validity_contract", "semantic_plan_contract", "target_plan_contract",
    }, "typed contract")
    if data.get("schema") != "ownership-typed-contract-staging/1":
        raise ValidationError("typed contract schema is not exact")
    if data.get("activation") != "staging-only-before-i1-r4":
        raise ValidationError("typed contract activation boundary changed")
    authority = require_text(data, "authority", "typed contract")
    if authority != EXPECTED_TYPED_AUTHORITY:
        raise ValidationError("typed contract claims public authority")
    train_base = data.get("train_base")
    if not isinstance(train_base, dict) or train_base != {"commit": TRAIN_COMMIT, "tree": TRAIN_TREE}:
        raise ValidationError("typed contract train base changed")
    activation_requirements = require_text_list(
        data, "public_activation_forbidden_without", "typed contract"
    )
    if set(activation_requirements) != ALLOWED_LOCKS | {"I1/R4 clean checkpoint"}:
        raise ValidationError("typed contract activation requirements changed")
    compatibility = set(require_text_list(data, "forbidden_compatibility", "typed contract"))
    required_compatibility = {
        "alias", "shim", "legacy schema reader", "dual write", "dual read", "fallback selector",
        "migration flag", "second planner", "second executor", "second cache", "transition facade",
    }
    if compatibility != required_compatibility:
        raise ValidationError("typed contract compatibility prohibition changed")

    axes = data.get("axes")
    if axes != EXPECTED_AXES:
        raise ValidationError("typed contract semantic axes changed")
    if data.get("parameter_modes") != EXPECTED_PARAMETER_MODES:
        raise ValidationError("parameter mode contract changed")

    receiver = data.get("receiver_contract")
    if not isinstance(receiver, dict) or receiver.get("modes") != ["READ", "REF", "MOVE"]:
        raise ValidationError("receiver contract modes changed")
    require_exact_keys(receiver, {
        "ast_field", "type_field", "modes", "identity_owner", "body_role", "fixed_forms",
        "identity_consumers", "override_rule", "forbidden_authorities",
    }, "receiver contract")
    if receiver.get("ast_field") != "AstMethodDecl.receiver_mode":
        raise ValidationError("receiver AST owner changed")
    if receiver.get("type_field") != "XrFunctionType.receiver_mode":
        raise ValidationError("receiver type owner changed")
    if receiver.get("identity_owner") != "method or interface declaration and function type":
        raise ValidationError("receiver identity owner changed")
    if receiver.get("body_role") != "validate implementation effects do not exceed the declaration":
        raise ValidationError("receiver body role changed")
    if receiver.get("fixed_forms") != [
        "static and receiver mode are mutually exclusive",
        "constructor owns construction-only mutable this and declares no receiver mode",
        "computed-property getter is READ", "syntactic setter is REF",
        "sync-interior mutation is a sealed typed-registry capability and adds no public mode",
    ]:
        raise ValidationError("receiver fixed-form contract changed")
    if receiver.get("identity_consumers") != [
        "interface", "override", "method item/value", "API fingerprint",
    ]:
        raise ValidationError("receiver identity consumer set changed")
    if receiver.get("override_rule") != (
        "receiver mode must exactly match the base or interface declaration"
    ):
        raise ValidationError("receiver override rule changed")
    receiver_forbidden = set(require_text_list(receiver, "forbidden_authorities", "receiver contract"))
    if receiver_forbidden != {
        "recursive body scan", "method name", "mutates_receiver boolean", "backend inference",
    }:
        raise ValidationError("receiver forbidden authority set changed")

    origins = data.get("borrow_origin_contract")
    if not isinstance(origins, dict):
        raise ValidationError("borrow origin contract must be an object")
    require_exact_keys(origins, {
        "syntax_field", "ast_origin_array", "function_type_named_params", "normalized_type_field",
        "elision_type_field", "syntax_states", "ast_origin_kinds", "canonical_origin_kinds",
        "canonical_identity", "kind_order", "eligibility", "ineligible", "elision",
        "identity_exclusions", "required_invariants", "return_surface_constraints",
    }, "borrow origin contract")
    if origins.get("syntax_field") != "AstFunctionDecl.borrow_origin_syntax":
        raise ValidationError("borrow origin syntax owner changed")
    if origins.get("ast_origin_array") != "AstBorrowOriginRef[]":
        raise ValidationError("borrow origin AST row owner changed")
    if origins.get("function_type_named_params") != (
        "AstFunctionType.named_params are optional non-semantic names"
    ):
        raise ValidationError("borrow origin named-parameter role changed")
    if origins.get("normalized_type_field") != (
        "XrFunctionType.view_origin_set[] stores canonical (kind, parameter ordinal) rows"
    ):
        raise ValidationError("borrow origin normalized type field changed")
    if origins.get("elision_type_field") != (
        "XrFunctionType.view_origin_was_elided is diagnostic/source-map data and not type identity"
    ):
        raise ValidationError("borrow origin elision-field role changed")
    if origins.get("syntax_states") != ["OMITTED", "EXPLICIT_SET"]:
        raise ValidationError("borrow origin syntax states changed")
    if origins.get("ast_origin_kinds") != ["PARAM_NAME", "RECEIVER", "STATIC"]:
        raise ValidationError("borrow origin AST kinds changed")
    if origins.get("canonical_origin_kinds") != EXPECTED_ORIGIN_KINDS:
        raise ValidationError("borrow origin canonical kinds changed")
    if origins.get("kind_order") != EXPECTED_ORIGIN_KINDS:
        raise ValidationError("borrow origin canonical order changed")
    if origins.get("canonical_identity") != "sorted and deduplicated (kind, parameter ordinal) set":
        raise ValidationError("borrow origin identity changed")
    if origins.get("eligibility") != [
        "READ parameter whose input type is a total type-system match for the returned const Slice backing storage",
        "READ receiver whose input type is a total type-system match for the returned const Slice backing storage",
        "verified static immutable storage",
    ]:
        raise ValidationError("borrow origin eligibility judgement changed")
    if set(require_text_list(origins, "ineligible", "borrow origin contract")) != {
        "REF", "MOVE", "local", "temporary", "foreign unknown",
    }:
        raise ValidationError("borrow origin eligibility boundary changed")
    elision = origins.get("elision")
    if elision != EXPECTED_BORROW_ELISION:
        raise ValidationError("borrow origin elision outcomes changed")
    if set(require_text_list(origins, "identity_exclusions", "borrow origin contract")) != {
        "parameter name", "source spelling order", "view_origin_was_elided", "function body",
        "call-site value flow",
    }:
        raise ValidationError("borrow origin identity exclusions changed")
    if set(require_text_list(origins, "required_invariants", "borrow origin contract")) != {
        "borrowed return origin set is nonempty", "parameter ordinals are in range",
        "parameter and receiver origins retain READ mode",
        "each return-path provenance is a member of the normalized set",
        "static provenance creates no invalidatable caller root",
    }:
        raise ValidationError("borrow origin verifier invariants changed")
    if set(require_text_list(origins, "return_surface_constraints", "borrow origin contract")) != {
        "the first public cut permits only direct const Slice<T> borrowed returns",
        "tuple union enum and struct may not conceal a borrowed return",
        "local temporary and unknown provenance cannot impersonate static or input provenance",
        "static origin storage is readonly and lives for the program or module use period",
        "interface extern bodyless declaration function value import sidecar and dynamic target normalize from the signature",
        "every dynamic target has an exactly equal normalized origin set",
        "missing sidecar or inconsistent target set fails closed",
    }:
        raise ValidationError("borrowed return surface contract changed")

    domain = data.get("domain_transfer_contract")
    if not isinstance(domain, dict) or domain.get("type") != "XaDomainTransferState":
        raise ValidationError("domain transfer owner changed")
    require_exact_keys(domain, {
        "type", "states", "allowed_transitions", "forbidden_recovery_triggers", "copy_semantics",
        "move_semantics",
    }, "domain transfer contract")
    if domain.get("states") != [
        {"symbol": symbol, "value": value} for symbol, value in EXPECTED_DOMAIN_STATES
    ]:
        raise ValidationError("domain transfer state records changed")
    require_symbol_values(domain.get("states"), EXPECTED_DOMAIN_STATES, "domain transfer states")
    transitions = domain.get("allowed_transitions")
    if not isinstance(transitions, list) or len(transitions) != len(set(transitions)):
        raise ValidationError("domain transfer transitions must be unique")
    if set(transitions) != EXPECTED_DOMAIN_TRANSITIONS:
        raise ValidationError("domain transfer transition set changed")
    if any(transition.split("->", 1)[1] == "CANDIDATE" for transition in transitions):
        raise ValidationError("domain transfer permits a recovery backedge")
    if set(require_text_list(domain, "forbidden_recovery_triggers", "domain contract")) != {
        "scope exit", "last use", "retain count drops to one", "container overwrite",
    }:
        raise ValidationError("domain recovery prohibition changed")
    if domain.get("copy_semantics") != "copy creates a new candidate domain":
        raise ValidationError("domain copy semantics changed")
    if domain.get("move_semantics") != "move transfers the root token of the same candidate domain":
        raise ValidationError("domain move semantics changed")

    edges = data.get("ownership_edge_contract")
    if not isinstance(edges, dict) or edges.get("type") != "XaOwnershipEdgeKind":
        raise ValidationError("ownership edge owner changed")
    require_exact_keys(edges, {
        "type", "kinds", "required_fields", "borrow_fields", "owned_foreign_fields",
        "internal_owned_criteria", "adoption_rules", "external_alias_triggers", "unknown_policy",
        "forbidden_classifiers",
    }, "ownership edge contract")
    if edges.get("kinds") != EXPECTED_EDGE_KINDS:
        raise ValidationError("ownership edge taxonomy changed")
    if set(require_text_list(edges, "required_fields", "ownership edge contract")) != {
        "source value/place", "target value/place", "source domain id", "target domain id", "kind",
        "provenance", "creation site", "complete", "unknown reason",
    }:
        raise ValidationError("ownership edge required fields changed")
    if set(require_text_list(edges, "borrow_fields", "ownership edge contract")) != {
        "loan boundary", "view fact id",
    }:
        raise ValidationError("ownership borrow edge fields changed")
    if set(require_text_list(edges, "owned_foreign_fields", "ownership edge contract")) != {
        "drop/finalizer policy",
    }:
        raise ValidationError("ownership drop policy fields changed")
    if set(require_text_list(edges, "internal_owned_criteria", "ownership edge contract")) != {
        "source and target already share one ownership domain or source enters by legal move adoption",
        "the domain ledger owns the edge lifetime",
        "the edge publishes no ordinary strong handle outside the domain",
        "drop/finalizer order is complete or domain teardown handles the internal strong cycle",
        "the edge carries no execution-affine unknown-foreign or non-transferable weak-table state",
    }:
        raise ValidationError("INTERNAL_OWNED criteria changed")
    if set(require_text_list(edges, "adoption_rules", "ownership edge contract")) != {
        "child source domain is CANDIDATE with no loan and a complete plan",
        "child binding becomes MOVED",
        "child allocation/drop ledger merges or attaches to the parent ownership domain",
        "the field edge is INTERNAL_OWNED", "the parent domain transfer state does not worsen",
        "adoption cannot cross const sync or foreign boundaries or change execution-affine finalizer constraints",
    }:
        raise ValidationError("ownership adoption rules changed")
    if set(require_text_list(edges, "external_alias_triggers", "ownership edge contract")) != {
        "ordinary root handle binding or assignment",
        "store into another ownership domain field container or closure",
        "return an ordinary managed alias", "dynamic or foreign call that may retain",
        "publish to module mutable global or unknown storage",
        "stored closure captures a mutable root by reference", "incomplete edge provenance",
    }:
        raise ValidationError("external alias trigger set changed")
    if edges.get("unknown_policy") != "UNKNOWN edge or provenance fails closed":
        raise ValidationError("unknown edge policy changed")
    if set(require_text_list(edges, "forbidden_classifiers", "ownership edge contract")) != {
        "field name", "method name", "function name", "runtime retain count",
    }:
        raise ValidationError("ownership edge classifier prohibition changed")

    views = data.get("view_validity_contract")
    if not isinstance(views, dict) or views.get("type") != "XaViewValidityState":
        raise ValidationError("view validity owner changed")
    require_exact_keys(views, {
        "type", "states", "required_fields", "transfer_rules", "join_table", "join_commutative",
        "unknown_precedence", "evaluation_order_rules", "owner_effects", "source_legality_owner",
        "forbidden_legality_owners",
    }, "view validity contract")
    if views.get("states") != [
        {"symbol": symbol, "value": value} for symbol, value in EXPECTED_VIEW_STATES
    ]:
        raise ValidationError("view validity state records changed")
    require_symbol_values(views.get("states"), EXPECTED_VIEW_STATES, "view validity states")
    if views.get("join_table") != EXPECTED_VIEW_JOIN_TABLE or views.get("join_commutative") is not True:
        raise ValidationError("view validity join contract changed")
    if views.get("unknown_precedence") != (
        "UNKNOWN is the top state and absorbs every other join input"
    ):
        raise ValidationError("view validity UNKNOWN precedence changed")
    if views.get("owner_effects") != [
        "PRESERVES_VIEW", "INVALIDATES_VIEW", "UNKNOWN_VIEW_EFFECT",
    ]:
        raise ValidationError("view effect taxonomy changed")
    if set(require_text_list(views, "required_fields", "view validity contract")) != {
        "view value/binding id", "canonical origin RootId set", "validity state",
        "creation/rebind site", "first invalidation site", "invalidation reason", "escape bit",
        "capture bit", "call-active bit", "complete", "unknown reason",
    }:
        raise ValidationError("view validity required fields changed")
    if set(require_text_list(views, "transfer_rules", "view validity contract")) != {
        "create or rebind from an owner or proven view creates a LIVE version with a canonical origin set",
        "read or project a LIVE view preserves its state and origin set",
        "use of INVALIDATED, MAYBE_INVALIDATED, or UNKNOWN rejects",
        "owner-preserving action leaves view state unchanged",
        "owner-invalidating action atomically invalidates every tracked local view with an intersecting origin set",
        "overwriting or leaving scope makes only that view version unreachable",
        "an active, escaped, captured, or foreign-unknown view blocks owner invalidation",
    }:
        raise ValidationError("view validity transfer rules changed")
    if views.get("evaluation_order_rules") != [
        "data.push(len(view)) completes the READ call before receiver invalidation and is allowed",
        "mutate(ref data, view) holds overlapping REF and view facts in one call and is rejected",
        "owner move rebind or destroy invalidates tracked views before owner state commits",
        "any failed check commits neither owner state nor view state",
        "READ REF and immediate non-retaining closure loans end at the enclosing call return",
    ]:
        raise ValidationError("view evaluation-order contract changed")
    if views.get("source_legality_owner") != "forward validity state machine and typed invalidation effect":
        raise ValidationError("view source-legality owner changed")
    if set(require_text_list(views, "forbidden_legality_owners", "view validity contract")) != {
        "AST last-use", "CFG future-use scan", "fixed lexical block lifetime", "runtime capacity",
    }:
        raise ValidationError("view source-legality prohibition changed")

    semantic_plan = data.get("semantic_plan_contract")
    if not isinstance(semantic_plan, dict):
        raise ValidationError("SemanticPlan contract must be an object")
    require_exact_keys(semantic_plan, {
        "required_facts", "domain_plan_required_facts", "required_types", "atomicity_invariant",
        "independent_checker_obligations", "fingerprint_role",
    }, "SemanticPlan contract")
    if set(require_text_list(semantic_plan, "required_facts", "SemanticPlan contract")) != EXPECTED_SEMANTIC_FACTS:
        raise ValidationError("SemanticPlan required facts changed")
    if set(require_text_list(semantic_plan, "domain_plan_required_facts", "SemanticPlan contract")) != {
        "domain id", "root value id", "transfer state and first witness", "allocation sites",
        "internal edge closure", "outbound const/sync/weak/foreign edges", "drop/finalizer ledger",
        "storage domain", "transfer capability", "complete and unknown reasons",
    }:
        raise ValidationError("ownership domain plan facts changed")
    if semantic_plan.get("required_types") != [
        "XaDomainTransferState", "XaOwnershipEdgeKind", "XaOwnershipEdge[]",
        "XaOwnershipDomainPlan", "XaDomainAdoptionPlan", "XaViewFact", "XaOwnerViewIndex",
        "XaCallBoundLoan",
    ]:
        raise ValidationError("ownership plan type set changed")
    if semantic_plan.get("independent_checker_obligations") != [
        "borrowed return origin set is nonempty sorted deduplicated and ordinal-bounded",
        "parameter and receiver origins are READ and type eligible",
        "every return provenance is a member of the normalized origin set",
        "each invalidation and its owner mutation or move share one plan node",
        "every intersecting local view has a validity transition",
        "active escaped and unknown views are never silently marked dead",
        "every view join is a row in the frozen validity join table",
    ]:
        raise ValidationError("SemanticPlan independent checker obligations changed")
    if semantic_plan.get("atomicity_invariant") != (
        "owner invalidation and every intersecting local-view transition are one semantic action"
    ):
        raise ValidationError("ownership invalidation atomicity changed")
    if semantic_plan.get("fingerprint_role") != (
        "byte integrity only; structural and semantic verification remains mandatory"
    ):
        raise ValidationError("SemanticPlan fingerprint role changed")
    target_plan = data.get("target_plan_contract")
    if not isinstance(target_plan, dict):
        raise ValidationError("TargetPlan contract must be an object")
    require_exact_keys(target_plan, {"allowed_choices", "forbidden_inference"}, "TargetPlan contract")
    if target_plan.get("allowed_choices") != [
        "target-specific representation", "layout", "slot/call convention", "materialization",
    ]:
        raise ValidationError("TargetPlan choice boundary changed")
    forbidden_target_inference = set(
        require_text_list(target_plan, "forbidden_inference", "TargetPlan contract")
    )
    if forbidden_target_inference != {
        "alias versus owned edge", "receiver mode", "borrowed origin",
        "domain transfer eligibility", "storage domain", "view validity or invalidation",
        "call-bound loan scope", "allocation drop or boundary-transfer action",
        "source legality from backend liveness",
        "ownership from VM, C, or helper names",
    }:
        raise ValidationError("TargetPlan inference boundary changed")


def validate_atomic_cut(data: dict[str, object], inventory: dict[str, object]) -> None:
    require_exact_keys(data, {
        "schema", "activation", "authority", "train_base", "activation_batch", "preparation_order",
        "nodes", "activation_gates", "external_dependencies", "delivery_state",
        "remaining_external_boundary",
    }, "atomic cut")
    if data.get("schema") != "ownership-atomic-cut-staging/1":
        raise ValidationError("atomic cut schema is not exact")
    if data.get("activation") != "staging-only-before-i1-r4":
        raise ValidationError("atomic cut activation boundary changed")
    authority = require_text(data, "authority", "atomic cut")
    if authority != EXPECTED_CUT_AUTHORITY:
        raise ValidationError("atomic cut claims independent activation authority")
    train_base = data.get("train_base")
    if not isinstance(train_base, dict) or train_base != {"commit": TRAIN_COMMIT, "tree": TRAIN_TREE}:
        raise ValidationError("atomic cut train base changed")
    batch = data.get("activation_batch")
    if not isinstance(batch, dict):
        raise ValidationError("atomic activation batch must be an object")
    require_exact_keys(batch, {
        "atomic", "independently_activatable_nodes", "checkpoint", "required_locks",
        "required_outcomes", "forbidden_shortcuts",
    }, "atomic activation batch")
    if batch.get("atomic") is not True or batch.get("independently_activatable_nodes") is not False:
        raise ValidationError("atomic activation was weakened")
    if batch.get("checkpoint") != "I1/R4 clean":
        raise ValidationError("atomic activation checkpoint changed")
    require_locks(batch, ALLOWED_LOCKS, "atomic activation batch")
    if set(require_text_list(batch, "required_outcomes", "atomic activation batch")) != {
        "only the new parser and source spelling remain",
        "stdlib tests examples ports and tooling consumers migrate in the same batch",
        "function type PSC SemanticPlan TargetPlan Xi VM AOT and runtime consume one typed ownership fact chain",
        "cache schema artifact identity contracts generated projections and completion residue update together",
        "every covered old owner is deleted in the same batch after positive and negative evidence passes",
    }:
        raise ValidationError("atomic activation outcome set changed")
    shortcuts = set(require_text_list(batch, "forbidden_shortcuts", "atomic activation batch"))
    if shortcuts != {
        "compatibility parser", "body-inferred receiver", "body-inferred borrowed origin",
        "last-use source legality", "dual summary", "runtime fallback", "schema alias",
        "migration flag", "skip", "allowlist", "disabled verifier",
    }:
        raise ValidationError("atomic activation shortcut prohibition changed")
    if data.get("preparation_order") != EXPECTED_CUT_ORDER:
        raise ValidationError("atomic cut preparation order changed")

    nodes = require_exact_ids(data.get("nodes"), set(EXPECTED_CUT_NODES), "atomic cut nodes")
    node_by_id = {str(node["id"]): node for node in nodes}
    preparation_coverage: list[str] = []
    deletion_coverage: list[str] = []
    negative_coverage: set[str] = set()
    seen: set[str] = set()
    for node_id in EXPECTED_CUT_ORDER:
        node = node_by_id[node_id]
        (
            expected_phase, expected_prerequisites, expected_locks, expected_preparations,
            expected_deletions, expected_reasons,
        ) = EXPECTED_CUT_NODES[node_id]
        require_exact_keys(node, {
            "id", "task_phase", "owned_fact", "prerequisites", "required_locks",
            "protected_surfaces", "positive_gates", "negative_reasons",
            "prepares_replacement_for_rows", "deletes_inventory_rows",
            "required_activation_gate_ids",
        }, f"atomic cut node {node_id}")
        if node.get("task_phase") != expected_phase:
            raise ValidationError(f"atomic cut node {node_id} task phase changed")
        expected_fact, expected_surfaces, expected_gates = EXPECTED_CUT_NODE_DETAILS[node_id]
        if require_text(node, "owned_fact", f"atomic cut node {node_id}") != expected_fact:
            raise ValidationError(f"atomic cut node {node_id} owned fact changed")
        if require_text_list(
            node, "protected_surfaces", f"atomic cut node {node_id}"
        ) != expected_surfaces:
            raise ValidationError(f"atomic cut node {node_id} protected surfaces changed")
        if require_text_list(node, "positive_gates", f"atomic cut node {node_id}") != expected_gates:
            raise ValidationError(f"atomic cut node {node_id} positive gates changed")
        require_text_list(node, "negative_reasons", f"atomic cut node {node_id}")
        prerequisites = node.get("prerequisites")
        if not isinstance(prerequisites, list) or set(prerequisites) != expected_prerequisites:
            raise ValidationError(f"atomic cut node {node_id} prerequisites changed")
        if not set(prerequisites) <= seen:
            raise ValidationError(f"atomic cut node {node_id} has a forward prerequisite")
        require_locks(node, expected_locks, f"atomic cut node {node_id}")
        preparations = node.get("prepares_replacement_for_rows")
        if not isinstance(preparations, list) or set(preparations) != expected_preparations:
            raise ValidationError(f"atomic cut node {node_id} replacement preparation changed")
        deletions = node.get("deletes_inventory_rows")
        if not isinstance(deletions, list) or set(deletions) != expected_deletions:
            raise ValidationError(f"atomic cut node {node_id} deletion coverage changed")
        if set(node["negative_reasons"]) != expected_reasons:
            raise ValidationError(f"atomic cut node {node_id} negative coverage changed")
        required_gate_ids = node.get("required_activation_gate_ids")
        expected_gate_ids = (
            [gate[0] for gate in EXPECTED_ACTIVATION_GATES]
            if node_id == "retirement-contract" else []
        )
        if required_gate_ids != expected_gate_ids:
            raise ValidationError(f"atomic cut node {node_id} activation-gate dependency changed")
        preparation_coverage.extend(str(item) for item in preparations)
        deletion_coverage.extend(str(item) for item in deletions)
        negative_coverage.update(str(item) for item in node["negative_reasons"])
        seen.add(node_id)
    if len(preparation_coverage) != len(set(preparation_coverage)):
        raise ValidationError("an old owner is assigned to multiple replacement preparation nodes")
    if set(preparation_coverage) != set(EXPECTED_ROWS):
        raise ValidationError("atomic cut does not prepare every inventoried replacement exactly once")
    if len(deletion_coverage) != len(set(deletion_coverage)):
        raise ValidationError("an old owner is assigned to multiple deletion nodes")
    if set(deletion_coverage) != set(EXPECTED_ROWS):
        raise ValidationError("atomic cut does not delete every inventoried old owner exactly once")
    expected_reasons = {case[2] for case in EXPECTED_CASES.values()}
    if negative_coverage != expected_reasons:
        raise ValidationError("atomic cut does not cover every staged negative reason")

    inventory_rows_value = inventory.get("rows")
    if not isinstance(inventory_rows_value, list):
        raise ValidationError("inventory rows are unavailable for atomic-cut cross-check")
    inventory_rows = {
        str(row.get("id")): row for row in inventory_rows_value if isinstance(row, dict)
    }
    preparation_node_by_row = {
        row_id: node_id for node_id, node in node_by_id.items()
        for row_id in node["prepares_replacement_for_rows"]
    }
    deletion_node_by_row = {
        row_id: node_id for node_id, node in node_by_id.items()
        for row_id in node["deletes_inventory_rows"]
    }
    for row_id, row in inventory_rows.items():
        preparation_node = node_by_id[preparation_node_by_row[row_id]]
        deletion_node = node_by_id[deletion_node_by_row[row_id]]
        if preparation_node.get("task_phase") != row.get("deletion_phase"):
            raise ValidationError(f"atomic cut row {row_id} preparation phase disagrees with inventory")
        row_locks = row.get("required_locks")
        if not isinstance(row_locks, list):
            raise ValidationError(f"atomic cut row {row_id} inventory locks are invalid")
        preparation_locks = set(preparation_node["required_locks"])
        if not (set(row_locks) - {"LOCK-RESIDUE"}) <= preparation_locks:
            raise ValidationError(f"atomic cut row {row_id} replacement preparation lacks a shared lock")
        if not set(row_locks) <= set(deletion_node["required_locks"]):
            raise ValidationError(f"atomic cut row {row_id} deletion barrier lacks a shared lock")
        if deletion_node.get("id") != "retirement-contract":
            raise ValidationError(f"atomic cut row {row_id} bypasses the retirement deletion barrier")

    expected_gate_records = [
        {
            "id": gate_id, "required_stage": stage, "status": "UNRUN", "evidence": [],
            "blocking_reason": reason,
        }
        for gate_id, stage, reason in EXPECTED_ACTIVATION_GATES
    ]
    if data.get("activation_gates") != expected_gate_records:
        raise ValidationError("activation gate inventory or UNRUN boundary changed")
    if data.get("external_dependencies") != EXPECTED_EXTERNAL_DEPENDENCIES:
        raise ValidationError("atomic cut external dependency boundary changed")
    if data.get("delivery_state") != "NOT_READY":
        raise ValidationError("staging must remain NOT_READY before activation")
    if data.get("remaining_external_boundary") != (
        "I1/R4 clean checkpoint, explicit lock grants, matching default binary, downstream "
        "coordination, and every activation gate still marked UNRUN"
    ):
        raise ValidationError("atomic cut external boundary changed")


def validate(
    inventory: dict[str, object], oracles: dict[str, object], typed_contract: dict[str, object],
    atomic_cut: dict[str, object],
) -> None:
    validate_staging_file_hashes()
    validate_inventory(inventory)
    validate_oracles(oracles)
    validate_typed_contract(typed_contract)
    validate_atomic_cut(atomic_cut, inventory)


def self_test(
    inventory: dict[str, object], oracles: dict[str, object], typed_contract: dict[str, object],
    atomic_cut: dict[str, object],
) -> None:
    for name, expected in EXPECTED_STAGING_SHA256.items():
        try:
            validate_staging_bytes(name, b"altered staging bytes", expected)
        except ValidationError:
            continue
        raise ValidationError(f"self-test byte mutation was accepted: {name}")
    incomplete_hashes = dict(EXPECTED_STAGING_SHA256)
    incomplete_hashes.pop(TYPED_CONTRACT_PATH.name)
    try:
        validate_staging_hash_coverage(incomplete_hashes)
    except ValidationError:
        pass
    else:
        raise ValidationError("self-test incomplete staging hash coverage was accepted")
    mutations: list[
        tuple[str, dict[str, object], dict[str, object], dict[str, object], dict[str, object]]
    ] = []
    missing_row = copy.deepcopy(inventory)
    missing_row["rows"] = missing_row["rows"][:-1]
    mutations.append((
        "missing inventory row", missing_row, copy.deepcopy(oracles), copy.deepcopy(typed_contract),
        copy.deepcopy(atomic_cut),
    ))
    stale_anchor = copy.deepcopy(inventory)
    stale_anchor["rows"][0]["producers"][0]["anchor"] = "definitely-not-a-source-anchor"
    mutations.append((
        "stale source anchor", stale_anchor, copy.deepcopy(oracles), copy.deepcopy(typed_contract),
        copy.deepcopy(atomic_cut),
    ))
    weak_lock = copy.deepcopy(inventory)
    weak_lock["rows"][2]["required_locks"] = ["LOCK-SCHEMA"]
    mutations.append((
        "weakened inventory lock", weak_lock, copy.deepcopy(oracles), copy.deepcopy(typed_contract),
        copy.deepcopy(atomic_cut),
    ))
    missing_case = copy.deepcopy(oracles)
    missing_case["cases"] = missing_case["cases"][:-1]
    mutations.append((
        "missing oracle case", copy.deepcopy(inventory), missing_case, copy.deepcopy(typed_contract),
        copy.deepcopy(atomic_cut),
    ))
    wrong_reason = copy.deepcopy(oracles)
    wrong_reason["cases"][0]["activation_reason"] = "OWN-E-WRONG"
    mutations.append((
        "wrong oracle reason", copy.deepcopy(inventory), wrong_reason, copy.deepcopy(typed_contract),
        copy.deepcopy(atomic_cut),
    ))
    swapped_fixture = copy.deepcopy(oracles)
    swapped_fixture["cases"][0]["fixture"] = swapped_fixture["cases"][1]["fixture"]
    mutations.append((
        "swapped oracle fixture", copy.deepcopy(inventory), swapped_fixture,
        copy.deepcopy(typed_contract), copy.deepcopy(atomic_cut),
    ))
    weakened_case = copy.deepcopy(oracles)
    weakened_case["cases"][0]["baseline"] = "expected-parser-error with fallback"
    mutations.append((
        "weakened oracle", copy.deepcopy(inventory), weakened_case, copy.deepcopy(typed_contract),
        copy.deepcopy(atomic_cut),
    ))
    weakened_fixture = copy.deepcopy(oracles)
    weakened_fixture["cases"][0]["sha256"] = "0" * 64
    mutations.append((
        "weakened fixture content", copy.deepcopy(inventory), weakened_fixture,
        copy.deepcopy(typed_contract), copy.deepcopy(atomic_cut),
    ))
    missing_harnesses = copy.deepcopy(oracles)
    missing_harnesses["existing_harnesses"] = []
    mutations.append((
        "missing harness inventory", copy.deepcopy(inventory), missing_harnesses,
        copy.deepcopy(typed_contract), copy.deepcopy(atomic_cut),
    ))
    recovery_backedge = copy.deepcopy(typed_contract)
    recovery_backedge["domain_transfer_contract"]["allowed_transitions"].append(
        "EXTERNAL_ALIASED->CANDIDATE"
    )
    mutations.append((
        "domain recovery backedge", copy.deepcopy(inventory), copy.deepcopy(oracles),
        recovery_backedge, copy.deepcopy(atomic_cut),
    ))
    missing_receiver_mode = copy.deepcopy(typed_contract)
    missing_receiver_mode["receiver_contract"]["modes"] = ["READ", "REF"]
    mutations.append((
        "missing receiver mode", copy.deepcopy(inventory), copy.deepcopy(oracles),
        missing_receiver_mode, copy.deepcopy(atomic_cut),
    ))
    read_can_escape = copy.deepcopy(typed_contract)
    read_can_escape["parameter_modes"][0]["maximum_capability"] = "retain escape and suspend"
    mutations.append((
        "expanded READ capability", copy.deepcopy(inventory), copy.deepcopy(oracles),
        read_can_escape, copy.deepcopy(atomic_cut),
    ))
    body_authority = copy.deepcopy(typed_contract)
    body_authority["receiver_contract"]["body_role"] = "infer receiver authority"
    mutations.append((
        "receiver body authority", copy.deepcopy(inventory), copy.deepcopy(oracles),
        body_authority, copy.deepcopy(atomic_cut),
    ))
    worker_activation = copy.deepcopy(typed_contract)
    worker_activation["authority"] = (
        "non-authoritative integration train optional; worker may activate after local checks"
    )
    mutations.append((
        "worker activation authority", copy.deepcopy(inventory), copy.deepcopy(oracles),
        worker_activation, copy.deepcopy(atomic_cut),
    ))
    body_elision = copy.deepcopy(typed_contract)
    body_elision["borrow_origin_contract"]["elision"]["one_signature_candidate"] = (
        "scan the body and normalize to its origin"
    )
    mutations.append((
        "body-inferred origin elision", copy.deepcopy(inventory), copy.deepcopy(oracles),
        body_elision, copy.deepcopy(atomic_cut),
    ))
    concealed_return = copy.deepcopy(typed_contract)
    concealed_return["borrow_origin_contract"]["return_surface_constraints"].pop()
    mutations.append((
        "weakened borrowed return surface", copy.deepcopy(inventory), copy.deepcopy(oracles),
        concealed_return, copy.deepcopy(atomic_cut),
    ))
    weak_internal_edge = copy.deepcopy(typed_contract)
    weak_internal_edge["ownership_edge_contract"]["internal_owned_criteria"] = []
    mutations.append((
        "weakened internal owned edge", copy.deepcopy(inventory), copy.deepcopy(oracles),
        weak_internal_edge, copy.deepcopy(atomic_cut),
    ))
    active_view_dies = copy.deepcopy(typed_contract)
    active_view_dies["view_validity_contract"]["transfer_rules"][-1] = (
        "an escaped captured or foreign-unknown view blocks owner invalidation"
    )
    mutations.append((
        "active view silently invalidated", copy.deepcopy(inventory), copy.deepcopy(oracles),
        active_view_dies, copy.deepcopy(atomic_cut),
    ))
    ambiguous_join = copy.deepcopy(typed_contract)
    ambiguous_join["view_validity_contract"]["join_table"][11]["result"] = "MAYBE_INVALIDATED"
    mutations.append((
        "ambiguous UNKNOWN view join", copy.deepcopy(inventory), copy.deepcopy(oracles),
        ambiguous_join, copy.deepcopy(atomic_cut),
    ))
    weak_plan_checker = copy.deepcopy(typed_contract)
    weak_plan_checker["semantic_plan_contract"]["independent_checker_obligations"] = []
    mutations.append((
        "weakened SemanticPlan checker", copy.deepcopy(inventory), copy.deepcopy(oracles),
        weak_plan_checker, copy.deepcopy(atomic_cut),
    ))
    target_reinfers = copy.deepcopy(typed_contract)
    target_reinfers["target_plan_contract"]["forbidden_inference"].remove("receiver mode")
    mutations.append((
        "TargetPlan receiver inference", copy.deepcopy(inventory), copy.deepcopy(oracles),
        target_reinfers, copy.deepcopy(atomic_cut),
    ))
    extra_typed_owner = copy.deepcopy(typed_contract)
    extra_typed_owner["dual_read"] = True
    mutations.append((
        "extra typed authority", copy.deepcopy(inventory), copy.deepcopy(oracles),
        extra_typed_owner, copy.deepcopy(atomic_cut),
    ))
    missing_deletion = copy.deepcopy(atomic_cut)
    missing_deletion["nodes"][-1]["deletes_inventory_rows"].pop()
    mutations.append((
        "missing old-owner deletion", copy.deepcopy(inventory), copy.deepcopy(oracles),
        copy.deepcopy(typed_contract), missing_deletion,
    ))
    early_deletion = copy.deepcopy(atomic_cut)
    early_deletion["nodes"][-1]["deletes_inventory_rows"].remove("read-retain-return-alias")
    early_deletion["nodes"][1]["deletes_inventory_rows"].append("read-retain-return-alias")
    mutations.append((
        "early old-owner deletion", copy.deepcopy(inventory), copy.deepcopy(oracles),
        copy.deepcopy(typed_contract), early_deletion,
    ))
    missing_residue_lock = copy.deepcopy(atomic_cut)
    missing_residue_lock["nodes"][-1]["required_locks"].remove("LOCK-RESIDUE")
    mutations.append((
        "deletion barrier without residue lock", copy.deepcopy(inventory), copy.deepcopy(oracles),
        copy.deepcopy(typed_contract), missing_residue_lock,
    ))
    wrong_preparation_phase = copy.deepcopy(atomic_cut)
    wrong_preparation_phase["nodes"][5]["task_phase"] = "P7"
    mutations.append((
        "wrong replacement preparation phase", copy.deepcopy(inventory), copy.deepcopy(oracles),
        copy.deepcopy(typed_contract), wrong_preparation_phase,
    ))
    weakened_node_reason = copy.deepcopy(atomic_cut)
    weakened_node_reason["nodes"][0]["negative_reasons"] = ["OWN-E-READ-ESCAPE"]
    mutations.append((
        "weakened node-specific reason", copy.deepcopy(inventory), copy.deepcopy(oracles),
        copy.deepcopy(typed_contract), weakened_node_reason,
    ))
    placeholder_gate = copy.deepcopy(atomic_cut)
    placeholder_gate["nodes"][6]["positive_gates"] = ["placeholder"]
    mutations.append((
        "placeholder execution gate", copy.deepcopy(inventory), copy.deepcopy(oracles),
        copy.deepcopy(typed_contract), placeholder_gate,
    ))
    false_gate_pass = copy.deepcopy(atomic_cut)
    false_gate_pass["activation_gates"][0]["status"] = "PASS"
    false_gate_pass["activation_gates"][0]["evidence"] = ["stale binary"]
    mutations.append((
        "false activation gate pass", copy.deepcopy(inventory), copy.deepcopy(oracles),
        copy.deepcopy(typed_contract), false_gate_pass,
    ))
    missing_retirement_gate = copy.deepcopy(atomic_cut)
    missing_retirement_gate["nodes"][-1]["required_activation_gate_ids"].remove(
        "aot-ubsan-generated-output"
    )
    mutations.append((
        "retirement without generated-output UBSan", copy.deepcopy(inventory),
        copy.deepcopy(oracles), copy.deepcopy(typed_contract), missing_retirement_gate,
    ))
    dual_runtime = copy.deepcopy(atomic_cut)
    dual_runtime["activation_batch"]["forbidden_shortcuts"].remove("runtime fallback")
    mutations.append((
        "runtime fallback shortcut", copy.deepcopy(inventory), copy.deepcopy(oracles),
        copy.deepcopy(typed_contract), dual_runtime,
    ))
    partial_activation = copy.deepcopy(atomic_cut)
    partial_activation["activation_batch"]["independently_activatable_nodes"] = True
    mutations.append((
        "partial activation", copy.deepcopy(inventory), copy.deepcopy(oracles),
        copy.deepcopy(typed_contract), partial_activation,
    ))
    node_activation_authority = copy.deepcopy(atomic_cut)
    node_activation_authority["authority"] = (
        "non-authoritative integration-train ledger; nodes may activate independently"
    )
    mutations.append((
        "independent node authority", copy.deepcopy(inventory), copy.deepcopy(oracles),
        copy.deepcopy(typed_contract), node_activation_authority,
    ))
    for label, mutated_inventory, mutated_oracles, mutated_typed, mutated_cut in mutations:
        try:
            validate(mutated_inventory, mutated_oracles, mutated_typed, mutated_cut)
        except ValidationError:
            continue
        raise ValidationError(f"self-test mutation was accepted: {label}")


def main(argv: list[str]) -> int:
    try:
        validate_checkout("--require-clean" in argv)
        inventory = load_json(INVENTORY_PATH)
        oracles = load_json(ORACLES_PATH)
        typed_contract = load_json(TYPED_CONTRACT_PATH)
        atomic_cut = load_json(ATOMIC_CUT_PATH)
        validate(inventory, oracles, typed_contract, atomic_cut)
        if "--self-test" in argv:
            self_test(inventory, oracles, typed_contract, atomic_cut)
    except ValidationError as exc:
        print(f"ownership source-contract staging: FAIL: {exc}", file=sys.stderr)
        return 1
    suffix = " with mutation self-test" if "--self-test" in argv else ""
    print(
        f"ownership source-contract staging: PASS: {len(EXPECTED_ROWS)} owner rows, "
        f"{len(EXPECTED_CASES)} negative oracles, {len(EXPECTED_CUT_ORDER)} atomic-cut nodes{suffix}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

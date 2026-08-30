#!/usr/bin/env python3
"""Validate language-cut preparation inventories against exact Git trees."""

from __future__ import annotations

import argparse
import csv
import fnmatch
import hashlib
import json
import re
import subprocess
from collections import Counter
from pathlib import Path

import inventory_enum_migration as enum_inventory
import inventory_language_cut_residue as residue_inventory


FILE_PATTERN = re.compile(
    r"(?:[A-Za-z0-9_.-]+/)*[A-Za-z0-9_.-]+\.(?:c|h|py|md|def|xr)"
)
SPECIAL_OWNER_ANCHORS = {
    ("enum", "psc"): ("src/frontend/analyzer/xa_program_semantic_closure.c",),
    ("enum", "target-contract"): ("src/plan/target/xr_target_plan.h",),
    ("all", "root-registration"): ("CMakeLists.txt",),
}
FROZEN_PROVENANCE = {
    "frontier_count": 753,
    "accepted_count": 739,
    "rejected_count": 14,
    "accepted_identity_sha256":
        "75a38bf07e41c4a9e488caefef244ac648900028ee69a721df2c2573518abaf7",
}
EXPECTED_SNAPSHOT = "49f71d1ea0f8e12aa2af8e64fb5d8d1c6f3ee79c"
EXPECTED_SNAPSHOT_TREE = "478e72a21e54ae5cc182caaa77cf9e526725a0a6"
EXPECTED_DELIVERY = "eabe56cc986cf2147d02254ae1b1acaa1418663f"
EXPECTED_DELIVERY_TREE = "45f708658f0945bf3f588be332859d3ecde58782"
EXPECTED_CHANGED_PATHS = frozenset(
    {
        "blockers/r3-6-5-aot-regex-runtime-has-no-xray-backed-path.md",
        "contracts/assertion-semantics.md",
        "contracts/differential-protocol.md",
        "contracts/effect-semantics.md",
        "contracts/incremental-cache-store.md",
        "contracts/memory-model.md",
        "contracts/print-semantics.md",
        "contracts/program-semantic-closure.md",
        "contracts/rc-contract.md",
        "contracts/runtime-target-plan-load.md",
        "contracts/semantic-owner-registry.json",
        "contracts/semantic-owners.toml",
        "contracts/semantic-ownership.md",
        "contracts/shared-core-inventory.json",
        "contracts/structural-object-json-map-boundary.md",
        "contracts/target-abi.md",
        "contracts/target-machine/legacy-vm-inventory.json",
        "contracts/target-machine/object-extent-inventory.json",
        "contracts/target-machine/semantic-authority-manifest.json",
        "contracts/target-machine/semantic-owner-inventory.json",
        "contracts/typed-target-plan-execution.md",
        "contracts/unified-target-machine-discovery.md",
        "contracts/xi-canonical-ops.md",
        "contracts/zero-cost-residue.md",
        "scripts/check_binary_stdlib_runtime_baseline.py",
        "scripts/check_live_refusal_manifest.py",
        "scripts/check_runtime_header_dependencies.py",
        "scripts/check_semantic_owners.py",
        "scripts/check_stdlib_full_xray_completion.py",
        "scripts/check_target_machine_completion.py",
        "scripts/check_target_machine_migration_classification.py",
        "scripts/report_stdlib_self_hosting.py",
        "scripts/stdlib_migration.py",
        "scripts/target_machine_phase0.py",
        "src/analysis/xglobal_producer.c",
        "src/aot/xi_cgen.c",
        "src/aot/xi_cgen_dispatch_helpers.inc.c",
        "src/aot/xr_leaf_value_product_program_emission.h",
        "src/aot/xrt.h",
        "src/aot/xrt_arc.h",
        "src/aot/xrt_coll.h",
        "src/aot/xrt_regex.h",
        "src/frontend/analyzer/xanalyzer_builtins_generated.h",
        "src/ir/xi_ops_gen.h",
        "src/plan/semantic/xr_semantic_ops_gen.h",
        "src/runtime/xstdlib_bridge.h",
        "src/shared/xr_regex_core.h",
        "src/shared/xr_semantic_owner_ids_gen.h",
        "src/stdlib/xstdlib_defs_generated.h",
        "src/vm/xvm.c",
        "src/vm/xvm_dispatch_assert.inc.c",
        "stdlib/defs/core.def",
        "stdlib/native_leaf_allowlist.toml",
        "stdlib/regex/regex.xr",
        "stdlib/regex/xregex_binding.c",
        "tests/aot/filetest_expect.py",
        "tests/aot/filetests/README.md",
        "tests/aot/filetests/link/core_regex.expect",
        "tests/aot/filetests/link/no_alloc_regex_scalar_methods_ok.expect",
        "tests/aot/filetests/link/regex_literal_no_aot_owner.expect",
        "tests/aot/filetests/link/regex_literal_no_aot_owner.xr",
        "tests/aot/run_aot_filetests.py",
        "tests/aot/run_freestanding_provider_abi_test.py",
        "tests/benchmarks/stdlib/manifest.toml",
        "tests/benchmarks/stdlib/run.py",
        "tests/diff/cases/semantics/stdlib/regex_escape_direct.xr",
        "tests/diff/cases/semantics/stdlib/regex_escape_direct.xr.expected",
        "tests/diff/survey_refusals.py",
        "tests/lib/tests/test_binary.py",
        "tests/lib/tests/test_filetests.py",
        "tests/lib/xraytest/binary.py",
        "tests/regression/10_stdlib/1131_regex_native_field_ownership.xr",
        "tests/stdlib/contracts/regex/cases.jsonl",
        "tests/stdlib/contracts/regex/contract.toml",
        "tests/unit/analysis/test_xglobal_summary.c",
        "tests/unit/ir/test_xi_cgen.c",
        "tests/unit/ir/test_xi_stage.c",
        "tests/unit/plan/test_semantic_plan.c",
        "tests/unit/plan/test_target_plan.c",
        "tests/unit/stdlib/test_stdlib_boundary_manifest.py",
        "tools/xisagen/xisagen.py",
        "xisa/xi/ops.def",
    }
)
EXPECTED_CHANGED_CONTRACT_INPUTS = frozenset(
    {
        "contracts/assertion-semantics.md",
        "contracts/differential-protocol.md",
        "contracts/effect-semantics.md",
        "contracts/memory-model.md",
        "contracts/print-semantics.md",
        "contracts/program-semantic-closure.md",
        "contracts/rc-contract.md",
        "contracts/runtime-target-plan-load.md",
        "contracts/structural-object-json-map-boundary.md",
        "contracts/target-abi.md",
        "contracts/typed-target-plan-execution.md",
    }
)
EXPECTED_GOVERNED_DELTA = EXPECTED_CHANGED_CONTRACT_INPUTS | frozenset(
    {
        "src/analysis/xglobal_producer.c",
        "src/aot/xi_cgen.c",
        "src/aot/xi_cgen_dispatch_helpers.inc.c",
        "src/aot/xr_leaf_value_product_program_emission.h",
        "src/aot/xrt.h",
        "src/aot/xrt_arc.h",
        "src/aot/xrt_coll.h",
        "src/aot/xrt_regex.h",
        "src/frontend/analyzer/xanalyzer_builtins_generated.h",
        "src/ir/xi_ops_gen.h",
        "src/plan/semantic/xr_semantic_ops_gen.h",
        "src/runtime/xstdlib_bridge.h",
        "src/shared/xr_regex_core.h",
        "src/shared/xr_semantic_owner_ids_gen.h",
        "src/stdlib/xstdlib_defs_generated.h",
        "src/vm/xvm.c",
        "src/vm/xvm_dispatch_assert.inc.c",
        "stdlib/regex/regex.xr",
        "tests/aot/filetests/link/regex_literal_no_aot_owner.xr",
        "tests/diff/cases/semantics/stdlib/regex_escape_direct.xr",
        "tests/regression/10_stdlib/1131_regex_native_field_ownership.xr",
    }
)
EXPECTED_ENUM_REPORT_SHA256 = {
    "snapshot": "d7a978a782105dceef0d97464607c9eebf4c58e0eff7d3cebf34fcfeec755548",
    "delivery": "d05e2f0ab80784632f2a8738a9da97f86ee2509df68f2ac21b0d90110d87934e",
}
EXPECTED_ENUM_DELIVERY_FILE_COUNT = 3556
EXPECTED_ENUM_DELIVERY_FILE_COUNT_BY_ROOT = {
    "bench": 15,
    "demos": 21,
    "stdlib": 54,
    "tests": 3466,
}
EXPECTED_ENUM_DELIVERY_EXCLUDED_CANDIDATES = {
    "import->missing-export": 217,
    "import->reexport->missing-export": 2,
    "non-enum-type-shadow": 122,
    "resolved-enum-nonpayload-member": 2,
    "unresolved-module-name": 8732,
    "value-shadow": 7321,
}
EXPECTED_ENUM_CHANGED_USE_COUNT = 3
EXPECTED_ENUM_CHANGED_USE_PATHS = frozenset(
    {
        "stdlib/regex/regex.xr",
        "tests/regression/10_stdlib/1101_regex_engine.xr",
    }
)
FROZEN_OWNER_SHA256 = "6e710400fd7780e553cec254ce451d06b99d0737cb3c8fcb51434826c419e6ba"
FROZEN_CONTRACT_SHA256 = "caf2bb5aa5784d4b9e5d6d76006553fdf6c228f3010f6810f45b37d4d3cf9088"


def revision_paths(root: Path, revision: str) -> list[str]:
    output = enum_inventory.git_output(root, "ls-tree", "-r", "--name-only", revision)
    return [line for line in output.splitlines() if line]


def resolve_path_spec(spec: str, paths: list[str]) -> list[str]:
    if spec in paths:
        return [spec]
    if "*" in spec:
        matched = [path for path in paths if fnmatch.fnmatch(path, spec)]
        if matched:
            return matched
    basename = Path(spec).name
    matched = [path for path in paths if Path(path).name == basename]
    if len(matched) == 1:
        return matched
    return []


def extract_row_paths(row: dict[str, str], paths: list[str]) -> set[str]:
    resolved: set[str] = set()
    text = f"{row['owner_or_symbol']} {row['primary_consumers']}"
    for spec in FILE_PATTERN.findall(text):
        resolved.update(resolve_path_spec(spec, paths))
    wildcard = re.search(r"([A-Za-z0-9_./-]+\.\*)", text)
    if wildcard:
        prefix = wildcard.group(1)[:-1]
        resolved.update(path for path in paths if path.startswith(prefix))
    resolved.update(SPECIAL_OWNER_ANCHORS.get((row["surface"], row["layer"]), ()))
    return resolved


def primary_owner_symbols(row: dict[str, str]) -> tuple[str | None, list[str]]:
    owner = row["owner_or_symbol"]
    if ":" not in owner:
        return None, []
    path_spec, symbol_text = owner.split(":", 1)
    path_spec = path_spec.split(",", 1)[0]
    symbols = [symbol.rsplit(".", 1)[-1] for symbol in symbol_text.split(",")]
    return path_spec, [symbol for symbol in symbols if symbol]


def validate_owner_map(
    root: Path, revision: str, owner_map: Path
) -> tuple[dict[str, object], set[str]]:
    actual_digest = hashlib.sha256(owner_map.read_bytes()).hexdigest()
    if actual_digest != FROZEN_OWNER_SHA256:
        raise SystemExit(
            "owner inventory validation failed:\n  "
            f"reviewed TSV sha256 {actual_digest} != frozen {FROZEN_OWNER_SHA256}"
        )
    paths = revision_paths(root, revision)
    with owner_map.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream, delimiter="\t"))
    required = {
        "surface", "layer", "owner_or_symbol", "primary_consumers", "lock",
        "planned_action", "preparation_status",
    }
    errors: list[str] = []
    if not rows or set(rows[0]) != required:
        errors.append("owner map columns do not match the required schema")
    keys: set[tuple[str, str]] = set()
    input_paths: set[str] = set()
    anchor_rows: list[dict[str, object]] = []
    for row_number, row in enumerate(rows, 2):
        key = (row["surface"], row["layer"])
        if key in keys:
            errors.append(f"row {row_number}: duplicate surface/layer key {key}")
        keys.add(key)
        anchors = extract_row_paths(row, paths)
        if not anchors:
            errors.append(f"row {row_number}: no exact-revision source anchor resolved")
        input_paths.update(anchors)
        path_spec, symbols = primary_owner_symbols(row)
        symbol_path: str | None = None
        if path_spec is not None:
            candidates = resolve_path_spec(path_spec, paths)
            if len(candidates) != 1:
                errors.append(
                    f"row {row_number}: primary owner path {path_spec!r} is not unique"
                )
            else:
                symbol_path = candidates[0]
                source = enum_inventory.load_revision_text(root, revision, symbol_path)
                for symbol in symbols:
                    if re.search(rf"\b{re.escape(symbol)}\b", source) is None:
                        errors.append(
                            f"row {row_number}: symbol {symbol!r} absent from {symbol_path}"
                        )
        anchor_rows.append(
            {
                "surface": row["surface"],
                "layer": row["layer"],
                "anchors": sorted(anchors),
                "symbol_path": symbol_path,
                "symbols": symbols,
            }
        )
    if errors:
        raise SystemExit("owner inventory validation failed:\n  " + "\n  ".join(errors))
    if len(rows) != 51:
        raise SystemExit(f"owner inventory validation failed:\n  expected 51 rows, found {len(rows)}")
    return {"row_count": len(rows), "rows": anchor_rows}, input_paths


def validate_contract_map(
    root: Path, revision: str, contract_map: Path
) -> tuple[dict[str, object], set[str]]:
    actual_digest = hashlib.sha256(contract_map.read_bytes()).hexdigest()
    if actual_digest != FROZEN_CONTRACT_SHA256:
        raise SystemExit(
            "contract inventory validation failed:\n  "
            f"reviewed TSV sha256 {actual_digest} != frozen {FROZEN_CONTRACT_SHA256}"
        )
    paths = set(revision_paths(root, revision))
    with contract_map.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream, delimiter="\t"))
    required = {"contract", "surfaces", "impact_to_verify", "planned_evidence", "lock", "status"}
    errors: list[str] = []
    if not rows or set(rows[0]) != required:
        errors.append("contract map columns do not match the required schema")
    seen: set[str] = set()
    evidence: list[dict[str, str]] = []
    digest = hashlib.sha256()
    for row_number, row in enumerate(rows, 2):
        path = row["contract"]
        if path in seen:
            errors.append(f"row {row_number}: duplicate contract {path}")
        seen.add(path)
        if path not in paths:
            errors.append(f"row {row_number}: contract is absent at revision: {path}")
            continue
        if row["lock"] != "LOCK-SCHEMA":
            errors.append(f"row {row_number}: contract impact is not protected by LOCK-SCHEMA")
        if row["status"] not in ("impact-only", "conditional-shared-anchor"):
            errors.append(f"row {row_number}: invalid impact status {row['status']!r}")
        if not row["surfaces"] or not row["impact_to_verify"] or not row["planned_evidence"]:
            errors.append(f"row {row_number}: incomplete reviewed impact record")
        source = enum_inventory.load_revision_text(root, revision, path)
        source_digest = hashlib.sha256(source.encode("utf-8")).hexdigest()
        digest.update(path.encode("utf-8"))
        digest.update(b"\0")
        digest.update(source.encode("utf-8"))
        evidence.append({"path": path, "sha256": source_digest})
    if errors:
        raise SystemExit("contract inventory validation failed:\n  " + "\n  ".join(errors))
    if len(rows) != 14:
        raise SystemExit(
            f"contract inventory validation failed:\n  expected 14 rows, found {len(rows)}"
        )
    return {
        "row_count": len(rows),
        "input_sha256": digest.hexdigest(),
        "contracts": evidence,
    }, seen


def comparable_report(report: dict[str, object]) -> dict[str, object]:
    return {key: value for key, value in report.items() if key not in ("revision", "tree")}


def stable_json_sha256(value: object) -> str:
    payload = json.dumps(
        value, ensure_ascii=False, separators=(",", ":"), sort_keys=True
    ).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def enum_use_semantic_identity(use: dict[str, object]) -> tuple[tuple[str, object], ...]:
    positional_keys = {"start", "end", "line", "declaration_line"}
    return tuple(
        (key, use[key]) for key in sorted(use) if key not in positional_keys
    )


def enum_delivery_delta(
    snapshot: dict[str, object], delivery: dict[str, object]
) -> dict[str, object]:
    snapshot_digest = stable_json_sha256(snapshot)
    delivery_digest = stable_json_sha256(delivery)
    digest_errors: list[str] = []
    if snapshot_digest != EXPECTED_ENUM_REPORT_SHA256["snapshot"]:
        digest_errors.append(
            f"snapshot report sha256 {snapshot_digest} != "
            f"{EXPECTED_ENUM_REPORT_SHA256['snapshot']}"
        )
    if delivery_digest != EXPECTED_ENUM_REPORT_SHA256["delivery"]:
        digest_errors.append(
            f"delivery report sha256 {delivery_digest} != "
            f"{EXPECTED_ENUM_REPORT_SHA256['delivery']}"
        )
    if digest_errors:
        raise SystemExit(
            "enum report identity differs from reviewed freeze:\n  "
            + "\n  ".join(digest_errors)
        )

    variable_keys = {
        "file_count",
        "file_count_by_root",
        "excluded_qualified_call_candidates",
        "qualified_uses",
    }
    snapshot_static = {
        key: value for key, value in snapshot.items() if key not in variable_keys
    }
    delivery_static = {
        key: value for key, value in delivery.items() if key not in variable_keys
    }
    if snapshot_static != delivery_static:
        raise SystemExit("enum declarations, payloads, or use totals differ at delivery base")
    if delivery["file_count"] != EXPECTED_ENUM_DELIVERY_FILE_COUNT:
        raise SystemExit("delivery enum source-file count differs from reviewed freeze")
    if delivery["file_count_by_root"] != EXPECTED_ENUM_DELIVERY_FILE_COUNT_BY_ROOT:
        raise SystemExit("delivery enum source-root counts differ from reviewed freeze")
    if (
        delivery["excluded_qualified_call_candidates"]
        != EXPECTED_ENUM_DELIVERY_EXCLUDED_CANDIDATES
    ):
        raise SystemExit("delivery enum excluded-candidate counts differ from reviewed freeze")

    snapshot_uses = snapshot["qualified_uses"]
    delivery_uses = delivery["qualified_uses"]
    if not isinstance(snapshot_uses, list) or not isinstance(delivery_uses, list):
        raise SystemExit("enum report comparison failed: invalid qualified-use shape")
    snapshot_semantic = Counter(enum_use_semantic_identity(use) for use in snapshot_uses)
    delivery_semantic = Counter(enum_use_semantic_identity(use) for use in delivery_uses)
    if snapshot_semantic != delivery_semantic:
        raise SystemExit("delivery enum qualified-use semantic identities differ")

    def full_use_rows(uses: list[dict[str, object]]) -> Counter[str]:
        return Counter(
            json.dumps(use, ensure_ascii=False, separators=(",", ":"), sort_keys=True)
            for use in uses
        )

    snapshot_rows = full_use_rows(snapshot_uses)
    delivery_rows = full_use_rows(delivery_uses)
    removed = snapshot_rows - delivery_rows
    added = delivery_rows - snapshot_rows
    if (
        sum(removed.values()) != EXPECTED_ENUM_CHANGED_USE_COUNT
        or sum(added.values()) != EXPECTED_ENUM_CHANGED_USE_COUNT
    ):
        raise SystemExit("delivery enum source-position delta has an unexpected row count")
    changed_paths = {
        json.loads(row)["path"] for row in set(removed) | set(added)
    }
    require_exact_path_set(
        "enum source-position delta paths",
        changed_paths,
        EXPECTED_ENUM_CHANGED_USE_PATHS,
    )
    return {
        "snapshot_report_sha256": snapshot_digest,
        "delivery_report_sha256": delivery_digest,
        "changed_use_count": EXPECTED_ENUM_CHANGED_USE_COUNT,
        "changed_use_paths": sorted(changed_paths),
    }


def content_digest(root: Path, revision: str, paths: set[str]) -> str:
    tree_rows = enum_inventory.git_output(root, "ls-tree", "-r", revision).splitlines()
    blob_by_path: dict[str, str] = {}
    for row in tree_rows:
        metadata, path = row.split("\t", 1)
        blob_by_path[path] = metadata.rsplit(" ", 1)[-1]
    digest = hashlib.sha256()
    for path in sorted(paths):
        digest.update(path.encode("utf-8"))
        digest.update(b"\0")
        digest.update(blob_by_path[path].encode("ascii"))
        digest.update(b"\n")
    return digest.hexdigest()


def require_exact_path_set(
    label: str, actual: set[str], expected: frozenset[str]
) -> None:
    errors: list[str] = []
    missing = sorted(expected - actual)
    unexpected = sorted(actual - expected)
    if missing:
        errors.append(f"missing: {missing}")
    if unexpected:
        errors.append(f"unexpected: {unexpected}")
    if errors:
        raise SystemExit(f"{label} differs from reviewed freeze:\n  " + "\n  ".join(errors))


def changed_contract_inputs(
    snapshot: dict[str, object], delivery: dict[str, object]
) -> set[str]:
    snapshot_rows = snapshot["contracts"]
    delivery_rows = delivery["contracts"]
    if not isinstance(snapshot_rows, list) or not isinstance(delivery_rows, list):
        raise SystemExit("contract inventory comparison failed: invalid report shape")
    snapshot_by_path = {row["path"]: row["sha256"] for row in snapshot_rows}
    delivery_by_path = {row["path"]: row["sha256"] for row in delivery_rows}
    if snapshot_by_path.keys() != delivery_by_path.keys():
        raise SystemExit("contract inventory paths differ between snapshot and delivery base")
    return {
        path for path, source_sha in snapshot_by_path.items()
        if delivery_by_path[path] != source_sha
    }


def validate_revision(
    root: Path,
    revision: str,
    manifest: Path,
    residue: Path,
    owner_map: Path,
    contract_map: Path,
    provenance: Path | None,
) -> tuple[dict[str, object], set[str]]:
    resolved = enum_inventory.git_output(root, "rev-parse", revision).strip()
    tree = enum_inventory.git_output(root, "rev-parse", f"{resolved}^{{tree}}").strip()
    enum_report = enum_inventory.make_report(root, resolved)
    enum_inventory.validate_manifest(
        enum_report, manifest, enum_inventory.load_reserved_words(root, resolved)
    )
    provenance_report = None
    if provenance is not None:
        provenance_report = enum_inventory.validate_provenance(root, resolved, provenance)
        provenance_report.pop("accepted_uses")
        for key, expected in FROZEN_PROVENANCE.items():
            if provenance_report[key] != expected:
                raise SystemExit(
                    f"enum provenance freeze mismatch: {key}={provenance_report[key]!r}, "
                    f"expected {expected!r}"
                )
    residue_report = residue_inventory.make_report(root, resolved)
    residue_inventory.validate_residue_tsv(residue_report, residue)
    owner_report, owner_paths = validate_owner_map(root, resolved, owner_map)
    contract_report, contract_paths = validate_contract_map(root, resolved, contract_map)
    source_paths = set(enum_inventory.load_sources(root, resolved))
    source_paths.update(
        {
            "src/frontend/lexer/xkeywords.def",
            "src/shared/xr_exact_scalar_registry.def",
            "stdlib/prelude/builtin_symbols.def",
        }
    )
    c_paths = {
        path for path in revision_paths(root, resolved)
        if path.startswith("src/") and path.endswith((".c", ".h"))
    }
    governed_paths = source_paths | c_paths | owner_paths | contract_paths
    return {
        "revision": resolved,
        "tree": tree,
        "enum": comparable_report(enum_report),
        "enum_provenance": provenance_report,
        "residue": comparable_report(residue_report),
        "owner_inventory": owner_report,
        "contract_inventory": contract_report,
        "governed_input_count": len(governed_paths),
        "governed_input_sha256": content_digest(root, resolved, governed_paths),
    }, governed_paths


def run_self_tests() -> None:
    paths = ["src/a/file.c", "src/b/unique.h", "CMakeLists.txt"]
    if resolve_path_spec("unique.h", paths) != ["src/b/unique.h"]:
        raise AssertionError("unique basename resolution failed")
    if resolve_path_spec("src/*/file.c", paths) != ["src/a/file.c"]:
        raise AssertionError("wildcard source-anchor resolution failed")
    if resolve_path_spec("missing.c", paths):
        raise AssertionError("missing source anchor was resolved")
    require_exact_path_set("constructed exact set", {"a", "b"}, frozenset({"a", "b"}))
    for mutant in ({"a"}, {"a", "b", "c"}):
        try:
            require_exact_path_set("constructed mutant set", mutant, frozenset({"a", "b"}))
        except SystemExit:
            continue
        raise AssertionError("path-set mutation was accepted")
    semantic_use = {
        "path": "a.xr",
        "start": 1,
        "end": 2,
        "line": 1,
        "enum": "E",
        "variant": "V",
        "role": "constructor",
        "declaration_path": "a.xr",
        "declaration_line": 4,
        "resolution": "same-module",
    }
    moved_use = dict(semantic_use, start=10, end=11, line=2, declaration_line=5)
    if enum_use_semantic_identity(semantic_use) != enum_use_semantic_identity(moved_use):
        raise AssertionError("enum semantic identity retained source positions")
    changed_use = dict(moved_use, variant="Other")
    if enum_use_semantic_identity(semantic_use) == enum_use_semantic_identity(changed_use):
        raise AssertionError("enum semantic identity ignored a variant change")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--snapshot", required=True, help="frozen inventory revision")
    parser.add_argument("--delivery", required=True, help="delivery-base revision")
    parser.add_argument("--manifest", required=True, help="enum naming manifest TSV")
    parser.add_argument("--provenance", required=True, help="reviewed enum-use provenance TSV")
    parser.add_argument("--residue", required=True, help="legacy residue TSV")
    parser.add_argument("--owner-map", required=True, help="owner-consumer TSV")
    parser.add_argument("--contracts", required=True, help="contract-impact TSV")
    parser.add_argument("--json", action="store_true", help="emit machine-readable evidence")
    parser.add_argument("--self-test", action="store_true", help="run constructed validator tests")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    snapshot_revision = enum_inventory.git_output(root, "rev-parse", args.snapshot).strip()
    delivery_revision = enum_inventory.git_output(root, "rev-parse", args.delivery).strip()
    snapshot_tree = enum_inventory.git_output(
        root, "rev-parse", f"{snapshot_revision}^{{tree}}"
    ).strip()
    delivery_tree = enum_inventory.git_output(
        root, "rev-parse", f"{delivery_revision}^{{tree}}"
    ).strip()
    identity_errors: list[str] = []
    if snapshot_revision != EXPECTED_SNAPSHOT or snapshot_tree != EXPECTED_SNAPSHOT_TREE:
        identity_errors.append(
            f"snapshot is {snapshot_revision}/{snapshot_tree}, expected "
            f"{EXPECTED_SNAPSHOT}/{EXPECTED_SNAPSHOT_TREE}"
        )
    if delivery_revision != EXPECTED_DELIVERY or delivery_tree != EXPECTED_DELIVERY_TREE:
        identity_errors.append(
            f"delivery is {delivery_revision}/{delivery_tree}, expected "
            f"{EXPECTED_DELIVERY}/{EXPECTED_DELIVERY_TREE}"
        )
    ancestry = subprocess.run(
        ["git", "merge-base", "--is-ancestor", snapshot_revision, delivery_revision],
        cwd=root,
        check=False,
    )
    if ancestry.returncode != 0:
        identity_errors.append("frozen snapshot is not an ancestor of delivery base")
    current_parent = enum_inventory.git_output(root, "rev-parse", "HEAD^").strip()
    if current_parent != delivery_revision:
        identity_errors.append(
            f"current preparation parent is {current_parent}, expected delivery {delivery_revision}"
        )
    if identity_errors:
        raise SystemExit("revision identity validation failed:\n  " + "\n  ".join(identity_errors))

    if args.self_test:
        run_self_tests()
        enum_inventory.run_self_tests(
            enum_inventory.load_reserved_words(
                Path(args.root).resolve(),
                enum_inventory.git_output(
                    Path(args.root).resolve(), "rev-parse", args.snapshot
                ).strip(),
            )
        )
        residue_inventory.run_self_tests()
        print("language-cut preparation self-tests: PASS")

    common = (Path(args.manifest), Path(args.residue), Path(args.owner_map), Path(args.contracts))
    snapshot, snapshot_paths = validate_revision(
        root, args.snapshot, *common, Path(args.provenance)
    )
    delivery, delivery_paths = validate_revision(root, args.delivery, *common, None)
    enum_delta = enum_delivery_delta(snapshot["enum"], delivery["enum"])
    if snapshot["residue"] != delivery["residue"]:
        raise SystemExit("language residue differs between frozen snapshot and delivery base")
    if snapshot["owner_inventory"] != delivery["owner_inventory"]:
        raise SystemExit("owner inventory anchors differ between snapshot and delivery base")
    if (
        snapshot["contract_inventory"]["row_count"]
        != delivery["contract_inventory"]["row_count"]
    ):
        raise SystemExit("contract inventory row count differs between snapshot and delivery base")
    contract_delta = changed_contract_inputs(
        snapshot["contract_inventory"], delivery["contract_inventory"]
    )
    require_exact_path_set(
        "changed contract inputs", contract_delta, EXPECTED_CHANGED_CONTRACT_INPUTS
    )
    changed = set(
        enum_inventory.git_output(
            root, "diff", "--name-only", snapshot["revision"], delivery["revision"]
        ).splitlines()
    )
    require_exact_path_set("delivery changed paths", changed, EXPECTED_CHANGED_PATHS)
    governed_delta = changed & (snapshot_paths | delivery_paths)
    require_exact_path_set(
        "delivery governed delta", governed_delta, EXPECTED_GOVERNED_DELTA
    )
    report = {
        "schema_version": 3,
        "snapshot": snapshot,
        "delivery": delivery,
        "enum_delivery_delta": enum_delta,
        "changed_paths": sorted(changed),
        "changed_contract_input_paths": sorted(contract_delta),
        "governed_delta_paths": sorted(governed_delta),
    }
    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print(f"snapshot: {snapshot['revision']} tree={snapshot['tree']}")
        print(f"delivery: {delivery['revision']} tree={delivery['tree']}")
        print(f"owner rows: {delivery['owner_inventory']['row_count']}")
        print(f"contract rows: {delivery['contract_inventory']['row_count']}")
        print(
            "enum report sha256: "
            f"{enum_delta['snapshot_report_sha256']} -> "
            f"{enum_delta['delivery_report_sha256']}"
        )
        print(
            "enum moved use rows: "
            f"{enum_delta['changed_use_count']} in {enum_delta['changed_use_paths']}"
        )
        print(f"governed inputs: {delivery['governed_input_count']}")
        print(f"snapshot governed input sha256: {snapshot['governed_input_sha256']}")
        print(f"delivery governed input sha256: {delivery['governed_input_sha256']}")
        print(
            "snapshot contract input sha256: "
            f"{snapshot['contract_inventory']['input_sha256']}"
        )
        print(
            "delivery contract input sha256: "
            f"{delivery['contract_inventory']['input_sha256']}"
        )
        print(f"changed paths: {sorted(changed)}")
        print(f"changed contract input paths: {sorted(contract_delta)}")
        print(f"governed delta paths: {sorted(governed_delta)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

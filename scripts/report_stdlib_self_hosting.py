#!/usr/bin/env python3
"""Emit the source-derived stdlib governance report.

What this report checks is agreement: every module's contracts, oracles,
benchmarks and declared surface match the policy recorded against it in the
boundary manifest. A module implemented entirely in C agrees with a policy
that says so, so agreement holds while no public semantics are owned by Xray
source at all.

Whether the standard library owns its public semantics in Xray is a different
question, decided per symbol rather than per module, and answered by
`check_stdlib_full_xray_completion.py`. Neither report subsumes the other.
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any

from check_stdlib_boundary import (
    check_dynamic,
    check_error_model_policy,
    check_fastpaths,
    check_manifest,
    check_semantic_owners,
)
from stdlib_manifest import api_inventory, load_manifest, load_toml
from stdlib_migration import contract_modules, validate_contract


def benchmark_backend_agreement(
    contracts: list[dict[str, Any]], benchmarks: list[dict[str, Any]]
) -> list[dict[str, Any]]:
    """Return exact benchmark rows whose backend set disagrees with its contract."""
    contract_backends = {
        str(contract["module"]): list(contract.get("backends", ["vm", "aot"]))
        for contract in contracts
    }
    mismatches: list[dict[str, Any]] = []
    for benchmark in benchmarks:
        module = str(benchmark.get("module", ""))
        declared = benchmark.get("compare")
        expected = contract_backends.get(module)
        if expected is None or declared != expected:
            mismatches.append(
                {
                    "benchmark": str(benchmark.get("id", "")),
                    "module": module,
                    "contract_backends": expected,
                    "benchmark_backends": declared,
                }
            )
    return mismatches


def benchmark_oracle_agreement(
    root: Path, contracts: list[dict[str, Any]], benchmarks: list[dict[str, Any]]
) -> list[dict[str, Any]]:
    """Prove each VM-only benchmark uses its contract case's byte-exact oracle."""
    contracts_by_module = {str(contract["module"]): contract for contract in contracts}
    mismatches: list[dict[str, Any]] = []
    for benchmark in benchmarks:
        if benchmark.get("compare") != ["vm"]:
            continue
        module = str(benchmark.get("module", ""))
        source = str(benchmark.get("source", ""))
        oracle = benchmark.get("output_oracle")
        contract = contracts_by_module.get(module, {})
        manifest_value = contract.get("diff_cases_manifest")
        reasons: list[str] = []
        cases: set[str] = set()
        if not isinstance(manifest_value, str) or not manifest_value:
            reasons.append("contract has no diff_cases_manifest")
        else:
            manifest_path = root / manifest_value
            try:
                cases = {
                    line.strip()
                    for line in manifest_path.read_text(encoding="utf-8").splitlines()
                    if line.strip() and not line.lstrip().startswith("#")
                }
            except OSError as exc:
                reasons.append(f"cannot read contract diff manifest: {exc}")
        if source not in cases:
            reasons.append("benchmark source is absent from contract diff manifest")
        expected_oracle = f"{source}.expected"
        if oracle != expected_oracle:
            reasons.append("benchmark output_oracle is not the source's .expected oracle")
        elif not (root / expected_oracle).is_file():
            reasons.append("benchmark output_oracle does not exist")
        if reasons:
            mismatches.append(
                {
                    "benchmark": str(benchmark.get("id", "")),
                    "module": module,
                    "contract_diff_cases_manifest": manifest_value,
                    "source": source,
                    "output_oracle": oracle,
                    "expected_oracle": expected_oracle,
                    "reasons": reasons,
                }
            )
    return mismatches


def build_report(root: Path) -> tuple[list[str], dict[str, Any]]:
    manifest = load_manifest(root)
    errors = (
        check_manifest(root)
        + check_semantic_owners(root)
        + check_error_model_policy(root)
        + check_fastpaths(root)
    )
    dynamic_errors, dynamic_report = check_dynamic(root)
    errors.extend(dynamic_errors)
    owners: dict[str, list[str]] = defaultdict(list)
    boundary_names = set(manifest.by_name)
    for item in api_inventory(root).get("items", []):
        if item.get("category") != "stdlib-module":
            continue
        module = str(item.get("doc_module") or item.get("namespace") or "")
        if module not in boundary_names:
            continue
        symbol = str(item.get("qualified", ""))
        source = str(item.get("source", ""))
        if source.endswith(".xr"):
            owner = "xray_semantic"
        elif manifest.by_name[module]["policy"] == "native_library":
            owner = "native_library"
        else:
            owner = "native_primitive"
        owners[owner].append(symbol)

    contracts: list[dict[str, Any]] = []
    for module in contract_modules(root):
        contract_errors, contract = validate_contract(root, module)
        errors.extend(contract_errors)
        cases_path = root / "tests/stdlib/contracts" / module / "cases.jsonl"
        case_count = sum(1 for line in cases_path.read_text(encoding="utf-8").splitlines() if line.strip())
        contracts.append(
            {
                "module": module,
                "contract_revision": contract.get("contract_revision"),
                "legacy_commit": contract.get("legacy_commit"),
                "legacy_oracle": contract.get("legacy_oracle"),
                "backends": contract.get("backends", ["vm", "aot"]),
                "diff_cases_manifest": contract.get("diff_cases_manifest"),
                "case_count": case_count,
                "equivalence": contract.get("equivalence", []),
            }
        )

    perf = load_toml(root / "tests/benchmarks/stdlib/manifest.toml")
    if perf.get("schema") != 2:
        errors.append("tests/benchmarks/stdlib/manifest.toml: schema must be 2")
    layer_counts = Counter(str(module["layer"]) for module in manifest.modules)
    policy_counts = Counter(str(module["policy"]) for module in manifest.modules)
    native_boundaries = [
        {
            "module": module["name"],
            "layer": module["layer"],
            "policy": module["policy"],
            "public_native": module.get("public_native", []),
            "private_native_sources": module.get("private_native_sources", []),
            "reason": module.get("private_native_reason", "typed native boundary declared in .def"),
        }
        for module in manifest.modules
        if module.get("public_native") or module.get("private_native_sources")
    ]
    contract_names = {str(contract["module"]) for contract in contracts}
    governed_suites = {str(value) for value in perf.get("governed_suites", [])}
    benchmark_suites = {
        str(item.get("suite", "")) for item in perf.get("benchmark", []) if item.get("suite")
    }
    expected_contracts = {
        suite.removeprefix("stdlib/") for suite in governed_suites if suite.startswith("stdlib/")
    }
    missing_contracts = sorted(expected_contracts - contract_names)
    missing_benchmarks = sorted(governed_suites - benchmark_suites)
    non_executable_legacy = sorted(
        str(contract["module"])
        for contract in contracts
        if contract.get("legacy_oracle") != "executable"
    )
    agreement_blockers: list[dict[str, Any]] = []
    if dynamic_report.get("migration_debt_count", 0):
        agreement_blockers.append(
            {
                "kind": "dynamic_migration_debt",
                "count": dynamic_report["migration_debt_count"],
            }
        )
    if missing_contracts:
        agreement_blockers.append(
            {"kind": "missing_correctness_contracts", "modules": missing_contracts}
        )
    if missing_benchmarks:
        agreement_blockers.append(
            {"kind": "missing_active_benchmarks", "suites": missing_benchmarks}
        )
    if non_executable_legacy:
        agreement_blockers.append(
            {"kind": "non_executable_legacy_oracles", "modules": non_executable_legacy}
        )
    backend_mismatches = benchmark_backend_agreement(
        contracts, list(perf.get("benchmark", []))
    )
    if backend_mismatches:
        agreement_blockers.append(
            {
                "kind": "contract_benchmark_backend_mismatch",
                "benchmarks": backend_mismatches,
            }
        )
    oracle_mismatches = benchmark_oracle_agreement(
        root, contracts, list(perf.get("benchmark", []))
    )
    if oracle_mismatches:
        agreement_blockers.append(
            {
                "kind": "contract_benchmark_oracle_mismatch",
                "benchmarks": oracle_mismatches,
            }
        )

    report = {
        "schema": 1,
        "status": {
            "consistent": not errors,
            "policy_agreement": not errors and not agreement_blockers,
            "policy_agreement_blockers": agreement_blockers,
        },
        "public_symbols_by_semantic_owner": {
            owner: {"count": len(set(symbols)), "symbols": sorted(set(symbols))}
            for owner, symbols in sorted(owners.items())
        },
        "modules": {
            "count": len(manifest.modules),
            "by_layer": dict(sorted(layer_counts.items())),
            "by_policy": dict(sorted(policy_counts.items())),
        },
        "remaining_native_boundaries": native_boundaries,
        "dynamic_surface": {
            **dynamic_report,
        },
        "vm_fastpaths": list(manifest.vm_fastpaths),
        "correctness_contracts": contracts,
        "performance_governance": {
            "schema": perf.get("schema"),
            "governed_suite_count": len(perf.get("governed_suites", [])),
            "active_benchmarks": perf.get("benchmark", []),
            "raw_samples_required": perf.get("raw_samples_required", False),
        },
        "residue_policy": {
            "def_migration_complete_modules": list(manifest.def_migrated_modules),
            "aot_helper_forbidden_modules": list(manifest.aot_helper_forbidden_modules),
        },
    }
    return errors, report


def render_markdown(report: dict[str, Any]) -> str:
    lines = ["# Xray stdlib governance report", ""]
    modules = report["modules"]
    lines.append(f"Registered modules: {modules['count']}")
    lines.append("")
    lines.append("## Public symbols by semantic owner")
    lines.append("")
    for owner, data in report["public_symbols_by_semantic_owner"].items():
        lines.append(f"- {owner}: {data['count']}")
    lines.extend(
        [
            "",
            "## Governance status",
            "",
            f"- Source consistency: {report['status']['consistent']}",
            f"- Policy agreement: {report['status']['policy_agreement']}",
            f"- Native boundary modules: {len(report['remaining_native_boundaries'])}",
            f"- Dynamic migration debts: {report['dynamic_surface']['migration_debt_count']}",
            f"- Approved VM fastpaths: {len(report['vm_fastpaths'])}",
            f"- Migration contracts: {len(report['correctness_contracts'])}",
            f"- Governed performance suites: {report['performance_governance']['governed_suite_count']}",
            f"- Active performance benchmarks: {len(report['performance_governance']['active_benchmarks'])}",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".")
    parser.add_argument("--json", type=Path)
    parser.add_argument("--markdown", type=Path)
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify the report is source-derived and internally consistent",
    )
    parser.add_argument(
        "--require-complete",
        action="store_true",
        help=(
            "require every module to agree with its recorded policy; this is "
            "governance agreement, not Xray ownership of public semantics"
        ),
    )
    args = parser.parse_args()
    root = Path(args.root).resolve()
    errors, report = build_report(root)
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(encoded, encoding="utf-8")
    if args.markdown:
        args.markdown.parent.mkdir(parents=True, exist_ok=True)
        args.markdown.write_text(render_markdown(report), encoding="utf-8")
    if not args.json and not args.markdown and not args.check and not args.require_complete:
        print(encoded, end="")
    if args.check:
        print("OK: stdlib governance report is source-derived and consistent")
    if args.require_complete:
        blockers = report["status"]["policy_agreement_blockers"]
        if blockers:
            print("stdlib governance gate failed:", file=sys.stderr)
            for blocker in blockers:
                print(f"  {blocker['kind']}: {json.dumps(blocker, sort_keys=True)}", file=sys.stderr)
            return 1
        print(
            "OK: stdlib governance gate passed -- every module's contracts, "
            "oracles, benchmarks and declared surface agree with its recorded policy"
        )
        print(
            "This gate does not check that public semantics are owned by Xray "
            "source; check_stdlib_full_xray_completion.py is the gate that does"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

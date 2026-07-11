#!/usr/bin/env python3
"""Emit the task-196 source-derived stdlib self-hosting governance report."""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any

from check_stdlib_boundary import (
    check_dynamic,
    check_fastpaths,
    check_manifest,
    check_semantic_owners,
)
from stdlib_manifest import api_inventory, load_manifest, load_toml
from stdlib_migration import contract_modules, validate_contract


def build_report(root: Path) -> tuple[list[str], dict[str, Any]]:
    manifest = load_manifest(root)
    errors = check_manifest(root) + check_semantic_owners(root) + check_fastpaths(root)
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
                "case_count": case_count,
                "equivalence": contract.get("equivalence", []),
            }
        )

    perf = load_toml(root / "tests/benchmarks/stdlib/manifest.toml")
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
    report = {
        "schema": 1,
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
            "allowed_symbols": manifest.raw.get("dynamic_audit", {}).get("allowed_symbols", []),
            **dynamic_report,
        },
        "vm_fastpaths": list(manifest.vm_fastpaths),
        "correctness_contracts": contracts,
        "performance_governance": {
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
    lines = ["# Xray stdlib self-hosting governance report", ""]
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
    parser.add_argument("--check", action="store_true")
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
    if not args.json and not args.markdown and not args.check:
        print(encoded, end="")
    if args.check:
        print("OK: stdlib self-hosting report is source-derived and complete")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

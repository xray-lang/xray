#!/usr/bin/env python3
"""Verify task-196 stdlib ownership, native boundary and dynamic-surface policy."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

from stdlib_manifest import (
    MANIFEST_PATH,
    VALID_LAYERS,
    VALID_POLICIES,
    def_public_symbols,
    dynamic_public_items,
    load_manifest,
    load_toml,
    registry_modules,
)


def check_manifest(root: Path) -> list[str]:
    manifest = load_manifest(root)
    errors: list[str] = []
    if manifest.raw.get("schema") != 1:
        errors.append(f"{MANIFEST_PATH}: schema must be 1")
    names = [str(module.get("name", "")) for module in manifest.modules]
    if len(names) != len(set(names)):
        errors.append(f"{MANIFEST_PATH}: module names must be unique")
    source_registry = registry_modules(root)
    if set(names) != set(source_registry):
        missing = sorted(set(source_registry) - set(names))
        stale = sorted(set(names) - set(source_registry))
        if missing:
            errors.append(f"manifest misses registered modules: {', '.join(missing)}")
        if stale:
            errors.append(f"manifest lists unregistered modules: {', '.join(stale)}")

    ignored_dirs = {"defs", "types", "__pycache__"}
    source_dirs = {
        path.name
        for path in (root / "stdlib").iterdir()
        if path.is_dir() and path.name not in ignored_dirs and any(path.glob("*.c"))
    }
    untracked_dirs = sorted(source_dirs - set(names))
    if untracked_dirs:
        errors.append(f"stdlib native directories are not in manifest: {', '.join(untracked_dirs)}")

    for module in manifest.modules:
        name = str(module.get("name", ""))
        layer = module.get("layer")
        policy = module.get("policy")
        if layer not in VALID_LAYERS:
            errors.append(f"module {name}: invalid layer {layer!r}")
        if policy not in VALID_POLICIES:
            errors.append(f"module {name}: invalid policy {policy!r}")
        for field in ("semantic_source", "loader", "perf_suite"):
            if not module.get(field):
                errors.append(f"module {name}: missing {field}")
        for field in ("semantic_source", "loader"):
            value = module.get(field)
            if value and not (root / str(value)).is_file():
                errors.append(f"module {name}: {field} does not exist: {value}")
        expected_loader = source_registry.get(name)
        declared_loader = str(module.get("loader_symbol") or Path(str(module.get("loader", ""))).stem)
        if expected_loader and declared_loader != expected_loader:
            errors.append(
                f"module {name}: loader {declared_loader!r} does not match registry symbol "
                f"xr_load_module_{expected_loader}"
            )
    return errors


def check_semantic_owners(root: Path) -> list[str]:
    manifest = load_manifest(root)
    errors: list[str] = []
    def_symbols = def_public_symbols(root)
    for module in manifest.modules:
        name = str(module["name"])
        source = root / str(module["semantic_source"])
        if module["policy"] == "xray_semantic":
            if source.suffix != ".xr":
                errors.append(f"module {name}: xray_semantic owner must be an .xr source")
            elif not re.search(r"(?m)^export\s*\{", source.read_text(encoding="utf-8")):
                errors.append(f"module {name}: xray_semantic source has no export block")
        declared = set(module.get("public_native", ()))
        manual = set(module.get("manual_public_native", ()))
        actual = def_symbols.get(name, set()) | manual
        if declared != actual:
            missing = sorted(actual - declared)
            stale = sorted(declared - actual)
            if missing:
                errors.append(f"module {name}: public_native misses .def symbols: {', '.join(missing)}")
            if stale:
                errors.append(f"module {name}: public_native has stale symbols: {', '.join(stale)}")
        if manual:
            loader_text = (root / str(module["loader"])).read_text(encoding="utf-8")
            for symbol in sorted(manual):
                leaf = symbol.rsplit(".", 1)[-1]
                if f'"{leaf}"' not in loader_text:
                    errors.append(
                        f"module {name}: manual public native {symbol!r} is not registered by loader"
                    )
        private_sources = module.get("private_native_sources", ())
        if private_sources and not module.get("private_native_reason"):
            errors.append(f"module {name}: private native sources require private_native_reason")
        for pattern in private_sources:
            if not list(root.glob(str(pattern))):
                errors.append(f"module {name}: private native source pattern matches nothing: {pattern}")
    return errors


def check_fastpaths(root: Path) -> list[str]:
    manifest = load_manifest(root)
    errors: list[str] = []
    option_name = str(manifest.raw.get("governance", {}).get("vm_fastpath_option", ""))
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    if not option_name or not re.search(rf"option\(\s*{re.escape(option_name)}\b", cmake):
        errors.append("governance.vm_fastpath_option must name a real CMake option")
    required = {
        "symbol", "reference", "native", "reason", "benchmark", "diff_case", "review_date"
    }
    benchmark_data = load_toml(root / "tests/benchmarks/stdlib/manifest.toml")
    benchmark_ids = {entry.get("id") for entry in benchmark_data.get("benchmark", ())}
    aot_text = "\n".join(
        path.read_text(encoding="utf-8", errors="ignore")
        for path in (root / "src/aot").rglob("*")
        if path.is_file() and path.suffix in {".c", ".h"}
    )
    seen: set[str] = set()
    for entry in manifest.vm_fastpaths:
        symbol = str(entry.get("symbol", ""))
        if symbol in seen:
            errors.append(f"duplicate vm_fastpath symbol: {symbol}")
        seen.add(symbol)
        missing = sorted(required - set(entry))
        if missing:
            errors.append(f"vm_fastpath {symbol or '<unnamed>'}: missing {', '.join(missing)}")
        reference_path = str(entry.get("reference", "")).split("::", 1)[0]
        if reference_path and not (root / reference_path).is_file():
            errors.append(f"vm_fastpath {symbol}: reference source does not exist: {reference_path}")
        if entry.get("disable_build_option") != option_name:
            errors.append(f"vm_fastpath {symbol}: must use global disable option {option_name}")
        if entry.get("benchmark") not in benchmark_ids:
            errors.append(f"vm_fastpath {symbol}: benchmark id is absent from stdlib perf manifest")
        diff_case = root / str(entry.get("diff_case", ""))
        if not diff_case.is_file():
            errors.append(f"vm_fastpath {symbol}: diff_case does not exist: {entry.get('diff_case')}")
        native = str(entry.get("native", ""))
        if native and re.search(rf"\b{re.escape(native)}\b", aot_text):
            errors.append(f"vm_fastpath {symbol}: AOT sources reference VM-only symbol {native}")
    return errors


def _dynamic_module(item: dict[str, object]) -> str:
    return str(item.get("doc_module") or item.get("namespace") or "")


def check_dynamic(root: Path, require_clean: bool = False) -> tuple[list[str], dict[str, object]]:
    manifest = load_manifest(root)
    policy = manifest.raw.get("dynamic_audit", {})
    migration_modules = set(policy.get("migration_modules", ()))
    allowed = set(policy.get("allowed_symbols", ()))
    items = dynamic_public_items(root)
    errors: list[str] = []
    debt: list[dict[str, object]] = []
    for item in items:
        symbol = str(item.get("qualified", ""))
        module = _dynamic_module(item)
        if symbol in allowed:
            continue
        if module in migration_modules:
            debt.append(
                {
                    "module": module,
                    "symbol": symbol,
                    "signature": item.get("signature", ""),
                    "source": item.get("source", ""),
                    "line": item.get("line", 0),
                }
            )
            continue
        errors.append(
            f"unclassified public Json/unknown surface: {module}:{symbol} "
            f"{item.get('signature', '')} ({item.get('source', '')}:{item.get('line', 0)})"
        )
    actual_symbols = {str(item.get("qualified", "")) for item in items}
    for symbol in sorted(allowed - actual_symbols):
        errors.append(f"dynamic_audit.allowed_symbols contains stale symbol: {symbol}")
    if require_clean and debt:
        errors.append(f"dynamic migration debt remains: {len(debt)} public surfaces")
    report = {
        "schema": 1,
        "allowed_count": sum(1 for item in items if str(item.get("qualified", "")) in allowed),
        "migration_debt_count": len(debt),
        "migration_debt": debt,
    }
    return errors, report


CHECKS = {"manifest", "semantic", "fastpath", "dynamic", "all"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--check", choices=sorted(CHECKS), default="all")
    parser.add_argument("--report-json", type=Path)
    parser.add_argument("--require-clean-dynamic", action="store_true")
    args = parser.parse_args()
    root = Path(args.root).resolve()
    errors: list[str] = []
    report: dict[str, object] = {}
    if args.check in {"manifest", "all"}:
        errors.extend(check_manifest(root))
    if args.check in {"semantic", "all"}:
        errors.extend(check_semantic_owners(root))
    if args.check in {"fastpath", "all"}:
        errors.extend(check_fastpaths(root))
    if args.check in {"dynamic", "all"}:
        dynamic_errors, report = check_dynamic(root, args.require_clean_dynamic)
        errors.extend(dynamic_errors)
    if args.report_json:
        args.report_json.parent.mkdir(parents=True, exist_ok=True)
        args.report_json.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if errors:
        print("stdlib boundary gate failed:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1
    debt_count = int(report.get("migration_debt_count", 0))
    suffix = f"; {debt_count} classified dynamic migration debts" if report else ""
    print(f"OK: stdlib boundary {args.check} governance is source-consistent{suffix}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

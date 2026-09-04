#!/usr/bin/env python3
"""Verify the provider-backed portion of binary-stdlib and L2 readiness.

Of the former cluster/http2/compress/crypto migration group, only crypto still
requires a private host provider and belongs to this gate.  The source-only
modules are governed by the ordinary stdlib source/contract checks and must not
be described as retained native modules here.  L2 io/os/net keep their separate
zero-public-native boundary checks.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_api_inventory import collect_def_stdlib, collect_pure_stdlib  # noqa: E402
from stdlib_manifest import def_public_symbols, load_manifest, load_toml  # noqa: E402


NATIVE_PROVIDER_MODULES = {"crypto"}
L2_PUBLIC_NATIVE = {
    "io": set(),
    "os": set(),
    "net": set(),
}
CONTRACT_FILES = ("contract.toml", "cases.jsonl", "diff_cases.txt")
ABI_EVIDENCE = {
    "tests/unit/api/test_crypto_native_error_abi.py": (
        "stdlib/crypto/crypto.c",
        "CryptoError.InvalidLength",
        "assert_vm_native_aot",
    ),
    "tests/regression/10_stdlib/1433_net_loopback.xr": (
        "test_loopback_binary_high_bytes",
        "test_native_copy_loopback",
    ),
    "tests/benchmarks/stdlib/manifest.toml": (
        'id = "net.async-loopback.contract"',
        'id = "net.server-lifecycle.contract"',
    ),
}


@dataclass(frozen=True)
class CheckResult:
    category: str
    subject: str
    ok: bool
    detail: str


def load_boundary_modules(root: Path) -> dict[str, dict[str, Any]]:
    return load_manifest(root).by_name


def result(category: str, subject: str, failures: list[str]) -> CheckResult:
    return CheckResult(
        category,
        subject,
        not failures,
        "readiness verified" if not failures else "; ".join(failures),
    )


def check_boundary(
    root: Path,
    stdlib_modules: dict[str, dict[str, Any]] | None = None,
) -> list[CheckResult]:
    modules = stdlib_modules or load_boundary_modules(root)
    results: list[CheckResult] = []

    failures: list[str] = []
    actual = set(modules)
    missing = NATIVE_PROVIDER_MODULES - actual
    if missing:
        failures.append(f"missing native-provider stdlib modules: {sorted(missing)}")
    results.append(result("NATIVE_PROVIDER_SET", "stdlib/stdlib_boundary.toml", failures))

    for module in sorted(NATIVE_PROVIDER_MODULES):
        entry = modules.get(module)
        failures = []
        if entry is None:
            failures.append("missing stdlib boundary entry")
        else:
            if entry.get("perf_suite") != f"stdlib/{module}":
                failures.append("perf_suite must use the stdlib namespace")
            semantic_source = root / str(entry.get("semantic_source", ""))
            if not semantic_source.is_file():
                failures.append("semantic_source is missing")
            private_sources = [
                path
                for pattern in entry.get("private_native_sources", ())
                for path in root.glob(str(pattern))
            ]
            if entry.get("def_migration_complete"):
                failures.append("native-provider module cannot declare migration complete")
            if entry.get("aot_helper_forbidden"):
                failures.append("native-provider module cannot forbid its provider helper")
            if not private_sources:
                failures.append("private native sources are missing")
        results.append(result("NATIVE_PROVIDER_MODULE", module, failures))

    for module, expected in L2_PUBLIC_NATIVE.items():
        entry = modules.get(module)
        failures = []
        if entry is None:
            failures.append("missing stdlib boundary entry")
        else:
            if entry.get("layer") != "L2" or entry.get("policy") != "xray_semantic":
                failures.append(
                    f"expected L2/xray_semantic, got {entry.get('layer')}/{entry.get('policy')}"
                )
            actual_native = set(entry.get("public_native", ()))
            if actual_native != expected:
                failures.append(
                    f"public_native mismatch: missing={sorted(expected - actual_native)} "
                    f"extra={sorted(actual_native - expected)}"
                )
            if entry.get("semantic_source") != f"stdlib/{module}/{module}.xr":
                failures.append("semantic_source must be the public .xr module")
        results.append(result("L2_THIN_PUBLIC_SURFACE", module, failures))
    return results


def check_module_payloads(root: Path) -> list[CheckResult]:
    modules = load_boundary_modules(root)
    public_symbols = def_public_symbols(root)
    results: list[CheckResult] = []
    for module in sorted(NATIVE_PROVIDER_MODULES):
        entry = modules.get(module, {})
        failures: list[str] = []
        module_dir = root / "stdlib" / module
        if not module_dir.is_dir():
            failures.append(f"missing stdlib/{module}")
        if not entry.get("private_native_sources"):
            failures.append("private_native_sources must be declared")
        declared = set(entry.get("public_native", ()))
        # Private providers may expose no public native surface.  The boundary
        # manifest and core.def must nevertheless agree exactly.
        if declared and not public_symbols.get(module):
            failures.append("core.def has no public declarations")
        if declared != public_symbols.get(module, set()):
            failures.append("public_native does not exactly match core.def")
        results.append(result("NATIVE_PROVIDER_PAYLOAD", module, failures))
    return results


def check_contracts(root: Path) -> list[CheckResult]:
    results: list[CheckResult] = []
    for module in sorted(NATIVE_PROVIDER_MODULES):
        oracle = root / "tests" / "stdlib" / "contracts" / module
        failures = [name for name in CONTRACT_FILES if not (oracle / name).is_file()]
        detail = ["missing contract files: " + ", ".join(failures)] if failures else []
        contract_path = oracle / "contract.toml"
        if contract_path.is_file():
            contract = load_toml(contract_path)
            if contract.get("module") != module:
                detail.append(f"contract module must be {module!r}")
            if contract.get("benchmark_suite") != f"stdlib/{module}":
                detail.append(f"benchmark_suite must be stdlib/{module}")
            if contract.get("legacy_oracle") == "executable":
                required = ("legacy_expected.jsonl", "probes/current.xr", "probes/legacy.xr")
                missing = [name for name in required if not (oracle / name).is_file()]
                if missing:
                    detail.append("missing executable legacy oracle files: " + ", ".join(missing))
        results.append(result("NATIVE_PROVIDER_CONTRACT", module, detail))
    return results


def check_perf_manifest(root: Path) -> list[CheckResult]:
    data = load_toml(root / "tests/benchmarks/stdlib/manifest.toml")
    benchmarks = data.get("benchmark", ())
    results: list[CheckResult] = []
    for module in sorted(NATIVE_PROVIDER_MODULES):
        suite = f"stdlib/{module}"
        matches = [b for b in benchmarks if b.get("module") == module and b.get("suite") == suite]
        failures = []
        if not matches:
            failures.append(f"missing benchmark for module={module!r}, suite={suite!r}")
        for bench in matches:
            if bench.get("compare") != ["vm", "aot"] or "wall_ns" not in bench.get("metrics", ()):
                failures.append(f"{bench.get('id')}: must compare VM/AOT wall_ns")
        results.append(result("NATIVE_PROVIDER_PERF", module, failures))
    return results


def check_api_classification(root: Path) -> list[CheckResult]:
    items = [*collect_def_stdlib(root), *collect_pure_stdlib(root)]
    results: list[CheckResult] = []
    for module in sorted(NATIVE_PROVIDER_MODULES):
        owned = [
            item
            for item in items
            if item.get("namespace") == module
            or str(item.get("namespace", "")).startswith(module + ".")
        ]
        failures = []
        if not owned:
            failures.append("no source-derived API entries")
        for entry in owned:
            if entry.get("category") != "stdlib-module" or entry.get("doc_surface") != "stdlib":
                failures.append(f"misclassified API: {entry.get('qualified')}")
                break
        results.append(result("NATIVE_PROVIDER_API", module, failures))
    return results


def check_abi_evidence(root: Path) -> list[CheckResult]:
    results: list[CheckResult] = []
    for path_text, anchors in ABI_EVIDENCE.items():
        path = root / path_text
        failures = []
        if not path.is_file():
            failures.append("missing file")
        else:
            text = path.read_text(encoding="utf-8")
            failures.extend(f"missing anchor {anchor!r}" for anchor in anchors if anchor not in text)
        results.append(result("NATIVE_PROVIDER_ABI_EVIDENCE", path_text, failures))
    return results


def build_results(root: Path) -> list[CheckResult]:
    return [
        *check_boundary(root),
        *check_module_payloads(root),
        *check_contracts(root),
        *check_perf_manifest(root),
        *check_api_classification(root),
        *check_abi_evidence(root),
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    root = Path(args.root).resolve()
    results = build_results(root)
    if args.json:
        print(json.dumps([asdict(item) for item in results], indent=2, sort_keys=True))
    else:
        print("Binary-stdlib and L2 native-provider readiness")
        for item in results:
            print(f"{item.category}: {'ok' if item.ok else 'blocked'}: {item.subject}: {item.detail}")
    return 0 if all(item.ok for item in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Verify task-256's terminal built-in stdlib and L2 native boundary.

The retained cluster/http2/compress/crypto modules share Xray's release and
installation unit.  They have one physical home under ``stdlib/`` and one bare
module name; package aliases and package-loader fallbacks are forbidden.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_api_inventory import collect_def_stdlib, collect_pure_stdlib  # noqa: E402
from stdlib_manifest import def_public_symbols, load_manifest, load_toml  # noqa: E402


RETAINED_STDLIB_MODULES = {"cluster", "http2", "compress", "crypto"}
TERMINAL_STDLIB_MODULE_COUNT = 34
L2_PUBLIC_NATIVE = {
    "io": set(),
    "os": {"arch", "eol", "platform", "sep"},
    "net": {
        "NetConn",
        "NetConn.close",
        "NetConn.fd",
        "NetConn.isClosed",
        "NetConn.isTLS",
        "NetError",
        "NetListener",
        "NetListener.close",
        "NetListener.fd",
        "NetListener.isClosed",
        "NetListener.port",
    },
}
CONTRACT_FILES = ("contract.toml", "cases.jsonl", "diff_cases.txt")
ABI_EVIDENCE = {
    "tests/unit/api/test_compress_native_error_abi.py": (
        "stdlib/compress/compress.c",
        "CompressionError.InvalidData",
        "test_vm_native_aot_typed_catch_parity",
    ),
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
FORBIDDEN_PACKAGE_IMPORT = re.compile(
    r"(?m)^\s*import\s+(?:\{[^}\n]+\}\s+from\s+)?"
    r"(?:[\"'])?xray/(?:cluster|http2|compress|crypto)\b"
)


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
        "cutover verified" if not failures else "; ".join(failures),
    )


def check_boundary(
    root: Path,
    stdlib_modules: dict[str, dict[str, Any]] | None = None,
) -> list[CheckResult]:
    modules = stdlib_modules or load_boundary_modules(root)
    results: list[CheckResult] = []

    failures: list[str] = []
    actual = set(modules)
    missing = RETAINED_STDLIB_MODULES - actual
    if missing:
        failures.append(f"missing retained stdlib modules: {sorted(missing)}")
    if len(actual) != TERMINAL_STDLIB_MODULE_COUNT:
        failures.append(
            f"terminal stdlib module count must be {TERMINAL_STDLIB_MODULE_COUNT}, got {len(actual)}"
        )
    results.append(result("BUILTIN_STDLIB_SET", "stdlib/stdlib_boundary.toml", failures))

    for module in sorted(RETAINED_STDLIB_MODULES):
        entry = modules.get(module)
        failures = []
        if entry is None:
            failures.append("missing stdlib boundary entry")
        else:
            if entry.get("perf_suite") != f"stdlib/{module}":
                failures.append("perf_suite must use the stdlib namespace")
            semantic_source = root / str(entry.get("semantic_source", ""))
            factory = root / str(entry.get("factory_source", ""))
            if not semantic_source.is_file():
                failures.append("semantic_source is missing")
            if not factory.is_file():
                failures.append("factory source is missing")
        results.append(result("BUILTIN_STDLIB_MODULE", module, failures))

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
    for module in sorted(RETAINED_STDLIB_MODULES):
        entry = modules.get(module, {})
        failures: list[str] = []
        module_dir = root / "stdlib" / module
        if not module_dir.is_dir():
            failures.append(f"missing stdlib/{module}")
        if not entry.get("private_native_sources"):
            failures.append("private_native_sources must be declared")
        if not public_symbols.get(module):
            failures.append("core.def has no public declarations")
        if set(entry.get("public_native", ())) != public_symbols.get(module, set()):
            failures.append("public_native does not exactly match core.def")
        results.append(result("BUILTIN_STDLIB_PAYLOAD", module, failures))
    return results


def check_contracts(root: Path) -> list[CheckResult]:
    results: list[CheckResult] = []
    for module in sorted(RETAINED_STDLIB_MODULES):
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
        results.append(result("BUILTIN_STDLIB_CONTRACT", module, detail))
    return results


def check_perf_manifest(root: Path) -> list[CheckResult]:
    data = load_toml(root / "tests/benchmarks/stdlib/manifest.toml")
    benchmarks = data.get("benchmark", ())
    results: list[CheckResult] = []
    for module in sorted(RETAINED_STDLIB_MODULES):
        suite = f"stdlib/{module}"
        matches = [b for b in benchmarks if b.get("module") == module and b.get("suite") == suite]
        failures = []
        if not matches:
            failures.append(f"missing benchmark for module={module!r}, suite={suite!r}")
        for bench in matches:
            if bench.get("compare") != ["vm", "aot"] or "wall_ns" not in bench.get("metrics", ()):
                failures.append(f"{bench.get('id')}: must compare VM/AOT wall_ns")
        results.append(result("BUILTIN_STDLIB_PERF", module, failures))
    return results


def check_api_classification(root: Path) -> list[CheckResult]:
    items = [*collect_def_stdlib(root), *collect_pure_stdlib(root)]
    results: list[CheckResult] = []
    for module in sorted(RETAINED_STDLIB_MODULES):
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
        results.append(result("BUILTIN_STDLIB_API", module, failures))
    return results


def check_import_cutover(root: Path) -> list[CheckResult]:
    failures: list[str] = []
    for base in ("stdlib", "tests"):
        for path in sorted((root / base).rglob("*.xr")):
            match = FORBIDDEN_PACKAGE_IMPORT.search(path.read_text(encoding="utf-8"))
            if match:
                failures.append(f"{path.relative_to(root)}:{match.group(0).strip()}")
    return [result("CANONICAL_STDLIB_IMPORTS", "bare module names", failures[:20])]


def check_physical_cutover(root: Path) -> list[CheckResult]:
    failures = [
        f"missing stdlib/{name}"
        for name in sorted(RETAINED_STDLIB_MODULES)
        if not (root / "stdlib" / name).is_dir()
    ]
    failures.extend(str(path.relative_to(root)) for path in (root / "stdlib/http").glob("http2*"))
    package_root = root / "packages" / "official"
    if package_root.exists():
        failures.extend(str(path.relative_to(root)) for path in package_root.rglob("*") if path.is_file())
    markers = ("packages/official", "XR_PACKAGE_", "XR_OFFICIAL_PACKAGE", "official-package")
    for relpath in ("CMakeLists.txt", "src/module/xmodule.c", "scripts/generate_stdlib_embedded.py"):
        text = (root / relpath).read_text(encoding="utf-8")
        failures.extend(f"{relpath}: {marker}" for marker in markers if marker in text)
    return [result("PHYSICAL_STDLIB_CUTOVER", "one built-in stdlib tree", failures)]


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
        results.append(result("BINARY_ABI_EVIDENCE", path_text, failures))
    return results


def build_results(root: Path) -> list[CheckResult]:
    return [
        *check_boundary(root),
        *check_module_payloads(root),
        *check_contracts(root),
        *check_perf_manifest(root),
        *check_api_classification(root),
        *check_import_cutover(root),
        *check_physical_cutover(root),
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
        print("Task 256 built-in stdlib and public-native readiness")
        for item in results:
            print(f"{item.category}: {'ok' if item.ok else 'blocked'}: {item.subject}: {item.detail}")
    return 0 if all(item.ok for item in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())

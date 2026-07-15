#!/usr/bin/env python3
"""Check task-200 public-native switch readiness without cutting the switch.

This gate is intentionally conservative.  Task 200 still depends on task 197
Slice provenance and task 198 typed native error ABI before compress/crypto/io
and net can expose final public byte APIs.  The gate therefore proves that the
current branch has the inventory, contract, fuzz, AOT, generated-residue, and
owner metadata needed for a later switch, while also failing if that switch is
silently cut early.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any


BINARY_MODULES = ("base64", "encoding", "compress", "crypto", "io", "net", "http", "ws")
PRE_SWITCH_NATIVE_MODULES = {
    "compress": "native_library",
    "crypto": "native_library",
    "io": "native_primitive",
    "net": "native_primitive",
}
PURE_BYTE_MODULES = ("base64", "encoding")
REQUIRED_CONTRACT_MODULES = ("base64", "encoding", "compress", "crypto", "io", "http", "ws")
REQUIRED_SURFACE_CATEGORIES = (
    "PUBLIC_BINARY_STRING_SIGNATURE",
    "PUBLIC_ARBITRARY_STRING_CREATOR",
    "PUBLIC_NULL_SENTINEL",
    "CONSUMER_OLD_BINARY_API_CALL",
    "NATIVE_ARBITRARY_STRING_CREATOR",
    "GENERATED_METADATA_STALE_BINARY_SURFACE",
)
DEPENDENCY_MARKERS = (
    "TASK_197_SLICE_PROVENANCE_READY",
    "TASK_198_TYPED_NATIVE_ERRORS_READY",
)


@dataclass(frozen=True)
class CheckResult:
    category: str
    subject: str
    ok: bool
    detail: str


def load_toml(root: Path, path: Path) -> dict[str, Any]:
    sys.path.insert(0, str(root / "scripts"))
    try:
        import stdlib_manifest  # type: ignore[import-not-found]
    finally:
        try:
            sys.path.remove(str(root / "scripts"))
        except ValueError:
            pass
    return stdlib_manifest.load_toml(path)


def load_boundary_modules(root: Path) -> dict[str, dict[str, Any]]:
    data = load_toml(root, root / "stdlib" / "stdlib_boundary.toml")
    return {str(module.get("name", "")): module for module in data.get("module", ())}


def load_surface_inventory(root: Path) -> dict[str, list[Any]]:
    sys.path.insert(0, str(root / "scripts"))
    try:
        import check_binary_stdlib_surface  # type: ignore[import-not-found]
    finally:
        try:
            sys.path.remove(str(root / "scripts"))
        except ValueError:
            pass
    return check_binary_stdlib_surface.build_inventory(root)


def check_boundary(root: Path) -> list[CheckResult]:
    modules = load_boundary_modules(root)
    results: list[CheckResult] = []

    for name in BINARY_MODULES:
        module = modules.get(name)
        if module is None:
            results.append(CheckResult("BOUNDARY_BINARY_MODULE", name, False, "missing stdlib boundary entry"))
            continue

        semantic_source = str(module.get("semantic_source", ""))
        perf_suite = str(module.get("perf_suite", ""))
        public_native = module.get("public_native", None)
        failures: list[str] = []
        if not semantic_source:
            failures.append("missing semantic_source")
        if not perf_suite:
            failures.append("missing perf_suite")
        if not isinstance(public_native, list):
            failures.append("public_native must be a list")

        results.append(
            CheckResult(
                "BOUNDARY_BINARY_MODULE",
                name,
                not failures,
                "; ".join(failures) if failures else f"{semantic_source}, {perf_suite}",
            )
        )

    for name in PURE_BYTE_MODULES:
        module = modules.get(name, {})
        failures = []
        if module.get("policy") != "xray_semantic":
            failures.append(f"policy={module.get('policy')!r}, expected xray_semantic")
        if module.get("public_native") != []:
            failures.append("public_native must stay empty for pure byte module")
        if module.get("def_migration_complete") is not True:
            failures.append("def_migration_complete must be true")
        results.append(
            CheckResult(
                "PURE_BYTE_MODULE_READY",
                name,
                not failures,
                "; ".join(failures) if failures else "pure byte owner is already cut over",
            )
        )

    for name, expected_policy in PRE_SWITCH_NATIVE_MODULES.items():
        module = modules.get(name, {})
        public_native = module.get("public_native", [])
        failures = []
        if module.get("policy") != expected_policy:
            failures.append(f"policy={module.get('policy')!r}, expected {expected_policy}")
        if not isinstance(public_native, list) or not public_native:
            failures.append("public_native must remain explicit until dependencies close")
        if module.get("def_migration_complete") is True:
            failures.append("def_migration_complete was set before task-197/198 dependency closure")
        results.append(
            CheckResult(
                "PRE_SWITCH_NATIVE_BLOCKED",
                name,
                not failures,
                "; ".join(failures) if failures else "native public surface still gated",
            )
        )

    return results


def check_contracts(root: Path) -> list[CheckResult]:
    results: list[CheckResult] = []
    for name in REQUIRED_CONTRACT_MODULES:
        contract = root / "tests" / "stdlib" / "contracts" / name / "contract.toml"
        cases = contract.with_name("cases.jsonl")
        diff_cases = contract.with_name("diff_cases.txt")
        missing = [str(path.relative_to(root)) for path in (contract, cases, diff_cases) if not path.is_file()]
        results.append(
            CheckResult(
                "BINARY_CONTRACT_CORPUS",
                name,
                not missing,
                "files present" if not missing else "missing " + ", ".join(missing),
            )
        )
    return results


def check_surface(root: Path) -> list[CheckResult]:
    inventory = load_surface_inventory(root)
    results: list[CheckResult] = []
    for category in REQUIRED_SURFACE_CATEGORIES:
        count = len(inventory.get(category, ()))
        results.append(
            CheckResult(
                "PRE_SWITCH_RESIDUE_TRACKED",
                category,
                count > 0,
                f"{count} tracked hits" if count > 0 else "expected tracked pre-switch residue",
            )
        )
    return results


def check_dependency_markers(root: Path) -> list[CheckResult]:
    marker_dir = root / "tests" / "stdlib" / "contracts"
    results: list[CheckResult] = []
    for marker in DEPENDENCY_MARKERS:
        hits = list(root.rglob(marker))
        results.append(
            CheckResult(
                "PUBLIC_SWITCH_DEPENDENCY_BLOCKER",
                marker,
                not hits,
                "not present; public-native switch remains blocked"
                if not hits
                else "unexpected readiness marker present: "
                + ", ".join(str(path.relative_to(root)) for path in hits[:5]),
            )
        )
    if not marker_dir.is_dir():
        results.append(CheckResult("PUBLIC_SWITCH_DEPENDENCY_BLOCKER", str(marker_dir), False, "missing contracts dir"))
    return results


def check_harness_anchors(root: Path) -> list[CheckResult]:
    anchors = {
        "tests/diff/fuzz_binary_stdlib.py": (
            "base64.b64encode(data)",
            'data.decode("utf-8", "strict")',
            '[str(xray), "build", "--native"',
        ),
        "tests/diff/fuzz_binary_native_stdlib.py": (
            "zlib.crc32(data)",
            "hashlib.sha256(data).hexdigest()",
            "hmac.new(key, data, getattr(hashlib, algo)).hexdigest()",
            '[str(xray), "build", "--native"',
        ),
        "tests/aot/filetests/link/core_compress.expect": (
            "c_contains=xrt_compress_crc32(",
            "c_not_contains=xrt_method_",
        ),
        "tests/aot/filetests/link/core_crypto.expect": (
            "c_contains=xrt_crypto_sha512(",
            "c_not_contains=xrt_method_",
        ),
        "tests/benchmarks/stdlib/manifest.toml": (
            'id = "compress.contract"',
            'id = "crypto.contract"',
            'compare = ["vm", "aot"]',
        ),
    }
    results: list[CheckResult] = []
    for path_text, needles in anchors.items():
        path = root / path_text
        if not path.is_file():
            results.append(CheckResult("BINARY_ORACLE_HARNESS", path_text, False, "missing file"))
            continue
        text = path.read_text(encoding="utf-8")
        missing = [needle for needle in needles if needle not in text]
        results.append(
            CheckResult(
                "BINARY_ORACLE_HARNESS",
                path_text,
                not missing,
                "anchors ok" if not missing else "missing anchors: " + ", ".join(missing),
            )
        )
    return results


def build_results(root: Path) -> list[CheckResult]:
    results: list[CheckResult] = []
    results.extend(check_boundary(root))
    results.extend(check_contracts(root))
    results.extend(check_surface(root))
    results.extend(check_harness_anchors(root))
    results.extend(check_dependency_markers(root))
    return results


def print_text(results: list[CheckResult]) -> None:
    print("Task 200 binary public-native switch readiness")
    for result in results:
        status = "ok" if result.ok else "blocked"
        print(f"{result.category}: {status}: {result.subject}: {result.detail}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--json", action="store_true", help="print machine-readable JSON")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    results = build_results(root)

    if args.json:
        print(json.dumps([asdict(result) for result in results], indent=2, sort_keys=True))
    else:
        print_text(results)

    return 0 if all(result.ok for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())

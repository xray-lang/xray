#!/usr/bin/env python3
"""Fail-closed Task 299 typed-VM coverage and runtime-isolation gate."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any


REGISTRY = Path("xisa/core/registry.json")
VM_HEADER = Path("src/vm/xr_program_vm.h")
VM_SOURCE = Path("src/vm/xr_program_vm.c")
VM_TEST = Path("tests/unit/vm/test_xr_program_vm.c")
RUNTIME_TEST = Path("tests/unit/vm/test_xr_program_vm_runtime.c")
TARGET_FIXTURE = Path("tests/unit/plan/target_profile_test_fixture.c")
CMAKE = Path("CMakeLists.txt")
COVERAGE = Path("contracts/canonical-program/xrprogram-vm-coverage.json")

RUNTIME_SOURCES = (
    "src/base/xsha256.c",
    "src/base/xtarget_data_layout.c",
    "src/core/xr_core_spec_gen.c",
    "src/plan/semantic/xr_semantic_ids.c",
    "src/runtime/abi/xr_runtime_contract.c",
    "src/runtime/abi/xr_runtime_descriptor.c",
    "src/runtime/abi/xr_runtime_object_header.c",
    "src/runtime/abi/xr_runtime_string_object.c",
    "src/plan/target/xr_target_profile.c",
    "src/plan/target/xr_target_profile_verify.c",
    "src/program/xr_program_diagnostic.c",
    "src/program/xr_program_identity.c",
    "src/program/xr_program_decode.c",
    "src/program/xr_program_verify.c",
    "src/execution/xr_execution.c",
    "src/execution/xr_boundary_materialization.c",
    "src/vm/xr_program_vm.c",
)

FORBIDDEN_VM_TOKENS = (
    "xr_reference_",
    "xr_core_ir_",
    "xr_program_write",
    "xr_program_encode",
    "xr_target_plan",
    "xaot_",
    "xvm_",
    "/aot/",
    "/frontend/",
    "/ir/",
)

FORBIDDEN_RUNTIME_SYMBOLS = (
    r"xr_core_ir_",
    r"xr_program_write",
    r"xr_reference_",
    r"xr_target_plan",
    r"xr_semantic_plan",
    r"xaot_",
    r"xvm_",
    r"xi_(?:lower|emit|pipeline|program)",
)


class GateError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise GateError(message)


def canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2) + "\n"


def read_json(path: Path) -> dict[str, Any]:
    raw = path.read_text(encoding="utf-8", errors="strict")
    value = json.loads(raw)
    require(isinstance(value, dict), f"{path} must contain an object")
    require(raw == canonical_json(value), f"{path} is not canonical JSON")
    return value


def enum_token(spelling: str) -> str:
    suffix = re.sub(r"[^A-Z0-9]+", "_", spelling.upper()).strip("_")
    return f"XR_CORE_OP_{suffix}"


def expected_coverage(registry: dict[str, Any]) -> dict[str, Any]:
    operations = [
        {
            "stable_id": row["stable_id"],
            "spelling": row["spelling"],
            "vm": "COMPLETE",
            "baseline_view": "COMPLETE",
            "fixed_row_view": "COMPLETE",
            "differential_oracle": "XrReferenceEvaluator",
        }
        for row in registry["operations"]
    ]
    return {
        "schema": "xray-program-vm-coverage/1",
        "task": 299,
        "vm_build_id": "xray-program-vm-v1",
        "input_authority": "XrValidatedProgram",
        "execution_authority": "XrInstance",
        "distribution_format": "XrProgram",
        "public_bytecode_format": None,
        "private_views": ["baseline-validated-graph", "fixed-instruction-rows"],
        "persistent_private_code": False,
        "quickening": "DISABLED_UNTIL_MEASURED",
        "execution_budgets": ["steps", "call-depth", "aggregate-value-cells"],
        "private_qualification": [
            "ExecutionId",
            "generation",
            "vm-build-id",
            "decode-policy",
            "quickening-policy",
        ],
        "operation_count": len(operations),
        "operations": operations,
        "profile_matrix": ["lp64-x86_64-windows", "ilp32-wasm32-wasi"],
        "lifecycle_matrix": [
            "active-execute",
            "retired-code-refused",
            "successor-generation-refused",
        ],
        "runtime_product": {
            "target": "xray_program_vm_runtime",
            "source_count": len(RUNTIME_SOURCES),
            "forbidden_closures": [
                "frontend",
                "CoreIR-writer",
                "reference-evaluator",
                "TargetPlan",
                "legacy-Proto-VM",
                "AOT",
            ],
            "embedder_test": "tests/unit/vm/test_xr_program_vm_runtime.c",
        },
        "inactive_contracts": [
            "persistent-private-code-cache",
            "adaptive-quickening",
            "full-language-operation-families",
            "public-embedding-ABI",
        ],
    }


def cmake_runtime_sources(cmake: str) -> tuple[str, ...]:
    match = re.search(
        r"set\(XRAY_PROGRAM_VM_RUNTIME_SOURCES\s*(.*?)\n\)", cmake, re.DOTALL
    )
    require(match is not None, "missing XRAY_PROGRAM_VM_RUNTIME_SOURCES")
    rows = []
    for raw in match.group(1).splitlines():
        row = raw.split("#", 1)[0].strip()
        if row:
            rows.append(row)
    return tuple(rows)


def validate_sources(root: Path, overrides: dict[Path, str] | None = None) -> None:
    paths = (VM_HEADER, VM_SOURCE, VM_TEST, RUNTIME_TEST, TARGET_FIXTURE, CMAKE)
    sources = {
        path: (overrides or {}).get(path, (root / path).read_text(encoding="utf-8"))
        for path in paths
    }
    registry = read_json(root / REGISTRY)
    header = sources[VM_HEADER]
    vm = sources[VM_SOURCE]
    test = sources[VM_TEST]
    runtime_test = sources[RUNTIME_TEST]
    cmake = sources[CMAKE]

    require('#define XR_VM_BUILD_ID "xray-program-vm-v1"' in header,
            "VM build identity is missing")
    for token in ("XR_VM_DECODE_BASELINE_VIEW", "XR_VM_DECODE_FIXED_ROWS",
                  "XR_VM_QUICKENING_NONE", "XrExecutionCacheKey", "max_value_cells"):
        require(token in header or token in vm, f"missing VM contract token {token}")
    for forbidden in FORBIDDEN_VM_TOKENS:
        require(forbidden not in vm, f"VM depends on forbidden semantic owner {forbidden}")
    require("xr_execution_instance_pin" in vm, "VM does not pin execution generations")
    require("xr_vm_code_matches_instance" in vm, "VM code has no exact generation match")
    require("xr_program_validate" not in vm,
            "VM must consume a validated graph instead of decoding bytes")
    require("serialize" not in vm.lower() and "deserialize" not in vm.lower(),
            "private VM code acquired a persistence format")

    for row in registry["operations"]:
        coverage = row.get("coverage", {}).get("vm", {})
        require(coverage == {"status": "COMPLETE", "task": 299},
                f"registry VM coverage is stale for {row['spelling']}")
        token = enum_token(row["spelling"])
        require(re.search(rf"\bcase\s+{re.escape(token)}\s*:", vm) is not None,
                f"VM has no explicit handler for {row['spelling']}")
        require(token in test, f"VM differential test omits {row['spelling']}")

    for token in ("xr_reference_evaluate", "XR_VM_DECODE_BASELINE_VIEW",
                  "XR_VM_DECODE_FIXED_ROWS", "XR_VM_OUTCOME_STALE_CODE",
                  "max_value_cells"):
        require(token in test, f"VM test omits {token}")
    require("XR_TARGET_ARCH_WASM32" in sources[TARGET_FIXTURE] and
            "run_program(control, true" in test,
            "VM test omits the explicit foreign ILP32 profile")
    require("xr_core_ir_" not in runtime_test and "xr_program_write" not in runtime_test,
            "runtime-only embedder contains a compiler-side producer")
    require("xray_program_vm_runtime" in cmake,
            "runtime-only VM product target is missing")
    require(cmake_runtime_sources(cmake) == RUNTIME_SOURCES,
            "runtime-only VM product source closure changed")
    require("include/xr_program_vm.h" not in cmake,
            "private VM code surface was installed as public API")
    require(not (root / "include/xr_program_vm.h").exists(),
            "private VM header leaked into the public include tree")

    expected = canonical_json(expected_coverage(registry))
    actual = (root / COVERAGE).read_text(encoding="utf-8", errors="strict")
    require(actual == expected, f"{COVERAGE} is stale")


def nm_symbols(path: Path) -> str:
    result = subprocess.run(
        ["nm", "-g", str(path)], check=False, capture_output=True, text=True
    )
    require(result.returncode == 0, f"nm failed for {path}: {result.stderr.strip()}")
    return result.stdout


def validate_artifacts(archive: Path, executable: Path) -> None:
    require(archive.is_file(), f"runtime archive is missing: {archive}")
    require(executable.is_file(), f"runtime embedder is missing: {executable}")
    archive_symbols = nm_symbols(archive)
    executable_symbols = nm_symbols(executable)
    for pattern in FORBIDDEN_RUNTIME_SYMBOLS:
        require(re.search(pattern, archive_symbols, re.IGNORECASE) is None,
                f"runtime archive contains forbidden symbol pattern {pattern}")
        require(re.search(pattern, executable_symbols, re.IGNORECASE) is None,
                f"runtime embedder contains forbidden symbol pattern {pattern}")
    for required in ("xr_program_validate", "xr_execution_instance_create",
                     "xr_execution_materialize_boundary_type", "xr_vm_code_build",
                     "xr_vm_code_execute"):
        require(required in archive_symbols, f"runtime archive omits {required}")


def self_test(root: Path) -> None:
    validate_sources(root)
    registry = read_json(root / REGISTRY)
    vm = (root / VM_SOURCE).read_text(encoding="utf-8")
    first = enum_token(registry["operations"][0]["spelling"])
    mutated = vm.replace(f"case {first}:", "case XR_CORE_OP_MISSING:", 1)
    require(mutated != vm, "missing-operation mutation did not apply")
    try:
        validate_sources(root, {VM_SOURCE: mutated})
    except GateError:
        pass
    else:
        raise GateError("missing VM operation mutation was accepted")

    mutated = vm.replace('#include "../core/xr_core_spec_gen.h"',
                         '#include "../core/xr_core_spec_gen.h"\n/* xr_reference_evaluate */', 1)
    require(mutated != vm, "reference dependency mutation did not apply")
    try:
        validate_sources(root, {VM_SOURCE: mutated})
    except GateError:
        pass
    else:
        raise GateError("reference-evaluator dependency mutation was accepted")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--executable", type=Path)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--print-coverage", action="store_true")
    parser.add_argument("--generate", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        if args.generate:
            (root / COVERAGE).write_text(
                canonical_json(expected_coverage(read_json(root / REGISTRY))), encoding="utf-8"
            )
            print(f"XrProgram VM contracts: generated {COVERAGE}")
            return 0
        if args.print_coverage:
            print(canonical_json(expected_coverage(read_json(root / REGISTRY))), end="")
            return 0
        if args.self_test:
            self_test(root)
            print("XrProgram VM contracts self-test: PASS")
        else:
            validate_sources(root)
            if bool(args.archive) != bool(args.executable):
                raise GateError("--archive and --executable must be provided together")
            if args.archive and args.executable:
                validate_artifacts(args.archive.resolve(), args.executable.resolve())
            count = len(read_json(root / REGISTRY)["operations"])
            print(f"XrProgram VM contracts: PASS ({count} operations, runtime-only closure)")
    except (GateError, OSError, UnicodeError, json.JSONDecodeError) as exc:
        print(f"XrProgram VM contracts: FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

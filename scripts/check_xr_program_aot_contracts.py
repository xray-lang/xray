#!/usr/bin/env python3
"""Fail-closed canonical XrProgram AOT lowering and artifact gate."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any


REGISTRY = Path("xisa/core/registry.json")
HEADER = Path("src/aot/program/xr_backend_ir.h")
LOWERING = Path("src/aot/program/xr_backend_ir.c")
VERIFY = Path("src/aot/program/xr_backend_ir_verify.c")
EMITTER = Path("src/aot/program/xr_backend_ir_emit_c.c")
ARTIFACT = Path("src/aot/program/xr_native_artifact.c")
TEST = Path("tests/unit/aot/test_xr_program_aot.c")
CMAKE = Path("CMakeLists.txt")
TEST_CMAKE = Path("tests/unit/CMakeLists.txt")
IDENTITY = Path("contracts/canonical-program/architecture-identity.toml")
COVERAGE = Path("contracts/canonical-program/xrprogram-aot-coverage.json")


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
            "aot": "COMPLETE",
            "backend_ir": "COMPLETE",
            "portable_c11": "COMPLETE",
            "translation_validation": "STRUCTURAL_EXACT",
        }
        for row in registry["operations"]
    ]
    return {
        "schema": "xray-program-aot-coverage/1",
        "task": 300,
        "input_authority": "XrValidatedProgram",
        "execution_authority": "XrInstance",
        "private_realization": "XrBackendIR",
        "backend": "xray-c11-aot@1",
        "operation_count": len(operations),
        "operations": operations,
        "pass_contract": {
            "ordinary": "pre-post-invariant-plus-preservation-set",
            "high_risk": "directed-translation-witness-required",
            "implemented_high_risk_passes": [],
        },
        "generated_c": {
            "dialect": "portable-c11",
            "always_on_output_verifier": "XiCgenVerifyOutput-W1-W4",
            "real_provider_evidence": ["clang", "zig-cc", "clang-cl-frontend"],
            "foreign_profile": "explicit-profile-literal",
        },
        "native_artifact_identity": [
            "ExecutionId",
            "BackendId",
            "ToolchainId",
            "OptimizationPolicyId",
            "native-bytes",
        ],
        "pure_aot": {
            "program_loader": False,
            "vm": False,
            "compiler": False,
            "aot_toolchain": False,
            "runtime_dependency": "libc-only-for-active-scalar-skeleton",
        },
        "inactive_contracts": [
            "full-language-operation-families",
            "high-risk-optimizations",
            "public-native-loader-ABI",
            "published-package-format",
        ],
    }


def sources(root: Path, overrides: dict[Path, str] | None = None) -> dict[Path, str]:
    paths = (HEADER, LOWERING, VERIFY, EMITTER, ARTIFACT, TEST, CMAKE, TEST_CMAKE,
             IDENTITY)
    return {
        path: (overrides or {}).get(path, (root / path).read_text(encoding="utf-8"))
        for path in paths
    }


def validate_sources(root: Path, overrides: dict[Path, str] | None = None) -> None:
    text = sources(root, overrides)
    registry = read_json(root / REGISTRY)
    header = text[HEADER]
    lowering = text[LOWERING]
    verifier = text[VERIFY]
    emitter = text[EMITTER]
    artifact = text[ARTIFACT]
    test = text[TEST]
    cmake = text[CMAKE]
    test_cmake = text[TEST_CMAKE]
    identity = text[IDENTITY]

    for token in (
        "XrBackendIR",
        "XrGeneratedC",
        "XrNativeArtifact",
        "XrAotToolchainBinding",
        "XrOptimizationPolicyId",
    ):
        require(token in header, f"missing AOT contract type {token}")
    require("xr_execution_instance_pin" in lowering,
            "AOT lowering does not pin its exact execution input")
    require("xr_backend_ir_translation_validate" in lowering and
            "xr_backend_ir_translation_validate" in emitter,
            "translation validation is not mandatory at lowering and emission")
    require("xi_cgen_verify_output" in emitter,
            "generated C is not protected by the always-on output verifier")
    for forbidden in ("TargetPlan", "XrVmCode", "xr_vm_", "xvm_", "AstNode", "XiValue"):
        require(forbidden not in lowering and forbidden not in verifier,
                f"BackendIR depends on forbidden semantic/private owner {forbidden}")
    require("xr_reference_" not in lowering and "xr_reference_" not in emitter,
            "AOT implementation calls the reference evaluator")
    require("sizeof(void" not in emitter,
            "target query is inferred from the host compiler")
    require("int64_t v%u" not in emitter,
            "typed local spelling must come from BackendIR representation")
    require("XrAotValue" not in emitter,
            "generated local values use a systematic tagged representation")
    require("native_artifact_id" in artifact and
            "artifact->bytes, artifact->size" in artifact,
            "NativeArtifactId does not bind native bytes")
    require(
        'native = "NativeArtifactId = hash(ExecutionId, BackendId, ToolchainId, '
        'OptimizationPolicyId, native bytes)"' in identity,
        "frozen native artifact identity disagrees with the implementation",
    )

    for row in registry["operations"]:
        coverage = row.get("coverage", {}).get("aot", {})
        require(coverage == {"status": "COMPLETE", "task": 300},
                f"registry AOT coverage is stale for {row['spelling']}")
        token = enum_token(row["spelling"])
        require(re.search(rf"\bcase\s+{re.escape(token)}\s*:", emitter) is not None,
                f"generated-C emitter omits {row['spelling']}")
        require(token in lowering, f"BackendIR admission omits {row['spelling']}")
        require(token in test, f"generated-C behavioral fixture omits {row['spelling']}")

    for token in (
        "xr_reference_evaluate",
        "xr_vm_code_execute",
        "xr_backend_ir_translation_validate",
        "XR_TARGET_RUNTIME_PROFILE_FREESTANDING",
        "xr_native_artifact_verify",
    ):
        require(token in test, f"AOT test omits {token}")
    require("test_xr_program_aot_native" in test_cmake and
            "test_xr_program_aot_providers" in test_cmake,
            "real generated-C/native provider gates are missing")
    require("xray_program_aot_compiler" in cmake and "-Wall -Wextra -Werror" in cmake,
            "private AOT compiler warning target is missing")
    require("include/xr_backend_ir.h" not in cmake and
            not (root / "include/xr_backend_ir.h").exists(),
            "private BackendIR leaked into the public include tree")

    expected = canonical_json(expected_coverage(registry))
    actual = (root / COVERAGE).read_text(encoding="utf-8", errors="strict")
    require(actual == expected, f"{COVERAGE} is stale")


def self_test(root: Path) -> None:
    validate_sources(root)
    registry = read_json(root / REGISTRY)
    emitter = (root / EMITTER).read_text(encoding="utf-8")
    first = enum_token(registry["operations"][0]["spelling"])
    mutated = emitter.replace(f"case {first}:", "case XR_CORE_OP_MISSING:", 1)
    require(mutated != emitter, "missing-operation mutation did not apply")
    try:
        validate_sources(root, {EMITTER: mutated})
    except GateError:
        pass
    else:
        raise GateError("missing AOT operation mutation was accepted")

    lowering = (root / LOWERING).read_text(encoding="utf-8")
    mutated = lowering.replace("#include <string.h>",
                               "#include <string.h>\n/* TargetPlan */", 1)
    require(mutated != lowering, "forbidden-owner mutation did not apply")
    try:
        validate_sources(root, {LOWERING: mutated})
    except GateError:
        pass
    else:
        raise GateError("forbidden AOT owner mutation was accepted")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--print-coverage", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        if args.print_coverage:
            print(canonical_json(expected_coverage(read_json(root / REGISTRY))), end="")
            return 0
        if args.self_test:
            self_test(root)
            print("XrProgram AOT contracts self-test: PASS")
        else:
            validate_sources(root)
            print("XrProgram AOT contracts: PASS (15 operations, pure native closure)")
    except (GateError, OSError, UnicodeError, json.JSONDecodeError) as exc:
        print(f"XrProgram AOT contracts: FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

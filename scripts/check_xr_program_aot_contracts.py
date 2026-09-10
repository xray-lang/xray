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
COPY_EMITTER = Path("src/aot/program/xr_backend_ir_emit_copy.inc.c")
ARTIFACT = Path("src/aot/program/xr_native_artifact.c")
TEST = Path("tests/unit/aot/test_xr_program_aot.c")
CMAKE = Path("CMakeLists.txt")
TEST_CMAKE = Path("tests/unit/CMakeLists.txt")
IDENTITY = Path("contracts/canonical-program/architecture-identity.toml")
EXECUTION_IDENTITY_HEADER = Path("src/execution/xr_execution_identity.h")
EXECUTION_IDENTITY_SOURCE = Path("src/execution/xr_execution_identity.c")
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
            "aot": row["coverage"]["aot"]["status"],
            "backend_ir": row["coverage"]["aot"]["status"],
            "portable_c11": row["coverage"]["aot"]["status"],
            "translation_validation": (
                "STRUCTURAL_EXACT"
                if row["coverage"]["aot"]["status"] == "COMPLETE" else None
            ),
        }
        for row in registry["operations"]
    ]
    incomplete = [
        row["spelling"] for row in registry["operations"]
        if row["coverage"]["aot"]["status"] != "COMPLETE"
    ]
    return {
        "schema": "xray-program-aot-coverage/1",
        "task": 300,
        "input_authority": "XrValidatedProgram + exact XrTargetProfile + XrBackendOptions",
        "execution_authority": "not-an-AOT-compiler-input",
        "private_realization": "XrBackendIR",
        "backend": "xray-c11-aot@1",
        "operation_count": len(operations),
        "operations": operations,
        "incomplete_operations": incomplete,
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
            "live_instance": False,
            "provider_instance": False,
            "generation_lease": False,
            "program_loader": False,
            "vm": False,
            "compiler": False,
            "aot_toolchain": False,
            "runtime_dependency": "libc-only",
        },
        "inactive_contracts": [
            "full-language-operation-families",
            "high-risk-optimizations",
            "public-native-loader-ABI",
            "published-package-format",
        ],
    }


def sources(root: Path, overrides: dict[Path, str] | None = None) -> dict[Path, str]:
    paths = (HEADER, LOWERING, VERIFY, EMITTER, COPY_EMITTER, ARTIFACT, TEST, CMAKE, TEST_CMAKE,
             IDENTITY, EXECUTION_IDENTITY_HEADER, EXECUTION_IDENTITY_SOURCE)
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
    emission = emitter + text[COPY_EMITTER]
    artifact = text[ARTIFACT]
    test = text[TEST]
    cmake = text[CMAKE]
    test_cmake = text[TEST_CMAKE]
    identity = text[IDENTITY]
    execution_identity_header = text[EXECUTION_IDENTITY_HEADER]
    execution_identity_source = text[EXECUTION_IDENTITY_SOURCE]

    for token in (
        "XrBackendIR",
        "XrGeneratedC",
        "XrNativeArtifact",
        "XrAotToolchainBinding",
        "XrOptimizationPolicyId",
    ):
        require(token in header, f"missing AOT contract type {token}")
    aot_sources = header + lowering + verifier + emission + artifact
    for forbidden in (
        "XrInstance",
        "XrExecutionLease",
        "xr_execution_instance_",
        "xr_execution_lease_",
        "XR_BACKEND_INSTANCE_UNAVAILABLE",
    ):
        require(forbidden not in aot_sources,
                f"pure AOT compiler depends on runtime execution authority {forbidden}")
    require(re.search(
        r"xr_backend_ir_build\s*\(\s*const\s+XrValidatedProgram\s*\*\s*program\s*,\s*"
        r"const\s+XrTargetProfile\s*\*\s*profile\s*,\s*"
        r"const\s+XrBackendOptions\s*\*\s*options",
        header,
    ) is not None, "AOT build input is not program + exact profile + backend options")
    require("xr_execution_id_compute(program, profile" in lowering,
            "AOT lowering does not derive identity from its immutable inputs")
    require("xr_execution_id_compute" in execution_identity_header and
            "xray-execution-id-v1" in execution_identity_source,
            "shared pure execution identity owner is missing")
    require("xr_backend_ir_translation_validate" in lowering and
            "xr_backend_ir_translation_validate" in emitter,
            "translation validation is not mandatory at lowering and emission")
    require("xi_cgen_verify_output" in emitter,
            "generated C is not protected by the always-on output verifier")
    for forbidden in ("TargetPlan", "XrVmCode", "xr_vm_", "xvm_", "AstNode", "XiValue"):
        require(forbidden not in aot_sources,
                f"BackendIR depends on forbidden semantic/private owner {forbidden}")
    require("xr_reference_" not in aot_sources,
            "AOT implementation calls the reference evaluator")
    require("sizeof(void" not in emission,
            "target query is inferred from the host compiler")
    require("int64_t v%u" not in emission,
            "typed local spelling must come from BackendIR representation")
    require("XrAotValue" not in emission,
            "generated local values use a systematic tagged representation")
    require(re.search(
        r"case\s+XR_CORE_TYPE_U16\s*:\s*"
        r"representation\s*=\s*XR_BACKEND_VALUE_U16\s*;",
        lowering,
    ) is not None,
            "BackendIR omits the exact u16 representation")
    require(re.search(
        r"case\s+XR_CORE_OP_CORE_TARGET_POINTER_WIDTH\s*:\s*"
        r"return\s+append_format\([^;]*UINT16_C",
        emitter, re.DOTALL,
    ) is not None, "pointer-width generated C is not exact u16")
    for token in (
        "XrAotContext *xr_ctx",
        "xr_aot_alloc(xr_ctx",
        "xr_aot_context_destroy(&xr_ctx)",
        ".data = (void *)existential_payload_",
    ):
        require(token in emitter or token in test,
                f"AOT existential execution-lifetime contract lacks {token}")
    require('.data = (void *)&v' not in emitter,
            "AOT existential carrier points at a callee-local value")
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
        require(coverage.get("status") in {"COMPLETE", "NOT_YET_ACTIVE"} and
                coverage.get("task") == 300,
                f"registry AOT coverage is stale for {row['spelling']}")
        token = enum_token(row["spelling"])
        emitted = re.search(rf"\bcase\s+{re.escape(token)}\s*:", emitter) is not None
        admitted = re.search(rf"\bcase\s+{re.escape(token)}\s*:", lowering) is not None
        active = coverage["status"] == "COMPLETE"
        require(emitted == active,
                f"generated-C emitter lifecycle disagrees with registry for {row['spelling']}")
        require(admitted == active,
                f"BackendIR lifecycle disagrees with registry for {row['spelling']}")
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
    require("add_xr_program_aot_native_case(pointer_width pointer-width 32)" in test_cmake,
            "real pointer-width generated-C/native gate is missing")
    require("xray_program_aot_compiler" in cmake and "-Wall -Wextra -Werror" in cmake,
            "private AOT compiler warning target is missing")
    contract_command = re.search(
        r"add_custom_command\(\s*OUTPUT\s+\$\{XRAY_PROGRAM_AOT_CONTRACT_CHECK_STAMP\}"
        r"(?P<body>.*?)\n\)", cmake, re.DOTALL)
    dependencies = re.search(r"\bDEPENDS\b(?P<paths>.*?)\bCOMMENT\b",
                             contract_command.group("body") if contract_command else "",
                             re.DOTALL)
    require(dependencies is not None and
            "${CMAKE_SOURCE_DIR}/" + COPY_EMITTER.as_posix() in dependencies.group("paths"),
            "AOT contract stamp does not depend on the copy emitter")
    require("src/execution/xr_execution_identity.c" in cmake,
            "pure execution identity is absent from the canonical product closure")
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

    mutated = lowering.replace("#include <string.h>",
                               "#include <string.h>\n/* XrInstance */", 1)
    require(mutated != lowering, "live-instance mutation did not apply")
    try:
        validate_sources(root, {LOWERING: mutated})
    except GateError:
        pass
    else:
        raise GateError("live XrInstance AOT input was accepted")

    copy_emitter = (root / COPY_EMITTER).read_text(encoding="utf-8")
    for forbidden in ("XrInstance", "XrExecutionLease", "xr_execution_instance_",
                      "xr_execution_lease_", "TargetPlan", "XrVmCode", "xr_vm_",
                      "xvm_", "AstNode", "XiValue", "xr_reference_", "sizeof(void",
                      "int64_t v%u", "XrAotValue"):
        try:
            validate_sources(root, {COPY_EMITTER: copy_emitter + f"\n/* {forbidden} */\n"})
        except GateError:
            pass
        else:
            raise GateError(f"copy emitter accepted forbidden authority/representation {forbidden}")

    cmake = (root / CMAKE).read_text(encoding="utf-8")
    dependency = "            ${CMAKE_SOURCE_DIR}/" + COPY_EMITTER.as_posix() + "\n"
    require(cmake.count(dependency) == 1, "copy emitter dependency mutation is ambiguous")
    try:
        validate_sources(root, {CMAKE: cmake.replace(dependency, "", 1)})
    except GateError:
        pass
    else:
        raise GateError("missing copy emitter contract dependency was accepted")

    mutated, mutation_count = re.subn(
        r"(case\s+XR_CORE_OP_CORE_TARGET_POINTER_WIDTH\s*:\s*"
        r"return\s+append_format\([^;]*?)UINT16_C",
        lambda match: match.group(1) + "UINT32_C",
        emitter,
        count=1,
        flags=re.DOTALL,
    )
    require(mutation_count == 1, "pointer-width generated-C mutation did not apply exactly once")
    try:
        validate_sources(root, {EMITTER: mutated})
    except GateError:
        pass
    else:
        raise GateError("legacy u32 pointer-width generated C was accepted")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
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
            print(f"XrProgram AOT contracts: generated {COVERAGE}")
            return 0
        if args.print_coverage:
            print(canonical_json(expected_coverage(read_json(root / REGISTRY))), end="")
            return 0
        if args.self_test:
            self_test(root)
            print("XrProgram AOT contracts self-test: PASS")
        else:
            validate_sources(root)
            count = len(read_json(root / REGISTRY)["operations"])
            print(f"XrProgram AOT contracts: PASS ({count} operations, pure native closure)")
    except (GateError, OSError, UnicodeError, json.JSONDecodeError) as exc:
        print(f"XrProgram AOT contracts: FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

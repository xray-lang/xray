#!/usr/bin/env python3
"""Validate and, on request, enforce the W7 Wave 4 exit contract.

The ordinary mode is an inventory gate: it validates every completed executor
surface, the forbidden-owner zero scan, and the truthfulness of the OPEN/READY
state.  ``--require-ready`` is the actual exit gate and stays red until every
source and three-channel differential entry is present.  The frozen pre-cut
product route is deliberately outside the zero scan because its physical
deletion belongs to the later atomic product cutover.
"""

from __future__ import annotations

import argparse
import ast
import json
import re
import shutil
import sys
import tempfile
from pathlib import Path


class ContractError(ValueError):
    """Raised when the Wave 4 exit contract is inconsistent."""


EXPECTED_OPERATIONS = [
    (38, "core.call.indirect_direct", "XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT", "indirect-call"),
    (39, "core.call.indirect_invoke", "XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE", "indirect-invoke"),
    (40, "core.call.witness_direct", "XR_CORE_OP_CORE_CALL_WITNESS_DIRECT", "witness-call"),
    (41, "core.call.witness_invoke", "XR_CORE_OP_CORE_CALL_WITNESS_INVOKE", "witness-invoke"),
    (86, "core.existential.pack", "XR_CORE_OP_CORE_EXISTENTIAL_PACK", "existential-pack"),
    (87, "core.existential.test", "XR_CORE_OP_CORE_EXISTENTIAL_TEST", "existential-test"),
    (88, "core.existential.project", "XR_CORE_OP_CORE_EXISTENTIAL_PROJECT", "existential-project"),
    (89, "core.callable.pack", "XR_CORE_OP_CORE_CALLABLE_PACK", "callable-pack"),
]

EXPECTED_SURFACES = {
    "spec_oracle": "tools/corespecgen/corespecgen.py",
    "source_producer": "src/program/xr_program_from_xi.c",
    "source_end_to_end": "tests/unit/ir/test_xi_pipeline.c",
    "verifier": "src/program/xr_program_verify.c",
    "verifier_test": "tests/unit/program/test_xr_program_verify.c",
    "reference": "src/program/xr_reference_evaluator.c",
    "reference_test": "tests/unit/program/test_xr_program_verify.c",
    "vm": "src/vm/xr_program_vm.c",
    "vm_test": "tests/unit/vm/test_xr_program_vm.c",
    "aot": "src/aot/program/xr_backend_ir_emit_c.c",
    "aot_test": "tests/unit/aot/test_xr_program_aot.c",
}

EXECUTOR_SURFACES = (
    "verifier",
    "verifier_test",
    "reference",
    "reference_test",
    "vm",
    "vm_test",
    "aot",
    "aot_test",
)

EXPECTED_CATEGORIES = (
    "CallDecision",
    "selector/name fallback",
    "class-only itable",
    "erased closure ABI",
    "backend signature recovery",
)

EXPECTED_DIFFERENTIAL_TESTS = (
    "test_xi_pipeline",
    "test_xr_program_imported_callable",
    "test_xr_program_vm",
    "test_xr_program_aot",
    "test_xr_program_aot_callable",
    "test_xr_program_aot_existential",
    "test_xr_program_source_aot_native",
    "test_xr_program_imported_callable_native",
)

EXPECTED_DIFFERENTIAL_REGEX = (
    "^(test_xi_pipeline|test_xr_program_imported_callable|test_xr_program_vm|"
    "test_xr_program_aot|test_xr_program_aot_callable|"
    "test_xr_program_aot_existential|test_xr_program_source_aot_native|"
    "test_xr_program_imported_callable_native)$"
)

EXPECTED_DIFFERENTIAL_COMMAND = (
    "ctest --test-dir build --output-on-failure -R " + EXPECTED_DIFFERENTIAL_REGEX
)

EXPECTED_NORMAL_QUALIFICATION = (
    "ctest --test-dir build --output-on-failure -R "
    "^(xr_program_wave4_exit_inventory|xr_program_wave4_exit_self_test|test_xi_lower|"
    "test_xi_pipeline|test_xr_program_imported_callable|test_xr_program_verify|"
    "test_xr_program_vm|test_xr_program_aot|test_xr_program_aot_callable|"
    "test_xr_program_aot_existential|test_xr_program_source_aot_native|"
    "test_xr_program_imported_callable_native)$"
)

EXPECTED_ASAN_QUALIFICATION = (
    "ctest --test-dir build-asan --output-on-failure -R "
    "^(test_xi_lower|test_xi_pipeline|test_xr_program_imported_callable|"
    "test_xr_program_verify|test_xr_program_vm|test_xr_program_aot)$"
)

EXPECTED_AOT_NATIVE_FORBIDDEN = (
    r"xr_program_(?:validate|write|decode)",
    r"xr_backend_ir",
    r"xr_vm_",
    r"xvm_",
    r"xaot_",
    r"xr_target_plan",
    r"xr_core_ir_",
    r"xi_(?:lower|pipeline|cgen)",
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractError(message)


def load(root: Path, relative: str) -> dict[str, object]:
    path = root / relative
    raw = path.read_text(encoding="utf-8", errors="strict")
    value = json.loads(raw)
    require(isinstance(value, dict), f"{relative} must contain an object")
    require(raw == json.dumps(value, ensure_ascii=False, indent=2) + "\n",
            f"{relative} is not canonical JSON")
    return value


def read(root: Path, relative: str) -> str:
    return (root / relative).read_text(encoding="utf-8", errors="strict")


def c_code_only(text: str) -> str:
    """Remove comments and literal contents before looking for C syntax anchors."""
    output: list[str] = []
    index = 0
    state = "code"
    while index < len(text):
        char = text[index]
        following = text[index + 1] if index + 1 < len(text) else ""
        if state == "code":
            if char == "/" and following == "/":
                output.extend("  ")
                index += 2
                state = "line-comment"
                continue
            if char == "/" and following == "*":
                output.extend("  ")
                index += 2
                state = "block-comment"
                continue
            if char == '"':
                output.append(" ")
                state = "string"
            elif char == "'":
                output.append(" ")
                state = "character"
            else:
                output.append(char)
            index += 1
            continue
        if state == "line-comment":
            output.append("\n" if char == "\n" else " ")
            if char == "\n":
                state = "code"
            index += 1
            continue
        if state == "block-comment":
            if char == "*" and following == "/":
                output.extend("  ")
                index += 2
                state = "code"
            else:
                output.append("\n" if char == "\n" else " ")
                index += 1
            continue
        if char == "\\" and following:
            output.extend("  ")
            index += 2
        elif ((state == "string" and char == '"') or
              (state == "character" and char == "'")):
            output.append(" ")
            index += 1
            state = "code"
        else:
            output.append("\n" if char == "\n" else " ")
            index += 1
    return "".join(output)


def python_assignment(tree: ast.Module, name: str) -> object:
    for node in tree.body:
        if not isinstance(node, ast.Assign) or len(node.targets) != 1:
            continue
        target = node.targets[0]
        if isinstance(target, ast.Name) and target.id == name:
            try:
                return ast.literal_eval(node.value)
            except (ValueError, TypeError) as exc:
                raise ContractError(f"Wave 4 runner {name} is not a literal") from exc
    raise ContractError(f"Wave 4 runner lacks {name}")


def python_expression_name(node: ast.AST) -> str | None:
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    if isinstance(node, ast.Name):
        return node.id
    if isinstance(node, ast.Attribute):
        owner = python_expression_name(node.value)
        return f"{owner}.{node.attr}" if owner else None
    if (isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and
            node.func.id == "str" and len(node.args) == 1 and not node.keywords):
        argument = python_expression_name(node.args[0])
        return f"str({argument})" if argument else None
    return None


def python_run_vectors(tree: ast.Module) -> list[tuple[str | None, ...]]:
    vectors: list[tuple[str | None, ...]] = []
    for node in ast.walk(tree):
        if not (isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and
                node.func.id == "run" and node.args and isinstance(node.args[0], ast.List)):
            continue
        vectors.append(tuple(python_expression_name(item) for item in node.args[0].elts))
    return vectors


def python_call_names(tree: ast.Module) -> set[str]:
    names: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Call):
            name = python_expression_name(node.func)
            if name:
                names.add(name)
    return names


def validate_differential_runner(runner: str) -> None:
    try:
        tree = ast.parse(runner)
    except SyntaxError as exc:
        raise ContractError(f"Wave 4 differential runner is invalid Python: {exc}") from exc
    require(python_assignment(tree, "EXPECTED_TESTS") == EXPECTED_DIFFERENTIAL_TESTS,
            "Wave 4 differential runner discovery set drifted")
    require(python_assignment(tree, "TEST_REGEX") == EXPECTED_DIFFERENTIAL_REGEX,
            "Wave 4 differential runner test set drifted")
    vectors = python_run_vectors(tree)
    require(("sys.executable", "str(gate)", "--root", "str(root)", "--require-ready")
            in vectors, "Wave 4 differential runner does not enforce the READY gate")
    require(("args.ctest", "--test-dir", "str(build_dir)", "--show-only=json-v1", "-R",
             "TEST_REGEX") in vectors,
            "Wave 4 differential runner does not verify its exact CTest discovery")
    require(("args.ctest", "--test-dir", "str(build_dir)", "--output-on-failure", "-R",
             "TEST_REGEX") in vectors,
            "Wave 4 differential runner does not execute its declared CTest set")


def validate_aot_native_gate(gate: str) -> None:
    try:
        tree = ast.parse(gate)
    except SyntaxError as exc:
        raise ContractError(f"Wave 4 native-AOT gate is invalid Python: {exc}") from exc
    require(python_assignment(tree, "FORBIDDEN") == EXPECTED_AOT_NATIVE_FORBIDDEN,
            "Wave 4 native-AOT forbidden-symbol inventory drifted")
    calls = python_call_names(tree)
    require("toolchain.find_symbol_dumper" in calls,
            "Wave 4 native-AOT gate does not require a verified symbol dumper")
    require("dumper.dump_defined_symbols" in calls,
            "Wave 4 native-AOT gate does not consume normalized defined symbols")
    require("symbol_text.strip" in calls,
            "Wave 4 native-AOT gate does not reject an empty symbol inventory")
    require("expected_process_exit" in calls,
            "Wave 4 native-AOT gate does not normalize host process exit status")


def cmake_code_only(text: str) -> str:
    output: list[str] = []
    quoted = False
    escaped = False
    for line in text.splitlines(keepends=True):
        index = 0
        while index < len(line):
            char = line[index]
            if not quoted and char == "#":
                output.extend(" " for _ in line[index:].rstrip("\r\n"))
                output.extend(line[len(line.rstrip("\r\n")):])
                break
            output.append(char)
            if quoted:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == '"':
                    quoted = False
            elif char == '"':
                quoted = True
            index += 1
    return "".join(output)


def cmake_invocations(text: str, command: str) -> list[str]:
    source = cmake_code_only(text)
    pattern = re.compile(rf"(?i)(?<![A-Za-z0-9_]){re.escape(command)}\s*\(")
    invocations: list[str] = []
    for match in pattern.finditer(source):
        start = match.end()
        depth = 1
        quoted = False
        escaped = False
        index = start
        while index < len(source) and depth:
            char = source[index]
            if quoted:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == '"':
                    quoted = False
            elif char == '"':
                quoted = True
            elif char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
            index += 1
        require(depth == 0, f"unterminated CMake {command} invocation")
        invocations.append(" ".join(source[start:index - 1].split()))
    return invocations


def cmake_has_invocation(text: str, command: str, required: tuple[str, ...]) -> bool:
    return any(all(fragment in invocation for fragment in required)
               for invocation in cmake_invocations(text, command))


def validate_source_native_targets(cmake: str) -> list[str]:
    gaps: list[str] = []
    cases = (
        (
            "source existential/witness/callable",
            "XR_PROGRAM_SOURCE_AOT_GENERATED_C",
            "source_aot_native.c",
            "test_xi_pipeline",
            "--source-aot-c ${XR_PROGRAM_SOURCE_AOT_GENERATED_C}",
            "test_xr_program_source_aot_native",
            "477",
        ),
        (
            "source imported callable",
            "XR_PROGRAM_IMPORTED_CALLABLE_GENERATED_C",
            "imported_callable_native.c",
            "test_xr_program_imported_callable",
            "${XR_PROGRAM_IMPORTED_CALLABLE_GENERATED_C}",
            "test_xr_program_imported_callable_native",
            "42",
        ),
    )
    for (label, generated, generated_name, producer, producer_arguments, executable,
         expected_exit) in cases:
        if not cmake_has_invocation(cmake, "set", (generated, generated_name)):
            gaps.append(f"{label} generated-C output is not declared")
        if not cmake_has_invocation(cmake, "add_custom_command", (
                f"OUTPUT ${{{generated}}}",
                f"COMMAND $<TARGET_FILE:{producer}> {producer_arguments}",
                f"DEPENDS {producer}",
        )):
            gaps.append(f"{label} generated-C command is not wired")
        if not cmake_has_invocation(cmake, "add_executable", (
                executable, f"${{{generated}}}",
        )):
            gaps.append(f"{label} native executable is not wired")
        if not cmake_has_invocation(cmake, "target_compile_options", (
                executable, "PRIVATE /W4 /WX",
        )) or not cmake_has_invocation(cmake, "target_compile_options", (
                executable, "PRIVATE -std=c11 -pedantic-errors -Wall -Wextra -Werror",
        )):
            gaps.append(f"{label} native executable lacks strict provider diagnostics")
        if not cmake_has_invocation(cmake, "add_test", (
                f"NAME {executable}",
                "${CMAKE_SOURCE_DIR}/scripts/check_xr_program_aot_native.py",
                f"--executable $<TARGET_FILE:{executable}>",
                f"--expected-exit {expected_exit}",
        )):
            gaps.append(f"{label} native execution CTest is not wired")
    return gaps


def validate_qualification_cmake(root: Path) -> list[str]:
    gaps: list[str] = []
    top_level = read(root, "CMakeLists.txt")
    unit = read(root, "tests/unit/CMakeLists.txt")
    if not cmake_has_invocation(top_level, "set", (
            "PROGRAMWAVE4EXITCHECK",
            "${CMAKE_SOURCE_DIR}/scripts/check_xr_program_wave4_exit.py",
    )):
        gaps.append("top-level Wave 4 exit checker path is not declared")
    for name, extra in (
        ("xr_program_wave4_exit_inventory", "--root ${CMAKE_CURRENT_SOURCE_DIR}"),
        ("xr_program_wave4_exit_self_test", "--self-test"),
    ):
        if not cmake_has_invocation(top_level, "add_test", (
                f"NAME {name}",
                "COMMAND ${XRAY_PYTHON} ${PROGRAMWAVE4EXITCHECK}",
                extra,
        )):
            gaps.append(f"top-level CTest {name} is not wired")

    unit_calls = (
        ("add_xray_unit_test", ("test_xi_lower", "ir/test_xi_lower.c")),
        ("add_xray_unit_test", ("test_xi_pipeline", "ir/test_xi_pipeline.c")),
        ("add_xray_unit_test", ("test_xr_program_imported_callable",
                                "ir/test_xr_program_imported_callable.c")),
        ("add_xray_unit_test", ("test_xr_program_vm", "vm/test_xr_program_vm.c")),
        ("add_xray_unit_test", ("test_xr_program_aot", "aot/test_xr_program_aot.c")),
        ("add_test", ("NAME test_xr_program_verify",
                      "COMMAND $<TARGET_FILE:test_xr_program_verify>")),
        ("add_xr_program_aot_native_case", ("existential existential 42",)),
        ("add_xr_program_aot_native_case", ("callable callable 42",)),
    )
    for command, required in unit_calls:
        if not cmake_has_invocation(unit, command, required):
            gaps.append(f"unit CMake lacks {required[0]}")
    return gaps


def validate_source_execution_paths(root: Path) -> list[str]:
    gaps: list[str] = []
    paths = (
        (
            "single-module source differential",
            "tests/unit/ir/test_xi_pipeline.c",
            (
                "run_e2e_program_input_stops_before_legacy_semantic_and_backend_owners();",
                "validated_program_has_operation(validated, required_source_operations[index])",
                "xr_reference_evaluate(",
                "reference.value.as.i64 == 477",
                "xr_vm_code_execute(",
                "vm.value.as.i64 == reference.value.as.i64",
                "xr_backend_ir_translation_validate(",
                "xr_backend_ir_emit_c(",
                "fwrite(generated.bytes",
            ),
        ),
        (
            "imported-callable source differential",
            "tests/unit/ir/test_xr_program_imported_callable.c",
            (
                "RUN_TEST(test_imported_callable_multi_target_program);",
                "XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT",
                "callsite->callable_target_count == 2u",
                "xr_reference_evaluate(",
                "reference.value.as.i64, 42",
                "xr_vm_code_execute(",
                "vm.value.as.i64, reference.value.as.i64",
                "xr_backend_ir_translation_validate(",
                "xr_backend_ir_emit_c(",
                "fwrite(generated.bytes",
            ),
        ),
    )
    for label, relative, anchors in paths:
        code = c_code_only(read(root, relative))
        for anchor in anchors:
            if anchor not in code:
                gaps.append(f"{label} lacks executable anchor {anchor}")
    return gaps


def evidence_gaps(root: Path, evidence: object) -> list[str]:
    require(isinstance(evidence, list) and evidence, "source-domain evidence is absent")
    gaps: list[str] = []
    for row in evidence:
        require(isinstance(row, dict) and set(row) == {"path", "tokens"},
                "source-domain evidence row is malformed")
        relative = row["path"]
        tokens = row["tokens"]
        require(isinstance(relative, str) and relative, "source-domain path is malformed")
        require(isinstance(tokens, list) and tokens and
                all(isinstance(token, str) and token for token in tokens),
                f"source-domain tokens are malformed: {relative}")
        path = root / relative
        if not path.is_file():
            gaps.append(f"missing {relative}")
            continue
        text = path.read_text(encoding="utf-8", errors="strict")
        for token in tokens:
            if token not in text:
                gaps.append(f"{relative} lacks {token}")
    return gaps


def canonical_pipeline_files(root: Path, zero: dict[str, object]) -> list[Path]:
    scan_roots = zero.get("scan_roots")
    scan_files = zero.get("scan_files")
    require(scan_roots == ["src/program", "src/aot/program", "src/execution"],
            "Wave 4 canonical scan roots drifted")
    require(scan_files == ["src/vm/xr_program_vm.c", "src/vm/xr_program_vm_dispatch.inc.c",
                           "src/vm/xr_program_vm.h"],
            "Wave 4 canonical scan files drifted")
    paths: list[Path] = []
    for relative in scan_roots:
        directory = root / relative
        require(directory.is_dir(), f"canonical scan root is absent: {relative}")
        paths.extend(sorted(path for path in directory.rglob("*")
                            if path.is_file() and path.suffix in {".c", ".h"}))
    for relative in scan_files:
        path = root / relative
        require(path.is_file(), f"canonical scan file is absent: {relative}")
        paths.append(path)
    return paths


def validate_zero_scan(root: Path, zero: object) -> None:
    require(isinstance(zero, dict) and set(zero) == {
        "scan_roots", "scan_files", "categories"
    }, "Wave 4 old-owner zero scan is malformed")
    categories = zero.get("categories")
    require(isinstance(categories, list), "Wave 4 old-owner categories are absent")
    require([row.get("id") for row in categories if isinstance(row, dict)] ==
            list(EXPECTED_CATEGORIES), "Wave 4 old-owner category set drifted")
    files = canonical_pipeline_files(root, zero)
    for row in categories:
        require(isinstance(row, dict) and set(row) == {"id", "tokens"},
                "Wave 4 old-owner category is malformed")
        category = row["id"]
        tokens = row["tokens"]
        require(isinstance(tokens, list) and tokens and
                all(isinstance(token, str) and token for token in tokens),
                f"Wave 4 old-owner token set is malformed: {category}")
        for path in files:
            text = path.read_text(encoding="utf-8", errors="strict")
            for token in tokens:
                require(token not in text,
                        f"new canonical path depends on {category}: "
                        f"{path.relative_to(root)}: {token}")


def validate_operation_evidence(root: Path, data: dict[str, object],
                                blockers: list[str]) -> None:
    operations = data.get("operations")
    require(isinstance(operations, list), "Wave 4 exit operation inventory is absent")
    actual = [(row.get("stable_id"), row.get("id"), row.get("macro"),
               row.get("kat_validator"))
              for row in operations if isinstance(row, dict)]
    require(actual == EXPECTED_OPERATIONS, "Wave 4 exit operation set or order drifted")

    surfaces = data.get("evidence_surfaces")
    require(surfaces == EXPECTED_SURFACES, "Wave 4 evidence surfaces drifted")
    texts = {name: read(root, relative) for name, relative in EXPECTED_SURFACES.items()}
    code = {name: c_code_only(text) for name, text in texts.items()
            if EXPECTED_SURFACES[name].endswith((".c", ".h"))}

    registry = load(root, "xisa/core/registry.json")
    registry_rows = {row.get("spelling"): row for row in registry.get("operations", [])
                     if isinstance(row, dict)}
    kats = load(root, "xisa/core/kats.json")
    kat_rows = {row.get("id"): row for row in kats.get("cases", [])
                if isinstance(row, dict)}
    matrix = load(root, "contracts/canonical-program/operation-capability-matrix.json")
    matrix_rows = {row.get("id"): row for row in matrix.get("operations", [])
                   if isinstance(row, dict)}

    status = data.get("status")
    transition = data.get("matrix_status")
    require(transition == {
        "open": "IN_PROGRESS_W7_WAVE4_EXECUTOR",
        "ready": "COMPLETE_W7_WAVE4",
    }, "Wave 4 matrix status transition drifted")
    require(status in {"OPEN_W7_WAVE4", "READY_W7_WAVE4"},
            "Wave 4 exit status is invalid")
    expected_matrix_status = transition["open" if status == "OPEN_W7_WAVE4" else "ready"]

    for operation in operations:
        require(isinstance(operation, dict) and set(operation) == {
            "stable_id", "id", "macro", "kat_validator", "kats"
        }, "Wave 4 operation row is malformed")
        spelling = operation["id"]
        stable_id = operation["stable_id"]
        macro = operation["macro"]
        kat_validator = operation["kat_validator"]
        required_kats = operation["kats"]
        registry_row = registry_rows.get(spelling)
        require(registry_row is not None and registry_row.get("stable_id") == stable_id,
                f"Wave 4 CoreSpec identity drifted: {spelling}")
        require(registry_row.get("kat_validator") == kat_validator and
                f'"{kat_validator}"' in texts["spec_oracle"],
                f"Wave 4 CoreSpec oracle is absent: {spelling}")
        coverage = registry_row.get("coverage")
        require(isinstance(coverage, dict) and set(coverage) == {
            "spec_oracle", "decoder", "verifier", "evaluator", "vm", "aot"
        }, f"Wave 4 CoreSpec coverage shape drifted: {spelling}")
        for owner, claim in coverage.items():
            require(isinstance(claim, dict) and claim.get("status") == "COMPLETE",
                    f"Wave 4 CoreSpec {owner} is not complete: {spelling}")

        require(isinstance(required_kats, list) and required_kats,
                f"Wave 4 KAT inventory is empty: {spelling}")
        actual_kats = [row.get("id") for row in kats.get("cases", [])
                       if isinstance(row, dict) and row.get("operation") == spelling]
        require(actual_kats == required_kats, f"Wave 4 KAT set drifted: {spelling}")
        validity = {kat_rows[kat].get("expect", {}).get("valid") for kat in required_kats}
        require(validity == {False, True},
                f"Wave 4 KATs lack positive/negative mutation evidence: {spelling}")

        for surface in EXECUTOR_SURFACES:
            require(macro in code[surface],
                    f"Wave 4 {surface} evidence lacks {spelling}: {macro}")
        missing_source = [name for name in ("source_producer", "source_end_to_end")
                          if macro not in code[name]]
        if missing_source:
            blockers.append(f"{spelling}: missing source evidence in " +
                            ", ".join(missing_source))
        for surface in ("reference", "vm", "aot"):
            require(re.search(rf"\bcase\s+{re.escape(macro)}\b", code[surface]) is not None,
                    f"Wave 4 {surface} lacks an executable case for {spelling}")

        matrix_row = matrix_rows.get(spelling)
        require(matrix_row is not None, f"Wave 4 matrix row is absent: {spelling}")
        require(matrix_row.get("status") == expected_matrix_status,
                f"Wave 4 matrix status is not truthful: {spelling}: "
                f"expected {expected_matrix_status}, got {matrix_row.get('status')}")
        evidence = matrix_row.get("evidence")
        require(isinstance(evidence, list), f"Wave 4 matrix evidence is malformed: {spelling}")
        required_matrix_evidence = {
            "xisa/core/registry.json",
            "xisa/program/schema.json",
            EXPECTED_SURFACES["verifier"],
            EXPECTED_SURFACES["reference"],
            EXPECTED_SURFACES["vm"],
            EXPECTED_SURFACES["aot"],
        }
        require(required_matrix_evidence <= set(evidence),
                f"Wave 4 matrix executor evidence is incomplete: {spelling}")
        if status == "READY_W7_WAVE4":
            require({EXPECTED_SURFACES["source_producer"],
                     EXPECTED_SURFACES["source_end_to_end"]} <= set(evidence),
                    f"Wave 4 completed matrix row lacks source evidence: {spelling}")
            if spelling in {"core.call.indirect_direct", "core.callable.pack"}:
                require("tests/unit/ir/test_xr_program_imported_callable.c" in evidence,
                        f"Wave 4 completed matrix row lacks imported source evidence: {spelling}")
            source_claim = matrix_row.get("source_producer")
            deletion_claim = matrix_row.get("old_owner_deletion")
            require(isinstance(source_claim, str) and source_claim and
                    not source_claim.startswith("pending") and
                    isinstance(deletion_claim, str) and deletion_claim and
                    not deletion_claim.startswith("pending"),
                    f"Wave 4 completed matrix row still claims pending work: {spelling}")
        else:
            require(str(matrix_row.get("source_producer", "")).startswith("pending") and
                    str(matrix_row.get("old_owner_deletion", "")).startswith("pending"),
                    f"Wave 4 open matrix row lost its pending boundary: {spelling}")

    test_anchors = {
        "verifier_test": (
            "test_existential_pack_test_project();",
            "test_callable_pack_and_indirect_calls();",
        ),
        "vm_test": (
            "test_existential_pack_test_project();",
            "test_callable_pack_and_indirect_calls();",
        ),
        "aot_test": (
            "test_existential_pack_test_project_lowering();",
            "test_callable_pack_and_indirect_call_lowering();",
        ),
    }
    for surface, anchors in test_anchors.items():
        for anchor in anchors:
            require(anchor in code[surface],
                    f"Wave 4 {surface} does not execute {anchor[:-3]}")

    groups = matrix.get("capability_groups")
    require(isinstance(groups, list), "Wave 4 capability groups are absent")
    interface = next((row for row in groups
                      if isinstance(row, dict) and row.get("id") == "interface"), None)
    require(interface is not None, "Wave 4 interface capability group is absent")
    expected_group_status = ("EXECUTOR_COMPLETE_SOURCE_PENDING"
                             if status == "OPEN_W7_WAVE4" else "COMPLETE_W7_WAVE4")
    require(interface.get("status") == expected_group_status,
            "Wave 4 interface capability-group status is not truthful: "
            f"expected {expected_group_status}, got {interface.get('status')}")


def validate_source_domains(root: Path, data: dict[str, object],
                            blockers: list[str]) -> None:
    domains = data.get("source_domains")
    require(isinstance(domains, list) and [row.get("id") for row in domains
                                          if isinstance(row, dict)] == [
        "callable-captureless-captured-fallible",
        "callable-escape-multiuse-cross-cfg-loop-copy",
        "imported-callable-multi-target-union",
        "class-struct-enum-existential-pack",
        "existential-test-project",
        "witness-direct-invoke",
    ], "Wave 4 source-domain inventory drifted")
    for row in domains:
        require(isinstance(row, dict) and set(row) == {"id", "evidence"},
                "Wave 4 source-domain row is malformed")
        gaps = evidence_gaps(root, row["evidence"])
        if gaps:
            blockers.append(f"source domain {row['id']}: " + "; ".join(gaps))


def validate_differential_entry(root: Path, data: dict[str, object],
                                blockers: list[str]) -> None:
    entry = data.get("three_channel_differential_entry")
    require(isinstance(entry, dict) and set(entry) == {
        "entry_script", "command", "channels", "source_native_aot"
    }, "Wave 4 three-channel differential entry is malformed")
    require(entry.get("entry_script") == "scripts/run_xr_program_wave4_differential.py",
            "Wave 4 differential entry script drifted")
    runner = read(root, "scripts/run_xr_program_wave4_differential.py")
    validate_differential_runner(runner)
    validate_aot_native_gate(read(root, "scripts/check_xr_program_aot_native.py"))
    require(entry.get("command") == EXPECTED_DIFFERENTIAL_COMMAND,
            "Wave 4 differential command drifted")
    channels = entry.get("channels")
    require(isinstance(channels, list) and [row.get("id") for row in channels
                                           if isinstance(row, dict)] == [
        "source-reference",
        "source-vm",
        "source-aot-translation",
        "native-aot-executor-fixtures",
    ], "Wave 4 differential channel set drifted")
    for row in channels:
        gaps = evidence_gaps(root, [{"path": row.get("path"), "tokens": row.get("tokens")}])
        require(not gaps, f"Wave 4 differential channel is incomplete: {row.get('id')}: " +
                "; ".join(gaps))
    require(entry.get("source_native_aot") == {
        "path": "tests/unit/CMakeLists.txt",
        "tokens": [
            "test_xr_program_source_aot_native",
            "test_xr_program_imported_callable_native",
        ],
    }, "Wave 4 source native-AOT evidence set drifted")
    native_gaps = evidence_gaps(root, [entry.get("source_native_aot")])
    native_gaps.extend(validate_source_native_targets(
        read(root, "tests/unit/CMakeLists.txt")))
    native_gaps.extend(validate_source_execution_paths(root))
    if native_gaps:
        blockers.append("three-channel source differential: " + "; ".join(native_gaps))


def validate_freeze_state(root: Path, status: object) -> None:
    freeze = load(root, "contracts/canonical-program/w7-wave4-contract-freeze.json")
    implementation = freeze.get("implementation_state")
    require(isinstance(implementation, dict), "Wave 4 implementation state is absent")
    if status == "OPEN_W7_WAVE4":
        require(implementation.get("source_integration") == "PENDING" and
                implementation.get("wave_closure") == "PENDING" and
                implementation.get("existential_operations") ==
                    "EXECUTOR_COMPLETE_SOURCE_PENDING" and
                implementation.get("witness_dispatch") ==
                    "EXECUTOR_COMPLETE_SOURCE_PENDING" and
                implementation.get("callable_dispatch") ==
                    "EXECUTOR_COMPLETE_SOURCE_PENDING",
                "Wave 4 OPEN exit state disagrees with the contract freeze")
    else:
        require(implementation.get("source_integration") == "COMPLETE" and
                implementation.get("wave_closure") == "COMPLETE" and
                implementation.get("existential_operations") == "COMPLETE" and
                implementation.get("witness_dispatch") == "COMPLETE" and
                implementation.get("callable_dispatch") == "COMPLETE",
                "Wave 4 READY exit state disagrees with the contract freeze")


def validate(root: Path) -> list[str]:
    data = load(root, "contracts/canonical-program/w7-wave4-exit-gate.json")
    require(data.get("schema") == "xray-w7-wave4-exit-gate/1",
            "Wave 4 exit schema drifted")
    require(data.get("owner_tasks") == [293, 301], "Wave 4 exit owners drifted")
    require(data.get("compatibility") == "none", "Wave 4 regained compatibility")
    require(data.get("precut_product_route") ==
            "frozen during W7; physical deletion belongs to atomic Task 302",
            "Wave 4 pre-cut product boundary drifted")
    qualification = data.get("qualification_entries")
    require(isinstance(qualification, dict) and set(qualification) == {
        "normal", "asan_ubsan", "result_claims"
    }, "Wave 4 qualification entry set drifted")
    require(qualification.get("normal") == EXPECTED_NORMAL_QUALIFICATION and
            qualification.get("asan_ubsan") == EXPECTED_ASAN_QUALIFICATION and
            qualification.get("result_claims") == [],
            "Wave 4 qualification entries drifted or contain an unverified result claim")

    blockers: list[str] = []
    validate_operation_evidence(root, data, blockers)
    validate_source_domains(root, data, blockers)
    validate_zero_scan(root, data.get("old_owner_new_canonical_path_zero"))
    validate_differential_entry(root, data, blockers)
    cmake_gaps = validate_qualification_cmake(root)
    if cmake_gaps:
        blockers.append("Wave 4 CMake qualification: " + "; ".join(cmake_gaps))
    validate_freeze_state(root, data.get("status"))
    if data.get("status") == "OPEN_W7_WAVE4":
        require(blockers, "Wave 4 has no blocker but its exit state is still OPEN")
    else:
        require(not blockers, "Wave 4 READY state has unresolved blockers: " +
                " | ".join(blockers))
    return blockers


def copy_input(root: Path, target: Path, relative: str) -> None:
    source = root / relative
    if not source.exists():
        return
    destination = target / relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    if source.is_dir():
        shutil.copytree(source, destination)
    else:
        shutil.copy2(source, destination)


def expect_failure(target: Path, label: str) -> None:
    try:
        validate(target)
    except ContractError:
        return
    raise ContractError(f"injected {label} mutation was accepted")


def expect_blocker_or_failure(target: Path, baseline_count: int, label: str) -> None:
    try:
        blockers = validate(target)
    except ContractError:
        return
    require(len(blockers) > baseline_count,
            f"injected {label} mutation did not add a blocker")


def self_test(root: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="xray-wave4-exit-") as temporary:
        target = Path(temporary)
        for relative in (
            "CMakeLists.txt",
            "contracts/canonical-program/w7-wave4-exit-gate.json",
            "contracts/canonical-program/w7-wave4-contract-freeze.json",
            "contracts/canonical-program/operation-capability-matrix.json",
            "xisa/core/registry.json",
            "xisa/core/kats.json",
            "tools/corespecgen/corespecgen.py",
            "src/program",
            "src/aot/program",
            "src/execution",
            "src/vm/xr_program_vm.c",
            "src/vm/xr_program_vm_dispatch.inc.c",
            "src/vm/xr_program_vm.h",
            "tests/unit/ir/test_xi_pipeline.c",
            "tests/unit/ir/test_xi_lower.c",
            "tests/unit/ir/test_xr_program_imported_callable.c",
            "tests/unit/program/test_xr_program_verify.c",
            "tests/unit/vm/test_xr_program_vm.c",
            "tests/unit/aot/test_xr_program_aot.c",
            "tests/unit/CMakeLists.txt",
            "scripts/check_xr_program_aot_native.py",
            "scripts/run_xr_program_wave4_differential.py",
        ):
            copy_input(root, target, relative)
        baseline_blockers = validate(target)
        exit_path = target / "contracts/canonical-program/w7-wave4-exit-gate.json"
        exit_data = json.loads(exit_path.read_text(encoding="utf-8"))
        baseline_open = exit_data["status"] == "OPEN_W7_WAVE4"
        require(bool(baseline_blockers) == baseline_open,
                "Wave 4 exit self-test baseline is not truthful")

        matrix_path = target / "contracts/canonical-program/operation-capability-matrix.json"
        original_matrix = matrix_path.read_text(encoding="utf-8")
        matrix = json.loads(original_matrix)
        for row in matrix["operations"]:
            if row.get("id") == EXPECTED_OPERATIONS[0][1]:
                row["status"] = ("COMPLETE_W7_WAVE4" if baseline_open else
                                 "IN_PROGRESS_W7_WAVE4_EXECUTOR")
                break
        matrix_path.write_text(json.dumps(matrix, ensure_ascii=False, indent=2) + "\n",
                               encoding="utf-8")
        expect_failure(target, "matrix status mismatch")
        matrix_path.write_text(original_matrix, encoding="utf-8")

        matrix = json.loads(original_matrix)
        for group in matrix["capability_groups"]:
            if group.get("id") == "interface":
                group["status"] = ("COMPLETE_W7_WAVE4" if baseline_open else
                                   "EXECUTOR_COMPLETE_SOURCE_PENDING")
                break
        matrix_path.write_text(json.dumps(matrix, ensure_ascii=False, indent=2) + "\n",
                               encoding="utf-8")
        expect_failure(target, "interface capability-group status mismatch")
        matrix_path.write_text(original_matrix, encoding="utf-8")

        for relative in ("src/program/xr_program_identity.c",
                         "src/vm/xr_program_vm_dispatch.inc.c"):
            victim = target / relative
            original_victim = victim.read_text(encoding="utf-8")
            victim.write_text(original_victim + "\n/* injected XrScalarCallDecision */\n",
                              encoding="utf-8")
            expect_failure(target, "old-owner")
            victim.write_text(original_victim, encoding="utf-8")

        reference = target / EXPECTED_SURFACES["reference"]
        original_reference = reference.read_text(encoding="utf-8")
        reference.write_text(original_reference.replace(EXPECTED_OPERATIONS[0][2],
                                                        "INJECTED_MISSING_OPERATION") +
                             f"\n/* decoy {EXPECTED_OPERATIONS[0][2]} */\n",
                             encoding="utf-8")
        expect_failure(target, "executor evidence removal with comment decoy")
        reference.write_text(original_reference, encoding="utf-8")

        runner_path = target / "scripts/run_xr_program_wave4_differential.py"
        original_runner = runner_path.read_text(encoding="utf-8")
        runner_path.write_text(
            original_runner.replace("test_xr_program_imported_callable_native",
                                    "INJECTED_MISSING_NATIVE_TEST") +
            "\n# decoy test_xr_program_imported_callable_native\n",
            encoding="utf-8",
        )
        expect_failure(target, "differential runner test removal with comment decoy")
        runner_path.write_text(original_runner, encoding="utf-8")

        runner_path.write_text(
            original_runner.replace('"--show-only=json-v1"',
                                    '"INJECTED_DISABLED_DISCOVERY"', 1) +
            '\n# decoy "--show-only=json-v1"\n',
            encoding="utf-8",
        )
        expect_failure(target, "differential runner discovery removal with comment decoy")
        runner_path.write_text(original_runner, encoding="utf-8")

        native_gate_path = target / "scripts/check_xr_program_aot_native.py"
        original_native_gate = native_gate_path.read_text(encoding="utf-8")
        native_gate_path.write_text(
            original_native_gate.replace("toolchain.find_symbol_dumper()", "None", 1) +
            "\n# decoy toolchain.find_symbol_dumper()\n",
            encoding="utf-8",
        )
        expect_failure(target, "native-AOT verified symbol-dumper removal with comment decoy")
        native_gate_path.write_text(original_native_gate, encoding="utf-8")

        native_gate_path.write_text(
            original_native_gate.replace("if not symbol_text.strip():", "if False:", 1) +
            "\n# decoy symbol_text.strip()\n",
            encoding="utf-8",
        )
        expect_failure(target, "native-AOT empty-inventory guard removal with comment decoy")
        native_gate_path.write_text(original_native_gate, encoding="utf-8")

        cmake_path = target / "tests/unit/CMakeLists.txt"
        original_cmake = cmake_path.read_text(encoding="utf-8")
        cmake_path.write_text(
            original_cmake.replace(
                "add_test(NAME test_xr_program_imported_callable_native",
                "add_test(NAME INJECTED_MISSING_IMPORTED_CALLABLE_NATIVE",
                1,
            ),
            encoding="utf-8",
        )
        expect_blocker_or_failure(target, len(baseline_blockers),
                                  "source native-AOT CTest removal")
        cmake_path.write_text(original_cmake, encoding="utf-8")

        top_level_path = target / "CMakeLists.txt"
        original_top_level = top_level_path.read_text(encoding="utf-8")
        top_level_path.write_text(
            original_top_level.replace(
                "NAME xr_program_wave4_exit_self_test",
                "NAME INJECTED_MISSING_WAVE4_EXIT_SELF_TEST",
                1,
            ),
            encoding="utf-8",
        )
        expect_blocker_or_failure(target, len(baseline_blockers),
                                  "top-level Wave 4 self-test CTest removal")
        top_level_path.write_text(original_top_level, encoding="utf-8")

        exit_data["status"] = ("READY_W7_WAVE4" if baseline_open else
                               "OPEN_W7_WAVE4")
        exit_path.write_text(json.dumps(exit_data, ensure_ascii=False, indent=2) + "\n",
                             encoding="utf-8")
        expect_failure(target, "forged exit state")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--require-ready", action="store_true")
    args = parser.parse_args()
    require(not (args.self_test and args.require_ready),
            "--self-test and --require-ready are mutually exclusive")
    try:
        root = args.root.resolve()
        if args.self_test:
            self_test(root)
            print("XrProgram Wave 4 exit self-test: PASS")
            return 0
        blockers = validate(root)
        if args.require_ready and blockers:
            raise ContractError("Wave 4 exit is blocked: " + " | ".join(blockers))
        if blockers:
            print(f"XrProgram Wave 4 exit inventory: OPEN ({len(blockers)} blockers)")
            for blocker in blockers:
                print(f"- {blocker}")
        else:
            print("XrProgram Wave 4 exit: READY")
        return 0
    except (ContractError, OSError, UnicodeError, json.JSONDecodeError) as exc:
        print(f"XrProgram Wave 4 exit: FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Fail-closed H2.2 cross-executor differential manifest gate.

The checked-in manifest is an explicit ratchet.  While an executor is marked
``expected-unsupported`` this gate verifies the exact CoreSpec state and the
real rejection witness.  Changing the CoreSpec coverage to COMPLETE therefore
breaks the gate until the manifest is deliberately switched to ``execute``.
In execute mode the gate runs the backend-owned probe and compares its complete
normalized value/outcome/identity/drop record with the Reference oracle.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "tests/unit/program/xr_program_h2_backend_differential.json"
REGISTRY = ROOT / "xisa/core/registry.json"

EXECUTOR_IDS = (
    "vm-baseline",
    "vm-fixed",
    "aot-backendir",
    "aot-generated-c-native",
)
EXPECTED_OPERATIONS = (
    (142, "core.class.construct"),
    (143, "core.class.share"),
    (144, "core.class.field_load"),
    (145, "core.class.field_place"),
    (146, "core.place.exchange"),
)
EXPECTED_OBSERVABLES = ("value", "outcome", "identity", "drop-events")
EXPECTED_ORACLE = {
    "scenario": "class-alias-mutation-lifecycle",
    "outcome": {"kind": "return"},
    "value": {"kind": "i64", "data": 42},
    "identities": {"constructed": "class-0", "shared": "class-0"},
    "events": [
        {"kind": "class-construct", "identity": "class-0"},
        {"kind": "class-share", "identity": "class-0", "related": "class-0"},
        {"kind": "class-field-place", "identity": "class-0", "field": 0},
        {"kind": "place-exchange", "type": "i64",
         "old": {"value": 7, "identity": "none"},
         "replacement": {"value": 42, "identity": "none"}},
        {"kind": "class-field-load", "identity": "class-0", "field": 0},
        {"kind": "owner-drop", "identity": "class-0"},
        {"kind": "owner-drop", "identity": "class-0"},
        {"kind": "class-finalize", "identity": "class-0"},
        {"kind": "class-reclaim", "identity": "class-0"},
    ],
}
REFERENCE_TOKENS = (
    "XR_REFERENCE_OUTCOME_RETURN",
    "XR_REFERENCE_VALUE_I64",
    "result.value.as.i64 == 42",
    "XR_REFERENCE_EVENT_CLASS_CONSTRUCT",
    "XR_REFERENCE_EVENT_CLASS_SHARE",
    "XR_REFERENCE_EVENT_CLASS_FIELD_PLACE",
    "XR_REFERENCE_EVENT_PLACE_EXCHANGE",
    "XR_REFERENCE_EVENT_CLASS_FIELD_LOAD",
    "XR_REFERENCE_EVENT_OWNER_DROP",
    "XR_REFERENCE_EVENT_OWNER_DROP",
    "XR_REFERENCE_EVENT_CLASS_FINALIZE",
    "XR_REFERENCE_EVENT_CLASS_RECLAIM",
    "log.type_ids[index] == (index == 3u ? XR_CORE_TYPE_I64 : class_type_id)",
    "log.identities[3] == UINT64_MAX",
    "log.related_identities[3] == UINT64_MAX",
    "log.related_identities[1] == log.identities[0]",
)
REFERENCE_PROGRAM_TOKENS = (
    ".value.i64 = 7",
    ".value.i64 = 42",
    "XrCoreIrKey exchange_operands[] = {place, forty_two};",
    "XR_CORE_OP_CORE_PLACE_EXCHANGE",
    ".result = old",
    ".result_type_id = XR_CORE_TYPE_I64",
    ".operands = exchange_operands",
)
VM_PENDING_TOKENS = (
    "XR_VM_DECODE_BASELINE_VIEW",
    "XR_VM_DECODE_FIXED_ROWS",
    "XR_VM_CODE_UNSUPPORTED_OPERATION",
    "diagnostic.status == XR_VM_CODE_UNSUPPORTED_OPERATION",
    "diagnostic.operation_id == XR_CORE_OP_CORE_CLASS_CONSTRUCT",
)
AOT_PENDING_TOKENS = (
    "xr_backend_ir_build",
    "XR_BACKEND_UNSUPPORTED_OPERATION",
    "diagnostic.status, XR_BACKEND_UNSUPPORTED_OPERATION",
    "diagnostic.operation_id, expected_operation",
    "spec->aot_status, XR_CORE_COVERAGE_NOT_YET_ACTIVE",
)
ACTIVE_REQUIRED_TOKENS = {
    "vm-baseline": ("XR_VM_DECODE_BASELINE_VIEW", "XR_VM_OUTCOME_RETURN",
                    "XR_VM_VALUE_I64", "h2-class-differential"),
    "vm-fixed": ("XR_VM_DECODE_FIXED_ROWS", "XR_VM_OUTCOME_RETURN",
                 "XR_VM_VALUE_I64", "h2-class-differential"),
    "aot-backendir": ("xr_backend_ir_build", "XR_BACKEND_OK", "xr_backend_ir_emit_c",
                      "h2-class-differential"),
    "aot-generated-c-native": ("xr_backend_ir_emit_c", "strict-native-host-run",
                               "h2-class-differential"),
}
EXPECTED_ACTIVE_COMMANDS = {
    "vm-baseline": {"binary": "vm", "arguments": ["--h2-class-differential", "baseline"],
                    "route": "vm-baseline-view"},
    "vm-fixed": {"binary": "vm", "arguments": ["--h2-class-differential", "fixed"],
                 "route": "vm-fixed-rows"},
    "aot-backendir": {"binary": "aot", "arguments": ["--h2-class-differential", "backend-ir"],
                      "route": "aot-backendir"},
    "aot-generated-c-native": {
        "binary": "aot",
        "arguments": ["--h2-class-differential", "generated-c-native"],
        "route": "aot-generated-c-strict-native-host-run",
    },
}


class GateError(RuntimeError):
    pass


def strict_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for name, value in pairs:
        if name in result:
            raise GateError(f"duplicate JSON key: {name}")
        result[name] = value
    return result


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=strict_object)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise GateError(f"cannot load {path}: {error}") from error


def exact_keys(value: Any, expected: set[str], where: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise GateError(f"{where} must be an object")
    actual = set(value)
    if actual != expected:
        missing = sorted(expected - actual)
        unknown = sorted(actual - expected)
        raise GateError(f"{where} keys differ (missing={missing}, unknown={unknown})")
    return value


def string_list(value: Any, where: str, *, nonempty: bool = True,
                unique: bool = True) -> tuple[str, ...]:
    if not isinstance(value, list) or (nonempty and not value):
        raise GateError(f"{where} must be a{' non-empty' if nonempty else ''} string list")
    if any(not isinstance(item, str) or not item for item in value):
        raise GateError(f"{where} contains a non-string or empty item")
    result = tuple(value)
    if unique and len(result) != len(set(result)):
        raise GateError(f"{where} contains duplicates")
    return result


def _matching_brace(text: str, opening: int) -> int:
    depth = 0
    state = "code"
    index = opening
    while index < len(text):
        char = text[index]
        next_char = text[index + 1] if index + 1 < len(text) else ""
        if state == "code":
            if char == '"':
                state = "string"
            elif char == "'":
                state = "character"
            elif char == "/" and next_char == "*":
                state = "block-comment"
                index += 1
            elif char == "/" and next_char == "/":
                state = "line-comment"
                index += 1
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return index
        elif state in ("string", "character"):
            if char == "\\":
                index += 1
            elif (state == "string" and char == '"') or (state == "character" and char == "'"):
                state = "code"
        elif state == "block-comment" and char == "*" and next_char == "/":
            state = "code"
            index += 1
        elif state == "line-comment" and char in "\r\n":
            state = "code"
        index += 1
    raise GateError("unterminated C function body")


def function_body(text: str, name: str, where: str) -> str:
    declaration = re.search(
        rf"\b(?:static\s+)?[A-Za-z_]\w*(?:\s*\*+)?\s+{re.escape(name)}"
        rf"\s*\([^;]*?\)\s*\{{",
        text,
        re.DOTALL,
    )
    if declaration is None:
        raise GateError(f"{where} lacks function {name}")
    opening = text.find("{", declaration.start())
    return text[opening + 1:_matching_brace(text, opening)]


def require_ordered_tokens(body: str, tokens: tuple[str, ...], where: str) -> None:
    offset = 0
    for token in tokens:
        found = body.find(token, offset)
        if found < 0:
            raise GateError(f"{where} lacks ordered evidence token: {token}")
        offset = found + len(token)


def load_source(root: Path, relative: str) -> str:
    path = root / relative
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise GateError(f"cannot load evidence source {relative}: {error}") from error


def validate_evidence(root: Path, evidence: Any, where: str,
                      *, required_tokens: tuple[str, ...] = ()) -> None:
    evidence = exact_keys(evidence, {"source", "function", "ordered_tokens"}, where)
    source = evidence["source"]
    function = evidence["function"]
    if not isinstance(source, str) or not isinstance(function, str) or not source or not function:
        raise GateError(f"{where} source/function must be non-empty strings")
    tokens = string_list(evidence["ordered_tokens"], f"{where}.ordered_tokens", unique=False)
    for token in required_tokens:
        if token not in tokens:
            raise GateError(f"{where} may not weaken required token: {token}")
    text = load_source(root, source)
    body = function_body(text, function, where)
    require_ordered_tokens(body, tokens, where)
    if len(re.findall(rf"\b{re.escape(function)}\s*\(", text)) < 2:
        raise GateError(f"{where} function is declared but never invoked")


def operation_coverage(registry: Any) -> dict[int, dict[str, str]]:
    if not isinstance(registry, dict) or not isinstance(registry.get("operations"), list):
        raise GateError("CoreSpec registry has no operations list")
    result: dict[int, dict[str, str]] = {}
    for operation in registry["operations"]:
        if not isinstance(operation, dict) or type(operation.get("stable_id")) is not int:
            continue
        coverage = operation.get("coverage")
        if not isinstance(coverage, dict):
            raise GateError(f"operation {operation['stable_id']} has no coverage")
        result[operation["stable_id"]] = {
            "spelling": operation.get("spelling"),
            **{name: value.get("status") if isinstance(value, dict) else None
               for name, value in coverage.items()},
        }
    return result


def validate_manifest(document: Any, registry: Any, root: Path = ROOT) -> dict[str, Any]:
    document = exact_keys(document, {
        "schema", "phase", "required_observables", "operations", "oracle",
        "reference_evidence", "reference_program_evidence", "qualification", "executors",
    }, "manifest")
    if document["schema"] != 1 or document["phase"] != "H2.2":
        raise GateError("manifest schema/phase is not the frozen H2.2 contract")
    if tuple(document["required_observables"]) != EXPECTED_OBSERVABLES:
        raise GateError("required observable set/order drifted")
    if document["oracle"] != EXPECTED_ORACLE:
        raise GateError("Reference value/outcome/identity/drop oracle drifted")

    operations = document["operations"]
    if not isinstance(operations, list):
        raise GateError("operations must be a list")
    actual_operations = []
    for index, operation in enumerate(operations):
        operation = exact_keys(operation, {"stable_id", "spelling"}, f"operations[{index}]")
        actual_operations.append((operation["stable_id"], operation["spelling"]))
    if tuple(actual_operations) != EXPECTED_OPERATIONS:
        raise GateError("H2.2 operation inventory drifted")

    coverage = operation_coverage(registry)
    for stable_id, spelling in EXPECTED_OPERATIONS:
        if stable_id not in coverage or coverage[stable_id]["spelling"] != spelling:
            raise GateError(f"CoreSpec operation identity mismatch: {stable_id}/{spelling}")

    validate_evidence(root, document["reference_evidence"], "reference_evidence",
                      required_tokens=REFERENCE_TOKENS)
    validate_evidence(root, document["reference_program_evidence"],
                      "reference_program_evidence",
                      required_tokens=REFERENCE_PROGRAM_TOKENS)

    qualification = exact_keys(document["qualification"], {"ctests", "targets"},
                               "qualification")
    qualification["ctests"] = list(string_list(qualification["ctests"],
                                                 "qualification.ctests"))
    qualification["targets"] = list(string_list(qualification["targets"],
                                                  "qualification.targets"))
    required_ctests = {
        "test_xr_program_verify", "test_xr_program_source_build", "test_xr_program_vm",
        "test_xr_program_vm_runtime", "test_xr_program_aot",
    }
    required_targets = {
        "test_xr_program_verify", "test_xr_program_source_build", "test_xr_program_vm",
        "test_xr_program_vm_runtime", "test_xr_program_aot",
    }
    if not required_ctests <= set(qualification["ctests"]):
        raise GateError("qualification omits a required existing CTest")
    if not required_targets <= set(qualification["targets"]):
        raise GateError("qualification omits a required existing build target")

    executors = document["executors"]
    if not isinstance(executors, list) or len(executors) != len(EXECUTOR_IDS):
        raise GateError("executor inventory is incomplete")
    by_id: dict[str, dict[str, Any]] = {}
    for index, raw in enumerate(executors):
        executor = exact_keys(raw, {
            "id", "coverage_key", "state", "pending_witness", "active_command",
            "active_evidence",
        }, f"executors[{index}]")
        identifier = executor["id"]
        if identifier not in EXECUTOR_IDS or identifier in by_id:
            raise GateError(f"unknown or duplicate executor: {identifier}")
        coverage_key = executor["coverage_key"]
        expected_key = "vm" if identifier.startswith("vm-") else "aot"
        if coverage_key != expected_key:
            raise GateError(f"{identifier} is bound to the wrong CoreSpec consumer")
        state = executor["state"]
        if state not in ("expected-unsupported", "execute"):
            raise GateError(f"{identifier} has invalid state: {state}")
        expected_status = "NOT_YET_ACTIVE" if state == "expected-unsupported" else "COMPLETE"
        for stable_id, _ in EXPECTED_OPERATIONS:
            if coverage[stable_id].get(coverage_key) != expected_status:
                raise GateError(
                    f"{identifier} state disagrees with CoreSpec operation {stable_id}: "
                    f"expected {expected_status}, got {coverage[stable_id].get(coverage_key)}"
                )

        command = exact_keys(executor["active_command"],
                             {"binary", "arguments", "route"},
                             f"{identifier}.active_command")
        if command["binary"] not in ("vm", "aot"):
            raise GateError(f"{identifier} active command has unknown binary")
        string_list(command["arguments"], f"{identifier}.active_command.arguments",
                    nonempty=False)
        if not isinstance(command["route"], str) or not command["route"]:
            raise GateError(f"{identifier} active command lacks route identity")
        if command != EXPECTED_ACTIVE_COMMANDS[identifier]:
            raise GateError(f"{identifier} active command/provenance drifted")
        active = exact_keys(executor["active_evidence"],
                            {"source", "function", "ordered_tokens"},
                            f"{identifier}.active_evidence")
        string_list(active["ordered_tokens"], f"{identifier}.active_evidence.ordered_tokens",
                    unique=False)

        if state == "expected-unsupported":
            if executor["pending_witness"] is None:
                raise GateError(f"{identifier} pending state lacks exact rejection witness")
            validate_evidence(
                root,
                executor["pending_witness"],
                f"{identifier}.pending_witness",
                required_tokens=(VM_PENDING_TOKENS if coverage_key == "vm"
                                 else AOT_PENDING_TOKENS),
            )
        else:
            if executor["pending_witness"] is not None:
                raise GateError(f"{identifier} execute state retains a refusal witness")
            validate_evidence(root, active, f"{identifier}.active_evidence",
                              required_tokens=ACTIVE_REQUIRED_TOKENS[identifier])
        by_id[identifier] = executor

    if tuple(by_id) != EXECUTOR_IDS:
        raise GateError("executor order/set drifted")
    for left, right in (("vm-baseline", "vm-fixed"),
                        ("aot-backendir", "aot-generated-c-native")):
        if by_id[left]["state"] != by_id[right]["state"]:
            raise GateError(f"paired executor states are asymmetric: {left}/{right}")
    return document


def qualification_ctest_names(manifest: Path = MANIFEST) -> tuple[str, ...]:
    document = load_json(manifest)
    qualification = exact_keys(document.get("qualification"), {"ctests", "targets"},
                               "qualification")
    return string_list(qualification["ctests"], "qualification.ctests")


def qualification_build_targets(manifest: Path = MANIFEST) -> tuple[str, ...]:
    document = load_json(manifest)
    qualification = exact_keys(document.get("qualification"), {"ctests", "targets"},
                               "qualification")
    return string_list(qualification["targets"], "qualification.targets")


def parse_probe_record(stdout: bytes, identifier: str, route: str) -> dict[str, Any]:
    try:
        text = stdout.decode("utf-8", errors="strict")
        if not text.endswith("\n") or text.count("\n") != 1:
            raise GateError(f"{identifier} probe must emit exactly one UTF-8 JSON line")
        record = json.loads(text, object_pairs_hook=strict_object)
    except (UnicodeError, json.JSONDecodeError) as error:
        raise GateError(f"{identifier} emitted invalid probe JSON: {error}") from error
    record = exact_keys(record, {"schema", "executor", "route", "oracle"},
                        f"{identifier} probe")
    if record != {"schema": 1, "executor": identifier, "route": route,
                  "oracle": EXPECTED_ORACLE}:
        raise GateError(f"{identifier} probe disagrees with the Reference oracle")
    return record


def run_active(document: dict[str, Any], binaries: dict[str, Path]) -> int:
    executed = 0
    for executor in document["executors"]:
        if executor["state"] != "execute":
            continue
        identifier = executor["id"]
        command_spec = executor["active_command"]
        binary = binaries.get(command_spec["binary"])
        if binary is None or not binary.is_file():
            raise GateError(f"{identifier} active probe binary is missing")
        command = [str(binary.resolve()), *command_spec["arguments"]]
        try:
            result = subprocess.run(command, check=False, capture_output=True, timeout=120)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise GateError(f"{identifier} active probe could not run: {error}") from error
        if result.returncode != 0 or result.stderr:
            detail = (result.stdout + result.stderr).decode("utf-8", errors="replace")
            raise GateError(f"{identifier} active probe failed ({result.returncode}): {detail}")
        parse_probe_record(result.stdout, identifier, command_spec["route"])
        executed += 1
    return executed


def check(manifest: Path = MANIFEST, registry_path: Path = REGISTRY,
          root: Path = ROOT, binaries: dict[str, Path] | None = None) -> tuple[int, int]:
    document = validate_manifest(load_json(manifest), load_json(registry_path), root)
    active = sum(executor["state"] == "execute" for executor in document["executors"])
    executed = run_active(document, binaries or {}) if active else 0
    if executed != active:
        raise GateError("active executor probe set was not executed exactly")
    return active, len(document["executors"]) - active


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--manifest", type=Path, default=MANIFEST)
    parser.add_argument("--registry", type=Path, default=REGISTRY)
    parser.add_argument("--vm", type=Path)
    parser.add_argument("--aot", type=Path)
    args = parser.parse_args(argv)
    try:
        active, pending = check(args.manifest, args.registry, ROOT,
                                {name: path for name, path in
                                 (("vm", args.vm), ("aot", args.aot)) if path is not None})
    except GateError as error:
        print(f"H2 backend differential gate: FAIL: {error}", file=sys.stderr)
        return 1
    print(f"H2 backend differential gate: PASS (active={active}, "
          f"expected-unsupported={pending}; H2.2 remains OPEN while pending is nonzero)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

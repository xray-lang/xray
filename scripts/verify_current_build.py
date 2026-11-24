#!/usr/bin/env python3
"""Verify CLI identity, current LIR/register kernels and strict smoke after all."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
REQUIRED = (
    "test_api_inventory_predicates",
    "test_lir_core", "test_lir_carriers", "test_lir_source_input", "test_lir_lower", "test_lir_c",
    "test_lir_c_faults", "test_lir_c_native", "test_cgen_verify_output",
    "register_vm", "register_vm_native_print", "register_vm_alloc",
    "lir_bytecode", "lir_bytecode_alloc", "lir_bytecode_source",
    "lir_vm_source", "lir_vm_output", "skeleton_diff", "smoke", "test_lir_cli_effects",
    # Preserve written Bool/String evaluation order, retained messages and real
    # callsite locations through valid LIR and Source on both execution backends.
    "lir_assertion", "test_lir_cli_assertions", "test_lir_cli_invoke_try_assert",
    "test_lir_cli_invoke_try_propagate", "test_lir_cli_invoke_try_optional",
    "test_lir_cli_invoke_managed_string", "test_lir_cli_invoke_catch",
    "test_lir_cli_invoke_catch_local",
    "test_lir_cli_invoke_borrowed_slice", "test_lir_cli_invoke_strict_utf8",
    "test_lir_cli_class_optional_i64",
    "test_lir_cli_defer_sync", "test_lir_cli_defer_lowering", "test_lir_cli_defer_source",
    "test_lir_cli_defer_propagation",
    "test_lir_cli_defer_panic", "test_lir_cli_defer_caught",
    "test_lir_cli_invoke_init_root",
    "test_lir_cli_string_byte_length",
    "test_lir_cli_host_path", "test_lir_cli_host_file_write", "test_lir_cli_target_platform",
    "test_lir_cli_host_environment", "test_lir_host_stat_control",
    "test_lir_host_remove_control",
    "test_lir_cli_string_slice",
    "test_lir_cli_string_index_of", "test_lir_cli_f64_try_parse", "test_lir_cli_string_test", "test_lir_cli_host_method_import", "test_cli_method_import_visibility", "test_lir_cli_class_enum_field", "test_lir_cli_enum_equality",
    # Cross-module ownership and capability transport need real native execution.
    "test_lir_state", "native_module_state_owner", "test_lir_cli_module_state",
    # Exercise Array ownership, failure cleanup and complete Source/native parity.
    "test_lir_array",
    "test_lir_array_faults", "test_lir_array_native", "test_lir_cli_array_native",
    # Fixed receiving layouts, independent images and constructor facts replace
    # retired owner-bank tests; these retain metadata and failure cleanup checks.
    "lir_value_layout", "lir_value_image", "lir_binding_homes", "lir_callable_facts", "lir_callable_bytecode",
    # Loop consumers rely on fresh ownership facts and failure-atomic proofs.
    "test_lir_ownership", "test_lir_ownership_faults",
    # Physical metadata, allocator and foreign-thread owner retirement.
    "native_managed_kernel", "native_managed_root", "native_backing", "native_string_backing_ops",
    "native_place", "native_value_plan", "native_managed_status", "native_capture_backing", "rbc_code_owner",
    # A real allocator can clear State before or after a Slice keeper is published.
    "native_array_slice",
    # Shared scalar decoding and text must retain independent native/RBC witnesses.
    "native_f64_i64_conversion", "native_f64_text",
    "test_lexer", "test_parser_recoverable", "test_parser_arrow_syntax",
    "test_fmt_roundtrip", "test_value_arrow_syntax", "spec_snippets_cn", "spec_snippets_en",
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    args.build = args.build.resolve()
    args.evidence = args.evidence.resolve()
    args.evidence.mkdir(parents=True, exist_ok=True)
    facts = []

    def run(name: str, command: list) -> dict:
        argv = [str(item) for item in command]
        output = args.evidence / (name + ".stdout")
        errors = args.evidence / (name + ".stderr")
        started = time.perf_counter()
        with output.open("wb") as out, errors.open("wb") as err:
            child = subprocess.run(argv, stdout=out, stderr=err, check=False)
        fact = {
            "name": name, "command": argv, "exit": child.returncode,
            "seconds": time.perf_counter() - started,
            "stdoutSha256": hashlib.sha256(output.read_bytes()).hexdigest(),
            "stderrSha256": hashlib.sha256(errors.read_bytes()).hexdigest(),
        }
        facts.append(fact)
        (args.evidence / "commands.json").write_text(
            json.dumps(facts, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(fact), flush=True)
        require(child.returncode == 0, name + " failed; original logs retained")
        return fact

    head = subprocess.check_output(
        ["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip()
    executable = args.build / ("xray.exe" if os.name == "nt" else "xray")
    run("identity", [executable, "--version", "--json"])
    identity = json.loads((args.evidence / "identity.stdout").read_text(encoding="utf-8"))
    require(identity["commit"] == head and identity["dirty"] is False,
            "CLI is not the exact clean source")

    listing = run("ctest-list", ["ctest", "--test-dir", args.build, "--show-only=json-v1"])
    require(listing["seconds"] <= 5, "Complete CTest listing exceeded 5 seconds")
    registry = json.loads((args.evidence / "ctest-list.stdout").read_text(encoding="utf-8"))
    names = [test["name"] for test in registry["tests"]]
    for name in REQUIRED:
        require(names.count(name) == 1, "Missing or duplicate current gate: " + name)

    pattern = "^(" + "|".join(map(re.escape, REQUIRED)) + ")$"
    junit = args.evidence / "current.xml"
    run("current-tests", ["ctest", "--test-dir", args.build, "--output-on-failure",
                          "--no-tests=error", "-j", "2", "-R", pattern,
                          "--output-junit", junit])
    result = ET.parse(junit).getroot()
    cases = list(result.iter("testcase"))
    require(sorted(case.attrib["name"] for case in cases) == sorted(REQUIRED),
            "Requested and executed tests differ")
    require(not list(result.iter("skipped")) and not list(result.iter("failure"))
            and not list(result.iter("error")), "Current gates contain non-passing cases")
    print(f"Current verification passed: exact CLI, full registration, {len(REQUIRED)} tests.", flush=True)


if __name__ == "__main__":
    main()

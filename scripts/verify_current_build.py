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
    "test_lir_core", "test_lir_carriers", "test_lir_lower", "test_lir_c",
    "test_lir_c_faults", "test_lir_c_native", "test_cgen_verify_output",
    "register_vm", "register_vm_native_print", "register_vm_alloc",
    "lir_bytecode", "lir_bytecode_alloc", "lir_bytecode_source",
    "lir_vm_source", "lir_vm_output", "skeleton_diff", "smoke", "test_lir_cli_effects",
    # Cross-module ownership and capability transport need real native execution.
    "test_lir_state", "native_module_state_owner", "test_lir_cli_module_state",
    # Exercise Array ownership, failure cleanup and complete Source/native parity.
    "test_native_array", "test_native_array_allocations", "test_lir_array",
    "test_lir_array_faults", "test_lir_array_native", "test_lir_cli_array_native",
    # Canonical Record/String? ownership, external schemas and failure cleanup.
    "test_native_boxed", "test_native_boxed_allocations",
    # Value delimiters must reject type arrows and preserve source ownership.
    "test_lexer", "test_parser_recoverable", "test_parser_arrow_syntax",
    "test_fmt_roundtrip", "test_value_arrow_syntax",
    "spec_snippets_cn", "spec_snippets_en",
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

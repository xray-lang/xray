#!/usr/bin/env python3
"""Preserve the complete legacy consumer census and exact source inputs."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / "tests/unit/xir/product_consumers"
MANIFEST = ROOT / "tests/unit/program/xr_program_source_cases.json"
SOURCE = ROOT / "tests/unit/program/test_xr_program_source_build.c"
BASE = "01e6d38401b0f5c8684b64c35a99ac651024c0c1"
MANIFEST_SHA = "c298392053c1557d8402a8da25078749d99ad095c9671b514526b1b89d643205"
SELECTED = {
    "source_owner_static_method_declarations": "static_methods",
    "source_owner_array_index_reads_and_replaces_elements": "array_places",
    "source_owner_generic_specializations_are_exact_program_functions": "generics",
    "source_owner_function_parameter_callable_has_one_program_and_private_executors": "callables",
    "source_owner_array_append_preserves_class_identity": "class_identity",
}
COMMON_OBLIGATIONS = {
    "original_source_and_fixed_result": "The original source prefix and 42/48 oracle are retained byte for byte.",
    "detached_lifetimes": "Parse session, SourceProduct and independent Checked reader die before execution.",
    "native_emission": "Two emissions are byte-identical; actual C is compiled and bound by its SHA256.",
    "backend_equivalence": "VM, native and both mixed partitions each verify independent fixed goldens.",
    "instance_ownership": "Two instances retain one immutable Program after caller ownership is dropped.",
    "host_authority": "Calling private answer is rejected without allocations or initialization.",
    "compiler_failures": "Whole producer/read/emit/seal/reverify operation needs every measured physical ordinal.",
    "compiler_budgets": "Each allocated/live/work threshold is measured and exact/minus1 checked.",
    "runtime_failures": "Actual instance/init/entry allocation ordinals, sticky init failure and physical release.",
    "cancellation": "Every real active prefix cancels, preserves initialized state and releases physically.",
    "full_safety": "Complete applicable sanitizer gates and integration-head revalidation remain required.",
}
STRING = r'"(?:\\.|[^"\\])*"'


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def c_literal(literal: str) -> bytes:
    result = bytearray()
    text = literal[1:-1]
    offset = 0
    simple = {"a": 7, "b": 8, "f": 12, "n": 10, "r": 13, "t": 9, "v": 11,
              "?": 63, '"': 34, "'": 39, "\\": 92}
    while offset < len(text):
        character = text[offset]
        offset += 1
        if character != "\\":
            result.extend(character.encode("utf-8"))
            continue
        escape = text[offset]
        offset += 1
        if escape in simple:
            result.append(simple[escape])
        elif escape in "01234567" or escape == "x":
            tail = re.match(r"[0-7]{0,2}" if escape != "x" else r"[0-9a-fA-F]+", text[offset:])
            if not tail:
                raise ValueError("empty C hex escape")
            digits = (escape if escape != "x" else "") + tail.group()
            value = int(digits, 8 if escape != "x" else 16)
            if value > 255:
                raise ValueError("nonportable byte escape in source input")
            result.append(value)
            offset += len(tail.group())
        elif escape in ("u", "U"):
            count = 4 if escape == "u" else 8
            result.extend(chr(int(text[offset:offset + count], 16)).encode("utf-8"))
            offset += count
        elif escape != "\n":
            raise ValueError(f"unsupported C escape: {escape}")
    return bytes(result)


def static_strings(body: str) -> dict[str, bytes]:
    result = {}
    pattern = rf'(?:static\s+)?const\s+char\s+(\w+)\[\]\s*=\s*((?:{STRING}\s*)+);'
    for match in re.finditer(pattern, body, re.S):
        name, literals = match.groups()
        if name in result:
            raise ValueError(f"ambiguous source literal: {name}")
        result[name] = b"".join(c_literal(s) for s in re.findall(STRING, literals))
    return result


def function_end(text: str, start: int) -> int:
    """Count C braces while ignoring comments and both literal forms."""
    tokens = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)
    depth = 0
    for token in tokens.finditer(text, start):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return token.end()
    raise ValueError("unterminated C function")


def row(path: Path, name: str, body: str, line: int) -> dict:
    return {"name": name, "legacy_path": path.relative_to(ROOT).as_posix(),
            "legacy_line": line, "body_lf_sha256": digest(body.encode()),
            "assertion_sites": len(re.findall(r'\b(?:ASSERT_\w+|REQUIRE|CHECK)\s*\(', body)),
            "loop_sites": len(re.findall(r'\b(?:for|while)\s*\(', body)),
            "status": "OPEN", "replacement_gates": [],
            "note": "Function census preserves its whole body; sites do not count executed loop iterations."}


def inventory() -> tuple[dict, dict[Path, bytes]]:
    raw = MANIFEST.read_bytes()
    blob = subprocess.check_output(["git", "show", f"{BASE}:tests/unit/program/xr_program_source_cases.json"], cwd=ROOT)
    if digest(blob) != MANIFEST_SHA or raw.replace(b"\r\n", b"\n") != blob:
        raise ValueError("legacy source manifest changed from the assigned baseline")
    manifest = json.loads(raw)
    text = SOURCE.read_text(encoding="utf-8")
    bodies = {}
    for match in re.finditer(r'^TEST\((\w+)\)\s*\{', text, re.M):
        end = function_end(text, text.index("{", match.start()))
        bodies[match[1]] = (text[match.start():end], text.count("\n", 0, match.start()) + 1)
    if set(bodies) != {case["name"] for case in manifest["cases"]}:
        raise ValueError("source responsibility census differs from manifest")
    sources = []
    files = {}
    for case in manifest["cases"]:
        body, line = bodies[case["name"]]
        current = row(SOURCE, case["name"], body, line)
        current["original_fixture"] = case["fixture"]
        literals = static_strings(body)
        current["static_input_literals"] = {name: {"bytes": len(value),
            "sha256": digest(value)} for name, value in literals.items()}
        if case["name"] in SELECTED:
            selected = SELECTED[case["name"]]
            fixture = DEST / "fixtures" / selected / "root.xr"
            data = literals["source"]
            adapter = b"export fn consumerAnswer() -> i64 { return answer() }\n"
            files[fixture] = data + adapter
            current["initial_projection"] = {"fixture": fixture.relative_to(ROOT).as_posix(),
                "sha256": digest(data + adapter), "original_prefix_sha256": digest(data),
                "entry_adapter": adapter.decode(), "entry": "consumerAnswer", "expected_i64": case["fixture"]["expected_exit"],
                "scope": "Exact original source prefix and fixed result through an explicitly exported adapter; additional legacy assertions stay OPEN until mapped."}
            current["projected_obligations"] = {name: {"status": "PENDING_FULL_QUALIFICATION", "verification": note}
                                                for name, note in COMMON_OBLIGATIONS.items()}
            prefix = f"test_source_product_{selected}"
            current["candidate_replacement_gates"] = [f"{prefix}_{mode}" for mode in ("vm", "native", "mixed_even", "mixed_odd")]
            current["candidate_replacement_gates"] += [f"{prefix}_{kind}_{mode}"
                for kind in ("axes", "compiler", "runtime", "cancel") for mode in range(4)]
            if selected == "generics":
                current["additional_legacy_obligations"] = {
                    "specialization_identity": {"status": "IMPLEMENTED_NOT_QUALIFIED",
                        "verification": "Closed Checked answer calls one exact i64 specialization twice and a distinct bool specialization once; operand/result types are exact."},
                    "independent_product_determinism": {"status": "OPEN"},
                    "open_generic_entry_rejection": {"status": "OPEN",
                        "note": "The removed selectable legacy source entry must be mapped to a current public authority/admission rejection."},
                    "legacy_four_function_count_and_param_mode_layout": {"status": "OPEN",
                        "note": "The new graph has an explicit host adapter and synthetic entry; retirement of old representation assertions needs a reviewed mapping."}}
            elif selected == "callables":
                current["additional_legacy_obligations"] = {
                    "independent_product_determinism": {"status": "OPEN"},
                    "entry_callable_effect_rejection": {"status": "OPEN",
                        "note": "The old source entry rejection is not a language ban on legal callable parameters; preserve its authority/effect responsibility in a current admission test."},
                    "legacy_function_id_emission_spelling": {"status": "OPEN",
                        "note": "Actual sealed bindings are verified; the obsolete generated representation assertion needs a reviewed retirement mapping."}}
        sources.append(current)
    families = {}
    paths = sorted((ROOT / "tests/unit/program").glob("*.c"))
    paths += sorted((ROOT / "tests/unit/aot").glob("test_xr_program_aot*.c"))
    paths += sorted((ROOT / "tests/unit/aot").glob("test_xr_program_aot*.inc.c"))
    for path in paths:
        if path == SOURCE:
            continue
        code = path.read_text(encoding="utf-8")
        functions = []
        for match in re.finditer(r'^(?:static\s+)?(?:void|int|size_t)\s+((?:test_\w+)|(?:run_allocation_case)|main)\s*\([^;]*?\)\s*\{', code, re.M):
            end = function_end(code, code.index("{", match.start()))
            functions.append(row(path, match[1], code[match.start():end], code.count("\n", 0, match.start()) + 1))
        families[path.relative_to(ROOT).as_posix()] = {"lf_sha256": digest(code.encode()),
            "functions": functions, "whole_file_responsibility": "OPEN"}
    includes = {}
    for path in sorted((ROOT / "tests").rglob("*")):
        if path.suffix not in (".c", ".h"):
            continue
        code = path.read_text(encoding="utf-8")
        if re.search(r'#include\s+"[^"\n]*xr_backend_emission_owner\.h"', code):
            includes[path.relative_to(ROOT).as_posix()] = digest(code.encode())
    result = {"schema": 1, "base_commit": BASE, "legacy_source_manifest_blob_sha256": digest(blob),
        "source_responsibilities": len(sources), "source_native_fixtures": sum(bool(x["original_fixture"]) for x in sources),
        "source_non_fixture_responsibilities": sum(x["original_fixture"] is None for x in sources),
        "source": sources, "other_consumer_files": families,
        "source_allocation_scenarios": {"count": 8, "status": "OPEN", "ordinal_counts": "Must be measured on actual rebuilt old or migrated binaries."},
        "shared_emission_owner_consumers": includes,
        "shared_emission_owner_policy": "Read only: shared with active IR/provider consumers outside this lane.",
        "completion": "OPEN; generated census and partial projections are not replacement qualification."}
    return result, files


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("write", "check"))
    args = parser.parse_args()
    result, files = inventory()
    files[DEST / "legacy_responsibilities.json"] = (json.dumps(result, ensure_ascii=False, indent=2) + "\n").encode()
    for path, data in files.items():
        if args.action == "write":
            path.parent.mkdir(parents=True, exist_ok=True)
            if not path.is_file() or path.read_bytes() != data:
                path.write_bytes(data)
        elif not path.is_file() or path.read_bytes() != data:
            raise ValueError(f"stale migration inventory or fixture: {path.relative_to(ROOT)}")
    print(json.dumps({"source": result["source_responsibilities"], "fixtures": result["source_native_fixtures"],
        "non_fixture": result["source_non_fixture_responsibilities"], "other_files": len(result["other_consumer_files"]),
        "other_function_rows": sum(len(x["functions"]) for x in result["other_consumer_files"].values()),
        "shared_owner_consumers": len(result["shared_emission_owner_consumers"]), "completion": "OPEN"}))


if __name__ == "__main__":
    main()

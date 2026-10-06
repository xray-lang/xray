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
    "source_owner_value_struct_string_copy_preserves_original": "struct_string",
    "source_owner_explicit_bool_conditions_execute": "bool_conditions",
    "source_owner_single_module_is_deterministic_and_detached": "single_module",
    "source_owner_text_program_is_exact_across_private_executors": "text_program",
    "source_owner_exact_integer_constants_and_conversions": "integer_conversions",
    "source_owner_array_runtime_length": "array_runtime_length",
}
PROBES = {
    "source_owner_tuple_array_elements": "tuple_array",
    "source_owner_repeated_tuple_elements": "tuple_repeated",
    "source_owner_string_slice_scalar_range": "string_slice",
    "source_owner_integer_bitwise_exact_width": "integer_width",
    "source_owner_two_module_graph_is_deterministic": "two_modules",
    "source_owner_byte_comparison": "byte_comparison",
    "source_owner_generic_value_struct_specializations_are_exact_nominal_aggregates": "generic_value_struct",
    "source_owner_generic_scalar_class_specializations_are_exact_class_references": "generic_scalar_class",
    "source_owner_generic_constraint_methods_have_exact_concrete_targets": "generic_constraint_methods",
}
REJECTIONS = {
    "source_owner_bare_nullable_condition_has_no_product": {
        "fixture": "bare_nullable", "status": "XR_XIR_BAD_TYPE", "message": "if requires bool"},
    "source_owner_reports_structured_analysis_failure": {
        "fixture": "missing_name", "status": "XR_XIR_BAD_VALUE", "message": "name is not an initialized value"},
}
COMMON_OBLIGATIONS = {
    "original_source_and_fixed_result": "The original source prefix and manifest result oracle are retained byte for byte.",
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


def allocation_scenarios() -> dict:
    path = ROOT / "tests/unit/program/test_xr_program_source_allocations.c"
    code = path.read_text(encoding="utf-8")
    match = re.search(r'static const char \*const sources\[\]\s*=\s*\{', code)
    if not match:
        raise ValueError("legacy allocation source array is missing")
    start = code.index("{", match.start())
    end = function_end(code, start)
    values = []
    value = bytearray()
    for token in re.finditer(STRING + r'|,', code[start + 1:end - 1]):
        if token.group() == ",":
            values.append(bytes(value))
            value.clear()
        else:
            value.extend(c_literal(token.group()))
    if value:
        values.append(bytes(value))
    if len(values) != 8 or any(not item for item in values):
        raise ValueError("legacy allocation scenario denominator changed")
    manifest = static_strings(code)["manifest"]
    return {"count": len(values), "status": "OPEN",
        "shared_native_manifest_bytes": len(manifest), "shared_native_manifest_sha256": digest(manifest),
        "original_obligations": ["all actual source-build allocation ordinals", "exact diagnostic status and nonempty failure message",
            "failure leaves program/artifact/export/test/retained-root outputs empty", "two exports and one test on success",
            "exact external symbol and valid export function id", "parse fixture destroyed before product destruction",
            "physical allocation zero after each injected failure and final teardown", "baseline allocation count repeats exactly"],
        "scenarios": [{"ordinal": index, "input_bytes": len(data), "input_sha256": digest(data),
            "source": data.decode("utf-8"), "status": "OPEN", "original_allocation_count": "NOT_MEASURED",
            "replacement_allocation_count": "NOT_MEASURED", "replacement_gates": []} for index, data in enumerate(values)]}


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
        if case["name"] in REJECTIONS:
            rejection = REJECTIONS[case["name"]]
            data = literals.get("source")
            if data is None:
                match = re.search(rf'source_build_fixture_init\(&fixture,\s*((?:{STRING}\s*)+),\s*NULL\)', body)
                if not match:
                    raise ValueError("original rejection source literal is missing")
                data = b"".join(c_literal(s) for s in re.findall(STRING, match[1]))
            fixture = DEST / "fixtures" / rejection["fixture"] / "main.xr"
            files[fixture] = data
            prefix = f"test_source_product_rejections_{rejection['fixture']}"
            current["initial_projection"] = {"fixture": fixture.relative_to(ROOT).as_posix(),
                "sha256": digest(data), "input_bytes": len(data), "entry_adapter": None,
                "stage": "XR_XIR_SOURCE_PRODUCT_CHECK", "status": rejection["status"],
                "message": rejection["message"], "module": 0, "line": 1,
                "column": 29 if rejection["fixture"] == "missing_name" else "NOT_REQUIRED_BY_ORIGINAL_TEST",
                "scope": "Exact negative source input; reject before any executable product or backend projection."}
            current["candidate_replacement_gates"] = [prefix, prefix + "_axes", prefix + "_compiler"]
            current["projected_obligations"] = {
                "semantic_rejection": {"status": "PENDING_FULL_QUALIFICATION",
                    "verification": "Exact Check status, semantic module/line/path and diagnostic text; structured missing-name position is column 29; product remains NULL."},
                "diagnostic_ownership": {"status": "PENDING_FULL_QUALIFICATION",
                    "verification": "Diagnostic path remains readable after parser/session and caller ledger ownership are released."},
                "compiler_failures": {"status": "PENDING_FULL_QUALIFICATION",
                    "verification": "Every measured actual allocation ordinal returns OOM, with no partial product and physical release."},
                "compiler_budgets": {"status": "PENDING_FULL_QUALIFICATION",
                    "verification": "Measured allocated/live/work exact threshold preserves the semantic rejection; minus1 reports BUDGET."},
                "full_safety": {"status": "PENDING_FULL_QUALIFICATION",
                    "verification": "Complete applicable sanitizer gates and integration-head qualification remain required."}}
            if rejection["fixture"] == "bare_nullable":
                current["additional_diagnostic_precision"] = {"status": "OPEN",
                    "note": "The original only requires the condition-type rejection. Current AST_IF retains line 1 but no column; accurate if-token column is a production gap outside this lane."}
        if case["name"] in SELECTED or case["name"] in PROBES:
            selected = (SELECTED | PROBES)[case["name"]]
            fixture = DEST / "fixtures" / selected / "root.xr"
            data = literals.get("source", literals.get("entry_source"))
            if data is None:
                raise ValueError("original positive source literal is missing")
            additional_files = {filename: literals[literal]
                for literal, filename in (("library_source", "library.xr"), ("facade_source", "facade.xr"))
                if literal in literals}
            for filename, content in additional_files.items():
                files[fixture.parent / filename] = content
            adapter = b"export fn consumerAnswer() -> i64 { return answer() }\n"
            files[fixture] = data + adapter
            current["initial_projection"] = {"fixture": fixture.relative_to(ROOT).as_posix(),
                "sha256": digest(data + adapter), "original_prefix_sha256": digest(data),
                "entry_adapter": adapter.decode(), "entry": "consumerAnswer", "expected_i64": case["fixture"]["expected_exit"] if case["fixture"] else 42,
                "scope": "Exact original source prefix and fixed result through an explicitly exported adapter; additional legacy assertions stay OPEN until mapped."}
            if additional_files:
                current["initial_projection"]["additional_files"] = {filename: {
                    "bytes": len(content), "sha256": digest(content)}
                    for filename, content in additional_files.items()}
            if selected == "text_program":
                golden = literals["expected_stdout"]
                if golden.hex() != case["fixture"]["expected_stdout_hex"]:
                    raise ValueError("original text output golden differs from the legacy manifest")
                files[fixture.parent / "expected.stdout"] = golden
                header = ("/*\n * xray - Lightweight typed scripting with native concurrency\n"
                    " * https://www.xray-lang.org\n *\n"
                    " * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>\n"
                    " * Licensed under the MIT License\n *\n"
                    " * expected_output.h - Original independent text output golden\n *\n"
                    " * KEY CONCEPT:\n *   Expected bytes come from the original source test and manifest.\n */\n"
                    "#ifndef SOURCE_PRODUCT_TEXT_EXPECTED_OUTPUT_H\n#define SOURCE_PRODUCT_TEXT_EXPECTED_OUTPUT_H\n"
                    "static const char consumer_text_golden[] = {" + ",".join(str(b) for b in golden) + ",0};\n"
                    "#endif // SOURCE_PRODUCT_TEXT_EXPECTED_OUTPUT_H\n")
                files[fixture.parent / "expected_output.h"] = header.encode()
                current["initial_projection"]["output_golden"] = {"bytes": len(golden),
                    "sha256": digest(golden), "hex": golden.hex(), "groups_per_call": 3}
            if case["name"] in PROBES:
                current["initial_projection"]["scope"] = "Exact positive source prefix and fixed oracle retained for admission probing; no replacement gates are registered yet."
                current["initial_projection"]["probe_target"] = "test_source_product_probe"
                if selected == "two_modules":
                    current["initial_projection"]["detached_probe_target"] = "test_source_product_detachment"
                    current["pending_admission"] = "Exact original library var offset is currently rejected by the root-only mutable module-slot boundary; retain its positive 42 oracle and all original graph assertions."
                elif selected in ("generic_value_struct", "generic_scalar_class"):
                    current["pending_admission"] = "The exact original facade re-export is rejected at the assigned production baseline with statement syntax is not implemented in XIR (node 90). Root, library and facade bytes, the positive 42 oracle and all nominal-identity assertions remain OPEN."
                elif selected == "generic_constraint_methods":
                    current["pending_admission"] = "The exact original root/library/facade input is rejected at the assigned production baseline with import requires an exported declaration. All original positive and negative variant literals remain in the responsibility census; the positive 42 oracle is not a rejection test."
                elif selected == "byte_comparison":
                    current["pending_admission"] = "The original crypto import is rejected at the assigned production baseline with name does not resolve to an admitted nominal type. Preserve every empty, alias, equal, differing-value and differing-length assertion and its positive 42 oracle."
                sources.append(current)
                continue
            current["projected_obligations"] = {name: {"status": "PENDING_FULL_QUALIFICATION", "verification": note}
                                                for name, note in COMMON_OBLIGATIONS.items()}
            prefix = f"test_source_product_{selected}"
            current["candidate_replacement_gates"] = [f"{prefix}_{mode}" for mode in ("vm", "native", "mixed_even", "mixed_odd")]
            current["candidate_replacement_gates"] += [f"{prefix}_{kind}_{mode}"
                for kind in ("axes", "compiler", "runtime", "cancel") for mode in range(4)]
            if selected in ("generics", "callables", "single_module", "two_modules", "text_program"):
                current["candidate_replacement_gates"].append(f"test_source_product_detachment_{selected}")
                current["candidate_replacement_gates"] += [f"test_source_product_detachment_{selected}_{kind}"
                                                          for kind in ("axes", "compiler")]
                current["projected_obligations"]["independent_product_failure_release"] = {
                    "status": "PENDING_FULL_QUALIFICATION",
                    "verification": "Whole two-session/product, physical source deletion, independent Checked read/write, actual C emission and post-producer verification use one finite compiler ledger; every actual allocation fault releases all owners. Exact private fixture replay is finite and physically released before the measured operation."}
            if selected == "generics":
                current["additional_legacy_obligations"] = {
                    "specialization_identity": {"status": "IMPLEMENTED_NOT_QUALIFIED",
                        "verification": "Closed Checked answer calls one exact i64 specialization twice and a distinct bool specialization once; operand/result types are exact."},
                    "independent_product_determinism": {"status": "IMPLEMENTED_NOT_QUALIFIED",
                        "verification": "Two fresh sessions publish independent source/closed packets and identical actual C; detached readers survive both producer destructions."},
                    "open_generic_entry_rejection": {"status": "OPEN",
                        "note": "The removed selectable legacy source entry must be mapped to a current public authority/admission rejection."},
                    "legacy_four_function_count_and_param_mode_layout": {"status": "OPEN",
                        "note": "The new graph has an explicit host adapter and synthetic entry; retirement of old representation assertions needs a reviewed mapping."}}
            elif selected == "callables":
                current["additional_legacy_obligations"] = {
                    "independent_product_determinism": {"status": "IMPLEMENTED_NOT_QUALIFIED",
                        "verification": "Two fresh sessions publish independent source/closed packets and identical actual C; detached readers survive both producer destructions."},
                    "entry_callable_effect_rejection": {"status": "OPEN",
                        "note": "The old source entry rejection is not a language ban on legal callable parameters; preserve its authority/effect responsibility in a current admission test."},
                    "legacy_function_id_emission_spelling": {"status": "OPEN",
                        "note": "Actual sealed bindings are verified; the obsolete generated representation assertion needs a reviewed retirement mapping."}}
            elif selected == "single_module":
                current["additional_legacy_obligations"] = {
                    "independent_product_determinism": {"status": "IMPLEMENTED_NOT_QUALIFIED",
                        "verification": "Independent parser sessions, exact source/closed packet bytes, facts and actual C."},
                    "detached_input_admission": {"status": "IMPLEMENTED_NOT_QUALIFIED",
                        "verification": "Copied packet corruption is rejected; existing independent Checked readers verify after producer destruction, and reader work=1 rejects without output."},
                    "module_shape": {"status": "IMPLEMENTED_NOT_QUALIFIED",
                        "verification": "One root module has no dependency or slot; its initializer is distinct from the synthetic entry."},
                    "original_function_and_operation_counts": {"status": "OPEN",
                        "note": "The current whole program includes a synthetic entry and exported host adapter; retired selective-program counts require reviewed representation mapping."},
                    "source_file_unlink_before_admission": {"status": "IMPLEMENTED_NOT_QUALIFIED",
                        "verification": "Exact private source copies are physically removed and probed missing before the independent Checked readers; tracked originals stay intact."}}
                if selected == "single_module":
                    current["additional_legacy_obligations"]["record_and_net_resource_type_identity"] = {
                        "status": "OPEN", "note": "The original preliminary type copy/equality/subclass and exact resource identity checks remain separate responsibilities."}
            elif selected == "text_program":
                current["candidate_replacement_gates"] += [f"{prefix}_output_status_{mode}" for mode in range(4)]
                current["additional_legacy_obligations"] = {
                    "fixed_text_and_typed_groups": {"status": "IMPLEMENTED_NOT_QUALIFIED",
                        "verification": "Every successful call verifies original 48 rendered bytes and three exact string/bool/rune/i64 groups independently in each backend and Instance."},
                    "provider_failure_cleanup": {"status": "IMPLEMENTED_NOT_QUALIFIED",
                        "verification": "Each group position preserves prior bytes and exact closed output failure status, then the initialized Instance runs another successful call and releases physically."},
                    "independent_product_determinism": {"status": "IMPLEMENTED_NOT_QUALIFIED",
                        "verification": "Two independent sessions publish byte-identical source/closed packets and actual C; private source files are physically removed before reader admission, and both readers survive producer destruction."},
                    "original_operation_and_generated_spelling": {"status": "OPEN",
                        "note": "Original owner-copy/drop, provider requirement and operation count assertions and obsolete generated spelling need a reviewed semantic mapping."},
                    "original_private_executor_step_identity": {"status": "OPEN",
                        "note": "The new public backends have independent fixed oracles; old private executor step equality needs a reviewed retirement mapping."}}
            elif selected == "integer_conversions":
                current["additional_legacy_obligations"] = {
                    "original_integer_conversion_operation_count": {"status": "OPEN",
                        "note": "The original requires at least four legacy INTEGER_CONVERT operations. Independent 255 results preserve the semantic oracle; the new Checked representation count still needs a reviewed mapping."}}
        sources.append(current)
    expected = {}
    for case in manifest["cases"]:
        if case["name"] not in SELECTED:
            continue
        if case["fixture"]:
            expected[case["name"]] = case["fixture"]["expected_exit"]
        else:
            literal = static_strings(bodies[case["name"]][0])["source"]
            constants = re.findall(rb'fn answer\(\) -> i64 \{ return (-?\d+) \}', literal)
            if len(constants) != 1:
                raise ValueError("a non-fixture consumer needs one original constant-result oracle")
            expected[case["name"]] = int(constants[0])
    cmake = ["# Fixed result oracles come from the unchanged legacy manifest or original constant source.",
             "set(product_consumer_cases " + " ".join(SELECTED.values()) + ")"]
    cmake += [f"set(product_consumer_expected_{fixture} {expected[name]})"
              for name, fixture in SELECTED.items()]
    files[DEST / "source_product_consumer_cases.cmake"] = ("\n".join(cmake) + "\n").encode()
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
        "source_allocation_scenarios": allocation_scenarios(),
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

#!/usr/bin/env python3
"""Preserve the complete legacy consumer census and exact source inputs."""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
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
NUMERIC_SPEC = importlib.util.spec_from_file_location("numeric_sources", ROOT / "scripts/source_product_consumer_numeric_sources.py")
NUMERIC = importlib.util.module_from_spec(NUMERIC_SPEC)
NUMERIC_SPEC.loader.exec_module(NUMERIC)
NOMINAL_SPEC = importlib.util.spec_from_file_location("nominal_sources", ROOT / "scripts/source_product_consumer_nominal_sources.py")
NOMINAL = importlib.util.module_from_spec(NOMINAL_SPEC)
NOMINAL_SPEC.loader.exec_module(NOMINAL)
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
    "source_owner_class_alias_escape_projects_one_share": "class_alias_escape",
    "source_owner_class_field_self_assignment_shares_before_exchange": "class_field_self_assignment",
    "source_owner_class_alias_borrows_coalesce_without_share": "class_alias_borrow",
    "source_owner_class_alias_final_transfer_coalesces_to_move": "class_alias_transfer",
    "source_owner_function_parameter_suspending_callable_has_one_program": "parameter_coroutine",
    "source_owner_cross_module_instance_method_preserves_receiver_across_suspend": "module_receiver_suspend",
    "source_owner_narrow_array_elements_precede_allocation": "narrow_array",
    "source_owner_cross_module_coroutine_call_has_one_program_and_private_executors": "cross_module_coroutine",
    "source_owner_cross_module_static_method_coroutine_has_one_program_and_private_executors": "cross_module_static_coroutine",
    "source_owner_runs_each_dense_coroutine_state_across_private_executors": "multi_safepoint",
    "source_owner_folds_constructor_literal_and_reordered_stores": "constructor_folding",
    "source_owner_module_initializer_is_a_canonical_entry": "canonical_initializer",
    **NUMERIC.MATRICES,
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
    "source_owner_generic_nested_class_types_are_exact": "generic_nested_class",
}
DESCRIPTORS = {
    "source_owner_recursive_class_type_reservation_is_cycle_safe": ("recursive_class_graph", 1),
    "source_owner_mutual_class_type_reservation_is_cycle_safe": ("mutual_class_graph", 2),
    NOMINAL.DEEP_TEST: ("deep_class_graph", 80),
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


def multi_safepoint_oracle(body: str, literals: dict[str, bytes], fixture: dict) -> int:
    """Separate the original returned value from its native harness success exit."""
    values = re.findall(r'ASSERT_EQ_INT\(vm_outcome\.value\.as\.i64,\s*(-?\d+)\)', body)
    source_values = re.findall(rb'\breturn\s+(-?\d+)\b', literals["source"])
    if values != ["42"] or source_values != [b"42"]:
        raise ValueError("the original dense coroutine result oracle changed")
    if literals["source"].count(b"Coro.yield()") != 2:
        raise ValueError("the original dense coroutine needs both real yields")
    if "outcome.value != INT64_C(42)" not in body or '"    return 227;' not in body:
        raise ValueError("the original native assertions or success exit changed")
    if fixture["id"] != "multi_safepoint" or fixture["expected_exit"] != 227:
        raise ValueError("the native harness success exit must remain 227")
    return 42


def constructor_oracle(body: str, literals: dict[str, bytes]) -> int:
    """Add an independent executable consequence to the original structural proof."""
    source = literals["source"]
    required = [b"constructor() { this.value = 0 }", b"this.b = true", b"this.a = x",
        b"this.second = x", b"this.first = y",
        b"return c.value + p.a + s.first * 100 + s.second"]
    if not all(text in source for text in required):
        raise ValueError("the original constructor field-value relationships changed")
    pair = re.findall(rb'var p = Pair\((\d+)\)', source)
    swap = re.findall(rb'var s = Swap\((\d+), (\d+)\)', source)
    if pair != [b"5"] or swap != [(b"1", b"10")] or b"var c = Counter()" not in source:
        raise ValueError("the original constructor argument values changed")
    if not re.search(r'program_operation_count\(program, XR_CORE_OP_CORE_CLASS_CONSTRUCT\),\s*3u', body):
        raise ValueError("the original three-construction structural assertion changed")
    return 0 + int(pair[0]) + int(swap[0][1]) * 100 + int(swap[0][0])


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
    numeric_constructors = {}
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
        if case["name"] in DESCRIPTORS:
            selected, count = DESCRIPTORS[case["name"]]
            fixture = DEST / "fixtures" / selected / "root.xr"
            if case["name"] == NOMINAL.DEEP_TEST:
                data, nominal_metadata, constructor = NOMINAL.construct_deep_source(body, c_literal, STRING)
                files[DEST / "source_product_nominal_constructors.c"] = NOMINAL.constructor_oracle(constructor)
            else:
                data = literals["source"]
            files[fixture] = data
            current["initial_projection"] = {"fixture": fixture.relative_to(ROOT).as_posix(),
                "sha256": digest(data), "original_prefix_sha256": digest(data), "expected_classes": count,
                "scope": "Exact original structural-only input, without a callable adapter or invented runtime result. Checked and Lowered descriptor readers preserve the original one/two nullable class cycles after producers die."}
            prefix = f"test_source_product_{selected}"
            current["candidate_replacement_gates"] = [prefix, prefix + "_axes", prefix + "_compiler"]
            current["projected_obligations"] = {
                "exact_nominal_graph": {"status": "IMPLEMENTED_NOT_QUALIFIED",
                    "verification": "Exactly one self-cycle or two mutual class/Nullable cycles retain their nominal declaration/field identities and owned classification in Checked and Lowered readers."},
                "detached_producers_and_native_projection": {"status": "IMPLEMENTED_NOT_QUALIFIED",
                    "verification": "Session and SourceProduct die before descriptor inspection; rewritten Checked and re-lowered emitted C agree with the producer, and actual emitted C is strictly compiled."},
                "compiler_failures_and_budgets": {"status": "PENDING_FULL_QUALIFICATION",
                    "verification": "One finite ledger covers the full producer/read/write/lower/emit operation, all actual malloc ordinals and allocated/live/work boundaries, ending at physical zero."},
                "full_safety": {"status": "PENDING_FULL_QUALIFICATION",
                    "verification": "Full applicable safety and integration-head revalidation remain required."}}
            current["additional_legacy_obligations"] = {"explicit_copy_contract_mapping": {
                "status": "OPEN", "note": "Original AFFINE/COPY_EXPLICIT enum assertions need a reviewed mapping to the current owned descriptor and source copy-permission rules. Owned classification alone does not prove source copy authority."}}
            if case["name"] == NOMINAL.DEEP_TEST:
                current["initial_projection"]["original_source_constructor"] = nominal_metadata
                current["initial_projection"]["scope"] = "Exact original descending C79-to-C0 structural-only input, without an adapter or invented runtime result. All eighty owned class identities, seventy-nine Nullable edges and one terminal i64 field are inspected after producers die; eighty classes do not prove unbounded depth."
                current["candidate_replacement_gates"].append("source_product_nominal_source_constructors")
                current["projected_obligations"]["exact_nominal_graph"]["verification"] = "Exactly C0 through C79 retain one field each: next: Nullable<C(i+1)> for C0 through C78 and value: i64 for C79. Declaration order and source bytes come from the complete original C constructor; Checked and Lowered independently preserve all identities, edges and owned classification."
                current["additional_legacy_obligations"]["complete_original_source_constructor"] = {
                    "status": "IMPLEMENTED_NOT_QUALIFIED", "verification": "An independently compiled exact copy of the original C loop, including CLASS_COUNT=80, SOURCE_CAPACITY=32768 and every snprintf, emits byte-identical fixture source."}
            sources.append(current)
            continue
        if case["name"] in SELECTED or case["name"] in PROBES:
            selected = (SELECTED | PROBES)[case["name"]]
            fixture = DEST / "fixtures" / selected / "root.xr"
            data = literals.get("source", literals.get("entry_source"))
            numeric_metadata = None
            if case["name"] in NUMERIC.MATRICES:
                data, numeric_metadata, constructor = NUMERIC.construct_source(
                    body, selected, c_literal, static_strings, function_end, STRING)
                numeric_constructors[selected] = constructor
                files[fixture.parent / "original.xr"] = data
            if data is None:
                raise ValueError("original positive source literal is missing")
            additional_files = {filename: literals[literal]
                for literal, filename in (("library_source", "library.xr"), ("facade_source", "facade.xr"))
                if literal in literals}
            for filename, content in additional_files.items():
                files[fixture.parent / filename] = content
            adapter = b"" if selected == "canonical_initializer" else b"export fn consumerAnswer() -> i64 { return answer() }\n"
            files[fixture] = data + adapter
            current["initial_projection"] = {"fixture": fixture.relative_to(ROOT).as_posix(),
                "sha256": digest(data + adapter), "original_prefix_sha256": digest(data),
                "entry_adapter": adapter.decode(), "entry": "consumerAnswer", "expected_i64": multi_safepoint_oracle(body, literals, case["fixture"]) if selected == "multi_safepoint" else (case["fixture"]["expected_exit"] if case["fixture"] else 42),
                "scope": "Exact original source prefix and fixed result through an explicitly exported adapter; additional legacy assertions stay OPEN until mapped."}
            if numeric_metadata is not None:
                current["initial_projection"]["original_source_constructor"] = numeric_metadata
            if selected == "constructor_folding":
                current["initial_projection"]["expected_i64"] = constructor_oracle(body, literals)
                current["initial_projection"]["scope"] = "Exact original structural constructor input plus an exported execution adapter. The added independent runtime oracle is 0+5+10*100+1=1006; the original test contains only structural assertions."
            if selected == "canonical_initializer":
                if data != b"struct Box<T> { value: T }\nprint(Box<i64>{value: 41}.value + 1)\n":
                    raise ValueError("original root initializer source differs")
                golden = b"42\n"
                files[fixture.parent / "expected.stdout"] = golden
                files[fixture.parent / "expected_output.h"] = (
                    "/*\n * xray - Lightweight typed scripting with native concurrency\n"
                    " * https://www.xray-lang.org\n *\n"
                    " * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>\n"
                    " * Licensed under the MIT License\n *\n"
                    " * expected_output.h - Independent original initializer output\n *\n"
                    " * KEY CONCEPT:\n *   Expected bytes follow the original print expression, 41 + 1.\n */\n"
                    "#ifndef SOURCE_PRODUCT_INITIALIZER_EXPECTED_OUTPUT_H\n#define SOURCE_PRODUCT_INITIALIZER_EXPECTED_OUTPUT_H\n"
                    "static const char consumer_initializer_golden[] = {52,50,10,0};\n"
                    "#endif // SOURCE_PRODUCT_INITIALIZER_EXPECTED_OUTPUT_H\n").encode()
                current["initial_projection"].update(entry_adapter=None, entry="canonical root entry", expected_i64=0,
                    output_golden={"bytes": len(golden), "sha256": digest(golden), "hex": golden.hex(), "groups_per_instance": 1},
                    scope="Exact original structural-only module initializer source, without an adapter. Added independent execution proof is one typed i64 output 42/newline per Instance, including repeated canonical entry calls; the entry protocol returns zero.")
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
                elif selected == "generic_nested_class":
                    current["pending_admission"] = "The exact original root/library/facade positive source is rejected at the assigned production baseline with constructor parameter contract is not admitted. Preserve its 42 oracle, all three distinct nominal declaration scopes, original managed-struct negative literals and whole legacy type/ownership assertions."
                sources.append(current)
                continue
            current["projected_obligations"] = {name: {"status": "PENDING_FULL_QUALIFICATION", "verification": note}
                                                for name, note in COMMON_OBLIGATIONS.items()}
            prefix = f"test_source_product_{selected}"
            current["candidate_replacement_gates"] = [f"{prefix}_{mode}" for mode in ("vm", "native", "mixed_even", "mixed_odd")]
            current["candidate_replacement_gates"] += [f"{prefix}_{kind}_{mode}"
                for kind in ("axes", "compiler", "runtime", "cancel") for mode in range(4)]
            if selected == "canonical_initializer":
                current["candidate_replacement_gates"] += [f"{prefix}_output_status_{mode}" for mode in range(4)]
                current["projected_obligations"]["original_source_and_fixed_result"]["verification"] = "The entire original two-line structural-only input is retained without an adapter. Added independent output is one i64 value 42/newline per Instance; the canonical entry protocol returns zero."
                current["projected_obligations"]["host_authority"]["verification"] = "Direct host starts of the private module initializer are rejected without allocation or initialization; only the canonical root entry initializes it."
                current["additional_legacy_obligations"] = {
                    "original_generic_aggregate_and_field_counts": {"status": "IMPLEMENTED_NOT_QUALIFIED", "verification": "Closed Checked retains exactly one Box<i64> STRUCT_NEW with value 41, one i64 STRUCT_GET and one typed PRINT of the field plus one."},
                    "one_time_initialization_output": {"status": "IMPLEMENTED_NOT_QUALIFIED", "verification": "Two isolated Instances each print typed i64 42 and exact bytes 42/newline once. Repeated root entries do not repeat output; all four backends use independent oracles."},
                    "sticky_provider_failure_and_isolation": {"status": "IMPLEMENTED_NOT_QUALIFIED", "verification": "Six output-provider statuses fail initialization, stick without re-execution/allocation, preserve an owned failure copy and leave an independent Instance successful."},
                    "cold_and_warm_cancellation": {"status": "IMPLEMENTED_NOT_QUALIFIED", "verification": "Every measured quantum-one prefix of first initialization and repeated entry is cancelled. Public module publication events independently require sticky FAILED before publication and READY after publication, with at most one original output and release of both physical domains."},
                    "old_entry_and_native_representation": {"status": "OPEN", "note": "Original one-function/entry-zero/provider-requirement-one and int-main emission assertions need a reviewed mapping to explicit Unit module initializer plus canonical i64 entry and sealed native ProgramSpec. No legacy main or selective-program format is restored."}}
            if selected == "narrow_array":
                current["candidate_replacement_gates"] = [gate for gate in current["candidate_replacement_gates"]
                    if not re.fullmatch(rf"{prefix}_cancel_[0-3]", gate)]
                current["pending_replacement_gates"] = [f"{prefix}_cancel_{mode}" for mode in range(4)]
                current["initial_projection"]["invocation_policy"] = {
                    "first_result_per_instance": 42, "successful_calls_per_instance": 1,
                    "second_call_status": "XR_XIR_CALL_ASSERTION",
                    "second_call_panic": "XR_XIR_PANIC_ASSERTION",
                    "note": "The original visits slot remains mutable; a second call increments it again and fails the original ordered-element assertion. No reset or source alteration is allowed."}
                current["projected_obligations"]["cancellation"] = {"status": "OPEN",
                    "verification": "Every active cancellation prefix must preserve the original partial visits state. The repeatable-fixture cancellation oracle is inapplicable; these gates are not registered until an independent state oracle is implemented."}
                current["projected_obligations"]["runtime_failures"]["verification"] = (
                    "Actual instance/init/first answer allocation ordinals retain the original first-call 42 oracle, sticky initialization failure and final physical release.")
            if selected in ("generics", "callables", "single_module", "two_modules", "text_program",
                            "cross_module_coroutine", "cross_module_static_coroutine"):
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
            elif selected == "narrow_array":
                current["additional_legacy_obligations"] = {
                    "initializer_prefix_membership": {"status": "OPEN",
                        "note": "The original preliminary initializer-prefix membership checks remain a separate responsibility."},
                    "element_evaluation_precedes_allocation": {"status": "OPEN",
                        "note": "Normal original assertions retain narrow element bounds and left-to-right visits 1,2,3. Allocation-failure ordering and retired Core/CGen representation checks still need a reviewed mapping."},
                    "stateful_repeat_and_owned_panic": {"status": "IMPLEMENTED_NOT_QUALIFIED",
                        "verification": "Two detached instances each return the independent original 42 once, then the untouched answer asserts. Each copied assertion panic remains valid after its Instance is consumed; both panic owners are dropped before physical release."}}
            elif selected in ("class_alias_escape", "class_field_self_assignment", "class_alias_borrow", "class_alias_transfer"):
                counts = {
                    "class_alias_escape": {"CLASS_CONSTRUCT": 3, "OWNER_ALIAS": 1,
                        "CLASS_FIELD_PLACE": 2, "PLACE_EXCHANGE": 2, "CLASS_FIELD_LOAD": 2, "OWNER_COPY": 0},
                    "class_field_self_assignment": {"CLASS_CONSTRUCT": 2, "OWNER_ALIAS": 1,
                        "CLASS_FIELD_PLACE": 1, "PLACE_EXCHANGE": 1},
                    "class_alias_borrow": {"CLASS_CONSTRUCT": 1, "OWNER_ALIAS": 0,
                        "CLASS_FIELD_PLACE": 1, "PLACE_EXCHANGE": 1, "CLASS_FIELD_LOAD": 1, "OWNER_COPY": 0},
                    "class_alias_transfer": {"CLASS_CONSTRUCT": 2, "OWNER_ALIAS": 0,
                        "OWNER_MOVE": 1, "CLASS_FIELD_LOAD": 2, "OWNER_COPY": 0},
                }
                current["additional_legacy_obligations"] = {
                    "original_operation_counts": {"status": "OPEN", "counts": counts[selected],
                        "note": "Exact retired Core operation counts require a reviewed mapping to Checked/Lowered ownership facts; independent 42 results do not qualify these counts."}}
                if selected in ("class_alias_escape", "class_field_self_assignment"):
                    current["additional_legacy_obligations"]["exchange_drop_edges"] = {
                        "status": "OPEN",
                        "note": "The original requires field-place/exchange operand identity and exactly one matching owner drop after every affine exchange; escape also distinguishes one trivial exchange. Self-assignment requires sharing the borrowed field before its exchange."}
                    current["additional_legacy_obligations"]["class_lifecycle_order"] = {
                        "status": "OPEN",
                        "note": "Original lifecycle logs must not overflow or use domain teardown. Escape requires distinct exchanged identities, an immediate operation-origin drop and exactly one finalize/reclaim for both identities. Self-assignment requires share/place/exchange/drop order with identical referent identities and exactly one finalize/reclaim."}
            elif selected in ("parameter_coroutine", "module_receiver_suspend"):
                current["projected_obligations"]["real_yield_and_wake_authority"] = {
                    "status": "IMPLEMENTED_NOT_QUALIFIED",
                    "verification": "Every ordinary answer call observes exactly two actual YIELD waits, rejects wrong epoch/wake without allocations or result output, and resumes with its exact tokens. Complete cancellation prefixes include both pending yields and reject each stale wake after cancellation."}
                current["additional_legacy_obligations"] = {
                    "original_coroutine_representation": {"status": "OPEN",
                        "note": "Original operation counts, coroutine state/safepoint identities, parameter modes, live owners and normal/cancel cleanup edges require a reviewed Checked/Lowered mapping."}}
                if selected == "parameter_coroutine":
                    current["additional_legacy_obligations"]["original_coroutine_representation"]["counts"] = {
                        "COROUTINE_CALL_INDIRECT": 2, "COROUTINE_CALL_SEALED": 1,
                        "entry_coroutine_states": 3, "entry_coroutine_safepoints": 2}
                else:
                    current["additional_legacy_obligations"]["original_coroutine_representation"]["counts"] = {
                        "CLASS_CONSTRUCT": 1, "COROUTINE_CALL_SEALED": 1}
                    current["additional_legacy_obligations"]["receiver_lifecycle_across_suspension"] = {
                        "status": "OPEN",
                        "note": "The original keeps the one receiver alive through both yields, cancels after each yield, rejects lifecycle-log overflow and requires one finalize/reclaim per construction on success and cancellation. Physical release and the fixed 42 oracle alone do not qualify event ordering."}
            elif selected == "constructor_folding":
                current["additional_legacy_obligations"] = {
                    "class_construction_field_mapping": {"status": "IMPLEMENTED_NOT_QUALIFIED",
                        "verification": "Checked keeps exactly one construction for each Counter/Pair/Swap; all construction operands follow declared field types and order, Counter has constant0, Pair has constant true and Swap uses distinct values. Independent execution verifies Swap's crossed parameter values."},
                    "literal_folding_representation": {"status": "OPEN",
                        "note": "The original requires each folded literal to be a direct constant producer in the caller construction. Current declaration-owned constructor locals require a reviewed equivalent Checked/Lowered mapping; do not silently retire this original shape assertion."}}
            elif selected == "multi_safepoint":
                current["initial_projection"]["legacy_native_harness_exit"] = case["fixture"]["expected_exit"]
                current["initial_projection"]["scope"] = "Exact original source and independent VM/native assertion value 42 through the exported adapter. Original native harness success exit 227 is retained separately and is not a language return value."
                current["projected_obligations"]["real_yield_and_wake_authority"] = {
                    "status": "IMPLEMENTED_NOT_QUALIFIED",
                    "verification": "Every answer call observes both original real YIELD waits before returning 42; wrong or stale epoch/wake tokens are rejected. All active cancellation prefixes include both pending yields and preserve a callable initialized instance."}
                current["additional_legacy_obligations"] = {
                    "original_coroutine_representation": {"status": "OPEN",
                        "counts": {"coroutine_states": 3, "coroutine_safepoints": 2,
                            "COROUTINE_YIELD": 2, "safepoint_live_values": [0, 0]},
                        "legacy_native_harness_exit": 227,
                        "note": "Original dense resume states 1/2, safepoints 0/1, cancellation state identities, no live values and generated C state/suspend spellings require a reviewed Checked/Lowered mapping. Native harness exit 227 follows all independent value/state/cancellation assertions; retain its protocol separately from the returned 42."}}
            elif selected in ("cross_module_coroutine", "cross_module_static_coroutine"):
                helper_start = text.index("static void assert_cross_module_coroutine_program(")
                helper_body = text[helper_start:function_end(text, text.index("{", helper_start))]
                current["projected_obligations"]["real_yield_and_wake_authority"] = {
                    "status": "IMPLEMENTED_NOT_QUALIFIED",
                    "verification": "Each original cross-module answer returns the independent 7 after two real YIELD waits. Wrong epoch/wake queries and resumes preserve their outputs and allocations; complete cancellation prefixes include both pending yields and reject their stale wake tokens."}
                current["additional_legacy_obligations"] = {
                    "independent_product_determinism_and_source_detachment": {"status": "IMPLEMENTED_NOT_QUALIFIED",
                        "verification": "Two independent sessions publish identical source/closed bytes, facts and actual C. Both root and library private copies are physically removed before detached Checked reader admission; readers verify and rewrite after both products are destroyed."},
                    "original_coroutine_representation": {"status": "OPEN",
                        "shared_helper_body_lf_sha256": digest(helper_body.encode()),
                        "counts": {"entry_coroutine_states": 2, "entry_coroutine_safepoints": 1,
                            "child_coroutine_states": 3, "child_coroutine_safepoints": 2,
                            "child_parameters": 1, "sealed_call_successors": 2},
                        "note": "Original entry/child effect and capability masks, READ i64 parameter, exact cancel successor publication, private safepoint/state identities and old generated spellings require a reviewed Checked/Lowered mapping. The full shared helper is retained by its body hash."}}
            elif numeric_metadata is not None:
                current["candidate_replacement_gates"].append("source_product_integer_source_constructors")
                current["additional_legacy_obligations"] = {
                    "complete_original_matrix": {"status": "IMPLEMENTED_NOT_QUALIFIED",
                        "verification": "All eight widths, original failure codes and every mixed-width or signedness boundary remain in the source prefix. An independently compiled copy of the original C constructor must emit byte-identical original source."}}
        sources.append(current)
    files[DEST / "source_product_integer_constructors.c"] = NUMERIC.constructor_oracle(numeric_constructors)
    expected = {}
    for case in manifest["cases"]:
        if case["name"] not in SELECTED:
            continue
        if SELECTED[case["name"]] == "canonical_initializer":
            expected[case["name"]] = 0
        elif SELECTED[case["name"]] == "constructor_folding":
            body = bodies[case["name"]][0]
            expected[case["name"]] = constructor_oracle(body, static_strings(body))
        elif SELECTED[case["name"]] == "multi_safepoint":
            body = bodies[case["name"]][0]
            expected[case["name"]] = multi_safepoint_oracle(body, static_strings(body), case["fixture"])
        elif case["fixture"]:
            expected[case["name"]] = case["fixture"]["expected_exit"]
        else:
            literal = static_strings(bodies[case["name"]][0])["source"]
            constants = re.findall(rb'fn answer\(\) -> i64 \{ return (-?\d+) \}', literal)
            if len(constants) != 1:
                raise ValueError("a non-fixture consumer needs one original constant-result oracle")
            expected[case["name"]] = int(constants[0])
    cmake = ["# Fixed result oracles come from original language-result assertions, the manifest or constant source.",
             "set(product_consumer_cases " + " ".join(SELECTED.values()) + ")"]
    cmake += [f"set(product_consumer_expected_{fixture} {expected[name]})"
              for name, fixture in SELECTED.items()]
    files[DEST / "source_product_consumer_cases.cmake"] = ("\n".join(cmake) + "\n").encode()
    graph_cmake = ["# Exact original structural inputs have no invented runtime result.",
        "set(product_nominal_graph_cases " + " ".join(case for case, _ in DESCRIPTORS.values()) + ")"]
    graph_cmake += [f"set(product_nominal_graph_expected_{case} {count})" for case, count in DESCRIPTORS.values()]
    files[DEST / "source_product_nominal_graph_cases.cmake"] = ("\n".join(graph_cmake) + "\n").encode()
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

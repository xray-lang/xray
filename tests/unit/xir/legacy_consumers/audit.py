#!/usr/bin/env python3
"""Inspect legacy consumer obligations without running or qualifying Xray."""
from __future__ import annotations

import argparse
import collections
import dataclasses
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import subprocess

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]


# Explicit reviewed transitions prevent old negative oracles becoming target truth.
FROZEN_TARGETS = {}
for name in ("001_comptime_non_const_call.xr", "012_comptime_fixed_array_element_call.xr",
             "014_comptime_struct_field_call.xr", "016_comptime_member_access_call.xr",
             "028_comptime_block_return_call.xr"):
    FROZEN_TARGETS["tests/compile_errors/binding/" + name] = ("POSITIVE_REQUIRED_BY_FROZEN_TASK320", "P11")
for name in ("007_in_instance_mutating_method.xr", "008_in_instance_indirect_mutating_method.xr",
             "009_in_instance_inherited_mutating_method.xr"):
    FROZEN_TARGETS["tests/compile_errors/ownership/" + name] = ("POSITIVE_AFTER_FROZEN_CLASS_SURFACE_PORT", "P10")
for name in ("generator_await_rejected.xr", "generator_coro_yield_rejected.xr",
             "generator_defer_rejected.xr", "generator_dynamic_call_rejected.xr",
             "generator_scope_rejected.xr", "generator_transitive_suspend_rejected.xr"):
    FROZEN_TARGETS["tests/compile_errors/concurrency/" + name] = ("POSITIVE_REQUIRED_BY_FROZEN_TASK316", "P08")
FROZEN_TARGETS["tests/compile_errors/oop/020_enum_implements_rejected.xr"] = ("REJECTION_FOR_MISSING_INTERFACE_REQUIREMENT", "P10")


for name in ("024_struct_dynamic_container_field.xr", "084_const_object_field_store_rejected.xr",
             "085_const_array_alias_mutating_method.xr", "086_const_map_alias_index_store_rejected.xr",
             "087_const_object_alias_field_store_rejected.xr", "088_const_array_mutable_alias_init_rejected.xr",
             "089_const_map_mutable_alias_init_rejected.xr", "090_const_object_mutable_alias_init_rejected.xr",
             "099_shared_object_field_store_rejected.xr", "100_shared_object_compound_field_store_rejected.xr",
             "105_shared_member_array_alias_index_store_rejected.xr", "106_shared_member_array_alias_mutating_method_rejected.xr",
             "109_shared_member_array_alias_mutating_param_rejected.xr", "118_shared_derived_closure_capture_mutation_rejected.xr"):
    FROZEN_TARGETS["tests/compile_errors/type/" + name] = ("POSITIVE_VALUE_OR_IDENTITY_AFTER_FROZEN_TASK313_PORT", "P10")
for name in ("union_indiscriminable_int_members.xr", "union_indiscriminable_float_members.xr", "union_indiscriminable_via_alias.xr"):
    FROZEN_TARGETS["tests/compile_errors/type/" + name] = ("POSITIVE_EXACT_UNION_MEMBER_AFTER_FROZEN_TASK319_PORT", "P10")
FROZEN_TARGETS["tests/compile_errors/type/span_tuple_literal_escape.xr"] = ("POSITIVE_LOCAL_VIEW_TUPLE_AFTER_FROZEN_TASK313_PORT", "P10")
FROZEN_TARGETS["tests/compile_errors/ownership/080_transfer_task_second_await_rejected.xr"] = (
    "POSITIVE_STICKY_TASK_RESULT_REQUIRED_BY_FROZEN_TASK316", "P08")
FROZEN_TARGETS["tests/diff/cases/semantics/coro/task_transfer_already_taken.xr"] = (
    "POSITIVE_STICKY_TASK_RESULT_AFTER_FROZEN_TASK316_PORT", "P08")
FROZEN_TARGETS["tests/diff/cases/semantics/coro/task_transfer_return_native_class.xr"] = (
    "REJECT_NONSENDABLE_TASK_RESULT_AFTER_FROZEN_TASK313_PORT", "P10")


# The demanded imported diagnostics depend on these complete companion inputs.
IMPORTED_ORACLE_INPUTS = {}
for name in ("156_imported_ref_call_requires_marker.xr", "158_imported_const_function_value_ref_call_requires_marker.xr"):
    IMPORTED_ORACLE_INPUTS["tests/compile_errors/ownership/" + name] = ("tests/fixtures/param_mode_import_lib.xr",)
for name in ("sys_thread_spawn_imported_function_value_suspendable_rejected.xr",
             "sys_thread_spawn_imported_suspendable_call_rejected.xr",
             "sys_thread_spawn_namespace_suspendable_call_rejected.xr"):
    IMPORTED_ORACLE_INPUTS["tests/compile_errors/concurrency/" + name] = ("tests/fixtures/thread_spawn_suspendable_lib.xr",)
for name in ("parallel_callback_imported_function_value_suspendable_rejected.xr",
             "parallel_callback_imported_suspendable_call_rejected.xr",
             "parallel_callback_namespace_suspendable_call_rejected.xr"):
    IMPORTED_ORACLE_INPUTS["tests/compile_errors/stdlib/" + name] = ("tests/fixtures/thread_spawn_suspendable_lib.xr",)
for name in ("parallel_callback_selective_xrd_unknown_effect_rejected.xr",
             "parallel_callback_xrd_handle_method_unknown_effect_rejected.xr",
             "parallel_callback_xrd_unknown_effect_rejected.xr"):
    IMPORTED_ORACLE_INPUTS["tests/compile_errors/stdlib/" + name] = ("tests/compile_errors/stdlib/parallel_unknown_effect_native.xrd",)


for name in ("113_shared_derived_imported_function_value_mutating_param_rejected.xr",
             "114_shared_derived_namespace_function_value_mutating_param_rejected.xr"):
    IMPORTED_ORACLE_INPUTS["tests/compile_errors/type/" + name] = ("tests/fixtures/shared_mutation_lib.xr",)
IMPORTED_ORACLE_INPUTS["tests/compile_errors/type/115_shared_derived_reexported_function_value_mutating_param_rejected.xr"] = (
    "tests/fixtures/shared_mutation_reexport.xr", "tests/fixtures/shared_mutation_lib.xr")
IMPORTED_ORACLE_INPUTS["tests/compile_errors/type/116_shared_derived_star_reexported_function_value_mutating_param_rejected.xr"] = (
    "tests/fixtures/shared_mutation_reexport_all.xr", "tests/fixtures/shared_mutation_lib.xr")
IMPORTED_ORACLE_INPUTS["tests/compile_errors/type/slice_active_view_cross_module_relocation.xr"] = (
    "tests/fixtures/slice/_slice_memory_effect_lib.xr",)
for name, companion in (
    ("1149_module_call_stack_grow.xr", "modules/big_stack.xr"),
    ("1170_value_tag_is_the_type_answer.xr", "_value_tag_lib.xr"),
    ("1172_module_readonly_capture.xr", "modules/module_readonly_capture.xr"),
):
    IMPORTED_ORACLE_INPUTS["tests/regression/11_coroutine/" + name] = (
        "tests/regression/11_coroutine/" + companion,)


def load_module(name: str, relative: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def relative(path: Path) -> str:
    return path.relative_to(ROOT).as_posix()


def file_record(path: Path) -> dict:
    if not path.is_file():
        return {"path": relative(path), "state": "MISSING"}
    raw = path.read_bytes()
    return {"path": relative(path), "state": "PRESENT", "bytes": len(raw),
            "sha256": sha(raw), "lf_sha256": sha(raw.replace(b"\r\n", b"\n"))}


def read_json(path: Path) -> dict:
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON key: {key}")
            result[key] = value
        return result
    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique)


def validate_responsibilities(data: dict) -> None:
    expected = {"capabilities": 27, "task308_obligations": 8,
                "task318_families": 14, "task322_requirements": 18}
    for key, count in expected.items():
        rows = data[key]
        ids = [r["id"] for r in rows]
        if len(rows) != count or len(set(ids)) != count:
            raise ValueError(f"missing or duplicate responsibility in {key}")
        for row in rows:
            if row["status"] != "OPEN" or not row["packages"]:
                raise ValueError(f"preparation cannot qualify {row['id']}")
    if {r["id"] for r in data["task322_requirements"]} != {f"R{i:02}" for i in range(1, 19)}:
        raise ValueError("root-effect requirement set changed")


def validate_routes(data: dict, legacy: dict, responsibilities: dict) -> dict:
    """Preserve the full original responsibility set while assigning producers."""
    families = {row["id"] for row in responsibilities["capabilities"]}
    packages = {"P02"} | {f"P{i:02}" for i in range(6, 19)}
    if data["status"] != "ROUTED_NOT_QUALIFIED":
        raise ValueError("routing cannot publish product qualification")
    originals = {row["name"]: row for row in legacy["source"]}
    rows = data["source_cases"]
    identities = [row["case_id"] for row in rows]
    if len(set(identities)) != len(rows) or set(identities) != set(originals):
        raise ValueError("source routing changed the original responsibility set")
    for row in rows:
        original = originals[row["case_id"]]
        if row["primary_family"] not in families or not row["producer_packages"] or not set(row["producer_packages"]) <= packages:
            raise ValueError("source routing has no known family or producer")
        if row["body_lf_sha256"] != original["body_lf_sha256"] or row["legacy_path"] != original["legacy_path"]:
            raise ValueError("source routing detached its original assertion body")
        if row["original_native_harness_expectation"] != original["original_fixture"]:
            raise ValueError("native harness expectation changed during routing")
        if row["existing_projection"] != original.get("initial_projection"):
            raise ValueError("source projection changed during routing")
        if row["replacement_qualification"] != "OPEN" or row["new_entry_status"] != "PENDING_FIXED_PRODUCT_DELIVERY":
            raise ValueError("routing cannot grant replacement qualification")
    paths = [row["path"] for row in data["other_files"]]
    originals_by_path = legacy["other_consumer_files"]
    if len(set(paths)) != len(paths) or set(paths) != set(originals_by_path):
        raise ValueError("other-file routing omitted a function or helper owner")
    for row in data["other_files"]:
        original = originals_by_path[row["path"]]
        functions = [{"name": f["name"], "body_lf_sha256": f["body_lf_sha256"]} for f in original["functions"]]
        if row["lf_sha256"] != original["lf_sha256"] or row["functions"] != functions:
            raise ValueError("other-file routing changed original function obligations")
        if (not row["producer_packages"] or not set(row["producer_packages"]) <= packages
                or row["replacement_qualification"] != "OPEN" or row["assertion_mapping"] != "OPEN"):
            raise ValueError("other-file routing is unowned or incorrectly qualified")
    return {"source_routes": len(rows), "other_file_routes": len(paths),
            "other_function_routes": sum(len(r["functions"]) for r in data["other_files"])}


def textual_test_declarations(text: str) -> list[dict]:
    """Preserve reviewed marker segments; this is not parser/runtime discovery."""
    import re
    hits = list(re.finditer(r"(?m)^[ \t]*@test[ \t\r\n]+fn[ \t]+([A-Za-z_]\w*)[ \t]*\(", text))
    return [{"name": m.group(1), "marker_line": text.count("\n", 0, m.start()) + 1,
             "source_segment_lf_sha256": sha(text[m.start():hits[i + 1].start() if i + 1 < len(hits) else len(text)]
                                             .replace("\r\n", "\n").encode("utf-8"))}
            for i, m in enumerate(hits)]


def differential_input_obligation(source: Path, runner) -> dict:
    """Preserve parsed arguments and exact stdin bytes, including absent sidecars."""
    return {"arguments": runner.read_args(source), "stdin_hex": runner.read_stdin(source).hex(),
            "arguments_sidecar": file_record(source.with_suffix(".args")),
            "stdin_sidecar": file_record(source.with_suffix(".stdin")), "execution": "NOT_RUN"}


def validate_runner_routes(data: dict, responsibilities: dict, actual_entries: dict | None = None) -> dict:
    """Bind explicit reviewed routes to original source, oracle and runner entry."""
    if data["status"] != "REVIEWED_ROUTING_NOT_QUALIFICATION":
        raise ValueError("runner routing cannot qualify execution")
    families = ({r["id"] for r in responsibilities["capabilities"]}
                | {"task318:" + r["id"] for r in responsibilities["task318_families"]})
    packages = {"P02"} | {f"P{i:02}" for i in range(6, 19)}
    rows = data["cases"]
    ids = [r["case_id"] for r in rows]
    if not rows or len(ids) != len(set(ids)):
        raise ValueError("empty or duplicate reviewed runner inputs")
    errors = load_module("runner_route_expectations", "tests/compile_errors/run_compile_error_tests.py")
    diff = load_module("runner_route_differential", "tests/diff/run_backend_diff.py")
    if actual_entries is None:
        reg = load_module("runner_route_regression", "scripts/run_regression_tests.py")
        actual_entries = collections.defaultdict(list)
        for path in reg.collect_cases():
            actual_entries[relative(path)].append({"suite": "regression", "entry": "test" if reg.is_test_module(path) else "run"})
        for path in diff.collect_cases("", str(diff.SCRIPT_DIR / "coro_regression_cases.txt")):
            actual_entries[relative(path)].append({"suite": "differential", "entry": "run"})
        for path in errors.collect_cases():
            actual_entries[relative(path)].append({"suite": "compile_errors", "entry": "implicit_compile"})
    for row in rows:
        identity = row["case_id"]
        source = ROOT / identity
        if identity not in actual_entries or row["original_runner_entries"] != actual_entries[identity]:
            raise ValueError("reviewed input changed its original runner entry")
        if not source.is_file() or file_record(source)["lf_sha256"] != row["source_lf_sha256"]:
            raise ValueError("reviewed runner source changed")
        if (row["primary_family"] not in families or not set(row["secondary_families"]) <= families
                or not row["producer_packages"] or not set(row["producer_packages"]) <= packages):
            raise ValueError("runner routing has an unknown family or producer")
        if row["replacement_qualification"] != "OPEN" or row["new_entry_status"] != "PENDING_FIXED_PRODUCT_DELIVERY":
            raise ValueError("runner routing cannot grant product qualification")
        if not row["semantic_basis"]:
            raise ValueError("runner routing lacks reviewed semantic evidence")
        differential = row.get("original_differential_obligation")
        if any(e["suite"] == "differential" for e in actual_entries[identity]):
            if differential != differential_input_obligation(source, diff):
                raise ValueError("differential input lost arguments, stdin bytes or sidecar identity")
        elif differential is not None:
            raise ValueError("differential input obligation has no original runner entry")
        imported = row.get("original_imported_inputs", [])
        required_imports = IMPORTED_ORACLE_INPUTS.get(identity, ())
        if required_imports and tuple(i["path"] for i in imported) != required_imports:
            raise ValueError("reviewed imported oracle lost its required companion inputs")
        for item in imported:
            actual = file_record(ROOT / item["path"])
            if item != {"path": actual["path"], "state": actual["state"], "lf_sha256": actual.get("lf_sha256")}:
                raise ValueError("reviewed imported oracle companion changed")
        regression = row.get("original_regression_obligation")
        if regression:
            entries = [e["entry"] for e in actual_entries[identity] if e["suite"] == "regression"]
            if (entries != [regression["entry"]] or type(regression["expected_exit"]) is not int
                    or regression["expected_exit"] != 0 or regression["test_discovery"] != "NOT_RUN"):
                raise ValueError("original regression positive/test discovery obligation changed")
        named = row.get("original_named_test_obligation")
        reviewed_named_scope = identity.startswith(("tests/regression/08_mem/", "tests/regression/08_oop/",
                                                    "tests/regression/09_advanced/", "tests/regression/11_coroutine/"))
        if named is not None or (reviewed_named_scope and regression and regression["entry"] == "test"):
            if (not named or named["state"] != "REVIEWED_TEXTUAL_DECLARATIONS_NOT_RUNTIME_DISCOVERY"
                    or named["execution"] != "NOT_RUN"
                    or named["declarations"] != textual_test_declarations(source.read_text(encoding="utf-8"))):
                raise ValueError("named source obligations changed or falsely qualified discovery")
        port = row.get("target_source_port")
        if port:
            path = ROOT / port["fixture"]
            if (port["qualification"] != "NOT_RUN" or port["owner_package"] not in row["producer_packages"]
                    or not path.is_file() or file_record(path)["lf_sha256"] != port["lf_sha256"]):
                raise ValueError("planned source port changed or incorrectly qualified")
        migration = row.get("named_test_migration")
        if migration:
            original_names = [d["name"] for d in named["declarations"]] if named else []
            targets = list(migration["mapping"].values())
            if (not port or not original_names or set(migration["mapping"]) != set(original_names)
                    or len(targets) != len(set(targets)) or migration["state"] != "PREPARED_NOT_EXECUTED"
                    or migration["original_tests_removed"] is not False
                    or targets != [d["name"] for d in textual_test_declarations((ROOT / port["fixture"]).read_text(encoding="utf-8"))]):
                raise ValueError("named target migration omitted or requalified an original test")
        partial = row.get("partial_target_program")
        if partial:
            actual = file_record(ROOT / partial["fixture"])
            names = [d["name"] for d in named["declarations"]] if named else []
            covered, remaining = partial["covered_original_tests"], partial["remaining_original_tests"]
            if (not names or not covered or len(covered) != len(set(covered))
                    or len(remaining) != len(set(remaining)) or set(covered) & set(remaining)
                    or set(covered) | set(remaining) != set(names)
                    or partial["complete_file_replacement"] is not False or partial["qualification"] != "NOT_RUN"
                    or partial["owner_package"] not in row["producer_packages"]
                    or actual.get("lf_sha256") != partial["lf_sha256"]):
                raise ValueError("partial target program changed or hid remaining original tests")
        disposition = row.get("target_disposition")
        target = FROZEN_TARGETS.get(identity)
        if target is not None or disposition is not None:
            if (not disposition or target != (disposition["kind"], disposition["owner_package"])
                    or disposition["owner_package"] not in row["producer_packages"]
                    or disposition["qualification"] != "NOT_RUN" or not disposition.get("reason")):
                raise ValueError("planned frozen migration cannot publish qualification or change its required disposition")
            if (target[0] == "POSITIVE_AFTER_FROZEN_CLASS_SURFACE_PORT" or target[0].endswith("_PORT")) and not port:
                raise ValueError("planned frozen migration requires its frozen source port")
        if "original_rejection_oracle" in row:
            oracle = row["original_rejection_oracle"]
            path, kind = errors.expected_path_for(source)
            if oracle["path"] != relative(path) or oracle["kind"] != kind:
                raise ValueError("rejection oracle changed expectation kind")
            if path.is_file():
                assertions = [dataclasses.asdict(a) for a in errors.parse_expected(path)]
                if (oracle["state"] != "PRESENT" or oracle["lf_sha256"] != file_record(path)["lf_sha256"]
                        or oracle["assertions"] != assertions):
                    raise ValueError("rejection oracle lost original position, code or text")
            elif oracle["state"] != "MISSING" or oracle["assertions"] is not None or oracle["lf_sha256"] is not None:
                raise ValueError("missing rejection oracle cannot be fabricated")
        else:
            oracle = row["original_stdout_oracle"]
            path = Path(str(source) + ".expected")
            state = "PRESENT" if path.is_file() else "MISSING"
            digest = file_record(path).get("lf_sha256")
            if oracle != {"path": relative(path), "state": state, "lf_sha256": digest}:
                raise ValueError("runner stdout oracle changed")
    return {"reviewed_runner_inputs": len(rows),
            "reviewed_runner_entry_links": sum(len(r["original_runner_entries"]) for r in rows),
            "reviewed_dual_runner_inputs": sum(len(r["original_runner_entries"]) > 1 for r in rows),
            "reviewed_differential_input_obligations": sum("original_differential_obligation" in r for r in rows),
            "reviewed_regression_inputs": sum(any(e["suite"] == "regression" for e in r["original_runner_entries"]) for r in rows),
            "prepared_target_source_ports": sum("target_source_port" in r for r in rows),
            "reviewed_named_test_files": sum("original_named_test_obligation" in r for r in rows),
            "reviewed_textual_test_declarations": sum(len(r.get("original_named_test_obligation", {}).get("declarations", [])) for r in rows),
            "prepared_partial_target_programs": sum("partial_target_program" in r for r in rows),
            "reviewed_frozen_target_transitions": sum("target_disposition" in r for r in rows),
            "reviewed_imported_companion_links": sum(len(r.get("original_imported_inputs", [])) for r in rows),
            "reviewed_rejection_cases": sum("original_rejection_oracle" in r for r in rows),
            "unreviewed_rejection_inputs": len({p for p, e in actual_entries.items()
                if any(i["suite"] == "compile_errors" for i in e)} - set(ids)),
            "reviewed_rejection_oracles": sum(r.get("original_rejection_oracle", {}).get("state") == "PRESENT" for r in rows),
            "reviewed_missing_rejection_oracles": sum(r.get("original_rejection_oracle", {}).get("state") == "MISSING" for r in rows)}


def compare_required(expected: dict, observed: dict) -> None:
    """Compare one observation to its independent oracle, never to another backend."""
    for key, value in expected.items():
        if key not in observed or type(observed[key]) is not type(value) or observed[key] != value:
            raise ValueError(f"independent observation mismatch: {key}")


def validate_oracles(data: dict) -> dict:
    ids = [row["id"] for row in data["cases"]]
    if len(ids) != len(set(ids)) or not ids:
        raise ValueError("empty or duplicate oracle cases")
    legacy = load_module("legacy_consumer_literals", "scripts/source_product_consumer_inventory.py")
    source = legacy.SOURCE.read_text(encoding="utf-8")
    for row in data["cases"]:
        if row["status"] != "PREPARED_NOT_EXECUTED" or not row["rationale"] or not row["expected"]:
            raise ValueError("an oracle needs independent reasoning and must remain unqualified")
        for item in row["inputs"]:
            path = ROOT / item["path"]
            if not path.is_file() or file_record(path)["lf_sha256"] != item["lf_sha256"]:
                raise ValueError(f"oracle input changed: {item['path']}")
            if "original_literal" in item:
                start = source.index("TEST(" + row["legacy_case"] + ")")
                end = legacy.function_end(source, source.index("{", start))
                original = legacy.static_strings(source[start:end])[item["original_literal"]]
                if path.read_bytes().replace(b"\r\n", b"\n") != original:
                    raise ValueError(f"original positive program changed: {row['id']}")
        execution_source = row.get("execution_source")
        if execution_source and execution_source not in {i["path"] for i in row["inputs"]}:
            raise ValueError("oracle execution source is not an owned input")
        for backend in ("vm", "native"):
            if row["execution"][backend] != "NOT_RUN":
                raise ValueError("fixture preparation is not execution evidence")
        if not row["remaining_qualification"]:
            raise ValueError("preparation must retain the full qualification gap")
    return {"oracle_cases": len(ids), "product_execution": "NOT_RUN"}


def snapshot() -> dict:
    responsibilities = read_json(HERE / "responsibilities.json")
    validate_responsibilities(responsibilities)
    reg = load_module("legacy_regression_discovery", "scripts/run_regression_tests.py")
    diff = load_module("legacy_diff_discovery", "tests/diff/run_backend_diff.py")
    errors = load_module("legacy_error_discovery", "tests/compile_errors/run_compile_error_tests.py")
    ratchet = load_module("current_corpus_discovery", "scripts/check_regression_corpus.py")
    inventory = load_module("legacy_source_discovery", "scripts/source_product_consumer_inventory.py")
    legacy, generated = inventory.inventory()
    recorded = read_json(ROOT / "tests/unit/xir/product_consumers/legacy_responsibilities.json")
    if recorded != legacy:
        raise ValueError("legacy responsibility ledger drift; review before accepting a new denominator")
    for path, expected in generated.items():
        if path.read_bytes().replace(b"\r\n", b"\n") != expected:
            raise ValueError(f"legacy projection drift: {relative(path)}")
    anchors = collections.defaultdict(list)
    for family in responsibilities["capabilities"]:
        for path in family["original_anchors"]:
            anchors[path].append(family["id"])

    reviewed_runner_routes = read_json(HERE / "runner_routes.json")
    explicit_routes = {row["case_id"]: row for row in reviewed_runner_routes["cases"]}

    def case_base(path: Path, entry: str) -> dict:
        key = relative(path)
        families = anchors[key]
        route = explicit_routes.get(key)
        return {"case_id": key, "entry": entry, "input": file_record(path),
                "primary_family": route["primary_family"] if route else families[0] if len(families) == 1 else "UNMAPPED",
                "reviewed_anchor_families": families, "execution": "NOT_RUN",
                "explicit_reviewed_route": route}

    regression = []
    for path in reg.collect_cases():
        row = case_base(path, "test" if reg.is_test_module(path) else "run")
        row.update(expected_exit=0, stdout_sidecar=file_record(Path(str(path) + ".expected")),
                   stdout_enforced_by_original_runner=False,
                   independent_oracle="SOURCE_ASSERTIONS_REQUIRE_SEMANTIC_REVIEW")
        regression.append(row)
    selected_reg = {r["case_id"] for r in regression}
    auxiliary = [file_record(p) for p in sorted(reg.TEST_DIR.rglob("*.xr")) if relative(p) not in selected_reg]
    differential = []
    for path in diff.collect_cases("", str(diff.SCRIPT_DIR / "coro_regression_cases.txt")):
        row = case_base(path, "run")
        expected = diff.read_expected_stdout(path)
        row.update(stdout_hex=None if expected is None else expected.hex(),
                   independent_stdout_oracle=expected is not None,
                   arguments=diff.read_args(path), stdin_hex=diff.read_stdin(path).hex(),
                   original_regression_entry=next((r["entry"] for r in regression if r["case_id"] == relative(path)), None))
        differential.append(row)
    negative = []
    for path in errors.collect_cases():
        row = case_base(path, "implicit_compile")
        expected_path, kind = errors.expected_path_for(path)
        row.update(expectation=file_record(expected_path), expectation_kind=kind)
        try:
            assertions = errors.parse_expected(expected_path)
            row.update(assertion_count=len(assertions),
                       diagnostic_position_gap=any(a.misplaced or a.where.degenerate for a in assertions),
                       expectation_parse="PARSED")
        except (errors.ExpectedFormatError, OSError) as exc:
            row.update(expectation_parse="FAILED", expectation_error=str(exc).replace(str(ROOT), "<repository>"))
        negative.append(row)
    actual_entries = collections.defaultdict(list)
    for suite, rows in (("regression", regression), ("differential", differential), ("compile_errors", negative)):
        for row in rows:
            actual_entries[row["case_id"]].append({"suite": suite, "entry": row["entry"]})
    runner_routing_counts = validate_runner_routes(reviewed_runner_routes, responsibilities, actual_entries)
    routing = read_json(HERE / "consumer_routes.json")
    routing_counts = validate_routes(routing, legacy, responsibilities)
    source_routes = {row["case_id"]: row for row in routing["source_cases"]}
    sources = []
    for row in legacy["source"]:
        sources.append({"case_id": row["name"], "legacy_path": row["legacy_path"],
                        "body_lf_sha256": row["body_lf_sha256"], "original_fixture": row["original_fixture"],
                        "projection": row.get("initial_projection"),
                        "candidate_gates": row.get("candidate_replacement_gates", []),
                        "responsibility_detail": "tests/unit/xir/product_consumers/legacy_responsibilities.json#" + row["name"],
                        "execution": "NOT_RUN", "status": "OPEN",
                        "primary_family": source_routes[row["name"]]["primary_family"],
                        "producer_packages": source_routes[row["name"]]["producer_packages"],
                        "tasks": source_routes[row["name"]]["tasks"],
                        "new_entry_status": source_routes[row["name"]]["new_entry_status"]})
    extra = []
    for path in sorted((ROOT / "tests/unit/execution").glob("*.c")):
        extra.append({"input": file_record(path), "tasks": ["315", "316", "318", "322"],
                      "packages": ["P07", "P08", "P09"], "assertion_mapping": "OPEN",
                      "qualification_owner": "P20/P21; read-only for this preparation"})
    baseline = ratchet.read_baseline(ratchet.BASELINE)
    counts = {"registered_corpus_passing_test_baseline": len(baseline), "regression_entries": len(regression), "regression_test": sum(r["entry"] == "test" for r in regression),
              "regression_run": sum(r["entry"] == "run" for r in regression), "regression_auxiliary": len(auxiliary),
              "regression_stdout_sidecars": sum(r["stdout_sidecar"]["state"] == "PRESENT" for r in regression),
              "diff_entries": len(differential), "diff_independent_stdout": sum(r["independent_stdout_oracle"] for r in differential),
              "diff_regression_entry_mismatches": sum(r["original_regression_entry"] == "test" for r in differential),
              "compile_errors": len(negative), "expectation_parse_failures": sum(r["expectation_parse"] == "FAILED" for r in negative),
              "diagnostic_position_gaps": sum(bool(r.get("diagnostic_position_gap")) for r in negative),
              "source_responsibilities": len(sources), "source_fixtures": legacy["source_native_fixtures"],
              "source_projections": sum(r["projection"] is not None for r in sources),
              "other_consumer_files": len(legacy["other_consumer_files"]),
              "other_consumer_functions": sum(len(r["functions"]) for r in legacy["other_consumer_files"].values()),
              "execution_files": len(extra), "allocation_scenarios": legacy["source_allocation_scenarios"]["count"],
              "runner_rows_without_reviewed_primary_family": sum(r["primary_family"] == "UNMAPPED" for r in regression + differential + negative)}
    counts.update(routing_counts)
    counts.update(runner_routing_counts)
    counts["prepared_independent_oracles"] = validate_oracles(read_json(HERE / "oracles.json"))["oracle_cases"]
    return {"scope": "PREPARATION_ONLY_NO_PRODUCT_QUALIFICATION", "summary": counts,
            "source_identity": {"seed": "e16797c62636f1d138583ead18d234c86a3cde6e",
                                "head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                                "working_tree": subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=all"], cwd=ROOT, text=True).splitlines(),
                                "baseline_acceptance": "See fixed delivery receipt; preparation checks do not qualify dependencies"},
            "regression": regression, "regression_auxiliary": auxiliary,
            "differential": differential, "compile_errors": negative,
            "source_responsibilities": sources, "other_consumers": legacy["other_consumer_files"],
            "source_allocation_scenarios": legacy["source_allocation_scenarios"],
            "execution_responsibilities": extra, "other_consumer_routes": routing["other_files"],
            "registered_corpus": {"gate": "regression_corpus", "runner": "scripts/check_regression_corpus.py",
                                  "entry": "xray test tests/regression --jobs <registered_jobs> --quiet --report <report>",
                                  "runner_default_timeout_seconds": 900, "ctest_timeout_expression": "600 * XR_TEST_TIMEOUT_SCALE",
                                  "registered_jobs": "min(4, XRAY_HOST_PROCESSOR_COUNT)", "runtime_discovery": "NOT_RUN",
                                  "passing_test_baseline": baseline, "status": "NOT_RUN"},
            "original_baselines": [file_record(ROOT / p) for p in ["tests/regression/baseline_failures.txt", "tests/regression/nondeterministic_cases.txt", "tests/regression/xir_passing.txt"]],
            "discovery_inputs": [file_record(ROOT / p) for p in ["CMakeLists.txt", "scripts/check_regression_corpus.py", "scripts/run_regression_tests.py", "tests/diff/run_backend_diff.py", "tests/diff/coro_regression_cases.txt", "tests/compile_errors/run_compile_error_tests.py", "tests/unit/xir/product_consumers/legacy_responsibilities.json"]],
            "original_anchor_presence": [file_record(ROOT / p) for p in sorted(anchors)],
            "preparation_inputs": [file_record(p) for p in sorted(HERE.rglob("*")) if p.is_file() and "__pycache__" not in p.parts],
            "note": "Runner entries overlap; counts are not a language completion percentage. Unmapped rows and unchanged old failures remain OPEN."}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("snapshot", "check"))
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    validate_responsibilities(read_json(HERE / "responsibilities.json"))
    result = validate_oracles(read_json(HERE / "oracles.json"))
    result.update(validate_routes(read_json(HERE / "consumer_routes.json"),
                                  read_json(ROOT / "tests/unit/xir/product_consumers/legacy_responsibilities.json"),
                                  read_json(HERE / "responsibilities.json")))
    if args.action == "snapshot":
        result = snapshot()
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
        print(json.dumps(result["summary"], sort_keys=True))
    else:
        result.update(validate_runner_routes(read_json(HERE / "runner_routes.json"),
                                             read_json(HERE / "responsibilities.json")))
        print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    main()

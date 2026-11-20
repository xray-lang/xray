"""Execute authentic Array sources through the VM and ordinary C projections."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from xraytest.artifacts import complete_native_artifacts


def assert_fixed_update_loop(name: str, generated: str) -> None:
    loop = re.search(
        r"^(?P<label>xrl_fixed_(?P<region>\d+)_bb\d+): ;\n"
        r"    for \(; (?P<iv>v\d+) < \(int64_t\)xrl_fixed_cache_(?P=region)\.length; \+\+(?P=iv)\) \{\n",
        generated, re.MULTILINE,
    )
    assert loop, (name, "fixed Array loop missing")
    label, region, iv = loop.group("label", "region", "iv")
    cache = f"xrl_fixed_cache_{region}"
    guard = re.search(
        rf"\b{iv} >= INT64_C\(0\) && xrt_array_i64_region_try_begin\([^\n;]+, true, &{cache}\)"
        rf" && \(uint64_t\){iv} < {cache}\.length\) \{{[^{{}}]*\bgoto {label};",
        generated[:loop.start()],
    )
    assert guard, (name, "fixed Array loop has no matching entry guard")

    # The numeric loop may contain nested blocks for scalar edge copies.
    depth = 1
    tail = generated[loop.end():]
    for brace in re.finditer(r"[{}]", tail):
        depth += 1 if brace[0] == "{" else -1
        if depth == 0:
            break
    else:
        raise AssertionError((name, "fixed Array loop has no closing brace"))
    body = tail[:brace.start()]

    # Match the two access directions on the proven backing rather than
    # counting unrelated scalar bit copies or ordinary fallback calls.
    element = rf"{cache}\.elements\s*\+\s*[^,\n]+"
    assert re.search(rf"\bXRT_ARRAY_I64_REGION_COPY\(&v\d+,\s*{element}\);", body), (name, "fixed Array read missing")
    assert re.search(rf"\bXRT_ARRAY_I64_REGION_COPY\({element},\s*&v\d+\);", body), (name, "fixed Array write missing")
    assert not re.search(r"\b(?:xrt_\w+|xrl_edge_\w+)\s*\(", body), (name, "runtime work in fixed Array loop")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", action="append", default=[])
    parser.add_argument("--case", action="append", default=[])
    args = parser.parse_args()
    cases = ("callable_values", "callable_contracts", "callable_frames", "callable_owned", "callable_compose", "callable_compose_weaken",
             "callable_inline_array_parameter", "callable_inline_array_result",
             "factory_bindings", "factory_loop", "factory_transfer_temporaries",
             "module_double", "module_snapshot", "module_counter", "counter_shared_binding",
             "tuple_values", "tuple_flow", "tuple_mixed_fields", "tuple_mixed_owned", "tuple_array_state", "tuple_array_return",
             "scalar_ref_call", "scalar_ref_array_callable",
             "map_values", "map_bindings", "map_pod_calls", "map_fixed_operations", "record_values", "record_nullable", "record_lifetimes", "record_multileaf", "record_nullable_pod", "record_state_first_read", "record_generic_branch", "record_generic_owned_branch", "nullable_managed",
             "panic_map_index_missing", "panic_map_direct_missing", "panic_map_formal_missing", "panic_map_stationary_cleanup", "panic_nullable_managed", "callable_map_capture", "callable_map_value", "callable_unit_owned", "reject_owned_binding",
             "range_fill", "panic_range_negative", "panic_range_reversed", "panic_range_end",
             "snapshots", "values", "strings", "imports", "conditional", "large", "large_string",
             "local_loop", "loop_temporary", "nested_lifetimes", "loop_lifetimes", "loop_carry",
             "region_zero", "region_one", "region_set", "region_two", "region_scalar_phi", "push_capacity", "array_stationary_edges",
             "array_stationary_cells", "panic_stationary_cell", "array_held_edits", "call_result_homes",
             "array_call_staging", "panic_array_call_staging",
             "element_update", "element_update_carried", "fixed_update_carried",
             "panic_region_get", "panic_region_set",
             "ref_borrow", "ref_cfg", "ref_mixed", "ref_global_alias", "repeat_values", "repeat_strings", "panic_repeat_negative",
             "panic_repeat_string_negative", "unsigned_values", "unsigned_large_byte", "unsigned_large_word",
             "panic_bounds", "panic_get_i64_negative", "panic_get_u8_length", "panic_get_u64_huge", "panic_get_inline_length",
             "panic_unpublished", "panic_len_unpublished_inline", "fixed_values", "fixed_flow", "fixed_length_effects",
             "fixed_large", "fixed_unused", "panic_fixed_bounds", "panic_fixed_negative", "panic_fixed_unpublished_length",
             "nullable_scalar", "i64_try_parse", "nullable_nested", "nullable_fixed", "nullable_unit", "nullable_lazy",
             "nullable_default", "nullable_imports", "panic_nullable_unwrap", "panic_nullable_inner", "panic_nullable_unit",
             "display_nullable", "display_nullable_values", "display_nullable_array", "display_values", "display_strings", "display_imports", "display_loop", "display_tuples", "panic_display_unpublished",
             "view_local", "view_late_callable", "view_scalar_callable_write", "view_scalar_branch_cells", "view_state_callback_read", "view_callback_relay", "view_callback_scalar_context", "view_call", "view_return", "view_loop", "view_copy", "view_module_alias",
             "view_module_const", "view_conditional_cleanup", "view_phi", "view_phi_mixed", "view_copy_argument",
             "view_return_module_unrelated", "view_recursive_module_unrelated", "view_multiple_module_unrelated",
             "view_copy_unused_parameter", "view_call_module_write", "panic_nullable_view", "string_append_values", "string_append_ref",
             "string_join", "string_append_loop", "string_append_assignment", "string_append_grouping",
             "string_append_invariant", "string_parameter_loop", "panic_nullable_string_append", "map_snapshots", "map_display",
             "map_stationary_reads", "map_mixed_scalar_cycle", "map_local_forwarding", "map_edit_lifetime",
             "map_key_mutation", "map_borrowed_literal_key",
             "set_values")
    unknown = sorted(set(args.case) - set(cases))
    if unknown:
        parser.error("unknown cases: " + ", ".join(unknown))
    root, binary = args.root.resolve(), args.binary.resolve()
    sys.path.insert(0, str(root / "scripts"))
    from xray_process import run_child

    args.output.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="array-native-", dir=args.output.resolve()))
    fixtures = root / "tests/fixtures/lir_cli/array_lower"
    environment = dict(os.environ, XRAY_STDLIB_PATH=str(root / "stdlib"), NO_COLOR="1")
    if os.name == "nt":
        environment["PATH"] = str(binary.parent) + os.pathsep + environment.get("PATH", "")
    journal = work / "result.jsonl"
    records = []
    failures = []

    def run(name, command, expected_exit):
        command = list(map(str, command))
        paths = {stream: work / (name + "." + stream) for stream in ("stdout", "stderr")}
        with paths["stdout"].open("xb") as out, paths["stderr"].open("xb") as err:
            status = run_child(command, cwd=work, env=environment, stdin=subprocess.DEVNULL,
                               stdout=out, stderr=err, timeout=30)
        row = {"name": name, "argv": command, "exit": status, "expected_exit": expected_exit,
               "state": "TERMINAL_DRAINED"}
        for stream, path in paths.items():
            row[stream] = {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
        # Keep the completed command available even if the suite times out.
        with journal.open("a", encoding="utf-8", newline="\n") as output:
            output.write(json.dumps(row, separators=(",", ":")) + "\n")
        records.append(row)
        if status != expected_exit:
            failures.append((name, status, paths["stderr"].read_text(encoding="utf-8", errors="replace")))
        return status, paths["stdout"].read_bytes(), paths["stderr"].read_bytes()

    for name in cases:
        if args.case and name not in args.case:
            continue
        if name in ("ref_global_alias", "string_append_ref"):
            continue
        source = fixtures / (name + ".xr")
        panic = name.startswith("panic_")
        status, _, _ = run(name + "-check", [binary, "check", source], 0)
        if status:
            continue
        reference = None
        for repeat in range(2):
            result = run(name + f"-vm-{repeat}", [binary, "run", source], 1 if panic else 0)
            _, out, err = result
            if panic:
                diagnostic = b"[Uncaught Panic] E0431:" if name.startswith("panic_map_") else b"[Uncaught Panic] E0413:" if name.startswith("panic_nullable_") else b"XR_RUN_6002: value operation rejected"
                if out or diagnostic not in err or (not name.startswith("panic_map_") and source.name.encode() not in err):
                    failures.append((name, "VM runtime diagnostic", out, err))
            elif out != source.with_suffix(".out").read_bytes() or err:
                failures.append((name, "VM output", out, err))
            if reference is not None and reference != result:
                failures.append((name, "VM repeat", reference, result))
            reference = result
        generated = work / (name + ".c")
        status, _, _ = run(name + "-c", [binary, "build", "-c", source, "-o", generated], 0)
        if status:
            continue
        generated_text = generated.read_text(encoding="utf-8")
        assert "({" not in generated_text
        if name == "push_capacity":
            # The dependent bound and live snapshot must keep ordinary updates.
            assert generated_text.count("static const XrtArrayI64RegionSpec xrl_ap_spec_") == 4
            assert "xrl_ap_0_ready = true;" in generated_text
            assert "xrt_native_array_update_place(" in generated_text
            assert len(re.findall(r"if \(xrl_ap_\d+_ready\) \{\s+xrt_native_array_push_local_i64\(",
                                  generated_text)) == 4
        elif name == "array_stationary_edges":
            # A changing bound keeps pointwise updates. Its selected edges can
            # be scalar-only while the later read window keeps real owner moves.
            assert "xrl_ap_spec_" not in generated_text
            bodies = re.findall(r"^static int64_t xrl_f\d+\([^\n]*\) \{\n(.*?)^}",
                                generated_text, re.MULTILINE | re.DOTALL)
            pointwise, = (body for body in bodies if "xrt_array_i64_update_held(" in body)
            append, read = pointwise.split("xrl_ar_0_cache = (", 1)
            assert ("xrt_native_array_push_local_i64(" in append or
                    "xrt_native_array_update_place(" in append) and "xrl_edge_" not in append
            assert "xrl_ar_loop_" in read and "xrl_edge_" in read
        elif name == "array_stationary_cells":
            # Each mutation Cell shares its stationary owner's slot: no runtime
            # forwarding remains, and every slow update edits a local view's binding.
            assert "xrt_managed_frame_try_forward(" not in generated_text
            bindings = set(re.findall(r"\(XrtArrayI64LocalGeometry\)\{(\d+), ", generated_text))
            updates = set(re.findall(r"xrt_native_array_update_place\(\(XrtPlace\)\{\.frame=xrl_frame, \.slot=(\d+),",
                                     generated_text))
            assert updates and updates <= bindings, (updates, bindings)
        elif name == "array_held_edits":
            # Held acquisitions never survive an ordinary call: the last held
            # reference before each direct call is a clear, not a use.
            assert "xrt_array_i64_update_held(" in generated_text
            bodies = re.findall(r"^static [^\n]*\bxrl_f\d+\([^\n]*\) \{\n(.*?)^}", generated_text,
                                re.MULTILINE | re.DOTALL)
            calls = 0
            for body in (body for body in bodies if "XrtArrayBacking *xrl_held" in body):
                for call in re.finditer(r"\bxrl_f\d+\(", body):
                    last = list(re.finditer(r"xrl_held\d+( = NULL;|,)", body[:call.start()]))
                    assert last and last[-1][1] == " = NULL;", name
                    calls += 1
            assert calls, name
        elif name == "call_result_homes":
            # Each reader keeps the direct call's owning result in its landing
            # home across the loop, including when another binding shares it.
            bodies = re.findall(r"^static int64_t xrl_f\d+\([^\n]*\) \{\n(.*?)^}",
                                generated_text, re.MULTILINE | re.DOTALL)
            readers = [body for body in bodies if "xrt_native_array_get(" in body]
            assert len(readers) == 2, name
            for body in readers:
                assert re.search(r"xrl_f\d+\(\(XrlLanding_\d+\)", body), name
                assert "xrl_edge_" not in body and "xrt_place_edge_transfer(" not in body, name
        elif name in ("array_call_staging", "panic_array_call_staging"):
            assert "xrt_managed_slot_try_stage_unique(xrl_frame," in generated_text, name
            assert "xrt_managed_slot_move(xrl_frame," in generated_text and "xrl_array_argument.frame" in generated_text, name
        elif name in ("element_update", "element_update_carried", "fixed_update_carried"):
            assert_fixed_update_loop(name, generated_text)
        elif "xrt_native_array_push_local_i64(" in generated_text:
            # Growth in place follows only a capacity window or a failed held
            # local edit, whose acquisitions were cleared before the call.
            for match in re.finditer(r"xrt_native_array_push_local_i64\(", generated_text):
                prefix = generated_text[max(0, match.start() - 600):match.start()]
                assert (re.search(r"if \(xrl_ap_\d+_ready\) \{\s+$", prefix) or
                        re.search(r"xrt_array_i64_update_held\([^;]*\{\n(?:.*\n)*?\s+xrl_held\d+ = NULL;", prefix)), name
        if name.startswith("region_") or name.startswith("panic_region_"):
            specs = re.findall(r"static const XrtArrayI64RegionSpec xrl_ar_spec_", generated_text)
            clones = re.findall(r"^xrl_ar_\d+_bb\d+: ;\n(.*?)(?=^xrl_ar_\d+_bb\d+: ;|^}\n)",
                                generated_text, re.MULTILINE | re.DOTALL)
            assert len(specs) == (2 if name == "region_two" else 1), (name, len(specs))
            assert clones and all("xrt_" not in body for body in clones), name
            if name == "region_set":
                assert "XRT_ARRAY_I64_REGION_COPY(" not in generated_text
                assert "XRT_ARRAY_I64_REGION_RECONSTRUCT(" not in generated_text
        if name == "string_append_assignment":
            assert generated_text.count("xrt_native_string_append_owned(") == 4
            assert generated_text.count("xrt_native_string_concat_create(") == 1
        if name == "string_append_grouping":
            assert generated_text.count("xrt_native_string_append_owned(") == 5
            assert generated_text.count("xrt_native_string_append_window(") == 3
            assert generated_text.count("xrt_native_string_concat_create(") == 2
        if name == "string_append_invariant":
            assert generated_text.count("xrt_native_string_append_window(") == 9
            assert generated_text.count("xrt_native_string_append_owned(") == 12
        if name in ("map_local_forwarding", "panic_map_stationary_cleanup", "map_edit_lifetime"):
            assert "xrt_managed_frame_try_forward(" not in generated_text
            assert "xrt_place_copy_replace(" in generated_text
            assert "xrt_place_edge_transfer(owners," not in generated_text
            updates = re.findall(r"xrt_native_map_update_place\(\(XrtPlace\)\{\.frame=xrl_frame, \.slot=(\d+),",
                                 generated_text)
            assert len(updates) == (2 if name == "map_edit_lifetime" else 1) and len(set(updates)) == 1
        if name == "map_stationary_reads":
            readers = {function for function, body in re.findall(
                r"^static int64_t xrl_f(\d+)\([^\n]*\) \{\n(.*?)^}", generated_text, re.MULTILINE | re.DOTALL
            ) if "xrt_native_map_index(" in body}
            moving = {function for function, body in re.findall(
                r"^static XrtManagedStatus xrl_edge_(\d+)_\d+_\d+\([^\n]*\) \{\n(.*?)^}",
                generated_text, re.MULTILINE | re.DOTALL
            ) if "xrt_place_edge_transfer(owners," in body}
            assert len(readers) == 4 and len(readers & moving) == 2
        # The original large-case oracles check both length and element 299.
        # Keep that executed span check independent of C literal spelling.
        for ordinal, compiler in enumerate(args.compiler or [None]):
            executable = work / (name + f"-{ordinal}" + (".exe" if os.name == "nt" else ""))
            command = [binary, "build", source, "-o", executable]
            if compiler:
                command += ["--cc", compiler]
            status, _, _ = run(name + f"-build-{ordinal}", command, 0)
            if status:
                continue
            for repeat in range(2):
                result = run(name + f"-native-{ordinal}-{repeat}", [executable], 1 if panic else 0)
                _, out, err = result
                if panic:
                    diagnostic = b"[Uncaught Panic] E0431:" if name.startswith("panic_map_") else b"[Uncaught Panic] E0413:" if name.startswith("panic_nullable_") else b"XR_RUN_6002: value operation rejected"
                    if out or diagnostic not in err or (not name.startswith("panic_map_") and source.name.encode() not in err):
                        failures.append((name, "runtime diagnostic", out, err))
                elif out != source.with_suffix(".out").read_bytes() or err:
                    failures.append((name, "output", out, err))
                if result != reference:
                    failures.append((name, "VM/native byte comparison", reference, result))
    for name in ("reject_bool", "reject_nested", "reject_function_reassign",
                 "reject_function_alias", "reject_function_ref", "reject_tuple_unit"):
        source = fixtures / (name + ".xr")
        run(name + "-check", [binary, "check", source], 0)
        _, out, err = run(name + "-c", [binary, "build", "-c", source, "-o", work / (name + ".c")], 1)
        diagnostic = (b"Tuple field has no canonical inline value carrier" if name == "reject_tuple_unit" else
                      b"owned mutable function binding extent" if name == "reject_function_reassign" else
                      b"FUNCTION_REF captured physical binding transport" if name == "reject_function_alias" else
                      b"COPY is not supported" if name == "reject_function_ref" else
                      b"module slot payload type is not supported")
        if out or diagnostic not in err:
            failures.append((name, "unsupported payload", out, err))
        _, vm_out, vm_err = run(name + "-vm", [binary, "run", source], 1)
        if vm_out != out or vm_err != err:
            failures.append((name, "VM/native refusal byte comparison", (out, err), (vm_out, vm_err)))
    # This paired State control tests semantic admission. The indirect higher-
    # order callable ABI still has its separate Lower implementation frontier.
    for name in ("check_view_state_indirect_callback_read", "check_view_callback_declaration", "check_view_callback_derived"):
        status, out, err = run(name + "-check", [binary, "check", fixtures / (name + ".xr")], 0)
        if status or out or err:
            failures.append((name, "Source callback borrowing control", out, err))
    rejected_sources = {
        "reject_i64_try_parse_type": b"expression cannot satisfy its declared type",
        "reject_i64_try_parse_shadow": b"'i64' is a keyword and cannot be used as an identifier",
        "reject_callable_inline_array_mismatch": b"callback argument requires exact shape and an admitted advertised bound",
        "ref_global_alias": b"E0382: ref of module binding `shared` conflicts with access by `update` via `inspect`",
        "string_append_ref": b"E0382: ref of module binding `published` conflicts with access by `actual` via `actual`",
        "reject_display_nullable_fixed": b"value has no admitted builtin display",
        "reject_fixed_empty": b"this fixed byte length",
        "reject_fixed_length_mismatch": b"expression cannot satisfy its declared type",
        "reject_repeat_string_default": b"type has no admitted default initializer",
        "reject_unsigned_family": b"expression cannot satisfy its declared type",
        "reject_unsigned_byte_literal": b"integer literal is outside its contextual type",
        "reject_ref_alias": b"ref arguments of one call must not alias",
        "reject_scalar_ref_callback_write": b"E0382: ref of module binding `shared` conflicts with",
        "reject_local_scalar_ref_callback_write": b"ref argument conflicts with a callable capture of the same mutable root",
        "reject_view_live_write": b"during view lifetime normalization",
        "reject_view_copy_live_write": b"during view lifetime normalization",
        "reject_view_module_alias": b"during view lifetime normalization",
        "reject_view_module_write": b"E0382: Slice of module binding `values` conflicts with write by `first` via `grow`",
        "reject_view_capture_module_write": b"during view lifetime normalization",
        # These are capability boundaries, not proof of captured local Array
        # write conflict checking. Their original inputs remain unchanged.
        "reject_view_indirect_callback_write": b"borrowing captured Array storage is not implemented yet",
        "reject_view_local_byte_capture": b"borrowing captured Array storage is not implemented yet",
        "reject_callable_local_array_transport": b"callable transport of captured Array storage is not implemented yet",
        "reject_callable_local_byte_transport": b"callable transport of captured Array storage is not implemented yet",
        "reject_callable_unknown_before_array_transport": b"constructed XIR failed checking",
        "reject_view_state_indirect_callback_write": b"E0382: Slice of module binding `values` conflicts with write",
        "reject_view_callback_relay_write": b"E0382: Slice of module binding `values` conflicts with write",
        "reject_view_callback_opaque": b"callback arguments require a completed readonly Slice borrowing contract",
        "reject_view_local_return": b"constructed XIR failed checking",
    }
    for name, diagnostic in rejected_sources.items():
        source = fixtures / (name + ".xr")
        _, out, err = run(name + "-check", [binary, "check", source], 1)
        if out or diagnostic not in err:
            failures.append((name, "Source diagnostic", out, err))
        for stage, command in (
            ("run", [binary, "run", source]),
            ("build-c", [binary, "build", "-c", source, "-o", work / (name + ".c")]),
            ("build", [binary, "build", source, "-o", work / name]),
        ):
            _, actual_out, actual_err = run(name + "-" + stage, command, 1)
            if actual_out != out or actual_err != err:
                failures.append((name, "Source refusal byte comparison", stage, (out, err), (actual_out, actual_err)))
    report = work / "result.json.tmp"
    report.write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")
    report.replace(work / "result.json")
    assert not failures, failures
    complete_native_artifacts(work)
    print(f"{len(records)} Array Source/VM/native commands passed")


if __name__ == "__main__":
    main()

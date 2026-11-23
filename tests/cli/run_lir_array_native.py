"""Execute authentic Array sources through the VM and ordinary C projections."""
from __future__ import annotations

import argparse
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import threading


class CaseCancelled(Exception):
    pass


class SpawnGate:
    def __init__(self):
        self._lock = threading.Lock()
        self._stopped = False

    def stopped(self):
        with self._lock:
            return self._stopped

    def stop(self):
        with self._lock:
            self._stopped = True

    @contextmanager
    def launch(self):
        with self._lock:
            if self._stopped:
                raise CaseCancelled()
            yield


def run_ordered(cases, execute, jobs, gate, recovery_required):
    pending = {}
    queued = iter(enumerate(cases))
    outcomes = {}
    fatal = []

    def guarded(index, case):
        try:
            if gate.stopped():
                raise CaseCancelled()
            return execute(index, case)
        except CaseCancelled:
            raise
        except BaseException:
            gate.stop()
            raise

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        def queue_one():
            if not gate.stopped() and (entry := next(queued, None)) is not None:
                index, case = entry
                pending[pool.submit(guarded, index, case)] = index

        try:
            for _ in range(jobs):
                queue_one()
            while pending:
                done, _ = wait(pending, return_when=FIRST_COMPLETED)
                for future in sorted(done, key=pending.__getitem__):
                    index = pending.pop(future)
                    try:
                        outcomes[index] = future.result()
                    except CaseCancelled:
                        pass
                    except BaseException as error:
                        fatal.append((index, error))
                    queue_one()
        except BaseException:
            gate.stop()
            for future in pending:
                future.cancel()
            raise

    if fatal:
        # Never lose unverified child drain behind an ordinary assertion.
        _, error = min(fatal, key=lambda failure: (
            not isinstance(failure[1], recovery_required), failure[0]))
        raise error
    return [outcomes[index] for index in range(len(cases))]


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
             "snapshots", "values", "strings", "imports", "conditional", "class_constructor_array_field",
             "byte_copy_before_write", "large", "large_string",
             "local_loop", "loop_temporary", "nested_lifetimes", "loop_lifetimes", "loop_carry",
             "region_zero", "region_one", "region_set", "region_two", "region_scalar_phi", "push_capacity", "array_stationary_edges",
             "element_update", "element_update_carried",
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
    sys.path.insert(0, str(root / "tests/lib"))
    from xraytest.artifacts import complete_native_artifacts
    sys.path.insert(0, str(root / "scripts"))
    from xray_process import RecoveryRequired, run_child

    args.output.mkdir(parents=True, exist_ok=True)
    attempt = Path(tempfile.mkdtemp(prefix="array-native-", dir=args.output.resolve()))
    fixtures = root / "tests/fixtures/lir_cli/array_lower"
    environment = dict(os.environ, XRAY_STDLIB_PATH=str(root / "stdlib"), NO_COLOR="1")
    if os.name == "nt":
        environment["PATH"] = str(binary.parent) + os.pathsep + environment.get("PATH", "")
    rejected_sources = {
        "reject_i64_try_parse_type": b"expression cannot satisfy its declared type",
        "reject_i64_try_parse_shadow": b"'i64' is a keyword and cannot be used as an identifier",
        "reject_callable_inline_array_mismatch": b"callback argument requires exact shape and an admitted advertised bound",
        "ref_global_alias": b"E0382: ref of module binding `shared` conflicts with access by `update` via `inspect`",
        "string_append_ref": b"E0382: ref of module binding `published` conflicts with access by `actual` via `actual`",
        "reject_display_nullable_fixed": b"value has no admitted builtin display",
        "reject_fixed_empty": b"this fixed byte length",
        "reject_fixed_length_mismatch": b"expression cannot satisfy its declared type",
        "reject_class_constructor_array_uninitialized": b"read requires storage initialized on every incoming path",
        "reject_class_constructor_array_const": b"field access is not permitted",
        "reject_repeat_string_default": b"type has no admitted default initializer",
        "reject_unsigned_family": b"expression cannot satisfy its declared type",
        "reject_unsigned_byte_literal": b"integer literal is outside its contextual type",
        "reject_ref_alias": b"ref arguments of one call must not alias",
        "reject_scalar_ref_callback_write": b"E0382: ref of module binding `shared` conflicts with",
        "reject_local_scalar_ref_callback_write": b"ref argument conflicts with a callable capture of the same mutable root",
        "reject_view_live_write": b"during view lifetime normalization",
        "reject_byte_slice_live_write": b"during view lifetime normalization",
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

    lower_names = ("reject_bool", "reject_nested", "reject_function_reassign",
                 "reject_function_alias", "reject_function_ref", "reject_tuple_unit")
    callback_names = ("check_view_state_indirect_callback_read", "check_view_callback_declaration", "check_view_callback_derived")
    cases = ([("ordinary", name, None) for name in cases
              if (not args.case or name in args.case) and name not in ("ref_global_alias", "string_append_ref")]
             + [("lower", name, None) for name in lower_names]
             + [("callback", name, None) for name in callback_names]
             + [("rejected", name, diagnostic) for name, diagnostic in rejected_sources.items()])
    gate = SpawnGate()

    def run_ordinary(name, work, run, failures):
        source = fixtures / (name + ".xr")
        panic = name.startswith("panic_")
        status, _, _ = run(name + "-check", [binary, "check", source], 0)
        if status:
            return
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
            return
        generated_text = generated.read_text(encoding="utf-8")
        assert "({" not in generated_text
        if name == "push_capacity":
            # The dependent bound and live snapshot must keep ordinary updates.
            assert generated_text.count("static const XrtArrayI64RegionSpec xrl_ap_spec_") == 4
            assert "xrl_ap_0_ready = true;" in generated_text
            assert "xrt_native_array_update_place(" in generated_text
            assert generated_text.count("xrt_native_array_push_local_i64(") == 4
        elif name == "array_stationary_edges":
            # A changing bound keeps pointwise updates. Its selected edges can
            # be scalar-only while the later read window keeps real owner moves.
            assert "xrl_ap_spec_" not in generated_text
            bodies = re.findall(r"^static int64_t xrl_f\d+\([^\n]*\) \{\n(.*?)^}",
                                generated_text, re.MULTILINE | re.DOTALL)
            pointwise, = (body for body in bodies if "xrt_array_i64_try_update_local(" in body)
            append, read = pointwise.split("xrl_ar_0_cache = (", 1)
            assert "xrt_native_array_update_place(" in append and "xrl_edge_" not in append
            assert "xrl_ar_loop_" in read and "xrl_edge_" in read
        elif "xrt_native_array_push_local_i64(" in generated_text:
            assert re.search(r"if \(xrl_ap_\d+_ready\) \{\s+xrt_native_array_push_local_i64\(", generated_text)
        if name.startswith("region_") or name.startswith("panic_region_"):
            specs = re.findall(r"static const XrtArrayI64RegionSpec xrl_ar_spec_", generated_text)
            clones = re.findall(r"^xrl_ar_\d+_bb\d+: ;\n(.*?)(?=^xrl_ar_\d+_bb\d+: ;|^}\n)",
                                generated_text, re.MULTILINE | re.DOTALL)
            assert len(specs) == (2 if name == "region_two" else 1), (name, len(specs))
            assert clones and all("xrt_" not in body for body in clones), name
            if name == "region_set":
                assert "XRT_ARRAY_I64_REGION_COPY(" not in generated_text
                assert "XRT_ARRAY_I64_REGION_RECONSTRUCT(" not in generated_text
        if name in ("element_update", "element_update_carried"):
            assert "XRT_ARRAY_I64_REGION_RECONSTRUCT(" in generated_text
            assert ("xrt_array_i64_pair_store(" in generated_text) == (name == "element_update"), name
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

    def run_lower_refusal(name, work, run, failures):
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

    def run_callback_check(name, run, failures):
        status, out, err = run(name + "-check", [binary, "check", fixtures / (name + ".xr")], 0)
        if status or out or err:
            failures.append((name, "Source callback borrowing control", out, err))

    def run_source_refusal(name, diagnostic, work, run, failures):
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

    def execute(index, case):
        kind, name, diagnostic = case
        work = attempt / f"{index:03d}"
        work.mkdir()
        temporary = work / "t"
        temporary.mkdir()
        case_environment = environment | dict.fromkeys(("TEMP", "TMP", "TMPDIR"), str(temporary))
        records = []
        failures = []
        journal = work / "result.jsonl"

        def run(name, command, expected_exit):
            command = list(map(str, command))
            paths = {stream: work / (name + "." + stream) for stream in ("stdout", "stderr")}
            with paths["stdout"].open("xb") as out, paths["stderr"].open("xb") as err:
                status = run_child(command, cwd=work, env=case_environment, stdin=subprocess.DEVNULL,
                                   stdout=out, stderr=err, timeout=30, spawn_guard=gate.launch)
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

        if kind == "ordinary":
            run_ordinary(name, work, run, failures)
        elif kind == "lower":
            run_lower_refusal(name, work, run, failures)
        elif kind == "callback":
            run_callback_check(name, run, failures)
        elif kind == "rejected":
            run_source_refusal(name, diagnostic, work, run, failures)
        else:
            raise AssertionError(f"invalid case class: {kind}")
        return records, failures

    outputs = run_ordered(cases, execute, 2, gate, RecoveryRequired)
    records = [record for case_records, _ in outputs for record in case_records]
    failures = [failure for _, case_failures in outputs for failure in case_failures]
    report = attempt / "result.json.tmp"
    report.write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")
    report.replace(attempt / "result.json")
    assert not failures, failures
    complete_native_artifacts(attempt)
    print(f"{len(records)} Array Source/VM/native commands passed")


if __name__ == "__main__":
    main()

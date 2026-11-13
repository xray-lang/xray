"""Execute authentic Array sources through the VM and ordinary C projections."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", action="append", default=[])
    parser.add_argument("--case", action="append", default=[])
    args = parser.parse_args()
    cases = ("callable_values", "snapshots", "values", "strings", "imports", "conditional", "large", "large_string",
             "local_loop", "loop_temporary", "nested_lifetimes", "loop_lifetimes", "loop_carry",
             "ref_borrow", "ref_cfg", "ref_mixed", "ref_global_alias", "repeat_values", "repeat_strings", "panic_repeat_negative",
             "panic_repeat_string_negative", "unsigned_values", "unsigned_large_byte", "unsigned_large_word",
             "panic_bounds", "panic_unpublished", "fixed_values", "fixed_flow", "fixed_length_effects",
             "fixed_large", "fixed_unused", "panic_fixed_bounds", "panic_fixed_negative", "panic_fixed_unpublished_length",
             "nullable_scalar", "nullable_nested", "nullable_fixed", "nullable_unit", "nullable_lazy",
             "nullable_default", "nullable_imports", "panic_nullable_unwrap", "panic_nullable_inner", "panic_nullable_unit",
             "display_nullable", "display_nullable_values", "display_nullable_array", "display_values", "display_strings", "display_imports", "display_loop", "panic_display_unpublished",
             "view_local", "view_call", "view_return", "view_loop", "view_copy", "view_module_alias",
             "view_module_const", "view_conditional_cleanup", "view_phi", "view_copy_argument",
             "view_copy_unused_parameter", "panic_nullable_view", "string_append_values", "string_append_ref",
             "string_join", "string_append_loop", "string_parameter_loop", "panic_nullable_string_append")
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
        records.append(row)
        (work / "result.json").write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")
        if status != expected_exit:
            failures.append((name, status, paths["stderr"].read_text(encoding="utf-8", errors="replace")))
        return status, paths["stdout"].read_bytes(), paths["stderr"].read_bytes()

    for name in cases:
        if args.case and name not in args.case:
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
                diagnostic = b"[Uncaught Panic] E0413:" if name.startswith("panic_nullable_") else b"XR_RUN_6002: value operation rejected"
                if out or diagnostic not in err or source.name.encode() not in err:
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
        assert "({" not in generated.read_text(encoding="utf-8")
        if name in ("large", "large_string"):
            assert "UINT32_C(300)" in generated.read_text(encoding="utf-8")
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
                    diagnostic = b"[Uncaught Panic] E0413:" if name.startswith("panic_nullable_") else b"XR_RUN_6002: value operation rejected"
                    if out or diagnostic not in err or source.name.encode() not in err:
                        failures.append((name, "runtime diagnostic", out, err))
                elif out != source.with_suffix(".out").read_bytes() or err:
                    failures.append((name, "output", out, err))
                if result != reference:
                    failures.append((name, "VM/native byte comparison", reference, result))
    for name in ("reject_bool", "reject_nested", "reject_nullable_managed"):
        source = fixtures / (name + ".xr")
        run(name + "-check", [binary, "check", source], 0)
        _, out, err = run(name + "-c", [binary, "build", "-c", source, "-o", work / (name + ".c")], 1)
        diagnostic = b"Nullable payload has no inline POD layout" if name == "reject_nullable_managed" else b"module slot payload type is not supported"
        if out or diagnostic not in err:
            failures.append((name, "unsupported payload", out, err))
        _, vm_out, vm_err = run(name + "-vm", [binary, "run", source], 1)
        if vm_out != out or vm_err != err:
            failures.append((name, "VM/native refusal byte comparison", (out, err), (vm_out, vm_err)))
    rejected_sources = {
        "reject_display_nullable_fixed": b"value has no admitted builtin display",
        "reject_fixed_empty": b"this fixed byte length",
        "reject_fixed_length_mismatch": b"expression cannot satisfy its declared type",
        "reject_repeat_string_default": b"type has no admitted default initializer",
        "reject_unsigned_family": b"expression cannot satisfy its declared type",
        "reject_unsigned_byte_literal": b"integer literal is outside its contextual type",
        "reject_ref_alias": b"ref arguments of one call must not alias",
        "reject_view_live_write": b"during view lifetime normalization",
        "reject_view_copy_live_write": b"during view lifetime normalization",
        "reject_view_module_alias": b"during view lifetime normalization",
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
    assert not failures, failures
    print(f"{len(records)} Array Source/VM/native commands passed")


if __name__ == "__main__":
    main()

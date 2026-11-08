"""Exercise public LIR execution, ordinary native output and runtime relocation."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runtime-library", type=Path, action="append")
    parser.add_argument("--effects-only", action="store_true")
    args = parser.parse_args()
    if not args.effects_only and not args.runtime_library:
        parser.error("--runtime-library is required for native command checks")
    binary, root = args.binary.resolve(), args.root.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="lir-cli-", dir=args.output.resolve()))
    fixtures = root / "tests/fixtures/lir_cli"
    environment = dict(os.environ, XRAY_STDLIB_PATH=str(root / "stdlib"), NO_COLOR="1")
    environment["PATH"] = str(binary.parent) + os.pathsep + environment.get("PATH", "")
    records = []
    sentinel = b"previous output\n"

    def run(name, command, expected):
        argv = list(map(str, command))
        started = time.perf_counter()
        child = subprocess.run(argv, cwd=work, env=environment, capture_output=True, timeout=30)
        record = {"name": name, "command": argv, "exit": child.returncode,
                  "seconds": time.perf_counter() - started}
        for stream, data in (("stdout", child.stdout), ("stderr", child.stderr)):
            path = work / (name + "." + stream)
            path.write_bytes(data)
            record[stream] = {"path": str(path), "sha256": hashlib.sha256(data).hexdigest()}
        records.append(record)
        (work / "result.json").write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")
        assert child.returncode == expected, (name, child.returncode, child.stdout, child.stderr)
        return child

    def verify_effects():
        effects = fixtures / "effects"
        for name in ("pure_recursive_import", "pure_import_go", "pure_scoped_go", "generic_stringable", "nosuspend_literal",
                     "root_const", "signature_throws", "signature_generic_try",
                     "closed_dispatch_pure", "closed_dispatch_reference",
                     "root_parameter_forward", "root_parameter_capture", "root_parameter_recursive"):
            checked = run(name + "-check", [binary, "check", effects / (name + ".xr")], 0)
            assert not checked.stdout and not checked.stderr, name

        checked = run("root-rest-captures-check",
                      [binary, "check", root / "tests/unit/xir/rest_values/values.xr"], 0)
        assert not checked.stdout and not checked.stderr, "root-rest-captures"

        rejected_effects = (
            ("nosuspend_await", b"declared no_suspend function may suspend"),
            ("nosuspend_fixed_callable", b"declared no_suspend function may suspend"),
            ("nosuspend_chain", b"entry -> bridge -> leaf (suspending operation)"),
            ("nosuspend_callable_chain", b"forward (unqualified callable)"),
            ("nosuspend_chain_truncated", b"intermediate_function_with_a_long_name -> ..."),
            ("nosuspend_strengthen", b"qualified reference requires an explicit target promise"),
            ("closed_dispatch_suspend", b"declared no_suspend function may suspend"),
            ("closed_dispatch_erased", b"declared no_suspend function may suspend"),
            ("closed_dispatch_late_erased", b"declared no_suspend function may suspend"),
            ("root_mutable", b"GO target requires the current instance root execution"),
            ("root_cleanup", b"GO target requires the current instance root execution"),
            ("root_default", b"GO target requires the current instance root execution"),
            ("root_import", b"GO target requires the current instance root execution"),
            ("root_parameter_escape", b"GO target lacks proof for worker execution"),
            ("redundant_root_apply", b"E0336: try body has no business-error source"),
            ("redundant_root_fixed", b"E0336: try body has no business-error source"),
            ("redundant_root_root", b"E0336: try body has no business-error source"),
            ("redundant_root_forward", b"E0336: try body has no business-error source"),
            ("redundant_root_capture_pure", b"E0336: try body has no business-error source"),
            ("redundant_root_capture_root", b"E0336: try body has no business-error source"),
            ("redundant_root_capture_fixed", b"E0336: try body has no business-error source"),
            ("signature_missing_try", b"E0330: throwing call requires try or a try body"),
            ("signature_redundant_try", b"E0336: try expression has no business-error source"),
        )
        for name, diagnostic in rejected_effects:
            source = effects / (name + ".xr")
            destination = work / (name + ".c")
            destination.write_bytes(sentinel)
            for command in ("check", "run", "build"):
                options = ["--c-only", "-o", destination] if command == "build" else []
                refused = run(name + "-" + command, [binary, command, source, *options], 1)
                assert not refused.stdout and diagnostic in refused.stderr, (name, command)
            assert destination.read_bytes() == sentinel, name

    if args.effects_only:
        verify_effects()
        print(f"{len(records)} public effect checks passed")
        return

    expected = {"hello": b"42\n", "call": b"42\n", "unicode": "你好\n".encode(),
                "f64": b"3.75\n", "instance_growth": b"1\n" * 8,
                "module_scalar_constants": b"1\n"}
    expected["generic_stringable"] = b"42\nhello\n"
    generated = {"generic_stringable": fixtures / "effects" / "generic_stringable.xr"}
    scalar_types = ("i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64")
    capacity_cases = (
        ("many_generic_instances",
         "".join(f"fn genericFn{i}<T>()->i64{{return {i}}}\n" for i in range(16))
         + "".join(f"print(genericFn{i}<{kind}>())\n" for i in range(16) for kind in scalar_types),
         b"".join(f"{i}\n".encode() for i in range(16) for _ in scalar_types)),
        ("many_functions",
         "".join(f"fn capacityFn{i}()->i64{{return {i}}}\n" for i in range(1100))
         + "print(capacityFn1099())\n", b"1099\n"),
        ("many_blocks", "fn choose(value:i64)->i64{\n"
         + "".join(f"if(value=={i}){{return {i}}}\n" for i in range(2100))
         + "return -1\n}\nprint(choose(2099))\n", b"2099\n"),
    )
    for name, text, stdout in capacity_cases:
        source = work / (name + ".xr")
        source.write_text(text, encoding="utf-8", newline="\n")
        generated[name] = source
        expected[name] = stdout
    modules = work / "modules"
    modules.mkdir()
    for index in range(1025):
        (modules / f"module{index}.xr").write_text("\n", encoding="utf-8")
    entry = modules / "entry.xr"
    entry.write_text("".join(f'import "./module{i}" as capacityMod{i}\n' for i in range(1025))
                     + "print(1)\n", encoding="utf-8", newline="\n")
    generated["many_modules"] = entry
    expected["many_modules"] = b"1\n"
    for name, stdout in expected.items():
        source = generated.get(name, fixtures / (name + ".xr"))
        vm = run(name + "-vm", [binary, "run", source], 0)
        assert vm.stdout == stdout and not vm.stderr, name
        executable = work / (name + (".exe" if os.name == "nt" else ""))
        run(name + "-build", [binary, "build", source, "-o", executable], 0)
        native = run(name + "-native", [executable], 0)
        assert (native.stdout, native.stderr) == (vm.stdout, vm.stderr), name

    source = fixtures / "panic.xr"
    vm = run("panic-vm", [binary, "run", source], 1)
    executable = work / ("panic.exe" if os.name == "nt" else "panic")
    run("panic-build", [binary, "build", source, "-o", executable], 0)
    native = run("panic-native", [executable], 1)
    assert vm.stderr and (native.stdout, native.stderr) == (vm.stdout, vm.stderr)

    generated = work / "hello.c"
    run("c-only", [binary, "build", fixtures / "hello.xr", "--c-only", "-o", generated], 0)
    assert "int main(void)" in generated.read_text(encoding="utf-8")
    for flag, path in (([], work / "protected.exe"), (["--c-only"], generated)):
        path.write_bytes(sentinel)
        run("invalid-" + path.suffix[1:],
            [binary, "build", fixtures / "invalid.xr", *flag, "-o", path], 1)
        assert path.read_bytes() == sentinel

    for name in ("constant_div_zero", "constant_conversion_range"):
        source = fixtures / (name + ".xr")
        refused = run(name + "-vm", [binary, "run", source], 1)
        assert not refused.stdout and b"constant scalar initializer" in refused.stderr
        executable = work / (name + (".exe" if os.name == "nt" else ""))
        refused = run(name + "-build", [binary, "build", source, "-o", executable], 1)
        assert not executable.exists() and b"constant scalar initializer" in refused.stderr

    verify_effects()

    for command in ("repl", "dap", "test"):
        refused = run("unavailable-" + command, [binary, command], 1)
        assert not refused.stdout and b"temporarily unavailable" in refused.stderr
    refused = run("unavailable-sandbox", [binary, "run", "--sandbox", fixtures / "hello.xr"], 1)
    assert not refused.stdout and b"temporarily unavailable" in refused.stderr
    probe = run("probe", [binary, "toolchain", "probe", "--json"], 0)
    assert json.loads(probe.stdout)["ready"] is True and not probe.stderr
    refused = run("unavailable-zig", [binary, "toolchain", "probe", "--provider", "zig", "--json"], 3)
    assert json.loads(refused.stdout)["ready"] is False
    assert b"temporarily unavailable" in refused.stderr

    prefix = work / "relocated with 空格"
    (prefix / "bin").mkdir(parents=True)
    relocated = prefix / "bin" / binary.name
    shutil.copy2(binary, relocated)
    for runtime in binary.parent.glob("clang_rt.*.dll"):
        shutil.copy2(runtime, relocated.parent / runtime.name)
    absent = run("relocated-missing-runtime",
                 [relocated, "build", fixtures / "hello.xr", "-o", work / "absent.exe"], 1)
    assert b"native runtime" in absent.stderr and not (work / "absent.exe").exists()
    runtime = prefix / "lib/xray/native"
    (runtime / "include").mkdir(parents=True)
    for library in args.runtime_library:
        shutil.copy2(library, runtime / library.name)
    for header in ("xray_native_runtime.h", "xray_export.h"):
        shutil.copy2(root / "include" / header, runtime / "include" / header)
    executable = work / ("relocated.exe" if os.name == "nt" else "relocated")
    run("relocated-build", [relocated, "build", fixtures / "hello.xr", "-o", executable], 0)
    native = run("relocated-run", [executable], 0)
    assert native.stdout == b"42\n" and not native.stderr
    print(f"{len(records)} public LIR command checks passed")


if __name__ == "__main__":
    main()

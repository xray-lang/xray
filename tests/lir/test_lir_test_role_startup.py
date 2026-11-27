"""Run a selected Source test through register bytecode and real generated C."""
from pathlib import Path
import argparse
import json
import os
import subprocess
import sys
import tempfile

from test_lir_pipeline_eval import compile_command

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from xraytest.artifacts import complete_native_artifacts


REJECTED = b"no matching root-module @test declaration"


def invoke(root, name, command):
    argv = [str(value) for value in command]
    (root / (name + ".command.json")).write_text(json.dumps(argv, indent=2) + "\n", encoding="utf-8")
    completed = subprocess.run(argv, cwd=root, capture_output=True, timeout=30)
    (root / (name + ".stdout")).write_bytes(completed.stdout)
    (root / (name + ".stderr")).write_bytes(completed.stderr)
    (root / (name + ".exit")).write_text(str(completed.returncode) + "\n", encoding="ascii")
    return completed.returncode, completed.stdout, completed.stderr


def run_entry(args, root, source, label, *options, session=False, run_options=()):
    selection = ["--stdlib", args.source_root / "stdlib", *options]
    generated = root / (label + ".c")
    vm = invoke(root, label + "-vm", [args.vm_driver, source, *selection, *run_options])
    emission = invoke(root, label + "-emit", [args.driver, source, generated, *selection])
    assert emission == (0, b"", b""), (emission, root)
    assert b"({" not in generated.read_bytes(), (generated, root)
    binary = root / (label + (".exe" if args.kind == "msvc" else "-native"))
    command = compile_command(args, generated, binary)
    if session:
        host = args.source_root / "tests/lir/test_role_session_host.c"
        command.insert(command.index(generated) + 1, host)
        if args.kind == "msvc":
            command = ["/Fo" + str(root) + os.sep if isinstance(arg, str) and arg.startswith("/Fo")
                       else arg for arg in command]
            command.insert(1, "/D_CRT_SECURE_NO_WARNINGS")
    compilation = invoke(root, label + "-compile", command)
    assert compilation[0] == 0, (compilation, root)
    native = invoke(root, label + "-native", [binary, *run_options])
    assert vm == native, (label, vm, native, root)
    return native


def assert_panic(result, source, expected_stdout, message):
    prefix = b"[Uncaught Panic] E0445: " + message + b"\n"
    assert result[:2] == (1, expected_stdout), result
    first, location = result[2].splitlines()
    assert first + b"\n" == prefix, result
    assert location.startswith(b"    at "), result
    line, text = next((line, text) for line, text in enumerate(source.read_bytes().splitlines(), 1)
                      if b'assert(false, "' + message + b'")' in text)
    column = text.index(b"assert(") + 1
    assert location.endswith(f"{source.name}:{line}:{column}".encode("ascii")), result


def assert_rejected(args, root, source, label, *entry_options, lowering=None):
    generated = root / ("reject-" + label + ".c")
    options = ["--stdlib", args.source_root / "stdlib", *entry_options]
    vm = invoke(root, label + "-reject-vm", [args.vm_driver, source, *options])
    native = invoke(root, label + "-reject-emit", [args.driver, source, generated, *options])
    assert vm[:2] == native[:2] == (1, b"") and not generated.exists(), (label, vm, native, root)
    if lowering is None:
        assert vm == native and vm[2].splitlines() == [REJECTED], (label, vm, native, root)
    else:
        assert native[2].splitlines() == [b"lowering: " + lowering], (native, root)
        lines = vm[2].splitlines()
        assert len(lines) == 1 and lines[0].startswith(b"lir_vm_run: lower "), (vm, root)
        problem, metadata, _ = lines[0].partition(b" [function=")
        assert metadata and problem.endswith(b": " + lowering), (vm, root)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("driver", "vm-driver", "compiler", "include", "source-root", "work"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--library", type=Path, nargs="+", required=True)
    parser.add_argument("--kind", choices=("msvc", "gnu"), required=True)
    parser.add_argument("--msvc-runtime", default="MultiThreaded")
    parser.add_argument("--asan", action="store_true")
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="test-role-", dir=args.work.resolve()))
    fixtures = args.source_root / "tests/lir/programs"
    source = fixtures / "test-role-startup.xr"
    initializer_failure = fixtures / "test-role-init-failure.xr"

    assert run_entry(args, root, source, "chosen_test", "--test", "chosen_test") == (0, b"41\n42\n", b""), root
    assert_panic(run_entry(args, root, source, "failing_test", "--test", "failing_test"), source,
                 b"41\n43\n", b"selected test failure")
    assert_panic(run_entry(args, root, initializer_failure, "blocked_test", "--test", "blocked_test"), initializer_failure,
                 b"17\n", b"initializer failure")

    ordinary = invoke(root, "ordinary-vm", [args.vm_driver, source, "--stdlib", args.source_root / "stdlib"])
    assert ordinary == (0, b"41\n", b""), (ordinary, root)

    for name in ("skipped_test", "imported_test", "ordinary_helper", "local_hook", "chosen", "missing_test"):
        assert_rejected(args, root, source, name, "--test", name)
    assert_rejected(args, root, source, "deadline_test", "--test", "deadline_test",
                    lowering=b"test-role startup with a deadline is not supported by LIR lowering yet")
    assert_rejected(args, root, source, "typed_failure_test", "--test", "typed_failure_test",
                    lowering=b"test-role startup signature or typed errors is not supported by LIR lowering yet")

    session = fixtures / "test-role-session.xr"
    assert run_entry(args, root, session, "session-init", "--test-plan") == (0, b"17\n41\n", b""), root
    assert run_entry(args, root, session, "session-optimized", "--test-plan", "--optimize") == (0, b"17\n41\n", b""), root
    assert_panic(run_entry(args, root, initializer_failure, "session-blocked", "--test-plan"),
                 initializer_failure, b"17\n", b"initializer failure")
    assert_rejected(args, root, source, "session-deadline", "--test-plan",
                    lowering=b"test-role startup with a deadline is not supported by LIR lowering yet")
    typed_error = fixtures / "test-role-session-typed-error.xr"
    assert_rejected(args, root, typed_error, "session-typed-error", "--test-plan",
                    lowering=b"test-role startup signature or typed errors is not supported by LIR lowering yet")

    actions = fixtures / "test-role-session-actions.xr"
    assert run_entry(args, root, actions, "file-pass", "--test-session", "passing_action",
                     session=True) == (0, b"17\n20\n22\n27\nseed!.\n", b""), root
    assert_panic(run_entry(args, root, actions, "file-panic", "--test-session", "failing_action",
                           "--optimize", session=True), actions, b"17\n20\n22\n27\nseed!?\n",
                 b"selected inner panic")
    assert_panic(run_entry(args, root, initializer_failure, "file-sticky", "--test-session", "blocked_test",
                           session=True, run_options=("--retry-first",)),
                 initializer_failure, b"17\n", b"initializer failure")
    pending = fixtures / "test-role-session-close.xr"
    assert_panic(run_entry(args, root, pending, "file-close", "--test-session", "selected_close",
                           session=True, run_options=("--defer-close",)),
                 pending, b"17\n42\n43\n", b"session close panic")
    for name in ("skipped_action", "ordinary_action", "imported_session_test", "before_action", "missing"):
        assert_rejected(args, root, actions, "file-" + name, "--test-session", name)
    complete_native_artifacts(root)
    print(f"Selected test startup, session initialization and rejection passed through both backends. Artifacts at {root}")


if __name__ == "__main__":
    main()

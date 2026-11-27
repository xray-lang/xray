"""Run a selected Source test through register bytecode and real generated C."""
from pathlib import Path
import argparse
import json
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


def run_selected(args, root, source, test_name):
    selection = [source, "--stdlib", args.source_root / "stdlib", "--test", test_name]
    generated = root / (test_name + ".c")
    vm = invoke(root, test_name + "-vm", [args.vm_driver, *selection])
    emission = invoke(root, test_name + "-emit", [args.driver, source, generated, *selection[1:]])
    assert emission == (0, b"", b""), (emission, root)
    assert b"({" not in generated.read_bytes(), (generated, root)
    binary = root / (test_name + (".exe" if args.kind == "msvc" else "-native"))
    compilation = invoke(root, test_name + "-compile", compile_command(args, generated, binary))
    assert compilation[0] == 0, (compilation, root)
    native = invoke(root, test_name + "-native", [binary])
    assert vm == native, (test_name, vm, native, root)
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


def assert_rejected(args, root, source, test_name):
    generated = root / ("reject-" + test_name + ".c")
    options = ["--stdlib", args.source_root / "stdlib", "--test", test_name]
    vm = invoke(root, test_name + "-reject-vm", [args.vm_driver, source, *options])
    native = invoke(root, test_name + "-reject-emit", [args.driver, source, generated, *options])
    assert vm == native and vm[:2] == (1, b""), (test_name, vm, native, root)
    assert vm[2].splitlines() == [REJECTED] and not generated.exists(), (test_name, vm, root)


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

    assert run_selected(args, root, source, "chosen_test") == (0, b"41\n42\n", b""), root
    assert_panic(run_selected(args, root, source, "failing_test"), source,
                 b"41\n43\n", b"selected test failure")
    assert_panic(run_selected(args, root, initializer_failure, "blocked_test"), initializer_failure,
                 b"17\n", b"initializer failure")

    ordinary = invoke(root, "ordinary-vm", [args.vm_driver, source, "--stdlib", args.source_root / "stdlib"])
    assert ordinary == (0, b"41\n", b""), (ordinary, root)

    for name in ("skipped_test", "imported_test", "ordinary_helper", "local_hook", "chosen", "missing_test"):
        assert_rejected(args, root, source, name)
    complete_native_artifacts(root)
    print(f"Selected test startup and rejection passed through both backends. Artifacts at {root}")


if __name__ == "__main__":
    main()

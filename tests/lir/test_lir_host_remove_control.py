"""Check file and empty-directory removal at both host cancellation boundaries."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_lir_pipeline_eval import compile_command


def invoke(work, command, *, env=None, status=0):
    process = subprocess.run(command, cwd=work, env=env, capture_output=True, timeout=30)
    assert process.returncode == status, (command, process)
    return process.stdout, process.stderr


def build_native(args, work, source, stdlib):
    generated = work / "generated.c"
    env = dict(os.environ, XRAY_STDLIB_PATH=str(stdlib), NO_COLOR="1")
    invoke(work, [args.binary, "build", "--c-only", source, "-o", generated], env=env)
    target = work / ("host-native.exe" if os.name == "nt" else "host-native")
    command = compile_command(args, Path(__file__).with_name("lir_host_remove_control.c"), target)
    if args.kind == "msvc":
        options = ["/DXR_TEST_NATIVE_REMOVE", "/D_CRT_SECURE_NO_WARNINGS", "/I" + str(work)]
        if args.ubsan:
            if not args.asan:
                options.append("/clang:-fsanitize=undefined")
            options.append("/clang:-fno-sanitize=function")
        if args.ubsan_runtime:
            command.append(args.ubsan_runtime)
        if args.lld_no_tail_merge:
            command.append("/OPT:NOLLDTAILMERGE")
    else:
        options = ["-DXR_TEST_NATIVE_REMOVE", "-I" + str(work)]
        if args.ubsan:
            if not args.asan:
                options.append("-fsanitize=undefined")
            options.append("-fno-sanitize=function")
    command[1:1] = options
    invoke(work, command)
    return target


def check_removal(directory, driver, source, stdlib, operation, mode):
    directory.mkdir()
    leaf = directory / "leaf"
    if operation == "rmdir":
        leaf.mkdir()
    else:
        leaf.write_bytes(b"original")
    if mode == "run":
        expected = (b"11\ntrue\n22\n", b"")
        status = 0
    else:
        expected = (b"11\n", b"[Uncaught Cancellation] at unknown source\n")
        status = 1
    actual = invoke(directory, [driver, source, stdlib, mode, operation], status=status)
    assert actual == expected, (operation, mode, driver, actual)
    if mode == "run":
        assert not leaf.exists(), (operation, mode, driver)
    elif operation == "rmdir":
        assert leaf.is_dir() and not list(leaf.iterdir()), (operation, mode, driver)
    else:
        assert leaf.read_bytes() == (b"original" if mode == "before" else b"replacement"), (mode, driver)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "driver", "compiler", "include", "source-root", "work"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--kind", required=True, choices=("msvc", "gnu"))
    parser.add_argument("--library", required=True, nargs="+")
    parser.add_argument("--msvc-runtime", default="MultiThreadedDLL")
    parser.add_argument("--asan", action="store_true")
    parser.add_argument("--ubsan", action="store_true")
    parser.add_argument("--ubsan-runtime", type=Path)
    parser.add_argument("--lld-no-tail-merge", action="store_true")
    args = parser.parse_args()

    tests = Path(__file__).resolve().parent
    source_root = args.source_root.resolve()
    args.work.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="remove-", dir=args.work) as directory:
        work = Path(directory).resolve()
        stdlib = work / "stdlib"
        shutil.copytree(source_root / "stdlib", stdlib)
        observer = source_root / "tests/fixtures/lir_cli/host_path/rmdir_observer.xr"
        with (stdlib / "io/io.xr").open("ab") as module:
            module.write(b"\n" + observer.read_bytes())
        for operation in ("remove", "rmdir"):
            source = tests / "programs" / ("host-" + operation + "-control.xr")
            case = work / operation
            case.mkdir()
            native = build_native(args, case, source, stdlib)
            for engine, driver in (("vm", args.driver), ("native", native)):
                for mode in ("run", "before", "after"):
                    check_removal(case / (engine + "-" + mode), driver, source, stdlib, operation, mode)
            print(f"{operation}: RBC and emitted C agree on output and filesystem effects before and after cancellation")


if __name__ == "__main__":
    main()

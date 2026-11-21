"""Run the Class-union `is` source program through the VM and native C paths."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from xraytest.toolchain import archive_link_arguments, native_c11_options  # noqa: E402

EXPECTED = b"true\nfalse\n1\n2\n"


def main():
    parser = argparse.ArgumentParser()
    for name in ("vm-driver", "source-driver", "compiler", "include", "source-root", "work"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--library", type=Path, nargs="+", required=True)
    parser.add_argument("--kind", choices=("msvc", "gnu"), required=True)
    parser.add_argument("--msvc-runtime", default="MultiThreaded")
    parser.add_argument("--asan", action="store_true")
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    folder = Path(tempfile.mkdtemp(prefix="class-union-is-", dir=args.work)).resolve()
    source = (args.source_root / "tests/lir/programs/class-union-is.xr").resolve()

    def run(label, command, stdout=subprocess.PIPE):
        result = subprocess.run(list(map(str, command)), cwd=folder, stdout=stdout,
                                stderr=subprocess.PIPE, timeout=120)
        (folder / (label + ".stdout")).write_bytes(result.stdout or b"")
        (folder / (label + ".stderr")).write_bytes(result.stderr)
        return result

    vm = run("vm", [args.vm_driver, source])
    assert (vm.returncode, vm.stdout, vm.stderr) == (0, EXPECTED, b""), (folder, vm.stderr)

    generated = folder / "generated.c"
    emitted = run("emit", [args.source_driver, source, generated])
    assert emitted.returncode == 0, (folder, emitted.stderr)
    main_source = folder / "main.c"
    main_source.write_text(
        "#include <stdint.h>\nextern int64_t xray_lir_entry(void);\n"
        "int main(void){return xray_lir_entry()!=0;}\n", encoding="ascii")
    if args.kind == "msvc":
        crt = {"MultiThreaded": "/MT", "MultiThreadedDebug": "/MTd",
               "MultiThreadedDLL": "/MD", "MultiThreadedDebugDLL": "/MDd"}[args.msvc_runtime]
        flags = ["/nologo", *native_c11_options(args.compiler, driver=args.kind), "/W4", "/WX", "/utf-8",
                 crt, "/O2", "/I" + str(args.include)]
        if args.asan:
            flags += ["/fsanitize=address"]
    else:
        flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-O2",
                 "-I" + str(args.include)]
        if args.asan:
            flags += ["-fsanitize=address,undefined"]
    objects = []
    for item in (generated, main_source):
        obj = folder / (item.stem + (".obj" if args.kind == "msvc" else ".o"))
        command = ([args.compiler, *flags, "/c", item, "/Fo:" + str(obj)] if args.kind == "msvc" else
                   [args.compiler, *flags, "-c", item, "-o", obj])
        built = run("compile-" + item.stem, command)
        assert built.returncode == 0, (folder, built.stderr)
        objects.append(obj)
    executable = folder / ("class-union-is.exe" if args.kind == "msvc" else "class-union-is")
    command = [args.compiler, *flags, *objects, *archive_link_arguments(args.library, driver=args.kind)]
    command += ["/Fe:" + str(executable)] if args.kind == "msvc" else ["-o", executable]
    linked = run("link", command)
    assert linked.returncode == 0, (folder, linked.stderr)
    native = run("native", [executable])
    assert (native.returncode, native.stdout, native.stderr) == (0, EXPECTED, b""), (folder, native.stderr)
    print("Class-union is: VM and native C both computed the expected table")


if __name__ == "__main__":
    main()

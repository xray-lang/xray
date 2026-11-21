"""Run one tagged-sum source program through the VM and the native C path."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from xraytest.toolchain import archive_link_arguments, native_c11_options  # noqa: E402


def run_case(args, source, expected, tag):
    folder = Path(tempfile.mkdtemp(prefix="sum-", dir=args.work)).resolve()

    def run(label, command, stdout=subprocess.PIPE):
        result = subprocess.run(list(map(str, command)), cwd=folder, stdout=stdout,
                                stderr=subprocess.PIPE, timeout=args.command_timeout)
        (folder / (label + ".stdout")).write_bytes(result.stdout or b"")
        (folder / (label + ".stderr")).write_bytes(result.stderr)
        return result

    vm = run("vm", [args.vm_driver, source])
    assert (vm.returncode, vm.stdout, vm.stderr) == (0, expected, b""), (folder, vm.stderr)

    generated = folder / "generated.c"
    emitted = run("emit", [args.source_driver, source, generated])
    assert emitted.returncode == 0, (folder, emitted.stderr)
    if getattr(args, "c_contains", None) or getattr(args, "c_absent", None):
        text = generated.read_text(encoding="utf-8", errors="replace")
        for needle in filter(None, (getattr(args, "c_contains", None) or "").split(",")):
            assert needle in text, (folder, "emitted C lost " + needle)
        for needle in filter(None, (getattr(args, "c_absent", None) or "").split(",")):
            assert needle not in text, (folder, "emitted C still has " + needle)
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
    executable = folder / ("sum.exe" if args.kind == "msvc" else "sum")
    command = [args.compiler, *flags, *objects, *archive_link_arguments(args.library, driver=args.kind)]
    command += ["/Fe:" + str(executable)] if args.kind == "msvc" else ["-o", executable]
    linked = run("link", command)
    assert linked.returncode == 0, (folder, linked.stderr)
    native = run("native", [executable])
    assert (native.returncode, native.stdout, native.stderr) == (0, expected, b""), (folder, native.stderr)
    print(f"sum types ({tag}): VM and native C both printed the expected table")


def run_refusal(args, source, message):
    """Both LIR drivers must refuse one program and publish no C for it."""
    folder = Path(tempfile.mkdtemp(prefix="sum-refused-", dir=args.work)).resolve()
    generated = folder / "refused.c"
    for label, command in (("vm", [args.vm_driver, source]),
                           ("source", [args.source_driver, source, generated])):
        result = subprocess.run(list(map(str, command)), cwd=folder, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=args.command_timeout)
        (folder / (label + ".stderr")).write_bytes(result.stderr)
        assert (result.returncode, result.stdout) == (1, b""), (folder, label, result.stderr)
        assert message in result.stderr, (folder, label, result.stderr)
    assert not generated.exists(), (folder, "refused program published C")
    print(f"sum types ({Path(args.program).stem}): both drivers refused the payload carrier")


def main():
    parser = argparse.ArgumentParser()
    for name in ("vm-driver", "source-driver", "compiler", "include", "source-root", "work"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--program", required=True)
    parser.add_argument("--expect", help="expected stdout lines, comma separated")
    parser.add_argument("--refuse", help="expected lowering message; the program must be refused")
    parser.add_argument("--c-contains", help="comma-separated substrings the emitted C must keep")
    parser.add_argument("--c-absent", help="comma-separated substrings the emitted C must not contain")
    parser.add_argument("--library", type=Path, nargs="+", required=True)
    parser.add_argument("--kind", choices=("msvc", "gnu"), required=True)
    parser.add_argument("--msvc-runtime", default="MultiThreaded")
    parser.add_argument("--asan", action="store_true")
    parser.add_argument("--command-timeout", type=int, default=120,
                        help="seconds allowed for one compiler, link or run step")
    args = parser.parse_args()
    if bool(args.expect) == bool(args.refuse):
        parser.error("exactly one of --expect and --refuse is required")
    args.work.mkdir(parents=True, exist_ok=True)
    source = (args.source_root / args.program).resolve()
    if args.refuse:
        run_refusal(args, source, args.refuse.encode())
        return
    expected = ("\n".join(args.expect.split(",")) + "\n").encode()
    run_case(args, source, expected, Path(args.program).stem)


if __name__ == "__main__":
    main()

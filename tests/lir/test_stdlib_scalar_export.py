"""Link the archive produced by the toolchain build and check export authority."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from test_lir_c_native import run


def private_dependency(args, root):
    build = args.generated.parents[2]
    if (build / "build.ninja").is_file():
        cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
        ninja = next(line.split("=", 1)[1] for line in cache.splitlines()
                     if line.startswith("CMAKE_MAKE_PROGRAM:"))
        command = [ninja]
        config = build / f"build-{args.generated.parent.name}.ninja"
        if config.is_file():
            command += ["-f", config.name]
        graph = subprocess.check_output(
            [*command, "-t", "query", args.generated.relative_to(build).as_posix()],
            cwd=build, encoding="utf-8", timeout=10,
        )
        dependencies = {os.path.normcase(line.strip()) for line in graph.splitlines()}
        lexical_source = args.stdlib / "path/_lexical.xr"
        assert os.path.normcase(str(lexical_source)) in dependencies

    stdlib = root / "模块 🧪"
    math = stdlib / "math"
    private = math / "_private"
    private.mkdir(parents=True)
    (math / "math.xr").write_text(
        'import "std/math/_private/blend" as privateBlend\n'
        'export fn lerp(a: f64, b: f64, t: f64) -> f64 { return privateBlend.blend(a, b, t) }\n',
        encoding="utf-8",
    )
    leaf = private / "blend.xr"
    output = root / "产物 🧪.c"
    leaf.write_text('export fn blend(a: f64, b: f64, t: f64) -> f64 { return a * (b + t) }\n',
                    encoding="utf-8")
    run([args.producer, stdlib, output], cwd=root)
    before = output.read_bytes()
    leaf.write_text('export fn blend(a: f64, b: f64, t: f64) -> f64 { return a + (b * t) }\n',
                    encoding="utf-8")
    run([args.producer, stdlib, output], cwd=root)
    after = output.read_bytes()
    assert before != after
    assert b"double xray_stdlib_math_lerp(double p0, double p1, double p2)" in before
    assert b"double xray_stdlib_math_lerp(double p0, double p1, double p2)" in after

    missing = root / "缺少目录 🧪" / "output.c"
    failure = run([args.producer, stdlib, missing], cwd=root, expected_exit=1)
    assert b"cannot write the generated scalar export" in failure.stderr
    assert not missing.exists()


def authority_cases(args, root):
    repeated = root / "regenerated.c"
    run([args.producer, args.stdlib, repeated], cwd=root)
    generated = args.generated.read_bytes()
    assert generated == repeated.read_bytes()
    assert b"double xray_stdlib_math_lerp(double p0, double p1, double p2)" in generated
    assert all(fragment not in generated for fragment in
               (b"xray_lir_entry", b"xray_native_", b"xrt_", b"int main("))

    rejected = root / "rejected.c"
    sentinel = b"preserve any prior output"
    rejected.write_bytes(sentinel)
    empty = root / "no-stdlib"
    empty.mkdir()
    failure = run([args.producer, empty, rejected], cwd=root, expected_exit=1)
    assert b"cannot resolve canonical standard library source" in failure.stderr
    assert rejected.read_bytes() == sentinel

    alternate = root / "alternate-stdlib"
    math = alternate / "math"
    math.mkdir(parents=True)
    math_source = math / "math.xr"
    foreign_signature = "export fn lerp(a: f64, b: f64, t: f64) -> f64 { return a }\n"
    (math / "_foreign.xr").write_text(foreign_signature, encoding="utf-8")
    math_source.write_text(
        'import "std/math/_foreign" as foreign\n' + foreign_signature.removeprefix("export "),
        encoding="utf-8",
    )
    failure = run([args.producer, alternate, rejected], cwd=root, expected_exit=1)
    assert b"cannot select unique exported Checked math.lerp" in failure.stderr
    assert rejected.read_bytes() == sentinel

    math_source.write_text("export fn lerp(a: f64) -> f64 { return a }\n", encoding="utf-8")
    failure = run([args.producer, alternate, rejected], cwd=root, expected_exit=1)
    assert b"cannot validate the declared scalar ABI" in failure.stderr
    assert rejected.read_bytes() == sentinel

    math_source.write_text(
        "export fn lerp(a: f64, b: f64, t: f64) -> f64 {\n"
        "  print(a)\n  return b + t\n}\n",
        encoding="utf-8",
    )
    failure = run([args.producer, alternate, rejected], cwd=root, expected_exit=1)
    assert (b"cannot validate closed scalar effects" in failure.stderr or
            b"scalar export requires a runtime-free call closure" in failure.stderr)
    assert rejected.read_bytes() == sentinel
    return empty


def link_archive(args, root, empty):
    consumer = root / "consumer.c"
    consumer.write_text(
        '#include <stdio.h>\n'
        '#if defined(_WIN32)\n#include <fcntl.h>\n#include <io.h>\n#endif\n'
        'extern double xray_stdlib_math_lerp(double, double, double);\n'
        'int main(void) {\n'
        '#if defined(_WIN32)\n    if (_setmode(_fileno(stdout), _O_BINARY) == -1) return 1;\n#endif\n'
        '    double a = xray_stdlib_math_lerp(2.0, 10.0, 0.25);\n'
        '    double b = xray_stdlib_math_lerp(5.0, -3.0, 1.5);\n'
        '    return printf("%.1f\\n%.1f\\n", a, b) < 0;\n'
        '}\n',
        encoding="ascii",
    )
    binary = root / ("consumer.exe" if os.name == "nt" else "consumer")
    if args.kind == "msvc":
        crt = {"MultiThreaded": "/MT", "MultiThreadedDebug": "/MTd",
               "MultiThreadedDLL": "/MD", "MultiThreadedDebugDLL": "/MDd"}
        command = [args.compiler, "/nologo", "/std:c11", "/W4", "/WX",
                   crt[args.msvc_runtime], consumer, args.archive, "/Fe:" + str(binary)]
        if args.asan:
            command += ["/fsanitize=address", "/Zi"]
        if args.ubsan and "clang" in args.compiler.name.lower():
            command += ["/clang:-fsanitize=undefined"]
        if args.asan:
            command += ["/link", "/INCREMENTAL:NO"]
    else:
        command = [args.compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   consumer, args.archive, "-o", binary]
        if args.asan:
            command.append("-fsanitize=address")
        if args.ubsan:
            command.append("-fsanitize=undefined")
    run(command, cwd=root)
    completed = run([binary], cwd=empty, env=dict(os.environ, XRAY_STDLIB_PATH=str(empty)))
    assert completed.stdout == b"4.0\n-7.0\n" and completed.stderr == b""
    assert not list(empty.iterdir())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--producer", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--generated", type=Path, required=True)
    parser.add_argument("--stdlib", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--kind", choices=["msvc", "gnu"], required=True)
    parser.add_argument("--msvc-runtime")
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--asan", action="store_true")
    parser.add_argument("--ubsan", action="store_true")
    parser.add_argument("native_test", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="stdlib-export-", dir=args.work))
    empty = authority_cases(args, root)
    private_dependency(args, root)
    link_archive(args, root, empty)
    print(f"Same-build standard library archive and authority checks passed: {root}", flush=True)
    subprocess.run(args.native_test[1:], check=True)


if __name__ == "__main__":
    main()

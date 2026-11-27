"""Link the archive produced by the toolchain build and check export authority."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile

from test_lir_c_native import run


def abs_generated(args):
    return args.generated.with_name("stdlib_scalar_math_abs.c")


def private_dependency(args, root):
    build = args.generated.parents[2]
    inventory = build / "generated/stdlib_scalar_source_paths.txt"
    expected = sorted(path.relative_to(args.stdlib).as_posix()
                      for path in args.stdlib.rglob("*.xr") if path.is_file())
    assert inventory.read_text(encoding="utf-8").splitlines() == expected

    if (build / "build.ninja").is_file():
        cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
        ninja = next(line.split("=", 1)[1] for line in cache.splitlines()
                     if line.startswith("CMAKE_MAKE_PROGRAM:"))
        command = [ninja]
        config = build / f"build-{args.generated.parent.name}.ninja"
        if config.is_file():
            command += ["-f", config.name]
        lexical_source = args.stdlib / "path/_lexical.xr"
        outputs = [args.generated, abs_generated(args)]
        if args.interface:
            outputs.append(args.interface)
        targets = [path.relative_to(build).as_posix() for path in outputs]
        for target in targets:
            graph = subprocess.check_output(
                [*command, "-t", "query", target],
                cwd=build, encoding="utf-8", timeout=10,
            )
            dependencies = {os.path.normcase(line.strip()) for line in graph.splitlines()}
            assert os.path.normcase(str(lexical_source)) in dependencies
            assert os.path.normcase(inventory.relative_to(build).as_posix()) in dependencies
        invocations = subprocess.check_output(
            [*command, "-t", "commands", *targets], cwd=build, encoding="utf-8", timeout=10,
        ).splitlines()
        bundle = [line for line in invocations if " --bundle " in line]
        assert len(bundle) == 1 and all(path.name in bundle[0] for path in outputs), bundle
        assert ("stdlib_checked_math_interface.c" in bundle[0]) == bool(args.interface)

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
    updated = 'export fn blend(a: f64, b: f64, t: f64) -> f64 { return a + (b * t) }\n'
    leaf.write_text(updated, encoding="utf-8")
    run([args.producer, stdlib, output], cwd=root)
    after = output.read_bytes()
    assert before != after
    assert b"double xray_stdlib_math_lerp(double p0, double p1, double p2)" in before
    assert b"double xray_stdlib_math_lerp(double p0, double p1, double p2)" in after

    leaf.unlink()
    failure = run([args.producer, stdlib, output], cwd=root, expected_exit=1)
    assert b"module not found" in failure.stderr
    assert output.read_bytes() == after
    leaf.write_text(updated, encoding="utf-8")
    run([args.producer, stdlib, output], cwd=root)
    assert output.read_bytes() == after

    missing = root / "缺少目录 🧪" / "output.c"
    failure = run([args.producer, stdlib, missing], cwd=root, expected_exit=1)
    assert b"cannot write the generated scalar export" in failure.stderr
    assert not missing.exists()


def authority_cases(args, root):
    exports = (
        ("lerp", args.generated, b"double xray_stdlib_math_lerp(double p0, double p1, double p2)"),
        ("abs", abs_generated(args), b"double xray_stdlib_math_abs(double p0)"),
    )
    for name, path, prototype in exports:
        repeated = root / f"regenerated-{name}.c"
        run([args.producer, args.stdlib, repeated, name], cwd=root)
        generated = path.read_bytes()
        assert generated == repeated.read_bytes()
        assert prototype in generated
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
    assert b"math.lerp: cannot select the unique exported Checked declaration" in failure.stderr
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

    failure = run([args.producer, args.stdlib, rejected, "sqrt"], cwd=root, expected_exit=2)
    assert b"unknown export selection" in failure.stderr
    assert rejected.read_bytes() == sentinel

    math_source.write_text("export fn abs(x: i64) -> f64 { return 17.0 }\n", encoding="utf-8")
    failure = run([args.producer, alternate, rejected, "abs"], cwd=root, expected_exit=1)
    assert b"math.abs: cannot validate the declared scalar ABI" in failure.stderr
    assert rejected.read_bytes() == sentinel
    return empty


def bundle_cases(args, root):
    published = [args.generated, abs_generated(args)]
    if args.interface:
        published.append(args.interface)
    pending = [root / (path.name + ".pending") for path in published]
    run([args.producer, args.stdlib, "--bundle", *pending], cwd=root)
    assert [path.read_bytes() for path in pending] == [path.read_bytes() for path in published]

    stdlib = root / "bundle-stdlib"
    math_source = stdlib / "math/math.xr"
    math_source.parent.mkdir(parents=True)
    stock = (args.stdlib / "math/math.xr").read_text(encoding="utf-8")
    prior = [path.read_bytes() for path in pending]

    math_source.write_text(stock.replace("return __abs(x)", "return true"), encoding="utf-8")
    failure = run([args.producer, stdlib, "--bundle", *pending], cwd=root, expected_exit=1)
    assert len(re.findall(rb"math[\\/]math\.xr:\d+:\d+:", failure.stderr)) == 1
    assert b"cannot check standard library source" in failure.stderr
    assert [path.read_bytes() for path in pending] == prior

    wrong = stock.replace("export fn abs(x: f64) -> f64 {\n    return __abs(x)",
                          "export fn abs(x: i64) -> f64 {\n    return 17.0")
    assert wrong != stock
    math_source.write_text(wrong, encoding="utf-8")
    failure = run([args.producer, stdlib, "--bundle", *pending], cwd=root, expected_exit=1)
    assert b"math.abs: cannot validate the declared scalar ABI" in failure.stderr
    assert [path.read_bytes() for path in pending] == prior

    if args.interface:
        unrepresented_exports(args, root, stdlib, math_source, stock, pending)
        helper = math_source.with_name("_bundle_helper.xr")
        helper.write_text("export fn blend(a: f64, b: f64, t: f64) -> f64 { return a * (b + t) }\n",
                          encoding="utf-8")
        imported = 'import "std/math/_bundle_helper" as helper\n' + stock.replace(
            "return a + (b - a) * t", "return helper.blend(a, b, t)")
        math_source.write_text(imported, encoding="utf-8")
        run([args.producer, stdlib, "--bundle", *pending[:2]], cwd=root)
        assert pending[0].read_bytes() != prior[0]
        before = [path.read_bytes() for path in pending]
        failure = run([args.producer, stdlib, "--bundle", *pending], cwd=root, expected_exit=1)
        assert b"cannot project the complete dependency-free Checked interface" in failure.stderr
        assert [path.read_bytes() for path in pending] == before


def unrepresented_exports(args, root, stdlib, math_source, stock, pending):
    fixtures = {
        "enum": (
            "export enum VerifiedMark { Ready }",
            "import { VerifiedMark } from math\n"
            "fn observed() -> i64 {\n"
            "    const v = VerifiedMark.Ready\n"
            "    if (v == VerifiedMark.Ready) { return 42 }\n"
            "    return 0\n"
            "}\n"
            "print(observed())\n",
        ),
        "interface": (
            "export interface FutureMathMode { mode(value: i64) -> i64 }",
            "import { FutureMathMode } from math\nprint(42)\n",
        ),
    }
    before = [path.read_bytes() for path in pending]
    env = dict(os.environ, XRAY_STDLIB_PATH=str(stdlib), NO_COLOR="1")
    for name, (declaration, guest) in fixtures.items():
        math_source.write_text(f"{stock}\n{declaration}\n", encoding="utf-8")
        entry = root / f"import-{name}.xr"
        entry.write_text(guest, encoding="utf-8")
        binary = root / f"import-{name}{'.exe' if os.name == 'nt' else ''}"
        vm = run([args.xray, "run", entry], cwd=root, env=env)
        run([args.xray, "build", entry, "-o", binary, "--cc", args.compiler], cwd=root, env=env)
        native = run([binary], cwd=root, env=env)
        assert vm.stdout == native.stdout == b"42\n" and not vm.stderr and not native.stderr
        failure = run([args.producer, stdlib, "--bundle", *pending], cwd=root, expected_exit=1)
        assert b"cannot project the complete dependency-free Checked interface" in failure.stderr
        assert [path.read_bytes() for path in pending] == before


def expected_interface(source):
    functions = re.findall(r"^export\s+fn\s+(\w+)\s*\(([^)]*)\)\s*->\s*(\w+)\s*\{", source, re.M)
    slots = re.findall(r"^export\s+const\s+(\w+)\s*:\s*(\w+)\s*=", source, re.M)
    assert (len(functions), len(slots)) == (37, 14)
    result = []
    for name, parameters, output in functions:
        labels = ", ".join(item.strip() for item in parameters.split(",") if item.strip())
        result.append(f"fn {name}({labels}) -> {output}")
    result.extend(f"const {name} {type_name}" for name, type_name in slots)
    return result


def link_archive(args, root, empty):
    consumer = root / "consumer.c"
    consumer.write_text(
        r'''#include <stdint.h>
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif
#ifdef XRAY_TEST_MATH_INTERFACE
#include "xir/xxir_builtin_stdlib.h"
#include "xir/xxir_target.h"
#include <assert.h>
#endif
extern double xray_stdlib_math_lerp(double, double, double);
extern double xray_stdlib_math_abs(double);

typedef double (*Unary)(double);
typedef double (*Ternary)(double, double, double);

#ifdef XRAY_TEST_MATH_INTERFACE
static bool named(XrXirLiteral name, const char *s) {
    return name.length == strlen(s) && !memcmp(name.bytes, s, name.length);
}

static const char *type_name(XrXirType type) {
    switch (type) {
    case XR_XIR_BOOL: return "bool";
    case XR_XIR_I64: return "i64";
    case XR_XIR_F64: return "f64";
    default: assert(false); return "";
    }
}

static void check_initializer(const XrXirBuiltinStdlibModule *module) {
    assert(module->slot_count == 14 && module->initializer_count == 35);
    assert(module->initializer_error == XR_XIR_ERROR &&
           module->initializer[module->initializer_count - 1].op == XR_XIR_RETURN);
    bool initialized[14] = {false};
    for (uint32_t i = 0; i < module->initializer_count; ++i) {
        const XrXirInstruction *op = module->initializer + i;
        if (op->op != XR_XIR_SLOT_INIT) continue;
        assert(op->immediate >= 0 && op->immediate < module->slot_count && op->args[0] < i);
        uint32_t slot = (uint32_t)op->immediate;
        assert(!initialized[slot]);
        initialized[slot] = true;
    }
    for (uint32_t i = 0; i < module->slot_count; ++i) assert(initialized[i]);
}

static void load_bindings(Unary *absolute, Ternary *blend) {
    const XrXirBuiltinStdlibModule *module = &xray_stdlib_math_interface;
    assert(named(module->canonical, "stdlib-module-v1:module=4:math:path=12:math/math.xr"));
    assert(named(module->logical_path, "math/math.xr"));
    assert(xr_xir_target_is_host(&module->producer_target));
    assert(module->function_count == 37 && module->binding_count == 2);
    check_initializer(module);
    for (uint32_t i = 0; i < module->binding_count; ++i) {
        const XrXirBuiltinScalarBinding *binding = &module->bindings[i];
        assert(binding->function_row < module->function_count);
        const XrXirBuiltinFunction *function = &module->functions[binding->function_row];
        assert(function->parameter_count == (uint32_t)binding->abi &&
            function->result.scalar == XR_XIR_F64 && !function->result.array &&
            function->control.suspend == XR_XIR_EFFECT_NONE && function->control.throws == XR_XIR_EFFECT_NONE &&
            !function->root.requires_root && !function->root.unresolved);
        for (uint32_t p = 0; p < function->parameter_count; ++p)
            assert(function->parameters[p].mode == XR_PARAM_READ &&
                function->parameters[p].type.scalar == XR_XIR_F64 && !function->parameters[p].type.array);
        if (binding->abi == XR_XIR_BUILTIN_F64_UNARY) {
            assert(!*absolute && named(function->name, "abs") && binding->entry.unary == xray_stdlib_math_abs);
            *absolute = binding->entry.unary;
        } else {
            assert(binding->abi == XR_XIR_BUILTIN_F64_TERNARY && !*blend &&
                named(function->name, "lerp") && binding->entry.ternary == xray_stdlib_math_lerp);
            *blend = binding->entry.ternary;
        }
    }
    assert(*absolute && *blend);
    for (uint32_t i = 0; i < module->function_count; ++i) {
        const XrXirBuiltinFunction *function = &module->functions[i];
        bool rest = named(function->name, "min") || named(function->name, "max");
        assert(function->error == XR_XIR_NEVER && !function->result.array);
        assert(!!(function->promises & XR_XIR_FUNCTION_REST) == rest);
        printf("fn %.*s(", (int)function->name.length, function->name.bytes);
        for (uint32_t p = 0; p < function->parameter_count; ++p) {
            const XrXirBuiltinParameter *parameter = &function->parameters[p];
            assert(parameter->mode == XR_PARAM_READ && parameter->type.array == (rest && p + 1 == function->parameter_count));
            printf("%s%s%.*s: %s", p ? ", " : "", parameter->type.array ? "..." : "",
                (int)parameter->name.length, parameter->name.bytes, type_name(parameter->type.scalar));
        }
        printf(") -> %s\n", type_name(function->result.scalar));
    }
    for (uint32_t i = 0; i < module->slot_count; ++i) {
        const XrXirBuiltinSlot *slot = &module->slots[i];
        printf("const %.*s %s\n", (int)slot->name.length, slot->name.bytes, type_name(slot->type));
    }
    fputs("source ", stdout);
    for (size_t i = 0; i < sizeof(module->source_fingerprint.bytes); ++i)
        printf("%02x", (unsigned)module->source_fingerprint.bytes[i]);
    putchar('\n');
}
#endif

static int check_abs(Unary absolute) {
    static const uint64_t positive[][2] = {
        {UINT64_C(0), UINT64_C(0)},
        {UINT64_C(1), UINT64_C(1)},
        {UINT64_C(0x4045000000000000), UINT64_C(0x4045000000000000)},
        {UINT64_C(0x7fefffffffffffff), UINT64_C(0x7fefffffffffffff)},
        {UINT64_C(0x7ff0000000000000), UINT64_C(0x7ff0000000000000)},
        {UINT64_C(0x7ff8123456789abc), UINT64_C(0x7ff8000000000000)},
        {UINT64_C(0x7ff0000000000042), UINT64_C(0x7ff8000000000000)}
    };
    for (size_t i = 0; i < sizeof(positive) / sizeof(*positive); ++i) {
        for (unsigned int negative = 0; negative <= 1; ++negative) {
            uint64_t raw = positive[i][0] | ((uint64_t)negative << 63);
            double x;
            memcpy(&x, &raw, sizeof(x));
            double result = absolute(x);
            memcpy(&raw, &result, sizeof(raw));
            if (raw != positive[i][1]) {
                fprintf(stderr, "math.abs returned unexpected bits for row %zu, sign %u\n", i, negative);
                return 1;
            }
        }
    }
    return 0;
}

int main(void) {
#if defined(_WIN32)
    if (_setmode(_fileno(stdout), _O_BINARY) == -1) return 1;
#endif
#ifdef XRAY_TEST_MATH_INTERFACE
    Unary absolute = NULL;
    Ternary blend = NULL;
    load_bindings(&absolute, &blend);
#else
    Unary absolute = xray_stdlib_math_abs;
    Ternary blend = xray_stdlib_math_lerp;
#endif
    if (check_abs(absolute)) return 1;
    double a = blend(2.0, 10.0, 0.25);
    double b = blend(5.0, -3.0, 1.5);
    return printf("%.1f\n%.1f\n", a, b) < 0;
}
''',
        encoding="ascii",
    )
    binary = root / ("consumer.exe" if os.name == "nt" else "consumer")
    if args.kind == "msvc":
        crt = {"MultiThreaded": "/MT", "MultiThreadedDebug": "/MTd",
               "MultiThreadedDLL": "/MD", "MultiThreadedDebugDLL": "/MDd"}
        command = [args.compiler, "/nologo", "/std:c11", "/W4", "/WX",
                   crt[args.msvc_runtime], consumer, args.archive, "/Fe:" + str(binary)]
        if args.interface:
            command += ["/DXRAY_TEST_MATH_INTERFACE", "/D_CRT_SECURE_NO_WARNINGS",
                        "/I" + str(args.internal_include), "/I" + str(args.internal_include.parent / "include")]
        if args.asan:
            command += ["/fsanitize=address", "/Zi"]
        if args.ubsan and "clang" in args.compiler.name.lower():
            command += ["/clang:-fsanitize=undefined"]
        if args.asan:
            command += ["/link", "/INCREMENTAL:NO"]
    else:
        command = [args.compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   consumer, args.archive, "-o", binary]
        if args.interface:
            command += ["-DXRAY_TEST_MATH_INTERFACE", "-I" + str(args.internal_include),
                        "-I" + str(args.internal_include.parent / "include")]
        if args.asan:
            command.append("-fsanitize=address")
        if args.ubsan:
            command.append("-fsanitize=undefined")
    run(command, cwd=root)
    completed = run([binary], cwd=empty, env=dict(os.environ, XRAY_STDLIB_PATH=str(empty)))
    wanted = b"4.0\n-7.0\n"
    if args.interface:
        source = (args.stdlib / "math/math.xr").read_bytes()
        digest = hashlib.sha256(b"xray-module-source-v1\0" + len(source).to_bytes(8, "little") + source).hexdigest()
        expected = expected_interface(source.decode("utf-8")) + [f"source {digest}"]
        wanted = ("\n".join(expected) + "\n").encode("utf-8") + wanted
    assert completed.stdout == wanted and completed.stderr == b"", (completed.stdout, wanted, completed.stderr)
    assert not list(empty.iterdir())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--producer", type=Path, required=True)
    parser.add_argument("--xray", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--generated", type=Path, required=True)
    parser.add_argument("--interface", type=Path)
    parser.add_argument("--internal-include", type=Path, required=True)
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
    bundle_cases(args, root)
    link_archive(args, root, empty)
    print(f"Same-build standard library archive and authority checks passed: {root}", flush=True)
    subprocess.run(args.native_test[1:], check=True)


if __name__ == "__main__":
    main()

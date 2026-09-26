"""Execute generated-C programs from test_xi_cgen against independent answers.

Each case below is a complete Xray program lowered through the optimized
pipeline by ``test_xi_cgen --case <name> <output.c>``.  This runner compiles
that C strictly with the host compiler, links it, runs it, and compares the
exact stdout with the answer written here once, independently of either
backend.  A non-zero exit, any stderr output, or a different byte fails.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

EXPECTED_STDOUT = {
    "cgen_nullable_unit_enum_compare_execution":
        "false\nfalse\ntrue\nfalse\nfalse\ntrue\ntrue\nfalse\n",
    "cgen_nullable_unit_enum_execution": "true\ntrue\n",
    "cgen_ref_slice_copy_execution": "9\n8\n9\n",
    "cgen_string_copy_execution": "hello\nx\n0\n",
    "cgen_direct_move_array_execution": "3\n2\n",
}


def run(command, cwd, timeout):
    # English compiler diagnostics keep failure reports readable on any console.
    environment = dict(os.environ, VSLANG="1033")
    return subprocess.run(command, cwd=cwd, capture_output=True, timeout=timeout,
                          env=environment)


def strict_compile(args, root, source, executable, folder):
    if args.msvc:
        command = [args.host_compiler, "/nologo", "/std:c11", "/utf-8",
                   "/D_CRT_SECURE_NO_WARNINGS", "/W4", "/WX", "/wd4702",
                   "/external:I" + str(root / "src/aot"), "/external:I" + str(root / "include"),
                   "/external:W0", str(source), "/Fe:" + str(executable),
                   "/Fo:" + str(folder / "program.obj"), str(args.aot_core.resolve(strict=True))]
        if args.msvc_atomics:
            command.append("/experimental:c11atomics")
    else:
        command = [args.host_compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                   "-pedantic-errors", "-isystem", str(root / "src/aot"), "-isystem",
                   str(root / "include"), str(source), str(args.aot_core.resolve(strict=True)),
                   "-o", str(executable), "-lm", "-lpthread"]
    return run(command, folder, 240)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cgen", type=Path, required=True)
    parser.add_argument("--case", choices=tuple(EXPECTED_STDOUT), required=True)
    parser.add_argument("--host-compiler", required=True)
    # The CLI links this archive into every native program: the runtime headers
    # call into the Unicode property tables it carries.
    parser.add_argument("--aot-core", type=Path, required=True)
    parser.add_argument("--msvc", action="store_true")
    parser.add_argument("--msvc-atomics", action="store_true")
    args = parser.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    root = Path(__file__).resolve().parents[2]
    started = time.perf_counter()
    with tempfile.TemporaryDirectory(prefix="xray-cgen-execution-") as temporary:
        folder = Path(temporary)
        source = folder / (args.case + ".c")
        executable = folder / (args.case + ".exe")
        generated = run([str(args.cgen.resolve(strict=True)), "--case", args.case, str(source)],
                        folder, 240)
        if generated.returncode or not source.is_file():
            print(f"{args.case}: C generation failed (exit {generated.returncode})\n" +
                  (generated.stdout + generated.stderr).decode("utf-8", errors="replace"))
            return 1
        compiled = strict_compile(args, root, source, executable, folder)
        if compiled.returncode:
            print(f"{args.case}: strict host compilation failed (exit {compiled.returncode})\n" +
                  (compiled.stdout + compiled.stderr).decode("utf-8", errors="replace"))
            return 1
        executed = run([str(executable)], folder, 60)
        stdout = executed.stdout.decode("utf-8", errors="replace").replace("\r\n", "\n")
        stderr = executed.stderr.decode("utf-8", errors="replace")
        expected = EXPECTED_STDOUT[args.case]
        if executed.returncode or stderr or stdout != expected:
            print(f"{args.case}: FAIL exit={executed.returncode}\n"
                  f"expected stdout: {expected!r}\nactual stdout:   {stdout!r}\n"
                  f"stderr: {stderr!r}")
            return 1
    print(f"{args.case}: generated, strictly compiled, executed PASS "
          f"({time.perf_counter() - started:.3f}s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

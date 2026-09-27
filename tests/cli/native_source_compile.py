"""Compile source fixtures through frozen plans and strict host C compilation."""
import argparse
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def run(command, cwd, timeout=60):
    result = subprocess.run(command, cwd=cwd, capture_output=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"{command!r}: exit {result.returncode}\n" +
                           (result.stdout + result.stderr).decode("utf-8", errors="replace"))


def compile_cases(cases, extra_sources=None, case_timeouts=None, *,
                  runtime_headers_first=False, reject_statement_expressions=False,
                  generated_check=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--host-compiler", required=True)
    parser.add_argument("--msvc", action="store_true")
    parser.add_argument("--msvc-atomics", action="store_true")
    parser.add_argument("--case", choices=tuple(cases), help="Compile one exact source case")
    args = parser.parse_args()
    # Compiler diagnostics may carry bytes the console code page cannot encode;
    # a report must never fail while describing a failure.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if args.case:
        cases = {args.case: cases[args.case]}
    failures = []
    compiler = args.compiler.resolve(strict=True)
    root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix="xray-native-source-") as temporary:
        folder = Path(temporary)
        (folder / "xray.toml").write_text(
            '[project]\nname = "native-source-authority"\nmain = "main.xr"\n'
            '[[export.c]]\nxray = "probe"\nsymbol = "xr_probe"\n'
            'visibility = "hidden"\nabi = "hosted-vm-v1"\nheader = true\n', encoding="utf-8")
        for name, source in (extra_sources or {}).items():
            (folder / name).write_text(source, encoding="utf-8")
        for name, source in cases.items():
            started = time.perf_counter()
            try:
                (folder / "main.xr").write_text(source, encoding="utf-8")
                generated = folder / (name + ".c")
                run([str(compiler), "native-fastpaths", "main.xr", "--output", str(generated),
                     "--header", str(folder / (name + ".h"))], folder,
                    timeout=(case_timeouts or {}).get(name, 60))
                if reject_statement_expressions and re.search(
                        r"\(\s*\{", generated.read_text(encoding="utf-8")):
                    raise RuntimeError("generated C contains a GNU statement expression")
                if generated_check:
                    generated_check(name, generated.read_text(encoding="utf-8"))
                if args.msvc:
                    command = [args.host_compiler, "/nologo", "/std:c11",
                               "/utf-8", "/D_CRT_SECURE_NO_WARNINGS", "/W4", "/WX", "/wd4702",
                               "/external:I" + str(root / "src/aot"),
                               "/external:I" + str(root / "include"), "/external:W0", "/c",
                               str(generated), "/Fo" + str(folder / (name + ".obj"))]
                    if args.msvc_atomics:
                        command.append("/experimental:c11atomics")
                else:
                    command = [args.host_compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                               "-pedantic-errors", "-isystem", str(root / "src/aot"),
                               "-isystem", str(root / "include"), "-c", str(generated),
                               "-o", str(folder / (name + ".o"))]
                if runtime_headers_first:
                    roots = (["/external:I" + str(root / "src/coro")] if args.msvc else
                             ["-isystem", str(root / "src/coro")])
                    command[1:1] = roots
                run(command, folder)
            except (RuntimeError, subprocess.TimeoutExpired, OSError) as error:
                failures.append(name)
                print(f"{name}: FAIL after {time.perf_counter() - started:.3f}s\n{error}", flush=True)
            else:
                print(f"{name}: generated C and strict host compilation PASS "
                      f"({time.perf_counter() - started:.3f}s)", flush=True)
    if failures:
        print("Failed cases: " + ", ".join(failures), flush=True)
        return 1
    return 0

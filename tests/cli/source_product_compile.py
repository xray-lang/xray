"""Compile unchanged source fixtures through the current Source owner and C11."""
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
    return result


def compile_cases(cases, extra_sources=None, case_timeouts=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--host-compiler", required=True)
    parser.add_argument("--msvc", action="store_true")
    parser.add_argument("--msvc-atomics", action="store_true")
    parser.add_argument("--case", choices=tuple(cases), help="Compile one exact source case")
    args = parser.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if args.case:
        cases = {args.case: cases[args.case]}
    compiler = args.compiler.resolve(strict=True)
    root = Path(__file__).resolve().parents[2]
    failures = []
    with tempfile.TemporaryDirectory(prefix="xray-source-c-") as temporary:
        folder = Path(temporary)
        (folder / "xray.toml").write_text(
            '[project]\nname = "native-source-authority"\nmain = "main.xr"\n', encoding="utf-8")
        for name, source in (extra_sources or {}).items():
            (folder / name).write_text(source, encoding="utf-8")
        for name, source in cases.items():
            started = time.perf_counter()
            try:
                entry = folder / "main.xr"
                entry.write_text(source, encoding="utf-8")
                generated = folder / (name + ".c")
                result = run([str(compiler), str(entry), str(generated), str(root / "stdlib")],
                             folder, timeout=(case_timeouts or {}).get(name, 60))
                if re.search(r"\(\s*\{", generated.read_text(encoding="utf-8")):
                    raise RuntimeError("generated C contains a GNU statement expression")
                if args.msvc:
                    command = [args.host_compiler, "/nologo", "/std:c11", "/utf-8",
                               "/D_CRT_SECURE_NO_WARNINGS", "/W4", "/WX",
                               "/I" + str(root / "src"), "/c", str(generated),
                               "/Fo" + str(folder / (name + ".obj"))]
                    if args.msvc_atomics:
                        command.append("/experimental:c11atomics")
                else:
                    command = [args.host_compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                               "-pedantic-errors", "-I", str(root / "src"), "-c", str(generated),
                               "-o", str(folder / (name + ".o"))]
                run(command, folder)
            except (RuntimeError, subprocess.TimeoutExpired, OSError) as error:
                failures.append(name)
                print(f"{name}: FAIL after {time.perf_counter() - started:.3f}s\n{error}", flush=True)
            else:
                print(f"{name}: Source/Checked/C and strict C11 PASS "
                      f"({time.perf_counter() - started:.3f}s)\n" +
                      result.stdout.decode("utf-8", errors="replace"), end="", flush=True)
    if failures:
        print("Failed cases: " + ", ".join(failures), flush=True)
        return 1
    return 0

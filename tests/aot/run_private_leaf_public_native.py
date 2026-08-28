#!/usr/bin/env python3
"""Compile and execute the public private-native-leaf source with MSVC."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT_FUNCTION = re.compile(
    r"XRT_INTERNAL\s+(?:(?:XR_FORCEINLINE|XR_NOINLINE)\s+)?int64_t\s+"
    r"(xr_pf_[0-9a-f]{32})\(xrt_closure_t \*_cl\)\s*\{(?P<body>.*?)\n\}",
    re.DOTALL,
)


def run(argv: list[str], cwd: Path) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(argv, cwd=cwd, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, check=False)


def fail(message: str, result: subprocess.CompletedProcess[bytes] | None = None) -> int:
    print(message, file=sys.stderr)
    if result is not None and result.stdout:
        print(result.stdout.decode("utf-8", errors="replace"), file=sys.stderr)
    return 1


def find_root_symbol(generated: str, source_name: str) -> str | None:
    matches = [
        match.group(1)
        for match in ROOT_FUNCTION.finditer(generated)
        if source_name in match.group("body")
    ]
    return matches[0] if len(matches) == 1 else None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--xray", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--core", type=Path, required=True)
    args = parser.parse_args()

    root = args.root.resolve()
    source = args.source.resolve()
    build_dir = args.build_dir.resolve()
    with tempfile.TemporaryDirectory(prefix="xray-private-leaf-public-",
                                     dir=build_dir) as raw:
        work = Path(raw)
        generated_path = work / "generated.c"
        generation = run([
            str(args.xray.resolve()), "build", "--native", "--rebuild", "-c",
            "-o", str(generated_path), str(source),
        ], root)
        if generation.returncode != 0 or not generated_path.is_file():
            return fail("public Xray source did not generate C", generation)

        generated = generated_path.read_text(encoding="utf-8")
        if "xr_os_core_getpid()" not in generated:
            return fail("generated C has no scalar OS core getpid call")
        if "xrt_os_getpid(" in generated or "__getpid" in generated:
            return fail("generated C revived a tagged or source-name fallback")
        root_symbol = find_root_symbol(generated, source.name)
        if root_symbol is None:
            return fail("generated C does not expose exactly one source root function")

        harness = work / "harness.c"
        harness.write_text(
            "#define main xray_generated_program_main\n"
            "#include \"generated.c\"\n"
            "#undef main\n\n"
            "int main(void) {\n"
            "    xrt_arc_init();\n"
            f"    int64_t process_id = {root_symbol}(NULL);\n"
            "    xrt_arc_shutdown();\n"
            "    return process_id > 0 ? 0 : 2;\n"
            "}\n",
            encoding="utf-8",
            newline="\n",
        )
        executable = work / "private-leaf-public.exe"
        compile_link = run([
            str(args.compiler.resolve()), "/nologo", "/TC", "/std:c11",
            "/experimental:c11atomics", "/utf-8", "/D_CRT_SECURE_NO_WARNINGS",
            "/W4", "/wd4702", f"/external:I{root / 'src' / 'aot'}",
            f"/external:I{root / 'include'}", "/external:W0", str(harness),
            f"/Fe:{executable}", "/link", str(args.core.resolve()),
            "ws2_32.lib", "synchronization.lib",
        ], root)
        if compile_link.returncode != 0 or not executable.is_file():
            return fail("MSVC did not compile and link the generated C harness",
                        compile_link)

        execution = run([str(executable)], work)
        if execution.returncode != 0:
            return fail("generated native program did not return a positive process id",
                        execution)

    print("public private-native-leaf MSVC oracle passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

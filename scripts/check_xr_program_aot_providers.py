#!/usr/bin/env python3
"""Compile real generated C with available strict portable-C providers."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path


def run(command: list[str]) -> None:
    result = subprocess.run(command, check=False, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"{result.stdout}{result.stderr}"
        )


def execute(path: Path, expected: int) -> None:
    result = subprocess.run([str(path)], check=False)
    if result.returncode != expected:
        raise RuntimeError(
            f"{path.name} returned {result.returncode}; expected {expected}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--expected-exit", type=int, required=True)
    args = parser.parse_args()
    source = args.source.resolve()
    output = args.output_dir.resolve()
    try:
        if not source.is_file():
            raise RuntimeError(f"generated C source is missing: {source}")
        text = source.read_text(encoding="utf-8", errors="strict")
        for forbidden in ("({", "typeof(", "__attribute__", "__asm__", "#pragma"):
            if forbidden in text:
                raise RuntimeError(f"provider-specific C residue: {forbidden}")
        output.mkdir(parents=True, exist_ok=True)
        clang = shutil.which("clang")
        zig = shutil.which("zig")
        if not clang or not zig:
            raise RuntimeError("strict provider gate requires both clang and zig")

        clang_binary = output / "generated-clang"
        run(
            [
                clang,
                "-std=c11",
                "-pedantic-errors",
                "-Wall",
                "-Wextra",
                "-Werror",
                str(source),
                "-o",
                str(clang_binary),
            ]
        )
        execute(clang_binary, args.expected_exit)

        zig_binary = output / "generated-zig"
        run(
            [
                zig,
                "cc",
                "-std=c11",
                "-pedantic-errors",
                "-Wall",
                "-Wextra",
                "-Werror",
                str(source),
                "-o",
                str(zig_binary),
            ]
        )
        execute(zig_binary, args.expected_exit)

        providers = ["clang", "zig-cc"]
        clang_cl = shutil.which("clang-cl")
        if clang_cl:
            clang_cl_object = output / "generated-clang-cl.o"
            run(
                [
                    clang_cl,
                    "/nologo",
                    "/TC",
                    "/std:c11",
                    "/W4",
                    "/WX",
                    "/c",
                    f"/Fo{clang_cl_object}",
                    "--",
                    str(source),
                ]
            )
            providers.append("clang-cl")
        print(f"generated-C provider gate: PASS ({', '.join(providers)})")
    except (OSError, UnicodeError, RuntimeError) as exc:
        print(f"generated-C provider gate: FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

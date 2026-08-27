#!/usr/bin/env python3
"""Compile portable-C refusal regressions with the real MSVC provider."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
CASES = {
    "fixed-array": (
        Path("tests/diff/cases/semantics/int_wrap/fixed_array_uint64_lane.xr"),
    ),
    "slice": (
        Path("tests/diff/cases/semantics/slice/array_negative_start.xr"),
        Path("tests/diff/cases/semantics/slice/array_negative_end.xr"),
        Path("tests/diff/cases/semantics/slice/array_negative_both.xr"),
    ),
}


def run(command: list[str], env: dict[str, str], timeout: int = 90) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        command,
        cwd=ROOT,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
        check=False,
    )


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("xray", type=Path)
    parser.add_argument("--family", choices=tuple(CASES), required=True)
    args = parser.parse_args()

    require(sys.platform == "win32", "the portable-C MSVC gate is Windows-only")
    xray = args.xray.resolve()
    require(xray.is_file(), f"xray executable is missing: {xray}")

    coll_header = (ROOT / "src/aot/xrt_coll.h").read_text(encoding="utf-8")
    require(
        "xrt_array_stack_borrow_span_view_typed" not in coll_header,
        "retired GNU borrowed-span macro remains in xrt_coll.h",
    )

    with tempfile.TemporaryDirectory(prefix=f"xray-portable-c11-{args.family}-") as raw_tmp:
        tmp = Path(raw_tmp)
        env = os.environ.copy()
        env["XRAY_AOT_CACHE_DIR"] = str(tmp / "cache")

        for relative in CASES[args.family]:
            source = ROOT / relative
            stem = source.stem
            generated = tmp / f"{stem}.c"
            native = tmp / f"{stem}.exe"

            vm = run([str(xray), "run", str(source)], env)
            require(vm.returncode == 0, f"VM failed for {relative}:\n{vm.stderr.decode(errors='replace')}")

            emit = run(
                [
                    str(xray),
                    "build",
                    "--native",
                    "-c",
                    "--toolchain",
                    "msvc",
                    "--target",
                    "x86_64-windows-msvc",
                    "-o",
                    str(generated),
                    str(source),
                ],
                env,
            )
            require(
                emit.returncode == 0 and generated.is_file(),
                f"generated-C emission failed for {relative}:\n{emit.stderr.decode(errors='replace')}",
            )
            c_text = generated.read_text(encoding="utf-8")
            for token in ("({", "xrt_array_stack_borrow_span_view_typed", "__builtin_alloca"):
                require(token not in c_text, f"non-portable generated-C token {token!r} in {relative}")
            if args.family == "fixed-array":
                require(
                    "xrt_fixed_index_checked(" in c_text,
                    f"fixed-array checked-index owner is missing in {relative}",
                )
            else:
                require(
                    "xrt_array_stack_borrow_span_view_init(&_xspan_print_view_" in c_text,
                    f"scoped borrowed-span view is missing in {relative}",
                )

            build = run(
                [
                    str(xray),
                    "build",
                    "--native",
                    "--toolchain",
                    "msvc",
                    "--target",
                    "x86_64-windows-msvc",
                    "-o",
                    str(native),
                    str(source),
                ],
                env,
            )
            require(
                build.returncode == 0 and native.is_file(),
                f"MSVC native build failed for {relative}:\n{build.stderr.decode(errors='replace')}",
            )
            aot = run([str(native)], env)
            require(aot.returncode == vm.returncode, f"VM/AOT exit mismatch for {relative}")
            require(aot.stdout == vm.stdout, f"VM/AOT stdout mismatch for {relative}")

    print(f"portable C11 MSVC {args.family}: PASS ({len(CASES[args.family])} case(s))")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, subprocess.TimeoutExpired) as exc:
        print(f"portable C11 MSVC regression: FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)

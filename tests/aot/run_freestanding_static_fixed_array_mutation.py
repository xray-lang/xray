#!/usr/bin/env python3
"""Freestanding top-level fixed arrays stay mutable and persist across calls.

A `var` fixed array at top level lowers to file-scope storage. Two shapes would
break it silently: emitting the storage `const` (mutation becomes undefined
behavior the C compiler may fold away), or routing it through `xrt_shared`
(which does not exist in the freestanding profile). The generated C is checked
for both, then compiled and driven by a C harness that mutates and re-reads.
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path
from typing import List


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, toolchain, workspace  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent

SOURCE = SCRIPT_DIR / "filetests" / "link" / "freestanding_top_var_fixed_array_static.xr"
MANIFEST = SOURCE.with_suffix(".toml")
HARNESS = SCRIPT_DIR / "provider" / "freestanding_static_fixed_array_mutation.c"

SKIP_EXIT = 77

# Storage must be emitted as plain file-scope arrays, one scalar and one struct.
REQUIRED_SHAPES = (
    "static int64_t _xctarr_",
    "static struct { int64_t id; int64_t ticks; int64_t scratch[2]; } _xctarr_",
)
# Shapes that would mean the storage is not mutable, or escaped the profile.
FORBIDDEN_SHAPES = (
    ("static const int64_t _xctarr_", "mutable fixed-array storage was emitted const"),
    ("xrt_shared[", "mutable fixed-array storage escaped through xrt_shared"),
)


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(description="Freestanding static fixed-array mutation gate")
    ap.add_argument("xray", nargs="?", default=None)
    ns = ap.parse_args(argv[1:])

    xray = Path(ns.xray or os.environ.get("XRAY_BIN") or (PROJECT_DIR / "build" / "xray"))
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300)

    cc = toolchain.find_c_compiler()
    if cc is None:
        print("SKIP: no C compiler found")
        return SKIP_EXIT

    with workspace.Workspace("xray-freestanding-fixed-array-mutation") as ws:
        (ws.root / "main.xr").write_bytes(SOURCE.read_bytes())
        (ws.root / "xray.toml").write_bytes(MANIFEST.read_bytes())

        generated = ws.path("generated.c")
        result = proc.run(
            [xray, "build", "--native", "--profile", "freestanding", "--rebuild",
             "-c", "-o", generated, "main.xr"],
            cwd=ws.root, timeout=timeout,
        )
        if not result.ok:
            sys.stderr.write("freestanding build failed\n")
            sys.stderr.write(result.combined_text()[:8000])
            return 1

        text = generated.read_text(encoding="utf-8")
        for needle in REQUIRED_SHAPES:
            if needle not in text:
                sys.stderr.write(f"FAIL: generated C missing expected storage shape: {needle}\n")
                return 1
        for needle, message in FORBIDDEN_SHAPES:
            if needle in text:
                sys.stderr.write(f"FAIL: {message}\n")
                return 1

        obj = ws.path("generated.o")
        compile_result = proc.run(
            [cc.path, "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-Dmain=xray_generated_main",
             f"-I{PROJECT_DIR / 'src' / 'aot'}", f"-I{PROJECT_DIR / 'include'}",
             "-c", generated, "-o", obj],
            timeout=timeout,
        )
        if not compile_result.ok:
            sys.stderr.write("generated C failed to compile\n")
            sys.stderr.write(compile_result.combined_text()[:8000])
            return 1

        harness_bin = ws.path("mutation-harness")
        link_result = proc.run(
            [cc.path, "-std=c11", "-Wall", "-Wextra", "-Werror",
             HARNESS, obj, "-o", harness_bin],
            timeout=timeout,
        )
        if not link_result.ok:
            sys.stderr.write("mutation harness failed to link\n")
            sys.stderr.write(link_result.combined_text()[:8000])
            return 1

        run_result = proc.run([harness_bin], timeout=timeout)
        if not run_result.ok:
            sys.stderr.write("mutation harness failed\n")
            sys.stderr.write(run_result.combined_text()[:8000])
            return 1

    print("PASS: freestanding mutable scalar/matrix/cube/struct fixed arrays persist across calls")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

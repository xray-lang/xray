#!/usr/bin/env python3
"""Extern layout survives the bytecode round-trip, deterministically.

Two properties, and the second is the one worth having:

  1. compiling the same source twice produces byte-identical bytecode -- so
     extern layout resolution carries no address, timestamp or hash-order
     nondeterminism into the artifact;
  2. running the bytecode gives the same output as running the source.

Property 1 is what makes a bytecode artifact cacheable and comparable at all; a
layout that embedded, say, a pointer would still satisfy property 2 while making
every build differ.

Usage: run_extern_layout_bytecode_roundtrip.py <xray> <entry.xr> <layout-lib.xr>
"""

from __future__ import annotations

import shutil
import sys
from pathlib import Path
from typing import List


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, workspace  # noqa: E402

# The library must keep this exact name: the entry imports it by name.
LAYOUT_LIB_NAME = "_extern_layout_bytecode_lib.xr"


def main(argv: List[str]) -> int:
    if len(argv) != 4:
        sys.stderr.write(f"usage: {argv[0]} <xray> <entry.xr> <layout-lib.xr>\n")
        return 2

    xray, entry_src, layout_lib = Path(argv[1]), Path(argv[2]), Path(argv[3])
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300)

    with workspace.Workspace("xray-layout-bytecode") as ws:
        entry = ws.path("entry.xr")
        shutil.copy2(entry_src, entry)
        shutil.copy2(layout_lib, ws.path(LAYOUT_LIB_NAME))

        source_run = proc.run([xray, "run", entry], timeout=timeout)
        if not source_run.ok:
            sys.stderr.write("running the source failed\n")
            sys.stderr.write(source_run.combined_text())
            return 1

        first, second = ws.path("entry-a.xrc"), ws.path("entry-b.xrc")
        for out in (first, second):
            compiled = proc.run([xray, "compile", "-f", "bytecode", "-o", out, entry],
                                timeout=timeout)
            if not compiled.ok:
                sys.stderr.write("bytecode compilation failed\n")
                sys.stderr.write(compiled.combined_text())
                return 1

        if first.read_bytes() != second.read_bytes():
            sys.stderr.write(
                "bytecode is not deterministic: two compilations of the same source "
                "produced different artifacts\n")
            return 1

        bytecode_run = proc.run([xray, "run", first], timeout=timeout)
        if not bytecode_run.ok:
            sys.stderr.write("running the bytecode failed\n")
            sys.stderr.write(bytecode_run.combined_text())
            return 1

        if source_run.stdout != bytecode_run.stdout:
            sys.stderr.write("source and bytecode runs disagree\n")
            sys.stderr.write(f"  source:   {source_run.stdout!r}\n")
            sys.stderr.write(f"  bytecode: {bytecode_run.stdout!r}\n")
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

#!/usr/bin/env python3
"""Extern layout produces a deterministic offline C bytecode container.

Compiling the same source twice must produce byte-identical container bytes, so
extern layout resolution carries no address, timestamp, or hash-order
nondeterminism into the artifact.

The source run remains the semantic smoke test. Standalone XRC execution is a
retired product route, so this gate compares the two serializer outputs rather
than publishing or running an XRC artifact.

Usage: run_extern_layout_bytecode_roundtrip.py <xray> <entry.xr> <layout-lib.xr>
"""

from __future__ import annotations

import shutil
import sys
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, workspace  # noqa: E402

# The library must keep this exact name: the entry imports it by name.
LAYOUT_LIB_NAME = "_extern_layout_bytecode_lib.xr"


def main(argv: list[str]) -> int:
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

        first, second = ws.path("entry-a.c"), ws.path("entry-b.c")
        for out in (first, second):
            compiled = proc.run([xray, "compile", "-f", "c", "-o", out, entry],
                                timeout=timeout)
            if not compiled.ok:
                sys.stderr.write("C-container compilation failed\n")
                sys.stderr.write(compiled.combined_text())
                return 1

        if first.read_bytes() != second.read_bytes():
            sys.stderr.write(
                "C-container serialization is not deterministic: two compilations "
                "produced different artifacts\n")
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

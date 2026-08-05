#!/usr/bin/env python3
"""Assert the optimized native shape of unchecked scalar memory access.

`mem.load*/store*` on a raw pointer is documented as compiling to a bare machine
load/store. This gate reads the actual disassembly and fails if either function
gained a call, a conditional branch, or a re-entry into a checked runtime
helper -- the shapes that would mean the unchecked path silently regained a
bounds check or a helper dispatch.

Skips (exit 77) when no disassembler is installed; a shape gate with no
disassembler proves nothing and must not report a pass.
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import binary as binlib  # noqa: E402
from xraytest import platform, proc, workspace  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent
FIXTURE = SCRIPT_DIR / "filetests" / "cgen" / "mem_load_store_rawptr_shape.xr"

SKIP_EXIT = 77

STRAIGHT_LINE_SYMBOLS = ("xray_mem_u32_load_le", "xray_mem_u32_store_le")

# Re-entering any of these means the unchecked access went back through a
# checked or runtime-mediated path.
import re  # noqa: E402

RUNTIME_HELPER_RE = re.compile(r"xr_array_core|xrt_(ptr|endian_arg|has_pending_error)")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Raw scalar assembly shape gate")
    ap.add_argument("xray", nargs="?", default=None)
    ns = ap.parse_args(argv[1:])

    xray = Path(ns.xray or os.environ.get("XRAY_BIN") or (PROJECT_DIR / "build" / "xray"))
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300)

    with workspace.Workspace("xray-raw-scalar-asm") as ws:
        source = ws.path(FIXTURE.name)
        source.write_bytes(FIXTURE.read_bytes())
        manifest = FIXTURE.with_suffix(".toml")
        if manifest.is_file():
            (ws.root / "xray.toml").write_bytes(manifest.read_bytes())

        out_bin = ws.path("raw-scalar")
        result = proc.run(
            [xray, "build", "--native", "-O2", "--shared", "--rebuild",
             "-o", out_bin, source],
            timeout=timeout,
        )
        if not result.ok:
            sys.stderr.write("raw scalar assembly gate: native build failed\n")
            sys.stderr.write(result.combined_text()[:8000])
            return 1

        disassembly = binlib.disassemble(out_bin, timeout=timeout)
        if disassembly is None:
            print("raw scalar assembly gate: no supported disassembler; skipped")
            return SKIP_EXIT

        for symbol in STRAIGHT_LINE_SYMBOLS:
            body = binlib.extract_symbol_body(disassembly, symbol)
            if not body.strip():
                sys.stderr.write(f"raw scalar assembly gate: missing symbol {symbol}\n")
                return 1
            if binlib.has_control_flow(body):
                sys.stderr.write(
                    f"raw scalar assembly gate: {symbol} contains a call or conditional branch\n")
                sys.stderr.write("\n".join(body.splitlines()[:80]) + "\n")
                return 1
            if RUNTIME_HELPER_RE.search(body):
                sys.stderr.write(
                    f"raw scalar assembly gate: {symbol} re-entered a checked/runtime helper\n")
                sys.stderr.write("\n".join(body.splitlines()[:80]) + "\n")
                return 1

    print("raw scalar assembly gate: straight-line unchecked load/store PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

#!/usr/bin/env python3
"""The shared numeric conversion core compiles for every freestanding target,
without libc and without a memory-copy dependency.

Two assertions per target. The compile itself proves the core is
self-contained C; the undefined-symbol check proves the compiler did not lower
a struct assignment into a memcpy/memmove call, which would need a libc that a
freestanding image does not have.

Then each target's SDK compile probe must report ready, so the gate also covers
the toolchain plumbing rather than only this one translation unit.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import sys
from pathlib import Path
from typing import List, Optional, Tuple


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import binary as binlib  # noqa: E402
from xraytest import platform, proc, workspace  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent
CASE = SCRIPT_DIR / "provider" / "freestanding_numeric_core.c"

SKIP_EXIT = 77

# (target triple, optional -mcpu)
TARGETS: Tuple[Tuple[str, Optional[str]], ...] = (
    ("riscv32-freestanding-none", None),
    ("riscv64-freestanding-none", None),
    ("thumb-freestanding-eabi", "cortex_m4"),
    ("x86_64-freestanding-none", None),
)

MEMCPY_RE = re.compile(r"mem(cpy|move)")

CFLAGS = (
    "-std=c11", "-O2", "-fno-inline", "-ffreestanding", "-fno-builtin",
    "-fno-stack-protector", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
)


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(description="Freestanding numeric core compile gate")
    ap.add_argument("xray", nargs="?", default=None)
    ns = ap.parse_args(argv[1:])

    xray = Path(ns.xray or os.environ.get("XRAY_BIN") or (PROJECT_DIR / "build" / "xray"))
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300)

    zig = os.environ.get("XRAY_ZIG") or shutil.which("zig")
    if not zig:
        print("SKIP: zig not found")
        return SKIP_EXIT
    if binlib.find_nm() is None:
        print("SKIP: nm not found")
        return SKIP_EXIT
    if not (xray.is_file() and os.access(xray, os.X_OK)):
        print(f"SKIP: xray binary not found: {xray}")
        return SKIP_EXIT

    with workspace.Workspace("xray-freestanding-numeric") as ws:
        probe_home = ws.subdir("home")
        zig_env = dict(os.environ)
        zig_env["ZIG_GLOBAL_CACHE_DIR"] = str(ws.path("zig-global-cache"))
        zig_env["ZIG_LOCAL_CACHE_DIR"] = str(ws.path("zig-local-cache"))

        for target, cpu in TARGETS:
            obj = ws.path(f"{target}.o")
            argv_cc = [zig, "cc", "-target", target]
            if cpu:
                argv_cc.append(f"-mcpu={cpu}")
            argv_cc.extend([*CFLAGS, f"-I{PROJECT_DIR / 'src'}", "-c", CASE, "-o", obj])
            result = proc.run(argv_cc, env=zig_env, timeout=timeout)
            if not result.ok:
                sys.stderr.write(result.combined_text()[:8000])
                sys.stderr.write(f"FAIL: numeric core did not compile for {target}\n")
                return 1

            undefined = binlib.undefined_symbol_names(obj, timeout=timeout) or []
            leaked = [n for n in undefined if MEMCPY_RE.search(n)]
            if leaked:
                for name in leaked:
                    sys.stderr.write(f"  {name}\n")
                sys.stderr.write(
                    f"FAIL: numeric core retained a memory-copy dependency for {target}\n")
                return 1

        probe_env = dict(os.environ)
        probe_env["HOME"] = str(probe_home)
        for target, _cpu in TARGETS:
            result = proc.run(
                [xray, "toolchain", "probe", "--target", target, "--provider", "zig",
                 "--zig", zig, "--profile", "freestanding", "--no-run", "--refresh", "--json"],
                env=probe_env, timeout=timeout,
            )
            output = result.combined_text()
            if not result.ok:
                sys.stderr.write(output[:8000])
                sys.stderr.write(f"FAIL: freestanding SDK compile probe failed for {target}\n")
                return 1
            if '"sdkCompile":"ok"' not in output.replace(" ", ""):
                sys.stderr.write(output[:8000])
                sys.stderr.write(f"FAIL: freestanding SDK compile was not ready for {target}\n")
                return 1

    print("PASS: freestanding numeric core compiled without libc for RV32/RV64/Thumb/x86")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

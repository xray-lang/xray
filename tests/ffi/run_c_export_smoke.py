#!/usr/bin/env python3
"""Manifest-driven C export smoke test.

The positive half goes all the way through: generate C plus a header, compile
both the generated unit and an external C caller with a real compiler, link
them, and run. Stopping at "the header mentions the symbol" would not show that
the symbol is callable from C, which is the whole point of a manifest export.

The negative half pins the schema failing closed -- a duplicate symbol and a
managed (non-C-ABI) signature must both be rejected at build time rather than
producing a header that lies about what C can call.

Environment:
    CC                  C compiler (default: cc)
    XRAY_FFI_KEEP_WORK  1 = keep the work directory and print its path

Usage: run_c_export_smoke.py [xray_binary]
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path
from typing import Sequence


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, workspace  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent
FIXTURE_ROOT = PROJECT_DIR / "tests" / "fixtures" / "manifest_export"

MANAGED_REJECTION = re.compile(r"C ABI|managed|export")


class Recorder:
    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0

    def ok(self, name: str) -> None:
        print(f"  PASS: {name}")
        self.passed += 1

    def bad(self, name: str, *logs: str) -> None:
        print(f"  FAIL: {name}")
        self.failed += 1
        for text in logs:
            for line in text.splitlines()[:140]:
                print(f"      {line}")

    def expect_contains(self, path: Path, needle: str, name: str) -> None:
        text = path.read_text(encoding="utf-8") \
            if path.is_file() else None
        if text is not None and needle in text:
            self.ok(name)
        else:
            self.bad(name, text or "")


def main(argv: list[str]) -> int:
    xray = Path(argv[1] if len(argv) > 1
                else os.environ.get("XRAY_BIN",
                                    str(PROJECT_DIR / "build" / "xray"))).resolve()
    if not (xray.is_file() and os.access(xray, os.X_OK)):
        sys.stderr.write(f"FAIL: xray binary not executable: {xray}\n")
        return 1
    # Plain CC, as the shell used: this links and runs real code, so a fallback
    # to some other compiler on PATH would quietly change what was tested.
    cc = os.environ.get("CC", "cc")
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 600)
    keep = platform.env_flag("XRAY_FFI_KEEP_WORK")

    rec = Recorder()
    with workspace.Workspace("xray_manifest_export", keep=keep) as ws:
        w = ws.root
        gen_c, gen_h = w / "generated.c", w / "exports.h"

        build = proc.run([xray, "build", "--native", "--c-only",
                          "--c-header", gen_h, "-o", gen_c, "main.xr"],
                         cwd=FIXTURE_ROOT / "positive", timeout=timeout)
        if build.ok:
            rec.ok("manifest export generates C and header")
        else:
            rec.bad("manifest export generates C and header", build.combined_text())

        rec.expect_contains(gen_h, "xr_add_i32", "header exposes manifest symbol")
        rec.expect_contains(gen_c, "xr_add_i32",
                            "generated C defines manifest symbol")

        # Compile, link and run an external C caller against the generated unit.
        gen_o, caller_o = w / "generated.o", w / "caller.o"
        caller_bin = w / platform.exe_name("caller")
        steps = (
            [cc, "-std=c11", "-Dmain=xray_generated_main",
             "-I", PROJECT_DIR / "include", "-I", PROJECT_DIR / "src" / "aot",
             "-c", gen_c, "-o", gen_o],
            [cc, "-std=c11", "-I", w, "-c",
             FIXTURE_ROOT / "positive" / "caller.c", "-o", caller_o],
            [cc, gen_o, caller_o, "-pthread", "-lm", "-o", caller_bin],
            [caller_bin],
        )
        logs: list[str] = []
        linked = True
        for step in steps:
            result = proc.run(step, timeout=timeout)
            logs.append(result.combined_text())
            if not result.ok:
                linked = False
                break
        if linked:
            rec.ok("external C caller links and observes 42")
        else:
            rec.bad("external C caller links and observes 42", *logs)

        # A duplicate manifest symbol must be a schema error, not a silent win.
        dup = proc.run([xray, "build", "--native", "--c-only",
                        "-o", w / "duplicate.c", "main.xr"],
                       cwd=FIXTURE_ROOT / "duplicate", timeout=timeout)
        name = "duplicate manifest symbol is rejected"
        if dup.ok:
            rec.bad(name)
        elif "E-EXPORT-SCHEMA" in dup.combined_text():
            rec.ok(name)
        else:
            rec.bad(name, dup.combined_text())

        # A managed signature cannot cross the C ABI and must be refused.
        managed = proc.run([xray, "build", "--native", "--c-only",
                            "-o", w / "managed.c", "main.xr"],
                           cwd=FIXTURE_ROOT / "managed", timeout=timeout)
        name = "managed export signature is rejected"
        if managed.ok:
            rec.bad(name)
        elif MANAGED_REJECTION.search(managed.combined_text()):
            rec.ok(name)
        else:
            rec.bad(name, managed.combined_text())

        # Multi-module C-only output must be a single compilable unit.
        amalgam = proc.run([xray, "build", "--native", "--c-only",
                            "-o", w / "amalgam.c", "main.xr"],
                           cwd=FIXTURE_ROOT / "amalgam", timeout=timeout)
        name = "multi-module C-only output is one compilable translation unit"
        if amalgam.ok:
            compiled = proc.run(
                [cc, "-std=c11", "-I", PROJECT_DIR / "include",
                 "-I", PROJECT_DIR / "src" / "aot", "-c", w / "amalgam.c",
                 "-o", w / "amalgam.o"], timeout=timeout)
            if compiled.ok:
                rec.ok(name)
            else:
                rec.bad(name, amalgam.combined_text(), compiled.combined_text())
        else:
            rec.bad(name, amalgam.combined_text())

        if keep:
            print(f"Work dir: {w}")

    print("")
    print(f"Manifest C export smoke: {rec.passed} passed, {rec.failed} failed")
    return 1 if rec.failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

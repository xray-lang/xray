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
import shutil
import sys
from pathlib import Path
from typing import Sequence


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, workspace, toolchain  # noqa: E402

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
    cc = toolchain.find_c_syntax_compiler()
    pinned = os.environ.get("CC")
    if not cc or (pinned and shutil.which(pinned) != cc.path):
        sys.stderr.write("FAIL: requested C compiler is unavailable\n")
        return 1

    def compile_c(source: Path, obj: Path, includes: Sequence = (), defines: Sequence = ()) -> list:
        if cc.driver == toolchain.CC_DRIVER_MSVC:
            return [cc.path, "/nologo", "/TC", "/std:c11", "/experimental:c11atomics",
                    "/utf-8", "/W4", "/WX", *[f"/I{x}" for x in includes],
                    *[f"/D{x}" for x in defines], "/c", source, f"/Fo{obj}"]
        return [cc.path, *(["cc"] if cc.driver == toolchain.CC_DRIVER_ZIG else []),
                "-std=c11", *[f"-I{x}" for x in includes], *[f"-D{x}" for x in defines],
                "-c", source, "-o", obj]

    def link_c(objects: Sequence, binary: Path) -> list:
        if cc.driver == toolchain.CC_DRIVER_MSVC:
            return [cc.path, "/nologo", *objects, f"/Fe{binary}"]
        return [cc.path, *(["cc"] if cc.driver == toolchain.CC_DRIVER_ZIG else []),
                *objects, "-pthread", "-lm", "-o", binary]
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
            compile_c(gen_c, gen_o, [PROJECT_DIR / "include", PROJECT_DIR / "src" / "aot"],
                      ["main=xray_generated_main"]),
            compile_c(FIXTURE_ROOT / "positive" / "caller.c", caller_o, [w]),
            link_c([gen_o, caller_o], caller_bin),
            [caller_bin],
        )
        logs: list[str] = []
        linked = True
        for step in steps:
            result = proc.run(step, cwd=w, timeout=timeout)
            logs.append(result.combined_text())
            if not result.ok:
                linked = False
                break
        if linked:
            rec.ok("external C caller links and observes 42")
        else:
            rec.bad("external C caller links and observes 42", *logs)

        # Independent scalar ABI expectations include roots unused by the entry.
        scalar = w / "scalar"
        scalar.mkdir()
        types = ("i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64")
        source = 'import { plus } from "./helper"\n' + "\n".join(f"fn identity_{t}(x:{t})->{t} {{ return x }}" for t in types)
        source += "\nfn invert(x:bool)->bool { return !x }\nfn noop() {}\n"
        source += "fn helper(x:i32)->i32 { return x + 2 }\n"
        source += "fn indirect(x:i32)->i32 { return helper(x) }\n"
        source += "fn imported(x:i32)->i32 { return plus(x) }\n"
        source += "fn hiddenAnswer()->i64 { return 42 }\n"
        (scalar / "helper.xr").write_text("export fn plus(x:i32)->i32 {return x + 3}\n", encoding="utf-8")
        (scalar / "main.xr").write_text(source, encoding="utf-8")
        manifest = '[package]\nname="tests/scalar-exports"\nversion="1.0.0"\nmain="main.xr"\n'
        for name in [*(f"identity_{t}" for t in types), "invert", "noop", "indirect", "imported"]:
            manifest += f'[[export.c]]\nxray="{name}"\nsymbol="public_{name}"\nheader=true\n'
        manifest += ('[[export.c]]\nxray="hiddenAnswer"\nsymbol="private_answer"\n'
                     'header=false\nvisibility="hidden"\n')
        (scalar / "xray.toml").write_text(manifest, encoding="utf-8")
        built = proc.run([xray, "build", "--native", "--c-only", "--c-header", scalar / "exports.h",
                          "-o", scalar / "generated.c", "main.xr"], cwd=scalar, timeout=timeout)
        checks = [f"public_identity_{t}({limit}) == {limit}" for t, limit in (
            ("i8", "INT8_MIN"), ("u8", "UINT8_MAX"), ("i16", "INT16_MIN"), ("u16", "UINT16_MAX"),
            ("i32", "INT32_MIN"), ("u32", "UINT32_MAX"), ("i64", "INT64_MIN"), ("u64", "UINT64_MAX"))]
        checks += ["public_invert(false)", "!public_invert(true)", "public_indirect(40) == 42",
                   "private_answer() == 42", "public_indirect(41) == 43", "public_imported(39) == 42"]
        (scalar / "caller.c").write_text(
            '#include "exports.h"\nint64_t private_answer(void);\nint main(void) {\n'
            'public_noop();\nreturn (' + ' && '.join(checks) + ') ? 0 : 1;\n}\n', encoding="utf-8")
        scalar_bin = scalar / platform.exe_name("caller")
        scalar_steps = [compile_c(scalar / "generated.c", scalar / "generated.o",
                                 defines=["main=xray_generated_main"]),
                        compile_c(scalar / "caller.c", scalar / "caller.o", [scalar]),
                        link_c([scalar / "generated.o", scalar / "caller.o"], scalar_bin), [scalar_bin]]
        passed = built.ok
        logs = [built.combined_text()]
        if passed:
            for step in scalar_steps:
                result = proc.run(step, cwd=scalar, timeout=timeout)
                logs.append(result.combined_text())
                if not result.ok:
                    passed = False
                    break
        name = "Program C exports preserve integer bounds, bool, void and transitive calls"
        if passed:
            rec.ok(name)
        else:
            rec.bad(name, *logs)
        header_text = (scalar / "exports.h").read_text(encoding="utf-8") if built.ok else ""
        if passed and "private_answer" not in header_text:
            rec.ok("header=false preserves callable hidden export without public prototype")
        else:
            rec.bad("header=false preserves callable hidden export without public prototype", header_text)

        native_binary = scalar / platform.exe_name("native_program")
        native_build = proc.run([xray, "build", "--native", "-o", native_binary, "main.xr"],
                                cwd=scalar, timeout=timeout)
        native_run = proc.run([native_binary], cwd=scalar, timeout=timeout) if native_build.ok else None
        if native_run and native_run.ok and native_run.stdout == b"":
            rec.ok("native CLI builds and runs the Program containing C exports")
        else:
            rec.bad("native CLI builds and runs the Program containing C exports",
                    native_build.combined_text(), native_run.combined_text() if native_run else "")

        for name, source in (
            ("direct module state", "var state:i32=42\nfn bad()->i32 {return state}\n"),
            ("transitive module state", "var state:i32=42\nfn helper()->i32 {return state}\n"
                                        "fn bad()->i32 {return helper()}\n"),
            ("managed boundary", "fn bad(x:string)->i32 {return 42}\n"),
        ):
            (scalar / "main.xr").write_text(source, encoding="utf-8")
            (scalar / "xray.toml").write_text(
                '[package]\nname="tests/export-reject"\nversion="1.0.0"\nmain="main.xr"\n'
                '[[export.c]]\nxray="bad"\nsymbol="public_bad"\nheader=true\n', encoding="utf-8")
            rejected_c, rejected_h = scalar / "rejected.c", scalar / "rejected.h"
            sentinel = "existing output must survive admission failure"
            rejected_c.write_text(sentinel, encoding="utf-8")
            rejected_h.write_text(sentinel, encoding="utf-8")
            rejected = proc.run([xray, "build", "--native", "--c-only", "--c-header", rejected_h,
                                 "-o", rejected_c, "main.xr"], cwd=scalar, timeout=timeout)
            if (not rejected.ok and "C export binding failed: binding-rejected" in rejected.combined_text()
                    and rejected_c.read_text(encoding="utf-8") == sentinel
                    and rejected_h.read_text(encoding="utf-8") == sentinel):
                rec.ok(f"C ABI rejects {name} before publishing outputs")
            else:
                rec.bad(f"C ABI rejects {name} before publishing outputs", rejected.combined_text())

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
                compile_c(w / "amalgam.c", w / "amalgam.o",
                          [PROJECT_DIR / "include", PROJECT_DIR / "src" / "aot"]),
                cwd=w, timeout=timeout)
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

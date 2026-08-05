#!/usr/bin/env python3
"""Manifest-first AOT link smoke: typed plans reach the link command.

The broad source/code-shape matrix lives in run_aot_filetests.py --mode link.
This gate stays deliberately small and proves four things:

  - the language surface exposes exactly the nine public attributes, and none of
    the retired ones came back;
  - an audited prebuilt dynamic library declared in xray.toml (not in source)
    links and runs;
  - reachability decides what gets linked -- a core math call pulls -lm and not
    the hosted umbrella runtime;
  - explicit SIMD intent stays *scoped*: AVX2/AVX-512 belong to attributed
    function islands, so the translation unit keeps its baseline target and
    runtime dispatch stays valid on older CPUs.

Replaces run_aot_link_command_tests.sh.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import platform as host_platform
import shutil
import sys
from pathlib import Path
from typing import List, Optional


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, toolchain, workspace  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent

RETIRED_ATTRIBUTES = ("c_export", "section", "weak", "used", "naked",
                      "interrupt", "no_alloc", "zero_cost")

FFI_C_SOURCE = """#include <stdint.h>
int32_t xr_manifest_add1(int32_t value) { return value + 1; }
"""

FFI_XR_SOURCE = """extern "C" {
    fn manifestAdd1(value: i32) -> i32
}
print(unsafe { manifestAdd1(41) })
"""

SIMD_XR_SOURCE = """import { Capabilities } from simd
print(Capabilities.nativeBytes())
"""


class Recorder:
    """Accumulates PASS/FAIL lines in the shell runner's output shape."""

    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0

    def ok(self, name: str) -> None:
        print(f"  PASS: {name}")
        self.passed += 1

    def bad(self, name: str, log: str = "", limit: int = 100) -> None:
        print(f"  FAIL: {name}")
        self.failed += 1
        for line in log.splitlines()[:limit]:
            print(f"      {line}")

    def contains(self, haystack: str, needle: str, name: str) -> bool:
        if needle in haystack:
            self.ok(name)
            return True
        self.bad(name, haystack)
        return False

    def not_contains(self, haystack: str, needle: str, name: str) -> bool:
        if needle in haystack:
            self.bad(name, haystack)
            return False
        self.ok(name)
        return True


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(description="Manifest-first AOT link smoke")
    ap.add_argument("xray", nargs="?", default=None)
    ns = ap.parse_args(argv[1:])

    xray = Path(ns.xray or os.environ.get("XRAY_BIN") or (PROJECT_DIR / "build" / "xray"))
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 600)
    rec = Recorder()

    print("=== Manifest-first AOT Link Smoke ===")
    print(f"Binary: {xray}")
    print("")

    if not (xray.is_file() and os.access(xray, os.X_OK)):
        sys.stderr.write(f"FAIL: xray binary not executable: {xray}\n")
        return 1

    with workspace.Workspace("xray_manifest_link") as ws:
        cache = ws.path("cache")

        # 1. The public attribute surface.
        attrs = proc.run([xray, "language", "attributes"], timeout=timeout)
        if attrs.ok:
            text = attrs.combined_text()
            rec.contains(text, "Public attributes (9):",
                         "language surface exposes exactly nine public attributes")
            for retired in RETIRED_ATTRIBUTES:
                rec.not_contains(text, f"@{retired}",
                                 f"language surface excludes retired @{retired}")
        else:
            rec.bad("language attributes command succeeds", attrs.combined_text())

        # 2. An audited prebuilt dynamic library: the source carries only an
        # extern signature; the path and symbol mapping live in xray.toml.
        ffi_dir = ws.subdir("dynamic")
        ffi_c = ffi_dir / "native.c"
        ffi_c.write_text(FFI_C_SOURCE, encoding="utf-8")

        is_darwin = host_platform.system() == "Darwin"
        ffi_lib = ffi_dir / ("libmanifest_smoke.dylib" if is_darwin else "libmanifest_smoke.so")
        cc = toolchain.find_c_compiler()
        if cc is None:
            rec.bad("native fixture library builds", "no C compiler found")
            print("")
            print(f"=== Results: {rec.passed} passed, {rec.failed} failed ===")
            return 1
        cc_args = (["-dynamiclib", "-install_name", str(ffi_lib)] if is_darwin
                   else ["-shared", "-fPIC"])
        built = proc.run([cc.path, *cc_args, "-o", ffi_lib, ffi_c], timeout=timeout)
        if built.ok:
            rec.ok("native fixture library builds")
        else:
            rec.bad("native fixture library builds", built.combined_text())

        (ffi_dir / "main.xr").write_text(FFI_XR_SOURCE, encoding="utf-8")
        (ffi_dir / "xray.toml").write_text(f"""[package]
name = "manifest-dynamic-smoke"
version = "1.0.0"
license = "MIT"
main = "main.xr"

[native]
name = "manifest-dynamic-smoke"
version = "1"
license = "test"
source = "generated test fixture"
audit_mode = "exploratory"
vm = "verified-dynamic"

[[native.unit]]
name = "fixture"
kind = "dynamic-library"
sources = ["{ffi_lib.name}"]
source_hashes = ["{sha256_file(ffi_lib)}"]
optimization = "none"
visibility = "default"
warnings = "system"
purpose = "prove audited prebuilt library linking"

[[native.symbol]]
xray = "manifestAdd1"
native = "xr_manifest_add1"
kind = "function"
calling_convention = "c"
unit = "fixture"
""", encoding="utf-8")

        ffi_bin = ffi_dir / "app"
        ffi = proc.run(
            [xray, "build", "--native", "--dump-link-command", "--cache-dir", cache,
             "-o", ffi_bin, ffi_dir / "main.xr"],
            timeout=timeout,
        )
        if ffi.ok:
            rec.ok("typed prebuilt dynamic-library plan links")
            log = ffi.combined_text()
            rec.contains(log, "Link command:", "dynamic-library build emits link command")
            rec.contains(log, ffi_lib.name,
                         "dynamic-library link command uses audited artifact path")
            env = dict(os.environ)
            env["DYLD_LIBRARY_PATH"] = str(ffi_dir)
            env["LD_LIBRARY_PATH"] = str(ffi_dir)
            run = proc.run([ffi_bin], env=env, timeout=timeout)
            if run.stdout.decode("utf-8", "replace").strip() == "42":
                rec.ok("dynamic-library binary executes")
            else:
                rec.bad("dynamic-library binary executes with output 42", run.combined_text())
        else:
            rec.bad("typed prebuilt dynamic-library plan links", ffi.combined_text(), 120)

        # 3. Reachability decides the link: libm yes, hosted umbrella no.
        math_src = PROJECT_DIR / "tests/aot/filetests/link/core_math_single_symbol.xr"
        math_bin = ws.path("core-math")
        math = proc.run(
            [xray, "build", "--native", "--dump-link-command", "--cache-dir", cache,
             "-o", math_bin, math_src],
            timeout=timeout,
        )
        if math.ok:
            rec.ok("core math native binary links")
            log = math.combined_text()
            rec.contains(log, "-lm", "core math links libm")
            rec.not_contains(log, "-lxray_core",
                             "core math does not link hosted umbrella runtime")
            run = proc.run([math_bin], timeout=timeout)
            if run.stdout.decode("utf-8", "replace").strip() == "9.0":
                rec.ok("core math binary executes")
            else:
                rec.bad("core math binary executes with output 9.0", run.combined_text())

            # The AArch64 machine outliner is disabled for speed builds and
            # left alone for size builds; both directions are asserted.
            is_clang = cc is not None and not cc.is_zig
            if host_platform.machine() == "arm64" and is_clang:
                rec.contains(log, "-mno-outline",
                             "AArch64 speed build disables the machine outliner")
                size = proc.run(
                    [xray, "build", "--native", "-O", "s", "--dump-link-command",
                     "--cache-dir", str(cache) + "-size",
                     "-o", ws.path("core-math-size"), math_src],
                    timeout=timeout,
                )
                if size.ok:
                    rec.not_contains(size.combined_text(), "-mno-outline",
                                     "AArch64 size build retains outlining policy")
                else:
                    rec.bad("AArch64 size-policy probe links", size.combined_text())
        else:
            rec.bad("core math native binary links", math.combined_text(), 120)

        # 4. SIMD intent stays scoped to attributed islands (x86_64 only).
        if host_platform.machine() in ("x86_64", "AMD64"):
            simd_src = ws.write("simd-intent.xr", SIMD_XR_SOURCE)
            for mode in ("avx2", "avx512", "dispatch"):
                r = proc.run(
                    [xray, "build", "--native", "--simd", mode, "--rebuild",
                     "--dump-link-command", "--dump-link-manifest",
                     "--cache-dir", ws.path(f"cache-simd-{mode}"),
                     "-o", ws.path(f"simd-{mode}"), simd_src],
                    timeout=timeout,
                )
                if not r.ok:
                    rec.bad(f"semantic SIMD {mode} native build succeeds",
                            r.combined_text(), 160)
                    continue
                rec.ok(f"semantic SIMD {mode} native build succeeds")
                log = r.combined_text()
                rec.contains(log, '"provider_cc_flags": []',
                             f"semantic SIMD {mode} does not use raw manifest flags")
                if mode == "avx2":
                    rec.not_contains(log, "-mavx2",
                                     "AVX2 intent does not retarget the whole GNU translation unit")
                elif mode == "avx512":
                    rec.not_contains(log, "-mavx512f",
                                     "AVX-512 intent does not retarget the whole GNU translation unit")
                else:
                    rec.not_contains(log, "-mavx2", "dispatch keeps baseline compile target")
                    rec.not_contains(log, "-mavx512f",
                                     "dispatch keeps AVX-512 in attributed islands")

            dispatch_src = PROJECT_DIR / "tests/aot/filetests/cgen/simd_x86_runtime_dispatch.xr"
            r = proc.run(
                [xray, "build", "--native", "--simd", "dispatch", "--rebuild",
                 "--dump-link-command", "--dump-link-manifest",
                 "--cache-dir", ws.path("cache-simd-dispatch-islands"),
                 "-o", ws.path("simd-dispatch-islands"), dispatch_src],
                timeout=timeout,
            )
            if r.ok:
                rec.ok("dispatch AVX2/AVX-512 attributed islands compile at baseline")
                log = r.combined_text()
                rec.not_contains(log, "-mavx2", "dispatch island fixture has no global AVX2 flag")
                rec.not_contains(log, "-mavx512f",
                                 "dispatch island fixture has no global AVX-512 flag")
            else:
                rec.bad("dispatch AVX2/AVX-512 attributed islands compile at baseline",
                        r.combined_text(), 180)

            for mode in ("avx2", "avx512"):
                static_src = PROJECT_DIR / f"tests/aot/filetests/cgen/simd_x86_static_{mode}_island.xr"
                r = proc.run(
                    [xray, "build", "--native", "--simd", mode, "--rebuild",
                     "--dump-link-command", "--dump-link-manifest",
                     "--cache-dir", ws.path(f"cache-simd-static-{mode}-islands"),
                     "-o", ws.path(f"simd-static-{mode}-islands"), static_src],
                    timeout=timeout,
                )
                if not r.ok:
                    rec.bad(f"static {mode} attributed island compiles at baseline",
                            r.combined_text(), 180)
                    continue
                rec.ok(f"static {mode} attributed island compiles with a scoped unit target")
                log = r.combined_text()
                if mode == "avx2":
                    rec.contains(log, "-mavx2", "static avx2 vector-bearing unit receives AVX2")
                    rec.not_contains(log, "-mavx512f",
                                     "static avx2 unit does not receive AVX-512")
                else:
                    rec.contains(log, "-mavx512f",
                                 "static avx512 vector-bearing unit receives AVX-512")
                    rec.not_contains(log, "-mavx2",
                                     "static avx512 unit does not receive a separate AVX2 flag")

        # 5. Export and link-symbol policy must reach the generated C and object.
        free_dir = ws.subdir("freestanding")
        shutil.copy2(PROJECT_DIR / "tests/aot/filetests/link/freestanding_symbol_attrs.xr",
                     free_dir / "main.xr")
        manifest_text = (PROJECT_DIR / "tests/aot/filetests/link/freestanding_symbol_attrs.toml"
                         ).read_text(encoding="utf-8")
        (free_dir / "xray.toml").write_text(
            manifest_text.replace('main = "freestanding_symbol_attrs.xr"', 'main = "main.xr"'),
            encoding="utf-8")

        free_obj = free_dir / "kernel.o"
        r = proc.run(
            [xray, "build", "--native", "--profile", "freestanding", "--shared",
             "--keep-c", "--rebuild", "--dump-link-command", "--dump-link-manifest",
             "--cache-dir", cache, "-o", free_obj, free_dir / "main.xr"],
            timeout=timeout,
        )
        if r.ok:
            rec.ok("freestanding manifest object builds")
            log = r.combined_text()
            rec.contains(log, "-nostdlib", "freestanding command remains nostdlib")
            rec.contains(log, '"runtime_objects": []',
                         "freestanding manifest has no hosted runtime objects")
            kept = [ln[len("Kept C source: "):].strip()
                    for ln in log.splitlines() if ln.startswith("Kept C source: ")]
            if kept and Path(kept[-1]).is_file():
                generated = Path(kept[-1]).read_text(encoding="utf-8", errors="replace")
                rec.contains(generated, 'XRT_ATTR_SECTION("__TEXT,.xray_boot")',
                             "link.symbol section plan reaches generated C")
                rec.contains(generated, "XRT_ATTR_WEAK",
                             "link.symbol weak plan reaches generated C")
                rec.contains(generated, "XRT_ATTR_USED",
                             "link.symbol used plan reaches generated C")
            else:
                rec.bad("freestanding kept C source is available")

            nm_out = proc.run(["nm", "-g", free_obj], timeout=timeout)
            names = {ln.split()[-1].lstrip("_") for ln in
                     nm_out.stdout.decode("utf-8", "replace").splitlines() if ln.split()}
            if "xray_boot_score" in names:
                rec.ok("export.c symbol is present in freestanding object")
            else:
                rec.bad("export.c symbol is present in freestanding object")
        else:
            rec.bad("freestanding manifest object builds", r.combined_text(), 140)

    print("")
    print(f"=== Results: {rec.passed} passed, {rec.failed} failed ===")
    return 1 if rec.failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

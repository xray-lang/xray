#!/usr/bin/env python3
"""FFI diagnostic regression tests.

Locks the user-facing error contracts for manifest-declared native failures
that are not compile errors: VM runtime resolution and AOT native link
failures.

Each case asserts both halves -- the right diagnostic appears AND the wrong one
does not. A missing library that reports "symbol not found" sends the user
looking in the wrong place, so the negative assertion carries as much weight as
the positive one.

Usage: run_ffi_diagnostics.py [xray_binary]
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path
from typing import List


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, workspace  # noqa: E402

PROJECT_DIR = Path(__file__).resolve().parents[2]

_ANSI = re.compile(r"\x1B\[[0-9;]*[a-zA-Z]")

MISSING_LIB = "xray_missing_library_nope_zz"
MISSING_SYMBOL = "xray_missing_symbol_nope_zz"

NO_LIBFFI = "this build has no libffi"

MISSING_LIB_SOURCE = '''extern "C" { fn nope(x: i32) -> i32 }
print(unsafe { nope(1) })
'''

MISSING_LIB_MANIFEST = f'''[package]
name = "ffi-missing-library"
version = "1.0.0"
license = "MIT"
main = "main.xr"

[native]
name = "ffi-missing-library"
version = "1"
license = "test"
source = "negative platform-library fixture"
audit_mode = "exploratory"
vm = "verified-dynamic"

[[native.unit]]
name = "missing"
kind = "platform"
system_links = ["{MISSING_LIB}"]
optimization = "none"
visibility = "default"
warnings = "system"
purpose = "prove missing-library diagnostics"

[[native.symbol]]
xray = "nope"
native = "nope"
kind = "function"
calling_convention = "c"
unit = "missing"
'''

MISSING_SYMBOL_SOURCE = f'''extern "C" {{ fn {MISSING_SYMBOL}(x: i32) -> i32 }}
print(unsafe {{ {MISSING_SYMBOL}(1) }})
'''

MISSING_SYMBOL_MANIFEST = f'''[package]
name = "ffi-missing-symbol"
version = "1.0.0"
license = "MIT"
main = "main.xr"

[native]
name = "ffi-missing-symbol"
version = "1"
license = "test"
source = "negative process-symbol fixture"
audit_mode = "exploratory"
vm = "verified-dynamic"

[[native.unit]]
name = "process"
kind = "platform"
optimization = "none"
visibility = "default"
warnings = "system"
purpose = "prove missing-symbol diagnostics"

[[native.symbol]]
xray = "{MISSING_SYMBOL}"
native = "{MISSING_SYMBOL}"
kind = "function"
calling_convention = "c"
unit = "process"
'''


def strip_ansi(text: str) -> str:
    return _ANSI.sub("", text)


class Recorder:
    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0
        self.skipped = 0

    def ok(self, name: str) -> None:
        print(f"  PASS: {name}")
        self.passed += 1

    def bad(self, name: str, text: str = "") -> None:
        print(f"  FAIL: {name}")
        self.failed += 1
        for line in text.splitlines()[:100]:
            print(f"      {line}")

    def skip(self, name: str) -> None:
        print(f"  SKIP: {name}")
        self.skipped += 1

    def expect_contains(self, text: str, needle: str, name: str) -> None:
        if needle in text:
            self.ok(name)
        else:
            self.bad(name, text)

    def expect_not_contains(self, text: str, needle: str, name: str) -> None:
        if needle in text:
            self.bad(name, text)
        else:
            self.ok(name)


def run_vm_case(xray: Path, source: Path, timeout: "float | None") -> str:
    """stderr then stdout, as the shell concatenated them, ANSI stripped."""
    result = proc.run([xray, "run", source], timeout=timeout)
    combined = (result.stderr.decode("utf-8", "replace")
                + result.stdout.decode("utf-8", "replace"))
    return strip_ansi(combined)


def main(argv: List[str]) -> int:
    xray = Path(argv[1] if len(argv) > 1
                else os.environ.get("XRAY_BIN",
                                    str(PROJECT_DIR / "build" / "xray")))

    print("=== FFI Diagnostics Tests ===")
    print(f"Binary: {xray}")
    print("")

    if not (xray.is_file() and os.access(xray, os.X_OK)):
        sys.stderr.write(f"FAIL: xray binary not executable: {xray}\n")
        return 1
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 600)

    rec = Recorder()
    with workspace.Workspace("xray_ffi_diag") as ws:
        cache = ws.path(".cache")

        lib_dir = ws.subdir("missing_lib")
        lib_src = lib_dir / "main.xr"
        platform.write_text_lf(lib_src, MISSING_LIB_SOURCE)
        platform.write_text_lf(lib_dir / "xray.toml", MISSING_LIB_MANIFEST)

        sym_dir = ws.subdir("missing_symbol")
        sym_src = sym_dir / "main.xr"
        platform.write_text_lf(sym_src, MISSING_SYMBOL_SOURCE)
        platform.write_text_lf(sym_dir / "xray.toml", MISSING_SYMBOL_MANIFEST)

        vm_lib_text = run_vm_case(xray, lib_src, timeout)
        if NO_LIBFFI in vm_lib_text:
            rec.skip("vm missing library: libffi disabled")
        else:
            rec.expect_contains(vm_lib_text,
                                f"FFI: cannot load library '{MISSING_LIB}'",
                                "vm missing library: reports load failure")
            rec.expect_not_contains(vm_lib_text, "FFI: symbol 'nope' not found",
                                    "vm missing library: no misleading symbol error")

        vm_sym_text = run_vm_case(xray, sym_src, timeout)
        if NO_LIBFFI in vm_sym_text:
            rec.skip("vm missing symbol: libffi disabled")
        else:
            rec.expect_contains(vm_sym_text,
                                f"FFI: symbol '{MISSING_SYMBOL}' not found",
                                "vm missing symbol: reports symbol failure")
            rec.expect_not_contains(vm_sym_text, "FFI: cannot load library",
                                    "vm missing symbol: no library failure")

        for source, label, needle in ((lib_src, "aot missing library", MISSING_LIB),
                                      (sym_src, "aot missing symbol", MISSING_SYMBOL)):
            out = ws.path(label.replace(" ", "_"))
            build = proc.run([xray, "build", "--native", "--dump-link-command",
                              "--cache-dir", cache, "-o", out, source],
                             timeout=timeout)
            if build.ok:
                rec.bad(f"{label}: build should fail")
                continue
            text = strip_ansi(build.combined_text())
            rec.expect_contains(text, "Link command:", f"{label}: emits link command")
            rec.expect_contains(
                text, needle,
                f"{label}: names missing "
                f"{'library' if needle == MISSING_LIB else 'symbol'}")
            rec.expect_contains(text, "AOT manifest linking failed",
                                f"{label}: reports AOT link failure")

    print("")
    print(f"=== Results: {rec.passed} passed, {rec.failed} failed, "
          f"{rec.skipped} skipped ===")
    return 1 if rec.failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

#!/usr/bin/env python3
"""AOT UndefinedBehaviorSanitizer lane: native output must be UB-free.

Unlike the other sanitizer lanes, this one does not build an instrumented
compiler. It uses an ordinary xray to compile each case to a native binary whose
*generated C* is compiled with UBSan, then runs it and compares stdout against
the VM. So it checks the emitted code, not the emitter.

Three things must hold per case: the binary runs cleanly (halt_on_error makes a
finding fatal), stderr carries no UBSan diagnostic, and stdout matches the VM
oracle. A case the VM cannot run is skipped rather than guessed at -- without an
oracle there is no way to tell "UBSan changed behavior" from "case needs a
fixture".

The self-check matters most: before trusting any clean run, the lane confirms
__ubsan_handle_* symbols are actually linked into the produced binary. A lane
that silently stopped instrumenting would otherwise pass forever.

Skips cleanly (exit 0) when the AOT toolchain is not READY, so hosts without a
native toolchain do not report a failure they cannot fix.

Environment overrides:
    XRAY_BIN                    xray binary (default: build/xray)
    XR_AOT_UBSAN_TOOLCHAIN      provider (default: clang)
    XR_AOT_UBSAN_TARGET         target triple (default: from toolchain doctor)
    XR_AOT_UBSAN_CASES          newline-separated case list
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parent.parent / "tests" / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import binary as binlib  # noqa: E402
from xraytest import platform, proc, workspace  # noqa: E402

PROJECT_DIR = Path(__file__).resolve().parent.parent

# Chosen for UB density rather than breadth: integer wrap/convert paths,
# operator lowering, bit manipulation, indexing and slicing, string and byte
# buffers, and the struct/FFI layout surface. Each must be a self-contained
# single file the VM can also run, so its stdout is a usable oracle.
DEFAULT_CASES = (
    "tests/regression/03_operators/0300_arithmetic.xr",
    "tests/regression/03_operators/0301_int64_overflow.xr",
    "tests/regression/03_operators/0302_int64_native.xr",
    "tests/regression/03_operators/0302_uint64_print_compare.xr",
    "tests/regression/03_operators/0304_fixed_width_wrapping.xr",
    "tests/regression/03_operators/0310_comparison.xr",
    "tests/regression/03_operators/0330_bitwise.xr",
    "tests/regression/03_operators/0331_shift_mod64.xr",
    "tests/regression/03_operators/0332_int_bits_methods.xr",
    "tests/regression/03_operators/0340_compound_assign.xr",
    "tests/regression/03_operators/0006_as_numeric_cast.xr",
    "tests/regression/01_literals/0101_int_boundary.xr",
    "tests/regression/01_literals/0140_special_values.xr",
    "tests/regression/01_literals/0120_string_basic.xr",
    "tests/regression/06_collections/0600_array.xr",
    "tests/regression/06_collections/0650_array_index_set_strict.xr",
)

# unsigned wraparound is defined behavior and intentionally not a finding.
MANIFEST_TEMPLATE = """[project]
name = "aot-ubsan-lane"
main = "case.xr"

[target.{target}]
toolchain = "{toolchain}"
cc_flags = [
    "-fsanitize=undefined,integer",
    "-fno-sanitize=unsigned-integer-overflow,unsigned-shift-base",
    "-fno-sanitize-recover=all",
    "-fno-omit-frame-pointer",
]
ld_flags = ["-fsanitize=undefined"]
"""

UBSAN_HANDLER_RE = re.compile(r"ubsan_handle")


def main(argv: list[str]) -> int:
    xray = Path(os.environ.get("XRAY_BIN", str(PROJECT_DIR / "build" / "xray")))
    toolchain_name = os.environ.get("XR_AOT_UBSAN_TOOLCHAIN", "clang")
    timeout = platform.env_timeout("XR_AOT_UBSAN_TIMEOUT", 600)

    os.environ["UBSAN_OPTIONS"] = (
        "print_stacktrace=1:halt_on_error=1:" + os.environ.get("UBSAN_OPTIONS", ""))

    print("=" * 72)
    print("AOT UBSan lane")
    print("=" * 72)

    if not (xray.is_file() and os.access(xray, os.X_OK)):
        print(f"FAIL: xray binary not found: {xray}", file=sys.stderr)
        return 1

    doctor = proc.run([xray, "toolchain", "doctor"], timeout=timeout)
    doctor_text = doctor.combined_text()
    if not doctor.ok:
        print("SKIP: AOT toolchain is not READY on this host")
        return 0

    target = os.environ.get("XR_AOT_UBSAN_TARGET")
    if not target:
        match = re.search(r"^\s*Target:\s*(\S+)", doctor_text, re.MULTILINE)
        if not match:
            print("SKIP: could not determine the native target from toolchain doctor")
            return 0
        target = match.group(1)

    raw_cases = os.environ.get("XR_AOT_UBSAN_CASES")
    cases = ([c.strip() for c in raw_cases.splitlines() if c.strip()]
             if raw_cases else list(DEFAULT_CASES))

    passed = failed = skipped = total = 0
    instrumentation_verified = False
    have_nm = binlib.find_nm() is not None

    with workspace.Workspace("xray-aot-ubsan") as ws:
        # The typed link plan is how a project declares extra C flags; using it
        # here means the lane also proves that path keeps working.
        (ws.root / "xray.toml").write_text(
            MANIFEST_TEMPLATE.format(target=target, toolchain=toolchain_name),
            encoding="utf-8")
        case_src = ws.root / "case.xr"
        case_bin = ws.root / "case.bin"

        for case_path in cases:
            absolute = PROJECT_DIR / case_path
            if not absolute.is_file():
                print(f"  skip {case_path:<56} (missing)")
                skipped += 1
                continue
            total += 1

            case_src.write_bytes(absolute.read_bytes())
            if case_bin.exists():
                case_bin.unlink()

            # VM oracle first: without it a difference cannot be attributed.
            vm = proc.run([xray, "run", "case.xr"], cwd=ws.root, timeout=timeout)
            if not vm.ok:
                print(f"  skip {case_path:<56} (VM run failed)")
                skipped += 1
                total -= 1
                continue

            build = proc.run(
                [xray, "build", "-N", "--target", target, "--toolchain", toolchain_name,
                 "-o", "case.bin", "case.xr"],
                cwd=ws.root, timeout=timeout)
            if not (case_bin.is_file() and os.access(case_bin, os.X_OK)):
                print(f"  FAIL {case_path:<56} (AOT build failed)")
                for line in build.combined_text().splitlines()[-12:]:
                    print(f"        {line}")
                failed += 1
                continue

            # A lane that silently stopped instrumenting would pass forever.
            if not instrumentation_verified and have_nm:
                undefined = binlib.undefined_symbol_names(case_bin, timeout=timeout) or []
                handlers = [s for s in undefined if UBSAN_HANDLER_RE.search(s)]
                if not handlers:
                    print(f"  FAIL {case_path:<56} (no UBSan handlers linked)")
                    print("        The generated binary carries no __ubsan_handle_* symbols,")
                    print("        so this lane would report success without checking anything.")
                    print("        Verify that xray.toml cc_flags still reach the C compile step.")
                    failed += 1
                    continue
                print(f"  (instrumentation verified: {len(handlers)} UBSan check handlers linked)")
                instrumentation_verified = True

            run = proc.run([case_bin], cwd=ws.root, timeout=timeout)
            stderr_text = run.stderr.decode("utf-8", "replace")
            if run.timed_out:
                print(f"  FAIL {case_path:<56} (timed out after {timeout}s)")
                failed += 1
                continue
            if run.returncode != 0:
                print(f"  FAIL {case_path:<56} (exit {run.returncode})")
                for line in stderr_text.splitlines()[:20]:
                    print(f"        {line}")
                failed += 1
                continue
            # halt_on_error makes a finding fatal, so a non-zero exit is the
            # usual signal; this catches a diagnostic that somehow did not abort.
            if "runtime error:" in stderr_text:
                print(f"  FAIL {case_path:<56} (UBSan diagnostic)")
                for line in stderr_text.splitlines()[:20]:
                    print(f"        {line}")
                failed += 1
                continue
            if run.stdout != vm.stdout:
                print(f"  FAIL {case_path:<56} (stdout differs from VM)")
                import difflib

                diff = difflib.unified_diff(
                    vm.stdout.decode("utf-8", "replace").splitlines(),
                    run.stdout.decode("utf-8", "replace").splitlines(),
                    lineterm="", n=1)
                for line in list(diff)[:12]:
                    print(f"        {line}")
                failed += 1
                continue

            print(f"  ok   {case_path}")
            passed += 1

    print("=" * 72)
    print(f"AOT UBSan: {passed}/{total} passed, {failed} failed, {skipped} skipped")

    if total == 0:
        print("SKIP: no runnable cases")
        return 0
    if not instrumentation_verified and have_nm:
        print("FAIL: never confirmed the sanitizer was linked", file=sys.stderr)
        return 1
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

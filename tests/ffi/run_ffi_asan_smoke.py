#!/usr/bin/env python3
"""FFI memory cases under AddressSanitizer, on both backends.

Normal builds skip with 77. An ASan build must run representative FFI memory
cases through the VM and through AOT, and the AOT native binaries must inherit
the ASan flags -- an AOT child compiled without them would run uninstrumented
and report clean no matter what the FFI code did, so the flag assertion is what
keeps the AOT half meaningful.

Exit 77 (SKIP_RETURN_CODE) when the binary is not an ASan build.

Usage: run_ffi_asan_smoke.py [xray_binary]
"""

from __future__ import annotations

import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, workspace  # noqa: E402

PROJECT_DIR = Path(__file__).resolve().parents[2]

SKIP_EXIT = 77
_ANSI = re.compile(r"\x1B\[[0-9;]*[a-zA-Z]")
ASAN_FLAG = "-fsanitize=address"
NO_LIBFFI = "this build has no libffi"

# halt_on_error/abort_on_error make the first ASan report fatal: a case that
# kept running after a detected fault could still print the expected output.
DEFAULT_ASAN_OPTIONS = "detect_leaks=0:halt_on_error=1:abort_on_error=1"


@dataclass(frozen=True)
class Case:
    name: str
    source: str
    expected: str


CASES: tuple[Case, ...] = (
    Case("ffi_ptr_memory", "tests/diff/cases/semantics/ffi/ptr_memory.xr",
         "10\n20\n30\n40\n30\nfalse\nfalse"),
    Case("ffi_cfn_bsearch", "tests/diff/cases/semantics/ffi/cfn_bsearch.xr",
         "false"),
    Case("fixed_layout_struct", "tests/diff/cases/semantics/oop/repr_c_struct.xr",
         "12\n20\ntrue\n4\n2\n3"),
)


def normalize(text: str) -> str:
    """ANSI stripped, trailing whitespace trimmed, blank lines dropped."""
    lines = [line.rstrip() for line in _ANSI.sub("", text).splitlines()]
    return "\n".join(line for line in lines if line)


class Recorder:
    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0
        self.skipped = 0

    def ok(self, name: str) -> None:
        print(f"  PASS: {name}")
        self.passed += 1

    def bad(self, name: str, detail: str = "") -> None:
        print(f"  FAIL: {name}")
        self.failed += 1
        if detail:
            for line in detail.splitlines():
                print(f"      {line}")

    def skip(self, name: str) -> None:
        print(f"  SKIP: {name}")
        self.skipped += 1

    def expect_output(self, got: str, expected: str, name: str) -> None:
        if got == expected:
            self.ok(name)
        else:
            print(f"  FAIL: {name}")
            self.failed += 1
            print("      expected:")
            for line in expected.splitlines():
                print(f"        {line}")
            print("      got:")
            for line in got.splitlines():
                print(f"        {line}")


def asan_env() -> dict:
    env = dict(os.environ)
    env.setdefault("ASAN_OPTIONS", DEFAULT_ASAN_OPTIONS)
    return env


def main(argv: list[str]) -> int:
    xray = Path(argv[1] if len(argv) > 1
                else os.environ.get("XRAY_BIN",
                                    str(PROJECT_DIR / "build-sanitizers" / "xray")))

    print("=== FFI ASan Smoke Tests ===")
    print(f"Binary: {xray}")
    print("")

    if not (xray.is_file() and os.access(xray, os.X_OK)):
        sys.stderr.write(f"FAIL: xray binary not executable: {xray}\n")
        return 1
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 900)

    rec = Recorder()
    with workspace.Workspace("xray_ffi_asan") as ws:
        cache = ws.path(".cache")

        # Probe by building anything and reading the link manifest: whether the
        # flags reach an AOT child is exactly what this lane needs to know, and
        # a CMake cache lookup would not answer that.
        probe_src = ws.write("asan_probe.xr", "print(1)\n")
        probe = proc.run([xray, "build", "--native", "--dump-link-manifest",
                          "--cache-dir", cache, "-o", ws.path("asan_probe"),
                          probe_src], timeout=timeout)
        if not probe.ok:
            sys.stderr.write("FAIL: cannot build ASan probe\n")
            for line in normalize(probe.combined_text()).splitlines():
                sys.stderr.write(f"      {line}\n")
            return 1
        if ASAN_FLAG not in probe.combined_text():
            rec.skip("xray binary is not an ASan build")
            print("")
            print(f"=== Results: {rec.passed} passed, {rec.failed} failed, "
                  f"{rec.skipped} skipped ===")
            return SKIP_EXIT
        rec.ok("xray ASan build detected")

        for case in CASES:
            source = PROJECT_DIR / case.source
            print("")
            print(f"--- {case.name} ---")
            if not source.is_file():
                rec.bad(f"{case.name}: source exists")
                continue

            vm = proc.run([xray, "run", source], env=asan_env(), timeout=timeout)
            if not vm.ok and NO_LIBFFI in vm.combined_text():
                rec.skip(f"{case.name}: VM libffi disabled")
            elif vm.ok:
                rec.expect_output(normalize(vm.stdout.decode("utf-8", "replace")),
                                  case.expected, f"{case.name}: VM output")
            else:
                sys.stderr.write(
                    f"      VM case failed with status {vm.returncode}\n")
                for line in vm.combined_text().splitlines():
                    sys.stderr.write(f"      {line}\n")
                rec.bad(f"{case.name}: VM run")

            binary = ws.path(platform.exe_name(f"{case.name}.bin"))
            build = proc.run([xray, "build", "--native", "--dump-link-manifest",
                              "--dump-link-command", "--cache-dir", cache,
                              "-o", binary, source], timeout=timeout)
            if not build.ok:
                sys.stderr.write(
                    f"      AOT build failed with status {build.returncode}\n")
                for line in normalize(build.combined_text()).splitlines():
                    sys.stderr.write(f"      {line}\n")
                rec.bad(f"{case.name}: AOT run")
                continue

            flag_name = f"{case.name}: AOT manifest carries ASan flags"
            if ASAN_FLAG in build.combined_text():
                rec.ok(flag_name)
            else:
                rec.bad(flag_name, normalize(build.combined_text()))

            run = proc.run([binary], env=asan_env(), timeout=timeout)
            if not run.ok:
                sys.stderr.write(
                    f"      AOT case failed with status {run.returncode}\n")
                for line in run.combined_text().splitlines():
                    sys.stderr.write(f"      {line}\n")
                rec.bad(f"{case.name}: AOT run")
                continue
            rec.expect_output(normalize(run.stdout.decode("utf-8", "replace")),
                              case.expected, f"{case.name}: AOT output")

    print("")
    print(f"=== Results: {rec.passed} passed, {rec.failed} failed, "
          f"{rec.skipped} skipped ===")
    return 1 if rec.failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

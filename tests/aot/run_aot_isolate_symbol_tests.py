#!/usr/bin/env python3
"""AOT isolation gate: a native binary must not carry the compiler or VM.

Each case builds a small program through the public AOT path and then asserts
what the resulting binary is *made of*: it stays under a size ceiling, its link
command never pulls in xray_core, and its symbol table contains none of the
isolate/VM/parser/analyzer symbols that would mean the whole language runtime
got dragged along. Some cases additionally assert that eagerly-initialized
script builtins are absent.

This is a shape contract, not a behavior one -- which is why it checks defined
symbols and byte sizes rather than program output alone (though cases with an
expected stdout check that too).

Replaces run_aot_isolate_symbol_tests.sh, the last consumer of the shared shell
helper tests/test_common.sh.
"""

from __future__ import annotations

import argparse
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
from xraytest import (cache, platform, proc, progress, report, scheduler,
                      toolchain, workspace)  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent

Status = report.Status

# Symbols whose presence means the binary carries the isolate, the VM, or the
# compiler front end. Darwin's nm prefixes C symbols with '_', so each
# alternative tolerates an optional leading underscore.
FORBIDDEN_SYMBOL_RE = re.compile(
    r"(^|[^A-Za-z0-9_])_?(xray_vm_|xray_isolate_|xr_vm_|xr_parse|xr_compile"
    r"|xanalyzer_|xr_native_module_create_)"
)

# Script builtins that must not be eagerly initialized in a minimal runtime.
EAGER_SCRIPT_BUILTIN_SYMBOL_RE = re.compile(
    r"(^|[^A-Za-z0-9_])_?(xr_string_intern_core|xr_string_value)([^A-Za-z0-9_]|$)"
)

PURE_TINY_AOT_MAX_BYTES = 70000
PURE_CRYPTO_AOT_MAX_BYTES = 96000
RUNTIME_TIME_SLEEP_MAX_BYTES = 524288

BUILD_ATTEMPTS = 3


@dataclass(frozen=True)
class Case:
    slug: str
    name: str
    source: str                       # repo-relative
    want_output: str = ""
    max_bytes: int | None = None
    # Runtime cases may additionally forbid eagerly-initialized script builtins.
    no_eager_script_builtins: bool = False


CASES: tuple[Case, ...] = (
    Case("core_math_single_symbol", "core-math-single-symbol",
         "tests/aot/filetests/link/core_math_single_symbol.xr",
         want_output="9.0", max_bytes=PURE_TINY_AOT_MAX_BYTES),
    Case("system_time_queries", "system-time-queries",
         "tests/aot/filetests/link/system_time_queries.xr",
         max_bytes=PURE_TINY_AOT_MAX_BYTES),
    Case("core_crypto", "core-crypto",
         "tests/aot/filetests/link/core_crypto.xr",
         max_bytes=PURE_CRYPTO_AOT_MAX_BYTES),
    Case("runtime_time_sleep", "runtime-time-sleep",
         "tests/aot/filetests/link/runtime_time.xr",
         want_output="7", max_bytes=RUNTIME_TIME_SLEEP_MAX_BYTES),
    Case("runtime_coro_minimal", "runtime-coro-minimal",
         "tests/aot/coro/spawn_await_yield.xr",
         want_output="42"),
)


@dataclass
class Config:
    xray: Path
    jobs: int
    build_cache: Path
    case_timeout: float | None


@dataclass
class Check:
    """One assertion's verdict. A case contributes several."""

    name: str
    ok: bool
    detail: str = ""


def configure_jobs(requested: str) -> int:
    if requested in ("", "auto"):
        cap = platform.env_int("XRAY_AOT_ISOLATE_MAX_AUTO_JOBS", 4)
        return max(1, min(platform.cpu_count(), cap))
    return int(requested) if requested.isdigit() and int(requested) > 0 else 1


def build_native(config: Config, source: Path, out_bin: Path) -> tuple[bool, str]:
    """Build with --dump-link-command so the link line can be asserted on."""
    last = ""
    for _ in range(BUILD_ATTEMPTS):
        tmp = out_bin.with_name(out_bin.name + ".tmp")
        try:
            tmp.unlink()
        except OSError:
            pass
        result = proc.run(
            [config.xray, "build", "--native", "--dump-link-command",
             "--cache-dir", config.build_cache, "-o", tmp, source],
            timeout=config.case_timeout,
        )
        last = result.combined_text()
        if result.ok:
            tmp.replace(out_bin)
            return True, last
    return False, last


def dump_symbols(binary: Path) -> tuple[bool, str]:
    """Return normalized defined symbols from a verified host capability."""
    dumper = toolchain.find_symbol_dumper()
    if dumper is None:
        return False, "no verified defined-symbol dumper is available"
    return dumper.dump_defined_symbols(binary)


def run_case(config: Config, case: Case, ws: workspace.Workspace) -> list[Check]:
    checks: list[Check] = []
    source = PROJECT_DIR / case.source
    binary = ws.path(case.slug)

    ok, log = build_native(config, source, binary)
    if not ok:
        return [Check(f"{case.name}: build failed", False,
                      "|".join(log.splitlines()[-5:]))]

    size = binary.stat().st_size
    if case.max_bytes is not None:
        within = size <= case.max_bytes
        checks.append(Check(
            f"{case.name}: binary size within {case.max_bytes}", within,
            "" if within else f"{size} > {case.max_bytes}"))

    # The link command is in the build log; a pure/minimal binary must never
    # link the full core archive.
    linked_core = "-lxray_core" in log
    checks.append(Check(f"{case.name}: does not link xray_core", not linked_core,
                        "link command pulls in -lxray_core" if linked_core else ""))

    got, symbols = dump_symbols(binary)
    if not got:
        checks.append(Check(f"{case.name}: symbol dump failed", False, symbols[:200]))
        return checks

    hits = [ln for ln in symbols.splitlines() if FORBIDDEN_SYMBOL_RE.search(ln)]
    checks.append(Check(
        f"{case.name}: no forbidden isolate/VM/compiler symbols", not hits,
        "|".join(hits[:5]) if hits else ""))

    if case.no_eager_script_builtins:
        eager = [ln for ln in symbols.splitlines()
                 if EAGER_SCRIPT_BUILTIN_SYMBOL_RE.search(ln)]
        checks.append(Check(
            f"{case.name}: no eager script builtin symbols", not eager,
            "|".join(eager[:5]) if eager else ""))

    if case.want_output:
        result = proc.run([binary], timeout=config.case_timeout)
        actual = result.stdout.decode("utf-8", "replace").strip()
        matched = result.ok and actual == case.want_output
        checks.append(Check(
            f"{case.name}: binary output", matched,
            "" if matched else f"want {case.want_output!r}, got {actual!r} (rc={result.returncode})"))

    return checks


def check_runtime_archive(config: Config) -> list[Check]:
    """The coroutine runtime archive itself must be free of compiler/VM symbols.

    This checks the shipped static library rather than any one case's binary: if
    libxray_rt_coro.a already carried the isolate, every program linking it
    would, and a per-case size ceiling might still pass by luck.
    """
    archive = config.xray.resolve().parent / platform.static_lib_name("xray_rt_coro")
    label = "runtime-archive/xray_rt_coro"
    if not archive.is_file():
        return [Check(f"{label}: archive missing at {archive}", False)]
    got, symbols = dump_symbols(archive)
    if not got:
        return [Check(f"{label}: symbol dump failed", False, symbols[:200])]
    hits = [ln for ln in symbols.splitlines() if FORBIDDEN_SYMBOL_RE.search(ln)]
    return [Check(f"{label}: no forbidden isolate/VM/compiler symbols", not hits,
                  "|".join(hits[:5]) if hits else "")]


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="AOT isolate/symbol gate")
    ap.add_argument("--xray", default=None)
    ap.add_argument("xray_positional", nargs="?", default=None)
    ns = ap.parse_args(argv[1:])

    xray_raw = ns.xray or ns.xray_positional or os.environ.get("XRAY_BIN")
    if not xray_raw:
        build_dir = os.environ.get("XRAY_BUILD_DIR")
        xray_raw = str(Path(build_dir) / "xray") if build_dir else str(PROJECT_DIR / "build" / "xray")
    xray = Path(xray_raw)

    config = Config(
        xray=xray,
        jobs=configure_jobs(os.environ.get("XRAY_AOT_ISOLATE_JOBS",
                                           os.environ.get("XRAY_TEST_JOBS", "auto"))),
        build_cache=cache.stable_cache_dir(PROJECT_DIR, "aot-isolate-symbol", xray),
        case_timeout=platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300),
    )

    print("=== AOT Isolate / Symbol Tests ===")
    print(f"Binary: {xray}")
    print(f"Jobs:   {config.jobs}")
    print("")

    if not (xray.is_file() and os.access(xray, os.X_OK)):
        print(f"FAIL: xray binary not executable: {xray}")
        print("=== Results: 0 passed, 1 failed ===")
        return 1

    results: list[tuple[str, list[Check]]] = []
    with workspace.Workspace("xray_aot_isolate") as ws:
        reporter = progress.ProgressReporter(len(CASES))
        if config.jobs <= 1:
            for case in CASES:
                results.append((case.name, run_case(config, case, ws)))
                reporter.tick(case.name)
        else:
            sched = scheduler.Scheduler({scheduler.CPU: config.jobs})
            tasks = [scheduler.Task(key=str(i), fn=(lambda c=case: run_case(config, c, ws)),
                                    tag=scheduler.CPU)
                     for i, case in enumerate(CASES)]
            by_key = sched.run(tasks, on_done=lambda k, r: reporter.tick(""))
            for i, case in enumerate(CASES):
                value = by_key.get(str(i))
                if isinstance(value, BaseException):
                    raise value
                results.append((case.name, value))
        reporter.finish()

    results.append(("runtime-archive/xray_rt_coro", check_runtime_archive(config)))

    passed = failed = 0
    for name, checks in results:
        print("")
        print(f"-- {name}")
        for check in checks:
            if check.ok:
                print(f"  PASS: {check.name}")
                passed += 1
            else:
                print(f"  FAIL: {check.name}")
                if check.detail:
                    print(f"    {check.detail}")
                failed += 1

    print("")
    print(f"=== Results: {passed} passed, {failed} failed ===")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

#!/usr/bin/env python3
"""Native AOT debug smoke: `xray build --native -g` must be debuggable by source.

Three scenarios -- scalar locals, aggregate locals, and a coroutine -- each
build with debug info, then assert three layers:

  1. the build ran dsymutil and enabled AOT source-variable locals;
  2. the dSYM line table names the .xr file (not the generated C);
  3. lldb can break on an .xr line and print the *source* local names.

Layer 3 is the one that matters: DWARF that mentions the file but exposes only
generated C temporaries would satisfy 1 and 2 while being useless to a user.

Skips (exit 77) when the debugger toolchain is absent. debugserver ships inside
Xcode's LLDB framework and is not on PATH or discoverable via `xcrun -f`, so it
is located explicitly -- without it lldb cannot launch a process and every
assertion below would fail for an environmental reason.

Replaces run_native_debug_smoke.sh.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import sys
from dataclasses import dataclass, field
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, workspace  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent

SKIP_EXIT = 77

# debugserver lives inside Xcode's LLDB framework. `xcrun -f debugserver` fails
# even on a full Xcode install, so the known location is probed directly.
DEBUGSERVER_CANDIDATES = (
    "/Applications/Xcode.app/Contents/SharedFrameworks/LLDB.framework/"
    "Versions/A/Resources/debugserver",
    "/Library/Developer/CommandLineTools/Library/PrivateFrameworks/"
    "LLDB.framework/Resources/debugserver",
)

DWARFDUMP_CANDIDATES = (
    "/opt/homebrew/opt/llvm/bin/llvm-dwarfdump",
    "/usr/local/opt/llvm/bin/llvm-dwarfdump",
)

# A breakpoint is placed on the line carrying this marker rather than on a
# hard-coded number, so editing a fixture cannot silently move the breakpoint
# onto an unreachable line (a branch not taken never stops, and the suite would
# report "no source locals" for what is really a fixture bug).
BREAK_MARKER = "// BREAK"

SCALAR_SOURCE = """fn compute(seed: i64) -> i64 {
    if (seed <= 0) { return 0 }
    var answer = seed + 1
    var doubled = answer * 2
    var ratio = (doubled as f64) / 2.0
    var ok = ratio == 21.0
    if (!ok) { return 0 }
    print(doubled)  // BREAK
    return compute(seed - seed) + doubled
}
var runtimeSeed = process.args.length > 1000 ? 1 : 20
compute(runtimeSeed)
"""

AGGREGATE_SOURCE = """struct Point {
    x: i32
    y: i32
}
fn make(seed: i32) -> Point {
    if (seed < 0) { return Point{x: 0, y: 0} }
    var p = Point{x: seed + 1, y: seed + 2}
    var q = p
    var total = q.x + q.y
    print(total)  // BREAK
    return q
}
make(20)
"""

# No bare `yield`: that statement form was removed with the go/defer surface
# convergence, and the fixture had kept it, so this scenario failed to parse.
CORO_SOURCE = """fn produce(seed: i64) -> i64 {
    return seed + 1
}
fn worker(seed: i64) -> i64 {
    var task = go produce(seed)
    var answer = await task
    var doubled = answer * 2
    var ratio = (doubled as f64) / 2.0
    var ok = ratio == 21.0
    if (!ok) { return 0 }
    return doubled  // BREAK
}
var task = go worker(20)
print(await task)
"""


@dataclass
class Scenario:
    name: str
    filename: str
    source: str
    expect_output: str
    # Regex the backtrace must match. The emitted symbol carries a module
    # identity hash (foo_<16 hex>_fn_<n>), so this is a pattern, not a literal:
    # a fixed string went stale the moment hashing was introduced.
    symbol_re: str
    locals_expected: tuple[str, ...]
    frame_vars: tuple[str, ...]


SCENARIOS = (
    Scenario(
        name="scalar",
        filename="debug_locals.xr",
        source=SCALAR_SOURCE,
        expect_output="42",
        symbol_re=r"debug_locals_[0-9a-f]+_compute",
        locals_expected=("answer = 21", "doubled = 42", "ratio = 21"),
        frame_vars=("answer", "doubled", "ratio", "ok"),
    ),
    Scenario(
        name="aggregate",
        filename="agg_debug_locals.xr",
        source=AGGREGATE_SOURCE,
        expect_output="43",
        symbol_re=r"agg_debug_locals_[0-9a-f]+_make",
        locals_expected=("total = 43",),
        frame_vars=("p", "q", "total"),
    ),
    Scenario(
        name="coroutine",
        filename="coro_debug_locals.xr",
        source=CORO_SOURCE,
        expect_output="42",
        symbol_re=r"coro_debug_locals_[0-9a-f]+_worker",
        locals_expected=("answer = 21", "doubled = 42"),
        frame_vars=("answer", "doubled"),
    ),
)


class Recorder:
    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0

    def ok(self, name: str) -> None:
        print(f"PASS: {name}")
        self.passed += 1

    def bad(self, name: str, detail: str = "") -> None:
        print(f"FAIL: {name}")
        self.failed += 1
        for line in detail.splitlines()[:40]:
            print(f"    {line}")


def find_debugserver() -> str | None:
    for candidate in DEBUGSERVER_CANDIDATES:
        if os.access(candidate, os.X_OK):
            return candidate
    return shutil.which("debugserver")


def find_dwarfdump() -> str | None:
    override = os.environ.get("LLVM_DWARFDUMP")
    if override and os.access(override, os.X_OK):
        return override
    found = shutil.which("llvm-dwarfdump")
    if found:
        return found
    for candidate in DWARFDUMP_CANDIDATES:
        if os.access(candidate, os.X_OK):
            return candidate
    return None


def marker_line(source: str) -> int:
    """1-based line number carrying the breakpoint marker."""
    for index, line in enumerate(source.splitlines(), start=1):
        if BREAK_MARKER in line:
            return index
    raise ValueError(f"fixture has no {BREAK_MARKER} marker")


def run_scenario(rec: Recorder, scenario: Scenario, xray: Path, ws: workspace.Workspace,
                 dwarfdump: str, lldb: str, debugserver: str,
                 timeout: float | None) -> None:
    label = scenario.name
    source_path = ws.write(scenario.filename, scenario.source)
    binary = ws.path(f"{scenario.name}_bin")

    build = proc.run(
        [xray, "build", "--native", "-g", "--rebuild", "--dump-link-command",
         "-o", binary, source_path],
        timeout=timeout,
    )
    if not build.ok:
        rec.bad(f"{label} debug build", build.combined_text())
        return
    rec.ok(f"{label} debug build")

    log = build.combined_text()
    if "Debug info command: dsymutil" in log:
        rec.ok(f"{label} build runs dsymutil")
    else:
        rec.bad(f"{label} build runs dsymutil", log)
    if "-DXRAY_AOT_DEBUG_LOCALS=1" in log:
        rec.ok(f"{label} build enables AOT source-variable locals")
    else:
        rec.bad(f"{label} build enables AOT source-variable locals", log)

    run = proc.run([binary], timeout=timeout)
    actual = run.stdout.decode("utf-8", "replace").strip()
    if actual == scenario.expect_output:
        rec.ok(f"{label} debug binary output")
    else:
        rec.bad(f"{label} debug binary output",
                f"want {scenario.expect_output!r}, got {actual!r}")

    dsym = Path(str(binary) + ".dSYM")
    if not dsym.is_dir():
        rec.bad(f"{label} build produces dSYM")
        return
    rec.ok(f"{label} build produces dSYM")

    dwarf = proc.run([dwarfdump, "--debug-line", dsym], timeout=timeout)
    if not dwarf.ok:
        rec.bad(f"{label} llvm-dwarfdump runs", dwarf.combined_text())
    elif f'name: "{scenario.filename}"' in dwarf.stdout.decode("utf-8", "replace"):
        rec.ok(f"{label} dSYM line table references {scenario.filename}")
    else:
        rec.bad(f"{label} dSYM line table references {scenario.filename}")

    line = marker_line(scenario.source)
    lldb_argv = [lldb, "--no-lldbinit", "-b",
                 "-o", f"breakpoint set --file {scenario.filename} --line {line}",
                 "-o", "run"]
    for var in scenario.frame_vars:
        lldb_argv.extend(["-o", f"frame variable {var}"])
    lldb_argv.extend(["-o", "bt", "--", str(binary)])

    env = dict(os.environ)
    env["PATH"] = os.path.dirname(debugserver) + os.pathsep + env.get("PATH", "")
    session = proc.run(lldb_argv, env=env, cwd=ws.root, timeout=timeout)
    text = session.combined_text()
    if not session.ok:
        rec.bad(f"{label} lldb runs", text)
        return
    rec.ok(f"{label} lldb runs")

    if "stop reason = breakpoint" in text:
        rec.ok(f"{label} lldb stops at breakpoint")
    else:
        rec.bad(f"{label} lldb stops at breakpoint", text)

    if re.search(scenario.symbol_re, text):
        rec.ok(f"{label} backtrace shows xray-derived function name")
    else:
        rec.bad(f"{label} backtrace shows xray-derived function name", text)

    if f"{scenario.filename}:{line}" in text:
        rec.ok(f"{label} backtrace reports {scenario.filename}:{line}")
    else:
        rec.bad(f"{label} backtrace reports {scenario.filename}:{line}", text)

    for expected in scenario.locals_expected:
        if re.search(re.escape(expected), text):
            rec.ok(f"{label} lldb exposes source local {expected}")
        else:
            rec.bad(f"{label} lldb exposes source local {expected}", text)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Native AOT debug smoke")
    ap.add_argument("xray", nargs="?", default=None)
    ns = ap.parse_args(argv[1:])

    xray = Path(ns.xray or os.environ.get("XRAY_BIN") or (PROJECT_DIR / "build" / "xray"))
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 600)

    print("=== Native Debug Smoke ===")
    print(f"Binary: {xray}")

    if sys.platform != "darwin":
        print("SKIP: native debug lldb smoke currently requires Darwin dSYM")
        return SKIP_EXIT
    if not (xray.is_file() and os.access(xray, os.X_OK)):
        print(f"FAIL: xray binary not executable: {xray}")
        return 1

    lldb = shutil.which("lldb")
    if not lldb:
        print("SKIP: lldb not found")
        return SKIP_EXIT
    if not shutil.which("dsymutil"):
        print("SKIP: dsymutil not found")
        return SKIP_EXIT
    dwarfdump = find_dwarfdump()
    if not dwarfdump:
        print("SKIP: llvm-dwarfdump not found")
        return SKIP_EXIT
    debugserver = find_debugserver()
    if not debugserver:
        print("SKIP: debugserver not found; install full Xcode to run the lldb smoke")
        return SKIP_EXIT

    print(f"lldb:   {lldb}")
    print(f"server: {debugserver}")
    print("")

    rec = Recorder()
    with workspace.Workspace("xray_native_debug") as ws:
        for scenario in SCENARIOS:
            run_scenario(rec, scenario, xray, ws, dwarfdump, lldb, debugserver, timeout)

    print("")
    print(f"=== Results: {rec.passed} passed, {rec.failed} failed ===")
    return 1 if rec.failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

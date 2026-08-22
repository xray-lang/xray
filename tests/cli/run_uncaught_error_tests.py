#!/usr/bin/env python3
"""An uncaught value-return error prints a diagnostic to stderr.

Two wordings, and they must stay byte-identical across the VM and AOT:

    [Uncaught Error] <value>                  - the top-level program
    [Uncaught Error in go coroutine] <value>  - a dropped fire-and-forget `go`

This is a content gate, not a parity gate. The VM/AOT differential net in
tests/diff cannot catch a regression here. The top-level shape regressed once
already -- an elided root (a program that spawns nothing) runs on the native
stack with no main coroutine and never reached run_finalize() where the report
lives. The `go` shape exits 0 on BOTH backends: the diagnostic is the only
observable difference, and stderr comparison is off by default
(XRAY_DIFF_STDERR=0), so a backend that prints nothing still "agrees". Only an
assertion on the actual text keeps either failure from going silent.

Usage: run_uncaught_error_tests.py [xray_binary]
"""

from __future__ import annotations

import os
import sys
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, workspace  # noqa: E402

PROJECT_DIR = Path(__file__).resolve().parents[2]

TOP_ERR = '[Uncaught Error] TopErr.Failed("top-level")'
GO_ERR = '[Uncaught Error in go coroutine] GoErr.Failed("in-go")'

# An elided root spawns nothing, so it runs on the native stack with no main
# coroutine -- the shape that regressed. The scheduled variant (a `go` forces a
# main coroutine) takes the other finalization path, so both stay covered.
ELIDED = '''enum TopErr { Failed(reason: string) }

fn run() {
    throw TopErr.Failed("top-level")
}

print("before")
run()
'''

SCHEDULED = '''enum TopErr { Failed(reason: string) }

fn run() {
    throw TopErr.Failed("top-level")
}

go fn() { }()
print("before")
run()
'''

CAUGHT = '''enum TopErr { Failed(reason: string) }

fn run() {
    throw TopErr.Failed("top-level")
}

try { run() } catch (e) { print("caught") }
'''

# No Task handle and no enclosing scope, so nothing is left to observe the
# error. Both backends must report it, and both exit 0 -- the spawning program
# itself completed normally.
IN_GO = '''enum GoErr { Failed(reason: string) }

print("before")
go fn() { throw GoErr.Failed("in-go") }()
'''

# The two shapes that must stay SILENT. Both reach the same finalization path
# with the same error, and only the observer differs -- they are what keeps the
# report from being written as an unconditional print.
#   (a) a Task handle: the error is delivered to whoever awaits it.
OBSERVED = '''enum GoErr { Failed(reason: string) }

fn fail() -> i64 {
    throw GoErr.Failed("observed")
    return 0
}

var task = go fail()
match (task.awaitResult()) {
    TaskResult.Failed(err) -> print("caught")
    _ -> print("unexpected")
}
'''

#   (b) a parent scope: the scope collects the terminal state at scope exit.
SCOPED = '''enum GoErr { Failed(reason: string) }

fn fail() {
    throw GoErr.Failed("scoped")
}

scope {
    go fail()
}
print("after")
'''


class Recorder:
    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0

    def ok(self, label: str) -> None:
        print(f"PASS: {label}")
        self.passed += 1

    def bad(self, *lines: str) -> None:
        for line in lines:
            sys.stderr.write(line + "\n")
        self.failed += 1

    def check(self, label: str, want_rc: int, want_out: str, want_err: str,
              result) -> None:
        got_out = result.stdout.decode("utf-8", "replace").rstrip("\n")
        got_err = result.stderr.decode("utf-8", "replace").rstrip("\n")
        if result.returncode != want_rc:
            self.bad(f"FAIL: {label} - exit code {result.returncode}, "
                     f"want {want_rc}")
        elif got_out != want_out:
            self.bad(f"FAIL: {label} - stdout '{got_out}', want '{want_out}'")
        elif want_err not in got_err:
            self.bad(f"FAIL: {label} - stderr does not contain '{want_err}'",
                     f"  actual stderr: '{got_err}'")
        else:
            self.ok(label)


def build_native(xray: Path, source: Path, out: Path,
                 timeout: float | None) -> Path | None:
    result = proc.run([xray, "build", "--native", source, "-o", out],
                      timeout=timeout)
    return out if result.ok and os.access(out, os.X_OK) else None


def main(argv: list[str]) -> int:
    xray = Path(argv[1] if len(argv) > 1
                else os.environ.get("XRAY_BIN",
                                    str(PROJECT_DIR / "build" / "xray")))
    if not (xray.is_file() and os.access(xray, os.X_OK)):
        sys.stderr.write(f"FAIL: xray not found at {xray}\n")
        return 1
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 600)

    rec = Recorder()
    with workspace.Workspace("xray_uncaught_error") as ws:
        sources = {name: ws.write(f"{name}.xr", body) for name, body in (
            ("elided", ELIDED), ("scheduled", SCHEDULED), ("caught", CAUGHT),
            ("in_go", IN_GO), ("observed", OBSERVED), ("scoped", SCOPED))}

        vm = {name: proc.run([xray, "run", path], timeout=timeout)
              for name, path in sources.items()}

        rec.check("vm elided root reports uncaught error", 1, "before", TOP_ERR,
                  vm["elided"])
        rec.check("vm scheduled root reports uncaught error", 1, "before",
                  TOP_ERR, vm["scheduled"])
        rec.check("vm caught error prints nothing", 0, "caught", "", vm["caught"])
        rec.check("vm dropped go coroutine reports uncaught error", 0, "before",
                  GO_ERR, vm["in_go"])
        rec.check("vm awaited go coroutine stays silent", 0, "caught", "",
                  vm["observed"])
        rec.check("vm scoped go coroutine stays silent", 0, "after", "",
                  vm["scoped"])

        # The native toolchain is optional in this gate: when no provider is
        # READY the AOT leg is skipped rather than failed.
        elided_native = build_native(xray, sources["elided"],
                                     ws.path(platform.exe_name("elided_native")),
                                     timeout)
        if elided_native is None:
            print("SKIP: aot legs - no native toolchain provider available")
        else:
            rec.check("aot elided root reports uncaught error", 1, "before",
                      TOP_ERR, proc.run([elided_native], timeout=timeout))

            go_native = build_native(xray, sources["in_go"],
                                     ws.path(platform.exe_name("in_go_native")),
                                     timeout)
            if go_native is None:
                rec.bad("FAIL: aot dropped go coroutine - native build failed")
            else:
                aot_go = proc.run([go_native], timeout=timeout)
                rec.check("aot dropped go coroutine reports uncaught error", 0,
                          "before", GO_ERR, aot_go)
                # Byte-for-byte, not just "both contain the message": the whole
                # point of routing AOT through its own printer is that the
                # wording cannot drift.
                if vm["in_go"].stderr == aot_go.stderr:
                    rec.ok("vm and aot stderr are byte-identical")
                else:
                    rec.bad("FAIL: vm and aot stderr differ",
                            f"  vm:  {vm['in_go'].stderr!r}",
                            f"  aot: {aot_go.stderr!r}")

            observed_native = build_native(
                xray, sources["observed"],
                ws.path(platform.exe_name("observed_native")), timeout)
            if observed_native is None:
                rec.bad("FAIL: aot awaited go coroutine - native build failed")
            else:
                rec.check("aot awaited go coroutine stays silent", 0, "caught",
                          "", proc.run([observed_native], timeout=timeout))

            scoped_native = build_native(
                xray, sources["scoped"],
                ws.path(platform.exe_name("scoped_native")), timeout)
            if scoped_native is None:
                rec.bad("FAIL: aot scoped go coroutine - native build failed")
            else:
                rec.check("aot scoped go coroutine stays silent", 0, "after", "",
                          proc.run([scoped_native], timeout=timeout))

    print("----------------------------------------")
    print(f"Uncaught error diagnostics: {rec.passed} passed, {rec.failed} failed")
    return 1 if rec.failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

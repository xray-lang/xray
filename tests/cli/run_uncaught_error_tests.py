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
ELIDED = '''enum TopErr { Failed { reason: string } }

fn run() {
    throw TopErr.Failed { reason: "top-level" }
}

print("before")
run()
'''

SCHEDULED = '''enum TopErr { Failed { reason: string } }

fn run() {
    throw TopErr.Failed { reason: "top-level" }
}

go fn() { }()
print("before")
run()
'''

CAUGHT = '''enum TopErr { Failed { reason: string } }

fn run() {
    throw TopErr.Failed { reason: "top-level" }
}

try { run() } catch (e) { print("caught") }
'''

# No Task handle and no enclosing scope, so nothing is left to observe the
# error. Both backends must report it, and both exit 0 -- the spawning program
# itself completed normally.
IN_GO = '''enum GoErr { Failed { reason: string } }

print("before")
go fn() { throw GoErr.Failed { reason: "in-go" } }()
'''

# The two shapes that must stay SILENT. Both reach the same finalization path
# with the same error, and only the observer differs -- they are what keeps the
# report from being written as an unconditional print.
#   (a) a Task handle: the error is delivered to whoever awaits it.
OBSERVED = '''enum GoErr { Failed { reason: string } }

fn fail() -> i64 {
    throw GoErr.Failed { reason: "observed" }
    return 0
}

var task = go fail()
match (task.awaitResult()) {
    TaskResult.Failed { error: err } -> print("caught")
    _ -> print("unexpected")
}
'''

#   (b) a parent scope: the scope collects the terminal state at scope exit.
SCOPED = '''enum GoErr { Failed { reason: string } }

fn fail() {
    throw GoErr.Failed { reason: "scoped" }
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
        elif (want_err and want_err not in got_err) or (not want_err and result.stderr):
            self.bad(f"FAIL: {label} - stderr violates required diagnostic {want_err!r}",
                     f"  actual stderr: '{got_err}'")
        else:
            self.ok(label)


def build_native(rec: Recorder, xray: Path, source: Path, out: Path,
                 timeout: float | None) -> Path | None:
    result = proc.run([xray, "build", "--native", source, "-o", out],
                      timeout=timeout)
    if not result.ok or not os.access(out, os.X_OK):
        rec.bad(f"FAIL: aot {source.stem} - native build failed (exit {result.returncode})",
                result.stdout.decode("utf-8", "replace"),
                result.stderr.decode("utf-8", "replace"))
        return None
    return out


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

        # A compiler rejection is a failed case, never evidence of toolchain absence.
        for name, label, status, output, diagnostic in (
            ("elided", "elided root reports uncaught error", 1, "before", TOP_ERR),
            ("in_go", "dropped go coroutine reports uncaught error", 0, "before", GO_ERR),
            ("observed", "awaited go coroutine stays silent", 0, "caught", ""),
            ("scoped", "scoped go coroutine stays silent", 0, "after", ""),
        ):
            native = build_native(rec, xray, sources[name],
                                  ws.path(platform.exe_name(f"{name}_native")), timeout)
            if native is None:
                continue
            executed = proc.run([native], timeout=timeout)
            rec.check(f"aot {label}", status, output, diagnostic, executed)
            if name == "in_go":
                if vm[name].stderr == executed.stderr:
                    rec.ok("vm and aot stderr are byte-identical")
                else:
                    rec.bad("FAIL: vm and aot stderr differ",
                            f"  vm:  {vm[name].stderr!r}",
                            f"  aot: {executed.stderr!r}")

    print("----------------------------------------")
    print(f"Uncaught error diagnostics: {rec.passed} passed, {rec.failed} failed")
    return 1 if rec.failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

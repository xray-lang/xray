#!/usr/bin/env python3
"""Uncaught panics read the same on the VM, embedded-bytecode and AOT forms.

One line, built from the fault's error code and message:

    [Uncaught Panic] E<code>: <message>

This is a content gate, not a parity gate. The VM/AOT differential net runs
with stderr comparison off by default (XRAY_DIFF_STDERR=0), so a regression
that changed the wording on BOTH backends would still pass it. Only an
assertion on the actual text keeps the report honest.

It also pins the two policy decisions that make cross-backend parity possible:
  - the stack trace is opt-in (XRAY_BACKTRACE), absent by default, so the
    default report matches a backend that carries no unwind state;
  - colour is TTY-gated, so piped output is plain on both backends.

Usage: run_panic_report_tests.py [xray_binary]
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

PROJECT_DIR = Path(__file__).resolve().parents[2]

ESC = "\x1b"
_ANSI = re.compile(r"\x1b\[[0-9;]*m")

DIV_SOURCE = 'var z = 0\nprint("before")\nprint(1 / z)\n'
OOB_SOURCE = 'var xs = [1, 2, 3]\nprint("before")\nxs[9] = 0\n'

DIV_REPORT = "[Uncaught Panic] E0420: division by zero"
OOB_REPORT = "[Uncaught Panic] E0430: array index out of range: 9 (length 3)"
TRACE = "Stack trace:"


def strip_ansi(text: str) -> str:
    """Assertions are colour-independent; the raw-escape check is separate."""
    return _ANSI.sub("", text)


class Recorder:
    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0

    def ok(self, label: str) -> None:
        print(f"PASS: {label}")
        self.passed += 1

    def bad(self, message: str) -> None:
        sys.stderr.write(message + "\n")
        self.failed += 1


def check_run(rec: Recorder, xray: Path, label: str, args: Sequence,
              want_rc: int, want_out: str, want_err: str,
              forbid_err: str | None, timeout: float | None) -> None:
    """Run piped (never a TTY) and assert on exit code, stdout and stderr."""
    result = proc.run([xray] + list(args), timeout=timeout)
    raw_err = result.stderr.decode("utf-8", "replace")
    got_out = result.stdout.decode("utf-8", "replace").rstrip("\r\n")
    got_err = strip_ansi(raw_err)

    if result.returncode != want_rc:
        rec.bad(f"FAIL: {label} - exit {result.returncode}, want {want_rc}")
        return
    if got_out != want_out:
        rec.bad(f"FAIL: {label} - stdout '{got_out}', want '{want_out}'")
        return
    if want_err not in got_err:
        rec.bad(f"FAIL: {label} - stderr lacks '{want_err}'; got '{got_err}'")
        return
    if forbid_err and forbid_err in got_err:
        rec.bad(f"FAIL: {label} - stderr must not contain '{forbid_err}'; "
                f"got '{got_err}'")
        return
    if ESC in raw_err:
        rec.bad(f"FAIL: {label} - piped stderr contains ANSI escapes")
        return
    rec.ok(label)


def check_backtrace(rec: Recorder, xray: Path, div: Path,
                    timeout: float | None) -> None:
    env = dict(os.environ)
    env["XRAY_BACKTRACE"] = "1"
    result = proc.run([xray, "run", div], env=env, timeout=timeout)
    err = strip_ansi(result.stderr.decode("utf-8", "replace"))
    if result.returncode == 1 and DIV_REPORT in err and TRACE in err:
        rec.ok("vm XRAY_BACKTRACE adds stack trace")
    else:
        rec.bad(f"FAIL: vm XRAY_BACKTRACE - want report + '{TRACE}'; got '{err}'")


def check_aot(rec: Recorder, xray: Path, div: Path, work: Path,
              timeout: float | None) -> None:
    """AOT parity, skipped when no native toolchain provider is READY."""
    native = work / platform.exe_name("div_native")
    build = proc.run([xray, "build", "--native", div, "-o", native],
                     timeout=timeout)
    if not build.ok or not os.access(native, os.X_OK):
        print("SKIP: aot div-by-zero - no native toolchain provider available")
        return

    result = proc.run([native], timeout=timeout)
    out = result.stdout.decode("utf-8", "replace").rstrip("\r\n")
    err = strip_ansi(result.stderr.decode("utf-8", "replace"))
    if (result.returncode == 1 and out == "before"
            and DIV_REPORT in err and TRACE not in err):
        rec.ok("aot div-by-zero matches the VM panic report")
    else:
        rec.bad(f"FAIL: aot div-by-zero - rc={result.returncode} out='{out}' "
                f"err='{err}'")


def check_embed(rec: Recorder, xray: Path, div: Path, work: Path,
                timeout: float | None) -> None:
    """The generated entry must map every VM failure to process status 1."""
    generated = work / "div_embed.c"
    emit = proc.run([xray, "build", "-c", div, "-o", generated], timeout=timeout)
    if not emit.ok or not generated.is_file():
        rec.bad("FAIL: embedded div-by-zero - C source generation failed")
        return

    normalized_return = "return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;"
    try:
        generated_text = generated.read_text(encoding="utf-8")
    except OSError as exc:
        rec.bad(f"FAIL: embedded div-by-zero - cannot read generated C: {exc}")
        return
    if normalized_return not in generated_text:
        rec.bad("FAIL: embedded div-by-zero - generated entry leaks the VM result "
                "as a process exit status")
        return
    rec.ok("embedded div-by-zero normalizes the process exit status")

    embedded = work / platform.exe_name("div_embed")
    build = proc.run([xray, "build", div, "-o", embedded], timeout=timeout)
    if not build.ok:
        diagnostic = build.stdout + build.stderr
        if b"failed to start compiler" in diagnostic:
            print("SKIP: embedded div-by-zero runtime - no portable C compiler available")
            return
        rec.bad("FAIL: embedded div-by-zero - binary build failed")
        return
    if not os.access(embedded, os.X_OK):
        rec.bad("FAIL: embedded div-by-zero - binary was not produced")
        return

    result = proc.run([embedded], timeout=timeout)
    out = result.stdout.decode("utf-8", "replace").rstrip("\r\n")
    err = strip_ansi(result.stderr.decode("utf-8", "replace"))
    if (result.returncode == 1 and out == "before"
            and DIV_REPORT in err and TRACE not in err):
        rec.ok("embedded div-by-zero matches the VM panic report")
    else:
        rec.bad(f"FAIL: embedded div-by-zero - rc={result.returncode} out='{out}' "
                f"err='{err}'")


def main(argv: list[str]) -> int:
    xray = Path(argv[1] if len(argv) > 1
                else os.environ.get("XRAY_BIN", str(PROJECT_DIR / "build" / "xray")))
    if not (xray.is_file() and os.access(xray, os.X_OK)):
        sys.stderr.write(f"FAIL: xray not found at {xray}\n")
        return 1
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300)

    rec = Recorder()
    with workspace.Workspace("xray_panic_report") as ws:
        div = ws.write("div.xr", DIV_SOURCE)
        oob = ws.write("oob.xr", OOB_SOURCE)

        # VM: canonical panic report, no trace by default.
        check_run(rec, xray, "vm div-by-zero panic report", ["run", div],
                  1, "before", DIV_REPORT, TRACE, timeout)
        check_run(rec, xray, "vm array-oob panic report", ["run", oob],
                  1, "before", OOB_REPORT, TRACE, timeout)
        check_backtrace(rec, xray, div, timeout)
        check_embed(rec, xray, div, ws.root, timeout)
        check_aot(rec, xray, div, ws.root, timeout)

    print("----------------------------------------")
    print(f"Panic report: {rec.passed} passed, {rec.failed} failed")
    return 1 if rec.failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

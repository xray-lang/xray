#!/usr/bin/env python3
"""Uncaught panics satisfy independent expectations on run and native build.

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
import json
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
    """Require a real native build before checking the independent report."""
    native = work / platform.exe_name("div_native")
    build = proc.run([xray, "build", "--native", div, "-o", native],
                     timeout=timeout)
    if not build.ok or not os.access(native, os.X_OK):
        rec.bad(f"FAIL: aot div-by-zero - native build failed (exit {build.returncode}): "
                + (build.stdout + build.stderr).decode("utf-8", "replace"))
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


def check_default_build(rec: Recorder, xray: Path, div: Path, work: Path,
                timeout: float | None) -> None:
    """The default product entry must report the panic and exit with status 1."""
    generated = work / "div_default.c"
    emit = proc.run([xray, "build", "-c", div, "-o", generated], timeout=timeout)
    if not emit.ok or not generated.is_file():
        rec.bad("FAIL: default build div-by-zero - C source generation failed")
        return

    executable = work / platform.exe_name("div_default")
    build = proc.run([xray, "build", div, "-o", executable], timeout=timeout)
    if not build.ok:
        rec.bad("FAIL: default build div-by-zero - binary build failed: "
                + (build.stdout + build.stderr).decode("utf-8", "replace"))
        return
    if not os.access(executable, os.X_OK):
        rec.bad("FAIL: default build div-by-zero - binary was not produced")
        return

    result = proc.run([executable], timeout=timeout)
    out = result.stdout.decode("utf-8", "replace").rstrip("\r\n")
    err = strip_ansi(result.stderr.decode("utf-8", "replace"))
    if (result.returncode == 1 and out == "before"
            and DIV_REPORT in err and TRACE not in err):
        rec.ok("default build div-by-zero matches the VM panic report")
    else:
        rec.bad(f"FAIL: default build div-by-zero - rc={result.returncode} out='{out}' "
                f"err='{err}'")


def check_sequence_defer(rec: Recorder, xray: Path, work: Path,
                         timeout: float | None) -> None:
    for kind, index in ((kind, index) for kind in ("i64", "string")
                        for index in (1, 3, -1)):
        for store in (False, True):
            label = f"array<{kind}> {'store' if store else 'read'} index {index} with defer"
            source = work / f"defer_{kind}_{index}_{int(store)}.xr"
            replacement = "42" if kind == "i64" else '"newer"'
            initial = "[10, 20, 30]" if kind == "i64" else '["old", "two", "three"]'
            operation = f"values[{index}] = {replacement}" if store else f"print(values[{index}])"
            source.write_text('fn main() {\n' +
                              f' var values: Array<{kind}> = {initial}\n' +
                              ' defer { print("cleanup") }\n ' + operation +
                              '\n}\nmain()\n', encoding="utf-8")
            success = index == 1
            stdout = "cleanup" if store or not success else ("20\ncleanup" if kind == "i64" else "two\ncleanup")
            report = "" if success else (
                f"[Uncaught Panic] E0430: array index out of range: {index} (length 3)")
            check_run(rec, xray, "vm " + label, ["run", source],
                      0 if success else 1, stdout, report,
                      "[Uncaught Panic]" if success else TRACE, timeout)
            native = work / platform.exe_name(f"defer_{kind}_{index}_{int(store)}")
            build = proc.run([xray, "build", source, "-o", native], timeout=timeout)
            if not build.ok or not native.is_file():
                rec.bad(f"FAIL: native {label} build: " +
                        (build.stdout + build.stderr).decode("utf-8", "replace"))
                continue
            check_run(rec, native, "native " + label, [],
                      0 if success else 1, stdout, report,
                      "[Uncaught Panic]" if success else TRACE, timeout)


def check_borrowed_sequence_defer(rec: Recorder, xray: Path, work: Path,
                                   timeout: float | None) -> None:
    for nested in (False, True):
        for index in (1, 3):
            label = f"{'nested' if nested else 'module'} array borrow index {index} with defer"
            source = work / f"borrow_{int(nested)}_{index}.xr"
            if nested:
                text = ('fn main() {\n'
                        ' var xs: Array<Array<i64>> = [[10,20,30], [40,50,60]]\n'
                        ' defer { print("cleanup") }\n'
                        f' print(xs[0][{index}])\n' + '}\nmain()\n')
            else:
                text = ('var xs: Array<i64> = [10,20,30]\n'
                        'fn main() {\n defer { print("cleanup") }\n'
                        f' print(xs[{index}])\n' + '}\nmain()\n')
            source.write_text(text, encoding="utf-8")
            success = index == 1
            stdout = "20\ncleanup" if success else "cleanup"
            report = "" if success else (
                "[Uncaught Panic] E0430: array index out of range: 3 (length 3)")
            check_run(rec, xray, "vm " + label, ["run", source],
                      0 if success else 1, stdout, report,
                      "[Uncaught Panic]" if success else TRACE, timeout)
            native = work / platform.exe_name(f"borrow_{int(nested)}_{index}")
            build = proc.run([xray, "build", source, "-o", native], timeout=timeout)
            if not build.ok or not native.is_file():
                rec.bad(f"FAIL: native {label} build: " +
                        (build.stdout + build.stderr).decode("utf-8", "replace"))
                continue
            check_run(rec, native, "native " + label, [],
                      0 if success else 1, stdout, report,
                      "[Uncaught Panic]" if success else TRACE, timeout)


def check_assert_messages(rec: Recorder, xray: Path, work: Path,
                          timeout: float | None) -> None:
    cases = [(True, "ready"), (False, "消息: ready"), (False, ""), (False, "long" * 800)]
    for index, (success, message) in enumerate(cases):
        source = work / f"assert_message_{index}.xr"
        source.write_text(
            'fn message() -> string { print("message"); return ' + json.dumps(message, ensure_ascii=False) + '}\n'
            'fn checked() { defer { print("cleanup") }; assert(' + str(success).lower() + ', message()) }\n'
            'print("before")\nchecked()\nprint("after")\n', encoding="utf-8")
        expected_out = b"before\nmessage\ncleanup\n" + (b"after\n" if success else b"")
        expected_err = b"" if success else ("[Uncaught Panic] E0001: " + message + "\n").encode("utf-8")
        native = work / platform.exe_name(f"assert_message_{index}")
        commands = [("vm", [xray, "run", source])]
        build = proc.run([xray, "build", source, "-o", native], timeout=timeout)
        if build.ok and native.is_file():
            commands.append(("native", [native]))
        else:
            rec.bad(f"FAIL: assertion message {index} native build: " +
                    (build.stdout + build.stderr).decode("utf-8", "replace"))
        for backend, command in commands:
            result = proc.run(command, timeout=timeout)
            if (result.returncode == (0 if success else 1) and
                    result.stdout == expected_out and result.stderr == expected_err):
                rec.ok(f"{backend} assertion message {index} evaluates once and renders exact bytes")
            else:
                rec.bad(f"FAIL: {backend} assertion message {index}: rc={result.returncode} "
                        f"stdout={result.stdout!r} stderr={result.stderr!r}")


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
        check_assert_messages(rec, xray, ws.root, timeout)
        check_sequence_defer(rec, xray, ws.root, timeout)
        check_borrowed_sequence_defer(rec, xray, ws.root, timeout)
        check_backtrace(rec, xray, div, timeout)
        check_default_build(rec, xray, div, ws.root, timeout)
        check_aot(rec, xray, div, ws.root, timeout)

    print("----------------------------------------")
    print(f"Panic report: {rec.passed} passed, {rec.failed} failed")
    return 1 if rec.failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

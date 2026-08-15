"""The one subprocess entry point for every suite.

Suites do not call `subprocess` directly. Routing every child through here means
one definition of the things each shell script re-decided: bytes vs text,
timeout and what happens after it, and where a failing command's full context
goes so the report can quote it.

Bytes by default follows task 257: an external tool's stdout/stderr is a byte
stream. We do not guess an encoding. A caller that knows a stream is UTF-8
(a protocol it defined) decodes it itself, at the boundary, on purpose.
"""

from __future__ import annotations

import os
import signal
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping, Sequence


@dataclass(frozen=True)
class ProcResult:
    """Outcome of one child process. Streams are bytes; the caller decodes."""

    argv: tuple
    returncode: int
    stdout: bytes
    stderr: bytes
    timed_out: bool

    @property
    def ok(self) -> bool:
        return self.returncode == 0 and not self.timed_out

    def stdout_text(self, errors: str = "strict") -> str:
        """Decode stdout as UTF-8. Only call when the stream is known text."""
        return self.stdout.decode("utf-8", errors)

    def stderr_text(self, errors: str = "strict") -> str:
        return self.stderr.decode("utf-8", errors)

    def combined_text(self, errors: str = "replace") -> str:
        """stdout then stderr, for human-facing failure excerpts."""
        return self.stdout.decode("utf-8", errors) + self.stderr.decode("utf-8", errors)


def _terminate_tree(proc: "subprocess.Popen") -> None:
    """Kill a timed-out child and any group it leads.

    On POSIX the child is its own process group (start_new_session), so one
    killpg reaches the grandchildren a compiler or linker spawned. Windows
    process groups do not provide forced tree termination, so taskkill /T /F
    walks the process tree rooted at the exact child PID.
    """
    if proc.poll() is not None:
        return
    if os.name == "nt":
        system_root = Path(os.environ.get("SystemRoot") or r"C:\Windows")
        taskkill = system_root / "System32" / "taskkill.exe"
        try:
            killed = subprocess.run(
                [str(taskkill), "/PID", str(proc.pid), "/T", "/F"],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=5,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired):
            proc.kill()
            return
        if killed.returncode != 0 and proc.poll() is None:
            proc.kill()
        return
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        proc.kill()


def run(
    argv: Sequence,
    *,
    cwd: Path | None = None,
    env: Mapping | None = None,
    stdin: bytes | None = None,
    timeout: float | None = None,
    check: bool = False,
) -> ProcResult:
    """Run one command to completion and capture its byte streams.

    timeout terminates the whole child tree and returns timed_out=True with
    whatever was captured, rather than raising, so a runner records a timeout
    as an ordinary red verdict instead of aborting the whole suite.

    check=True raises CommandError on a non-zero exit, for setup steps whose
    failure should stop the run (probe compiles, fixture builds). Gate cases
    that are allowed to fail must leave check=False and inspect .ok.
    """
    argv = [str(a) for a in argv]
    popen_kwargs = dict(
        cwd=str(cwd) if cwd is not None else None,
        env=dict(env) if env is not None else None,
        stdin=subprocess.PIPE if stdin is not None else subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if os.name != "nt":
        # Own process group so a timeout kill reaches the tool's own children.
        popen_kwargs["start_new_session"] = True

    try:
        proc = subprocess.Popen(argv, **popen_kwargs)
    except OSError as exc:
        # A missing or non-executable tool is an ordinary red verdict, reported
        # the way a shell reports it: exit 127 with the reason on stderr. A
        # traceback here would bury the one useful line -- which binary is
        # absent -- under a stack from inside subprocess, and would abort a
        # runner partway instead of letting it report the step it was on.
        result = ProcResult(
            argv=tuple(argv),
            returncode=127,
            stdout=b"",
            stderr=f"{argv[0]}: {exc.strerror}\n".encode("utf-8"),
            timed_out=False,
        )
        if check:
            raise CommandError(result)
        return result

    timed_out = False
    try:
        out, err = proc.communicate(input=stdin, timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        _terminate_tree(proc)
        out, err = proc.communicate()

    result = ProcResult(
        argv=tuple(argv),
        returncode=proc.returncode if proc.returncode is not None else -1,
        stdout=out or b"",
        stderr=err or b"",
        timed_out=timed_out,
    )
    if check and not result.ok:
        raise CommandError(result)
    return result


def run_passthrough(
    argv: Sequence,
    *,
    cwd: Path | None = None,
    env: Mapping | None = None,
    timeout: float | None = None,
) -> int:
    """Run one command with the parent's own stdout/stderr, returning its exit code.

    For runners that only relay a step's output and judge it by the exit code.
    Capturing the two streams separately and echoing them back reorders the
    output: a tool that interleaves progress on stdout with logging on stderr
    comes out with all of one stream and then all of the other. Handing the
    child the real descriptors keeps the interleaving the user would see, which
    is what the shell runners did.

    Use run() instead whenever the output has to be inspected.
    """
    argv = [str(a) for a in argv]
    popen_kwargs = dict(
        cwd=str(cwd) if cwd is not None else None,
        env=dict(env) if env is not None else None,
        stdin=subprocess.DEVNULL,
    )
    if os.name != "nt":
        popen_kwargs["start_new_session"] = True

    # Our own buffered writes must land before the child's, or a label printed
    # here would surface after the output it introduces.
    sys.stdout.flush()
    sys.stderr.flush()

    try:
        proc = subprocess.Popen(argv, **popen_kwargs)
    except OSError as exc:
        sys.stderr.write(f"{argv[0]}: {exc.strerror}\n")
        return 127
    try:
        return proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        _terminate_tree(proc)
        proc.wait()
        return 124


class CommandError(RuntimeError):
    """A check=True command failed. Carries the full result for the report."""

    def __init__(self, result: ProcResult) -> None:
        self.result = result
        shown = " ".join(result.argv)
        state = "timed out" if result.timed_out else f"exit {result.returncode}"
        super().__init__(f"command {state}: {shown}")

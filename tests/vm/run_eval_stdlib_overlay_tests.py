#!/usr/bin/env python3
"""`-e` / `eval -` / `run -` must see the stdlib, including the embedded copy.

Two axes are covered: how the code arrives (a `-e` argument or stdin) and how
the module is imported (whole-module or selective). The last cases point
XRAY_STDLIB_PATH at an empty directory -- the stdlib must still resolve, which
is what proves the embedded copy is reachable rather than the on-disk tree
merely being found by luck.

Output is compared after trimming trailing whitespace and dropping blank lines,
matching the shell version: these check which value came back, not its layout.

Usage: run_eval_stdlib_overlay_tests.py [xray]
"""

from __future__ import annotations

import os
import sys
from dataclasses import dataclass
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, workspace  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent

CLUSTER_CODE = ('import cluster\n'
                'print(cluster.topicMatches("events.*", "events.user"))')
CLUSTER_SELECTIVE = ('import { topicMatches } from cluster\n'
                     'print(topicMatches("events.*", "events.user"))')
HTTP_WHOLE = "import http\nprint(http.router)"
HTTP_SELECTIVE = "import { router } from http\nprint(router)"


@dataclass(frozen=True)
class Case:
    label: str
    expected: str
    code: str
    # "arg" passes the code with -e; "eval"/"run" feed it on stdin.
    mode: str
    # Point XRAY_STDLIB_PATH at an empty directory to force the embedded stdlib.
    empty_stdlib: bool = False


CASES: tuple[Case, ...] = (
    Case("eval_http_whole", "<fn>", HTTP_WHOLE, "arg"),
    Case("eval_http_selective", "<fn>", HTTP_SELECTIVE, "arg"),
    Case("eval_cluster_whole", "true", CLUSTER_CODE, "eval"),
    Case("eval_cluster_selective", "true", CLUSTER_SELECTIVE, "eval"),
    Case("eval_stdin_cluster", "true", CLUSTER_CODE, "eval"),
    Case("run_stdin_cluster", "true", CLUSTER_CODE, "run"),
    Case("eval_embedded_http_empty_stdlib_path", "<fn>", HTTP_WHOLE, "arg",
         empty_stdlib=True),
    Case("eval_embedded_cluster_empty_stdlib_path", "true", CLUSTER_CODE, "eval",
         empty_stdlib=True),
)


def normalize(data: bytes) -> str:
    """Trailing whitespace trimmed and blank lines dropped, as the shell did."""
    lines = [line.rstrip() for line in data.decode("utf-8", "replace").splitlines()]
    return "\n".join(line for line in lines if line)


def run_case(xray: Path, case: Case, empty_stdlib: Path,
             timeout: float | None) -> str | None:
    """None on success, else a failure description."""
    env = dict(os.environ)
    if case.empty_stdlib:
        env["XRAY_STDLIB_PATH"] = str(empty_stdlib)

    if case.mode == "arg":
        result = proc.run([xray, "-e", case.code], env=env, cwd=PROJECT_DIR,
                          timeout=timeout)
    else:
        result = proc.run([xray, case.mode, "-"], env=env, cwd=PROJECT_DIR,
                          stdin=(case.code + "\n").encode(), timeout=timeout)

    if not result.ok:
        return f"command failed\n{result.combined_text()}"
    actual = normalize(result.stdout)
    if actual != case.expected:
        return (f"expected {case.expected!r}, got {actual!r}\n"
                f"{result.stderr.decode('utf-8', 'replace')}")
    return None


def main(argv: list[str]) -> int:
    xray = Path(argv[1] if len(argv) > 1
                else os.environ.get("XRAY", str(PROJECT_DIR / "build" / "xray")))
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300)

    with workspace.Workspace("xray_eval_stdlib_overlay") as ws:
        empty_stdlib = ws.subdir("empty-stdlib")
        for case in CASES:
            problem = run_case(xray, case, empty_stdlib, timeout)
            if problem is not None:
                sys.stderr.write(f"FAIL {case.label}: {problem}\n")
                return 1

    print("eval stdlib overlay tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

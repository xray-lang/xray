#!/usr/bin/env python3
"""The embedded stdlib is complete enough to run without any stdlib on disk.

Every case points XRAY_STDLIB_PATH at an empty directory, so nothing can be
satisfied from the source tree: whatever resolves came out of the binary. The
first group checks the VM directly; the second runs backend-diff cases, which
additionally require the embedded copy to produce identical results on the VM
and AOT paths.

Usage: run_stdlib_embedded_layout_tests.py [xray] [python]
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
ROOT = SCRIPT_DIR.parent.parent

CLUSTER_CODE = ('import cluster\n'
                'print(cluster.topicMatches("events.*", "events.user"))')


@dataclass(frozen=True)
class Case:
    label: str
    expected: str
    code: str


CASES: tuple[Case, ...] = (
    Case("vm_path_empty_stdlib", "demo.xr",
         'import path; print(path.basename(path.from("/tmp/demo.xr")))'),
    Case("vm_http_empty_stdlib", "<fn>",
         "import http\nprint(http.router)"),
    Case("vm_cluster_empty_stdlib", "true", CLUSTER_CODE),
)

DIFF_CASES: tuple[str, ...] = (
    "tests/diff/cases/semantics/stdlib/probe_module_shapes.xr",
    "tests/diff/cases/semantics/stdlib/http_pure_helpers_direct.xr",
    "tests/diff/cases/semantics/stdlib/cluster_protocol_pure_direct.xr",
    "tests/diff/cases/semantics/stdlib/parallel_api_reference.xr",
    "tests/diff/cases/semantics/stdlib/parallel_plan_close_lifecycle.xr",
    "tests/diff/cases/semantics/stdlib/parallel_plan_close_during_dispatch.xr",
)


def normalize(data: bytes) -> str:
    """Trailing whitespace trimmed and blank lines dropped, as the shell did."""
    lines = [line.rstrip() for line in data.decode("utf-8", "replace").splitlines()]
    return "\n".join(line for line in lines if line)


def run_case(xray: Path, case: Case, env: dict,
             timeout: float | None) -> str | None:
    """None on success, else a failure description."""
    result = proc.run([xray, "run", "-"], env=env, cwd=ROOT,
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
                else os.environ.get("XRAY", str(ROOT / "build" / "xray")))
    python = argv[2] if len(argv) > 2 else os.environ.get("PYTHON", sys.executable)
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 600)

    with workspace.Workspace("xray_stdlib_embedded_layout") as ws:
        empty_stdlib = ws.subdir("empty-stdlib")
        cache_root = ws.subdir("diff-cache")

        env = dict(os.environ)
        env["XRAY_STDLIB_PATH"] = str(empty_stdlib)

        for case in CASES:
            problem = run_case(xray, case, env, timeout)
            if problem is not None:
                sys.stderr.write(f"FAIL {case.label}: {problem}\n")
                return 1

        diff_env = dict(env)
        diff_env["XRAY_TEST_CACHE_ROOT"] = str(cache_root)
        # One job: these cases are about resolution and parity, and a parallel
        # sweep only adds scheduling noise to a six-case run.
        diff_env["XRAY_DIFF_MAX_AUTO_JOBS"] = "1"
        for case_file in DIFF_CASES:
            diff_env["XRAY_DIFF_SINGLE_CASE"] = case_file
            code = proc.run_passthrough(
                [python, ROOT / "tests" / "diff" / "run_backend_diff.py", xray],
                env=diff_env, cwd=ROOT, timeout=timeout)
            if code != 0:
                return 1

    print("stdlib embedded layout tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

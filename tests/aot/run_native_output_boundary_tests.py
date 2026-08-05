#!/usr/bin/env python3
"""Native-output package boundary: build isolation, VM refusal, layout checking.

A package with a native unit must (1) materialize its C output parameter, (2)
never leave a target-specific object inside the package tree -- including when
two cross-target builds run concurrently, (3) be refused by the VM when the
manifest declares vm=unsupported, and (4) reject a C header whose layout no
longer matches what the Xray side expects.

Point (2) is why the two cross builds run in parallel rather than in sequence:
a shared package-tree object is a race, and a serial run would not expose it.
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, scheduler, workspace  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent
DEFAULT_FIXTURE = PROJECT_DIR / "tests" / "fixtures" / "native_output"

CROSS_TARGETS = (
    ("x86_64-linux-musl", "native-output-linux"),
    ("x86_64-windows-gnu", "native-output-windows.exe"),
)

LEAKED_OBJECT = "native_output.o"


def fail(message: str, log: str = "") -> int:
    sys.stderr.write(message + "\n")
    if log:
        sys.stderr.write("\n".join(log.splitlines()[:160]) + "\n")
    return 1


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Native output boundary tests")
    ap.add_argument("xray", nargs="?", default=None)
    ap.add_argument("fixture", nargs="?", default=None)
    ns = ap.parse_args(argv[1:])

    xray = Path(ns.xray or os.environ.get("XRAY_BIN") or (PROJECT_DIR / "build" / "xray"))
    fixture = Path(ns.fixture) if ns.fixture else DEFAULT_FIXTURE
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300)

    with workspace.Workspace("xray_native_output") as ws:
        project = ws.root / "project"
        shutil.copytree(fixture, project)

        # 1. Host build materializes the C output parameter.
        out_bin = ws.path("native-output")
        result = proc.run(
            [xray, "build", "--native", "--cache-dir", ws.path("cache"),
             "-o", out_bin, "main.xr"],
            cwd=project, timeout=timeout,
        )
        if not result.ok:
            return fail("native build failed", result.combined_text())

        run = proc.run([out_bin], timeout=timeout)
        if run.stdout.decode("utf-8", "replace").strip() != "42":
            return fail("native output parameter did not materialize CValue{42}",
                        run.combined_text())

        if (project / LEAKED_OBJECT).exists():
            return fail("native unit leaked a target-specific object into the package tree")

        # 2. Two cross-target builds at once must not share a package-tree object.
        cross_cache = ws.path("cross-cache")

        def cross(target: str, out_name: str):
            return proc.run(
                [xray, "build", "--native", "--target", target,
                 "--cache-dir", cross_cache, "-o", ws.path(out_name), "main.xr"],
                cwd=project, timeout=timeout,
            )

        sched = scheduler.Scheduler({scheduler.LINK: 2})
        tasks = [
            scheduler.Task(key=target, fn=(lambda t=target, o=out: cross(t, o)),
                           tag=scheduler.LINK)
            for target, out in CROSS_TARGETS
        ]
        results = sched.run(tasks)
        for target, out_name in CROSS_TARGETS:
            r = results.get(target)
            if isinstance(r, BaseException):
                raise r
            if not r.ok:
                return fail(f"cross build failed for {target}", r.combined_text())
            produced = ws.path(out_name)
            if not (produced.is_file() and produced.stat().st_size > 0):
                return fail("parallel cross-target native outputs were not produced")
        if (project / LEAKED_OBJECT).exists():
            return fail("parallel cross-target native units shared a package-tree object")

        # 3. The VM must refuse a package declared vm=unsupported.
        vm = proc.run([xray, "run", "main.xr"], cwd=project, timeout=timeout)
        if vm.ok:
            return fail("VM unexpectedly accepted a package declared vm=unsupported",
                        vm.combined_text())
        if "does not support the VM backend" not in vm.combined_text():
            return fail("VM refusal carried an unexpected diagnostic", vm.combined_text())

        # 4. A C header whose layout drifted must be rejected, not silently used.
        bad = ws.root / "bad-layout"
        shutil.copytree(fixture, bad)
        header = bad / "native_output.h"
        header.write_text(
            header.read_text(encoding="utf-8").replace("int64_t value", "int32_t value"),
            encoding="utf-8",
        )
        bad_result = proc.run(
            [xray, "build", "--native", "--cache-dir", ws.path("bad-cache"),
             "-o", ws.path("bad-output"), "main.xr"],
            cwd=bad, timeout=timeout,
        )
        if bad_result.ok:
            return fail("mismatched C header layout unexpectedly passed",
                        bad_result.combined_text())
        if "E-NATIVE-LAYOUT" not in bad_result.combined_text():
            return fail("layout mismatch was rejected without E-NATIVE-LAYOUT",
                        bad_result.combined_text())

    print("native output boundary tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

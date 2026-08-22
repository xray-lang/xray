#!/usr/bin/env python3
"""AOT cross-target smoke: build for nine targets through Zig, assert the link.

Each target builds a standalone program and the link command is inspected: a
host-only flag leaking into a cross link (`-Wl,-dead_strip`, `-lxray_aot_core`)
means the driver reused host state for a foreign target, which produces a
binary that happens to link today and breaks on the target.

Three focused cases beyond the matrix: runtime-dependent code must still be
*rejected* for cross targets (no per-target runtime objects yet), Windows time
must build without the host AOT core, and a Windows shared library must use
`-shared` rather than Darwin's `-dynamiclib`.

Replaces run_aot_cross_smoke.sh.
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, scheduler, workspace  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent
RUNTIME_SRC = SCRIPT_DIR / "coro" / "typed_channel.xr"

SKIP_EXIT = 77

CROSS_TARGETS = (
    "i386-linux-musl",
    "x86_64-linux-musl",
    "arm-linux-gnueabi",
    "aarch64-linux-musl",
    "powerpc64-linux-musl",
    "powerpc64le-linux-musl",
    "loongarch64-linux-musl",
    "x86_64-windows-gnu",
    "aarch64-windows-gnu",
)

BASIC_SOURCE = "fn answer() -> i64 {\n    return 42\n}\n\nanswer()\n"
TIME_SOURCE = (
    "import time\n\n"
    "print(time.now() > 0)\n"
    "print(time.nanos() > 0)\n"
    "print(time.clock() >= 0)\n"
)
SHARED_SOURCE = "export fn answer() -> i64 {\n    return 42\n}\n"
SHARED_MANIFEST = """[package]
name = "test/windows-shared-cross"
version = "0.1.0"

[[export.c]]
xray = "answer"
symbol = "xr_answer"
visibility = "default"
header = true
"""


@dataclass
class Result:
    label: str
    ok: bool
    detail: str = ""
    log: str = ""
    described: str = ""


def target_suffix(target: str) -> str:
    return ".exe" if "windows" in target else ""


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="AOT cross-target smoke")
    ap.add_argument("xray", nargs="?", default=None)
    ns = ap.parse_args(argv[1:])

    xray = Path(ns.xray or os.environ.get("XRAY_BIN") or (PROJECT_DIR / "build" / "xray"))
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 600)

    print("=== AOT Cross-Target Smoke Tests ===")
    print(f"Binary: {xray}")

    if not (xray.is_file() and os.access(xray, os.X_OK)):
        print(f"FAIL: xray binary not executable: {xray}")
        return 1

    zig = os.environ.get("XRAY_ZIG") or shutil.which("zig")
    if not zig or not os.access(zig, os.X_OK):
        print("SKIP: Zig not found; set XRAY_ZIG=/path/to/zig")
        return SKIP_EXIT

    xray_dir = xray.resolve().parent
    cache_root = Path(os.environ.get("XRAY_ZIG_CACHE_ROOT", str(xray_dir / "zig-cache")))

    with workspace.Workspace("xray_aot_cross_smoke") as ws:
        env = dict(os.environ)
        env["XRAY_ZIG"] = str(zig)
        env.setdefault("ZIG_GLOBAL_CACHE_DIR", str(cache_root / "global"))
        env["ZIG_LOCAL_CACHE_DIR"] = str(ws.path("zig-local-cache"))
        Path(env["ZIG_GLOBAL_CACHE_DIR"]).mkdir(parents=True, exist_ok=True)
        Path(env["ZIG_LOCAL_CACHE_DIR"]).mkdir(parents=True, exist_ok=True)

        print(f"Work:   {ws.root}")
        print(f"Zig:    {zig}")
        print(f"Cache:  {env['ZIG_GLOBAL_CACHE_DIR']}")
        print("")

        basic = ws.write("basic_cross_standalone.xr", BASIC_SOURCE)
        time_src = ws.write("time_cross_standalone.xr", TIME_SOURCE)
        shared_dir = ws.subdir("shared_cross")
        (shared_dir / "main.xr").write_text(SHARED_SOURCE, encoding="utf-8")
        (shared_dir / "xray.toml").write_text(SHARED_MANIFEST, encoding="utf-8")

        def describe(path: Path) -> str:
            if not shutil.which("file"):
                return ""
            out = proc.run(["file", path], timeout=120)
            return out.stdout.decode("utf-8", "replace").strip()

        def cross_build(target: str) -> Result:
            out = ws.path(f"basic_cross_{target}{target_suffix(target)}")
            r = proc.run(
                [xray, "build", "--native", "--target", target,
                 "--dump-link-command", "-o", out, basic],
                env=env, timeout=timeout,
            )
            log = r.combined_text()
            if not r.ok:
                return Result(target, False, "build failed", log)
            # A host-only strip flag in a cross link means host state leaked.
            if "-Wl,-dead_strip" in log:
                return Result(target, False, "host-only dead_strip leaked into cross link", log)
            if not (out.is_file() and out.stat().st_size > 0):
                return Result(target, False, "missing output")
            return Result(target, True, described=describe(out))

        def runtime_rejection() -> Result:
            out = ws.path("runtime_x86_64-linux-musl")
            r = proc.run(
                [xray, "build", "--native", "--target", "x86_64-linux-musl",
                 "-o", out, RUNTIME_SRC],
                env=env, timeout=timeout,
            )
            log = r.combined_text()
            if r.ok:
                return Result("runtime-rejection", False,
                              "runtime-dependent cross build unexpectedly succeeded", log)
            if "cannot consume runtime objects" not in log:
                return Result("runtime-rejection", False, "wrong rejection", log)
            return Result("runtime-rejection", True)

        def windows_time() -> Result:
            out = ws.path("time_x86_64-windows-gnu.exe")
            r = proc.run(
                [xray, "build", "--native", "--target", "x86_64-windows-gnu",
                 "--dump-link-command", "-o", out, time_src],
                env=env, timeout=timeout,
            )
            log = r.combined_text()
            if not r.ok:
                return Result("windows-time", False, "build failed", log)
            if "-lxray_aot_core" in log:
                return Result("windows-time", False, "host AOT core leaked into cross link", log)
            if not (out.is_file() and out.stat().st_size > 0):
                return Result("windows-time", False, "missing output")
            return Result("windows-time", True, described=describe(out))

        def windows_shared() -> Result:
            out = ws.path("shared_x86_64-windows-gnu.dll")
            r = proc.run(
                [xray, "build", "--native", "--artifact", "shared-library", "--target", "x86_64-windows-gnu",
                 "--dump-link-command", "-o", out, shared_dir / "main.xr"],
                env=env, timeout=timeout,
            )
            log = r.combined_text()
            if not r.ok:
                return Result("windows-shared", False, "build failed", log)
            # Darwin's -dynamiclib must never be used for a Windows DLL.
            if "-shared" not in log or "-dynamiclib" in log:
                return Result("windows-shared", False, "wrong target shared-library flag", log)
            if not (out.is_file() and out.stat().st_size > 0):
                return Result("windows-shared", False, "missing output")
            return Result("windows-shared", True, described=describe(out))

        jobs = [scheduler.Task(key=t, fn=(lambda x=t: cross_build(x)), tag=scheduler.LINK)
                for t in CROSS_TARGETS]
        jobs.append(scheduler.Task(key="runtime-rejection", fn=runtime_rejection,
                                   tag=scheduler.LINK))
        jobs.append(scheduler.Task(key="windows-time", fn=windows_time, tag=scheduler.LINK))
        jobs.append(scheduler.Task(key="windows-shared", fn=windows_shared, tag=scheduler.LINK))

        sched = scheduler.Scheduler({scheduler.LINK: platform.cpu_count()})
        by_key = sched.run(jobs)

        order = list(CROSS_TARGETS) + ["runtime-rejection", "windows-time", "windows-shared"]
        passed = failed = 0
        for key in order:
            value = by_key.get(key)
            if isinstance(value, BaseException):
                raise value
            print(f"  {value.label:<28}", end="")
            if value.ok:
                print("PASS")
                passed += 1
                if value.described:
                    print(f"    {value.described}")
            else:
                print(f"FAIL ({value.detail})")
                failed += 1
                for line in value.log.splitlines()[:20]:
                    print(f"    {line}")

    print("")
    print(f"=== Results: {passed} passed, {failed} failed, 0 skipped ===")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

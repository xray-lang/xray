#!/usr/bin/env python3
"""An installed Xray layout must discover its bundled Zig, not the dev shell's.

The smoke stages a package tree:

    <tmp>/pkg/bin/xray
    <tmp>/pkg/libexec/xray/zig/zig

then runs it with XRAY_ZIG unset and PATH restricted to /usr/bin:/bin. Success
therefore means discovery came from the bundled layout: a developer shell that
happened to have zig on PATH cannot make this pass.

The default mode dry-runs the link, because this gate owns *discovery and
command construction*; run_aot_cross_smoke owns real cross links. Set
XRAY_BUNDLED_ZIG_REAL_BUILD=1 to link for real.
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
from xraytest import platform, proc, workspace  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent

SKIP_EXIT = 77
RESTRICTED_PATH = "/usr/bin:/bin"
CROSS_TARGET = "x86_64-linux-musl"

PROBE_SOURCE = "fn answer() -> int {\n    return 42\n}\n\nanswer()\n"


def find_zig() -> "Path | None":
    """Zig for staging: XRAY_ZIG wins, else PATH. Resolved through symlinks so
    the bundled root points at the real toolchain directory."""
    override = os.environ.get("XRAY_ZIG")
    if override:
        candidate = Path(override) if "/" in override else (
            Path(shutil.which(override)) if shutil.which(override) else None)
        if candidate and candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate.resolve()
        return None
    found = shutil.which("zig")
    return Path(found).resolve() if found else None


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Bundled Zig discovery smoke")
    ap.add_argument("xray", nargs="?", default=None)
    ns = ap.parse_args(argv[1:])

    xray = Path(ns.xray or os.environ.get("XRAY_BIN") or (PROJECT_DIR / "build" / "xray"))
    if not (xray.is_file() and os.access(xray, os.X_OK)):
        print(f"FAIL: xray binary not executable: {xray}")
        return 1
    xray = xray.resolve()
    xray_dir = xray.parent
    real_build = platform.env_flag("XRAY_BUNDLED_ZIG_REAL_BUILD")
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 600)

    zig = find_zig()
    if zig is None:
        print("SKIP: Zig not found; set XRAY_ZIG=/path/to/zig or install Zig")
        return SKIP_EXIT
    zig_root = zig.parent
    if not (zig_root / "lib").is_dir():
        print(f"SKIP: Zig root has no lib directory, cannot model bundled Zig: {zig_root}")
        return SKIP_EXIT

    if not (xray_dir / "cmake_install.cmake").is_file():
        print("SKIP: bundled Zig smoke requires a build-tree xray to stage Core+SDK")
        return SKIP_EXIT

    with workspace.Workspace("xray_aot_bundled_zig") as ws:
        pkg = ws.path("pkg")
        install = proc.run(["cmake", "--install", xray_dir, "--prefix", pkg], timeout=timeout)
        if not install.ok:
            print("FAIL: could not stage package layout from build tree")
            return 1

        libexec = pkg / "libexec" / "xray"
        libexec.mkdir(parents=True, exist_ok=True)
        (libexec / "zig").symlink_to(zig_root)

        cache_root = Path(os.environ.get("XRAY_ZIG_CACHE_ROOT", str(xray_dir / "zig-cache")))
        env = dict(os.environ)
        env.pop("XRAY_ZIG", None)
        env["PATH"] = RESTRICTED_PATH
        env.setdefault("ZIG_GLOBAL_CACHE_DIR", str(cache_root / "global"))
        env.setdefault("ZIG_LOCAL_CACHE_DIR", str(cache_root / "bundled-smoke-local"))
        Path(env["ZIG_GLOBAL_CACHE_DIR"]).mkdir(parents=True, exist_ok=True)
        Path(env["ZIG_LOCAL_CACHE_DIR"]).mkdir(parents=True, exist_ok=True)

        source = ws.write("basic_bundled_zig.xr", PROBE_SOURCE)
        out = ws.path("basic_bundled_zig")
        staged_xray = pkg / "bin" / "xray"
        expected_zig = str(zig)

        print("=== AOT Bundled Zig Smoke ===")
        print(f"Binary:       {xray}")
        print(f"Zig root:     {zig_root}")
        print(f"Package root: {pkg}")
        print(f"Expected Zig: {expected_zig}")
        print(f"Mode:         {'real-link' if real_build else 'dry-run-link'}")
        print("")

        doctor = proc.run(
            [staged_xray, "toolchain", "doctor", "--target", CROSS_TARGET, "--provider", "zig"],
            env=env, timeout=timeout,
        )
        if not doctor.ok:
            print("FAIL: bundled toolchain doctor failed")
            for line in doctor.combined_text().splitlines()[:40]:
                print(f"    {line}")
            return 1
        if expected_zig not in doctor.combined_text():
            print("FAIL: doctor did not report bundled Zig path")
            for line in doctor.combined_text().splitlines()[:40]:
                print(f"    {line}")
            return 1

        build_argv = [staged_xray, "build", "--native", "--target", CROSS_TARGET,
                      "--dump-link-command"]
        if not real_build:
            build_argv.append("--dry-run-link")
        build_argv.extend(["-o", out, source])

        build = proc.run(build_argv, env=env, timeout=timeout)
        if not build.ok:
            print("FAIL: bundled Zig cross build failed")
            for line in build.combined_text().splitlines()[:60]:
                print(f"    {line}")
            return 1
        if expected_zig not in build.combined_text():
            print("FAIL: link command did not use bundled Zig path")
            for line in build.combined_text().splitlines()[:60]:
                print(f"    {line}")
            return 1
        if real_build and not (out.is_file() and out.stat().st_size > 0):
            print("FAIL: missing cross build output")
            return 1

        print("PASS: bundled Zig discovery works with package layout")
        if real_build and shutil.which("file"):
            described = proc.run(["file", out], timeout=timeout)
            for line in described.stdout.decode("utf-8", "replace").splitlines():
                print(f"    {line}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

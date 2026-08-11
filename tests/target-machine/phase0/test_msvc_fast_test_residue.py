#!/usr/bin/env python3
"""Exercise the MSVC direct-link fast-test object placement and cleanup path."""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


RESIDUE_GLOBS = ("*.fast-test.obj", "*.fast-test.o", "*.fast-test.c")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def residue(directory: Path) -> list[Path]:
    found: set[Path] = set()
    for pattern in RESIDUE_GLOBS:
        found.update(path for path in directory.glob(pattern) if path.is_file())
    return sorted(found)


def capture(command: list[str], root: Path, timeout: int = 120,
            env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=root,
        text=True,
        encoding="utf-8",
        errors="replace",
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
        check=False,
        env=env,
    )


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_msvc_fast_test_residue.py <xray-binary>")
    binary = Path(sys.argv[1]).resolve()
    root = Path(__file__).resolve().parents[3]
    require(binary.is_file(), f"xray binary missing: {binary}")
    require(not residue(root), "source root already contains fast-test residue")

    probe = capture([str(binary), "toolchain", "probe", "--json", "--no-run"], root)
    require(probe.returncode == 0, "native toolchain probe failed:\n" + probe.stdout)
    try:
        selection = json.loads(probe.stdout)["selection"]
    except (KeyError, TypeError, json.JSONDecodeError) as error:
        raise RuntimeError("native toolchain probe is not a selection JSON") from error
    require(selection.get("provider") == "msvc", "MSVC provider is required for this regression")
    require(selection.get("ready") is True, "MSVC provider is not ready")

    parent = binary.parent / "target-machine" / "phase0"
    parent.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="msvc-fast-test-residue.", dir=parent))
    cache = work / "cache"
    output = work / "arith.exe"
    source = root / "tests" / "diff" / "cases" / "basic" / "arith.xr"
    try:
        fast_test_environment = {**os.environ, "XRAY_AOT_FAST_TEST_BUILD": "1"}
        result = capture([
            str(binary), "build", "--native", "--toolchain", "msvc",
            "--cache-dir", str(cache), "--output", str(output), "--dump-link-command", str(source),
        ], root, env=fast_test_environment)
        require(result.returncode == 0, "MSVC direct-link build failed:\n" + result.stdout)
        require(output.is_file(), "MSVC direct-link build did not produce its executable")
        normalized = result.stdout.replace("\\", "/")
        expected_prefix = "/Fo" + str(cache.resolve()).replace("\\", "/") + "/"
        require(expected_prefix in normalized and "fast-test.obj" in normalized,
                "MSVC direct-link command did not direct its object into the cache")
        require(not residue(cache), "fast-test object remained in the controlled cache")
        require(not residue(root), "MSVC direct-link left fast-test residue in the source root")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    require(not residue(root), "MSVC cleanup left fast-test residue in the source root")
    print("MSVC direct-link fast-test residue regression: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

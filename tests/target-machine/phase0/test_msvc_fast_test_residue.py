#!/usr/bin/env python3
"""Exercise MSVC Program object placement, cleanup and failed output preservation."""

from __future__ import annotations

import json
import os
import re
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
    output = work / "arith.exe"
    source = root / "tests" / "diff" / "cases" / "basic" / "arith.xr"
    try:
        command = [str(binary), "build", "--native", "--toolchain", "msvc",
                   "--output", str(output), "--dump-link-command", str(source)]
        result = capture(command, root)
        require(result.returncode == 0, "MSVC Program build failed:\n" + result.stdout)
        require(output.is_file(), "MSVC Program build did not produce its executable")
        executed = capture([str(output)], root)
        require(executed.returncode == 0 and executed.stdout == "5\n42\n17\n",
                "native arithmetic output drifted: " + executed.stdout)
        normalized = result.stdout.replace("\\", "/")
        objects = re.findall(r"/Fo([^\r\n]+?/unit[0-2]\.obj)(?: |$)", normalized)
        require(len(objects) == 3, "native compilation did not own all three object paths")
        for name in objects:
            require("xray-program-" in name and not Path(name).parent.exists(),
                    "native private build directory survived success: " + name)
        debugged = capture([*command, "--debug"], root)
        require(debugged.returncode == 0, "MSVC debug build failed: " + debugged.stdout)
        require(Path(str(output) + ".pdb").is_file(), "native debug build lost its PDB")
        require(b"RSDS" in output.read_bytes() and b"arith.exe.pdb" in output.read_bytes(),
                "native executable lacks the published debug information reference")
        for name in re.findall(r"/Fo([^\r\n]+?/unit[0-2]\.obj)(?: |$)",
                               debugged.stdout.replace("\\", "/")):
            require(not Path(name).parent.exists(), "native debug directory survived success")
        for filename in ("without-extension", "custom.binary"):
            exact_output = work / filename
            exact = capture([str(binary), "build", "--toolchain", "msvc", "--debug",
                             "--output", str(exact_output), str(source)], root)
            require(exact.returncode == 0 and exact_output.is_file(),
                    "native build lost the exact output path: " + exact.stdout)
            require(not Path(str(exact_output) + ".exe").exists(),
                    "native provider published an unsolicited extension")
            executed = subprocess.run([str(exact_output)], executable=str(exact_output),
                                      cwd=root, capture_output=True, timeout=120, check=False)
            require(executed.returncode == 0 and executed.stdout == b"5\n42\n17\n",
                    "exact output failed independent native execution")
            require(Path(str(exact_output) + ".pdb").is_file() and
                    (filename + ".pdb").encode() in exact_output.read_bytes(),
                    "exact output lost its matching debug information")
        original = output.read_bytes()
        poisoned = {**os.environ, "CL": "/Dxr_aot_entry_coroutine_descriptor=42"}
        failed = capture(command, root, env=poisoned)
        require(failed.returncode != 0 and "native toolchain command 0 failed" in failed.stdout,
                "compiler failure did not reach the generated Program: " + failed.stdout)
        require(output.read_bytes() == original, "compiler failure overwrote the executable")
        for name in re.findall(r"/Fo([^\r\n]+?/unit[0-2]\.obj)(?: |$)",
                               failed.stdout.replace("\\", "/")):
            require(not Path(name).parent.exists(), "native private directory survived failure")
        require(not list(work.glob("*.xray-*")), "atomic publication left a staging file")
        require(not residue(root), "native compilation left old fast-test residue in source root")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    require(not residue(root), "MSVC cleanup left fast-test residue in the source root")
    print("MSVC Program native placement and failure cleanup: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

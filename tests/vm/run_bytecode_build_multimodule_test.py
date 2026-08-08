#!/usr/bin/env python3
"""Bytecode bundles must preserve exports across the complete module graph."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import sys


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, workspace  # noqa: E402


SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent


def main(argv: list[str]) -> int:
    xray = Path(argv[1] if len(argv) > 1 else PROJECT_DIR / "build" / "xray")
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 60)
    with workspace.Workspace("xray_bytecode_build_multimodule") as work:
        suffix = ".exe" if os.name == "nt" else ""
        binary = work.path(f"multimodule{suffix}")
        typed_binary = work.path(f"multimodule-types{suffix}")
        source_root = work.path("source")
        modules_root = source_root / "modules"
        modules_root.mkdir(parents=True)
        shutil.copy2(SCRIPT_DIR / "bytecode_build_multimodule.xr", source_root / "main.xr")
        shutil.copy2(
            SCRIPT_DIR / "bytecode_build_multimodule_types.xr", source_root / "types.xr"
        )
        for name in (
            "bytecode_build_multimodule_facade.xr",
            "bytecode_build_multimodule_lib.xr",
            "bytecode_build_multimodule_nested.xr",
            "bytecode_build_multimodule_payload.xr",
            "bytecode_build_multimodule_reexported.xr",
            "bytecode_build_multimodule_star_facade.xr",
        ):
            shutil.copy2(SCRIPT_DIR / "modules" / name, modules_root / name)
        build = proc.run(
            [xray, "build", "-O", "2", "-o", binary,
             source_root / "main.xr"],
            cwd=PROJECT_DIR,
            timeout=timeout,
        )
        if not build.ok:
            sys.stderr.write(build.combined_text())
            return 1
        typed_build = proc.run(
            [xray, "build", "-O", "2", "-o", typed_binary,
             source_root / "types.xr"],
            cwd=PROJECT_DIR,
            timeout=timeout,
        )
        if not typed_build.ok:
            sys.stderr.write(typed_build.combined_text())
            return 1

        shutil.rmtree(source_root)
        result = proc.run([binary], cwd=PROJECT_DIR, timeout=timeout)
        typed_result = proc.run([typed_binary], cwd=PROJECT_DIR, timeout=timeout)
        if (
            result.ok
            and result.stdout_text().strip() == "60"
            and not result.stderr.strip()
            and typed_result.ok
            and typed_result.stdout_text().strip() == "1560/64/64"
            and not typed_result.stderr.strip()
        ):
            print("bytecode build multimodule: PASS")
            return 0
        sys.stderr.write(result.combined_text())
        sys.stderr.write(typed_result.combined_text())
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

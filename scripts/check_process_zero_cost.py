#!/usr/bin/env python3
"""Structural zero-cost and Windows Unicode process-boundary gate."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


def require(text: str, needle: str, path: str, errors: list[str]) -> None:
    if needle not in text:
        errors.append(f"{path}: missing required anchor {needle!r}")


def forbid(text: str, needle: str, path: str, errors: list[str]) -> None:
    if needle in text:
        errors.append(f"{path}: forbidden zero-cost/legacy residue {needle!r}")


def read(root: Path, relative: str, errors: list[str]) -> str:
    try:
        return (root / relative).read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        errors.append(f"{relative}: cannot read: {exc}")
        return ""


def check(root: Path) -> list[str]:
    errors: list[str] = []
    build_path = "src/app/cli/xcmd_build.c"
    build = read(root, build_path, errors)
    for needle in (
        "xtc_process_run",
        "xr_pipe_create",
        "MultiByteToWideChar",
        "WideCharToMultiByte",
        "xr_utf8_validate",
        "WriteConsoleW",
    ):
        forbid(build, needle, build_path, errors)
    if build.count("xr_proc_spawn(") < 5:
        errors.append(f"{build_path}: expected direct inherited-stdio provider spawn sites")

    windows_paths = (
        "src/os/win/proc_win.c",
        "src/aot/xrt_sys.h",
        "src/aot/xrt_os.h",
    )
    windows_text = {path: read(root, path, errors) for path in windows_paths}
    for path, text in windows_text.items():
        for needle in (
            "CreateProcessA",
            "STARTUPINFOA",
            "GetEnvironmentStringsA",
            "FreeEnvironmentStringsA",
        ):
            forbid(text, needle, path, errors)
    require(windows_text[windows_paths[0]], "CreateProcessW", windows_paths[0], errors)
    require(windows_text[windows_paths[1]], "CreateProcessW", windows_paths[1], errors)
    require(
        windows_text[windows_paths[0]],
        '"../../shared/xr_win_utf.h"',
        windows_paths[0],
        errors,
    )
    require(
        windows_text[windows_paths[1]],
        '"../shared/xr_win_utf.h"',
        windows_paths[1],
        errors,
    )

    process_paths = (
        "src/app/toolchain/xtc_process.h",
        "src/app/toolchain/xtc_process.c",
        "src/app/toolchain/xtc_discovery.c",
        "src/app/toolchain/xtc_probe.c",
        "src/app/toolchain/xtc_shape_oracle.c",
        "src/app/cli/xcmd_self.c",
    )
    combined = "\n".join(read(root, path, errors) for path in process_paths)
    for needle in ("stdout_data", "stderr_data", "output_truncated", "redact_output"):
        forbid(combined, needle, "toolchain process boundary", errors)
    for needle in ("XrProcessByteBuffer", "stdout_bytes", "stderr_bytes"):
        require(combined, needle, "toolchain process boundary", errors)

    for path in (
        "src/runtime/value/xvalue_print.c",
        "src/aot/xrt_arith.h",
    ):
        text = read(root, path, errors)
        for needle in ("xr_utf8_validate", "MultiByteToWideChar", "WriteConsoleW"):
            forbid(text, needle, path, errors)
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    errors = check(args.root.resolve())
    for error in errors:
        print(error, file=sys.stderr)
    if errors:
        print(f"process zero-cost gate: FAIL ({len(errors)} errors)", file=sys.stderr)
        return 1
    print("process zero-cost gate: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

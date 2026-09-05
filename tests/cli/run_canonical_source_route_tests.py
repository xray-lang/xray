#!/usr/bin/env python3
"""Exercise the single canonical source route exposed by ``xray run``."""

from __future__ import annotations

import argparse
import subprocess
import tempfile
from pathlib import Path


def invoke(binary: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(binary), *arguments],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=30,
    )


def invoke_bytes(binary: Path, *arguments: str) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        [str(binary), *arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=30,
    )


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--fixtures", required=True, type=Path)
    args = parser.parse_args()

    zero = invoke(args.binary, "run", str(args.fixtures / "main_zero.xr"))
    require(zero.returncode == 0, f"zero initializer failed: {zero.stderr}")
    require(zero.stdout == "", f"zero initializer produced stdout: {zero.stdout!r}")

    printed = invoke_bytes(args.binary, "run", str(args.fixtures / "main_print.xr"))
    require(printed.returncode == 0, f"print initializer failed: {printed.stderr!r}")
    require(printed.stdout == b"42\n", f"initializer output bytes drifted: {printed.stdout!r}")

    clock = invoke(args.binary, "run", str(args.fixtures / "time_now.xr"))
    require(clock.returncode == 0, f"clock provider route failed: {clock.stderr!r}")
    clock_text = clock.stdout.strip()
    require(
        clock_text.isdecimal() and int(clock_text) > 0,
        f"clock provider did not produce positive epoch milliseconds: {clock.stdout!r}",
    )

    implicit = invoke(args.binary, str(args.fixtures / "main_zero.xr"))
    require(implicit.returncode == 0, f"implicit source run failed: {implicit.stderr}")

    with tempfile.TemporaryDirectory(prefix="xray-canonical-crlf-") as temporary:
        crlf = Path(temporary) / "main.xr"
        crlf.write_bytes(b"fn main() -> i64 {\r\n    return 0\r\n}\r\n")
        normalized = invoke(args.binary, "run", str(crlf))
        require(
            normalized.returncode == 0,
            f"source fingerprint drifted across CRLF ingestion: {normalized.stderr}",
        )

    declaration_only = invoke(
        args.binary, "run", str(args.fixtures / "declaration_only.xr")
    )
    require(
        declaration_only.returncode == 0,
        f"declaration-only module initializer failed: {declaration_only.stderr}",
    )

    retired = invoke(
        args.binary,
        "run",
        str(args.fixtures / "main_zero.xr"),
        "--semantic-plan",
        "removed.xtp",
    )
    require(retired.returncode != 0, "retired TargetPlan option was accepted")
    require(
        "unknown option '--semantic-plan'" in retired.stderr,
        f"retired option did not fail in the CLI parser: {retired.stderr!r}",
    )

    stdin = invoke(args.binary, "run", "-")
    require(stdin.returncode != 0, "source without file authority was accepted")
    require(
        "XR_RUN_6011: canonical run requires a file-backed source authority" in stdin.stderr,
        f"file authority rejection drifted: {stdin.stderr!r}",
    )

    artifact = invoke(args.binary, "run", str(args.fixtures / "retired.xtp"))
    require(artifact.returncode != 0, "retired artifact path was accepted")
    require(
        "XR_RUN_6013: canonical run accepts only an exact '.xr' source path"
        in artifact.stderr,
        f"retired artifact path did not stop at the source boundary: {artifact.stderr!r}",
    )

    print("canonical source run route passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

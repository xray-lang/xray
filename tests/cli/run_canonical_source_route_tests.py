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


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--fixtures", required=True, type=Path)
    args = parser.parse_args()

    zero = invoke(args.binary, "run", str(args.fixtures / "main_zero.xr"))
    require(zero.returncode == 0, f"zero main failed: {zero.stderr}")
    require(zero.stdout == "", f"zero main produced stdout: {zero.stdout!r}")

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

    seven = invoke(args.binary, "run", str(args.fixtures / "main_seven.xr"))
    require(seven.returncode == 7, f"main status 7 became {seven.returncode}")
    require(
        "XR_RUN_6012: main returned process status 7" in seven.stderr,
        f"non-zero main lost its exact result: {seven.stderr!r}",
    )

    missing = invoke(args.binary, "run", str(args.fixtures / "missing_main.xr"))
    require(missing.returncode != 0, "missing explicit main was accepted")
    require(
        "stage=9 status=entry-rejected" in missing.stderr,
        f"missing main did not fail at exact entry selection: {missing.stderr!r}",
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

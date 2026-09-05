#!/usr/bin/env python3
"""Prove retired artifacts cannot enter the canonical source execution route."""

from __future__ import annotations

import argparse
import subprocess
import tempfile
from pathlib import Path


def run(command: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        text=True,
        encoding="utf-8",
        errors="strict",
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=30,
        check=False,
    )


def require(condition: bool, message: str, output: str = "") -> None:
    if condition:
        print(f"  PASS: {message}")
        return
    raise AssertionError(f"{message}\n{output}")


def require_source_boundary(
    result: subprocess.CompletedProcess[str], message: str
) -> None:
    require(result.returncode != 0, message, result.stdout)
    require(
        "XR_RUN_6013: canonical run accepts only an exact '.xr' source path"
        in result.stdout,
        f"{message} stops at the exact source boundary",
        result.stdout,
    )
    for legacy_diagnostic in ("XR_ARTIFACT_", "XR_EXEC_", "unsupported bytecode"):
        require(
            legacy_diagnostic not in result.stdout,
            f"{message} does not invoke a retired artifact reader",
            result.stdout,
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)

    with tempfile.TemporaryDirectory(prefix="xray-target-artifact-boundary-") as temporary:
        root = Path(temporary)

        for name, payload in (
            ("semantic-module.xsm", b"XRAYXSM\0"),
            ("target-plan.xtp", b"XTPF"),
            ("legacy-bytecode.xrc", b"XRAY\x1e\x00"),
            ("renamed-artifact.bin", b"XRAYXTP\0"),
            ("extensionless-artifact", b"XRAYQQQ\0"),
        ):
            artifact = root / name
            artifact.write_bytes(payload)
            require_source_boundary(
                run([str(binary), "run", str(artifact)]),
                f"retired artifact {name!r} is unreachable",
            )

        source_with_retired_extension = root / "source.xsm"
        source_with_retired_extension.write_text(
            "fn main() -> i64 { return 0 }\n", encoding="utf-8"
        )
        require_source_boundary(
            run([str(binary), "run", str(source_with_retired_extension)]),
            "source text under a retired artifact extension is not executable",
        )

        malformed_source = root / "malformed.xr"
        malformed_source.write_bytes(b"XRAYXTP\0")
        malformed = run([str(binary), "run", str(malformed_source)])
        require(malformed.returncode != 0, "malformed .xr source is rejected", malformed.stdout)
        require(
            "XR_RUN_6001: canonical source build failed" in malformed.stdout,
            "malformed .xr source fails in the shared source owner",
            malformed.stdout,
        )
        require(
            "XR_ARTIFACT_" not in malformed.stdout and "XR_EXEC_" not in malformed.stdout,
            "malformed .xr source cannot recover an artifact identity",
            malformed.stdout,
        )

        source = root / "source.xr"
        source.write_text("fn main() -> i64 { return 0 }\n", encoding="utf-8")
        for option, values in (
            ("--semantic-plan", ["removed.xsm"]),
            ("--timings", []),
            ("--dump-bytecode", []),
            ("--trace", []),
            ("--workers", ["2"]),
        ):
            retired = run([str(binary), "run", str(source), option, *values])
            require(retired.returncode != 0, f"retired option {option} is rejected", retired.stdout)
            require(
                f"unknown option '{option}'" in retired.stdout,
                f"retired option {option} is absent from the command schema",
                retired.stdout,
            )

    print("Target artifact CLI boundary tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

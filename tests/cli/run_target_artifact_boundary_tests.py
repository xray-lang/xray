#!/usr/bin/env python3
"""Prove target artifacts cannot enter source or legacy execution paths."""

from __future__ import annotations

import argparse
import shutil
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


def require_rejection(result: subprocess.CompletedProcess[str], code: str,
                      message: str) -> None:
    require(result.returncode != 0, message, result.stdout)
    require(code in result.stdout, f"{message} uses {code}", result.stdout)
    lowered = result.stdout.lower()
    require("unsupported bytecode version" not in lowered,
            f"{message} bypasses the legacy bytecode reader", result.stdout)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--xtp-writer", type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    writer = args.xtp_writer.resolve(strict=True)

    with tempfile.TemporaryDirectory(prefix="xray-target-artifact-boundary-") as temporary:
        root = Path(temporary)

        xsm = root / "semantic-module.bin"
        xsm.write_bytes(b"XRAYXSM\0")
        require_rejection(run([str(binary), "run", str(xsm)]),
                          "XR_ARTIFACT_2005", "renamed XSM is not executable")

        malformed_xtp = root / "malformed-target-plan.bin"
        malformed_xtp.write_bytes(b"XTPF")
        require_rejection(run([str(binary), "run", str(malformed_xtp)]),
                          "XR_EXEC_5003", "malformed XTP preserves its decoder diagnostic")

        valid_xtp = root / "verified-target-plan.bin"
        write = run([str(writer), "--write", str(valid_xtp)])
        require(write.returncode == 0 and valid_xtp.is_file(),
                "fixture writer produced a verified XTP v10 artifact", write.stdout)
        require_rejection(run([str(binary), "run", str(valid_xtp)]),
                          "XR_ARTIFACT_2007",
                          "XTP candidate stops before authority-bound materialization")

        old_xtp = root / "removed-target-plan.bin"
        old_xtp.write_bytes(b"XRAYXTP\0")
        require_rejection(run([str(binary), "run", str(old_xtp)]),
                          "XR_ARTIFACT_2000", "removed XTP schema is unsupported")

        for length in range(5, 8):
            truncated = root / f"truncated-xsm-{length}.bin"
            truncated.write_bytes(b"XRAYXSM\0"[:length])
            require_rejection(run([str(binary), "run", str(truncated)]),
                              "XR_ARTIFACT_2001",
                              f"{length}-byte XSM prefix is fail-closed")

        for name, payload in (
            ("corrupt-removed.bin", b"XRAYXTP\1"),
            ("unknown-reserved.bin", b"XRAYQQQ\0"),
            ("wrong-xrc-version.bin", b"XRAY\x1d\x00"),
        ):
            reserved = root / name
            reserved.write_bytes(payload)
            require_rejection(run([str(binary), "run", str(reserved)]),
                              "XR_ARTIFACT_2000",
                              f"{name} cannot fall through to legacy XRC")

        conflict = root / "wrong.xtp"
        conflict.write_bytes(b"XRAYXSM\0")
        require_rejection(run([str(binary), "run", str(conflict)]),
                          "XR_ARTIFACT_2006", "extension and byte identity conflict")

        extension_only = root / "source.xsm"
        extension_only.write_text('print("extension is not identity")\n', encoding="utf-8")
        extension_result = run([str(binary), "run", str(extension_only)])
        require_rejection(extension_result, "XR_ARTIFACT_2006",
                          "reserved extension cannot create an artifact")
        require("XR_ARTIFACT_2005" not in extension_result.stdout,
                "extension-only input is not classified as XSM", extension_result.stdout)

        source = root / "legacy-source.xr"
        source.write_text('print("legacy-route")\n', encoding="utf-8")
        xrc = root / "legacy.xrc"
        compiled = run([str(binary), "compile", str(source), "-o", str(xrc)], cwd=root)
        require(compiled.returncode == 0 and xrc.is_file(),
                "transitional XRC fixture compiled", compiled.stdout)
        renamed_xrc = root / "legacy.bin"
        shutil.copyfile(xrc, renamed_xrc)
        executed = run([str(binary), "run", str(renamed_xrc)], cwd=root)
        require(executed.returncode == 0 and "legacy-route" in executed.stdout,
                "legacy XRC remains explicitly magic-routed during cutover",
                executed.stdout)

    print("Target artifact CLI boundary tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

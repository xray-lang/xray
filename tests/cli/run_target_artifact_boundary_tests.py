#!/usr/bin/env python3
"""Prove target artifacts cannot enter source or legacy execution paths."""

from __future__ import annotations

import argparse
import re
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
                "fixture writer produced a verified XTP v29 artifact", write.stdout)
        require_rejection(run([str(binary), "run", str(valid_xtp)]),
                          "XR_ARTIFACT_2007",
                          "XTP candidate stops before authority-bound materialization")

        runtime_xsm = root / "runtime-semantic.bin"
        runtime_xtp = root / "runtime-target.bin"
        write_runtime = run([
            str(writer), "--write-runtime-artifacts", str(runtime_xsm),
            str(runtime_xtp),
        ])
        require(write_runtime.returncode == 0 and runtime_xsm.is_file()
                and runtime_xtp.is_file(),
                "fixture writer produced a matching exact XSM/XTP pair",
                write_runtime.stdout)
        executed_runtime = run([
            str(binary), "run", str(runtime_xtp),
            "--semantic-plan", str(runtime_xsm), "--timings",
        ])
        require(executed_runtime.returncode == 0,
                "matching XSM/XTP executes through runtime generation",
                executed_runtime.stdout)
        require(re.search(r"(?m)^42$", executed_runtime.stdout) is not None,
                "sole scalar artifact prints its exact result",
                executed_runtime.stdout)
        timing_match = re.search(
            r"xray-run-timing artifact_read_ns=(\d+) "
            r"semantic_verify_ns=(\d+) target_verify_ns=(\d+) "
            r"activation_ns=(\d+) entry_output_ns=(\d+) total_ns=(\d+)",
            executed_runtime.stdout,
        )
        require(timing_match is not None and
                all(int(value) >= 0 for value in timing_match.groups()),
                "artifact execution reports every exact stage timing",
                executed_runtime.stdout)

        corrupt_xsm = root / "corrupt-runtime-semantic.bin"
        corrupt_bytes = bytearray(runtime_xsm.read_bytes())
        corrupt_bytes[-1] ^= 1
        corrupt_xsm.write_bytes(corrupt_bytes)
        require_rejection(run([
            str(binary), "run", str(runtime_xtp),
            "--semantic-plan", str(corrupt_xsm),
        ]), "XR_ARTIFACT_2002",
            "corrupt XSM cannot authorize a matching XTP")

        source_with_authority = root / "source-with-authority.xr"
        source_with_authority.write_text("42\n", encoding="utf-8")
        require_rejection(run([
            str(binary), "run", str(source_with_authority),
            "--semantic-plan", str(runtime_xsm),
        ]), "XR_ARTIFACT_2006",
            "semantic authority cannot attach to a source or legacy route")
        require_rejection(run([
            str(binary), "run", str(source_with_authority), "--timings",
        ]), "XR_ARTIFACT_2004",
            "exact artifact timings cannot decorate a legacy source route")

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
        require_rejection(compiled, "XR_ARTIFACT_2000",
                          "legacy XRC compilation is removed")
        require(not xrc.exists(), "legacy XRC rejection leaves no artifact")
        for legacy_format in ("bytecode", "bc"):
            require_rejection(run([
                str(binary), "compile", str(source), "--format", legacy_format,
                "--output", str(root / "legacy.c"),
            ], cwd=root), "XR_ARTIFACT_2000",
                f"legacy {legacy_format} format is removed")
        for retired_alias in ("h", "header"):
            require_rejection(run([
                str(binary), "compile", str(source), "--format", retired_alias,
                "--output", str(root / "retired-header.h"),
            ], cwd=root), "XR_ARTIFACT_2000",
                f"retired {retired_alias} output alias is removed")
        require(not (root / "retired-header.h").exists(),
                "retired header alias leaves no artifact")
        require_rejection(run([
            str(binary), "compile", str(source), "--format", "c",
            "--output", str(root / "disguised.XRC"),
        ], cwd=root), "XR_ARTIFACT_2000",
            "legacy XRC extension cannot disguise a C container")
        c_container = root / "offline-container.c"
        c_compiled = run([
            str(binary), "compile", str(source), "--format", "c",
            "--output", str(c_container),
        ], cwd=root)
        require(c_compiled.returncode == 0 and c_container.is_file(),
                "the sole offline C container remains available to compiler development",
                c_compiled.stdout)
        xrc.write_bytes(b"XRAY\x1e\x00")
        renamed_xrc = root / "legacy.bin"
        shutil.copyfile(xrc, renamed_xrc)
        executed = run([str(binary), "run", str(renamed_xrc)], cwd=root)
        require_rejection(executed, "XR_ARTIFACT_2000",
                          "legacy XRC magic is rejected before execution")

    print("Target artifact CLI boundary tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

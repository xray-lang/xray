#!/usr/bin/env python3
"""Prove that a Windows GNU manifest cannot advertise an MSVC-spelled archive."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--zig", type=Path, required=True)
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("this ABI isolation gate requires Windows")

    root = args.root.resolve(strict=True)
    zig = args.zig.resolve(strict=True)
    source_manifest = root / "lib/xray/aot/x86_64-windows-gnu/manifest.json"
    source_archive = source_manifest.parent / "libxray_aot_core.a"
    manifest = json.loads(source_manifest.read_text(encoding="utf-8"))
    if manifest.get("target") != "x86_64-windows-gnu" or manifest.get("providers") != ["zig"]:
        parser.error("installed Windows GNU runtime contract is missing")

    with tempfile.TemporaryDirectory(prefix="xray-windows-abi-negative-") as temporary:
        fixture = Path(temporary)
        shutil.copytree(root / "bin", fixture / "bin")
        marker = fixture / "share/xray/install/install-marker.json"
        marker.parent.mkdir(parents=True)
        shutil.copyfile(root / "share/xray/install/install-marker.json", marker)
        (fixture / "include/xray").mkdir(parents=True)
        runtime_dir = fixture / "lib/xray/aot/x86_64-windows-gnu"
        runtime_dir.mkdir(parents=True)
        (fixture / "lib/xray/sdk/src/aot").mkdir(parents=True)

        mixed_archive = runtime_dir / "xray_aot_core.lib"
        shutil.copyfile(source_archive, mixed_archive)
        manifest["artifacts"][0]["path"] = mixed_archive.name
        manifest["artifacts"][0]["sha256"] = digest(mixed_archive)
        (runtime_dir / "manifest.json").write_text(
            json.dumps(manifest, indent=2, separators=(",", ":")) + "\n", encoding="utf-8"
        )

        process = subprocess.run(
            [
                str(fixture / "bin/xray.exe"),
                "toolchain",
                "doctor",
                "--target",
                "native",
                "--provider",
                "zig",
                "--zig",
                str(zig),
                "--refresh",
                "--json",
            ],
            text=True,
            encoding="utf-8",
            errors="strict",
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=30,
            check=False,
        )
        if process.returncode == 0:
            raise SystemExit("mixed Windows GNU/MSVC runtime unexpectedly passed doctor")
        try:
            report = json.loads(process.stdout)
        except json.JSONDecodeError as exc:
            raise SystemExit(f"doctor did not return JSON: {process.stdout}\n{process.stderr}") from exc
        diagnostics = report.get("diagnostics", [])
        if not any(
            item.get("code") == "RUNTIME_ARTIFACT_MISSING"
            and "invalid artifact" in item.get("message", "")
            for item in diagnostics
        ):
            raise SystemExit(f"mixed ABI did not fail at runtime artifact validation: {report}")

    print("Windows GNU/MSVC archive mixing fails closed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

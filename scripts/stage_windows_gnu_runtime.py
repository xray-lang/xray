#!/usr/bin/env python3
"""Stage Zig-built Windows GNU runtime archives into an installed Xray tree."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import tempfile
from pathlib import Path


ARCH_TO_RELEASE_TARGET = {
    "x86_64-windows-gnu": "windows-x86_64",
    "aarch64-windows-gnu": "windows-arm64",
}
ARTIFACTS = {
    "aot-core": "libxray_aot_core.a",
    "rt-coro": "libxray_rt_coro.a",
}
ALLOWED_DESTINATION_FILES = {"manifest.json", *ARTIFACTS.values()}


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def read_json(path: Path) -> dict[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot read JSON contract {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise ValueError(f"JSON contract must be an object: {path}")
    return value


def validate_source(path: Path, expected_name: str) -> Path:
    resolved = path.resolve(strict=True)
    if not resolved.is_file() or resolved.name != expected_name:
        raise ValueError(f"expected Zig GNU archive named {expected_name}: {path}")
    if resolved.stat().st_size <= 8 or resolved.read_bytes()[:8] != b"!<arch>\n":
        raise ValueError(f"not a static archive: {path}")
    return resolved


def atomic_copy(source: Path, destination: Path) -> None:
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{destination.name}.", dir=destination.parent)
    os.close(descriptor)
    temporary = Path(temporary_name)
    try:
        shutil.copyfile(source, temporary)
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)


def atomic_write_json(value: dict[str, object], destination: Path) -> None:
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{destination.name}.", dir=destination.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            json.dump(value, stream, indent=2, separators=(",", ":"))
            stream.write("\n")
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)


def stage(root: Path, target: str, aot_core: Path, rt_coro: Path) -> Path:
    if target not in ARCH_TO_RELEASE_TARGET:
        raise ValueError(f"unsupported Windows GNU runtime target: {target}")
    root = root.resolve(strict=True)
    if not root.is_dir():
        raise ValueError(f"installed root is not a directory: {root}")

    marker_path = root / "share" / "xray" / "install" / "install-marker.json"
    marker = read_json(marker_path)
    expected_release_target = ARCH_TO_RELEASE_TARGET[target]
    if (
        marker.get("schema") != 1
        or marker.get("product") != "xray-lang"
        or marker.get("layout") != "xray-payload-v1"
        or marker.get("target") != expected_release_target
    ):
        raise ValueError(
            f"installed root identity does not match {target}: "
            f"expected target {expected_release_target}"
        )

    msvc_target = target.removesuffix("-gnu") + "-msvc"
    msvc_manifest = root / "lib" / "xray" / "aot" / msvc_target / "manifest.json"
    msvc = read_json(msvc_manifest)
    if msvc.get("target") != msvc_target or msvc.get("providers") != ["msvc"]:
        raise ValueError(f"MSVC runtime contract is missing or invalid: {msvc_manifest}")

    sources = {
        "aot-core": validate_source(aot_core, ARTIFACTS["aot-core"]),
        "rt-coro": validate_source(rt_coro, ARTIFACTS["rt-coro"]),
    }
    destination = root / "lib" / "xray" / "aot" / target
    destination.mkdir(parents=True, exist_ok=True)
    unexpected = sorted(
        child.name for child in destination.iterdir() if child.name not in ALLOWED_DESTINATION_FILES
    )
    if unexpected:
        raise ValueError(f"unexpected files in Windows GNU runtime directory: {', '.join(unexpected)}")

    for artifact_id, source in sources.items():
        atomic_copy(source, destination / ARTIFACTS[artifact_id])

    manifest = {
        "schema": 1,
        "sdkAbi": 1,
        "target": target,
        "objectFormat": "coff",
        "providers": ["zig"],
        "artifacts": [
            {
                "id": f"xray-{artifact_id}-{target}-v1",
                "kind": "static-library",
                "path": file_name,
                "sha256": digest(destination / file_name),
            }
            for artifact_id, file_name in ARTIFACTS.items()
        ],
        # Zig's MinGW import set resolves WaitOnAddress/WakeByAddress through
        # its normal Windows libraries. `synchronization.lib` is an MSVC SDK
        # import library and must not leak into the GNU driver contract.
        "systemLibraries": ["ws2_32"],
    }
    manifest_path = destination / "manifest.json"
    atomic_write_json(manifest, manifest_path)
    return manifest_path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--target", choices=sorted(ARCH_TO_RELEASE_TARGET), required=True)
    parser.add_argument("--aot-core", type=Path, required=True)
    parser.add_argument("--rt-coro", type=Path, required=True)
    args = parser.parse_args()
    try:
        manifest = stage(args.root, args.target, args.aot_core, args.rt_coro)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    print(manifest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

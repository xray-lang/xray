#!/usr/bin/env python3
"""Fail-closed verifier for an installed Xray payload manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys


def fail(message: str) -> None:
    raise ValueError(message)


def load_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        fail(f"cannot read valid JSON from {path}: {exc}")
    if not isinstance(value, dict):
        fail(f"top-level JSON value in {path} must be an object")
    return value


def safe_relative_path(raw: object) -> PurePosixPath:
    if not isinstance(raw, str) or not raw or "\0" in raw or "\\" in raw:
        fail(f"invalid manifest path: {raw!r}")
    path = PurePosixPath(raw)
    if path.is_absolute() or ".." in path.parts or "." in path.parts:
        fail(f"non-canonical manifest path: {raw!r}")
    if re.match(r"^[A-Za-z]:", raw):
        fail(f"drive-prefixed manifest path: {raw!r}")
    return path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_identity(binary: Path, manifest: dict, root: Path) -> None:
    version = json.loads(
        subprocess.check_output([str(binary), "--version", "--json"], text=True,
                                encoding="utf-8", errors="strict")
    )
    for key in ("product", "version", "commit", "dirty", "target", "buildProfile"):
        if version.get(key) != manifest.get(key):
            fail(f"binary/manifest identity mismatch for {key}")

    installation = json.loads(
        subprocess.check_output(
            [str(binary), "info", "--installation", "--json"], text=True,
            encoding="utf-8", errors="strict"
        )
    ).get("installation", {})
    if installation.get("installed") is not True:
        fail("installed binary did not recognize its payload marker")
    if Path(installation.get("root", "")).resolve() != root.resolve():
        fail("installed binary reported the wrong payload root")


def verify(root: Path, binary: Path | None) -> None:
    manifest_path = root / "share/xray/install/payload-manifest.json"
    manifest = load_json(manifest_path)
    expected_scalars = {
        "schema": int,
        "product": str,
        "version": str,
        "commit": str,
        "dirty": bool,
        "target": str,
        "buildProfile": str,
        "bytecodeVersion": int,
        "moduleAbiVersion": int,
        "toolchainProtocol": int,
        "preferredZig": str,
        "requiredZig": str,
    }
    for key, expected_type in expected_scalars.items():
        if type(manifest.get(key)) is not expected_type:
            fail(f"manifest field {key!r} must be {expected_type.__name__}")
    if manifest["schema"] != 1 or manifest["product"] != "xray-lang":
        fail("unsupported payload manifest identity")
    if not re.fullmatch(r"[0-9a-f]{40}", manifest["commit"]):
        fail("payload commit must be a lowercase 40-hex Git identity")

    components = manifest.get("components")
    if not isinstance(components, list) or components != sorted(set(components)):
        fail("components must be a sorted unique array")
    if not components or any(
        item not in {"core", "sdk", "toolchain-integration", "zig"}
        for item in components
    ):
        fail("payload contains an unknown or empty component set")

    files = manifest.get("files")
    if not isinstance(files, list) or not files:
        fail("files must be a non-empty array")
    previous = ""
    casefolded: set[str] = set()
    resolved_root = root.resolve()
    for entry in files:
        if not isinstance(entry, dict):
            fail("file inventory entry must be an object")
        component = entry.get("component")
        if component not in {"core", "sdk", "toolchain-integration", "zig"}:
            fail(f"invalid component for payload entry: {entry.get('path')!r}")
        if component not in components:
            fail(f"payload entry references undeclared component: {entry.get('path')!r}")
        relative = safe_relative_path(entry.get("path"))
        relative_text = relative.as_posix()
        if relative_text <= previous:
            fail("file inventory must be strictly sorted and unique")
        previous = relative_text
        folded = relative_text.casefold()
        if folded in casefolded:
            fail(f"case-colliding payload path: {relative_text}")
        casefolded.add(folded)

        installed = root.joinpath(*relative.parts)
        try:
            common = os.path.commonpath(
                (str(resolved_root), str(installed.resolve(strict=False)))
            )
        except ValueError as exc:
            fail(f"payload path cannot be canonicalized: {relative_text}: {exc}")
        if common != str(resolved_root):
            fail(f"payload path escapes root: {relative_text}")

        if entry.get("kind") == "symlink":
            if set(entry) != {"path", "component", "kind", "target"}:
                fail(f"invalid symlink inventory fields: {relative_text}")
            if not installed.is_symlink() or os.readlink(installed) != entry.get("target"):
                fail(f"symlink mismatch: {relative_text}")
            safe_relative_path(entry.get("target"))
            continue
        if set(entry) != {"path", "component", "size", "sha256", "mode"}:
            fail(f"invalid regular-file inventory fields: {relative_text}")
        if not installed.is_file() or installed.is_symlink():
            fail(f"missing regular payload file: {relative_text}")
        if entry.get("size") != installed.stat().st_size:
            fail(f"size mismatch: {relative_text}")
        if not re.fullmatch(r"[0-9a-f]{64}", str(entry.get("sha256", ""))):
            fail(f"invalid SHA-256: {relative_text}")
        if entry["sha256"] != sha256(installed):
            fail(f"SHA-256 mismatch: {relative_text}")
        if entry.get("mode") not in {"0644", "0755"}:
            fail(f"unsupported payload mode: {relative_text}")

    if binary is not None:
        verify_identity(binary, manifest, root)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--binary", type=Path)
    args = parser.parse_args()
    try:
        verify(args.root, args.binary)
    except (ValueError, OSError, subprocess.CalledProcessError, json.JSONDecodeError) as exc:
        print(f"payload verification failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

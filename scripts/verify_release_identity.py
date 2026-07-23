#!/usr/bin/env python3
"""Verify tag, source, binary, and payload identities before publication."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys


def run(*args: str, cwd: Path | None = None) -> str:
    return subprocess.check_output(args, cwd=cwd, text=True).strip()


def cmake_version(source: Path) -> str:
    text = (source / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(
        r"project\s*\(\s*Xray\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)",
        text,
        re.IGNORECASE,
    )
    if not match:
        raise ValueError("cannot derive Xray VERSION from CMakeLists.txt")
    return match.group(1)


def load_json(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"{path} is not a JSON object")
    return value


def verify(source: Path, binary: Path, payload: Path, tag: str) -> None:
    version = cmake_version(source)
    if tag != f"v{version}":
        raise ValueError(f"release tag {tag!r} does not match CMake version {version!r}")

    head = run("git", "rev-parse", "HEAD", cwd=source)
    tag_commit = run("git", "rev-parse", f"{tag}^{{commit}}", cwd=source)
    if tag_commit != head:
        raise ValueError(f"tag {tag} targets {tag_commit}, but release checkout is {head}")
    if run("git", "status", "--porcelain", "--untracked-files=normal", cwd=source):
        raise ValueError("release checkout is dirty after build")

    binary_identity = json.loads(
        subprocess.check_output([str(binary), "--version", "--json"], text=True)
    )
    payload_identity = load_json(payload)
    expected = {
        "product": "xray-lang",
        "version": version,
        "commit": head,
        "dirty": False,
    }
    for key, value in expected.items():
        if binary_identity.get(key) != value:
            raise ValueError(f"binary field {key!r} is not {value!r}")
        if payload_identity.get(key) != value:
            raise ValueError(f"payload field {key!r} is not {value!r}")
    for key in ("target", "buildProfile"):
        if binary_identity.get(key) != payload_identity.get(key):
            raise ValueError(f"binary/payload mismatch for {key!r}")
    if binary_identity.get("buildProfile") != "Release":
        raise ValueError("publication requires a Release build identity")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--payload", type=Path, required=True)
    parser.add_argument("--tag", required=True)
    args = parser.parse_args()
    try:
        verify(args.source.resolve(), args.binary.resolve(), args.payload.resolve(), args.tag)
    except (ValueError, OSError, subprocess.CalledProcessError, json.JSONDecodeError) as exc:
        print(f"release identity verification failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Install a pinned Zig binary for Xray AOT cross-target smoke tests.

Idempotent: an already-extracted install is only re-linked, and a cached
archive is not re-downloaded. Extraction goes to a temporary directory and is
moved into place, so an interrupted run never leaves a half-unpacked toolchain
where the shim points.

Defaults:
    install dir:  ~/.local/opt/zig/zig-<arch>-<os>-<version>
    shim:         ~/.local/bin/zig
    download dir: ~/.cache/xray-tools/zig

Overrides:
    ZIG_VERSION=0.16.0
    XRAY_ZIG_PREFIX=$HOME/.local/opt/zig
    XRAY_ZIG_BIN_DIR=$HOME/.local/bin
    XRAY_TOOL_CACHE=$HOME/.cache/xray-tools/zig

Usage: install_zig_toolchain.py
"""

from __future__ import annotations

import os
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

DEFAULT_VERSION = "0.16.0"

OS_NAMES = {"Darwin": "macos", "Linux": "linux"}
ARCH_NAMES = {"arm64": "aarch64", "aarch64": "aarch64",
              "x86_64": "x86_64", "amd64": "x86_64"}


def home() -> Path:
    return Path.home()


def safe_extract(archive: Path, destination: Path) -> None:
    """Extract, refusing any member that would escape the destination.

    `tar -xJf` would happily follow `../` in a member name. Nothing about a
    Zig release should contain one, which is exactly why an unpack that finds
    one must stop rather than continue.
    """
    destination = destination.resolve()
    with tarfile.open(archive, "r:xz") as tar:
        for member in tar.getmembers():
            target = (destination / member.name).resolve()
            if destination != target and destination not in target.parents:
                raise RuntimeError(
                    f"refusing to extract outside the destination: {member.name}")
        tar.extractall(destination)


def download(url: str, target: Path) -> None:
    print(f"Downloading {url}")
    # Write to a sibling temp file and rename, so an interrupted download can
    # never be mistaken for a complete cached archive on the next run.
    partial = target.with_suffix(target.suffix + ".partial")
    with urllib.request.urlopen(url) as response, partial.open("wb") as handle:
        shutil.copyfileobj(response, handle)
    partial.replace(target)


def main(argv: list[str]) -> int:
    version = os.environ.get("ZIG_VERSION", DEFAULT_VERSION)
    prefix = Path(os.environ.get("XRAY_ZIG_PREFIX", str(home() / ".local/opt/zig")))
    bin_dir = Path(os.environ.get("XRAY_ZIG_BIN_DIR", str(home() / ".local/bin")))
    cache_root = os.environ.get("XDG_CACHE_HOME", str(home() / ".cache"))
    cache = Path(os.environ.get("XRAY_TOOL_CACHE",
                                str(Path(cache_root) / "xray-tools" / "zig")))

    host_os = platform.system()
    zig_os = OS_NAMES.get(host_os)
    if zig_os is None:
        sys.stderr.write(
            f"error: unsupported host OS for Zig installer: {host_os}\n")
        return 2

    host_arch = platform.machine()
    zig_arch = ARCH_NAMES.get(host_arch)
    if zig_arch is None:
        sys.stderr.write(
            f"error: unsupported host arch for Zig installer: {host_arch}\n")
        return 2

    pkg = f"zig-{zig_arch}-{zig_os}-{version}"
    archive_name = f"{pkg}.tar.xz"
    url = f"https://ziglang.org/download/{version}/{archive_name}"
    install_dir = prefix / pkg
    archive_path = cache / archive_name

    for directory in (prefix, bin_dir, cache):
        directory.mkdir(parents=True, exist_ok=True)

    zig = install_dir / "zig"
    if not (zig.is_file() and os.access(zig, os.X_OK)):
        if not archive_path.is_file():
            download(url, archive_path)

        with tempfile.TemporaryDirectory(prefix="xray_zig_install.") as tmp:
            safe_extract(archive_path, Path(tmp))
            staged = install_dir.with_name(install_dir.name + ".tmp")
            shutil.rmtree(staged, ignore_errors=True)
            shutil.move(str(Path(tmp) / pkg), str(staged))
            shutil.rmtree(install_dir, ignore_errors=True)
            staged.replace(install_dir)

    shim = bin_dir / "zig"
    if shim.is_symlink() or shim.exists():
        shim.unlink()
    shim.symlink_to(zig)

    # `zig version` prints one ASCII version line; decode it strictly so a
    # surprise in that output fails here rather than downstream.
    reported = subprocess.run([str(shim), "version"], stdout=subprocess.PIPE,
                              text=True, encoding="utf-8",
                              errors="strict").stdout.strip()
    print("Installed Zig:")
    print(f"  version: {reported}")
    print(f"  binary:  {shim}")
    print(f"  target:  {install_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

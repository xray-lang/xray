#!/usr/bin/env python3
"""Mutation tests for the exact installed AOT SDK header closure."""

from __future__ import annotations

import argparse
import json
import shutil
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import derive_aot_sdk_closure as closure  # noqa: E402


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def fixture(root: Path) -> None:
    for relative in closure.PRIVATE_ROOTS:
        write(root / relative, '#include "dep.h"\n')
        write((root / relative).parent / "dep.h", "#define PRIVATE_DEP 1\n")
    for relative in closure.PUBLIC_ROOTS:
        write(root / relative, '#include "xray_value_abi.h"\n')
    write(root / "include/xray_value_abi.h", "#define PUBLIC_DEP 1\n")


def expect_rejected(root: Path, label: str) -> None:
    try:
        closure.derive(root)
    except closure.ClosureError:
        return
    raise AssertionError(f"{label} mutation was accepted")


def self_test() -> int:
    with tempfile.TemporaryDirectory(prefix="xray-aot-sdk-closure-") as directory:
        root = Path(directory) / "repo"
        fixture(root)
        files = closure.derive(root)
        cmake = closure.render_cmake(root, files)
        manifest = json.loads(closure.render_manifest(root, files))
        if manifest["generator"] != closure.GENERATOR or not manifest["entries"]:
            raise AssertionError("clean closure manifest is incomplete")
        if "XRAY_AOT_SDK_PUBLIC_FILES" not in cmake or "XRAY_AOT_SDK_PRIVATE_FILES" not in cmake:
            raise AssertionError("clean closure CMake projection is incomplete")
        paths = [row["install_path"] for row in manifest["entries"]]
        if paths != sorted(paths) or len(paths) != len(set(paths)):
            raise AssertionError("closure install paths are not canonical")

        missing = Path(directory) / "missing"
        shutil.copytree(root, missing)
        (missing / closure.PRIVATE_ROOTS[0]).write_text('#include "missing.h"\n', encoding="utf-8")
        expect_rejected(missing, "missing include")

        legacy_path = Path(directory) / "legacy-path"
        shutil.copytree(root, legacy_path)
        write(legacy_path / closure.PRIVATE_ROOTS[0], '#include "xray_vm.h"\n')
        write((legacy_path / closure.PRIVATE_ROOTS[0]).parent / "xray_vm.h", "#define OLD 1\n")
        expect_rejected(legacy_path, "legacy path")

        legacy_text = Path(directory) / "legacy-text"
        shutil.copytree(root, legacy_text)
        write(legacy_text / closure.PRIVATE_ROOTS[0], "void xray_vm_new(void);\n")
        expect_rejected(legacy_text, "legacy text")

        unsupported = Path(directory) / "unsupported"
        shutil.copytree(root, unsupported)
        write(unsupported / closure.PRIVATE_ROOTS[0], '#include "implementation.c"\n')
        write((unsupported / closure.PRIVATE_ROOTS[0]).parent / "implementation.c", "int x;\n")
        expect_rejected(unsupported, "unsupported source kind")
    print("AOT SDK exact closure self-test: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    return self_test() if args.self_test else 2


if __name__ == "__main__":
    raise SystemExit(main())

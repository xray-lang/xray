#!/usr/bin/env python3
"""Shared factory and source inventory for stdlib governance manifests."""

from __future__ import annotations

import importlib.util
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import tomllib


MANIFEST_PATH = Path("stdlib/stdlib_boundary.toml")
VALID_LAYERS = {"L0", "L1", "L2", "L3", "L4", "L5"}
VALID_POLICIES = {"xray_semantic", "native_primitive", "native_library"}

@dataclass(frozen=True)
class BoundaryManifest:
    root: Path
    raw: dict[str, Any]
    modules: tuple[dict[str, Any], ...]
    vm_fastpaths: tuple[dict[str, Any], ...]

    @property
    def by_name(self) -> dict[str, dict[str, Any]]:
        return {str(module["name"]): module for module in self.modules}

    @property
    def def_migrated_modules(self) -> tuple[str, ...]:
        return tuple(
            sorted(str(module["name"]) for module in self.modules if module.get("def_migration_complete"))
        )

    @property
    def aot_helper_forbidden_modules(self) -> tuple[str, ...]:
        return tuple(
            sorted(str(module["name"]) for module in self.modules if module.get("aot_helper_forbidden"))
        )


def load_toml(path: Path) -> dict[str, Any]:
    """Load a repository TOML manifest.

    Binary mode is what tomllib requires: it decodes UTF-8 itself and rejects
    anything else, which is the behaviour a manifest parser should have.
    """
    with path.open("rb") as handle:
        return tomllib.load(handle)


def load_manifest(root: Path) -> BoundaryManifest:
    root = root.resolve()
    path = root / MANIFEST_PATH
    if not path.is_file():
        raise RuntimeError(f"missing stdlib boundary manifest: {path}")
    raw = load_toml(path)
    return BoundaryManifest(
        root=root,
        raw=raw,
        modules=tuple(raw.get("module", ())),
        vm_fastpaths=tuple(raw.get("vm_fastpath", ())),
    )


def native_entry_binder_modules(root: Path) -> dict[str, str]:
    """Modules whose native entries the generic loader installs via a binder.

    There is no registry of hand-written factories to read any more: a module
    has native entries exactly when stdlibgen generates a binder for it, which
    it decides from the .def rows and the manifest's loader declarations.
    """
    stdlibgen = load_stdlibgen(root)
    return {
        name: f"xr_stdlib_vm_bind_{name}_generated"
        for name in stdlibgen.derive_binder_modules(root)
    }


def source_modules(root: Path) -> set[str]:
    stdlib_root = root / "stdlib"
    return {
        path.name
        for path in stdlib_root.iterdir()
        if path.is_dir() and (path / f"{path.name}.xr").is_file()
    }


def loadable_modules(root: Path) -> set[str]:
    """Every module an import can resolve.

    A module is loadable when it has an Xray source, native entries, or both.
    `sourceless_loadable` covers the remaining case of a module with neither,
    which nothing else can imply and which therefore has to be declared.
    """
    manifest = load_manifest(root)
    declared = {
        str(module["name"]) for module in manifest.modules if module.get("sourceless_loadable")
    }
    return set(native_entry_binder_modules(root)) | source_modules(root) | declared


def load_stdlibgen(root: Path):
    module_path = root / "tools/stdlibgen/stdlibgen.py"
    spec = importlib.util.spec_from_file_location("xray_stdlibgen_for_boundary", module_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load stdlib metadata parser: {module_path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def def_public_symbols(root: Path) -> dict[str, set[str]]:
    stdlibgen = load_stdlibgen(root)
    parts = stdlibgen.parse_def_metadata(root)
    symbols: dict[str, set[str]] = {}
    for part in parts:
        for entry in part:
            if entry.is_internal:
                continue
            if hasattr(entry, "type_name"):
                name = f"{entry.type_name}.{entry.name}"
            elif type(entry).__name__ in {"StdlibClassMethodEntry", "StdlibClassFieldEntry"}:
                name = f"{entry.class_name}.{entry.name}"
            else:
                name = getattr(entry, "name", None) or getattr(entry, "class_name", None)
            if name:
                symbols.setdefault(entry.module, set()).add(str(name))
    return symbols


def api_inventory(root: Path) -> dict[str, Any]:
    path = root / "scripts/gen_api_inventory.py"
    spec = importlib.util.spec_from_file_location("xray_api_inventory_for_boundary", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load API inventory: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module.build_inventory(root, None, None)


def dynamic_public_items(root: Path) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    for item in api_inventory(root).get("items", []):
        signature = str(item.get("signature", ""))
        if item.get("category") != "stdlib-module":
            continue
        if not re.search(r"\b(?:Json|unknown)\b", signature):
            continue
        out.append(item)
    return out

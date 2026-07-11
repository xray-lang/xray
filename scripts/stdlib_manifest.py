#!/usr/bin/env python3
"""Shared loader and source inventory for stdlib governance manifests."""

from __future__ import annotations

import importlib.util
import ast
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

try:
    import tomllib
except ModuleNotFoundError:  # pragma: no cover - Python < 3.11 is unsupported by CI
    tomllib = None  # type: ignore[assignment]


MANIFEST_PATH = Path("stdlib/stdlib_boundary.toml")
VALID_LAYERS = {"L0", "L1", "L2", "L3", "L4", "L5"}
VALID_POLICIES = {"xray_semantic", "native_primitive", "native_library"}
REGISTRY_ENTRY_RE = re.compile(
    r'\{\s*"(?P<name>[A-Za-z_][A-Za-z0-9_]*)"\s*,\s*'
    r'xr_load_module_(?P<loader>[A-Za-z_][A-Za-z0-9_]*)\s*\}'
)


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


def _strip_toml_comment(line: str) -> str:
    in_string = False
    escaped = False
    out: list[str] = []
    for char in line:
        if escaped:
            out.append(char)
            escaped = False
            continue
        if char == "\\" and in_string:
            out.append(char)
            escaped = True
            continue
        if char == '"':
            in_string = not in_string
            out.append(char)
            continue
        if char == "#" and not in_string:
            break
        out.append(char)
    return "".join(out).strip()


def _parse_toml_value(raw: str, path: Path, line: int) -> Any:
    translated = re.sub(r"\btrue\b", "True", raw)
    translated = re.sub(r"\bfalse\b", "False", translated)
    try:
        return ast.literal_eval(translated)
    except (SyntaxError, ValueError) as exc:
        raise RuntimeError(f"{path}:{line}: unsupported manifest TOML value: {raw}") from exc


def _load_toml_subset(path: Path) -> dict[str, Any]:
    """Parse the dependency-free TOML subset used by stdlib_boundary.toml.

    Python 3.9 is still a supported build host, so the governance gate cannot
    require tomllib. The accepted subset is deliberately narrow: scalar
    key/value pairs, string/bool/list values, tables, and array-of-table blocks.
    """

    raw: dict[str, Any] = {}
    current: dict[str, Any] = raw
    lines = path.read_text(encoding="utf-8").splitlines()
    index = 0
    while index < len(lines):
        line_no = index + 1
        line = _strip_toml_comment(lines[index])
        index += 1
        if not line:
            continue
        array_table = re.fullmatch(r"\[\[([A-Za-z_][A-Za-z0-9_]*)\]\]", line)
        if array_table:
            name = array_table.group(1)
            entry: dict[str, Any] = {}
            raw.setdefault(name, []).append(entry)
            current = entry
            continue
        table = re.fullmatch(r"\[([A-Za-z_][A-Za-z0-9_]*)\]", line)
        if table:
            name = table.group(1)
            value = raw.setdefault(name, {})
            if not isinstance(value, dict):
                raise RuntimeError(f"{path}:{line_no}: table {name!r} conflicts with another value")
            current = value
            continue
        assignment = re.fullmatch(r"([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)", line)
        if not assignment:
            raise RuntimeError(f"{path}:{line_no}: unsupported manifest TOML syntax: {line}")
        key, value_text = assignment.groups()
        start_line = line_no
        while value_text.count("[") > value_text.count("]") and index < len(lines):
            continuation = _strip_toml_comment(lines[index])
            index += 1
            if continuation:
                value_text += " " + continuation
        if key in current:
            raise RuntimeError(f"{path}:{start_line}: duplicate key {key!r}")
        current[key] = _parse_toml_value(value_text, path, start_line)
    return raw


def load_manifest(root: Path) -> BoundaryManifest:
    root = root.resolve()
    path = root / MANIFEST_PATH
    if not path.is_file():
        raise RuntimeError(f"missing stdlib boundary manifest: {path}")
    if tomllib is None:
        raw = _load_toml_subset(path)
    else:
        with path.open("rb") as handle:
            raw = tomllib.load(handle)
    return BoundaryManifest(
        root=root,
        raw=raw,
        modules=tuple(raw.get("module", ())),
        vm_fastpaths=tuple(raw.get("vm_fastpath", ())),
    )


def registry_modules(root: Path) -> dict[str, str]:
    path = root / "src/module/xmodule.c"
    text = path.read_text(encoding="utf-8")
    return {match.group("name"): match.group("loader") for match in REGISTRY_ENTRY_RE.finditer(text)}


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
            if getattr(entry, "is_internal", False):
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

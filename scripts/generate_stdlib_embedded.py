#!/usr/bin/env python3
"""Generate embedded standard-library source and native-entry metadata.

Every script module has one canonical source at
``stdlib/<module>/<module>.xr`` and one bare module name. Explicit
dependency sources are keyed by their complete logical paths.
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import re
import sys
import tempfile
from pathlib import Path


def c_ident(name: str) -> str:
    out = []
    for ch in name:
        if ch.isalnum():
            out.append(ch)
        else:
            out.append("_")
    ident = "".join(out).strip("_")
    return ident or "module"


def c_string_bytes(data: bytes) -> str:
    lines: list[str] = []
    for i in range(0, len(data), 18):
        chunk = data[i : i + 18]
        escaped = "".join(f"\\{b:03o}" for b in chunk)
        lines.append(f'    "{escaped}"')
    lines.append('    ""')
    return "\n".join(lines)


def canonical_module_name(root: Path, path: Path) -> str | None:
    rel = path.relative_to(root)
    if len(rel.parts) == 3 and rel.parts[0] == "stdlib":
        module_dir = rel.parts[1]
        return module_dir if rel.name == f"{module_dir}.xr" else None
    return None


def collect_entries(root: Path, sources: list[Path]) -> list[tuple[str, Path, bytes]]:
    entries: list[tuple[str, Path, bytes]] = []
    seen: set[str] = set()
    for src in sources:
        path = src.resolve()
        name = canonical_module_name(root, path)
        if name is None:
            raise RuntimeError(f"non-canonical stdlib module source: {path}")
        if name in seen:
            raise RuntimeError(f"duplicate stdlib module source: {name}")
        seen.add(name)
        entries.append((name, path, path.read_bytes()))
    entries.sort(key=lambda item: item[0])
    return entries


def collect_submodules(root: Path, sources: list[Path], owners: set[str]) -> list[tuple[str, bytes]]:
    entries: dict[str, bytes] = {}
    for source in sources:
        path = source.resolve()
        relative = path.relative_to(root / "stdlib")
        parts = relative.with_suffix("").parts
        if (relative.suffix != ".xr" or len(parts) < 2 or parts[0] not in owners
                or (len(parts) == 2 and parts[0] == parts[1])
                or any(re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", part) is None for part in parts)):
            raise RuntimeError(f"invalid embedded stdlib submodule: {relative}")
        logical = relative.as_posix()
        if logical in entries:
            raise RuntimeError(f"duplicate embedded stdlib submodule: {logical}")
        data = path.read_bytes()
        if b"\0" in data:
            raise RuntimeError(f"NUL in embedded stdlib submodule: {logical}")
        entries[logical] = data
    return sorted(entries.items())


def derive_binder_modules(root: Path) -> set[str]:
    """Ask stdlibgen which modules it emits a native-entry binder for."""
    generator_path = root / "tools" / "stdlibgen" / "stdlibgen.py"
    sys.path.insert(0, str(generator_path.parent))
    spec = importlib.util.spec_from_file_location("xray_stdlibgen_for_embedding", generator_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load stdlib metadata generator: {generator_path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module.derive_binder_modules(root)


def source_descriptors(
    entries: list[tuple[str, Path, bytes]], native_only: list[str], binder_modules: set[str]
) -> list[tuple[str, str | None, bool]]:
    source_names = {name for name, _path, _data in entries}
    overlap = source_names & set(native_only)
    if overlap:
        raise RuntimeError(
            "modules declared native-only also have a canonical source: "
            + ", ".join(sorted(overlap))
        )

    if len(native_only) != len(set(native_only)):
        raise RuntimeError("duplicate native-only stdlib module")
    descriptors = sorted(
        [(name, f"xr_stdlib_src_{c_ident(name)}_{idx}") for idx, (name, _p, _d) in enumerate(entries)]
        + [(name, None) for name in native_only]
    )
    return [(name, source, name in binder_modules) for name, source in descriptors]


def emit_sources(root: Path, entries: list[tuple[str, Path, bytes]],
                 descriptors: list[tuple[str, str | None, bool]],
                 submodules: list[tuple[str, bytes]]) -> str:
    out = ["/* Auto-generated source metadata. Do not edit by hand. */", ""]
    for idx, (name, path, data) in enumerate(entries):
        ident = c_ident(name)
        rel = path.relative_to(root).as_posix()
        out.append(f"/* {rel} */")
        out.append(f"static const char xr_stdlib_src_{ident}_{idx}[] =")
        out.append(c_string_bytes(data))
        out.append(";")
        out.append("")

    logical_sources = [(f"{name}/{name}.xr", symbol) for name, symbol, _bound in descriptors if symbol]
    for index, (logical, data) in enumerate(submodules):
        symbol = f"xr_stdlib_submodule_{index}"
        out.extend([f"/* stdlib/{logical} */", f"static const char {symbol}[] =",
                    c_string_bytes(data), ";", ""])
        logical_sources.append((logical, symbol))

    out.append("static const XrStdlibLogicalSource xr_stdlib_logical_sources[] = {")
    for logical, symbol in sorted(logical_sources):
        out.append(f'    {{"{logical}", {symbol}}},')
    out.extend(["};", ""])
    out.append("static const XrStdlibSourceDescriptor xr_stdlib_source_descriptors[] = {")
    for name, source_symbol, bound in descriptors:
        out.append(f'    {{"{name}", {source_symbol or "NULL"}, {str(bound).lower()}}},')
    out.append("};")
    out.append("")
    out.append(
        "static const size_t xr_stdlib_source_descriptor_count = "
        "sizeof(xr_stdlib_source_descriptors) / sizeof(xr_stdlib_source_descriptors[0]);"
    )
    out.append("")
    return "\n".join(out)


def emit_runtime_bindings(descriptors: list[tuple[str, str | None, bool]]) -> str:
    out = ["/* Auto-generated runtime bindings. Do not edit by hand. */", ""]
    names = [name for name, _source, bound in descriptors if bound]
    for name in names:
        out.append(f"XR_FUNC bool xr_stdlib_vm_bind_{c_ident(name)}_generated("
                   "struct XrVMRuntime *isolate, struct XrModule *module);")
    out.append("static const XrEmbeddedStdlibNativeBinding xr_stdlib_native_bindings[] = {")
    if not names:
        out.append("    {NULL, NULL},")
    for name in names:
        out.append(f'    {{"{name}", xr_stdlib_vm_bind_{c_ident(name)}_generated}},')
    out.extend(["};", f"static const size_t xr_stdlib_native_binding_count = {len(names)};", ""])
    return "\n".join(out)


def write_if_changed(path: Path | None, content: str) -> bool:
    if path is None:
        return False
    if path.is_file() and path.read_text(encoding="utf-8") == content:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    return True


def self_test() -> int:
    with tempfile.TemporaryDirectory(prefix="xray-stdlib-source-embedding-") as directory:
        root = Path(directory).resolve()
        table = root / "embedded.inc"
        assert write_if_changed(table, "first\n")
        fixed_time = 1_700_000_000_000_000_000
        os.utime(table, ns=(fixed_time, fixed_time))
        assert not write_if_changed(table, "first\n")
        assert table.stat().st_mtime_ns == fixed_time
        assert write_if_changed(table, "second\n")
        assert table.read_text(encoding="utf-8") == "second\n"
        assert table.stat().st_mtime_ns != fixed_time
        module_dir = root / "stdlib" / "demo"
        module_dir.mkdir(parents=True)
        canonical_source = module_dir / "demo.xr"
        noncanonical_source = module_dir / "helper.xr"
        canonical_source.write_text("export fn ready() -> bool { return true }\n", encoding="utf-8")
        noncanonical_source.write_text("fn helper() {}\n", encoding="utf-8")
        selected = [("demo", canonical_source, canonical_source.read_bytes())]
        rows = source_descriptors(selected, ["native", "empty"], {"demo", "native", "not-selected"})
        assert rows == [("demo", "xr_stdlib_src_demo_0", True),
                        ("empty", None, False), ("native", None, True)]
        submodules = collect_submodules(root, [noncanonical_source], {"demo"})
        assert submodules == [("demo/helper.xr", noncanonical_source.read_bytes())]
        source_text = emit_sources(root, selected, rows, submodules)
        assert '{"demo/helper.xr", xr_stdlib_submodule_0}' in source_text
        assert '{"demo/demo.xr", xr_stdlib_src_demo_0}' in source_text
        for sources, owners in (([noncanonical_source, noncanonical_source], {"demo"}),
                                ([noncanonical_source], set()), ([canonical_source], {"demo"})):
            try:
                collect_submodules(root, sources, owners)
            except RuntimeError:
                pass
            else:
                raise AssertionError("invalid embedded submodule accepted")
        runtime_text = emit_runtime_bindings(rows)
        assert 'XrStdlibSourceDescriptor' in source_text
        assert 'xr_stdlib_vm_bind_' not in source_text
        assert '{"empty", NULL, false}' in source_text
        assert 'xr_stdlib_vm_bind_demo_generated' in runtime_text
        assert 'xr_stdlib_vm_bind_native_generated' in runtime_text
        assert 'not_selected' not in runtime_text and 'xr_stdlib_src_' not in runtime_text
        for invalid in (["demo"], ["native", "native"]):
            try:
                source_descriptors(selected, invalid, {"demo", "native"})
            except RuntimeError:
                pass
            else:
                raise AssertionError("ambiguous source/native descriptor accepted")
        for label, sources in (
            ("duplicate", [canonical_source, canonical_source]),
            ("non-canonical", [noncanonical_source]),
        ):
            try:
                collect_entries(root, sources)
            except RuntimeError:
                pass
            else:
                print(
                    f"stdlib source descriptor self-test: {label} input accepted",
                    file=sys.stderr,
                )
                return 1
    print("stdlib source embedding self-test: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--root", type=Path)
    parser.add_argument("--source-output", type=Path)
    parser.add_argument("--runtime-output", type=Path)
    parser.add_argument("--submodule-source", action="append", type=Path, default=[])
    # Modules this build can load that have no .xr source of their own. The
    # build owns this list because it owns which module TUs are compiled, and a
    # binder that was not compiled must not be named by the descriptor table.
    parser.add_argument("--native-only-module", action="append", default=[])
    parser.add_argument("sources", nargs="*", type=Path)
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if args.root is None:
        parser.error("--root is required")
    if not args.sources:
        parser.error("at least one stdlib source is required")

    root = args.root.resolve()
    entries = collect_entries(root, args.sources)

    try:
        descriptors = source_descriptors(entries, args.native_only_module, derive_binder_modules(root))
        submodules = collect_submodules(root, args.submodule_source, {name for name, _, _ in entries})
        source_text = emit_sources(root, entries, descriptors, submodules)
        runtime_text = emit_runtime_bindings(descriptors)
    except (RuntimeError, ValueError) as exc:
        print(f"generate_stdlib_embedded.py: {exc}", file=sys.stderr)
        return 1

    write_if_changed(args.source_output, source_text)
    write_if_changed(args.runtime_output, runtime_text)
    if not args.source_output and not args.runtime_output:
        print("generate_stdlib_embedded.py: no output path provided", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

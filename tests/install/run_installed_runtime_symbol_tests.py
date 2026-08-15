#!/usr/bin/env python3
"""Inspect the installed runtime TargetPlan load boundary fail closed."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))
    scripts = Path(__file__).resolve().parents[2] / "scripts"
    if str(scripts) not in sys.path:
        sys.path.insert(0, str(scripts))


bootstrap()
from xraytest import binary as binlib  # noqa: E402
import target_machine_retired_runtime_symbols as retired_runtime  # noqa: E402


REQUIRED = {
    "xr_artifact_probe",
    "xr_runtime_artifact_authority_load_available",
    "xr_runtime_artifact_authority_load_xsm",
    "xr_runtime_artifact_authority_free",
    "xr_runtime_artifact_authority_identity",
    "xr_runtime_artifact_authority_verify",
    "xr_runtime_target_authority_native_hosted",
    "xr_runtime_target_plan_load",
    "xr_runtime_generation_activation_available",
    "xr_runtime_generation_authority_create",
    "xr_runtime_generation_authority_destroy",
    "xr_module_generation_activate",
    "xr_module_generation_begin_drain",
    "xr_module_generation_execute_sole_scalar_i64",
    "xr_module_generation_load_verified_target_plan",
    "xr_module_generation_pin_acquire",
    "xr_module_generation_pin_release",
    "xr_module_generation_poison",
    "xr_module_generation_prepare",
    "xr_module_generation_retire",
    "xr_module_generation_rollback",
    "xr_module_generation_snapshot",
    "xr_module_generation_unload",
    "xr_module_generation_verify",
    "xr_runtime_create",
    "xr_runtime_destroy",
    "xr_module_load_target_plan",
    "xr_module_find_export",
    "xr_export_call",
    "xr_module_unload",
    "xr_target_plan_free",
    "xr_target_plan_verify",
    "xr_target_profile_verify",
    "xr_typed_dispatch_execute_i64",
    "xr_typed_dispatch_provider_contract_is_exact",
    "xr_typed_debug_emit",
    "xr_typed_debug_session_create",
    "xr_typed_debug_session_free",
    "xr_typed_debug_session_matches_plan",
    "xr_typed_decoded_cache_create",
    "xr_typed_decoded_cache_free",
    "xr_typed_decoded_cache_function",
    "xr_typed_decoded_cache_require_exact",
    "xr_typed_decoded_cache_size_within_budget",
    "xr_typed_decoded_cache_stats",
    "xr_typed_materialize_event",
    "xr_typed_profile_init",
    "xr_typed_profile_is_initialized",
    "xr_typed_profile_record_event",
    "xr_typed_profile_snapshot",
    "xr_typed_trace_buffer_init",
    "xr_typed_trace_buffer_sink",
    "xr_xsm_decode",
    "xr_xtp_decode_candidate",
    "xr_xtp_materialize_target_plan",
}
FORBIDDEN_EXACT_SYMBOLS = {
    "artifact-encoders": frozenset({"xr_xsm_encode", "xr_xtp_encode_plan"}),
}
FORBIDDEN_BUILDER_PREFIXES = {
    "semantic-plan-builders": ("xr_semantic_plan_build",),
    "target-plan-builders": ("xr_target_plan_build",),
    "ownership-builders": ("xr_ownership_certificate_build",),
}
FORBIDDEN_LAYER_PREFIXES = {
    "analyzer": ("xa_", "xanalyzer_"),
    "parser": ("xr_parse", "xlex_", "xast_", "xattribute_registry_"),
    "compiler": ("xr_compile", "xr_compiler_context_"),
    "xi": ("xi_",),
    "aot-cgen": ("xaot_", "xi_cgen", "xrt_"),
    "toolchain": ("xtc_", "xr_compiler_session_", "xr_toolchain_"),
}

APPROVED_RUNTIME_SOURCE_DIRECTORIES = (
    "src/base",
    "src/os",
    "src/runtime/abi",
)
APPROVED_RUNTIME_SOURCES = frozenset({
    "src/shared/xr_http_url.c",
    "src/shared/xr_unicode_grapheme.c",
    "src/shared/xr_unicode_grapheme_data.c",
    "src/plan/format/xr_artifact_kind.c",
    "src/plan/format/xr_xsm_decode.c",
    "src/plan/format/xr_xtp_artifact.c",
    "src/plan/format/xr_xtp_decode.c",
    "src/plan/format/xr_xtp_instruction_stream.c",
    "src/plan/format/xr_xtp_rows.c",
    "src/plan/semantic/xr_semantic_graph.c",
    "src/plan/semantic/xr_semantic_ids.c",
    "src/plan/semantic/xr_semantic_ops.c",
    "src/plan/semantic/xr_semantic_plan.c",
    "src/plan/semantic/xr_semantic_verify.c",
    "src/plan/ownership/xr_ownership_certificate.c",
    "src/plan/ownership/xr_ownership_check.c",
    "src/plan/ownership/xr_ownership_replay.c",
    "src/plan/target/xr_target_instruction_verify.c",
    "src/plan/target/xr_target_entry_abi.c",
    "src/plan/target/xr_target_plan.c",
    "src/plan/target/xr_target_profile.c",
    "src/plan/target/xr_target_verify.c",
    "src/plan/target/xr_xtp_materialize.c",
    "src/runtime/xr_module_generation.c",
    "src/runtime/xr_module_generation_verify.c",
    "src/runtime/xr_dynamic_entry_runtime.c",
    "src/runtime/xr_runtime_api.c",
    "src/runtime/xr_runtime_artifact_authority.c",
    "src/runtime/xr_runtime_artifact_verify.c",
    "src/runtime/xr_entry_cell.c",
    "src/runtime/xr_target_plan_load.c",
    "src/vm/xr_typed_dispatch.c",
    "src/vm/xr_typed_frame.c",
    "src/vm/xr_vm_decoded_cache.c",
    "src/vm/debug/xr_vm_materialize.c",
    "src/vm/debug/xr_vm_debug_control.c",
    "src/vm/debug/xr_vm_profile.c",
    "src/vm/debug/xr_vm_trace.c",
})
REQUIRED_RUNTIME_SOURCE_BASENAMES = frozenset({
    "xr_dynamic_entry_runtime.c",
    "xr_module_generation.c",
    "xr_runtime_api.c",
    "xr_runtime_artifact_authority.c",
    "xr_runtime_contract.c",
    "xr_runtime_target_authority.c",
    "xr_target_plan_load.c",
    "xr_target_entry_abi.c",
    "xr_target_verify.c",
    "xr_typed_dispatch.c",
    "xr_typed_frame.c",
    "xr_vm_decoded_cache.c",
    "xr_vm_materialize.c",
    "xr_vm_debug_control.c",
    "xr_vm_profile.c",
    "xr_vm_trace.c",
    "xr_xsm_decode.c",
    "xr_xtp_decode.c",
    "xr_xtp_instruction_stream.c",
    "xr_xtp_materialize.c",
})


def run(command: list[str], *, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        text=True,
        encoding="utf-8",
        errors="strict",
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=120,
        check=False,
    )


def install(build: Path, prefix: Path, component: str | None = None) -> subprocess.CompletedProcess[str]:
    command = ["cmake", "--install", str(build), "--prefix", str(prefix)]
    if component:
        command.extend(["--component", component])
    return run(command)


def legacy_symbols(symbols: set[str]) -> set[str]:
    return {symbol for symbol in symbols if retired_runtime.matches(symbol)}


def grouped_prefix_hits(
    symbols: set[str], groups: dict[str, tuple[str, ...]]
) -> dict[str, list[str]]:
    hits = {}
    for layer, prefixes in groups.items():
        matched = sorted(symbol for symbol in symbols if symbol.startswith(prefixes))
        if matched:
            hits[layer] = matched
    return hits


def grouped_exact_hits(
    symbols: set[str], groups: dict[str, frozenset[str]]
) -> dict[str, list[str]]:
    hits = {}
    for layer, forbidden in groups.items():
        matched = sorted(symbols & forbidden)
        if matched:
            hits[layer] = matched
    return hits


def approved_runtime_sources(root: Path) -> set[str]:
    approved = set(APPROVED_RUNTIME_SOURCES)
    for relative in APPROVED_RUNTIME_SOURCE_DIRECTORIES:
        directory = root / relative
        approved.update(
            path.relative_to(root).as_posix() for path in directory.rglob("*.c")
        )
    return approved


def repository_source_owners(root: Path) -> dict[str, set[str]]:
    owners: dict[str, set[str]] = {}
    for top_level in ("src", "stdlib", "tests"):
        directory = root / top_level
        for path in directory.rglob("*.c"):
            relative = path.relative_to(root).as_posix()
            owners.setdefault(path.name, set()).add(relative)
    return owners


def archive_members(archive: Path, cc: Path) -> list[str]:
    if os.name == "nt":
        archiver = cc.parent / "lib.exe"
        if not archiver.is_file():
            raise AssertionError(f"MSVC archive inspector is missing: {archiver}")
        result = run([str(archiver), "/nologo", "/list", str(archive)])
    else:
        archiver_name = shutil.which("llvm-ar") or shutil.which("ar")
        if not archiver_name:
            raise AssertionError("no supported archive member inspector is available")
        result = run([archiver_name, "t", str(archive)])
    if result.returncode != 0:
        raise AssertionError(f"archive member inspection failed:\n{result.stdout}")
    members = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    if not members:
        raise AssertionError("installed runtime archive has no inspectable members")
    return members


def member_source(member: str) -> str:
    normalized = member.replace("\\", "/")
    for suffix in (".obj", ".o"):
        if normalized.endswith(suffix):
            normalized = normalized[: -len(suffix)]
            break
    marker = normalized.rfind("/src/")
    if marker >= 0:
        return normalized[marker + 1 :]
    if normalized.startswith("src/"):
        return normalized
    return normalized.rsplit("/", 1)[-1]


def resolve_member_source(
    member: str, owners: dict[str, set[str]]
) -> tuple[str | None, list[str]]:
    source = member_source(member)
    if "/" in source:
        return source, [source]
    candidates = sorted(owners.get(source, set()))
    if len(candidates) != 1:
        return None, candidates
    return candidates[0], candidates


def verify_member_resolution_invariants() -> None:
    retired_multicore = "".join(("xray_", "vm_", "multicore_", "destroy"))
    retired_script_info = "".join(("xray_", "vm_", "set_", "script_", "info"))
    retired = {retired_multicore, retired_script_info}
    if legacy_symbols(retired) != retired:
        raise AssertionError("installed archive inventory accepts retired public VM APIs")
    owners = {
        "collision.c": {"src/base/collision.c", "src/frontend/collision.c"},
        "unique.c": {"src/base/unique.c"},
    }
    resolved, candidates = resolve_member_source("collision.c.o", owners)
    if resolved is not None or len(candidates) != 2:
        raise AssertionError("basename-only archive member collisions must fail closed")
    resolved, _ = resolve_member_source("unknown.c.o", owners)
    if resolved is not None:
        raise AssertionError("unknown basename-only archive members must fail closed")
    resolved, _ = resolve_member_source("unique.c.o", owners)
    if resolved != "src/base/unique.c":
        raise AssertionError("unique basename-only archive members must resolve exactly")
    resolved, _ = resolve_member_source(
        "objects/src/base/collision.c.obj", owners
    )
    if resolved != "src/base/collision.c":
        raise AssertionError("path-qualified archive members must preserve exact ownership")


def inspect(archive: Path, root: Path, cc: Path) -> tuple[list[str], list[str]]:
    names = binlib.defined_symbol_names(archive)
    if names is None:
        raise AssertionError("no supported symbol inspector is available")
    symbols = set(names)
    missing = sorted(REQUIRED - symbols)
    forbidden_encoders = grouped_exact_hits(symbols, FORBIDDEN_EXACT_SYMBOLS)
    forbidden_builders = grouped_prefix_hits(symbols, FORBIDDEN_BUILDER_PREFIXES)
    forbidden_layers = grouped_prefix_hits(symbols, FORBIDDEN_LAYER_PREFIXES)
    unexpected_legacy = sorted(legacy_symbols(symbols))
    members = archive_members(archive, cc)
    approved = approved_runtime_sources(root)
    owners = repository_source_owners(root)
    resolved_sources = []
    ambiguous_members = {}
    unexpected_members = []
    for member in members:
        source, candidates = resolve_member_source(member, owners)
        if source is None:
            ambiguous_members[member] = candidates
            continue
        resolved_sources.append(source)
        if source not in approved:
            unexpected_members.append(member)
    unexpected_members.sort()
    present_basenames = {Path(source).name for source in resolved_sources}
    missing_runtime_members = sorted(
        REQUIRED_RUNTIME_SOURCE_BASENAMES - present_basenames
    )
    if (
        missing
        or forbidden_encoders
        or forbidden_builders
        or forbidden_layers
        or unexpected_legacy
        or ambiguous_members
        or unexpected_members
        or missing_runtime_members
    ):
        raise AssertionError(
            json.dumps({
                "missingRequired": missing,
                "forbiddenEncoders": forbidden_encoders,
                "forbiddenBuilders": forbidden_builders,
                "forbiddenCompilerLayers": forbidden_layers,
                "uninventoriedLegacySymbols": unexpected_legacy,
                "ambiguousArchiveMembers": ambiguous_members,
                "unexpectedArchiveMembers": unexpected_members,
                "missingRuntimeBoundaryMembers": missing_runtime_members,
            }, indent=2, sort_keys=True)
        )
    return names, members


def compile_header(cc: Path, prefix: Path, work: Path) -> None:
    work.mkdir(parents=True, exist_ok=True)
    source = work / "target_plan_load_header_probe.c"
    source.write_text(
        "#include <xray_target_plan_load.h>\n"
        "#include <xray_runtime_generation.h>\n"
        "#include <xray_runtime_api.h>\n"
        "static XrRuntimeArtifactAuthority *authority;\n"
        "static XrRuntimeArtifactAuthorityIdentity identity;\n"
        "static XrTargetPlan *plan;\n"
        "static XrRuntimeGenerationAuthority *generation_authority;\n"
        "static XrLoadedModuleGeneration *generation;\n"
        "static int64_t scalar_result;\n"
        "static XrRuntime *runtime;\n"
        "static XrModule *module;\n"
        "static const XrExport *module_export;\n"
        "static XrExportValue call_result;\n"
        "int main(void) { return !xr_runtime_artifact_authority_load_available() || "
        "xr_runtime_artifact_authority_load_xsm(0, 0, &authority, 0, 0) || "
        "!xr_runtime_generation_activation_available() || "
        "authority != 0 || plan != 0 || generation_authority != 0 || generation != 0 || "
        "scalar_result != 0 || identity.schema_version != 0 || "
        "runtime != 0 || module != 0 || module_export != 0 || call_result.kind != 0 || "
        "xr_runtime_create(0, &runtime, 0, 0) || "
        "xr_module_load_target_plan(runtime, 0, 0, 0, 0, &module, 0, 0) || "
        "xr_module_find_export(module, \"absent\", &module_export, 0, 0) || "
        "xr_export_call(module_export, 0, 0, &call_result, 0, 0) || "
        "xr_module_unload(&module, 0, 0) || xr_runtime_destroy(&runtime, 0, 0) || "
        "xr_module_generation_execute_sole_scalar_i64(generation, &scalar_result, 0, 0); }\n",
        encoding="utf-8",
    )
    if os.name == "nt":
        obj = work / "target_plan_load_header_probe.obj"
        command = [
            str(cc), "/nologo", "/std:c11", "/W4", "/WX", "/c",
            f"/I{prefix / 'include/xray'}", f"/Fo{obj}", str(source),
        ]
    else:
        obj = work / "target_plan_load_header_probe.o"
        command = [
            str(cc), "-std=c11", "-Wall", "-Wextra", "-Werror", "-c",
            "-I", str(prefix / "include/xray"), "-o", str(obj), str(source),
        ]
    result = run(command, cwd=work)
    if result.returncode != 0 or not obj.is_file():
        raise AssertionError(f"installed header is not standalone:\n{result.stdout}")


def main() -> int:
    verify_member_resolution_invariants()
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--cc", type=Path, required=True)
    args = parser.parse_args()
    root = args.project_root.resolve(strict=True)
    build = args.build_dir.resolve(strict=True)
    binary = args.binary.resolve(strict=True)
    cc = args.cc.resolve(strict=True)
    dumpbin = cc.parent / "dumpbin.exe"
    if os.name == "nt":
        if not dumpbin.is_file():
            raise SystemExit(f"MSVC symbol inspector is missing: {dumpbin}")
        os.environ["DUMPBIN"] = str(dumpbin)
    target = run([str(binary), "toolchain", "list", "--target", "native", "--json"])
    host = json.loads(target.stdout)["normalizedTarget"]
    archive_name = "xray_vm.lib" if os.name == "nt" else "libxray_vm.a"

    with tempfile.TemporaryDirectory(prefix="xray-installed-runtime-symbols-") as temporary:
        work = Path(temporary)
        symbol_sets: list[list[str]] = []
        member_sets: list[list[str]] = []
        for component, directory in (("XrayCore", "core"), (None, "full")):
            prefix = work / directory
            installed = install(build, prefix, component)
            if installed.returncode != 0:
                raise AssertionError(f"install failed:\n{installed.stdout}")
            header = prefix / "include/xray/xray_target_plan_load.h"
            if not header.is_file():
                raise AssertionError(f"installed load header is missing: {header}")
            generation_header = prefix / "include/xray/xray_runtime_generation.h"
            if not generation_header.is_file():
                raise AssertionError(
                    f"installed generation header is missing: {generation_header}"
                )
            archive = prefix / f"lib/xray/vm/{host}/{archive_name}"
            if not archive.is_file():
                raise AssertionError(f"installed runtime archive is missing: {archive}")
            symbols, members = inspect(archive, root, cc)
            symbol_sets.append(symbols)
            member_sets.append(members)
            compile_header(cc, prefix, work / directory)
        if symbol_sets[0] != symbol_sets[1]:
            raise AssertionError("Core and full installs expose different runtime symbols")
        if member_sets[0] != member_sets[1]:
            raise AssertionError("Core and full installs contain different runtime members")

    print("installed runtime symbol inventory: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

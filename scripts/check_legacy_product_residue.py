#!/usr/bin/env python3
"""Inventory legacy VM/artifact residue across the five product surfaces."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import tempfile
from collections import defaultdict
from pathlib import Path


SCHEMA = 3
INVENTORY = Path("contracts/target-machine/legacy-product-residue.json")
MIGRATION_CLASSIFICATION = Path(
    "contracts/target-machine/migration-source-classification.json"
)
SELF_PATH = Path("scripts/check_legacy_product_residue.py")
TEXT_SUFFIXES = {".c", ".h", ".in", ".py", ".sh", ".ps1", ".cmake", ".toml"}

CANDIDATE_RE = re.compile(
    r"(?P<xrc>\.xrc\b)|"
    r"(?P<xrc_identity>\bXR_ARTIFACT_KIND_LEGACY_XRC\b)|"
    r"(?P<xrc_staging_identity>\bXRAY_STDLIB_XRC_DIR\b)|"
    r"(?P<bytecode_abi>\b(?:XrBcError|XR_BC_(?:MAGIC|VERSION|OK|ERR_[A-Z0-9_]+|"
    r"STRIP_[A-Z0-9_]+)|XR_LEGACY_XRC_VERSION)\b)|"
    r"(?P<public_vm_symbol>\bxray_vm_[A-Za-z0-9_]+\b)|"
    r"(?P<public_runtime_allocation>\bxray_(?:alloc|realloc)\b)|"
    r"(?P<public_runtime_lifecycle>\bxray_runtime_(?:init|cleanup|version)\b)|"
    r"(?P<internal_runtime_error_state>\bxr_runtime_(?:has_error|error_message|clear_error)\b)|"
    r"(?P<public_runtime_set_constructor>\bxray_set_new\b)|"
    r"(?P<public_runtime_map_constructor>\bxray_map_new\b)|"
    r"(?P<public_runtime_string_constructor>\bxray_string_new\b)|"
    r"(?P<internal_upvalue_lifecycle>\bxr_upvalue_(?:new|close)\b)|"
    r"(?P<retired_internal_multicore_destroy>\bxr_multicore_destroy\b)|"
    r"(?P<public_vm_type>\bXr(?:VMRuntime|VMConfig|VMBackendType|BytecodeModule|BytecodeBundle)\b)|"
    r"(?P<internal_vm_alias>\bxr_vm_[A-Za-z0-9_]+\b)|"
    r"(?P<module_loader>\bxr_load_module_[A-Za-z0-9_]+\b)|"
    r"(?P<bytecode_owner>\bxr_(?:bytecode[A-Za-z0-9_]*|compile_(?:stdlib_)?to_file|"
    r"eval_bytecode|run_bytecode_file|detect_output_format|output_c_source)\b)|"
    r"(?P<proto_loader_helper>\bxr_proto_[A-Za-z0-9_]+\b)|"
    r"(?P<bundle>\bxr_bundle_[A-Za-z0-9_]+\b)|"
    r"(?P<bytecode_macro>\bXR_(?:DECL|EVAL)_BYTECODE\b)|"
    r"(?P<output_format>\bXR_OUTPUT_(?:BYTECODE|C_SOURCE|C_HEADER)\b)|"
    r"(?P<runtime_archive>\bxray_(?:vm_runtime|stdlib_bcgen)\b|"
    r"\bgen-stdlib-embedded-bytecodes\b)|"
    r"(?P<embed_build_owner>\b(?:bytecode_embed_[A-Za-z0-9_]+|"
    r"XRAY_BYTECODE_[A-Za-z0-9_]+)\b)|"
    r"(?P<legacy_header>\bxray_vm\.h\b)|"
    r"(?P<payload>\bbytecodeVersion\b)|"
    r"(?P<embed_owner>\b(?:stdlib_embedded_bytecodes|generate_stdlib_embedded)\b)|"
    r"(?P<cli_alias>\"(?:bytecode|bc|source|header|c|h)\")",
)

# These exact namespaces belong to the typed TargetPlan runtime.  A governed
# manifest relationship must also match so an arbitrary legacy path cannot
# evade the residue ratchet by copying a prefix.
SURVIVOR_INTERNAL_VM_NAMESPACES = (
    "xr_vm_debug_control",
    "xr_vm_decoded_cache",
    "xr_vm_dynamic_entry",
    "xr_vm_entry_adapter",
    "xr_vm_materialize",
    "xr_vm_ops",
    "xr_vm_profile",
    "xr_vm_trace",
)


def is_exact_migration_path(root: Path, relative: object) -> bool:
    if not isinstance(relative, str) or not relative or "\\" in relative:
        return False
    path = Path(relative)
    return (
        not path.is_absolute()
        and ".." not in path.parts
        and not any(character in relative for character in ("*", "?", "[", "]"))
        and (root / path).is_file()
    )


def is_exact_internal_vm_namespace(namespace: object) -> bool:
    return (
        isinstance(namespace, str)
        and namespace in SURVIVOR_INTERNAL_VM_NAMESPACES
    )


def migration_survivor_paths(root: Path) -> dict[str, frozenset[str]]:
    try:
        data = json.loads(
            (root / MIGRATION_CLASSIFICATION).read_text(
                encoding="utf-8", errors="strict"
            )
        )
    except (OSError, json.JSONDecodeError):
        return {}
    items = data.get("items") if isinstance(data, dict) else None
    if not isinstance(items, list):
        return {}
    paths: dict[str, set[str]] = defaultdict(set)
    for item in items:
        if not isinstance(item, dict) or item.get("category") != "SURVIVOR_AUTHORITY":
            continue
        scan = item.get("legacy_residue_scan")
        if not isinstance(scan, dict) or set(scan) != {"namespaces", "consumers"}:
            continue
        namespaces = scan.get("namespaces")
        consumers = scan.get("consumers")
        owners = item.get("paths")
        if (
            not isinstance(namespaces, list)
            or not namespaces
            or not all(isinstance(value, str) for value in namespaces)
            or len(set(namespaces)) != len(namespaces)
            or not all(is_exact_internal_vm_namespace(value) for value in namespaces)
            or not isinstance(consumers, list)
            or not all(isinstance(value, str) for value in consumers)
            or len(set(consumers)) != len(consumers)
            or not isinstance(owners, list)
        ):
            continue
        relationships = owners + consumers
        if not all(is_exact_migration_path(root, value) for value in relationships):
            continue
        for relative in relationships:
            paths[relative].update(namespaces)
    return {relative: frozenset(namespaces)
            for relative, namespaces in paths.items()}


def is_survivor_internal_vm_token(
    relative: str, token: str, survivor_paths: dict[str, frozenset[str]]
) -> bool:
    return any(
        token == namespace or token.startswith(namespace + "_")
        for namespace in survivor_paths.get(relative, ())
    )


def inventory_policy() -> dict[str, object]:
    return {
        "candidate_spelling": "case-sensitive",
        "survivor_internal_vm_namespaces": list(SURVIVOR_INTERNAL_VM_NAMESPACES),
        "new_or_changed_residue": "error",
        "unclassified_residue": "error",
        "compatibility_loader_or_alias_after_cutover": "forbidden",
        "terminal_zero_residue": "valid",
        "removal": "replace-complete-owner-family-and-delete-old-owner-atomically",
    }


def digest_text(parts: list[str]) -> str:
    digest = hashlib.sha256()
    for part in sorted(parts):
        encoded = part.encode("utf-8")
        digest.update(len(encoded).to_bytes(8, "big"))
        digest.update(encoded)
    return digest.hexdigest()


def discovery_files(root: Path) -> list[Path]:
    files: set[Path] = set()
    if (root / "CMakeLists.txt").is_file():
        files.add(root / "CMakeLists.txt")
    for directory in ("cmake", "include", "src", "scripts", "tests"):
        base = root / directory
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if path.is_file() and path.suffix.lower() in TEXT_SUFFIXES:
                relative = path.relative_to(root)
                if relative not in {SELF_PATH, INVENTORY}:
                    files.add(path)
    return sorted(files)


def surface_for(relative: str, line: str) -> str:
    if relative.startswith("include/"):
        return "api"
    if relative.startswith(("src/app/cli/", "tests/cli/")):
        return "cli"
    if relative.startswith("tests/install/"):
        return "install"
    if relative == "CMakeLists.txt":
        return "install" if "install" in line.lower() else "build"
    if relative.startswith("cmake/"):
        return "install" if "install" in relative.lower() else "build"
    if relative.startswith("scripts/"):
        name = Path(relative).name.lower()
        return "install" if any(word in name for word in ("install", "package", "payload")) else "build"
    return "source"


def family_for(group: str, token: str) -> str:
    families = {
        "xrc": "xrc-artifact",
        "xrc_identity": "xrc-artifact-identity",
        "xrc_staging_identity": "xrc-artifact-identity",
        "bytecode_abi": "legacy-bytecode-abi",
        "public_vm_type": "legacy-vm-handle-or-bytecode-type",
        "public_runtime_allocation": "legacy-runtime-allocation-api",
        "public_runtime_lifecycle": "legacy-runtime-lifecycle-api",
        "internal_runtime_error_state": "legacy-runtime-error-state-api",
        "public_runtime_set_constructor": "legacy-runtime-set-constructor-api",
        "public_runtime_map_constructor": "legacy-runtime-map-constructor-api",
        "public_runtime_string_constructor": "legacy-runtime-string-constructor-api",
        "internal_upvalue_lifecycle": "legacy-upvalue-object-lifecycle-api",
        "retired_internal_multicore_destroy": "legacy-vm-internal-api-or-alias",
        "internal_vm_alias": "legacy-vm-internal-api-or-alias",
        "module_loader": "legacy-vm-module-loader",
        "bytecode_owner": "legacy-loader-writer-or-converter",
        "proto_loader_helper": "legacy-loader-proto-helper",
        "bundle": "legacy-bundle-converter",
        "bytecode_macro": "legacy-embed-macro",
        "output_format": "legacy-output-format",
        "runtime_archive": "legacy-build-or-install-target",
        "embed_build_owner": "legacy-embed-build-owner",
        "legacy_header": "legacy-installed-header",
        "payload": "legacy-install-metadata",
        "embed_owner": "legacy-stdlib-bootstrap-owner",
        "cli_alias": "legacy-cli-format-alias",
    }
    if group == "public_vm_symbol":
        lowered = token.lower()
        if lowered == "xray_vm_runtime":
            return "legacy-build-or-install-target"
        if lowered in {"xray_vm_new", "xray_vm_new_runtime", "xray_vm_new_full"}:
            return "legacy-constructor-surface"
        if lowered in {"xray_vm_dostring", "xray_vm_dofile", "xray_vm_dofile_debug"}:
            return "source-eval-runtime-api"
        return "legacy-vm-api"
    if group not in families:
        raise ValueError(f"unclassified residue token {token!r}")
    return families[group]


def is_cli_alias_owner(relative: str) -> bool:
    return relative in {
        "src/app/cli/xcmd_compile.c",
        "src/module/xproto_codec.c",
        "src/app/tools/xstdlib_bcgen.c",
    }


def collect(root: Path) -> dict[str, object]:
    groups: dict[tuple[str, str, str], dict[str, object]] = defaultdict(
        lambda: {"tokens": set(), "evidence": [], "hit_count": 0}
    )
    symbol_tokens: set[str] = set()
    survivor_paths = migration_survivor_paths(root)
    for path in discovery_files(root):
        relative = path.relative_to(root).as_posix()
        text = path.read_text(encoding="utf-8", errors="strict")
        for line in text.splitlines():
            for match in CANDIDATE_RE.finditer(line):
                if match.lastgroup == "cli_alias" and not is_cli_alias_owner(relative):
                    continue
                token = match.group(0)
                if (match.lastgroup == "internal_vm_alias"
                        and is_survivor_internal_vm_token(
                            relative, token, survivor_paths
                        )):
                    continue
                family = family_for(match.lastgroup or "", token)
                surface = surface_for(relative, line)
                item = groups[(surface, family, relative)]
                item["tokens"].add(token)
                item["evidence"].append(f"{token}\0{line.strip()}")
                item["hit_count"] += 1
                if re.fullmatch(
                    r"(?:(?:xray_vm|xr_vm|xr_bytecode|xr_bundle|xr_load_module|xr_proto)_"
                    r"[A-Za-z0-9_]+|xr_(?:eval_bytecode|run_bytecode_file|detect_output_format|"
                    r"output_c_source))",
                    token,
                ):
                    symbol_tokens.add(token)

    owners = []
    for (surface, family, path), item in sorted(groups.items()):
        tokens = sorted(item["tokens"])
        owners.append({
            "surface": surface,
            "family": family,
            "path": path,
            "status": "transitional-owner",
            "disposition": "remove-with-atomic-target-plan-product-cutover",
            "hit_count": item["hit_count"],
            "tokens": tokens,
            "evidence_sha256": digest_text(item["evidence"]),
        })
    surfaces = ("source", "build", "install", "api", "cli")
    counts = {
        surface: sum(owner["hit_count"] for owner in owners
                     if owner["surface"] == surface)
        for surface in surfaces
    }
    generated_from = sorted({owner["path"] for owner in owners})
    return {
        "schema": SCHEMA,
        "generator": SELF_PATH.as_posix(),
        "policy": inventory_policy(),
        "counts": counts,
        "total": sum(counts.values()),
        "owner_count": len(owners),
        "generated_from": generated_from,
        "source_tree_fingerprint": digest_text([
            f"{owner['surface']}\0{owner['family']}\0{owner['path']}\0"
            f"{owner['hit_count']}\0{owner['evidence_sha256']}"
            for owner in owners
        ]),
        "legacy_symbol_tokens": sorted(symbol_tokens),
        "owners": owners,
    }


def render(data: dict[str, object]) -> str:
    return json.dumps(data, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


def validate(data: dict[str, object]) -> list[str]:
    errors: list[str] = []
    surfaces = {"source", "build", "install", "api", "cli"}
    if data.get("schema") != SCHEMA:
        errors.append("inventory schema is not exact")
    if data.get("policy") != inventory_policy():
        errors.append("inventory classification policy is not exact")
    counts = data.get("counts")
    if not isinstance(counts, dict) or set(counts) != surfaces:
        errors.append("inventory does not cover the five required product surfaces")
    owners = data.get("owners")
    if not isinstance(owners, list):
        errors.append("inventory owners are not machine-readable rows")
    elif any(
        not isinstance(row, dict)
        or row.get("surface") not in surfaces
        or not row.get("family")
        or not row.get("path")
        or row.get("status") != "transitional-owner"
        or not isinstance(row.get("hit_count"), int)
        or row.get("hit_count", 0) <= 0
        or not row.get("evidence_sha256")
        for row in owners
    ):
        errors.append("inventory contains an incomplete owner row")
    if not isinstance(data.get("legacy_symbol_tokens"), list):
        errors.append("legacy symbol inventory is missing")
    return errors


def check(root: Path, current: dict[str, object] | None = None) -> tuple[bool, str]:
    if current is None:
        current = collect(root)
    errors = validate(current)
    path = root / INVENTORY
    if not path.is_file():
        errors.append(f"missing inventory: {INVENTORY.as_posix()}")
    else:
        try:
            committed = json.loads(path.read_text(encoding="utf-8", errors="strict"))
        except (OSError, json.JSONDecodeError) as exc:
            errors.append(f"cannot read inventory: {exc}")
        else:
            errors.extend(validate(committed))
            committed_rows = {
                (row["surface"], row["family"], row["path"]): row
                for row in committed.get("owners", [])
                if isinstance(row, dict)
            }
            for row in current.get("owners", []):
                key = (row["surface"], row["family"], row["path"])
                ceiling = committed_rows.get(key)
                if ceiling is None:
                    errors.append(f"new residue owner: {key}")
                    continue
                if not set(row["tokens"]).issubset(ceiling.get("tokens", [])):
                    errors.append(f"new residue token in owner: {key}")
                if row["hit_count"] > ceiling.get("hit_count", -1):
                    errors.append(f"residue count grew in owner: {key}")
                if (row["hit_count"] == ceiling.get("hit_count") and
                        row["tokens"] == ceiling.get("tokens") and
                        row["evidence_sha256"] != ceiling.get("evidence_sha256")):
                    errors.append(f"residue evidence changed without removal: {key}")
            current_symbols = set(current.get("legacy_symbol_tokens", []))
            committed_symbols = set(committed.get("legacy_symbol_tokens", []))
            if not current_symbols.issubset(committed_symbols):
                errors.append("legacy symbol inventory grew")
    if errors:
        return False, "\n".join(errors)
    return True, f"PASS ({current['total']} hits, {current['owner_count']} owners)"


def self_test() -> int:
    with tempfile.TemporaryDirectory(prefix="xray-legacy-product-residue-") as directory:
        root = Path(directory)
        for item in ("contracts/target-machine", "include", "src/app/cli",
                     "src/app/tools", "src/vm", "scripts", "tests"):
            (root / item).mkdir(parents=True, exist_ok=True)
        migration_classification = root / MIGRATION_CLASSIFICATION
        migration_classification_text = json.dumps(
            {
                "items": [
                    {
                        "id": "typed-target-vm",
                        "category": "SURVIVOR_AUTHORITY",
                        "paths": ["src/vm/xr_vm_decoded_cache.c"],
                        "legacy_residue_scan": {
                            "namespaces": ["xr_vm_decoded_cache"],
                            "consumers": ["src/typed_consumer.c"],
                        },
                    },
                    {
                        "category": "LEGACY_TOMBSTONE",
                        "paths": ["src/legacy_vm.c"],
                    },
                ]
            }
        )
        migration_classification.write_text(
            migration_classification_text,
            encoding="utf-8",
        )
        (root / "CMakeLists.txt").write_text("# clean\n", encoding="utf-8")
        owner = root / "src/app/cli/owner.c"
        owner.write_text('const char *suffix = ".xrc";\n', encoding="utf-8")
        compile_format_owner = root / "src/app/cli/xcmd_compile.c"
        compile_format_owner.write_text('const char *format = "c";\n', encoding="utf-8")
        bcgen_format_owner = root / "src/app/tools/xstdlib_bcgen.c"
        bcgen_format_owner.write_text('const char *format = "bytecode";\n', encoding="utf-8")
        staging_owner = root / "scripts/generate_stdlib_embedded.py"
        staging_owner.write_text("# canonical extensionless staging\n", encoding="utf-8")
        typed_vm_owner = root / "src/vm/xr_vm_decoded_cache.c"
        typed_vm_owner.write_text(
            "XrVmDecodedCache *cache;\n"
            "int status = XR_VM_DECODED_CACHE_OK;\n"
            "void *a = xr_vm_decoded_cache_create;\n",
            encoding="utf-8",
        )
        typed_vm_consumer = root / "src/typed_consumer.c"
        typed_vm_consumer.write_text(
            "void *cache = xr_vm_decoded_cache_create;\n",
            encoding="utf-8",
        )
        survivor_spelling_safe = collect(root)["total"] == 3
        inventory = root / INVENTORY
        inventory.write_text(render(collect(root)), encoding="utf-8")
        clean, _ = check(root, collect(root))
        migration_classification.unlink()
        classification_missing_rejected = not check(root, collect(root))[0]
        migration_classification.write_text("{", encoding="utf-8")
        classification_damaged_rejected = not check(root, collect(root))[0]
        migration_classification.write_text(
            migration_classification_text,
            encoding="utf-8",
        )
        survivor_prefix_spoof = root / "src/legacy_vm.c"
        survivor_prefix_spoof.write_text(
            "void *legacy = xr_vm_decoded_cache_legacy_call;\n",
            encoding="utf-8",
        )
        survivor_prefix_spoof_rejected = not check(root, collect(root))[0]
        survivor_prefix_spoof.unlink()
        typed_vm_owner.write_text(
            "XrVmDecodedCache *cache;\n"
            "int status = XR_VM_DECODED_CACHE_OK;\n"
            "void *legacy = xr_vm_call_closure;\n",
            encoding="utf-8",
        )
        legacy_internal_spelling_drifted, _ = check(root, collect(root))
        typed_vm_owner.write_text(
            "XrVmDecodedCache *cache;\n"
            "int status = XR_VM_DECODED_CACHE_OK;\n"
            "void *a = xr_vm_decoded_cache_create;\n",
            encoding="utf-8",
        )
        compile_format_owner.write_text(
            'const char *format = "c";\nconst char *alias = "source";\n',
            encoding="utf-8",
        )
        compile_format_alias_drifted, _ = check(root, collect(root))
        compile_format_owner.write_text('const char *format = "c";\n', encoding="utf-8")
        compile_format_owner.write_text(
            'const char *format = "c";\nconst char *retired = ".xrc";\n',
            encoding="utf-8",
        )
        compile_xrc_compatibility_drifted, _ = check(root, collect(root))
        compile_format_owner.write_text('const char *format = "c";\n', encoding="utf-8")
        bcgen_format_owner.write_text(
            'const char *format = "bytecode";\nconst char *alias = "bc";\n',
            encoding="utf-8",
        )
        bcgen_format_alias_drifted, _ = check(root, collect(root))
        bcgen_format_owner.write_text('const char *format = "bytecode";\n', encoding="utf-8")
        staging_owner.write_text('suffix = ".xrc"\n', encoding="utf-8")
        staging_suffix_drifted, _ = check(root, collect(root))
        staging_owner.write_text("# canonical extensionless staging\n", encoding="utf-8")
        (root / "CMakeLists.txt").write_text(
            "set(XRAY_STDLIB_XRC_DIR legacy)\n", encoding="utf-8"
        )
        staging_variable_drifted, _ = check(root, collect(root))
        (root / "CMakeLists.txt").write_text("# clean\n", encoding="utf-8")
        retired_backend = root / "include/xray_vm.h"
        retired_backend.write_text(
            "typedef enum { XR_VM_BACKEND_BYTECODE } XrVMBackendType;\n",
            encoding="utf-8",
        )
        backend_drifted, _ = check(root, collect(root))
        retired_backend.unlink()
        retired_debug_setters = root / "include/xray_vm.h"
        retired_debug_setters.write_text(
            "void xray_vm_set_trace(void *, int);\n"
            "void xray_vm_set_dump_bytecode(void *, int);\n",
            encoding="utf-8",
        )
        debug_setters_drifted, _ = check(root, collect(root))
        retired_debug_setters.unlink()
        retired_stats = root / "include/xray_vm.h"
        retired_stats.write_text(
            "void xray_vm_get_stats(void *, unsigned long *);\n",
            encoding="utf-8",
        )
        stats_drifted, _ = check(root, collect(root))
        retired_stats.unlink()
        retired_runtime_constructor = root / "include/xray_vm.h"
        retired_runtime_constructor.write_text(
            "void *xray_vm_new_runtime(const void *);\n",
            encoding="utf-8",
        )
        runtime_constructor_drifted, _ = check(root, collect(root))
        retired_runtime_constructor.unlink()
        retired_userdata_api = root / "include/xray_vm.h"
        retired_userdata_api.write_text(
            "void xray_vm_set_userdata(void *, void *);\n"
            "void *xray_vm_get_userdata(void *);\n",
            encoding="utf-8",
        )
        userdata_api_drifted, _ = check(root, collect(root))
        retired_userdata_api.unlink()
        retired_execution_policy_api = root / "include/xray_vm.h"
        retired_execution_policy_api.write_text(
            "void xray_vm_set_stdout(void *, void *);\n"
            "void xray_vm_set_deadline_ms(void *, long long);\n"
            "int xray_vm_timed_out(void *);\n"
            "void xray_vm_set_module_allowlist(void *, const char **, unsigned long);\n",
            encoding="utf-8",
        )
        execution_policy_api_drifted, _ = check(root, collect(root))
        retired_execution_policy_api.unlink()
        retired_coro_monitor_api = root / "include/xray_vm.h"
        retired_coro_monitor_api.write_text(
            "void xray_vm_coro_monitor_start(void *, int, int);\n",
            encoding="utf-8",
        )
        coro_monitor_api_drifted, _ = check(root, collect(root))
        retired_coro_monitor_api.unlink()
        retired_dofile_debug_api = root / "include/xray_vm.h"
        retired_dofile_debug_api.write_text(
            "int xray_vm_dofile_debug(void *, const char *, void **);\n",
            encoding="utf-8",
        )
        dofile_debug_api_drifted, _ = check(root, collect(root))
        retired_dofile_debug_api.unlink()
        retired_dofile_api = root / "include/xray_vm.h"
        retired_dofile_api.write_text(
            "int xray_vm_dofile(void *, const char *);\n",
            encoding="utf-8",
        )
        dofile_api_drifted, _ = check(root, collect(root))
        retired_dofile_api.unlink()
        retired_dostring_api = root / "include/xray_vm.h"
        retired_dostring_api.write_text(
            "int xray_vm_dostring(void *, const char *);\n",
            encoding="utf-8",
        )
        dostring_api_drifted, _ = check(root, collect(root))
        retired_dostring_api.unlink()
        retired_multicore_init_api = root / "include/xray_vm.h"
        retired_multicore_init_api.write_text(
            "void xray_vm_multicore_init(void *, int);\n",
            encoding="utf-8",
        )
        multicore_init_api_drifted, _ = check(root, collect(root))
        retired_multicore_init_api.unlink()
        retired_multicore_destroy_api = root / "include/xray_vm.h"
        retired_multicore_destroy_api.write_text(
            "void " + "".join(("xray_", "vm_", "multicore_", "destroy"))
            + "(void *);\n",
            encoding="utf-8",
        )
        multicore_destroy_api_drifted, _ = check(root, collect(root))
        retired_multicore_destroy_api.unlink()
        retired_script_info_setter = root / "include/xray_vm.h"
        retired_script_info_setter.write_text(
            "void " + "".join(("xray_", "vm_", "set_", "script_", "info"))
            + "(void *, const char *, int, char **);\n",
            encoding="utf-8",
        )
        script_info_setter_drifted, _ = check(root, collect(root))
        retired_script_info_setter.unlink()
        retired_minimal_constructor = root / "include/xray_vm.h"
        retired_minimal_constructor.write_text(
            "void *" + "".join(("xray_", "vm_", "new")) + "(const void *);\n",
            encoding="utf-8",
        )
        minimal_constructor_drifted, _ = check(root, collect(root))
        retired_minimal_constructor.unlink()
        retired_config_initializer = root / "include/xray_vm.h"
        retired_config_initializer.write_text(
            "void " + "".join(("xray_", "vm_", "config_", "init"))
            + "(void *);\n",
            encoding="utf-8",
        )
        config_initializer_drifted, _ = check(root, collect(root))
        retired_config_initializer.unlink()
        retired_tls_api = root / "include/xray_vm.h"
        retired_tls_api.write_text(
            "void xray_vm_enter(void *);\n"
            "void xray_vm_exit(void);\n"
            "void *xray_vm_current(void);\n",
            encoding="utf-8",
        )
        tls_api_drifted, _ = check(root, collect(root))
        retired_tls_api.unlink()
        retired_dead_vm_lifecycle = root / "include/xray_vm.h"
        retired_dead_vm_lifecycle.write_text(
            "void xr_vm_vm_init(void *);\n"
            "void xr_vm_vm_free(void *);\n",
            encoding="utf-8",
        )
        dead_vm_lifecycle_drifted, _ = check(root, collect(root))
        retired_dead_vm_lifecycle.unlink()
        retired_machine_ctx_setter = root / "include/xray_vm.h"
        retired_machine_ctx_setter.write_text(
            "void xr_vm_machine_ctx_set_isolate(void *, void *);\n",
            encoding="utf-8",
        )
        machine_ctx_setter_drifted, _ = check(root, collect(root))
        retired_machine_ctx_setter.unlink()
        retired_cfunction_free = root / "include/xray_vm.h"
        retired_cfunction_free.write_text(
            "void xr_vm_cfunction_free(void *);\n",
            encoding="utf-8",
        )
        cfunction_free_drifted, _ = check(root, collect(root))
        retired_cfunction_free.unlink()
        retired_interpret_stub = root / "include/xray_vm.h"
        retired_interpret_stub.write_text(
            "int xr_vm_interpret(const char *);\n",
            encoding="utf-8",
        )
        interpret_stub_drifted, _ = check(root, collect(root))
        retired_interpret_stub.unlink()
        retired_stacktrace_split = root / "include/xray_vm.h"
        retired_stacktrace_split.write_text(
            "void xr_vm_add_stacktrace(void *, void *);\n",
            encoding="utf-8",
        )
        stacktrace_split_drifted, _ = check(root, collect(root))
        retired_stacktrace_split.unlink()
        retired_proto_isolate_wrapper = root / "include/xray_vm.h"
        retired_proto_isolate_wrapper.write_text(
            "int xr_vm_interpret_proto_isolate(void *, void *);\n",
            encoding="utf-8",
        )
        proto_isolate_wrapper_drifted, _ = check(root, collect(root))
        retired_proto_isolate_wrapper.unlink()
        retired_runtime_allocation = root / "include/xray_runtime.h"
        retired_runtime_allocation.write_text(
            "void *xray_alloc(void *, size_t);\n"
            "void *xray_realloc(void *, void *, size_t, size_t);\n",
            encoding="utf-8",
        )
        runtime_allocation_drifted, _ = check(root, collect(root))
        retired_runtime_allocation.unlink()
        retired_runtime_lifecycle = root / "include/xray_runtime.h"
        retired_runtime_lifecycle.write_text(
            "void *xray_runtime_init(void);\n"
            "void xray_runtime_cleanup(void *);\n"
            "const char *xray_runtime_version(void);\n",
            encoding="utf-8",
        )
        runtime_lifecycle_drifted, _ = check(root, collect(root))
        retired_runtime_lifecycle.unlink()
        retired_runtime_error_state = root / "include/xray_runtime.h"
        retired_runtime_error_state.write_text(
            "bool xr_runtime_has_error(void *);\n"
            "const char *xr_runtime_error_message(void *);\n"
            "void xr_runtime_clear_error(void *);\n",
            encoding="utf-8",
        )
        runtime_error_state_drifted, _ = check(root, collect(root))
        retired_runtime_error_state.unlink()
        retired_runtime_set_constructor = root / "include/xray_runtime.h"
        retired_runtime_set_constructor.write_text(
            "void *xray_set_new(void *);\n",
            encoding="utf-8",
        )
        runtime_set_constructor_drifted, _ = check(root, collect(root))
        retired_runtime_set_constructor.unlink()
        retired_runtime_map_constructor = root / "include/xray_runtime.h"
        retired_runtime_map_constructor.write_text(
            "void *xray_map_new(void *);\n",
            encoding="utf-8",
        )
        runtime_map_constructor_drifted, _ = check(root, collect(root))
        retired_runtime_map_constructor.unlink()
        retired_runtime_string_constructor = root / "include/xray_runtime.h"
        retired_runtime_string_constructor.write_text(
            "void *xray_string_new(void *, const char *, size_t);\n",
            encoding="utf-8",
        )
        runtime_string_constructor_drifted, _ = check(root, collect(root))
        retired_runtime_string_constructor.unlink()
        retired_upvalue_lifecycle = root / "include/xray_runtime.h"
        retired_upvalue_lifecycle.write_text(
            "void *xr_upvalue_new(void *, void *);\n"
            "void xr_upvalue_close(void *, void *);\n",
            encoding="utf-8",
        )
        upvalue_lifecycle_drifted, _ = check(root, collect(root))
        retired_upvalue_lifecycle.unlink()
        retired_instance_duplicate = root / "include/xray_runtime.h"
        retired_instance_duplicate.write_text(
            "void *xr_instance_new(XrVMRuntime *, void *);\n",
            encoding="utf-8",
        )
        instance_duplicate_drifted, _ = check(root, collect(root))
        retired_instance_duplicate.unlink()
        retired_runtime_error_duplicate = root / "include/xray_runtime.h"
        retired_runtime_error_duplicate.write_text(
            "void xr_runtime_error(XrVMRuntime *, const char *, ...);\n",
            encoding="utf-8",
        )
        runtime_error_duplicate_drifted, _ = check(root, collect(root))
        retired_runtime_error_duplicate.unlink()
        retired_map_operation_split = root / "include/xray_runtime.h"
        retired_map_operation_split.write_text(
            "void xr_map_set(XrVMRuntime *, void *, void *, void *);\n"
            "void *xr_map_get(XrVMRuntime *, void *, void *);\n"
            "bool xr_map_delete(XrVMRuntime *, void *, void *);\n"
            "bool xr_map_contains(XrVMRuntime *, void *, void *);\n"
            "size_t xr_map_size(void *);\n",
            encoding="utf-8",
        )
        map_operation_split_drifted, _ = check(root, collect(root))
        retired_map_operation_split.unlink()
        retired_array_operation_split = root / "include/xray_runtime.h"
        retired_array_operation_split.write_text(
            "void xr_array_push(XrVMRuntime *, void *, void *);\n"
            "void *xr_array_pop(XrVMRuntime *, void *);\n"
            "void *xr_array_get(XrVMRuntime *, void *, int);\n"
            "void xr_array_set(XrVMRuntime *, void *, int, void *);\n"
            "size_t xr_array_length(void *);\n",
            encoding="utf-8",
        )
        array_operation_split_drifted, _ = check(root, collect(root))
        retired_array_operation_split.unlink()
        retired_string_operation_split = root / "include/xray_runtime.h"
        retired_string_operation_split.write_text(
            "void *xr_string_concat(XrVMRuntime *, void *, void *);\n"
            "int xr_string_compare(void *, void *);\n"
            "size_t xr_string_length(void *);\n"
            "const char *xr_string_cstr(void *);\n",
            encoding="utf-8",
        )
        string_operation_split_drifted, _ = check(root, collect(root))
        retired_string_operation_split.unlink()
        retired_value_print_split = root / "include/xray_runtime.h"
        retired_value_print_split.write_text(
            "void xr_value_print(XrVMRuntime *, void *);\n",
            encoding="utf-8",
        )
        value_print_split_drifted, _ = check(root, collect(root))
        retired_value_print_split.unlink()
        retired_public_array_constructors = root / "include/xray_runtime.h"
        retired_public_array_constructors.write_text(
            "void *xray_array_new(XrVMRuntime *);\n"
            "void *xray_array_new_with_capacity(XrVMRuntime *, size_t);\n",
            encoding="utf-8",
        )
        public_array_constructors_drifted, _ = check(root, collect(root))
        retired_public_array_constructors.unlink()
        retired_runtime_compatibility_header = root / "include/xruntime.h"
        retired_runtime_compatibility_header.write_text(
            '#include "xray_vm.h"\n'
            "void xr_legacy_runtime_surface(XrVMRuntime *);\n",
            encoding="utf-8",
        )
        runtime_compatibility_header_drifted, _ = check(root, collect(root))
        retired_runtime_compatibility_header.unlink()
        retired_dap_exception_wrapper = root / "include/xray_runtime.h"
        retired_dap_exception_wrapper.write_text(
            "int xr_debug_on_exception(XrVMRuntime *, const char *, bool);\n",
            encoding="utf-8",
        )
        dap_exception_wrapper_drifted, _ = check(root, collect(root))
        retired_dap_exception_wrapper.unlink()
        retired_instance_construct_wrapper = root / "include/xray_runtime.h"
        retired_instance_construct_wrapper.write_text(
            "void *xr_instance_construct(XrVMRuntime *, void *, void *, int);\n",
            encoding="utf-8",
        )
        instance_construct_wrapper_drifted, _ = check(root, collect(root))
        retired_instance_construct_wrapper.unlink()
        retired_regex_registration_wrapper = root / "include/xray_runtime.h"
        retired_regex_registration_wrapper.write_text(
            "void xr_regex_register_class(XrVMRuntime *);\n",
            encoding="utf-8",
        )
        regex_registration_wrapper_drifted, _ = check(root, collect(root))
        retired_regex_registration_wrapper.unlink()
        retired_socket_wait_wrapper = root / "include/xray_runtime.h"
        retired_socket_wait_wrapper.write_text(
            "void xr_socket_register_wait(XrVMRuntime *, int, int);\n",
            encoding="utf-8",
        )
        socket_wait_wrapper_drifted, _ = check(root, collect(root))
        retired_socket_wait_wrapper.unlink()
        retired_global_object_authority = root / "include/xray_runtime.h"
        retired_global_object_authority.write_text(
            '/* xray_vm.h owns the global object */\n',
            encoding="utf-8",
        )
        global_object_authority_drifted, _ = check(root, collect(root))
        retired_global_object_authority.unlink()
        retired_internal_multicore_destroy = root / "include/xray_runtime.h"
        retired_internal_multicore_destroy.write_text(
            "void xr_multicore_destroy(XrVMRuntime *);\n",
            encoding="utf-8",
        )
        internal_multicore_destroy_drifted, _ = check(root, collect(root))
        retired_internal_multicore_destroy.unlink()
        retired_proto_registration_helpers = root / "include/xray_runtime.h"
        retired_proto_registration_helpers.write_text(
            "const char *xr_proto_name(void *);\n"
            "void **xr_proto_children(void *, int *);\n"
            "void xr_proto_set_param_types(void *, const unsigned char *, int, unsigned char);\n",
            encoding="utf-8",
        )
        proto_registration_helpers_drifted, _ = check(root, collect(root))
        retired_proto_registration_helpers.unlink()
        retired_bundle_constructor_wrapper = root / "include/xray_runtime.h"
        retired_bundle_constructor_wrapper.write_text(
            "void *xr_bundle_create(XrVMRuntime *, const char *);\n",
            encoding="utf-8",
        )
        bundle_constructor_wrapper_drifted, _ = check(root, collect(root))
        retired_bundle_constructor_wrapper.unlink()
        (root / "src/new_loader.c").write_text(
            "void xr_bytecode_load(void);\n", encoding="utf-8"
        )
        drifted, _ = check(root, collect(root))
        (root / "src/new_codec.c").write_text(
            "XrBcError error = XR_BC_ERR_CORRUPT;\n", encoding="utf-8"
        )
        codec_abi_drifted, _ = check(root, collect(root))
        owner.unlink()
        compile_format_owner.unlink()
        bcgen_format_owner.unlink()
        staging_owner.unlink()
        typed_vm_owner.unlink()
        typed_vm_consumer.unlink()
        (root / "src/new_loader.c").unlink()
        (root / "src/new_codec.c").unlink()
        zero = collect(root)
        terminal, _ = check(root, zero)
    if (not survivor_spelling_safe or not clean
            or not classification_missing_rejected
            or not classification_damaged_rejected
            or not survivor_prefix_spoof_rejected
            or legacy_internal_spelling_drifted
            or compile_format_alias_drifted or compile_xrc_compatibility_drifted
            or bcgen_format_alias_drifted
            or staging_suffix_drifted or staging_variable_drifted
            or backend_drifted or debug_setters_drifted or stats_drifted
            or runtime_constructor_drifted or userdata_api_drifted
            or execution_policy_api_drifted
            or coro_monitor_api_drifted
            or dofile_debug_api_drifted
            or dofile_api_drifted
            or dostring_api_drifted
            or multicore_init_api_drifted
            or multicore_destroy_api_drifted
            or script_info_setter_drifted
            or minimal_constructor_drifted
            or config_initializer_drifted
            or tls_api_drifted
            or dead_vm_lifecycle_drifted
            or machine_ctx_setter_drifted
            or cfunction_free_drifted
            or interpret_stub_drifted
            or stacktrace_split_drifted
            or proto_isolate_wrapper_drifted
            or runtime_allocation_drifted
            or runtime_lifecycle_drifted
            or runtime_error_state_drifted
            or runtime_set_constructor_drifted
            or runtime_map_constructor_drifted
            or runtime_string_constructor_drifted
            or upvalue_lifecycle_drifted
            or instance_duplicate_drifted
            or runtime_error_duplicate_drifted
            or map_operation_split_drifted
            or array_operation_split_drifted
            or string_operation_split_drifted
            or value_print_split_drifted
            or public_array_constructors_drifted
            or runtime_compatibility_header_drifted
            or dap_exception_wrapper_drifted
            or instance_construct_wrapper_drifted
            or regex_registration_wrapper_drifted
            or socket_wait_wrapper_drifted
            or global_object_authority_drifted
            or internal_multicore_destroy_drifted
            or proto_registration_helpers_drifted
            or bundle_constructor_wrapper_drifted
            or drifted or codec_abi_drifted or not terminal
            or zero["total"] != 0 or validate(zero)):
        print("legacy product residue self-test: FAIL")
        return 1
    print("legacy product residue self-test: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    root = args.root.resolve(strict=True)
    current = collect(root)
    ok, message = check(root, current)
    if args.json:
        print(json.dumps({"ok": ok, "inventory": current,
                          "message": message}, sort_keys=True))
    else:
        print(f"legacy product residue inventory: {'PASS' if ok else 'FAIL'}")
        print(message)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())

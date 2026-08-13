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


SCHEMA = 2
INVENTORY = Path("contracts/target-machine/legacy-product-residue.json")
SELF_PATH = Path("scripts/check_legacy_product_residue.py")
TEXT_SUFFIXES = {".c", ".h", ".in", ".py", ".sh", ".ps1", ".cmake", ".toml"}

CANDIDATE_RE = re.compile(
    r"(?P<xrc>\.xrc\b)|"
    r"(?P<xrc_identity>\bXR_ARTIFACT_KIND_LEGACY_XRC\b)|"
    r"(?P<bytecode_abi>\b(?:XrBcError|XR_BC_(?:MAGIC|VERSION|OK|ERR_[A-Z0-9_]+|"
    r"STRIP_[A-Z0-9_]+)|XR_LEGACY_XRC_VERSION)\b)|"
    r"(?P<public_vm_symbol>\bxray_vm_[A-Za-z0-9_]+\b)|"
    r"(?P<public_runtime_allocation>\bxray_(?:alloc|realloc)\b)|"
    r"(?P<public_runtime_lifecycle>\bxray_runtime_(?:init|cleanup|version)\b)|"
    r"(?P<internal_runtime_error_state>\bxr_runtime_(?:has_error|error_message|clear_error)\b)|"
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
    re.IGNORECASE,
)


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
        "bytecode_abi": "legacy-bytecode-abi",
        "public_vm_type": "legacy-vm-handle-or-bytecode-type",
        "public_runtime_allocation": "legacy-runtime-allocation-api",
        "public_runtime_lifecycle": "legacy-runtime-lifecycle-api",
        "internal_runtime_error_state": "legacy-runtime-error-state-api",
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
    for path in discovery_files(root):
        relative = path.relative_to(root).as_posix()
        text = path.read_text(encoding="utf-8", errors="strict")
        for line in text.splitlines():
            for match in CANDIDATE_RE.finditer(line):
                if match.lastgroup == "cli_alias" and not is_cli_alias_owner(relative):
                    continue
                token = match.group(0)
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
        "policy": {
            "new_or_changed_residue": "error",
            "unclassified_residue": "error",
            "compatibility_loader_or_alias_after_cutover": "forbidden",
            "terminal_zero_residue": "valid",
            "removal": "replace-complete-owner-family-and-delete-old-owner-atomically",
        },
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
        for item in ("contracts/target-machine", "include", "src/app/cli", "scripts", "tests"):
            (root / item).mkdir(parents=True, exist_ok=True)
        (root / "CMakeLists.txt").write_text("# clean\n", encoding="utf-8")
        owner = root / "src/app/cli/owner.c"
        owner.write_text('const char *suffix = ".xrc";\n', encoding="utf-8")
        inventory = root / INVENTORY
        inventory.write_text(render(collect(root)), encoding="utf-8")
        clean, _ = check(root, collect(root))
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
        (root / "src/new_loader.c").write_text(
            "void xr_bytecode_load(void);\n", encoding="utf-8"
        )
        drifted, _ = check(root, collect(root))
        (root / "src/new_codec.c").write_text(
            "XrBcError error = XR_BC_ERR_CORRUPT;\n", encoding="utf-8"
        )
        codec_abi_drifted, _ = check(root, collect(root))
        owner.unlink()
        (root / "src/new_loader.c").unlink()
        (root / "src/new_codec.c").unlink()
        zero = collect(root)
        terminal, _ = check(root, zero)
    if (not clean or backend_drifted or debug_setters_drifted or stats_drifted
            or runtime_constructor_drifted or userdata_api_drifted
            or execution_policy_api_drifted
            or coro_monitor_api_drifted
            or dofile_debug_api_drifted
            or dofile_api_drifted
            or dostring_api_drifted
            or multicore_init_api_drifted
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

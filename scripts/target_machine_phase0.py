#!/usr/bin/env python3
"""Generate and verify the governed unified-target-machine discovery inventory.

The inventory is deliberately derived from the current implementation rather
than copied from a design document.  It gives later cutovers a fail-closed list
of Xi operations, AOT plan rows, legacy VM machinery, object extents, and the
qualification matrix that must be migrated or deleted.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
import tempfile

if sys.version_info < (3, 11):
    raise SystemExit("target_machine_phase0.py requires Python 3.11 or newer")

import tomllib
from pathlib import Path
from typing import Any, Iterable


SCHEMA = 1
GENERATOR_VERSION = "target-machine-phase0/1"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="strict")


def source_files(root: Path, directory: str) -> list[Path]:
    """Return a stable traversal order for generated inventory inputs."""
    return sorted((root / directory).rglob("*"), key=lambda path: path.as_posix())


def canonical_source_bytes(data: bytes) -> bytes:
    """Return the repository-canonical bytes for governed text inputs."""
    return data.replace(b"\r\n", b"\n")


def sha256_paths(root: Path, paths: Iterable[str]) -> str:
    digest = hashlib.sha256()
    for relative in sorted(set(paths)):
        data = canonical_source_bytes((root / relative).read_bytes())
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(len(data).to_bytes(8, "big"))
        digest.update(data)
    return digest.hexdigest()


def git(root: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", *args], cwd=root, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, check=False
    )
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or "git command failed")
    return result.stdout.strip()


def meta(root: Path, inputs: list[str]) -> dict[str, Any]:
    return {
        "schema": SCHEMA,
        "generator": GENERATOR_VERSION,
        "source_tree_fingerprint": sha256_paths(root, inputs),
        "generated_from": sorted(inputs),
    }


def sexpr_atom(block: str, key: str, default: str = "none") -> str:
    match = re.search(rf":{re.escape(key)}\s+([^\s()]+)", block)
    return match.group(1) if match else default


def sexpr_list(block: str, key: str) -> list[str]:
    match = re.search(rf":{re.escape(key)}\s+\(([^)]*)\)", block)
    return match.group(1).split() if match else []


def op_family(op_class: str) -> str:
    mapping = {
        "constant": "constant-and-target-query",
        "arithmetic": "arithmetic-and-conversion",
        "bitwise": "arithmetic-and-conversion",
        "comparison": "equality-and-order",
        "control": "control-flow",
        "memory-read": "memory-and-place",
        "memory-write": "memory-and-place",
        "memory": "memory-and-place",
        "call": "call-and-dispatch",
        "closure": "call-and-dispatch",
        "allocation": "allocation-and-ownership",
        "ownership": "allocation-and-ownership",
        "string": "string-and-codec",
        "aggregate": "aggregate-and-object",
        "object": "aggregate-and-object",
        "container": "container-and-iterator",
        "collection": "container-and-iterator",
        "exception": "error-and-cleanup",
        "coroutine": "coroutine-and-scheduler",
        "channel": "coroutine-and-scheduler",
        "vector": "vector-and-simd",
        "ffi": "ffi-and-provider",
        "runtime": "capability-and-provider",
        "meta": "metadata-and-debug",
    }
    return mapping.get(op_class, f"xi-class-{op_class}")


def oracle_for_family(family: str) -> str:
    if family in {"arithmetic-and-conversion", "equality-and-order", "vector-and-simd"}:
        return "independent-kat-property-oracle"
    if family == "coroutine-and-scheduler":
        return "deterministic-trace-and-lifecycle-oracle"
    if family == "allocation-and-ownership":
        return "ownership-certificate-plus-runtime-event-oracle"
    if family == "ffi-and-provider":
        return "external-c-abi-probe"
    return "typed-source-expected-result-plus-mutation-oracle"


def semantic_owner_inventory(root: Path) -> dict[str, Any]:
    inputs = [
        "xisa/xi/ops.def",
        "contracts/semantic-owners.toml",
        "src/ir/xi_emit_vm_gen.h",
        "src/aot/xi_to_c_dispatch_gen.h",
        "contracts/semantic-owner-registry.json",
    ]
    text = read(root / inputs[0])
    manifest = tomllib.loads(read(root / inputs[1]))
    registry = json.loads(read(root / inputs[4]))
    kernels_by_owner = {
        f"shared.{item['id']}": item["header"] for item in manifest.get("core", [])
    }
    kernels_by_owner.update({
        item["typed_plan"]: item["semantic_kernel"]
        for item in manifest.get("operation", [])
        if item.get("typed_plan") and item.get("semantic_kernel")
    })
    explicit_by_operation: dict[str, dict[str, Any]] = {}
    for owner in registry.get("owners", []):
        canonical_source = kernels_by_owner.get(owner.get("owner"))
        consumers = set(owner.get("consumers", []))
        # CGen is the production AOT consumer for compile-time-only semantics.
        # Runtime owners additionally name their hosted/freestanding adapters,
        # but requiring those profiles would hide source-backed target queries
        # that are fully reduced before generated C crosses that boundary.
        if not canonical_source or "vm" not in consumers or "cgen" not in consumers:
            continue
        for operation in owner.get("operations", []):
            explicit_by_operation[operation] = {
                "owner": owner["owner"],
                "source": canonical_source,
            }
    blocks = re.findall(r"\(define-xi-op\s+([^\s()]+)(.*?)(?=\n\(define-xi-op|\Z)", text, re.S)
    rows: list[dict[str, Any]] = []
    for name, body in blocks:
        op_class = sexpr_atom(body, "class", "unclassified")
        family = op_family(op_class)
        explicit = explicit_by_operation.get(name)
        rows.append({
            "operation_id": name,
            "family": family,
            "observable_contract": (explicit["source"] if explicit
                                    else "contracts/xi-canonical-ops.md"),
            "current_vm_owner": ("representation adapter" if explicit else
                                 "src/ir/xi_emit_vm_gen.h -> src/runtime/value/xinstruction_table.h -> src/vm"),
            "current_aot_owner": ("representation adapter" if explicit else
                                  "src/aot/xi_to_c_dispatch_gen.h -> src/aot/xi_cgen*.c"),
            "current_shared_owner": (explicit["source"] if explicit else "xisa/xi/ops.def"),
            "future_semantic_owner": (explicit["owner"] if explicit
                                      else "SemanticPlan.operation_registry"),
            "effects": sexpr_list(body, "effects"),
            "capabilities": sexpr_list(body, "requires"),
            "observable_edges": sexpr_list(body, "observable"),
            "ownership": sexpr_atom(body, "own-use", "explicit-none"),
            "targets": sexpr_list(body, "targets"),
            "error_panic_suspend_cancel_publication": [
                item for item in sexpr_list(body, "effects") + sexpr_list(body, "observable")
                if item in {"may-throw", "panic", "suspend", "cancel", "publication"}
            ],
            "independent_oracle": oracle_for_family(family),
            "benchmark_anchor": f"target-machine/{family}",
            "migration_task": 278 if explicit else 271,
        })

    shared = []
    for item in manifest.get("core", []):
        shared.append({
            "operation_id": f"shared.{item['id']}",
            "family": "shared-kernel",
            "observable_contract": item["header"],
            "current_vm_owner": "representation adapter",
            "current_aot_owner": "representation adapter",
            "current_shared_owner": item["header"],
            "future_semantic_owner": f"SemanticOwnerRegistry/{item['id']}",
            "effects": [],
            "capabilities": item["profiles"],
            "observable_edges": [],
            "ownership": item["representation"],
            "targets": item["profiles"],
            "error_panic_suspend_cancel_publication": [],
            "independent_oracle": "shared-kernel-unit-plus-differential-oracle",
            "benchmark_anchor": f"shared-core/{item['id']}",
            "migration_task": 278,
        })
    rows.extend(shared)
    rows.sort(key=lambda row: row["operation_id"])
    result = meta(root, inputs)
    result.update({
        "policy": {
            "future_owner_count": 1,
            "unknown_owner": "error",
            "missing_oracle": "error",
            "missing_migration_task": "error",
        },
        "operation_count": len(rows),
        "operations": rows,
    })
    return result


def struct_body(text: str, type_name: str) -> str:
    match = re.search(rf"typedef\s+struct\s+{re.escape(type_name)}\s*\{{(.*?)\}}\s*{re.escape(type_name)}\s*;", text, re.S)
    return match.group(1) if match else ""


def aot_destination(type_name: str) -> tuple[str, str, int]:
    name = type_name.removeprefix("Xaot").removesuffix("Plan").lower()
    if "json" in name:
        return "obsolete", "delete-without-replacement", 272
    if any(word in name for word in ("staticdata", "linkdependency", "metadatareachability")):
        return "emission-link", "XrEmissionLinkPlan", 272
    if any(word in name for word in ("alias", "bounds", "cache", "specialization", "codesize")):
        return "refinement", "XrRefinementPlan", 279
    if any(word in name for word in ("generic", "derive", "objectshape", "options", "hash", "encoding")):
        return "semantic", "XrSemanticPlan", 271
    return "target", "XrTargetPlan", 272


def reference_files(root: Path, needle: str, directories: tuple[str, ...]) -> list[str]:
    rows = []
    for directory in directories:
        for path in source_files(root, directory):
            if not path.is_file() or not (path.suffix in {".c", ".h"} or path.name.endswith(".inc.c")):
                continue
            if needle in read(path):
                rows.append(path.relative_to(root).as_posix())
    return sorted(rows)


def aot_plan_inventory(root: Path) -> dict[str, Any]:
    inputs = [
        "src/aot/xaot_bundle.h", "src/aot/xaot_bundle.c", "src/aot/xaot_prepare.c",
        "src/aot/xaot_verify.c", "src/aot/xaot_rep.h", "src/aot/xaot_abi.h",
    ]
    header = read(root / inputs[0])
    bundle_match = re.search(r"typedef struct XaotBundle\s*\{(.*?)\}\s*XaotBundle\s*;", header, re.S)
    if not bundle_match:
        raise RuntimeError("XaotBundle definition not found")
    bundle = bundle_match.group(1)
    types = sorted(set(re.findall(r"\b(Xaot[A-Za-z0-9_]+Plan)\s*\*\s*[A-Za-z0-9_]+\s*;", bundle)))
    rows = []
    for type_name in types:
        body = struct_body(header, type_name)
        category, destination, task = aot_destination(type_name)
        refs = reference_files(root, type_name, ("src/aot",))
        producer = next((path for path in refs if path.endswith("xaot_prepare.c")), None)
        if producer is None:
            producer = next((path for path in refs if path.endswith("xaot_bundle.c")), "none-current-gap")
        verifier = "src/aot/xaot_verify.c" if "src/aot/xaot_verify.c" in refs else "none-current-gap"
        consumers = [path for path in refs if "cgen" in path or path.endswith("xaot_link.c")]
        pointer_fields = re.findall(r"\b(?:const\s+)?(?:char|XrType|XiValue|XiFunc)\s*\*+\s*([A-Za-z0-9_]+)", body)
        c_fields = sorted(set(re.findall(r"\b(c_[A-Za-z0-9_]+|[A-Za-z0-9_]*c_type|[A-Za-z0-9_]*c_symbol)\b", body)))
        rows.append({
            "current_type": type_name,
            "category": category,
            "producer": producer,
            "verifier": verifier,
            "consumers": consumers or ["none-current-gap"],
            "current_fallback": "backend-reconstruction-risk" if verifier == "none-current-gap" else "none-declared",
            "pointer_lifetime_fields": sorted(set(pointer_fields)),
            "c_coupling_fields": c_fields,
            "destination": destination,
            "deletion_task": task,
            "reference_files": refs,
        })
    result = meta(root, inputs)
    result.update({
        "policy": {"unclassified_row": "error", "unresolved_destination": "error"},
        "row_count": len(rows),
        "rows": rows,
        "mixed_representation_types": {
            "XaotValueRep": {
                "machine_rep": "XrMachineRep",
                "memory_layout": "XrLayoutPlan",
                "c_spelling": "XrCEmissionPlan",
                "delete_fields": ["XrType pointer", "c_type"],
            },
            "XaotFuncAbi": {
                "logical_call": "XrCallPlan",
                "native_abi": "XrTargetAbiPlan",
                "c_spelling": "XrCEmissionPlan",
                "delete_fields": ["c_name", "c_symbol", "boxed_symbol"],
            },
        },
    })
    return result


def legacy_vm_inventory(root: Path) -> dict[str, Any]:
    inputs = [
        "src/runtime/value/xinstruction_table.h", "src/runtime/value/xvalue.h",
        "src/runtime/value/xchunk.h", "src/module/xproto_codec.h",
        "include/xray_vm.h", "src/vm/xvm.c", "src/vm/xvm_coro_backend.c",
    ]
    op_text = read(root / inputs[0])
    pattern = re.compile(r"_\(\s*([A-Z][A-Z0-9_]*)\s*,\s*(FMT_[A-Za-z0-9_]+)\s*,\s*(KOP_[A-Za-z0-9_]+)\s*,")
    opcodes = []
    for name, fmt, operands in pattern.findall(op_text):
        family = "typed-operation"
        disposition = "map-to-target-plan-op"
        if name in {"PLACE_LOAD", "PLACE_STORE"}:
            family, disposition = "vm-only-place", "delete-use-explicit-address-slot-plan"
        elif name.startswith(("CALL", "RETURN", "TAILCALL", "INVOKE")):
            family, disposition = "call-frame", "map-to-XrCallPlan"
        elif name.startswith(("CORO", "YIELD", "AWAIT", "CHAN", "SELECT", "GO", "SCOPE_EXIT")):
            family, disposition = "retained-coroutine-stack", "map-to-XrCoroutineMachinePlan"
        elif name.startswith(("GETMODULE", "SETMODULE", "GETGLOBAL", "SETGLOBAL")):
            family, disposition = "name-or-index-dispatch", "map-to-stable-declaration-id"
        opcodes.append({
            "opcode": f"OP_{name}", "operand_format": fmt, "operand_roles": operands,
            "tag_assumption": "XrValue-register-frame", "family": family,
            "observable_behavior": "description-in-xinstruction-table",
            "ownership": "implicit-opcode-handler-contract",
            "error_suspend_edges": "handler-specific",
            "target_plan_mapping": disposition,
            "deletion_task": 276 if disposition.startswith("map") else 277,
        })

    tagged_sites = []
    frame_pattern = re.compile(r"\bXrValue\s*\*\s*([A-Za-z_][A-Za-z0-9_]*)")
    for directory in ("src/vm", "src/api", "src/module", "src/runtime"):
        for path in source_files(root, directory):
            if not path.is_file() or path.suffix not in {".c", ".h"}:
                continue
            names = sorted(set(frame_pattern.findall(read(path))))
            if names:
                tagged_sites.append({"path": path.relative_to(root).as_posix(), "pointer_names": names})

    api_text = read(root / "include/xray_vm.h")
    api_symbols = sorted(set(re.findall(r"\b(xray_vm_[A-Za-z0-9_]+)\s*\(", api_text)))
    bc_text = read(root / "src/module/xproto_codec.h")
    artifact_symbols = sorted(set(re.findall(r"\b(xr_[A-Za-z0-9_]*(?:bytecode|output)[A-Za-z0-9_]*)\s*\(", bc_text)))
    result = meta(root, inputs)
    result.update({
        "policy": {"legacy_translation": "forbidden", "default_tagged_frame": "delete"},
        "opcode_count": len(opcodes),
        "opcodes": opcodes,
        "tagged_frame_sites": tagged_sites,
        "vm_public_api_symbols": api_symbols,
        "legacy_artifact_symbols": artifact_symbols,
        "artifact": {
            "extension": ".xrc",
            "writer": "src/module/xproto_codec.c",
            "reader": "src/module/xproto_codec.c",
            "constant_unknown_behavior": "must-audit-and-delete-default-to-null",
            "replacement": ".xtp exact-version verified target plan",
            "deletion_task": 277,
        },
    })
    return result


OBJECT_ROWS = [
    ("string", "src/runtime/object/xstring.h", "src/aot/xrt_arc.h", "fixed-prefix + byte-length + terminator", "inline-tail-or-static-view"),
    ("closure", "src/runtime/closure/xclosure.h", "src/aot/xrt_coll.h", "fixed-prefix + capture-count * capture-stride", "inline-tail"),
    ("tuple", "src/runtime/object/xtuple.h", "src/aot/xrt_coll.h", "fixed-prefix + arity * element-stride", "inline-tail"),
    ("array", "src/runtime/object/xarray.h", "src/aot/xrt_coll.h", "object-prefix plus capacity * element-stride", "external-or-inline-buffer"),
    ("map", "src/runtime/object/xmap.h", "src/aot/xrt_coll.h", "object-prefix plus index-capacity and entry-capacity buffers", "multi-buffer"),
    ("set", "src/runtime/object/xset.h", "src/aot/xrt_coll.h", "object-prefix plus index-capacity and entry-capacity buffers", "multi-buffer"),
    ("class", "src/runtime/class/xclass.h", "src/aot/xrt_coll.h", "descriptor-fixed plus method/field tables", "multi-buffer"),
    ("instance", "src/runtime/class/xinstance.h", "src/aot/xrt_coll.h", "layout fixed-prefix plus planned field extent", "inline-tail"),
    ("enum", "src/runtime/class/xenum.h", "src/aot/xrt_coll.h", "box-prefix + payload-count * payload-stride", "inline-tail"),
    ("cell", "src/runtime/closure/xcell.h", "src/aot/xrt_coll.h", "descriptor fixed size", "fixed"),
    ("view", "src/runtime/value/xvalue.h", "src/aot/xrt_value.h", "two-word non-owning data/length pair", "fixed"),
    ("coroutine-frame", "src/vm/xvm_coro_backend.c", "src/aot/xi_cgen_coro.inc.c", "backend-private frame formulas", "replace-with-plan"),
    ("native-wrapper", "src/runtime/object/xnative_type.h", "src/aot/xrt_provider_abi.h", "provider contract extent", "provider-defined"),
    ("bigint", "src/runtime/object/xbigint.h", "src/aot/xrt_arith.h", "prefix + limb-capacity * limb-stride", "inline-or-external-buffer"),
]


def object_extent_inventory(root: Path) -> dict[str, Any]:
    inputs = sorted(set(path for row in OBJECT_ROWS for path in row[1:] if path.startswith("src/")))
    missing = [path for path in inputs if not (root / path).is_file()]
    if missing:
        raise RuntimeError(f"object inventory source missing: {missing}")
    rows = []
    for family, vm, aot, formula, backing in OBJECT_ROWS:
        rows.append({
            "family": family,
            "vm_layout_owner": vm,
            "aot_layout_owner": aot,
            "current_allocation_formula": formula,
            "external_or_multiple_buffers": backing,
            "clone_account_sized_free_destructor_root": "current-backend-specific-audit-required",
            "storage_domain_publication": "current-runtime-domain-contract",
            "boundary_bridge_chain": "XrValue or backend-native adapter",
            "future_layout_owner": "XrLayoutPlan",
            "future_extent_owner": "XrExtentPlan",
            "future_allocation_owner": "XrAllocationPlan",
            "migration_task": 272,
        })
    result = meta(root, inputs)
    result.update({
        "policy": {
            "unique_extent_owner": "XrExtentPlan",
            "universal_object_size_field": "forbidden",
            "dynamic_shape_or_json_plan_family": "forbidden",
            "private_size_formula_after_cutover": "error",
        },
        "family_count": len(rows),
        "families": rows,
    })
    return result


def validation_matrix(root: Path) -> dict[str, Any]:
    inputs = ["CMakePresets.json", ".github/workflows/ci.yml", "tests/diff/run_backend_diff.py"]
    rows = [
        ("macos-arm64", "host-clang", "source", "vm", "Release", "supported", "ctest --test-dir build -R backend_diff --output-on-failure"),
        ("macos-arm64", "host-clang", "native", "aot", "Release", "supported", "ctest --test-dir build -R 'aot_(filetests|standalone_suite|manifest_sweep)' --output-on-failure"),
        ("macos-arm64", "host-clang", "source+native", "vm+aot", "ASan-UBSan", "supported", "ctest --test-dir build -R asan_focused --output-on-failure"),
        ("macos-arm64", "host-clang", "source+native", "vm+aot", "TSan", "supported", "ctest --test-dir build -R tsan_focused --output-on-failure"),
        ("linux-x86_64", "gcc", "source+native", "vm+aot", "Release+sanitizer", "ci-only", "CI: linux gcc matrix"),
        ("linux-x86_64", "clang", "source+native", "vm+aot", "Release+sanitizer", "ci-only", "CI: linux clang matrix"),
        ("windows-x86_64", "msvc", "source+native", "vm+aot", "Release", "ci-only", "CI: windows msvc ninja matrix"),
        ("windows-x86_64", "clang", "native", "aot", "Release", "unqualified", "portable generated-C smoke"),
        ("freestanding-aarch64", "zig", "native", "aot", "Release", "unqualified", "ctest --test-dir build -R freestanding --output-on-failure"),
        ("any-32-bit", "any", "any", "any", "any", "unsupported", "compile-time pointer-width rejection"),
    ]
    matrix = []
    for index, (target, provider, artifact, executor, build, tier, command) in enumerate(rows, 1):
        matrix.append({
            "id": f"TM-MATRIX-{index:03d}", "target": target, "provider": provider,
            "artifact_route": artifact, "executor_or_generation": executor,
            "build_or_sanitizer": build, "support_tier": tier,
            "owner": "target-machine-matrix", "command": command,
            "oracle": "independent expected output plus plan/fingerprint validation",
            "baseline": "contracts/target-machine/baseline-manifest.json",
            "timeout_seconds": 1800, "artifact_retention": "on-failure",
            "negative_pair": "target/provider/schema/capability/fingerprint mismatch",
        })
    result = meta(root, inputs)
    result.update({
        "status_values": ["passed", "unsupported", "skipped-config", "blocked", "failed"],
        "failed_may_be_relabelled_skip": False,
        "rows": matrix,
    })
    return result


GENERATORS = {
    "semantic-owner-inventory.json": semantic_owner_inventory,
    "aot-plan-destination-inventory.json": aot_plan_inventory,
    "legacy-vm-inventory.json": legacy_vm_inventory,
    "object-extent-inventory.json": object_extent_inventory,
    "validation-matrix.json": validation_matrix,
}


def render(data: dict[str, Any]) -> str:
    return json.dumps(data, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


def validate_inventory(name: str, data: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    if data.get("schema") != SCHEMA:
        errors.append(f"{name}: wrong schema")
    if not data.get("source_tree_fingerprint"):
        errors.append(f"{name}: missing source tree fingerprint")
    if name == "semantic-owner-inventory.json":
        ids = [row["operation_id"] for row in data["operations"]]
        if len(ids) != len(set(ids)):
            errors.append(f"{name}: duplicate operation identity")
        for row in data["operations"]:
            if not row["future_semantic_owner"] or not row["independent_oracle"] or not row["migration_task"]:
                errors.append(f"{name}: incomplete row {row['operation_id']}")
    elif name == "aot-plan-destination-inventory.json":
        for row in data["rows"]:
            if row["category"] not in {"semantic", "target", "refinement", "emission-link", "obsolete"}:
                errors.append(f"{name}: unclassified {row['current_type']}")
            if not row["destination"] or not row["deletion_task"]:
                errors.append(f"{name}: unresolved destination {row['current_type']}")
    elif name == "legacy-vm-inventory.json":
        if not data["opcodes"] or not data["tagged_frame_sites"] or not data["vm_public_api_symbols"]:
            errors.append(f"{name}: incomplete legacy surface")
    elif name == "object-extent-inventory.json":
        for row in data["families"]:
            if row["future_extent_owner"] != "XrExtentPlan":
                errors.append(f"{name}: noncanonical extent owner for {row['family']}")
    elif name == "validation-matrix.json":
        tiers = {row["support_tier"] for row in data["rows"]}
        if tiers != {"supported", "ci-only", "unqualified", "unsupported"}:
            errors.append(f"{name}: incomplete support tiers: {sorted(tiers)}")
    return errors


def validate_policies(root: Path) -> list[str]:
    errors: list[str] = []
    diagnostics_path = root / "contracts/target-machine/diagnostic-codes.toml"
    identity_path = root / "contracts/target-machine/id-and-fingerprint-policy.toml"
    diagnostics = tomllib.loads(read(diagnostics_path))
    identity = tomllib.loads(read(identity_path))

    expected_prefixes = {
        "XR_SEM_", "XR_TARGET_", "XR_ARTIFACT_", "XR_OWN_", "XR_CORO_", "XR_EXEC_"
    }
    families = diagnostics.get("family", [])
    prefixes = {family.get("prefix") for family in families}
    if prefixes != expected_prefixes:
        errors.append(f"diagnostic family mismatch: {sorted(prefixes)}")
    ranges: dict[str, tuple[int, int]] = {}
    for family in families:
        match = re.fullmatch(r"([0-9]{4})-([0-9]{4})", family.get("range", ""))
        if not match:
            errors.append(f"invalid diagnostic range for {family.get('prefix')}")
            continue
        ranges[family["prefix"]] = (int(match.group(1)), int(match.group(2)))
    seen: set[str] = set()
    for code in diagnostics.get("code", []):
        code_id = code.get("id", "")
        if code_id in seen:
            errors.append(f"duplicate diagnostic code {code_id}")
        seen.add(code_id)
        prefix = next((item for item in expected_prefixes if code_id.startswith(item)), None)
        if prefix is None or prefix not in ranges:
            errors.append(f"diagnostic code outside registered family: {code_id}")
            continue
        suffix = code_id[len(prefix):]
        if not re.fullmatch(r"[0-9]{4}", suffix):
            errors.append(f"invalid diagnostic code spelling: {code_id}")
            continue
        low, high = ranges[prefix]
        if not low <= int(suffix) <= high:
            errors.append(f"diagnostic code outside family range: {code_id}")
        if not code.get("name") or not code.get("message"):
            errors.append(f"diagnostic code lacks stable name/message: {code_id}")

    if identity.get("schema") != SCHEMA:
        errors.append("identity policy has wrong schema")
    if identity.get("entity_id", {}).get("algorithm") != "sha256":
        errors.append("entity ID algorithm must be sha256")
    if identity.get("entity_id", {}).get("serialized_width_bits") != 128:
        errors.append("entity ID serialized width must be 128 bits")
    if identity.get("plan_fingerprint", {}).get("serialized_width_bits") != 256:
        errors.append("plan fingerprint serialized width must be 256 bits")
    if identity.get("artifact", {}).get("file_endianness") != "little":
        errors.append("artifact canonical endianness must be little")
    forbidden = set(identity.get("canonical_key", {}).get("forbids", []))
    required_forbidden = {"pointer-or-address", "insertion-or-worker-completion-order"}
    if not required_forbidden.issubset(forbidden):
        errors.append("identity policy does not reject address/order inputs")
    return errors


def validate_phase0_evidence(root: Path) -> list[str]:
    errors: list[str] = []
    retained = (
        "tests/target-machine/phase0/coroutine_vertical/manifest.toml",
        "tests/target-machine/phase0/coroutine_vertical/report.json",
        "tests/target-machine/phase0/coroutine_vertical/loop_suspend_try_catch.xr",
        "tests/target-machine/phase0/typed_slot_calibration/manifest.toml",
        "tests/target-machine/phase0/typed_slot_calibration/report.json",
        "tests/target-machine/phase0/typed_slot_calibration/workload.xr",
        "tests/target-machine/phase0/typed_slot_calibration/mailbox.xr",
        "tests/target-machine/phase0/negative/manifest.toml",
    )
    for relative in retained:
        if not (root / relative).is_file():
            errors.append(f"missing retained Phase 0 evidence: {relative}")

    disposed = (
        "tests/target-machine/phase0/coroutine_vertical/run.py",
        "tests/target-machine/phase0/typed_slot_calibration/run.py",
        "tests/target-machine/phase0/typed_slot_calibration/calibrate.c",
    )
    for relative in disposed:
        if (root / relative).exists():
            errors.append(f"disposed spike implementation remains: {relative}")
    cmake = read(root / "CMakeLists.txt")
    for token in (
        "target_machine_coroutine_vertical",
        "target_machine_typed_slot_calibration",
    ):
        if token in cmake:
            errors.append(f"disposed spike build/test selector remains: {token}")

    reports = []
    for relative in (
        "tests/target-machine/phase0/coroutine_vertical/report.json",
        "tests/target-machine/phase0/typed_slot_calibration/report.json",
    ):
        path = root / relative
        if not path.is_file():
            continue
        report = json.loads(read(path))
        reports.append(report)
        if report.get("schema") != SCHEMA or report.get("result") != "passed":
            errors.append(f"invalid Phase 0 report result/schema: {relative}")
        source = report.get("measured_source", {})
        if source.get("git_dirty") is not False:
            errors.append(f"Phase 0 report was not measured from clean source: {relative}")
        if not re.fullmatch(r"[0-9a-f]{40}", source.get("git_commit", "")):
            errors.append(f"Phase 0 report lacks exact source commit: {relative}")
    if len(reports) == 2:
        commits = {report["measured_source"]["git_commit"] for report in reports}
        if len(commits) != 1:
            errors.append("Phase 0 spike reports were measured from different commits")

    typed_path = root / "tests/target-machine/phase0/typed_slot_calibration/report.json"
    if typed_path.is_file():
        typed = json.loads(read(typed_path))
        decision = typed.get("decision", {})
        if decision.get("universal_tagged_frame_exception") is not False:
            errors.append("typed-slot calibration created a universal tagged frame exception")
        mutations = typed.get("typed_plan_calibration", {}).get("negative_mutations", {})
        if not mutations or set(mutations.values()) != {"detected"}:
            errors.append("typed-slot calibration has incomplete negative mutation evidence")

    negative_path = root / "tests/target-machine/phase0/negative/manifest.toml"
    diagnostic_path = root / "contracts/target-machine/diagnostic-codes.toml"
    if negative_path.is_file() and diagnostic_path.is_file():
        negative = tomllib.loads(read(negative_path))
        diagnostics = tomllib.loads(read(diagnostic_path))
        registered = {row["id"] for row in diagnostics.get("code", [])}
        ids: set[str] = set()
        for row in negative.get("case", []):
            case_id = row.get("id", "")
            if not case_id or case_id in ids:
                errors.append(f"duplicate or empty Phase 0 negative case: {case_id!r}")
            ids.add(case_id)
            if row.get("expected_diagnostic") not in registered:
                errors.append(f"negative case uses unregistered diagnostic: {case_id}")
            if not isinstance(row.get("production_owner_task"), int):
                errors.append(f"negative case has no production owner task: {case_id}")
        if len(ids) != 10:
            errors.append(f"expected 10 Phase 0 negative cases, found {len(ids)}")
    return errors


def run(root: Path, write: bool) -> int:
    target = root / "contracts/target-machine"
    target.mkdir(parents=True, exist_ok=True)
    errors: list[str] = []
    summaries = []
    for name, generate in GENERATORS.items():
        data = generate(root)
        errors.extend(validate_inventory(name, data))
        expected = render(data)
        path = target / name
        if write:
            path.write_text(expected, encoding="utf-8")
        elif not path.is_file():
            errors.append(f"{path.relative_to(root)} is missing")
        elif read(path) != expected:
            errors.append(f"{path.relative_to(root)} drifted; run --write and review")
        summaries.append(f"{name}={len(data.get('operations', data.get('rows', data.get('opcodes', data.get('families', [])))))}")
    errors.extend(validate_policies(root))
    summaries.append("stable-policies=2")
    errors.extend(validate_phase0_evidence(root))
    summaries.append("disposed-spike-evidence=2")
    if errors:
        print("target-machine Phase 0 inventory: FAIL", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    print("target-machine Phase 0 inventory: PASS (" + ", ".join(summaries) + ")")
    return 0


def self_test() -> int:
    relative = "governed.txt"
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        path = root / relative
        path.write_bytes(b"first\nsecond\n")
        lf_fingerprint = sha256_paths(root, [relative])
        path.write_bytes(b"first\r\nsecond\r\n")
        crlf_fingerprint = sha256_paths(root, [relative])
        (root / "z").mkdir()
        (root / "a").mkdir()
        (root / "z" / "later.c").write_text("", encoding="utf-8")
        (root / "a" / "first.c").write_text("", encoding="utf-8")
        assert [path.relative_to(root).as_posix() for path in source_files(root, ".")] == [
            "a", "a/first.c", "governed.txt", "z", "z/later.c"
        ]
    if lf_fingerprint != crlf_fingerprint:
        print("target-machine Phase 0 inventory self-test: FAIL", file=sys.stderr)
        return 1
    print("target-machine Phase 0 inventory self-test: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".")
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    root = Path(args.root).resolve()
    try:
        return run(root, args.write)
    except (OSError, RuntimeError, KeyError, tomllib.TOMLDecodeError) as error:
        print(f"target-machine Phase 0 inventory: ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

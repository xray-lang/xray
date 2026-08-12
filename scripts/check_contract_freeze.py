#!/usr/bin/env python3
"""Semantic-contract digest gate.

Each contract records an anchor-sha256 line per source file it governs. A
file whose content no longer matches its recorded digest means the contract
was not re-read when the code under it moved, and that is what this catches.

Digest drift is the whole gate. There is deliberately no commit-message
requirement: during high-speed development the change itself is the record,
and a rule that every anchor edit must be re-justified in a trailer would be
either noise or a permanently red gate.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class ContractSpec:
    name: str
    anchors: tuple[str, ...]


CONTRACT_SPECS = (
    ContractSpec("intrinsic-identity.md", ("src/frontend/analyzer/xa_intrinsic_registry.def",)),
    ContractSpec("xi-canonical-ops.md", ("xisa/xi/ops.def", "xisa/xi/lowering.def")),
    ContractSpec(
        "effect-semantics.md",
        (
            "src/app/cli/xcmd_verify.c",
            "src/frontend/analyzer/xa_effect_db.c",
            "src/frontend/analyzer/xa_effect_db.h",
            "src/frontend/analyzer/xa_memory_effect_db.c",
            "src/frontend/analyzer/xa_memory_effect_db.h",
            "src/frontend/analyzer/xa_typed_program.c",
            "src/frontend/analyzer/xanalyzer.c",
            "src/frontend/analyzer/xanalyzer.h",
            "src/frontend/analyzer/xanalyzer_errorset.c",
            "src/frontend/analyzer/xanalyzer_allocation.c",
            "src/frontend/analyzer/xanalyzer_memory_effect.c",
            "src/frontend/analyzer/xanalyzer_suspend.c",
            "src/frontend/analyzer/xanalyzer_visitor_internal.h",
            "src/frontend/analyzer/xanalyzer_visitor_decl.c",
            "src/frontend/analyzer/xanalyzer_visitor_call.c",
            "src/frontend/analyzer/xanalyzer_visitor_stmt.c",
            "src/ir/xi.h",
            "src/ir/xi_lower.c",
            "src/plan/format/xr_xsm_encode.c",
            "src/plan/format/xr_xsm_schema.h",
            "src/plan/semantic/xr_semantic_builder.c",
            "src/plan/semantic/xr_semantic_ids.h",
            "src/plan/semantic/xr_semantic_plan.c",
            "src/plan/semantic/xr_semantic_plan.h",
            "src/plan/semantic/xr_semantic_plan_internal.h",
            "src/plan/semantic/xr_semantic_verify.c",
            "src/runtime/value/xtype.h",
            "src/stdlib/xstdlib_metadata.h",
            "tests/cli/run_verify_contract_tests.py",
            "tests/unit/analyzer/test_analyzer.c",
            "tests/unit/analyzer/test_effect_db.c",
            "tests/unit/ir/test_xi_lower.c",
            "tests/unit/plan/test_semantic_plan.c",
        ),
    ),
    ContractSpec(
        "zero-cost-residue.md",
        (
            "src/aot/xi_cgen.h",
            "src/aot/xi_cgen.c",
            "src/aot/xi_cgen_ctx_impl.inc.c",
            "src/app/cli/xcmd_verify.c",
        ),
    ),
    ContractSpec(
        "rc-contract.md",
        (
            "src/ir/xi_arc_verify.c",
            "src/ir/xi_arc.c",
            "src/ir/xi_lower_expr.c",
            "src/aot/xi_cgen_dispatch_helpers.inc.c",
            "src/aot/xrt_coll.h",
            "src/runtime/mem/xfixed_heap.c",
            "src/runtime/core/xr_runtime_core.c",
            "src/api/xisolate.c",
            "tests/unit/mem/test_fixed_heap_teardown.c",
        ),
    ),
    ContractSpec(
        "cgen-wellformedness.md",
        (
            "src/aot/xi_cgen_verify_output.h",
            "src/aot/xi_cgen_verify_output.c",
            "tests/unit/aot/test_cgen_verify_output.c",
        ),
    ),
    ContractSpec("meta-ownership.md", ("scripts/check_meta_ownership.py",)),
    ContractSpec(
        "differential-protocol.md",
        (
            "tests/diff/run_backend_diff.py",
            "tests/aot/TOMBSTONES.tsv",
        ),
    ),
    ContractSpec(
        "process-byte-stream.md",
        (
            "src/os/win/proc_win.c",
            "src/aot/xrt_sys.h",
            "src/aot/xrt_os.h",
            "src/app/toolchain/xtc_process.h",
            "src/app/toolchain/xtc_process.c",
            "scripts/check_subprocess_text_boundaries.py",
            "scripts/check_process_zero_cost.py",
            "tests/probes/rc/check_execution_arena_l2.py",
            "tests/probes/rc/check_mutable_capture_cell_rss.py",
            "tests/unit/cli/test_cli_toolchain.c",
        ),
    ),
    ContractSpec(
        "target-abi.md",
        (
            "src/aot/xaot_link.c",
            "src/aot/xaot_prepare.c",
            "src/aot/xaot_verify.c",
            "src/aot/xaot_coro.h",
            "src/aot/xi_cgen_abi_helpers.inc.c",
            "src/aot/xi_cgen_class_helpers.inc.c",
            "src/aot/xi_cgen_class_native_helpers.inc.c",
            "src/aot/xi_cgen_dispatch_helpers.inc.c",
            "src/aot/xi_cgen_program_entry.inc.c",
            "src/aot/xi_cgen_struct_helpers.inc.c",
            "src/aot/xi_cgen.c",
            "src/aot/xrt_coll.h",
            "src/aot/xrt_core_freestanding.h",
            "src/aot/xrt_provider_abi.h",
            "src/aot/xrt_time.h",
            "include/xray_hosted_fragment_abi.h",
            "src/app/cli/xcmd_build.c",
            "src/app/toolchain/xtc_model.c",
            "src/app/toolchain/xtc_probe.c",
            "src/ir/xi.h",
            "stdlib/simd/simd.xr",
        ),
    ),
    ContractSpec(
        "memory-model.md",
        (
            "xisa/xi/ops.def",
            "src/ir/xi_tbaa.c",
            "src/ir/xi_tbaa.h",
            "src/ir/xi_opt_licm.c",
            "src/ir/xi_opt_gvn_pre.c",
            "src/ir/xi_memssa.c",
            "src/coro/xchannel.c",
            "src/coro/xtask.c",
            "src/coro/xtask_await.c",
            "src/frontend/canonical/xcanon.c",
        ),
    ),
    ContractSpec(
        "structural-object-json-map-boundary.md",
        (
            "src/shared/xobject_shape.h",
            "src/runtime/value/xtype.h",
            "src/runtime/value/xtype.c",
            "src/frontend/parser/xtype_ref.h",
            "src/frontend/parser/xtype_ref.c",
            "src/frontend/analyzer/xanalyzer_capability.h",
            "src/frontend/analyzer/xanalyzer_capability.c",
            "src/frontend/analyzer/xanalyzer_visitor_expr.c",
            "src/frontend/analyzer/xanalyzer_visitor_call.c",
            "src/frontend/analyzer/xtype_ref_resolve.c",
            "src/analysis/xglobal_summary.h",
            "src/ir/xi.h",
            "xisa/xi/ops.def",
            "src/aot/xrt_coll.h",
            "src/aot/xi_cgen_dispatch_helpers.inc.c",
            "src/aot/xi_cgen_program_entry.inc.c",
            "src/runtime/class/xclass.h",
            "src/runtime/class/xinstance.c",
            "src/runtime/object/xjson.c",
            "src/runtime/object/xjson_serde.c",
            "stdlib/types/json.xr",
            "src/module/xbytecode_io.h",
            "src/module/xbytecode_io.c",
            "src/aot/xaot_verify.c",
        ),
    ),
    ContractSpec(
        "sort-semantics.md",
        (
            "src/shared/xr_sort_core.h",
            "src/runtime/object/xarray_vm.c",
            "src/aot/xrt_sort.inc.c",
            "tests/diff/cases/semantics/collections/array_sort_shared_core.xr",
            "tests/unit/stdlib/test_array_core.c",
        ),
    ),
    ContractSpec(
        "semantic-ownership.md",
        (
            "contracts/semantic-owners.toml",
            "contracts/semantic-owner-registry.json",
            "contracts/hof-shape-matrix.toml",
            "contracts/shared-core-inventory.json",
            "src/shared/xr_semantic_owner_ids_gen.h",
            "scripts/check_semantic_owners.py",
        ),
    ),
    ContractSpec(
        "unified-target-machine-discovery.md",
        (
            "contracts/target-machine/semantic-owner-inventory.json",
            "contracts/target-machine/aot-plan-destination-inventory.json",
            "contracts/target-machine/legacy-vm-inventory.json",
            "contracts/target-machine/legacy-product-residue.json",
            "contracts/target-machine/object-extent-inventory.json",
            "contracts/target-machine/validation-matrix.json",
            "contracts/target-machine/baseline-manifest.json",
            "contracts/target-machine/diagnostic-codes.toml",
            "contracts/target-machine/id-and-fingerprint-policy.toml",
            "scripts/target_machine_phase0.py",
            "scripts/check_legacy_product_residue.py",
            "scripts/check_runtime_header_dependencies.py",
            "tests/target-machine/phase0/run_baseline.py",
            "tests/benchmarks/target-machine/source_run/main.xr",
            "tests/benchmarks/target-machine/source_run/helper.xr",
            "tests/benchmarks/target-machine/source_run/helper.edited.txt",
            "tests/target-machine/phase0/coroutine_vertical/loop_suspend_try_catch.xr",
            "tests/target-machine/phase0/coroutine_vertical/manifest.toml",
            "tests/target-machine/phase0/coroutine_vertical/report.json",
            "tests/target-machine/phase0/typed_slot_calibration/mailbox.xr",
            "tests/target-machine/phase0/typed_slot_calibration/manifest.toml",
            "tests/target-machine/phase0/typed_slot_calibration/report.json",
            "tests/target-machine/phase0/typed_slot_calibration/workload.xr",
            "tests/target-machine/phase0/negative/manifest.toml",
        ),
    ),
    ContractSpec(
        "runtime-abi-foundation.md",
        (
            "src/base/xstable_id.h",
            "contracts/target-machine/id-and-fingerprint-policy.toml",
            "src/plan/semantic/xr_semantic_ids.h",
            "src/runtime/abi/xr_runtime_descriptor.h",
            "src/runtime/abi/xr_runtime_descriptor.c",
            "src/runtime/abi/xr_target_runtime_profile.h",
            "src/runtime/abi/xr_runtime_contract.h",
            "src/runtime/abi/xr_runtime_contract.c",
            "src/runtime/abi/xr_runtime_object_header.h",
            "src/runtime/abi/xr_runtime_object_header.c",
            "contracts/target-machine/runtime-string-object-contract.toml",
            "src/runtime/abi/xr_runtime_string_object.h",
            "src/runtime/abi/xr_runtime_string_object.c",
            "tests/unit/runtime/test_runtime_descriptor.c",
            "tests/unit/runtime/test_runtime_abi_contract.c",
            "tests/unit/runtime/test_runtime_object_header.c",
            "tests/unit/runtime/test_runtime_string_object.c",
            "tests/unit/CMakeLists.txt",
            "scripts/check_runtime_object_header_boundary.py",
            "scripts/check_runtime_string_object_boundary.py",
        ),
    ),
    ContractSpec(
        "runtime-target-plan-load.md",
        (
            "CMakeLists.txt",
            "include/xray_target_plan_load.h",
            "src/plan/format/xr_artifact_kind.h",
            "src/plan/format/xr_artifact_kind.c",
            "src/plan/format/xr_xsm_schema.h",
            "src/plan/format/xr_xsm_decode.c",
            "src/plan/format/xr_xtp_schema.h",
            "src/plan/format/xr_xtp_internal.h",
            "src/plan/format/xr_xtp_artifact.c",
            "src/plan/format/xr_xtp_decode.c",
            "src/plan/format/xr_xtp_rows.c",
            "src/runtime/abi/xr_target_machine_facts.h",
            "src/runtime/abi/xr_runtime_target_authority.h",
            "src/runtime/abi/xr_runtime_target_authority.c",
            "src/plan/target/xr_target_profile.h",
            "src/plan/target/xr_target_profile.c",
            "src/plan/target/xr_target_plan.h",
            "src/plan/target/xr_target_plan.c",
            "src/plan/target/xr_target_builder.c",
            "src/plan/target/xr_target_verify.c",
            "src/plan/target/xr_xtp_materialize.c",
            "src/runtime/xr_runtime_artifact_authority_internal.h",
            "src/runtime/xr_runtime_artifact_authority.c",
            "src/runtime/xr_runtime_artifact_verify.c",
            "src/runtime/xr_target_plan_load.c",
            "src/app/cli/xcmd_run.c",
            "contracts/target-machine/legacy-product-residue.json",
            "scripts/check_legacy_product_residue.py",
            "tests/unit/plan/test_target_plan.c",
            "tests/unit/plan/test_xtp_format.c",
            "tests/unit/CMakeLists.txt",
            "tests/unit/runtime/test_runtime_target_plan_load_archive.c",
            "tests/cli/run_target_artifact_boundary_tests.py",
            "tests/install/run_installed_runtime_symbol_tests.py",
        ),
    ),
    ContractSpec(
        "typed-target-plan-frame.md",
        (
            "src/plan/target/xr_target_plan.h",
            "src/vm/xr_typed_frame.h",
            "src/vm/xr_typed_frame.c",
            "tests/unit/vm/test_typed_frame.c",
            "tests/unit/runtime/test_typed_frame_runtime_archive.c",
            "tests/unit/runtime/test_runtime_generation.c",
        ),
    ),
    ContractSpec(
        "typed-target-plan-execution.md",
        (
            "src/plan/target/xr_target_plan.h",
            "src/plan/target/xr_target_plan.c",
            "src/plan/target/xr_target_builder.c",
            "src/plan/target/xr_target_instruction_verify.h",
            "src/plan/target/xr_target_instruction_verify.c",
            "src/plan/target/xr_target_verify.c",
            "src/plan/format/xr_xtp_schema.h",
            "src/plan/format/xr_xtp_rows.c",
            "src/plan/format/xr_xtp_encode.c",
            "src/plan/target/xr_xtp_materialize.c",
            "src/vm/xr_typed_dispatch.h",
            "src/vm/xr_typed_dispatch.c",
            "src/vm/xr_typed_frame.c",
            "tests/unit/vm/test_typed_dispatch.c",
            "tests/unit/plan/test_xtp_format.c",
            "tests/unit/runtime/test_typed_frame_runtime_archive.c",
            "include/xray_runtime_generation.h",
            "src/runtime/xr_module_generation.c",
            "tests/unit/runtime/test_runtime_generation.c",
        ),
    ),
    ContractSpec(
        "incremental-cache-store.md",
        (
            "src/incremental/xr_cache_artifact_verify.h",
            "src/incremental/xr_cache_artifact_verify.c",
            "src/incremental/xr_cache_store.h",
            "src/incremental/xr_cache_store.c",
            "src/incremental/xr_target_plan_tasks.h",
            "src/incremental/xr_target_plan_tasks.c",
            "src/aot/xaot_driver.h",
            "src/aot/xaot_driver.c",
            "src/os/os_fs.h",
            "src/os/unix/fs_unix.c",
            "src/os/win/fs_win.c",
            "tests/unit/CMakeLists.txt",
            "tests/unit/incremental/test_cache_artifact_verify.c",
            "tests/unit/incremental/test_cache_store.c",
            "tests/unit/incremental/test_target_plan_tasks.c",
            "tests/unit/aot/test_xaot_driver.c",
            "tests/unit/os/test_fs_atomic.c",
        ),
    ),
    ContractSpec(
        "incremental-compiler-session.md",
        (
            "src/toolchain/xcompiler_session.h",
            "src/toolchain/xcompiler_session.c",
            "tests/unit/toolchain/test_compiler_session_generation.c",
        ),
    ),
    ContractSpec(
        "runtime-generation-lifecycle.md",
        (
            "include/xray_runtime_generation.h",
            "src/runtime/xr_module_generation_internal.h",
            "src/runtime/xr_module_generation.c",
            "src/runtime/xr_module_generation_verify.c",
            "src/vm/xr_typed_dispatch.h",
            "src/vm/xr_typed_dispatch.c",
            "src/vm/xr_typed_frame.h",
            "src/vm/xr_typed_frame.c",
            "contracts/target-machine/diagnostic-codes.toml",
            "tests/unit/runtime/test_runtime_generation.c",
            "tests/unit/runtime/test_runtime_generation_archive.c",
            "tests/install/run_installed_runtime_symbol_tests.py",
            "tests/install/run_install_public_surface_tests.py",
        ),
    ),
    ContractSpec(
        "ownership-audit-foundation.md",
        (
            "CMakeLists.txt",
            "src/shared/xr_ownership_event.h",
            "src/plan/ownership/xr_ownership_certificate.h",
            "src/runtime/ownership/xr_ownership_audit.h",
            "src/runtime/ownership/xr_ownership_audit.c",
            "scripts/check_ownership_audit_record_no_alloc.py",
            "scripts/check_ownership_audit_release_boundary.py",
            "scripts/run_tsan_focused.py",
            "tests/unit/CMakeLists.txt",
            "tests/unit/runtime/test_ownership_audit.c",
        ),
    ),
)

ANCHOR_RE = re.compile(r"^anchor-sha256:\s+(\S+)\s+([0-9a-f]{64})\s*$")
def digest(path: Path) -> str:
    # Contract anchors describe repository content, whose canonical Git form
    # uses LF. Normalize checkout-only CRLF so the gate has the same result on
    # Windows and Unix hosts while preserving every other byte.
    content = path.read_bytes().replace(b"\r\n", b"\n")
    return hashlib.sha256(content).hexdigest()


def parse_contract(path: Path) -> tuple[dict[str, str], list[str]]:
    anchors: dict[str, str] = {}
    errors: list[str] = []
    if not path.is_file():
        return anchors, [f"missing contract: {path}"]
    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.startswith("anchor-sha256:"):
            continue
        match = ANCHOR_RE.match(line)
        if not match:
            errors.append(f"{path}:{lineno}: malformed anchor-sha256 record")
            continue
        anchor, expected = match.groups()
        if anchor in anchors:
            errors.append(f"{path}:{lineno}: duplicate anchor {anchor}")
        anchors[anchor] = expected
    return anchors, errors


def verify_digests(root: Path, contracts_dir: Path, specs=CONTRACT_SPECS) -> list[str]:
    errors: list[str] = []
    for spec in specs:
        path = contracts_dir / spec.name
        recorded, parse_errors = parse_contract(path)
        errors.extend(parse_errors)
        expected_anchors = set(spec.anchors)
        if set(recorded) != expected_anchors:
            missing = sorted(expected_anchors - set(recorded))
            extra = sorted(set(recorded) - expected_anchors)
            if missing:
                errors.append(f"{path}: missing anchors: {', '.join(missing)}")
            if extra:
                errors.append(f"{path}: unregistered anchors: {', '.join(extra)}")
        for anchor in spec.anchors:
            source = root / anchor
            if not source.is_file():
                errors.append(f"{path}: anchor does not exist: {anchor}")
                continue
            actual = digest(source)
            if recorded.get(anchor) != actual:
                errors.append(
                    f"{path}: digest drift for {anchor}: "
                    f"recorded={recorded.get(anchor, '<missing>')} actual={actual}"
                )
    return errors


def self_test() -> int:
    with tempfile.TemporaryDirectory(prefix="xray-contract-freeze-") as tmp:
        root = Path(tmp)
        (root / "contracts").mkdir()
        (root / "src").mkdir()
        anchor = root / "src" / "truth.def"
        anchor.write_text("v1\n", encoding="utf-8")
        spec = ContractSpec("sample.md", ("src/truth.def",))
        contract = root / "contracts" / spec.name
        contract.write_text(
            f"# Sample\n\nanchor-sha256: src/truth.def {digest(anchor)}\n", encoding="utf-8"
        )
        assert verify_digests(root, root / "contracts", (spec,)) == []
        anchor.write_bytes(b"v1\r\n")
        assert verify_digests(root, root / "contracts", (spec,)) == []
        anchor.write_text("v2\n", encoding="utf-8")
        assert any("digest drift" in error for error in verify_digests(root, root / "contracts", (spec,)))
    print("contract freeze injection self-test: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--contracts-dir", default="contracts", help="contract directory")
    parser.add_argument("--self-test", action="store_true", help="run injected drift checks")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    root = Path(args.root).resolve()
    contracts_dir = (root / args.contracts_dir).resolve()
    errors = verify_digests(root, contracts_dir)


    if errors:
        print("task-220 contract freeze gate failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1

    print(f"contract freeze: PASS ({len(CONTRACT_SPECS)} contracts)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

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
            "src/frontend/analyzer/xanalyzer_errorset.c",
            "src/frontend/analyzer/xanalyzer_allocation.c",
            "src/frontend/analyzer/xanalyzer_memory_effect.c",
            "src/frontend/analyzer/xanalyzer_suspend.c",
            "src/frontend/analyzer/xanalyzer_visitor_internal.h",
            "src/frontend/analyzer/xanalyzer_visitor_decl.c",
            "src/frontend/analyzer/xanalyzer_visitor_stmt.c",
            "src/ir/xi.h",
            "src/ir/xi_lower.c",
            "src/runtime/value/xtype.h",
            "tests/cli/run_verify_contract_tests.py",
            "tests/unit/analyzer/test_analyzer.c",
            "tests/unit/analyzer/test_effect_db.c",
            "tests/unit/ir/test_xi_lower.c",
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
    ContractSpec("rc-contract.md", ("src/ir/xi_arc_verify.c",)),
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
            "src/aot/xi_cgen_abi_helpers.inc.c",
            "src/aot/xi_cgen_class_native_helpers.inc.c",
            "src/aot/xi_cgen_dispatch_helpers.inc.c",
            "src/aot/xi_cgen_program_entry.inc.c",
            "src/aot/xi_cgen_struct_helpers.inc.c",
            "src/aot/xi_cgen.c",
            "src/aot/xrt_coll.h",
            "src/aot/xrt_core_freestanding.h",
            "src/aot/xrt_time.h",
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
        "object-json-domain.md",
        (
            "src/shared/xobject_row.h",
            "src/shared/xobject_shape.h",
            "src/runtime/value/xtype.h",
            "src/runtime/value/xtype.c",
            "src/frontend/parser/xtype_ref.h",
            "src/frontend/parser/xtype_ref.c",
            "src/frontend/analyzer/xanalyzer_capability.h",
            "src/frontend/analyzer/xanalyzer_capability.c",
            "src/frontend/analyzer/xanalyzer_visitor_expr.c",
            "src/analysis/xglobal_summary.h",
            "src/ir/xi.h",
            "xisa/xi/ops.def",
            "src/aot/xrt_coll.h",
            "src/aot/xi_cgen_dispatch_helpers.inc.c",
            "src/aot/xi_cgen_program_entry.inc.c",
            "src/runtime/class/xclass.h",
            "src/runtime/class/xinstance.c",
            "src/module/xbytecode_io.h",
            "src/module/xbytecode_io.c",
            "src/aot/xaot_verify.c",
        ),
    ),
)

ANCHOR_RE = re.compile(r"^anchor-sha256:\s+(\S+)\s+([0-9a-f]{64})\s*$")
def digest(path: Path) -> str:
    # Contract anchors describe repository content, whose canonical Git form
    # uses LF.  Normalize checkout-only CRLF so the gate has the same result on
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

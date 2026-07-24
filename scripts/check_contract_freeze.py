#!/usr/bin/env python3
"""Task 220 semantic-contract digest and CONTRACT-CHANGE gate."""

from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
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
            "tests/cli/run_verify_contract_tests.sh",
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
            "tests/diff/run_backend_diff.sh",
            "tests/diff/run_backend_diff_fast.py",
            "tests/aot/TOMBSTONES.tsv",
        ),
    ),
    ContractSpec(
        "target-abi.md",
        (
            "src/aot/xaot_link.c",
            "src/aot/xi_cgen_class_native_helpers.inc.c",
            "src/aot/xi_cgen_dispatch_helpers.inc.c",
            "src/aot/xi_cgen_struct_helpers.inc.c",
            "src/aot/xi_cgen.c",
            "src/aot/xrt_coll.h",
            "src/aot/xrt_core_freestanding.h",
            "src/aot/xrt_time.h",
            "src/app/cli/xcmd_build.c",
            "src/app/cli/xcli_toolchain.c",
        ),
    ),
)

ANCHOR_RE = re.compile(r"^anchor-sha256:\s+(\S+)\s+([0-9a-f]{64})\s*$")
TRAILER_RE = re.compile(r"^CONTRACT-CHANGE:\s+(\S+)\s+(.+)$", re.MULTILINE)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


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


def run_git(root: Path, args: list[str]) -> str:
    proc = subprocess.run(
        ["git", *args], cwd=root, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    return proc.stdout if proc.returncode == 0 else ""


def repository_is_dirty(root: Path) -> bool:
    return bool(run_git(root, ["status", "--porcelain"]).strip())


def head_changed_paths(root: Path) -> set[str]:
    text = run_git(root, ["diff-tree", "--root", "--no-commit-id", "--name-only", "-r", "HEAD"])
    return {line.strip() for line in text.splitlines() if line.strip()}


def head_message(root: Path) -> str:
    return run_git(root, ["log", "-1", "--format=%B"])


def required_contracts(changed: set[str], specs=CONTRACT_SPECS) -> set[str]:
    required: set[str] = set()
    for spec in specs:
        contract_path = f"contracts/{spec.name}"
        if contract_path in changed or any(anchor in changed for anchor in spec.anchors):
            required.add(contract_path)
    return required


def verify_trailers(changed: set[str], message: str, specs=CONTRACT_SPECS) -> list[str]:
    required = required_contracts(changed, specs)
    present: set[str] = set()
    for match in TRAILER_RE.finditer(message):
        name = match.group(1)
        if not name.startswith("contracts/"):
            name = f"contracts/{name}"
        present.add(name)
    return [
        f"missing commit trailer: CONTRACT-CHANGE: {name} <one-line reason>"
        for name in sorted(required - present)
    ]


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
        changed = {"src/truth.def", "contracts/sample.md"}
        assert verify_trailers(changed, "subject\n", (spec,))
        assert not verify_trailers(
            changed,
            "subject\n\nCONTRACT-CHANGE: contracts/sample.md injected test\n",
            (spec,),
        )
        anchor.write_text("v2\n", encoding="utf-8")
        assert any("digest drift" in error for error in verify_digests(root, root / "contracts", (spec,)))
    print("contract freeze injection self-test: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--contracts-dir", default="contracts", help="contract directory")
    parser.add_argument("--self-test", action="store_true", help="run injected drift/trailer checks")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    root = Path(args.root).resolve()
    contracts_dir = (root / args.contracts_dir).resolve()
    errors = verify_digests(root, contracts_dir)

    # A commit trailer cannot exist until a dirty change is committed. Digest
    # checking still runs in a dirty developer tree; trailer enforcement is
    # deferred to the clean post-commit/CI state.
    if not repository_is_dirty(root):
        errors.extend(verify_trailers(head_changed_paths(root), head_message(root)))

    if errors:
        print("task-220 contract freeze gate failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1

    print(f"contract freeze: PASS ({len(CONTRACT_SPECS)} contracts)")
    if repository_is_dirty(root):
        print("contract trailer check: deferred until the working tree is committed")
    else:
        print("contract trailer check: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

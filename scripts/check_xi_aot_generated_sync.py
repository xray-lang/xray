#!/usr/bin/env python3
"""Generated Xi and AOT artifacts must match their xisa source descriptions.

The `.def` files under xisa/ are the single source for Xi's opcode set, its
verifier, its lowering tables, and the AOT target's representation/ABI/layout
headers. Everything under src/ that those generate is checked in, so a build
never has to run the generator -- which means a stale checked-in artifact would
be silently authoritative. This regenerates into a scratch tree and compares.

Shared by the Xi and codegen invariant gates so the freshness contract has one
implementation.

Usage: check_xi_aot_generated_sync.py
"""

from __future__ import annotations

import filecmp
import sys
from pathlib import Path
from typing import List, Sequence


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parent.parent / "tests" / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import proc, workspace  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent
XISAGEN = Path("tools/xisagen/xisagen.py")

XI_SOURCES = (XISAGEN, Path("xisa/xi/ops.def"), Path("xisa/xi/lowering.def"),
              Path("xisa/xi/verifier.def"))
AOT_SOURCES = (XISAGEN, Path("xisa/aot/rep.def"), Path("xisa/aot/abi.def"),
               Path("xisa/aot/layout.def"))

XI_ARTIFACTS = (
    "src/ir/xi_ops_gen.h",
    "src/ir/xi_verify_gen.h",
    "src/ir/xi_lowering_coverage_gen.h",
    "src/ir/xi_emit_vm_gen.h",
    "src/vm/xvm_template_width_gen.inc.c",
    "src/vm/xvm_template_bitwise_binary_gen.inc.c",
    "src/vm/xvm_template_bitwise_unary_gen.inc.c",
    "src/vm/xvm_template_unary_gen.inc.c",
    "src/vm/xvm_template_arith_binary_gen.inc.c",
    "src/vm/xvm_template_shift_gen.inc.c",
    "src/vm/xvm_template_compare_gen.inc.c",
    "src/aot/xi_to_c_dispatch_gen.h",
    "src/aot/xi_to_c_stmt_dispatch_gen.h",
    "tests/unit/ir/test_xi_lowering_gen.c",
)

AOT_ARTIFACTS = (
    "src/aot/xaot_rep_gen.h",
    "src/aot/xaot_abi_gen.h",
    "src/aot/xaot_layout_gen.h",
)

RED = "\033[31m"
GREEN = "\033[32m"
NC = "\033[0m"


class Gate:
    def __init__(self) -> None:
        self.failed = False

    def red(self, message: str) -> None:
        print(f"{RED}{message}{NC}")
        self.failed = True

    def green(self, message: str) -> None:
        print(f"{GREEN}{message}{NC}")


def section(title: str) -> None:
    print("")
    print(f"=== {title} ===")


def require_files(gate: Gate, paths: Sequence[Path]) -> bool:
    ok = True
    for path in paths:
        if not (REPO_ROOT / path).is_file():
            gate.red(f"FAIL: required xisa source is missing: {path}")
            ok = False
    return ok


def run_gen(gate: Gate, argv: Sequence) -> bool:
    result = proc.run([sys.executable, REPO_ROOT / XISAGEN, *argv], cwd=REPO_ROOT)
    if result.ok:
        return True
    gate.red("FAIL: generator command failed:")
    shown = " ".join(str(a) for a in argv)
    print(f"  {XISAGEN} {shown}")
    for line in result.combined_text().splitlines():
        print(f"  {line}")
    return False


def compare(gate: Gate, group: str, scratch: Path,
            artifacts: Sequence[str]) -> None:
    stale = False
    for relative in artifacts:
        fresh = scratch / relative
        committed = REPO_ROOT / relative
        # shallow=False: same size and mtime is not evidence, the bytes are.
        if not (fresh.is_file() and committed.is_file()
                and filecmp.cmp(fresh, committed, shallow=False)):
            gate.red(f"FAIL: generated {group} artifact is stale: {relative}")
            stale = True
    if not stale:
        gate.green(f"OK: generated {group} artifacts match xisa sources.")


def main(argv: List[str]) -> int:
    gate = Gate()
    with workspace.Workspace("xray_xi_aot_sync") as ws:
        scratch = ws.root

        section("Xi semantic generated artifacts")
        if require_files(gate, XI_SOURCES):
            if (run_gen(gate, ["xi-ops", "xisa/xi/ops.def",
                               scratch / "src/ir/xi_ops_gen.h"])
                    and run_gen(gate, ["xi-verify", "xisa/xi/ops.def",
                                       "xisa/xi/verifier.def",
                                       scratch / "src/ir/xi_verify_gen.h"])
                    and run_gen(gate, ["xi-lowering", "xisa/xi/ops.def",
                                       "xisa/xi/lowering.def", scratch])):
                compare(gate, "Xi", scratch, XI_ARTIFACTS)

        section("AOT target generated artifacts")
        if require_files(gate, AOT_SOURCES):
            if (run_gen(gate, ["aot-rep", "xisa/aot/rep.def",
                               scratch / "src/aot/xaot_rep_gen.h"])
                    and run_gen(gate, ["aot-abi", "xisa/aot/rep.def",
                                       "xisa/aot/abi.def",
                                       scratch / "src/aot/xaot_abi_gen.h"])
                    and run_gen(gate, ["aot-layout", "xisa/aot/rep.def",
                                       "xisa/aot/layout.def",
                                       scratch / "src/aot/xaot_layout_gen.h"])):
                compare(gate, "AOT", scratch, AOT_ARTIFACTS)

    return 1 if gate.failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

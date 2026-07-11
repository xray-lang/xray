#!/usr/bin/env python3
"""Guard Task 193 parallel backend ABI/name convergence.

The stdlib `parallel` surface lowers to VM OP_PAR_* dispatch and AOT
`xr_parallel_*` runtime helpers. This checker blocks old AOT-private names from
returning and makes the current descriptor/runtime ABI expectations explicit in
source and cgen fixtures.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


SCAN_DIRS = ("src", "stdlib", "tests")
SCAN_SUFFIXES = {
    ".c",
    ".h",
    ".inc.c",
    ".def",
    ".xr",
    ".expect",
    ".expected",
}

LEGACY_BACKEND_PATTERNS = (
    (re.compile(r"\bxr_aot_parallel_[A-Za-z0-9_]+\b"), "legacy xr_aot_parallel_* ABI"),
    (re.compile(r"\bXrAotPar[A-Za-z0-9_]*\b"), "legacy XrAotPar* descriptor"),
    (re.compile(r"\bXR_AOT_PAR[A-Z0-9_]*\b"), "legacy XR_AOT_PAR* constant"),
    (re.compile(r"\bXrParallelPool\b"), "legacy global XrParallelPool"),
    (re.compile(r"\bxr_parallel_pool_[A-Za-z0-9_]+\b"), "legacy xr_parallel_pool_* helper"),
)

REQUIRED_TEXT = {
    Path("src/coro/xaot_coro.h"): (
        "typedef void (*XrParallelRangeI64Fn)",
        "typedef void (*XrParallelRangeStateI64Fn)",
        "typedef bool (*XrParallelReduceRangeI64Fn)",
        "typedef bool (*XrParallelReduceRangeStateI64Fn)",
        "typedef bool (*XrParallelReduceRangeAggFn)",
        "typedef bool (*XrParallelReduceRangeStateAggFn)",
        "typedef void (*XrParallelReduceCombineAggFn)",
        "XR_FUNC bool xr_parallel_for_range_i64(",
        "XR_FUNC bool xr_parallel_for_range_state_i64(",
        "XR_FUNC bool xr_parallel_reduce_i64(",
        "XR_FUNC bool xr_parallel_reduce_state_i64(",
        "XR_FUNC bool xr_parallel_reduce_agg(",
        "XR_FUNC bool xr_parallel_reduce_state_agg(",
    ),
    Path("src/coro/xaot_coro.c"): (
        "bool xr_parallel_for_range_i64(",
        "bool xr_parallel_for_range_state_i64(",
        "bool xr_parallel_reduce_i64(",
        "bool xr_parallel_reduce_state_i64(",
        "bool xr_parallel_reduce_agg(",
        "bool xr_parallel_reduce_state_agg(",
    ),
    Path("src/aot/xi_cgen_dispatch_helpers.inc.c"): (
        "xr_parallel_for_range_i64(",
        "xr_parallel_for_range_state_i64(",
        "xr_parallel_reduce_i64(",
        "xr_parallel_reduce_state_i64(",
        "xr_parallel_reduce_agg(",
        "xr_parallel_reduce_state_agg(",
    ),
    Path("src/vm/xvm_dispatch_parallel.inc.c"): (
        "vmcase(OP_PAR_FOR)",
        "vmcase(OP_PAR_MAP)",
        "vmcase(OP_PAR_REDUCE)",
        "vm_par_for_dispatch(",
        "vm_par_map_dispatch(",
        "vm_par_reduce_dispatch(",
    ),
    Path("src/ir/xi_emit_eh.c"): (
        "CREATE_ABC(OP_PAR_FOR",
        "CREATE_ABC(OP_PAR_MAP",
        "CREATE_ABC(OP_PAR_REDUCE",
    ),
    Path("tests/aot/filetests/cgen/parallel_named_import_for_each_intrinsic.expect"): (
        "c_contains=xr_parallel_for_range_i64(",
    ),
    Path("tests/aot/filetests/cgen/parallel_named_import_map_reduce_intrinsic.expect"): (
        "c_contains=xr_parallel_for_range_i64(",
        "c_contains=xr_parallel_reduce_i64(",
    ),
    Path("tests/aot/filetests/cgen/parallel_plan_for_each_state_intrinsic.expect"): (
        "c_contains=xr_parallel_for_range_state_i64(",
        "c_contains=XrParallelRangeStateI64Fn",
    ),
    Path("tests/aot/filetests/cgen/parallel_plan_map_state_intrinsic.expect"): (
        "c_contains=xr_parallel_for_range_state_i64(",
        "c_contains=XrParallelRangeStateI64Fn",
    ),
    Path("tests/aot/filetests/cgen/parallel_plan_reduce_state_intrinsic.expect"): (
        "c_contains=xr_parallel_reduce_state_i64(",
        "c_contains=XrParallelReduceRangeStateI64Fn",
    ),
    Path("tests/aot/filetests/cgen/parallel_plan_reduce_state_aggregate_intrinsic.expect"): (
        "c_contains=xr_parallel_reduce_state_agg(",
        "c_contains=XrParallelReduceRangeStateAggFn",
        "c_contains=XrParallelReduceCombineAggFn",
        "c_not_contains=xr_parallel_reduce_agg(",
        "c_not_contains=xr_parallel_reduce_state_i64(",
    ),
}


def rel(root: Path, path: Path) -> Path:
    return path.resolve().relative_to(root)


def iter_files(root: Path):
    for directory in SCAN_DIRS:
        base = root / directory
        if not base.exists():
            continue
        for path in sorted(base.rglob("*")):
            if path.is_file() and any(str(path).endswith(suffix) for suffix in SCAN_SUFFIXES):
                yield path


def check_legacy_symbols(root: Path) -> list[str]:
    errors: list[str] = []
    for path in iter_files(root):
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except UnicodeDecodeError:
            continue
        for lineno, line in enumerate(lines, 1):
            for pattern, label in LEGACY_BACKEND_PATTERNS:
                if pattern.search(line):
                    errors.append(f"{rel(root, path)}:{lineno}: {label}: {line.strip()}")
    return errors


def check_required_text(root: Path) -> list[str]:
    errors: list[str] = []
    for rel_path, snippets in REQUIRED_TEXT.items():
        path = root / rel_path
        if not path.exists():
            errors.append(f"{rel_path}: required parallel backend ABI file is missing")
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for snippet in snippets:
            if snippet not in text:
                errors.append(f"{rel_path}: missing required parallel backend ABI snippet: {snippet}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    errors = check_legacy_symbols(root) + check_required_text(root)
    if errors:
        print("parallel backend ABI convergence gate failed:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        print(
            "\nUse VM OP_PAR_* dispatch and AOT xr_parallel_* runtime helpers only; "
            "do not restore xr_aot_parallel*/XrAotPar*/XR_AOT_PAR* names.",
            file=sys.stderr,
        )
        return 1

    print("OK: parallel backend ABI uses current VM OP_PAR_* and AOT xr_parallel_* names")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""check_codegen_dispatch.py — verify backend handler tables match isel.def.

Compares the isel.def coverage manifest (src/jit/xm_dispatch_coverage_gen.h)
against the actual per-backend dispatch coverage in JIT source. Fails on drift
in either direction:

  - orphan handler: dispatch table entry for an op not declared in isel.def
  - missing handler: isel.def declares the op but no backend handler covers it

Backend dispatch sources of truth:
  - x64:     x64_ins_handlers[] in src/jit/xm_codegen_x64_ins.c
  - riscv64: rv64_ins_handlers[] in src/jit/xm_codegen_riscv64_ins.c
  - arm64:   a64_ins_handlers[] in src/jit/xm_codegen_ins.c
             plus shared lowering in xm_codegen_call.c / xm_codegen_mem.c

Some ops in isel.def are intentionally NOT routed through the per-instruction
dispatch tables because they are lowered earlier (xi_to_xm) or handled at
block boundaries (block terminators). They are listed in EXPECTED_FALLTHROUGH
below; this list is the only place where dispatch coverage can be exempted
from the strict manifest match.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


# Ops declared in isel.def but intentionally not routed through the per-
# instruction dispatch tables. Each entry must have a clear reason — anything
# without an explicit reason should be removed from this set.
EXPECTED_FALLTHROUGH: dict[str, str] = {
    # Block terminators: emitted by the per-block driver, not per-instruction.
    "JMP": "block terminator, emitted by block driver",
    "BR": "block terminator, emitted by block driver",
    "RET": "function epilogue, emitted by frame manager",
    # Lowered earlier by xi_to_xm.c before reaching codegen dispatch.
    "LOAD_UPVAL": "lowered by xi_to_xm to coro/field load",
    "STORE_UPVAL": "lowered by xi_to_xm to coro/field store",
    "DEFER_PUSH": "lowered earlier; no direct codegen entry",
    "CALL_INTRINSIC": "lowered by xi_to_xm to other CALL_* variants",
    # ARC stubs declared in isel.def for future use; never reaches codegen.
    "RETAIN": "ARC stub (unused), no codegen entry",
    "RELEASE": "ARC stub (unused), no codegen entry",
    # Generic placeholder; xi_to_xm always lowers calls to a more specific
    # CALL_C / CALL_DIRECT / CALL_SELF_DIRECT / CALL_KNOWN / CALL_KNOWN_REG /
    # CALL_C_LEAF variant. The x64 / riscv64 handler-table entries for
    # [XM_CALL] are defensive stubs that hard-error if ever reached.
    "CALL": "never emitted by xi_to_xm; lowered to CALL_C / CALL_* variants",
}


def die(msg: str) -> None:
    print(f"check_codegen_dispatch: error: {msg}", file=sys.stderr)
    sys.exit(1)


def parse_manifest(path: Path) -> dict[str, set[str]]:
    """Parse xm_dispatch_coverage_gen.h X-macros into {backend: {ops}}."""
    text = path.read_text()
    result: dict[str, set[str]] = {}
    for macro, key in (
        ("XM_DISPATCH_X64", "x64"),
        ("XM_DISPATCH_ARM64", "arm64"),
        ("XM_DISPATCH_RISCV64", "riscv64"),
    ):
        m = re.search(
            rf"#define\s+{macro}\(X\)((?:.|\n)*?)\n\n",
            text,
        )
        if not m:
            die(f"could not find {macro} in {path}")
        result[key] = set(re.findall(r"X\((\w+)\)", m.group(1)))
    return result


def extract_handler_table(path: Path, table_name: str) -> set[str]:
    """Extract op names from a C designated-initializer dispatch table."""
    text = path.read_text()
    pattern = (
        rf"\b{re.escape(table_name)}\s*\[\s*XM_OP_COUNT\s*\]\s*=\s*\{{"
        r"(.*?)"
        r"\n\}\s*;"
    )
    m = re.search(pattern, text, re.DOTALL)
    if not m:
        die(f"could not find table '{table_name}' in {path}")
    return set(re.findall(r"\[\s*XM_([A-Z0-9_]+)\s*\]\s*=", m.group(1)))


def extract_case_labels(path: Path) -> set[str]:
    """Extract `case XM_FOO:` labels from a C source file."""
    text = path.read_text()
    return set(re.findall(r"^\s*case\s+XM_([A-Z0-9_]+)\s*:", text, re.MULTILINE))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="Repository root (defaults to the parent of scripts/)",
    )
    ns = parser.parse_args()
    root: Path = ns.root.resolve()

    manifest_path = root / "src" / "jit" / "xm_dispatch_coverage_gen.h"
    if not manifest_path.exists():
        die(f"manifest not found: {manifest_path} — run cmake build first")
    manifest = parse_manifest(manifest_path)

    x64_handlers = extract_handler_table(
        root / "src" / "jit" / "xm_codegen_x64_ins.c", "x64_ins_handlers"
    )
    rv64_handlers = extract_handler_table(
        root / "src" / "jit" / "xm_codegen_riscv64_ins.c", "rv64_ins_handlers"
    )
    a64_handlers = extract_handler_table(
        root / "src" / "jit" / "xm_codegen_ins.c", "a64_ins_handlers"
    )
    shared_call_cases = extract_case_labels(
        root / "src" / "jit" / "xm_codegen_call.c"
    )
    shared_mem_cases = extract_case_labels(
        root / "src" / "jit" / "xm_codegen_mem.c"
    )

    actual: dict[str, set[str]] = {
        "x64": x64_handlers,
        "riscv64": rv64_handlers,
        "arm64": a64_handlers | shared_call_cases | shared_mem_cases,
    }

    fallthrough = set(EXPECTED_FALLTHROUGH)
    errors: list[str] = []
    report: list[str] = []

    for backend in ("x64", "arm64", "riscv64"):
        declared = manifest[backend]
        covered = actual[backend]
        expected = declared - fallthrough
        missing = sorted(expected - covered)
        orphan = sorted(covered - declared)

        report.append(
            f"{backend}: declared={len(declared)} covered={len(covered)} "
            f"fallthrough={len(declared & fallthrough)} "
            f"missing={len(missing)} orphan={len(orphan)}"
        )

        for op in missing:
            errors.append(
                f"{backend}: XM_{op} declared in isel.def but no handler found "
                f"(add a dispatch handler or list it in EXPECTED_FALLTHROUGH)"
            )
        for op in orphan:
            errors.append(
                f"{backend}: handler for XM_{op} exists but op is not in isel.def "
                f"(remove the handler or add an ISEL entry)"
            )

    for line in report:
        print(f"check_codegen_dispatch: {line}", file=sys.stderr)

    if errors:
        for e in errors:
            print(f"check_codegen_dispatch: error: {e}", file=sys.stderr)
        print(
            f"check_codegen_dispatch: {len(errors)} dispatch drift error(s)",
            file=sys.stderr,
        )
        return 1

    print("check_codegen_dispatch: OK — handler tables match isel.def manifest",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

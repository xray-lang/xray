#!/usr/bin/env python3
"""check_codegen_helpers.py — enforce xr_jit_* helper address closure.

Every JIT helper whose address is taken in `src/jit/*.c` must either:

  1. Be a registered helper in `xisa/xm/helpers.def`, and must be reached
     through `xm_helper_func(XM_HELPER_<name>)` — this direction is enforced
     by section 5 of `scripts/check_codegen_invariants.sh`.

  2. Or be classified explicitly in the NON_REGISTERED_HELPERS map below,
     with a category and rationale describing why it cannot (yet) be lowered
     through the registry.

Adding a new bare `&xr_jit_<name>` reference to `src/jit/*.c` therefore has
to either register the helper in helpers.def or extend this map with an
explicit classification. This blocks silent growth of the helper-bypass
surface that the L2 helper registry is supposed to own.

`src/jit/xm_helper_table.c` is intentionally excluded because it is the
canonical X-macro expansion site that turns helpers.def into the runtime
function-pointer table.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


# Non-registered helpers that are intentionally NOT in helpers.def.
# Each entry: name -> (category, reason).
# Categories:
#   runtime-stub      — JIT-emitted machine code calls this directly with a
#                       non-CALL_C ABI; cannot use the generic helper table
#                       lowering until the ABI is generalised.
NON_REGISTERED_HELPERS: dict[str, tuple[str, str]] = {
    "xr_jit_alloc": (
        "runtime-stub",
        "inline allocator slow path; emitted as load_imm64 + CALL with the "
        "bump-pointer fast-path ABI (not the generic CALL_C ABI), so it "
        "cannot be lowered through the L2 helper registry without first "
        "generalising the inline-alloc lowering",
    ),
    "xr_jit_barrier_fwd": (
        "runtime-stub",
        "GC write barrier (forward) emitted as a JIT stub with a "
        "fixed-register ABI (parent in SCRATCH_REG, child in SCRATCH_REG2); "
        "the registry currently models only the generic CALL_C ABI",
    ),
    "xr_jit_barrier_back": (
        "runtime-stub",
        "GC write barrier (back) emitted as a JIT stub with the same "
        "fixed-register ABI as xr_jit_barrier_fwd",
    ),
}

# Regexes describing the address-taking patterns that count as a "direct
# helper reference" (intentionally aligned with section 5 of
# check_codegen_invariants.sh so the two checks stay in lock-step).
ADDRESS_PATTERNS = (
    re.compile(r"\(uintptr_t\)\s*&?xr_jit_([A-Za-z0-9_]+)"),
    re.compile(r"=\s*\(void\s*\*\)\s*xr_jit_([A-Za-z0-9_]+)"),
    re.compile(r"&xr_jit_([A-Za-z0-9_]+)"),
)

EXCLUDED_FILES = {"xm_helper_table.c"}


def die(msg: str) -> None:
    print(f"check_codegen_helpers: error: {msg}", file=sys.stderr)
    sys.exit(1)


def parse_registered_helpers(helpers_def: Path) -> set[str]:
    """Extract registered helper short names from helpers.def."""
    names: set[str] = set()
    text = helpers_def.read_text()
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"XM_HELPER\(\s*(\w+)\s*,", line)
        if m:
            names.add(m.group(1))
    if not names:
        die(f"no helpers parsed from {helpers_def}")
    return names


def find_address_references(jit_dir: Path) -> list[tuple[str, int, str, str]]:
    """Return list of (relpath, lineno, name, line_text)."""
    refs: list[tuple[str, int, str, str]] = []
    for path in sorted(jit_dir.glob("*.c")):
        if path.name in EXCLUDED_FILES:
            continue
        try:
            text = path.read_text()
        except OSError as e:
            die(f"cannot read {path}: {e}")
        for lineno, line in enumerate(text.splitlines(), 1):
            for pat in ADDRESS_PATTERNS:
                for m in pat.finditer(line):
                    refs.append((path.name, lineno, m.group(1), line.strip()))
    return refs


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="Repository root (defaults to scripts/.. )",
    )
    ns = parser.parse_args()
    root: Path = ns.root.resolve()

    registered = parse_registered_helpers(root / "xisa" / "xm" / "helpers.def")
    refs = find_address_references(root / "src" / "jit")

    seen_non_registered: dict[str, int] = {}
    errors: list[str] = []
    for relname, lineno, name, _ in refs:
        full = f"xr_jit_{name}"
        if name in registered:
            # Section 5 of check_codegen_invariants.sh enforces this direction.
            continue
        if full in NON_REGISTERED_HELPERS:
            seen_non_registered[full] = seen_non_registered.get(full, 0) + 1
            continue
        errors.append(
            f"src/jit/{relname}:{lineno}: bare &{full} reference is not in "
            f"helpers.def and not classified in NON_REGISTERED_HELPERS — "
            f"either register the helper in xisa/xm/helpers.def or add an "
            f"entry to scripts/check_codegen_helpers.py with a rationale."
        )

    # Report classified usage counts.
    print(
        f"check_codegen_helpers: scanned {len(refs)} helper address sites in "
        f"src/jit/*.c (excluding {sorted(EXCLUDED_FILES)})",
        file=sys.stderr,
    )
    for name in sorted(NON_REGISTERED_HELPERS):
        cat, _ = NON_REGISTERED_HELPERS[name]
        count = seen_non_registered.get(name, 0)
        print(
            f"check_codegen_helpers:   {name} [{cat}]: {count} site(s)",
            file=sys.stderr,
        )

    # Surface stale entries so they cannot rot silently.
    stale = [n for n in NON_REGISTERED_HELPERS if n not in seen_non_registered]
    if stale:
        for n in stale:
            errors.append(
                f"NON_REGISTERED_HELPERS lists {n} but no address site was "
                f"found in src/jit/*.c; remove the stale classification."
            )

    if errors:
        for e in errors:
            print(f"check_codegen_helpers: error: {e}", file=sys.stderr)
        print(
            f"check_codegen_helpers: {len(errors)} helper closure error(s)",
            file=sys.stderr,
        )
        return 1

    print(
        "check_codegen_helpers: OK — every direct xr_jit_* reference is "
        "either registered or explicitly classified",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

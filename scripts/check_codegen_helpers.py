#!/usr/bin/env python3
"""check_codegen_helpers.py — enforce xr_jit_* helper address closure.

Every JIT helper whose address is taken in `src/jit/*.c` must either:

  1. Be a registered helper in `xisa/xm/helpers.def`, and must be reached
     through `xm_helper_func(XM_HELPER_<name>)` — this direction is enforced
     by section 5 of `scripts/check_codegen_invariants.sh`.

  2. Or be declared as a runtime stub in `xisa/xm/runtime_stubs.def`, with
     an explicit non-CALL_C ABI contract.

Adding a new bare `&xr_jit_<name>` reference to `src/jit/*.c` therefore has
to either register the helper or declare the runtime-stub ABI in xisa. This
blocks silent growth of the helper-bypass surface that the L2 helper registry
is supposed to own.

`src/jit/xm_helper_table.c` is intentionally excluded because it is the
canonical X-macro expansion site that turns helpers.def into the runtime
function-pointer table.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


def parse_runtime_stubs(runtime_stubs_def: Path) -> dict[str, str]:
    """Extract runtime stub C symbols and ABI names from runtime_stubs.def."""
    stubs: dict[str, str] = {}
    text = runtime_stubs_def.read_text()
    for line_no, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(
            r"XM_RUNTIME_STUB\(\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,",
            line,
        )
        if not m:
            die(f"invalid runtime_stubs.def line {line_no}: {line}")
        _, c_symbol, abi = m.groups()
        if c_symbol in stubs:
            die(f"duplicate runtime stub symbol '{c_symbol}' on line {line_no}")
        stubs[c_symbol] = abi
    if not stubs:
        die(f"no runtime stubs parsed from {runtime_stubs_def}")
    return stubs


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


def find_address_references(paths: list[Path]) -> list[tuple[str, int, str, str]]:
    """Return list of (relpath, lineno, name, line_text)."""
    refs: list[tuple[str, int, str, str]] = []
    for path in sorted(paths):
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
    runtime_stubs = parse_runtime_stubs(root / "xisa" / "xm" / "runtime_stubs.def")
    jit_dir = root / "src" / "jit"
    ref_paths = list(jit_dir.glob("*.c")) + [jit_dir / "xm_runtime_stubs_gen.h"]
    refs = find_address_references(ref_paths)

    seen_runtime_stubs: dict[str, int] = {}
    errors: list[str] = []
    for relname, lineno, name, _ in refs:
        full = f"xr_jit_{name}"
        if name in registered:
            # Section 5 of check_codegen_invariants.sh enforces this direction.
            continue
        if full in runtime_stubs:
            seen_runtime_stubs[full] = seen_runtime_stubs.get(full, 0) + 1
            continue
        errors.append(
            f"src/jit/{relname}:{lineno}: bare &{full} reference is not in "
            f"helpers.def and not declared in xisa/xm/runtime_stubs.def — "
            f"either register the helper or declare the runtime-stub ABI."
        )

    # Report classified usage counts.
    print(
        f"check_codegen_helpers: scanned {len(refs)} helper address sites in "
        f"src/jit codegen files (excluding {sorted(EXCLUDED_FILES)})",
        file=sys.stderr,
    )
    for name in sorted(runtime_stubs):
        abi = runtime_stubs[name]
        count = seen_runtime_stubs.get(name, 0)
        print(
            f"check_codegen_helpers:   {name} [{abi}]: {count} site(s)",
            file=sys.stderr,
        )

    # Surface stale entries so they cannot rot silently.
    stale = [n for n in runtime_stubs if n not in seen_runtime_stubs]
    if stale:
        for n in stale:
            errors.append(
                f"xisa/xm/runtime_stubs.def declares {n} but no address site "
                f"was found in src/jit codegen files; remove the stale declaration."
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
        "either registered or declared as a runtime stub",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

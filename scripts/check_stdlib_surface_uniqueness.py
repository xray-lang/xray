#!/usr/bin/env python3
"""Check stdlib public surfaces do not duplicate the same native helper.

Task 151's R3 rule says each low-level semantic capability should have one
canonical user-facing surface.  This checker enforces the mechanical part for
declarative stdlib metadata: two different public symbols must not bind to the
same VM/AOT helper unless they are an explicitly recorded legacy debt.
"""

from __future__ import annotations

import argparse
import sys
from collections import defaultdict
from pathlib import Path


KNOWN_DUPLICATE_SURFACES = set()


IGNORED_AOT_HELPERS = {
    "",
    "builtin",  # aot_kind=builtin dispatches per symbol, not a shared helper.
}


def load_stdlibgen(root: Path):
    sys.path.insert(0, str((root / "tools" / "stdlibgen").resolve()))
    try:
        import stdlibgen  # type: ignore
    except Exception as exc:  # pragma: no cover - startup diagnostic
        raise SystemExit(f"failed to import tools/stdlibgen/stdlibgen.py: {exc}") from exc
    return stdlibgen


def known_debt_lookup():
    return {(axis, helper, symbols): reason for axis, helper, symbols, reason in KNOWN_DUPLICATE_SURFACES}


def add_module_entry_groups(groups, entries) -> None:
    for entry in entries:
        if entry.is_internal:
            continue
        if entry.vm:
            groups[("vm", entry.vm)].append(entry.symbol)
        if entry.aot_direct and entry.aot not in IGNORED_AOT_HELPERS:
            groups[("aot", entry.aot)].append(entry.symbol)


def add_class_method_groups(groups, class_methods) -> None:
    for method in class_methods:
        symbol = f"{method.module}.{method.class_name}.{method.name}"
        if method.vm:
            groups[("class-vm", method.vm)].append(symbol)


def find_duplicates(groups):
    debts = known_debt_lookup()
    duplicates = []
    known = []
    for (axis, helper), raw_symbols in sorted(groups.items()):
        symbols = frozenset(raw_symbols)
        if len(symbols) <= 1:
            continue
        debt_key = (axis, helper, symbols)
        if debt_key in debts:
            known.append((axis, helper, sorted(symbols), debts[debt_key]))
        else:
            duplicates.append((axis, helper, sorted(symbols)))
    return duplicates, known


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", default=".", help="repository root (default: current directory)")
    ap.add_argument(
        "--list-known",
        action="store_true",
        help="also print currently tolerated duplicate-surface debts",
    )
    args = ap.parse_args()

    root = Path(args.root).resolve()
    stdlibgen = load_stdlibgen(root)
    entries, _constants, _handles, _type_methods, _native_classes, _classes, class_methods, _fields = (
        stdlibgen.parse_def_metadata(root)
    )

    groups: dict[tuple[str, str], list[str]] = defaultdict(list)
    add_module_entry_groups(groups, entries)
    add_class_method_groups(groups, class_methods)
    duplicates, known = find_duplicates(groups)

    if duplicates:
        print("stdlib public surface uniqueness check failed:", file=sys.stderr)
        for axis, helper, symbols in duplicates:
            joined = ", ".join(symbols)
            print(f"  {axis}:{helper} is bound by multiple public symbols: {joined}", file=sys.stderr)
        print(
            "\nEither collapse these into one canonical surface or record a tightly scoped debt in "
            "KNOWN_DUPLICATE_SURFACES.",
            file=sys.stderr,
        )
        return 1

    if args.list_known and known:
        print("Known duplicate-surface debts:")
        for axis, helper, symbols, reason in known:
            print(f"  {axis}:{helper}: {', '.join(symbols)} ({reason})")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

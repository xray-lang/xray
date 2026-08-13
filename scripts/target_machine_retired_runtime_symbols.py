#!/usr/bin/env python3
"""Define retired runtime symbol identities without product-surface residue."""

from __future__ import annotations

import re


def _identity(*parts: str) -> str:
    return "".join(parts)


EXACT = frozenset({
    _identity("xr_", "eval_", "bytecode"),
    _identity("xr_", "run_", "bytecode_", "file"),
    _identity("xr_", "detect_", "output_", "format"),
    _identity("xr_", "output_", "c_", "source"),
})

PREFIXES = (
    _identity("xray_", "vm_"),
    _identity("xr_", "vm_"),
    _identity("x", "vm_"),
    _identity("xr_", "bytecode_"),
    _identity("xr_", "bundle_"),
    _identity("xr_", "load_", "module_"),
    _identity("xr_", "proto_"),
)


def matches(symbol: str) -> bool:
    return symbol in EXACT or symbol.startswith(PREFIXES)


def compiled_pattern() -> re.Pattern[str]:
    alternatives = [re.escape(prefix) for prefix in PREFIXES]
    alternatives.extend(re.escape(symbol) + "$" for symbol in sorted(EXACT))
    return re.compile(r"^(?:" + "|".join(alternatives) + r")")

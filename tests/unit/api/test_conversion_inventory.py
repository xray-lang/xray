#!/usr/bin/env python3
"""Cross-platform gate for the source-aware numeric conversion inventory."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path


def run(xray: Path, source: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(xray), "language", "conversions", "--json", str(source)],
        text=True,
        encoding="utf-8",
        errors="strict",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_conversion_inventory.py XRAY FIXTURE", file=sys.stderr)
        return 2
    xray = Path(sys.argv[1]).resolve()
    fixture = Path(sys.argv[2]).resolve()

    first = run(xray, fixture)
    second = run(xray, fixture)
    if first.returncode != 0 or second.returncode != 0:
        print(first.stderr or second.stderr, file=sys.stderr)
        return 1
    if first.stdout != second.stdout:
        print("conversion inventory is not deterministic", file=sys.stderr)
        return 1

    payload = json.loads(first.stdout)
    required_kinds = {
        "contextual_literal",
        "lossless_widen",
        "explicit_truncate",
        "explicit_sign_change",
        "explicit_int_float",
    }
    if (payload.get("schema_version") != 1 or payload.get("unresolved") != 0 or
            payload.get("file_count") != len(payload.get("files", []))):
        print(f"invalid inventory header: {payload}", file=sys.stderr)
        return 1
    if not required_kinds.issubset(payload.get("kinds", {})):
        missing = required_kinds - set(payload.get("kinds", {}))
        print(f"missing conversion kinds: {missing}", file=sys.stderr)
        return 1
    records = payload.get("conversions", [])
    if payload.get("count") != len(records) or not records:
        print("inventory count does not match records", file=sys.stderr)
        return 1
    if any(record.get("syntax") == "Unknown" for record in records):
        print("inventory contains an unclassified AST syntax", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="xray-conversion-inventory-") as tmp:
        invalid = Path(tmp) / "invalid.xr"
        invalid.write_text("var value: u8 = 256\n", encoding="utf-8")
        rejected = run(xray, invalid)
        if rejected.returncode == 0 or rejected.stdout.strip():
            print("invalid source must fail closed without emitting an inventory", file=sys.stderr)
            return 1

    print(f"conversion inventory: PASS ({len(records)} classified records)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

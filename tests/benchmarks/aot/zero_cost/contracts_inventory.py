#!/usr/bin/env python3
"""Validate the proof-carrying zero-cost contract registry as one strict schema.

Every row must name its evidence: which fixtures prove it, which shape and
performance gates enforce it, and who owns it. The schema is checked strictly
rather than leniently because a half-filled row is worse than a missing one --
it looks like a contract has evidence when it does not.

Usage: contracts_inventory.py [registry.tsv]
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_REGISTRY = SCRIPT_DIR / "contracts.tsv"

COLUMNS = (
    "contract_id", "semantic_condition", "required_evidence", "allowed_reps",
    "forbidden_residue", "positive_fixture", "negative_fixture",
    "corruption_fixture", "shape_gate", "performance_gate", "owner_task", "status",
)
VALID_STATUS = ("baseline-gap", "partial", "verified")
ID_RE = re.compile(r"^[a-z][a-z0-9_]*$")


def main(argv: list[str]) -> int:
    registry = Path(argv[1]) if len(argv) > 1 else DEFAULT_REGISTRY
    if not registry.is_file():
        sys.stderr.write(f"zero-cost contracts: missing registry: {registry}\n")
        return 1

    lines = registry.read_text(encoding="utf-8").splitlines()
    failures = 0
    seen = set()
    rows = 0
    status_counts = {name: 0 for name in VALID_STATUS}

    if not lines or lines[0].split("\t") != list(COLUMNS):
        sys.stderr.write("zero-cost contracts: invalid header\n")
        failures += 1

    for number, line in enumerate(lines[1:], start=2):
        fields = line.split("\t")
        if len(fields) != len(COLUMNS):
            sys.stderr.write(
                f"zero-cost contracts: line {number} has {len(fields)} columns, "
                f"expected {len(COLUMNS)}\n")
            failures += 1
            continue

        contract_id = fields[0]
        if not ID_RE.match(contract_id) or contract_id in seen:
            sys.stderr.write(
                f"zero-cost contracts: invalid or duplicate id at line {number}: "
                f"{contract_id}\n")
            failures += 1
        seen.add(contract_id)

        # Every column after the id must carry something: an empty evidence or
        # gate cell is the shape this registry exists to forbid.
        for index in range(1, len(COLUMNS)):
            if fields[index] == "":
                sys.stderr.write(
                    f"zero-cost contracts: empty field {index + 1} at line {number}\n")
                failures += 1

        status = fields[-1]
        if status not in VALID_STATUS:
            sys.stderr.write(
                f"zero-cost contracts: invalid status at line {number}: {status}\n")
            failures += 1
        else:
            status_counts[status] += 1
        rows += 1

    if rows == 0:
        sys.stderr.write("zero-cost contracts: registry is empty\n")
        failures += 1

    print(f"zero-cost contracts: {rows} rows, {status_counts['verified']} verified, "
          f"{status_counts['partial']} partial, "
          f"{status_counts['baseline-gap']} baseline gaps")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

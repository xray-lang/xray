#!/bin/bash
# Validate the proof-carrying zero-cost contract registry as one strict schema.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd -P)"
REGISTRY="${1:-$SCRIPT_DIR/contracts.tsv}"

[ -f "$REGISTRY" ] || {
    echo "zero-cost contracts: missing registry: $REGISTRY" >&2
    exit 1
}

awk -F '\t' '
    BEGIN {
        expected = "contract_id\tsemantic_condition\trequired_evidence\tallowed_reps\tforbidden_residue\tpositive_fixture\tnegative_fixture\tcorruption_fixture\tshape_gate\tperformance_gate\towner_task\tstatus"
        failures = 0
    }
    NR == 1 {
        if ($0 != expected) {
            print "zero-cost contracts: invalid header" > "/dev/stderr"
            failures++
        }
        next
    }
    {
        if (NF != 12) {
            printf "zero-cost contracts: line %d has %d columns, expected 12\n", NR, NF > "/dev/stderr"
            failures++
            next
        }
        if ($1 !~ /^[a-z][a-z0-9_]*$/ || seen[$1]++) {
            printf "zero-cost contracts: invalid or duplicate id at line %d: %s\n", NR, $1 > "/dev/stderr"
            failures++
        }
        for (i = 2; i <= 12; i++) {
            if ($i == "") {
                printf "zero-cost contracts: empty field %d at line %d\n", i, NR > "/dev/stderr"
                failures++
            }
        }
        if ($12 != "baseline-gap" && $12 != "partial" && $12 != "verified") {
            printf "zero-cost contracts: invalid status at line %d: %s\n", NR, $12 > "/dev/stderr"
            failures++
        }
        rows++
        status[$12]++
    }
    END {
        if (rows == 0) {
            print "zero-cost contracts: registry is empty" > "/dev/stderr"
            failures++
        }
        printf "zero-cost contracts: %d rows, %d verified, %d partial, %d baseline gaps\n", rows, status["verified"], status["partial"], status["baseline-gap"]
        exit failures != 0
    }
' "$REGISTRY"

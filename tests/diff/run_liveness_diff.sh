#!/bin/bash
# run_liveness_diff.sh — VM/AOT liveness differential (task 260 §8 F.1).
#
# The ordinary differential net compares the byte-for-byte output of programs
# that TERMINATE — which is exactly why a progress divergence (VM finishes,
# AOT spins forever) survived thirteen months without a test being red.
# This runner closes that class: every case must produce its expected
# observable ON BOTH BACKENDS within a wall-clock budget.
#
# Case layout: tests/diff/cases/liveness/<name>.xr with a sidecar
# tests/diff/cases/liveness/<name>.live:
#
#   timeout=SECONDS          wall-clock budget per backend (required)
#   exit=N | exit=nonzero    expected exit code (default: 0)
#   contains=SUBSTRING       required in the backend's combined output
#                            (repeatable; checked on both backends)
#   vm_only=1                skip the AOT half (documented reason required
#                            in the case header) — use sparingly.
#
# A backend that is still running when the budget expires FAILS the case:
# that is the whole point.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
CASE_DIR="$SCRIPT_DIR/cases/liveness"
XRAY="${XRAY_BIN:-$PROJECT_DIR/build/xray}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_liveness.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

TIMEOUT_BIN="$(command -v timeout || command -v gtimeout || true)"
if [ -z "$TIMEOUT_BIN" ]; then
    echo "SKIP: no timeout(1) available"
    exit 0
fi
if [ ! -x "$XRAY" ]; then
    echo "SKIP: xray binary not found: $XRAY"
    exit 0
fi

PASS=0
FAIL=0

read_conf() { # $1=conf $2=key -> prints values (one per line)
    grep "^$2=" "$1" | sed "s/^$2=//"
}

check_output() { # $1=label $2=exit_got $3=exit_want $4=out_file $5=conf
    local label="$1" got="$2" want="$3" out="$4" conf="$5"
    if [ "$got" -eq 124 ] || [ "$got" -eq 137 ]; then
        echo "FAIL ($label: no progress within budget — killed by timeout)"
        return 1
    fi
    if [ "$want" = "nonzero" ]; then
        if [ "$got" -eq 0 ]; then
            echo "FAIL ($label: expected nonzero exit, got 0)"
            return 1
        fi
    elif [ "$got" -ne "$want" ]; then
        echo "FAIL ($label: exit $got, expected $want)"
        return 1
    fi
    local needle
    while IFS= read -r needle; do
        [ -z "$needle" ] && continue
        if ! grep -qF -- "$needle" "$out"; then
            echo "FAIL ($label: output missing: $needle)"
            return 1
        fi
    done <<EOF
$(read_conf "$conf" contains)
EOF
    return 0
}

echo "=== liveness differential ==="
echo "Binary: $XRAY"

for xr in "$CASE_DIR"/*.xr; do
    [ -f "$xr" ] || continue
    base="$(basename "$xr" .xr)"
    conf="${xr%.xr}.live"
    printf '  %-44s' "$base"
    if [ ! -f "$conf" ]; then
        echo "FAIL (missing $base.live sidecar)"
        FAIL=$((FAIL + 1))
        continue
    fi
    budget="$(read_conf "$conf" timeout | head -1)"
    want_exit="$(read_conf "$conf" exit | head -1)"
    [ -z "$want_exit" ] && want_exit=0
    if [ -z "$budget" ]; then
        echo "FAIL (sidecar has no timeout=)"
        FAIL=$((FAIL + 1))
        continue
    fi

    vm_out="$WORK/${base}.vm.out"
    "$TIMEOUT_BIN" "$budget" "$XRAY" run "$xr" >"$vm_out" 2>&1
    vm_rc=$?
    if ! msg="$(check_output vm "$vm_rc" "$want_exit" "$vm_out" "$conf")"; then
        echo "$msg"
        sed 's/^/      | /' "$vm_out" | head -6
        FAIL=$((FAIL + 1))
        continue
    fi

    if [ "$(read_conf "$conf" vm_only | head -1)" = "1" ]; then
        echo "PASS (vm only)"
        PASS=$((PASS + 1))
        continue
    fi

    bin="$WORK/${base}.bin"
    if ! (cd "$WORK" && "$XRAY" build --native "$xr" -o "$bin") >"$WORK/${base}.build" 2>&1; then
        echo "FAIL (aot build failed)"
        sed 's/^/      | /' "$WORK/${base}.build" | tail -4
        FAIL=$((FAIL + 1))
        continue
    fi
    aot_out="$WORK/${base}.aot.out"
    "$TIMEOUT_BIN" "$budget" "$bin" >"$aot_out" 2>&1
    aot_rc=$?
    if ! msg="$(check_output aot "$aot_rc" "$want_exit" "$aot_out" "$conf")"; then
        echo "$msg"
        sed 's/^/      | /' "$aot_out" | head -6
        FAIL=$((FAIL + 1))
        continue
    fi

    echo "PASS"
    PASS=$((PASS + 1))
done

echo ""
if [ "$FAIL" -eq 0 ]; then
    echo "VERDICT: PASS  ($PASS cases)"
    exit 0
fi
echo "VERDICT: FAIL  ($PASS passed, $FAIL failed)"
exit 1

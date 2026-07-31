#!/bin/bash
# run_litmus.sh - concurrency memory-model litmus tests (spec §16.9).
#
# Each case runs a classic shared-memory shape many times and prints
# "<name> forbidden=<count>". A non-zero count is a memory-model violation.
#
# Usage:
#   tests/concurrency_litmus/run_litmus.sh [xray_binary]
#
# Environment:
#   XRAY_BIN                xray binary (default: build/xray)
#   XRAY_LITMUS_BACKENDS    comma list subset of vm,aot (default: vm,aot)
#   XRAY_LITMUS_OPT         AOT optimization level (default: 2)
#   XRAY_LITMUS_TIMEOUT     per-case seconds (default: 300)
#
# The AOT backend is the one that matters. The VM is an interpreter and does
# not reorder, so a litmus run against it alone proves nothing about the
# memory model; every failure mode these cases exist to catch lives in the Xi
# optimiser, which only runs from -O2 (XI_OPT_FULL). The VM lane is kept as a
# control: if a case fails there too, the bug is in the runtime or the test,
# not in code motion.
#
# Exit: 0 when every enabled backend reports forbidden=0 for every case.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

XRAY="${1:-${XRAY_BIN:-$REPO_ROOT/build/xray}}"
BACKENDS="${XRAY_LITMUS_BACKENDS:-vm,aot}"
OPT="${XRAY_LITMUS_OPT:-2}"
CASE_TIMEOUT="${XRAY_LITMUS_TIMEOUT:-300}"

if [ ! -x "$XRAY" ]; then
    echo "run_litmus: xray binary not found or not executable: $XRAY" >&2
    exit 1
fi

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

backend_enabled() {
    case ",$BACKENDS," in
        *",$1,"*) return 0 ;;
        *) return 1 ;;
    esac
}

# Prints the case's stdout, or a diagnostic line starting with '!' on failure.
run_vm() {
    local case_file="$1"
    if ! out="$("$XRAY" run "$case_file" 2>&1)"; then
        echo "! vm run failed: $out"
        return 1
    fi
    echo "$out"
}

run_aot() {
    local case_file="$1"
    local bin="$WORK_DIR/$(basename "$case_file" .xr).aot"
    if ! build_out="$("$XRAY" build --native -O "$OPT" "$case_file" -o "$bin" 2>&1)"; then
        echo "! aot build failed: $build_out"
        return 1
    fi
    if ! out="$("$bin" 2>&1)"; then
        echo "! aot run failed: $out"
        return 1
    fi
    echo "$out"
}

failures=0
total=0

for case_file in "$SCRIPT_DIR"/*.xr; do
    [ -e "$case_file" ] || continue
    name="$(basename "$case_file" .xr)"
    for backend in vm aot; do
        backend_enabled "$backend" || continue
        total=$((total + 1))
        printf '  %-32s %-4s ' "$name" "$backend"
        if output="$(run_$backend "$case_file")"; then
            # Every case prints "<name> forbidden=<count>" as its last line.
            summary="$(printf '%s\n' "$output" | tail -1)"
            count="${summary##*forbidden=}"
            case "$summary" in
                *forbidden=*)
                    if [ "$count" = "0" ]; then
                        echo "PASS"
                    else
                        echo "FAIL ($count forbidden observations)"
                        failures=$((failures + 1))
                    fi
                    ;;
                *)
                    echo "FAIL (no forbidden= summary; got: $summary)"
                    failures=$((failures + 1))
                    ;;
            esac
        else
            echo "FAIL"
            printf '        %s\n' "$output"
            failures=$((failures + 1))
        fi
    done
done

echo
echo "AOT opt: -O$OPT"
echo "=== Litmus: $((total - failures)) passed, $failures failed ==="
[ "$failures" -eq 0 ]

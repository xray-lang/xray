#!/usr/bin/env bash
#
# run_fuzz_30min.sh - fixed-duration JIT differential fuzz driver
#
# Usage: scripts/run_fuzz_30min.sh [seed]
#
# Wraps scripts/jit_fuzz.sh in a wall-clock budget so nightly CI
# always spends a known amount of compute looking for JIT bugs.
# Iterations are dynamically chunked so the script returns shortly
# after the budget elapses; failing seeds remain in tests/tmp/jit_fuzz
# regardless of when the budget expires.
#
# Environment:
#   FUZZ_DURATION_SEC - override the default 1800s budget
#   FUZZ_CHUNK        - iterations per inner jit_fuzz.sh invocation (default 200)
#   XRAY_BIN          - xray binary path (forwarded to jit_fuzz.sh)
#
# Exit codes:
#   0  - budget elapsed, no diff/crash captured
#   1  - at least one diff or crash captured (artifact under tests/tmp/jit_fuzz)
#   2  - infrastructure error (binary missing, jit_fuzz.sh missing, etc.)
#

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

INNER="${SCRIPT_DIR}/jit_fuzz.sh"
if [ ! -x "$INNER" ] && [ ! -r "$INNER" ]; then
    echo "FAIL: $INNER not found" >&2
    exit 2
fi

DURATION="${FUZZ_DURATION_SEC:-1800}"
CHUNK="${FUZZ_CHUNK:-200}"
SEED_BASE="${1:-$(date +%s)}"

# Sanity-check seed and chunk values to avoid runaway loops.
case "$DURATION" in ''|*[!0-9]*) echo "FAIL: FUZZ_DURATION_SEC must be integer" >&2; exit 2;; esac
case "$CHUNK"    in ''|*[!0-9]*) echo "FAIL: FUZZ_CHUNK must be integer" >&2; exit 2;; esac
case "$SEED_BASE" in ''|*[!0-9]*) echo "FAIL: seed must be integer" >&2; exit 2;; esac

OUT_DIR="${PROJECT_ROOT}/tests/tmp/jit_fuzz"
mkdir -p "$OUT_DIR"

start=$(date +%s)
deadline=$((start + DURATION))
chunk_idx=0
total_iter=0
crash_total=0
diff_total=0
fail=0

echo "fuzz: budget=${DURATION}s chunk=${CHUNK} seed_base=${SEED_BASE}"
echo "fuzz: artifacts go to ${OUT_DIR}"

while :; do
    now=$(date +%s)
    if [ "$now" -ge "$deadline" ]; then
        break
    fi
    chunk_idx=$((chunk_idx + 1))
    seed=$((SEED_BASE + chunk_idx))

    # Spawn one fuzz chunk. Keep its raw output for forensic logs but
    # parse the summary lines for a numeric roll-up.
    chunk_log="${OUT_DIR}/chunk_${chunk_idx}.log"
    bash "$INNER" -n "$CHUNK" -s "$seed" -o "$OUT_DIR" >"$chunk_log" 2>&1 || rc_chunk=$?
    rc_chunk="${rc_chunk:-0}"

    # The inner script prints lines of the form `Crash:    N` and
    # `Diff:     N`; these are the authoritative bug counters.
    crash=$(awk '/^Crash:/ {print $2}'  "$chunk_log" | tail -n 1)
    diff=$( awk '/^Diff:/  {print $2}'  "$chunk_log" | tail -n 1)
    crash="${crash:-0}"
    diff="${diff:-0}"

    crash_total=$((crash_total + crash))
    diff_total=$((diff_total + diff))
    total_iter=$((total_iter + CHUNK))

    if [ "$crash" -gt 0 ] || [ "$diff" -gt 0 ]; then
        fail=1
    fi

    elapsed=$((now - start))
    echo "  chunk ${chunk_idx}: seed=${seed} iters=${CHUNK} crash=${crash} diff=${diff} elapsed=${elapsed}s"

    # Strip the inner success summary log on clean chunks to keep the
    # artifact dir small; failures keep the whole thing for triage.
    if [ "$crash" -eq 0 ] && [ "$diff" -eq 0 ]; then
        rm -f "$chunk_log"
    fi
done

elapsed=$(( $(date +%s) - start ))
echo
echo "fuzz: budget=${DURATION}s actual=${elapsed}s chunks=${chunk_idx} iters=${total_iter}"
echo "fuzz: crash_total=${crash_total} diff_total=${diff_total}"
if [ "$fail" -ne 0 ]; then
    echo "fuzz: FAIL — see ${OUT_DIR} for repro inputs and chunk logs"
    exit 1
fi
echo "fuzz: OK — no diff/crash within budget"
exit 0

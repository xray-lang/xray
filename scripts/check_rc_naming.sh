#!/usr/bin/env bash
# check_rc_naming.sh -- detect legacy GC terminology in active RC runtime code.
#
# Default mode is strict and fails on any legacy active name. During the
# migration, run with --allow-current to print the current distribution while
# keeping the command usable as a baseline gate.

set -euo pipefail

cd "$(dirname "$0")/.."

ALLOW_CURRENT=0
SHOW_HELP=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --allow-current)
            ALLOW_CURRENT=1
            ;;
        -h|--help)
            SHOW_HELP=1
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 2
            ;;
    esac
    shift
done

if [ "$SHOW_HELP" -eq 1 ]; then
    cat <<'USAGE'
Usage: scripts/check_rc_naming.sh [--allow-current]

Scans active source/test files for legacy GC names that should disappear as
Xray converges on RC/object/heap/cycle-collector terminology.

Options:
  --allow-current   Print matches and exit 0 even if legacy names remain.
                   Use only for migration baselines.
USAGE
    exit 0
fi

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[0;33m'
    CYAN='\033[0;36m'
    RESET='\033[0m'
else
    RED=''
    GREEN=''
    YELLOW=''
    CYAN=''
    RESET=''
fi

PATTERN='\bXrGCHeader\b|\bXrCoroGC\b|\bXrGC\b|xr_gc_|xr_coro_gc_|g_type_ops|XR_GC_|XGC_|GC_BARRIER'
SCAN_DIRS=(src include stdlib tests)
INCLUDES=(
    --include='*.c'
    --include='*.h'
    --include='*.inc.c'
    --include='*.xr'
)

echo "=== RC naming convergence check ==="
echo "pattern: ${PATTERN}"
echo "dirs: ${SCAN_DIRS[*]}"
echo

matches=$(grep -RInE "${INCLUDES[@]}" "${PATTERN}" "${SCAN_DIRS[@]}" 2>/dev/null || true)

if [ -z "$matches" ]; then
    echo -e "${GREEN}OK${RESET}: no legacy GC active names found."
    exit 0
fi

hit_count=$(printf '%s\n' "$matches" | wc -l | tr -d ' ')

echo -e "${CYAN}Legacy-name hits by token:${RESET}"
for token in XrGCHeader XrCoroGC XrGC xr_gc_ xr_coro_gc_ g_type_ops XR_GC_ XGC_ GC_BARRIER; do
    count=$(printf '%s\n' "$matches" | grep -o "$token" | wc -l | tr -d ' ' || true)
    if [ "$count" -gt 0 ] 2>/dev/null; then
        printf '  %-18s %s\n' "$token" "$count"
    fi
done

echo
echo -e "${CYAN}First matches:${RESET}"
printf '%s\n' "$matches" | awk 'NR <= 80 { print }'
if [ "$hit_count" -gt 80 ]; then
    echo "... (${hit_count} total matches, truncated)"
fi

echo
if [ "$ALLOW_CURRENT" -eq 1 ]; then
    echo -e "${YELLOW}ALLOW-CURRENT${RESET}: ${hit_count} legacy-name hit(s) remain."
    exit 0
fi

echo -e "${RED}FAIL${RESET}: ${hit_count} legacy-name hit(s) remain."
echo "Run with --allow-current only while recording a migration baseline."
exit 1

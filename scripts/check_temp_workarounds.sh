#!/usr/bin/env bash
# ============================================================================
# check_temp_workarounds.sh
# ----------------------------------------------------------------------------
# Reconciles `DEFENSIVE-TEMP[NNN]` tags in source against the
# table in `tests/known_temp_workarounds.md`.
#
# The table lives under `tests/` (not `docs/`) because the `docs/`
# tree is intentionally untracked (.git/info/exclude). This file is
# a CI-enforced engineering contract, peer to `tests/known_failures.txt`
# and `tests/baseline_silent_fallback.txt`.
#
# Two-way check (both directions are mandatory; one-way checks let
# rot accumulate silently):
#
#   1. Tag without row -> FAIL
#      Every source line of the form
#          // DEFENSIVE-TEMP[NNN]: <summary>.
#          //   Tracking row "<id>" in tests/known_temp_workarounds.md.
#      must have a row whose `Tag` column equals `<id>`.
#
#   2. Row without tag -> FAIL
#      Every row in the table must have at least one matching
#      `Tracking row "<id>"` reference somewhere under `src/`.
#
# Additional structural checks:
#
#   3. Each table row must fill all eight columns (no blank cells).
#   4. Each tag block must include the `Tracking row "..."` line on
#      the line immediately following the `// DEFENSIVE-TEMP[NNN]:`
#      header. A header with no tracking line is a malformed tag.
#
# Exit codes:
#   0  all checks passed
#   1  at least one check failed
#
# Cross-platform: pure POSIX + grep -E + awk. Tested on Linux, macOS,
# Git Bash. Run from repo root or any subdirectory.
# ============================================================================

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

TABLE_FILE="tests/known_temp_workarounds.md"
SCAN_DIR="src"
EXIT=0

# ----------------------------------------------------------------------------
# helpers
# ----------------------------------------------------------------------------
red()    { printf '\033[31m%s\033[0m\n' "$*"; }
green()  { printf '\033[32m%s\033[0m\n' "$*"; }
yellow() { printf '\033[33m%s\033[0m\n' "$*"; }
section() { printf '\n=== %s ===\n' "$*"; }

# ----------------------------------------------------------------------------
# Step 0: table file present
# ----------------------------------------------------------------------------
if [ ! -f "${TABLE_FILE}" ]; then
    red "FAIL: ${TABLE_FILE} is missing."
    exit 1
fi

# ----------------------------------------------------------------------------
# Extract IDs from the markdown table.
#
# A valid table row is exactly nine pipe-delimited fields:
#   |<empty>|Tag|Location|Commit|Root cause|Removal|Owner|Date|Target|<empty>|
#
# We skip the header row, the alignment row, and rows with fewer fields.
# ----------------------------------------------------------------------------
section "table parse"

table_ids=$(awk -F'|' '
    /^\|/ {
        # Trim each field.
        for (i = 1; i <= NF; i++) {
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", $i)
        }
        # Skip the header row (`Tag` literal in column 2) and the
        # alignment row (column 2 starts with `---`).
        if ($2 == "Tag") next
        if ($2 ~ /^-+$/) next
        # A real row has 10 fields (leading + 8 cells + trailing).
        if (NF < 10) next
        if ($2 == "") next
        print $2
    }
' "${TABLE_FILE}" | sort -u)

if [ -z "${table_ids}" ]; then
    red "FAIL: ${TABLE_FILE} has no data rows."
    EXIT=1
else
    table_count=$(printf '%s\n' "${table_ids}" | wc -l | tr -d ' ')
    green "Parsed ${table_count} row(s) from ${TABLE_FILE}."
fi

# ----------------------------------------------------------------------------
# Step 1: every row must have all eight cells filled.
# ----------------------------------------------------------------------------
section "row completeness"

empty_cells=$(awk -F'|' '
    /^\|/ {
        for (i = 1; i <= NF; i++) {
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", $i)
        }
        if ($2 == "Tag") next
        if ($2 ~ /^-+$/) next
        if (NF < 10) next
        if ($2 == "") next
        # Cells 2..9 are the eight required fields.
        for (i = 2; i <= 9; i++) {
            if ($i == "") {
                printf("row \"%s\" has empty cell #%d\n", $2, i - 1)
            }
        }
    }
' "${TABLE_FILE}")

if [ -n "${empty_cells}" ]; then
    red "FAIL: empty cells in table:"
    printf '%s\n' "${empty_cells}" | sed 's/^/  /'
    EXIT=1
else
    green "OK: every row has all eight cells filled."
fi

# ----------------------------------------------------------------------------
# Step 2: collect every `Tracking row "..."` reference under SCAN_DIR.
# ----------------------------------------------------------------------------
section "tag collection"

# grep prints `<file>:<line>:<text>`; awk extracts the quoted ID.
tag_refs=$(grep -RnoE 'Tracking row "[^"]+"' --include='*.c' --include='*.h' "${SCAN_DIR}" 2>/dev/null \
    | awk -F'"' '{ print $2 }' \
    | sort -u)

if [ -z "${tag_refs}" ]; then
    yellow "Note: no DEFENSIVE-TEMP tracking refs found under ${SCAN_DIR}."
fi

# ----------------------------------------------------------------------------
# Step 3: each DEFENSIVE-TEMP block must include a tracking line.
#
# Walk every .c/.h under SCAN_DIR; whenever a line matches
# `DEFENSIVE-TEMP[NNN]:`, the next non-blank line must be the
# `Tracking row "..."` reference.
# ----------------------------------------------------------------------------
section "tag block shape"

malformed=$(find "${SCAN_DIR}" -type f \( -name '*.c' -o -name '*.h' \) 2>/dev/null \
    | sort \
    | while read -r f; do
        # Two-pass scan via END so lines[N+1] is always populated by
        # the time we evaluate it. The standard block format places the
        # `Tracking row "..."` reference on the line immediately after
        # the `DEFENSIVE-TEMP[...]` header.
        awk -v file="${f}" '
            { lines[NR] = $0 }
            END {
                for (i = 1; i <= NR; i++) {
                    if (lines[i] ~ /DEFENSIVE-TEMP\[[0-9A-Za-z_-]+\]:/) {
                        if (lines[i + 1] !~ /Tracking row "[^"]+"/) {
                            printf("%s:%d  missing `Tracking row \"...\"` on line %d\n", \
                                   file, i, i + 1)
                        }
                    }
                }
            }
        ' "${f}"
    done)

if [ -n "${malformed}" ]; then
    red "FAIL: malformed DEFENSIVE-TEMP block(s):"
    printf '%s\n' "${malformed}" | sed 's/^/  /'
    EXIT=1
else
    green "OK: every DEFENSIVE-TEMP header has a tracking line."
fi

# ----------------------------------------------------------------------------
# Step 4: every tag id must appear in the table.
# ----------------------------------------------------------------------------
section "tag -> row reconciliation"

if [ -n "${tag_refs}" ]; then
    orphan_tags=$(comm -23 <(printf '%s\n' "${tag_refs}") <(printf '%s\n' "${table_ids}"))
    if [ -n "${orphan_tags}" ]; then
        red "FAIL: source tags without a matching row in ${TABLE_FILE}:"
        printf '%s\n' "${orphan_tags}" | sed 's/^/  /'
        echo
        echo "  Add the missing row(s) to ${TABLE_FILE} with all eight columns,"
        echo "  or correct the tag id in source."
        EXIT=1
    else
        green "OK: every source tag has a matching table row."
    fi
fi

# ----------------------------------------------------------------------------
# Step 5: every table row id must appear in source.
# ----------------------------------------------------------------------------
section "row -> tag reconciliation"

orphan_rows=$(comm -13 <(printf '%s\n' "${tag_refs}") <(printf '%s\n' "${table_ids}"))
if [ -n "${orphan_rows}" ]; then
    red "FAIL: table rows without a matching source tag:"
    printf '%s\n' "${orphan_rows}" | sed 's/^/  /'
    echo
    echo "  Either re-add the DEFENSIVE-TEMP tag in source, or remove the"
    echo "  stale row from ${TABLE_FILE}."
    EXIT=1
else
    green "OK: every table row has a matching source tag."
fi

# ----------------------------------------------------------------------------
# summary
# ----------------------------------------------------------------------------
section "summary"
if [ ${EXIT} -eq 0 ]; then
    green "DEFENSIVE-TEMP reconciliation: all checks passed."
else
    red "DEFENSIVE-TEMP reconciliation: one or more checks failed."
fi
exit ${EXIT}

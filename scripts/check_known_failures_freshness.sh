#!/usr/bin/env bash
# ============================================================================
# check_known_failures_freshness.sh
# ----------------------------------------------------------------------------
# Reverse invariant E.4: every line in tests/known_failures.txt that
# represents a temporarily disabled test must carry an ADDED=<date>
# field, and that date must be <= 30 days old. Older entries fail CI
# unconditionally, forcing the owner to either fix the root cause or
# re-justify the suppression.
#
# Line format (one per disabled test):
#   <test path>  ISSUE=<url>  ADDED=YYYY-MM-DD  OWNER=<email>
#
# Comment lines (starting with #) and blank lines are ignored.
#
# Exit codes:
#   0   no expired entries
#   1   at least one expired entry, malformed line, or missing field
# ============================================================================

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

FILE="tests/known_failures.txt"
MAX_AGE_DAYS=30
EXIT=0

red()    { printf '\033[31m%s\033[0m\n' "$*"; }
green()  { printf '\033[32m%s\033[0m\n' "$*"; }
yellow() { printf '\033[33m%s\033[0m\n' "$*"; }

if [ ! -f "${FILE}" ]; then
    red "ERR: ${FILE} missing."
    exit 1
fi

# Portable epoch-from-date converter for both GNU and BSD date.
date_to_epoch() {
    local d="$1"
    # GNU date
    if date -d "${d}" +%s >/dev/null 2>&1; then
        date -d "${d}" +%s
        return
    fi
    # BSD date (macOS)
    if date -j -f '%Y-%m-%d' "${d}" +%s >/dev/null 2>&1; then
        date -j -f '%Y-%m-%d' "${d}" +%s
        return
    fi
    echo ""  # unsupported
}

today_epoch=$(date +%s)

line_no=0
expired_count=0
malformed_count=0
total_count=0

while IFS= read -r line || [ -n "${line}" ]; do
    line_no=$((line_no + 1))

    # skip comments and blanks
    case "${line}" in
        ''|\#*) continue ;;
    esac

    total_count=$((total_count + 1))

    # Must contain ISSUE=, ADDED=, OWNER=
    if ! echo "${line}" | grep -q 'ISSUE='; then
        red "  ${FILE}:${line_no}: missing ISSUE= field"
        malformed_count=$((malformed_count + 1))
        continue
    fi
    if ! echo "${line}" | grep -q 'OWNER='; then
        red "  ${FILE}:${line_no}: missing OWNER= field"
        malformed_count=$((malformed_count + 1))
        continue
    fi

    added_value=$(echo "${line}" | sed -n 's/.*ADDED=\([0-9-]*\).*/\1/p')
    if [ -z "${added_value}" ]; then
        red "  ${FILE}:${line_no}: missing ADDED=YYYY-MM-DD field"
        malformed_count=$((malformed_count + 1))
        continue
    fi

    added_epoch=$(date_to_epoch "${added_value}")
    if [ -z "${added_epoch}" ]; then
        red "  ${FILE}:${line_no}: unparseable date '${added_value}'"
        malformed_count=$((malformed_count + 1))
        continue
    fi

    age_days=$(( (today_epoch - added_epoch) / 86400 ))
    if [ ${age_days} -gt ${MAX_AGE_DAYS} ]; then
        red "  ${FILE}:${line_no}: entry is ${age_days} days old (limit ${MAX_AGE_DAYS}):"
        echo "      ${line}"
        expired_count=$((expired_count + 1))
    fi
done < "${FILE}"

if [ ${malformed_count} -gt 0 ]; then EXIT=1; fi
if [ ${expired_count} -gt 0 ]; then EXIT=1; fi

if [ ${EXIT} -eq 0 ]; then
    if [ ${total_count} -eq 0 ]; then
        green "OK: ${FILE} contains 0 active suppressions."
    else
        green "OK: ${total_count} active suppression(s), all <= ${MAX_AGE_DAYS} days old."
    fi
else
    red "${expired_count} expired, ${malformed_count} malformed entries."
    echo
    echo "  Either fix the underlying test or re-justify with a new ADDED= date."
    echo "  Permanent suppressions are not allowed."
fi

exit ${EXIT}

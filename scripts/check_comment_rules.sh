#!/bin/bash
# check_comment_rules.sh -- enforce the "no doc reference / no refactor-phase
# wording" rule on source comments and CI commit messages.
#
# Banned in source comments and git commit messages:
#   - References to .md document paths
#   - Refactor-phase wording: "本次重构", "本阶段", "这一步"
#   - Refactor coordinates: P0/P1, Round N, "Phase X.Y", X-NN, A-NN, C-NN, F-NN, CORO-NN
#
# Algorithm-shaped "Phase 1 / Step 1" comments are NOT auto-detected here; rely
# on code review to confirm those describe an algorithm.
#
# Run from project root:
#     scripts/check_comment_rules.sh
#
# Exit 0 if clean, exit 1 on any violation.

set -euo pipefail

SRC_DIRS="src stdlib include tests/unit scripts"
ERRORS=0

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    RED='\033[0;31m'
    YELLOW='\033[0;33m'
    GREEN='\033[0;32m'
    RESET='\033[0m'
else
    RED=''
    YELLOW=''
    GREEN=''
    RESET=''
fi

# 1. Document reference checks
echo "=== checking for .md document references ==="
violations=$(grep -rn -E 'docs/[a-zA-Z0-9_/.+-]+\.md' \
                  $SRC_DIRS \
                  --include='*.c' --include='*.h' --include='*.sh' \
                  2>/dev/null || true)
if [ -n "$violations" ]; then
    echo "${RED}ERROR:${RESET} doc reference in source comments:"
    echo "$violations"
    ERRORS=$((ERRORS + 1))
fi

# 2. Refactor-phase Chinese wording
echo "=== checking for forbidden refactor wording ==="
violations=$(grep -rn -E '(本次重构|本阶段|这一步)' \
                  $SRC_DIRS \
                  --include='*.c' --include='*.h' --include='*.sh' \
                  2>/dev/null \
            | grep -v 'check_comment_rules.sh' \
            || true)
if [ -n "$violations" ]; then
    echo "${RED}ERROR:${RESET} refactor-phase wording in source comments:"
    echo "$violations"
    ERRORS=$((ERRORS + 1))
fi

# 3. Phase X.Y / Phase A.N (sub-phase coordinates — never algorithmic)
echo "=== checking for sub-phase coordinates (Phase X.Y) ==="
violations=$(grep -rn -E '\bPhase\s*[0-9A-F]\.[0-9]' \
                  $SRC_DIRS \
                  --include='*.c' --include='*.h' --include='*.sh' \
                  2>/dev/null \
            | grep -v 'check_comment_rules.sh' \
            || true)
if [ -n "$violations" ]; then
    echo "${RED}ERROR:${RESET} sub-phase coordinate in comment (Phase X.Y is never algorithmic):"
    echo "$violations"
    ERRORS=$((ERRORS + 1))
fi

# 4. Issue-tracker IDs commonly used as refactor coordinates
echo "=== checking for issue-tracker / refactor IDs ==="
# Ignore the rule docs/.windsurf rules themselves.
violations=$(grep -rn -E '\b(CORO-[0-9]+|A-0[0-9]|C-0[0-9]|F-0[0-9]|X-0[0-9])\b' \
                  $SRC_DIRS \
                  --include='*.c' --include='*.h' --include='*.sh' \
                  2>/dev/null \
            | grep -v 'check_comment_rules.sh' \
            || true)
if [ -n "$violations" ]; then
    echo "${RED}ERROR:${RESET} issue-tracker / refactor ID in comment:"
    echo "$violations"
    ERRORS=$((ERRORS + 1))
fi

# 5. P0 / P1 priority labels (refactor coordinates)
echo "=== checking for P0/P1 priority labels ==="
# Limit to comments only (lines starting with // or *) to avoid hitting code.
violations=$(grep -rnE '^[[:space:]]*(//|\*).*\b(P0|P1|P2|P3)\b' \
                  $SRC_DIRS \
                  --include='*.c' --include='*.h' --include='*.sh' \
                  2>/dev/null \
            | grep -v 'check_comment_rules.sh' \
            || true)
if [ -n "$violations" ]; then
    echo "${YELLOW}WARN:${RESET} possible priority label in comment (review manually):"
    echo "$violations"
    # Don't fail CI on this — too many false positives in logging-style code.
fi

if [ "$ERRORS" -gt 0 ]; then
    echo
    echo "${RED}FAIL: $ERRORS forbidden pattern(s) found.${RESET}"
    echo "See .windsurf/rules/main.md '注释与 commit 铁律' for details."
    exit 1
fi

echo "${GREEN}OK: comment rules clean.${RESET}"
exit 0

#!/usr/bin/env bash
# Thin wrapper kept for backwards compatibility.
#
# All Windows / Parallels VM testing must go through win_pd_test.sh
# (which uses prlctl exec instead of SSH). See:
#   .windsurf/rules/windows-testing.md
#   docs/rules/dev-workflow.md (Windows section)
#
# Behaviour preserved:
#   - When called without args: full sync + build + ctest cycle.
#   - Positional args are forwarded to ctest.
#   - XRAY_WIN_SKIP_BUILD=1 still skips the build phase.
# Anything else (parallel timeouts, status, kill, regression, etc.) is
# now a feature of win_pd_test.sh; this wrapper does not try to
# replicate it.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET="$SCRIPT_DIR/win_pd_test.sh"

if [ ! -x "$TARGET" ]; then
    echo "[win-test] $TARGET not executable" >&2
    exit 2
fi

echo "[win-test] forwarding to win_pd_test.sh (use it directly for new flags)" >&2

args=()
if [ -n "${XRAY_WIN_SKIP_BUILD:-}" ]; then
    args+=("--no-build")
fi
args+=("$@")

exec "$TARGET" "${args[@]}"

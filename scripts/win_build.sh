#!/usr/bin/env bash
# Thin wrapper kept for backwards compatibility.
#
# Windows builds inside the Parallels VM now go through
# win_pd_test.sh (prlctl exec, with timeouts + stale-process
# cleanup). This wrapper triggers a sync + build only, no tests.
#
# Environment variables that win_build.sh used to honour are still
# read by win_pd_test.sh (XRAY_WIN_BUILD_TYPE, XRAY_WIN_CMAKE_EXTRA,
# XRAY_WIN_BUILD, XRAY_WIN_SRC, etc.).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET="$SCRIPT_DIR/win_pd_test.sh"

if [ ! -x "$TARGET" ]; then
    echo "[win-build] $TARGET not executable" >&2
    exit 2
fi

echo "[win-build] forwarding to win_pd_test.sh --build-only (use it directly for new flags)" >&2

exec "$TARGET" --build-only "$@"

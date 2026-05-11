#!/usr/bin/env bash
# Run ctest inside the Parallels Win11 VM and pipe results back to macOS.
#
# Prerequisites:
#   - VM has been provisioned (OpenSSH, MSVC Build Tools, CMake, Ninja)
#   - scripts/win_build.sh has produced a build under C:\workspace\xray-build
#
# Behavior:
#   - Streams ctest output live to this terminal
#   - On exit, fetches the saved ctest log into /tmp/win_ctest.log
#   - Non-zero exit code propagates so CI / shells can react

set -euo pipefail

REMOTE_HOST="${XRAY_WIN_HOST:-xray-win}"
REMOTE_BUILD="${XRAY_WIN_BUILD:-C:/workspace/xray-build}"
CTEST_ARGS="${XRAY_WIN_CTEST_ARGS:---output-on-failure --timeout 120}"
LOCAL_LOG="${XRAY_WIN_LOG:-/tmp/win_ctest.log}"

echo "[win-test] ctest on $REMOTE_HOST ($REMOTE_BUILD)"

# We let ctest's exit status flow through. tee inside Windows captures the
# log; we then scp it back.
REMOTE_CMD="cd /d \"$REMOTE_BUILD\" && ctest $CTEST_ARGS"

set +e
ssh "$REMOTE_HOST" "cmd /c $REMOTE_CMD"
RC=$?
set -e

# Try to fetch the log even on failure
scp -q "$REMOTE_HOST:$REMOTE_BUILD/Testing/Temporary/LastTest.log" "$LOCAL_LOG" 2>/dev/null \
    || echo "[win-test] no LastTest.log to fetch"

if [ -f "$LOCAL_LOG" ]; then
    echo "[win-test] log saved to $LOCAL_LOG"
fi

exit $RC

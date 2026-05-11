#!/usr/bin/env bash
# One-shot Windows regression: sync -> build -> ctest, on the Parallels
# Win11 runner described by the xray-win ssh alias.
#
# Env overrides:
#   XRAY_WIN_HOST         ssh alias (default: xray-win)
#   XRAY_WIN_BUILD        Windows build dir (default: C:\workspace\xray-build)
#   XRAY_WIN_CTEST_ARGS   extra ctest args (default: --output-on-failure -j 8)
#   XRAY_WIN_LOG          local path to copy LastTest.log (default: /tmp/win_ctest.log)
#   XRAY_WIN_SKIP_BUILD   non-empty to skip the build phase (use when iterating
#                         on a single test that does not require recompile)
#
# Positional args are passed verbatim to ctest on the VM side, so things
# like `./scripts/win_test.sh -R test_vm_api -V` work.
#
# Exit status is ctest's, so this can drop straight into CI.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REMOTE_HOST="${XRAY_WIN_HOST:-xray-win}"
REMOTE_BUILD="${XRAY_WIN_BUILD:-C:\\workspace\\xray-build}"
CTEST_ARGS="${XRAY_WIN_CTEST_ARGS:---output-on-failure -j 8 --timeout 120}"
LOCAL_LOG="${XRAY_WIN_LOG:-/tmp/win_ctest.log}"

# Normalize path separators. cmd.exe's `cd /d` accepts forward slashes
# in most modern builds but the older behavior (silently cd into cwd
# on failure) still shows up through OpenSSH's stripped environment,
# so we pay the tiny cost of converting to backslashes unconditionally.
REMOTE_BUILD_WIN="${REMOTE_BUILD//\//\\}"

if [ -z "${XRAY_WIN_SKIP_BUILD:-}" ]; then
    echo "[win-test] step 1/2: sync + build"
    "$SCRIPT_DIR/win_build.sh"
else
    echo "[win-test] step 1/2: SKIPPED (XRAY_WIN_SKIP_BUILD set)"
fi

echo "[win-test] step 2/2: ctest on $REMOTE_HOST ($REMOTE_BUILD_WIN)"

# Render the ctest invocation into a .bat file and ship+exec it, mirroring
# win_build.sh. Going through a .bat avoids the cmd-over-ssh double-quoting
# minefield: things like `-R 'test_a|test_b'` survive intact because cmd
# does not re-parse `|` inside a "..." string.
REMOTE_BAT="${REMOTE_BUILD_WIN}\\xray-ctest.bat"
TMP=$(mktemp -t xray_win_test.XXXXXX.bat)
trap 'rm -f "$TMP"' EXIT

# Build a single-line ctest command with every positional arg quoted so
# glob / pipe chars don't reach cmd's parser unprotected.
CTEST_LINE="ctest $CTEST_ARGS"
for a in "$@"; do
    # Escape any embedded double quote (rare but keep it correct).
    esc=${a//\"/\\\"}
    CTEST_LINE="$CTEST_LINE \"$esc\""
done

printf '%s\n' "@echo off" \
    "cd /d $REMOTE_BUILD_WIN || exit /b 1" \
    "$CTEST_LINE" > "$TMP"

# Ship the .bat in, then exec it.
tar -cf - -C "$(dirname "$TMP")" "$(basename "$TMP")" \
    | ssh "$REMOTE_HOST" "cd /d C:\\workspace && tar -xf -"
ssh "$REMOTE_HOST" "move /y C:\\workspace\\$(basename "$TMP") $REMOTE_BAT" >/dev/null

set +e
ssh "$REMOTE_HOST" "cmd /c $REMOTE_BAT"
RC=$?
set -e

# Best-effort log fetch. A missing log on pass-everything runs is
# normal (CTest only creates LastTest.log when there's something to
# record), so we don't treat scp failure as an error.
scp -q "$REMOTE_HOST:$REMOTE_BUILD/Testing/Temporary/LastTest.log" "$LOCAL_LOG" 2>/dev/null \
    && echo "[win-test] log saved to $LOCAL_LOG" \
    || echo "[win-test] (no LastTest.log to fetch)"

exit $RC

#!/usr/bin/env bash
# Build xray inside the Parallels Win11 VM after syncing source.
#
# Pipeline:
#   1. rsync macOS source -> VM (delegates to win_sync.sh)
#   2. ssh into VM, run cmake (Ninja generator, MSVC x64) + ninja
#   3. stdout/stderr stream back to this terminal
#
# Notes:
#   - Build directory: C:\workspace\xray-build (separate from source)
#   - Build type defaults to Release; override with XRAY_WIN_BUILD_TYPE
#   - MSVC environment is loaded via vcvarsamd64_arm64.bat so that we
#     produce x64 binaries on an ARM64 host (Apple Silicon -> Parallels)
#   - If zlib is needed, install vcpkg + zlib inside the VM and pass
#     CMAKE_TOOLCHAIN_FILE through XRAY_WIN_CMAKE_EXTRA.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REMOTE_HOST="${XRAY_WIN_HOST:-xray-win}"
REMOTE_SRC="${XRAY_WIN_SRC:-C:/workspace/xray}"
REMOTE_BUILD="${XRAY_WIN_BUILD:-C:/workspace/xray-build}"
BUILD_TYPE="${XRAY_WIN_BUILD_TYPE:-Release}"
CMAKE_EXTRA="${XRAY_WIN_CMAKE_EXTRA:-}"

# Path to vcvars batch file. Adjust if Build Tools was installed elsewhere.
VCVARS='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsamd64_arm64.bat'

echo "[win-build] step 1/2: sync source"
"$SCRIPT_DIR/win_sync.sh"

echo "[win-build] step 2/2: cmake + ninja on $REMOTE_HOST"
# Use cmd.exe to chain vcvars with cmake/ninja inside the same shell so
# the environment variables propagate. We pass the entire command as a
# single -c argument to avoid Windows quoting hell over SSH.
REMOTE_CMD=$(cat <<EOF
if not exist "$REMOTE_BUILD" mkdir "$REMOTE_BUILD"
call "$VCVARS" || exit /b 1
cmake -S "$REMOTE_SRC" -B "$REMOTE_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=$BUILD_TYPE $CMAKE_EXTRA || exit /b 1
ninja -C "$REMOTE_BUILD"
EOF
)

# Write to a temp file to avoid PTY quoting issues with long heredocs.
TMP=$(mktemp -t xray_win_build.XXXXXX.bat)
printf '%s\n' "$REMOTE_CMD" > "$TMP"
trap 'rm -f "$TMP"' EXIT

ssh "$REMOTE_HOST" "cmd /c" < "$TMP"
echo "[win-build] done"

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
#   - MSVC environment is loaded via vcvarsarm64_amd64.bat so that we
#     produce x64 binaries on an ARM64 host (Apple Silicon -> Parallels).
#     vcvars64.bat is a shim that dispatches to the right host/target
#     pair automatically, so it works regardless of whether the runner
#     is ARM64 (cross-compile to x64) or x64 (native).
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
# vcvars64.bat picks the right host/target shim automatically (on
# ARM64 it routes to vcvarsarm64_amd64.bat).
VCVARS='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'

echo "[win-build] step 1/2: sync source"
"$SCRIPT_DIR/win_sync.sh"

echo "[win-build] step 2/2: cmake + ninja on $REMOTE_HOST"
# Use cmd.exe to chain vcvars with cmake/ninja inside one shell so the
# vcvars-exported env (PATH, INCLUDE, LIB) is visible to cmake/ninja.
# Strategy: render the full sequence into a .bat file on the VM, then
# exec it. Writing the .bat via stdin redirect ("cat | ssh ... 'type
# con > x.bat'") avoids cmd.exe quote-nesting headaches.
REMOTE_BAT='C:\workspace\xray-build.bat'
REMOTE_CMD=$(cat <<EOF
@echo on
if not exist "$REMOTE_BUILD" mkdir "$REMOTE_BUILD"
call "$VCVARS" || exit /b 1
cmake -S "$REMOTE_SRC" -B "$REMOTE_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=$BUILD_TYPE $CMAKE_EXTRA || exit /b 1
ninja -C "$REMOTE_BUILD"
EOF
)

TMP=$(mktemp -t xray_win_build.XXXXXX.bat)
printf '%s\n' "$REMOTE_CMD" > "$TMP"
trap 'rm -f "$TMP"' EXIT

# 1. ship the .bat to the VM via tar (the only reliable stdin->file
#    transport over Windows OpenSSH without scp gymnastics)
tar -cf - -C "$(dirname "$TMP")" "$(basename "$TMP")" \
    | ssh "$REMOTE_HOST" "cd /d C:\\workspace && tar -xf -"
ssh "$REMOTE_HOST" "move /y C:\\workspace\\$(basename "$TMP") $REMOTE_BAT" >/dev/null

# 2. execute it. cmd /c <bat> streams stdout/stderr back through ssh.
ssh "$REMOTE_HOST" "cmd /c $REMOTE_BAT"
echo "[win-build] done"

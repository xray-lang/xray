#!/usr/bin/env bash
# Sync xray source tree from macOS host into a Parallels Win11 VM.
#
# Transport: tar-over-ssh. Windows does not ship rsync and making
# every contributor install it (MSYS2 / cwRsync / Git-for-Windows)
# is heavier than the transfer itself. Win10+ ships a native tar.exe
# in System32, and macOS has bsdtar too, so tar|ssh|tar rebuilds the
# source tree at ~60-80 MB/s over Parallels Shared networking --
# enough for a ~100 MB codebase to land in 1-2 seconds.
#
# Prerequisites (one-time):
#   1. VM has OpenSSH Server enabled with key-based auth
#   2. macOS ~/.ssh/config has a host alias `xray-win` pointing at the VM
#   3. Workspace dir exists on VM (scripts/win_vm_provision.ps1 creates
#      C:\workspace\xray by default)
#
# Direction is always macOS -> VM; never edit source inside the VM.
# Build artifacts live in C:\workspace\xray-build (separate dir),
# untouched by this script.

set -euo pipefail

REMOTE_HOST="${XRAY_WIN_HOST:-xray-win}"
SRC_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# REMOTE_DST must use Windows path separators when passed to tar.exe
# via cmd.exe on the remote side.
REMOTE_DST_WIN="${XRAY_WIN_SRC_WIN:-C:\\workspace\\xray}"

echo "[win-sync] $SRC_ROOT/  ->  $REMOTE_HOST:$REMOTE_DST_WIN"

# Exclusions: keep in sync with the list below and .gitignore spirit.
# We intentionally drop .git/ (history isn't needed for build), build
# dirs (VM maintains its own), and macOS/editor noise.
EXCLUDES=(
    --exclude='.git'
    --exclude='build'
    --exclude='build-*'
    --exclude='.cache'
    --exclude='__pycache__'
    --exclude='*.o'
    --exclude='*.obj'
    --exclude='*.dSYM'
    --exclude='node_modules'
    --exclude='.DS_Store'
    --exclude='.windsurf'
)

# Pipe a tar of the working tree directly into a tar -xf on the VM,
# overwriting in place. The remote command is kept simple to avoid
# cmd.exe quote-nesting traps; the destination is created up front in
# a separate ssh call so the streaming step has only `cd && tar`.
start=$(date +%s)
ssh "$REMOTE_HOST" "if not exist $REMOTE_DST_WIN mkdir $REMOTE_DST_WIN" >/dev/null
# COPYFILE_DISABLE=1 tells macOS bsdtar not to emit AppleDouble (._*)
# resource fork sidecars, which otherwise pollute the Windows tree.
COPYFILE_DISABLE=1 tar --no-xattrs -C "$(dirname "$SRC_ROOT")" \
    "${EXCLUDES[@]}" \
    -cf - "$(basename "$SRC_ROOT")" \
    | ssh "$REMOTE_HOST" "cd /d $REMOTE_DST_WIN && tar -xf - --strip-components=1"
end=$(date +%s)

echo "[win-sync] done in $((end - start))s"

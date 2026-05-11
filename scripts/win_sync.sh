#!/usr/bin/env bash
# Sync xray source tree from macOS host into a Parallels Win11 VM
# for build/test verification on Windows.
#
# Prerequisites (one-time):
#   1. VM has OpenSSH Server enabled with key-based auth
#   2. macOS ~/.ssh/config has a host alias `xray-win` pointing at the VM
#      Example:
#        Host xray-win
#            HostName 10.211.55.3
#            User <your-windows-user>
#            IdentityFile ~/.ssh/id_ed25519
#   3. VM has C:\workspace\xray as an existing directory
#
# This script is one-way: macOS -> VM. NEVER edit source inside the VM.
# Build artifacts live in C:\workspace\xray-build (separate from the
# synced source tree) and are not touched by this script.

set -euo pipefail

REMOTE_HOST="${XRAY_WIN_HOST:-xray-win}"
SRC_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REMOTE_DST="${XRAY_WIN_SRC:-/c/workspace/xray/}"

echo "[win-sync] $SRC_ROOT/  ->  $REMOTE_HOST:$REMOTE_DST"

rsync -az --delete \
    --exclude='build*/' \
    --exclude='.git/' \
    --exclude='.cache/' \
    --exclude='__pycache__/' \
    --exclude='*.o' \
    --exclude='*.obj' \
    --exclude='*.dSYM/' \
    --exclude='node_modules/' \
    --exclude='.DS_Store' \
    "$SRC_ROOT/" \
    "$REMOTE_HOST:$REMOTE_DST"

echo "[win-sync] done"

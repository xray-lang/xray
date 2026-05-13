#!/usr/bin/env bash
# Install git hooks for the xray project.
# Works on macOS, Linux, and Windows (Git Bash / MSYS2).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HOOKS_DIR="$REPO_ROOT/.git/hooks"

if [ ! -d "$HOOKS_DIR" ]; then
    echo "Error: .git/hooks not found. Run from inside the xray repo."
    exit 1
fi

cp "$SCRIPT_DIR/pre-commit" "$HOOKS_DIR/pre-commit"
chmod +x "$HOOKS_DIR/pre-commit"

echo "Installed pre-commit hook to $HOOKS_DIR/pre-commit"
echo "Staged .c/.h files will be auto-formatted on commit."

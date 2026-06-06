#!/usr/bin/env bash
# Coroutine scaling gate entry point.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "$SCRIPT_DIR/coro_scaling_gate.py" "$@"

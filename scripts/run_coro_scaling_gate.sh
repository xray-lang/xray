#!/usr/bin/env bash
# Coroutine scaling gate entry point.
#
# Runtime-mode semantics live in coro_scaling_gate.py. The M0 VM/JIT gate
# defaults to xray-vm; use --jit-only, --vm-jit, or --vm-jit-go for the current
# performance line. AOT is opt-in via --include-aot-correctness, --aot-only, or
# --all-backends so the known AOT pipeline blocker does not pollute VM/JIT gates.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "$SCRIPT_DIR/coro_scaling_gate.py" "$@"

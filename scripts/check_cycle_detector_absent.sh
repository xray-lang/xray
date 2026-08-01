#!/bin/bash
# Task 247 phase D: the cycle detector must leave NO trace in a default build.
#
# It is a compile-time switch, not a runtime flag — section 6.1 requires the
# production binary to contain none of it, and section 0.3 forbids a no-op
# stdlib entry point standing in for it (the spec was already criticised once
# for a tracing-GC hook that did nothing).
#
# Usage: check_cycle_detector_absent.sh <binary>
set -o pipefail
BIN="${1:-build/xray}"
if [ ! -x "$BIN" ]; then
    echo "SKIP: binary not found: $BIN"
    exit 0
fi
if ! command -v nm >/dev/null 2>&1; then
    echo "SKIP: nm not available"
    exit 0
fi
hits="$(nm "$BIN" 2>/dev/null | grep -c "xr_cycle_detector" || true)"
if [ "$hits" != "0" ]; then
    echo "FAIL: $BIN contains $hits cycle-detector symbol(s); it must be compiled out by default"
    nm "$BIN" 2>/dev/null | grep "xr_cycle_detector" | head -10
    exit 1
fi
echo "OK: no cycle-detector symbols in $BIN"

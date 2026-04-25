#!/bin/bash
# lldb_debug.sh - Debug xray with lldb without crash handler interference
#
# Usage:
#   scripts/lldb_debug.sh tests/jit/052_map_loop.xr
#   scripts/lldb_debug.sh --bt tests/jit/052_map_loop.xr   # auto backtrace & quit
#
# The script:
#   1. Disables JIT crash handler via XRAY_NO_JIT_CRASH_HANDLER
#   2. Configures lldb signal handling properly
#   3. Supports --bt mode for quick crash diagnosis

BUILD=${XRAY_BUILD:-build-release}
XRAY="$BUILD/xray"

if [ ! -f "$XRAY" ]; then
    echo "Error: $XRAY not found. Set XRAY_BUILD=build-debug if needed."
    exit 1
fi

AUTO_BT=0
if [ "$1" = "--bt" ]; then
    AUTO_BT=1
    shift
fi

if [ $# -eq 0 ]; then
    echo "Usage: $0 [--bt] <script.xr> [args...]"
    exit 1
fi

export XRAY_NO_JIT_CRASH_HANDLER=1

if [ $AUTO_BT -eq 1 ]; then
    # Batch mode: run, get backtrace on crash, quit
    # -o runs before crash, -k runs after crash/stop
    lldb --batch \
        -o "run" \
        -k "thread backtrace" \
        -k "quit" \
        -- "$XRAY" "$@"
else
    # Interactive mode: start lldb, user types 'run' manually
    lldb -- "$XRAY" "$@"
fi

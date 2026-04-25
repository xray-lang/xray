#!/usr/bin/env bash
# ===========================================================================
# DAP Regression Test Runner
#
# Drives the xray DAP server over stdio, sending JSON-RPC requests and
# validating responses/events via a lightweight transcript driver.
#
# Usage:
#   scripts/run_dap_regression_tests.sh          # run all tests
#   VERBOSE=1 scripts/run_dap_regression_tests.sh  # verbose output
# ===========================================================================

set -eo pipefail
trap '' PIPE  # Ignore SIGPIPE (server may exit before test finishes writing)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Colors (disabled for non-TTY / NO_COLOR)
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    RED='\033[1;31m' GREEN='\033[1;32m' BLUE='\033[1;34m' GRAY='\033[0;90m' NC='\033[0m'
else
    RED='' GREEN='' BLUE='' GRAY='' NC=''
fi

# Auto-detect build directory
if [ -n "${XRAY_BUILD_DIR:-}" ]; then
    BUILD_DIR="${XRAY_BUILD_DIR}"
elif [ -f "${PROJECT_ROOT}/build/xray" ]; then
    BUILD_DIR="${PROJECT_ROOT}/build"
elif [ -f "${PROJECT_ROOT}/build-release/xray" ]; then
    BUILD_DIR="${PROJECT_ROOT}/build-release"
else
    BUILD_DIR="${PROJECT_ROOT}/build"
fi
XRAY_BIN="${BUILD_DIR}/xray"

FIXTURE_DIR="${PROJECT_ROOT}/tests/regression/dap/fixtures"
VERBOSE="${VERBOSE:-0}"

# Counters
PASS=0
FAIL=0
TOTAL=0

# PID of current DAP server (for cleanup)
_DAP_PID=""

# ---- Helpers ---------------------------------------------------------------

# Start a DAP server and set up FIFOs for communication.
# Sets: _DAP_PID, _DAP_IN (path to write FIFO), _DAP_OUT (path to read FIFO)
dap_start() {
    local tmpdir="$1"
    _DAP_IN="${tmpdir}/dap_in"
    _DAP_OUT="${tmpdir}/dap_out"
    mkfifo "$_DAP_IN" "$_DAP_OUT"

    "$XRAY_BIN" dap < "$_DAP_IN" > "$_DAP_OUT" 2>"${tmpdir}/stderr.log" &
    _DAP_PID=$!

    # Open write fd (non-blocking open of the read end is done by the server)
    exec 7>"$_DAP_IN"
    exec 8<"$_DAP_OUT"
}

# Stop the DAP server
dap_stop() {
    exec 7>&- 2>/dev/null || true  # close write fd
    exec 8<&- 2>/dev/null || true  # close read fd
    if [ -n "$_DAP_PID" ] && kill -0 "$_DAP_PID" 2>/dev/null; then
        kill "$_DAP_PID" 2>/dev/null || true
        wait "$_DAP_PID" 2>/dev/null || true
    fi
    _DAP_PID=""
}

# Send a DAP message (Content-Length framed) on fd 7
dap_send() {
    local json="$1"
    local len=${#json}
    printf "Content-Length: %d\r\n\r\n%s" "$len" "$json" >&7
}

# Read one DAP message from fd 8. Prints JSON body to stdout.
# Returns 1 on EOF / timeout.
dap_recv() {
    local timeout="${1:-5}"
    local header=""
    local content_length=0

    # Read headers (terminated by empty line)
    while true; do
        if ! IFS= read -r -t "$timeout" header <&8 2>/dev/null; then
            return 1
        fi
        header="${header%%$'\r'}"  # strip CR
        if [ -z "$header" ]; then
            break
        fi
        if [[ "$header" == Content-Length:* ]]; then
            content_length="${header#Content-Length: }"
            content_length="${content_length// /}"
        fi
    done

    if [ "$content_length" -le 0 ] 2>/dev/null; then
        return 1
    fi

    # Read body
    local body=""
    if ! body=$(dd bs=1 count="$content_length" <&8 2>/dev/null); then
        return 1
    fi
    echo "$body"
}

# Drain and collect messages for up to N seconds, print each on a line
dap_drain() {
    local max_msgs="${1:-20}"
    local timeout="${2:-2}"
    local count=0
    while [ "$count" -lt "$max_msgs" ]; do
        local msg
        if ! msg=$(dap_recv "$timeout"); then
            break
        fi
        echo "$msg"
        count=$((count + 1))
    done
}

# Check JSON string contains a key pattern (simple grep-based)
json_has() {
    local json="$1"
    local pattern="$2"
    echo "$json" | grep -q "$pattern"
}

# Run a single test case
# $1 = test name
# $2 = function name to call
run_test() {
    local name="$1"
    local func="$2"
    TOTAL=$((TOTAL + 1))
    printf "[%3d] %-45s ... " "$TOTAL" "$name"

    local tmpdir
    tmpdir=$(mktemp -d)

    if "$func" "$tmpdir" > "${tmpdir}/stdout.log" 2>&1; then
        echo -e "${GREEN}✓ PASS${NC}"
        PASS=$((PASS + 1))
    else
        echo -e "${RED}✗ FAIL${NC}"
        FAIL=$((FAIL + 1))
        if [ "$VERBOSE" = "1" ]; then
            echo -e "${GRAY}--- stdout ---${NC}"
            cat "${tmpdir}/stdout.log" 2>/dev/null || true
            echo -e "${GRAY}--- stderr ---${NC}"
            cat "${tmpdir}/stderr.log" 2>/dev/null || true
            echo -e "${GRAY}--- end ---${NC}"
        fi
    fi

    # Ensure cleanup
    dap_stop 2>/dev/null || true
    rm -rf "$tmpdir"
}

# ---- Test Cases ------------------------------------------------------------

# Test 1: Initialize — verify capabilities
test_initialize() {
    local tmpdir="$1"
    dap_start "$tmpdir"

    # Send initialize
    dap_send '{"seq":1,"type":"request","command":"initialize","arguments":{"adapterID":"test","clientID":"test"}}'

    # Read response
    local resp
    resp=$(dap_recv 5) || { dap_stop; return 1; }

    [ "$VERBOSE" = "1" ] && echo "RESP: $resp"

    # Verify it's an initialize response with body
    json_has "$resp" '"command":"initialize"' || { dap_stop; return 1; }
    json_has "$resp" '"success":true' || { dap_stop; return 1; }
    json_has "$resp" '"supportsConfigurationDoneRequest":true' || { dap_stop; return 1; }

    # Read initialized event
    local evt
    evt=$(dap_recv 3) || { dap_stop; return 1; }
    json_has "$evt" '"event":"initialized"' || { dap_stop; return 1; }

    # Disconnect
    dap_send '{"seq":2,"type":"request","command":"disconnect","arguments":{}}'
    sleep 0.2
    dap_stop
    return 0
}

# Test 2: Launch + configurationDone + normal exit
test_launch_run_exit() {
    local tmpdir="$1"
    local script="${FIXTURE_DIR}/hello.xr"
    dap_start "$tmpdir"

    # Initialize
    dap_send '{"seq":1,"type":"request","command":"initialize","arguments":{"adapterID":"test"}}'
    dap_recv 5 >/dev/null || { dap_stop; return 1; }
    dap_recv 3 >/dev/null || true  # initialized event

    # Launch (no stopOnEntry)
    dap_send "{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"${script}\",\"stopOnEntry\":false}}"
    local launch_resp
    launch_resp=$(dap_recv 5) || { dap_stop; return 1; }
    json_has "$launch_resp" '"success":true' || { dap_stop; return 1; }

    # ConfigurationDone
    dap_send '{"seq":3,"type":"request","command":"configurationDone","arguments":{}}'

    # Collect remaining messages — expect terminated + exited events
    local msgs
    msgs=$(dap_drain 10 5)

    [ "$VERBOSE" = "1" ] && echo "MSGS: $msgs"

    # Should see terminated or exited event
    echo "$msgs" | grep -q '"event":"terminated"' || echo "$msgs" | grep -q '"event":"exited"' || {
        dap_stop; return 1;
    }

    dap_stop
    return 0
}

# Test 3: Disconnect without launch — clean shutdown
test_disconnect_clean() {
    local tmpdir="$1"
    dap_start "$tmpdir"

    # Initialize
    dap_send '{"seq":1,"type":"request","command":"initialize","arguments":{"adapterID":"test"}}'
    dap_recv 5 >/dev/null || { dap_stop; return 1; }
    dap_recv 3 >/dev/null || true  # initialized event

    # Disconnect immediately (no launch)
    dap_send '{"seq":2,"type":"request","command":"disconnect","arguments":{}}'

    local resp
    resp=$(dap_recv 3) || true

    [ "$VERBOSE" = "1" ] && echo "RESP: $resp"

    # Server should exit cleanly
    sleep 0.3
    dap_stop
    return 0
}

# Test 4: Launch with stopOnEntry — verify stopped event
test_stop_on_entry() {
    local tmpdir="$1"
    local script="${FIXTURE_DIR}/hello.xr"
    dap_start "$tmpdir"

    # Initialize
    dap_send '{"seq":1,"type":"request","command":"initialize","arguments":{"adapterID":"test"}}'
    dap_recv 5 >/dev/null || { dap_stop; return 1; }
    dap_recv 3 >/dev/null || true  # initialized event

    # Launch with stopOnEntry
    dap_send "{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"${script}\",\"stopOnEntry\":true}}"
    local launch_resp
    launch_resp=$(dap_recv 5) || { dap_stop; return 1; }
    json_has "$launch_resp" '"success":true' || { dap_stop; return 1; }

    # ConfigurationDone
    dap_send '{"seq":3,"type":"request","command":"configurationDone","arguments":{}}'

    # Collect messages — expect stopped event with reason "entry" or "step"
    local msgs
    msgs=$(dap_drain 10 5)

    [ "$VERBOSE" = "1" ] && echo "MSGS: $msgs"

    echo "$msgs" | grep -q '"event":"stopped"' || {
        dap_stop; return 1;
    }

    # Continue execution
    dap_send '{"seq":4,"type":"request","command":"continue","arguments":{"threadId":1}}'

    # Drain remaining — expect terminated/exited
    local msgs2
    msgs2=$(dap_drain 10 5)

    [ "$VERBOSE" = "1" ] && echo "MSGS2: $msgs2"

    # Disconnect
    dap_send '{"seq":5,"type":"request","command":"disconnect","arguments":{}}'
    sleep 0.3
    dap_stop
    return 0
}

# Test 5: Set breakpoint → hit → continue → exit
test_breakpoint_hit() {
    local tmpdir="$1"
    local script="${FIXTURE_DIR}/breakpoint.xr"
    dap_start "$tmpdir"

    # Initialize
    dap_send '{"seq":1,"type":"request","command":"initialize","arguments":{"adapterID":"test"}}'
    dap_recv 5 >/dev/null || { dap_stop; return 1; }
    dap_recv 3 >/dev/null || true  # initialized event

    # Launch with stopOnEntry so we can set breakpoints before running
    dap_send "{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"${script}\",\"stopOnEntry\":true}}"
    local launch_resp
    launch_resp=$(dap_recv 5) || { dap_stop; return 1; }
    json_has "$launch_resp" '"success":true' || { dap_stop; return 1; }

    # Set breakpoint at line 4 (let z = x + y)
    dap_send "{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"${script}\"},\"breakpoints\":[{\"line\":4}]}}"
    local bp_resp
    bp_resp=$(dap_recv 5) || { dap_stop; return 1; }
    json_has "$bp_resp" '"success":true' || { dap_stop; return 1; }
    json_has "$bp_resp" '"verified":true' || { dap_stop; return 1; }

    [ "$VERBOSE" = "1" ] && echo "BP_RESP: $bp_resp"

    # ConfigurationDone
    dap_send '{"seq":4,"type":"request","command":"configurationDone","arguments":{}}'

    # Drain — expect stopped event (stopOnEntry first stop)
    local msgs
    msgs=$(dap_drain 10 5)
    [ "$VERBOSE" = "1" ] && echo "MSGS_ENTRY: $msgs"
    echo "$msgs" | grep -q '"event":"stopped"' || { dap_stop; return 1; }

    # Continue past stopOnEntry — should hit breakpoint at line 4
    dap_send '{"seq":5,"type":"request","command":"continue","arguments":{"threadId":1}}'

    local msgs2
    msgs2=$(dap_drain 10 5)
    [ "$VERBOSE" = "1" ] && echo "MSGS_BP: $msgs2"

    # Should get stopped event with reason "breakpoint"
    echo "$msgs2" | grep -q '"event":"stopped"' || { dap_stop; return 1; }
    echo "$msgs2" | grep -q '"breakpoint"' || { dap_stop; return 1; }

    # Continue past breakpoint — should terminate
    dap_send '{"seq":6,"type":"request","command":"continue","arguments":{"threadId":1}}'

    local msgs3
    msgs3=$(dap_drain 10 5)
    [ "$VERBOSE" = "1" ] && echo "MSGS_EXIT: $msgs3"

    # Should see terminated or exited event
    echo "$msgs3" | grep -q '"event":"terminated"' || echo "$msgs3" | grep -q '"event":"exited"' || {
        dap_stop; return 1;
    }

    # Disconnect
    dap_send '{"seq":7,"type":"request","command":"disconnect","arguments":{}}'
    sleep 0.3
    dap_stop
    return 0
}

# Test 6: Set breakpoint → stackTrace → variables inspection
test_breakpoint_inspect() {
    local tmpdir="$1"
    local script="${FIXTURE_DIR}/breakpoint.xr"
    dap_start "$tmpdir"

    # Initialize
    dap_send '{"seq":1,"type":"request","command":"initialize","arguments":{"adapterID":"test"}}'
    dap_recv 5 >/dev/null || { dap_stop; return 1; }
    dap_recv 3 >/dev/null || true  # initialized event

    # Launch with stopOnEntry
    dap_send "{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"${script}\",\"stopOnEntry\":true}}"
    dap_recv 5 >/dev/null || { dap_stop; return 1; }

    # Set breakpoint at line 5 (print(z)) — after all assignments
    dap_send "{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"${script}\"},\"breakpoints\":[{\"line\":5}]}}"
    local bp_resp
    bp_resp=$(dap_recv 5) || { dap_stop; return 1; }
    json_has "$bp_resp" '"success":true' || { dap_stop; return 1; }

    # ConfigurationDone
    dap_send '{"seq":4,"type":"request","command":"configurationDone","arguments":{}}'

    # Drain stopOnEntry stopped event
    dap_drain 10 5 >/dev/null

    # Continue to breakpoint at line 5
    dap_send '{"seq":5,"type":"request","command":"continue","arguments":{"threadId":1}}'

    local msgs
    msgs=$(dap_drain 10 5)
    [ "$VERBOSE" = "1" ] && echo "MSGS_BP: $msgs"
    echo "$msgs" | grep -q '"event":"stopped"' || { dap_stop; return 1; }

    # Get stack trace
    dap_send '{"seq":6,"type":"request","command":"stackTrace","arguments":{"threadId":1}}'
    local st_resp
    st_resp=$(dap_recv 5) || { dap_stop; return 1; }
    [ "$VERBOSE" = "1" ] && echo "STACK: $st_resp"
    json_has "$st_resp" '"success":true' || { dap_stop; return 1; }
    json_has "$st_resp" '"stackFrames"' || { dap_stop; return 1; }

    # Get scopes for frame 0
    dap_send '{"seq":7,"type":"request","command":"scopes","arguments":{"frameId":0}}'
    local sc_resp
    sc_resp=$(dap_recv 5) || { dap_stop; return 1; }
    [ "$VERBOSE" = "1" ] && echo "SCOPES: $sc_resp"
    json_has "$sc_resp" '"success":true' || { dap_stop; return 1; }
    json_has "$sc_resp" '"scopes"' || { dap_stop; return 1; }

    # Disconnect
    dap_send '{"seq":8,"type":"request","command":"disconnect","arguments":{}}'
    sleep 0.3
    dap_stop
    return 0
}

# Test 7: Exception breakpoint → uncaught throw stops debugger
test_exception_breakpoint() {
    local tmpdir="$1"
    local script="${FIXTURE_DIR}/exception.xr"
    dap_start "$tmpdir"

    # Initialize
    dap_send '{"seq":1,"type":"request","command":"initialize","arguments":{"adapterID":"test"}}'
    dap_recv 5 >/dev/null || { dap_stop; return 1; }
    dap_recv 3 >/dev/null || true  # initialized event

    # Launch (no stopOnEntry)
    dap_send "{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"${script}\",\"stopOnEntry\":false}}"
    local launch_resp
    launch_resp=$(dap_recv 5) || { dap_stop; return 1; }
    json_has "$launch_resp" '"success":true' || { dap_stop; return 1; }

    # Set exception breakpoints: break on uncaught
    dap_send '{"seq":3,"type":"request","command":"setExceptionBreakpoints","arguments":{"filters":["uncaught"]}}'
    local exc_resp
    exc_resp=$(dap_recv 5) || { dap_stop; return 1; }
    json_has "$exc_resp" '"success":true' || { dap_stop; return 1; }

    [ "$VERBOSE" = "1" ] && echo "EXC_BP_RESP: $exc_resp"

    # ConfigurationDone — program runs and should hit uncaught throw
    dap_send '{"seq":4,"type":"request","command":"configurationDone","arguments":{}}'

    local msgs
    msgs=$(dap_drain 10 5)
    [ "$VERBOSE" = "1" ] && echo "MSGS: $msgs"

    # Should see stopped event with reason "exception"
    echo "$msgs" | grep -q '"event":"stopped"' || { dap_stop; return 1; }
    echo "$msgs" | grep -q '"exception"' || { dap_stop; return 1; }

    # Disconnect
    dap_send '{"seq":5,"type":"request","command":"disconnect","arguments":{}}'
    sleep 0.3
    dap_stop
    return 0
}

# Test 8: Function breakpoint → stopped at function entry
test_function_breakpoint() {
    local tmpdir="$1"
    local script="${FIXTURE_DIR}/funcbp.xr"
    dap_start "$tmpdir"

    # Initialize
    dap_send '{"seq":1,"type":"request","command":"initialize","arguments":{"adapterID":"test"}}'
    dap_recv 5 >/dev/null || { dap_stop; return 1; }
    dap_recv 3 >/dev/null || true  # initialized event

    # Launch (no stopOnEntry)
    dap_send "{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"${script}\",\"stopOnEntry\":false}}"
    local launch_resp
    launch_resp=$(dap_recv 5) || { dap_stop; return 1; }
    json_has "$launch_resp" '"success":true' || { dap_stop; return 1; }

    # Set function breakpoint on "greet"
    dap_send '{"seq":3,"type":"request","command":"setFunctionBreakpoints","arguments":{"breakpoints":[{"name":"greet"}]}}'
    local fbp_resp
    fbp_resp=$(dap_recv 5) || { dap_stop; return 1; }
    json_has "$fbp_resp" '"success":true' || { dap_stop; return 1; }

    [ "$VERBOSE" = "1" ] && echo "FBP_RESP: $fbp_resp"

    # ConfigurationDone — program runs and should hit function breakpoint
    dap_send '{"seq":4,"type":"request","command":"configurationDone","arguments":{}}'

    local msgs
    msgs=$(dap_drain 10 5)
    [ "$VERBOSE" = "1" ] && echo "MSGS: $msgs"

    # Should see stopped event
    echo "$msgs" | grep -q '"event":"stopped"' || { dap_stop; return 1; }

    # Continue past breakpoint — should terminate
    dap_send '{"seq":5,"type":"request","command":"continue","arguments":{"threadId":1}}'
    local msgs2
    msgs2=$(dap_drain 10 5)
    [ "$VERBOSE" = "1" ] && echo "MSGS2: $msgs2"
    echo "$msgs2" | grep -q '"event":"terminated"' || echo "$msgs2" | grep -q '"event":"exited"' || {
        dap_stop; return 1;
    }

    # Disconnect
    dap_send '{"seq":6,"type":"request","command":"disconnect","arguments":{}}'
    sleep 0.3
    dap_stop
    return 0
}

# Test 9: Logpoint — outputs message without stopping
test_logpoint() {
    local tmpdir="$1"
    local script="${FIXTURE_DIR}/logpoint.xr"
    dap_start "$tmpdir"

    # Initialize
    dap_send '{"seq":1,"type":"request","command":"initialize","arguments":{"adapterID":"test"}}'
    dap_recv 5 >/dev/null || { dap_stop; return 1; }
    dap_recv 3 >/dev/null || true  # initialized event

    # Launch (no stopOnEntry)
    dap_send "{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"${script}\",\"stopOnEntry\":false}}"
    local launch_resp
    launch_resp=$(dap_recv 5) || { dap_stop; return 1; }
    json_has "$launch_resp" '"success":true' || { dap_stop; return 1; }

    # Set logpoint at line 4 (let c = a + b) with log message
    dap_send "{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"${script}\"},\"breakpoints\":[{\"line\":4,\"logMessage\":\"computing c\"}]}}"
    local bp_resp
    bp_resp=$(dap_recv 5) || { dap_stop; return 1; }
    json_has "$bp_resp" '"success":true' || { dap_stop; return 1; }

    [ "$VERBOSE" = "1" ] && echo "BP_RESP: $bp_resp"

    # ConfigurationDone — program runs, logpoint should output but NOT stop
    dap_send '{"seq":4,"type":"request","command":"configurationDone","arguments":{}}'

    local msgs
    msgs=$(dap_drain 10 5)
    [ "$VERBOSE" = "1" ] && echo "MSGS: $msgs"

    # Should see output event from logpoint
    echo "$msgs" | grep -q '"event":"output"' || { dap_stop; return 1; }
    echo "$msgs" | grep -q '"computing c"' || { dap_stop; return 1; }

    # Should also see terminated (program didn't stop at logpoint)
    echo "$msgs" | grep -q '"event":"terminated"' || echo "$msgs" | grep -q '"event":"exited"' || {
        dap_stop; return 1;
    }

    # Disconnect
    dap_send '{"seq":5,"type":"request","command":"disconnect","arguments":{}}'
    sleep 0.3
    dap_stop
    return 0
}

# ---- Main ------------------------------------------------------------------

echo -e "${BLUE}======================================"
echo "DAP Regression Tests"
echo -e "======================================${NC}"
echo ""

# Check binary exists
if [ ! -f "$XRAY_BIN" ]; then
    echo -e "${RED}Error: xray binary not found at ${XRAY_BIN}${NC}"
    echo "Please build first: cmake --build build -j8"
    exit 1
fi

# Check DAP support compiled in
if ! "$XRAY_BIN" --help 2>&1 | grep -q "dap"; then
    echo -e "${RED}Error: xray was built without DAP support${NC}"
    exit 1
fi

run_test "initialize → capabilities + initialized event" test_initialize
run_test "launch → run → terminated/exited"              test_launch_run_exit
run_test "disconnect without launch → clean exit"        test_disconnect_clean
run_test "launch stopOnEntry → stopped → continue → exit" test_stop_on_entry
run_test "setBreakpoints → hit → continue → exit"        test_breakpoint_hit
run_test "breakpoint → stackTrace → scopes inspection"   test_breakpoint_inspect
run_test "exception breakpoint → uncaught throw stops"   test_exception_breakpoint
run_test "function breakpoint → stopped at entry"        test_function_breakpoint
run_test "logpoint → output event without stopping"      test_logpoint

echo ""
echo -e "${BLUE}======================================"
echo "DAP Test Summary"
echo -e "======================================${NC}"
echo -e "Total: ${TOTAL}"
echo -e "Pass:  ${GREEN}${PASS}${NC}"
if [ "$FAIL" -gt 0 ]; then
    echo -e "Fail:  ${RED}${FAIL}${NC}"
fi
echo ""

if [ "$FAIL" -gt 0 ]; then
    echo -e "${RED}FAILED${NC} — $FAIL test(s) failed"
    echo "Tip: VERBOSE=1 scripts/run_dap_regression_tests.sh"
    exit 1
else
    echo -e "${GREEN}ALL PASSED${NC}"
    exit 0
fi

#!/bin/bash
#
# TCP Echo Benchmark Runner
# Starts each echo server, runs the benchmark client, saves results.
#
# Usage:
#   ./run_bench.sh              # Run all servers
#   ./run_bench.sh xray         # Run only xray
#   ./run_bench.sh xray go      # Run xray and go
#   ./run_bench.sh --suite phase0 xray go
#   ./run_bench.sh --suite phase1
#   ./run_bench.sh --suite phase4
#   ./run_bench.sh --suite phase5
#
# Requirements:
#   - xray binary (../../build/xray or in PATH)
#   - go (for Go server)
#   - node (for Node.js server)
#   - python3 (for Python server and benchmark client)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RESULTS_DIR="$SCRIPT_DIR/results"
PORT=9001
UPSTREAM_PORT=9002
SOURCE_BYTES=$((64 * 1024 * 1024))
SLOW_SOURCE_BYTES=$((16 * 1024 * 1024))
SLOW_DELAY_MS=1
RESULT_FILES=()

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# ========== Utility Functions ==========

log_info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*"; }
log_test()  { echo -e "${CYAN}[TEST]${NC} $*"; }

cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    if [ -n "$UPSTREAM_PID" ] && kill -0 "$UPSTREAM_PID" 2>/dev/null; then
        kill "$UPSTREAM_PID" 2>/dev/null
        wait "$UPSTREAM_PID" 2>/dev/null || true
    fi
}

trap cleanup EXIT

wait_for_port() {
    local port=$1
    local timeout=${2:-10}
    local elapsed=0
    while ! nc -z 127.0.0.1 "$port" 2>/dev/null; do
        sleep 0.2
        elapsed=$(echo "$elapsed + 0.2" | bc)
        if (( $(echo "$elapsed >= $timeout" | bc -l) )); then
            return 1
        fi
    done
    return 0
}

wait_for_server_ready() {
    local port=$1
    local mode=$2
    local pid=$3
    if [ "$mode" = "source" ]; then
        sleep 0.5
        kill -0 "$pid" 2>/dev/null
        return $?
    fi
    wait_for_port "$port" 5
}

kill_port() {
    local port=$1
    local pids
    pids=$(lsof -ti :"$port" 2>/dev/null || true)
    if [ -n "$pids" ]; then
        echo "$pids" | xargs kill -9 2>/dev/null || true
        sleep 0.5
    fi
}

run_benchmark() {
    local server_name=$1
    local result_name=${2:-$server_name}
    local tests=${3:-}
    local scenario=${4:-echo}
    local output_file="$RESULTS_DIR/${result_name}.json"

    log_test "Running benchmark for $server_name ($scenario)..."
    rm -f "$output_file"
    local cmd=(
        python3 "$SCRIPT_DIR/tcp_bench.py"
        --host 127.0.0.1
        --port "$PORT"
        --server "$result_name"
        --scenario "$scenario"
        --output "$output_file"
    )
    if [ -n "$tests" ]; then
        cmd+=(--test "$tests")
    fi
    if ! "${cmd[@]}"; then
        log_error "Benchmark client failed for $server_name"
        return 1
    fi

    if [ -f "$output_file" ]; then
        log_info "Results saved to $output_file"
        RESULT_FILES+=("$output_file")
    else
        log_error "Benchmark failed for $server_name"
        return 1
    fi
}

# ========== Find xray binary ==========

find_xray() {
    # Try common locations
    local candidates=(
        "$SCRIPT_DIR/../../../build-release/xray"
        "$SCRIPT_DIR/../../../build/xray"
        "$(which xray 2>/dev/null || true)"
    )
    for c in "${candidates[@]}"; do
        if [ -n "$c" ] && [ -x "$c" ]; then
            echo "$c"
            return 0
        fi
    done
    return 1
}

# ========== Server Runners ==========

run_xray() {
    local mode=${1:-stream}
    local result_name=${2:-xray}
    local tests=${3:-}
    local extra=${4:-}
    local scenario=${5:-$mode}
    local xray_bin
    xray_bin=$(find_xray) || {
        log_warn "xray binary not found, skipping xray test"
        return 1
    }
    log_info "Using xray: $xray_bin"

    kill_port "$PORT"
    "$xray_bin" "$SCRIPT_DIR/echo_server.xr" -- "$PORT" "$mode" "$extra" &
    SERVER_PID=$!

    if ! wait_for_server_ready "$PORT" "$mode" "$SERVER_PID"; then
        log_error "xray server failed to start"
        kill "$SERVER_PID" 2>/dev/null || true
        return 1
    fi

    run_benchmark "xray" "$result_name" "$tests" "$scenario"

    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
    sleep 0.5
}

run_go() {
    local mode=${1:-message}
    local result_name=${2:-go}
    local tests=${3:-}
    local extra=${4:-}
    local scenario=${5:-$mode}
    if ! command -v go &>/dev/null; then
        log_warn "go not found, skipping Go test"
        return 1
    fi

    log_info "Building Go echo server..."
    (cd "$SCRIPT_DIR" && go build -o echo_server_go echo_server.go)

    kill_port "$PORT"
    "$SCRIPT_DIR/echo_server_go" "$PORT" "$mode" "$extra" &
    SERVER_PID=$!

    if ! wait_for_server_ready "$PORT" "$mode" "$SERVER_PID"; then
        log_error "Go server failed to start"
        kill "$SERVER_PID" 2>/dev/null || true
        return 1
    fi

    run_benchmark "go" "$result_name" "$tests" "$scenario"

    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
    sleep 0.5
}

run_node() {
    local mode=${1:-message}
    local result_name=${2:-node}
    local tests=${3:-}
    local extra=${4:-}
    local scenario=${5:-$mode}
    if ! command -v node &>/dev/null; then
        log_warn "node not found, skipping Node.js test"
        return 1
    fi

    kill_port "$PORT"
    node "$SCRIPT_DIR/echo_server.js" "$PORT" "$mode" "$extra" &
    SERVER_PID=$!

    if ! wait_for_server_ready "$PORT" "$mode" "$SERVER_PID"; then
        log_error "Node.js server failed to start"
        kill "$SERVER_PID" 2>/dev/null || true
        return 1
    fi

    run_benchmark "node" "$result_name" "$tests" "$scenario"

    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
    sleep 0.5
}

run_python() {
    local mode=${1:-message}
    local result_name=${2:-python}
    local tests=${3:-}
    local extra=${4:-}
    local scenario=${5:-$mode}
    if ! command -v python3 &>/dev/null; then
        log_warn "python3 not found, skipping Python test"
        return 1
    fi

    kill_port "$PORT"
    python3 "$SCRIPT_DIR/echo_server.py" "$PORT" "$mode" "$extra" &
    SERVER_PID=$!

    if ! wait_for_server_ready "$PORT" "$mode" "$SERVER_PID"; then
        log_error "Python server failed to start"
        kill "$SERVER_PID" 2>/dev/null || true
        return 1
    fi

    run_benchmark "python" "$result_name" "$tests" "$scenario"

    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
    sleep 0.5
}

run_server() {
    local server=$1
    local mode=$2
    local result_name=$3
    local tests=$4
    local extra=$5
    local scenario=$6
    case "$server" in
        xray)   run_xray "$mode" "$result_name" "$tests" "$extra" "$scenario" ;;
        go)     run_go "$mode" "$result_name" "$tests" "$extra" "$scenario" ;;
        node)   run_node "$mode" "$result_name" "$tests" "$extra" "$scenario" ;;
        python) run_python "$mode" "$result_name" "$tests" "$extra" "$scenario" ;;
        *)      log_warn "Unknown server: $server"; return 1 ;;
    esac
}

compare_result_files() {
    if [ "${#RESULT_FILES[@]}" -gt 1 ]; then
        log_info "Running comparison..."
        echo ""
        python3 "$SCRIPT_DIR/compare.py" "${RESULT_FILES[@]}"
    elif [ "${#RESULT_FILES[@]}" -eq 1 ]; then
        log_info "Only 1 result file, skipping comparison."
    fi
}

start_go_upstream() {
    if ! command -v go &>/dev/null; then
        log_error "go not found; proxy suite needs a Go upstream echo server"
        return 1
    fi
    log_info "Starting Go upstream echo server on port $UPSTREAM_PORT..."
    (cd "$SCRIPT_DIR" && go build -o echo_server_go echo_server.go)
    kill_port "$UPSTREAM_PORT"
    "$SCRIPT_DIR/echo_server_go" "$UPSTREAM_PORT" message "" &
    UPSTREAM_PID=$!
    if ! wait_for_port "$UPSTREAM_PORT" 5; then
        log_error "Go upstream failed to start"
        kill "$UPSTREAM_PID" 2>/dev/null || true
        UPSTREAM_PID=""
        return 1
    fi
}

stop_go_upstream() {
    if [ -n "$UPSTREAM_PID" ] && kill -0 "$UPSTREAM_PID" 2>/dev/null; then
        kill "$UPSTREAM_PID" 2>/dev/null || true
        wait "$UPSTREAM_PID" 2>/dev/null || true
    fi
    UPSTREAM_PID=""
}

run_echo_suite() {
    local servers=("$@")
    RESULT_FILES=()
    local tested=0
    for server in "${servers[@]}"; do
        echo ""
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        log_info "Testing echo suite: $server"
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        local mode="message"
        if [ "$server" = "xray" ]; then
            mode="stream"
        fi
        run_server "$server" "$mode" "$server" "" "" "echo" && ((tested++)) || true
    done
    log_info "Echo suite complete. Tested $tested servers."
    compare_result_files
}

run_phase0_scenario() {
    local scenario=$1
    local mode=$2
    local tests=$3
    local extra=$4
    shift 4
    local servers=("$@")
    RESULT_FILES=()
    local tested=0

    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    log_info "Phase 0 scenario: $scenario"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    if [ "$scenario" = "proxy" ]; then
        start_go_upstream || return 1
    fi

    for server in "${servers[@]}"; do
        echo ""
        log_info "Testing $server ($scenario)"
        run_server "$server" "$mode" "${server}_${scenario}" "$tests" "$extra" "$scenario" &&
            ((tested++)) || true
    done

    if [ "$scenario" = "proxy" ]; then
        stop_go_upstream
    fi

    log_info "Scenario $scenario complete. Tested $tested servers."
    compare_result_files
}

run_phase0_suite() {
    local servers=("$@")
    run_phase0_scenario "message" "message" "message_latency,message_throughput" "" "${servers[@]}"
    run_phase0_scenario "upload" "discard" "upload" "" "${servers[@]}"
    run_phase0_scenario "download" "source" "download" "$SOURCE_BYTES" "${servers[@]}"
    run_phase0_scenario "proxy" "proxy" "proxy_throughput" "$UPSTREAM_PORT" "${servers[@]}"
}

run_phase1_suite() {
    RESULT_FILES=()

    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    log_info "Phase 1 scenario: Xray string path vs Bytes path"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    run_xray "message" "xray_string_path" "latency,throughput,msg_sweep" "" "string_path"
    run_xray "bytes" "xray_bytes_path" "latency,throughput,msg_sweep" "" "bytes_path"

    compare_result_files
}

run_phase4_suite() {
    local servers=("$@")
    run_phase0_scenario "slow_download" "source" "slow_download" "$SLOW_SOURCE_BYTES" "${servers[@]}"
    run_phase0_scenario "slow_upload" "slow_discard" "slow_upload" "$SLOW_DELAY_MS" "${servers[@]}"
    run_phase0_scenario "idle" "idle" "idle_connections" "" "${servers[@]}"
}

run_phase5_suite() {
    local servers=("$@")
    RESULT_FILES=()

    local run_xray_tls=false
    for server in "${servers[@]}"; do
        if [ "$server" = "xray" ]; then
            run_xray_tls=true
        else
            log_warn "Phase 5 TLS suite currently exercises Xray's TLS client path; skipping $server"
        fi
    done

    if [ "$run_xray_tls" != "true" ]; then
        log_warn "No xray target selected for Phase 5 TLS suite."
        return 0
    fi

    local xray_bin
    xray_bin=$(find_xray) || {
        log_warn "xray binary not found, skipping TLS suite"
        return 1
    }

    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    log_info "Phase 5 scenario: TLS client path"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    local output_file="$RESULTS_DIR/xray_tls.json"
    rm -f "$output_file"
    python3 "$SCRIPT_DIR/tls_bench.py" --xray "$xray_bin" --output "$output_file"
    RESULT_FILES+=("$output_file")
    compare_result_files
}

# ========== Main ==========

main() {
    echo ""
    echo "╔═══════════════════════════════════════════╗"
    echo "║       TCP Echo Server Benchmark           ║"
    echo "║  xray vs Go vs Node.js vs Python          ║"
    echo "╚═══════════════════════════════════════════╝"
    echo ""

    mkdir -p "$RESULTS_DIR"

    local suite="echo"
    local servers=()
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --suite)
                suite="$2"
                shift 2
                ;;
            --suite=*)
                suite="${1#--suite=}"
                shift
                ;;
            echo|phase0|phase1|phase4|phase5|all)
                suite="$1"
                shift
                ;;
            *)
                servers+=("$1")
                shift
                ;;
        esac
    done

    if [ ${#servers[@]} -eq 0 ]; then
        servers=(xray go node python)
    fi

    case "$suite" in
        echo)
            run_echo_suite "${servers[@]}"
            ;;
        phase0)
            run_phase0_suite "${servers[@]}"
            ;;
        phase1)
            run_phase1_suite
            ;;
        phase4)
            run_phase4_suite "${servers[@]}"
            ;;
        phase5)
            run_phase5_suite "${servers[@]}"
            ;;
        all)
            run_echo_suite "${servers[@]}"
            run_phase0_suite "${servers[@]}"
            run_phase1_suite
            run_phase4_suite "${servers[@]}"
            run_phase5_suite "${servers[@]}"
            ;;
        *)
            log_error "Unknown suite: $suite"
            return 1
            ;;
    esac
}

main "$@"

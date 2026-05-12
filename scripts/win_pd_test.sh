#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

PD_VM="${XRAY_PD_VM:-Windows 11}"
REMOTE_SRC="${XRAY_WIN_SRC:-C:/workspace/xray}"
REMOTE_SRC_WIN="${XRAY_WIN_SRC_WIN:-C:\\workspace\\xray}"
REMOTE_BUILD="${XRAY_WIN_BUILD:-C:/workspace/xray-build}"
REMOTE_BUILD_WIN="${XRAY_WIN_BUILD_WIN:-C:\\workspace\\xray-build}"
BUILD_TYPE="${XRAY_WIN_BUILD_TYPE:-Release}"
CMAKE_EXTRA="${XRAY_WIN_CMAKE_EXTRA:-}"
VCVARS="${XRAY_WIN_VCVARS:-C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Auxiliary\\Build\\vcvars64.bat}"
CTEST_ARGS="${XRAY_WIN_CTEST_ARGS:---output-on-failure -j 8 --timeout 120}"
BUILD_TIMEOUT="${XRAY_WIN_BUILD_WALL_TIMEOUT:-1800}"
TEST_TIMEOUT="${XRAY_WIN_TEST_WALL_TIMEOUT:-360}"
REGRESSION_TIMEOUT="${XRAY_WIN_REG_WALL_TIMEOUT:-1800}"
REGRESSION_PER_TEST_TIMEOUT="${XRAY_WIN_REG_PER_TEST_TIMEOUT:-60}"
HEARTBEAT="${XRAY_WIN_PD_HEARTBEAT:-20}"
NO_OUTPUT_TIMEOUT="${XRAY_WIN_PD_NO_OUTPUT_TIMEOUT:-90}"
SNAPSHOT_TIMEOUT="${XRAY_WIN_PD_SNAPSHOT_TIMEOUT:-10}"
CLEAN_STALE="${XRAY_WIN_PD_CLEAN_STALE:-1}"
CLEAN_ON_TIMEOUT="${XRAY_WIN_PD_CLEAN_ON_TIMEOUT:-1}"
LAST_TEST_LOG_TAIL="${XRAY_WIN_PD_LASTTEST_TAIL:-200}"
SKIP_SYNC="${XRAY_WIN_SKIP_SYNC:-0}"
SKIP_BUILD="${XRAY_WIN_SKIP_BUILD:-0}"
LOG_DIR="${XRAY_WIN_PD_LOG_DIR:-/tmp/xray-win-pd}"
REMOTE_BUILD_BAT="${XRAY_WIN_PD_BUILD_BAT:-C:\\workspace\\xray-pd-build.bat}"
REMOTE_REG_PS1="${XRAY_WIN_PD_REG_PS1:-C:\\workspace\\xray\\scripts\\win_pd_regression.ps1}"

MODE="ctest"
XRAY_TEST_PATH=""
CTEST_EXTRA=()

usage() {
    cat <<USAGE
Usage: scripts/win_pd_test.sh [options] [ctest args...]

Options:
  --no-sync              Do not sync source to the VM before running.
  --no-build             Do not build before running tests.
  --build-only           Sync/build only; do not run ctest.
  --xray-test PATH       Run xray.exe test PATH instead of ctest.
  --regression           Run the full regression suite via
                         scripts/win_pd_regression.ps1 inside the VM.
  --status               Print VM build/test process snapshot and exit.
  --kill                 Kill stale build/test processes in the VM and exit.
  --clean-stale          Kill stale build/test processes before running.
  --no-clean-stale       Do not kill stale build/test processes before running.
  -h, --help             Print this help.

Exit codes:
  0   pass
  1   test failure
  2   invocation error / VM unavailable
  124 wall timeout (script killed the inner command)
  127 prlctl missing

Environment:
  XRAY_PD_VM                       VM name, default: Windows 11
  XRAY_WIN_BUILD_WALL_TIMEOUT      Build wall timeout in seconds, default: 1800
  XRAY_WIN_TEST_WALL_TIMEOUT       ctest / xray-test wall timeout, default: 360
  XRAY_WIN_REG_WALL_TIMEOUT        regression wall timeout, default: 1800
  XRAY_WIN_REG_PER_TEST_TIMEOUT    regression per-test timeout, default: 30
  XRAY_WIN_PD_HEARTBEAT            Heartbeat interval in seconds, default: 20
  XRAY_WIN_PD_NO_OUTPUT_TIMEOUT    Print diagnostics after this many quiet seconds, default: 90
  XRAY_WIN_PD_CLEAN_STALE          Kill stale cmake/ctest/ninja/cl/link/xray first, default: 1
  XRAY_WIN_PD_LASTTEST_TAIL        Lines of LastTest.log to dump on ctest failure, default: 200
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --no-sync)
            SKIP_SYNC=1
            ;;
        --no-build)
            SKIP_BUILD=1
            ;;
        --build-only)
            MODE="none"
            ;;
        --xray-test)
            if [ $# -lt 2 ]; then
                echo "[win-pd] --xray-test requires a path" >&2
                exit 2
            fi
            MODE="xray-test"
            XRAY_TEST_PATH="$2"
            shift
            ;;
        --regression)
            MODE="regression"
            ;;
        --status)
            MODE="status"
            SKIP_SYNC=1
            SKIP_BUILD=1
            CLEAN_STALE=0
            ;;
        --kill)
            MODE="kill"
            SKIP_SYNC=1
            SKIP_BUILD=1
            CLEAN_STALE=0
            ;;
        --clean-stale)
            CLEAN_STALE=1
            ;;
        --no-clean-stale)
            CLEAN_STALE=0
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            while [ $# -gt 0 ]; do
                CTEST_EXTRA+=("$1")
                shift
            done
            break
            ;;
        *)
            CTEST_EXTRA+=("$1")
            ;;
    esac
    shift
done

mkdir -p "$LOG_DIR"

pd_exec() {
    prlctl exec "$PD_VM" "$@"
}

host_run_limited() {
    local limit="$1"
    shift
    "$@" &
    local pid=$!
    local elapsed=0
    while kill -0 "$pid" 2>/dev/null; do
        if [ "$elapsed" -ge "$limit" ]; then
            kill "$pid" 2>/dev/null || true
            sleep 1
            kill -9 "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
            return 124
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    set +e
    wait "$pid"
    local rc=$?
    set -e
    return "$rc"
}

ps_sq() {
    printf "%s" "$1" | sed "s/'/''/g"
}

cmd_quote() {
    printf "%s" "$1" | sed 's/"/\\"/g'
}

process_snapshot() {
    echo "[win-pd] process snapshot:"
    local ps_cmd
    ps_cmd="\$ErrorActionPreference='SilentlyContinue'; "
    ps_cmd+="\$names=@('cmake','ctest','ninja','cl','link','xray'); "
    ps_cmd+="\$items=@(); foreach(\$n in \$names){ \$items += @(Get-Process -Name \$n -ErrorAction SilentlyContinue) }; "
    ps_cmd+="if(\$items.Count -eq 0){ Write-Host '  no matching process'; exit 0 }; "
    ps_cmd+="\$items | Sort-Object ProcessName,Id | Select-Object ProcessName,Id,CPU,StartTime | Format-Table -AutoSize; "
    ps_cmd+="Write-Host '----- command lines -----'; "
    ps_cmd+="Get-CimInstance Win32_Process | Where-Object { \$names -contains \$_.Name.Replace('.exe','') } | "
    ps_cmd+="Sort-Object Name,ProcessId | Select-Object Name,ProcessId,CommandLine | Format-List"
    if ! host_run_limited "$SNAPSHOT_TIMEOUT" prlctl exec "$PD_VM" powershell -NoProfile -Command "$ps_cmd"; then
        echo "[win-pd] process snapshot unavailable within ${SNAPSHOT_TIMEOUT}s"
    fi
}

stop_stale_processes() {
    echo "[win-pd] stopping stale build/test processes in VM"
    local ps_cmd
    ps_cmd="\$ErrorActionPreference='SilentlyContinue'; "
    ps_cmd+="\$names=@('cmake','ctest','ninja','cl','link','xray'); "
    ps_cmd+="foreach(\$n in \$names){ Get-Process -Name \$n -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue }; exit 0"
    if ! host_run_limited "$SNAPSHOT_TIMEOUT" prlctl exec "$PD_VM" powershell -NoProfile -Command "$ps_cmd"; then
        echo "[win-pd] stale process cleanup timed out" >&2
        return 124
    fi
}

run_with_watchdog() {
    local label="$1"
    local limit="$2"
    shift 2
    local log="$LOG_DIR/${label}-$(date +%Y%m%d-%H%M%S).log"

    echo "[win-pd] ${label}: start (timeout=${limit}s, log=$log)"
    "$@" >"$log" 2>&1 &
    local pid=$!
    local start
    start=$(date +%s)
    local last_change="$start"
    local last_size=0
    local last_diag=0

    while kill -0 "$pid" 2>/dev/null; do
        sleep "$HEARTBEAT"
        local now elapsed idle size
        now=$(date +%s)
        elapsed=$((now - start))
        size=$(wc -c <"$log" 2>/dev/null || echo 0)
        if [ "$size" != "$last_size" ]; then
            last_size="$size"
            last_change="$now"
            idle=0
            echo "[win-pd] ${label}: running ${elapsed}s, log updated (${size} bytes)"
            tail -n 8 "$log" || true
        else
            idle=$((now - last_change))
            echo "[win-pd] ${label}: running ${elapsed}s, no new output for ${idle}s"
        fi

        if [ "$idle" -ge "$NO_OUTPUT_TIMEOUT" ] && [ $((now - last_diag)) -ge "$NO_OUTPUT_TIMEOUT" ]; then
            last_diag="$now"
            echo "[win-pd] ${label}: quiet for ${idle}s; checking whether VM is still busy"
            process_snapshot
        fi

        if [ "$elapsed" -ge "$limit" ]; then
            echo "[win-pd] ${label}: TIMEOUT after ${elapsed}s" >&2
            process_snapshot
            kill "$pid" 2>/dev/null || true
            sleep 1
            kill -9 "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
            if [ "$CLEAN_ON_TIMEOUT" = "1" ]; then
                stop_stale_processes || true
            fi
            echo "[win-pd] ${label}: last log lines"
            tail -n 80 "$log" || true
            return 124
        fi
    done

    set +e
    wait "$pid"
    local rc=$?
    set -e
    echo "[win-pd] ${label}: exit=$rc"
    tail -n 40 "$log" || true
    return "$rc"
}

ensure_vm() {
    if ! command -v prlctl >/dev/null 2>&1; then
        echo "[win-pd] prlctl not found" >&2
        exit 127
    fi
    if ! prlctl status "$PD_VM" >/dev/null 2>&1; then
        echo "[win-pd] VM not found: $PD_VM" >&2
        exit 2
    fi
    if ! prlctl status "$PD_VM" | grep -qi running; then
        echo "[win-pd] VM is not running: $PD_VM" >&2
        exit 2
    fi
    pd_exec cmd /c ver >/dev/null
}

write_build_bat() {
    # Build the .bat locally then ship it as base64 to bypass cmd.exe /
    # PowerShell quoting headaches when paths contain spaces (e.g. the
    # vcvars64.bat path under "C:\Program Files (x86)\..."). Using
    # WriteAllBytes preserves the embedded double quotes and uses CRLF
    # line endings that cmd.exe expects.
    local tmp
    tmp=$(mktemp -t xray_pd_build.XXXXXX.bat)
    {
        printf '@echo on\r\n'
        printf 'if not exist "%s" mkdir "%s"\r\n' "$REMOTE_BUILD" "$REMOTE_BUILD"
        printf 'call "%s" || exit /b 1\r\n' "$VCVARS"
        printf 'cmake -S "%s" -B "%s" -G Ninja -DCMAKE_BUILD_TYPE=%s %s || exit /b 1\r\n' \
            "$REMOTE_SRC" "$REMOTE_BUILD" "$BUILD_TYPE" "$CMAKE_EXTRA"
        printf 'ninja -C "%s"\r\n' "$REMOTE_BUILD"
    } > "$tmp"
    local b64
    b64=$(base64 < "$tmp" | tr -d '\n')
    rm -f "$tmp"
    local ps_cmd
    ps_cmd="[IO.File]::WriteAllBytes('$(ps_sq "$REMOTE_BUILD_BAT")', [Convert]::FromBase64String('$b64'))"
    pd_exec powershell -NoProfile -Command "$ps_cmd" >/dev/null
}

remote_repo_path() {
    local p="$1"
    if [[ "$p" == [A-Za-z]:* ]]; then
        printf "%s" "$p"
        return
    fi
    if [[ "$p" == "$PROJECT_ROOT"/* ]]; then
        p="${p#"$PROJECT_ROOT"/}"
    fi
    p="${p#./}"
    p="${p//\//\\}"
    printf "%s\\%s" "$REMOTE_SRC_WIN" "$p"
}

run_build() {
    write_build_bat
    run_with_watchdog "build" "$BUILD_TIMEOUT" prlctl exec "$PD_VM" cmd /c "$REMOTE_BUILD_BAT"
}

dump_last_test_log() {
    local tail_lines="$LAST_TEST_LOG_TAIL"
    local ps_cmd
    ps_cmd="\$p='${REMOTE_BUILD_WIN//\\/\\\\}\\\\Testing\\\\Temporary\\\\LastTest.log'; "
    ps_cmd+="if(Test-Path \$p){ Write-Host '----- LastTest.log (tail $tail_lines) -----'; Get-Content -LiteralPath \$p -Tail $tail_lines } "
    ps_cmd+="else{ Write-Host '[win-pd] LastTest.log not found in VM' }"
    if ! host_run_limited "$SNAPSHOT_TIMEOUT" prlctl exec "$PD_VM" powershell -NoProfile -Command "$ps_cmd"; then
        echo "[win-pd] LastTest.log fetch timed out"
    fi
}

run_ctest() {
    local args="$CTEST_ARGS"
    local a
    if [ "${#CTEST_EXTRA[@]}" -gt 0 ]; then
        for a in "${CTEST_EXTRA[@]}"; do
            args="$args \"$(cmd_quote "$a")\""
        done
    fi
    local cmd="cd /d \"$REMOTE_BUILD_WIN\" && ctest $args"
    set +e
    run_with_watchdog "ctest" "$TEST_TIMEOUT" prlctl exec "$PD_VM" cmd /c "$cmd"
    local rc=$?
    set -e
    if [ "$rc" != "0" ] && [ "$rc" != "124" ]; then
        dump_last_test_log
    fi
    return "$rc"
}

run_xray_test() {
    local target
    target=$(remote_repo_path "$XRAY_TEST_PATH")
    local cmd="\"$REMOTE_BUILD_WIN\\xray.exe\" test \"$target\""
    run_with_watchdog "xray-test" "$TEST_TIMEOUT" prlctl exec "$PD_VM" cmd /c "$cmd"
}

run_regression() {
    local cmd="powershell -NoProfile -ExecutionPolicy Bypass -File \"$REMOTE_REG_PS1\""
    cmd+=" -PerTestTimeout $REGRESSION_PER_TEST_TIMEOUT"
    run_with_watchdog "regression" "$REGRESSION_TIMEOUT" prlctl exec "$PD_VM" cmd /c "$cmd"
}

ensure_vm

case "$MODE" in
    status)
        process_snapshot
        exit 0
        ;;
    kill)
        stop_stale_processes
        exit 0
        ;;
esac

if [ "$CLEAN_STALE" = "1" ]; then
    process_snapshot
    stop_stale_processes
fi

if [ "$SKIP_SYNC" != "1" ]; then
    "$SCRIPT_DIR/win_sync.sh"
else
    echo "[win-pd] sync skipped"
fi

if [ "$SKIP_BUILD" != "1" ]; then
    run_build
else
    echo "[win-pd] build skipped"
fi

set +e
case "$MODE" in
    none)
        echo "[win-pd] test skipped"
        rc=0
        ;;
    ctest)
        run_ctest
        rc=$?
        ;;
    xray-test)
        run_xray_test
        rc=$?
        ;;
    regression)
        run_regression
        rc=$?
        ;;
    *)
        echo "[win-pd] unknown mode: $MODE" >&2
        exit 2
        ;;
esac
set -e

case "$rc" in
    0)   echo "[win-pd] result: PASS" ;;
    124) echo "[win-pd] result: TIMEOUT (wall clock hit)" ;;
    *)   echo "[win-pd] result: FAIL (exit=$rc)" ;;
esac
exit "$rc"

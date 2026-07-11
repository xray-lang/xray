#!/usr/bin/env bash
set -euo pipefail

XRAY="${1:-${XRAY:-./build/xray}}"
PYTHON_BIN="${2:-${PYTHON:-python3}}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

EMPTY_STDLIB="$WORK/empty-stdlib"
CACHE_ROOT="$WORK/diff-cache"
mkdir -p "$EMPTY_STDLIB" "$CACHE_ROOT"

cd "$ROOT"

check_output() {
    local label="$1"
    local expected="$2"
    shift 2
    local out="$WORK/$label.out"
    local err="$WORK/$label.err"

    if ! env XRAY_STDLIB_PATH="$EMPTY_STDLIB" "$@" >"$out" 2>"$err"; then
        echo "FAIL $label: command failed" >&2
        cat "$err" >&2
        return 1
    fi

    local actual
    actual="$(sed -e 's/[[:space:]]*$//' -e '/^$/d' "$out")"
    if [ "$actual" != "$expected" ]; then
        echo "FAIL $label: expected '$expected', got '$actual'" >&2
        cat "$err" >&2
        return 1
    fi
}

check_backend_diff() {
    local case_file="$1"
    env \
        XRAY_STDLIB_PATH="$EMPTY_STDLIB" \
        XRAY_TEST_CACHE_ROOT="$CACHE_ROOT" \
        XRAY_DIFF_SINGLE_CASE="$case_file" \
        XRAY_DIFF_MAX_AUTO_JOBS=1 \
        "$PYTHON_BIN" tests/diff/run_backend_diff_fast.py "$XRAY"
}

check_output "vm_path_empty_stdlib" "demo.xr" \
    "$XRAY" -e 'import path; print(path.basename("/tmp/demo.xr"))'
check_output "vm_http_empty_stdlib" "fn router" \
    "$XRAY" -e $'import http\nprint(http.router)'
check_output "vm_cluster_empty_stdlib" "true" \
    "$XRAY" -e $'import cluster\nprint(cluster.topicMatches("events.*", "events.user"))'

check_backend_diff "tests/diff/cases/semantics/stdlib/probe_module_shapes.xr"
check_backend_diff "tests/diff/cases/semantics/stdlib/http_pure_helpers_direct.xr"
check_backend_diff "tests/diff/cases/semantics/stdlib/cluster_protocol_pure_direct.xr"
check_backend_diff "tests/diff/cases/semantics/stdlib/parallel_api_reference.xr"
check_backend_diff "tests/diff/cases/semantics/stdlib/parallel_plan_close_lifecycle.xr"

echo "stdlib embedded layout tests passed"

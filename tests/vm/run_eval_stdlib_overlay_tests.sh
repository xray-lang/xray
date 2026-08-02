#!/usr/bin/env bash
set -euo pipefail

XRAY="${1:-${XRAY:-./build/xray}}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
EMPTY_STDLIB="$WORK/empty-stdlib"
mkdir -p "$EMPTY_STDLIB"

cd "$ROOT"

check_output() {
    local label="$1"
    local expected="$2"
    local out="$WORK/$label.out"
    local err="$WORK/$label.err"
    shift 2

    if ! "$@" >"$out" 2>"$err"; then
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

check_stdin_output() {
    local label="$1"
    local mode="$2"
    local expected="$3"
    local code="$4"
    local out="$WORK/$label.out"
    local err="$WORK/$label.err"

    if ! printf '%s\n' "$code" | "$XRAY" "$mode" - >"$out" 2>"$err"; then
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

check_output "eval_http_whole" "<fn>" \
    "$XRAY" -e $'import http\nprint(http.router)'
check_output "eval_http_selective" "<fn>" \
    "$XRAY" -e $'import { router } from http\nprint(router)'
check_stdin_output "eval_cluster_whole" "eval" "true" \
    $'import cluster\nprint(cluster.topicMatches("events.*", "events.user"))'
check_stdin_output "eval_cluster_selective" "eval" "true" \
    $'import { topicMatches } from cluster\nprint(topicMatches("events.*", "events.user"))'

check_stdin_output "eval_stdin_cluster" "eval" "true" \
    $'import cluster\nprint(cluster.topicMatches("events.*", "events.user"))'
check_stdin_output "run_stdin_cluster" "run" "true" \
    $'import cluster\nprint(cluster.topicMatches("events.*", "events.user"))'

check_output "eval_embedded_http_empty_stdlib_path" "<fn>" \
    env XRAY_STDLIB_PATH="$EMPTY_STDLIB" "$XRAY" -e $'import http\nprint(http.router)'
if ! printf '%s\n' $'import cluster\nprint(cluster.topicMatches("events.*", "events.user"))' | \
    env XRAY_STDLIB_PATH="$EMPTY_STDLIB" "$XRAY" eval - >"$WORK/eval_embedded_cluster_empty_stdlib_path.out" 2>"$WORK/eval_embedded_cluster_empty_stdlib_path.err"; then
    echo "FAIL eval_embedded_cluster_empty_stdlib_path: command failed" >&2
    cat "$WORK/eval_embedded_cluster_empty_stdlib_path.err" >&2
    exit 1
fi
if [ "$(sed -e 's/[[:space:]]*$//' -e '/^$/d' "$WORK/eval_embedded_cluster_empty_stdlib_path.out")" != "true" ]; then
    echo "FAIL eval_embedded_cluster_empty_stdlib_path: expected 'true'" >&2
    exit 1
fi

echo "eval stdlib overlay tests passed"

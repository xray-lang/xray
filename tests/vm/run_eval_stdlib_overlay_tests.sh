#!/usr/bin/env bash
set -euo pipefail

XRAY="${1:-${XRAY:-./build/xray}}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

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

check_output "eval_http_whole" "fn router" \
    "$XRAY" -e $'import http\nprint(http.router)'
check_output "eval_http_selective" "fn router" \
    "$XRAY" -e $'import { router } from http\nprint(router)'
check_output "eval_cluster_whole" "true" \
    "$XRAY" -e $'import cluster\nprint(cluster.topicMatches("events.*", "events.user"))'
check_output "eval_cluster_selective" "true" \
    "$XRAY" -e $'import { topicMatches } from cluster\nprint(topicMatches("events.*", "events.user"))'

check_stdin_output "eval_stdin_cluster" "eval" "true" \
    $'import cluster\nprint(cluster.topicMatches("events.*", "events.user"))'
check_stdin_output "run_stdin_cluster" "run" "true" \
    $'import cluster\nprint(cluster.topicMatches("events.*", "events.user"))'

echo "eval stdlib overlay tests passed"

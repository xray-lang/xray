#!/usr/bin/env bash
set -euo pipefail

RUNNER="${1:-}"
ARCHIVE="${2:-}"

if [ -z "$RUNNER" ] || [ -z "$ARCHIVE" ]; then
    echo "usage: $0 <bytecode-embed-runner> <libxray_vm_runtime.a>" >&2
    exit 2
fi

if [ ! -x "$RUNNER" ]; then
    echo "runner not executable: $RUNNER" >&2
    exit 2
fi

if [ ! -f "$ARCHIVE" ]; then
    echo "archive not found: $ARCHIVE" >&2
    exit 2
fi

PASSED=0
FAILED=0

record_pass() {
    printf '  PASS: %s\n' "$1"
    PASSED=$((PASSED + 1))
}

record_fail() {
    printf '  FAIL: %s\n' "$1"
    FAILED=$((FAILED + 1))
}

check_no_symbols() {
    local file="$1"
    local label="$2"
    local out
    out="$(nm -g "$file" 2>/dev/null || true)"
    if printf '%s\n' "$out" | grep -E '(^|[[:space:]])_?(xr_parse|xr_compile|xa_analyzer|xanalyzer_|xi_|xr_aot_|xray_build|xr_bundle|xr_module_graph)' >/tmp/xray_bytecode_embed_symbols.$$; then
        record_fail "$label has compiler/toolchain symbols"
        sed 's/^/    /' /tmp/xray_bytecode_embed_symbols.$$
    else
        record_pass "$label has no compiler/toolchain symbols"
    fi
    rm -f /tmp/xray_bytecode_embed_symbols.$$
}

printf '=== Bytecode Embed Symbol Gate ===\n'
printf 'Runner:  %s\n' "$RUNNER"
printf 'Archive: %s\n\n' "$ARCHIVE"

if "$RUNNER" >/tmp/xray_bytecode_embed_runner.out.$$ 2>/tmp/xray_bytecode_embed_runner.err.$$; then
    record_pass "bytecode embed runner executes"
else
    rc=$?
    record_fail "bytecode embed runner executes"
    sed 's/^/    stdout: /' /tmp/xray_bytecode_embed_runner.out.$$
    sed 's/^/    stderr: /' /tmp/xray_bytecode_embed_runner.err.$$
    printf '    exit: %s\n' "$rc"
fi
rm -f /tmp/xray_bytecode_embed_runner.out.$$ /tmp/xray_bytecode_embed_runner.err.$$

check_no_symbols "$RUNNER" "bytecode embed runner"
check_no_symbols "$ARCHIVE" "xray_vm_runtime archive"

printf '\n=== Results: %d passed, %d failed ===\n' "$PASSED" "$FAILED"

if [ "$FAILED" -ne 0 ]; then
    exit 1
fi

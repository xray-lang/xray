#!/bin/bash
set -euo pipefail

XRAY=${1:?usage: run_verify_contract_tests.sh /path/to/xray /path/to/fixture}
FIXTURE=${2:?usage: run_verify_contract_tests.sh /path/to/xray /path/to/fixture}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/xray-verify-contract.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

cd "$FIXTURE"

"$XRAY" verify --contract positive.toml >"$WORK/positive.out" 2>"$WORK/positive.err"
grep -q 'verified symbol-id=' "$WORK/positive.out"

if "$XRAY" verify --contract negative-effect.toml >"$WORK/effect.out" 2>"$WORK/effect.err"; then
    echo "allocation contract unexpectedly passed" >&2
    exit 1
fi
grep -q "contract 'allocates' failed" "$WORK/effect.err"
grep -q 'effect summary' "$WORK/effect.err"

if "$XRAY" verify --contract negative-schema.toml >"$WORK/schema.out" 2>"$WORK/schema.err"; then
    echo "unknown contract key unexpectedly passed" >&2
    exit 1
fi
grep -q "unknown contract key 'legacy_allow'" "$WORK/schema.err"

echo "PASS: versioned verify contract succeeds and fails closed"

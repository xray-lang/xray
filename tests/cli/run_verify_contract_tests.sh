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

if "$XRAY" verify --contract negative-order.toml >"$WORK/order.out" 2>"$WORK/order.err"; then
    echo "ordered negative contract unexpectedly passed" >&2
    exit 1
fi
grep -q 'symbol=pure scope=semantic' "$WORK/order.out"
grep -q "contract 'allocates' failed" "$WORK/order.err"

if "$XRAY" verify --contract negative-schema.toml >"$WORK/schema.out" 2>"$WORK/schema.err"; then
    echo "unknown contract key unexpectedly passed" >&2
    exit 1
fi
grep -q "unknown contract key 'legacy_allow'" "$WORK/schema.err"

# A bodyless extern "C" with no audited native contract is not evidence of
# anything.  LANGUAGE_SPEC 5.2.11 requires native unknowns to fail closed.
if "$XRAY" verify --contract negative-native-unknown.toml >"$WORK/native.out" 2>"$WORK/native.err"; then
    echo "uncontracted extern \"C\" call unexpectedly proved a semantic contract" >&2
    exit 1
fi
grep -q "contract 'callsUncontractedNative' failed" "$WORK/native.err"
grep -q 'proof incomplete' "$WORK/native.err"

# A requirement with no inference source must be rejected, not granted.
if "$XRAY" verify --contract negative-uninferred.toml >"$WORK/uninferred.out" 2>"$WORK/uninferred.err"; then
    echo "requirement without an inference source unexpectedly passed" >&2
    exit 1
fi
grep -q 'has no inference source' "$WORK/uninferred.err"

echo "PASS: versioned verify contract succeeds and fails closed"

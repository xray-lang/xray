#!/usr/bin/env bash
set -euo pipefail

XRAY_BIN="${1:?usage: run_no_throw_contract_tests.sh /path/to/xray}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FIXTURE_DIR="$ROOT_DIR/tests/aot/filetests/cgen"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/xray-no-throw.XXXXXX")"
trap 'rm -rf "$TMP_DIR"' EXIT

# The assertion is proof-only metadata: compiling the same canonical path with
# or without it must produce byte-identical C.
cp "$FIXTURE_DIR/no_throw_equivalence_plain.xr" "$TMP_DIR/equivalence.xr"
"$XRAY_BIN" build --native --c-only --output "$TMP_DIR/plain.c" \
    "$TMP_DIR/equivalence.xr" >/dev/null
cp "$FIXTURE_DIR/no_throw_equivalence_asserted.xr" "$TMP_DIR/equivalence.xr"
"$XRAY_BIN" build --native --c-only --output "$TMP_DIR/asserted.c" \
    "$TMP_DIR/equivalence.xr" >/dev/null
if ! cmp -s "$TMP_DIR/plain.c" "$TMP_DIR/asserted.c"; then
    diff -u "$TMP_DIR/plain.c" "$TMP_DIR/asserted.c" || true
    echo "@no_throw changed generated C for an already-proven function" >&2
    exit 1
fi

HOF_FIXTURE="$FIXTURE_DIR/no_throw_hof_specialization.xr"
"$XRAY_BIN" build --native --c-only --dump-xaot-plan \
    --output "$TMP_DIR/hof.c" "$HOF_FIXTURE" >"$TMP_DIR/hof.plan"

variant_count="$(grep -Ec '^function [0-9]+ name=apply\$i64(\$nothrow)? ' "$TMP_DIR/hof.plan")"
if [[ "$variant_count" != "2" ]]; then
    echo "generic HOF must have exactly two effect variants, got $variant_count" >&2
    exit 1
fi

plan_function() {
    local name="$1"
    awk -v target="$name" '
        /^function [0-9]+ name=/ {
            if (inside) exit
            inside = (index($0, " name=" target " ") != 0)
        }
        inside { print }
    ' "$TMP_DIR/hof.plan"
}

if plan_function 'apply$i64$nothrow' | grep -q 'op=ERR_CHECK'; then
    echo "NO_THROW HOF specialization retained ERR_CHECK" >&2
    exit 1
fi
if ! plan_function 'apply$i64' | grep -q 'op=ERR_CHECK'; then
    echo "MAY_THROW HOF specialization lost ERR_CHECK" >&2
    exit 1
fi
if plan_function 'xray_apply_pure' | grep -q 'op=ERR_CHECK'; then
    echo "pure HOF call path retained ERR_CHECK" >&2
    exit 1
fi
if ! plan_function 'xray_apply_checked' | grep -q 'op=ERR_CHECK'; then
    echo "throwing HOF call path lost ERR_CHECK" >&2
    exit 1
fi

"$XRAY_BIN" build --native --output "$TMP_DIR/hof" "$HOF_FIXTURE" >/dev/null
"$TMP_DIR/hof"

echo "no-throw contract tests passed"

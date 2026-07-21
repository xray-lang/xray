#!/bin/bash
set -euo pipefail

XRAY=${1:?usage: run_fix_no_throw_tests.sh /path/to/xray}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/xray-fix-no-throw.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

cat >"$WORK/valid.xr" <<'XR'
enum FixError { Boom }

fn pure(value: int) -> int {
    return value + 1
}

fn fallible() {
    throw FixError.Boom
}

@no_throw
fn alreadyPure() -> int {
    return 7
}
XR

"$XRAY" fix --annotate-no-throw "$WORK/valid.xr" >"$WORK/first.out"
grep -q "Annotated 1 function" "$WORK/first.out"
[ "$(grep -c '^@no_throw$' "$WORK/valid.xr")" -eq 2 ]
grep -q '^fn fallible' "$WORK/valid.xr"

cp "$WORK/valid.xr" "$WORK/after-first.xr"
"$XRAY" fix --annotate-no-throw "$WORK/valid.xr" >"$WORK/second.out"
cmp "$WORK/valid.xr" "$WORK/after-first.xr"
grep -q "No provably no-throw functions require annotation" "$WORK/second.out"

cat >"$WORK/invalid.xr" <<'XR'
fn broken( {
XR
cp "$WORK/invalid.xr" "$WORK/invalid-before.xr"
if "$XRAY" fix --annotate-no-throw "$WORK/invalid.xr" >"$WORK/invalid.out" 2>&1; then
    echo "invalid source was unexpectedly accepted" >&2
    exit 1
fi
cmp "$WORK/invalid.xr" "$WORK/invalid-before.xr"

echo "PASS: fix --annotate-no-throw is selective, idempotent, and fail-closed"

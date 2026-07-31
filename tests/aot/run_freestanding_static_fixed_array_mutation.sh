#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
XRAY=${1:-$ROOT/build/xray}
CC=${CC:-cc}
SOURCE="$ROOT/tests/aot/filetests/link/freestanding_top_var_fixed_array_static.xr"
MANIFEST="$ROOT/tests/aot/filetests/link/freestanding_top_var_fixed_array_static.toml"
HARNESS="$ROOT/tests/aot/provider/freestanding_static_fixed_array_mutation.c"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/xray-freestanding-fixed-array-mutation.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

cp "$SOURCE" "$WORK/main.xr"
cp "$MANIFEST" "$WORK/xray.toml"

(
    cd "$WORK"
    "$XRAY" build --native --profile freestanding --rebuild -c \
        -o "$WORK/generated.c" main.xr
)

grep -Fq 'static int64_t _xctarr_' "$WORK/generated.c"
grep -Fq 'static struct { int64_t id; int64_t ticks; int64_t scratch[2]; } _xctarr_' \
    "$WORK/generated.c"
if grep -Fq 'static const int64_t _xctarr_' "$WORK/generated.c"; then
    echo "FAIL: mutable fixed-array storage was emitted const" >&2
    exit 1
fi
if grep -Fq 'xrt_shared[' "$WORK/generated.c"; then
    echo "FAIL: mutable fixed-array storage escaped through xrt_shared" >&2
    exit 1
fi

"$CC" -std=c11 -Wall -Wextra -Werror -Dmain=xray_generated_main \
    -I"$ROOT/src/aot" -c "$WORK/generated.c" -o "$WORK/generated.o"
"$CC" -std=c11 -Wall -Wextra -Werror \
    "$HARNESS" "$WORK/generated.o" -o "$WORK/mutation-harness"
"$WORK/mutation-harness"

echo "PASS: freestanding mutable scalar/matrix/cube/struct fixed arrays persist across calls"

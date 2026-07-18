#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "usage: $0 <xray> <entry.xr> <layout-lib.xr>" >&2
    exit 2
fi

XRAY_BIN=$1
ENTRY_SRC=$2
LAYOUT_LIB=$3
WORK=$(mktemp -d "${TMPDIR:-/tmp}/xray-layout-bytecode.XXXXXX")
trap 'rm -rf -- "$WORK"' EXIT

cp "$ENTRY_SRC" "$WORK/entry.xr"
cp "$LAYOUT_LIB" "$WORK/_extern_layout_bytecode_lib.xr"

"$XRAY_BIN" run "$WORK/entry.xr" >"$WORK/source.out"
"$XRAY_BIN" compile -f bytecode -o "$WORK/entry-a.xrc" "$WORK/entry.xr"
"$XRAY_BIN" compile -f bytecode -o "$WORK/entry-b.xrc" "$WORK/entry.xr"
cmp "$WORK/entry-a.xrc" "$WORK/entry-b.xrc"
"$XRAY_BIN" run "$WORK/entry-a.xrc" >"$WORK/bytecode.out"

cmp "$WORK/source.out" "$WORK/bytecode.out"

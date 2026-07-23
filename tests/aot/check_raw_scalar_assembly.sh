#!/bin/bash
# Verify the optimized native shape of unchecked scalar memory access.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
XRAY="${1:-$PROJECT_DIR/build/xray}"
FIXTURE="$SCRIPT_DIR/filetests/cgen/mem_load_store_rawptr_shape.xr"
MANIFEST="${FIXTURE%.xr}.toml"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray-raw-scalar-asm.XXXXXX")" || exit 1
trap 'rm -rf "$WORK"' EXIT

cp "$FIXTURE" "$WORK/mem_load_store_rawptr_shape.xr"
cp "$MANIFEST" "$WORK/xray.toml"

if ! "$XRAY" build --native -O2 --shared --rebuild -o "$WORK/raw-scalar" \
        "$WORK/mem_load_store_rawptr_shape.xr" \
        >"$WORK/build.log" 2>&1; then
    echo "raw scalar assembly gate: native build failed" >&2
    sed -n '1,160p' "$WORK/build.log" >&2
    exit 1
fi

if command -v llvm-objdump >/dev/null 2>&1; then
    llvm-objdump --no-show-raw-insn -d "$WORK/raw-scalar" >"$WORK/disassembly"
elif command -v objdump >/dev/null 2>&1; then
    objdump -d "$WORK/raw-scalar" >"$WORK/disassembly"
elif command -v otool >/dev/null 2>&1; then
    otool -tvV "$WORK/raw-scalar" >"$WORK/disassembly"
else
    echo "raw scalar assembly gate: no supported disassembler; skipped"
    exit 77
fi

extract_symbol() {
    symbol="$1"
    awk -v symbol="$symbol" '
        $0 ~ "<_?" symbol ">:" || $0 ~ "^_?" symbol ":" {
            active = 1
            print
            next
        }
        active && ($0 ~ /<[A-Za-z_.$][A-Za-z0-9_.$]*>:/ ||
                   $0 ~ /^_?[A-Za-z_.$][A-Za-z0-9_.$]*:/) { exit }
        active { print }
    ' "$WORK/disassembly"
}

check_straight_line_symbol() {
    symbol="$1"
    body="$WORK/$symbol.asm"
    extract_symbol "$symbol" >"$body"
    if [ ! -s "$body" ]; then
        echo "raw scalar assembly gate: missing symbol $symbol" >&2
        exit 1
    fi
    if grep -Eiq '[[:space:]](call[a-z]*|bl|blr|cbz|cbnz|tbz|tbnz|b\.[a-z]+|j[a-z]+)[[:space:]]' \
            "$body"; then
        echo "raw scalar assembly gate: $symbol contains a call or conditional branch" >&2
        sed -n '1,80p' "$body" >&2
        exit 1
    fi
    if grep -Eq 'xr_array_core|xrt_(ptr|endian_arg|has_pending_error)' "$body"; then
        echo "raw scalar assembly gate: $symbol re-entered a checked/runtime helper" >&2
        sed -n '1,80p' "$body" >&2
        exit 1
    fi
}

check_straight_line_symbol xray_mem_u32_load_le
check_straight_line_symbol xray_mem_u32_store_le

echo "raw scalar assembly gate: straight-line unchecked load/store PASS"

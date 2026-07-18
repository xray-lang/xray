#!/usr/bin/env bash
set -euo pipefail

XRAY=${1:-./build/xray}
CC=${CC:-cc}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CASE="$ROOT/tests/aot/provider/freestanding_coro/main.xr"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/xray-provider-abi.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

"$XRAY" build --native --target native -c -o "$TMP/provider.c" "$CASE" >/dev/null

if grep -Eq 'xrt_runtime_value_ops|runtime_cfg\.value_ops' "$TMP/provider.c"; then
    echo "freestanding provider object emitted the hosted value-ops bridge" >&2
    exit 1
fi

"$CC" -std=c11 -ffreestanding -fno-stack-protector \
    -DXRAY_TARGET_RUNTIME_PROVIDER=1 \
    -DXRAY_PROVIDER_ABI=1 \
    -DXRAY_PROVIDER_REQUIRED_CAPS=0x1021 \
    -DXRAY_PROVIDER_REQUIRED_HOOKS=0x17 \
    -DXRAY_PROVIDER_TARGET_METADATA_HASH=1 \
    -I "$ROOT/src/aot" -c "$TMP/provider.c" -o "$TMP/provider.o"

nm -u "$TMP/provider.o" | awk '{print $NF}' | sed 's/^_//' | sort -u > "$TMP/undefined.txt"

cat > "$TMP/allowed.txt" <<'EOF'
memcpy
memset
xr_aot_await_task
xr_aot_await_task_resume
xr_aot_frame_alloc
xr_aot_frame_free
xr_aot_run_main
xr_aot_runtime_config_init
xr_aot_runtime_delete
xr_aot_runtime_new
xr_aot_spawn
xr_aot_trace_frame_value
xr_runtime_core_enable_object_destroy_ops
xr_runtime_core_enable_task_destroy_ops
xrt_closure_new
EOF

if ! diff -u "$TMP/allowed.txt" "$TMP/undefined.txt"; then
    echo "freestanding provider object escaped its declared ABI" >&2
    exit 1
fi

if grep -Eiq '^(pthread|malloc$|free$|epoll|kqueue|netpoll|xray_rt_coro|xray_core)' \
    "$TMP/undefined.txt"; then
    echo "freestanding provider object references a hosted runtime symbol" >&2
    exit 1
fi

echo "freestanding provider ABI test passed"

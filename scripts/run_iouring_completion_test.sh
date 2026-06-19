#!/usr/bin/env bash
# ===========================================================================
# io_uring Completion-Mode Test Runner (Linux only)
#
# Compiles tests/manual/iouring_completion_test.c against the built
# libxray_core.a and runs it. Verifies the io_uring completion submit/reap
# primitives (xr_netpoll_uring_submit_recv/_send). Skips cleanly when not on
# Linux or when liburing / the static lib is unavailable.
#
# In a container, io_uring syscalls must be permitted:
#   docker run --security-opt seccomp=unconfined ...
# ===========================================================================

set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

skip() { echo "SKIP — $1"; exit 0; }

[ "$(uname -s)" = "Linux" ] || skip "io_uring is Linux-only (host is $(uname -s))"
echo '#include <liburing.h>' | cc -E - >/dev/null 2>&1 || skip "liburing.h not found (install liburing-dev)"

# Find a static lib built with io_uring support.
LIB=""
for d in "${XRAY_BUILD_DIR:-}" "${PROJECT_ROOT}/build-linux" "${PROJECT_ROOT}/build"; do
    [ -n "$d" ] && [ -f "${d}/libxray_core.a" ] && { LIB="${d}/libxray_core.a"; break; }
done
[ -n "$LIB" ] || skip "libxray_core.a not found (build first, e.g. cmake -B build-linux && cmake --build build-linux)"
echo "Using lib: ${LIB}"

SRC="${PROJECT_ROOT}/tests/manual/iouring_completion_test.c"
BIN="$(mktemp -d)/iouring_completion_test"

# TLS libs are optional; link them only if present so the archive resolves.
EXTRA=""
for l in ssl crypto z; do
    if echo "int main(){return 0;}" | cc -x c - -l"$l" -o /dev/null 2>/dev/null; then
        EXTRA="${EXTRA} -l${l}"
    fi
done

set -x
cc -O2 -DXR_HAS_IO_URING "$SRC" -o "$BIN" \
    -Wl,--start-group "$LIB" -luring -lpthread -lm ${EXTRA} -Wl,--end-group
set +x

"$BIN"
rc=$?
rm -rf "$(dirname "$BIN")"
exit $rc

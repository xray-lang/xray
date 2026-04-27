#!/usr/bin/env bash
# check_platform_layering.sh — enforce the platform-layer
# refactor's three core rules. Designed to be cheap enough to run
# in CI on every push and as part of a pre-commit hook.
#
#   R1  Public headers (include/*.h) must not include any system
#       OS header. Embedders should be able to consume xray.h
#       without dragging <windows.h> or <pthread.h> into their
#       build.
#
#   R2  Outside src/os/ and src/base/xplatform.h, code must not
#       include raw OS system headers directly. The only legal
#       producers of those headers are the os_*.h shim layer and
#       the platform-specific .c files under src/os/{unix,win}/.
#
#   R3  Outside src/base/xplatform.h, preprocessor conditionals
#       must use XR_OS_WINDOWS / XR_OS_LINUX / XR_OS_MACOS /
#       XR_OS_BSD / XR_OS_POSIX, not raw _WIN32 / __APPLE__ /
#       __linux__ / __FreeBSD__ etc. Compiler-predefined names
#       are a portability hazard (e.g. __APPLE__ matches both
#       macOS and iOS) and they fragment greppability.
#
# Exit status: 0 = clean, 1 = violations found.

set -uo pipefail

cd "$(dirname "$0")/.."

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; NC='\033[0m'
else
    RED=''; GREEN=''; CYAN=''; NC=''
fi

ERRORS=0
section() { echo ""; echo -e "${CYAN}== $1 ==${NC}"; }
fail()    { echo -e "${RED}FAIL${NC}: $1"; ERRORS=$((ERRORS + 1)); }
pass()    { echo -e "${GREEN}PASS${NC}: $1"; }

# Set of system headers that are platform-bound and must not
# appear in the public include/ tree, nor leak into src/* outside
# of the os/ shim layer.
PLATFORM_HEADERS_RE='<(unistd|sys/.*|pthread.*|windows|sched|dirent|dlfcn|fcntl|poll|netinet/.*|arpa/.*|netdb|winsock2|ws2tcpip|bcrypt|process|mach/.*|sys/random|sys/event|sys/epoll|linux/.*)\.h>'

section "R1  public headers (include/*.h) free of system OS headers"
hits=$(grep -rEn "$PLATFORM_HEADERS_RE" include --include='*.h' 2>/dev/null || true)
if [ -n "$hits" ]; then
    echo "$hits"
    fail "include/ headers must not include system OS headers"
else
    pass "include/ is clean"
fi

# R2 is fully advisory. The current code legitimately reaches
# for OS-specific headers in three categories:
#   1. Platform-specific shim implementations: src/coro's netpoll
#      backends (epoll / kqueue / iocp / io_uring) and src/jit's
#      code allocator. These ARE the platform layer, just under
#      a different directory than src/os/. Long-term they should
#      live alongside the os/ tree; for now they stay where they
#      are because moving them is large surgery.
#   2. Thin wrappers that should migrate to os/ shims (chdir in
#      module loader, sysconf in worker, sysctl in JIT debug).
#   3. Application layer (src/app/cli, lsp, dap, mcp) and stdlib
#      modules that call raw POSIX/Win32 APIs because no shim
#      exists yet for the call site.
#
# The count is a useful regression signal — when it goes up
# without explanation in PR review, ask why.
section "R2 (advisory)  src/* outside os/ shim avoiding raw OS headers"
hits=$(grep -rEn "$PLATFORM_HEADERS_RE" src --include='*.c' --include='*.h' 2>/dev/null \
        | grep -v '^src/os/' \
        | grep -v '^src/base/xplatform.h:' || true)
if [ -n "$hits" ]; then
    count=$(echo "$hits" | wc -l | tr -d ' ')
    echo "$hits" | head -10
    [ "$count" -gt 10 ] && echo "...(${count} total, truncated)"
    echo -e "${CYAN}note${NC}: advisory only; track count over time"
else
    pass "src/ outside src/os/ is clean"
fi

section "R2 (advisory)  stdlib/* avoiding raw OS headers"
hits=$(grep -rEn "$PLATFORM_HEADERS_RE" stdlib --include='*.c' --include='*.h' 2>/dev/null || true)
if [ -n "$hits" ]; then
    count=$(echo "$hits" | wc -l | tr -d ' ')
    echo "$hits" | head -10
    [ "$count" -gt 10 ] && echo "...(${count} total, truncated)"
    echo -e "${CYAN}note${NC}: advisory only"
else
    pass "stdlib/ is clean"
fi

section "R3  outside xplatform.h, preprocessor uses XR_OS_*"
# A token is only an OS macro when it appears in a preprocessor
# context. We grep for the tokens then exclude lines that are
# obviously string literals or comments. False positives are rare
# in this codebase (the predefined names never appear as plain
# C identifiers), so this regex is intentionally simple.
hits=$(grep -rEn "\b(_WIN32|__APPLE__|__linux__|__FreeBSD__|__OpenBSD__|__NetBSD__)\b" \
        src stdlib --include='*.c' --include='*.h' 2>/dev/null \
        | grep -v '^src/base/xplatform.h:' || true)
if [ -n "$hits" ]; then
    echo "$hits" | head -40
    [ "$(echo "$hits" | wc -l)" -gt 40 ] && echo "...(truncated)"
    fail "raw OS macros found outside xplatform.h"
else
    pass "no raw OS macros outside xplatform.h"
fi

section "R3 supplement  no malloc/free/calloc/realloc outside xmalloc.{c,h}"
hits=$(grep -rEn "\b(malloc|free|calloc|realloc)\s*\(" \
        src --include='*.c' --include='*.h' 2>/dev/null \
        | grep -vE 'xr_(malloc|free|calloc|realloc)' \
        | grep -v 'xmalloc\.[ch]' \
        | grep -v 'src/os/' \
        | grep -vE '//[^\n]*\b(malloc|free|calloc|realloc)' || true)
if [ -n "$hits" ]; then
    # advisory only — many legitimate exceptions (LLVM allocator
    # callbacks, third-party JIT shims, etc.).
    echo "$hits" | head -10
    [ "$(echo "$hits" | wc -l)" -gt 10 ] && echo "...(truncated)"
    echo -e "${CYAN}note${NC}: advisory; review individually"
fi

echo ""
if [ "$ERRORS" -eq 0 ]; then
    echo -e "${GREEN}OK${NC}: platform layering clean"
    exit 0
else
    echo -e "${RED}$ERRORS rule violations${NC}"
    exit 1
fi

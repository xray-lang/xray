#!/bin/bash
# test_cross_platform.sh - Local cross-platform testing via Docker
#
# Usage:
#   ./scripts/test_cross_platform.sh              # test all platforms
#   ./scripts/test_cross_platform.sh linux-x64     # test Linux x86-64 only
#   ./scripts/test_cross_platform.sh linux-arm64   # test Linux ARM64 only
#   ./scripts/test_cross_platform.sh native        # test macOS native only
#   ./scripts/test_cross_platform.sh windows       # cross-compile for Windows x64
#   ./scripts/test_cross_platform.sh linux-x64-cross # cross-compile for Linux x64 (fast)
#   ./scripts/test_cross_platform.sh macos-x64     # macOS x64 via Rosetta 2
#
# Prerequisites: Docker Desktop with buildx (multi-arch support)

set -euo pipefail

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Docker images (cached after first run)
DOCKER_IMAGE="xray-build"
DOCKER_TAG="ubuntu2404"
DOCKER_MINGW_IMAGE="xray-mingw"
DOCKER_MINGW_TAG="latest"

# APT mirror (set to empty string to use default)
# Common mirrors: mirrors.tuna.tsinghua.edu.cn, mirrors.aliyun.com, mirrors.ustc.edu.cn
APT_MIRROR="${XRAY_APT_MIRROR:-mirrors.tuna.tsinghua.edu.cn}"

PLATFORM="${1:-all}"
PASSED=0
FAILED=0
SKIPPED=0

# ============================================================
# Helper functions
# ============================================================

log_header() {
    echo ""
    echo -e "${BLUE}════════════════════════════════════════════════════${NC}"
    echo -e "${BLUE}  $1${NC}"
    echo -e "${BLUE}════════════════════════════════════════════════════${NC}"
}

log_pass() { echo -e "  ${GREEN}✓ $1${NC}"; PASSED=$((PASSED + 1)); }
log_fail() { echo -e "  ${RED}✗ $1${NC}"; FAILED=$((FAILED + 1)); }
log_skip() { echo -e "  ${YELLOW}○ $1 (skipped)${NC}"; SKIPPED=$((SKIPPED + 1)); }

# ============================================================
# Build Docker image (once, cached)
# ============================================================

# Generate apt mirror sed commands for Dockerfile
apt_mirror_sed() {
    if [ -n "${APT_MIRROR}" ]; then
        echo "RUN sed -i \"s|http://ports.ubuntu.com|http://${APT_MIRROR}|g\" /etc/apt/sources.list.d/ubuntu.sources && \\"
        echo "    sed -i \"s|http://archive.ubuntu.com|http://${APT_MIRROR}|g\" /etc/apt/sources.list.d/ubuntu.sources && \\"
        echo "    sed -i \"s|http://security.ubuntu.com|http://${APT_MIRROR}|g\" /etc/apt/sources.list.d/ubuntu.sources"
    fi
}

build_docker_image() {
    local platform="$1"   # linux/amd64 or linux/arm64
    local arch_tag
    arch_tag=$(echo "$platform" | tr '/' '-')  # linux-amd64 or linux-arm64
    local full_tag="${DOCKER_IMAGE}:${DOCKER_TAG}-${arch_tag}"

    if docker image inspect "${full_tag}" &>/dev/null; then
        echo "  Docker image ${full_tag} already exists (cached)"
        return 0
    fi

    log_header "Building Docker image for ${platform} (one-time setup)..."

    local MIRROR_CMD=""
    if [ -n "${APT_MIRROR}" ]; then
        MIRROR_CMD="RUN sed -i \"s|http://ports.ubuntu.com|http://${APT_MIRROR}|g\" /etc/apt/sources.list.d/ubuntu.sources && sed -i \"s|http://archive.ubuntu.com|http://${APT_MIRROR}|g\" /etc/apt/sources.list.d/ubuntu.sources && sed -i \"s|http://security.ubuntu.com|http://${APT_MIRROR}|g\" /etc/apt/sources.list.d/ubuntu.sources"
    fi

    docker buildx build --platform "${platform}" --load \
        -t "${full_tag}" -f - "${PROJECT_ROOT}" <<DOCKERFILE
FROM ubuntu:24.04
${MIRROR_CMD}
RUN apt-get update && apt-get install -y --no-install-recommends \\
    cmake gcc g++ make zlib1g-dev libssl-dev \\
    && rm -rf /var/lib/apt/lists/*
WORKDIR /xray
DOCKERFILE
}

build_linux_cross_image() {
    local full_tag="xray-cross-x64:latest"

    if docker image inspect "${full_tag}" &>/dev/null; then
        echo "  Docker image ${full_tag} already exists (cached)"
        return 0
    fi

    log_header "Building Linux x64 cross-compile Docker image (one-time setup)..."

    local MIRROR_CMD=""
    if [ -n "${APT_MIRROR}" ]; then
        MIRROR_CMD="RUN sed -i \"s|http://ports.ubuntu.com|http://${APT_MIRROR}|g\" /etc/apt/sources.list.d/ubuntu.sources && sed -i \"s|http://archive.ubuntu.com|http://${APT_MIRROR}|g\" /etc/apt/sources.list.d/ubuntu.sources && sed -i \"s|http://security.ubuntu.com|http://${APT_MIRROR}|g\" /etc/apt/sources.list.d/ubuntu.sources"
    fi

    # Cross-compiling to x64 on ARM64 Ubuntu requires amd64 libraries.
    # ubuntu-ports only hosts arm64. We need the regular Ubuntu archive for amd64.
    # Solution: write two clean DEB822 source files (arm64 + amd64).
    local PORTS_URI="http://ports.ubuntu.com/ubuntu-ports"
    local ARCHIVE_URI="http://archive.ubuntu.com/ubuntu"
    if [ -n "${APT_MIRROR}" ]; then
        PORTS_URI="http://${APT_MIRROR}/ubuntu-ports"
        ARCHIVE_URI="http://${APT_MIRROR}/ubuntu"
    fi

    docker buildx build --platform linux/arm64 --load \
        -t "${full_tag}" -f - "${PROJECT_ROOT}" <<DOCKERFILE
FROM ubuntu:24.04
RUN dpkg --add-architecture amd64 && \\
    rm -f /etc/apt/sources.list.d/ubuntu.sources && \\
    printf 'Types: deb\\nURIs: ${PORTS_URI}\\nSuites: noble noble-updates noble-security\\nComponents: main universe\\nArchitectures: arm64\\nSigned-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\\n' > /etc/apt/sources.list.d/arm64.sources && \\
    printf 'Types: deb\\nURIs: ${ARCHIVE_URI}\\nSuites: noble noble-updates noble-security\\nComponents: main universe\\nArchitectures: amd64\\nSigned-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\\n' > /etc/apt/sources.list.d/amd64.sources && \\
    apt-get update && \\
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \\
    cmake make gcc-x86-64-linux-gnu g++-x86-64-linux-gnu \\
    zlib1g-dev:amd64 libssl-dev:amd64 \\
    && rm -rf /var/lib/apt/lists/*
WORKDIR /xray
DOCKERFILE
}

build_mingw_image() {
    local full_tag="${DOCKER_MINGW_IMAGE}:${DOCKER_MINGW_TAG}"

    if docker image inspect "${full_tag}" &>/dev/null; then
        echo "  Docker image ${full_tag} already exists (cached)"
        return 0
    fi

    log_header "Building MinGW Docker image (one-time setup)..."

    local MIRROR_CMD=""
    if [ -n "${APT_MIRROR}" ]; then
        MIRROR_CMD="RUN sed -i \"s|http://ports.ubuntu.com|http://${APT_MIRROR}|g\" /etc/apt/sources.list.d/ubuntu.sources && sed -i \"s|http://archive.ubuntu.com|http://${APT_MIRROR}|g\" /etc/apt/sources.list.d/ubuntu.sources && sed -i \"s|http://security.ubuntu.com|http://${APT_MIRROR}|g\" /etc/apt/sources.list.d/ubuntu.sources"
    fi

    docker buildx build --platform linux/arm64 --load \
        -t "${full_tag}" -f - "${PROJECT_ROOT}" <<DOCKERFILE
FROM ubuntu:24.04
${MIRROR_CMD}
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \\
    cmake make gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64 \\
    && rm -rf /var/lib/apt/lists/*
WORKDIR /xray
DOCKERFILE
}

# ============================================================
# Run tests in Docker for a given platform
# ============================================================

run_docker_test() {
    local platform="$1"    # linux/amd64 or linux/arm64
    local label="$2"       # display name
    local arch_tag
    arch_tag=$(echo "$platform" | tr '/' '-')
    local full_tag="${DOCKER_IMAGE}:${DOCKER_TAG}-${arch_tag}"

    # Ensure image exists for this platform
    build_docker_image "${platform}"

    log_header "Testing: ${label} (${platform})"

    # Run build + unit tests + quick regression in Docker
    # Mount source read-only, build in container tmpfs for speed
    if docker run --rm \
        --platform "${platform}" \
        -v "${PROJECT_ROOT}:/xray-src:ro" \
        -e "XRAY_SKIP_BUILD=1" \
        "${full_tag}" \
        bash -c '
            set -e
            # Copy source to writable location, excluding host build dirs
            mkdir -p /tmp/xray
            cd /xray-src && find . \
                -path ./build -prune -o \
                -path ./build-release -prune -o \
                -path ./build-asan -prune -o \
                -path ./.git -prune -o \
                -print0 | (cd /tmp/xray && xargs -0 -I{} cp -a --parents /xray-src/{} . 2>/dev/null || true)
            # Simpler fallback: rsync-style copy excluding build dirs
            rm -rf /tmp/xray
            cp -a /xray-src /tmp/xray
            rm -rf /tmp/xray/build /tmp/xray/build-release /tmp/xray/build-asan
            cd /tmp/xray

            echo ">>> cmake configure..."
            cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="-w" 2>&1 | tail -5

            echo ">>> cmake build..."
            cmake --build build -j$(nproc) 2>&1 | tail -3

            echo ">>> binary check..."
            ./build/xray --version

            echo ">>> ctest (unit tests)..."
            cd build && ctest --output-on-failure --timeout 60 2>&1 | tail -20
            CTEST_RC=$?
            cd ..

            echo ">>> regression tests (quick)..."
            if [ -f ./scripts/run_regression_tests.sh ]; then
                chmod +x ./scripts/run_regression_tests.sh
                XRAY_SKIP_BUILD=1 XRAY_PATH=./build/xray \
                    timeout 300 ./scripts/run_regression_tests.sh 2>&1 | tail -30
            fi

            exit $CTEST_RC
        ' 2>&1; then
        log_pass "${label}: all tests passed"
    else
        log_fail "${label}: some tests failed (see output above)"
    fi
}

# ============================================================
# Windows cross-compile test (MinGW)
# ============================================================

run_windows_crosscompile() {
    build_mingw_image

    local full_tag="${DOCKER_MINGW_IMAGE}:${DOCKER_MINGW_TAG}"

    log_header "Testing: Windows x64 (MinGW cross-compile)"

    if docker run --rm \
        --platform linux/arm64 \
        -v "${PROJECT_ROOT}:/xray-src:ro" \
        "${full_tag}" \
        bash -c '
            set -e
            cp -a /xray-src /tmp/xray
            rm -rf /tmp/xray/build* /tmp/xray/.git
            cd /tmp/xray

            echo ">>> cmake configure for Windows x64..."
            cmake -B build-win \
                -DCMAKE_SYSTEM_NAME=Windows \
                -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
                -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \
                -DCMAKE_BUILD_TYPE=Release \
                -DCMAKE_C_FLAGS="-w" \
                -DBUILD_TESTS=OFF \
                -DENABLE_TLS=OFF \
                2>&1 | tail -10

            echo ""
            echo ">>> cmake build (cross-compile)..."
            cmake --build build-win -j$(nproc) 2>&1 | tail -5
            BUILD_RC=$?

            echo ""
            if [ -f build-win/xray.exe ]; then
                echo ">>> xray.exe built successfully"
                ls -lh build-win/xray.exe
                file build-win/xray.exe
            else
                echo ">>> FAILED: xray.exe not produced"
                echo ">>> Errors:"
                cmake --build build-win -j1 2>&1 | grep -E "error:|undefined reference" | head -15
                exit 1
            fi
        ' 2>&1; then
        log_pass "Windows x64: cross-compilation succeeded"
    else
        log_fail "Windows x64: cross-compilation failed (see errors above)"
    fi
}

# ============================================================
# Linux x64 cross-compile test
# ============================================================

run_linux_x64_crosscompile() {
    build_linux_cross_image

    local full_tag="xray-cross-x64:latest"

    log_header "Testing: Linux x64 (cross-compile, no QEMU)"

    if docker run --rm \
        --platform linux/arm64 \
        -v "${PROJECT_ROOT}:/xray-src:ro" \
        "${full_tag}" \
        bash -c '
            set -e
            cp -a /xray-src /tmp/xray
            rm -rf /tmp/xray/build* /tmp/xray/.git
            cd /tmp/xray

            echo ">>> cmake configure for Linux x64 (cross-compile)..."
            cmake -B build-x64 \
                -DCMAKE_SYSTEM_NAME=Linux \
                -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
                -DCMAKE_C_COMPILER=x86_64-linux-gnu-gcc \
                -DCMAKE_CXX_COMPILER=x86_64-linux-gnu-g++ \
                -DCMAKE_FIND_ROOT_PATH=/usr/x86_64-linux-gnu \
                -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH \
                -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH \
                -DCMAKE_BUILD_TYPE=RelWithDebInfo \
                -DCMAKE_C_FLAGS="-w" \
                -DBUILD_TESTS=OFF \
                2>&1 | tail -10

            echo ""
            echo ">>> cmake build (cross-compile)..."
            cmake --build build-x64 -j$(nproc) 2>&1 | tail -5

            echo ""
            if [ -f build-x64/xray ]; then
                echo ">>> xray (linux-x64) built successfully"
                ls -lh build-x64/xray
                file build-x64/xray
            else
                echo ">>> FAILED: xray not produced"
                echo ">>> Errors:"
                cmake --build build-x64 -j1 2>&1 | grep -E "error:|undefined reference" | head -15
                exit 1
            fi
        ' 2>&1; then
        log_pass "Linux x64: cross-compilation succeeded"
    else
        log_fail "Linux x64: cross-compilation failed (see errors above)"
    fi
}

# ============================================================
# macOS x64 test (Rosetta 2)
# ============================================================

run_macos_x64_test() {
    log_header "Testing: macOS x64 (cross-compile + Rosetta 2)"

    # Check we're on Apple Silicon macOS
    if [ "$(uname -s)" != "Darwin" ]; then
        log_skip "macOS x64: not on macOS"
        return
    fi

    local BUILD_DIR="${PROJECT_ROOT}/build-x64"

    echo "  Cross-compiling for x86_64..."
    cmake -B "${BUILD_DIR}" \
        -DCMAKE_OSX_ARCHITECTURES=x86_64 \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS="-w" \
        "${PROJECT_ROOT}" >/dev/null 2>&1
    cmake --build "${BUILD_DIR}" -j8 2>&1 | tail -5

    if [ ! -f "${BUILD_DIR}/xray" ]; then
        log_fail "macOS x64: build failed"
        return
    fi

    echo "  Checking binary architecture..."
    file "${BUILD_DIR}/xray"

    echo "  Running ctest via Rosetta 2..."
    if (cd "${BUILD_DIR}" && ctest --output-on-failure --timeout 60 2>&1 | tail -10); then
        log_pass "macOS x64: ctest passed (Rosetta 2)"
    else
        log_fail "macOS x64: ctest failed"
    fi

    echo "  Running regression tests via Rosetta 2..."
    if XRAY_SKIP_BUILD=1 XRAY_PATH="${BUILD_DIR}/xray" \
        "${PROJECT_ROOT}/scripts/run_regression_tests.sh" 2>&1 | tail -10; then
        log_pass "macOS x64: regression passed (Rosetta 2)"
    else
        log_fail "macOS x64: regression failed"
    fi
}

# ============================================================
# Native macOS test
# ============================================================

run_native_test() {
    log_header "Testing: macOS ARM64 (native)"

    local BUILD_DIR="${PROJECT_ROOT}/build"

    if [ ! -f "${BUILD_DIR}/xray" ]; then
        echo "  Building..."
        cmake -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release "${PROJECT_ROOT}" >/dev/null 2>&1
        cmake --build "${BUILD_DIR}" -j8 2>&1 | tail -3
    fi

    echo "  Running ctest..."
    if (cd "${BUILD_DIR}" && ctest --output-on-failure --timeout 60 2>&1 | tail -10); then
        log_pass "macOS ARM64: ctest passed"
    else
        log_fail "macOS ARM64: ctest failed"
    fi

    echo "  Running regression tests..."
    if XRAY_SKIP_BUILD=1 XRAY_PATH="${BUILD_DIR}/xray" \
        "${PROJECT_ROOT}/scripts/run_regression_tests.sh" 2>&1 | tail -10; then
        log_pass "macOS ARM64: regression passed"
    else
        log_fail "macOS ARM64: regression failed"
    fi
}

# ============================================================
# Main
# ============================================================

echo -e "${BLUE}Xray Cross-Platform Test Runner${NC}"
echo -e "Project: ${PROJECT_ROOT}"
echo -e "Platform: ${PLATFORM}"
echo ""

# Docker images are built on-demand per platform in run_docker_test()

case "$PLATFORM" in
    all)
        run_native_test
        run_macos_x64_test
        run_docker_test "linux/arm64" "Linux ARM64"
        run_linux_x64_crosscompile
        run_windows_crosscompile
        ;;
    native|macos)
        run_native_test
        ;;
    linux-x64|x64|amd64)
        run_docker_test "linux/amd64" "Linux x86-64"
        ;;
    linux-arm64|arm64|aarch64)
        run_docker_test "linux/arm64" "Linux ARM64"
        ;;
    linux)
        run_docker_test "linux/amd64" "Linux x86-64"
        run_docker_test "linux/arm64" "Linux ARM64"
        ;;
    windows|win|mingw)
        run_windows_crosscompile
        ;;
    linux-x64-cross)
        run_linux_x64_crosscompile
        ;;
    macos-x64)
        run_macos_x64_test
        ;;
    *)
        echo -e "${RED}Unknown platform: ${PLATFORM}${NC}"
        echo "Usage: $0 [all|native|macos-x64|linux-x64|linux-x64-cross|linux-arm64|linux|windows]"
        echo ""
        echo "Environment variables:"
        echo "  XRAY_APT_MIRROR  APT mirror (default: mirrors.tuna.tsinghua.edu.cn, set empty for default)"
        exit 1
        ;;
esac

# ============================================================
# Summary
# ============================================================

echo ""
echo -e "${BLUE}════════════════════════════════════════════════════${NC}"
echo -e "${BLUE}  Summary${NC}"
echo -e "${BLUE}════════════════════════════════════════════════════${NC}"
echo -e "  ${GREEN}Passed:  ${PASSED}${NC}"
[ $FAILED -gt 0 ] && echo -e "  ${RED}Failed:  ${FAILED}${NC}" || echo -e "  Failed:  0"
[ $SKIPPED -gt 0 ] && echo -e "  ${YELLOW}Skipped: ${SKIPPED}${NC}"
echo ""

if [ $FAILED -gt 0 ]; then
    echo -e "${RED}Some platforms failed!${NC}"
    exit 1
else
    echo -e "${GREEN}All platforms passed!${NC}"
fi

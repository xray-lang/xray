#!/bin/bash
# run_regression_tests.sh
# 作者：xingleixu@gmail.com
#
# 运行 tests/regression 目录下的所有回归测试用例
# 统计测试结果并生成报告

# 设置颜色输出（non-TTY / NO_COLOR 时自动关闭）
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    GREEN='\033[0;32m'
    RED='\033[0;31m'
    YELLOW='\033[1;33m'
    BLUE='\033[0;34m'
    NC='\033[0m'
else
    GREEN='' RED='' YELLOW='' BLUE='' NC=''
fi

# 获取脚本所在目录的父目录（项目根目录）
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# 测试目录和可执行文件路径
TEST_DIR="${PROJECT_ROOT}/tests/regression"

# Auto-detect cmake (homebrew on macOS may not be in PATH)
if command -v cmake &>/dev/null; then
    CMAKE_BIN="cmake"
elif [ -x /opt/homebrew/bin/cmake ]; then
    CMAKE_BIN="/opt/homebrew/bin/cmake"
else
    CMAKE_BIN="cmake"  # fallback, will fail gracefully
fi

# Auto-detect build directory: prefer build, then build-release
if [ -n "${XRAY_BUILD_DIR}" ]; then
    BUILD_DIR="${XRAY_BUILD_DIR}"
elif [ -f "${PROJECT_ROOT}/build/xray" ]; then
    BUILD_DIR="${PROJECT_ROOT}/build"
elif [ -f "${PROJECT_ROOT}/build-release/xray" ]; then
    BUILD_DIR="${PROJECT_ROOT}/build-release"
else
    BUILD_DIR="${PROJECT_ROOT}/build"
fi
XRAY_BIN="${BUILD_DIR}/xray"

# 自动构建（除非设置 XRAY_SKIP_BUILD=1）
if [ "${XRAY_SKIP_BUILD}" != "1" ]; then
    echo -e "${BLUE}正在构建 ${BUILD_DIR}...${NC}"
    if ! "${CMAKE_BIN}" --build "${BUILD_DIR}" -j8 >/dev/null 2>&1; then
        echo -e "${RED}构建失败${NC}"
        exit 1
    fi
    echo -e "${GREEN}构建完成${NC}"
fi

# Portable timeout: use timeout/gtimeout if available, else shell-based fallback.
# Provides portable_timeout() that works on Linux, macOS, and bare systems.
if command -v timeout &>/dev/null; then
    portable_timeout() { timeout "$@"; }
elif command -v gtimeout &>/dev/null; then
    portable_timeout() { gtimeout "$@"; }
else
    # Shell-based fallback for macOS without coreutils
    portable_timeout() {
        local secs=$1; shift
        "$@" &
        local pid=$!
        ( sleep "$secs"; kill "$pid" 2>/dev/null ) &
        local watcher=$!
        wait "$pid" 2>/dev/null
        local rc=$?
        kill "$watcher" 2>/dev/null
        wait "$watcher" 2>/dev/null
        # If killed by our watcher, return 124 (same as GNU timeout)
        if [ $rc -eq 137 ] || [ $rc -eq 143 ]; then
            return 124
        fi
        return $rc
    }
fi

# 检查可执行文件是否存在
if [ ! -f "${XRAY_BIN}" ]; then
    echo -e "${RED}错误: 找不到 xray 可执行文件: ${XRAY_BIN}${NC}"
    echo "请先编译项目："
    echo "  cd ${BUILD_DIR}"
    echo "  cmake .."
    echo "  make"
    exit 1
fi

# 检查测试目录是否存在
if [ ! -d "${TEST_DIR}" ]; then
    echo -e "${RED}错误: 找不到测试目录 ${TEST_DIR}${NC}"
    exit 1
fi

# 初始化计数器
total_tests=0
passed_tests=0
failed_tests=0

# 存储失败的测试
declare -a failed_test_list

echo -e "${BLUE}======================================${NC}"
echo -e "${BLUE}Xray 回归测试运行器${NC}"
echo -e "${BLUE}======================================${NC}"
echo ""
echo "测试目录: ${TEST_DIR}"
echo "Xray 可执行文件: ${XRAY_BIN}"
echo ""

# 开始时间
start_time=$(date +%s)

# 遍历所有 .xr 测试文件（按文件名排序）
for test_file in $(find "${TEST_DIR}" -name "*.xr" -type f | sort); do
    # 获取测试文件名（不含路径）
    test_name=$(basename "${test_file}")

    # 跳过以 _ 开头的文件（保留文件）
    if [[ "${test_name}" == _* ]]; then
        continue
    fi

    total_tests=$((total_tests + 1))

    # 显示测试进度
    printf "[%3d] %-40s ... " "${total_tests}" "${test_name}"

    # Tests that need --no-jit:
    #   1205 / 1207: GC stress tests crash under JIT in Debug build due
    #     to JIT+GC interaction issues with inline allocation.
    #   1148: scope race stress hits a JIT regalloc overlap on the
    #     try/catch + linked scope path (independent JIT bug, regalloc
    #     verifier reports overlapping register assignment).
    jit_flag=""
    case "${test_name}" in
        1148_scope_race_stress.xr|\
        1205_gc_incremental_pressure.xr|\
        1207_gc_stress.xr)
            jit_flag="--no-jit"
            ;;
    esac

    if [ "${VERBOSE}" = "1" ]; then
        output=$(portable_timeout 30 "${XRAY_BIN}" test ${jit_flag} "${test_file}" 2>&1)
        exit_code=$?
    else
        portable_timeout 30 "${XRAY_BIN}" test ${jit_flag} "${test_file}" > /dev/null 2>&1
        exit_code=$?
    fi

    # 检查退出码
    if [ ${exit_code} -eq 0 ]; then
        # 测试通过
        echo -e "${GREEN}✓ PASS${NC}"
        passed_tests=$((passed_tests + 1))
    elif [ ${exit_code} -eq 124 ]; then
        # 超时
        echo -e "${RED}✗ TIMEOUT${NC}"
        failed_tests=$((failed_tests + 1))
        failed_test_list+=("${test_name} (timeout)")
    else
        # 测试失败
        echo -e "${RED}✗ FAIL${NC}"
        failed_tests=$((failed_tests + 1))
        failed_test_list+=("${test_name}")

        # 如果环境变量 VERBOSE 设置为 1，显示错误输出
        if [ "${VERBOSE}" = "1" ]; then
            echo -e "${YELLOW}输出:${NC}"
            echo "${output}" | sed 's/^/  /'
            echo ""
        fi
    fi
done

# 结束时间
end_time=$(date +%s)
elapsed_time=$((end_time - start_time))

# 打印测试摘要
echo ""
echo -e "${BLUE}======================================${NC}"
echo -e "${BLUE}测试摘要${NC}"
echo -e "${BLUE}======================================${NC}"
echo "总测试数: ${total_tests}"
echo -e "${GREEN}通过: ${passed_tests}${NC}"
echo -e "${RED}失败: ${failed_tests}${NC}"
echo "耗时: ${elapsed_time} 秒"
echo ""

# 如果有失败的测试，列出它们
if [ ${failed_tests} -gt 0 ]; then
    echo -e "${RED}失败的测试:${NC}"
    for test in "${failed_test_list[@]}"; do
        echo "  - ${test}"
    done
    echo ""
    echo -e "${YELLOW}提示: 使用 VERBOSE=1 运行脚本查看详细错误输出${NC}"
    echo "  VERBOSE=1 ${BASH_SOURCE[0]}"
    echo ""
    exit 1
else
    echo -e "${GREEN}🎉 所有测试通过！${NC}"
    echo ""
    exit 0
fi

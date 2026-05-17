#!/bin/bash
# run_regression_tests.sh
# 作者：xingleixu@gmail.com
#
# 运行 tests/regression 目录下的所有回归测试用例
# 并行执行，统计测试结果并生成报告

set -o pipefail

# 设置颜色输出（non-TTY / NO_COLOR 时自动关闭）
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    GREEN='\033[0;32m'
    RED='\033[0;31m'
    YELLOW='\033[1;33m'
    BLUE='\033[0;34m'
    CYAN='\033[0;36m'
    NC='\033[0m'
else
    GREEN='' RED='' YELLOW='' BLUE='' CYAN='' NC=''
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
    CMAKE_BIN="cmake"
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

# Locate the actual binary. Layouts encountered in CI:
#   Linux / macOS / mingw     : ${BUILD_DIR}/xray
#   MSVC multi-config Debug   : ${BUILD_DIR}/Debug/xray.exe
#   MSVC multi-config Release : ${BUILD_DIR}/Release/xray.exe
# XRAY_PATH env wins if the caller already knows the path (e.g. CI).
if [ -n "${XRAY_PATH:-}" ] && [ -f "${XRAY_PATH}" ]; then
    XRAY_BIN="${XRAY_PATH}"
elif [ -f "${BUILD_DIR}/xray" ]; then
    XRAY_BIN="${BUILD_DIR}/xray"
elif [ -f "${BUILD_DIR}/xray.exe" ]; then
    XRAY_BIN="${BUILD_DIR}/xray.exe"
elif [ -f "${BUILD_DIR}/Debug/xray.exe" ]; then
    XRAY_BIN="${BUILD_DIR}/Debug/xray.exe"
elif [ -f "${BUILD_DIR}/Release/xray.exe" ]; then
    XRAY_BIN="${BUILD_DIR}/Release/xray.exe"
else
    XRAY_BIN="${BUILD_DIR}/xray"
fi

# Configuration
TIMEOUT_SECS=${XRAY_TEST_TIMEOUT:-10}
PARALLEL_JOBS=${XRAY_TEST_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}

# Known-skip list (colon-separated for export to subshells)
SKIP_TESTS=""

# Tests requiring --no-jit flag (colon-separated)
NOJIT_TESTS="1148_scope_race_stress.xr:1205_gc_incremental_pressure.xr:1207_gc_stress.xr"

# 自动构建（除非设置 XRAY_SKIP_BUILD=1）
if [ "${XRAY_SKIP_BUILD}" != "1" ]; then
    echo -e "${BLUE}正在构建 ${BUILD_DIR}...${NC}"
    if ! "${CMAKE_BIN}" --build "${BUILD_DIR}" -j8 >/dev/null 2>&1; then
        echo -e "${RED}构建失败${NC}"
        exit 1
    fi
    echo -e "${GREEN}构建完成${NC}"
fi

# Portable timeout detection
if command -v gtimeout &>/dev/null; then
    TIMEOUT_CMD="gtimeout"
elif command -v timeout &>/dev/null; then
    TIMEOUT_CMD="timeout"
else
    TIMEOUT_CMD=""
fi

# 检查可执行文件是否存在
if [ ! -f "${XRAY_BIN}" ]; then
    echo -e "${RED}错误: 找不到 xray 可执行文件: ${XRAY_BIN}${NC}"
    exit 1
fi

# 检查测试目录是否存在
if [ ! -d "${TEST_DIR}" ]; then
    echo -e "${RED}错误: 找不到测试目录 ${TEST_DIR}${NC}"
    exit 1
fi

echo -e "${BLUE}======================================${NC}"
echo -e "${BLUE}Xray 回归测试运行器${NC}"
echo -e "${BLUE}======================================${NC}"
echo ""
echo "测试目录: ${TEST_DIR}"
echo "并行度: ${PARALLEL_JOBS}  超时: ${TIMEOUT_SECS}s"
echo ""

# 临时结果目录
RESULTS_DIR=$(mktemp -d)
trap "rm -rf ${RESULTS_DIR}" EXIT

# Helper: check if name is in colon-separated list
is_in_list() {
    local name="$1" list="$2"
    [[ ":${list}:" == *":${name}:"* ]]
}

# Worker function: run one test, write result to file
run_one_test() {
    local test_file="$1"
    local test_name=$(basename "${test_file}")
    local result_file="${RESULTS_DIR}/${test_name}.result"

    # Skip reserved files
    if [[ "${test_name}" == _* ]]; then
        return
    fi

    # Check skip list
    if is_in_list "${test_name}" "${SKIP_TESTS}"; then
        echo "SKIP" > "${result_file}"
        return
    fi

    # Determine flags
    # XRAY_JIT_FORCE=1 forces all eligible tests through the JIT path,
    # mirroring the PR-gate sanitizer matrix that exposed the May 2026
    # x64 codegen / GC corruption family. NOJIT_TESTS still wins
    # because those scenarios deliberately exercise interpreter-only
    # behaviour (stress / scope-race / GC-pressure).
    local jit_flag=""
    if is_in_list "${test_name}" "${NOJIT_TESTS}"; then
        jit_flag="--no-jit"
    elif [ "${XRAY_JIT_FORCE:-0}" = "1" ]; then
        jit_flag="--jit-force"
    fi

    # All regression tests use @test functions — run with 'xray test'
    local xray_cmd="test"

    # Run with timeout
    local exit_code
    local output
    if [ -n "${TIMEOUT_CMD}" ]; then
        output=$("${TIMEOUT_CMD}" "${TIMEOUT_SECS}" "${XRAY_BIN}" ${xray_cmd} ${jit_flag} "${test_file}" 2>&1)
        exit_code=$?
    else
        # Shell-based timeout fallback — capture output to temp file
        local tmp_out="${RESULTS_DIR}/${test_name}.out"
        "${XRAY_BIN}" ${xray_cmd} ${jit_flag} "${test_file}" > "${tmp_out}" 2>&1 &
        local pid=$!
        ( sleep "${TIMEOUT_SECS}"; kill "$pid" 2>/dev/null ) &
        local watcher=$!
        wait "$pid" 2>/dev/null
        exit_code=$?
        kill "$watcher" 2>/dev/null
        wait "$watcher" 2>/dev/null
        if [ $exit_code -eq 137 ] || [ $exit_code -eq 143 ]; then
            exit_code=124
        fi
        output=$(cat "${tmp_out}" 2>/dev/null)
        rm -f "${tmp_out}"
    fi

    # Extract executed count from output (e.g., "7 passed")
    local exec_count=0
    local passed_count=$(echo "${output}" | sed 's/\x1b\[[0-9;]*m//g' | grep -o '[0-9]\+ passed' | head -1 | grep -o '[0-9]\+')
    local failed_count=$(echo "${output}" | sed 's/\x1b\[[0-9;]*m//g' | grep -o '[0-9]\+ failed' | head -1 | grep -o '[0-9]\+')
    exec_count=$(( ${passed_count:-0} + ${failed_count:-0} ))

    if [ ${exit_code} -eq 0 ]; then
        echo "PASS:${exec_count}" > "${result_file}"
    elif [ ${exit_code} -eq 124 ]; then
        echo "TIMEOUT:0" > "${result_file}"
    else
        echo "FAIL:${exec_count}" > "${result_file}"
    fi
}
export -f run_one_test is_in_list
export XRAY_BIN TIMEOUT_SECS TIMEOUT_CMD RESULTS_DIR
export SKIP_TESTS NOJIT_TESTS

# 开始时间
start_time=$(date +%s)

# Collect test files (exclude helper/fixture dirs that aren't standalone tests)
test_files=$(find "${TEST_DIR}" -name "*.xr" -type f ! -name '_*' \
    ! -path '*/fixtures/*' ! -path '*/modules/*' ! -path '*/reexport_test/*' | sort)
total_tests=$(echo "${test_files}" | wc -l | tr -d ' ')

echo -e "${CYAN}运行 ${total_tests} 个测试 (${PARALLEL_JOBS} 并行)...${NC}"
echo ""

# Run tests in parallel
echo "${test_files}" | xargs -P "${PARALLEL_JOBS}" -I {} bash -c 'run_one_test "$@"' _ {}

# Collect results
passed_tests=0
failed_tests=0
skipped_tests=0
total_executed=0
declare -a failed_test_list

for result_file in $(find "${RESULTS_DIR}" -name "*.result" | sort); do
    test_name=$(basename "${result_file}" .result)
    raw=$(cat "${result_file}")
    result=${raw%%:*}
    exec_n=${raw#*:}
    # If no colon, exec_n equals raw — treat as 0
    if [ "${exec_n}" = "${raw}" ]; then exec_n=0; fi
    total_executed=$((total_executed + exec_n))
    case "${result}" in
        PASS)
            passed_tests=$((passed_tests + 1))
            ;;
        SKIP)
            skipped_tests=$((skipped_tests + 1))
            ;;
        TIMEOUT)
            failed_tests=$((failed_tests + 1))
            failed_test_list+=("${test_name} (timeout)")
            ;;
        FAIL)
            failed_tests=$((failed_tests + 1))
            failed_test_list+=("${test_name}")
            ;;
    esac
done

# 结束时间
end_time=$(date +%s)
elapsed_time=$((end_time - start_time))

# 打印测试摘要
echo -e "${BLUE}======================================${NC}"
echo -e "${BLUE}测试摘要${NC}"
echo -e "${BLUE}======================================${NC}"
echo "总文件数: ${total_tests}"
echo "执行测试: ${total_executed}"
echo -e "${GREEN}通过: ${passed_tests}${NC}"
if [ ${skipped_tests} -gt 0 ]; then
    echo -e "${CYAN}跳过: ${skipped_tests}${NC}"
fi
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
    echo -e "${YELLOW}提示: 使用 VERBOSE=1 运行单个失败测试查看详细输出${NC}"
    echo "  ${XRAY_BIN} test <test_file>"
    echo ""
    exit 1
else
    echo -e "${GREEN}所有测试通过！${NC}"
    echo ""
    exit 0
fi

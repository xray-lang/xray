#!/bin/bash
# run_compile_error_tests.sh
# 作者：xingleixu@gmail.com
#
# 运行编译错误测试用例
# 每个 .xr 文件期望编译失败，并验证错误消息匹配 .expected 文件

# 颜色定义（non-TTY / NO_COLOR 时自动关闭）
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    GREEN='\033[0;32m' RED='\033[0;31m' YELLOW='\033[1;33m' BLUE='\033[0;34m' NC='\033[0m'
else
    GREEN='' RED='' YELLOW='' BLUE='' NC=''
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

TEST_DIR="${PROJECT_ROOT}/tests/compile_errors"
XRAY_BIN="${PROJECT_ROOT}/build/xray"

# 检查可执行文件
if [ ! -f "${XRAY_BIN}" ]; then
    echo -e "${RED}错误: 找不到 xray 可执行文件${NC}"
    exit 1
fi

# 计数器
total=0
passed=0
failed=0
declare -a failed_list

echo -e "${BLUE}======================================${NC}"
echo -e "${BLUE}Xray 编译错误测试${NC}"
echo -e "${BLUE}======================================${NC}"
echo ""

# 遍历所有 .xr 测试文件
for test_file in $(find "${TEST_DIR}" -name "*.xr" -type f | sort); do
    test_name=$(basename "${test_file}")
    expected_file="${test_file}.expected"

    total=$((total + 1))
    printf "[%3d] %-45s ... " "${total}" "${test_name}"

    # 运行编译器（期望失败）
    raw_output=$(timeout 3 "${XRAY_BIN}" "${test_file}" 2>&1)
    exit_code=$?
    # strip ANSI escape sequences for matching
    output=$(echo "$raw_output" | sed $'s/\x1b\\[[0-9;]*m//g')

    # 检查是否编译失败
    if [ ${exit_code} -eq 0 ]; then
        echo -e "${RED}✗ FAIL (应该报错但编译成功)${NC}"
        failed=$((failed + 1))
        failed_list+=("${test_name}: 应该报错但编译成功")
        continue
    fi

    # 如果有 .expected 文件，验证错误消息
    if [ -f "${expected_file}" ]; then
        expected_msg=$(cat "${expected_file}" | head -1)
        if echo "${output}" | grep -Fq "${expected_msg}"; then
            echo -e "${GREEN}✓ PASS${NC}"
            passed=$((passed + 1))
        else
            echo -e "${RED}✗ FAIL (错误消息不匹配)${NC}"
            echo -e "${YELLOW}  期望包含: ${expected_msg}${NC}"
            echo -e "${YELLOW}  实际输出: ${output}${NC}"
            failed=$((failed + 1))
            failed_list+=("${test_name}: 错误消息不匹配")
        fi
    else
        # 没有 .expected 文件，只要编译失败就算通过
        echo -e "${GREEN}✓ PASS (编译失败)${NC}"
        passed=$((passed + 1))
    fi
done

# 摘要
echo ""
echo -e "${BLUE}======================================${NC}"
echo -e "${BLUE}测试摘要${NC}"
echo -e "${BLUE}======================================${NC}"
echo "总测试数: ${total}"
echo -e "${GREEN}通过: ${passed}${NC}"
echo -e "${RED}失败: ${failed}${NC}"

if [ ${failed} -gt 0 ]; then
    echo ""
    echo -e "${RED}失败的测试:${NC}"
    for t in "${failed_list[@]}"; do
        echo "  - ${t}"
    done
    exit 1
else
    echo ""
    echo -e "${GREEN}🎉 所有编译错误测试通过！${NC}"
    exit 0
fi

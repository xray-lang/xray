#!/bin/bash
#
# run_all_tests.sh - 运行所有工作窃取测试
# 作者：xingleixu@gmail.com
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
XRAY="${SCRIPT_DIR}/../../build/xray"

echo "╔══════════════════════════════════════════════════════╗"
echo "║          工作窃取功能测试套件                        ║"
echo "╚══════════════════════════════════════════════════════╝"
echo ""

# 检查 xray 是否存在
if [ ! -f "$XRAY" ]; then
    echo "错误: xray 可执行文件不存在: $XRAY"
    echo "请先编译: cmake --build build"
    exit 1
fi

# 测试文件列表
TESTS=(
    "01_basic_stealing.xr"
    "02_fairness_test.xr"
    "03_burst_workload.xr"
    "04_mixed_workload.xr"
    "05_priority_test.xr"
    "06_chain_spawn.xr"
    "07_producer_consumer.xr"
    "08_benchmark.xr"
)

PASSED=0
FAILED=0
TOTAL=${#TESTS[@]}

for test in "${TESTS[@]}"; do
    echo ""
    echo "────────────────────────────────────────────────────────"
    echo "运行: $test"
    echo "────────────────────────────────────────────────────────"
    
    if "$XRAY" "${SCRIPT_DIR}/${test}"; then
        echo ""
        echo "结果: ✓ PASS"
        ((PASSED++))
    else
        echo ""
        echo "结果: ✗ FAIL"
        ((FAILED++))
    fi
done

echo ""
echo "╔══════════════════════════════════════════════════════╗"
echo "║  测试摘要                                            ║"
echo "╠══════════════════════════════════════════════════════╣"
echo "║  总测试数: $TOTAL                                        ║"
echo "║  通过: $PASSED                                           ║"
echo "║  失败: $FAILED                                           ║"
echo "╚══════════════════════════════════════════════════════╝"

if [ $FAILED -gt 0 ]; then
    exit 1
fi

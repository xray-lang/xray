#!/bin/bash
# 作者：xingleixu@gmail.com
# 协程性能对比测试运行脚本

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BENCHMARK_DIR="$PROJECT_DIR/tests/benchmarks/coro"
XRAY_BIN="$PROJECT_DIR/build/xray"

# 颜色输出（non-TTY / NO_COLOR 时自动关闭）
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    RED='\033[0;31m' GREEN='\033[0;32m' YELLOW='\033[1;33m' NC='\033[0m'
else
    RED='' GREEN='' YELLOW='' NC=''
fi

# 默认只运行 xray
RUN_GO=false
RUN_XRAY=true

# 解析参数
while [[ $# -gt 0 ]]; do
    case $1 in
        --go)
            RUN_GO=true
            shift
            ;;
        --xray-only)
            RUN_GO=false
            shift
            ;;
        --all)
            RUN_GO=true
            RUN_XRAY=true
            shift
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--go] [--xray-only] [--all]"
            exit 1
            ;;
    esac
done

echo "=========================================="
echo "  xray vs Go 协程性能对比测试"
echo "=========================================="
echo ""

# 检查 xray 是否存在
if [ ! -f "$XRAY_BIN" ]; then
    echo -e "${RED}错误: xray 未编译，请先运行 cmake --build build${NC}"
    exit 1
fi

# 检查 Go 是否安装
if $RUN_GO && ! command -v go &> /dev/null; then
    echo -e "${YELLOW}警告: Go 未安装，跳过 Go 测试${NC}"
    RUN_GO=false
fi

# 测试列表
TESTS=(
    "spawn"
    "pingpong"
    "ring"
    "skynet"
    "fanout"
    "producer_consumer"
    "parallel_sum"
    "sleep_storm"
)

# 运行单个测试
run_test() {
    local test_name=$1
    local test_dir="$BENCHMARK_DIR/$test_name"

    echo -e "${GREEN}>>> $test_name${NC}"
    echo "----------------------------------------"

    if $RUN_XRAY; then
        local xr_file="$test_dir/$test_name.xr"
        if [ -f "$xr_file" ]; then
            echo -e "${YELLOW}[xray]${NC}"
            "$XRAY_BIN" "$xr_file" 2>&1 || echo -e "${RED}xray 测试失败${NC}"
            echo ""
        fi
    fi

    if $RUN_GO; then
        local go_file="$test_dir/$test_name.go"
        if [ -f "$go_file" ]; then
            echo -e "${YELLOW}[Go]${NC}"
            (cd "$test_dir" && go run "$test_name.go") 2>&1 || echo -e "${RED}Go 测试失败${NC}"
            echo ""
        fi
    fi

    echo ""
}

# 运行所有测试
for test in "${TESTS[@]}"; do
    run_test "$test"
done

echo "=========================================="
echo "  测试完成"
echo "=========================================="

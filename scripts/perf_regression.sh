#!/bin/bash
# 作者：xingleixu@gmail.com
# 性能回归测试脚本

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BASELINE_FILE="$PROJECT_DIR/docs/perf_baseline.txt"
XRAY="$PROJECT_DIR/build/xray"

# 颜色输出（non-TTY / NO_COLOR 时自动关闭）
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    RED='\033[0;31m' GREEN='\033[0;32m' YELLOW='\033[1;33m' BLUE='\033[0;34m' NC='\033[0m'
else
    RED='' GREEN='' YELLOW='' BLUE='' NC=''
fi

# 允许的波动范围（1.15 = 允许慢 15%）
THRESHOLD=1.15

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}性能回归测试${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""

# 检查可执行文件
if [ ! -f "$XRAY" ]; then
    echo -e "${RED}错误: xray 可执行文件不存在: $XRAY${NC}"
    echo "请先编译: cd build && make"
    exit 1
fi

# 检查基线文件
if [ ! -f "$BASELINE_FILE" ]; then
    echo -e "${RED}错误: 基线文件不存在: $BASELINE_FILE${NC}"
    exit 1
fi

run_benchmark() {
    local name=$1
    local result=$(/usr/bin/time -p "$XRAY" "$PROJECT_DIR/tests/benchmarks/game/$name.xr" 2>&1 | grep "real" | awk '{print $2}')
    echo "$result"
}

get_baseline() {
    local name=$1
    grep "^$name:" "$BASELINE_FILE" | awk '{print $2}'
}

check_regression() {
    local name=$1
    local current=$2
    local baseline=$3

    # 使用 awk 进行浮点数比较
    local is_regression=$(awk -v c="$current" -v b="$baseline" -v t="$THRESHOLD" \
        'BEGIN { print (c > b * t) ? 1 : 0 }')

    local ratio=$(awk -v c="$current" -v b="$baseline" 'BEGIN { printf "%.2f", c/b }')

    if [ "$is_regression" -eq 1 ]; then
        echo -e "${RED}❌ $name: 回退 (${current}s vs ${baseline}s, ${ratio}x)${NC}"
        return 1
    else
        local improvement=$(awk -v c="$current" -v b="$baseline" 'BEGIN { printf "%.0f", (1-c/b)*100 }')
        if [ "$improvement" -gt 5 ]; then
            echo -e "${GREEN}✓ $name: 提升 ${improvement}% (${current}s vs ${baseline}s)${NC}"
        else
            echo -e "${GREEN}✓ $name: 正常 (${current}s vs ${baseline}s)${NC}"
        fi
        return 0
    fi
}

# 运行所有基准测试
failed=0
improved=0

echo "运行基准测试..."
echo ""

for bench in nbody spectralnorm binary_trees fannkuch_redux mandelbrot knucleotide; do
    baseline=$(get_baseline $bench)
    if [ -z "$baseline" ]; then
        echo -e "${YELLOW}⚠ $bench: 无基线数据${NC}"
        continue
    fi

    # 运行 3 次取最小值（减少波动）
    min_result=""
    for i in 1 2 3; do
        result=$(run_benchmark $bench)
        if [ -z "$min_result" ] || [ "$(awk -v r="$result" -v m="$min_result" 'BEGIN{print (r<m)?1:0}')" -eq 1 ]; then
            min_result="$result"
        fi
    done

    if ! check_regression $bench $min_result $baseline; then
        ((failed++))
    fi
done

echo ""
echo -e "${BLUE}========================================${NC}"
if [ $failed -gt 0 ]; then
    echo -e "${RED}⚠️ $failed 个测试存在性能回退${NC}"
    exit 1
else
    echo -e "${GREEN}✅ 所有测试通过，无性能回退${NC}"
fi
echo -e "${BLUE}========================================${NC}"

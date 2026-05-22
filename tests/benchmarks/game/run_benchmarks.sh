#!/bin/bash
# 作者：xingleixu@gmail.com
# Xray vs Lua 性能基准测试脚本
#
# 用法：
#   ./run_benchmarks.sh              # 运行所有测试
#   ./run_benchmarks.sh binary_trees # 运行单个测试
#   ./run_benchmarks.sh --quick      # 快速模式（较小参数）

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="${SCRIPT_DIR}/../.."
BUILD_DIR="${PROJECT_DIR}/build-release"
XRAY="${BUILD_DIR}/xray"
LUA="luajit"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Auto-build Release binary
build_release() {
    echo -e "${BLUE}编译 Release 版本...${NC}"
    cmake -DCMAKE_BUILD_TYPE=Release -B "$BUILD_DIR" "$PROJECT_DIR" > /dev/null 2>&1
    cmake --build "$BUILD_DIR" --target xray -j$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4) > /dev/null 2>&1
    if [ $? -ne 0 ]; then
        echo -e "${RED}Release 编译失败${NC}"
        exit 1
    fi
    echo -e "${GREEN}Release 编译完成${NC}"
}

# 检查依赖
check_dependencies() {
    # Always rebuild Release to ensure latest code
    build_release
    if [ ! -x "$XRAY" ]; then
        echo -e "${RED}错误: Release 编译产物不存在${NC}"
        exit 1
    fi
    if ! command -v $LUA &> /dev/null; then
        echo -e "${YELLOW}警告: luajit 未安装，将只运行 xray 测试${NC}"
        LUA=""
    fi
}

# 10 个标准基准测试及其参数
# 格式: "名称:xray参数:快速参数:描述[:lua参数]"
# 如果指定了 lua参数，则 lua 使用该参数；否则使用 xray参数
BENCHMARKS=(
    "binary_trees:16:12:二叉树创建/遍历（GC 压力测试）"
    "fannkuch_redux:10:9:排列翻转算法"
    "fasta:2500000:25000:DNA 序列生成"
    "knucleotide:100000:10000:K-核苷酸频率统计"
    "mandelbrot:1000:200:Mandelbrot 集计算"
    "nbody:5000000:50000:N-体问题模拟"
    "pidigits:50:30:Pi 数字计算（大整数运算）"
    "regexredux:100000:10000:正则表达式替换"
    "revcomp:2500000:250000:DNA 反向补码"
    "spectralnorm:500:100:光谱范数计算"
)

# 运行单个基准测试
run_benchmark() {
    local name=$1
    local xray_param=$2
    local desc=$3
    local lua_param=${4:-$xray_param}  # 如果没有指定 lua 参数，使用 xray 参数
    
    echo -e "\n${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BLUE}【${name}】${desc}${NC}"
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    
    local xray_file="${SCRIPT_DIR}/${name}.xr"
    local lua_file="${SCRIPT_DIR}/${name}.lua"
    local xray_time=""
    local lua_time=""
    
    # 运行 Xray
    if [ -f "$xray_file" ]; then
        echo -e "${GREEN}  Xray (n=$xray_param):${NC}"
        xray_time=$( { time "$XRAY" "$xray_file" -- $xray_param > /dev/null 2>&1; } 2>&1 | grep real | awk '{print $2}')
        echo -e "    时间: ${GREEN}${xray_time}${NC}"
    fi
    
    # 运行 Lua
    if [ -n "$LUA" ] && [ -f "$lua_file" ]; then
        echo -e "${YELLOW}  LuaJIT (n=$lua_param):${NC}"
        lua_time=$( { time $LUA "$lua_file" $lua_param > /dev/null 2>&1; } 2>&1 | grep real | awk '{print $2}')
        echo -e "    时间: ${YELLOW}${lua_time}${NC}"
        echo -e "    (LuaJIT)"
    fi
    
    # 记录结果
    echo "$name|xray=$xray_param,lua=$lua_param|$xray_time|$lua_time" >> "$RESULT_FILE"
}

# 生成报告
generate_report() {
    echo -e "\n${BLUE}╔════════════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${BLUE}║                    📊 性能测试结果汇总                              ║${NC}"
    echo -e "${BLUE}╠════════════════════════════════════════════════════════════════════╣${NC}"
    printf "${BLUE}║${NC} %-20s │ %8s │ %10s │ %10s ${BLUE}║${NC}\n" "测试项" "参数" "Xray" "LuaJIT"
    echo -e "${BLUE}╠════════════════════════════════════════════════════════════════════╣${NC}"
    
    while IFS='|' read -r name param xray lua; do
        printf "${BLUE}║${NC} %-20s │ %8s │ ${GREEN}%10s${NC} │ ${YELLOW}%10s${NC} ${BLUE}║${NC}\n" "$name" "$param" "$xray" "$lua"
    done < "$RESULT_FILE"
    
    echo -e "${BLUE}╚════════════════════════════════════════════════════════════════════╝${NC}"
    
    # 保存到历史记录
    local history_file="${SCRIPT_DIR}/benchmark_history.log"
    echo "" >> "$history_file"
    echo "=== $(date '+%Y-%m-%d %H:%M:%S') ===" >> "$history_file"
    cat "$RESULT_FILE" >> "$history_file"
    
    echo -e "\n${GREEN}结果已保存到: ${history_file}${NC}"
}

# 主函数
main() {
    check_dependencies
    
    RESULT_FILE=$(mktemp)
    QUICK_MODE=0
    SINGLE_TEST=""
    
    # 解析参数
    for arg in "$@"; do
        case $arg in
            --quick|-q)
                QUICK_MODE=1
                echo -e "${YELLOW}快速模式：使用较小参数${NC}"
                ;;
            *)
                SINGLE_TEST="$arg"
                ;;
        esac
    done
    
    echo -e "${BLUE}╔════════════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${BLUE}║          🚀 Xray vs LuaJIT 性能基准测试                             ║${NC}"
    echo -e "${BLUE}║          作者：xingleixu@gmail.com                                 ║${NC}"
    echo -e "${BLUE}╚════════════════════════════════════════════════════════════════════╝${NC}"
    echo ""
    echo "Xray: $XRAY"
    echo "LuaJIT: $(which $LUA 2>/dev/null || echo '未安装')"
    echo "时间: $(date '+%Y-%m-%d %H:%M:%S')"
    
    for bench in "${BENCHMARKS[@]}"; do
        IFS=':' read -r name std_param quick_param desc lua_param <<< "$bench"
        
        # 如果指定了单个测试，跳过其他
        if [[ -n "$SINGLE_TEST" && "$name" != "$SINGLE_TEST" ]]; then
            continue
        fi
        
        # 选择参数
        local xray_param=$std_param
        if [[ "$QUICK_MODE" == "1" ]]; then
            xray_param=$quick_param
        fi
        
        # lua 参数：如果指定了专用参数则使用，否则使用 xray 参数
        local final_lua_param=${lua_param:-$xray_param}
        
        run_benchmark "$name" "$xray_param" "$desc" "$final_lua_param"
    done
    
    generate_report
    rm -f "$RESULT_FILE"
}

main "$@"

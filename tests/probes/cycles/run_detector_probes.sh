#!/bin/bash
# run_detector_probes.sh — 环检测器的功能验收（任务 247 阶段 D §6.6）
#
# 检测器是**编译期开关**，默认构建里它的符号根本不存在，所以这些用例进不了
# 常规 ctest。用一个专门构建跑：
#
#   cmake -B build-detector -G Ninja -DXR_ENABLE_CYCLE_DETECTOR=ON
#   cmake --build build-detector -j 8
#   tests/probes/cycles/run_detector_probes.sh build-detector/xray
#
# 断言放在脚本里而不是 .xr 里，因为检测器**不提供**任何 stdlib 入口
# （§0.3：规范已经因为一个空操作的 tracing-GC 钩子被批评过，不得制造第二个）。
# 脚本比对的是检测器的机器可读输出行（`#cycle objects=... members=...`）。

set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
XRAY="${1:-${PROJECT_DIR}/build-detector/xray}"

if [ ! -x "$XRAY" ]; then
    echo "SKIP: detector build not found: $XRAY"
    echo "      cmake -B build-detector -G Ninja -DXR_ENABLE_CYCLE_DETECTOR=ON && cmake --build build-detector"
    exit 0
fi

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    GREEN='\033[0;32m'; RED='\033[0;31m'; NC='\033[0m'
else
    GREEN=''; RED=''; NC=''
fi

PASS=0
FAIL=0

check() {
    local desc="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then
        printf "  ${GREEN}✓${NC} %s\n" "$desc"
        PASS=$((PASS + 1))
    else
        printf "  ${RED}✗${NC} %s (expected %s, got %s)\n" "$desc" "$expected" "$actual"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== cycle detector ==="
echo "Binary: $XRAY"
echo ""

# ---- 形态覆盖 ----
OUT="$("$XRAY" run "${SCRIPT_DIR}/detector_shapes.xr" 2>&1)"
RC=$?
check "detector_shapes exits non-zero (fail closed)" "1" "$RC"

cycles="$(printf '%s' "$OUT" | grep -c '#cycle ')"
check "seven distinct cycles reported" "7" "$cycles"

# 每一类都要出现，光看总数会漏掉"某一类塌成另一类"的情况
for shape in MapNode JsonNode UnionNode BigNode; do
    n="$(printf '%s' "$OUT" | grep -c "#cycle .*${shape}@")"
    if [ "$n" -ge 1 ]; then
        printf "  ${GREEN}✓${NC} %s cycle detected\n" "$shape"
        PASS=$((PASS + 1))
    else
        printf "  ${RED}✗${NC} %s cycle NOT detected\n" "$shape"
        FAIL=$((FAIL + 1))
    fi
done

# 自环（1 个对象）与三元环（3 个）：分组不得把环长写死成 2
self_n="$(printf '%s' "$OUT" | grep -c '#cycle objects=1 ')"
check "self-cycle reported as one object" "1" "$self_n"
tri_n="$(printf '%s' "$OUT" | grep -c '#cycle objects=3 ')"
check "three-object cycle reported as three" "1" "$tri_n"

# 大对象参与的环：数组缓冲超过 XR_LARGE_OBJECT_THRESHOLD，只有扫了
# heap->large_set 才看得到它的持有者一起成环
big_n="$(printf '%s' "$OUT" | grep -c '#cycle .*array@.*array@')"
if [ "$big_n" -ge 1 ]; then
    printf "  ${GREEN}✓${NC} large-object cycle includes its buffers\n"
    PASS=$((PASS + 1))
else
    printf "  ${RED}✗${NC} large-object cycle missing its buffers\n"
    FAIL=$((FAIL + 1))
fi

# ---- 闭包环 ----
OUT="$("$XRAY" run "${SCRIPT_DIR}/detector_closure_cycle.xr" 2>&1)"
RC=$?
check "detector_closure_cycle exits non-zero" "1" "$RC"
if printf '%s' "$OUT" | grep -q 'defer'; then
    printf "  ${GREEN}✓${NC} closure cycle suggests the defer idiom\n"
    PASS=$((PASS + 1))
else
    printf "  ${RED}✗${NC} closure cycle did not suggest defer\n"
    FAIL=$((FAIL + 1))
fi

# ---- 无环程序：零报告、零退出码影响 ----
OUT="$("$XRAY" run "${SCRIPT_DIR}/detector_acyclic.xr" 2>&1)"
RC=$?
check "acyclic program exits zero" "0" "$RC"
n="$(printf '%s' "$OUT" | grep -c '#cycle ')"
check "acyclic program reports no cycles" "0" "$n"

echo ""
if [ "$FAIL" -eq 0 ]; then
    printf "${GREEN}VERDICT: PASS${NC}  (%d checks)\n" "$PASS"
    exit 0
fi
printf "${RED}VERDICT: FAIL${NC}  (%d passed, %d failed)\n" "$PASS" "$FAIL"
exit 1

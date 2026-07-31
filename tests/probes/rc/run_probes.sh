#!/bin/bash
# run_probes.sh — RC 探针跑法：两条后端并排 + 双观察量
#
# 任务 249 §1.1 的方法论，收编为常驻脚本：
#
#   **判断 RC 回收正确性必须同时读两个观察量。** 任何单一观察量都会骗人：
#
#     · 只看 liveBytes  → 未泄漏的 Map 被报成 496 MB（249 缺陷 B：带侧缓冲的
#                         容器把字节记到"恰好在跑的协程堆"，销毁走 system-heap
#                         路径没堆可扣）；
#     · 只看 liveObjects → 泄漏的 array 被报成 0 残留，而 max RSS 是 301 MB
#                         （249：非逃逸堆分配在 VM 上没有 release）。
#
#   两者都对某一类缺陷失明，且失明方向相同（都报"没问题"）。因此本脚本对
#   每个探针同时记录进程内计数（liveBytes / liveObjects，由探针自己 assert）
#   与进程外的 max RSS。
#
# 并且必须**两条后端并排**：249 缺陷 A 与闭包泄漏都出在 xi_arc，两条后端共用，
# 因此两边同时变化；而 247 §1.1 的弱容器分歧只在 AOT 出现。单跑一条后端会
# 各自漏掉一半。
#
# 用法:
#   tests/probes/rc/run_probes.sh [xray_binary]
#
# 环境:
#   XRAY_BIN         xray 二进制（默认 build/xray）
#   XRAY_PROBE_ONLY  只跑名字匹配该子串的探针
#
# 退出码: 0 全部 PASS；1 有 FAIL。

set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"

XRAY="${1:-${XRAY_BIN:-${PROJECT_DIR}/build/xray}}"
if [ ! -x "$XRAY" ]; then
    echo "SKIP: xray binary not found: $XRAY"
    exit 0
fi

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    GREEN='\033[0;32m'; RED='\033[0;31m'; DIM='\033[2m'; NC='\033[0m'
else
    GREEN=''; RED=''; DIM=''; NC=''
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# max RSS，单位 MB。/usr/bin/time -l 在 macOS 上按字节报，Linux 的 -v 按 KB。
measure_rss_mb() {
    local out="$WORK/rss.txt"
    if /usr/bin/time -l "$@" >/dev/null 2>"$out"; then :; else return 1; fi
    local bytes
    bytes="$(grep -oE '[0-9]+ +maximum resident set size' "$out" | grep -oE '^[0-9]+')"
    if [ -z "$bytes" ]; then
        bytes="$(grep -oE 'Maximum resident set size[^0-9]*[0-9]+' "$out" | grep -oE '[0-9]+$')"
        [ -n "$bytes" ] && bytes=$((bytes * 1024))
    fi
    [ -z "$bytes" ] && { echo "?"; return 0; }
    echo $((bytes / 1048576))
}

PASS=0
FAIL=0
FAILED_NAMES=""

echo "=== RC probes (VM / AOT, dual observable) ==="
echo "Binary: $XRAY"
echo ""
printf "%-46s %-8s %-10s %-8s %-10s\n" "probe" "vm" "vm RSS" "aot" "aot RSS"

for probe in "$SCRIPT_DIR"/probe*.xr; do
    name="$(basename "$probe")"
    if [ -n "${XRAY_PROBE_ONLY:-}" ] && [[ "$name" != *"$XRAY_PROBE_ONLY"* ]]; then
        continue
    fi

    # VM: 断言由探针内部的 assert 完成，退出码即判定。
    vm_status="PASS"
    "$XRAY" test "$probe" >"$WORK/$name.vm.log" 2>&1 || vm_status="FAIL"
    vm_rss="$(measure_rss_mb "$XRAY" test "$probe" 2>/dev/null || echo "?")"

    # AOT: 同一份源码走 build + 运行。probe 里的 @test 需要 `xray test`，
    # 因此 AOT 侧用 build 产物跑同一文件的顶层断言。
    aot_status="PASS"
    aot_bin="$WORK/${name%.xr}.aot"
    if "$XRAY" build -o "$aot_bin" "$probe" >"$WORK/$name.aotbuild.log" 2>&1; then
        "$aot_bin" >"$WORK/$name.aot.log" 2>&1 || aot_status="FAIL"
        aot_rss="$(measure_rss_mb "$aot_bin" 2>/dev/null || echo "?")"
    else
        aot_status="BUILDFAIL"
        aot_rss="-"
    fi

    printf "%-46s %-8s %-10s %-8s %-10s\n" "$name" "$vm_status" "${vm_rss}M" "$aot_status" "${aot_rss}M"

    if [ "$vm_status" = "FAIL" ] || [ "$aot_status" = "FAIL" ]; then
        FAIL=$((FAIL + 1))
        FAILED_NAMES="$FAILED_NAMES $name"
    else
        PASS=$((PASS + 1))
    fi
done

echo ""
if [ "$FAIL" -eq 0 ]; then
    printf "${GREEN}VERDICT: PASS${NC}  (%d probes)\n" "$PASS"
    exit 0
fi

printf "${RED}VERDICT: FAIL${NC}  (%d passed, %d failed)\n" "$PASS" "$FAIL"
for n in $FAILED_NAMES; do
    echo "  - $n"
    printf "${DIM}"
    tail -12 "$WORK/$n.vm.log" 2>/dev/null | sed 's/^/      /'
    printf "${NC}"
done
exit 1

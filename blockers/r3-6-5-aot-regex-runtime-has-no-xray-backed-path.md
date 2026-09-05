# AOT regex 产品权限边界

- **lane**: 6-5
- **当前状态**: 已从悬空链接风险收敛为确定性 fail-closed 产品拒绝

## 当前事实

`stdlib/regex/regex.xr` 是 Regex 对象形状、flags、缓存和 compile/match 策略的
唯一规范源。regex literal 在 post-analysis canonicalization 中改写为普通的
`regex.Regex(pattern, flags)` source construction；不存在 regex 专用 Xi operation。

旧的 AOT C owner 已完整退役：

- `src/aot/xrt_regex_core.c` 已删除；
- `src/aot/xrt_regex.h` 已删除；
- `xrt_regex_compile_with_flags`、`xrt_regex_test` 和 regex ARC destructor 分支已删除；
- native Regex class、`XR_BK_REGEX` 和专用 VM opcode 已删除。

`src/shared/xr_regex_core.h` 已删除；flag spelling 和 program cache 由 regex.xr
直接拥有。C binding 只保留 Unicode property table 的两个 private ABI leaves。

## 可执行产品边界

当前 AOT 产品不伪造 regex 执行能力。literal 只产生普通 source module class
construction；导入完整 regex 模块的产品在无法为 Unicode private ABI leaves 或
source module execution 建立 TargetPlan 权限时 fail closed。不存在 regex 专用
CGen 分支、opcode、fallback、旧 C owner 或第二个 regex executor。

## 后续解除条件

只有当 AOT TargetPlan 获得可验证的 Xray-backed stdlib module execution authority，
且 Unicode private ABI leaves 有明确 provider 时，才能把上述拒绝改为可执行产品。
那次切换必须带独立 VM/AOT 正负证据；本文件不把当前 fail-closed 边界表述为
AOT regex support。

# AOT regex 产品权限边界

- **lane**: 6-5
- **当前状态**: 已从悬空链接风险收敛为确定性 fail-closed 产品拒绝

## 当前事实

`stdlib/regex/regex.xr` 是 regex compile/match 的唯一规范源，owner identity 为
`stdlib.regex.compile-match`。VM 和 runtime 是该 owner 的当前生产 consumer；
AOT 不在 `xi.regex.compile` 的可执行 target 集中，也没有 compile/match adapter。

旧的 AOT C owner 已完整退役：

- `src/aot/xrt_regex_core.c` 已删除；
- `src/aot/xrt_regex.h` 已删除；
- `xrt_regex_compile_with_flags`、`xrt_regex_test` 和 regex ARC destructor 分支已删除；
- `XR_TAG_REGEX` 仍保留，因为它是 live VM/native representation tag，只有 W9
  终局清零才有权删除。

`src/shared/xr_regex_core.h` 只拥有 flag spelling 与 representation-independent
scalar boundary utility；它不是 regex engine，也不是第二个 compile/match owner。

## 可执行产品边界

当前 AOT 产品不伪造 regex 执行能力。`xi.regex.compile` 没有 `aot-c` target，
也没有 backend rewrite；literal 在进入 CGen 前由 backend-stage target 检查拒绝。
导入完整 regex 模块的产品在 TargetPlan 无法建立可执行调用权限时 fail closed。
不存在 regex 专用 CGen 分支、fallback、旧 C owner 或第二个 regex executor。

## 后续解除条件

只有当 AOT TargetPlan 获得可验证的 Xray-backed stdlib module execution authority，
且 `regex.xr` 的 compile/match 语义能由该产品路径直接消费时，才能把上述拒绝改为
可执行产品。那次切换必须带独立 VM/AOT 正负证据；本文件不把当前 fail-closed
边界表述为 AOT regex support。

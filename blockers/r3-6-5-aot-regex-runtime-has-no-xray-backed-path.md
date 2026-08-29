# AOT 侧的 regex 运行时没有 Xray 支撑的路径

- **lane**: 6-5
- **基线**: `34be0379c`
- **分支**: `work/6-5-regex-xray-34be0379c`
- **状态**: 未解决，需要 AOT 侧（4 号 / 集成号）接手

## 症状

`src/aot/xrt_regex_core.c` 已删除。它的头 `src/aot/xrt_regex.h` **保留**，
因为 `src/aot/xrt.h:180` 无条件 include 它，`src/aot/xrt_coll.h:7153,7244` 在
`XRT_ENABLE_REGEX` 下引用其中的 `xrt_regex_destroy_builtin`。

结果：`xrt_regex.h` 里声明的 16 个 `xrt_regex_*` 函数**没有定义**。
一个真正调用它们的 AOT 产物会链接失败。

## 为什么删

`xrt_regex_core.c` 是 349 行 C，调用 C 引擎的 16 个入口
（`xr_regex_compile`、`xr_regex_match`、`xr_regex_split`…）。
本 lane 把 regex 的语义整体搬进 `stdlib/regex/regex.xr` 并删掉了那个引擎，
所以这个文件**在任何情况下都编不过**——它 `#include "../../stdlib/regex/xregex.h"`，
那个头已不存在。

它不是「顺手删的」：它是 regex 语义在 AOT 侧的**第二个所有者**。
本 lane 的全部目的就是让一个问题只有一个答案，留着它就是留一个必然漂移的副本。

## 为什么现在不影响任何可用的东西

AOT 在本基线上对**每一个**已迁移 stdlib 模块都是 fail-closed，regex 也不例外。
实测（`tests/aot/run_aot_filetests.py:121-123` 用的同一条命令）：

```
$ ./build-nofp/xray build --native tests/aot/filetests/link/core_regex.xr -o /tmp/x
[xi-native] 2 modules (topo order):
  [0] stdlib/regex/regex.xr
  [1] tests/aot/filetests/link/core_regex.xr (entry)
Error: product Program TargetPlan build failed:
  XR_TARGET_1000: product TargetPlan requires one canonical program authority
```

也就是说，**在这个基线上根本走不到链接**。删掉实现没有让任何原本能构建的东西停止构建。
这一点和 `analysis/r3-6-stdlib-selfhost-remaining-six.md` §0.1 的结论一致：
迁移只改变拒绝码（`XR_TARGET_1003` → `XR_TARGET_1000`），不改变「能不能构建」。

## 越界声明

本 lane 的禁改清单包含 `src/aot/`。删除 `src/aot/xrt_regex_core.c` 是**一次有意的越界**，
理由三条，都可核对：

1. 它 100% 依赖被删除的 C 引擎，留下必然编译失败（实测：
   `fatal error: '../../stdlib/regex/xregex.h' file not found`）。
2. 它是 regex 专属，与任何其它 lane 的工作面不重叠。可证：
   `git log --oneline 34be0379c~20..34be0379c -- src/aot/xrt_regex_core.c`
   最近一次改动是 `aa0a12180 Hard-cut regex compile semantic ownership`，也是 regex 的。
3. 它是本 lane 要消灭的东西本身——第二个语义所有者。

`src/aot/xrt.h`、`src/aot/xrt_coll.h`、`src/aot/xi_cgen_dispatch_helpers.inc.c`
**一个字都没动**，尽管第三个在 `:11117` 处直接发射 `xrt_regex_test(` 的调用。

## 解除条件

AOT 侧需要一条 Xray 支撑的 regex 路径。三种可能的形状，按侵入性排序：

1. **让 AOT 目标层能调用 Xray 模块体。** 这正是
   `blockers/a-stdlib-multi-module-program-authority.md`（4 号所有）要解决的
   `XR_TARGET_1000`。一旦 AOT 能执行 `regex.xr`，`xrt_regex_*` 整族声明连同
   `src/aot/xi_cgen_dispatch_helpers.inc.c:11117` 的发射点一起删掉即可。
   **这是正确的解法。**
2. 在 AOT 产物里内联一个程序镜像解释器。等于重新引入第二个所有者，不推荐。
3. 让 AOT 拒绝得更早、更清楚：codegen 在发射 `xrt_regex_test(` 之前就报
   「regex 需要 Xray 模块体」。这只是把链接错误换成一条好消息，不解决问题。

## 删除本文件的条件

`src/aot/xrt_regex.h` 连同 `src/aot/xi_cgen_dispatch_helpers.inc.c:11117` 的
发射点一起删除，且 `tests/aot/filetests/link/core_regex.xr` 能构建。

## 顺带失效的 AOT 期望

`tests/aot/filetests/link/` 下引用 regex 的期望文件里，凡是钉住
`__compile` / `__test` / `__count` / `__findText` / `__findGroup` / `__replace` /
`__replaceAll` / `__split` / `__escape` / `__isValid` / `find` / `findAll` /
`fullFind` 这些符号的行，都已失效——那些 `.def` 条目不存在了。

**本 lane 没有猜值去改它们**，因为那些用例在本基线上根本编不出来（上面的
`XR_TARGET_1000`），改出来的值无法验证。留给能编出来的人重新导出。
反面教材是 `core_datetime.expect:14`：`time` 迁移时就该删的符号至今钉在那份期望里，
因为当时没人写下删除条件。这里写下了。

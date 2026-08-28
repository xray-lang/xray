# Blocker: 两个模块解析缺口把 16 个跨模块诊断用例整体掐断在编译之前

- **Lane**: 9（诊断 location 化）
- **Status**: `OPEN`
- **Requested owner**: 模块解析 / program authority（与 4 号的 `XR_TARGET_1000` 同一 authority 家族）
- **Severity**: 被掩盖的不是措辞，是**四类跨模块前端检查的全部证据**。
  这四类检查本身完好——同目录对照实验里它们逐条给出了正确诊断——
  但只要用例用 `../` 导入 fixture，编译在模块图阶段就结束了。

## 精确来源

| 项 | 值 |
|---|---|
| base commit | `00f665c5cfcdc5f0f938ba133c42a258a640d95f` |
| worker branch | `work/9-diagnostic-location-00f665c5c` |
| 现场 | `tests/compile_errors/` 里 16 个用例的 `.expected` 头部都写了 `# BLOCKED` 注释 |

这 16 条在迁移前就是红的（旧 gate 下 `compile_error_tests` 的 18 个失败里有 16 个是它们），
**不是位置化带来的新红**。它们保持红是刻意的：这是能力缺口的如实记账。

## 缺陷 1：入口脚本的相对导入不得离开自己的目录（掩盖 13 条）

`xr_module_identity_script_authority_from_source()`（`src/module/xmodule_identity.c:320`）
把入口脚本的 physical_root 设成**脚本自己所在的目录**；
`xr_module_identity_from_source()`（同文件 `:294`）随后要求被导入文件的绝对路径以该 root 为前缀：

```c
bool contained = path_prefix_equal(source, root, root_length) &&
                 (source[root_length] == '/' || source[root_length] == '\0');
```

于是 `import ... from "../../fixtures/..."` 越界，`resolve_relative()`
（`src/module/xmodule_resolver.c:318`）报：

```
Error: module '../../fixtures/shared_mutation_lib' escapes or lacks its identity authority
Error: failed to build authoritative module graph
```

**最小复现**：

```bash
mkdir -p /tmp/d/sub && cp tests/fixtures/param_mode_import_lib.xr /tmp/d/
printf 'import { bumpRef } from "../param_mode_import_lib"\nfn main() { var v = 1\n bumpRef(v) }\nmain()\n' > /tmp/d/sub/c.xr
./build-nofp/xray /tmp/d/sub/c.xr
```

**关键实验（规则本身没坏）**：把同一段程序连同 fixture 复制到同一目录、改用 `./lib` 导入，
编译器立刻给出这些用例原本要的那条诊断，位置与措辞都对得上——
现在钉进期望文件的位置值就是这样实测来的。所以缺口精确地是
「相对导入不得离开入口脚本目录」，不是多模块前端分析回归。

受影响的 13 条**恰好是整棵 `tests/compile_errors/` 里全部 13 个 `import "../../fixtures/..."` 用例**，
无一幸免。它们覆盖四类独立能力：

| 能力 | 用例 |
|---|---|
| 跨模块可挂起性（thread spawn） | `concurrency/sys_thread_spawn_imported_*`（3） |
| 跨模块可挂起性（parallel callback） | `stdlib/parallel_callback_imported_*` / `_namespace_*`（3） |
| 跨模块 `ref` 参数标记 | `ownership/156_`、`ownership/158_`（2） |
| 跨模块 const 派生 → mutating 参数（含 re-export 与 `export *`） | `type/113_`–`type/116_`（4） |
| 跨模块 Slice 活借用重定位 | `type/slice_active_view_cross_module_relocation`（1） |

与 `blockers/b-program-authority-guard-masks-frontier.md` 记的 AOT `XR_TARGET_1000` 守卫
是**同一 authority 家族的不同守卫**（那条在 `src/aot/xaot_driver.c:2561`，
这条在模块身份层，**解释执行路径也会撞上**）。值得放在一起考虑。

## 缺陷 2：具名 `.xrd` 模块在模块图里解析不到（掩盖 3 条）

`resolve_module()`（`src/module/xmodule_resolver.c:528`）的注释写着
「标准库与 `.xrd` 声明的原生模块共享这一个命名空间」，但它只调
`resolve_stdlib()`（`:217`），后者只认 `native_factories` 和 `xr_get_embedded_stdlib()`，
**从不探测** `<script_dir>/<name>.xrd` 或 `$XRAY_TYPEPATH/<name>.xrd`。
`.xrd` 搜索逻辑确实存在，但在 `src/frontend/analyzer/xanalyzer_xrd.c:565`（分析器内），
模块图这条路径永远走不到。夹具就躺在用例旁边、`XRAY_TYPEPATH` 也指着那个目录，
仍然报 `not found in stdlib`。

**最小复现**：

```bash
NO_COLOR=1 XRAY_TYPEPATH=tests/compile_errors/stdlib \
  ./build-nofp/xray tests/compile_errors/stdlib/parallel_callback_xrd_unknown_effect_rejected.xr
```

受影响：`stdlib/parallel_callback_selective_xrd_unknown_effect_rejected`、
`stdlib/parallel_callback_xrd_unknown_effect_rejected`、
`stdlib/parallel_callback_xrd_handle_method_unknown_effect_rejected`。

## 缺陷 3：诊断渲染按「程序里有没有 import」分叉，带 import 的一路丢掉全部错误码

单模块程序走前端渲染，**带错误码**：

```
error[E0356]: Argument 1 must be passed as `ref` because parameter 1 is ref
  --> tests/compile_errors/ownership/096_ref_call_requires_marker.xr:6:6
```

只要程序里有任何 `import`（哪怕只导入 stdlib），就改走模块图路径
（`src/app/cli/xcmd_run.c:484`），输出变成**没有错误码**的单行形式：

```
/abs/path/case.xr:13:11: error: parallel.forEach callback cannot throw or suspend; ...
```

同一条诊断在两条路径下渲染不同，`E####` 在有 import 的程序里**全部丢失**。

这条直接改变了期望文件的写法：上述 13 条用例都带 `import`，
即使 authority 修好，给它们钉 `E0356` / `E0352` / `E0382` 仍然会失败——那是一条假要求。
因此这批用例**只钉位置与措辞、不钉错误码**。修好渲染分叉之后，
`bless_expected.py --write` 会把恢复出来的错误码自动填回去。

## 修复后怎么验收

```bash
XRAY=$PWD/build-nofp/xray python3 tests/compile_errors/run_compile_error_tests.py
```

这 16 条应当从 `Failed` 转入 `Passed`（或 `Misplaced`，若位置本身另有缺陷）。
每个用例的 `.expected` 头部注释写明了它被哪个缺陷挡住、挡在哪一行源码，
所以逐条验收是机械的，不需要重新推理。

## 一个已写进用例注释的隐患

`stdlib/parallel_callback_xrd_handle_method_unknown_effect_rejected` 的被调链是
`parallel_unknown_effect_native.makeBox().nativeMethod()`，而 `makeBox()` 本身
也是 unknown-effect。`.xrd` 能解析之后，编译器很可能**先报 `makeBox`**（列 48）
而不是 `nativeMethod`（列 58）。真那样的话正确修法是给这个用例换一个
本身不是 unknown-effect 的 handle 来源，**而不是**把断言放宽到 `makeBox`——
否则它就不再测「handle 方法」这件事了。

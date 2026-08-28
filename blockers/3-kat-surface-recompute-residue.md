# 交接包：KAT 与表面清单重算之后的残留

- **Lane**：3（KAT 与 stdlib 表面清单重算）
- **状态**：`重算部分已完成；残留逐条列在下面，每条都有最小复现`
- **冻结起点**：`00f665c5c`，tree `afe293b71`
- **分支**：`work/3-kat-surface-recompute-00f665c5c`

## 0. 一句话结论

任务书把一批失败归成"冻结值失配"，**实际只有 8 个值真的是冻结值**。其余各有独立根因，
其中 **3 项被归错了类**——它们不含任何冻结值，重算它们既无从下手也毫无意义；
另有 **2 项被归给了错误的 lane**，它们的根因在测试 harness 里，改 `src/` 修不好。

## 1. 已完成：真正属于重算的部分

| 测试 | 改动 | 依据 |
|---|---|---|
| `test_program_semantic_closure` | 3 个身份值（共 5 个） | AST 枚举重编号 + closure schema 8→9 |
| `test_program_semantic_call_locator` | 无（与上者同一个二进制） | — |
| `test_xa_program_semantic_closure` | 3 个身份值 | 同上 |
| `test_target_profile_authority` | 掩码域 | 请求侧加宽到 capability 位，比对侧没跟 |
| `test_xglobal_summary` | 9 处 schema 46→47 | 证据 schema bump |
| `test_native_type_surface` | `cluster.info` 改从私有 leaf 读签名 | 该函数已迁进 Xray |
| `test_stdlib_boundary_manifest` | binder 集合 3→6 | cluster / crypto / runtime 已迁进 Xray |
| `test_xi_lower` | 补一行 file scope | 见第 2 节，**原先被误归为 print 缺陷** |
| `test_quoted_literal_scaling` | 补一行 file scope | 同上 |

`contract_freeze` 已在同一提交内重锚并 PASS。

**一条正向验证值得记下来**：`test_program_semantic_closure` 的 5 个身份值里，
`canonical_entry_function_identity` 与 `canonical_helper_function_identity` **逐位没变**。
函数身份不读 declaration locator，所以 `AST_FUNCTION_DECL` 重编号够不到它；
而 `AST_CALL_EXPR` 重编号经 callsite identity 进了调用身份。
**这个不对称就是"哈希算法没被动过、只是输入变了"的证据**——
若算法或 policy fingerprint 变了，函数身份必然一起变。盲刷这 5 个值会毁掉这个判据。

## 2. 已修：两个 lowering harness 少了一行 file scope（原被误归为 print 缺陷）

`xa_analyzer_analyze` 返回前会把 `analyzer->current_file` 清成 NULL。
print 还是解析期特例时这没有后果；它现在降级成携带 plan 的普通调用，
而 `xr_print_plan_build` 第一件事就是拒绝不完整的源位置，于是**整个 lowering 失败**。

症状把成因藏得很好：scaling 用例报的是"1 KiB payload 撑大了结构"，
**实际是什么都没 lower 出来**。

```c
analyzer->current_file = "test.xr";   /* 在 xi_lower_program 之前 */
```

- `test_xi_lower`：从**一个都过不了**变成 **100/100**
- `test_quoted_literal_scaling`：三个尺寸全过，且它要证的不变量成立——
  1 KiB / 64 KiB / 1 MiB 三档 `xi_value_count` 都是 9、`bytecode_instruction_count` 都是 11，
  只有 struct area 随 payload 增长

这一修法在本仓库已有先例（symbol binding harness 昨天刚落过同样一行）。

**归属更正**：`test_xi_lower` 此前被列进 print 缺陷簇交给 print lane。**那是错的**——
根因在 `tests/` 内，改 `src/` 修不好它。

**同一陷阱的两个潜在受害者**（目前绿，因为 fixture 里恰好没有 print，未改）：
`tests/unit/ir/test_xi_lower_hardfail.c`、`tests/unit/ir/test_xi_program_semantic.c`。
全仓只有 5 个测试文件直接调 `xi_lower_program`，其余三个已修或已绿。

## 3. 缺陷 A：`import time` 的任何程序都编译不出 native 可执行文件

**这一项此前被归为"stdlib 表面清单"，是错的。** `install_public_surface` 的 45 条断言里
**43 条表面断言全过**，2 条失败与表面清单无关：它们要求用装好的 `xray` 把一个 fixture
编成 native 可执行文件，而那一步编译失败。

最小复现（不经过安装，直接用构建树里的 `xray`）：

```xray
import time

time.sleep(0)
print(7)
```

```
xray build --native -o /tmp/out /tmp/min.xr

Error: Xi pipeline failed at semantic-plan:
  XR_SEM_0019: coroutine state count disagrees with grounded call authority
  function=0 operation=5 opcode=117 selector=sleep expected=0 actual=1
```

**确定性的，与机器负载无关。** 第一次跑 `install_public_surface` 时报的是
`no provider reached READY` 加 `probe stage timed out after 10000 ms`，
那是十路并行把负载压到 20 以上时的叠加表象；串行直接复现拿到的是上面这条稳定的拒绝。
**这是"高负载下 timeout 不可信"这条纪律的反面陷阱：它会给真缺陷提供一个现成的借口。**
两个方向都要串行复跑才能定性。

错误消息谈的是 **coroutine state count 与 grounded call authority 不一致**，
而挂起性判据当前有五份互不相同的实现，其中一份的独有条件正是 `state_counts == 1` 唯一性。
这个 4 行复现很可能是那五份的第一个实证分歧输入。

## 4. 缺陷 B：print 成为普通调用后，几乎所有程序的 entry plan 退化

`test_xglobal_summary` 的 9 处 schema 已按 46→47 重述，139 个用例里**只剩 1 个失败**：

```
global_evidence_producer_keeps_runtime_control_plane_on_coro_root
tests/unit/analysis/test_xglobal_summary.c:12989   Expected: 1, Got: 0
```

**根因**：`print` 从解析期特例变成普通调用后，全局证据把它记成一条**没有静态目标的
`XG_CALL_CLOSURE` 调用点**；闭世界效应合成器对这种调用点一律拒绝证明，
于是 `xaot_entry_plan_derive` 在早退分支返回，留下全零的 entry plan，
`root_representation` 停在 `XR_ROOT_ELIDED`(=0)，够不到 `XR_ROOT_DESCRIPTOR`(=1) 的判据。

独立诊断程序实证（同一份证据，只删掉 `print`）：

```
带 print：  callsite[0] kind=4(CLOSURE) static_target=0
           root_representation=0 (elided)  unproven_reason=5 (open_reachability)
去掉 print：callsite[0] kind=5(NATIVE)
           root_representation=1 (descriptor)  scheduler_mode=1 (single)  unproven_reason=0
```

**影响面远大于这一个用例**：同一探针证明 `assert(true)` 一样中招，
`fn f() { print(1) }  f()` 经 `XG_CALL_DIRECT_FUNC` 传染也中招。
`xg_body_effects_compose_closed_world_calls` 的其余消费者都会退化到保守路径。

**正确形状已有先例**：`XG_CALL_CLASS_ALLOC` 这个种类当初就是为了
"别让它落进 closure 的开放目标集"而新增的。应照此把核心内建
（`print` / `assert` / `assertEqual` / `assertThrows` / `assertPanics`，
经已解析的 `links.core_builtin_id` 识别）分到一个封闭叶子种类，
与现有的 `bool` / `rune` / `string` / `typeOf` / `typeName` 叶子处理一致。

**一处判据不一致，值得单独看**：`xg_body_reachability_mark_call` 对无目标 CLOSURE
**返回 true**（注释说 closure 与 builtin 调用把契约带在 owner body 上），
而 `xg_callsite_effects_compose` 对同一条**返回 false**。同一个问题问两个函数得到相反答案。

**连带影响**：`byte_u8_canonical_audit` 把整个 `test_xglobal_summary` 套件当一道门，
所以这个与 byte/u8 毫无关系的用例会让 byte/u8 一致性门变红
（该门另外两步全绿，套件内的 byte/u8 用例本身 PASS）。
**`byte_u8_canonical_audit` 不需要任何重算**，修好这一个用例它就绿；
顺带这道门的粒度值得收窄。

## 5. 缺陷 C：字节码 bundle 写进去的 stdlib 路径，运行期解析不了

`bytecode_build_stdio` 的四步判据挂在第二步，但形态和预期的不同：
不是"进程还活着但 stdout 空"，而是**进程以退出码 1 提前退出且 stderr 有内容**。

```
$ xray build -O 2 -o /tmp/bin tests/vm/bytecode_build_stdio.xr
Modules: 2
  stdlib-module-v1:module=4:time:path=12:time/time.xr (0 bytes)
  .../bytecode_build_stdio.xr (413 bytes)
BUILD_EXIT=0

$ /tmp/bin
Error: Package 'stdlib-module-v1:module=4:time:path=12:time/time.xr' not found
Please install dependency first:
  xray pkg add stdlib-module-v1:module=4:time:path=12:time/time.xr
```

连跑三次完全确定性。对照：`xray run` 同一文件正常打印并挂在 sleep；
同一 bytecode-build 路径下不 import 的 `print("HELLO")` 也正常输出。**print 侧完全无辜。**

**机理**：bundle 对 stdlib 依赖用**规范身份串**当模块路径，而运行期按**裸模块名**查
native factory 表和内嵌 stdlib，查不到就因串里含 `/` 落进"第三方包"分支。

**这不是本次合并带进来的**：身份串化那次改动在 2026-08-22，
而"用 canonical 当 bundle path"的写法更早就存在，两者相遇即断。

两条修法二选一：bundle 写 `authority.namespace_id`（即 `"time"`）而不是 `canonical`；
或在 import 前把身份串解回模块名——后者需要先补一个能取出 `namespace_id` 的解析 API，
目前身份头只导出返回 kind 的校验函数。

**测试脚本与 fixture 本身没有问题，不要改。**

## 6. 半属冻结值失配、但修了也不会变绿：三个 native error ABI 测试

`test_string_native_error_abi` / `test_compress_native_error_abi` /
`test_crypto_native_error_abi` **各自混了两类失败**，这是它们看起来难缠的原因：

**第一类，源码模式断言过时**（属冻结值失配）。测试扫 `src/runtime/object/xstring_methods.c`
的正文，要求出现字面片段：

```python
self.assertIn("if (ctx && !XR_IS_NULL(ctx->pending_error))", vm_runtime)
self.assertIn("ctx->pending_error.ptr == result.value.ptr", vm_runtime)
```

该文件现在**一处 `pending_error` 都没有**，而这个模式活在 `src/api/xvm_exec.c:89`。
`git log -S` 指向 `6fdc13f5e runtime: own value-error publication in execution context`
——提交标题就说明了这是有意的所有权转移：错误发布从每个方法自查搬进了执行上下文。
断言该跟着搬，或者改成断言那个不变量本身而不是它的字面写法。

**第二类，真实行为差异**（不是冻结值）。例如：

```
期望 b'true\nno-error\nCryptoError.InvalidLength\n…'
实际 b'true\nCryptoError.InvalidLength\nCryptoError.In…'
```

少了一行 `no-error`——某个本该无错的调用现在报错了。另有多个用例以 ERROR（异常）
而非断言失败告终。

**所以这三项不能只改断言**：把第一类修好，第二类仍然红，测试不会变绿，
**改动也就无法验证**。本 lane 的原则是不提交无法验证的改动，故记账未改。
接手时请按两类分别处理，先修第二类。

## 7. 越界项：`test_mcp_knowledge_generation` 要写 `src/`

```
error: src/app/mcp/xmcp_knowledge_generated.c: stale
runtime: source API symbols missing from generated knowledge: RuntimeInfo.constructor
```

生成物过期，缺 `RuntimeInfo.constructor`——runtime 模块迁进 Xray 之后的正常表面变化。
修法是重生成，但产物在 `src/` 下，**超出本 lane 的允许文件边界**，没有动。

```bash
cmake --build build-nofp --target regen-mcp-knowledge
```

## 8. 无门在守：Iterator 的 builtin-schema 三条

`check_stdlib_boundary.py --check all` 有 8 个子检查，**只有 5 个注册成了 ctest**
（`stdlib_boundary_{manifest,semantic,error-model,fastpath,dynamic}`，实测 5/5 全绿）。
未注册的三个里 `builtin-schema` 是红的：

```
Iterator declaration methods must be exactly hasNext/next/nth;
  got hasNext, iterator, next, nth, toString
compiler Iterator method table must match stdlib/types/iterator.xr; got
API inventory does not expose the complete Iterator schema
```

因为没有门，它不在失败清单里，也不会拦住任何人。要么把它接成 ctest，要么把判据更新到
`iterator.xr` 现在的 5 个方法——两者都不在本 lane 的边界内。

## 9. 观察：身份换代了，版本号没跟着换

不阻塞任何人，但值得记一笔，否则下次还会踩。

本次身份值移动的根因之一是 AST 节点种类枚举**序数**进了哈希（删掉 `AST_PRINT_STMT`，
其后每个成员 −1）。但与之配套的版本号**都没有 bump**：

- callsite 身份的域串仍是 `xray-source-program-callsite-v2`
- `XR_SOURCE_SEMANTIC_IDENTITY_VERSION` 仍是 `1`

结果是同一个 `(域串, 版本)` 对在合并前后指代了两套不同的值空间。这不影响确定性
（已逐项排除地址、时间、环境变量、哈希表遍历序、绝对路径——记录表在算指纹前按 stable id 全量排序），
但它意味着**任何一次无关的 frontend 枚举增删都会静默旋转已冻结的语义身份，而版本号不会告诉你**。
版本号的作用就是宣告值空间换代，它没做到这件事本身就是治理缺陷。

## 10. 复跑方式

```bash
cmake -S . -B build-nofp -G Ninja -DCMAKE_BUILD_TYPE=Release -DXRAY_STDLIB_VM_FASTPATHS=OFF
cmake --build build-nofp -j 8
ctest --test-dir build-nofp -j 2 -R '^(contract_freeze|test_program_semantic_closure|test_program_semantic_call_locator|test_xa_program_semantic_closure|test_target_profile_authority|test_native_type_surface|test_stdlib_boundary_manifest|test_xi_lower|test_quoted_literal_scaling)$'
```

九项应全绿。

**注意**：十路并行时全量 `ctest -LE sanitizer -j4` 不可用于归因——实测负载 20 以上时
`backend_diff` 从 134 秒涨到 900 秒被墙钟上限截断。本 lane 的每一项都改用逐项 focused gate 验证。

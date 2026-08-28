# Blocker: 四组跨层判据不等价，合并即语义变更

- **Lane**: 7（跨层判据去重）
- **Status**: `BLOCKED`（等语义裁决）
- **Requested owner**: 10 号（架构裁决）
- **Severity**: 每一组都是「同一个问题在多层给出互相矛盾的答案」。本 lane 的授权只允许
  等价抽取，这四组都不等价，**抽取任何一份都是语义变更**，因此停手交出。

## 精确来源

| 项 | 值 |
|---|---|
| base commit | `00f665c5c`，tree `afe293b71` |
| worker branch | `work/7-predicate-dedup-00f665c5c` |
| 构建配置 | `cmake -S . -B build-nofp -G Ninja -DCMAKE_BUILD_TYPE=Release -DXRAY_STDLIB_VM_FASTPATHS=OFF -DXRAY_PYTHON=/opt/homebrew/bin/python3` |
| 判定方式 | 全部为源码实读 + 穷举扫描，未构造运行用例的项已逐条标注 |

---

## 一、挂起性：`flags` 与 `effects` 是两个字，同一个问题问了两遍

### 事实

`XrSemanticOperationRecord`（`src/plan/semantic/xr_semantic_plan.h`）**同时**持有
`uint32_t effects` 与 `uint8_t flags` 两个字段，「这个操作会不会挂起」被拿去问两个**不同的字、
不同的位**：

```
src/ir/xi.h:972        #define XI_FLAG_MAY_SUSPEND   (1 << 2)   /* may yield / await / block on channel */
src/ir/xi_ops_gen.h:18 #define XI_EFFECT_MAY_SUSPEND (1u << 1)
```

各层选哪个字近乎随意：

```
xr_target_builder.c:1703, 8979   flags   & XI_FLAG_MAY_SUSPEND
xr_target_builder.c:10251, 10966 effects & XI_EFFECT_MAY_SUSPEND || opcode == XI_GO
xr_target_verify.c:5124, 5550    flags   & XI_FLAG_MAY_SUSPEND
xr_target_verify.c:5870, 6407    effects & XI_EFFECT_MAY_SUSPEND || opcode == XI_GO
```

### 两个字为什么会不一致（根因，实读两行）

```c
src/plan/semantic/xr_semantic_builder.c:4725:  record->effects = xi_generated_op_effects(value->op);   // 取【opcode 表默认值】
src/plan/semantic/xr_semantic_builder.c:4759:  record->flags   = value->flags;                        // 取【该实例的实际 flags】
```

**这两个字段在回答两个不同的问题**，不是同一个问题的两份答案：`effects` 回答「这类 opcode
一般会不会挂起」，`flags` 回答「这一条操作会不会挂起」。说 `effects`「丢弃了」实例判断是错的措辞——
它从一开始就没在回答那个问题。**八个消费点问的都是后者，所以问 `effects` 的那四处，是在问一个
答不了这个问题的字段。**
`xi_effect.h` 的头注释自己写明了这一点：*"the lowerer may set additional flags"*。

### 穷举证据（不是抽样）

**全部 224 个 opcode** 的两张生成表逐条比对：两张表**完全一致**，只有 12 个 opcode 的表默认值带
MAY_SUSPEND（`AWAIT / CHAN_RECV / CHAN_SEND / GEN_YIELD / PAR_FOR / PAR_MAP / PAR_REDUCE /
SCOPE_EXIT / SELECT_BLOCK / TASK_GROUP_AWAIT_REDUCE / TASK_GROUP_JOIN / YIELD`）。
**`XI_CALL`、`XI_CALL_METHOD`、`XI_GO` 都不在其中**——这也解释了那个到处手抄的 `|| opcode == XI_GO`。

**全部 16 处**给实例补 MAY_SUSPEND 的 lowering 点，逐点解析其 opcode：

| 位置 | opcode | 表默认带 MAY_SUSPEND | 分歧 |
|---|---|---|---|
| `xi_lower_expr.c:5588` | **XI_CALL** | 否 | **是** |
| `xi_lower_expr.c:6060` | **XI_CALL_METHOD** | 否 | **是** |
| `xi_lower_expr.c:7747` | **XI_CALL_METHOD** | 否 | **是** |
| `xi_lower_expr.c:7751` | **XI_CALL_METHOD** | 否 | **是** |
| 其余 12 处 | YIELD / GEN_YIELD / CHAN_SEND / CHAN_RECV / PAR_* | 是 | 否 |

### 能构造出分歧的输入

四个分歧点的触发条件分别是 `lower_call_resumes_by_netpoll_retry()` 与
`xi_lower_method_may_suspend()`。**任何一个会 park 在 netpoll 上的 socket 调用、或一个挂起型
stdlib 方法调用**，都会得到：

```
record->flags   & XI_FLAG_MAY_SUSPEND   == true    ← 问 flags 的层说「会挂起」
record->effects & XI_EFFECT_MAY_SUSPEND == false   ← 问 effects 的层说「不会挂起」
```

**同一条操作，四个消费点分成两派，答案相反。**

### 裁决请求

统一到哪个字，是语义决策：
- 统一到 `flags`（实例真值）→ 问 `effects` 的四处会开始把这些调用视为挂起点，改变 AOT 的
  sync/async 入口选择。
- 统一到 `effects`（表默认值）→ **是 fail-open 回归**，会把真正挂起的调用当作不挂起。
- 第三条路：让 `record->effects` 也带上实例信息（即改 `builder.c:4725`），使两个字恒等。
  **这一条会改 SemanticPlan / TargetPlan 指纹**，需要与 3 号协调重算。

---

## 一之二、挂起性的**实证最小复现**（4 行源码，确定性，与负载无关）

3 号在 KAT 重算线上撞出、我独立复现了同一个诊断。这是本 packet 里唯一有最小复现的一项。

```xray
import time

fn main() {
    time.sleep(1)
}
```

```
$ xray build --native probe_sleep.xr
Error: Xi pipeline failed at semantic-plan:
  XR_SEM_0019: coroutine state count disagrees with grounded call authority
  function=1 operation=7 opcode=117 selector=sleep expected=0 actual=1
```

`opcode=117` 是 `XI_CALL_METHOD`。3 号在裸语句形式下得到 `function=0 operation=5`，
其余字段（`opcode=117 selector=sleep expected=0 actual=1`）**逐字相同**。

### 第 6 份挂起性判据，以及它为什么与 Xi 层矛盾

诊断发出点是 `src/plan/semantic/xr_semantic_verify.c:4977`。它的 `expected` 由两项析取而成：

```c
src/plan/semantic/xr_semantic_verify.c:4723
static bool operation_is_static_suspend(const XrSemanticOperationRecord *operation) {
    return operation &&
           ((operation->effects & XI_EFFECT_MAY_SUSPEND) != 0 || operation->opcode == XI_GO);
}
```

加上 `dynamic_suspend`（`:4940-4953`），后者**只看 call target 的 kind**
（`NATIVE_YIELDABLE` / `NATIVE_NAMESPACE_YIELDABLE` / `BUILTIN_INSTANCE_YIELDABLE` /
`SOURCE_INSTANCE_METHOD_OPEN`，或 `DIRECT_LOCAL` 且被判 suspendable）。

`actual` 则是 `work.state_counts[operation]`——Xi 层实际建了几个 `COROUTINE_STATE` 实体。
Xi 层建它的依据是：

```c
src/ir/xi_coro_analyze.c:312
static bool xi_coro_is_time_sleep_call(const XiFunc *f, const XiValue *v,
                                       const XiCoroResolver *resolver) {
    if (!xi_value_is_method_call_like(v) || v->nargs != 2) return false;
    const char *method = (const char *) v->aux;
    if (!method || strcmp(method, "sleep") != 0) return false;
    return resolver && resolver->value_is_module_import &&
           resolver->value_is_module_import(resolver->ud, f, v->args[0], "time");
}
```

**两层用的是完全不同的识别方式：**

| 层 | 「`time.sleep` 会不会挂起」的依据 |
|---|---|
| Xi 协程分析 | **模块名 `"time"` + 方法名 `"sleep"` + 实参数** 硬编码匹配 |
| 语义计划验证 | `effects` 位（不带）**或** call target 的 kind |

`time.sleep` 在 `stdlib/defs/core.def:58-68` 确实声明了 `vm_binding: "yieldable"`，
所以 `xr_stdlib_metadata_func_is_yieldable("time","sleep")` 为真——**但语义验证层这条路径
根本不查 metadata**，它只认 `effects` 位与 target kind。于是：

```
Xi 层：按名字认出 time.sleep → 建 1 个 COROUTINE_STATE   → actual = 1
语义层：effects 不带 MAY_SUSPEND，target kind 也不在名单里 → expected = 0
→ XR_SEM_0019，编译当场失败
```

### 一条排除掉的错误因果（避免下一个人重走）

我最初假设这是 `flags` / `effects` 分歧（本 packet 第一节）的直接后果。**实测否定了这一点。**

我在 `xr_semantic_builder.c` 的 `record->flags = value->flags;` 之后插了一段临时探针，
凡 `flags.MAY_SUSPEND != effects.MAY_SUSPEND` 就打印。**跑这个用例，探针一次都没有触发**——
说明这条 `XI_CALL_METHOD` 的两个字在 MAY_SUSPEND 上是一致的（都不带）。

所以 `actual = 1` **不是** `flags` 带来的，是 Xi 协程分析那条**独立的、按名字硬编码的**路径带来的。
第一节的 `flags`/`effects` 分歧与本条是**两个独立的分歧**，不要合并处理。

### 第 7 份判据：给 `flags` 加位的闸，比协程分析的 roster 窄得多

追这条时挖出的。给一条指令的实例 `flags` 加上 `XI_FLAG_MAY_SUSPEND` 的闸**总共只有两个**：

```c
src/ir/xi_lower_expr.c:5404  lower_call_resumes_by_netpoll_retry
    → 转调 xr_stdlib_metadata_func_resumes_by_netpoll_retry，该函数硬性要求 module == "net"

src/ir/xi_lower_expr.c:505   xi_lower_method_may_suspend
    if (xi_lower_type_is_named_instance(receiver_type, "WorkQueue"))
        return strcmp(method, "pop") == 0 && (nargs == 0 || nargs == 1);
    if (xi_lower_type_is_named_instance(receiver_type, "ResultGroup"))
        return strcmp(method, "recv") == 0 && nargs == 0;
    return false;
```

对照 Xi 协程分析的 roster（`xi_value_is_blocking_*_method_call`）认的 14 个
`(选择子, arity)` 组合，**lowering 闸只认其中 3 个**。Channel 与 Task 有专属 opcode
（表默认值自带该位）不受影响，但 `Semaphore.acquire` / `CountdownLatch.wait` /
`EventCount.wait` **既不在 lowering 闸里、也没有专属 opcode**，与 `time.sleep` 处境相同。

### 一条被实测否定的修法（重要，避免重走）

一个自然的修法猜想是：**让 `effects` 携带实例信息，使它与 `flags` 恒等**——如果 `time.sleep`
的 yieldable 性本该经 `effects` 传到验证层，那本节与第一节就是同一个修复。

**实测否定：`flags` 对这条调用也不带 `MAY_SUSPEND`。** 两条独立证据：

1. 前述临时探针的触发条件正是 `flags.MAY_SUSPEND != effects.MAY_SUSPEND`，跑这个用例**从未触发**
   → 两字段一致，都是 0。
2. 上面枚举的两个闸都不认 `time`（一个要求 `module == "net"`，另一个只认两个 handle 选择子）。

**所以第一节与本节是两个独立的缺陷，"让 effects 跟上 flags"修不了本节。**

### 影响面：45 条，10 个不同 selector

在 1 号的基线上按 `(码, 归一化消息)` 二元组聚类：

```
45 条 "coroutine state count disagrees with grounded call authority"
  27  selector=sleep
   5  selector=(空)
   4  selector=get
   3  selector=map
   1  selector=wait / call / run / runDirect / lock / push  各 1
```

用现成用例独立复现了两个非 `sleep` 的形态：

```
tests/diff/cases/semantics/concurrency/barrier_compose.xr   selector=wait expected=0 actual=1
tests/diff/cases/semantics/concurrency/once_compose.xr      selector=call expected=0 actual=1
```

**`get` / `map` / `push` / `lock` 不在协程分析那 14 个 roster 里**，说明产生协程状态的路径
不止 `xi_coro_is_*_call` 这一族——**影响面的边界尚未摸到底，在枚举完整之前不应确定修法。**

### 同族的 name-based 硬编码不止一处

`xi_coro_is_time_sleep_call` 之外，同文件还有两处同型：

- `xi_coro_is_test_yield_call`（`:323`）硬编码 3 个 `test_yield` 成员名
- `xi_coro_is_net_io_call`（`:338`）硬编码 4 个 net 成员名，且用的是**公开拼写**
  `accept/read/write/writeBytes`，而 metadata 侧是 `__accept/__read/...` **7 个私有拼写**

#### `xi_coro_is_net_io_call` 是一颗定时炸弹（6 号的迁移会触发）

stdlib 自举正在做的事，正是**把公开表面迁进 Xray、把 C leaf 改成 `__` 私有拼写**。
这个硬编码认的是迁移**之前**的公开拼写，所以：

> **6 号每迁一个涉及 net 的模块，那四个公开拼写就会失效，而协程分析会静默地不再认为
> 那些调用可挂起。**

它不报错——只是少建协程状态，然后在下游以本节这个诊断的**反面形态**冒出来：
`expected=1 actual=0`（而不是现在看到的 `expected=0 actual=1`）。

同类事故本轮已经发生过一次：5 号的新单测引用 `mem.cacheLineSize`，6 号把它改名为
`mem.__cacheLineSize`，**两个分支单独都绿，合并后单测查不到符号**。那一次有测试在喊；
`xi_coro_is_net_io_call` 这一处**没有测试钉住**，失效时不会有人喊。

### 完整枚举：协程状态有且仅有一个来源，而它有 7 个分支

收口判据是「每一条都能说出它的协程状态是哪一行代码建的」。**静态正推得到的答案是唯一的。**

实体的唯一产生点是 `xr_semantic_builder.c:6037-6041`，它逐条读 `XiCoroPlan::points`：

```c
const XiCoroPlan *coro = ctx->functions[function].source->coro_plan;
for (uint32_t state = 0; state < coro->nstates; state++) {
    const XiCoroSuspendPoint *point = &coro->points[state];
    ... append_entity(ctx, XR_SEM_ENTITY_COROUTINE_STATE, ...)
}
```

而 `points` 由 `xi_coro_analyze.c:2049-2145` 的两趟扫描填充（计数一趟、物化一趟），
**两趟用的是同一个判据，没有第二条路径**：

```c
if (xi_coro_is_suspend_point(f, v, resolver))   /* :2073 计数 */
    npoints++;
...
if (xi_coro_is_suspend_point(f, v, resolver)) { /* :2128 物化 */
    pt->state_id = pi + 1; pt->op = v; pt->kind = xi_coro_suspend_kind(f, v, resolver);
}
...
plan->nstates = npoints;                         /* :2145 */
```

所以**每一个协程状态都来自 `xi_coro_is_suspend_point_impl`（`:665`）判为真的一条 `XiValue`**，
而它有 **7 个互相独立的判定分支**：

| # | 分支 | 依据 | 验证层有无对应 |
|---|---|---|---|
| 1 | `xi_coro_is_net_io_call` | 硬编码 4 个 net **公开拼写** | **无** |
| 2 | `xi_coro_value_carries_suspend_contract` | **`v->flags & XI_FLAG_MAY_SUSPEND`** | 验证层问的是 `effects`，**不同的字** |
| 3 | opcode ∈ {YIELD, GEN_YIELD, GO, AWAIT, CHAN_SEND, CHAN_RECV, SELECT_BLOCK, SCOPE_EXIT} | opcode 直接匹配 | 部分（`effects` 表默认值 + `opcode == XI_GO`） |
| 4 | 7 个 blocking builtin 家族 | 14 个 (选择子, arity) 组合 | 部分（`BUILTIN_INSTANCE_YIELDABLE` target kind） |
| 5 | `xi_coro_is_time_sleep_call` | 硬编码 `time` + `sleep` | **无** |
| 6 | `xi_coro_is_test_yield_call` | 硬编码 3 个 `test_yield` 成员名 | **无** |
| 7 | `xi_coro_call_suspends` / `..._method_call_suspends` | **递归到被调方** | 部分（`DIRECT_LOCAL` 且 `suspendable[]`） |

对照语义验证层（`xr_semantic_verify.c:4956`）的 `expected`，它**只由两项析取而成**：

```c
operation_is_static_suspend(op)   /* effects 位 ‖ opcode == XI_GO */
|| dynamic_suspend                /* 只看 call target 的 kind */
```

**7 个分支 vs 2 个分支。** 这就是本节分歧的完整结构，也是那 10 个不同 selector 的来源：
`sleep` 走第 5 条、`wait` 走第 4 条、而 `get` / `map` / `push` / `lock` / `call` 这些
**不在任何 roster 里的选择子走的是第 7 条（递归）**——用户函数调用了一个挂起函数，
于是调用点自身成为挂起点，而验证层的 `dynamic_suspend` 只在 target kind 恰为
`DIRECT_LOCAL`/`SOURCE_INSTANCE_METHOD_LOCAL` 且被判 suspendable 时才跟得上。

**枚举到此闭合**：不存在「查不出协程状态由哪一行建」的条目，因为产生点唯一、判据唯一。
分歧的规模不是「45 条用例」，而是**判定分支数 7 : 2**。

### 实测交叉验证：两个判定集合是**交叉**，不是包含（推翻了本节先前的表述）

45 条逐条编译实测，**45/45 全部是本诊断，无一条因别的原因失败**。分布：

```
selector:  sleep 27 | (空) 5 | get 4 | map 3 | wait·call·run·runDirect·lock·push 各 1
opcode:    117 XI_CALL_METHOD 40 条 | 116 XI_CALL 5 条（空 selector ⟺ XI_CALL，一一对应）
(expected, actual):  0/1 共 44 条 | 1/0 共 1 条
```

**那一条 `expected=1 actual=0` 是本节最重要的实测结果**：

```
tests/diff/cases/semantics/coro/channel_for_in_recv_or.xr
  function=0 operation=53 opcode=116 selector=(空) expected=1 actual=0
  L14  print(drain_sum(ints))     ← drain_sum 体内是 for (msg in ch)，channel 迭代是挂起点
```

验证层判定它挂起，**而 Xi 没有建协程状态**——方向与其余 44 条相反。

**因此「Xi 的 7 条判定分支 ⊇ 验证层的 2 条」是错的**，本节先前按这个包含关系推导的部分应以此为准：
**两个判定集合是交叉的**。Xi 多认了一批（44 条），也漏认了一批（至少 1 条）。

这对修法方向有直接影响：**让 Xi 侧三条硬编码改读 metadata 只收拢了多认的那一侧**，
漏认的那一侧（经普通函数调用传递进来的挂起性）是**另一个缺口**，必须单独查。

其余实测事实：

- **12 个 http 用例全部指向同一处**：`stdlib/http/http.xr:1011` 的 `time.sleep(...)`。
  这与 fastpaths 构建期生成器死在 `http.xr` 是同一行。
- **survey 模式**（收集全部 gap 而非首报）下，45 条用例共 **75 处** gap，**全量仍只有 1 处反向**。
- 空 selector 的 5 条全是无接收者的 `f(args)` 语法（`XI_CALL` 没有方法名 metadata）。
  其中 2 条经 A/B 最小化确认：把 `defer { cleanup(...) }` 改成裸 `cleanup(...)`，
  gap 从 2 降为 1 但仍然报错——**触发因素是「经 upvalue cell 间接调嵌套函数」，
  `defer` 只是把同一处复制成正常路径与异常展开路径两份**。

### 修法方向（已裁决：本轮不做，留作下一轮开头的工作项）

枚举闭合后，问题的正确提法变了：**不是「哪一份判据是真值」，而是「为什么产生方有 7 种
识别方式，而消费方只有 2 种」。**

**关键论证——「验证层没有对应」不是疏忽，接手的人必须先读这一段：**

三条硬编码分支（第 1、5、6 条：net / `time.sleep` / `test_yield`）读的是**名字**：
模块名加成员名加实参数。验证层读的是 **metadata 与 call target kind**，那是**结构化的事实**。

**结构化事实里根本没有这些名字。** 所以「让验证层去追认这三条硬编码」在物理上做不到——
它没有那些名字可读，除非把名字表也复制一份进验证层，那只是把第 8 份判据造出来。

**因此唯一可行的方向是反过来：让 Xi 侧那三条硬编码改读 metadata。**
`vm_binding: "yieldable"` 已经在 `stdlib/defs/core.def` 里（`time.sleep` 见 `:58-68`），
`xr_stdlib_metadata_func_is_yieldable()` 已经存在于 `src/stdlib/xstdlib_metadata.h:182`。
改完之后 **7 条分支收敛成 4 条**，且与 stdlib 自举迁移同向——迁移正在把这些事实结构化，
硬编码是逆流。

**本轮不做的三条理由**（裁决记录）：

1. 它改协程状态数 → 改 SemanticPlan / TargetPlan 指纹 → 需与身份 KAT 的重算协调；
2. 集成分支已推进，在其上再引入一个改指纹的语义变更，风险与收益不成比例；
3. 它现在是一个**交接质量足够高**的工作项：产生点唯一、7 个分支逐条列出、每个 selector
   已归到分支、修法方向已论证。接手者第一天就能动手。

**接手时最容易走错的一步，就是去补验证层。** 上面那段论证说明了为什么那条路走不通。

### 与 1 号统计的对应关系

1 号的清单里 `XR_SEM_0019` 有 49 条，其中 **45 条**的消息是
`coroutine state count disagrees with grounded call authority`。本条最小复现产生的正是这条消息，
**但我没有逐条核对那 45 条是否都出自同一根因**，不作断言。

## 二、`representation_recipe`：同一张表两份，`OBJECT_REF` 一侧有一侧无

### 事实（逐字节比对，两份仅此一处不同）

```
生产方 src/aot/refine/xr_aot_refinement.c:367  representation_recipe
        case XR_MACHINE_REP_OBJECT_REF:     ← 有
        case XR_MACHINE_REP_RAW_PTR:
            return box ? BOX_REFERENCE : UNBOX_REFERENCE;

验证方 src/aot/refine/xr_aot_representation_refinement.c:8180  oracle_representation_recipe
        case XR_MACHINE_REP_RAW_PTR:        ← 只有这个，OBJECT_REF 落进 default → NONE
```

消费点使两者直接对撞：

```c
生产方 xr_aot_refinement.c:571   (!identity && recipe == NONE) → XR_AOT_REFINEMENT_REPRESENTATION
验证方 xr_aot_representation_refinement.c:11818
        (!identity_use && expected_recipe == NONE) → set_diag(XR_AOT_REFINEMENT_REPRESENTATION)
```

对 `OBJECT_REF`：生产方算出 `BOX_REFERENCE`（非 NONE）**放行并写出 evidence**；
验证方算出 `NONE`，**第一个条件即成立，无条件拒绝**——连 `record->recipe` 都不必比。

### 诚实的定性：**这是潜伏缺陷，不是活缺陷**

我穷举了 machine rep kind 的**全部产出点**：

```
xr_target_builder.c:748    → DYN_VALUE
xr_target_builder.c:785    → VIEW
xr_target_builder.c:11715  → AGGREGATE
xr_target_scalar_rep_shape.h → 17 种标量 kind
```

**`XR_MACHINE_REP_OBJECT_REF` 不在任何产出集合里**，当前从不被产出。

**因此它不解释 1 号 baseline 里的任何一条失败。** 需要特别说明：1 号统计的那 67 条是
`XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE`，与本条走的
`XR_AOT_REFINEMENT_REPRESENTATION` **是不同的诊断码**，baseline 里后者为 **0 条**。
我原以为二者可能闭环，实测**没有闭环**。

它的价值是定时装置：一旦有人让 `OBJECT_REF` 进入产出集合，两侧当场对撞，且表现为
「生产方写出的 evidence 被验证方无条件拒」这种最难定位的形态。

### 同一对里的第二处：只靠巧合成立的一致

```c
生产方 xr_aot_refinement.c:561   expected_input = identity ? machine->kind : ...
验证方 xr_aot_representation_refinement.c:11801
                                  expected_input = identity_use ? XR_MACHINE_REP_I64 : ...
```

IDENTITY 一档，一侧取 `machine->kind`，另一侧**硬编码 `XR_MACHINE_REP_I64`**。今天恰好都通过，
只因唯一的产出方 `authority_add_identity_obligation`（`:10274`）也硬编码 I64。
**一旦出现非 I64 的 identity obligation，两个检查里恰好会坏一个。** 建议进 `known_bugs.md`。

---

## 三、`XI_TRY.aux_int`：五层四个判定集合（与 B lane 根因同型）

`src/ir/xi.h:250-253` 定义了两个清理区拼写，生产侧值域是 `{1, 2, -1}`：

```
xi_lower_stmt.c:419   aux_int = XI_TRY_AUX_STATIC_CLEANUP           (=1)
xi_lower_stmt.c:3048  aux_int = cleanup_body_depth > 0 ? CLEANUP_LOCAL_HANDLER : -1   (=2 或 -1)
```

五个消费点，四个互不相同的判定集合：

| 层 | `aux_int = 1` | `aux_int = 2` | `aux_int = -1` |
|---|---|---|---|
| `xi_arc.c:1428` | 清理区 | **当作用户 try** | 用户 try |
| `xi_coro_lower.c:369` | 支持 | **不支持，直接拒** | 支持 |
| `xi_coro_exception_verify.c:47` | 合法 | **判为非法** | 合法 |
| `xi_emit_eh.c:168`（VM） | 发 flag 1 | **降级发 flag 0** | 发 flag 0 |
| `xi_cgen_coro.inc.c`（AOT） | 清理区 | **清理区**（另一组分支） | 普通 try |

`XI_TRY_AUX_CLEANUP_LOCAL_HANDLER`（=2）**在除 AOT CGen 外的每一层都被当成别的东西**。
这与 `xi.h:238-244` 那段注释描述的失败模式（B lane 已修的那个）是同一个形状换了一个 aux 字。

### 这是同一形状第二次出现，说明上一轮修的是实例不是类

B lane 修 `xi_local_addr_names_operand_storage` 时，处理的是 `XI_LOCAL_ADDR.aux_int`；
这里是 `XI_TRY.aux_int`。**同一个病换了一个 opcode 的 aux 字，就再长一次。**

我对「类」的判断：**根因不是某个判据写了多份，而是 `aux_int` 这个字段没有解释层。**
每个 opcode 各自约定 `aux_int` 的含义（`XI_LOCAL_ADDR` 用位标志、`XI_TRY` 用枚举值加哨兵
`-1`、`XI_CALL_METHOD` 用左移一位的方法符号），而**解释这个约定的代码散在每个消费点里**。
只要这个格局不变，任何新的 aux 约定都会以同样的方式再分裂一次——抽出第 N 个共享判据
只是修第 N 个实例。

因此本条的正确产出**可能不是再抽一个 `xi_try_is_generated_cleanup`**，而是先回答：
`aux_int` 的每一种约定应该在哪里被唯一地解释一次。这超出本 lane 的授权，交出。

**裁决请求**：统一判据 `xi_try_is_generated_cleanup(aux) = (aux == 1 || aux == 2)` 会让 `2`
在四层从「非清理」变「清理」，是行为变更。但五层当前互相矛盾本身就是缺陷，不是可保留的设计。

---

## 四、`XaBuiltinReceiverKind`：八份，七份漏掉 `MAP` 分支

`xa_builtin_receiver_matches_type`（`src/frontend/analyzer/xbuiltin_receiver_registry.h:322`）
是唯一完整的一份。另外七份（`xanalyzer_allocation.c:279`、`xanalyzer_memory_effect.c:259`、
`xglobal_producer.c:3146`、`xi_opt.c:2331`、`xi_own.c:286`、`xaot_prepare.c:570`、
`xi_cgen.c:385`）的 switch **没有 `XA_BUILTIN_RECEIVER_MAP` 分支**，直接落到函数尾 `return false`。

**这不是死枚举值**：`xbuiltin_receiver_method.def:213` 的 `MAP_ENTRIES_ITERATOR`
（`"entriesIterator"`）就用它。后果是一个 `Map.entriesIterator()` 调用在 analyzer 眼里是内建方法，
在**全局效应摘要、内存效应分析、Xi opt、Xi 所有权、AOT prepare、CGen 六层眼里都不是**。

旁证：`xi_lower_stmt.c:2412` 直接调用了注册表那一份，所以 lowering 那条路是对的——
**只有直接复用注册表的调用点才正确**。

第二处差异：`xanalyzer_memory_effect.c:259` 的 `POD_SLICE` 分支**完全不看元素类型**，
接受任意 `Slice<T>`，比其余七份宽松。

---

## 本 lane 已落地的等价抽取（对照组，证明方法有效）

以下五组经逐字节核对确认判定集合相同，已合并，**不在本 packet 的裁决范围**：

1. `mark_coroutine_functions`（`xr_target_builder.c` / `xr_target_verify.c` 逐字重复）
2. `machine_reps_have_same_call_abi`（两份，相同 11 字段、顺序不同，全为无副作用比较）
3. `tagged_container_value_boundary`（两份，连注释都近乎逐字）
4. `*_type_is_unsigned_int`（**五份**逐字节相同，而共享版 `xr_type_is_exact_unsigned_integer`
   早已存在于 `xtype.h:492`；其中三个文件**同时**调用共享版又留着自己的拷贝）
5. `xrt_value_is_enum_descriptor` / `xrt_value_is_span_ref`（AOT runtime 两组手抄判据）

**有意保留未合并的**：`xrt_core_freestanding.h:1271` 的 span 判据是 freestanding profile 的
独立副本（该文件不 include `xrt_coll.h`），属于**有意的 profile 隔离**，不是漏网的第三份。

## 五、无静态目标的 closure：两处各自论证过，论证的不是同一件事

3 号在全局效应摘要线上撞到、我核实的。**这一组的形态与前四组都不同，也最隐蔽。**

同一个调用点（`XG_CALL_CLOSURE` 且 `static_target_func_id == XG_NO_ID`），两个判据给出相反答案：

```c
src/analysis/xglobal_summary.c:2850   xg_body_reachability_mark_call
    if (call->kind == XG_CALL_CLOSURE)
        /* Closure and builtin calls carry their local effect/capability
         * contract on the owner body.  Concrete direct function targets are
         * recorded above; no declaration-tree fallback is permitted here. */
        return true;

src/analysis/xglobal_summary.c:2727   xg_callsite_effects_compose
    /* A closure without a stable target is deliberately unprovable. */
    return false;
```

两个函数的前两个分支**完全对称**（`NATIVE/EXTERN/CLASS_ALLOC` 一组，
`DIRECT_FUNC` 或带静态目标的 `CLOSURE` 一组）。分歧只在第三处：
**A 为无静态目标的 closure 多写了一个兜底分支，B 没有**——`XG_CALL_CLOSURE` 在 B 里
只出现一次（带静态目标的那次），其余落到函数尾的 `return false`。

### 为什么这一组比前四组更难发现

前四组都是**漂移**：某一份忘了跟上，没人为差异辩护过。这一组不是——
**两处各自写了注释论证自己为什么对，而且两段论证单独看都成立**：

- A 的论证成立：closure 的效应/能力契约确实记在 owner body 上，可达性标记到此为止是对的。
- B 的论证也成立：没有稳定目标就无法组合被调方的效应集合，判为不可证是保守且正确的。

**它们回答的根本不是同一个问题**——A 问「这个调用点处理完了吗」，B 问「能否算出它的效应集合」。
把两段注释并排读才看得出来，而它们相隔 160 行、在两个函数里。

**这是本 packet 里唯一一组「注释齐全、每一处都经过思考」的分歧。** 前四组的教训是
「重复会漂移」，这一组的教训不同：**判据分歧不一定源于疏忽，也可能源于两处各自想清楚了一个
略微不同的问题，而没有人把两个问题并排放在一起看过。** 加共享判据解决不了这一类——
需要先确定「无静态目标的 closure 在可达性与效应组合两个维度上分别意味着什么」。

### 3 号报告的后果（我未独立复现，如实标注）

据 3 号：print 变成普通调用之后，**几乎所有程序的 entry plan 都退化成全零**
（`root_representation` 停在 `ELIDED`，连 `assert(true)` 也中招），而只有
`test_xglobal_summary` 里一个用例在喊。他已写进 `blockers/3-kat-surface-recompute-residue.md` 第 4 节。

**这一条的后果我没有独立验证**，只核实了两处判据本身的分歧确实存在、注释确实如上。

---

## 附：显式排除的靶子（重复但**不该**合并）

判据去重最大的自伤风险是拆掉设计上的独立性。以下两处满足「逐位不变」的验收标准，
**但仍然不合并**，理由记录在此，避免下一个人重新发现并动手：

**1、`semantic_operation_is_call_shaped`（`xr_target_builder.c:8914`）与
`operation_is_call_shaped`（`xr_target_verify.c:5053`）** —— 正规化后逐字重复。
不合并的理由是同文件 `xr_target_verify.c:1105-1112` 明写的设计意图：

> *"The raw-pointer test stays local and derives its answer by re-parsing the frozen canonical
> key, which is an independent route to the same fact and the reason this verifier catches a
> record whose fields and key disagree."*

验证侧对 builder 的独立重算**是一道防线，不是漏抽的重复**。验证侧另有约 71 个
`*_is_exact` 同源副本，无条件合并会系统性地拆掉 verifier 的独立性。

**2、`xrt_core_freestanding.h:1271` 的 span 载体判据** —— 与 `xrt_coll.h` 那份逐字相同，
但该文件**不 include `xrt_coll.h`**，它是 freestanding profile 的独立副本。
这是 **profile 边界**，不是漂移。

### 由此得出的一条通则（建议纳入本 lane 的边界）

> **验证侧对 builder 的独立重算，默认推定为有意，除非注释或证据表明相反。**

判断一处重复该不该合，看的不是两份代码是否相同，而是**「两处各算一遍」本身是否在提供价值**。
`mark_coroutine_functions` 那一对该合（差异纯粹是诊断措辞），`operation_is_call_shaped`
那一对不该合（差异是独立取证路径）——两者在源码上都是「逐字重复」。


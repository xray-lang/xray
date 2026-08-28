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

`effects` 只带 opcode 的表默认值，**丢弃了 lowering 对这一条指令的判断**；`flags` 带的是实例值。
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

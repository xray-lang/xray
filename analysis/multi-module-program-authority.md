# 多模块 program authority：拓扑权威与可执行权威的分离

- **Lane**: 4（第三轮十路并行）
- **起点**: `xray@00f665c5c`，tree `afe293b71`
- **分支**: `work/4-multi-module-authority-00f665c5c`
- **状态**: 第一层已打通并实测；第四层（TargetPlan 模块分区）是剩余缺口

---

## 1. 结论摘要

`XR_TARGET_1000: product TargetPlan requires one canonical program authority`
不是「AOT 不支持多模块」，而是**一个指针身兼二职**造成的连带拒绝。

改动只有一个文件（`src/aot/xaot_driver.c`，+26/-1），把这两个职责拆成两个独立判据后，
树里每一个多模块用例都从第一层推进到了第四层，**单模块编译不受影响**。

剩余缺口是 TargetPlan 的 N 模块分区能力，位于 `src/plan/target/xr_target_builder.c`
与 `xr_target_verify.c`——本 lane 的禁止文件（与 5 号冲突），需要裁决后才能动。

---

## 2. 问题：`source_program_closure` 身兼二职

改动前，`src/aot/xaot_driver.c` 用**同一个指针**回答两个不同的问题：

| 职责 | 问题 | 对谁成立 |
|---|---|---|
| (a) 拓扑权威 | 这个程序的模块集是不是规范的？（无环、单可达根、每个模块到达 analyzed 生命周期、每条依赖边有一个确切的已解析 import） | **任何**合法程序 |
| (b) 可执行切片权威 | 这些函数能不能被直接调用？（entry 调用一个导出的 i64 函数，或调用一个生成的私有 native leaf） | 只有两个极窄家族 |

守卫写的是：

```c
if (!source_program_closure && nmodules != 1) {
    aot_bundle.error_msg =
        "XR_TARGET_1000: product TargetPlan requires one canonical program authority";
```

`source_program_closure` 只由两个发布器产生，两者都在入口硬拒
`graph->spec_count != 2`（`xa_program_semantic_closure.c:1118` 与 `:1733`），
形状还要求 entry 恰好 2 条顶层语句、依赖模块恰好 1 条、import 恰好 1 个 member 且无 alias。

于是职责 (a) 被职责 (b) 拖累：**所有多模块程序一起被拒**，包括 3 模块的纯 i64 链、
`import base64` + `main` 这种 2 模块程序、以及 stdlib fastpaths 生成器的 22 节点图。

这个族的定义域有多窄，值得写准：`publish_scalar_module_graph` 的接受条件展开是 **24 条 AND**，
除图形状外还包括——入口函数体内恰好 1 个 `AST_CALL_EXPR`、依赖函数体内一个调用都不能有、
callee 必须是裸 `AST_VARIABLE`（不能是成员访问）、入口返回必须精确 i64、
依赖导出函数 1 参且参数与返回都精确 i64、三个 stable id 必须完全相等。
**这个族的实际大小接近 1**：任何含 `print`、含第二个调用、含 class/enum、
含非 i64 类型的程序都进不来。让一个定义域接近空集的族去充当
「整个程序的模块集是否规范」的判据，这就是缺陷本身。

`nmodules != 1` 这个豁免本身就是同一个设计缺陷的另一面：单模块之所以能过，
不是因为它有权威，而是因为它被特例放行了。

## 3. 已经写好但从未接线的那条路

`xa_program_semantic_closure_publish_source_module_graph`
（`src/frontend/analyzer/xa_program_semantic_closure.c:2005`）是一个**通用 N 模块发布器**：

- 接受任意 `spec_count <= XR_PROGRAM_SEMANTIC_CLOSURE_MAX_MODULES`（256）的无环图
- 要求恰好一个入度为 0 的根，且该根等于 `graph->entry_index`
- 要求每个模块 `status == XR_MODSPEC_ANALYZED`、AST 是 `AST_PROGRAM`、依赖下标无重复无自环
- 为每条依赖边找出一个能 resolve 到该依赖的 import/export 语句，取最靠前的那个作为 locator

`XR_PROGRAM_SEMANTIC_FAMILY_SOURCE_MODULE_GRAPH` 家族在 PSC verifier 里**早就是完整支持的**：

- `xr_program_semantic_closure_verify.c:971` 是全表唯一允许 `function_count == 0` 的家族
- `:2183` 的家族基数表对它**不设模块数上限**（其余可执行家族全部写死 `!= 2` / `!= 1`）
- `:1544` 反过来禁止它携带函数根
- `:2206` 对它跳过 call graph 校验

头文件的注释把意图写得很清楚：

> This publication carries modules and exact resolved source edges only;
> downstream executable authority must be added by a later slice.

**它在 `src/` 下零调用者**——只有 `tests/unit/frontend/test_xa_program_semantic_closure.c:4123,4171`
用它跑过一个 3 模块的 csv 图。也就是说：能力早就造好了，只是没有接到驱动上。

## 4. 改动：两个独立判据

```c
XrProgramSemanticClosure *program_graph_authority = NULL;  /* 拓扑权威 */
XrProgramSemanticClosure *source_program_closure = NULL;   /* 可执行切片权威 */
```

- **拓扑权威**对**每个模块数**发布，包括单模块。发布失败就是真的失败。
- **可执行切片权威**保持原样：仍只由两个窄家族产生，仍只喂 `cfg.program_semantic_closure`。
  它的缺席**不再是拒绝**——拓扑权威已经回答了「这个程序是谁」。
- 守卫从 `!source_program_closure && nmodules != 1` 变成 `!program_graph_authority`。
  **单模块的特例一并消失了**：现在单模块与多模块走同一条判据。

### 4.1 为什么拓扑权威不进入 Xi / SemanticPlan / TargetPlan 通道

这是这个改动只需要动一个文件的原因。如果把 `SOURCE_MODULE_GRAPH` 家族塞进
`cfg.program_semantic_closure`，会连撞五处家族白名单：

| 位置 | 拒绝消息 |
|---|---|
| `src/ir/xi_pipeline.c:1175` | `borrowed graph PSC is not exact` |
| `src/ir/xi_program_semantic.c:419` | `Xi input does not bind its PSC partition` |
| `src/ir/xi_program_semantic_verify.c:790` | `Xi program semantic family is unsupported` |
| `src/ir/xi_program_semantic_plan.c:1364` | `graph SemanticPlan module set is incomplete` |
| `src/plan/semantic/xr_semantic_builder.c:202` | `SemanticPlan construction family is inconsistent` |

这五处白名单是**对的**：它们守的是「可执行」这件事，而拓扑-only 家族按定义不主张可执行。
把它塞进去等于让一个不主张可执行的权威去冒充可执行权威。

正确的做法是让它待在它该在的层：**它是驱动层对模块集规范性的判据，不是 Xi 的输入。**
`prepare_program_authority`（`xr_semantic_builder.c:180`）在 closure 为 NULL 时正常返回 true，
所以多模块程序的每个 SemanticPlan 照常构建，只是不带 program provenance。

## 5. 逐层判别实测

同一批用例，`build --native -c`，改动前后：

| 用例 | 模块数 | 改动前 | 改动后 |
|---|---|---|---|
| 单模块 `fn main()` | 1 | ✅ 通过 | ✅ **仍通过**（无回归） |
| `import base64` + main | 2 | XR_TARGET_1000 program authority | `program TargetPlan is missing, corrupt, or has the wrong module set` |
| 3 模块链 a→b→c（纯 i64） | 3 | 同上 | 同上 |
| 3 模块、2 个 import | 3 | 同上 | `XR_TARGET_1005: dynamic entry expectation table is not exact` |
| 2 模块、string 返回 | 2 | 同上 | `XR_TARGET_1003: source-export call authority is incomplete` |
| `tests/diff/.../multimod_calls.xr` | 2 | 同上 | `XR_TARGET_1003: source-export call authority is incomplete` |

每一条都**换了一条更靠后、更具体的拒绝消息**。这正是六层判据的逐层推进：
第一层（source fact → PSC → program authority）通了，现在卡在第四层（TargetPlan）。

## 5.1 第二层实测：TargetPlan 模块分区（L2）

拆开「分区」与「图边」两个事实、并为 N 个模块产出分区后，同一批用例再推进一层：

| 用例 | L1 后首拒 | L2 后首拒 |
|---|---|---|
| 单模块 | ✅ 通过（3908 字节 C） | ✅ **仍通过（3908 字节，逐字节相同）** |
| `import base64` + main | `program TargetPlan is missing, corrupt, or has the wrong module set` | `XR_TARGET_1003: direct-local signature or result storage is incomplete opcode=116 operand-count=3 parameter-count=2 result-type-match=1 storage-mask=0 method=0` |
| 3 模块链（纯 i64） | 同上 | `XR_TARGET_1001: target functions do not cover semantic value ownership` |
| 3 模块、2 个 import | `XR_TARGET_1005: dynamic entry expectation table is not exact` | 同上 |
| 2 模块、string 返回 | `XR_TARGET_1003: source-export call authority is incomplete` | 不变 |
| `multimod_calls.xr` | `XR_TARGET_1003: source-export call authority is incomplete` | 不变 |

`stdlib2` 的新拒绝值得单独一说：它现在是在**为 `base64.xr` 自己**构建 target plan 时失败的
（`opcode=116`），也就是说多模块的**架构**墙已经推倒，暴露出来的是各模块自身的 AOT 能力缺口。
这与 `blockers/a-stdlib-multi-module-program-authority.md` 记录的
「33 个 stdlib 探针里 AOT 只有 `time` 能生成 C」是同一件事。

## 6. 六层判据的完整地图

| 层 | 多模块能力 | 位置 |
|---|---|---|
| source fact（模块图） | ✅ 无上限，BFS + Tarjan | `src/module/xmodule_graph.c` |
| PSC 容器 | ✅ 256 模块，七张表全动态数组 | `xr_program_semantic_closure.h:29` |
| PSC verifier（`SOURCE_MODULE_GRAPH` 家族） | ✅ 不设模块数上限 | `xr_program_semantic_closure_verify.c:2183` |
| **PSC 发布（analyzer）** | **✅ 本轮接线** | `xa_program_semantic_closure.c:2005` |
| **AOT 驱动守卫** | **✅ 本轮打通** | `xaot_driver.c:2573` |
| SemanticPlan builder | ✅ 依赖表动态，上限 100000 | `xr_semantic_builder.c:2931` |
| SemanticPlan module-set verify | ✅ 逐条循环，无固定下标 | `xr_semantic_verify.c:5892` |
| `xaot_resolve_semantic_dependencies` | ✅ 干净的 N 循环 | `xaot_driver.c:1678` |
| `xr_target_plan_build_module_set` | ⚠️ 依赖数上限 4096，但**不产出模块分区** | `xr_target_builder.c:16620` |
| **TargetPlan 模块分区** | ❌ **只有 `build_program_graph` 产出，硬编码 2** | `xr_target_builder.c:16561, 16600` |
| **TargetPlan verify** | ❌ **非 graph plan 必须零分区** | `xr_target_verify.c:10661` |
| `XrTargetProgramGraphRecord` | ❌ 单 entry / 单 producer / 单 call / 单 argument 的扁平记录 | `xr_target_plan.h:928` |
| AOT bundle 安装 | ❌ 要求 `program_module_count == nmodules` | `xaot_bundle.c:681` |
| C emission | ❌ 每个模块都要能映射到一个分区 | `xaot_driver.c:1993` |

### 6.1 剩余缺口的确切形状

AOT 的多模块 C emission 设计**已经是**「一个 program TargetPlan + N 个模块分区」：
`xaot_module_emission_plans_build`（`xaot_driver.c:1991`）对每个模块都用同一个
program TargetPlan，但要求 `xaot_bundle_program_semantic_for_module` 能把它映射到一个分区。

而 `xr_target_plan_build_module_set` 产出的 plan 的 `semantic_module_count` 是 **0**
（`builder_freeze` 的 draft 根本没赋这几个字段），
`xr_target_plan_program_module_count` 于是返回 1，与 `nmodules` 不等。

更硬的是 `xr_target_verify.c:10661`：

```c
if (plan->program_graphs_count || plan->module_partitions_count ||
    plan->semantic_module_count || plan->semantic_modules)
    return report(error, error_size, "XR_TARGET_1001",
                  "non-graph TargetPlan carries program graph authority");
```

**非 graph plan 被明确禁止携带模块分区。** 所以这不是「在 `build_module_set` 里补两行赋值」，
而是要决定：TargetPlan 的「程序图」形态是否从 2 模块的专用记录推广为 N 模块的通用形态。

另外两条第四层拒绝各自是独立的能力缺口，不是同一堵墙：

- `XR_TARGET_1003: source-export call authority is incomplete`（`xr_target_builder.c:10729`）
  ——跨模块调用的结果类型必须过 `call_type_is_exact_scalar`。返回 `string` 的跨模块调用在这里被拒。
- `XR_TARGET_1005: dynamic entry expectation table is not exact`（`xr_target_verify.c:9082`）
  ——多个 import 的入口期望表。

## 7. 架构决策：VM 单文件入口与 AOT 图编译**不收敛为一条路径**

### 7.1 决策

**不收敛。** 但把这条分叉从「实现细节」改写成**显式的、有名字的设计事实**：
VM 执行路径是 **module-local 编译**，AOT 与字节码打包路径是 **module-set 编译**。

### 7.2 依据

**(0) 先纠正一个被广泛转述的错误归因。** 「`xray run` 从不建模块图」这句话**不成立**：
`src/app/cli/xcmd_run.c:429-563` 整段就是 "Pre-flight: build module graph"，
单文件也建（提交 `2dcdfaa85` 标题即「Publish the module graph for a single-file run too」），
`run -`（stdin）也建。`build --native` 也**不**走 `bundle_compile_graph`——
那是不带 `--native` 的字节码打包路径，`--native` 自己在 `xaot_driver.c:2130` 建图。
两条路早就共用同一个 `xr_module_graph_new/_build/_topological_sort`。

**真实的分叉不在建不建图，在图之后，而且是两条通道**：

- **通道一（analyzer 通道）**：`xa_analyzer_set_graph`。`xray run` 通过 session 拿到图
  （`xvm_compile.c:265` 从 `xr_compiler_session_module_graph(session)` 取回），
  所以 run 的每次模块编译都持有完整图，跨模块**类型**是通的。
- **通道二（`XiModule` 表通道）**：`XiPipelineConfig.graph_modules`。只有 AOT
  （`xaot_driver.c:2539`）与字节码打包（`xbundle.c:177`）传，
  `xray run` / `xray test` / DAP 传 `NULL, 0`（`xvm_compile.c:199-203`）。

**同一份「模块图」事实有两条互不相通的传递通道，这本身就是一处应当合并的表述重复。**
能力差全部落在通道二：没有 `XiModule` 表就没有 `xi_resolve_imports`、没有跨模块 ARC 借用、
没有跨模块协程可挂起性、SemanticPlan 依赖数恒为 0。

**(1) 完全收敛在语义上不可能。** 存在三条模块名在编译期不可知的运行时 import：

- `OP_IMPORT`（`src/vm/xvm_dispatch_module.inc.c:40`），模块名来自常量池字符串。
  发射条件是图内解析失败 + emit 期解析也失败（`src/ir/xi_emit_object.c:1389`），
  注释明写「stdlib/native modules not in the graph, or when module_table is unavailable (REPL)」。
- `src/stdlib/xstdlib_vm_fastpath.c:188` 与 `:252`——AOT 生成的原生代码在**运行时**按
  `nominal_owner` / `module_name` 字符串 import。

任何收敛都只能覆盖「图内 source-backed 模块」，图外 import 必须保留 `xr_module_import` 惰性入口。
所以「一条路径」是个伪目标；真实的目标是**让图内模块在两条路径上有相同的模块集语义**。

**(2) 入口其实是三个，不是两个。** 第三个是构建期生成器
`xray_stdlib_bcgen native-fastpaths`（`src/app/tools/xstdlib_bcgen.c:126`），
它绕过 CLI 直接调 `xaot_build`，硬编码 `HOSTED` + `XAOT_ARTIFACT_HOSTED_FRAGMENT`。
它撞的是**同一个** `xaot_driver.c` guard，所以本轮改驱动一处就同时解开它。

而通道二那一侧内部也已经不一致：`xray build`（字节码打包）**传** `graph_modules`，
`xray test`（`xcmd_test.c:431`）、DAP（`xisolate_scripting.c:391`）与 `xray run` 一样**不传**。
只改 `cmd_run` 会造成**同一份源码在 `run` 和 `test` 下产生不同的 SemanticPlan 指纹**，
这是引入一条新分叉，比现有分叉更糟。

**(3) 收敛此刻的收益为零。** 多模块 AOT 的打通完全不依赖它——第 5 节的实测是在
只改 `xaot_driver.c` 的前提下取得的。

**(4) 代价与风险是实的**，且其中两条会制造新的语义分歧：

- stdlib 模块走嵌入字节码时**根本没有 `XiModule`**（`xmodule.c:920`；
  `xchunk.h:292` 明确字节码不携带编译期 `xi_func`），`graph_modules` 必然有 NULL 洞。
  而 `append_dependency`（`xr_semantic_builder.c:2933`）对 NULL 直接返回 false
  ——**跨 stdlib 的 import 不会成为 semantic dependency**，与 AOT 不一致。
  这会新开一个 VM/AOT 语义分歧面（参见 `blockers/a-stdlib-vm-aot-leaf-semantic-divergence.md`）。
- `xi_resolve_imports` 首次在 VM 路径运行后，import 的解析口径从
  「`registry->module_table` 的 realpath 字符串匹配」换成「图 + `XiModule` 导出表」，
  **发射的字节码可能改变**（哪些 import 落到 `OP_LOAD_MODULE_SLOT` vs `OP_IMPORT`）。
- SemanticPlan 指纹直接哈希 `dependency_count`（`xr_semantic_plan.c:225`），
  任何带 import 的模块指纹都会变，波及 `test_xi_cgen` / `test_xtp_format` /
  `test_target_plan` / `test_xi_pipeline` / `test_xi_program_semantic` 的 KAT。
- ARC 语义变化（`xi_pipeline.c:786`：依赖解析成功后保留 caller 侧所有权，
  否则退回「移动参数」约定）、跨模块协程可挂起性判定变化。

### 7.3 这个决策留下的债，以及它为什么可以带着走

留下的债是：**`xray run` 的 SemanticPlan 依赖数恒为 0，而 `xray build` 与 AOT 不是。**
这个债可以带着走，因为它现在有一个名字和一条判据：VM 是 module-local 编译。
它不再是「有人忘了传参数」，而是「VM 声明自己不做跨模块 plan 聚合」。

真要收敛时，正确的方向是 **VM 向 `xray build` 靠**（后者已经传 `graph_modules`），
并且必须**同时**改 `run` / `test` / DAP 三个入口，否则只是把分叉挪个地方。
最小改动清单（10 处）与风险清单（9 类）已经取证完毕，记录在本轮 lane 4 的调查里。

---

## 8. 剩余工作与边界

| 项 | 位置 | 是否在 lane 4 授权范围 |
|---|---|---|
| 拓扑权威接线 | `src/aot/xaot_driver.c` | ✅ 已完成 |
| TargetPlan N 模块分区 | `src/plan/target/xr_target_builder.c` | ❌ **禁止文件**（与 5 号冲突），需裁决 |
| 非 graph plan 允许携带分区 | `src/plan/target/xr_target_verify.c:10661` | ❌ 需裁决 |
| 跨模块非标量返回 | `src/plan/target/xr_target_builder.c:10729` | ❌ 需裁决 |
| bundle 安装判据 | `src/aot/xaot_bundle.c:677` | ✅ 在范围内，但**放松它是不正确的**——见 8.1 |

### 8.1 一条不要重试的路：放松 `xaot_bundle` 的模块数判据

`src/aot/xaot_bundle.c:677` 的
`xr_target_plan_program_module_count(target_plan) != bundle->nmodules` 在 lane 4 的授权范围内，
放松它能让判别用例立刻"前进"。**不要这么做。** 记录代价，避免后来者再撞一次：

`xaot_bundle_program_partition_for_module`（`:497`）在 `partition_count == 0` 时会
兜底成 `partition_count = 1`，于是**所有** Xi 模块都去试 partition 0。
后果不是"少一个分区"，而是**模块本地的 semantic index 串到入口模块的行上**——
`xr_target_plan_value_rep_for_module`（`xr_target_plan.c:1444`）在
`module_partitions_count == 0` 且 `partition == 0` 时会在**全表**上二分查找，
而 `semantic_value` 是模块本地索引。

下游有 8 处依赖这个映射拿每个函数的 target value binding：
`xaot_bundle.c:5479`（rep adapter 精确性）、`:7578`、`:7644`、`:7727`、`:7788`、`:8334`、
`src/aot/xi_cgen.c:3035`（跨模块直接调用的 target semantic 校验）、
`src/aot/xaot_prepare.c:5294`。

放松判据换来的不是通过，是**把一个能被诊断的拒绝换成一个不能被诊断的错行绑定**。
正确的做法是让 plan 真的有 N 个分区，而不是让判据闭嘴。

### 8.2 L3 的架构诊断：verifier 的单 SemanticPlan 假设

这是本轮同一个病灶的**第三次**出现，形态一次比一次深：

| 层 | 形态 |
|---|---|
| L1 | 一个 authority 指针**身兼二职**，定义域几乎为空的窄职责拖累了普遍职责 |
| L2 | 一句 verify 判据**绑了两个事实**（携带图边 vs 覆盖多模块） |
| **L3** | **整条 verifier 通路建立在「一个 TargetPlan 对应一个 SemanticPlan」这个隐含假设上**，而 graph 族靠**另开一条为窄族定制的平行通路**绕过去 |

L3 最严重，因为它不是某一处写错，而是**一个结构性假设被几十个函数共享，
而第一个打破它的族选择了绕开而不是修正**。

证据：`verify_value_reps`（`xr_target_verify.c:4531`）第一句是

```c
if (plan->functions_count != xr_semantic_plan_function_count(plan->semantic_plan))
    return report(error, error_size, "XR_TARGET_1001",
                  "target functions do not cover semantic value ownership");
```

——用 **entry 一个 SemanticPlan 解释整张合并表**，后面还有几十个同形状的 `verify_*`。
graph 族之所以能过，是因为 `verify_program_graph_plan`（`:10376-10644`）是
**完全独立的一条通路**，自己逐分区校验；而它的逐分区版
`verify_program_graph_rows`（`:9863-10140`）是为第 2 节说的那 24 条 AND 的窄族定制的
——要求 extent 全是 `XR_TARGET_EXTENT_FIXED`、layout 数等于唯一类型数、
value_rep 逐个精确匹配——**对含 string / class / 协程的一般模块根本不成立，不能泛化。**

绕开的代价现在显形：那条平行通路无法承担通用职责，
于是打通多模块的人必须先把 verifier 从「单 SemanticPlan 解释全表」重构为「分区感知」。
**这不是下一个错误码，是一个有名字的工程。**

### 8.3 第二条不要走的捷径：在分区模式下绕过逐行校验

在分区模式下跳过 `verify_value_reps` 那批校验，能让判别用例立刻"前进"。**不要这么做。**

可以论证：合并是纯粹的行平移 + rep 重映射，若每个模块的行独立验证过，
且分区区间无缝、无重叠、全覆盖（`graph_partition_ranges_are_exact` 正是这个判据），
那么合并结果正确。这个论证可能是对的——

**但那是作者的论证，不是验证器的判据。** 在一个 fail-closed 的系统里，
绕过验证器等于把「作者相信它正确」冒充成「验证器确认了它正确」。
这条线一旦越过，之后每一次拒绝都不再能说明任何事。与 8.1 是同一类陷阱的两个入口。

### 8.4 一个已知限制：分区 plan 不进 XTP 缓存

`xr_cache_xtp_key`（`src/incremental/xr_cache_artifact_verify.c:104`）用
SemanticPlan 的 program provenance 作为程序身份，而分区集按定义不携带 provenance。
因此 `xr_program_target_plan_build` 对分区模式**禁用 XTP 缓存**（每次重建），
而不是用一个更弱的身份去 key 一个持久化制品。这是正确性优先的取舍，
代价是多模块 AOT 目前没有增量。要恢复增量，需要给 cache context 一条
以 `xr_target_semantic_module_partition_set_fingerprint` 为键的并行通路。

### 8.5 剩余缺口的可行路径（已取证）

`build_module_set` **没有行可分**：它的 draft 只 materialize entry 一个模块的表
（`builder_materialize`，`xr_target_builder.c:15595`），依赖只以索引形式出现在 call 行里
（`call->source_dependency` / `source_export` / `source_callee_identity`），
**被调方的 target function 行根本不在这个 plan 里**。所以"为 N 个模块产出 N 个分区"
不是补一段循环，而是「为 N 个模块各跑一遍 builder，再 merge 成一个 global plan 并记录分区偏移」
——那正是 `xr_target_plan_build_program_graph` 在做的事，只是它写死了 2。

好消息是这条路上大部分基础设施**已经是 per-module 泛型的**：

- `graph_merge_module`（`xr_target_builder.c:16005`）逐模块平移行 + 重映射 machine rep id
- `graph_partition_ranges_are_exact`（`xr_target_verify.c:9252`）逐表校验分区区间无缝无重叠全覆盖
- `xr_target_semantic_program_module_set_verify` / `xr_target_semantic_module_set_fingerprint`
  （`xr_target_plan.c:620` / `:663`）只受 256 上限
- XTP 序列化（`xr_xtp_materialize.c:97, 426`）已按 `semantic_module_count` 泛化

写死 2 的只有三处：`graph_merge_machine_reps`（`:15945`，签名带 `uint16_t rep_maps[2][256]`）、
`graph_merge_capabilities`（`:15973`）、以及 `build_program_graph` 自身的
`XrProgramGraphModuleDraft modules[2]` 与 `.module_partitions_count = 2u`。

**真正的设计问题不在这些数字，在 `xr_target_verify.c:10661`**：

```c
if (plan->program_graphs_count || plan->module_partitions_count ||
    plan->semantic_module_count || plan->semantic_modules)
    return report(error, error_size, "XR_TARGET_1001",
                  "non-graph TargetPlan carries program graph authority");
```

它把**两个不同的事实绑成了一处表述**：
「这个 plan 覆盖哪几个模块」（分区）与「这个 plan 里有一条已证明的跨模块直接调用」（图边）。
`XrTargetProgramGraphRecord`（`xr_target_plan.h:927`）是**一条边的完整证据**
（`entry_partition / producer_partition / entry_target_function / producer_target_function /
target_call / target_argument / caller_slot / callee_slot / ...`），
而 `verify_resource_budgets`（`xr_target_verify.c:704`）又限死 `program_graphs_count <= 1`。

**分区是模块集事实，图边是调用事实。** 把它们拆开，「N 个分区、0 条图边」就是一个合法形态，
这正是拓扑权威在 TargetPlan 层的对应物——与第 2 节拆开 authority 指针是**同一个病灶的第二层表现**：
一个为极窄族设计的记录被当成了通用形态的载体。

**默认配置（不加 `-DXRAY_STDLIB_VM_FASTPATHS=OFF`）能完成构建**这个验收标准，
要求 fastpaths 生成器的模块图走完整条 AOT 链路到 C emission。
这张图的规模需要说准：生成的 `main.xr` 约 1439 行、277 个 `export fn` 包装器、
25 条 import 语句涉及 **14 个直接 import 的模块**，
但沿 `stdlib/*/*.xr` 做**传递闭包是 22 个节点**（多出
`crypto, http2, io, mem, net, os, path, time`）。
「14 模块」只对直接 import 面成立，编译器实际要撑住的是 22 节点图。因此该验收
因此**必然**依赖上表中的 TargetPlan 改动。第一层打通是必要条件，不是充分条件。

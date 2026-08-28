# Blocker: `XR_AOT_REFINEMENT_*` 占了 16% 的差分拒绝面，却整族不在诊断治理之内；它背后的能力缺口不是一个，是 55 个 oracle 分支

- **Lane**: 1 (差分网解封)
- **Status**: `BLOCKED`
- **Requested owner**: 10 (裁决 A) / `aot-representation-refinement` (能力 B)
- **Severity**: 82 条差分用例（占全部悬空的 16%）压在这一族上，且该族在诊断治理之外

## Exact source identity

测量基线：commit `00f665c5c`，tree `afe293b71`。

```
cmake -S . -B build-nofp -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DXRAY_STDLIB_VM_FASTPATHS=OFF -DXRAY_PYTHON=/opt/homebrew/bin/python3
```

差分网 515 条「不可比」用例逐条复验后 510 条仍被 AOT 拒绝，其中首次拒绝落在本族的分布：

| 首次拒绝 | 条数 | 发出路径 |
| --- | ---: | --- |
| `XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE` | 67 | `src/aot/xaot_driver.c:1942` |
| `XR_AOT_REFINEMENT_INCOMPLETE_COVERAGE` | 12 | `src/aot/xaot_driver.c:1956` |
| `XR_AOT_REFINEMENT_USE_SITE` | 3 | `src/aot/xaot_driver.c:1956` |
| `XR_TARGET_1006` + `XR_AOT_REFINEMENT_REPRESENTATION` | 5 | `src/aot/xaot_driver.c:1956` |
| **合计** | **87** | |

前三行 82 条把裸枚举名直接打给用户；第四行 5 条是同一族的同一个 `fprintf`，只是多了一个
`XR_TARGET_1006: ` 前缀。82/510 = 16.1%，87/510 = 17.1%。

族定义在 `src/aot/refine/xr_aot_refinement.h:63-95`（`XrAotRefinementIssue`，28 个成员）。
枚举名到字符串的转换在 `src/aot/refine/xr_aot_refinement.c:1235-1300`
（`xr_aot_refinement_issue_name`）。该结构体没有 `message` 字段，也没有 owner 字段：

```c
typedef struct XrAotRefinementDiagnostic {
    uint32_t issue;
    uint32_t record_index;
    uint32_t pass_id;
    uint32_t target_call_index;
    uint32_t semantic_value;
    uint32_t semantic_operation;
} XrAotRefinementDiagnostic;
```

## Minimal case

### A. `SCHEMA_UNAVAILABLE`（67 条里最小的 VM-clean 用例，9 行）

`tests/diff/cases/semantics/string/template_nested_quotes.xr`：

```
var m = #{"k": "value"}
var name = "Ada"

print("${"x"}")
print("value=${"a}b"}")
print("${m["k"]}")
print("outer ${"inner ${name}"}")
print("${'}'}")
print("${'"'}")
```

VM 侧跑通、退出码 0。AOT 侧首次拒绝的原因是第 1 行的 map 字面量：`MAP_NEW` 产生的值被
`INDEX_SET` 消费，而定义侧 oracle 说不出这个值已经在什么存储里。

### B. `INCOMPLETE_COVERAGE`（12 条里最小的，16 行）

`tests/diff/cases/semantics/default_params/default_evaluated_per_call.xr`：

```
var counter = 0
fn next() -> i64 {
    counter = counter + 1
    return counter
}
fn tag(id: i64 = next()) -> i64 { return id }

print(tag())     // next() -> 1
print(tag())     // next() -> 2
print(tag(99))   // supplied -> 99, next() not called
print(tag())     // next() -> 3
print(counter)   // next() ran exactly 3 times
```

VM 侧同样跑通。这条在**构建期一条 obligation 都没被拒**（survey 计数 0），是在
materialization 复核阶段才炸的——方向和 A 正好相反，见 B 节。

## Reproduce

```bash
cd /Users/xuxinglei/workspace/xray-lang/worktrees/r3-1-diff-net-unseal

cmake -S . -B build-nofp -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DXRAY_STDLIB_VM_FASTPATHS=OFF -DXRAY_PYTHON=/opt/homebrew/bin/python3
cmake --build build-nofp -j

# A
./build-nofp/xray build -N tests/diff/cases/semantics/string/template_nested_quotes.xr -o /tmp/a
# B
./build-nofp/xray build -N tests/diff/cases/semantics/default_params/default_evaluated_per_call.xr -o /tmp/b

# 两个已有的诊断开关（本单所有分组数据都来自它们，src/ 未改动一行）：
#   XRAY_AOT_REFINE_TRACE=1     每条拒绝打印 stage / 定义 opcode / 使用 opcode / 两侧 rep
#   XRAY_COLLECT_ALL_REFUSALS=1 构建期不在首次拒绝返回，走完整轮，收全部被拒 operand
XRAY_AOT_REFINE_TRACE=1 XRAY_COLLECT_ALL_REFUSALS=1 \
  ./build-nofp/xray build -N tests/diff/cases/semantics/string/template_nested_quotes.xr -o /tmp/a
```

注：`XRAY_AOT_REFINE_TRACE` 定义在 `src/aot/refine/xr_aot_representation_refinement.c:581`，
`XRAY_COLLECT_ALL_REFUSALS` 在同文件 `:600`。并发跑多个 `xray build -N` 会让原生 toolchain
探针 10s 超时并伪造出 `no provider reached READY`——串行或低并发重跑即可，与本族无关。

## Expected and actual

**A 的完整原始拒绝消息：**

```
[xi-native] Building: tests/diff/cases/semantics/string/template_nested_quotes.xr
Error: module representation authority build failed for 'template_nested_quotes_eab2d8321b509082': XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE value=4 operation=4
```

开 trace 后的第一条：

```
[aot-refine] refused in the definition oracle: no definition oracle names this value, so the pass never learns what storage it is already in
[aot-refine]   value=4          the value being consumed (the source)
[aot-refine]     defined by      = operation 3 MAP_NEW in function 0, block 0, semantic type 1
[aot-refine]     definition rep  = unnamed
[aot-refine]   operation=4      the USE SITE consuming that value -- not where it is defined
[aot-refine]     opcode          = INDEX_SET, operand 0, function 0, block 0
[aot-refine]     required rep    = unnamed
[aot-refine]   read it as: operation=4 consumes value=4; the operation that defines value=4 is the one printed under "defined by" above, and the two indexes are unrelated.
```

**B 的完整原始拒绝消息：**

```
[xi-native] Building: tests/diff/cases/semantics/default_params/default_evaluated_per_call.xr
Error: module representation materialization failed for 'default_evaluated_per_call_7c0e88b1191699f6': XR_AOT_REFINEMENT_INCOMPLETE_COVERAGE record=0 value=2 operation=4294967295
```

两条都是 VM 侧跑通、AOT 侧编译期拒绝，所以在差分网里落成「不可比」而不是「不一致」。

## A. The governance gap

### 1. `[policy]` 原文，以及谁在执行它

`contracts/target-machine/diagnostic-codes.toml:4-10`：

```toml
[policy]
unknown_code = "error"
duplicate_code = "error"
renumbering = "forbidden"
pointer_in_diagnostic_identity = false
required_context = ["plan_fingerprint", "function_or_module_id", "source_span"]
optional_context = ["operation_id", "layout_id", "owner_id", "state_id", "generation_id"]
```

**执行者是 `scripts/target_machine_phase0.py`**，经 CMake 注册为两个 ctest：
`target_machine_inventory`（`CMakeLists.txt:4392-4396`）和
`target_machine_inventory_self_test`（`CMakeLists.txt:4555-4559`），标签
`meta;target-machine;contracts`。

链路是 `validate_policies()` → `scripts/target_machine_phase0.py:596-597`：

```python
if diagnostics.get("policy", {}).get("unknown_code") == "error":
    errors.extend(unregistered_emitted_codes(root, seen, ranges))
```

`unregistered_emitted_codes()` 在 `:601-625`，它的 docstring 把意图写得很清楚——
「A code reaches a user the moment a source spells it」。但实现只做了这件事
（`scripts/target_machine_phase0.py:611-613`）：

```python
shape = re.compile(
    r'"(' + "|".join(re.escape(prefix) for prefix in sorted(ranges)) + r')([0-9]{4})'
)
```

**`XR_AOT_REFINEMENT_*` 逃过检查有两道各自独立、各自充分的原因：**

1. **前缀不在候选集里。** `ranges` 的键来自 toml 已注册的 `[[family]]` 前缀，即
   `XR_SEM_ / XR_TARGET_ / XR_ARTIFACT_ / XR_OWN_ / XR_CORO_ / XR_EXEC_`。正则的 alternation
   只由这 6 个拼出来，`XR_AOT_` 从来不在里面，所以扫描器**根本看不见**这一族。
2. **形状不匹配。** 正则要求前缀后紧跟 4 位数字。`XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE`
   没有编号，即使把 `XR_AOT_` 塞进 alternation 也匹配不上。

也就是说这道闸是**闭世界**的：它只能抓「已注册族里编号没登记的成员」，结构上抓不到
「整族没注册」。实测把该函数的逻辑原样拿出来跑一遍：

```
alternation prefixes: ['XR_ARTIFACT_', 'XR_CORO_', 'XR_EXEC_', 'XR_OWN_', 'XR_SEM_', 'XR_TARGET_']
does the gate regex match the enum-name string literal in xr_aot_refinement.c:1260 ? False
does it match a real registered code literal?                                       True
codes the gate can even see in src/: 54
unregistered among those: (none -> gate passes)
total registered codes: 55
```

闸是绿的，同时 28 个 `XR_AOT_REFINEMENT_*` 枚举名正在流向用户终端。

还有第三道锁：`expected_prefixes` 在 `scripts/target_machine_phase0.py:548-550` 是一个**硬编码
的冻结集合**，并且 `:552-554` 要求 toml 里的族集合与它**完全相等**。所以就算有人往
`diagnostic-codes.toml` 里加一个 `XR_AOT_` 族，这个 gate 会立刻以
`diagnostic family mismatch` 失败——注册这一族必须同时改脚本，两处都在
`scripts/check_contract_freeze.py:335-337` 的冻结清单里。

**这不是孤例。** `src/aot/` 下同样形状、同样裸打给 stderr 的 issue 族还有两个：

| 枚举 | 文件 | 成员数 | 打印点 |
| --- | --- | ---: | --- |
| `XrAotRefinementIssue` | `src/aot/refine/xr_aot_refinement.h:63-95` | 28 | `xaot_driver.c:1838,1849,1891,1903,1946,1960` |
| `XaotBackendContractIssue` | `src/aot/xi_backend_plan_contract.h:28-` | 15 | `xaot_driver.c:1224,1245,1266` |
| `XrAotTailCallConformanceIssue` | `src/aot/refine/xr_aot_tail_call_conformance.h` | 11 | `xaot_driver.c:1916` |

合计 54 个标识符，都没有注册的 name / message / owner，都没有任何 gate 覆盖。作为对照，
toml 里注册在案的一共 55 个码。**AOT 事实上有一套和注册表等体量的影子诊断词表。**

顺带一提，`contracts/target-machine/baseline-manifest.json:184`
（`/qualification/measurement/attribution/real_defect/families[0]`）已经在用
`XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE` 这个字符串当 finding 标识。契约语料
已经在依赖这个名字的稳定性，只是没有任何东西保证它稳定。

### 2. 为什么 5 条走 `XR_TARGET_1006`、82 条裸露——是不是两条路径

`XR_TARGET_1006` 注册在 `contracts/target-machine/diagnostic-codes.toml:167-170`：

```toml
[[code]]
id = "XR_TARGET_1006"
name = "representation-materialization-mismatch"
message = "materialized AOT value representation does not match verified refinement authority"
```

**是两条路径，但切分点不在 82 / 5 那里。** 真正的结构是：两条发出路径 + 其中一条上的一个
单码条件前缀。两条路径都在同一个函数
`xaot_install_module_representation_refinements()`（`src/aot/xaot_driver.c:1926-1974`）里：

**路径一 —— 构建期（`src/aot/xaot_driver.c:1939-1950`），无论如何都不带码：**

```c
if (!module || !module->init || !target_plan ||
    !xr_aot_representation_refinement_build_from_authority(target_plan, policy, &refinement,
                                                           &diag)) {
    fprintf(stderr,
            "Error: module representation authority build failed for '%s': "
            "%s value=%u operation=%u\n",
            module && module->name ? module->name : "?",
            xr_aot_refinement_issue_name(diag.issue), diag.semantic_value,
            diag.semantic_operation);
```

格式串里没有 `record=`，也没有任何码位。67 条 `SCHEMA_UNAVAILABLE` 全部走这里。

**路径二 —— materialization 复核期（`src/aot/xaot_driver.c:1952-1963`），码是条件拼上去的：**

```c
const char *code =
    diag.issue == XR_AOT_REFINEMENT_REPRESENTATION ? "XR_TARGET_1006: " : "";
fprintf(stderr,
        "Error: %smodule representation materialization failed for '%s': "
        "%s record=%u value=%u operation=%u\n",
        code, module->name ? module->name : "?",
        xr_aot_refinement_issue_name(diag.issue), diag.record_index,
        diag.semantic_value, diag.semantic_operation);
```

`XR_TARGET_1006:` 前缀只在 `diag.issue` 恰好等于 `XR_AOT_REFINEMENT_REPRESENTATION`（枚举里
28 个成员中的 1 个）时才出现。所以：

- 12 条 `INCOMPLETE_COVERAGE` + 3 条 `USE_SITE` + 5 条 `REPRESENTATION` = **20 条走的是同一个
  `fprintf`**（都有 `record=` 字段可以验证），只有其中 5 条拿到了码；
- 67 条 `SCHEMA_UNAVAILABLE` 走另一个 `fprintf`，那里连这个条件都没有。

结论：**注册码的覆盖不是按路径切的，是按 28 个枚举成员里挑了 1 个硬编码。**同一行输出、同一个
诊断结构体、同一个失败语义，只因为 issue 值不同，一部分有码一部分没有。

### 3. 裁决选项

**选项 1：把这一族注册进 toml，分配 `XR_AOT_xxxx` 号段。**

- 代价：`diagnostic-codes.toml` 新增一个 `[[family]]` + 28 个 `[[code]]`（若三族一起收，
  54 个）；必须同步改 `scripts/target_machine_phase0.py:548-550` 的 `expected_prefixes`，
  而两个文件都在 `scripts/check_contract_freeze.py:335-337` 的冻结清单里，需要一次
  contract 解冻 + 重锚。`renumbering = "forbidden"` 意味着号段一旦发就永久固定。
- 收益：82 条拒绝立刻获得稳定 name / message / owner；`baseline-manifest.json` 对枚举名的
  依赖变成合法引用；`unknown_code` 政策对 AOT 首次真正生效。
- 风险：这一族有 28 个成员，但差分网只观察到 4 个在飞。把 24 个从未出现过的成员也永久
  编号，是拿冻结号段去赌未来的分类。

**选项 2：让它们一律经 `XR_TARGET_1006` 包裹。**

- 代价：`XR_TARGET_1006` 的注册 message 是「materialized AOT value representation does not
  match verified refinement authority」——它描述的是**不匹配**。67 条
  `SCHEMA_UNAVAILABLE` 的语义是**没有任何一侧说得出 representation**（下面 B 节数据：613/614
  条 `required rep = unnamed`），根本不是 mismatch。全部套 1006 会让这个码的 message 变成谎话，
  而 `renumbering = "forbidden"` 挡住了事后改口。
- 收益：改动最小，只碰 `src/aot/xaot_driver.c` 两处 `fprintf`，不动契约冻结面。
- 风险：把 16% 的拒绝面折叠进一个码，等于永久放弃在诊断层面区分「不匹配」和「说不出」。
  下一轮再想分开就得新发码，和选项 1 的代价一样，只是推迟了。

**本 lane 的判断（供 10 号参考，不是结论）**：两个选项都假设「码 = 枚举成员」。B 节的数据
说这 82 条真正的分类轴既不是枚举成员也不是 `operation=`，而是 **(拒绝阶段, opcode 分支)**
这个 55 元的集合。如果只做选项 2，等于给 55 个不同缺口盖同一个 message；如果只做选项 1，
拿到的是 4 个在飞的码，而它们对定位缺口的分辨率仍然是 4，不是 55。建议裁决时把
`optional_context` 里已经预留的 `operation_id` 语义一并定死——见 B 节第 3 点，它现在的含义
和名字对不上。

## B. The capability gap

### 4. `REPRESENTATION_SCHEMA_UNAVAILABLE` 的判据

这个枚举在 `src/aot/refine/xr_aot_representation_refinement.c` 里有 **16 个** `set_diag`
发出点，但差分网 67 条**全部**落在同一个：
`src/aot/refine/xr_aot_representation_refinement.c:10731-10745`，位于
`authority_collect_obligations_indexed()`（定义在 `:10694`）——构建期逐 operation、
逐 operand 收集 representation obligation 的主循环。判据表达式：

```c
bool has_definition =
    oracle_definition_storage(oracle, source_value, &input_storage, &ignored_machine);
bool identity_use = has_definition && collect_index_set_i64_identity_use_is_exact(
                                          ctx, i, a, source_value);
if (identity_use)
    output_storage = XR_REP_I64;
bool has_use =
    identity_use ||
    (has_definition && oracle_use_storage(oracle, i, a, source_value, &output_storage));
if (!has_definition || !has_use) {
    rep_trace_refusal(
        oracle, has_definition ? "the use-site oracle" : "the definition oracle",
        has_definition
            ? "the use site admits no storage for this operand: its opcode branch in "
              "oracle_use_storage names no family that covers the value"
            : "no definition oracle names this value, so the pass never learns what "
              "storage it is already in",
        source_value, i, a, has_definition ? input_storage : XR_REP_COUNT,
        XR_REP_COUNT);
    set_diag(ctx->diag, XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE,
             ctx->record_count, source_value, i);
```

也就是说：**两个 oracle 里任意一个说不出这个 operand 的存储，就拒。**
`oracle_use_storage` 定义在同文件 `:9331`，是一个按 opcode 分支的**封闭枚举**——代码自己的
措辞是「its opcode branch in `oracle_use_storage` names no family that covers the value」。
定义侧同理：`src/aot/refine/xr_aot_representation_refinement.c` 里
`static bool oracle_*storage(` 形状的函数共 **75 个**，逐个覆盖
array / string / closure / struct / map / channel 等 family——这 75 个函数就是那张手工枚举表
的规模，也是它为什么必然漏。

另有一个次要判据在 `src/aot/refine/xr_aot_refinement.c:552-557`
（`derive_representation_record()` 内）：

```c
if (!machine || enum_descriptor) {
    if (request->layout != XR_SEMANTIC_INDEX_NONE)
        return XR_AOT_REFINEMENT_LAYOUT;
    representation_record_fingerprint(baseline, out, &out->fingerprint);
    return XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE;
}
```

这一条是**被容忍的**（`xr_aot_refinement.c:1562` 和 `:1753` 显式把它从 fatal 里排除，
记录成 `REFUSED` 决定但不中止），所以它不产生用户可见的拒绝，差分网 67 条里一条都不来自这里。

### 5. `value=N operation=M` 是什么——以及为什么按 `operation=` 分组是错的轴

从 `rep_trace_refusal()`（`src/aot/refine/xr_aot_representation_refinement.c:730-770`）
自己的说明文字可以直接读出来：

- `value=N` 是**被消费的语义值 id**（`XrSemanticPlan` 的 value 编号），trace 里注释为
  "the value being consumed (the source)"；
- `operation=M` 是**使用点的语义 operation 索引**，trace 里注释为
  "the USE SITE consuming that value -- not where it is defined"。

**它不是 XI 操作码，也不是定义点。** trace 甚至专门印了一行防止误读：
"the two indexes are unrelated"。定义点的 opcode 要另外从
`rep_trace_origin()`（`:686`）打印的 `defined by = operation N <OPCODE>` 里取。

因此 packet 要求的「按 `operation=` 取值分组」这条轴测出来是这样，但**这个结果没有意义**：

| 码 | 条数 | `operation=` 不同取值数 | 最大组 |
| --- | ---: | ---: | ---: |
| `REPRESENTATION_SCHEMA_UNAVAILABLE` | 67 | 36 | 4（`operation=7/8/13/14/18` 各 4） |
| `INCOMPLETE_COVERAGE` | 12 | 1 | 12（全是 `4294967295`） |
| `USE_SITE` | 3 | 3 | 1 |

`operation=` 是**模块内使用点的序号**：同一个底层缺口在两个不同程序里会拿到两个不同的
`operation=`，不同缺口在同一程序里也可能撞号。36 这个数字既不是 36 个缺口，也不是
36 个类别，它只是 67 个程序里第一个炸掉的语句各自排第几。至于
`INCOMPLETE_COVERAGE` 那 12 条整齐的 `4294967295`，那是 `XR_SEMANTIC_INDEX_NONE` 常量，
从 `src/aot/refine/xr_aot_representation_refinement.c:11398/11407` 硬写进去的哨兵值，
含义是「这个使用点不是 operand，是块返回」——12 条同值不代表它们是同一组。

**真正的分组轴是 (拒绝阶段, opcode 分支)。** 用 `XRAY_COLLECT_ALL_REFUSALS=1` 走完整轮，
67 条构建期用例一共暴露 **614 个被拒 operand**（另外 20 条在构建期零拒绝，见下）：

| 拒绝阶段 | 被拒 operand 数 |
| --- | ---: |
| the definition oracle | 310 |
| the use-site oracle | 297 |
| the return-storage oracle | 6 |
| the adapter layout lookup | 1 |

而 (定义侧 rep → 需求侧 rep) 的分布是压倒性的一边倒：

| definition rep → required rep | 条数 |
| --- | ---: |
| `unnamed` → `unnamed` | 310 |
| `tagged` → `unnamed` | 203 |
| `i64` → `unnamed` | 97 |
| `ptr` → `unnamed` | 2 |
| `f64` → `unnamed` | 1 |
| `void` → `tagged` | 1 |

**614 条里 613 条的 `required rep` 是 `unnamed`。**（`unnamed` 是
`rep_trace_storage_name()` 对 `XR_REP_COUNT` 的渲染，注释写着 "this side never named a
storage"。）只有 1 条是真正的两侧都命名了但不相容。这直接否掉了「representation 选错了」
这个假设：pass 绝大多数时候**根本没走到选的那一步**。

按 opcode 分支切开（这才是「一个修复要扩哪个分支」的单位）：

**定义侧 oracle 的 310 条，26 个 opcode：**

| 定义点 opcode | 条数 | 占比 |
| --- | ---: | ---: |
| `ARRAY_NEW` | 78 | 25.2% |
| `CORO_OP` | 33 | 10.6% |
| `STR_CONCAT` | 20 | 6.5% |
| `INDEX_GET` | 17 | 5.5% |
| `GO` | 17 | 5.5% |
| `TYPENAME` | 14 | 4.5% |
| `LOAD_FIELD` | 13 | 4.2% |
| `<parameter>`（函数入参绑定，非 operation） | 12 | 3.9% |
| `STACK_ALLOC` | 11 | 3.5% |
| `RANGE` / `COPY` / `SOURCE_MOVE` | 各 10 | 各 3.2% |
| 其余 15 个 opcode | 65 | 21.0% |

**使用侧 oracle 的 297 条，27 个 opcode：**

| 使用点 opcode | 条数 | 占比 |
| --- | ---: | ---: |
| `INDEX_GET` | 82 | 27.6% |
| `INDEX_SET` | 43 | 14.5% |
| `ARRAY_NEW` | 20 | 6.7% |
| `GO` | 20 | 6.7% |
| `STR_CONCAT` | 20 | 6.7% |
| `ARRAY_EXTEND` | 15 | 5.1% |
| `STORE_FIELD` | 14 | 4.7% |
| `OBJECT_MERGE` | 14 | 4.7% |
| 其余 19 个 opcode | 69 | 23.2% |

## How the 82 cases split

先按发出路径分成两半，两半的缺口方向是**相反**的：

| | 条数 | 症状 |
| --- | ---: | --- |
| 构建期（`xaot_driver.c:1942`） | 67 | 权威**建不出来**：oracle 覆盖不到程序里的 opcode |
| materialization 复核期（`xaot_driver.c:1956`） | 20 | 权威**建出来了但盖不住**：后端物化了权威没授权的 adapter |

第二半的 20 条构建期零拒绝，全部在复核阶段炸。其中 12 条 `INCOMPLETE_COVERAGE` 来自
`verify_no_extra_materialized_adapters()`（`src/aot/refine/xr_aot_representation_refinement.c:11385-11412`），
判据是「Xi 里存在 `backend_origin != XI_BACKEND_VALUE_NONE` 的值，而权威的 record 集合不包含它」——
**后端已经插了一个 representation adapter，refinement 权威从没批准过它**。这 12 条按目录分：
`semantics/collections/` 7 条（5 条 `byte_array_*`/`byte_slice_*` + 2 条 `span_*_pod_*`）、
`semantics/ffi/` 2 条、`semantics/default_params/` `semantics/stdlib/` `semantics/types/`
各 1 条——除最后两条外全部落在 byte / span / 裸内存路径上。

第一半 67 条的真正维度是 **55 个不同的 (stage, opcode) 分支**。每条用例要全部清掉才算解封，
所需分支数的分布：

| 一条用例需要扩的分支数 | 用例数 |
| ---: | ---: |
| 1 | 21 |
| 2 | 18 |
| 3 | 8 |
| 4 | 8 |
| 5 | 5 |
| 6 | 5 |
| 7 | 1 |
| 8 | 1 |

**这一族既不是一个缺口，也不是几十个高价值缺口——它是一条极平的长尾。** 用贪心集合覆盖
（每步挑「能让最多用例彻底清零」的分支）算出的最优前缀：

| 累计扩的分支数 | 彻底解封的用例 |
| ---: | ---: |
| 5 | 10 / 67 (15%) |
| 10 | 17 / 67 (25%) |
| 20 | 29 / 67 (43%) |
| 26 | 39 / 67 (58%) |
| 35 | 53 / 67 (79%) |
| 45 | 63 / 67 (94%) |
| **55** | **67 / 67 (100%)** |

即使是理论最优的挑法，也要扩 26 个分支才过半，没有任何小子集能解开有意义的比例。
全部 55 个分支里，能靠自己单独清零用例的最高产的也只有 2 条
（`def TYPENAME`、`use AS`、`use INDEX_SET`、`use STORE_FIELD`、`def CALL_BUILTIN` 各 2 条），
因为 46/67 条用例同时踩了两个以上分支。

**这条曲线的形状本身就是结论**：`oracle_definition_storage` / `oracle_use_storage` 是一张
按 opcode 手工枚举的表，而差分网里的普通程序（一个 map 字面量、一个默认参数、一次数组下标）
routinely 落在表外。要清掉这 67 条，逐个补 opcode 分支需要 55 次改动且每次收益极低；
真正需要的是**让这两个 oracle 变成全函数**——对每个 (opcode, operand) 都有一条可判定的
存储规则（哪怕是保守的 `tagged` 兜底），而不是「表里没有就拒」。

## Requested decision

### 裁决结果（10 号，2026-08-28）：**注册进 `diagnostic-codes.toml` 分配号段，不走包裹**

裁决理由比本单原先写的两条代价更强，记在这里：

> **"经 `XR_TARGET_1006` 包裹"就是在制造第 186 条。**
> `XR_TARGET_1003` 在 `src/plan/target/` 下已有 185 条互不相关的消息文本，
> 已经被证明"按码聚类没有分辨率"。把 82 条本来还带着区分性枚举名的诊断塞进一个
> 已知没有分辨率的码里，方向正好相反。注册则让它们从此受
> `[policy] unknown_code = "error"` 治理——既然事实上已经是诊断，就该按诊断治理。

执行要动 `contracts/target-machine/diagnostic-codes.toml` 与
`src/aot/refine/xr_aot_refinement.h`，超出 1 号的 `tests/diff/` 范围，
由 10 号记为本轮独立工作项。

**下面第 2、3 条不受该裁决影响，仍待处理**——特别是第 2 条：裁决选了"注册"，
但如果 `unregistered_emitted_codes()` 仍然只能做闭世界扫描，注册完照样没有闸在守，
下一族裸标识符会以同样方式再长出来。

交 10 号：

1. `XR_AOT_REFINEMENT_*`（28 个成员）、`XaotBackendContractIssue`（15 个）、
   `XrAotTailCallConformanceIssue`（11 个）这三族共 54 个裸标识符，是注册进
   `diagnostic-codes.toml`，还是一律经已注册码包裹？两个选项的代价见 A.3。
   请注意这不是 82 条的局部问题——AOT 目前有一套和注册表等体量的影子词表。
2. 无论选哪个，`scripts/target_machine_phase0.py:601-625` 的
   `unregistered_emitted_codes()` 都需要改成开世界扫描：现在它只能抓
   「已注册族里编号没登记的成员」，抓不到「整族没注册」，所以
   `unknown_code = "error"` 这条政策在 AOT 上从未生效过。这一条独立于 1，
   即使裁决是「不注册」，也应该让闸能看见并显式豁免，而不是看不见。
3. `[policy]` 的 `optional_context` 里已经列了 `operation_id`。本族输出的
   `operation=` 字段实际含义是**使用点的 operation 索引**，不是定义点、不是 opcode。
   请一并定死这个字段的语义，否则下游（包括本轮另外三个 lane）会继续按定义点去读它。

## Requested capability

交 `aot-representation-refinement`（owner 名取自
`src/aot/refine/xr_aot_representation_refinement.c:650` survey 行里的
`owner=aot-representation-refinement`）：

1. **构建期 67 条**：`oracle_definition_storage` 与 `oracle_use_storage`
   （`src/aot/refine/xr_aot_representation_refinement.c:9331` 及 `oracle_*_storage` 系列）
   目前是按 opcode 分支的封闭枚举，表外即拒。请求把它们改成对 (opcode, operand) 全覆盖的
   判定——包括一条保守兜底规则。逐分支补的路线需要 55 次改动、26 次才过半，见上表。
2. **复核期 12 条 `INCOMPLETE_COVERAGE`**：请求确认这是权威覆盖不足还是 Xi 后端越权物化。
   `verify_no_extra_materialized_adapters()`
   （`src/aot/refine/xr_aot_representation_refinement.c:11385-11412`）拒的是
   「Xi 里有 `backend_origin != XI_BACKEND_VALUE_NONE` 的值不在权威 record 集合里」，
   集中在 byte_slice / span / FFI 裸内存路径。方向和 1 相反，可能需要不同的 owner。
3. **3 条 `USE_SITE` + 5 条 `XR_TARGET_1006`**：数量小，建议在 1、2 落地后重测再判断，
   本单不单独提请求。

## Files deliberately not modified

- `src/` 下未改动任何文件。本单所有分组数据都来自仓库**已有**的两个诊断开关
  `XRAY_AOT_REFINE_TRACE` 和 `XRAY_COLLECT_ALL_REFUSALS`，没有为取数加过一行代码。
- `tests/` 下未改动任何文件，包括
  `tests/diff/known_failures_not_comparable.txt` 和
  `tests/diff/known_failures_embedded_not_comparable.txt`。
- `contracts/target-machine/diagnostic-codes.toml` 未改动——注册与否是 10 号的裁决。
- `scripts/target_machine_phase0.py` 未改动——它和 toml 同在
  `scripts/check_contract_freeze.py:335-337` 的冻结清单里，两处必须同批改。
- 本轮另外三份 blocker（`r3-1-diff-net-multi-module-program-authority.md`、
  `r3-1-embed-lane-stdlib-module-not-bundled.md`、
  `r3-1-call-shaped-no-exact-target-authority.md`）未触碰。
- `blockers/r3-1-diff-net-refusal-census.md` 只读引用，未修改。

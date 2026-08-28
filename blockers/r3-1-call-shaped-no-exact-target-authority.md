# Blocker: 114 条差分用例卡在同一条 `XR_TARGET_1003`——TargetPlan 的 call 家族对 21 组内建调用形状没有任何识别器

- **Lane**: 1 (差分网解封)
- **Requested owner**: **5 号**（`src/plan/target/xr_target_builder.c` call 族段的能力实现）。理由：这 114 条不是"同一判据在多层写了两份且不一致"，而是**一份都没有**——`Array.push` / `Map.set` / `Set.add` / `Channel.trySend` / `Task.cancel` / `JSON.parseObject` / `XI_GEN_CALL` 在 `src/plan/` 全树没有任何 `*_is_exact` 识别器（证据见 `## Is this one gap or many`）。7 号确实能在这里找到一处逐字重复（两份 `is_call_shaped`），但**两份完全一致**，抽取后一条用例也解不开。
- **Status**: `BLOCKED`
- **Severity**: 114 条差分用例（占全部 515 条悬空的 22%）压在这一条上，是差分网最大的单点

## Exact source identity

| item | value |
|---|---|
| base commit | `00f665c5cfcdc5f0f938ba133c42a258a640d95f`，tree `afe293b71f96b8eae009027a876158d0a00375f2` |
| worker branch | `work/1-diff-net-unseal-00f665c5c` |
| binary | `build-nofp/xray`, sha256 `e6944de685d8829c0528087318db20c34c2a52c3e65fc4eee81b3518b6d5938b` |
| build configuration | `cmake -S . -B build-nofp -G Ninja -DCMAKE_BUILD_TYPE=Release -DXRAY_STDLIB_VM_FASTPATHS=OFF -DXRAY_PYTHON=/opt/homebrew/bin/python3` |
| 逐条原始数据 | `/tmp/r3-lane1-refusal-baseline.jsonl`（1030 行 = 515 用例 × aot/vm 各一条） |

`XRAY_STDLIB_VM_FASTPATHS=OFF` 不是可选项：默认配置在这个基线上构建不完。本单全部读数都是 OFF 档读数。

## Minimal case

三条按文件字节数排序最小、且分别落在三种不同 opcode 上的用例。

### 1. `tests/diff/cases/semantics/stdlib/native_tailcall_cfunction_direct.xr`（114 字节，opcode 116 `XI_CALL`）

```
import { isFinite } from math

fn finite_tail(x: f64) -> bool {
    return isFinite(x)
}

print(finite_tail(1.5))
```

### 2. `tests/diff/cases/semantics/collections/range_to_array_too_large_shared_core.xr`（170 字节，opcode 117 `XI_CALL_METHOD`）

```
// anchor: range-to-array-too-large-shared-core
var huge = 0..10000001

try {
    print(len(huge.toArray()))
} catch panic (p) {
    print(p.message)
    print(p.code)
}
```

### 3. `tests/regression/11_coroutine/1100_cancelled.xr`（241 字节，opcode 120 `XI_CALL_BUILTIN`）

```
// 测试 cancelled() 表达式
// 目前返回 false（尚未实现完整协程）

@test
fn test_cancelled() {
    var is_cancelled = cancelled()
    print(is_cancelled)  // 应输出 false
}

// 期望输出：
// false

test_cancelled()
```

## Reproduce

差分 runner 用的就是这条命令（`tests/diff/run_backend_diff.py:313-316`）：

```bash
XRAY_AOT_FAST_TEST_BUILD=1 ./build-nofp/xray build --native -O 1 \
  --cache-dir <cache> <case>.xr -o <out>
```

加 `XRAY_TARGET_TRACE=1` 可打印判据逐条走向；加 `XRAY_COLLECT_ALL_REFUSALS=1` 可一次收齐同一模块内的全部拒绝行，而不是只报第一条。

**注意**：这个二进制上 `native-run` 探针会间歇性 10s 超时并伪造出
`no provider reached READY for target 'aarch64-apple-darwin'`，掩盖真实拒绝
（已有单：`blockers/b-probe-timeout-fabricates-refusals.md`）。上面三条我各重试到拿到真实拒绝为止，
`range_to_array_too_large_shared_core.xr` 连续两次被探针掩盖、第三次才拿到 trace。**复验时看到探针错误请重跑，不要记成新拒绝。**

## Expected and actual

Expected：AOT 原生线与 VM 线都构建成功，产物字节可比。

Actual：AOT 侧 `TargetPlan` 构建失败，`XR_TARGET_1003`。三条最小用例的**完整原始拒绝消息**（操作数正是接手方要看的东西）：

```
Error: program TargetPlan build failed for 'native_tailcall_cfunction_direct_e95d03f0b2701bbf': XR_TARGET_1003: call-shaped operation has no exact target authority operation=11 function=1 opcode=116 selector= intrinsic=0 immediate=0 result=value:13,type:3,ownership:3,alias:-1 operands=2 receiver=value:12,type:1,role:1,flags:0 argument=value:10,type:0,role:3,flags:1 receiver-type=type-v3:14:0:0:0:0:0:0:0:0:255:0:
Error: product Program TargetPlan build failed: XR_TARGET_1000: program TargetPlan build did not produce verified authority
```

```
Error: program TargetPlan build failed for 'range_to_array_too_large_shared_core_7866f15f2c8409f1': XR_TARGET_1003: call-shaped operation has no exact target authority operation=6 function=0 opcode=117 selector=toArray intrinsic=0 immediate=158 result=value:7,type:2,ownership:0,alias:-1 operands=1 receiver=value:6,type:4,role:2,flags:1 argument=value:4294967295,type:4294967295,role:0,flags:0 receiver-type=type-v3:11:0:0:0:0:0:0:0:0:255:0:;named:5:Range[0]
Error: product Program TargetPlan build failed: XR_TARGET_1000: program TargetPlan build did not produce verified authority
```

```
Error: program TargetPlan build failed for '1100_cancelled_1f1e8bd8d0979bed': XR_TARGET_1003: call-shaped operation has no exact target authority operation=4 function=1 opcode=120 selector= intrinsic=0 immediate=0 result=value:6,type:3,ownership:3,alias:-1 operands=0 receiver=value:4294967295,type:4294967295,role:0,flags:0 receiver-type=<none>
Error: product Program TargetPlan build failed: XR_TARGET_1000: program TargetPlan build did not produce verified authority
```

第四种 opcode（158 `XI_GEN_CALL`，9 条）的代表，`tests/diff/cases/semantics/generator/yield_throw.xr`（342 字节）：

```
Error: program TargetPlan build failed for 'yield_throw_98b7296aaf6ca8ac': XR_TARGET_1003: call-shaped operation has no exact target authority operation=5 function=0 opcode=158 selector= intrinsic=0 immediate=0 result=value:6,type:4,ownership:0,alias:-1 operands=1 receiver=value:5,type:6,role:0,flags:0 receiver-type=type-v3:13:0:0:0:0:0:0:0:0:255:0:fn:0:0:0:0:1;ret:type-v3:11:0:0:0:0:0:0:0:0:255:0:;named:8:Iterator[1;type-v3:0:0:0:0:0:0:0:0:0:0:0:];view:0:-1:1
```

`XRAY_TARGET_TRACE=1` 下，第 2 条的判据走向（这段是本单最有信息量的一段）：

```
[target] refused in call coverage: this operation is call-shaped, SemanticPlan proved no call target for it, and no builtin family here claimed it either
[target]   operation=6      CALL_METHOD in function 0, block 1
[target]     metadata[0]                  toArray
[target]     operand[0]                   role=receiver value=6 type=4 ... flags=0x01
[target]     SemanticPlan bound a call target to this operation   NO
[target]   every builtin family was asked about this operation and each declined; the families are selected by selector and receiver shape ...
[target]     type of operand[0]           type 4 kind=11 ... type-v3:11:...;named:5:Range[0]
[target]   read it as: a CALL or TAIL_CALL here means the semantic layer could not name a callee at all, while a CALL_METHOD means the selector belongs to no family this pass describes.
```

## Where the refusal is decided

拒绝点在 `builder_add_calls_and_adapters`（`src/plan/target/xr_target_builder.c:10897`），消息模板在
**`src/plan/target/xr_target_builder.c:11373`**，`uncovered_call` 标号在 `:11267`。

这个函数对每个 SemanticPlan operation 走两道关：

1. **`src/plan/target/xr_target_builder.c:11142`** — `target_by_operation[i] != XR_SEMANTIC_INDEX_NONE`：
   语义层是否已经为这条 operation 证明了一个 `XrSemanticCallTargetRecord`。有就按 kind 分派给对应
   collector（`DIRECT_LOCAL` / `SOURCE_CLASS_CONSTRUCTOR` / `NATIVE_NAMESPACE_YIELDABLE` /
   `BUILTIN_INSTANCE_YIELDABLE` / `SOURCE_EXPORT`）。
2. **`src/plan/target/xr_target_builder.c:11180-11265`** — 一条约 35 分支的 `else if` 链，每个分支是一个
   内建家族识别器 `*_is_exact(...)`，命中就调对应的 `collect_*_call_intent`。

两道关都没接住，且 `semantic_operation_is_call_shaped(plan, operation)`
（**`src/plan/target/xr_target_builder.c:8914`**）为真，就落到 `:11267` 的 `uncovered_call` 发出本拒绝。

**这个拒绝在问什么**：这条 call 形状的 operation，"到底调用的是谁"这件事有没有一个**精确的、可命名的**权威来源。
它需要二者之一才会放行——(a) 语义层已证明的 call target 记录，或 (b) 目标层某个内建家族识别器认领它并建出 call intent。

**往上一层：谁决定 target authority 存不存在。**
`append_call_target`，**`src/plan/semantic/xr_semantic_builder.c:3631`**，由
`src/plan/semantic/xr_semantic_builder.c:5053` 对每条 operation 调用一次。它的结构决定了本单一半的分组：

- `:3634` — `value->nargs == 0` 直接 `return true`（成功但不建记录）。
- `:3635-3653` — `XI_CALL_METHOD`：依次试 `append_source_export_call_target`(`:3469`)、
  `append_native_namespace_call_target`、`append_builtin_instance_yieldable_call_target`、
  `append_source_instance_method_local_call_target`(`:3324`)、`append_source_instance_method_open_call_target`(`:3376`)。
  全不命中就没有记录。
- **`:3655`** — `if (value->op != XI_CALL && value->op != XI_TAIL_CALL) return true;`
  **`XI_CALL_BUILTIN` 和 `XI_GEN_CALL` 在这里被静默放行，语义层结构上永远不会为它们建 call target。**
- `:3737` — 其余走 `DIRECT_LOCAL` / `NATIVE_YIELDABLE` / `INDIRECT_CALLABLE`。

验证侧还有第三道关：`src/plan/target/xr_target_verify.c:7891-7895`，用
`operation_is_call_shaped`(`src/plan/target/xr_target_verify.c:5053`) 重扫一遍，任何 call 形状但
`covered[]` 为 0 的 operation 都让整个 TargetPlan 失效（`XR_TARGET_1003: call/adapter tables do not exactly cover target authority`）。
这道关是**结构性**的——`covered[]` 由实际发出的 call 行填充（`:7884`），所以 5 号在 builder 里新增家族后它会自动满足，
**不需要另外同步一张允许列表**。

## Root cause

`XI_CALL_METHOD` 上的内建方法（容器、Channel、Task、JSON 命名空间、标量方法）在这条编译线上**既不走语义层的
call target，也没有目标层的家族识别器**。语义层只为"能命名到一个函数体"的调用建 target
（源码导出、本地直接调用、类构造、native namespace、builtin instance yieldable）；内建方法按设计是由目标层
`*_is_exact` 家族认领的，而这 21 组的识别器根本不存在。

`XI_CALL_BUILTIN`（7 条）和 `XI_GEN_CALL`（9 条）更靠前一步：`append_call_target`
在 `xr_semantic_builder.c:3655` 就把它们排除在外，语义层**结构上**不可能给出权威，因此这 16 条只能由目标层家族认领。

## How the 114 cases split

opcode 数值经编译 `src/ir/xi.h` 的 `XiOp` 枚举确认；receiver 类型 kind 经编译
`src/runtime/value/xtype.h` 的 `XR_KIND_*` 枚举确认（`type-v3:` 规范键的第一个字段就是 kind）。
分组键取 **opcode × receiver 顶层类型**——`selector` 单独看是 42 个值的长尾，没有结构意义；
opcode × receiver 才对应"要写哪个家族识别器"。

| # | opcode | receiver 顶层类型 | 条数 | selectors |
|---|---|---|---|---|
| 1 | `XI_CALL_METHOD` (117) | `CLASS<JSON>` | 18 | `parseObject`×7, `parseValue`×5, `require`×3, `stringify`×2, `kindOf` |
| 2 | `XI_CALL_METHOD` (117) | `UNKNOWN` | 13 | `allocZeroed`×4, `make`, `sqrt`, `crc32`, `endsWith`, `origin`, `isFinite`, `alloc`, `abs`, `escape` |
| 3 | `XI_CALL_METHOD` (117) | `ARRAY` | 13 | `push`×4, `forEach`×2, `unshift`, `shift`, `toString`, `sort`, `map`, `pop`, `iterator` |
| 4 | `XI_CALL_METHOD` (117) | `MAP` | 12 | `set`×9, `get`, `keys`, `containsKey` |
| 5 | `XI_CALL_METHOD` (117) | `SET` | 10 | `add`×9, `contains` |
| 6 | `XI_GEN_CALL` (158) | `FUNCTION` | 9 | （无 selector） |
| 7 | `XI_CALL_METHOD` (117) | `INSTANCE<Task>` | 6 | `cancel`×4, `poll`×2 |
| 8 | `XI_CALL_METHOD` (117) | `CHANNEL` | 6 | `trySend`×6 |
| 9 | `XI_CALL_METHOD` (117) | `INT` | 5 | `toString`×2, `checkedAdd`×2, `addOverflows` |
| 10 | `XI_CALL_METHOD` (117) | `INSTANCE<Range>` | 4 | `contains`×2, `toArray`, `toString` |
| 11 | `XI_CALL_BUILTIN` (120) | `ARRAY` | 4 | `array_copy_new`×2, `array_resize`×2 |
| 12 | `XI_CALL` (116) | `UNKNOWN` | 3 | （无 selector） |
| 13 | `XI_CALL` (116) | `CLASS<Atomic>` | 3 | （无 selector） |
| 14 | `XI_CALL_BUILTIN` (120) | `NO-RECEIVER` | 1 | （无 selector） |
| 15 | `XI_CALL_METHOD` (117) | `CLASS<String>` | 1 | `fromUtf8` |
| 16 | `XI_CALL_METHOD` (117) | `STRING` | 1 | `indexOf` |
| 17 | `XI_CALL_BUILTIN` (120) | `INSTANCE<Config>` | 1 | `copy` |
| 18 | `XI_CALL_BUILTIN` (120) | `INSTANCE<Box>` | 1 | `copy` |
| 19 | `XI_CALL_METHOD` (117) | `ENUM<Color>` | 1 | `toString` |
| 20 | `XI_CALL_METHOD` (117) | `RUNE` | 1 | `toString` |
| 21 | `XI_CALL_METHOD` (117) | `ENUM<E>` | 1 | `toString` |

**21 组，合计 114。** 按 opcode 汇总：`XI_CALL_METHOD` 92、`XI_GEN_CALL` 9、`XI_CALL_BUILTIN` 7、`XI_CALL` 6。
`intrinsic` 全部为 0（114/114），所以 intrinsic 不是分组轴。

两点值得单独指出：

- **第 2 组的 receiver 是 `UNKNOWN`（kind 14）**，不是某个具体类型。这 13 条加上第 12 组的 3 条，
  共 16 条的 receiver 规范键都是裸的 `type-v3:14:0:0:0:0:0:0:0:0:255:0:`。从 selector 看
  （`sqrt` / `abs` / `isFinite` / `crc32` / `escape` / `allocZeroed` / `alloc`）这些是**模块命名空间调用**
  （`math.sqrt`、`mem.allocZeroed` 之类），"receiver"是没有类型的模块名。它们和容器方法**不是同一个缺口**，
  修法也不同——需要的是命名空间解析，不是 receiver 形状匹配。
- **第 6 组 `XI_GEN_CALL` 的 receiver 是 `FUNCTION` 且返回 `Iterator`**，全部来自
  `tests/diff/cases/semantics/generator/`，是生成器 resume 调用；这 9 条同源，很可能一个家族一次解开。

### 完整 114 条路径清单（按组）

### XI_CALL_METHOD / receiver CLASS<JSON> — 18 条

selectors: `parseObject`×7, `parseValue`×5, `require`×3, `stringify`×2, `kindOf`

- `tests/diff/cases/semantics/collections/byte_array_int_arg_shared_core.xr` — selector=`parseObject` immediate=652 operands=2
- `tests/diff/cases/semantics/collections/map_json_concrete_type_check.xr` — selector=`parseObject` immediate=652 operands=2
- `tests/diff/cases/semantics/collections/slice_int_arg_shared_core.xr` — selector=`parseObject` immediate=652 operands=2
- `tests/diff/cases/semantics/exception/type_cast_panic_shape.xr` — selector=`parseObject` immediate=652 operands=2
- `tests/diff/cases/semantics/exception/type_check_panic_shape.xr` — selector=`parseObject` immediate=652 operands=2
- `tests/diff/cases/semantics/json/json_decode_nullable_nested_object.xr` — selector=`parseValue` immediate=650 operands=2
- `tests/diff/cases/semantics/json/json_decode_object_loop_reuse.xr` — selector=`parseObject` immediate=652 operands=2
- `tests/diff/cases/semantics/json/json_decode_typed_fields.xr` — selector=`parseValue` immediate=650 operands=2
- `tests/diff/cases/semantics/json/json_derive_decode.xr` — selector=`require` immediate=660 operands=3
- `tests/diff/cases/semantics/json/json_derive_struct_encode.xr` — selector=`stringify` immediate=654 operands=2
- `tests/diff/cases/semantics/json/json_encode_boundary.xr` — selector=`require` immediate=660 operands=3
- `tests/diff/cases/semantics/json/json_kind_and_object_type.xr` — selector=`kindOf` immediate=646 operands=2
- `tests/diff/cases/semantics/json/json_parse_value_invalid_panic.xr` — selector=`parseValue` immediate=650 operands=2
- `tests/diff/cases/semantics/json/json_typed_root_values.xr` — selector=`parseValue` immediate=650 operands=2
- `tests/diff/cases/semantics/json/json_with_rest_and_path.xr` — selector=`stringify` immediate=654 operands=2
- `tests/diff/cases/semantics/json/object_json_codec_bridge.xr` — selector=`require` immediate=660 operands=3
- `tests/diff/cases/semantics/stdlib/math_random_int_arg_shared_core.xr` — selector=`parseObject` immediate=652 operands=2
- `tests/diff/cases/semantics/types/object_shape_test_and_narrow.xr` — selector=`parseValue` immediate=650 operands=2

### XI_CALL_METHOD / receiver ARRAY — 13 条

selectors: `push`×4, `forEach`×2, `unshift`, `shift`, `toString`, `sort`, `map`, `pop`, `iterator`

- `tests/diff/cases/semantics/collections/array_hof_shared_core.xr` — selector=`forEach` immediate=24 operands=2
- `tests/diff/cases/semantics/collections/array_shift_shared_core.xr` — selector=`shift` immediate=100 operands=1
- `tests/diff/cases/semantics/collections/array_sort_shared_core.xr` — selector=`sort` immediate=346 operands=2
- `tests/diff/cases/semantics/collections/array_unshift_shared_core.xr` — selector=`unshift` immediate=102 operands=2
- `tests/diff/cases/semantics/collections/byte_array_append_repeat_safe_shared_core.xr` — selector=`toString` immediate=170 operands=1
- `tests/diff/cases/semantics/collections/hof_index_param.xr` — selector=`map` immediate=0 operands=2
- `tests/diff/cases/semantics/collections/iterator_protocol_exhausted.xr` — selector=`iterator` immediate=108 operands=1
- `tests/diff/cases/semantics/exception/try_catch.xr` — selector=`push` immediate=96 operands=2
- `tests/diff/cases/semantics/functions/unit_return_expression.xr` — selector=`forEach` immediate=24 operands=2
- `tests/diff/cases/semantics/oop/enum_dynamic_read_aggregate.xr` — selector=`push` immediate=96 operands=2
- `tests/diff/cases/semantics/ownership/in_function_param_method_copy_escape_allowed.xr` — selector=`push` immediate=96 operands=2
- `tests/diff/cases/semantics/ownership/loop_ref_array_pop_backedge.xr` — selector=`pop` immediate=98 operands=1
- `tests/regression/11_coroutine/1146_multicore_string_concat.xr` — selector=`push` immediate=96 operands=2

### XI_CALL_METHOD / receiver UNKNOWN — 13 条

selectors: `allocZeroed`×4, `make`, `sqrt`, `crc32`, `endsWith`, `origin`, `isFinite`, `alloc`, `abs`, `escape`

- `tests/diff/cases/semantics/float_fmt/ieee_arithmetic.xr` — selector=`isFinite` immediate=792 operands=2
- `tests/diff/cases/semantics/float_fmt/rounding_boundary.xr` — selector=`sqrt` immediate=124 operands=2
- `tests/diff/cases/semantics/oop/private_constructor_factory.xr` — selector=`make` immediate=792 operands=2
- `tests/diff/cases/semantics/oop/static_methods.xr` — selector=`origin` immediate=800 operands=1
- `tests/diff/cases/semantics/stdlib/buffer_asbytes_write_rejected_shared_core.xr` — selector=`allocZeroed` immediate=792 operands=2
- `tests/diff/cases/semantics/stdlib/compress_checksum_direct.xr` — selector=`crc32` immediate=792 operands=2
- `tests/diff/cases/semantics/stdlib/math_core_direct_type_preserve.xr` — selector=`abs` immediate=122 operands=2
- `tests/diff/cases/semantics/stdlib/mem_alloc_shared_core.xr` — selector=`alloc` immediate=792 operands=2
- `tests/diff/cases/semantics/stdlib/mem_cache_maintenance_shared_core.xr` — selector=`allocZeroed` immediate=792 operands=2
- `tests/diff/cases/semantics/stdlib/mem_load_store_scalar_shared_core.xr` — selector=`allocZeroed` immediate=792 operands=2
- `tests/diff/cases/semantics/stdlib/mem_nontemporal_store_shared_core.xr` — selector=`allocZeroed` immediate=792 operands=2
- `tests/diff/cases/semantics/stdlib/process_schema_shared_core.xr` — selector=`endsWith` immediate=44 operands=2
- `tests/diff/cases/semantics/stdlib/regex_escape_direct.xr` — selector=`escape` immediate=792 operands=2

### XI_CALL_METHOD / receiver MAP — 12 条

selectors: `set`×9, `get`, `keys`, `containsKey`

- `tests/diff/cases/semantics/collections/map_bool_key.xr` — selector=`set` immediate=10 operands=3
- `tests/diff/cases/semantics/collections/map_index_missing_key_panic.xr` — selector=`get` immediate=8 operands=2
- `tests/diff/cases/semantics/collections/map_key_contextual_literal.xr` — selector=`keys` immediate=16 operands=1
- `tests/diff/cases/semantics/collections/map_set_derived_key_fields.xr` — selector=`set` immediate=10 operands=3
- `tests/diff/cases/semantics/collections/value_format_truncation_shared_core.xr` — selector=`set` immediate=10 operands=3
- `tests/diff/cases/semantics/coro/lazy_string_boundary.xr` — selector=`set` immediate=10 operands=3
- `tests/diff/cases/semantics/oop/hashable_contract.xr` — selector=`set` immediate=10 operands=3
- `tests/diff/cases/semantics/ownership/auto_borrow_heap_readonly_param.xr` — selector=`set` immediate=10 operands=3
- `tests/diff/cases/semantics/ownership/copy_deep_independent.xr` — selector=`set` immediate=10 operands=3
- `tests/diff/cases/semantics/ownership/shared_freeze_go.xr` — selector=`set` immediate=10 operands=3
- `tests/diff/cases/semantics/user_hashable_map_set_key.xr` — selector=`containsKey` immediate=462 operands=2
- `tests/regression/11_coroutine/1166_channel_map_deep_copy_stress.xr` — selector=`set` immediate=10 operands=3

### XI_CALL_METHOD / receiver SET — 10 条

selectors: `add`×9, `contains`

- `tests/diff/cases/semantics/collections/bool_conversion_truthy_shared_core.xr` — selector=`add` immediate=148 operands=2
- `tests/diff/cases/semantics/collections/container_print.xr` — selector=`add` immediate=148 operands=2
- `tests/diff/cases/semantics/collections/enum_payload_equality.xr` — selector=`add` immediate=148 operands=2
- `tests/diff/cases/semantics/collections/map_float_key_identity.xr` — selector=`add` immediate=148 operands=2
- `tests/diff/cases/semantics/collections/map_set_static_missing_key_prehashed.xr` — selector=`add` immediate=148 operands=2
- `tests/diff/cases/semantics/collections/type_names_shared_core.xr` — selector=`add` immediate=148 operands=2
- `tests/diff/cases/semantics/coro/task_transfer_return_map_set.xr` — selector=`contains` immediate=40 operands=2
- `tests/diff/cases/semantics/evaluation_order/e7_literal_element_order.xr` — selector=`add` immediate=148 operands=2
- `tests/diff/cases/semantics/generic_container_type_args.xr` — selector=`add` immediate=148 operands=2
- `tests/diff/cases/semantics/generic_iterable_constraint.xr` — selector=`add` immediate=148 operands=2

### XI_GEN_CALL / receiver FUNCTION — 9 条

selectors: `(no selector)`×9

- `tests/diff/cases/semantics/collections/method_self_return_ownership.xr` — selector=`` immediate=0 operands=2
- `tests/diff/cases/semantics/generator/early_stop.xr` — selector=`` immediate=0 operands=2
- `tests/diff/cases/semantics/generator/heap_value_yield.xr` — selector=`` immediate=0 operands=1
- `tests/diff/cases/semantics/generator/iterator_error_beats_exhaustion.xr` — selector=`` immediate=0 operands=1
- `tests/diff/cases/semantics/generator/iterator_exhausted.xr` — selector=`` immediate=0 operands=1
- `tests/diff/cases/semantics/generator/iterator_param_loop_carried.xr` — selector=`` immediate=0 operands=2
- `tests/diff/cases/semantics/generator/nested_generator.xr` — selector=`` immediate=0 operands=2
- `tests/diff/cases/semantics/generator/value_yield.xr` — selector=`` immediate=0 operands=2
- `tests/diff/cases/semantics/generator/yield_throw.xr` — selector=`` immediate=0 operands=1

### XI_CALL_METHOD / receiver CHANNEL — 6 条

selectors: `trySend`×6

- `tests/diff/cases/semantics/coro/channel_nonblocking_prelude_enums.xr` — selector=`trySend` immediate=306 operands=2
- `tests/diff/cases/semantics/coro/poll_try_send_safepoint.xr` — selector=`trySend` immediate=306 operands=2
- `tests/regression/11_coroutine/1101_channel_basic.xr` — selector=`trySend` immediate=306 operands=2
- `tests/regression/11_coroutine/1104_coroutine_combined.xr` — selector=`trySend` immediate=306 operands=2
- `tests/regression/11_coroutine/1141_concurrent_producer_consumer.xr` — selector=`trySend` immediate=306 operands=2
- `tests/regression/11_coroutine/1154_dynamic_channel_capacity.xr` — selector=`trySend` immediate=306 operands=2

### XI_CALL_METHOD / receiver INSTANCE<Task> — 6 条

selectors: `cancel`×4, `poll`×2

- `tests/diff/cases/semantics/coro/defer_runs_on_cancel.xr` — selector=`cancel` immediate=284 operands=1
- `tests/regression/11_coroutine/1109_await_any.xr` — selector=`cancel` immediate=284 operands=1
- `tests/regression/11_coroutine/1115_cancel.xr` — selector=`cancel` immediate=284 operands=1
- `tests/regression/11_coroutine/1116_task_status.xr` — selector=`cancel` immediate=284 operands=1
- `tests/regression/11_coroutine/1132_task_monitor.xr` — selector=`poll` immediate=290 operands=1
- `tests/regression/11_coroutine/1138_get_result.xr` — selector=`poll` immediate=290 operands=1

### XI_CALL_METHOD / receiver INT — 5 条

selectors: `toString`×2, `checkedAdd`×2, `addOverflows`

- `tests/diff/cases/semantics/int/wrapping_fixed_width.xr` — selector=`checkedAdd` immediate=390 operands=2
- `tests/diff/cases/semantics/int_wrap/generic_int_format_shared_core.xr` — selector=`toString` immediate=170 operands=1
- `tests/diff/cases/semantics/int_wrap/int_overflow_predicates.xr` — selector=`addOverflows` immediate=450 operands=2
- `tests/diff/cases/semantics/int_wrap/int_safe_methods.xr` — selector=`checkedAdd` immediate=390 operands=2
- `tests/diff/cases/semantics/int_wrap/numeric_method_shared_core.xr` — selector=`toString` immediate=170 operands=1

### XI_CALL_BUILTIN / receiver ARRAY — 4 条

selectors: `array_copy_new`×2, `array_resize`×2

- `tests/diff/cases/semantics/collections/array_resize_reserve_shared_core.xr` — selector=`array_resize` immediate=0 operands=3
- `tests/diff/cases/semantics/collections/array_typed_fill_storage_shared_core.xr` — selector=`array_resize` immediate=0 operands=3
- `tests/diff/cases/semantics/collections/byte_array_bool_short_circuit_liveness_shared_core.xr` — selector=`array_copy_new` immediate=12 operands=1
- `tests/diff/cases/semantics/collections/byte_array_constructor_shared_core.xr` — selector=`array_copy_new` immediate=12 operands=1

### XI_CALL_METHOD / receiver INSTANCE<Range> — 4 条

selectors: `contains`×2, `toArray`, `toString`

- `tests/diff/cases/basic/range_precedence.xr` — selector=`toString` immediate=170 operands=1
- `tests/diff/cases/basic/ranges.xr` — selector=`contains` immediate=40 operands=2
- `tests/diff/cases/semantics/collections/range_to_array_too_large_shared_core.xr` — selector=`toArray` immediate=158 operands=1
- `tests/diff/cases/semantics/range_inclusive.xr` — selector=`contains` immediate=40 operands=2

### XI_CALL / receiver CLASS<Atomic> — 3 条

selectors: `(no selector)`×3

- `tests/diff/cases/semantics/concurrency/atomic_rawptr.xr` — selector=`` immediate=0 operands=2
- `tests/diff/cases/semantics/generator/generator_in_coroutine.xr` — selector=`` immediate=0 operands=2
- `tests/regression/11_coroutine/1120_scope_basic.xr` — selector=`` immediate=0 operands=2

### XI_CALL / receiver UNKNOWN — 3 条

selectors: `(no selector)`×3

- `tests/diff/cases/semantics/coro/cycle_leak_bounded_by_coroutine_heap.xr` — selector=`` immediate=0 operands=2
- `tests/diff/cases/semantics/ownership/in_readonly_user_method_names.xr` — selector=`` immediate=0 operands=2
- `tests/diff/cases/semantics/stdlib/native_tailcall_cfunction_direct.xr` — selector=`` immediate=0 operands=2

### XI_CALL_BUILTIN / receiver INSTANCE<Box> — 1 条

selectors: `copy`

- `tests/diff/cases/semantics/ownership/copy_preserves_type.xr` — selector=`copy` immediate=0 operands=1

### XI_CALL_BUILTIN / receiver INSTANCE<Config> — 1 条

selectors: `copy`

- `tests/diff/cases/semantics/ownership/shared_class_copy_go.xr` — selector=`copy` immediate=0 operands=1

### XI_CALL_BUILTIN / receiver NO-RECEIVER — 1 条

selectors: `(no selector)`

- `tests/regression/11_coroutine/1100_cancelled.xr` — selector=`` immediate=0 operands=0

### XI_CALL_METHOD / receiver CLASS<String> — 1 条

selectors: `fromUtf8`

- `tests/diff/cases/semantics/slice/string_from_utf8_slice.xr` — selector=`fromUtf8` immediate=476 operands=2

### XI_CALL_METHOD / receiver ENUM<Color> — 1 条

selectors: `toString`

- `tests/diff/cases/semantics/oop/enum_value_aot_identity.xr` — selector=`toString` immediate=170 operands=1

### XI_CALL_METHOD / receiver ENUM<E> — 1 条

selectors: `toString`

- `tests/diff/cases/match/adt_enum_payload_tostring.xr` — selector=`toString` immediate=170 operands=1

### XI_CALL_METHOD / receiver RUNE — 1 条

selectors: `toString`

- `tests/diff/cases/semantics/collections/span_fill_pod_shared_core.xr` — selector=`toString` immediate=170 operands=1

### XI_CALL_METHOD / receiver STRING — 1 条

selectors: `indexOf`

- `tests/diff/cases/semantics/string/string_indexof_start_shared_core.xr` — selector=`indexOf` immediate=38 operands=2

## Is this one gap or many

**是 21 个缺口，不是 1 个；但它们分属 4 类修法，不是 21 类。**

按修法归类（这是给 5 号排期用的）：

1. **内建容器方法家族**（组 3/4/5/11 = 40 条，Array/Map/Set 的 `push`/`set`/`add`/…）——
   目标层缺 `*_is_exact` + `collect_*_call_intent`。同一套写法可复用。
2. **模块命名空间调用**（组 2/12 = 16 条，receiver 为 `UNKNOWN`）——需要命名空间解析，与 1 不同源。
3. **语义层结构性排除**（组 6/11/14/17/18 = 16 条，`XI_GEN_CALL` + `XI_CALL_BUILTIN`）——
   `xr_semantic_builder.c:3655` 就把这两个 opcode 排除了，只能由目标层家族认领。
4. **其余具体类型的方法**（组 1/7/8/9/10/13/15/16/19/20/21 = 42 条，JSON / Task / Channel / Int / Range /
   Atomic / String / Rune / Enum）——每类一个家族，彼此独立。

**"缺一个能力"而不是"多层判据不一致"的代码证据：**

这些家族的识别器在 `src/plan/` 全树**一个都不存在**。对 21 组涉及的选择器逐一 grep：

```
$ grep -rhoE "[a-z_]*_is_exact" src/plan/ | sort -u | grep -iE "push|pop|shift|sort|send|cancel|poll|json|gen|map|task|contains|add|set"
semantic_direct_local_scalar_ref_address_is_exact
semantic_json_namespace_type_is_exact
semantic_json_namespace_value_is_exact
semantic_map_entries_iterator_is_exact
semantic_map_entry_iterator_common_is_exact
semantic_map_entry_iterator_has_next_is_exact
semantic_map_entry_iterator_next_is_exact
xi_map_entries_iterator_is_exact
xi_map_entry_iterator_has_next_is_exact
xi_map_entry_iterator_next_is_exact
xr_semantic_await_task_operand_is_exact
xr_semantic_direct_local_go_task_result_is_exact
xr_semantic_iterator_rune_*_is_exact  (4 个)
xr_semantic_local_addr_is_exact
xr_semantic_map_entries_iterator_is_exact
xr_semantic_map_entry_iterator_*_is_exact  (3 个)
xr_semantic_ref_argument_local_addr_is_exact
xr_semantic_task_type_is_exact
```

只有 map **entry iterator** 一族存在；`Array.push`、`Map.set`、`Set.add`、`Channel.trySend`、
`Task.cancel`、`JSON.parseObject`、`XI_GEN_CALL` 全部**零实现**。零实现不能"抽取等价"，只能新增。

**7 号确实能在这里找到重复，但那份重复解不开任何一条用例。**

`semantic_operation_is_call_shaped`（`src/plan/target/xr_target_builder.c:8914`）和
`operation_is_call_shaped`（`src/plan/target/xr_target_verify.c:5053`）是一对**逐字重复**：
同样 9 个 opcode（`XI_CALL` / `XI_CALL_METHOD` / `XI_CALL_METHOD_DIRECT` / `XI_TAIL_CALL` /
`XI_CALL_BUILTIN` / `XI_ATOMIC_TO_STRING` / `XI_EXTRACT` / `XI_GEN_CALL` / `XI_MULTI_RET`），
同样的 operand-role 兜底。把两份正规化后逐行 diff，差异只有注释和形参名：

```
$ sed -n '8916,8945p' src/plan/target/xr_target_builder.c | sed 's/plan/PLAN/g' > a
$ sed -n '5055,5084p' src/plan/target/xr_target_verify.c  | sed 's/semantic/PLAN/g' > b
$ diff a b
2,3c2
<         /* Numeric SemanticPlan operation identity is explicit authority here;
<          * do not reclassify from effects, names, or a generated backend class. */
---
>         /* Keep the independent boundary on exact PLAN op identities. */
（其余仅函数名/形参名差异）
```

两点结论：

- **两份完全一致**，所以这 114 条的拒绝不是"两层判据打架"造成的，抽取合并后一条也解不开。
- 而且这份重复是**有意为之**，不该被无条件合并：`src/plan/target/xr_target_verify.c:1105-1107` 写明
  "Independent reconstruction ... Keep this separate from the builder so a malformed key or lifecycle row
  cannot pass because construction and verification shared one classifier"，验证侧还有一整批
  `*_is_exact_verify` / `operation_is_exact_*` 同源副本（`xr_target_verify.c` 有 71 个不同的
  `*_is_exact` 谓词、190 处调用）。**7 号若要合并这一对，需要先和该设计约束对齐**，这属于独立议题，
  与本单 114 条无因果关系。

`src/plan/semantic/xr_semantic_verify.c` 里**没有**第三份 `is_call_shaped`（grep 全树只有上述两处）。

## Requested capability

给 5 号，按 ROI 排序：

1. **组 6（`XI_GEN_CALL`，9 条）** 最先做：9 条同源、全在 `tests/diff/cases/semantics/generator/`，
   一个家族识别器应能一次解开，且是验证"新增家族"这条路走得通的最小实验。
2. **组 3/4/5/11（容器方法，40 条）**：一套写法覆盖最多用例。
3. **组 1（JSON 命名空间，18 条）**：已有 `semantic_json_namespace_value_is_exact` 可作模板扩展。
4. **组 2/12（模块命名空间，16 条）**：需要另一种解析路径，建议单独立项。

新增家族需要同时满足两处：`xr_target_builder.c:11180-11265` 的识别链（新增 `else if` 分支 +
`collect_*_call_intent`），以及 `xr_target_verify.c:7891` 的覆盖重扫——后者由 `covered[]` 自动满足，
**不需要同步第二张允许列表**。

调试入口：`XRAY_TARGET_TRACE=1` 打印每条判据的走向，`XRAY_COLLECT_ALL_REFUSALS=1` 一次收齐整模块的拒绝。

## Files deliberately not modified

本单只新增本文件。以下**未改动**：

- `src/plan/target/xr_target_builder.c` — call 族段是 5 号的允许文件。
- `src/plan/target/xr_target_verify.c`、`src/plan/semantic/xr_semantic_builder.c`、
  `src/plan/semantic/xr_semantic_verify.c` — 跨层判据去重是 7 号的范围。
- `tests/diff/known_failures_not_comparable.txt` — 这 114 条的首次拒绝已由本 lane 的普查写入行内注释；
  在 5 号交付前不应删除任何一行。
- `blockers/r3-1-diff-net-multi-module-program-authority.md`、
  `blockers/r3-1-embed-lane-stdlib-module-not-bundled.md`、
  `blockers/r3-1-diff-net-refusal-census.md` — 由本 lane 其它 agent 拥有。

# 第三轮 1 号 / 差分网解封：515 条不可比用例的逐条复验普查

- **Lane**: 1（差分网解封）
- **Status**: `HANDOFF`（普查完成；解封 23 条；余下 535 条的首次拒绝已逐条登记）
- **冻结起点**: `xray@00f665c5c`，tree `afe293b71`
- **分支**: `work/1-diff-net-unseal-00f665c5c`

本文是普查报告，不是阻塞单。三份阻塞单单列在 `blockers/r3-1-*.md`。

---

## 1. 一句话结论

差分网 78% 悬空**不是登记错误，而是 AOT 原生线真的还没到**：515 条里只有 5 条已可解封，
510 条仍被拒绝，且拒绝集中在 **43 个**互不相同的首次拒绝上，最大的一族一口气压住 114 条。

因此本轮的可交付价值不在"删了多少行"，而在**把 558 条拒绝从"要重建整棵树才能知道卡在哪"
变成"读一行注释就知道卡在哪"**，并由此第一次得到了一张按缺陷排序的工作队列。

---

## 2. 测量身份

| item | value |
|---|---|
| base commit | `00f665c5c`，tree `afe293b71` |
| worker branch | `work/1-diff-net-unseal-00f665c5c` |
| binary | `build-nofp/xray` |
| build configuration | `cmake -S . -B build-nofp -G Ninja -DCMAKE_BUILD_TYPE=Release -DXRAY_STDLIB_VM_FASTPATHS=OFF -DXRAY_PYTHON=/opt/homebrew/bin/python3` |
| runner | `tests/diff/run_backend_diff.py` 的 `RunnerConfig` + `run_case`（同 ctest lane 的同一份代码） |
| lane 参数 | `XRAY_AOT_TEST_OPT=0`、`xi_opt` 为空、`XRAY_TOOLCHAIN_PROBE_SCALE=4`、`case_timeout=180s` |
| 逐条原始数据 | `/tmp/r3-lane1-refusal-baseline.jsonl`（1030 行 = 515 用例 × aot/vm 各一条） |

**零探针伪造**：同一族失效模式还有第二个出口——`native-run` 探针在负载下 10s 超时，
把真实拒绝改写成 `no provider reached READY`。对全部 558 条日志检索
`no provider reached READY` / `READY`：**0 命中**（两条含 `probe` / `toolchain` 字样的
是用例名 `probe_module_shapes` 和一条 ICE 里的 "C toolchain"，不是探针事件）。

**零伪造拒绝**：`blockers/b-probe-timeout-fabricates-refusals.md` 记录过一种失效模式——
探针在负载下超时，整条 lane 的每个用例都被报成"构建拒绝"，与真能力拒绝无法区分。
本次复验对此做了显式检查：510 + 48 条拒绝的完整构建日志里，
**`timed out` 与 `cannot lock binary cache` 各 0 条**，每一条都带一个具体诊断。
本机在测量期间确实同时跑着其它 lane 的 `backend_diff`（负载很高），这条检查因此不是形式主义。

**但在负载起来之后，同一台机器已经不能再这样测了。** 复验结束后机器 load 冲到 50
（十条 lane 各自在跑全量），此时 `tests/diff/cases/semantics/modules/value_struct_arg_alias_import.xr`
**串行**连跑三次给出两个不同答案：

```
第 1 次  Error: ... XR_TARGET_1000: product TargetPlan requires one canonical program authority
第 2 次  Error: no provider reached READY for target 'aarch64-apple-darwin'
第 3 次  Error: no provider reached READY for target 'aarch64-apple-darwin'
```

这比超时危险：`***Timeout` 一眼就知道不可信，而 `no provider reached READY`
**长得像一条正常的能力诊断**，会被直接记进失败清单和归因表。
`XRAY_TOOLCHAIN_PROBE_SCALE` 在 ctest 的两条 diff lane 里都只设到 4；
在这种负载下要抵抗它得调到 16。

**第三种伪造拒绝，比前两种更阴**：kill 掉一个 diff lane 会在缓存里留下未释放的目录锁。
`DirLock` 用 `mkdir` 做原子锁（`tests/lib/xraytest/cache.py:218`），被 kill 的进程走不到
`release()`（`:229`），锁目录就留下了；下一次运行在每个残留锁上先卡
`XRAY_TEST_LOCK_TIMEOUT` 默认 **3000 ticks = 300 秒**（`:190-196`），
超时后 `build_case_binary` 返回 `cannot lock binary cache`
（`tests/diff/run_backend_diff.py:187`），rc=200 → **该用例被记成 REFUSED**。
它既不超时也不报诊断，就是安静地多出一条"这个用例不能构建"。

本 lane 实际踩到：pkill 掉超时的 `backend_diff_embedded` 后重跑，13 个残留锁让 runner
空转三分半。

清理有**两条**限定，缺一条都会出事。先看，再删：

```bash
find .cache/xray-test -name "*.lock" -type d -exec ls -ld {} \;
```

```bash
find .cache/xray-test -name "*.lock" -type d \
     -not -newermt "<你上次启动测试的时刻>" -exec rmdir {} \;
```

- `-type d`：`DirLock` 的锁是**目录**；同一片缓存下的
  `aot-objects/<tag>/O0/aot/<triple>/.cache-root.lock` 是**普通文件**，不该一起删。
- `-not -newermt <时刻>`：**只清死锁**。`rmdir` 删得掉活锁（`DirLock` 建的是空目录），
  删掉就等于拆掉正在跑的测试的互斥保护，两个进程会同时构建同一个二进制。
  10 号在自己的树上实测：11 个锁里只有 3 个是死的，无限定的命令会连删 8 个活锁。

**只清自己 worktree 的，且只清自己的死的。**

**归因纪律**：凡是 kill 过 diff lane 的树，重跑前必须先清锁；重跑结果里出现
`cannot lock binary cache` 的用例一律不可归因。

**本普查的数据在负载起来之前、任何 kill 之前就跑完了**（三项检索全 0 命中，见上），
但重跑的人必须先读这一段。

`FASTPATHS=OFF` 不是可选项：默认配置在这个基线上构建不完（`XR_TARGET_1000`，4 号的目标）。
**因此本普查的全部读数都是 OFF 档读数**，这一点已写进两份清单的文件头。

---

## 3. native lane（vm / aot）：515 → 510

| 结果 | 条数 |
| --- | --- |
| 仍被 AOT 拒绝 | **510** |
| 已可解封（两端构建成功且字节一致） | **5** |

解封的 5 条已从 `tests/diff/known_failures_not_comparable.txt` 删除：

```
tests/diff/cases/semantics/exception/force_unwrap_error_shape.xr
tests/diff/cases/semantics/object/object_width_nullable_field_omission.xr
tests/diff/cases/semantics/optimizer/pass_ifconv_select_shape.xr
tests/diff/cases/semantics/stdlib/mem_from_address_shared_core.xr
tests/diff/cases/semantics/types/object_optional_sparse_fields.xr
```

### 3.1 按首次拒绝排序的工作队列

43 个首次拒绝，前 11 族覆盖 441 条（86%）：

| 条数 | 首次拒绝 | 归属子系统 |
| ---: | --- | --- |
| 114 | `XR_TARGET_1003` call-shaped operation has no exact target authority | target plan builder |
| 90 | `XR_TARGET_1000` product TargetPlan requires one canonical program authority | program semantic closure（**4 号**） |
| 67 | `XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE` | AOT representation refinement |
| 45 | `XR_SEM_0019` coroutine state count disagrees with grounded call authority | semantic plan verifier |
| 35 | `XR_TARGET_1003` direct-local argument contract needs unsupported storage or ownership | target plan builder |
| 24 | `XR_TARGET_1003` direct-local signature or result storage is incomplete | target plan builder |
| 18 | `XR_TARGET_1003` call target has no consumable adapter authority | target plan builder |
| 15 | `XR_TARGET_1003` call result or argument partition cannot bind canonical storage | target plan builder |
| 13 | coroutine lowering failed closed before publishing CoroLowered | coroutine lowering |
| 12 | `XR_AOT_REFINEMENT_INCOMPLETE_COVERAGE` | AOT representation refinement |
| 8 | `XR_TARGET_1003` call argument lacks exact caller/callee storage | target plan builder |

余下 32 族共 69 条，每族 1–6 条。

### 3.1.1 最大那族 114 条不是一个缺口，是 21 个

写阻塞单时把这 114 条按**首次拒绝消息里的机器操作数**再切了一刀。`selector=` 单看是
42 个值的长尾、没有结构；真正的分组轴是 **opcode × receiver 顶层类型**
（opcode 对 `src/ir/xi.h` 的 `XiOp` 枚举、receiver kind 对 `src/runtime/value/xtype.h`
的 `XR_KIND_*` 逐一核对过），切出 21 组：

| opcode / receiver | 条数 |
| --- | ---: |
| `XI_CALL_METHOD` / `CLASS<JSON>` | 18 |
| `XI_CALL_METHOD` / `UNKNOWN`（模块命名空间调用） | 13 |
| `XI_CALL_METHOD` / `ARRAY` | 13 |
| `XI_CALL_METHOD` / `MAP` | 12 |
| `XI_CALL_METHOD` / `SET` | 10 |
| `XI_GEN_CALL` / `FUNCTION`（生成器 resume，全在 `semantics/generator/`） | 9 |
| `XI_CALL_METHOD` / `INSTANCE<Task>` | 6 |
| `XI_CALL_METHOD` / `CHANNEL` | 6 |
| 其余 13 组 | 27 |

`intrinsic=` 114/114 全为 0，不是分组轴。

这一刀的意义：**"最大的一族 114 条"读起来像一个能力，实际是 21 个**，其中最大的一个
（JSON 方法调用）18 条。同时它也给出两条可一次性收割的线：9 条生成器 resume 全在
`semantics/generator/` 一个目录下（那个目录的可比数正好是 0），
13 + 3 = 16 条 receiver 是裸 `UNKNOWN` 的模块命名空间调用（selector 是 `sqrt`/`abs`/
`allocZeroed` 这类），与容器方法不同源、修法也不同。

**关于 `XR_TARGET_1000` 那 90 条的一个修正**（在写阻塞单时实测出来的，改变了它的工作量估计）：
这一族**不是"多模块工程"**。90 条全部有 import，但按"用户写了几个 `.xr` 文件"这个真口径，
**67 条（74%）是单个用户文件，只 import 了标准库模块**。原因是带 `.xr` 源的标准库模块会进模块图，
所以"单文件 + 一条 `import os`"直接就是 `nmodules == 2`，撞上
`src/aot/xaot_driver.c:2554` 的 `if (!source_program_closure && nmodules != 1)`。
纯原生模块（如 `mem`）没有 `.xr` 源、不进图、不触发。

推论有两条。其一，这条 guard 的实际形状是"任何 import 了带源码标准库模块的程序"，
主战场在单文件而不在多模块工程。其二，**随着 6 号继续把 stdlib 模块迁成 `.xr` 源，
撞上这条 guard 的程序会持续增加**——它不是一个静态的 90。

### 3.2 一个必须先说清楚的方法学事实：光按诊断码聚类没有分辨率

`XR_TARGET_1003` 在 `src/plan/target/` 下有 **185 条互不相关的消息文本**
（`xr_target_builder.c` 122 处、`xr_target_verify.c` 28 处、`xr_target_instruction_verify.c` 11 处、
`xr_target_plan.c` 1 处，另有 5 个 `reject`/`fail` helper 经 `%s detail` 间接发出 23 处）。
而 `contracts/target-machine/diagnostic-codes.toml:153-155` 注册的语义是
`invalid-call-fingerprint`，真正谈 fingerprint 的只有 2 条。

**只按码聚类会把 185 个互不相关的成因塌缩成一个桶**。本普查按「码 + 剥掉机器操作数后的消息」
聚类，这是 `XR_TARGET_1003` 能分出 6 个不同规模族的原因。

同一条也适用于 `XR_TARGET_1000`：注册语义是 `profile-fingerprint-mismatch`
（`diagnostic-codes.toml:138-140`），而实际发出的消息是
`product TargetPlan requires one canonical program authority`，两者不是一回事。

### 3.3 契约缺口：`XR_AOT_REFINEMENT_*` 根本不是注册诊断码

它是 `src/aot/refine/xr_aot_refinement.h` 里的 C 枚举，没有 `message` 字段，
但枚举名会以裸文本流进诊断输出——本普查里 67 + 12 + 3 = **82 条**走的就是它。

它**没有经过 `diagnostic-codes.toml` 的 `unknown_code = "error"` 约束**。
一个占了 16% 拒绝面的标识族，在诊断治理之外。

**已裁决（10 号，2026-08-28）：注册进 `diagnostic-codes.toml` 分配号段，不走
`XR_TARGET_1006` 包裹。** 决定性理由是本文 §3.2 那条实测的直接推论——
`XR_TARGET_1003` 已经被 185 条互不相关的消息证明"按码聚类没有分辨率"，
**把这 82 条包进 `XR_TARGET_1006` 就是在制造第 186 条**，方向正好相反；
而注册让它们从此受 `[policy] unknown_code = "error"` 治理。
执行要动 `diagnostic-codes.toml` 与 `src/aot/refine/xr_aot_refinement.h`，
超出本 lane 的 `tests/diff/` 范围，记为独立工作项。

**但裁决要落地还差一个条件，写阻塞单时才挖出来**：
`unknown_code = "error"` **在 AOT 上从未生效过**。执行它的
`scripts/target_machine_phase0.py:601-625` 的 `unregistered_emitted_codes()` 是**闭世界**扫描——
它的正则 alternation 只由 toml 已注册的 6 个族前缀拼出，`XR_AOT_` 从不在内，
而且要求前缀后紧跟 4 位数字，这些枚举名根本没有编号。
所以不是这一族溜过去了，是**闸看不见它这一类**：实测把该函数原样跑一遍，
它在 `src/` 里只看得见 54 个码、全部已注册、**闸是绿的**，
同时 28 个枚举名正流向用户终端。

规模也不止一族。AOT 下有**三族共 54 个裸标识符**在注册表之外
（`XrAotRefinementIssue` 28 + `XaotBackendContractIssue` 15 + `XrAotTailCallConformanceIssue` 11），
而 toml 注册总数是 55——**一套和注册表等体量的影子诊断词表**。
更糟的是 `contracts/target-machine/baseline-manifest.json:184` 已经在拿其中一个枚举名当
finding 标识，**契约语料已经在依赖一个没有任何东西保证其稳定的名字**。

因此建议把"phase0 改成开世界扫描"并进那个工作项的验收条件：
不改它，注册完这一族之后，下一族裸标识符会以同样方式再长出来，而且照样没人看得见。

**一条待坐实的线索**：10 号在
`xray-docs/analysis/parallel-round-3-integration-decisions-2026-08-28-cn.md` §5.1 记录了
`representation_recipe`（`xr_aot_refinement.c:367-395`）与 `oracle_representation_recipe`
（`xr_aot_representation_refinement.c:8180-8205`）**已实际漂移**：
`XR_MACHINE_REP_OBJECT_REF` 在前者映射到 `BOX/UNBOX_REFERENCE`，在后者落进 `default → NONE`，
生产方接受、验证方在 `:11818` 直接拒 `XR_AOT_REFINEMENT_REPRESENTATION`。
他标注"疑与 `SCHEMA_UNAVAILABLE` 那一族现存失败有关，待坐实"。

本 lane 尝试用操作数坐实，**结论是操作数坐实不了**：67 条的 `operation=` 有 36 个不同值、
`value=` 有 37 个，最大的一组只有 4 条。原因是 `rep_trace_refusal()` 自己印明的：
`operation=` 是**使用点的语义 operation 索引**，不是 opcode、不是定义点，
trace 甚至专门印一行 "the two indexes are unrelated" 防误读；
那 12 条整齐的 `4294967295` 是 `XR_SEMANTIC_INDEX_NONE` 哨兵。
36 只是"67 个程序里第一个炸掉的语句各自排第几"。

**换用仓库已有的两个开关（`XRAY_AOT_REFINE_TRACE` / `XRAY_COLLECT_ALL_REFUSALS`，
没为取数改过一行代码）测出了真正的轴**：67 条构建期用例共 **614 个被拒 operand**，
其中 **613 条的 `required rep` 是 `unnamed`**——pass 绝大多数时候根本没走到"选 representation"
那一步。分组单位是 **(拒绝阶段, opcode 分支)**，共 **55 个**，长尾极平：
贪心最优也要 **26 个分支才过半**（39/67），单个分支最高只能独立清零 2 条。
**这一族既不是一个缺口，也不是几十个高价值缺口**——它是 55 个各值 1–2 条的分支。
细节与 §5.1 的下一步都在 `blockers/r3-1-aot-refinement-family-ungoverned.md`。

### 3.4 51 条不是 AOT 覆盖缺口

复验时另跑了一轮 **VM 单独**探测。515 条里 **51 条 VM 也在编译期就拒**，
其中相当一部分两侧同码同文（例如 `barrier_compose.xr` 两边都是
`XR_SEM_0019: coroutine state count disagrees with grounded call authority`）。

**这些用例当前在两个后端上都跑不了**，为它们去找 AOT target authority 是找错层。
清单里已用 `vm too:` 前缀标出，这是本次注释最直接省掉的一次误工。

### 3.4.1 另外 12 条：VM 跑到未捕获 panic，而且没有任何断言拦着

另有 12 条 VM 退出码非 0 但不是编译拒绝，全部是 `[Uncaught Panic]` / `[Uncaught Error]`。
它们属于可比范围，未加 `vm too:` 标记。但**这 12 条一条都没有 `.xr.expected`**：

```
byte_array_range_arg_shared_core.xr      E0430: Slice<u8>.load<u32>() offset out of bounds
slice_int_arg_shared_core.xr             E0471: JSON.require: value does not match the requested type
uncaught_top_level_error.xr              TopErr.Failed("top-level")
cleanup_init_error.xr                    E0403: attempt to call a non-function value
cleanup_init_panic.xr                    E0420: division by zero
buffer_asbytes_write_rejected_shared_core.xr  E0303: cannot write through readonly Slice
http_request_message_pure_direct.xr      Utf8Error.InvalidUtf8
http_response_text_pure_direct.xr        Utf8Error.InvalidUtf8
math_core_direct_type_preserve.xr        E0404: TypeError: expected 'i64', got 'f64'
math_random_int_arg_shared_core.xr       E0471: JSON.require: value does not match the requested type
process_schema_shared_core.xr            E0430: array index out of range: 0 (length 0)
range_generic_format_shared_core.xr      E0404: operator '+' requires both operands numeric or both string
```

有几条显然是**用例要测的东西本身失败了**——`math_core_direct_type_preserve.xr` 叫
"type preserve"，跑出来是 `expected 'i64', got 'f64'`；`range_generic_format_shared_core.xr`
在格式化一个 `Range` 时报 `operator '+'` 类型错。另外几条（`uncaught_top_level_error.xr`）
则显然是有意的。

**没有 `.xr.expected` 意味着差分网分辨不了这两者**：它只问"两个后端是否给出相同字节"，
所以即使这 12 条将来解封，只要 AOT 用同样的方式崩，它们照样 PASS。
这是差分网的固有边界，不是本 lane 能修的，但登记在这里，因为下一个人看到
"这 12 条解封后全绿"时应该知道那句绿话说的不是"它们对了"。

### 3.5 按用例类别的覆盖面：四类仍然可比数为零

`backend_diff` 实际发现 676 个用例（`tests/diff/cases/**` 减去 `liveness/`，加
`coro_regression_cases.txt` 的 66 条）。本次改动后 510 条悬空、**166 条可比**（改动前 161）。

| 类别 | 总数 | 悬空 | 可比 |
| --- | ---: | ---: | ---: |
| semantics/stdlib | 130 | 118 | 12 |
| regression（coro manifest） | 66 | 61 | 5 |
| semantics/collections | 59 | 52 | 7 |
| **semantics/optimizer** | 50 | 4 | **46** |
| **semantics/oop** | 39 | 39 | **0** |
| semantics/ownership | 32 | 27 | 5 |
| semantics/coro | 29 | 28 | 1 |
| semantics/ffi | 27 | 17 | 10 |
| semantics/modules | 25 | 21 | 4 |
| semantics/exception | 22 | 10 | 12 |
| **semantics/generator** | 9 | 9 | **0** |
| **semantics/concurrency** | 8 | 8 | **0** |
| **semantics/closure** | 4 | 4 | **0** |
| 其余 28 类 | 186 | 112 | 74 |
| **合计** | **676** | **510** | **166** |

测试质量审计的那条结论**在本轮基线上仍然成立**：oop / generator / concurrency / closure
四类的可比数依旧是 0，四类共 60 个用例一个都没进过比对。本次解封的 5 条全部落在
exception / object / optimizer / stdlib / types，没有触及这四类。

### 3.6 有绝对断言的只有 176 条，其中 74 条是可比的

`tests/diff/cases/**` 的 641 个用例里，**176 条**带 `.xr.expected`（一份绝对 stdout 预言），
其余 465 条只有"两个后端字节一致"这一条判据。按本次复验的悬空集合切开：

| | 有 `.xr.expected` | 无 |
| --- | ---: | ---: |
| 可比 | **74** | 92 |
| 悬空 | 102 | 373 |

**74** 才是差分网今天真正有强度的部分：既跑到了比对，又有一个不依赖另一个后端的预言。
另外 92 条可比用例只能保证"两边一样"，两边一起错就一起过（§3.4.1 给了实例）。

反过来最健康的是 `semantics/optimizer`：50 个用例只有 4 条悬空。它是唯一一个
"差分网真的在守着"的类别，也说明这套网本身没问题——问题在 AOT 原生线的覆盖面。

### 3.7 顺手复验了另外 166 条：4 条已经掉出网外，没人登记

既然 510 条的图景清楚了，把**不在清单里的 166 条**也跑了一遍（同一 runner、同一配置）：

```
161 pass    1 skip    4 refused
```

那 4 条不在任何清单里，却已经不构建了。它们的首次拒绝**全部**是
`XR_TARGET_1000: product TargetPlan requires one canonical program authority`：

```
tests/diff/cases/semantics/modules/value_struct_copy_without_type_import.xr
tests/diff/cases/semantics/modules/value_struct_arg_without_type_import.xr
tests/diff/cases/semantics/modules/value_struct_arg_alias_import.xr
tests/diff/cases/semantics/stdlib/time_query_system_direct.xr
```

按棘轮规则这会触发
`=== Cases that stopped building (not in known_failures_not_comparable.txt) ===`，
**`backend_diff` 因此在冻结基线上就是红的**——本 lane 没有碰过这四条中的任何一条。

`time_query_system_direct.xr` 值得单独看一眼：它是 §3.1.1 那条推论的现场证据。
A lane 把 `time` 迁成 `.xr` 源，这个用例就从"单模块"变成"两模块"，撞上同一条 guard。
**stdlib 自举每往前一步，AOT 的覆盖缺口就往外扩一点**，而扩出去的部分不在任何清单里。

**按纪律不把这四条加进清单。** 规则明写"不许为让改动变绿而加行"，而且加进去等于把
"stdlib 迁移正在扩大 AOT 覆盖缺口"这条事实盖住。它们属于 4 号的 `XR_TARGET_1000`。

---

## 4. embedded lane（vm / embed）：68 → 48

| 结果 | 条数 |
| --- | --- |
| 仍拒绝构建 | **48** |
| 已可解封 | **18** |
| **能构建但两端输出分歧（真分歧）** | **2** |

那 2 条真分歧稳定复现 3/3，单列为 `blockers/r3-1-embed-lane-stdlib-module-not-bundled.md`。
**这正是差分网存在的理由**：它们被登记成"不可比"埋了下去，一解封就露出来。

48 条的首次拒绝只有 9 族，且 44 条是 `vm too:` ——
说明 embed 形式共享 VM 前端，它自己的边界（`bytecode bundling failed`）只占 4 条。

### 4.1 追这 2 条真分歧追出来的东西比它们本身大得多

阻塞单里做的定位（发出点 `src/module/xmodule.c:1318`，**运行期**不是构建期；根因在
`src/module/xbundle.c:162` 把标准库条目名写成身份串
`stdlib-module-v1:module=4:time:path=12:time/time.xr` 而不是纯名 `time`）带出三条
必须记在这里的事实：

1. **不是 `time` 专属**。8 个带 `.xr` 源层的标准库模块（`time` `io` `os` `sys` `text`
   `base64` `log` `path`）用两行最小程序 8/8 复现。
2. **embed lane 在冻结基线上就已经大面积红**。649 条差分用例里 97 条 import 标准库，
   其中 **86 条在 `00f665c5c` 的封存清单里从未出现过**，且现在以同一条消息失败。
   本 lane 的解封把它从 86 变成 88，不是从 0 变成 2。
3. **A lane 迁 `time` 不是引入者**。bundle 改成现在形态是 `80c3733ca`（08-08），
   而 `io/os/sys/text/base64/log/path` 的 `.xr` 层最晚 08-02 就在了——
   这条缺陷从 08-08 起一直在漏。纯原生模块会在 `xmodule_graph.c:310-319` 被整个
   丢出依赖图从而绕过这条路径，所以给一个模块加 `.xr` 层，等于把它从豁免区挪进受害区。

同一份代码在 `xmodule_graph.c:823-826` 的 preload 路径上是**做对了**的（正确还原成
`namespace_id`），bundle 路径丢了这层映射——又是一处"同一事实两处实现、只改一侧就漂移"。

---

## 5. 三份阻塞单

| 文件 | Requested owner | 覆盖条数 |
| --- | --- | --- |
| `blockers/r3-1-call-shaped-no-exact-target-authority.md` | 5（缺能力，非等价抽取） | 114（切成 21 组） |
| `blockers/r3-1-diff-net-multi-module-program-authority.md` | 4（多模块 program authority） | 90（其中 67 条是单文件） |
| `blockers/r3-1-aot-refinement-family-ungoverned.md` | 10（治理裁决）+ 实现方 | 82 |
| `blockers/r3-1-embed-lane-stdlib-module-not-bundled.md` | H（compiler / module loader） | 88（含 2 条真分歧） |

四份合计直接覆盖 374 条 native 悬空（占 73%）。

**一条给 7 号的结论已单独同步给他**：`call-shaped` 那 114 条**不是**跨层判据重复，
是能力零实现，抽取解决不了。他那条 lane 在这一族里确实能找到一对逐字重复
（`semantic_operation_is_call_shaped` @ `xr_target_builder.c:8914` 与
`operation_is_call_shaped` @ `xr_target_verify.c:5053`），但两份完全一致、
抽了一条用例也解不开，而且 `xr_target_verify.c:1105-1107` 明写这是刻意的独立重建。
建议把它排除在等价抽取的靶子之外并写明理由——否则下一个人会重新发现它并想抽。

---

## 6. 下游影响（改动这两份清单必然波及，接手方需知）

1. **`scripts/check_live_refusal_manifest.py:131`** 把这两份清单的 sha256 + size 写进 manifest。
   本次改动使已生成的 live refusal manifest 失效，需重跑 `tests/diff/survey_refusals.py` 重生成。
2. **`analysis/a-stdlib-public-native-migration.{json,md}`** 有三处按**行号**引用
   `known_failures_not_comparable.txt:426`。本次重排注释块使这三处指向错误的用例，需要重指。
3. **`contracts/differential-protocol.md:42-43`** 硬编码 `564 refusals` / `68`。
   native 侧实际值在本次改动前已是 515、改动后是 510；embedded 侧从 68 变 48。
   该文本记的是 2026-08-16 的历史值，读起来却像活断言，建议改成"当时"或直接删数字。

---

## 7. 本 lane 未做、且明确不该做的事

- **没有给任何用例加 `// diff-backends: vm` 让它退出清单**。
  那会让用例变成 SKIP，既不比对也不再登记——是把覆盖缺口改写成看不见，不是解封。
- **没有把 2 条 embedded 真分歧写进 `known_failures_embedded.txt`**。
  那份文件自己写着 `This file is deliberately empty`，政策是让这类失败保持可见直到修好。
- **没有动 `src/` 一个字节。**

---

## 8. 顺带清掉的两处过期记录（都在 `tests/diff/` 内）

1. **`known_failures_not_comparable.txt` 的整个注释块曾被 `sort` 打散。**
   该文件此前对全文（含注释）排序，空行浮到顶、注释按字典序打散成不可连读的碎句，
   三个 `# --- 分组头 ---` 挤在一起且下面一条用例都没有。本次重排为「注释块在前、
   用例区有序」，与 `known_failures_embedded_not_comparable.txt` 已有的形态一致。
   用例区仍然按路径排序，行内注释跟着各自的行走，不改变排序键。

2. **`known_failures.txt` 的 Group 5 描述了一个不存在的状态。**
   它记的是 `semantics/functions/unproven_callee_targets.xr` 的 VM 侧在 2026-08-16
   开始以 `XR_SEM_0018` 拒绝，"两半都 refuse"。2026-08-28 实测：VM 又能跑通
   （rc=0，输出 `-10 / 14 / 0 / 2`），该用例的 `// diff-aot-reject:` 合约完整成立。
   这段散文只会把下一个人送去找一个不存在的分歧，已删。
   同时删掉了 Group 1–4 那四段 "Cleared 2026-07-31" 的历史考古——它们描述的缺陷
   都已修复、用例都已回到网内，散文留在 git history 里即可。

3. **两份分歧基线（`known_failures.txt` / `known_failures_embedded.txt`）都补上了
   "空不等于对" 这句话。** 两份都是 0 条用例，读起来像"两个后端一致"，
   实际是"只有 166 / 676 条走到了比对"。空文件现在自己说明它是关于**比过什么**的陈述，
   不是关于**证明了什么**的陈述；embedded 那份另外写明了这条 lane 实际红在 88 条上、
   其中 86 条在冻结基线上就红。

---

## 9. 如何重跑这份普查

没有新增脚本——仓库已经有一个更严格的收集器，它做的就是这件事：

```bash
/opt/homebrew/bin/python3 tests/diff/survey_refusals.py \
    --build build-nofp --output /tmp/live-refusal-manifest.json
```

它会跑同一套 discovery、保留完整拒绝日志、按 `contracts/target-machine/diagnostic-codes.toml`
解析诊断码，并报 `new_refusals` / `resolved_refusals`。**注意两点**：

1. 它对干净树、Release 构建、cmake identity 全部 fail-closed。本 lane 改过
   `tests/diff/` 下的清单，所以要先提交再跑，否则它按 dirty tree 拒绝。
2. 它的 `find_diagnostic` 只认注册码，所以 `XR_AOT_REFINEMENT_*` 那 82 条会落进
   `evidence_gaps`（见 §3.3）——这不是它的 bug，是那族确实没注册。

本普查在它之外多做的一件事，是把带机器操作数的消息**归一化后再聚类**：
剥掉 `'<case>_<hash>'`、绝对路径、以及尾部的 `key=value` 串。不做这一步，
`XR_TARGET_1003` 的 185 条消息文本会各自成族，43 族会碎成两百多族，工作队列就不成立了。

---

## 10. 给下一个动这张网的人：四条操作清单

**1. 想知道某条用例卡在哪，不要重建树——读清单的行内注释。** 这是本次改动的全部目的。
需要更细（完整机器操作数、VM 侧状态）时再去查逐条 JSONL 或单跑：

```bash
XRAY_DIFF_SINGLE_CASE=<case> XRAY_TOOLCHAIN_PROBE_SCALE=16 \
  python3 tests/diff/run_backend_diff.py build-nofp/xray
```

**2. 修好一个缺陷后，不要挨个试用例——跑一次全量，让棘轮自己报。**

```bash
XRAY_TOOLCHAIN_PROBE_SCALE=16 XRAY_TEST_CASE_TIMEOUT=900 XRAY_DIFF_JOBS=6 \
  python3 tests/diff/run_backend_diff.py build-nofp/xray
```

它会打印 `=== Listed cases now build; delete these entries ===`，**那一段的行数就是这次修复的价值度量**。删掉那些行即可，不需要任何额外脚本。走 ctest 也行，但在有负载的机器上 `backend_diff` 会撞 900s 超时（本 lane 实测 `***Timeout 900.50 sec`，而空闲机器上是 134.7s），直跑 runner 没有这个上限。

**3. 归因之前先排除三种伪造拒绝。** 全部在 §2 有实证：`***Timeout`（一眼可见）、
`no provider reached READY`（长得像真诊断，是探针在负载下超时）、
`cannot lock binary cache`（最阴，来自被 kill 的 lane 留下的残留锁，静默多出一条"不能构建"）。
kill 过 diff lane 的树，重跑前先清锁——**只清自己 worktree 的死锁**，
两条限定（`-type d` 与 `-not -newermt`）见 §2，无限定的清理会删掉别的测试正在持有的活锁。

**4. 分布对照的键必须是 `(诊断码, 归一化消息)` 二元组，不能只用码。** §3.2 给了理由，
本轮另有两条独立验证：同一个 `XR_SEM_0019` 在两条线上分别是
"coroutine state count disagrees with grounded call authority" 和
"native module scalar call authority is not exact"，是完全不同的缺陷；
`XR_AOT_REFINEMENT_REPRESENTATION` 与 `..._SCHEMA_UNAVAILABLE` 是相邻的两个 `return`，
只看码会当成同一族。清单的行内注释存的正是这个二元组，`grep` 即可，不必解析 JSON。

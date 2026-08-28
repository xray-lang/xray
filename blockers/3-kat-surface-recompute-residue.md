# 交接包：KAT 与表面清单重算之后的残留

- **Lane**：3（KAT 与 stdlib 表面清单重算）
- **状态**：`重算部分已完成；残留全部是真缺陷或越界项，逐条列在下面`
- **冻结起点**：`00f665c5c`，tree `afe293b71`
- **分支**：`work/3-kat-surface-recompute-00f665c5c`

## 0. 一句话结论

任务书把 13 个失败项归成"冻结值失配"，**实际只有 8 项是**。其余 5 项各有独立的真根因，
其中 3 项此前被归错了类——它们不含任何冻结值，重算它们既无从下手也毫无意义。

## 1. 已完成：真正属于重算的部分

| 测试 | 改动 | 依据 |
|---|---|---|
| `test_program_semantic_closure` | 3 个身份值 | AST 枚举重编号 + closure schema 8→9 |
| `test_program_semantic_call_locator` | 无（与上者同一个二进制） | — |
| `test_xa_program_semantic_closure` | 3 个身份值 | 同上 |
| `test_target_profile_authority` | 掩码域 | 请求侧加宽到 capability 位，比对侧没跟 |
| `test_xglobal_summary` | 9 处 schema 46→47 | 证据 schema bump |
| `test_native_type_surface` | `cluster.info` 改从私有 leaf 读签名 | 该函数已迁进 Xray |
| `test_stdlib_boundary_manifest` | binder 集合 3→6 | cluster / crypto / runtime 已迁进 Xray |

四个提交，每个都在提交信息里写明了旧值→新值与变化理由。`contract_freeze` 已在同一提交内重锚并 PASS。

**一条正向验证值得记下来**：`test_program_semantic_closure` 的 5 个身份值里,
`canonical_entry_function_identity` 与 `canonical_helper_function_identity` **逐位没变**。
函数身份不读 declaration locator，所以 `AST_FUNCTION_DECL` 重编号够不到它；
而 `AST_CALL_EXPR` 重编号经 callsite identity 进了调用身份。
**这个不对称就是"哈希算法没被动过、只是输入变了"的证据**——
若算法或 policy fingerprint 变了，函数身份必然一起变。盲刷这 5 个值会毁掉这个判据。

## 2. 缺陷 A：`import time` 的任何程序都编译不出 native 可执行文件

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

换成函数体内调用同样复现（`function=1 operation=7`）：

```xray
import time

fn main() {
    time.sleep(1)
}
```

**确定性的，与机器负载无关。** 第一次跑 `install_public_surface` 时报的是
`no provider reached READY` 加 `probe stage timed out after 10000 ms`，
那是十路并行把负载压到 20 以上时的叠加表象；串行直接复现拿到的是上面这条稳定的 `XR_SEM_0019`。
**不要按 flaky 处理。**

- **请求接手方**：语义/协程判据的所有者
- **判据位置**：搜 `XR_SEM_0019`
- **为什么重要**：`time.sleep` 是最常见的库调用之一，且这条路径是 2 模块图
  （`stdlib/time/time.xr` + 入口），与 4 号的多模块 program authority 目标相邻但**不是同一条**——
  这里的拒绝码是协程状态计数，不是 `XR_TARGET_1000`。

## 3. 缺陷 B：手工构造的 `XI_PRINT` 不再能建出 SemanticPlan

`test_xr_aot_refinement` **一个冻结值都没有**（全文无 16 进制字节数组、无指纹字面量，
24 处 `strcmp` 全是 ABI 类型名比对）。它 abort 在：

```
requirement failed at tests/unit/aot/test_xr_aot_refinement.c:664:
    build_fixture_semantic_plan_and_attach(fixture.function, error, sizeof(error))
```

紧邻的 fixture 手工建了一条 `XI_PRINT`（`:652-653`）。print 改成携带 plan 的单条 variadic
指令之后，手工建的这条没有 plan，SemanticPlan 建不出来。

- **归属**：print 缺陷簇，与 `test_xi_lower` / `test_xi_compare` /
  `test_xi_cgen_iterator_rune_nth_emission` / `test_repl` 同源，**交 2 号**
- **不需要新 packet**，只需归类；修 print 侧之后这一项应自动转绿

## 4. 缺陷 C：coro root 上的 runtime control plane 丢了 descriptor 表示

`test_xglobal_summary` 的 9 处 schema 已按 46→47 重述，139 个用例里**只剩 1 个失败**：

```
global_evidence_producer_keeps_runtime_control_plane_on_coro_root FAIL
  tests/unit/analysis/test_xglobal_summary.c:12989
  Expected: 1, Got: 0
```

该行是 `ASSERT_EQ_UINT(bundle.entry_plan.root_representation, XR_ROOT_DESCRIPTOR);`，
读的是活值，**没有冻结的对应物**，所以不能靠重述解决。

**它还连累了另一项**：`byte_u8_canonical_audit` 把整个 `test_xglobal_summary` 套件当成一道门，
所以这一个与 byte/u8 毫无关系的用例会让 byte/u8 一致性门变红。
实测该门的另外两步全绿（`xray test 1409_byte_u8_canonical_identity.xr` 与
`test_lsp_document` 45 条全过），且套件内 `..._canonicalizes_byte_u8_sequence_type_keys` 本身 PASS。

- **`byte_u8_canonical_audit` 不需要任何重算**，修好上面这一个用例它就绿
- **顺带建议**：这道门的粒度太粗，一个无关用例能伪装成 byte/u8 一致性失败

## 5. 越界项：`test_mcp_knowledge_generation` 要写 `src/`

```
error: src/app/mcp/xmcp_knowledge_generated.c: stale
runtime: source API symbols missing from generated knowledge: RuntimeInfo.constructor
```

生成物过期，缺 `RuntimeInfo.constructor`——这是 runtime 模块迁进 Xray 之后的正常表面变化。
修法是重生成，但产物在 `src/` 下，**超出本 lane 的允许文件边界**，没有动。

一条命令即可（有权改 `src/` 的人执行）：

```bash
cmake --build build-nofp --target regen-mcp-knowledge
```

## 6. 无门在守：Iterator 的 builtin-schema 三条

`check_stdlib_boundary.py --check all` 有 8 个子检查，**只有 5 个注册成了 ctest**
（`stdlib_boundary_{manifest,semantic,error-model,fastpath,dynamic}`，实测 5/5 全绿）。
未注册的三个里 `builtin-schema` 是红的：

```
Iterator declaration methods must be exactly hasNext/next/nth;
  got hasNext, iterator, next, nth, toString
compiler Iterator method table must match stdlib/types/iterator.xr; got
API inventory does not expose the complete Iterator schema
```

因为没有门，它不在 82 项失败里，也不会拦住任何人。**要么把它接成 ctest，要么把判据更新到
`iterator.xr` 现在的 5 个方法**——两者都不在本 lane 的边界内（前者是 8 号的接线，后者要改 `scripts/`）。

## 7. 观察：身份换代了，版本号没跟着换

这一条不阻塞任何人，但值得记一笔，否则下次还会踩。

本次身份值移动的根因之一是 AST 节点种类枚举**序数**进了哈希（删掉 `AST_PRINT_STMT`，
其后每个成员 −1）。但与之配套的版本号**都没有 bump**：

- `xr_source_semantic_callsite_identity` 的域串仍是 `xray-source-program-callsite-v2`
- `XR_SOURCE_SEMANTIC_IDENTITY_VERSION` 仍是 `1`

结果是同一个 `(域串, 版本)` 对在合并前后指代了两套不同的值空间。这不影响确定性
（已逐项排除地址、时间、环境变量、哈希表遍历序、绝对路径——记录表在算指纹前按 stable id 全量排序），
但它意味着**任何一次无关的 frontend 枚举增删都会静默旋转已冻结的语义身份，而版本号不会告诉你**。

## 8. 复跑方式

```bash
cmake -S . -B build-nofp -G Ninja -DCMAKE_BUILD_TYPE=Release -DXRAY_STDLIB_VM_FASTPATHS=OFF
cmake --build build-nofp -j 8
ctest --test-dir build-nofp -j 2 -R '^(contract_freeze|test_program_semantic_closure|test_program_semantic_call_locator|test_xa_program_semantic_closure|test_target_profile_authority|test_native_type_surface|test_stdlib_boundary_manifest)$'
```

七项应全绿。

**注意**：十路并行时全量 `ctest -LE sanitizer -j4` 不可用于归因——实测负载 20 以上时
`backend_diff` 从 134 秒涨到 900 秒超时。本 lane 的每一项都改用逐项 focused gate 验证。

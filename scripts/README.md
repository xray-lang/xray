# Xray 诊断脚本

> **配套规约**: `docs/tasks/082-pre-076-foundation.md` §4（任务 D — 诊断脚本固化）
> **范围**: 所有从手工命令固化为可重放的 shell 脚本
> **平台**: 全部 Bash；macOS / Linux 直跑，Windows 通过 Git Bash 或 WSL；不依赖网络与未打包的本地路径

## 总览

| 脚本 | 用途 | 输入 | 退出码语义 | 期望耗时 |
|---|---|---|---|---|
| `scripts/run_mem_stress.sh` | memory 重测试 1205/1206/1207 burn-in | `[rounds]`；env: `XRAY_BIN`, `MEM_STRESS_ROUNDS` | 全过=0；任一失败=1；参数错=2 | rounds × ~30s |
| `scripts/repro_win11_coro_burn.sh` | Win11 协程 4 用例 burn-in（1115/1109/1127/1128） | `[N]`；env: `XRAY_BIN` | 全过=0；任一失败=1；参数错=2 | N × 4 × ~5s |
| `scripts/check_temp_workarounds.sh` | `DEFENSIVE-TEMP[NNN]` 标签 ↔ `tests/known_temp_workarounds.md` 双向对账 | 无 | 任一不一致=非0 | < 10s |
| `scripts/check_stdlib_surface_uniqueness.py` | 151 R3：不同 public stdlib surface 不得绑定同一 VM/AOT helper | `--root <repo>`；可选 `--list-known` | 新重复=1；仅命中已登记债务=0 | < 1s |
| `scripts/check_parallel_surface_convergence.py` | 193：旧 `parallel for/range/reduce/collect/local/final` 语法与旧 AST/parser/spec/demo/API-doc 表面不得回流 | `--root <repo>` | 任一旧表面残留=1 | < 2s |
| `scripts/check_parallel_backend_abi_convergence.py` | 193：parallel backend ABI/descriptor 命名收敛，旧 AOT-private/global-pool 名称不得回流 | `--root <repo>` | ABI 缺失或旧名残留=1 | < 2s |
| `scripts/check_bytes_type_residue.py` | 204：`Bytes/ByteSpan/ByteView` 删除 residue 分类 inventory，区分 public 表面与 internal legacy 命名 | `--root <repo>`；可选 `--json`、`--fail-on-public` | 默认只输出 inventory=0；CTest `bytes_type_residue` 用 `--fail-on-public` 阻止 public 残余回流 | < 2s |
| `scripts/check_source_unknown_convergence.py` | 202：source `unknown` 删除与 typed erasure 边界收敛前的 source/runtime/analyzer/IR/AOT/Task residue 分类 inventory | `--root <repo>`；可选 `--json` | 默认只输出 inventory=0，为 P0 固定基线 | < 2s |
| `scripts/check_source_unknown_aot_baseline.py` | 202：Task、ThreadLocal、Json encode 与 HTTP handler 的 AOT baseline fixture/expect 覆盖检查 | `--root <repo>`；可选 `--json` | baseline fixture 或关键断言缺失=1 | < 1s |
| `scripts/check_error_effect_convergence.py` | 205：unchecked error-effect graph 收敛前的旧 error-set API、`MAY_THROW`、pending-error、LSP 与 `.xrd` 分类 inventory | `--root <repo>`；可选 `--json` | 默认只输出 inventory=0，为 P0 固定基线 | < 2s |
| `scripts/check_param_mode_convergence.py` | 206：`value/in/ref/out` 参数契约、调用授权、`move/copy` 来源动作与旧 `XR_PARAM_*`/并行数组 residue 分类 inventory | `--root <repo>`；可选 `--json` | 默认只输出 inventory=0，为 P0 固定基线 | < 2s |

## 详细说明

### `run_mem_stress.sh`

按 `082` 文档 D 表的 `<mode>` 设计，但 Xray 当前是 RC + cycle collection，不暴露用户可选 collector 模式。务实落地是把 `<mode>` 折成 `[rounds]`：每轮顺序跑全部 memory 重测试（1205/1206/1207），失败时把每次失败的尾 30 行写入 `tests/tmp/mem_stress_failures.log`。

放大机制：rounds × ASan/MSan + `MALLOC_PERTURB_=205` + MallocScribble = 当前 CI 暴露 May 2026 Bug #8/#11 的实际配方。继续按 rounds 投资就够了，无需新加 CLI 开关。

### `repro_win11_coro_burn.sh`

May 2026 在 Windows 上暴露 `STATUS_HEAP_CORRUPTION` 的协程场景：1115 cancel / 1109 await_any / 1128 yield。每场景跑 N 次（默认 5，匹配 `nightly.yml`），在 `tests/tmp/win11_coro/failures.log` 收集失败 tail。

可在非 Windows 平台运行——相关 race 是堆破坏而非真正 Windows-only 行为，Linux / macOS 在 ASan/MSan 下也可能暴露相同根因。

### `check_temp_workarounds.sh`

接入 PR 门禁（参考 `.github/workflows/ci.yml` 的 `reverse-invariants` job）。本表只做完整性收录，不再展开规则。

### `check_stdlib_surface_uniqueness.py`

扫描 `stdlib/defs/*.def` 的 public module functions 与 native class methods，按 VM/AOT helper 分组。不同 public symbol 复用同一 helper 会失败；同一 symbol 的 overload 合法，`visibility: "internal"` helper 不进入用户表面检查。当前仅允许脚本内精确登记的历史债务，避免 147/151 后续落地时重新出现平行 API。

### `check_parallel_surface_convergence.py`

扫描 active `.xr` 用户源码、active `spec/`/`demos`/API docs 文本、前端/IR 源码和 `tests/aot/coro` 迁移桶。除保留的 compile-error 负例外，旧 `parallel for/range/reduce/collect` 语法、旧 parallel `local/final` grammar、`TK_PARALLEL`、`AST_PARALLEL_*`、`XI_PAR_COLLECT` 等旧表面一律失败。当前 public API docs 还会拒绝 `worker_id`、`worker id`、`lane arrays`、`lanes[...]` 这类手写 worker-id/lane-array 推荐示例，避免重新把 executor 内部身份暴露给用户。`docs/` 是历史任务文档 symlink，脚本只扫描其中的 `spec/`、`language/`、`knowledge/`、`rules/` 等当前 public API 子树。该脚本接入 CTest 的 `parallel_surface_convergence`。

### `check_parallel_backend_abi_convergence.py`

扫描 `src/`、`stdlib/`、`tests/`、active `spec/`、`demos/` 与 API docs 中的 parallel backend 相关源码、cgen fixtures 和当前公开说明。旧
`xr_aot_parallel*`、`XrAotPar*`、`XR_AOT_PAR*`、`XrParallelPool`、`xr_parallel_pool_*`、
`parallel_pool` 命名一律失败；同时要求 VM 仍通过 `OP_PAR_FOR/MAP/REDUCE` dispatch，AOT
header/runtime/cgen fixtures 仍使用 `xr_parallel_*` 与 `XrParallel*` descriptor ABI。该脚本接入
CTest 的 `parallel_backend_abi_convergence`。

### `check_bytes_type_residue.py`

扫描 `src/`、`stdlib/`、`tests/`、`spec/`、`demos/`、`tools/`、`scripts/`，把 204 旧 public binary 类型残余分为
`PUBLIC_TYPE_BYTES*`、`PUBLIC_SIGNATURE_BYTES`、`PUBLIC_DIAGNOSTIC_BYTES`、
`PRELUDE_OR_RESOLVER_ALIAS`、`CONSTRUCTOR_OPCODE_BYTES`、`METHOD_RECEIVER_BYTES`、
`INTERNAL_LEGACY_BYTES_NAMING` 和允许保留的 compile-error 负例。默认模式只打印 inventory
并返回 0，便于 P0 固定基线；`--json` 输出机器可读结果；`--fail-on-public` 在 public 表面类目
非空时返回 1，CTest `bytes_type_residue` 已用该模式作为 public surface 回流门禁。历史 `XI_BYTES_*`、
`OP_BYTES_*`、`xr_array_bytes_*`、`emit_bytes_*` 与 `cg_bytes_*` 名称应保持为 0；若后续
backend 切片重新引入这些 internal legacy 命名，inventory 会把它们列为回流证据。

### `check_source_unknown_convergence.py`

扫描 `src/`、`stdlib/`、`tests/`、`spec/`、`demos/`、`tools/`、`scripts/` 与 active language spec，
把 202 source `unknown` 删除与 typed erasure 边界收敛前的事实分成
`SOURCE_UNKNOWN_TYPE_SURFACE`、`UNKNOWN_IDENTIFIER_ALLOWED_GUARD`、
`REMOVED_SOURCE_UNKNOWN_DIAGNOSTIC`、`ERROR_TYPE_RECOVERY`、
`RUNTIME_UNKNOWN_TYPE_SINGLETON_OR_FACTORY`、`XR_TYPE_IS_UNKNOWN_CONSUMER`、
`ASSIGNABILITY_OR_GENERIC_UNKNOWN_COMPAT`、`TYPE_ANY_OR_DYNAMIC_SLOT_FALLBACK`、
`IR_UNKNOWN_ERASURE_CONSUMER`、`AOT_UNKNOWN_ERASURE_CONSUMER`、
`FORMATTER_LSP_UNKNOWN_SURFACE`、`TASK_ERASED_RESULT_RESIDUE`、
`STDLIB_DYNAMIC_UNKNOWN_API` 和 `PUBLIC_SPEC_UNKNOWN_RESIDUE`。默认模式只打印 inventory
并返回 0，便于 P0 固定 source `unknown`、runtime unknown singleton、type_any / dynamic slot
fallback、TaskResult/TaskOutcome erased payload 与 active spec residue；后续 202 P1-P7 可按类别逐步增加 fail gate。

### `check_source_unknown_aot_baseline.py`

检查 202 P0 要求的四类 AOT baseline 是否都有当前 filetest 和关键 expect 断言：Task、ThreadLocal、
Json encode 与 HTTP handler。该脚本只证明 baseline 覆盖存在，不宣称这些边界已完成最终 typed
contract；ThreadLocal 与 HTTP handler 当前仍作为后续替换目标被固定在 baseline 中。该脚本接入 CTest
的 `source_unknown_aot_baseline`。

### `check_error_effect_convergence.py`

扫描 `src/`、`stdlib/`、`tests/`、`spec/`、`demos/`、`tools/`、`scripts/` 与 active language spec，
把 205 effect graph 收敛前的事实分成 `XR_ERROR_SET_RUNTIME_OR_API`、
`FUNCTION_TYPE_ERROR_SET_FIELD`、`SYMBOL_LINK_ERROR_SET_FIELD`、
`XI_MAY_THROW_FLAG_OR_EFFECT`、`GLOBAL_SUMMARY_MAY_THROW_EFFECT`、
`AOT_MAY_THROW_CONSUMER`、`IR_ERROR_CHANNEL_CONSUMER`、
`VM_RUNTIME_PENDING_ERROR_CHANNEL`、`LSP_ERROR_TOOLING_ENTRY`、
`XRD_METADATA_GENERATOR_OR_LOADER`、`NATIVE_ERROR_CONTRACT_SURFACE` 和
`TASK_TYPED_ERROR_RESIDUE`。默认模式只打印 inventory 并返回 0，便于 P0 固定旧 error-set、
旧 MAY_THROW 和 tooling metadata 入口；后续 205 P1/P2/P8/P10 可按类别逐步增加 fail gate。

### `check_param_mode_convergence.py`

扫描 `src/`、`stdlib/`、`tests/`、`spec/`、`demos/`、`tools/` 与 active language spec，
把 206 参数模式收敛前的事实分成 `CANON_DECL_MODE_SPELLING`、`PREFIX_DECL_MODE_SPELLING`、
`CALL_SITE_REF_OUT_MARKER`、`MOVE_AS_PARAM_MODE_RESIDUE`、`STALE_MODIFIER_EBNF`、
`XR_PARAM_MACRO_RESIDUE`、`PASSING_MODE_FIELD_OR_ARRAY`、`FUNCTION_TYPE_MODE_CONSUMER`、
`BACKEND_ABI_MODE_CONSUMER`、`BORROW_ESCAPE_SUSPEND_CONSUMER` 和
`ACTIVE_PUBLIC_SURFACE_PARAM_MODE_HIT`。默认模式只打印 inventory 并返回 0，便于 P0 固定
基线；后续 206 P1/P2/P3 可按类别逐步增加 fail gate。

## 与 nightly.yml 的关系

`nightly.yml` 的 `mem-stress` job 调用 `scripts/run_mem_stress.sh`，`windows-msvc-release` job 调用 `scripts/repro_win11_coro_burn.sh`：

```yaml
- name: Run Memory stress
  run: bash scripts/run_mem_stress.sh 10

- name: Run Win11 coroutine burn-in
  shell: bash
  run: bash scripts/repro_win11_coro_burn.sh 5

- name: stdlib public surface uniqueness
  run: python3 scripts/check_stdlib_surface_uniqueness.py --root .
```

## 修订历史

| 日期 | 改动 | 作者 |
|---|---|---|
| 2026-05-17 | 初稿；与 4 个新脚本同批；明确 `<mode>` → `[rounds]` 的务实落地决定 | Cascade + xingleixu |
| 2026-06-16 | 移除 JIT 相关脚本（repro_jit_force_burn / run_fuzz_30min / check_codegen_* / jit_fuzz / run_jit_*）；memory stress 去 jit-force/no-jit 模式 | — |
| 2026-07-08 | 增加 stdlib public surface uniqueness 检查，接 151 R3 单一规范表面门禁 | Codex |
| 2026-07-12 | 增加 parallel surface convergence 检查，接 193 旧专用语法删除门禁 | Codex |
| 2026-07-12 | 增加 parallel backend ABI convergence 检查，接 193 VM/AOT backend 命名收敛门禁 | Codex |
| 2026-07-13 | 增加 Bytes type residue inventory，接 204 P0/P7 public 表面与 internal legacy 命名分类 | Codex |
| 2026-07-13 | 增加 parameter mode convergence inventory，接 206 P0 参数契约与调用授权收敛基线 | Codex |
| 2026-07-14 | 增加 unchecked error-effect convergence inventory，接 205 P0 旧 error-set / MAY_THROW / tooling metadata 分类 | Codex |
| 2026-07-14 | 增加 source unknown convergence inventory，接 202 P0 source unknown 与 typed erasure 边界分类 | Codex |

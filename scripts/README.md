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
| `scripts/check_binary_stdlib_surface.py` | 200：binary stdlib 的 string-binary 签名、旧别名、null sentinel、Array-owner 输入与消费者分类 inventory | `--root <repo>`；可选 `--json`、`--fail-on-public-residue` | 默认只输出 inventory=0，为 P0 固定基线；最终 public residue 可切换为失败 | < 2s |
| `scripts/check_binary_stdlib_kat_baseline.py` | 200：base64/compress/crypto KAT、AOT link-command 与现有 stdlib bench 入口覆盖检查 | `--root <repo>`；可选 `--json` | 关键 fixture 或 anchor 缺失=1 | < 1s |
| `scripts/check_parallel_surface_convergence.py` | 193：旧 `parallel for/range/reduce/collect/local/final` 语法与旧 AST/parser/spec/demo/API-doc 表面不得回流 | `--root <repo>` | 任一旧表面残留=1 | < 2s |
| `scripts/check_parallel_backend_abi_convergence.py` | 193：parallel backend ABI/descriptor 命名收敛，旧 AOT-private/global-pool 名称不得回流 | `--root <repo>` | ABI 缺失或旧名残留=1 | < 2s |
| `scripts/check_query_surface_residue.py` | 192：`len` / container membership / `typeOf` / `typeName` 查询表面 residue 分类 inventory，区分 public alias 与内部 lowering/runtime 名 | `--root <repo>`；可选 `--json`、`--fail-on-public` | 默认输出 inventory；CTest `query_surface_residue` 阻止 public query alias 回流 | < 2s |
| `scripts/check_bytes_type_residue.py` | 204：`Bytes/ByteSpan/ByteView` 删除 residue 分类 inventory，区分 public 表面与 internal legacy 命名 | `--root <repo>`；可选 `--json`、`--fail-on-public`、`--fail-on-internal-legacy` | 默认只输出 inventory=0；CTest `bytes_type_residue` 阻止 public 与 internal legacy 残余回流 | < 2s |
| `scripts/check_byte_width_predicates.py` | 204：高层 analyzer/IR/AOT 不得重新用 `native_width == XR_NATIVE_U8` 或 `elem_name == "XR_ELEM_U8"` 选择 byte 语义 | `--root <repo>`；可选 `--json` | 未登记的直接 U8 width/string predicate=1；CTest `byte_width_predicate_audit` 固定共享 helper 边界 | < 2s |
| `scripts/run_byte_uint8_canonical_audit.sh` | 204：`byte`/`uint8` 规范化为同一 U8 identity 的 final audit，串联语言正例、LSP canonical docs 与 global evidence/cache type-key | env: `XRAY_BIN`, `XRAY_TEST_LSP_DOCUMENT`, `XRAY_TEST_XGLOBAL_SUMMARY` | 任一子门禁失败=1；CTest `byte_uint8_canonical_audit` 固定可复跑组合证据 | < 120s |
| `scripts/run_byte_receiver_effect_audit.sh` | 204：`Array<byte>` / `Slice<byte>` receiver effect 与 203 local/owned/const/shared provenance 对齐 audit | env: `XRAY_BIN` | 任一正例或负例漂移=1；CTest `byte_receiver_effect_audit` 固定可复跑组合证据 | < 120s |
| `scripts/check_source_unknown_convergence.py` | 202：source `unknown` 删除与 typed erasure 边界收敛前的 source/runtime/analyzer/IR/AOT/Task residue 分类 inventory | `--root <repo>`；可选 `--json` | 默认只输出 inventory=0，为 P0 固定基线 | < 2s |
| `scripts/check_source_unknown_aot_baseline.py` | 202：Task、ThreadLocal、Json encode 与 HTTP handler 的 AOT baseline fixture/expect 覆盖检查 | `--root <repo>`；可选 `--json` | baseline fixture 或关键断言缺失=1 | < 1s |
| `scripts/check_error_effect_convergence.py` | 205/216：unchecked error-effect graph、typed throw bit 与 backend 重推导分类 inventory | `--root <repo>`；可选 `--json`、`--max-category NAME=N` | 默认输出 inventory；CTest 固定 `THROW_BIT_RECOMPUTE=0`，阻止 backend/CGen 重推导 typed bit | < 2s |
| `scripts/check_param_mode_convergence.py` | 206：`value/in/ref/out` 参数契约、调用授权、`move/copy` 来源动作与旧 `XR_PARAM_*`/并行数组 residue 分类 inventory | `--root <repo>`；可选 `--json` | 默认只输出 inventory=0，为 P0 固定基线 | < 2s |
| `scripts/check_meta_ownership.py` | 218：编译器元级跨生命周期借用审计，分类 A `AST_PTR_INTO_IR`、B `PTR_ACROSS_GROWTH`、C `CGEN_BORROWED_NAME`（R-OWN-1..3） | `--root <repo>`；可选 `--json`、`--counts-json`、`--baseline <json>`、`--max-category NAME=N`、`--write-baseline <json>` | CTest `meta_ownership_inventory` 对三类均固定 `--max-category NAME=0`，发现回流即失败；standalone inventory 仍可用于审计 | < 2s |
| `scripts/check_contract_freeze.py` | 220：八份语义契约的 anchor digest 与 `CONTRACT-CHANGE` trailer 门禁 | `--root <repo>`；注入验证用 `--self-test` | digest 漂移、契约锚点缺失或干净提交缺 trailer=1；dirty tree 只检查 digest，trailer 延迟到 post-commit/CI | < 2s |
| `scripts/run_asan_focused.sh` | 218 防线 2：ASan+UBSan 聚焦门禁——C 单测 + 快速 backend-diff 子集（task190）+ xxhash 端口与已提交 bili-analysis-server fixture 的全量 AOT C 发射。`detect_leaks=0`（泄漏归 lsan_strict） | env: `XR_ASAN_JOBS`、`XR_ASAN_CTEST_REGEX`、`XR_ASAN_CTEST_EXCLUDE`、`XR_ASAN_DIFF_REGEX`、`XR_ASAN_XXHASH_MAIN`、`XR_ASAN_BILI_MAIN`、`XR_ASAN_SKIP_BUILD` | 必需 bili fixture 或任一已发现 workload/测试失败=非0；xxhash sibling 缺失时明确跳过；普通非 sanitizer 构建的 CTest 常驻 `asan_focused` | 增量测试面 <10min（全量 ASan 自举另计） |
| `scripts/run_lsan_strict.sh` | 218 防线 4：严格 LeakSanitizer lane——ASan+LSan（`detect_leaks=1`）跑单测面，配 `scripts/lsan.supp`。LSan 仅 Linux 支持，macOS 上明确跳过 | env: `XR_LSAN_JOBS`、`XR_LSAN_BUILD_DIR`、`XR_LSAN_CTEST_REGEX` | 非 Linux=0（跳过）；Linux 有泄漏=非0；普通非 sanitizer 构建的 CTest 常驻 `lsan_strict` | Linux CI 数分钟 |
| `scripts/bench_cgen_verifier.py` | 218 防线 3：在真实 xxhash AOT C 发射中统计常开 W1–W4 verifier 的 CPU 时间占端到端编译 wall time 比例 | `--xray`、`--main`、`--samples`、`--max-percent` | 中位开销 `<1%` 为 0；达到或超过预算为 1；计时环境变量只观测、不能关闭 verifier | 默认 5 次，约 1min |
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

### `check_binary_stdlib_surface.py`

扫描 `stdlib/defs/*.def` 与 pure-Xray stdlib exports 的 source-derived public API，再扫描 `stdlib/`、`tests/`、`spec/`、`demos/`、active docs knowledge 与 generated metadata，把 200 binary stdlib 收敛前的事实分成
`PUBLIC_BINARY_STRING_SIGNATURE`、`PUBLIC_FIXED_DIGEST_AS_STRING`、`PUBLIC_ARBITRARY_STRING_CREATOR`、
`PUBLIC_STRING_BINARY_UNION`、`PUBLIC_NULL_SENTINEL`、`PUBLIC_LEGACY_BINARY_ALIAS`、
`PUBLIC_ARRAY_OWNER_INPUT`、`PUBLIC_DOMAIN_BOOL_SENTINEL`、`CONSUMER_OLD_BINARY_API_CALL`、
`NATIVE_ARBITRARY_STRING_CREATOR` 和 `GENERATED_METADATA_STALE_BINARY_SURFACE`。默认模式只打印
inventory 并返回 0，便于 P0 固定当前 string-binary public surface、consumer、native creator 与
generated docs baseline；后续 200 P2-P6 可按类别逐步增加 fail gate。

### `check_binary_stdlib_kat_baseline.py`

检查 200 P0 要求的 base64/compress/crypto corpus 和当前 AOT baseline 是否仍可审计：RFC4648 base64 anchors、
base64 contract/diff cases、CRC32/Adler32 known values、hash/HMAC/AES/timing-safe crypto vectors、
core base64/compress/crypto 与 system crypto random AOT link-command expect，以及现有 `base64.contract`
stdlib benchmark入口。该脚本只固定现有 KAT/AOT/bench 入口覆盖，不宣称 legacy string-binary surface 已正确或完成迁移。

### `check_parallel_surface_convergence.py`

扫描 active `.xr` 用户源码、active `spec/`/`demos`/API docs 文本、前端/IR 源码和 `tests/aot/coro` 迁移桶。除保留的 compile-error 负例外，旧 `parallel for/range/reduce/collect` 语法、旧 parallel `local/final` grammar、`TK_PARALLEL`、`AST_PARALLEL_*`、`XI_PAR_COLLECT` 等旧表面一律失败。当前 public API docs 还会拒绝 `worker_id`、`worker id`、`lane arrays`、`lanes[...]` 这类手写 worker-id/lane-array 推荐示例，避免重新把 executor 内部身份暴露给用户。`docs/` 是历史任务文档 symlink，脚本只扫描其中的 `spec/`、`language/`、`knowledge/`、`rules/` 等当前 public API 子树。该脚本接入 CTest 的 `parallel_surface_convergence`。

### `check_parallel_backend_abi_convergence.py`

扫描 `src/`、`stdlib/`、`tests/`、active `spec/`、`demos/` 与 API docs 中的 parallel backend 相关源码、cgen fixtures 和当前公开说明。旧
`xr_aot_parallel*`、`XrAotPar*`、`XR_AOT_PAR*`、`XrParallelPool`、`xr_parallel_pool_*`、
`parallel_pool` 命名一律失败；同时要求 VM 仍通过 `OP_PAR_FOR/MAP/REDUCE` dispatch，AOT
header/runtime/cgen fixtures 仍使用 `xr_parallel_*` 与 `XrParallel*` descriptor ABI。该脚本接入
CTest 的 `parallel_backend_abi_convergence`。

### `check_query_surface_residue.py`

扫描 `src/`、`stdlib/`、`tests/`、`spec/`、`demos`、`tools`、`scripts/` 与顶层 spec，把 192 查询表面残留分为
`PUBLIC_TYPE_QUERY_ALIAS`、`PUBLIC_TYPE_QUERY_ALIAS_SUPPORT`、
`PUBLIC_CONTAINER_QUERY_ALIAS`、`PUBLIC_CONTAINER_QUERY_ALIAS_SUPPORT`、
允许保留的 removed-surface 负例、LSP 反向断言、内部 `OP_TYPEOF` / `XI_TYPENAME` / `xrt_typename`
lowering/runtime 名，以及领域对象或 C layout 中合法出现的 `length/size` 文本。默认模式只打印 inventory；
`--json` 输出机器可读结果；`--fail-on-public` 在 public alias/support 类目非空时返回 1。CTest
`query_surface_residue` 打开 `--fail-on-public`，阻止 `typeof/typename/Reflect.typeOf` 与旧容器查询 alias
重新进入公开语言、REPL、LSP、API inventory 或 native public surface。

### `check_bytes_type_residue.py`

扫描 `src/`、`stdlib/`、`tests/`、`spec/`、`demos/`、`tools/`、`scripts/`，把 204 旧 public binary 类型残余分为
`PUBLIC_TYPE_BYTES*`、`PUBLIC_SIGNATURE_BYTES`、`PUBLIC_DIAGNOSTIC_BYTES`、
`PRELUDE_OR_RESOLVER_ALIAS`、`CONSTRUCTOR_OPCODE_BYTES`、`METHOD_RECEIVER_BYTES`、
`INTERNAL_LEGACY_BYTES_NAMING`、允许保留的 compile-error 负例和 XRD removed-alias fail-closed guard。
默认模式只打印 inventory
并返回 0，便于 P0 固定基线；`--json` 输出机器可读结果；`--fail-on-public` 在 public 表面类目
非空时返回 1；`--fail-on-internal-legacy` 在 internal legacy 命名非空时返回 1。CTest
`bytes_type_residue` 同时打开这两个模式，阻止 public surface 与 internal helper 命名回流。
历史 `XI_BYTES_*`、`OP_BYTES_*`、`xr_array_bytes_*`、`xrt_bytes_*`、`emit_builtin_bytes_*`、
`emit_bytes_*`、`cg_bytes_*` 与 `XR_ERROR_CORE_BYTES_*` 名称应保持为 0。

### `check_c_interop_surface.py`

扫描 `src/`、`stdlib/`、`tests/`、`spec/`、`demos/`、`tools/`、`scripts/` 和语言规范，
把 task 190 的 C interop public surface 收敛状态分类为 canonical `Ptr<T>`/`MutPtr<T>`、
canonical `mem.ptr/mutPtr/addr/load/store`、允许保留的 compile-error 负例，以及旧
`RawPtr`/`RawMut`、`mem.fromAddress/addressOf`、raw-pointer `loadLE/storeLE` 和
`ptrUnchecked` 回流。默认模式只打印 inventory；`--json` 输出机器可读结果；
`--fail-on-active-removed` 在旧表面出现在负例之外时返回 1。CTest
`c_interop_surface_residue` 打开该 strict 模式，阻止 task 190 旧 C interop 表面回流。

### `check_byte_width_predicates.py`

扫描 `src/frontend/analyzer`、`src/ir` 与 `src/aot` 中最容易让 204 语义回流的直接 U8 判断：
`native_width == XR_NATIVE_U8`、`xg_synthetic_width_type_key(...U8)` 和
`elem_name`/`strcmp` 对 `"XR_ELEM_U8"` 的字符串 predicate。receiver/method selection 必须消费
`xr_type_is_exact_u8`、`xr_type_is_u8_array`、`xr_type_is_u8_slice`、`xr_type_is_u8_contiguous`
等共享 helper 或 receiver registry；脚本只允许数值宽度 lattice、bulk memset byte-pattern type-key
和 class-field schema verifier 这类低层验证/编码点继续直写 U8。

### `run_byte_uint8_canonical_audit.sh`

串联 204 完成定义中 `byte` / `uint8` canonical U8 identity 的三类证据：

- `1409_byte_uint8_canonical_identity.xr` 固定 scalar、`Array<T>`、`Slice<T>`、函数参数和 U8 条件方法互通。
- `test_lsp_document` 固定 `Array<uint8>` 源码拼写的 completion/hover docs canonicalize 到 `Array<byte>` / `Slice<byte>`。
- `test_xglobal_summary` 固定 global evidence 和 cache materialization 里的 `Array<byte>` / `Array<uint8>`、`Slice<byte>` / `Slice<uint8>` type key 同一。

CTest `byte_uint8_canonical_audit` 只在 LSP 单测可用的平台启用，避免 final audit 退回到手工命令列表。

### `run_byte_receiver_effect_audit.sh`

串联 204 完成定义中 receiver effect 与 203 storage/provenance 对齐的正反例：

- 正例固定 `var` / `owned` `Array<byte>` mutating receiver 可用，`const` / `shared` 派生 `Slice<byte>` 只读路径可用。
- 正例复跑 shared-derived readonly direct/import/re-export/returned function-value 参数，以及 owned move-to-shared 基础回归。
- 负例固定 `in` 参数、const view、shared binding、shared-derived Slice direct/param/function-value/import/re-export/returned callee，以及 active Slice borrow 下的 move/freeze 都会被拒绝。

CTest `byte_receiver_effect_audit` 只在 Bash 可用的平台启用，避免 204/203 对齐证据退回到散落命令列表。

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
`TASK_TYPED_ERROR_RESIDUE`。216 新增 `THROW_BIT_RECOMPUTE`：只跟踪 lowering 之后直接消费
`XrFnThrowEffect`/`XR_FN_EFFECT_*` 并重建 source-level throw bit 的位置；CGen 对已经生成的
`ERR_CHECK` 做低级操作折叠属于允许保留的 defense-in-depth，不计入该类别。默认模式仍输出
完整 inventory；CTest 以 `--max-category THROW_BIT_RECOMPUTE=0` 固定零基线，阻止 backend/CGen
重新形成 typed-bit 第二真源。

### `check_meta_ownership.py`

扫描 `src/ir/`、`src/aot/`、`src/analysis/`，把 218 防线 1（编译器元级内存安全）关注的
"跨生命周期借用"分成三类：`AST_PTR_INTO_IR`（`Xi*`/`Xaot*` 结构字段右值直接借用
`node->name` 式 AST 名称指针，且无 `arena_strdup`/`intern` 包裹）、`PTR_ACROSS_GROWTH`
（同一函数体内取 `&arr[i]`/`arr + i` 后又对同一数组 `push`/`append`/`grow`/`realloc`）、
`CGEN_BORROWED_NAME`（CGen ctx 族结构里的裸 `const char *` 借用面）。规约见 R-OWN-1..3
（脚本 docstring）。三类借用已经归零，CTest `meta_ownership_inventory` 对每类传入
`--max-category NAME=0`，因此任何回流都会 fail-closed。standalone 默认模式仍打印 inventory，
也可对照 `scripts/meta_ownership_baseline.json` 做历史审计。证明安全的借用用行内 `owned:` 注释
（如 `/* owned: cg arena */`）豁免，永不计数。

### `check_param_mode_convergence.py`

扫描 `src/`、`stdlib/`、`tests/`、`spec/`、`demos/`、`tools/` 与 active language spec，
把 206 参数模式收敛前的事实分成 `CANON_DECL_MODE_SPELLING`、`PREFIX_DECL_MODE_SPELLING`、
`REMOVED_SYNTAX_NEGATIVE_FIXTURE`、`CALL_SITE_REF_OUT_MARKER`、`MOVE_AS_PARAM_MODE_RESIDUE`、
`STALE_MODIFIER_EBNF`、`XR_PARAM_MACRO_RESIDUE`、`PASSING_MODE_FIELD_OR_ARRAY`、
`FUNCTION_TYPE_MODE_CONSUMER`、`BACKEND_ABI_MODE_CONSUMER`、`BORROW_ESCAPE_SUSPEND_CONSUMER` 和
`ACTIVE_PUBLIC_SURFACE_PARAM_MODE_HIT`。默认模式只打印 inventory 并返回 0，便于 P0 固定
基线；后续 206 P1/P2/P3 可按类别逐步增加 fail gate。

### `check_string_surface_residue.py`

扫描 `src/`、`stdlib/`、`tests/`、`spec/`、`docs/`、`demos/`、`tools/`、`scripts/`
与 active language spec，把 191 string/byte/rune 表面残余分成
`PUBLIC_LEGACY_STRING_MEMBER_NAME`、`PUBLIC_SPAN_VIEW_DIAGNOSTIC`、
`PUBLIC_STRING_INDEX_EXAMPLE`、`PUBLIC_BACKEND_STRING_INDEX_DIAGNOSTIC`、
`BACKEND_STRING_INDEX_SUPPORT`、`ALLOWED_REMOVED_STRING_INDEX_NEGATIVE_TEST`、
`ALLOWED_REMOVED_LEGACY_STRING_MEMBER_NEGATIVE_TEST` 和
`CANONICAL_STRING_INDEX_REJECTION_TEXT`，并以非失败分类
`PATH_STRING_OWNER_SURFACE` 跟踪仍以 `string` 暴露 OS/file path 的 public API。
CTest `string_surface_residue` 同时启用 public 与 backend legacy fail gate，确保
public “Span view” 诊断、`str[i] -> rune` 示例和
`charAt/fromBytes/runesLossy/byteLength` 旧成员名不再出现在公开规范、MCP 知识库、
tooling 或 VM/AOT fallback 中；显式替代路径是 `Slice<T>`、`s.runes().nth(i)`
与 `s.bytes()[i]`。`PATH_STRING_OWNER_SURFACE` 只作为 191 Path owner 未闭环的
基线统计，直到 OS-native `Path` owner 迁移完成后再升级为 fail gate。

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
| 2026-07-14 | `bytes_type_residue` 增加 internal legacy fail gate，阻止 `xrt_bytes_*` / `emit_builtin_bytes_*` 回流 | Codex |
| 2026-07-14 | `bytes_type_residue` 增加 `XR_ERROR_CORE_BYTES_*` fail gate，阻止旧 Bytes error macro 回流 | Codex |
| 2026-07-14 | 增加 byte width predicate audit，固定 204 共享 U8 helper 与低层验证/编码边界 | Codex |
| 2026-07-14 | 增加 binary stdlib surface inventory，接 200 P0 string-binary public surface 与 consumer 基线 | Codex |
| 2026-07-14 | 增加 binary stdlib KAT baseline，接 200 P0 base64/compress/crypto corpus 与 AOT baseline 覆盖 | Codex |
| 2026-07-13 | 增加 parameter mode convergence inventory，接 206 P0 参数契约与调用授权收敛基线 | Codex |
| 2026-07-14 | 增加 unchecked error-effect convergence inventory，接 205 P0 旧 error-set / MAY_THROW / tooling metadata 分类 | Codex |
| 2026-07-14 | 增加 source unknown convergence inventory，接 202 P0 source unknown 与 typed erasure 边界分类 | Codex |
| 2026-07-15 | 增加 string surface residue 检查，接 191 string indexing 删除与公开知识库同步门禁 | Codex |

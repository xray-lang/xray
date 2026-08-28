# Blocker: the AOT program-authority guard seals 90 differential cases, and 67 of them are single-file programs

- **Lane**: 1 (差分网解封)
- **Status**: `BLOCKED`
- **Requested owner**: 4 (multi-module program authority)
- **Severity**: 90 of the 515 sealed cases probed in this lane（占 17.5%）都停在这一条
  判据上，是整个封存集合的第二大族。解开它可以让差分网对这 90 条恢复 vm/aot 双跑
  比对——但这里有一个必须先说清楚的事实：**其中 67 条（74%）是单文件程序**，只是
  因为 import 了一个有 `.xr` 源码的标准库模块，才被算成"多模块"。所以 4 号解开
  `XR_TARGET_1000` 的收益比"多模块支持"这个名字听起来要大，覆盖面也不止于真正的
  多文件工程；但同时，这 90 条解封后并不等于 90 条立刻转绿，见 `## Two things it is not`。

## Exact source identity

| item | value |
|---|---|
| base commit | `00f665c5cfcdc5f0f938ba133c42a258a640d95f` |
| worker branch | `work/1-diff-net-unseal-00f665c5c` |
| tree state | 测量时 clean at base；`src/`、`tests/diff/cases/`、`tests/regression/` 全程未被修改（已复核）。本 lane 的同批产物随后改动了 `tests/diff/known_failures*_not_comparable.txt`，那两份清单不参与编译，不影响本 packet 的任何测量。 |
| binary | `build-nofp/xray`, sha256 `e6944de685d8829c0528087318db20c34c2a52c3e65fc4eee81b3518b6d5938b` |
| version banner | `xray v0.9.2 (VM+AOT, TLS, arm64-darwin)` |
| build configuration | `cmake -S . -B build-nofp -G Ninja -DCMAKE_BUILD_TYPE=Release -DXRAY_STDLIB_VM_FASTPATHS=OFF -DXRAY_PYTHON=/opt/homebrew/bin/python3` |
| command | `build-nofp/xray build --native -O 0 <case> -o <tmp>`（与 `tests/diff/run_backend_diff.py` 的 aot 分支一致，`XRAY_AOT_TEST_OPT=0`，xi_opt 为空） |

## Minimal case

这 90 条里最小的一个，`tests/diff/cases/semantics/stdlib/os_sleep_system_direct.xr`，
54 字节，**一个用户文件，一条 import**：

```xray
import os

print("before")
os.sleep(0)
print("after")
```

去掉 `import os` 之后同样的程序可以编译并运行通过。这一条 import 就是全部差别。

## Reproduce

```
build-nofp/xray build --native -O 0 tests/diff/cases/semantics/stdlib/os_sleep_system_direct.xr -o /tmp/probe.bin
```

对照组（应当成功）：

```
printf 'print("before")\nprint("after")\n' > /tmp/noimport.xr
build-nofp/xray build --native -O 0 /tmp/noimport.xr -o /tmp/noimport.bin
```

## Expected and actual

Expected：一个 import 了标准库模块的单文件程序能走到 C emission，从而让差分网拿到
一份 AOT 侧的观测，去和 VM 侧比对。

Actual：AOT 驱动在构建 program TargetPlan 之前就拒绝。注意编译器自己打印的模块表，
入口是唯一的用户文件，第二个模块完全来自标准库：

```
[xi-native] Building: tests/diff/cases/semantics/stdlib/os_sleep_system_direct.xr
[xi-native] 2 modules (topo order):
  [0] .../stdlib/os/os.xr
  [1] .../tests/diff/cases/semantics/stdlib/os_sleep_system_direct.xr (entry)
Error: product Program TargetPlan build failed: XR_TARGET_1000: product TargetPlan requires one canonical program authority
```

对照组：

```
[xi-native] Generated 3466 bytes of C (1 functions, 1 modules in 1 unit)
```

VM 侧这 90 条全部可跑（它们进入封存集合的原因就是 AOT 单边拒绝，不是双边分歧）。

## Where the refusal is decided

这条消息在 `src/` 下只有一个发出点。

**`src/aot/xaot_driver.c:2554`**（判据），消息字面量在 2555–2556 行：

```c
if (!source_program_closure && nmodules != 1) {
    aot_bundle.error_msg =
        "XR_TARGET_1000: product TargetPlan requires one canonical program authority";
```

判据在问两件事，任意一件成立就放行：

1. `source_program_closure != NULL` — 前端是否成功发布了一份 program semantic
   closure（把整个模块图作为一个可验证的权威冻结下来）；
2. `nmodules == 1` — 或者这个程序压根只有一个模块，不需要跨模块权威。

两件都不成立就 fail-closed。关键在于 `nmodules` 数的是**模块图里的模块**，标准库
模块只要有 `.xr` 源码就会进图，所以"单文件 + 一条 import"直接就是 `nmodules == 2`。

`source_program_closure` 在 `src/aot/xaot_driver.c:2366-2375` 赋值，依次尝试两个
发布器，第一个返回 `UNSUPPORTED` 时才试第二个：

- `xa_program_semantic_closure_publish_source_module_scalar_private_leaf_call()`
- `xa_program_semantic_closure_publish_scalar_module_graph()`
  （`src/frontend/analyzer/xa_program_semantic_closure.c:1113`）

## Root cause

`xa_program_semantic_closure_publish_scalar_module_graph()` 只接受一个极窄的图族，
任何一项不符就 `UNSUPPORTED`（读 `xa_program_semantic_closure.c:1113` 起）：

- `graph->spec_count != 2` 直接 unsupported——**恰好两个模块**；
- 入口模块语法恰好 2 条语句，一条 import + 一条函数声明；
- 依赖模块语法恰好 1 条语句，且必须是 `is_exported` 的函数声明；
- import 恰好 1 个 member，且 `!import->alias`（不能有别名）；
- 入口函数体恰好 1 个 call，依赖函数体 0 个 call；
- 该 call 恰好 1 个实参，`default_arg_count == 0`，`type_arg_count == 0`；
- call 的返回类型与实参类型都必须是 `XR_EXACT_SCALAR_I64`。

没有任何真实的标准库模块能满足"依赖模块只有一条语句、且是一个导出函数"这一项：
`stdlib/os/os.xr` 有 4448 字节、约 28 个导出函数声明。所以 group A 的 67 条里没有
一条有机会走通这个发布器；它们全部落到 `nmodules != 1` 上被拒。

## Two things it is not

**它不是"多模块工程"问题。** 90 条里只有 23 条是用户写了多个 `.xr` 文件的程序，
其余 67 条是单文件。把这一族叫作 multi-module 会让工作量估计跑偏：真正的形状是
"任何 import 了带 `.xr` 源码的标准库模块的程序"，而这在差分语料里主要是单文件用例。
（三条 `import "./x" as a` 形式的用例语法上没有 `from`，容易被 grep 漏成单文件，
本 packet 已按图中实际的用户文件数复核，两种口径结果一致：67 / 23。）

**解开它不等于 90 条立刻转绿。** 这条 guard 在任何 per-symbol target authority 被
查询之前就触发，所以它**掩盖**了后面的层。仓库里既有的
`blockers/a-stdlib-multi-module-program-authority.md` 已就标准库侧测到同一现象：
绕过 program-authority 之后，程序会落到下一层的 `XR_TARGET_1003` 之类的具体拒绝上。
本 lane 的封存集合里 `XR_TARGET_1003` 已经是最大的一族（217 条），所以合理预期是
这 90 条中有相当一部分解封后会转移到 `XR_TARGET_1003`，而不是直接通过。本 packet
不对转绿条数作任何声明——它声明的是：这 90 条目前**测不出**任何 AOT 信息。

## Affected cases

全部 90 条，按子形状分组。每条标注该用例在模块图里的实际模块数，以及被拉进图的
标准库模块。

### A. 单个用户文件，只 import 标准库模块 — 67 条

模块图里用户源文件恒为 1；模块数 2 的 43 条，其余因标准库模块自身的传递依赖而更多
（3/4/6/7 模块）。这一组共牵涉 23 个不同的标准库模块，最常见的是 `os`(19)、
`path`(17)、`io`(12)、`text`(11)、`sys`(9)。

- `tests/diff/cases/basic/strings.xr` — 2 modules (stdlib: text)
- `tests/diff/cases/semantics/concurrency/condvar_compose.xr` — 2 modules (stdlib: sync)
- `tests/diff/cases/semantics/coro/semaphore_double_acquire_defer_bool_slot.xr` — 2 modules (stdlib: sync)
- `tests/diff/cases/semantics/coro/work_queue_native_methods.xr` — 2 modules (stdlib: sync)
- `tests/diff/cases/semantics/stdlib/base64_isvalid_direct.xr` — 2 modules (stdlib: base64)
- `tests/diff/cases/semantics/stdlib/base64_module.xr` — 2 modules (stdlib: base64)
- `tests/diff/cases/semantics/stdlib/base64_property_direct.xr` — 2 modules (stdlib: base64)
- `tests/diff/cases/semantics/stdlib/cluster_protocol_pure_direct.xr` — 2 modules (stdlib: cluster)
- `tests/diff/cases/semantics/stdlib/cluster_typed_control_surface.xr` — 2 modules (stdlib: cluster)
- `tests/diff/cases/semantics/stdlib/codegen_controls_shared_core.xr` — 2 modules (stdlib: codegen)
- `tests/diff/cases/semantics/stdlib/crypto_random_system_direct.xr` — 2 modules (stdlib: crypto)
- `tests/diff/cases/semantics/stdlib/crypto_timing_safe_equal_direct.xr` — 2 modules (stdlib: crypto)
- `tests/diff/cases/semantics/stdlib/csv_pure_module_direct.xr` — 3 modules (stdlib: csv, text)
- `tests/diff/cases/semantics/stdlib/datetime_core_direct.xr` — 3 modules (stdlib: datetime, time)
- `tests/diff/cases/semantics/stdlib/datetime_int_arg_shared_core.xr` — 3 modules (stdlib: datetime, time)
- `tests/diff/cases/semantics/stdlib/datetime_offset_system_direct.xr` — 3 modules (stdlib: datetime, time)
- `tests/diff/cases/semantics/stdlib/encoding_core_direct.xr` — 2 modules (stdlib: encoding)
- `tests/diff/cases/semantics/stdlib/encoding_module.xr` — 2 modules (stdlib: encoding)
- `tests/diff/cases/semantics/stdlib/encoding_property_direct.xr` — 2 modules (stdlib: encoding)
- `tests/diff/cases/semantics/stdlib/io_binary_file_boundary_direct.xr` — 4 modules (stdlib: io, os, path)
- `tests/diff/cases/semantics/stdlib/io_chmod_shared_core.xr` — 4 modules (stdlib: io, os, path)
- `tests/diff/cases/semantics/stdlib/io_path_result_shared_core.xr` — 4 modules (stdlib: io, os, path)
- `tests/diff/cases/semantics/stdlib/io_read_dir_shared_core.xr` — 4 modules (stdlib: io, os, path)
- `tests/diff/cases/semantics/stdlib/io_read_stdin_shared_core.xr` — 4 modules (stdlib: io, os, path)
- `tests/diff/cases/semantics/stdlib/io_remove_all_shared_core.xr` — 4 modules (stdlib: io, os, path)
- `tests/diff/cases/semantics/stdlib/io_system_direct.xr` — 4 modules (stdlib: io, os, path)
- `tests/diff/cases/semantics/stdlib/io_write_shared_core.xr` — 4 modules (stdlib: io, os, path)
- `tests/diff/cases/semantics/stdlib/log_level_enum_direct.xr` — 6 modules (stdlib: io, log, os, path, text)
- `tests/diff/cases/semantics/stdlib/log_pure_module_direct.xr` — 6 modules (stdlib: io, log, os, path, text)
- `tests/diff/cases/semantics/stdlib/net_copy_bidirectional_error_enum.xr` — 2 modules (stdlib: net)
- `tests/diff/cases/semantics/stdlib/net_copy_bidirectional_type_surface.xr` — 2 modules (stdlib: net)
- `tests/diff/cases/semantics/stdlib/os_exec_system_direct.xr` — 2 modules (stdlib: os)
- `tests/diff/cases/semantics/stdlib/os_sleep_system_direct.xr` — 2 modules (stdlib: os)
- `tests/diff/cases/semantics/stdlib/parallel_default_options.xr` — 2 modules (stdlib: parallel)
- `tests/diff/cases/semantics/stdlib/parallel_for_each_vm_batch.xr` — 2 modules (stdlib: parallel)
- `tests/diff/cases/semantics/stdlib/parallel_map_vm_batch.xr` — 2 modules (stdlib: parallel)
- `tests/diff/cases/semantics/stdlib/parallel_plan_map_cleanup_after_panic.xr` — 2 modules (stdlib: parallel)
- `tests/diff/cases/semantics/stdlib/parallel_plan_nested_dispatch_cleanup.xr` — 2 modules (stdlib: parallel)
- `tests/diff/cases/semantics/stdlib/parallel_reduce_vm_batch.xr` — 2 modules (stdlib: parallel)
- `tests/diff/cases/semantics/stdlib/path_isabsolute.xr` — 3 modules (stdlib: os, path)
- `tests/diff/cases/semantics/stdlib/path_module.xr` — 3 modules (stdlib: os, path)
- `tests/diff/cases/semantics/stdlib/path_parse_schema_shared_core.xr` — 3 modules (stdlib: os, path)
- `tests/diff/cases/semantics/stdlib/path_string_ops.xr` — 3 modules (stdlib: os, path)
- `tests/diff/cases/semantics/stdlib/runtime_domain_bytes.xr` — 2 modules (stdlib: runtime)
- `tests/diff/cases/semantics/stdlib/simd_portable_integer_vectors.xr` — 2 modules (stdlib: simd)
- `tests/diff/cases/semantics/stdlib/simd_top_level_shared_vectors.xr` — 2 modules (stdlib: simd)
- `tests/diff/cases/semantics/stdlib/sync_cache_padded.xr` — 2 modules (stdlib: sync)
- `tests/diff/cases/semantics/stdlib/sys_barrier_direct.xr` — 2 modules (stdlib: sys)
- `tests/diff/cases/semantics/stdlib/sys_condvar_direct.xr` — 2 modules (stdlib: sys)
- `tests/diff/cases/semantics/stdlib/sys_dylib_direct.xr` — 4 modules (stdlib: os, path, sys)
- `tests/diff/cases/semantics/stdlib/sys_mutex_direct.xr` — 2 modules (stdlib: sys)
- `tests/diff/cases/semantics/stdlib/sys_once_direct.xr` — 2 modules (stdlib: sys)
- `tests/diff/cases/semantics/stdlib/sys_rwlock_direct.xr` — 2 modules (stdlib: sys)
- `tests/diff/cases/semantics/stdlib/sys_thread_const_function_value.xr` — 2 modules (stdlib: sys)
- `tests/diff/cases/semantics/stdlib/sys_thread_os_primitives_cross_thread.xr` — 2 modules (stdlib: sys)
- `tests/diff/cases/semantics/stdlib/sys_thread_utils_direct.xr` — 2 modules (stdlib: sys)
- `tests/diff/cases/semantics/stdlib/text_contract_direct.xr` — 2 modules (stdlib: text)
- `tests/diff/cases/semantics/stdlib/toml_pure_module_direct.xr` — 6 modules (stdlib: io, os, path, text, toml)
- `tests/diff/cases/semantics/stdlib/url_core_direct.xr` — 2 modules (stdlib: url)
- `tests/diff/cases/semantics/stdlib/ws_close_code_fastpath_direct.xr` — 7 modules (stdlib: base64, crypto, net, text, time, ws)
- `tests/diff/cases/semantics/stdlib/ws_connect_options_type_surface.xr` — 7 modules (stdlib: base64, crypto, net, text, time, ws)
- `tests/diff/cases/semantics/stdlib/ws_pure_protocol_direct.xr` — 7 modules (stdlib: base64, crypto, net, text, time, ws)
- `tests/diff/cases/semantics/stdlib/yaml_pure_module_direct.xr` — 6 modules (stdlib: io, os, path, text, yaml)
- `tests/diff/cases/semantics/string/string_surface_declared_methods.xr` — 2 modules (stdlib: text)
- `tests/regression/11_coroutine/1114_select_after.xr` — 2 modules (stdlib: time)
- `tests/regression/11_coroutine/1158_work_queue_native.xr` — 2 modules (stdlib: sync)
- `tests/regression/11_coroutine/1165_result_group_native.xr` — 2 modules (stdlib: sync)

### B. 只 import 同级文件（真正的多文件程序） — 18 条

- `tests/diff/cases/semantics/modules/cleanup_init_error.xr` — 2 modules (no stdlib module)
- `tests/diff/cases/semantics/modules/cleanup_init_import_once.xr` — 4 modules (no stdlib module)
- `tests/diff/cases/semantics/modules/cleanup_init_panic.xr` — 2 modules (no stdlib module)
- `tests/diff/cases/semantics/modules/cross_module_borrowed_arg_release.xr` — 2 modules (no stdlib module)
- `tests/diff/cases/semantics/modules/exported_struct_methods.xr` — 2 modules (no stdlib module)
- `tests/diff/cases/semantics/modules/multimod_calls.xr` — 2 modules (no stdlib module)
- `tests/diff/cases/semantics/modules/xmod_class.xr` — 2 modules (no stdlib module)
- `tests/diff/cases/semantics/modules/xmod_coro.xr` — 2 modules (no stdlib module)
- `tests/diff/cases/semantics/modules/xmod_dead_net_helper.xr` — 3 modules (stdlib: net)
- `tests/diff/cases/semantics/modules/xmod_inherit.xr` — 2 modules (no stdlib module)
- `tests/diff/cases/semantics/modules/xmod_nested_class_field.xr` — 2 modules (no stdlib module)
- `tests/diff/cases/semantics/modules/xmod_private_const_defaults.xr` — 2 modules (no stdlib module)
- `tests/diff/cases/semantics/modules/xmod_same_name_classes.xr` — 3 modules (no stdlib module)
- `tests/diff/cases/semantics/modules/xmod_same_name_methods.xr` — 3 modules (no stdlib module)
- `tests/diff/cases/semantics/modules/xmod_static.xr` — 2 modules (no stdlib module)
- `tests/diff/cases/semantics/modules/xmod_static_defaults.xr` — 2 modules (no stdlib module)
- `tests/diff/cases/semantics/slice/slice_borrow_return_cross_module.xr` — 2 modules (no stdlib module)
- `tests/regression/11_coroutine/1149_module_call_stack_grow.xr` — 2 modules (no stdlib module)

### C. 同级文件 + 标准库混合 — 5 条

注意其中几条标为 "no stdlib module"：它们 import 的标准库模块（如 `mem`）是纯原生
模块、没有 `.xr` 源码，因此**不会进入模块图**。这类程序的 `nmodules` 完全由同级
用户文件决定。这也是判断一条 import 会不会把程序推成多模块的实际判据：被 import
的模块有没有 `.xr` 源码。

- `tests/diff/cases/semantics/ffi/extern_layout_bytecode_roundtrip.xr` — 2 modules (no stdlib module)
- `tests/diff/cases/semantics/ffi/mem_view_imported_layout_native_size.xr` — 2 modules (no stdlib module)
- `tests/diff/cases/semantics/modules/value_struct_return_release.xr` — 3 modules (stdlib: runtime)
- `tests/diff/cases/semantics/modules/xmod_ptr_offset_imported_const.xr` — 2 modules (no stdlib module)
- `tests/diff/cases/semantics/stdlib/simd_intrinsic_identity_alias.xr` — 3 modules (stdlib: simd)

## Requested capability

一个能发布**任意无环模块图**的 program semantic closure：不限模块数、不限导出数、
允许 class / enum / 泛型导出、允许模块状态、允许带默认参数与类型参数的调用。

按本 lane 的数据，如果只能做增量，**优先级最高的是"入口是单个用户文件、依赖全部
来自标准库"这一形状**（67 条，占本族 74%）——它不需要跨用户模块的权威，只需要把
标准库模块作为已分析的依赖接纳进 closure。把 `spec_count != 2` 这一条放宽到 N，
以及把"依赖模块恰好一条导出函数语句"放宽到"任意导出集合"，是覆盖这 67 条的最小
改动方向。

## Files deliberately not modified

- `src/aot/xaot_driver.c` — 判据所在，属 4 号 owner。
- `src/frontend/analyzer/xa_program_semantic_closure.c` — 发布器所在，属 4 号 owner。
- `tests/diff/known_failures_not_comparable.txt` — 这 90 条目前均已在册且已标注
  `XR_TARGET_1000`；解封应由 4 号的改动落地后统一摘除，本 lane 不预先改动。

本 lane 未修改 `src/` 下任何文件；上述全部结论均为只读测量。

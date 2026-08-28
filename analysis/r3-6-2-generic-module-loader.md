# 通用模块加载路径：30 个 per-module C 工厂如何归零

- **Lane**：6-2（round 3）
- **基线**：`34be0379c`
- **分支**：`work/6-2-generic-loader-34be0379c`
- **构建**：`-DCMAKE_BUILD_TYPE=Release -DXRAY_STDLIB_VM_FASTPATHS=OFF -DXRAY_PYTHON=/opt/homebrew/bin/python3`

## 0. 门的变化

| component gate | 前 | 后 |
|---|---|---|
| `stdlib_no_module_specific_c_loader` | FAIL，30 | **PASS，0** |
| `stdlib_full_xray_source_coverage` | FAIL，1（math） | FAIL，1（math） |
| `stdlib_no_whole_module_native_policy` | FAIL，1（math） | FAIL，1（math） |
| `stdlib_no_public_native_surface` | FAIL，128 | FAIL，128 |
| `stdlib_native_leaf_allowlist` | PASS | PASS |
| `stdlib_no_handwritten_c_semantic_owner` | FAIL，844 | FAIL，844 |
| `stdlib_generated_c_reproducibility` | UNRUN | UNRUN |
| `stdlib_unified_target_plan_coverage` | UNRUN | UNRUN |

只有这条 lane 负责的数字动了，其余七条一字未变。`test_xi_cgen` 前后都是 `PASS 42`：
**本 lane 一行 core.def 都没改**，所以 `stdlib_registry_fingerprint` 没动，全树 plan
指纹与 stable id 也就没有跟着移动，00-SHARED §「改 core.def = 移动全树指纹」描述的
重锚动作在这条 lane 上完全不需要。

## 1. 门问的到底是什么

门自己的 docstring 说得很清楚，值得原样记住：

> A per-module C loader keeps the module's registration in C even once its
> bodies are Xray, so a generic source-derived load path has to replace every
> one of them.

关键词是 **source-derived**。它要的不是「再迁几个模块到 `.xr`」，而是
「注册这件事必须从声明派生」。所以 30 个里包含 `net` / `io` / `log` / `_probe` 这些
**已经是 `xray_semantic`** 的模块——它们的语义早就在 `.xr` 里，可加载性却仍然钉在 C 上。

## 2. 起点比看上去近：通用路径已经存在

`os` 是唯一没有工厂的模块，它证明了通用路径可以走通。原来的
`load_stdlib_module()` 是个二选一：

```c
XrModule *module = factory ? factory(isolate) : xr_module_create_native(isolate, module_name);
if (module && !factory) module->requires_script = true;
...
if (!factory && !xr_stdlib_embedded_private_leaves_install(isolate, module, module_name)) { ... }
```

也就是说「无工厂」这条分支已经完整。真正的工作是把 30 个工厂做的事**全部**表达成
声明，然后删掉那条 `factory ?` 分支。

### 2.1 30 个工厂的实际分布

逐个读完之后，形状比预想的集中：

| 类别 | 个数 | 模块 |
|---|---|---|
| 纯样板（`create_native` + `requires_script = true`，一字不差） | 15 | `_probe` `codegen` `text` `log` `parallel` `simd` `url` `datetime` `encoding` `http` `path` `toml` `xml` `yaml` `ws` |
| 只多一句 `xr_stdlib_vm_bind_<mod>_generated` | 9 | `time` `runtime` `compress` `crypto` `math` `sys` `regex` `io` `cluster` |
| 另有一个真实动作 | 5 | `mem` `sync` `http2` `net` `test_yield` |
| isolate 级初始化 | 1 | `prelude` |

前 15 个的 `.c` 文件**只含这一个函数**，所以它们不是「删函数」而是「删文件」。

## 3. 三个不能从既有声明推出的动作

`sync` / `test_yield` / `mem` 的动作没法从 core.def 或 `.xr` 推出来，所以在
`stdlib/stdlib_boundary.toml` 里各加一种声明行，由 stdlibgen 生成到同一个 binder：

| 声明 | 用户 | 它替代的手写 C |
|---|---|---|
| `native_type_exports` | `sync`（5 项） | `sync_export_native_class(...)` ×5 |
| `native_fn_exports` | `test_yield`（13 项） | 13 个 `XRS_EXPORT*` |
| `load_time_classes` | `mem`（`Buffer`） | `xr_stdlib_vm_register_buffer_class_generated(isolate)` |

选 boundary.toml 而不是 core.def 有两个具体理由：**core.def 的条目会进
registry fingerprint**（改它就要重锚全树 KAT），而且会同时进 analyzer builtins、
LSP 表和文档；`test_yield` 是 `public = false` 的测试桩，让它出现在语言表面是错的。
boundary.toml 本来就是「标准库所有权与边界」的声明源，`factory_source` 原本就住在这里。

### 3.1 `sync` 的 5 个类不是 core.def 的 native_class

容易搞混的一点：core.def 里 `module sync { }` 块**根本不存在**。
`Semaphore` / `CountdownLatch` / `EventCount` / `WorkQueue` / `ResultGroup` 是
调度器注册的 VM 内建类型（`XR_TSEMAPHORE` 等），工厂做的只是
`xr_isolate_get_native_type_class(isolate, type_id)` 拿到 `XrClass` 再导出。
所以声明里需要的是 **builtin type id**，不是类定义。

对应的通用 helper 是新加的 `xr_module_export_native_type_class()`
（`src/module/xmodule.c`）：类不存在时**不导出**，让 binder 的计数校验失败，
从而 fail-closed，而不是发布一个半拉子模块。

## 4. 三个不需要声明、直接搬回正确位置的动作

这三个原本挂在工厂上，但它们本来就不属于「模块加载」这一层：

**`prelude` 的三步**（`isolate->prelude_symbols`、`xr_prelude_register_all_native_types`、
`xr_prelude_register_builtin_enums`）全是 isolate 状态，而且是**每个模块加载都已经
假定就位**的状态。改成 `xr_prelude_install(isolate)`，在 `xisolate_full.c` 里
**先于** `xr_module_system_init()` 调用。`prelude` 模块本身退化成描述符表里一行
`{"prelude", NULL, NULL}`，唯一作用是让显式 `import prelude` 解析成 no-op
（MCP runner 的 allowlist 依赖这个名字存在，见 `src/app/mcp/xmcp_tools_run.c:37`）。

**`http2` 的 `native_handle_destroy`** 移进 `http2_get_context()`：

```c
if (!module->native_handle) {
    module->native_handle = xr_calloc(1, sizeof(XrHttp2Context));
    module->native_handle_destroy = http2_context_destroy;
}
```

析构器和它析构的东西现在在同一处设置，比加载期设置更不容易漏。

**`net_platform_init()`** 在非 Windows 上是空函数，在 Windows 上是
`xr_once_call(&g_net_winsock_once, ...)`。它下沉到 net 的 6 个入口
（3 处 `xr_dns_resolve*`、`xr_io_listen`、UDP `socket()`、connect 路径），
因为这些里任何一个都可能是程序第一个碰到的 socket API。`xr_once_call` 幂等，
代价是一次原子读。

## 5. binder 统一成一种形状

原来 stdlibgen 按 `derive_private_leaf_modules()` 分两种：

- 「VM 行全私有」的模块 → `XR_FUNC bool`，带 `expected_count` 校验
- 其余 → `static void`，无校验

`static void` 的那五个（`math` `mem` `regex` `sys` `time`）**根本无法被一张表引用**，
也无法报告失败。现在统一成 `XR_FUNC bool` + 计数校验，`derive_private_leaf_modules`
换成 `derive_binder_modules(root)`：core.def 有 fn/const 行的模块 ∪ boundary.toml
声明了上述三种行的模块，共 15 个。

计数把 `.def` 的 fn 行、const 行、`native_type_exports`、`native_fn_exports`
一起算进去，所以任何一条没装上都会让加载失败，而不是静默少一个导出。

## 6. 描述符表：可加载性的唯一答案

`XrEmbeddedStdlibSource`（嵌入源表）升级成 `XrStdlibModuleDescriptor`：

```c
typedef struct {
    const char *name;
    const char *source;                       /* NULL = 尚无 .xr 语义源 */
    XrStdlibNativeEntryBinder bind_native_entries;  /* NULL = 无原生条目 */
} XrStdlibModuleDescriptor;
```

`requires_script` 不再由每个工厂各自决定，而是 `descriptor->source != NULL` —— 
有源就必须找到脚本层，否则 fail 而不是发布空导出表。

行集合 = CMake 选中的 `.xr` 源 ∪ 无源但可加载的模块。后者由 CMake 显式声明
（`XRAY_STDLIB_SOURCELESS_MODULES`），因为**构建才知道哪些模块 TU 被编译**——
binder 定义在模块自己的 TU 里（`XR_STDLIB_VM_BIND_MODULE_<X>` 宏 + include 生成片段），
表里写一个没编译进来的 binder 名字会直接链接失败。这与 CMake 已有的
`XRAY_STDLIB_SCRIPT_SOURCES` 按 feature gate 过滤 `.xr` 列表是同一套逻辑，
那段注释本来就写着「a source file that merely exists in the repository must not
make a disabled module loadable」。

当前配置（`XR_STDLIB_FULL=ON`、`XR_BUILD_TEST_MODULES=OFF`）下表是 32 行：
30 个 `.xr` 模块 + `math`（无源有 binder）+ `prelude`（无源无 binder）。

模块解析器（`xmodule_resolver.c`）也改用同一张表判定「这是不是 stdlib 模块名」，
`registry->native_factories` 这个 hashmap 连同 `xr_module_register_native_factory`、
`xr_module_register_stdlib`、`stdlib_core[]` 等静态表、`xmodule_factories.h`
一起删除。

## 7. 门脚本随之变的地方

`scripts/stdlib_manifest.py`
- `registry_modules()`（从 `xmodule.c` 扫工厂符号）**删除**
- `private_leaf_binder_modules()` → `native_entry_binder_modules()`，改问 stdlibgen
- `loadable_modules()` = 有 `.xr` ∪ 有 binder ∪ 声明了 `sourceless_loadable`

最后那项是 `prelude` 独有的：既无源也无原生条目，这件事**没有任何东西能推出来**，
只能声明。

`scripts/check_stdlib_boundary.py`
- 删掉全部 `factory_source` / `factory_symbol` 一致性检查
- `manual_public_native` 原本是**读工厂源码文本 grep `"符号名"`**；现在改成
  「必须出现在 `native_type_exports` / `native_fn_exports` 的 name 里」——
  声明本身就是注册，比 grep 源码强
- 新增 `check_loader_declarations()`：`builtin_type` 必须形如 `XR_T*` 且能在
  `src/**/*.h` 里找到；`native_fn_exports` 的 `vm` 必须在该模块
  `private_native_sources` 匹配到的文件里出现；`load_time_classes` 必须是该模块
  在 core.def 里声明过的 `native_class`
- `check_builtin_distribution()` 里 cluster/http2/compress/crypto 的
  「bare-name factory registration」改为「必须有生成的 binder」

`scripts/check_binary_public_native_readiness.py`
- `RETAINED_STDLIB_MODULES` 从「工厂文件必须存在」改为「`private_native_sources`
  必须匹配到文件」——这些模块保留原生实现，要证明的是那份 C 还在，不是工厂还在

## 8. 副产品：14 个空头文件

删掉工厂声明之后，`stdlib/{datetime,encoding,http,log,math,net,runtime,test_yield,time,toml,url,ws,xml,yaml}/*.h`
只剩版权头和 include guard。全部删除，并从 4 个 `.c` 里摘掉自引 include。

`stdlib/time/time.c` 的 `#include "time.h"` **必须**跟着删：文件不在了之后，
`"time.h"` 会 fallback 到系统 `<time.h>`，那是个静默的语义变化。

## 9. 基线上就红、与本 lane 无关的

聚焦门 `ctest -R 'stdlib|boundary|manifest|eval_stdlib|embedded'` 共 38 项，8 项失败。
逐条归因如下——**判定方法是把基线树用 `git archive HEAD` 导出到 scratchpad
再跑同一份脚本**，不是靠推理：

| 失败项 | 归因 | 证据 |
|---|---|---|
| `eval_stdlib_overlay` | 基线红 | `xray run: stdin source requires --module-id`，与 stdlib 无关；6-2 简报已列 |
| `stdlib_embedded_layout` | 基线红 | 同一条 stdin 错误；6-2 简报已列 |
| `binary_stdlib_kat_baseline` | 基线红 | 基线树跑同脚本，`core_base64.expect` / `core_encoding.expect` 的 missing anchors **逐字相同** |
| `binary_stdlib_runtime_baseline` | 基线红 | 同上，同样两个文件同样两条 |
| `aot_link_command_manifest` | 基线红 | `XR_TARGET_1003` / `XR_TARGET_1000`；`analysis/r3-6-stdlib-selfhost-remaining-six.md` §3.1 已记录它在基线上失败 |
| `aot_manifest_sweep` | 与本 lane 无关 | 618 个用例中 3 个失败，全是 `tests/diff/cases/spread/*`，这三个文件**一个 import 都没有**，报的是 spread 表达式的 representation schema 问题 |
| `native_output_boundary` | 基线红 | `XR_TARGET_1000: product TargetPlan requires one canonical program authority`，模块图是 `mem.xr` + 入口两个模块 |
| `backend_diff_embedded` | 负载超时，串行复跑后**零回归**（见 §9.2） | 两边各跑一次完整差分：只在基线通过的用例 0 个 |

`check_stdlib_boundary.py` 还剩三条 Iterator 错误
（`Iterator declaration methods must be exactly hasNext/next/nth`、
`compiler Iterator method table must match stdlib/types/iterator.xr`、
`API inventory does not expose the complete Iterator schema`）。
基线树跑同一脚本，输出**逐字相同**。

### 9.0 运行期实测（不是推理）

改动后用 `build-nofp/xray` 实跑，每一条特殊路径都单独验证过：

| 验证的东西 | 怎么验的 | 结果 |
|---|---|---|
| `mem` 的 `Buffer` 类在模块加载时注册（从工厂移进 binder） | `mem.alloc(32).asBytes()` | `len == 32` |
| `sync` 的 5 个 `native_type_exports` | 逐个构造 `Semaphore` / `CountdownLatch` / `EventCount` / `WorkQueue` / `ResultGroup` | 全部构造成功 |
| 15 个纯样板模块走通用路径 | `import path/log/url/datetime/encoding/toml/text` 并调用 | 正常 |
| `math` 作为「无源有 binder」行 | `math.sqrt(81.0)` | `9.0` |
| `prelude` 作为「无源无 binder」行 | `import prelude` | no-op，不报错 |
| 叶子绑定未受影响 | `compress.crc32` / `crypto.sha256` / `os.getpid` / `regex.compile` / `mem.sizeOf<i64>` | 全部正常 |

`test_yield` 需要 `XR_BUILD_TEST_MODULES=ON`，所以两边**各建了一个 test-modules 二进制**
（源外构建，不动 `build-nofp`）做 A/B：

- 描述符表确实多出 `{"test_yield", NULL, xr_stdlib_vm_bind_test_yield_generated}`
- `test_yield_scheduler_contract.xr`：两边输出都是 `100 / 42 / 42 / 0 / 1 / 2 / 2`
- `test_yield_selective_import.xr`：两边都报同样的 `E0350: stdlib module 'test_yield'
  has no member 'add'`。原因是 `test_yield` 不在 core.def，analyzer 因此没有它的
  成员表，`import { add } from test_yield` 这条命名导入路径查不到——**这在基线上
  就是这样**，与 `native_fn_exports` 无关。

### 9.2 差分套件的 A/B：零回归

`backend_diff_embedded` 在聚焦门里是 `Timeout`（当时 load 59），按 00-SHARED §4 必须
串行复跑后才能归因。做法是**两边各跑一次完整的 `run_backend_diff.py`**——基线树用
`git archive HEAD` 导出并单独构建了一个二进制，两次都绕开 ctest 的 900s 上限：

| | 基线 | 本分支 |
|---|---|---|
| Results | 103 passed, 0 failed, 572 refused, 1 skipped | 150 passed, 0 failed, 525 refused, 1 skipped |
| `Cases that stopped building` | 59 | **14** |
| 用例总数 | 610 | 610 |

逐用例做集合差，**只在基线通过的用例是 0 个**——没有任何一个用例从通过变成不通过。
`Cases that stopped building` 我这边的 14 条是基线 59 条的**真子集**。

反方向的 47 个差异（我这边多通过 47 个）**不是本 lane 的功劳**，是环境噪声：基线树跑在
`/private/tmp` 下、只构建了 `xray` 目标因而缺 `libxray_compiler.a`，而 AOT 的
`native-run` 探针在负载下会 `probe stage timed out after 10000 ms`（基线 114 次，
本分支 108 次）。噪声的方向对本分支有利，但要判定的是回归，而回归数为零。

`Listed cases now build` 基线 2 条、本分支 4 条。这是 ratchet 在说
`tests/diff/known_failures_not_comparable.txt` 里有条目已经不需要了。
**本 lane 没有动那份清单**：多出来的两条同样落在上面那批噪声里，
而清单「只能缩不能加」的规矩要求删除有确切依据，不是在一次带噪声的跑里顺手删。

### 9.1 为什么模块加载的改动不可能改变 AOT 的拒绝

值得单独写下来，因为它决定了上表最后两行怎么归因：**AOT 根本不走这条改动**。

`xray build --native` 的模块图由 `src/module/xmodule_graph.c` 在编译期构建，
读的是 `.xr` 源；本 lane 改的 `load_stdlib_module()` 是 VM 运行期的加载路径。
两者唯一的交点是 `xmodule_resolver.c` 里「这个裸名字是不是 stdlib 模块」的判定，
而那个判定前后**集合相同**：

- 改前：`has_native_factory`（工厂表 30 个）`|| has_embedded_source`（`.xr` 30 个）
- 改后：描述符表 = 30 个 `.xr` ∪ 有 binder 的模块 ∪ `prelude`

两边都是 32 个（`test_yield` 两边都因 `XR_BUILD_TEST_MODULES=OFF` 不在）。
`mem.xr` 在基线 `34be0379c` 上就已经是 `.xr` 模块，所以
`native_output_boundary` 的模块图在改动前后都是两个模块，撞的都是 lane 4 的
`blockers/a-stdlib-multi-module-program-authority.md`。

## 10. 顺手清掉的与留给别人的

`contracts/stdlib-symbol-inventory.json` 已经**整体重算**（`--json` 重新导出，
`base` 记为本 lane 的提交）。原因是它的 `factory_source` 字段有 30 条指向已删除的
文件，而字段本身描述的东西也不存在了；模块行现在带 `loader_declarations`。
重算同时清掉了这份文件在基线上就带着的 `time` 迁移 drift
（见 `analysis/r3-6-stdlib-selfhost-remaining-six.md` §3）。
没有门读这份 JSON——`check_stdlib_full_xray_completion.py` 是 `import` inventory
模块重新扫描，不读快照——所以这是把记录改回它所记录的东西。

留给别人的：

- `tests/aot/filetests/link/core_datetime.expect:20` 有一行
  `c_not_contains=xr_native_module_create_datetime`。它现在**更**成立了
  （符号已彻底不存在），不需要动。
- `math` 仍是唯一没有 `.xr` 语义源的生产模块，它在描述符表里以「无源有 binder」
  的形式存在。6-3 号 lane 让它长出 `.xr` 之后，把它从 CMake 的
  `XRAY_STDLIB_SOURCELESS_MODULES` 里去掉即可，其余不用动。

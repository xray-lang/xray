# Blocker: the embedded-bytecode lane writes a stdlib module's identity string into the bundle as its import name, so every `xray build` binary that imports a source-backed stdlib module dies at startup

- **Lane**: 1 (差分网解封)
- **Status**: `BLOCKED`
- **Requested owner**: H (compiler / module loader)
- **Severity**: 这不是两条 coro 用例的问题。默认 `xray build`（非 `--native`，即 embed 形式）
  产出的二进制，只要程序 import 了任何一个**带 `.xr` 源码层的标准库模块**，就会在启动时
  失败退出。本 lane 实测 8 个标准库模块（`time` `io` `os` `sys` `text` `base64` `log` `path`）
  8/8 复现，7 条真实差分用例 7/7 复现。差分网里 `tests/diff/cases/` 共 649 条，其中 97 条
  import 了标准库模块；在 base commit 上就有 **86 条既没被 embed 封存清单收录、也没被
  `known_failures_embedded.txt` 收录**——而后者是空的（0 条），意味着 embed 车道在设计上
  应当全绿。也就是说 **embed 车道在 base commit 上已经是大面积红的**，本 lane 的解封只是
  把 86 条变成 88 条。受影响面不止差分网：`tests/regression/` 下另有 106 条用例 import 了
  标准库模块。

## Exact source identity

| item | value |
|---|---|
| base commit | `00f665c5cfcdc5f0f938ba133c42a258a640d95f` |
| worker branch | `work/1-diff-net-unseal-00f665c5c` |
| tree state | 仅 `tests/diff/known_failures_embedded_not_comparable.txt` 与 `tests/diff/known_failures_not_comparable.txt` 被本 lane 改动（`git status --short` 只有这两行）。这两份清单不参与编译。`src/`、`stdlib/`、`tests/diff/cases/`、`tests/regression/` 全程未被本 packet 修改。下文所有"base commit 上就已经红"的判断都用 `git show HEAD:<清单>` 复核过，不依赖工作区的改动。 |
| binary | `build-nofp/xray`, sha256 `e6944de685d8829c0528087318db20c34c2a52c3e65fc4eee81b3518b6d5938b` |
| version banner | `xray v0.9.2 (VM+AOT, TLS, arm64-darwin)` |
| build configuration | `cmake -S . -B build-nofp -G Ninja -DCMAKE_BUILD_TYPE=Release -DXRAY_STDLIB_VM_FASTPATHS=OFF -DXRAY_PYTHON=/opt/homebrew/bin/python3` |
| command | `XRAY_DIFF_BACKENDS=vm,embed XRAY_DIFF_SINGLE_CASE=<case> XRAY_TOOLCHAIN_PROBE_SCALE=4 /opt/homebrew/bin/python3 tests/diff/run_backend_diff.py build-nofp/xray` |

## Minimal case

不需要 coroutine，不需要 channel，不需要 timeout。两行：

```xray
import time
print(time.monotonic() >= 0)
```

把 `time` 换成 `io` / `os` / `sys` / `text` / `base64` / `log` / `path` 里任何一个，
现象完全一样，只是错误消息里的模块名跟着换。去掉那一行 `import`，同样的程序
`xray build` 出来的二进制正常运行。

## Reproduce

VM 正常，构建成功，产物启动即死：

```console
$ ./build-nofp/xray run /tmp/t_time.xr
true

$ ./build-nofp/xray build -o /tmp/t_time /tmp/t_time.xr
Modules: 2
  stdlib-module-v1:module=4:time:path=12:time/time.xr (0 bytes)
  /tmp/t_time.xr (4869 bytes)
Linking: cc -O3 -ffp-contract=off -o /tmp/t_time ...
Generated: /tmp/t_time

$ /tmp/t_time

Error: Package 'stdlib-module-v1:module=4:time:path=12:time/time.xr' not found

Please install dependency first:
  xray pkg add stdlib-module-v1:module=4:time:path=12:time/time.xr

[WARNING] [bytecode] failed to initialize bundled module
  'stdlib-module-v1:module=4:time:path=12:time/time.xr' (xproto_codec.c:2442)
$ echo $?
1
```

注意 `xray build` 自己打印的模块表已经把病灶摆在明面上：bundle 里第一条目的
**路径就是身份串本身**，而且 **0 bytes**。

差分网里稳定复现 3/3（三次连跑，输出逐字节相同）：

```console
$ XRAY_DIFF_BACKENDS=vm,embed \
  XRAY_DIFF_SINGLE_CASE=tests/diff/cases/semantics/coro/channel_timeout_after_sleep.xr \
  XRAY_TOOLCHAIN_PROBE_SCALE=4 \
  /opt/homebrew/bin/python3 tests/diff/run_backend_diff.py build-nofp/xray
  tests/diff/cases/semantics/coro/channel_timeout_after_sleep.xr   FAIL (exit code (vm=0 embed=1))
      vm: rc=0  stdout: Recv.Value(11)|sent|recv-value
      embed: rc=1  stdout:
      embed: |Error: Package 'stdlib-module-v1:module=4:time:path=12:time/time.xr' not found|...
```

## Expected and actual

| | expected | actual |
|---|---|---|
| `xray run`（VM） | rc=0，四行输出 | rc=0，`Recv.Value(11)` / `sent` / `recv-value` / `empty-timeout` — **符合预期** |
| `xray build` | 成功 | 成功（rc=0），并未拒绝 |
| 产物运行 | rc=0，同样四行输出 | rc=1，stdout 全空，stderr 报 `Package '...' not found` |

VM 侧完全正确。embed 侧的分歧**不在**序列化/反序列化往返上，也不在 coroutine 语义上——
程序体一个字节都没来得及执行。

## Where the refusal is decided

消息本体在 **`src/module/xmodule.c:1318`**：

```c
fprintf(stderr, "\nError: Package '%s' not found\n\n", module_name);
```

这是**运行期**、在产出的二进制里抛的，不是构建期。三点证据：

1. 它在 `xr_module_import(XrVMRuntime *isolate, const char *module_name)`
   （`src/module/xmodule.c:1273`）里，第一个形参是 isolate，是运行时 API。
2. `xray build` 自身 rc=0 并打印了 `Generated:`；错误只在执行 `/tmp/t_time` 时出现。
3. 伴随的 `[WARNING] [bytecode] failed to initialize bundled module` 来自
   `src/module/xproto_codec.c:2441`，那是 bundle 的运行期装载循环。

走到这一行的路径（`src/module/xmodule.c:1312-1320`）：

```c
char *path = xr_module_resolve_path(isolate, module_name);
if (!path) {
    if (strchr(module_name, '/') && module_name[0] != '.' && module_name[0] != '/') {
        fprintf(stderr, "\nError: Package '%s' not found\n\n", module_name);
```

分支判据是"名字里含 `/` 且不以 `.` 或 `/` 开头 ⇒ 第三方包"。身份串
`stdlib-module-v1:module=4:time:path=12:time/time.xr` 的尾段 `time/time.xr` 恰好带 `/`，
于是被误判成第三方包，才有了那句莫名其妙的 `xray pkg add`。**这句提示是误导性的，
装什么包都没用**——它只是分支猜错了。

## Root cause

一句话：**bundle 用 `spec->canonical`（模块身份串）当作条目名写盘，而运行期装载循环把
这个条目名当作 import 名传给 `xr_module_import`；身份串不是模块名，查不到。**

链路四步：

**1. 解析期造出身份串。** `resolve_stdlib`（`src/module/xmodule_resolver.c:217-269`）为标准库
模块生成 `out_id->canonical`，格式由 `src/module/xmodule_identity.c:234` 定义：

```c
return build_framed_identity("stdlib-module-v1:module=", namespace_id, ":path=",
                             logical_path, identity_out);
```

即 `stdlib-module-v1:module=<len>:<name>:path=<len>:<name>/<name>.xr`。同时
`out_id->authority.namespace_id` 保存了**纯模块名** `"time"`。

**2. 打包期把身份串写成条目路径，且不写字节码。** `bundle_compile_graph`
（`src/module/xbundle.c:161-165`）：

```c
if (spec->kind == XR_MOD_STDLIB) {
    if (!bundle_add_entry(bundle, spec->canonical, NULL, 0, spec->kind))
        goto cleanup;
    continue;
}
```

注意这里传的是 `spec->canonical` 而**不是** `spec->authority.namespace_id`，而且
`bc = NULL, bc_size = 0`。后者本身是**合理设计**——标准库不需要随程序打包，运行时
自带（见第 4 点）；问题只出在**用什么名字去要它**。这个条目随后由
`xr_bundle_to_c_source`（`src/module/xbundle.c:440-447`）原样写进生成的 C：

```c
{"stdlib-module-v1:module=4:time:path=12:time/time.xr", NULL, 0},
```

**3. 运行期拿条目路径当模块名去 import。** `xray_vm_eval_bundle`
（`src/module/xproto_codec.c:2436-2445`）：

```c
for (size_t i = 0; i < bundle->module_count; i++) {
    if (i == bundle->entry_index)
        continue;
    XrValue value = xr_module_import(X, bundle->modules[i].path);   // ← 传的是身份串
```

**4. 查表全部按纯模块名建索引，于是必然落空。** `xr_module_import` 先试
`load_stdlib_module`（`src/module/xmodule.c:1076-1084`）：

```c
XrNativeModuleFactory factory =
    (XrNativeModuleFactory) xr_hashmap_get(registry->native_factories, module_name);
bool has_embedded_source = xr_get_embedded_stdlib(module_name) != NULL;
if (!factory && !has_embedded_source)
    return NULL;
```

`native_factories` 与 `xr_get_embedded_stdlib`（`src/module/stdlib_embedded.c:76`，
经 `find_embedded_source` 逐条 `strcmp(entry->name, module_name)`）**都以纯名 `"time"`
为键**。传进来的是 74 字节的身份串，两边都 miss，返回 NULL；接着
`xr_module_resolve_path` 也解不出路径，就掉进第 1318 行那个第三方包分支。

**关键佐证：运行时其实什么都不缺。** `build-nofp/generated/stdlib_embedded_sources.inc`
里 26 个标准库模块（`_probe base64 cluster codegen crypto csv datetime encoding http io
log net os parallel path runtime simd sync sys text time toml url ws xml yaml`）都在，
`"time"` 赫然在列。二进制里躺着 `time/time.xr` 的源码，只是没人用对的名字去要它。

**同一份代码在别处做对了。** `xr_module_graph_preload`（`src/module/xmodule_graph.c:823-826`）
面对同样的问题时正确地还原了纯名：

```c
const char *import_name = (spec->kind == XR_MOD_STDLIB && spec->authority.namespace_id)
                              ? spec->authority.namespace_id
                              : spec->source_path;
XrValue value = xr_module_import(X, import_name);
```

bundle 这条路径丢掉了这层映射：`XrBundleEntry` 根本没有存 `namespace_id` 的字段，
所以到了运行期已经无从还原。

## Why it surfaced now

分两层，必须分开讲，否则会把账算错。

**第一层：这两条 coro 用例为什么今天才暴露。**
`tests/diff/cases/semantics/coro/channel_timeout_after_sleep.xr` 与
`channel_send_timeout_partner_arrives.xr` 原本登记在
`tests/diff/known_failures_embedded_not_comparable.txt`（base commit 的第 37、39 行）。
那份清单的语义写在它自己的表头里——被登记的用例是"默认 `xray build` 拒绝产出二进制"的,
"the run compared nothing and reached no verdict"。也就是说差分网**从来没有跑到过它们的
输出比对**：构建就停了，比对无从谈起。本 lane 在 `build-nofp` 上逐条复验原有 68 条时
发现这 2 条现在能构建了，按该清单"listed case that starts building must have its line
deleted"的棘轮规则删掉了它们的行。行一删，比对第一次真正执行，分歧当场露出来。
所以**封存掩盖的从来不是"构建失败"，而是"构建成功之后产物跑不起来"**——旧的封存条件
（构建拒绝）恰好把运行期故障一并挡在了门外。

**第二层：这个缺陷本身远比这两条用例老，而且在 base commit 上就已经在漏。**
`stdlib/time/time.xr` 是今天（2026-08-28, `d3fc9ad07` "Move time module semantics into
Xray over four clock leaves"）才由 A lane 建立的，在那之前 `time` 是纯 native 模块。
纯 native 模块因为没有 `.xr` 源，会在 `src/module/xmodule_graph.c:310-319` 这个闸门上
被整个丢出依赖图：

```c
if (!mid.source_path && (mid.kind == XR_MOD_STDLIB || mid.kind == XR_MOD_PACKAGE)) {
    if (graph_stdlib_embedded_path(...)) { mid.source_path = xr_strdup(embedded_path); }
    else { xr_module_id_cleanup(&mid); return; }   // ← 不进图，也就不会进 bundle
}
```

不进图 ⇒ 不进 bundle ⇒ 运行期不会有人拿身份串去 import ⇒ 由入口模块自己执行 `import time`
时走 native factory，一切正常。**A lane 给 `time` 加上 `.xr` 层，等于把 `time` 从"豁免区"
挪进了"受害区"。** 但这不是 A lane 引入的 bug——只是让 `time` 加入了早就中招的队伍：

| stdlib `.xr` 层 | 首次加入日期 |
|---|---|
| `base64/base64.xr` | 2026-07-02 |
| `path/path.xr` | 2026-07-02 |
| `sys/sys.xr` | 2026-07-07 |
| `log/log.xr` | 2026-07-08 |
| `text/text.xr` | 2026-07-11 |
| `io/io.xr` | 2026-07-20 |
| `os/os.xr` | 2026-08-02 |
| bundle 现有形态（`80c3733ca` "fix(vm): make bytecode bundles source independent"） | **2026-08-08** |
| `time/time.xr` | 2026-08-28（今天） |

bundle 改成现在这个样子（08-08）时，上面 7 个模块的 `.xr` 层都已经存在了。所以
**这条缺陷从 08-08 起就一直在漏，只是没人看**。实测坐实了这一点：
`tests/diff/cases/basic/strings.xr`（`import text`）在 **base commit 的封存清单里根本
没有出现过**，本来就该被 embed 车道跑到，而它现在照样以同一条消息失败：

```
tests/diff/cases/basic/strings.xr   FAIL (exit code (vm=0 embed=1))
    embed: |Error: Package 'stdlib-module-v1:module=4:text:path=12:text/text.xr' not found|
```

`busy_poll_progress.xr`、`sys_barrier_direct.xr` 同理，`git show HEAD:` 复核确认三者
在 base commit 的 embed 封存清单里都是 0 命中。

## Affected cases

判据是"import 了带 `.xr` 源码层的标准库模块"。`tests/diff/cases/` 共 **649** 条：

| 集合 | 条数 |
|---|---|
| import 了标准库模块 | 97 |
| 其中被 `known_failures_embedded_not_comparable.txt` 封存（工作区，48 条清单） | 9 |
| **未封存、embed 车道应当跑到并会失败** | **88** |
| 同一口径在 base commit 上（HEAD 清单 68 条） | **86** |
| `known_failures_embedded.txt`（embed 分歧清单）已登记条数 | **0** |

最后一行是要害：embed 分歧清单是**空的**，按棘轮设计 embed 车道应当全绿；实际上
base commit 就有 86 条会红。本 lane 删掉 2 行封存后变成 88 条。

`import time` 单独统计：`tests/diff/cases/` 12 条，`tests/regression/` 22 条。
但如前所述**这不是 `time` 专属问题**，按任意标准库模块统计：`tests/diff/cases/` 97 条、
`tests/regression/` 106 条。

实测覆盖（全部在 `build-nofp/xray` 上跑，全部复现，无一例外）：

**最小程序，8 个模块 8/8 失败：**

| 模块 | bundle 中 0 字节 stdlib 条目数 | 产物运行 |
|---|---|---|
| `time` | 1 | `Package 'stdlib-module-v1:module=4:time:path=12:time/time.xr' not found` |
| `io` | 3 | `...module=2:os:path=8:os/os.xr' not found` |
| `os` | 1 | `...module=2:os:path=8:os/os.xr' not found` |
| `sys` | 1 | `...module=3:sys:path=10:sys/sys.xr' not found` |
| `text` | 1 | `...module=4:text:path=12:text/text.xr' not found` |
| `base64` | 1 | `...module=6:base64:path=16:base64/base64.xr' not found` |
| `log` | 5 | `...module=4:text:path=12:text/text.xr' not found` |
| `path` | 2 | `...module=2:os:path=8:os/os.xr' not found` |

（`io`/`log`/`path` 的条目数 >1，是因为标准库之间互相 import；失败发生在拓扑序第一个
标准库依赖上，所以报出来的名字未必是源码里写的那个。）

对照组：不 import 任何标准库的程序（`print("none-ok")`），bundle 只有 1 条目，
产物正常输出 `none-ok`，rc=0。

**真实差分用例，7/7 失败：**

| 用例 | 触发模块 |
|---|---|
| `tests/diff/cases/semantics/coro/channel_timeout_after_sleep.xr` | `time` |
| `tests/diff/cases/semantics/coro/channel_send_timeout_partner_arrives.xr` | `time` |
| `tests/diff/cases/basic/strings.xr` | `text` |
| `tests/diff/cases/semantics/stdlib/http_formdata_pure_direct.xr` | `text` |
| `tests/diff/cases/semantics/stdlib/sys_barrier_direct.xr` | `sys` |
| `tests/diff/cases/semantics/stdlib/io_chmod_shared_core.xr` | `os` |
| `tests/diff/cases/liveness/busy_poll_progress.xr` | `time` |

后 5 条**都不在任何封存清单里**（base commit 与工作区均 0 命中）。

## Two things it is not

**（一）它不是 coroutine 或 channel timeout 的语义分歧。**
两条 coro 用例的名字（`channel_timeout_after_sleep`、`channel_send_timeout_partner_arrives`）
会把人引向 timer / resume-status 那套逻辑，而 `tests/diff/known_failures_not_comparable.txt`
第 174、176 行给它们贴的标签是 `XR_SEM_0019 coroutine state count disagrees with grounded
call authority` —— 更容易让人往协程状态上归因。**都不是。** embed 侧 stdout 完全为空，
用户程序的第一条指令都没执行；失败发生在 `xray_vm_eval_bundle` 的模块预装载循环里，
早于入口字节码。把这两条 `import time` 换成任何别的标准库 import，同样炸；把 timeout
逻辑整个删掉只留 `import time`，还是炸。协程语义在这条 packet 里是无辜的。

**（二）它不是"标准库没被打进 bundle"这个设计问题。**
`bundle_add_entry(bundle, spec->canonical, NULL, 0, spec->kind)` 里的 `NULL, 0` 是**对的**：
标准库不该随每个程序复制一份字节码，运行时自带
（`stdlib_embedded_sources.inc` / `stdlib_embedded_bytecodes.inc` 已经链进了 `libxray_core`，
`time` 的源码就在里面）。错的只有第一个实参——用了身份串而不是 `namespace_id`。
所以修复不需要动打包体积、不需要动 bundle 二进制格式的语义，只需要让运行期拿到正确的名字。
**请不要把这条 packet 读成"要把 stdlib 塞进 bundle"**，那会是个大得多、也没必要的改动。

## Requested capability

需要 embed 车道在运行期用**纯模块名**去 import 标准库条目。三条互斥的路子，按侵入性从小到大：

1. **在 `XrBundleEntry` 上补一个 import-name 字段**（本 lane 认为最干净）。
   `src/module/xbundle.c:161-165` 对 STDLIB 条目额外记下 `spec->authority.namespace_id`，
   `xr_bundle_to_c_source`（同文件 440-447 行）把它一并写进生成的 C，
   `xray_vm_eval_bundle`（`src/module/xproto_codec.c:2439`）改用该字段调用 `xr_module_import`。
   条目 `path` 保持身份串不变，`find_embedded_module`（`src/module/xmodule.c:727`）按
   `path` 匹配的既有行为不受影响。代价是 `XrBytecodeModule` 结构体加一个成员，
   生成的 C 的初始化式要同步。
2. **让 `xr_module_import` 认得身份串**：入口处若 `xr_module_identity_valid()` 判定为
   `XR_MODULE_IDENTITY_STDLIB`，就解析出 `module=` 段并改用纯名继续。
   （`src/module/xmodule_identity.c:390-399` 已经有现成的解析分支可复用。）
   好处是不动 bundle 格式；代价是给一个通用 import 入口加了一条身份串后门。
3. **打包期就写纯名**：`src/module/xbundle.c:162` 直接改传 `spec->authority.namespace_id`。
   改动最小，但会让 STDLIB 条目的 `path` 语义与其余条目（源码绝对路径）不一致，
   且 `src/app/cli/xcmd_deps.c:117-146,199-203` 依赖这些条目做依赖清单展示，需要一并复核。

无论走哪条，验收判据建议是：`known_failures_embedded.txt` 保持 0 条的前提下，
上面 7 条实测用例转绿，且 `tests/diff/cases/` 里那 88 条 stdlib-importing 用例在
`XRAY_DIFF_BACKENDS=vm,embed` 下不再出现 `Package '...' not found`。

**未验证的边界（请 owner 注意）：** 本 lane 只有 `build-nofp` 一份构建
（`-DXRAY_STDLIB_VM_FASTPATHS=OFF`；`XRAY_STDLIB_VM_FASTPATHS` 默认为 `ON`，但按已知
问题在当前 HEAD 上 ON 会导致构建失败，故无法 A/B）。从代码看
`src/module/xbundle.c:161` 这条分支不受该开关影响（开关的作用域在 `CMakeLists.txt:1749`
之后，而 `stdlib_embedded_*.inc` 的生成在 1212-1235 行，与开关无关），
但**"fastpaths=ON 时是否同样复现"本 lane 没有实测**，不做断言。

## Files deliberately not modified

- `src/` 下全部文件：本 packet 只读定位，未改一字。根因在
  `src/module/xbundle.c`、`src/module/xproto_codec.c`、`src/module/xmodule.c`，
  归属 H（compiler / module loader），不属本 lane 权限。
- `tests/diff/cases/`、`tests/regression/`：未增删改任何用例。上文所有"最小程序"都写在
  scratchpad 临时目录里，不在仓库内。
- `tests/diff/known_failures_embedded.txt`：**刻意保持为空**。把这 88 条登记进去可以让
  差分网转绿，但那是拿封存换绿色——正是这两条 coro 用例当初被藏起来的同一种做法，
  而该清单表头明写 "Do not add a line to make a change go green"。本 lane 不做这件事，
  改为提交这份 packet。
- `tests/diff/known_failures_embedded_not_comparable.txt` / `known_failures_not_comparable.txt`：
  本 lane 的复验产物确实改了这两份（按棘轮规则删除已能构建的行），但那是 lane 1 的本职
  工作，与本 packet 的根因无关，也不影响上文任何测量——所有"base commit 上就已经红"的
  结论都用 `git show HEAD:<清单>` 单独复核过。

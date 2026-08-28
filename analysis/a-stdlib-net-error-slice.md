# net.NetError 迁移可行性调研

只读调研，工作树 `worktrees/a-stdlib-selfhost-w0-inventory-bb6eac777369`，编译器 `./build/xray`（未重建）。
实验文件在 `scratchpad/nettest/`，仓库文件零修改。

---

## 结论（先说）

**不能作为独立切片落地。需要先解决 X = `net.__copyBidirectional` 的「native 直接构造并发布 NetError 枚举值」契约。**

`NetError` 与 `os.platform/arch/sep/eol` 有本质区别：`os` 那四个是 **C 只读地提供标量、Xray 侧只做包装**；
`NetError` 是 **C 侧要构造这个枚举的变体值并抛出**。C 侧构造 stdlib 枚举值只有一条通路 —— `xr_stdlib_enum_type_get()`
查 `.def` 生成的表。把 `NetError` 从 `.def` 挪走，这条通路直接断，而且 **三处会以不同方式失败，其中一处是静默的**。

同时查证了 **enum layout id 的算法在 `.def` 与 `.xr` 之间不是同一套输入** —— 这是本切片最大的隐藏风险，详见第 4 节。

---

## 1. 现状

### 1.1 `.def` 声明

`stdlib/defs/core.def:3257-3260`（`module net {` 在 3256，enum 是块内首个成员）：

```
module net {
  enum NetError {
    variants: "Timeout, Closed, Reset, Refused, Dns, Tls, Io, Invalid, Cancelled, OutOfMemory"
    doc: "Typed failure from network operations; classification from native codes lives in net.xr"
  }
```

生成物（都由 `tools/stdlibgen/stdlibgen.py` 产出，禁止手改）：

| 文件 | 内容 |
| --- | --- |
| `src/stdlib/xstdlib_defs_generated.h:516,550` | `xr_stdlib_enum_def_entries[]` 里的 net/NetError 行，`layout_id = UINT32_C(2184710811)` |
| `src/frontend/analyzer/xanalyzer_builtins_generated.h:453-455` | `g_gen_net_enums[]`，同一个 layout id |
| `src/frontend/analyzer/xanalyzer_builtins_generated.h:458-469,481` | `g_gen_net___copybidirectional_8_errors[]`（effect 契约的 10 个 `NetError.*` 字符串） |
| `src/aot/xstdlib_aot_methods_generated.inc.c:10,199` | AOT 直调表：`{"net","__copyBidirectional",...,UINT32_C(2184710811),"NetError",<variants>,10}` |
| `src/app/lsp/xlsp_stdlib_generated.inc:394`、`src/app/mcp/xmcp_knowledge_generated.c:3340+` | 文档/LSP 表面 |

### 1.2 三个**不同**的 C 侧错误枚举，别搞混

1. `stdlib/net/xneterror.h` — `XrNetError` / `XR_NERR_*`（23 个变体 + `net_error_string()`）。
   **与 Xray 的 NetError 无关**，只被 `stdlib/net/tls.h` 和 `stdlib/net/io.h` include，是 C 内部的错误字符串表。
   变体名和数量都对不上 NetError，迁移时不要动它。
2. `src/io/xnet_handle.h:55-66` — `XrNetErrorKind` / `XR_NETERR_NONE..CANCELLED` = 0..9。
   注释明写这是 **frozen script-facing contract**：`net.__lastCode` 原样返回这些值。
   handle 上的 `last_error` 字段存的就是它。
3. `src/aot/xrt_net.h:58-68` — `xrt_net_error_kind_t` / `XRT_NETERR_*`，2 的 AOT 镜像，同样 0..9。

### 1.3 C 侧唯一的 NetError **值**生产者：`net.__copyBidirectional`

`stdlib/net/net.c`：

```c
// net.c:1259-1270  变体序号的手抄副本
typedef enum {
    NET_ERROR_TIMEOUT, NET_ERROR_CLOSED, NET_ERROR_RESET, NET_ERROR_REFUSED,
    NET_ERROR_DNS, NET_ERROR_TLS, NET_ERROR_IO, NET_ERROR_INVALID,
    NET_ERROR_CANCELLED, NET_ERROR_OUT_OF_MEMORY,
} NetErrorVariant;

// net.c:1272   XrNetErrorKind(+250/251 哨兵) -> 变体序号
static uint32_t net_error_variant_index(uint8_t error) { ... }

// net.c:1299
static XrEnumType *net_error_type(XrVMRuntime *X) {
    return xr_stdlib_enum_type_get(X, "net", "NetError");   // <=== 只查 .def 表
}

// net.c:1303
static void net_publish_error(XrVMRuntime *X, uint8_t error) {
    XrEnumType *type = net_error_type(X);
    if (!type)
        return;                                   // <=== 静默返回！
    XrEnumAggregateValue *value = xr_enum_zero_payload_value(..., net_error_variant_index(error));
    ... xr_exec_context_publish_error_owned(...)
}
```

`net_publish_error` 的 9 个调用点全在 `copyBidirectional` 路径：`net.c:1629,1634,1663,1672,1677,1684,1706,1716`。
另有内部哨兵 `NET_BIDI_ERROR_CANCELLED = 250` / `NET_BIDI_ERROR_OUT_OF_MEMORY = 251`（`net.c:1254-1257`）。

`xr_stdlib_enum_type_get` 实现在 `stdlib/stdlib_cache.c:178`，先 `stdlib_enum_decl_find(module, name)` 遍历
`xr_stdlib_enum_def_entries`（`.def` 生成），再 `xr_enum_type_new(isolate, decl->module, decl->name, ...)`
并把 `decl->layout_id` 盖到 layout 上。**除 `.def` 外没有第二个数据源。**

全仓只有 3 个 stdlib C 文件碰 `xr_enum*`：`net.c`、`cluster.c`、`prelude.c`。
`cluster.c:1318,1366` 同样走 `xr_stdlib_enum_type_get`（ClusterDelivery / ClusterNodeState，也都在 `.def`）。
没有任何 stdlib C 代码通过 `xr_module_get_export`（`src/module/xmodule.h:282`）去拿 `.xr` 声明的类型。

### 1.4 AOT 侧

`src/aot/xrt_net.h:902` `xrt_net_error_variant_index(kind)` 把 kind 映射成**变体序号**（不是 code），
塞进 `XrtI64PairResult.error_index`（`xrt_net.h:892`）。

`src/aot/xi_cgen_coro.inc.c:2707-2735` 的 `emit_coro_net_bidi_call_stmt` 从
`xr_stdlib_enum_def_entries` 里按 `module=="net" && name=="NetError"` 找行；找不到就：

```c
ctx->error = true;
fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT net.copyBidirectional contract\n");
```

`emit_coro_net_bidi_materialize`（`xi_cgen_coro.inc.c:2638` 起）用 `error_enum->layout_id` 直接发射：

```c
xrt_pending_error = xrt_enum_box_new(UINT32_C(2184710811), "NetError", (const char*const[]){...}[i], i);
```

同样的模式在非协程直调路径 `src/aot/xi_cgen_stdlib_helpers.inc.c:597-611`（用 `m->enum_layout_id`）。

### 1.5 `.xr` 现状

`stdlib/net/net.xr` 已经把**几乎全部**分类逻辑放在 Xray 侧：

- `net.xr:15-23`：`_CODE_TIMEOUT..._CODE_CANCELLED` = 1..9，注释写明「mirrors XrNetErrorKind and is a frozen contract」。
- `net.xr:31-40`：`fn _classify(code: i64) -> NetError`，9 分支 + 默认 `NetError.Io`。
- `_classify` 调用点：`net.xr:124,126`（dial）、`135`（TLS 握手）、`162`（upgradeTLS）、`236`（`lastError`）。
- 另有 13 处直接 `throw NetError.X` 字面量：`net.xr:116,120,144,145,159,160,175,180,197,285,287,295,297`。

**所以 Xray 侧的错误分类已经 100% 就位。** `copyBidirectional`（`net.xr:277-280`）是唯一例外：

```xray
export fn copyBidirectional(a: NetConn, b: NetConn) -> CopyBidirectionalResult {
    var raw = __copyBidirectional(a, b)
    return CopyBidirectionalResult(raw.aToB, raw.bToA)
}
```

它没有任何错误处理 —— 因为错误是 C 侧 `net_publish_error` 直接发布到执行错误通道的。

`.def` 里 `__copyBidirectional`（`core.def:3427-3450`）的三个关键字段：

```
ret: "i64_pair_result"
aot_enum: "NetError"
effect: "NetError.Timeout, NetError.Closed, ... , NetError.OutOfMemory"
```

---

## 2. 可行性实验（scratchpad，仓库零改动）

| 实验 | 文件 | 结果 |
| --- | --- | --- |
| `.xr` 里声明 enum、同文件内比较、作返回类型、throw/catch | `nettest/e1_enum_basic.xr` | **全部通过**，输出 `true` × 4 |
| 普通脚本里声明 `enum NetError` | `nettest/e2_reserved.xr` | **允许**。`NetError` 不在 `g_reserved_builtin_names`（`xanalyzer_builtins.c:1213`），也不是 handle 名，所以不触发 E0350。E0350 只对 `NetConn`/`NetListener` 这类 native handle 无条件拒绝（`xanalyzer_visitor_decl.c:3711-3716`） |
| `import { NetError } from net` + `NetError.Timeout` 比较 | `nettest/e3_import.xr` | 通过 |
| 脚本里调 `net.__hasTLS()` | `nettest/e4_leaf.xr` | **拒绝**：`error: stdlib module 'net' has no member '__hasTLS'` |
| 现存差分用例基线 | `tests/diff/cases/semantics/stdlib/net_copy_bidirectional_error_enum.xr` | VM 下当前**通过**，输出 `Closed` / `true`，与 `.expected` 一致 |

### 「leaf 返回 `.xr` 声明的 enum」这一点 —— **测不了**

`__` 前缀的 leaf 在模块外不可见（e4 已验证），所以纯脚本实验无法构造「native leaf 返回 `.xr` enum」的场景；
要测就必须改 `stdlib/*.xr` + `.def` 并重新构建 stdlib 字节码，超出只读边界。

但**不需要实验**就能从代码判定答案：C 侧拿 stdlib 枚举类型的唯一 API 是 `xr_stdlib_enum_type_get`，
它只查 `.def` 生成表；AOT 侧 boxing 用的 layout id 是 `.def` 生成的编译期常量。
`.xr` 声明的 stdlib 模块枚举**没有任何机制**被 C 或 AOT 看见。

### 唯一的「.xr 声明 + native 生产」先例，以及它为什么不适用

`NumberParseError` 声明在 `stdlib/types/i64.xr:2-5`，由 native `i64.parse` 抛出。
但它**不在 stdlib 模块命名空间里**：`src/base/xnumber_parse_error.h` 把它冻结成一行注册表，
`XR_NUMBER_PARSE_ERROR_NOMINAL_OWNER = "prelude"`、`XR_NUMBER_PARSE_ERROR_LAYOUT_ID = 3802613823`，
运行时由 `stdlib/prelude/prelude.c:128` / `src/api/xisolate_runtime.c:36` 以 `xr_enum_type_new(X, "prelude", ...)` 注册，
并被 `src/plan/semantic/xr_semantic_number_parse_error_shape.h` 逐字段核证。
`Utf8Error`（`stdlib/types/string.xr:10`）、`StringSliceError` 同理。
即 `stdlib/types/*.xr` 是 **prelude 声明存根**，不是 stdlib 模块源；这条路要给 NetError 走，
等于给它造一份 frozen registry + semantic shape 核证头，工作量远大于「迁一个符号」。

---

## 3. 三个阻塞点（按发现顺序，两个硬失败 + 一个静默失败）

### 阻塞 1 — 生成器直接报错（**已实测**）

把 `core.def` 里 `enum NetError { ... }` 整块删掉（在 scratchpad 的副本上），
用 `importlib` 加载 `tools/stdlibgen/stdlibgen.py` 跑 `parse_def_metadata` + `emit_aot_methods`：

```
parse OK; enum count: 5   net enums: []
SystemExit: net.__copyBidirectional: unknown aot_enum net.NetError
```

来源 `stdlibgen.py:1174-1176`。
连带约束 `stdlibgen.py:530-534`：`ret in {"enum_i64","i64_pair_result"}` **必须**同时有 `aot_direct: true` 和 `aot_enum`。
所以「只删 `aot_enum`」也不行，必须同时改 `ret`（合法值只有 `value/i64/enum_i64/str_borrowed/i64_pair_result`，见 `stdlibgen.py:1137-1149`）。

### 阻塞 2 — VM 路径静默降级（**最危险**）

`net_publish_error`（`net.c:1303-1306`）在 `net_error_type()` 返回 NULL 时 **`return;` 静默**。
`.def` 表里没有 NetError → 每次 `copyBidirectional` 失败都变成：错误值没发布、`*result = XR_NULL_VAL`。
`net.xr:278` 的 `var raw = __copyBidirectional(a, b)` 声明类型是非空 `__CopyBidirectionalResult`，
拿到 null 后 `raw.aToB` 会以运行时故障（panic / 空解引用）收场 —— 
**一个 typed throw 静默退化成 panic**。编译期没有任何诊断。

### 阻塞 3 — AOT codegen 硬失败

`xi_cgen_coro.inc.c:2731-2737`：找不到 `.def` 的 NetError 行 → `ctx->error = true` +
`[xi_cgen] ERROR: unsupported AOT net.copyBidirectional contract`。
`xray build --native` 对任何用到 `net.copyBidirectional` 的程序直接失败。

### 顺带的门/契约改动

- `stdlib/stdlib_boundary.toml:349` `net.public_native` 要去掉 `"NetError"` —— 这是**推导值**，
  `scripts/check_stdlib_boundary.py:324-333` 用 `def_public_symbols()` 与之比对，删 `.def` 就必须同步删 toml。
- `scripts/check_stdlib_boundary.py:222-227` **硬编码**了 net 的期望集合（含 `"NetError"`），也要同步改。
  注：该门当前已经因 os 那批红着（见下）。
- `contracts/stdlib-symbol-inventory.json` 里 `{"module":"net","symbol":"NetError","kind":"enum","semantic_source":"stdlib/defs/core.def","plan_coverage":"native_binding"}`
  要用 `scripts/stdlib_symbol_inventory.py` 重生成。
- 单测直接写死了 `.def` 查找：`tests/unit/stdlib/test_native_type_surface.c:144,159,161`、
  `tests/unit/vm/test_bytecode_io.c:1187-1238`（后者还用 `xr_enum_type_new(writer,"net","NetError",...)` 造 nominal_owner=`"net"` 的枚举做字节码往返）。

### 基线噪声（不是本切片造成的）

```
$ python3 scripts/check_stdlib_boundary.py
stdlib boundary gate failed:
  L2 module os: public_native is outside the terminal primitive/handle set
  task-256 Xray public-symbol ownership ratio is 56.2738%; expected >= 85%
  Iterator declaration methods must be exactly hasNext/next/nth; ...
  compiler Iterator method table must match stdlib/types/iterator.xr; got
  API inventory does not expose the complete Iterator schema
```

`stdlib_boundary.toml` 里 `os.public_native = []` 已清空，但 `check_stdlib_boundary.py:222` 的
`expected_native["os"]` 还写着 `{"arch","eol","platform","sep"}` —— **os 那批的门脚本尚未同步**。
做 NetError 之前要么先补上 os 的门，要么至少知道这条红线不是自己引入的。

---

## 4. enum layout id：**两套规则不同**（重点结论）

### 算法本身是一套

`xr_enum_layout_nominal_id()`（`src/runtime/value/xenum_layout.c:41-57`）是唯一权威；
`tools/stdlibgen/stdlibgen.py:213` 和 `scripts/gen_stdlib_types.py:825` 各有一份 Python 手抄
（FNV-1a over `"xray.enum.nominal.v1"` + owner + name + variant_count + 每个变体名/payload 数，
再 splitmix64 mix，取低 32 位 `| 0x80000000`）。

### 但**喂进去的 `nominal_owner` 不同**

| 声明来源 | nominal_owner | 来自 |
| --- | --- | --- |
| `.def`（builtin） | **模块名** `"net"` | `xa_builtin_enum_nominal_owner()` = `module->name`（`xanalyzer_builtins.c:1089-1101`）；`stdlib_cache.c:146` 也用 `decl->module` |
| `.xr`（stdlib 模块源） | **完整 canonical module identity 字符串** | `xa_analyzer_nominal_owner_for_file()` 返回 `analyzer->current_module_identity` = `module_spec->canonical`（`xanalyzer.c:1795-1821` + `xanalyzer.c:2046`），由 `xr_module_identity_from_logical()` 拼成 `"stdlib-module-v1:module=<len>:<name>:path=<len>:<name>/<name>.xr"`（`xmodule_identity.c:235-237`） |
| `stdlib/types/*.xr` | `"prelude"` | 走 prelude 注册，不是模块源 |

### 实证（不是推断）

字节码里 `bc_write_enum_type`（`xproto_codec.c:912-935`）先写 `nominal_owner` 再写 enum 名。
直接读构建产物：

```
build/generated/stdlib-bytecode/csv @6137:
  30 00 00 00 "stdlib-module-v1:module=3:csv:path=10:csv/csv.xr"  0c 00 00 00 "CsvDelimiter"
      ^ 长度 0x30 = 48                                                 ^ .xr 声明的 enum

build/generated/stdlib-bytecode/net @9177:
  03 00 00 00 "net"  08 00 00 00 "NetError"
      ^ 长度 3                        ^ .def 声明的 enum
```

同一个 `net` 模块的字节码里，`.def` 的 NetError 用 `"net"`；同为 stdlib 模块源的 `csv.xr`
声明的 4 个 enum 全部用 48 字节的 framed identity。**两种 owner 并存，规则确实不同。**

### 数值验证（公式已用两个已知常量对表）

```
lid("prelude","NumberParseError",[InvalidSyntax,OutOfRange]) = 3802613823
                    对上 xnumber_parse_error.h 的 XR_NUMBER_PARSE_ERROR_LAYOUT_ID  ✓
lid("net","NetError",<10 变体>)                              = 2184710811
                    对上 xanalyzer_builtins_generated.h:454 / xstdlib_defs_generated.h:550  ✓
lid("stdlib-module-v1:module=3:net:path=10:net/net.xr","NetError",<10 变体>) = 2448215453
```

### 后果

**变体序号（tag 0..9）不会变** —— `net.c:1259` 的 `NetErrorVariant` 和 `xrt_net_error_variant_index`
按声明顺序算序号，只要 `.xr` 里变体顺序照抄就仍然对齐。**对不上的是 layout id 这个类型身份。**

layout id 是承重的，不是元数据：

- `src/aot/xi_cgen_dispatch_helpers.inc.c:13442-13462`
  `xicgen_emit_enum_is_predicate` / `xicgen_emit_exact_enum_is_predicate` 发射
  `v.tag == XR_TAG_ENUM && xrt_enum_value_layout_id(v) == <编译期 layout id>`；
  `src/aot/xi_cgen_value_helpers.inc.c:841-850` 同样。
- 也就是说：AOT 里 `xrt_enum_box_new(2184710811, "NetError", ...)` 造出来的值，
  拿去和期望 2448215453 的 `catch (e: NetError)` 比对 **不匹配** —— 
  `xicgen_emit_enum_is_predicate` 有 `== 0 ||` 逃逸口（layout id 为 0 时放行），
  但 box 里是 2184710811 而非 0，所以逃逸口救不了，**typed catch 会静默不命中**。
- `src/plan/semantic/xr_semantic_builder.c:360-372` 的 `source_enum_key` 把
  `nominal_owner` 直接编进语义身份 key（`source-enum-v1:...:owner=<nominal_owner>:name=...`），
  并要求 `layout->layout_id == xr_enum_layout_nominal_id(layout)`。owner 变了，语义身份就是另一个类型。

**结论：把 NetError 从 `.def` 挪到 `.xr`，会让 C/AOT 侧发布的枚举值与 Xray 侧的 NetError 变成两个不同身份的类型。
变体序号仍然对得上，但类型身份对不上，且在 AOT 下表现为「catch 静默不命中」而非编译错误。**
因此**绝不能**保留 C 侧 boxing 而只搬声明；必须先把 C 侧的枚举构造整个拆掉。

---

## 5. 如果要做：逐文件改动清单（enabling refactor + 迁移）

前提：先把 `__copyBidirectional` 改成 **code-based**（与模块里已有的 `__connectFd` / `__lastConnectCode` 同构），
让 C/AOT 完全不再构造 NetError 值。下面给推荐方案（**方案 A：thread-local last-code**），
它对 AOT 的改动面最小，且与 `net.xr` 现有风格一致。

### 5.1 让 native 不再造 enum

| 文件 | 改什么 |
| --- | --- |
| `src/io/xnet_handle.h:55-66` | `XrNetErrorKind` 追加 `XR_NETERR_OUT_OF_MEMORY = 10`（bidi 的 251 哨兵需要一个正式 code；Cancelled 复用 9） |
| `src/aot/xrt_net.h:58-68` | 同步追加 `XRT_NETERR_OUT_OF_MEMORY = 10`，保持两份镜像一致 |
| `stdlib/net/net.c:1254-1257` | 删 `NET_BIDI_ERROR_CANCELLED/OUT_OF_MEMORY` 哨兵，改用正式 code 9 / 10 |
| `stdlib/net/net.c:1259-1296` | **删** `NetErrorVariant` 与 `net_error_variant_index()` |
| `stdlib/net/net.c:1299-1301` | **删** `net_error_type()` |
| `stdlib/net/net.c:1303-1320` | **删** `net_publish_error()`；换成 `net_set_last_bidi_code(uint8_t)`，写一个 `XR_THREAD_LOCAL uint8_t g_last_bidi_code`（照抄 `net.c:394` 的 `g_last_connect_code` 写法） |
| `stdlib/net/net.c:1629,1634,1663,1672,1677,1684,1706,1716` | 8 处 `net_publish_error(X, e)` → `net_set_last_bidi_code(e)`，且失败路径 `*result = XR_NULL_VAL` 保持不变 |
| `stdlib/net/net.c` 新增 | `net_last_bidi_code` C 函数（VM binding），返回 `g_last_bidi_code` |
| `src/aot/xrt_net.h:902-906` | `xrt_net_error_variant_index()` 改名/改语义为 `xrt_net_error_code()`：直接返回 `kind`（不再 `-1` 成变体序号）；11 处调用点（`921,924,926,928,986,996,1010,1017,1033,1040,1059,1078`）随之改 |
| `src/aot/xrt_net.h` 新增 | `xrt_net_last_bidi_code()` 直调 shim（thread-local，与 VM 侧对齐） |

### 5.2 `.def`

| 位置 | 改什么 |
| --- | --- |
| `stdlib/defs/core.def:3257-3260` | **删** `enum NetError { ... }` 整块 |
| `stdlib/defs/core.def:3427-3450`（`fn __copyBidirectional`） | 删 `aot_enum: "NetError"`；`effect:` 那 10 个 `NetError.*` 保留即可（见 5.5 说明）；`ret: "i64_pair_result"` 保留但需要 5.4 放宽生成器约束 |
| `stdlib/defs/core.def` net 块新增 | `fn __lastBidiCode { signature: "(): i64"  vm: "net_last_bidi_code"  aot: "xrt_net_last_bidi_code"  aot_direct: true  argc: 0  arg_spec: ""  layer: "runtime" }`（照抄同块的 `__lastConnectCode`，`core.def` 里搜 `__lastConnectCode` 取模板） |

### 5.3 `net.xr`

| 位置 | 改什么 |
| --- | --- |
| 文件头（`net.xr:11` 注释之后、`_CODE_*` 之前） | 新增 `export enum NetError { Timeout, Closed, Reset, Refused, Dns, Tls, Io, Invalid, Cancelled, OutOfMemory }` —— **顺序必须逐字照抄 `.def` 原顺序**（虽然 layout id 会变，但 `net.xr:31` 的 `_classify` 与所有既有测试都按名字用，顺序错了会让 `error.name` 输出错乱） |
| `net.xr:15-23` | 追加 `const _CODE_OUT_OF_MEMORY: i64 = 10` |
| `net.xr:31-40` `_classify` | 追加 `if (code == _CODE_OUT_OF_MEMORY) { return NetError.OutOfMemory }` |
| `net.xr:277-280` `copyBidirectional` | 改成先查 code 再抛：`var code = __lastBidiCode()` 之前先判空 —— 具体形状取决于 5.4 里 `__copyBidirectional` 的返回类型是否改成 nullable。最直白：`.def` 签名改 `: __CopyBidirectionalResult?`，然后 `var raw = __copyBidirectional(a,b); if (raw == null) { throw _classify(__lastBidiCode()) } return CopyBidirectionalResult(raw!.aToB, raw!.bToA)` |

### 5.4 生成器与 AOT codegen

| 文件 | 改什么 |
| --- | --- |
| `tools/stdlibgen/stdlibgen.py:530-534` | 放宽：`i64_pair_result` 不再强制要求 `aot_enum`（或新增一个 `ret` 种类如 `i64_pair_nullable`） |
| `tools/stdlibgen/stdlibgen.py:1161-1200` `emit_aot_methods` | `aot_enum` 为空时不再是错误（现在 1174-1176 会 `SystemExit`）；生成行的 layout_id/enum_name/variants 走 `0/NULL/NULL/0` 分支（该分支已存在） |
| `src/aot/xi_cgen_coro.inc.c:2707-2737` | 删掉 `error_enum` 查表与 `!method->aot_enum || strcmp(...,"NetError")` 那条守卫 |
| `src/aot/xi_cgen_coro.inc.c:2638-2705` `emit_coro_net_bidi_materialize` | 删掉 `xrt_enum_box_new(...)` 那段（2672-2691），改成 `error_index >= 0` 时把 code 写进 thread-local 并把结果置 null |
| `src/aot/xi_cgen_stdlib_helpers.inc.c:597-611` | 同样的 i64-pair + enum boxing 分支要跟着改（非协程直调路径） |

### 5.5 契约 / 门 / 生成物

| 文件 | 改什么 |
| --- | --- |
| `stdlib/stdlib_boundary.toml:349` | `public_native` 去掉 `"NetError"`（剩 10 项） |
| `scripts/check_stdlib_boundary.py:222-227` | `expected_native["net"]` 去掉 `"NetError"`；顺带把 `expected_native["os"]` 改成 `set()`（os 那批的遗留） |
| `contracts/stdlib-symbol-inventory.json` | 用 `scripts/stdlib_symbol_inventory.py` 重生成 |
| `tests/unit/stdlib/test_native_type_surface.c:144,159-161` | 删掉对 `xa_builtin_get_enum_type("net","NetError")` / `xr_stdlib_enum_type_get(iso,"net","NetError")` 的断言，或换成另一个仍在 `.def` 的 enum（如 `cluster.ClusterDelivery`） |
| `tests/unit/vm/test_bytecode_io.c:1187-1238` | 同上，`.def` 枚举往返用例要换靶（这个用例的意义是「stdlib canonical enum 往返」，换成 cluster 的即可） |
| 全部生成物 | 按 `MEMORY: regenerate-never-hand-edit` 走官方生成器重跑：`xstdlib_defs_generated.h`、`xanalyzer_builtins_generated.h`、`xstdlib_aot_methods_generated.inc.c`、`xlsp_stdlib_generated.inc`、`xmcp_knowledge_generated.c`、`build/generated/stdlib-bytecode/*` |

关于 `effect:` 字段：**不需要改**。`es_apply_effect_contract`（`xanalyzer_errorset.c:3434-3474`）
用 `lookup_enum_symbol(analyzer, "NetError")` **按名字在作用域里查**，
`net.xr` 里 `export enum NetError` 声明后这条查找照样命中（先例：`xa_native_member_contract.def:52,66` 的
`NumberParseError.*` 就是这样引用 `.xr` 里声明的枚举的）。

### 5.6 符号冲突不会发生（已查证）

`xa_register_stdlib_native_module_types()`（`xanalyzer.c:1906-1928`）在 `xa_analyze_ast` **之前**
把 `.def` 的模块 enum 注册进文件作用域。一旦 `.def` 里删掉 NetError，这个循环不再注册它，
`net.xr` 自己的 `enum NetError` 就是唯一声明，不会撞名。
（反过来说：**不能两边都留**，那样 builtin 先注册、源声明会变成重复符号，且 layout id 还会分叉。）

---

## 6. 风险与验证步骤

### 风险排序

1. **[高·静默] layout id 从 2184710811 变成 2448215453。**
   任何遗漏的 C/AOT boxing 点都会在 AOT 下表现为 `catch (e: NetError)` 不命中，而不是编译错误。
   缓解：改完后全仓 grep `2184710811` 必须为零命中（除 git 历史）。
2. **[高·静默] `net_publish_error` 的 `if (!type) return;`。**
   只要有一条路径还调它而 `.def` 已经没有 NetError，错误就凭空消失。
   缓解：这个函数必须**整个删掉**，而不是留着让它 no-op。
3. **[中] `xnet_handle.h` / `xrt_net.h` 两份 `XR*_NETERR_*` 手抄副本**
   （MEMORY: `two-layer-predicate-drift`、`suspend-predicate-three-copies` 的同类形状）。
   加 `OUT_OF_MEMORY = 10` 必须两边同时改，且 `net.xr:15-23` 的 `_CODE_*` 是第三份手抄。
4. **[中] `ret: "i64_pair_result"` 的两条 codegen 路径**（协程 `xi_cgen_coro.inc.c` + 直调 `xi_cgen_stdlib_helpers.inc.c`）
   都要改，漏一条只在特定调用形状下暴露。
5. **[中] `xray build`（默认嵌入式）路径没有差分覆盖**（MEMORY: `bytecode-embed-form-test-blindspot`）。
   `net_copy_bidirectional_*.xr` 四个差分用例只覆盖 VM + `--native`。
6. **[低] os 那批留下的门脚本红线**已经存在，别把它当成自己的回归（MEMORY: `baseline-noise-before-blame`）。

### 验证步骤（改完之后）

1. `python3 tools/stdlibgen/stdlibgen.py --check` —— 生成物是最新的。
2. 全量重建（stdlib 在构建期编译，必须删 `build/generated/stdlib-bytecode` 后全量 ninja；MEMORY: `stdlib-compiled-at-build-time`）。
3. `grep -rn 2184710811 src/ stdlib/ contracts/` —— 必须零命中。
4. `python3 scripts/check_stdlib_boundary.py` —— 与改动前的基线（本文档第 3 节记录的 5 条）逐条对比，不得新增。
5. 四个差分用例 **VM + AOT 双跑**（MEMORY: `vm-aot-both-proven`）：
   - `tests/diff/cases/semantics/stdlib/net_copy_bidirectional_error_enum.xr`（当前基线 `Closed` / `true`）
   - `net_copy_bidirectional_cancel.xr` / `net_copy_bidirectional_transfer.xr` / `net_copy_bidirectional_type_surface.xr`
6. `tests/regression/10_stdlib/1431..1437_net_*.xr` 全部。
7. `tests/unit/stdlib/test_native_type_surface.c`、`tests/unit/vm/test_bytecode_io.c` 重跑。
8. 额外确认字节码里 NetError 的 owner 已变成 framed identity：
   `strings build/generated/stdlib-bytecode/net | grep stdlib-module-v1` 应出现 `module=3:net:path=10:net/net.xr`，
   且不再有 `\x03\x00\x00\x00net\x08\x00\x00\x00NetError` 这种 `.def` 形态。

---

## 7. 建议

**这一批（`net` 的 11 个 public_native）里，`NetError` 也不是可先迁的那个。**
`NetConn`/`NetListener` 及其 9 个方法被 builtin 保留名挡住；`NetError` 被 `__copyBidirectional`
的「native 构造枚举值」契约挡住。**net 模块本批 0 个可独立落地的切片。**

`NetError` 要迁，先做的是一个独立的、可自证的前置任务：
**把 `net.__copyBidirectional` 从「native 发布 NetError」改成「native 返回 code，net.xr 分类」**
—— 这个前置任务本身有价值（它消掉 `net.c` 里那份 `NetErrorVariant` 手抄副本，
让 `net.xr:11-13` 注释里「classification to NetError happens only here」这句话第一次变成真的），
且完成后可以用同一套差分用例自证，不依赖 NetError 是否搬家。
前置任务落地之后，NetError 的迁移才退化成一个真正的小切片（删 `.def` 块 + 加 `.xr` 声明 + 改门 + 重生成）。

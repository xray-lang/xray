# 6-10：sys · os · net 的 C 残留分类

基线 `34be0379c`，分支 `work/6-10-sys-os-net-residue-34be0379c`。

这份文档的目的**不是**把 306 个数字打到 0，而是让**每一个留下的 C 函数都能说出它为什么留**。
结论先行：306 条里只有 20 条是可以直接删的死代码（已删），
真正「本该在 Xray 侧却还留在 C 里」的只有 3 条，其余 283 条各有其不能走的理由，逐条列在下面。

---

## 0. 306 这个数字到底数的是什么

简报里的 `101 + 42 + 163` 不是 `stdlib_no_handwritten_c_semantic_owner` 的分模块计数，
而是 `scripts/stdlib_symbol_inventory.py` 里 **`covered_c_deletion` 落在该模块 C 文件上的行数**，
也就是「由该模块的 C 承载的全部符号行」。它比门的计数多，因为**叶子的实现体也在里面**：

| | sys | os | net | 合计 |
|---|---|---|---|---|
| 门计入的 C 语义所有者 | 84 | 16 | 135 | **235** |
| 已批准叶子（门不计） | 16 | 26 | 27 | **69** |
| 模块工厂（归另一条门） | 1 | 0 | 1 | **2** |
| **合计（= 简报的 306）** | **101** | **42** | **163** | **306** |

**这是先分类再动手的理由**：门的判据是「非叶子、非 factory 的 C 函数」，
所以盯着 306 追会把人推向「为了降数字而重写 pthread 封装」。
门实际能降的部分只有 235，而其中绝大多数是叶子的实现体，删不掉也迁不走。

---

## 1. 四档汇总

| 模块 | A 已覆盖可删 | B 叶子实现体 | C 纯计算可迁 | D 平台原语 | 其中公开面(归 6-1) | 模块工厂 | 合计 |
|---|---|---|---|---|---|---|---|
| sys | 0 | 62 | 0 | 38 | 22 | 1 | 101 |
| os | 0 | 16 | 2 | 24 | 0 | 0 | 42 |
| net | 20 | 107 | 1 | 34 | 8 | 1 | 163 |
| **合计** | **20** | **185** | **3** | **96** | **30** | **2** | **306** |

（C 档另有 1 条计在 B 档里的逻辑重复 `net_error_variant_index`，见 §4.1。）

- **A 档已清零**：20 条全部删除，`stdlib_no_handwritten_c_semantic_owner` **844 → 824**。
- **D 档的 96 条里有 30 条是公开原生表面本身**（sys 22 + net 8），归 6-1 的元数据缺口，不是本 lane 能动的。
- **C 档只有 3 条**，且三条的迁移条件都已经写在 `native_leaf_allowlist.toml` 的 `deletion_trigger` 里。

---

## 2. A 档：20 条死代码（已删，519 行）

全部在 net。sys 和 os **一条都没有** —— 这两个模块的 C 已经没有可白捡的分子了。

### 2.1 判据

一个 C 函数是死的，当且仅当：

1. 全仓库对它的**有效命中**只剩定义行和头文件声明
   （有效 = 排除注释；排除 `analysis/`、`contracts/`、`blockers/` 下的清单 JSON ——
   那是清单在**记录**它，不是在**用**它）；
2. 或者它的全部使用点都落在**已判死**的函数体内（级联死，迭代到不动点）。

**不用调用图可达性**，因为两种写法会让可达性误判：

- `stdlib/sys/sys.c:92` 的 `.init = sys_mutex_body_init,` 把函数挂进 VM 类描述符表。
  那张表不被任何函数调用，但它是活的数据。可达性会把这一族 10 个函数全判成死代码。
- 协程 step 回调（`net_copy_step`、`os_sleep_done`）和 signal handler 是**作为函数指针注册**的
  （`xr_yield_for_timeout(X, ms, os_sleep_done, NULL, result)`）。
  只认 `name(` 形式的调用会把它们全判成死代码。

两个陷阱都实测踩过：前者让 sys 假阳性 10 条，后者让 net 假阳性 25 条。
**判 A 判错的代价是构建断裂，所以判据只能保守。**

### 2.2 名单

| C 函数 | 位置 | 行数 |
|---|---|---|
| `xr_io_read` | `stdlib/net/io.c:222-239` | 18 |
| `xr_io_read_full` | `stdlib/net/io.c:241-251` | 11 |
| `xr_io_write` | `stdlib/net/io.c:253-267` | 15 |
| `xr_io_write_all` | `stdlib/net/io.c:269-279` | 11 |
| `xr_io_writev` | `stdlib/net/io.c:281-352` | 72 |
| `xr_io_accept` | `stdlib/net/io.c:423-441` | 19 |
| `xr_io_accept_tls_with_ctx` | `stdlib/net/io.c:443-468` | 27 |
| `xr_io_get_error` | `stdlib/net/io.c:497-499` | 3 |
| `alpn_select_cb` | `stdlib/net/tls.c:320-362` | 43 |
| `xr_tls_cleanup` | `tls.c:66-72` + `tls.c:721-722` | 9 |
| `xr_tls_conn_get_fd` | `tls.c:621-625` + `tls.c:828-831` | 9 |
| `xr_tls_conn_get_session` | `tls.c:662-668` + `tls.c:846-849` | 11 |
| `xr_tls_conn_set_session` | `tls.c:670-679` + `tls.c:850-854` | 15 |
| `xr_tls_conn_is_resumed` | `tls.c:687-691` + `tls.c:858-861` | 9 |
| `xr_tls_conn_handshake_server` | `tls.c:416-448` + `tls.c:786-790` | 38 |
| `xr_tls_session_free` | `tls.c:681-685` + `tls.c:855-857` | 8 |
| `xr_tls_context_set_alpn_callback` | `tls.c:364-370` + `tls.c:759-763` | 13 |
| `xr_tls_context_set_client_cert` | `tls.c:633-647` + `tls.c:836-841` | 22 |
| `xr_tls_context_enable_session_cache` | `tls.c:649-660` + `tls.c:842-845` | 16 |
| `xr_tls_context_enable_ocsp_stapling` | `tls.c:693-705` + `tls.c:862-865` | 17 |

`tls.c` 的每个函数有**两份定义**（`#ifdef XR_HAVE_OPENSSL` 一份实现 + `#else` 一份 stub），
两份都删了。连同各自的 `.h` 声明和前置注释块，四个文件合计 **519 行**：

```
stdlib/net/io.c   499 -> 314   (-185)
stdlib/net/io.h   155 -> 110   (-45)
stdlib/net/tls.c  867 -> 644   (-223)
stdlib/net/tls.h  199 -> 133   (-66)
```

### 2.3 这 20 条是一个整体，不能分批删

它们互相依赖，构成两条闭合的死链：

```
xr_io_read_full ──> xr_io_read
xr_io_writev ──> xr_io_write_all ──> xr_io_write
xr_io_accept_tls_with_ctx ──> xr_io_accept
                          └─> xr_tls_conn_handshake_server   (io.c:459，唯一使用点)
xr_tls_context_set_alpn_callback ──> alpn_select_cb          (tls.c:369，唯一使用点)
```

`xr_tls_conn_handshake_server` 和 `alpn_select_cb` **各有一处真实使用点**，
但那两处分别落在 `xr_io_accept_tls_with_ctx` 和 `xr_tls_context_set_alpn_callback` 的函数体内 ——
只有死代码在用它们。**只删一半会留下悬空引用。**

### 2.4 删掉的到底是什么能力

`io.c` 那 8 条是一整套**从未接通的服务端 TLS accept 路径**：

- `net.xr` 只暴露**客户端** TLS（`_dialTls` 走 `__tlsHandshake`，`upgradeTLS` 走 STARTTLS）。
  全文件没有任何服务端 TLS API。
- 服务端 TLS 的唯一 C 使用者是 `stdlib/cluster/cluster.c:324`，
  它自己调 `xr_tls_conn_new(c->tls_server_ctx, fd)`，**不经过** `xr_io_accept_tls_with_ctx`。
- 所以 `io.c` 的这条路径既没有 Xray 侧入口，也没有 C 侧消费者。

`tls.c` 那 12 条是**会话复用 + OCSP 装订 + 客户端证书 + ALPN 回调**的完整 C API 表面，
`tls.h` 逐个导出，仓库内零调用。属于「写好了备用但从未接线」。

**删除不损失任何已暴露的能力。** 要恢复这些功能时，
正确的做法是从 `.xr` 侧的需求倒推重新设计叶子，而不是复活这批没有调用约定的 C。

---

## 3. B 档：185 条叶子实现体（留）

这是最大的一档，全部是 `c-function` kind —— 即**不在 core.def 里声明、只存在于 C 文件内部**的函数。
它们是那 69 条已批准叶子的实现体和内部辅助。

**为什么留**：删掉它们等于删掉叶子本身。它们的存在理由 = 它们服务的那条叶子的存在理由，
而每条叶子在 `stdlib/native_leaf_allowlist.toml` 里都有一条记录，
写明 `class` / `abi` / `ownership` / `effect` / `deletion_trigger`。

**为什么不能靠拆分降数字**：门数的是「函数个数」而不是「代码行数」，
把一个 200 行的叶子实现体拆成 5 个 40 行的辅助函数，门的数字会**上升 4**。
反过来，把 5 个辅助函数内联进一个大函数能让数字降 4，但那是纯粹的数字游戏，
会让 `sys.c` / `net.c` 更难读。**本 lane 不做这种事。**

这一档要看的不是数量，而是**每条叶子的记录准不准**。见 §5。

---

## 4. C 档：3 条（`build_identity_leaf`，`abi = none`）

全 306 条叶子里只有这 3 条**不跨任何主机 ABI**；
另有 1 条 `c-function` 是逻辑重复（见 §4.1）。合起来是全部有迁移余地的部分：

| 符号 | VM 绑定 | AOT 绑定 | 内容 |
|---|---|---|---|
| `os.__platform` | `os_platform` | `xrt_os_platform` | 一个 `#if` 阶梯选出的编译期常量字符串 |
| `os.__arch` | `os_arch` | `xrt_os_arch` | 同上，`XR_ARCH_*` 阶梯 |
| `net.__hasTLS` | `net_has_tls` | `xrt_net_has_tls` | 编译期是否链接了 TLS 提供方 |

**为什么本 lane 不迁**：这三条的 `deletion_trigger` 已经写明了迁移条件，
而那个条件**不在本 lane 的允许改动范围内**（`src/` 全部禁改）：

> `deletion_trigger = when the compiler exposes the target platform to Xray source as a
> constant, at which point os.xr can name it without a call`

也就是说，要迁走它们，得让**编译器**把目标平台/架构/TLS 能力作为常量暴露给 `.xr`，
这是 `src/frontend` 或 `src/plan` 的改动。在那之前，
把 `__platform()` 换成 `.xr` 里的字符串字面量只会**把编译期常量变成运行时值**，是倒退。

### 4.1 第 4 条：`net_error_variant_index` —— 和 `_classify` 是同一张表写两遍

这一条不在叶子清单里（它是 `c-function`，本来归 B 档），
但它是**唯一一处「本该在 Xray 侧、逻辑上确实可迁」的重复**，单列出来。

| | 位置 | 内容 |
|---|---|---|
| C 侧 | `stdlib/net/net.c:1272-1297` `net_error_variant_index` | `XR_NETERR_* -> NET_ERROR_*` 的 switch |
| Xray 侧 | `stdlib/net/net.xr:31-41` `_classify` | `_CODE_* -> NetError.*` 的 if 链 |

两张表逐项对应（Timeout/Closed/Reset/Refused/Dns/Tls/Invalid/Cancelled，其余落 Io），
映射的是同一个 `XrNetErrorKind` 值域。

**消费面很窄**：`net_error_variant_index` 只被 `net_publish_error`（`net.c:1303`）调用，
后者只在 `copyBidirectional` 的协程路径上用了 7 次（`net.c:1629`–`1706`）。
其余所有 net 原语都已经走「返回可移植 i64 码 + `.xr` 侧 `_classify`」这条路——
这一处是上一轮迁移**没有覆盖到的最后一块**。

**理论上的迁法**：让 `__copyBidirectional` 把错误作为 i64 码交回，
`net.xr` 的 `copyBidirectional` 用现成的 `_classify` 转成 `NetError`，
然后删掉 `NetErrorVariant` / `net_error_variant_index` / `net_error_type` / `net_publish_error` 四个 C 函数。
改动**完全落在 `stdlib/net/` 内**，不碰 core.def，不移动指纹。

**本 lane 不做的理由**：`src/aot/xrt_net.h` 里**没有**对应的这张表——
AOT 侧的 `copyBidirectional` 错误路径和 VM 侧本来就不同构。
改 VM 侧而不能改 AOT 侧（`src/` 禁改）会把一处**已经存在但静止**的 VM/AOT 发散
变成一处**新的、活动的**发散，而验证它需要差分套件，
当前 load（12 条 lane 并行）下差分结果不可归因（见 §7）。
**建议**：与能改 `src/aot/` 的 lane 合并做，一次把 VM 和 AOT 两侧都切到 i64 码。

**已确认修复**：上一轮记录的「`os.arch` 有两份发散的 `#if` 表，
VM 报不出 `LANGUAGE_SPEC.md` 文档化的 `ppc64`/`riscv64`」这个缺陷，
在本基线上已经收敛成一条 `__arch()` 叶子，`leaf_reason` 里明确写着
「Two copies of the same ladder; neither calls uname(2) or sysctl(3)」——
两份阶梯仍在（VM `stdlib/os/os.c get_arch`，AOT `src/aot/xrt_os.h xrt_os_arch_cstr`），
但它们现在是同一条叶子的两个后端实现，发散风险已被记录在案。

---

## 5. D 档：96 条平台原语（留）

### 5.1 66 条已批准叶子

| 模块 | `host_abi_leaf` | `runtime_leaf` | `security_provider_leaf` | `build_identity_leaf` |
|---|---|---|---|---|
| sys | 14 | 2 | 0 | 0 |
| os | 23 | 1 | 0 | 2 |
| net | 15 | 10 | 1 | 1 |

**简报要求的「必须是有记录的叶子而不是裸的内部函数」这一条已经满足**：
`stdlib_native_leaf_allowlist` 门在本基线上是 **PASS（0 条未分类叶子）**，
三模块的 69 条叶子在 `native_leaf_allowlist.toml` 里逐条有记录。

不可迁的理由按 class 分：

- `host_abi_leaf`（52）：pthread、syscall、libc。Xray 语言层没有这些原语，也不该有。
- `runtime_leaf`（13）：VM 内部对象生命周期（句柄创建、协程挂起点、netpoll 注册）。
  它们跨的是 Xray 运行时自己的边界，不是主机边界，但同样不可能用 `.xr` 表达。
- `security_provider_leaf`（1）：OpenSSL。

### 5.2 30 条公开原生表面（归 6-1，本 lane 不动）

| 模块 | 符号 | 它们的 C 体 |
|---|---|---|
| sys（22） | `OsMutex`/`OsRwLock`/`OsCondvar`/`OsBarrier`/`OsOnce` 五个类 + 13 个方法 + `cpuCount`/`threadYield`/`sleepMs`/`pinToCpu` | `sys_mutex_new` / `sys_mutex_lock` … 全是 pthread 封装 |
| net（8） | `NetConn.{fd,close,isClosed,isTLS}`、`NetListener.{fd,port,close,isClosed}` | `conn_method_fd` / `listener_method_port` … 句柄字段访问器 |

这 30 条**不是已批准叶子**（叶子都是私有 `__*`），而是公开 native 符号，
计在 `stdlib_no_public_native_surface` 的 128 里。

要清掉它们，`StdlibNativeClassEntry` / `StdlibClassMethodEntry` 需要有 `is_internal` 属性
（见 `REF-public-native-128.md` §2：这两类和 `const`/`type_method`/`class`/`class_field` 一样恒为 public），
那是 `tools/stdlibgen/stdlibgen.py` 的改动，归 6-1。

**net 那 8 个 class_method 的处置建议**（有充分证据，但本 lane 判断不该现在做）：

- 它们在 stdlib 内部**零调用**——`grep` 全部 `stdlib/**/*.xr`，
  命中的 `.close()` 只有 `io.xr:128`/`io.xr:147`（File 句柄）和 `ws.xr:1175`（WsConn）。
- 但 `tests/` 里有**大量**调用方：`lst.port()` 遍布 `1432`–`1438`、`1502`、`1509` 等回归用例、
  3 个差分用例、以及 `tests/stdlib/contracts/net/probes/{current,legacy}.xr` 两个契约探针。
- `net.xr` 已有 `export fn close(handle)` 和 `export fn fd(handle)` 作为等价自由函数，
  **但没有 `port()` / `isClosed()` / `isTLS()` 的自由函数**。
  直接删这 8 个会真的砍掉「取监听端口」这个能力，除非同时补 3 条新叶子
  （`__listenerPort` / `__isTLS` / `__isClosed`）和 3 个 `.xr` 自由函数。
- 收益是门从 128 降到 120；`NetConn`/`NetListener`/`NetError` 这 3 个类型名**仍然清不掉**
  （要等 6-1），所以 net 的公开面只能 11 → 3，不能清零。
- 代价包括：改 core.def（**移动全树指纹**，要重锚 ~24 个 KAT 值）、
  新增 3 条叶子、改几十个测试文件、同步改两处硬编码
  （`check_binary_public_native_readiness.py:26-42` 和 `check_stdlib_boundary.py:260-268`，
  net 那 11 个名字在这两处**逐字复制了两份**）。

**本 lane 不做的理由**：十条 lane 同机并行，`00-SHARED.md` 明令不得跑全量 ctest，
而这个改动要动几十个回归用例和两个契约探针——**改完在本窗口内验证不了**。
在验证不了的前提下做大面积破坏性改动，不是「不做兼容层」，是赌。
建议留给能跑全量回归的窗口，与 6-1 的 stdlibgen 改动一起落，一次把 net 公开面清到 0。

---

## 6. 顺带查清的三件事

### 6.1 简报的「`xneterror.c` 的错误映射可迁」——**实测不成立**

net 有**两套互不相干**的错误枚举，不是同一张表写了两遍：

| 枚举 | 定义 | 码数 | 面向谁 |
|---|---|---|---|
| `XrNetErrorKind` | `src/io/xnet_handle.h:55-66` | 10（0–9） | **面向脚本**。`net.__lastCode` 原样返回，`net.xr:15-23` 的 `_CODE_*` 镜像它，`net.xr:31-41` 的 `_classify` 映射到 `NetError` 变体 |
| `XrNetError` | `stdlib/net/xneterror.h` | 23 | **C 内部**跨模块（net/io/tls/http/ws/cluster）诊断枚举 |

`xneterror.c`（44 行，唯一符号 `net_error_string`）是**后者**的人类可读字符串表，
唯一调用方是 `tls.c:628`。`XrNetError` 的值**从不跨越到 Xray 侧**，
所以把这张表迁进 `.xr` 会得到一个没有消费者的函数。

**真正的错误分类早就在 Xray 侧了** —— `_classify` 是上一轮迁移的成果。
`xneterror.c` 判 B 档（服务于 tls 诊断），保留。

`src/io/xnet_handle.h:50-53` 的注释把这层契约写得很清楚：

> Portable network error codes. The numbering is a stable script-facing contract:
> net.__lastCode returns these values verbatim […] so renumbering is a breaking
> semantic change, not a refactor.

### 6.2 `os` 为什么能没有 C 工厂

`stdlib/stdlib_boundary.toml` 里 **`os` 段没有 `factory_source` 字段**，
而 `sys` 段有 `factory_source = "stdlib/sys/sys.c"`、`net` 段有 `"stdlib/net/net.c"`。
`os` 是全 stdlib **唯一没有模块工厂的生产模块**（`stdlib_no_module_specific_c_loader` 数到 30 个，不含 os）。

对照三个模块的符号构成，充要条件很清楚：

| | native_class | class_method | object_shape | handle | 有工厂？ |
|---|---|---|---|---|---|
| `sys` | 5 | 13 | 0 | 0 | 有 |
| `net` | 2 | 8 | 1 | 0 | 有 |
| `os` | 0 | 0 | 0 | **1**（`__ExecResult`） | **无** |

**关键点：`handle` 不是障碍，`native_class` 才是。**
`os` 有一个 handle（`__ExecResult`）却仍然不需要工厂，
因为 handle 走的是 `xa_register_stdlib_native_module_types` 的通用路径
（`src/frontend/analyzer/xanalyzer.c:1861` `xa_register_stdlib_native_module_types`，
handles 循环在 `:1873`，数据来自 `XaBuiltinModule->handles[]`，
由 `scripts/gen_stdlib_types.py` 从 core.def 生成）。
object_shape 和 enum 也走同一个函数的另外两个循环（`:1880` / `:1907`）。

而 **`XaBuiltinModule` 结构体里根本没有 class 数组**
（`xanalyzer_builtins.h:128-138`：只有 functions / handles / object_shapes / enums 四组），
`gen_stdlib_types.py` 也从不调用
`parse_native_classes` / `parse_class_methods`。
所以有 native_class 的模块必须靠手写 C 工厂去注册那些类 —— 这就是 sys 和 net 还有工厂的原因。

**给 6-2（通用加载路径）的结论**：
别的模块要复刻 os，充分条件是**该模块在 core.def 里没有 `native_class` / `class` 条目**。
handle / object_shape / enum 都已经有通用路径。
要让有 native class 的模块也走通用路径，得先按 `REF-native-class-nameability.md` §4 的方案 A
把 class 纳入 `XaBuiltinModule`（改 3 处：`xanalyzer_builtins.h` 加 `XaBuiltinClass`、
`gen_stdlib_types.py` 加 `load_def_module_classes`、`xanalyzer.c:1873` 的 handles 循环后加一个同形循环）。
**那三处全在 `src/` 和 `scripts/`，是 6-2/6-1 的地盘，不是本 lane 的。**

### 6.3 `tls.c` 的双定义结构

`tls.c` 的每个公开函数都有两份定义：`#ifdef XR_HAVE_OPENSSL` 下的真实现，
和 `#else` 下返回错误的 stub。**删除时必须成对删**，
这和 `REF-public-native-128.md` 提醒的「22 个符号在 core.def 里有两个条目块」是同一类陷阱，
只是发生在 C 侧而不是 `.def` 侧。

本 lane 的死代码判据因此不能依赖「下一个定义行 - 1」这种函数体范围推断 ——
双定义会让范围跨越几十个函数（实测 `xr_tls_cleanup` 被算成 657 行）。
最终判据改用花括号配对求每份定义各自的范围。

---

## 7. 没做的事和理由

| 事项 | 为什么没做 |
|---|---|
| net 公开面 11 → 3 | 要改几十个回归用例 + 2 个契约探针，本窗口（十 lane 并行、禁跑全量 ctest）验证不了。方案和代价见 §5.2 |
| sys 22 + net 8 的公开面清零 | 卡在 `stdlibgen` 的 `is_internal` 缺口，归 6-1 |
| 3 条 `build_identity_leaf` 迁移 | 卡在「编译器把目标平台暴露为 `.xr` 常量」，要改 `src/`，本 lane 禁改 |
| 差分套件前后对比 | 见下 |

**差分套件**：基线跑过一次，但当时 `load average 113`（12 条 lane 同时在跑差分），
日志里大量 `native-run: probe stage timed out after 10000 ms`，
按 `00-SHARED.md` 的纪律这种结果在串行复跑前不可归因。
更重要的是**本 lane 的改动性质不需要它**：删的是 C 死代码，
没有把任何 C 迁成 `.xr` 模块体，不改变任何模块的 AOT 形状，
而差分套件测的正是「同一个程序在 VM 和 AOT 下的行为差异」。
`00-SHARED.md` 警示的那个陷阱（迁移让程序从「绕开 AOT」变成「要过 AOT」）在这里不适用。

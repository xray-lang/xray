# r3-6-8：cluster 的控制平面提到 Xray 之后，C 里剩下的是什么

基线 `34be0379c`，改动提交 `90c9c9be4`。

这份文档回答两个问题：**这一轮消灭了哪几处 VM/AOT 语义分叉**，以及
**为什么 cluster 的 C 语义所有者只从 170 降到 169**——后一个问题的答案不是
「没做完」，而是 cluster 剩下的 C 与 regex / compress / mem 剩下的 C 不是同一类东西。

---

## 一、这一轮消灭的分叉

每条都给了两侧的证据位置。行号是改动前的。

### 分叉 1：地址语法有三份实现，三套语义

| | 实现 | 行为 |
|---|---|---|
| VM | `cluster.c:1141-1163` | `strrchr(':')` 切分，**不脱 IPv6 方括号**，`strtol` 严格十进制 |
| AOT | `cluster_aot.c:515-536` | 脱方括号、拒绝裸 IPv6，**端口原文直接交给 `getaddrinfo(3)`** |
| `.xr` | `cluster.xr` `parseAddress` | 脱方括号、拒绝裸 IPv6、严格十进制、抛 `AddressError` |

两个可观察差异：

- `cluster.join("db:http")`：`getaddrinfo(3)` 也解析**服务名**，所以 AOT 上连上 80 端口，
  VM 上返回 `false`。
- `cluster.join("[::1]:9000")`：VM 把 host 交给 `getaddrinfo` 时是 `[::1`，解析失败；AOT 正确。

第二条早就被契约登记过：`tests/stdlib/contracts/cluster/contract.toml`
的 `known_legacy_bugs` 就是「未加方括号的 IPv6 被最后冒号切分歧义接受」。

**做法**：`__join` 的签名从 `(addr: string)` 改成 `(host: string, port: i64)`，
`.xr` 的 `join()` 先 `parseAddress`。`aot_cluster_parse_address` 整个删除，
`aot_cluster_connect` 改为从整数渲染端口十进制串——**一个由整数生成的串不可能是服务名**，
所以那个 bug 在结构上不可能复发。

### 分叉 2：准入规则被重复决定 2–3 次

| 规则 | 改前决定的地方 | 改后 |
|---|---|---|
| topic 合法性 | `cluster_topic.c:519`、`cluster_aot.c:772` | `.xr` `send()` |
| pattern 合法性 | `cluster_topic.c:352`、`cluster.c:1350`(间接)、`cluster_aot.c:818` | `.xr` `listen()` |
| 订阅 capacity 上下界 | `cluster.c:1350`、`cluster_topic.c:353`、`cluster_aot.c:818` | `.xr` `listen()` |
| 端口范围 | `cluster.c:1105`、`cluster_aot.c:679` | `.xr` `start()` |

顺带改了一处**顺序**，是有意的：`send()` 现在在 `stop()` 之后对拼错的 topic 答
`InvalidTopic`，而叶子过去答 `Unavailable`。理由写在 `cluster.xr` 的注释里——
topic 拼错与本节点是否在跑无关，用 `Unavailable` 盖住拼写错误是把两件事压成一件。

### 分叉 3：节点名两边都不校验

`cluster_runtime_start` 有一段可打印 ASCII + 长度检查，但 `xrt_cluster_start` 没有；
而且两边都用 `strncpy` 写进 64 字节定长字段。名字在第 64 字节之后才分叉的两个节点，
对集群来说是同一个节点。

`.xr` 的 `start()` 现在先过 `validNodeName`，C 侧那段检查删掉——**一个规则一个所有者**，
而截断的拷贝再也拿不到会被截断的名字。

### 分叉 4：入站 topic 只有一侧校验

从线上来的 topic 从没经过 `cluster.xr`（对端可能跑任何东西）。

- AOT 一直校验：`aot_cluster_deliver_local` → `xr_cluster_topic_matches` 内部调
  `xr_cluster_topic_valid`
- VM 不校验：`cluster_transport_handle_frame` 直接走 trie

畸形的线上 topic 因此能在一个后端触达订阅者、在另一个后端不能。现在两边都问，
问的是同一个函数。`cluster_topic_core.h` 的头注释改写了：它存在的理由从
「topic 语言的所有者」变成「**入站帧的防线**」，语言本身归 `cluster.xr`。

### 附带：两个常量从 C 的默认值变成 `.xr` 传下来的参数

- 心跳时间表 `5000 / 15000 / 3`（`cluster.c:504-506` 硬编码）
- hop 预算 `XR_TOPIC_DEFAULT_HOP_LIMIT`（`cluster_topic.c:534`、`cluster_aot.c:793`）

现在是 `cluster.xr` 的 `HEARTBEAT_INTERVAL_MS` / `HEARTBEAT_TIMEOUT_MS` /
`MAX_MISSED_HEARTBEATS` / `TOPIC_DEFAULT_HOP_LIMIT`，经 `__start` / `__send` 传下去。
AOT 侧把心跳时间表**存起来但不跑**（它还没有心跳协程），字段旁的注释写明了这一点：
一个有单一所有者的存值是诚实的，第二份默认值不是。

---

## 二、没有消灭、也不该在这一轮消灭的分叉

三条，`stdlib/native_leaf_allowlist.toml` 现在逐条记了。

1. **多跳转发**。VM 收到 transport 帧后，本地投递 + 以 `hop-1` 转发给其余 peer
   （split-horizon，`cluster_topic.c:479-512`）；AOT 只本地投递，
   **hop 字节读都不读**（`cluster_aot.c:337-353`）。
   A—B—C 三节点链，A 发的消息 VM 上 C 收得到，AOT 上收不到。
2. **心跳与故障检测**。VM 有 phi accrual（阈值 8.0）、超时、missed 计数、死节点墓碑；
   AOT 只回 PONG，没有心跳发送、没有超时、没有 phi、没有墓碑。
3. **TLS**。`xrt_cluster_start` 把六个 TLS 参数全部丢弃，`tlsEnabled` 为真直接返回 false。

三条都在**传输机制**里，不在规则里。`.xr` 够不着它们，因为它们发生在没有 Xray 栈的
reader/writer 循环里。allowlist 的 `deletion_trigger` 写了各自需要什么才能删。

---

## 三、为什么计数只降 1：169 个 C 所有者的角色分类

`stdlib_no_handwritten_c_semantic_owner` 数的是**每一个 `.c` 里的函数定义**
（`scripts/stdlib_symbol_inventory.py` 的 `c_functions` 只扫 `.c`，不扫 `.h`）。
所以让这个数下降的唯一办法是**删掉 C 函数**，而删掉的前提是 C 里不再有人调用它。

169 个按角色分（脚本按名字模式 + 调用图分类，逐个可核对）：

| 数量 | 角色 | 能不能由 `.xr` 取代 |
|---:|---|---|
| 68 | 运行时状态：节点表、输出队列、订阅表、锁、引用计数、握手驱动 | 否——`.xr` 没有这些结构的句柄 |
| 49 | netpoll 协程状态机（`_entry` / `_continue` / `_drive` / `_wait` / `_destroy` 样板） | 否——这是调度器的形状 |
| 22 | 帧与 announce 报文编解码 | **否，但理由不同**：见下 |
| 14 | OS 线程与 socket 适配（AOT 侧） | 否 |
| 6 | 订阅 trie 索引 | 否——入站投递在 C 的 reader 里 |
| 6 | 纯计算：phi 三函数、proof 两函数、`should_connect` | 否——全部在 C 的 I/O 循环里被调用 |
| 4 | VM 值构造与绑定入口 | 否——归 6-1 的描述符类问题 |

**22 个编解码函数是最容易被误判为「可迁」的一类。** 它们确实是纯字节运算，
`.xr` 完全能写。但删不掉，因为**握手和心跳的编解码发生在 reader/writer 协程里**——
那里没有 Xray 栈可以回调。把它们在 `.xr` 里再写一遍，得到的不是「迁移」，
而是**同一个规则的第二个所有者**，正是 `2ec8253c8`（compress 迁移）的提交信息
明确批判的事情：*a second copy of a rule is a copy that can drift*。

所以这一轮**没有**把 `cluster_proto.c` 翻译成 Xray。取而代之的是：
线格式在 `cluster.xr` 的文件头被**规范陈述**（normative），
`cluster_proto.c` 是两个后端共享的唯一实现，
`tests/unit/stdlib/test_cluster_proto.c` 从 2 个用例扩到 10 个，
逐字节钉住每一种帧，并且有一个用例专门断言
**`.xr` 的常量等于 C 的宏**——这是陈述与实现之间唯一的机械连接。

### 明确拒绝的两条捷径

- **把 `.c` 里的函数搬进 `.h`**。inventory 只扫 `.c`，搬过去计数立刻下降，
  而没有任何东西变好。不做。
- **合并只有单一调用者的间接层**（比如 `get_u64` + `get_i64`）。
  能减 1–2，但没有语义收益，属于为凑数而改。不做。

---

## 四、真要让这个数下降，需要什么

只有一条路：**把叶子边界从「发一条消息」下移到「读一帧 / 写一帧」**，
让 `cluster.xr` 拥有协议状态机，C 只剩字节进出。

前置条件（都不在这条 lane 的范围内）：

1. `net` 暴露 `setsockopt(2)` 的多播选项，以及 `cluster.xr` 能命名的
   connect/TLS 握手叶子——`__discover` 和 `__join` 的 `deletion_trigger` 就是这两条。
2. `blockers/a-stdlib-multi-module-program-authority.md` 的 multi-module program
   authority——否则 `cluster.xr` 调 `net` 的叶子跨不过目标层。
3. Channel 构造与协程注册表成为 Xray 源能命名的表面——`__listen` 和 `__monitor` 的
   `deletion_trigger`。

在这三条之前，cluster 的 C 计数只能靠**删掉重复的规则**下降，而重复的规则
这一轮已经删完了。

---

## 五、本轮的测量

| 项 | 结果 |
|---|---|
| `./build-nofp/tests/unit/test_cluster_proto` | 10 / 10 PASS（改前 2 / 2） |
| `test_xi_cgen` PASS 数 | 42（基线 42，重锚后恢复） |
| `1510_cluster_typed_control` | PASS |
| `1511_cluster_opaque_transport` | PASS |
| `cluster_protocol_pure_direct.xr` 探针 | 输出与改前逐行相同 |
| `cluster_typed_control_surface.xr` | 与 `.expected` 完全一致 |
| `stdlib_native_leaf_allowlist` | PASS（0） |
| `stdlib_no_handwritten_c_semantic_owner` | 844 → 843 |
| `contract_freeze` | PASS（28 contracts，重锚 6 个摘要） |

`1511` 顺带证明了三件语言层面的事（都是这次改动依赖的，之前没有先例）：
`.xr` 里 `move Buffer` 参数**可以在校验失败时提前返回**而不转移；
`move` 参数**可以读**（`len(envelope.asBytes())`）；
模块体内**可以构造** `.def` 声明的 enum 变体（`ClusterDelivery.InvalidTopic`）。

---
id: spec.15_standard_library
order: 016
---

<!-- xr-spec:cn -->
---

## 15. 标准库概览 (Standard Library)

> 真值源：标准库实现与 analyzer builtin metadata。
> MCP knowledge 通过 `xray builtin-dump` 获取 API 签名并在生成时注入模块知识卡片。
> 详见 [附录 D stdlib 模块索引](#d-stdlib-模块索引)。

> **真实 native 模块清单**（22 个，源码：`stdlib/<module>/*.c`）：
>
> `base64`、`cluster`、`compress`、`crypto`、`csv`、`datetime`、`encoding`、`gc`、`http`、`io`、`log`、`math`、`net`、`os`、`path`、`regex`、`time`、`toml`、`url`、`ws`、`xml`、`yaml`。
>
> 不需要 import 的内置类型由 prelude 注册（`Array` `Map` `Set` `Json` `Channel` `Bytes` `BigInt` `StringBuilder` `Exception` `Regex` `Logger` `NetConn` `NetListener` 等）。详见 §1.5.6 / §2.2。

### 15.1 文件 IO 与系统

| 模块 | 主题 | 关键 API |
|--|--|--|
| `io` | 文件 IO + 文件系统 | `readFile` `writeFile` `exists` `mkdir` `remove` `readdir` `stat` `stdin` `stdout` `stderr` |
| `path` | 路径操作 | `join` `dirname` `basename` `extname` `normalize` `isAbsolute` `resolve` `relative` `parse` `format` |
| `os` | 操作系统接口 | `getenv` `setenv` `environ` `exit` `getpid` `getcwd` `chdir` `hostname` `tmpdir` `homedir` `cpuCount` `sleep` `exec`；常量 `platform` `arch` `sep` `eol` |

> xray **没有**独立的 `fs` 模块，文件系统操作在 `io` 中；进程参数 / 进程信息走全局 `process` 对象（`process.args` / `process.file` / `process.dir`，见 §16.5），不在 `os` 中。
> `os.platform` / `os.arch` / `os.sep` / `os.eol` 是**常量字符串**，不带括号；其余 `os.*` 是函数调用。

### 15.2 网络

| 模块 | 主题 | 关键 API |
|--|--|--|
| `net` | TCP / UDP / TLS socket + DNS | `listen` `dial` `accept` `read` `readInto` `write` `writeBytes` `copy` `copyBidirectional` `setDeadline` `lastError` `lookup` `dialTLS` `NetConn` `NetListener` |
| `http` | HTTP / HTTPS 客户端 + 服务端 + HTTP/2 | `get` `post` `request` `Server` `urlEncode` `urlDecode` |
| `ws` | WebSocket | 客户端/服务端连接 |
| `url` | URL 解析与构造 | `parse` `format` `parseQuery` `buildQuery` `encode` `decode` |

> DNS 查询通过 `net.lookup(host)` 完成；没有独立的 `dns` 模块。

#### 15.2.1 TCP 数据路径

`net` 的 TCP API 明确区分三类数据路径：

- `read(conn)` / `write(conn, data)`：消息型路径，把 payload 暴露为 Xray `string`，适合协议解析、文本处理和需要检查内容的逻辑。
- `readInto(conn, bytes, maxlen?)` / `writeBytes(conn, bytes)`：可复用 `Bytes` buffer 路径，适合二进制协议热路径，避免为每个包创建临时字符串。
- `copy(src, dst)` / `copyBidirectional(a, b)`：流式 native 路径，payload 保持在可复用 C buffer 中，适合 proxy、relay、`copy(conn, conn)` echo 和其他不需要语言层查看每个字节的高吞吐场景。

设计原则：raw stream 不应为了“经过语言层”而创建临时字符串；只有业务逻辑需要看数据时才使用字符串 API。

TCP 等待操作使用协程友好的 netpoll 路径。`setReadDeadline(conn, deadline)`、`setWriteDeadline(conn, deadline)`、`setDeadline(conn, deadline)` 和 `setAcceptDeadline(listener, deadline)` 接受 `time.monotonic()` 毫秒 deadline；超时后操作按自身返回形态返回 `null` 或 `-1`，并通过 `lastError(handle)` / `lastErrno(handle)` 暴露诊断原因。

`shutdownRead(conn)`、`shutdownWrite(conn)` 和 `shutdown(conn)` 暴露 TCP 半关闭语义。通用 proxy/relay 应优先使用 `copyBidirectional(a, b)`，它会在单向 EOF 后半关闭对端写侧，并返回两个方向的字节统计。

TLS client 路径通过 `dialTLS(host, port, timeout?)` 和 `upgradeTLS(conn, hostname, timeout?)` 提供；TLS read/write/copy 与 plain TCP 共享同一套 deadline、错误诊断和 typed handle 生命周期语义。

### 15.3 数据格式

| 模块 | 主题 |
|--|--|
| `yaml` | YAML |
| `toml` | TOML |
| `xml` | XML |
| `csv` | CSV |
| `base64` | Base64 编/解 |
| `encoding` | hex / UTF-8 等通用编码（不含 Base64，base64 在自身模块） |

> JSON 编解码**不在**单独的 `json` 模块；通过内置类型 `Json` 的静态方法 `Json.parse(s)` / `Json.stringify(v)` 使用（无需 import；见 §14.10）。

### 15.4 加密与哈希

| 模块 | 关键 API |
|--|--|
| `crypto` | `md5` `sha1` `sha256` `sha512` `hmac` `aes` `rsa` 等；详细 API 详见 stdlib 源码 |

> stdlib **没有**独立的 `random` 模块；如需伪随机数请使用 `crypto` 模块的随机源或 `math` 模块的工具函数。

### 15.5 压缩

| 模块 | 关键 API |
|--|--|
| `compress` | `gzip` / `gunzip`、`deflate` / `inflate` 等 |

### 15.6 时间

| 模块 | 关键 API |
|--|--|
| `time` | `now()` `monotonic()` `sleep(ms)` `Duration` |
| `datetime` | `DateTime` / `Date` / `Time` 解析、格式化（详见 §14.12） |

### 15.7 数学

| 模块 | 关键 API |
|--|--|
| `math` | `sin` `cos` `tan` `log` `pow` `sqrt` `floor` `ceil` `round` `abs` `min` `max` 等；常量 `PI` / `E` / `MAX_INT` / `MIN_INT` |

### 15.8 文本

| 模块 | 关键 API |
|--|--|
| `regex` | `compile(pattern)` 返回 `Regex`；详见 §14.13。也支持 `/pattern/flags` 字面量 |

> stdlib **没有** `strconv` 模块；字符串 ↔ 数值转换使用内置函数 `int(s)` / `float(s)` / `string(n)`（见 §13.2）。

### 15.9 日志与诊断

| 模块 | 关键 API |
|--|--|
| `log` | `debug` / `info` / `warn` / `error` / `fatal` / `child()`、source 位置开关、异步写入模式 |
| `gc` | `collect()` `isrunning()` `count()` `state()` `stats()` |

### 15.10 分布式

| 模块 | 主题 |
|--|--|
| `cluster` | 节点发现、健康检查、Topic 消息总线（见 stdlib/cluster/）|

### 15.11 测试

`@test` 注解 + 全局 `assert*` 函数即可，**不需要**额外的 `test` 模块（见 §12）。

### 15.12 已**不存在**的模块

文档中可能引用过、但当前 stdlib 中**确实没有**的模块（避免误导）：

`fs` · `process` · `dns` · `random` · `strconv` · `sync` · `runtime` · `json`

这些功能或者归入其他模块（见上面各小节注），或者尚未实现。

> **完整索引**：见[附录 D](#d-stdlib-模块索引)。
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 15. Standard Library Overview

> Source of truth: stdlib implementations and analyzer builtin metadata.
> MCP knowledge fetches API signatures via `xray builtin-dump` and injects per-module knowledge cards at generation time.
> See [Appendix D — stdlib module index](#d-stdlib-module-index).

> **Authoritative native module list** (22 modules; source: `stdlib/<module>/*.c`):
>
> `base64`, `cluster`, `compress`, `crypto`, `csv`, `datetime`, `encoding`, `gc`, `http`, `io`, `log`, `math`, `net`, `os`, `path`, `regex`, `time`, `toml`, `url`, `ws`, `xml`, `yaml`.
>
> Built-in types that need no import are registered by the prelude (`Array`, `Map`, `Set`, `Json`, `Channel`, `Bytes`, `BigInt`, `StringBuilder`, `Exception`, `Regex`, `Logger`, `NetConn`, `NetListener`, etc.). See §1.5.6 / §2.2.

### 15.1 File I/O and System

| Module | Topic | Key APIs |
|--|--|--|
| `io` | file I/O + filesystem | `readFile` `writeFile` `exists` `mkdir` `remove` `readdir` `stat` `stdin` `stdout` `stderr` |
| `path` | path manipulation | `join` `dirname` `basename` `extname` `normalize` `isAbsolute` `resolve` `relative` `parse` `format` |
| `os` | OS interface | `getenv` `setenv` `environ` `exit` `getpid` `getcwd` `chdir` `hostname` `tmpdir` `homedir` `cpuCount` `sleep` `exec`; constants `platform` `arch` `sep` `eol` |

> Xray has **no** standalone `fs` module; filesystem operations live in `io`. Process arguments / process information are exposed through the global `process` object (`process.args` / `process.file` / `process.dir`, see §16.5), not `os`.
> `os.platform` / `os.arch` / `os.sep` / `os.eol` are **constant strings** (no parentheses); other `os.*` are function calls.

### 15.2 Networking

| Module | Topic | Key APIs |
|--|--|--|
| `net` | TCP / UDP / TLS sockets + DNS | `listen` `dial` `accept` `read` `readInto` `write` `writeBytes` `copy` `copyBidirectional` `setDeadline` `lastError` `lookup` `dialTLS` `NetConn` `NetListener` |
| `http` | HTTP / HTTPS client + server + HTTP/2 | `get` `post` `request` `Server` `urlEncode` `urlDecode` |
| `ws` | WebSocket | client/server connections |
| `url` | URL parsing and construction | `parse` `format` `parseQuery` `buildQuery` `encode` `decode` |

> DNS lookups go through `net.lookup(host)`; there is no standalone `dns` module.

#### 15.2.1 TCP Data Paths

The `net` TCP API intentionally has three data paths:

- `read(conn)` / `write(conn, data)`: message path. Payload is exposed as an Xray `string`, suitable for protocol parsing, text handling, and logic that must inspect bytes.
- `readInto(conn, bytes, maxlen?)` / `writeBytes(conn, bytes)`: reusable `Bytes` buffer path for binary protocol hot loops without per-packet temporary strings.
- `copy(src, dst)` / `copyBidirectional(a, b)`: native stream path. Payload stays in a reusable C buffer, suitable for proxy, relay, `copy(conn, conn)` echo, and other high-throughput workloads that do not need to inspect every byte in Xray code.

Design rule: raw streams should not allocate temporary strings merely to pass through the language layer; use string APIs only when application logic needs the bytes.

TCP waiting operations use the coroutine-friendly netpoll path. `setReadDeadline(conn, deadline)`, `setWriteDeadline(conn, deadline)`, `setDeadline(conn, deadline)`, and `setAcceptDeadline(listener, deadline)` accept `time.monotonic()` millisecond deadlines; after a timeout, operations return their normal `null` or `-1` failure shape, and `lastError(handle)` / `lastErrno(handle)` expose diagnostic causes.

`shutdownRead(conn)`, `shutdownWrite(conn)`, and `shutdown(conn)` expose TCP half-close semantics. Generic proxy and relay code should prefer `copyBidirectional(a, b)`, which half-closes the opposite write side after one-way EOF and returns byte counts for both directions.

The TLS client path is provided by `dialTLS(host, port, timeout?)` and `upgradeTLS(conn, hostname, timeout?)`; TLS read/write/copy share the same deadline, diagnostic error, and typed-handle lifecycle semantics as plain TCP.

### 15.3 Data Formats

| Module | Topic |
|--|--|
| `yaml` | YAML |
| `toml` | TOML |
| `xml` | XML |
| `csv` | CSV |
| `base64` | Base64 encode / decode |
| `encoding` | hex / UTF-8 and other generic encodings (Base64 lives in its own module) |

> JSON encoding/decoding is **not** in a separate `json` module; use the built-in type `Json`'s static methods `Json.parse(s)` / `Json.stringify(v)` (no import required; see §14.10).

### 15.4 Cryptography and Hashing

| Module | Key APIs |
|--|--|
| `crypto` | `md5` `sha1` `sha256` `sha512` `hmac` `aes` `rsa` etc.; full API in stdlib source |

> stdlib has **no** standalone `random` module; for pseudo-random numbers use `crypto`'s random source or `math` utilities.

### 15.5 Compression

| Module | Key APIs |
|--|--|
| `compress` | `gzip` / `gunzip`, `deflate` / `inflate`, etc. |

### 15.6 Time

| Module | Key APIs |
|--|--|
| `time` | `now()` `monotonic()` `sleep(ms)` `Duration` |
| `datetime` | `DateTime` / `Date` / `Time` parsing and formatting (see §14.12) |

### 15.7 Math

| Module | Key APIs |
|--|--|
| `math` | `sin` `cos` `tan` `log` `pow` `sqrt` `floor` `ceil` `round` `abs` `min` `max` etc.; constants `PI` / `E` / `MAX_INT` / `MIN_INT` |

### 15.8 Text

| Module | Key APIs |
|--|--|
| `regex` | `compile(pattern)` returns `Regex`; see §14.13. The `/pattern/flags` literal form is also supported |

> stdlib has **no** `strconv` module; for string ↔ numeric conversions use the built-ins `int(s)` / `float(s)` / `string(n)` (see §13.2).

### 15.9 Logging and Diagnostics

| Module | Key APIs |
|--|--|
| `log` | `debug` / `info` / `warn` / `error` / `fatal` / `child()`, source-position toggles, async write mode |
| `gc` | `collect()` `isrunning()` `count()` `state()` `stats()` |

### 15.10 Distributed

| Module | Topic |
|--|--|
| `cluster` | node discovery, health checks, topic-based message bus (see `stdlib/cluster/`) |

### 15.11 Testing

The `@test` attribute together with the global `assert*` family is enough; **no** separate `test` module is needed (see §12).

### 15.12 Modules That **Do Not Exist**

Modules that may have been referenced historically but are **not** part of the current stdlib (to avoid confusion):

`fs` · `process` · `dns` · `random` · `strconv` · `sync` · `runtime` · `json`

Their functionality has either moved into other modules (see the per-section notes above) or has not yet been implemented.

> **Full index**: see [Appendix D](#d-stdlib-module-index).
<!-- /xr-spec:en -->

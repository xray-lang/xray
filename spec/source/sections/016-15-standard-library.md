---
id: spec.15_standard_library
order: 016
---

<!-- xr-spec:cn -->
---

## 15. 标准库概览 (Standard Library)

> 真值源：`stdlib/defs/*.def`、纯 Xray `stdlib/<module>/<module>.xr` export、`stdlib/types/*.xr` native type 声明，以及合并这些来源的 `scripts/gen_api_inventory.py`。
> MCP knowledge 和 API inventory 使用 source-derived inventory；`xray builtin-dump` 只作为运行时 builtin 视图输入之一。
> 详见 [附录 D stdlib 模块索引](#d-stdlib-模块索引)。

> **真实 stdlib 模块清单**（27 个，源码：`stdlib/<module>/*.c` / `stdlib/<module>/*.xr`）：
>
> `base64`、`cluster`、`compress`、`crypto`、`csv`、`datetime`、`encoding`、`http`、`io`、`log`、`math`、`mem`、`net`、`os`、`parallel`、`path`、`regex`、`runtime`、`sync`、`sys`、`text`、`time`、`toml`、`url`、`ws`、`xml`、`yaml`。
>
> 不需要 import 的 prelude 类型/命名空间为：`Array`、`Atomic`、`OsBarrier`、`BigInt`、`Channel`、`OsCondvar`、`PanicInfo`、`JSON`（含 `JSON.Value` / `JSON.Object`）、`Map`、`OsMutex`、`OsOnce`、`Path`、`Range`、`Regex`、`OsRwLock`、`Set`、`StringBuilder`、`Thread`。`Array<u8>` 是 `Array` 的具体化；`DateTime`、`Logger`、`NetConn`、`NetListener` 等模块类型需要从对应模块导入。详见 §1.5.6 / §2.2。

### 15.1 文件 IO 与系统

| 模块 | 主题 | 关键 API |
|--|--|--|
| `io` | 文件 IO + 文件系统 | `readFile` `writeFile` `readFileBytes` `writeFileBytes` `exists` `mkdir` `mkdirp` `remove` `readDir` `stat` `readStdin` |
| `path` | 路径操作 | `join` `dirname` `basename` `extname` `normalize` `isAbsolute` `resolve` `relative` `parse` `format` |
| `os` | 操作系统接口 | `platform()` `arch()` `sep()` `eol()` `getenv` `setenv` `environ` `exit` `getpid` `getcwd` `chdir` `hostname` `tmpdir` `homedir` `cpuCount` `sleep` `exec` |

> xray **没有**独立的 `fs` 模块，文件系统操作在 `io` 中；进程参数 / 进程信息走全局 `process` 对象（`process.args` / `process.file` / `process.dir`，见 §16.5），不在 `os` 中。
> `os.platform()` / `os.arch()` 查询主机事实；`os.sep()` / `os.eol()` 在 Xray 模块中从平台名派生。

### 15.2 网络

| 模块 | 主题 | 关键 API |
|--|--|--|
| `net` | TCP / UDP / TLS socket + DNS | `listen` `dial` `accept` `read` `readInto` `write` `writeBytes` `copy` `copyBidirectional` `setDeadline` `lastError` `lookup` `dialTLS` `NetConn` `NetListener` |
| `http` | HTTP / HTTPS 客户端 + 服务端 + HTTP/2 | `request` `h2Request` `listen` `router` `routeHandler` `requestText` `responseText` `parseResponseText` |
| `ws` | WebSocket | `connect` `serve` `send` `recv` `close` `parseFrame` `parseUrl` `parseUpgradeRequest` `clientHandshakeRequest` |
| `url` | URL 解析与构造 | `URL` `QueryParams` `parse` `format` `parseQuery` `encode` `decode` |

> DNS 查询通过 `net.lookup(host)` 完成；没有独立的 `dns` 模块。

#### 15.2.1 TCP 数据路径

`net` 的 TCP API 明确区分三类数据路径：

- `read(conn)` / `write(conn, data)`：消息型路径，把 payload 暴露为 Xray `string`，适合协议解析、文本处理和需要检查内容的逻辑。
- `readInto(conn, bytes, maxlen?)` / `writeBytes(conn, bytes)`：可复用 `Array<u8>` buffer 路径，适合二进制协议热路径，避免为每个包创建临时字符串。
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

> JSON 编解码**不在**单独的 `json` 模块；通过 prelude `JSON` 命名空间的 `JSON.parse<T>(s)` / `JSON.parseObject(s)` / `JSON.value(v)` / `JSON.stringify(v)` 使用（无需 import；见 §14.11）。

### 15.4 加密与哈希

| 模块 | 关键 API |
|--|--|
| `crypto` | `md5` `sha1` `sha256` `sha512` `hmac` `encrypt` `decrypt` `randomBytes` `timingSafeEqual` `uuid` |

> stdlib **没有**独立的 `random` 模块；如需伪随机数请使用 `crypto` 模块的随机源或 `math` 模块的工具函数。

### 15.5 压缩

| 模块 | 关键 API |
|--|--|
| `compress` | `gzip` / `gunzip`、`deflate` / `inflate` 等 |

### 15.6 时间

| 模块 | 关键 API |
|--|--|
| `time` | `now()` `monotonic()` `clock()` `micros()` `nanos()` `sleep(ms)` `localOffset()` `localOffsetAt()` |
| `datetime` | `DateTime` 及 `now()` `utc()` `create()` `createUTC()` `fromTimestamp()` `parse()` 等工厂（详见 §14.13） |

### 15.7 数学

| 模块 | 关键 API |
|--|--|
| `math` | `sin` `cos` `tan` `log` `pow` `sqrt` `floor` `ceil` `round` `abs` `min` `max` 等；常量 `PI` / `E` / `MAX_I64` / `MIN_I64` |

### 15.8 文本

| 模块 | 关键 API |
|--|--|
| `regex` | `compile(pattern)` 返回 `Regex`；详见 §14.14。也支持 `/pattern/flags` 字面量 |
| `text` | `lower` `upper` `trim` `trimStart` `trimEnd` `padStart` `padEnd` `reverseRunes` `translate` |
十进制文本解析由 exact scalar 静态命名空间提供：`i64.parse(s)` / `i64.tryParse(s)` 与 `f64.parse(s)` / `f64.tryParse(s)`。数值间转换使用显式 `as`。

### 15.9 日志与诊断

| 模块 | 关键 API |
|--|--|
| `log` | `debug` / `info` / `warn` / `error` / `fatal` / `child()`、source 位置开关、异步写入模式 |
| `runtime` | `liveBytes()` `liveObjects()` `info()` |
| `mem` | `alloc()` / `allocZeroed()` / `allocAligned()` 返回受管 `Buffer`；`pageAlloc()` / `pageFree()`；`copy()` / `move()` / `set()` / `compare()`；`volatileLoad()` / `volatileStore()`；`fence()` |
| `sync` | 协程域同步：`Mutex` `RwLock` `Once` `Barrier` `Condvar` `CachePadded` `fence()` 等，需显式 `import sync` |
| `sys` | OS / 线程底层接口：编译器定义的 `sys.Thread.spawn(...)` 与 `ThreadOptions`，以及 `ThreadLocal`、`OsMutex` `OsRwLock` `OsCondvar` `OsBarrier` `OsOnce`、process/dylib/pipe handle、`threadYield()`、`pinToCpu()`、`onSignal()`；主机 CPU 拓扑统一使用 `os.cpuCount()`，协程友好延时统一使用 `time.sleep(ms)` |

### 15.10 并行

`parallel` 导出 `forEach` 以及 `Plan` 抽象，用于结构化 CPU 并行工作；语言关键字表中没有 `parallel` 关键字，需显式导入模块。

### 15.11 分布式

| 模块 | 主题 |
|--|--|
| `cluster` | 节点发现、健康检查、Topic 消息总线（见 stdlib/cluster/）|

### 15.12 测试

`@test` 注解 + 全局 `assert*` 函数即可，**不需要**额外的 `test` 模块（见 §12）。

### 15.13 已**不存在**的模块

文档中可能引用过、但当前 stdlib 中**确实没有**的模块（避免误导）：

`fs` · `process` · `dns` · `random` · `json`

这些功能或者归入其他模块（见上面各小节注），或者尚未实现。

> **完整索引**：见[附录 D](#d-stdlib-模块索引)。
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 15. Standard Library Overview

> Source of truth: `stdlib/defs/*.def`, pure-Xray `stdlib/<module>/<module>.xr` exports, `stdlib/types/*.xr` native type declarations, and `scripts/gen_api_inventory.py`, which merges those sources.
> MCP knowledge and the API inventory use the source-derived inventory; `xray builtin-dump` is only one runtime builtin-view input.
> See [Appendix D — stdlib module index](#d-stdlib-module-index).

> **Authoritative stdlib module list** (27 modules; source: `stdlib/<module>/*.c` / `stdlib/<module>/*.xr`):
>
> `base64`, `cluster`, `compress`, `crypto`, `csv`, `datetime`, `encoding`, `http`, `io`, `log`, `math`, `mem`, `net`, `os`, `parallel`, `path`, `regex`, `runtime`, `sync`, `sys`, `text`, `time`, `toml`, `url`, `ws`, `xml`, `yaml`.
>
> The exact prelude type/namespace set is: `Array`, `Atomic`, `OsBarrier`, `BigInt`, `Channel`, `OsCondvar`, `PanicInfo`, `JSON` (including `JSON.Value` / `JSON.Object`), `Map`, `OsMutex`, `OsOnce`, `Path`, `Range`, `Regex`, `OsRwLock`, `Set`, `StringBuilder`, and `Thread`. `Array<u8>` is an `Array` specialization; module types such as `DateTime`, `Logger`, `NetConn`, and `NetListener` must be imported. See §1.5.6 / §2.2.

### 15.1 File I/O and System

| Module | Topic | Key APIs |
|--|--|--|
| `io` | file I/O + filesystem | `readFile` `writeFile` `readFileBytes` `writeFileBytes` `exists` `mkdir` `mkdirp` `remove` `readDir` `stat` `readStdin` |
| `path` | path manipulation | `join` `dirname` `basename` `extname` `normalize` `isAbsolute` `resolve` `relative` `parse` `format` |
| `os` | OS interface | `platform()` `arch()` `sep()` `eol()` `getenv` `setenv` `environ` `exit` `getpid` `getcwd` `chdir` `hostname` `tmpdir` `homedir` `cpuCount` `sleep` `exec` |

> Xray has **no** standalone `fs` module; filesystem operations live in `io`. Process arguments / process information are exposed through the global `process` object (`process.args` / `process.file` / `process.dir`, see §16.5), not `os`.
> `os.platform()` / `os.arch()` query host facts; `os.sep()` / `os.eol()` are derived from the platform name in the Xray module.

### 15.2 Networking

| Module | Topic | Key APIs |
|--|--|--|
| `net` | TCP / UDP / TLS sockets + DNS | `listen` `dial` `accept` `read` `readInto` `write` `writeBytes` `copy` `copyBidirectional` `setDeadline` `lastError` `lookup` `dialTLS` `NetConn` `NetListener` |
| `http` | HTTP / HTTPS client + server + HTTP/2 | `request` `h2Request` `listen` `router` `routeHandler` `requestText` `responseText` `parseResponseText` |
| `ws` | WebSocket | `connect` `serve` `send` `recv` `close` `parseFrame` `parseUrl` `parseUpgradeRequest` `clientHandshakeRequest` |
| `url` | URL parsing and construction | `URL` `QueryParams` `parse` `format` `parseQuery` `encode` `decode` |

> DNS lookups go through `net.lookup(host)`; there is no standalone `dns` module.

#### 15.2.1 TCP Data Paths

The `net` TCP API intentionally has three data paths:

- `read(conn)` / `write(conn, data)`: message path. Payload is exposed as an Xray `string`, suitable for protocol parsing, text handling, and logic that must inspect bytes.
- `readInto(conn, bytes, maxlen?)` / `writeBytes(conn, bytes)`: reusable `Array<u8>` buffer path for binary protocol hot loops without per-packet temporary strings.
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

> JSON encoding/decoding is **not** in a separate `json` module; use the prelude `JSON` namespace through `JSON.parse<T>(s)` / `JSON.parseObject(s)` / `JSON.value(v)` / `JSON.stringify(v)` (no import required; see §14.11).

### 15.4 Cryptography and Hashing

| Module | Key APIs |
|--|--|
| `crypto` | `md5` `sha1` `sha256` `sha512` `hmac` `encrypt` `decrypt` `randomBytes` `timingSafeEqual` `uuid` |

> stdlib has **no** standalone `random` module; for pseudo-random numbers use `crypto`'s random source or `math` utilities.

### 15.5 Compression

| Module | Key APIs |
|--|--|
| `compress` | `gzip` / `gunzip`, `deflate` / `inflate`, etc. |

### 15.6 Time

| Module | Key APIs |
|--|--|
| `time` | `now()` `monotonic()` `clock()` `micros()` `nanos()` `sleep(ms)` `localOffset()` `localOffsetAt()` |
| `datetime` | `DateTime` plus factories such as `now()` `utc()` `create()` `createUTC()` `fromTimestamp()` and `parse()` (see §14.13) |

### 15.7 Math

| Module | Key APIs |
|--|--|
| `math` | `sin` `cos` `tan` `log` `pow` `sqrt` `floor` `ceil` `round` `abs` `min` `max` etc.; constants `PI` / `E` / `MAX_I64` / `MIN_I64` |

### 15.8 Text

| Module | Key APIs |
|--|--|
| `regex` | `compile(pattern)` returns `Regex`; see §14.14. The `/pattern/flags` literal form is also supported |
| `text` | `lower` `upper` `trim` `trimStart` `trimEnd` `padStart` `padEnd` `reverseRunes` `translate` |
Decimal text parsing is owned by exact scalar static namespaces: `i64.parse(s)` / `i64.tryParse(s)` and `f64.parse(s)` / `f64.tryParse(s)`. Numeric conversions use explicit `as`.

### 15.9 Logging and Diagnostics

| Module | Key APIs |
|--|--|
| `log` | `debug` / `info` / `warn` / `error` / `fatal` / `child()`, source-position toggles, async write mode |
| `runtime` | `liveBytes()` `liveObjects()` `info()` |
| `mem` | `alloc()` / `allocZeroed()` / `allocAligned()` return managed `Buffer`; `pageAlloc()` / `pageFree()`; `copy()` / `move()` / `set()` / `compare()`; `volatileLoad()` / `volatileStore()`; `fence()` |
| `sync` | coroutine-domain synchronization: `Mutex` `RwLock` `Once` `Barrier` `Condvar` `CachePadded` `fence()`, with explicit `import sync` |
| `sys` | low-level OS/thread surface: compiler-defined `sys.Thread.spawn(...)` with `ThreadOptions`, plus `ThreadLocal`, `OsMutex`, `OsRwLock`, `OsCondvar`, `OsBarrier`, `OsOnce`, process/dylib/pipe handles, `threadYield()`, `pinToCpu()`, and `onSignal()`; use `os.cpuCount()` for host CPU topology and `time.sleep(ms)` for coroutine-friendly delays |

### 15.10 Parallelism

`parallel` exports `forEach` and the `Plan` abstraction for structured CPU-parallel work. `parallel` is not a language keyword; import the module explicitly.

### 15.11 Distributed

| Module | Topic |
|--|--|
| `cluster` | node discovery, health checks, topic-based message bus (see `stdlib/cluster/`) |

### 15.12 Testing

The `@test` attribute together with the global `assert*` family is enough; **no** separate `test` module is needed (see §12).

### 15.13 Modules That **Do Not Exist**

Modules that may have been referenced historically but are **not** part of the current stdlib (to avoid confusion):

`fs` · `process` · `dns` · `random` · `json`

Their functionality has either moved into other modules (see the per-section notes above) or has not yet been implemented.

> **Full index**: see [Appendix D](#d-stdlib-module-index).
<!-- /xr-spec:en -->

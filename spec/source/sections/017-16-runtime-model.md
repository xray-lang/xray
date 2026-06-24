---
id: spec.16_runtime_model
order: 017
---

<!-- xr-spec:cn -->
---

## 16. 运行时模型 (Runtime Model)

> 真值源：`src/runtime/`、`src/vm/`、`src/runtime/mem/`、`docs/rules/architecture.md`。

### 16.1 值表示

Xray 值统一用 `XrValue` 表示。当前实现要求 64 位平台，并采用 **16 字节 tagged struct-of-union**：

- **Descriptor（8 字节）**：`tag: uint8`、`flags: uint8`、`heap_type: uint16`、`ext: uint32`。`tag` 是类型判定的唯一入口；`heap_type` 只在 `tag == PTR` 时表示堆对象类型。
- **Payload（8 字节）**：`int64` / `double` / 指针三选一，按 `tag` 解释。
- **无 NaN-boxing / 无指针低位标记**：整数保留完整 64 位；对象引用是普通堆指针，类型信息在 descriptor 中。
- **字符串不是值级 SSO**：`string` 始终是 `XrString` 堆对象，字符数据存放在对象内的 `data[]` flexible array 中。运行期短串默认协程本地无锁分配，仅字面量/符号、显式 `intern()` 与 map/set 键驻留全局池；跨协程边界（channel send、`go` 实参、task/scope 结果）按需提升为共享原子 RC。这些都是对象层存储策略，不改变 `XrValue` 表示。

| 值类型 | 内部表示 |
|--|--|
| `int` | `XR_TAG_I64` + 64-bit signed payload |
| `float` | `XR_TAG_F64` + IEEE-754 double payload |
| `bool` | `XR_TAG_BOOL` + `0/1` payload |
| `char` | `XR_TAG_CHAR` + Unicode scalar payload |
| `null` | `XR_TAG_NULL` + zero payload |
| `string` | `XR_TAG_PTR` + `XR_TSTRING` + `XrString*` |
| `Bytes` | `XR_TAG_PTR` + bytes heap object |
| 其他对象 | `XR_TAG_PTR` + heap type + heap pointer |

Typed array 元素布局是容器元数据的一部分。`Array<char>` 使用 `XR_ELEM_CHAR`，数据区是连续 `uint32_t[]` Unicode scalar；load 时重新装箱为 `XR_TAG_CHAR`，store 时拒绝非 `char` 值，因此不会与 `Array<uint32>` 混淆。

### 16.2 内存分配

| 区域 | 用途 |
|--|--|
| **系统堆** | C `malloc/free`，用于 native 数据结构 |
| **全局堆** | `shared const` / `shared let`，引用计数 |
| **协程堆** | 每协程独立的 RC 对象堆，强引用环由 cycle collector 回收 |
| **栈** | `struct` 值、局部 immediate、函数帧 |
| **Arena** | parser 临时分配、frame allocation |

### 16.3 内存模型

- 默认 **per-coroutine reference counting**。最后一个强引用释放时，对象立即进入释放路径。
- **循环引用回收**：强引用环由 cycle collector 处理；显式入口是 `mem.collectCycles()`。
- **内存安全点**：函数调用、后向跳转、显式 `mem.collectCycles()`。
- **用户可见 introspection**：`mem.liveBytes()` / `mem.liveObjects()` / `mem.info()` 只报告当前协程堆的 live memory 视图。

详见 `src/runtime/mem/`。

### 16.4 协程调度

- M:N 调度（M OS 线程 × N 协程）。
- **work-stealing**：空闲 worker 从其他 worker 队列偷任务。
- **协作式抢占**：协程在 safepoint 让出（非强制抢占）。
- **公平性**：单一 runnable 队列配合本地 run-next、全局注入队列和 work-stealing；调度顺序不暴露用户级优先级。
- **栈管理**：segmented stack 按需扩展。

详见 `src/runtime/coro/`。

### 16.5 进程级全局访问

- `process`（全局内置，无需 import）：进程自身信息。
- `os`（需 `import os`）：操作系统、环境、进程控制。

```xray
// 进程自身信息 — 全局对象
process.file              // 当前脚本路径（与 __file__ 等价）
process.args              // Array<string>，进程命令行参数
process.dir               // 脚本所在目录（与 __dir__ 等价）

// OS / 环境 — 需 import
import os
os.getenv("PATH")         // 读取环境变量 -> string?
os.environ()              // 获取全部环境变量 -> Map<string, string>
os.exit(0)                // 退出进程
os.getpid()               // 进程 ID
os.getcwd()               // 当前工作目录
os.hostname()             // 主机名
os.tmpdir()               // 临时目录
os.platform               // 常量："darwin" / "linux" / "windows"
os.arch                   // 常量："arm64" / "x64" / "x86"
os.sep                    // 常量：路径分隔符
os.eol                    // 常量：行尾
os.sleep(100)             // 休眠毫秒数（与 `time.sleep` 等价）
```

> **命名约定**：`os.*` 以 POSIX 函数名为主（`getenv` / `getcwd` / `getpid`）；不随 Node.js。Node 风格的 `process.env` 映射不提供，请用 `os.getenv(name)` / `os.environ()`。

详见 `stdlib/os/`。

### 16.6 Panic 运行时

内置 `Exception` 类是 prelude 类型（声明：`stdlib/types/exception.xr`），仅属于 **panic 通道**。运行时故障（越界、除零、不完整 `match`、运行时不变量违背等）由 VM/AOT runtime 构造 `Exception` 对象：

```xray
@native
class Exception {
    message: string             // 人类可读消息
    stack: Array<string>        // 自动 capture 的调用栈，每帧一行格式化字符串
    cause: Exception?           // 链式 cause
    code: int                   // 错误码（从 "E0xxx: ..." 前缀自动解析，默认 0）
    data: Json?                 // 运行时故障的可选结构化附加数据

    constructor(message: string = "", cause: Exception? = null)
    fn toString() -> string
}
```

用户级可恢复错误不使用 `Exception`：`throw` 表达式只接受 enum 变体值（见 §8.1.1）；非 enum 错误值在编译期被拒绝（错误码 `E0370`）。

栈展开（仅 panic 通道）：VM `xvm_unwind_stack()` 按 try-table 查找 `catch panic` handler，逐帧释放局部、执行 defer，到达 handler 后跳转。可恢复错误走值返回通道，不展开栈。详见 §8。

### 16.7 值返回错误通道运行时

`throw <enum>` 将枚举值写入帧的 `pending_error` 槽位，设置错误标志位并返回。调用方通过 `OP_ERR_CHECK` 检测标志位，决定进入 `catch` handler 或继续向上返回。该通道不展开栈、不分配 `Exception`；在需要传播或捕获错误的调用边界，正常路径只经过可预测的错误标志分支。

### 16.8 对象回收与析构时机契约

xray **当前不提供用户可见的确定性析构（destructor / finalizer / `Drop`）**。对象的**回收时机**不属于可观察语义契约，由实现定义，且不同执行后端不同：

- 回收可能发生在"最后一个引用消失时"、"某次 GC 时"、或"进程退出时"中的任意一种，VM 与 AOT 的回收时机**不保证一致**。
- 程序**不得依赖**：(a) 对象在某个确定时刻被回收；(b) 任何析构 / finalizer 是否运行、运行顺序或运行所在线程。

唯一保证确定性、且跨后端（VM / AOT）一致的资源清理机制是 **`defer`**：它在所属作用域退出时按 LIFO 顺序执行（含 panic 展开路径），与对象回收时机无关。需要确定性释放外部资源（文件 / 句柄 / 锁）的代码必须使用 `defer`，而非依赖对象析构。

> 演进说明：当确定性析构（RAII / `Drop`）被正式纳入语言时，本节将升级为**确定性回收契约**（明确析构点与顺序），并由跨后端差分测试逐字节守门。在此之前，"回收时机 / finalizer 行为"被显式声明为实现定义的非确定项。
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 16. Runtime Model

> Source of truth: `src/runtime/`, `src/vm/`, `src/runtime/mem/`, `docs/rules/architecture.md`.

### 16.1 Value Representation

Xray values are uniformly represented as `XrValue`. The current implementation requires a 64-bit platform and uses a **16-byte tagged struct-of-union**:

- **Descriptor (8 bytes)**: `tag: uint8`, `flags: uint8`, `heap_type: uint16`, and `ext: uint32`. The `tag` is the single entry point for type dispatch; `heap_type` is meaningful only when `tag == PTR`.
- **Payload (8 bytes)**: one of `int64`, `double`, or pointer, interpreted by the tag.
- **No NaN-boxing / no low-bit pointer tagging**: integers keep the full 64-bit payload; object references are ordinary heap pointers, with type metadata in the descriptor.
- **Strings are not value-level SSO**: `string` is always an `XrString` heap object, with bytes stored inside the object's `data[]` flexible array. Runtime short strings are coroutine-local with lock-free allocation by default; only literals/symbols, explicit `intern()`, and map/set keys are interned in the global pool, and strings are promoted to shared (atomic RC) on demand when crossing coroutine boundaries (channel send, `go` arguments, task/scope results). These are object-storage policies and do not change the `XrValue` representation.

| Value type | Internal representation |
|--|--|
| `int` | `XR_TAG_I64` + 64-bit signed payload |
| `float` | `XR_TAG_F64` + IEEE-754 double payload |
| `bool` | `XR_TAG_BOOL` + `0/1` payload |
| `char` | `XR_TAG_CHAR` + Unicode scalar payload |
| `null` | `XR_TAG_NULL` + zero payload |
| `string` | `XR_TAG_PTR` + `XR_TSTRING` + `XrString*` |
| `Bytes` | `XR_TAG_PTR` + bytes heap object |
| Other objects | `XR_TAG_PTR` + heap type + heap pointer |

Typed-array element layout is part of the container metadata. `Array<char>` uses `XR_ELEM_CHAR`; its data area is a contiguous `uint32_t[]` of Unicode scalars. Loads re-box values as `XR_TAG_CHAR`, and stores reject non-`char` values, so it cannot be confused with `Array<uint32>`.

### 16.2 Memory Allocation

| Region | Use |
|--|--|
| **System heap** | C `malloc/free`, used for native data structures |
| **Global heap** | `shared const` / `shared let`, reference counting |
| **Coroutine heap** | per-coroutine RC object heap; strong cycles are reclaimed by the cycle collector |
| **Stack** | `struct` values, local immediates, function frames |
| **Arena** | parser temporary allocation, frame allocation |

### 16.3 Memory Model

- Default reclamation is **per-coroutine reference counting**. When the last strong reference is released, the object enters its release path immediately.
- **Cycle collection** handles strong reference cycles; the explicit user entrypoint is `mem.collectCycles()`.
- **Memory safepoints**: function calls, backward branches, explicit `mem.collectCycles()`.
- **User-visible introspection**: `mem.liveBytes()` / `mem.liveObjects()` / `mem.info()` report the current coroutine heap's live-memory view.

See `src/runtime/mem/` for details.

### 16.4 Coroutine Scheduling

- M:N scheduling (M OS threads × N coroutines).
- **work-stealing**: idle workers steal tasks from other workers' queues.
- **Cooperative preemption**: coroutines yield at safepoints (no forced preemption).
- **Fairness**: a single runnable queue works with local run-next, global injection, and work-stealing; scheduling order does not expose user-level priorities.
- **Stack management**: segmented stacks grow on demand.

See `src/runtime/coro/` for details.

### 16.5 Process-Level Global Access

- `process` (global builtin, no import required): self-process information.
- `os` (requires `import os`): operating system, environment, process control.

```xray
// Self-process information — global object
process.file              // current script path (equivalent to __file__)
process.args              // Array<string>, process command-line arguments
process.dir               // script directory (equivalent to __dir__)

// OS / environment — requires import
import os
os.getenv("PATH")         // read environment variable -> string?
os.environ()              // get all environment variables -> Map<string, string>
os.exit(0)                // exit the process
os.getpid()               // process ID
os.getcwd()               // current working directory
os.hostname()             // host name
os.tmpdir()               // temporary directory
os.platform               // constant: "darwin" / "linux" / "windows"
os.arch                   // constant: "arm64" / "x64" / "x86"
os.sep                    // constant: path separator
os.eol                    // constant: end-of-line
os.sleep(100)             // sleep in milliseconds (equivalent to `time.sleep`)
```

> **Naming convention**: `os.*` follows POSIX function names (`getenv` / `getcwd` / `getpid`); it does not track Node.js. Node-style `process.env` mapping is not provided — use `os.getenv(name)` / `os.environ()`.

See `stdlib/os/` for details.

### 16.6 Panic Runtime

The built-in `Exception` class is a prelude type (declared in `stdlib/types/exception.xr`) and belongs only to the **panic channel**. Runtime faults (out-of-bounds, division by zero, non-exhaustive `match`, runtime invariant violations, and similar faults) are represented by `Exception` objects constructed by the VM/AOT runtime:

```xray
@native
class Exception {
    message: string             // human-readable message
    stack: Array<string>        // automatically captured call stack, one formatted line per frame
    cause: Exception?           // chained cause
    code: int                   // error code (auto-parsed from "E0xxx: ..." prefix; default 0)
    data: Json?                 // optional structured data for a runtime fault

    constructor(message: string = "", cause: Exception? = null)
    fn toString() -> string
}
```

Recoverable user-level errors do not use `Exception`: a `throw` expression accepts enum variant values only (see §8.1.1); non-enum error values are rejected at compile time (error code `E0370`).

Stack unwinding (panic channel only): the VM's `xvm_unwind_stack()` walks the try-table to find `catch panic` handlers, releasing locals frame by frame and running `defer` along the way before jumping to the handler. Recoverable errors use the value-return channel and never unwind the stack. See §8 for details.

### 16.7 Value-return Error Channel Runtime

`throw <enum>` writes the enum value into the frame's `pending_error` slot, sets the error flag, and returns. The caller detects the flag via `OP_ERR_CHECK` and either enters a `catch` handler or continues returning upward. This channel performs no stack unwinding and allocates no `Exception`; at call boundaries that may propagate or catch errors, the happy path goes through only a predictable error-flag branch.

### 16.8 Object Reclamation & Finalizer Timing Contract

xray **currently exposes no user-visible deterministic destructor (destructor / finalizer / `Drop`)**. The **timing** of object reclamation is NOT part of the observable semantic contract; it is implementation-defined and differs across execution backends:

- Reclamation may occur at "the moment the last reference disappears", "some GC point", or "process exit"; VM and AOT reclamation timing is **not guaranteed to agree**.
- Programs **must not depend on**: (a) an object being reclaimed at any particular moment; (b) whether any destructor / finalizer runs, in what order, or on which thread.

The only deterministic, cross-backend (VM / AOT) consistent cleanup mechanism is **`defer`**, which runs at owning-scope exit in LIFO order (including the panic-unwind path), independent of object reclamation timing. Code that must deterministically release external resources (files / handles / locks) must use `defer` rather than relying on object finalization.

> Evolution note: once deterministic destruction (RAII / `Drop`) is formally added to the language, this section will be upgraded to a **deterministic reclamation contract** (specifying destruction points and order), gated byte-for-byte by cross-backend differential tests. Until then, "reclamation timing / finalizer behavior" is explicitly declared an implementation-defined, non-deterministic aspect.
<!-- /xr-spec:en -->

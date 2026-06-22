---
id: spec.16_runtime_model
order: 017
---

<!-- xr-spec:cn -->
---

## 16. 运行时模型 (Runtime Model)

> 真值源：`src/runtime/`、`src/vm/`、`src/runtime/mem/`、`docs/rules/architecture.md`。

### 16.1 值表示

xray 值统一用 `xray_value_t` 表示。布局策略：

- **NaN-boxing**（在 64 位平台）：double 编码用未使用的 NaN 表示空间存放小整数、bool、指针标记。
- **指针标记**：低位 tag 区分对象类型。
- **对象引用**：堆对象通过 tagged pointer 引用；当前内存模型不移动对象。

| 值类型 | 内部表示 |
|--|--|
| `int` | 53-bit immediate（NaN-box） |
| `float` | 双精度直接存放 |
| `bool` | tag |
| `null` | 单一全局值 |
| `string` | 堆对象 + 短字符串内联（≤ 7 字节） |
| `Bytes` | 堆对象 + capacity/length |
| 其他对象 | 堆指针 |

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

### 16.6 异常运行时

内置 `Exception` 类是 prelude 类型（声明：`stdlib/types/exception.xr`），用户可直接 `new` 或继承：

```xray
@native
class Exception {
    message: string             // 人类可读消息
    stack: Array<string>        // 自动 capture 的调用栈，每帧一行格式化字符串
    cause: Exception?           // 链式 cause
    code: int                   // 错误码（从 "E0xxx: ..." 前缀自动解析，默认 0）
    data: Json?                 // throw 非异常值时原始值被包装在此

    constructor(message: string = "", cause: Exception? = null)
    fn toString() -> string
}
```

`throw` 表达式的操作数静态类型必须是 `Exception` 或其派生类（见 §8.1.1）；其它类型在编译期被拒绝（错误码 `E0370`）。VM 抛出的运行时错误也使用此 `Exception` 类型。

栈展开（仅 panic 通道）：VM `xvm_unwind_stack()` 按 try-table 查找 `catch panic` handler，逐帧释放局部、执行 defer，到达 handler 后跳转。可恢复错误走值返回通道，不展开栈。详见 §8。

### 16.7 值返回错误通道运行时

`throw <enum>` 将枚举值写入帧的 `pending_error` 槽位，设置错误标志位并返回。调用方通过 `OP_ERR_CHECK` 检测标志位，决定进入 `catch` handler 或继续向上返回。正常路径零开销（仅一次条件跳转），不展开栈、不分配对象。

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

Xray values are uniformly represented as `xray_value_t`. Layout strategy:

- **NaN-boxing** (on 64-bit platforms): unused IEEE-754 NaN bit space encodes small integers, booleans, and pointer tags.
- **Pointer tagging**: low-bit tags distinguish object kinds.
- **Object references**: heap objects are referenced via tagged pointers; the current memory model does not move objects.

| Value type | Internal representation |
|--|--|
| `int` | 53-bit immediate (NaN-box) |
| `float` | double precision stored directly |
| `bool` | tag |
| `null` | single global value |
| `string` | heap object + short-string inline (≤ 7 bytes) |
| `Bytes` | heap object + capacity/length |
| Other objects | heap pointer |

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

### 16.6 Exception Runtime

The built-in `Exception` class is a prelude type (declared in `stdlib/types/exception.xr`); users may directly `new` it or inherit from it:

```xray
@native
class Exception {
    message: string             // human-readable message
    stack: Array<string>        // automatically captured call stack, one formatted line per frame
    cause: Exception?           // chained cause
    code: int                   // error code (auto-parsed from "E0xxx: ..." prefix; default 0)
    data: Json?                 // when a non-exception value is thrown, the original value is wrapped here

    constructor(message: string = "", cause: Exception? = null)
    fn toString() -> string
}
```

The operand of a `throw` expression must have a static type that is `Exception` or one of its subclasses (see §8.1.1); other types are rejected at compile time (error code `E0370`). Runtime errors thrown by the VM also use this `Exception` type.

Stack unwinding (panic channel only): the VM's `xvm_unwind_stack()` walks the try-table to find `catch panic` handlers, releasing locals frame by frame and running `defer` along the way before jumping to the handler. Recoverable errors use the value-return channel and never unwind the stack. See §8 for details.

### 16.7 Value-return Error Channel Runtime

`throw <enum>` writes the enum value into the frame's `pending_error` slot, sets the error flag, and returns. The caller detects the flag via `OP_ERR_CHECK` and either enters a `catch` handler or continues returning upward. The happy path has zero overhead (only a single conditional branch); no stack unwinding or object allocation occurs.

### 16.8 Object Reclamation & Finalizer Timing Contract

xray **currently exposes no user-visible deterministic destructor (destructor / finalizer / `Drop`)**. The **timing** of object reclamation is NOT part of the observable semantic contract; it is implementation-defined and differs across execution backends:

- Reclamation may occur at "the moment the last reference disappears", "some GC point", or "process exit"; VM and AOT reclamation timing is **not guaranteed to agree**.
- Programs **must not depend on**: (a) an object being reclaimed at any particular moment; (b) whether any destructor / finalizer runs, in what order, or on which thread.

The only deterministic, cross-backend (VM / AOT) consistent cleanup mechanism is **`defer`**, which runs at owning-scope exit in LIFO order (including the panic-unwind path), independent of object reclamation timing. Code that must deterministically release external resources (files / handles / locks) must use `defer` rather than relying on object finalization.

> Evolution note: once deterministic destruction (RAII / `Drop`) is formally added to the language, this section will be upgraded to a **deterministic reclamation contract** (specifying destruction points and order), gated byte-for-byte by cross-backend differential tests. Until then, "reclamation timing / finalizer behavior" is explicitly declared an implementation-defined, non-deterministic aspect.
<!-- /xr-spec:en -->

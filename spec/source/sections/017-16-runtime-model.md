---
id: spec.16_runtime_model
order: 017
---

<!-- xr-spec:cn -->
---

## 16. 运行时模型 (Runtime Model)

本节定义运行时语言合同。各后端及具体类型族的当前实现资格见 §17。

### 16.1 值表示

值的机器表示属于实现。标量、struct、tuple、enum、可空值和定长数组可以使用寄存器、展平槽或聚合存储，语言不规定统一的值头、标签宽度或对象大小。静态类型、整数宽度、有符号性、值语义与身份语义在各后端保持一致。

`string`、`Array`、`Map` 和 `Set` 的 backing 可以共享并通过写时复制保持逻辑值独立。共享的物理表示不授予共享可变状态。具体后端与类型族的当前实现资格见 §17。

### 16.2 跨任务的值与身份

跨任务边界保留可复制 Sendable 值的逻辑副本，Sendable class 和同步句柄保持同一身份。值的后续修改互不影响，共享可变身份只能经其同步操作访问。物理复制、写时复制和引用计数带由实现选择，并满足 §16.3.1。当前跨任务实现资格见 §17。

### 16.3 对象生命周期与回收

普通对象由引用计数管理。最后一份强引用释放后销毁对象，并释放其拥有的出边。借用不会延长到 owner 之外。

L0 在特化后判定类型是否可能成环，L1 允许通过 `weak` 显式断环。L2 只登记可能成环的身份对象，包括 class、闭包环境和被捕获可变绑定的 cell。含未知函数值、未闭合泛型或未知捕获的身份类型保守视为可能成环。`string`、`Array`、`Map`、`Set` 的 backing 以及 struct、enum 值本身不登记。

任务结束时先释放任务根，再清空已登记存活对象的拥有式出边，由普通引用计数完成销毁。发布到其他任务的登记对象在发布前脱离源任务的回收域。该机制不直接批量释放对象内存，也不提供 tracing GC。共享环和长期运行的主任务中的环不因本条获得回收保证。当前实现资格见 §17。

#### 16.3.1 引用计数的两个带

对象的有符号计数区分本地带与共享带。本地带用正数表示引用数，共享带用负数的幅值表示引用数。共享带使用原子 retain/release，本地带使用仅由一个任务访问的快速路径。

其他任务可观察一个值之前，发布方提升其可达的本地对象，并在已共享子图处停止。提升完成后才能发布。共享值对象不可原地修改。独占拥有一个计数为 −1 的值对象时，可仅将该对象降为 +1 后修改，子对象保持原带。其余修改先建立本地 COW 副本。仍由共享父对象公开持有的子对象不因计数为 −1 自动取得独占修改资格。

身份对象、驻留缓存和 weak 句柄遵循同一发布不变量。weak 句柄在跨任务前提升，共享 weak 操作使用原子同步。共享对象可由任意线程释放。实现可以复制无身份的小值，或将整体独占移交的对象图保持本地，但可观察行为与语言合同一致。提升点见 §16.9.4，当前实现资格见 §17。

### 16.4 协程调度

- **并行**：协程可能在多个 OS 线程上并行运行，并可能在两个挂起点之间换到另一个线程；§16.9 的边 12 保证迁移前后的顺序。
- **协作式**：只在挂起点切换，不强制抢占；不含挂起点的循环会一直占用当前线程，也不响应取消（§10.1、§10.4）。
- **让出**：运行时可以在任何挂起点让出，包括本可立即完成的 Channel、I/O 与计时器操作；一个任务连续执行大量这类操作时，会暂时让给其它任务。
- **阻塞调用**：在 hosted 目标上，调用会阻塞 OS 线程的外部 C 函数或阻塞式系统调用时，同一线程上排队的其它协程转到别的线程继续运行。阻塞调用本身不是挂起点，期间不响应取消。freestanding 目标不提供这项保证。
- **公平性**：调度顺序不暴露用户级优先级；程序不得依赖任务的启动顺序或完成顺序。
- **栈**：只有可能挂起的函数保存跨挂起点的状态；不挂起的调用使用普通调用栈，深递归受普通栈容量限制。

### 16.5 进程级全局访问

- `process`（全局内置，无需 import）：进程自身信息。
- `os`（需 `import os`）：操作系统、环境、进程控制。

完整类型及操作族的当前实现资格见 §17。

> **命名约定**：`os.*` 以 POSIX 函数名为主（`getenv` / `getcwd` / `getpid`）；不随 Node.js。Node 风格的 `process.env` 映射不提供，请用 `os.getenv(name)` / `os.environ()`。

详见 `stdlib/os/`。

### 16.6 Panic 运行时

内置 `PanicInfo` 类是 prelude 类型（声明：`stdlib/types/panic_info.xr`），仅属于 **panic 通道**。运行时故障（越界、除零、不完整 `match`、运行时不变量违背等）由 VM/AOT runtime 构造 `PanicInfo` 对象：

```xray
class PanicInfo {
    message: string             // 人类可读消息
    stack: Array<string>        // 自动 capture 的调用栈，每帧一行格式化字符串
    cause: PanicInfo?           // 链式 cause
    code: i64                   // 错误码（从 "E0xxx: ..." 前缀自动解析，默认 0）
    data: JSON.Value            // 运行时故障的结构化附加数据；无数据时为 JSON null

    init(message: string = "", cause: PanicInfo? = null)
    fn toString() -> string
}
```

用户级可恢复错误不使用 `PanicInfo`：`throw` 语句只接受 enum 变体值（见 §8.1.1）；非 enum 错误值在编译期被拒绝（错误码 `E0370`）。

栈展开（仅 panic 通道）：VM `xvm_unwind_stack()` 按 try-table 查找 `catch panic` handler。每条跨越 cleanup 所属作用域的边都进入编译期生成的静态 cleanup 区域，按 LIFO 顺序执行后再释放局部并继续展开；运行时不维护动态 defer 栈。可恢复错误走值返回通道，不展开栈。详见 §8。

### 16.7 值返回错误通道运行时

`throw <enum>` 将枚举值写入帧的 `pending_error` 槽位，设置错误标志位并返回。调用方通过 `OP_ERR_CHECK` 检测标志位，决定进入 `catch` handler 或继续向上返回。该通道不展开栈、不分配 `PanicInfo`；在需要传播或捕获错误的调用边界，正常路径只经过可预测的错误标志分支。

### 16.8 绑定释放与临时值生命周期

所有离开作用域的路径先按 LIFO 执行该作用域的 `defer`，再按声明的逆序释放仍拥有值的局部绑定。已交出所有权的绑定不再释放原值。赋值在新值安装后释放被替换的旧值。

所有权可在拥有式存储、消费参数、返回、逃逸捕获、`go` 或发送处交出。编译器可在每条路径的最后一次拥有式使用处隐式移动，结果与显式 `move` 相同。只有部分路径交出时，其余路径仍在作用域退出时释放。不含可达身份的纯值在最后一次使用后可提前释放。class、同步句柄、Channel、Task、资源和函数值等可达身份保持上述可观察释放时机，未知泛型保守按含身份处理。

临时值在完整语句结束时释放，已经转移的除外。READ 和 `ref` 参数是借用，由原 owner 管理释放。消费参数在函数入口成为局部拥有式绑定，捕获随闭包环境存活，任务参数由任务拥有。最后一份强引用释放时进入普通 RC 销毁路径，环的边界见 §16.3。

`defer` 覆盖正常退出、返回、循环转移、错误传播、panic 和取消。当前 M2 清理体保持 NoSuspend/NoTask 义务，取消在清理期间屏蔽。后继阶段的清理效应和本节释放优化的当前实现资格见 §17。语言没有用户定义的析构或 `Drop` 回调，外部资源的显式清理使用 `defer`。

### 16.9 并发内存模型

本节定义两个执行体对同一内存位置的访问何时是**有序的**。§10.9 的 `Ordering` 枚举只描述单个原子操作，不足以推导程序行为；能推导的是本节。

#### 16.9.1 基本定义

**执行体 (execution agent)**：协程与 OS 线程都是执行体。一个协程在其生命周期内可能被 work-stealing 调度器搬到不同的 worker 线程上，它仍然是同一个执行体（见边 12）。

**sequenced-before**：单个执行体内部的求值全序，由 §3.0 求值顺序给出。§3.0 不留未指定顺序，因此这个关系是全序而非偏序。

**synchronizes-with**：由 §16.9.2 的同步边表给出。**该表是穷举的**：表中没有的运行时行为不建立 synchronizes-with。

**happens-before**：(sequenced-before ∪ synchronizes-with) 的传递闭包。

**冲突访问**：两个执行体对同一内存位置的访问，其中至少一个是写。

**数据竞争 (data race)**：一对冲突访问，二者之间**不存在**任一方向的 happens-before，且二者**不都是**同一原子对象上的原子操作。

引用计数的更新本身就是内存访问：非原子 RC 上的并发 retain/release 是数据竞争，其后果是内存损坏而不仅仅是数值错误。RC 提升规则见 §16.9.4。

#### 16.9.2 同步边（穷举）

| # | 边 | 建立的关系 |
|--:|---|---|
| 1 | Channel send → recv | 第 k 次成功 `send(v)` **之前**发生的一切，happens-before 接收到 v 的那次 `recv` / `recvOr` / `tryRecv` / `for-in` 迭代**之后**发生的一切。有缓冲与无缓冲同规则 |
| 2 | Channel recv → 后续 send（容量边） | 容量为 N 的 channel 上，第 k 次 `recv` 完成 happens-before 第 (k + N) 次 `send` 完成。这条边是把 `Channel(N)` 当信号量/限流器使用的正确性基础 |
| 3 | Channel close | `close()` 之前发生的一切，happens-before 任何观测到 `Recv.Closed` 或 `isClosed == true` 之后发生的一切 |
| 4 | `go` 启动 | `go expr` 处对实参、capture values 与 `move` 转移的求值，happens-before 协程体的第一条语句 |
| 5 | Task 完成 → 终态观察 | 任务体的最后一个动作，happens-before 等待者通过 `await t`、`awaitResult()` 或 `poll()` 成功观察到该终态后的操作；`poll()` 返回 `null` 不建立这条完成边，也不驱动子任务 |
| 6 | scope 退出 | scope 内全部子任务的完成，happens-before scope 块之后的语句。`Task.all` / `Task.first` / `Task.firstSuccess` 对它们实际观察到的每个终态建立第 5 条边；返回时仍在运行的余任务不建立边 |
| 7 | 模块 `const` 发布 | 顶层 `const` 的初始化与发布，happens-before 任何执行体对该绑定的首次读取 |
| 8 | Atomic release / acquire | `store(v, Release)` synchronizes-with 读到 v 的 `load(Acquire)`；RMW 构成 release sequence；`SeqCst` 操作另外参与一个单一全序。`AcquireRelease` 同时具备两侧 |
| 9 | 协程域锁 | `sync.Mutex` / `sync.RwLock`：一次 unlock synchronizes-with 后续成功的 lock |
| 10 | OS 线程域原语 | `sys.OsMutex` / `sys.OsRwLock` / `sys.OsCondvar` / `sys.OsBarrier`：同上；`sys.OsOnce` 的初始化 happens-before 所有观测到其已完成的调用 |
| 11 | `sys.Thread` | `spawn` 之前发生的一切 happens-before 线程体；线程体 happens-before `join` 返回之后 |
| 12 | 协程迁移 | 协程在 worker A 上挂起、在 worker B 上恢复：挂起前发生的一切 happens-before 恢复后发生的一切。这条边由运行时保证，用户代码看不见它，但**没有它，work-stealing 下的任何顺序推理都不成立** |
| 13 | `select` | 被选中的分支建立其对应 channel 操作的边（第 1 / 2 / 3 条） |

#### 16.9.3 非边（穷举地否定）

以下行为**不**建立 happens-before，不得用于同步：

- `Task.yieldNow()`：它是调度让出点，不是同步点。
- `codegen.compilerFence()`：只阻止有内存效果的操作跨越该编译器调度点，不是 CPU 屏障，不建立 happens-before，不能修复数据竞争（见 §12）。
- `Ordering.Relaxed` 的原子操作：保证该位置的访问不撕裂，不建立与其他位置的顺序关系。
- 对象回收 / RC 归零：回收点虽然精确（§16.8），但不建立跨执行体的顺序关系，因此不能用来承载同步语义。
- sysmon 心跳与 `XRAY_SYSMON_CANCEL_MS` 取消标记。
- 时间：`time.sleep` 与任何超时到期都不建立顺序关系。

#### 16.9.4 引用计数提升点

发布方在其他任务可观察之前完成可达本地对象的提升，已共享子图直接复用。共享可变身份在后续安装新值时也保持该不变量。提升同时使已登记对象脱离源任务回收域，计数带与独占修改规则见 §16.3.1。

| 提升点 | 发布边界 |
|--|--|
| `go` 实参与捕获 | 任务开始执行前 |
| Channel 发送，包括 `select` 发送分支 | 接收方可观察前 |
| Task / scope 结果 | 终态结果发布前 |
| 同步容器 | 新值对其他任务可见前，Mutex / RwLock 在解锁前重新提升 |
| 模块 `const` | 初始化完成后的发布前 |
| TaskLocal 提供的值 | 提供给任务前 |
| `sys.Thread` 体与 `CFn` 可达值 | 线程或外部回调可访问前 |

这些规则不授予裸指针额外权限，§16.9.5 的安全边界继续适用。当前各发布入口的实现资格见 §17。

#### 16.9.5 数据竞争自由的保证与边界

**定理（安全子集的 DRF）**：不含 `unsafe` 块、且不使用下方"逃逸口"能力的程序，不存在数据竞争；其行为等价于某个顺序一致（sequentially consistent）的交错执行。这就是 SC-DRF：在安全子集里可以按"语句交错"来推理，不需要理解本节的 happens-before 细节。

**逃逸口（穷举）**：以下能力可以引入数据竞争。它们的后果是**实现定义的，可能包含内存损坏**，证明责任在使用者：

- `unsafe` 块内的一切原始内存访问
- `Array.mutPtr()` 产生的可写原始指针（指针作为**实参**跨协程边界时不经过值发布检查）
- `mem.volatileLoad` / `volatileStore` / `fence` / `pageAlloc`
- `sys.Thread` 的线程体
- `CFn` C 回调体（可在任意线程被调用）

> 这是本语言"编译期消除数据竞争"这一说法的**准确形式**：它是一个带明确边界的定理，不是无条件断言。缩小逃逸口清单是语言演进的目标之一；每次缩小都必须同时给出该能力被纳入保证的机制。

#### 16.9.6 对编译器的约束

同步边不只是运行时承诺，它同时约束优化。`xisa/xi/ops.def` 的 `:sync` 列为每个 Xi 操作声明它建立的边（`none` / `acquire` / `release` / `acq-rel` / `seq-cst`）；当边的强度由运行期 `Ordering` 实参决定时，声明的是该操作可能承载的**最强**边，即 fail-closed 上界。

由此得到一条对所有 pass 的硬性规则：**别名不相交不构成重排许可**。两个内存操作即使 TBAA 证明不相交，也不得跨越携带 `:sync` 边或可挂起的操作移动。该规则由 `xi_op_is_ordering_barrier()` 统一回答。
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 16. Runtime Model

This section defines runtime language contracts. Current qualification of each backend and type family is specified in §17.

### 16.1 Value Representation

Machine representation is implementation-defined. Scalars, structs, tuples, enums, nullable values and fixed arrays may use registers, flattened slots or aggregate storage. The language specifies no universal value header, tag width or object size. Static types, integer widths, signedness, value semantics and identity semantics remain consistent across backends.

The backing of `string`, `Array`, `Map` and `Set` may be shared with copy-on-write preserving independent logical values. Shared physical representation grants no shared mutable state. Current backend and type-family qualification is specified in §17.

### 16.2 Values and Identities Across Tasks

A cross-task boundary preserves logical copies of copyable Sendable values, while Sendable classes and synchronization handles retain identity. Subsequent value mutations are independent, and shared mutable identities are accessed through synchronized operations. Physical copying, copy-on-write and reference-count bands are implementation choices subject to §16.3.1. Current cross-task qualification is specified in §17.

### 16.3 Object Lifetime and Reclamation

Reference counting manages ordinary objects. Releasing the final strong reference destroys the object and releases its owned outgoing edges. A loan cannot outlive its owner.

L0 determines potential cycles after specialization, and L1 permits explicit cycle breaking with `weak`. L2 registers only potentially cyclic identity objects, including classes, closure environments and cells of captured mutable bindings. Identity types with unknown function values, unresolved generics or unknown captures are conservatively potentially cyclic. The backing of `string`, `Array`, `Map` and `Set`, and struct or enum values themselves, are unregistered.

At task completion, task roots are released first, then owned outgoing edges of registered live objects are cleared, allowing ordinary reference counting to destroy them. Registered objects published to another task leave the source task's reclamation domain before publication. This mechanism clears edges and leaves memory release to reference counting. It provides no tracing GC. Shared cycles and cycles in a long-running main task receive no reclamation guarantee from this rule. Current implementation qualification is specified in §17.

#### 16.3.1 The Two Reference-Count Bands

A signed object count distinguishes local and shared bands. Positive counts encode local references, and the magnitude of a negative count encodes shared references. The shared band uses atomic retain/release, and the local band uses a fast path accessed by one task.

Before another task can observe a value, the publisher promotes reachable local objects, stopping at already-shared subgraphs. Publication follows completed promotion. Shared value objects are immutable. An exclusively owned value object with count −1 may be demoted to +1 for mutation, leaving its children in their existing bands. Other mutations first create a local COW copy. A child still publicly owned by a shared parent gains no exclusive mutation permission solely from a count of −1.

Identity objects, intern caches and weak handles obey the same publication invariant. Weak handles are promoted before crossing tasks, and shared weak operations use atomic synchronization. Shared objects may be released on any thread. An implementation may copy identity-free small values or keep a wholly exclusive transferred graph local, while preserving the observable language contract. Promotion points are specified in §16.9.4, and current qualification in §17.

### 16.4 Coroutine Scheduling

- **Parallelism**: coroutines may run in parallel on several OS threads and may move to another thread between two suspension points; edge 12 of §16.9 orders everything across such a move.
- **Cooperative**: switches happen only at suspension points and nothing is preempted; a loop without a suspension point keeps its thread and does not observe cancellation (§10.1, §10.4).
- **Yielding**: the runtime may yield at any suspension point, including Channel, I/O and timer operations that could complete immediately; a task that performs many such operations in a row is temporarily set aside for other tasks.
- **Blocking calls**: on hosted targets, while an external C function or blocking system call blocks an OS thread, the other coroutines queued on that thread continue on another thread. The blocking call itself is not a suspension point and does not observe cancellation meanwhile. Freestanding targets do not provide this guarantee.
- **Fairness**: scheduling order exposes no user-level priorities; programs must not rely on the order in which tasks start or finish.
- **Stack**: only functions that may suspend keep state across a suspension point; non-suspending calls use the ordinary call stack, so deep recursion is bounded by ordinary stack capacity.

### 16.5 Process-Level Global Access

- `process` (global builtin, no import required): self-process information.
- `os` (requires `import os`): operating system, environment, process control.

Current implementation qualification of complete type and operation families is specified in §17.

> **Naming convention**: `os.*` follows POSIX function names (`getenv` / `getcwd` / `getpid`); it does not track Node.js. Node-style `process.env` mapping is not provided — use `os.getenv(name)` / `os.environ()`.

See `stdlib/os/` for details.

### 16.6 Panic Runtime

The built-in `PanicInfo` class is a prelude type (declared in `stdlib/types/panic_info.xr`) and belongs only to the **panic channel**. Runtime faults (out-of-bounds, division by zero, non-exhaustive `match`, runtime invariant violations, and similar faults) are represented by `PanicInfo` objects constructed by the VM/AOT runtime:

```xray
class PanicInfo {
    message: string             // human-readable message
    stack: Array<string>        // automatically captured call stack, one formatted line per frame
    cause: PanicInfo?           // chained cause
    code: i64                   // error code (auto-parsed from "E0xxx: ..." prefix; default 0)
    data: JSON.Value            // structured data for a runtime fault; JSON null when absent

    init(message: string = "", cause: PanicInfo? = null)
    fn toString() -> string
}
```

Recoverable user-level errors do not use `PanicInfo`: a `throw` statement accepts enum variant values only (see §8.1.1); non-enum error values are rejected at compile time (error code `E0370`).

Stack unwinding (panic channel only): the VM's `xvm_unwind_stack()` walks the try-table to find `catch panic` handlers. Every edge that crosses a cleanup's owning scope enters a statically generated cleanup region, runs it in LIFO order, then releases locals and continues unwinding; the runtime maintains no dynamic defer stack. Recoverable errors use the value-return channel and never unwind the stack. See §8 for details.

### 16.7 Value-return Error Channel Runtime

`throw <enum>` writes the enum value into the frame's `pending_error` slot, sets the error flag, and returns. The caller detects the flag via `OP_ERR_CHECK` and either enters a `catch` handler or continues returning upward. This channel performs no stack unwinding and allocates no `PanicInfo`; at call boundaries that may propagate or catch errors, the happy path goes through only a predictable error-flag branch.

### 16.8 Binding Release and Temporary Lifetimes

Every scope-exit path first runs that scope's `defer` bodies in LIFO order, then releases still-owning local bindings in reverse declaration order. A binding that transferred ownership no longer releases the transferred value. Assignment installs the new value before releasing the replaced value.

Ownership can transfer into owning storage, a consuming parameter, a return, an escaping capture, `go` or a send. The compiler may move implicitly at the last owning use on every path, with the same result as explicit `move`. When only some paths transfer ownership, the remaining paths release at scope exit. Values with no reachable identity may be released after their last use. Reachable identities such as classes, synchronization handles, Channels, Tasks, resources and function values preserve the observable release timing above. Unknown generics conservatively count as containing identity.

Temporaries are released at the end of the complete statement unless transferred. READ and `ref` parameters are loans whose original owners manage release. A consuming parameter becomes an owning local at function entry, captures live with the closure environment, and task arguments are owned by the task. Releasing the final strong reference starts ordinary RC destruction, with cycle boundaries specified in §16.3.

`defer` covers normal exit, return, loop transfers, error propagation, panic and cancellation. Current M2 cleanup bodies retain NoSuspend/NoTask obligations, with cancellation masked during cleanup. Qualification of later cleanup effects and the release optimizations in this section is specified in §17. The language has no user-defined destructor or `Drop` callback. Explicit cleanup of external resources uses `defer`.

### 16.9 Concurrency Memory Model

This section defines when accesses by two execution agents to the same memory location are **ordered**. The `Ordering` enum in §10.9 describes one atomic operation; it is not enough to derive program behaviour. This section is.

#### 16.9.1 Basic definitions

**Execution agent**: coroutines and OS threads are both execution agents. A coroutine may be moved between worker threads by the work-stealing scheduler during its lifetime and remains the same agent (see edge 12).

**Sequenced-before**: the total order of evaluation within one agent, given by §3.0 Evaluation Order. Because §3.0 leaves no order unspecified, this relation is total rather than partial.

**Synchronizes-with**: given by the edge table in §16.9.2. **That table is exhaustive**: no runtime behaviour outside it establishes synchronizes-with.

**Happens-before**: the transitive closure of (sequenced-before ∪ synchronizes-with).

**Conflicting accesses**: accesses by two agents to the same memory location, at least one of which is a write.

**Data race**: a pair of conflicting accesses with **no** happens-before between them in either direction, where the two are **not both** atomic operations on the same atomic object.

Reference-count updates are themselves memory accesses: concurrent retain/release on a non-atomic RC is a data race whose consequence is memory corruption, not merely a wrong number. The promotion rules are in §16.9.4.

#### 16.9.2 Synchronisation edges (exhaustive)

| # | Edge | Relation established |
|--:|---|---|
| 1 | Channel send → recv | Everything sequenced **before** the k-th successful `send(v)` happens-before everything sequenced **after** the `recv` / `recvOr` / `tryRecv` / `for-in` iteration that receives v. Buffered and unbuffered channels follow the same rule |
| 2 | Channel recv → later send (capacity edge) | On a channel of capacity N, completion of the k-th `recv` happens-before completion of the (k + N)-th `send`. This edge is what makes `Channel(N)` correct as a semaphore or rate limiter |
| 3 | Channel close | Everything before `close()` happens-before everything after any observation of `Recv.Closed` or `isClosed == true` |
| 4 | `go` spawn | Evaluation of the arguments, capture values and `move` transfers at the `go expr` happens-before the first statement of the coroutine body |
| 5 | Task completion → terminal observation | The task body's last action happens-before operations after a waiter successfully observes that terminal outcome through `await t`, `awaitResult()`, or `poll()`; a `null` poll establishes no completion edge and does not drive the child |
| 6 | scope exit | Completion of every child task in a scope happens-before the statements after the scope block. `Task.all` / `Task.first` / `Task.firstSuccess` establish edge 5 for every terminal outcome they actually observe; remaining tasks still running when they return establish no edge |
| 7 | Module `const` publication | Initialization and publication of a top-level `const` happens-before any agent's first read of that binding |
| 8 | Atomic release / acquire | `store(v, Release)` synchronizes-with the `load(Acquire)` that reads v; RMWs form a release sequence; `SeqCst` operations additionally participate in one single total order. `AcquireRelease` carries both sides |
| 9 | Coroutine-domain locks | `sync.Mutex` / `sync.RwLock`: an unlock synchronizes-with a later successful lock |
| 10 | OS-thread-domain primitives | `sys.OsMutex` / `sys.OsRwLock` / `sys.OsCondvar` / `sys.OsBarrier`: as above; a `sys.OsOnce` initialisation happens-before every call that observes it as complete |
| 11 | `sys.Thread` | Everything before `spawn` happens-before the thread body; the thread body happens-before everything after `join` returns |
| 12 | Coroutine migration | A coroutine suspended on worker A and resumed on worker B: everything before the suspension happens-before everything after the resumption. The runtime guarantees this edge; user code never names it, but **without it no ordering argument holds under work-stealing at all** |
| 13 | `select` | The selected arm establishes the edge of its channel operation (edges 1 / 2 / 3) |

#### 16.9.3 Non-edges (exhaustively denied)

The following establish **no** happens-before and must not be used to synchronise:

- `Task.yieldNow()`: a scheduling yield point, not a synchronisation point.
- `codegen.compilerFence()`: it only prevents memory-effecting operations from moving across that compiler scheduling point. It is not a CPU fence, establishes no happens-before, and cannot repair a data race (see §12).
- Atomic operations with `Ordering.Relaxed`: they keep that location's accesses from tearing; they order nothing with respect to other locations.
- Object reclamation or an RC reaching zero establishes no synchronization edge. The release timing in §16.8 therefore cannot carry a cross-agent ordering relation.
- The sysmon heartbeat and the `XRAY_SYSMON_CANCEL_MS` cancellation flag.
- Time: neither `time.sleep` nor any timeout expiry establishes ordering.

#### 16.9.4 Reference-Count Promotion Points

The publisher promotes reachable local objects before another task can observe them, reusing already-shared subgraphs. Shared mutable identities preserve this invariant when installing new values later. Promotion also removes registered objects from the source task's reclamation domain. Count bands and exclusive mutation follow §16.3.1.

| Promotion point | Publication boundary |
|--|--|
| `go` arguments and captures | Before the task begins execution |
| Channel sends, including `select` send arms | Before the receiver can observe the value |
| Task / scope results | Before publication of the terminal result |
| Synchronized containers | Before another task can observe the new value, with Mutex / RwLock re-promotion before unlock |
| Module `const` | Before publication after initialization |
| Values provided by TaskLocal | Before provision to the task |
| Reachable values of `sys.Thread` bodies and `CFn` callbacks | Before access by the thread or external callback |

These rules grant no additional raw-pointer permission. The safety boundary in §16.9.5 continues to apply. Current qualification of each publication entry point is specified in §17.

#### 16.9.5 The data-race-freedom guarantee and its boundary

**Theorem (DRF for the safe subset)**: a program containing no `unsafe` block and using none of the escape hatches below has no data race, and its behaviour is equivalent to some sequentially consistent interleaving. That is SC-DRF: inside the safe subset you may reason in terms of interleaved statements and never need the happens-before detail in this section.

**Escape hatches (exhaustive)**: the following capabilities can introduce a data race. Their consequences are **implementation-defined and may include memory corruption**; the proof obligation is the user's:

- Any raw memory access inside an `unsafe` block
- The writable raw pointer from `Array.mutPtr()` (as an **argument** a pointer crosses a coroutine boundary without passing through value-publication checking)
- `mem.volatileLoad` / `volatileStore` / `fence` / `pageAlloc`
- The body of a `sys.Thread`
- The body of a `CFn` C callback (which may be invoked on any thread)

> This is the **precise form** of the claim that xray eliminates data races at compile time: a theorem with a stated boundary, not an unconditional assertion. Shrinking the escape-hatch list is a goal of the language's evolution, and each shrink must come with the mechanism that brings that capability inside the guarantee.

#### 16.9.6 Obligations on the compiler

A synchronisation edge is not only a runtime promise; it also constrains optimisation. The `:sync` column of `xisa/xi/ops.def` declares the edge each Xi operation establishes (`none` / `acquire` / `release` / `acq-rel` / `seq-cst`). Where the edge's strength is chosen at run time by an `Ordering` argument, the declaration is the **strongest** edge that operation can carry — a fail-closed upper bound.

From this follows one hard rule for every pass: **alias disjointness is not a licence to reorder**. Two memory operations must not be moved across an operation that carries a `:sync` edge or that may suspend, even when TBAA proves them disjoint. `xi_op_is_ordering_barrier()` is the single answer to that question.
<!-- /xr-spec:en -->

---
id: spec.10_concurrency_and_coroutines
order: 011
---

<!-- xr-spec:cn -->
---

## 10. 并发与协程 (Concurrency)

> 真值源：`src/runtime/coro/xcoro_*.c`、`src/runtime/sync/xchannel.c`、`src/runtime/sync/xscope.c`、`docs/rules/design-principles.md`。

xray 的并发是**协程 (goroutine 风格) + Channel + 强静态约束**。设计目标：写 `go { ... }` 就和写普通函数一样简单，但**编译期保证不发生数据竞争**。

### 10.1 协程模型

| 维度 | 选择 |
|--|--|
| 调度模型 | M:N（用户态协程 + 多 OS 线程） |
| 调度策略 | 协作式（GC-safepoint）+ work-stealing |
| 栈模型 | Stackless（每协程独立 VM 值栈 + 帧数组，按需增长，无原生 C 栈） |
| 创建开销 | ~微秒级（初始 VM 值栈约 64 槽 + 4 帧，非原生栈） |
| 上下文切换 | VM 上下文切换（保存/恢复 VM 帧），无原生栈切换、无 syscall |

协程默认分布在多个 worker 线程上；运行时根据 CPU 核数自动设置 GOMAXPROCS 风格的并行度。

### 10.2 `go` — 启动协程

```ebnf
GoExpr   ::= 'go' GoOptions? (Block | CallExpr | LambdaExpr CallArgs?)
GoOptions ::= '(' GoOption (',' GoOption)* ')'
GoOption ::= 'name' ':' StringLiteral
```

`go` 是**表达式**，返回 `Task<T>` 句柄。三种形式都合法：

```xray @id=coro-go-forms
// 形式 1：调用一个已声明的函数
let t1 = go worker(0, channel)

// 形式 2：调用一个 lambda 字面量（用于内联逻辑 + 显式传参）
let t2 = go fn(d: Json) -> int {
    return d.value * 2
}(payload)

// 形式 3：块形式（隐式包装为零参 lambda）
let t3 = go {
    return compute()
}

// 可选调试名称
let named = go(name: "worker-1") worker(1, channel)
```

**move 在参数位置**：跨协程转移所有权通过参数前缀 `move` 实现，**不是** `go` 的选项：

```xray @id=coro-move-argument
shared let data = { value: 10 }
let task = go fn(d: Json) -> int {
    return d.value + 1
}(move data)        // 把 data 的所有权移交给协程；之后 data 不可访问
```

**块形式限制**：`go { ... }` 是隐式零参 lambda，没有参数列表，也不会绕过并发捕获规则。块内不能捕获普通可变局部；可直接使用的外部状态必须是 `shared const`、全局不可变状态，或显式并发安全对象（如 Channel / Atomic）。需要把局部数据传入协程时，使用带参数的 lambda / 函数调用形式：

```xray
let n = 10
let task = go fn(x: int) -> int {
    return x + 1
}(n)
```

**语义**：
- 每个 `go` 表达式都返回一个 `Task<T>`，其中 `T` 是被调函数的返回类型；返回 `()` 的函数对应 `Task<null>`。
- 协程在闲置 worker 线程中调度（M:N）。
- `go(name: ...)` 只设置调试名称，不影响调度顺序。
- 协程内**未捕获**异常存在 `Task` 中，由 `await` 时重抛。
- 普通局部变量（非 `shared`、非 `move`）传给 `go` 时**自动深拷贝**；`shared const` 零拷贝共享；`shared let` 必须 `move`。
- `go { ... }` 块形式等价于零参 lambda，只能使用符合协程捕获规则的外部状态；传参请用 `go fn(x: T) -> R { ... }(arg)` 或 `go worker(arg)`。

### 10.3 `await` — 等待结果

```ebnf
AwaitExpr ::= 'await' Expression
           |  'await' 'all' Expression       // 等待全部完成
           |  'await' 'any' Expression       // 等待任一完成
```

```xray @id=coro-await-forms
// 单 task
let task = go fetch("https://example.com")
let result = await task                    // 让出当前协程直到 task 完成

// await all：等待全部完成，返回结果数组（与输入顺序一致）
let t1 = go compute(2)
let t2 = go compute(3)
let t3 = go compute(4)
let results: Array<int> = await all [t1, t2, t3]
// 也可直接对变量使用，无需中括号
let tasks = [t1, t2, t3]
let results2: Array<int> = await all tasks

// await any：等待任一完成，返回该任务结果；其他任务继续运行
let first = await any [t1, t2, t3]

// await anySuccess：跳过失败任务，等待第一个成功的任务
let firstOk = await anySuccess [t1, t2, t3]
```

**语义**：

- `await` 仅作用于 `Task<T>` 类型；其他类型为编译错误。
- 当前协程**让出**直到目标完成（不阻塞 OS 线程）。
- 异常传播：
  - `await t` 重抛 t 抛出的异常。
  - `await t` 成功时返回 `T`；如果 `T` 是 `T?`，返回的 `null` 是任务真实结果，不代表取消或失败。
  - `await all` 中任一任务抛异常即整体抛异常（其余任务会被取消）。
  - `await any` 仅当**全部失败**时抛异常；只要有一个完成，返回该任务结果。
  - `await anySuccess` 类似 `await any`，但**跳过**抛异常的任务，只等成功完成的。
- `all` / `any` / `anySuccess` 在 `await` 后面是**上下文关键字**，仅在此位置生效。
- `await all` 的输入必须是同构任务集合：每个元素都必须是同一静态 `Task<T>` 类型，结果类型为 `Array<T>`。异构任务（如 `Task<int>` 与 `Task<string>` 混合）不会自动擦除或装箱；需要逐个 `await`，或在任务内部显式转换为统一 enum / union / Json 结果类型。

### 10.4 `Task<T>` 句柄

`go expr` 返回 `Task<T>`，其中 `T` 为被调函数的返回类型。Task 句柄支持：

| 方法 / 属性 | 类型 | 说明 |
|--|--|--|
| `t.done` | `bool`（属性） | 任务是否已完成（成功、失败或取消） |
| `t.status` | `TaskStatus`（属性） | `Pending` / `Running` / `Success` / `Failed` / `Cancelled` |
| `t.cancel()` | `() -> ()` | 请求取消任务（合作式） |
| `t.poll()` | `() -> TaskResult<T>` | 非阻塞观察；未完成返回 `TaskResult.Pending` |
| `t.awaitResult()` | `() -> TaskResult<T>` | 阻塞等待并返回状态结果，不重抛异常 |
| `t.awaitTimeout(ms)` | `(int) -> TaskResult<T>` | 阻塞到完成或超时，超时返回 `TaskResult.Timeout` |

```xray @id=coro-task-handle
let t = go fetch(url)
if (!t.done) { /* 还在跑 */ }
let r = await t

match t.poll() {
    TaskResult.Pending -> print("running")
    TaskResult.Success(value) -> print(value)
    TaskResult.Failed(err) -> print(err)
    TaskResult.Cancelled -> print("cancelled")
    TaskResult.Timeout -> print("timeout")
}
```

**取消语义**：`cancel()` 设置取消标志；协程在下一个 safepoint（GC 检查点、Channel 操作、`await`、`yield`）检测到标志后抛出取消异常。plain `await` 已取消的 task 会抛 `TaskCancelled`；需要状态值时使用 `awaitResult()` 或 `awaitTimeout(ms)`。

**看门狗强制取消**：运行时监控线程（sysmon）会强制取消**长时间不经过 safepoint**的协程——当某协程在 RUNNING 状态下心跳冻结超过阈值（默认 5 秒）时被标记取消。该阈值可经环境变量 `XRAY_SYSMON_CANCEL_MS` 配置（单位毫秒），设为 `0` 则**禁用**强制取消（仅在 ~100ms 时打印一次告警）。纯 CPU 紧循环若可能长时间运行，应在循环内插入 `yield` 以提供 safepoint，避免被看门狗误取消。

### 10.5 Channel

```ebnf
ChannelType ::= 'Channel' '<' Type '>'
ChannelNew  ::= 'new' 'Channel' ('<' Type '>')? '(' Expression ')'
```

Channel 通常以 `shared const` 声明（生命周期跨协程，引用语义）：

```xray @id=channel-decl-variants
shared const ch  = new Channel<int>(10)    // 有缓冲，capacity = 10
shared const ch0 = new Channel<int>(0)     // 无缓冲（同步握手）
shared const cha = new Channel(3)          // 元素类型从首次 send 推断
```

**API**（注意全部为 **camelCase**）：

| 方法 | 签名 | 行为 |
|--|--|--|
| `send(v)` | `(T) -> ()` | 阻塞发送；满则等待消费者；channel 已关闭时抛异常 |
| `recv()` | `() -> Recv<T>` | 阻塞接收；关闭且缓冲为空时返回 `Recv.Closed` |
| `trySend(v)` | `(T) -> SendResult` | 非阻塞发送；返回 `Sent` / `Full` / `Closed` |
| `tryRecv()` | `() -> Recv<T>` | 非阻塞接收；空时返回 `Recv.Empty` |
| `sendTimeout(v, ms)` | `(T, int) -> SendResult` | 带超时发送；超时返回 `SendResult.Timeout` |
| `recvTimeout(ms)` | `(int) -> Recv<T>` | 带超时接收；超时返回 `Recv.Timeout` |
| `close()` | `() -> ()` | 关闭 channel；幂等 |
| `isClosed` | `bool`（属性） | channel 是否已关闭 |

```xray @id=channel-basic-ops
shared const ch = new Channel<int>(10)
ch.send(42)                             // 阻塞发送
let v = match ch.recv() {
    Recv.Value(value) -> value
    Recv.Closed -> -1
    _ -> -1
}

let sent = ch.trySend(99)               // SendResult.Sent / Full / Closed
match ch.tryRecv() {
    Recv.Value(next) -> print(next)
    Recv.Empty -> print("empty")
    Recv.Closed -> print("closed")
    Recv.Timeout -> print("timeout")
}

ch.close()
```

**send/recv 与 `move`**：发送大对象时用 `ch.send(move payload)` 转移所有权，避免拷贝；接收方独占。

Channel 在类型位置写作 `Channel<T>`，可用于函数参数、字段和返回类型：

```xray @id=channel-param-type
fn producer(ch: Channel<int>) {
    ch.send(42)
}
```

**语义**：
- **MPMC**（多生产者多消费者）。
- 有缓冲 ch：满则发送方挂起，空则接收方挂起。
- 无缓冲 ch：发送/接收必须同时握手（rendezvous）。
- 关闭后：`send` 抛异常；`recv` 返回剩余 buffered value 的 `Recv.Value(v)`，取完后返回 `Recv.Closed`；`tryRecv` 在空且未关闭时返回 `Recv.Empty`。

### 10.6 `select`

`select` 在多个 channel 操作中多路复用；非阻塞分支用 `_` 占位。

```ebnf
SelectStmt ::= 'select' '{' SelectArm+ '}'
SelectArm  ::= RecvArm | SendArm | TimeoutArm | DefaultArm
RecvArm    ::= Identifier 'from' Expression '->' Block
SendArm    ::= Expression 'to' Expression '->' Block
TimeoutArm ::= 'after' Expression '->' Block
DefaultArm ::= '_' '->' Block
```

```xray @id=coro-select
shared const ch1 = new Channel<int>(2)
shared const ch2 = new Channel<int>(2)

select {
    msg from ch1 -> { print("got from ch1:", msg) }      // 接收分支
    msg from ch2 -> { print("got from ch2:", msg) }      // 接收分支
    100  to   ch1 -> { print("sent 100 to ch1") }        // 发送分支
    _ -> { print("no channel ready") }                   // 默认分支（非阻塞）
}
```

**语义**：
- 接收分支 `name from ch -> body`：在 ch 有数据时被选中，并把 `Recv.Value(name)` 的 payload 绑定到 `name`。
- 发送分支 `value to ch -> body`：等价于 `ch.send(value)`，但仅在 ch 有空间时被选中。
- 默认分支 `_ -> body`：当前无任何分支就绪时立即执行；**省略默认分支**会让 select 阻塞直到某个分支就绪。
- 多个分支同时就绪时**随机**选择一个（与 Go 一致）。

### 10.7 `scope` 块（结构化并发 / 词法作用域）

`scope` 是**语句关键字**，建立一个新的词法作用域块。它服务两个目的：

1. **纯词法作用域**：与 C/Rust `{ ... }` 局部块一致，块内 `let` 不影响外层同名变量。
2. **结构化并发**（语义增强）：在 `scope` 块内 `go` 启动的协程，块退出前**自动等待**全部完成或取消。

```ebnf
ScopeStmt          ::= 'scope' Block
LinkedScopeStmt    ::= 'linked' 'scope' Block          // 兄弟失败 → 取消所有 + 重抛
SupervisorScopeExpr ::= 'supervisor' 'scope' Block     // 收集所有错误，返回 Array<string>
```

```xray @id=coro-scope
// 词法作用域用途
let x = 1
scope {
    let x = 10            // shadow 外层 x，块内有效
    print(x)              // 10
}
print(x)                  // 1

// 结构化并发用途（与 go 配合）
scope {
    go worker_a()
    go worker_b()
    // scope 块退出前，等待 a/b 全部完成；任一抛异常不影响兄弟
}
```

**三种 scope 变体**：

| 形式 | 子协程抛异常时的行为 | 返回值 |
|---|---|---|
| `scope { ... }` | 不取消兄弟；异常不向外传播（每个 task 独立） | 无（语句形式） |
| `linked scope { ... }` | **取消所有兄弟**协程，并向外**重抛**最先抛出的异常 | 无 |
| `supervisor scope { ... }` | **收集**所有失败子协程的异常消息，子协程之间互不影响 | `Array<string>`（错误列表；可为空表示全部成功） |

```xray @id=coro-linked-supervisor-scope
// linked scope：失败传播
try {
    linked scope {
        go ok_worker()
        go failing_worker()         // 抛异常
    }
} catch (e) {
    print("caught:", e)              // 命中此分支
}

// supervisor scope：收集错误
let errors = supervisor scope {
    go failing("error1")
    go failing("error2")
    go ok()
}
print(errors.length)                 // 2（只统计失败的）
```

**通用语义**：
- `scope` 不是函数调用，也不需要 import；是关键字块语句。
- 三种形式都在块退出前等待所有 `go` 启动的子协程完成。

### 10.8 `move` — 跨协程所有权转移

```ebnf
MoveExpr ::= 'move' Identifier        // 仅出现在调用参数位置
```

`move` 是**实参修饰前缀**（不是 `go` 的选项）。它把 `shared let` 变量的所有权从当前作用域转移到被调函数（包括 `go` 启动的协程、`ch.send()` 等）。move 后原变量在编译期被标记为**已 moved**，再次引用是编译错误。

```xray @id=coro-move-transfer
shared let buf = new Bytes(1024 * 1024)

// 移交给协程
let t = go fn(b: Bytes) -> int {
    return process(b)
}(move buf)
// 编译错误：buf has been moved
// print(buf.length)

// 移交给 channel
shared const ch = new Channel<Bytes>(1)
shared let payload = new Bytes(4096)
ch.send(move payload)
// 编译错误：payload has been moved
```

详见 §7.3、§7.4 关于 shared 变量的捕获规则。

### 10.9 同步原语

xray 的默认并发模型偏向**消息传递 + 不可变共享**——通过 `shared const`、`Channel`、`move`、`scope` 已能在编译期消除大部分数据竞争，因此**不**鼓励使用裸 Mutex/锁。

如确需互斥锁/原子操作，运行时层面提供：

| 原语 | 形态 | 说明 |
|---|---|---|
| Channel(1) | 单元素 channel | 互斥的最佳实践（通过 send/recv 模拟 lock/unlock） |
| `shared let` + `move` | 编译期独占 | 跨协程独占，无运行时开销 |
| `Atomic<T>` | 无锁原子包装 | 对 `int`/`float`/`bool` 提供 C11 原子操作 |

> **设计说明**：xray 不暴露 `Mutex`/`RwLock` 等通用锁原语。对于简单共享计数器、标志位等场景，`Atomic<T>` 是推荐选择；对于复杂互斥场景，使用 `Channel(1)` 模拟 lock/unlock。

#### `Atomic<T>` — 无锁原子类型

`Atomic<T>` 包装 `int`、`float` 或 `bool`，在系统堆上分配，底层使用 C11 原子指令，无需锁即可跨协程安全读写。

**声明约束**：`Atomic<T>` 变量必须声明为 `shared const`，禁止 `move`。

```xray
shared const counter = Atomic(0)         // Atomic<int>
shared const flag = Atomic(false)        // Atomic<bool>
shared const rate = Atomic(3.14)         // Atomic<float>
```

**方法一览**（完整签名见 §14.19）：

| 方法 | 说明 |
|---|---|
| `load(ord?)` | 原子读取 |
| `store(val, ord?)` | 原子写入 |
| `add(val, ord?)` / `sub(val, ord?)` | 原子加减（int/float） |
| `fetchAdd(val, ord?)` / `fetchSub(val, ord?)` | 原子加减并返回旧值 |
| `swap(val, ord?)` | 原子交换，返回旧值 |
| `compareExchange(expected, desired, ord?)` | CAS，返回 `(old, bool)` |
| `toggle(ord?)` | 原子取反（bool），返回旧值 |
| `toString()` | 返回当前值的字符串表示 |

#### `Ordering` 枚举

所有接受 `ord?` 参数的方法均可传入 `Ordering` 枚举指定内存序，默认 `SeqCst`（最强保证）。

```xray
enum Ordering {
    Relaxed = 0,          // 无跨线程排序保证
    Acquire = 1,          // 读屏障
    Release = 2,          // 写屏障
    AcquireRelease = 3,   // 读写屏障
    SeqCst = 4,           // 顺序一致（默认）
}
```

`Ordering` 枚举由编译器自动注入（prelude），无需 import。

```xray
shared const counter = Atomic(0)
counter.store(42, Ordering.Release)
let val = counter.load(Ordering.Acquire)
```


### 10.10 `yield` — 让出 CPU

```ebnf
YieldStmt ::= 'yield'
```

```xray @id=coro-yield-loop
for (i in 0..1000) {
    do_chunk(i)
    yield                       // 主动 safepoint，让其他协程有机会跑
}
```

**当前实现**：作为语句使用，等价 Go 的 `runtime.Gosched()`；不支持带值 `yield`。

### 10.11 并发安全模型

xray 通过类型系统**编译期消除大部分数据竞争**：

| 规则 | 强制 |
|--|--|
| `go` 闭包不能捕获普通 `let` 局部变量 | ✅ |
| `shared const` 跨协程零拷贝只读 | ✅ |
| `shared let` 必须 `move` 才能跨协程 | ✅ |
| Channel 跨协程传值 | ✅ |
| `Atomic<T>` 必须声明为 `shared const`，禁止 `move` | ✅ |

**仍可能存在数据竞争**（运行时检测，非编译期）：
- 在 Channel 中发送可变 class 引用（接收方可能与发送方同时修改）— 建议总是发送 `shared const` / `Bytes` / 不可变对象 / `move` 移交。
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 10. Concurrency and Coroutines

> Source of truth: `src/runtime/coro/xcoro_*.c`, `src/runtime/sync/xchannel.c`, `src/runtime/sync/xscope.c`, `docs/rules/design-principles.md`.

xray's concurrency model is **goroutine-style coroutines + channels + strong static guarantees**. Design goal: writing `go { ... }` is as simple as writing an ordinary function call, while the **compiler guarantees no data race**.

### 10.1 Coroutine model

| Dimension | Choice |
|--|--|
| Scheduling model | M:N (user-space coroutines on multiple OS threads) |
| Scheduling policy | Cooperative (GC safepoints) + work stealing |
| Stack model | Stackless (per-coroutine VM value stack + frame array, grows on demand, no native C stack) |
| Creation cost | ~microsecond (initial VM value stack ~64 slots + 4 frames, not a native stack) |
| Context switch | VM context switch (save/restore VM frames), no native stack switch, no syscall |

Coroutines are distributed across multiple worker threads by default; the runtime sets a Go-style `GOMAXPROCS` parallelism level based on the CPU core count.

### 10.2 `go` — start a coroutine

```ebnf
GoExpr   ::= 'go' GoOptions? (Block | CallExpr | LambdaExpr CallArgs?)
GoOptions ::= '(' GoOption (',' GoOption)* ')'
GoOption ::= 'name' ':' StringLiteral
```

`go` is an **expression** returning a `Task<T>` handle. Three forms are valid:

```xray @id=coro-go-forms
// Form 1: call an existing function
let t1 = go worker(0, channel)

// Form 2: call a lambda literal (inline logic + explicit arguments)
let t2 = go fn(d: Json) -> int {
    return d.value * 2
}(payload)

// Form 3: block form (implicitly wrapped as a zero-argument lambda)
let t3 = go {
    return compute()
}

// Optional debugging name
let named = go(name: "worker-1") worker(1, channel)
```

**`move` lives in argument position**: cross-coroutine ownership transfer goes through the argument prefix `move`, **not** through a `go` option:

```xray @id=coro-move-argument
shared let data = { value: 10 }
let task = go fn(d: Json) -> int {
    return d.value + 1
}(move data)        // transfer data ownership to the coroutine; data is unusable afterwards
```

**Block-form restriction**: `go { ... }` is an implicit zero-argument lambda. It has no parameter list and does not bypass concurrency capture rules. The block may not capture ordinary mutable locals; external state used directly inside the block must be `shared const`, immutable global state, or an explicitly concurrency-safe object such as a Channel or Atomic. To pass local data into a coroutine, use the lambda-call or function-call form:

```xray
let n = 10
let task = go fn(x: int) -> int {
    return x + 1
}(n)
```

**Semantics**:
- Every `go` expression returns a `Task<T>`, where `T` is the callee's return type; functions returning `()` correspond to `Task<null>`.
- Coroutines are scheduled on idle worker threads (M:N).
- `go(name: ...)` only sets the debugging name and does not affect scheduling order.
- Uncaught exceptions are stored in the `Task` and rethrown when `await` is called.
- Plain locals (not `shared`, not `move`d) passed to `go` are **deep-copied automatically**; `shared const` is shared zero-copy; `shared let` must be `move`d.
- The `go { ... }` block form is equivalent to a zero-argument lambda and may use only external state that satisfies the coroutine capture rules; pass data with `go fn(x: T) -> R { ... }(arg)` or `go worker(arg)`.

### 10.3 `await` — wait for a result

```ebnf
AwaitExpr ::= 'await' Expression
           |  'await' 'all' Expression       // wait for all to complete
           |  'await' 'any' Expression       // wait for any one to complete
```

```xray @id=coro-await-forms
// single task
let task = go fetch("https://example.com")
let result = await task                    // yields the current coroutine until task completes

// await all: wait for all, returns the result array (in input order)
let t1 = go compute(2)
let t2 = go compute(3)
let t3 = go compute(4)
let results: Array<int> = await all [t1, t2, t3]
// also works on a variable directly, no brackets needed
let tasks = [t1, t2, t3]
let results2: Array<int> = await all tasks

// await any: wait for the first to complete, return its result; the others keep running
let first = await any [t1, t2, t3]

// await anySuccess: skip failing tasks; wait for the first successful one
let firstOk = await anySuccess [t1, t2, t3]
```

**Semantics**:

- `await` only applies to `Task<T>`; other types are a compile error.
- The current coroutine **yields** until the target completes (without blocking the OS thread).
- Exception propagation:
  - `await t` rethrows the exception thrown by `t`.
  - On success, `await t` returns `T`; if `T` is nullable, a returned `null` is the task's real result, not a cancellation or failure marker.
  - `await all` throws if any task throws (the others are cancelled).
  - `await any` throws only when **every** task fails; if any one completes, its result is returned.
  - `await anySuccess` is similar to `await any` but **skips** throwing tasks, awaiting only the first successful one.
- `all` / `any` / `anySuccess` are **contextual keywords** after `await`; they apply only in this position.
- The input to `await all` must be homogeneous: every element must have the same static `Task<T>` type, and the result type is `Array<T>`. Heterogeneous tasks such as mixed `Task<int>` and `Task<string>` are not automatically erased or boxed; await them individually, or convert inside each task to a shared enum / union / Json result type.

### 10.4 `Task<T>` handle

`go expr` returns `Task<T>`, where `T` is the callee's return type. Task handles support:

| Method / property | Type | Description |
|--|--|--|
| `t.done` | `bool` (property) | Whether the task has completed (success, failure, or cancellation) |
| `t.status` | `TaskStatus` (property) | `Pending` / `Running` / `Success` / `Failed` / `Cancelled` |
| `t.cancel()` | `() -> ()` | Request cooperative cancellation |
| `t.poll()` | `() -> TaskResult<T>` | Non-blocking observation; returns `TaskResult.Pending` while incomplete |
| `t.awaitResult()` | `() -> TaskResult<T>` | Waits and returns a status result without rethrowing |
| `t.awaitTimeout(ms)` | `(int) -> TaskResult<T>` | Waits until completion or timeout; timeout returns `TaskResult.Timeout` |

```xray @id=coro-task-handle
let t = go fetch(url)
if (!t.done) { /* still running */ }
let r = await t

match t.poll() {
    TaskResult.Pending -> print("running")
    TaskResult.Success(value) -> print(value)
    TaskResult.Failed(err) -> print(err)
    TaskResult.Cancelled -> print("cancelled")
    TaskResult.Timeout -> print("timeout")
}
```

**Cancellation semantics**: `cancel()` sets the cancellation flag; the coroutine throws a cancellation exception at the next safepoint (GC checkpoint, channel operation, `await`, `yield`). Plain `await` on a cancelled task throws `TaskCancelled`; use `awaitResult()` or `awaitTimeout(ms)` when you want a status value.

**Watchdog forced cancellation**: the runtime monitor thread (sysmon) force-cancels coroutines that run **too long without crossing a safepoint**—a coroutine whose heartbeat stays frozen while RUNNING beyond a threshold (default 5 seconds) is marked for cancellation. The threshold is configurable via the `XRAY_SYSMON_CANCEL_MS` environment variable (milliseconds); setting it to `0` **disables** forced cancellation (only a one-time warning around ~100ms remains). A pure-CPU tight loop that may run long should insert `yield` inside the loop to provide a safepoint and avoid spurious watchdog cancellation.

### 10.5 Channel

```ebnf
ChannelType ::= 'Channel' '<' Type '>'
ChannelNew  ::= 'new' 'Channel' ('<' Type '>')? '(' Expression ')'
```

Channels are usually declared as `shared const` (cross-coroutine lifetime, reference semantics):

```xray @id=channel-decl-variants
shared const ch  = new Channel<int>(10)    // buffered, capacity = 10
shared const ch0 = new Channel<int>(0)     // unbuffered (synchronous handshake)
shared const cha = new Channel(3)          // element type inferred from the first send
```

**API** (note that all method names are **camelCase**):

| Method | Signature | Behaviour |
|--|--|--|
| `send(v)` | `(T) -> ()` | Blocking send; waits for a consumer when full; throws if the channel is closed |
| `recv()` | `() -> Recv<T>` | Blocking receive; returns `Recv.Closed` when closed and drained |
| `trySend(v)` | `(T) -> SendResult` | Non-blocking send; returns `Sent` / `Full` / `Closed` |
| `tryRecv()` | `() -> Recv<T>` | Non-blocking receive; returns `Recv.Empty` when empty |
| `sendTimeout(v, ms)` | `(T, int) -> SendResult` | Send with timeout; timeout returns `SendResult.Timeout` |
| `recvTimeout(ms)` | `(int) -> Recv<T>` | Receive with timeout; timeout returns `Recv.Timeout` |
| `close()` | `() -> ()` | Close the channel; idempotent |
| `isClosed` | `bool` (property) | Whether the channel is closed |

```xray @id=channel-basic-ops
shared const ch = new Channel<int>(10)
ch.send(42)                             // blocking send
let v = match ch.recv() {
    Recv.Value(value) -> value
    Recv.Closed -> -1
    _ -> -1
}

let sent = ch.trySend(99)               // SendResult.Sent / Full / Closed
match ch.tryRecv() {
    Recv.Value(next) -> print(next)
    Recv.Empty -> print("empty")
    Recv.Closed -> print("closed")
    Recv.Timeout -> print("timeout")
}

ch.close()
```

**send/recv with `move`**: when sending a large object, use `ch.send(move payload)` to transfer ownership and avoid copying; the receiver becomes the sole owner.

In type position, a channel is written as `Channel<T>` and may be used in function parameters, fields, and return types:

```xray @id=channel-param-type
fn producer(ch: Channel<int>) {
    ch.send(42)
}
```

**Semantics**:
- **MPMC** (multi-producer, multi-consumer).
- Buffered channel: senders suspend when full; receivers suspend when empty.
- Unbuffered channel: send and receive must rendezvous (synchronous handshake).
- After close: `send` throws; `recv` returns remaining buffered values as `Recv.Value(v)`, then `Recv.Closed`; `tryRecv` returns `Recv.Empty` when empty and not closed.

### 10.6 `select`

`select` multiplexes multiple channel operations. The non-blocking default branch uses `_`.

```ebnf
SelectStmt ::= 'select' '{' SelectArm+ '}'
SelectArm  ::= RecvArm | SendArm | TimeoutArm | DefaultArm
RecvArm    ::= Identifier 'from' Expression '->' Block
SendArm    ::= Expression 'to' Expression '->' Block
TimeoutArm ::= 'after' Expression '->' Block
DefaultArm ::= '_' '->' Block
```

```xray @id=coro-select
shared const ch1 = new Channel<int>(2)
shared const ch2 = new Channel<int>(2)

select {
    msg from ch1 -> { print("got from ch1:", msg) }      // receive arm
    msg from ch2 -> { print("got from ch2:", msg) }      // receive arm
    100  to   ch1 -> { print("sent 100 to ch1") }        // send arm
    _ -> { print("no channel ready") }                   // default arm (non-blocking)
}
```

**Semantics**:
- Receive arm `name from ch -> body`: selected when ch has data, and binds the `Recv.Value(name)` payload to `name`.
- Send arm `value to ch -> body`: equivalent to `ch.send(value)`, but selected only when `ch` has capacity.
- Default arm `_ -> body`: runs immediately when no arm is ready; **omitting the default arm** makes `select` block until an arm becomes ready.
- When multiple arms are ready at the same time, one is selected **randomly** (matching Go).

### 10.7 `scope` (structured concurrency / lexical scope)

`scope` is a **statement keyword** that introduces a new lexical block. It serves two purposes:

1. **Pure lexical scope**: identical to a C/Rust `{ ... }` local block; `let` inside the block does not affect outer same-named variables.
2. **Structured concurrency** (semantic enhancement): coroutines started via `go` inside the block are **awaited automatically** before the block exits.

```ebnf
ScopeStmt          ::= 'scope' Block
LinkedScopeStmt    ::= 'linked' 'scope' Block          // sibling failure → cancel all + rethrow
SupervisorScopeExpr ::= 'supervisor' 'scope' Block     // collect all errors, return Array<string>
```

```xray @id=coro-scope
// lexical scope use
let x = 1
scope {
    let x = 10            // shadow the outer x; in effect inside the block
    print(x)              // 10
}
print(x)                  // 1

// structured concurrency use (with go)
scope {
    go worker_a()
    go worker_b()
    // before the block exits, both a/b are awaited; an exception in either does not affect siblings
}
```

**Three scope variants**:

| Form | Behaviour when a child coroutine throws | Return value |
|---|---|---|
| `scope { ... }` | Siblings are not cancelled; exceptions do not propagate outward (each task is independent) | none (statement form) |
| `linked scope { ... }` | **Cancels all siblings** and **rethrows** the first exception outward | none |
| `supervisor scope { ... }` | **Collects** failure messages from every failing child; siblings do not affect each other | `Array<string>` (error list; empty means all succeeded) |

```xray @id=coro-linked-supervisor-scope
// linked scope: failure propagation
try {
    linked scope {
        go ok_worker()
        go failing_worker()         // throws
    }
} catch (e) {
    print("caught:", e)              // hits this branch
}

// supervisor scope: collect errors
let errors = supervisor scope {
    go failing("error1")
    go failing("error2")
    go ok()
}
print(errors.length)                 // 2 (only the failures are counted)
```

**General semantics**:
- `scope` is not a function call and does not require an import; it is a keyword block statement.
- All three forms await every coroutine started by `go` inside the block before exiting.

### 10.8 `move` — cross-coroutine ownership transfer

```ebnf
MoveExpr ::= 'move' Identifier        // only at call-argument position
```

`move` is an **argument-prefix modifier** (not a `go` option). It transfers ownership of a `shared let` variable from the current scope to the callee (including coroutines started by `go`, `ch.send()`, etc.). After `move`, the variable is statically marked as **moved**, and any subsequent reference is a compile error.

```xray @id=coro-move-transfer
shared let buf = new Bytes(1024 * 1024)

// hand off to a coroutine
let t = go fn(b: Bytes) -> int {
    return process(b)
}(move buf)
// compile error: buf has been moved
// print(buf.length)

// hand off to a channel
shared const ch = new Channel<Bytes>(1)
shared let payload = new Bytes(4096)
ch.send(move payload)
// compile error: payload has been moved
```

See §7.3 and §7.4 for the capture rules of shared variables.

### 10.9 Synchronisation primitives

xray's default concurrency model favours **message passing + immutable sharing**—`shared const`, `Channel`, `move`, and `scope` already eliminate most data races at compile time, so raw mutexes/locks are **discouraged**.

When mutual exclusion or atomic operations are unavoidable, the runtime provides:

| Primitive | Form | Description |
|---|---|---|
| Channel(1) | A single-element channel | The recommended mutex pattern (simulate lock/unlock via send/recv) |
| `shared let` + `move` | Compile-time exclusivity | Cross-coroutine exclusivity with no runtime overhead |
| `Atomic<T>` | Lock-free atomic wrapper | C11 atomic operations for `int`/`float`/`bool` |

> **Design note**: xray does not expose generic lock primitives such as `Mutex`/`RwLock`. For simple shared counters and flags, `Atomic<T>` is the recommended choice; for complex mutual exclusion, use `Channel(1)` to simulate lock/unlock.

#### `Atomic<T>` — lock-free atomic type

`Atomic<T>` wraps `int`, `float`, or `bool`, allocated on the system heap, using C11 atomic instructions for lock-free cross-coroutine reads and writes.

**Declaration constraint**: `Atomic<T>` variables must be declared as `shared const`; `move` is prohibited.

```xray
shared const counter = Atomic(0)         // Atomic<int>
shared const flag = Atomic(false)        // Atomic<bool>
shared const rate = Atomic(3.14)         // Atomic<float>
```

**Method overview** (full signatures in §14.19):

| Method | Description |
|---|---|
| `load(ord?)` | Atomic read |
| `store(val, ord?)` | Atomic write |
| `add(val, ord?)` / `sub(val, ord?)` | Atomic add/subtract (int/float) |
| `fetchAdd(val, ord?)` / `fetchSub(val, ord?)` | Atomic add/subtract returning old value |
| `swap(val, ord?)` | Atomic swap, returns old value |
| `compareExchange(expected, desired, ord?)` | CAS, returns `(old, bool)` |
| `toggle(ord?)` | Atomic negate (bool), returns old value |
| `toString()` | Returns string representation of current value |

#### `Ordering` enum

All methods accepting an `ord?` parameter take an `Ordering` enum to specify memory ordering. Default is `SeqCst` (strongest guarantee).

```xray
enum Ordering {
    Relaxed = 0,          // No cross-thread ordering guarantee
    Acquire = 1,          // Read barrier
    Release = 2,          // Write barrier
    AcquireRelease = 3,   // Read-write barrier
    SeqCst = 4,           // Sequential consistency (default)
}
```

The `Ordering` enum is automatically injected by the compiler (prelude); no import is needed.

```xray
shared const counter = Atomic(0)
counter.store(42, Ordering.Release)
let val = counter.load(Ordering.Acquire)
```


### 10.10 `yield` — yield the CPU

```ebnf
YieldStmt ::= 'yield'
```

```xray @id=coro-yield-loop
for (i in 0..1000) {
    do_chunk(i)
    yield                       // explicit safepoint, lets other coroutines run
}
```

**Current implementation**: usable as a statement, equivalent to Go's `runtime.Gosched()`; valued `yield` is not supported.

### 10.11 Concurrency safety model

xray uses the type system to **eliminate most data races at compile time**:

| Rule | Enforced |
|--|--|
| `go` closures cannot capture ordinary `let` locals | ✅ |
| `shared const` is read-only and zero-copy across coroutines | ✅ |
| `shared let` must be `move`d to cross a coroutine boundary | ✅ |
| Channels for cross-coroutine values | ✅ |
| `Atomic<T>` must be declared as `shared const`; `move` prohibited | ✅ |

**Residual data-race risk** (detected at runtime, not compile time):
- Sending a mutable class reference via a channel (the receiver and sender may mutate concurrently)—prefer to send `shared const` / `Bytes` / immutable objects, or transfer ownership via `move`.
<!-- /xr-spec:en -->

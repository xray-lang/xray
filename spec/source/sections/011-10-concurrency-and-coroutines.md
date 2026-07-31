---
id: spec.10_concurrency_and_coroutines
order: 011
---

<!-- xr-spec:cn -->
---

## 10. 并发与协程 (Concurrency)

> 真值源：`src/coro/xcoro*.c`、`src/coro/xtask*.c`、`src/coro/xchannel.c`、`src/coro/xscope*.c`、`src/frontend/analyzer/xanalyzer_escape.c` 与 `docs/rules/design-principles.md`。

xray 的并发是**协程 (goroutine 风格) + Channel + 强静态约束**。设计目标：写 `go { ... }` 就和写普通函数一样简单，但**编译期保证不发生数据竞争**。

### 10.1 协程模型

| 维度 | 选择 |
|--|--|
| 调度模型 | M:N（用户态协程 + 多 OS 线程） |
| 调度策略 | 协作式（后向跳转、Channel、`await`、`Coro.yield()` 等调度/挂起点）+ work-stealing |
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
var t1 = go worker(0, channel)

// 形式 2：调用一个 lambda 字面量（用于内联逻辑 + 显式传参）
var t2 = go fn(d: Json) -> int {
    return d.value * 2
}(payload)

// 形式 3：块形式（隐式包装为零参 lambda）
var t3 = go {
    return compute()
}

// 可选调试名称
var named = go(name: "worker-1") worker(1, channel)
```

**move 在源表达式位置**：跨协程转移推断为唯一的局部根通过 `move` 实现，**不是** `go` 的选项；`const` 能力不能作为可变 owner 的 `move` 源：

```xray @id=coro-move-argument
var data = { value: 10 }
var task = go fn(d: Json) -> int {
    return d.value + 1
}(move data)        // 把 data 的所有权移交给协程；之后 data 不可访问
```

**块形式限制**：`go { ... }` 是隐式零参 lambda，没有参数列表，也不会绕过统一 capture plan。它不得捕获任何外层 `var`，即使只读也不允许；已发布的 `const` 值和受验证同步句柄可以按能力捕获。需要跨界复制或转移局部数据时，使用带参数的 lambda / 函数调用形式并显式写出 `copy(...)` / `move`：

```xray
var n = 10
var task = go fn(x: int) -> int {
    return x + 1
}(n)
```

**语义**：
- 每个 `go` 表达式都返回一个 `Task<T>`，其中 `T` 是被调函数的返回类型；返回 `()` 的函数对应 `Task<null>`。
- 协程在闲置 worker 线程中调度（M:N）。
- `go(name: ...)` 只设置调试名称，不影响调度顺序。
- 协程内**未捕获**异常存在 `Task` 中，由 `await` 时重抛。
- 跨协程传递 execution-local heap 值（`Array` / `Map` / `Set` / `Json` / `Array<byte>` / `StringBuilder` 等）必须显式 `copy(x)` 或 `move x`，**裸传是编译错误**；标量、`string`、已发布 const 值和受审计的 Channel / Task / Atomic 等可直接传。`move` 只适用于 verifier 证明为唯一、无存活 alias/loan 的可重绑局部 `var` 根。`go` 实参与 `ch.send`、`select` 发送分支共用同一 transfer plan，每次边界传递都能从源码看出复制、转移或能力共享语义。
- `go { ... }` 块形式等价于零参 lambda，只能使用符合协程捕获规则的外部状态；传参请用 `go fn(x: T) -> R { ... }(arg)` 或 `go worker(arg)`。
- 普通外层 `var` 禁止被 `go` 闭包捕获，读和写都一样；这条规则不依赖协程数量或调度时序。多个协程的共享可变状态必须通过 `Channel`、`Atomic` 或 `sync` 的受审计句柄传递，直接捕获修改时报编译错误。

### 10.3 `await` — 等待结果

```ebnf
AwaitExpr ::= 'await' Expression
           |  'await' 'all' Expression       // 等待全部完成
           |  'await' 'any' Expression       // 等待任一完成
```

```xray @id=coro-await-forms
// 单 task
var task = go fetch("https://example.com")
var result = await task                    // 让出当前协程直到 task 完成

// await all：等待全部完成，返回结果数组（与输入顺序一致）
var t1 = go compute(2)
var t2 = go compute(3)
var t3 = go compute(4)
var results: Array<int> = await all [t1, t2, t3]
// 也可直接对变量使用，无需中括号
var tasks = [t1, t2, t3]
var results2: Array<int> = await all tasks

// await any：等待任一完成，返回该任务结果；其他任务继续运行
var first = await any [t1, t2, t3]

// await anySuccess：跳过失败任务，等待第一个成功的任务
var firstOk = await anySuccess [t1, t2, t3]
```

**语义**：

- `await` 仅作用于 `Task<T>` 类型；其他类型为编译错误。
- 用户统一写 `await task`，不写 `await (move task)`。若 `T` 是 unique mutable result，编译器把该 await 证明为 Task 的单次 terminal take；第二次 await 或随后再次使用该 single-owner task 是编译错误。若 `T` 是 const、同步共享或 inline-copy 结果，则按对应能力观察，不要求用户记另一套 await 语法。
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
var t = go fetch(url)
if (!t.done) { /* 还在跑 */ }
var r = await t

match (t.poll()) {
    TaskResult.Pending -> print("running")
    TaskResult.Success(value) -> print(value)
    TaskResult.Failed(err) -> print(err)
    TaskResult.Cancelled -> print("cancelled")
    TaskResult.Timeout -> print("timeout")
}
```

`TaskResult<T>` 的当前公开形状为 `Success(T)`、`Failed(PanicInfo)`、`Cancelled`、`Timeout`、`Pending`。运行时故障通过 `PanicInfo` 进入 `Failed`；业务 enum 错误仍走语言的错误通道，plain `await task` 会按对应错误路径传播，调用方需要状态值时使用显式 task handle 与 `awaitResult()` / `awaitTimeout(ms)`。

**取消语义**：`cancel()` 设置取消标志；协程在下一个调度/挂起检查点（后向跳转、Channel 操作、`await`、`Coro.yield()`）检测到标志后抛出取消异常。plain `await` 已取消的 task 会抛 `TaskCancelled`；需要状态值时使用 `awaitResult()` 或 `awaitTimeout(ms)`。

**看门狗策略**：运行时监控线程（sysmon）会观察 RUNNING 协程的心跳。纯 Xray 循环在后向跳转 safepoint 推进心跳，因此会被观测为持续进展；sysmon 主要用于发现长时间 native/FFI 或无 safepoint 区域卡住。如果心跳长时间冻结，默认行为是 **warn-only**：约 100ms 后打印一次 stuck warning，但不静默取消协程。强制取消是显式 opt-in：设置环境变量 `XRAY_SYSMON_CANCEL_MS=N`（`N > 0`，单位毫秒）后，心跳冻结超过该阈值的协程会被标记取消；未设置或设为 `0` 时保持仅告警。纯 CPU 长循环可在循环内插入 `Coro.yield()`，以改善调度公平性和取消响应性。

### 10.5 Channel

```ebnf
ChannelType ::= 'Channel' '<' Type '>'
ChannelNew  ::= 'Channel' ('<' Type '>')? '(' Expression ')'
```

Channel 以稳定的 `const` 绑定命名；其类型来自受审计的同步能力 registry，因此 `send`/`recv` 可修改受保护的内部状态：

```xray @id=channel-decl-variants
const ch  = Channel<int>(10)    // 有缓冲，capacity = 10
const ch0 = Channel<int>(0)     // 无缓冲（同步握手）
const cha = Channel(3)          // 元素类型从首次 send 推断
```

**API**（注意全部为 **camelCase**）：

| 方法 | 签名 | 行为 |
|--|--|--|
| `send(v)` | `(T) -> ()` | 阻塞发送；满则等待消费者；channel 已关闭时抛异常 |
| `recv()` | `() -> Recv<T>` | 阻塞接收；关闭且缓冲为空时返回 `Recv.Closed` |
| `recvOr(default)` | `(T) -> T` | 阻塞接收；收到值时直接返回 `T`，关闭且缓冲为空时返回 `default`，不分配 `Recv<T>` 包装 |
| `trySend(v)` | `(T) -> SendResult` | 非阻塞发送；返回 `Sent` / `Full` / `Closed` |
| `tryRecv()` | `() -> Recv<T>` | 非阻塞接收；空时返回 `Recv.Empty` |
| `sendTimeout(v, ms)` | `(T, int) -> SendResult` | 带超时发送；超时返回 `SendResult.Timeout` |
| `recvTimeout(ms)` | `(int) -> Recv<T>` | 带超时接收；超时返回 `Recv.Timeout` |
| `close()` | `() -> ()` | 关闭 channel；幂等 |
| `isClosed` | `bool`（属性） | channel 是否已关闭 |

```xray @id=channel-basic-ops
const ch = Channel<int>(10)
ch.send(42)                             // 阻塞发送
var v = match (ch.recv()) {
    Recv.Value(value) -> value
    Recv.Closed -> -1
    _ -> -1
}

var sent = ch.trySend(99)               // SendResult.Sent / Full / Closed
match (ch.tryRecv()) {
    Recv.Value(next) -> print(next)
    Recv.Empty -> print("empty")
    Recv.Closed -> print("closed")
    Recv.Timeout -> print("timeout")
}

ch.send(7)
ch.close()
for (msg in ch) {
    print(msg)
}

var value = ch.recvOr(-1)
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
- 关闭后：`send` 抛异常；`recv` 返回剩余 buffered value 的 `Recv.Value(v)`，取完后返回 `Recv.Closed`；`recvOr(default)` 返回剩余 buffered value，取完后返回 `default`；`tryRecv` 在空且未关闭时返回 `Recv.Empty`。
- `for (msg in ch)` 等价于阻塞接收直到 channel 关闭且 drained；循环变量类型为 `T`。Channel 不支持 key-value 迭代。

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
const ch1 = Channel<int>(2)
const ch2 = Channel<int>(2)

select {
    msg from ch1 -> { print("got from ch1:", msg) }      // 接收分支
    msg from ch2 -> { print("got from ch2:", msg) }      // 接收分支
    100  to   ch1 -> { print("sent 100 to ch1") }        // 发送分支
    _ -> { print("no channel ready") }                   // 默认分支（非阻塞）
}
```

**语义**：
- 接收分支 `name from ch -> body`：在 ch 有数据时被选中，并把 `Recv.Value(name)` 的 payload 绑定到 `name`。
- 发送分支 `value to ch -> body`：等价于 `ch.send(value)`，但仅在 ch 有空间时被选中；`value` 与 `ch.send` 遵守同一 transfer plan——execution-local heap 值必须显式写成 `copy(v)` 或 `move v`。
- 默认分支 `_ -> body`：当前无任何分支就绪时立即执行；**省略默认分支**会让 select 阻塞直到某个分支就绪。
- 多个分支同时就绪时**随机**选择一个（与 Go 一致）。

### 10.7 `scope` 块（结构化并发 / 词法作用域）

`scope` 是**语句关键字**，建立一个新的词法作用域块。它服务两个目的：

1. **纯词法作用域**：与 C/Rust `{ ... }` 局部块一致，块内 `var` 不影响外层同名变量。
2. **结构化并发**（语义增强）：在 `scope` 块内 `go` 启动的协程，块退出前**自动等待**全部完成或取消。

```ebnf
ScopeStmt           ::= 'scope' Block
LinkedScopeStmt     ::= 'linked' 'scope' Block          // 兄弟失败 -> 取消所有 + 重抛
SupervisorScopeStmt ::= 'supervisor' 'scope' Block      // 等待所有子协程；语句形式
```

```xray @id=coro-scope
// 词法作用域用途
var x = 1
scope {
    var x = 10            // shadow 外层 x，块内有效
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
| `supervisor scope { ... }` | 等待所有子协程完成；子协程之间互不影响 | 无（语句形式） |

`supervisor scope` 会等待块内所有 `go` 子协程完成，但不返回聚合结果。需要观察某个子任务状态时，显式保留 task handle 并调用 `awaitResult()` 或 `awaitTimeout(ms)`；把 `supervisor scope` 当表达式使用会被编译器拒绝。

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

// supervisor scope：保留 task handle，块退出后逐个观察 outcome
var first: Task<int>?
var second: Task<int>?
var third: Task<int>?
supervisor scope {
    first = go failing("error1")
    second = go failing("error2")
    third = go ok()
}
var outcomes = [first!.awaitResult(), second!.awaitResult(), third!.awaitResult()]
print(len(outcomes))                 // 3（每个子协程一个 outcome）
```

**通用语义**：
- `scope` 不是函数调用，也不需要 import；是关键字块语句。
- 三种形式都在块退出前等待所有 `go` 启动的子协程完成。

### 10.8 `move` — 跨协程所有权转移

```ebnf
MoveExpr ::= 'move' Identifier
```

`move` 是消费源动作（不是 `go` 的选项），可用于初始化、赋值、返回和调用实参。它要求可重新绑定的局部 `var` 根经 verifier 证明为唯一且没有存活 alias/loan。move 后原变量在编译期被标记为**已 moved**，再次引用是编译错误；非法 move 不会污染源状态。`const` 能力不能作为可变 owner 的 `move` 源。

```xray @id=coro-move-transfer
var buf = Array<byte>(1024 * 1024)

// 移交给协程
var t = go fn(b: Array<byte>) -> int {
    return process(b)
}(move buf)
// 编译错误：buf has been moved
// print(len(buf))

// 移交给 channel
const ch = Channel<Array<byte>>(1)
var payload = Array<byte>(4096)
ch.send(move payload)
// 编译错误：payload has been moved
```

详见 §7.3、§7.4 关于 `var` / `const` 能力的协程传递规则。

### 10.9 同步原语

xray 的默认并发模型偏向**消息传递 + 验证后能力共享 + 显式所有权转移**——通过 `const`/同步能力、`Channel`、`move`、`scope` 把跨协程数据边界写在源码里，因此**不**鼓励使用裸 Mutex/锁。

如确需互斥锁/原子操作，运行时层面提供：

| 原语 | 形态 | 说明 |
|---|---|---|
| Channel(1) | 单元素 channel | 互斥的最佳实践（通过 send/recv 模拟 lock/unlock） |
| `const`/同步能力 | 稳定只读值或同步身份 | 普通图深只读；受审计同步句柄只允许其能力方法修改内部状态 |
| `Atomic<T>` | 无锁原子包装 | 对 `int`/`float`/`bool` 提供 C11 原子操作 |
| `sync.Mutex<T>` / `sync.RwLock<T>` | 协程域锁 | 需显式 `import sync`；等待时挂起协程，不阻塞 worker；不得在 `sys.Thread` 线程体中使用 |
| `sys.OsMutex` / `sys.OsRwLock` / `sys.OsCondvar` 等 | OS 线程域锁 | 需显式 `import sys`；阻塞当前 OS 线程，适合 `sys.Thread`、运行时组件和短临界区 |

> **设计说明**：xray 不在 prelude 暴露裸 `Mutex`/`RwLock`。默认推荐 `Channel`、`const` 同步句柄、`move` 与 `Atomic<T>`；确需锁时必须通过 `sync` 或 `sys` 显式选定执行域，避免把协程挂起锁和 OS 阻塞锁混用。

#### `Atomic<T>` — 无锁原子类型

`Atomic<T>` 包装 `int`、`float` 或 `bool`，在系统堆上分配，底层使用 C11 原子指令，无需锁即可跨协程安全读写。

**声明约束**：`Atomic<T>` 句柄以 `const` 命名；其原子方法来自受审计的同步内部可变能力。

```xray
const counter = Atomic(0)         // Atomic<int>
const flag = Atomic(false)        // Atomic<bool>
const rate = Atomic(3.14)         // Atomic<float>
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
    Relaxed,          // 无跨线程排序保证
    Acquire,          // 读屏障
    Release,          // 写屏障
    AcquireRelease,   // 读写屏障
    SeqCst,           // 顺序一致（默认）
}
```

`Ordering` 枚举由编译器自动注入（prelude），无需 import；底层 intrinsic 读取声明顺序 tag，不依赖用户可见 backing value。

```xray
const counter = Atomic(0)
counter.store(42, Ordering.Release)
var val = counter.load(Ordering.Acquire)
```


### 10.10 `Coro.yield()` — 让出 CPU

```ebnf
CoroYieldCall ::= 'Coro' '.' 'yield' '(' ')'
```

```xray @id=coro-yield-loop
for (i in 0..1000) {
    do_chunk(i)
    Coro.yield()                // 主动 safepoint，让其他协程有机会跑
}
```

`Coro.yield()` 是协作式调度让出点，等价于显式 safepoint，让调度器有机会运行其他协程并响应取消。`yield expr` 已专用于生成器产值；裸 `yield` 被拒绝。

两者共用词根但不是同一种挂起：`Coro.yield()` 让出给调度器（可换 OS 线程、是取消点、沿调用边向调用方传播），`yield expr` 对称转移回驱动方（以上三条都不成立）。逐条对照见 §3.16.1。**生成器体内不得调用 `Coro.yield()`**（`E0385`，见 §3.16.2）。

### 10.11 并发安全模型

xray 通过类型系统**编译期消除大部分数据竞争**：

| 规则 | 强制 |
|--|--|
| 所有跨 execution 边界消费同一个 provenance-based capture plan | ✅ |
| execution-local graph 必须显式 `copy` 或从局部 `var` 执行 `move` | ✅ |
| 模块只读值可保留 module owner；模块可变状态不得直接跨界 | ✅ |
| 已发布 `const` 值与受审计同步句柄可按 verified plan 跨协程传递/捕获 | ✅ |
| `move` 只适用于普通局部 `var` 的显式所有权转移 | ✅ |
| Channel 跨协程传值 | ✅ |
| `Atomic<T>` 以 `const` 命名，只有受审计方法可执行同步内部修改 | ✅ |

**仍可能存在数据竞争**（运行时检测，非编译期）：
- Channel 不会隐式复制可变 class 引用；无法证明唯一转移或 const 发布时编译失败，应显式 `copy` 或 `move`。

### 10.12 逻辑根任务与可达运行时能力

程序语义只有一个逻辑 root task；物理实现由编译器从最终产物的可达 root 集合推导：纯同步入口使用 **ELIDED**，只启动子任务但自身不挂起时使用 **DESCRIPTOR**，入口或其可达调用发生挂起时使用 **RESUMABLE_FRAME**。普通不可挂起函数始终保留普通 ABI，不隐式增加 coroutine context、frame、safepoint 或 current-task 查询。

runtime capability 只从 executable entry、manifest C export 等最终 artifact roots 传播。不可达的 `go` / `await` / Channel helper 不会迫使产物链接 scheduler、timer、netpoll 或 hosted runtime。

Hosted target 按 verified entry plan 选择 NONE / SINGLE / MULTI scheduler。Freestanding target 若可达代码只需要 core，则保持零 coroutine runtime；若需要 task、frame、submit、park/wake、timer、interrupt completion 或 executor pump，target manifest 必须提供版本化 provider ABI 及所需 hooks，缺失能力在生成或链接前硬失败。provider 是 target/build 契约，不引入 `async main`、`static main` 或 freestanding 专用源语言关键字。
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 10. Concurrency and Coroutines

> Source of truth: `src/coro/xcoro*.c`, `src/coro/xtask*.c`, `src/coro/xchannel.c`, `src/coro/xscope*.c`, `src/frontend/analyzer/xanalyzer_escape.c`, and `docs/rules/design-principles.md`.

xray's concurrency model is **goroutine-style coroutines + channels + strong static guarantees**. Design goal: writing `go { ... }` is as simple as writing an ordinary function call, while the **compiler guarantees no data race**.

### 10.1 Coroutine model

| Dimension | Choice |
|--|--|
| Scheduling model | M:N (user-space coroutines on multiple OS threads) |
| Scheduling policy | Cooperative (back edges, channels, `await`, `Coro.yield()`, and similar scheduling/suspension points) + work stealing |
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
var t1 = go worker(0, channel)

// Form 2: call a lambda literal (inline logic + explicit arguments)
var t2 = go fn(d: Json) -> int {
    return d.value * 2
}(payload)

// Form 3: block form (implicitly wrapped as a zero-argument lambda)
var t3 = go {
    return compute()
}

// Optional debugging name
var named = go(name: "worker-1") worker(1, channel)
```

**`move` marks the source expression**: ownership transfer of an inferred-unique local root uses `move`, **not** a `go` option; a `const` capability is not a mutable-owner move source:

```xray @id=coro-move-argument
var data = { value: 10 }
var task = go fn(d: Json) -> int {
    return d.value + 1
}(move data)        // transfer data ownership to the coroutine; data is unusable afterwards
```

**Block-form restriction**: `go { ... }` is an implicit zero-argument lambda. It has no parameter list and does not bypass the unified capture plan. It may not capture any outer `var`, even for reads; published `const` values and verified synchronization handles may be captured according to their capabilities. To copy or transfer local data across the boundary, use the lambda-call or function-call form with explicit `copy(...)` / `move`:

```xray
var n = 10
var task = go fn(x: int) -> int {
    return x + 1
}(n)
```

**Semantics**:
- Every `go` expression returns a `Task<T>`, where `T` is the callee's return type; functions returning `()` correspond to `Task<null>`.
- Coroutines are scheduled on idle worker threads (M:N).
- `go(name: ...)` only sets the debugging name and does not affect scheduling order.
- Uncaught exceptions are stored in the `Task` and rethrown when `await` is called.
- Execution-local heap values (`Array` / `Map` / `Set` / `Json` / `Array<byte>` / `StringBuilder`, etc.) crossing a coroutine boundary must use explicit `copy(x)` or `move x`; **passing them bare is a compile error**. Scalars, `string`, published const values, and audited Channel / Task / Atomic handles pass directly. `move` requires a rebindable local `var` root proven unique with no live alias/loan. `go` arguments share the same transfer plan as `ch.send` and `select` send arms, so every boundary operation visibly states whether data is copied, moved, or capability-shared.
- The `go { ... }` block form is equivalent to a zero-argument lambda and may use only external state that satisfies the coroutine capture rules; pass data with `go fn(x: T) -> R { ... }(arg)` or `go worker(arg)`.
- An ordinary outer `var` may never be captured by a `go` closure, for either reads or writes. This rule does not depend on coroutine count or scheduling. Mutable state shared across coroutines must flow through audited `Channel`, `Atomic`, or `sync` handles; direct captured mutation is a compile error.

### 10.3 `await` — wait for a result

```ebnf
AwaitExpr ::= 'await' Expression
           |  'await' 'all' Expression       // wait for all to complete
           |  'await' 'any' Expression       // wait for any one to complete
```

```xray @id=coro-await-forms
// single task
var task = go fetch("https://example.com")
var result = await task                    // yields the current coroutine until task completes

// await all: wait for all, returns the result array (in input order)
var t1 = go compute(2)
var t2 = go compute(3)
var t3 = go compute(4)
var results: Array<int> = await all [t1, t2, t3]
// also works on a variable directly, no brackets needed
var tasks = [t1, t2, t3]
var results2: Array<int> = await all tasks

// await any: wait for the first to complete, return its result; the others keep running
var first = await any [t1, t2, t3]

// await anySuccess: skip failing tasks; wait for the first successful one
var firstOk = await anySuccess [t1, t2, t3]
```

**Semantics**:

- `await` only applies to `Task<T>`; other types are a compile error.
- Users always write `await task`, never `await (move task)`. If `T` is a unique mutable result, the compiler proves this await as the Task's one terminal take; a second await or later use of that single-owner task is a compile error. Const, synchronized-shared, and inline-copy results are observed according to their capability without a second await syntax.
- The current coroutine **yields** until the target completes (without blocking the OS thread).
- PanicInfo propagation:
  - `await t` rethrows the exception thrown by `t`.
  - On success, `await t` returns `T`; if `T` is nullable, a returned `null` is the task's real result, not a cancellation or failure marker.
  - `await all` throws if any task throws (the others are cancelled).
  - `await any` throws only when **every** task fails; if any one completes, its result is returned.
  - `await anySuccess` is similar to `await any` but **skips** throwing tasks, awaiting only the first successful one.
- `all` / `any` / `anySuccess` are **contextual keywords** after `await`; they apply only in this position.
- The input to `await all` must be homogeneous: every element must have the same static `Task<T>` type, and the result type is `Array<T>`. Heterogeneous tasks such as mixed `Task<int>` and `Task<string>` are not automatically erased or boxed; await them individually, or convert inside each task to a common enum / union / Json result type.

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
var t = go fetch(url)
if (!t.done) { /* still running */ }
var r = await t

match (t.poll()) {
    TaskResult.Pending -> print("running")
    TaskResult.Success(value) -> print(value)
    TaskResult.Failed(err) -> print(err)
    TaskResult.Cancelled -> print("cancelled")
    TaskResult.Timeout -> print("timeout")
}
```

The current public shape of `TaskResult<T>` is `Success(T)`, `Failed(PanicInfo)`, `Cancelled`, `Timeout`, and `Pending`. Runtime faults enter `Failed` as `PanicInfo`; business enum errors still use the language error channel, so plain `await task` propagates through the matching error path, and callers that need a status value keep an explicit task handle and call `awaitResult()` / `awaitTimeout(ms)`.

**Cancellation semantics**: `cancel()` sets the cancellation flag; the coroutine throws a cancellation exception at the next scheduling/suspension point (back edge, channel operation, `await`, `Coro.yield()`). Plain `await` on a cancelled task throws `TaskCancelled`; use `awaitResult()` or `awaitTimeout(ms)` when you want a status value.

**Watchdog policy**: the runtime monitor thread (sysmon) observes the heartbeat of RUNNING coroutines. Pure Xray loops advance the heartbeat at back-edge safepoints, so they are observed as making progress; sysmon is mainly for long native/FFI calls or no-safepoint regions that stop progressing. If a heartbeat stays frozen for too long, the default behavior is **warn-only**: a stuck warning is printed after roughly 100ms, but the coroutine is not silently cancelled. Forced cancellation is explicit opt-in: set `XRAY_SYSMON_CANCEL_MS=N` (`N > 0`, milliseconds) to mark a coroutine for cancellation after its heartbeat remains frozen past that threshold; unset or `0` keeps warn-only behavior. Long pure-CPU loops may insert `Coro.yield()` to improve scheduling fairness and cancellation responsiveness.

### 10.5 Channel

```ebnf
ChannelType ::= 'Channel' '<' Type '>'
ChannelNew  ::= 'Channel' ('<' Type '>')? '(' Expression ')'
```

Channels use stable `const` bindings. Their audited synchronization capability permits `send`/`recv` to mutate protected internal state:

```xray @id=channel-decl-variants
const ch  = Channel<int>(10)    // buffered, capacity = 10
const ch0 = Channel<int>(0)     // unbuffered (synchronous handshake)
const cha = Channel(3)          // element type inferred from the first send
```

**API** (note that all method names are **camelCase**):

| Method | Signature | Behaviour |
|--|--|--|
| `send(v)` | `(T) -> ()` | Blocking send; waits for a consumer when full; throws if the channel is closed |
| `recv()` | `() -> Recv<T>` | Blocking receive; returns `Recv.Closed` when closed and drained |
| `recvOr(default)` | `(T) -> T` | Blocking receive; returns the payload directly, or `default` when closed and drained, without allocating a `Recv<T>` wrapper |
| `trySend(v)` | `(T) -> SendResult` | Non-blocking send; returns `Sent` / `Full` / `Closed` |
| `tryRecv()` | `() -> Recv<T>` | Non-blocking receive; returns `Recv.Empty` when empty |
| `sendTimeout(v, ms)` | `(T, int) -> SendResult` | Send with timeout; timeout returns `SendResult.Timeout` |
| `recvTimeout(ms)` | `(int) -> Recv<T>` | Receive with timeout; timeout returns `Recv.Timeout` |
| `close()` | `() -> ()` | Close the channel; idempotent |
| `isClosed` | `bool` (property) | Whether the channel is closed |

```xray @id=channel-basic-ops
const ch = Channel<int>(10)
ch.send(42)                             // blocking send
var v = match (ch.recv()) {
    Recv.Value(value) -> value
    Recv.Closed -> -1
    _ -> -1
}

var sent = ch.trySend(99)               // SendResult.Sent / Full / Closed
match (ch.tryRecv()) {
    Recv.Value(next) -> print(next)
    Recv.Empty -> print("empty")
    Recv.Closed -> print("closed")
    Recv.Timeout -> print("timeout")
}

ch.send(7)
ch.close()
for (msg in ch) {
    print(msg)
}

var value = ch.recvOr(-1)
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
- After close: `send` throws; `recv` returns remaining buffered values as `Recv.Value(v)`, then `Recv.Closed`; `recvOr(default)` returns remaining buffered values, then `default`; `tryRecv` returns `Recv.Empty` when empty and not closed.
- `for (msg in ch)` is equivalent to blocking receive until the channel is closed and drained; the loop variable has type `T`. Channels do not support key-value iteration.

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
const ch1 = Channel<int>(2)
const ch2 = Channel<int>(2)

select {
    msg from ch1 -> { print("got from ch1:", msg) }      // receive arm
    msg from ch2 -> { print("got from ch2:", msg) }      // receive arm
    100  to   ch1 -> { print("sent 100 to ch1") }        // send arm
    _ -> { print("no channel ready") }                   // default arm (non-blocking)
}
```

**Semantics**:
- Receive arm `name from ch -> body`: selected when ch has data, and binds the `Recv.Value(name)` payload to `name`.
- Send arm `value to ch -> body`: equivalent to `ch.send(value)`, but selected only when `ch` has capacity; `value` follows the same transfer plan as `ch.send` — an execution-local heap value must be written as explicit `copy(v)` or `move v`.
- Default arm `_ -> body`: runs immediately when no arm is ready; **omitting the default arm** makes `select` block until an arm becomes ready.
- When multiple arms are ready at the same time, one is selected **randomly** (matching Go).

### 10.7 `scope` (structured concurrency / lexical scope)

`scope` is a **statement keyword** that introduces a new lexical block. It serves two purposes:

1. **Pure lexical scope**: identical to a C/Rust `{ ... }` local block; `var` inside the block does not affect outer same-named variables.
2. **Structured concurrency** (semantic enhancement): coroutines started via `go` inside the block are **awaited automatically** before the block exits.

```ebnf
ScopeStmt           ::= 'scope' Block
LinkedScopeStmt     ::= 'linked' 'scope' Block          // sibling failure -> cancel all + rethrow
SupervisorScopeStmt ::= 'supervisor' 'scope' Block      // wait for all children; statement form
```

```xray @id=coro-scope
// lexical scope use
var x = 1
scope {
    var x = 10            // shadow the outer x; in effect inside the block
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
| `supervisor scope { ... }` | Waits for every child coroutine to finish; siblings do not affect each other | none (statement form) |

`supervisor scope` waits for all child coroutines started by `go` inside the block, but it does not return an aggregate result. To observe a specific child status, keep an explicit task handle and call `awaitResult()` or `awaitTimeout(ms)`; using `supervisor scope` as an expression is rejected by the compiler.

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

// supervisor scope: keep task handles and inspect each outcome after the block
var first: Task<int>?
var second: Task<int>?
var third: Task<int>?
supervisor scope {
    first = go failing("error1")
    second = go failing("error2")
    third = go ok()
}
var outcomes = [first!.awaitResult(), second!.awaitResult(), third!.awaitResult()]
print(len(outcomes))                 // 3 (one outcome per child)
```

**General semantics**:
- `scope` is not a function call and does not require an import; it is a keyword block statement.
- All three forms await every coroutine started by `go` inside the block before exiting.

### 10.8 `move` — cross-coroutine ownership transfer

```ebnf
MoveExpr ::= 'move' Identifier
```

`move` is a consuming source action (not a `go` option) accepted in initializers, assignments, returns, and call arguments. It requires a rebindable local `var` root proven unique with no live alias/loan. After `move`, the variable is statically marked as **moved**, and any subsequent reference is a compile error; a rejected move does not poison the source state. A `const` capability is not a mutable-owner move source.

```xray @id=coro-move-transfer
var buf = Array<byte>(1024 * 1024)

// hand off to a coroutine
var t = go fn(b: Array<byte>) -> int {
    return process(b)
}(move buf)
// compile error: buf has been moved
// print(len(buf))

// hand off to a channel
const ch = Channel<Array<byte>>(1)
var payload = Array<byte>(4096)
ch.send(move payload)
// compile error: payload has been moved
```

See §7.3 and §7.4 for the coroutine transfer rules of `var` and `const` capabilities.

### 10.9 Synchronisation primitives

xray's default concurrency model favours **message passing + verified capability sharing + explicit ownership transfer**: `const`/synchronization capabilities, `Channel`, `move`, and `scope` make cross-coroutine data boundaries visible in source, so raw mutexes/locks are **discouraged**.

When mutual exclusion or atomic operations are unavoidable, the runtime provides:

| Primitive | Form | Description |
|---|---|---|
| Channel(1) | A single-element channel | The recommended mutex pattern (simulate lock/unlock via send/recv) |
| `const`/synchronized capability | Stable read-only value or synchronized identity | Ordinary graphs are deeply read-only; audited handles expose only capability-approved interior mutation |
| `Atomic<T>` | Lock-free atomic wrapper | C11 atomic operations for `int`/`float`/`bool` |
| `sync.Mutex<T>` / `sync.RwLock<T>` | Coroutine-domain locks | Require explicit `import sync`; wait by suspending a coroutine, not by blocking a worker; not allowed in `sys.Thread` bodies |
| `sys.OsMutex` / `sys.OsRwLock` / `sys.OsCondvar`, etc. | OS-thread-domain locks | Require explicit `import sys`; block the current OS thread, suitable for `sys.Thread`, runtime components, and short critical sections |

> **Design note**: xray does not expose bare `Mutex`/`RwLock` in the prelude. Prefer `Channel`, stable `const` synchronization handles, `move`, and `Atomic<T>` by default. When a lock is required, choose the execution domain explicitly through `sync` or `sys` so coroutine-suspending locks are not confused with OS-thread-blocking locks.

#### `Atomic<T>` — lock-free atomic type

`Atomic<T>` wraps `int`, `float`, or `bool`, allocated on the system heap, using C11 atomic instructions for lock-free cross-coroutine reads and writes.

**Declaration constraint**: name an `Atomic<T>` handle with `const`; its atomic methods come from an audited synchronized interior-mutation capability.

```xray
const counter = Atomic(0)         // Atomic<int>
const flag = Atomic(false)        // Atomic<bool>
const rate = Atomic(3.14)         // Atomic<float>
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
    Relaxed,          // No cross-thread ordering guarantee
    Acquire,          // Read barrier
    Release,          // Write barrier
    AcquireRelease,   // Read-write barrier
    SeqCst,           // Sequential consistency (default)
}
```

The `Ordering` enum is automatically injected by the compiler (prelude); no import is needed. Low-level intrinsics read the declaration-order tag and do not rely on user-visible backing values.

```xray
const counter = Atomic(0)
counter.store(42, Ordering.Release)
var val = counter.load(Ordering.Acquire)
```


### 10.10 `Coro.yield()` — yield the CPU

```ebnf
CoroYieldCall ::= 'Coro' '.' 'yield' '(' ')'
```

```xray @id=coro-yield-loop
for (i in 0..1000) {
    do_chunk(i)
    Coro.yield()                // explicit safepoint, lets other coroutines run
}
```

`Coro.yield()` is a cooperative scheduling point, equivalent to an explicit safepoint where the scheduler can run other coroutines and observe cancellation. `yield expr` is reserved for generator value production; bare `yield` is rejected.

The two share a word root but are not the same suspension: `Coro.yield()` yields to the scheduler (can migrate OS threads, is a cancellation point, propagates to callers along call edges), while `yield expr` transfers symmetrically back to the driver (none of those hold). See §3.16.1 for the point-by-point comparison. **A generator body must not call `Coro.yield()`** (`E0385`, see §3.16.2).

### 10.11 Concurrency safety model

xray uses the type system to **eliminate most data races at compile time**:

| Rule | Enforced |
|--|--|
| Every cross-execution boundary consumes the same provenance-based capture plan | ✅ |
| An execution-local graph requires explicit `copy`, or `move` from a local `var` | ✅ |
| Module-readonly values may retain the module owner; module-mutable state may not cross directly | ✅ |
| Published `const` values and audited synchronization handles may cross through a verified plan | ✅ |
| `move` only applies to explicit ownership transfer of ordinary local `var` values | ✅ |
| Channels for cross-coroutine values | ✅ |
| `Atomic<T>` uses a stable `const` binding and only audited methods mutate internal state | ✅ |

**Residual data-race risk** (detected at runtime, not compile time):
- Channels never copy a mutable class reference implicitly. If uniqueness transfer or const publication cannot be proven, compilation fails; use explicit `copy` or `move`.

### 10.12 Logical root task and reachable runtime capabilities

Program semantics expose one logical root task. Its physical representation is derived from the final artifact's reachable roots: a pure synchronous entry is **ELIDED**; an entry that only spawns children without suspending uses a **DESCRIPTOR**; an entry that suspends directly or transitively uses a **RESUMABLE_FRAME**. Ordinary non-suspendable functions keep the plain ABI, with no implicit coroutine context, frame, safepoint, or current-task lookup.

Runtime capabilities propagate only from final artifact roots such as the executable entry and manifest-selected C exports. An unreachable helper containing `go`, `await`, or Channel operations does not force the artifact to link a scheduler, timer, netpoll, or hosted runtime.

A hosted target selects a NONE / SINGLE / MULTI scheduler from the verified entry plan. A freestanding target remains coroutine-runtime-free when reachable code needs only core. If reachable code needs task/frame allocation, submit, park/wake, timer, interrupt completion, or an executor pump, the target manifest must provide a versioned provider ABI and the required hooks; missing capabilities fail before generation or linking. The provider is a target/build contract and introduces no `async main`, `static main`, or freestanding-only source-language keyword.
<!-- /xr-spec:en -->

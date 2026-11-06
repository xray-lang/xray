---
id: spec.10_concurrency_and_coroutines
order: 011
---

<!-- xr-spec:cn -->
---

## 10. 并发与协程 (Concurrency)

xray 的并发是**协程 (goroutine 风格) + Channel + 强静态约束**。设计目标：让 `go worker(args)` 与普通调用具有同一套显式参数和所有权边界，同时在语言的安全子集内**编译期保证不发生数据竞争**——这条保证的精确形式、它的边界，以及本节各构造建立的同步边，定义在 §16.9。

### 10.1 协程模型

| 维度 | 语言保证 |
|--|--|
| 函数着色 | 无。普通函数不写 `async` / `suspend`；编译器推导每个函数是否可能挂起。普通调用的合法性不依赖这个推导，只有显式要求不挂起的位置（`nosuspend fn` 类型的函数字面量、`defer` 体等）才检查它 |
| 调度 | 协作式：只在挂起点（`await`、Channel、`select`、`Task.yieldNow()`、sleep、IO）切换；不保证抢占，循环后向跳转既不是让出点也不是取消检查点 |
| 并行 | 协程可能在多个 OS 线程上并行运行（M:N），也可能在两个挂起点之间换到另一个线程；程序不得依赖单线程执行顺序 |
| 栈模型 | 无栈：只有可能挂起的函数保存跨挂起点存活的状态，其余函数按普通调用执行；不能跨 C 回调挂起，`CFn` 回调必须是 `nosuspend`（§2.8） |

未加 `nosuspend` 前缀的函数类型默认可能挂起（§2.8）。不含挂起点的长循环不会把 CPU 让给同一线程上的其他协程，也不响应取消；需要时调用 `Task.yieldNow()` 或检查 `Task.isCancelled()`（§10.4、§10.10）。worker 线程数、帧布局和创建开销属于实现，本规范不作承诺。

经接口值或可被覆写的方法发生的调用，按程序中全部可能的实现推导是否挂起：任一实现可能挂起，该调用就可能挂起。因此新增一个会挂起的实现，可能让远处原本合法的 `nosuspend` 上下文报错，诊断会指出是哪个实现。接口要求或类方法带有包级 `no_suspend` 声明承诺（§5.2.11）时，每个实现都必须在自己的定义处证明不挂起，经该要求的调用不会挂起。实现集合不闭合的情形（动态库导出的接口、可替换入口、外部实现）一律按可能挂起处理。

### 10.2 `go` — 启动协程

```ebnf
GoExpr   ::= 'go' GoOptions? CallExpr
GoOptions ::= '(' GoOption (',' GoOption)* ')'
GoOption ::= 'name' ':' StringLiteral
```

`go` 是**表达式**，返回 `Task<T, E>` 句柄。它只接受调用表达式；内联逻辑必须写成被立即调用的 lambda：

```xray @id=coro-go-forms @fragment
// 形式 1：调用一个已声明的函数
var t1 = go worker(0, channel)

// 内联逻辑：lambda 也必须形成完整调用，并显式传参
var t2 = go fn(d: JSON.Object) -> i64 {
    return d.value * 2
}(payload)

// 无参数内联逻辑仍写成零参 lambda 调用；不存在 go { ... } 语法
var t3 = go fn() -> i64 {
    return compute()
}()

// 可选调试名称
var named = go(name: "worker-1") worker(1, channel)
```

**按声明传值**：可复制且 Sendable 的实参形成拥有式逻辑副本，不要求 `copy(...)` 或 backing 唯一。不可复制且 Sendable 的资源仅按消费参数接管；可选 `move resource` 标记同一转移，不是 `go` 的选项。Task 结果必须可复制且 Sendable，资源不成为 Task 结果：

```xray @id=coro-move-argument @check
var data = [10]
var task = go fn(d: Array<i64>) -> i64 {
    return d[0] + 1
}(data)            // 保存逻辑副本，调用方 data 仍可使用
```

`go { ... }` 不是语法。这个单一调用形式保证所有入口都经过同一套参数求值、capture plan 与跨协程 transfer plan；需要内联逻辑时使用 lambda 调用，参数按声明的 READ 或消费模式处理：

```xray
var n = 10
var task = go fn(x: i64) -> i64 {
    return x + 1
}(n)
```

**语义**：
- 每个 `go` 表达式都返回一个 `Task<T, E>`，其中 `T` 是被调函数的返回类型，`E` 是其声明的错误类型，默认 `Never`。返回 Unit 的任务仍保留 Unit 成功值，不把它映射成 `null`。
- 协程由运行时调度，可能与启动它的代码并行运行（§10.1）。
- `go(name: ...)` 只设置调试名称，不影响调度顺序。
- 协程内**未捕获**异常存在 `Task` 中，由 `await` 时重抛。
- 参数、拥有式捕获和结果均按真实类型检查 Sendable；普通泛型在定义处按约束检查，Checked 特化后复验。可复制值按声明保留逻辑副本，复制在 class 与同步身份边界停止；Array 的资格由全部元素决定，const、COW、引用计数或单 worker 不额外授予 Sendable。借用 ref、Slice/MutSlice、Cell 与非 Sendable 值不能跨此边界。不可复制且 Sendable 的资源仅可按消费参数转移，不进入普通可复制捕获或 Task 结果。
- 普通外层 `var` 禁止被 `go` 闭包捕获，读和写都一样；这条规则不依赖协程数量或调度时序。多个协程的共享可变状态必须通过 `Channel`、`Atomic` 或 `sync` 的受审计句柄传递，直接捕获修改时报编译错误。

### 10.3 `await` — 等待结果

```ebnf
AwaitExpr ::= 'await' UnaryExpr
```

`await` 只作用于 `Task<T, E>`，成功时得到 `T`。等待是可能挂起的操作，不阻塞 OS 线程。`E = Never` 时 `await t` 不抛业务错误，不写 `try`；否则传播标记顺序固定为 `try await t`，在 `try { }` 区域内可直接写 `await t`。

```xray @id=coro-await-forms @fragment
var job = go compute(1)            // Task<i64, Never>
var n = await job                  // 不抛业务错误，不写 try

var task = go fetch(url)           // fetch 声明了 throws(FetchErr)
var result = try await task

var tasks = [go fetch(a), go fetch(b), go fetch(c)]
var results = try Task.all(tasks)
var first = try Task.first(tasks)
var firstOk = try Task.firstSuccess(tasks)
var bounded = try Task.timeout(task, 1000)
```

- 等待的业务错误类型就是 Task 的 `E`，无需从任务创建点或函数体反推错误集合。取消不是业务错误，不进入这个类型。
- 子任务的 enum 错误沿值错误通道传播，panic 沿 panic 通道传播。成功的 `null` 是真实 Optional 结果，不表示任务取消或失败。
- Task 结果必须可复制且 Sendable；重复等待产生独立的逻辑值副本，class 与同步身份保持其身份。某次结果复制失败不消费或修改任务的终态。
- **等待已取消的任务**：被等待的任务以 `Cancelled` 结束时，等待方自身在这个 `await` 处进入取消展开（§10.4）：执行 `defer`，不能被 `catch (e)` 或 `catch panic` 捕获，自身也以 `Cancelled` 结束，并按同一规则继续影响等待它的任务。取消因此沿等待链向上传播，不进入任何函数的错误签名。等待方自身已被请求取消时，同样在这个挂起点按 §10.4 展开。
- 需要观察取消而不传播时，使用 `t.awaitResult()` 或 `t.poll()`，它们以 `TaskResult.Cancelled` 值报告（§10.4）。

多个任务的等待使用挂起库函数，前面不再写 `await`：

| 函数 | 返回 | 业务错误类型 | 其余任务 |
|--|--|--|--|
| `Task.all(ts)` | `Array<T>`，与输入顺序一致 | `E` | 失败时先请求取消其余，再传播；不等待清理 |
| `Task.first(ts)` | 最先结束任务的值，或传播它的失败 | `E` | 先请求取消其余，再返回或传播；不等待清理 |
| `Task.firstSuccess(ts)` | 第一个成功值 | 全部失败时 `TaskFailures<E>` | 先请求取消其余，再返回或传播；不等待清理 |
| `Task.timeout(t, ms)` | `T` | `E \| Timeout` | 超时后取消 `t` |

输入任务以 `Cancelled` 结束、且这次取消不是该聚合调用自己请求的，按上面的等待规则使调用方进入取消展开：`Task.all` 在任一输入被取消时如此，`Task.first` 在最先结束的输入是取消时如此，`Task.timeout` 在 `t` 于超时前被取消时如此。被取消的输入如何参与 `Task.firstSuccess` 的结果，随下述混合结果仲裁一并冻结。

`Task.first`、`Task.firstSuccess` 及 `Task.all` 的失败分支，按各自规则选定结果后，先向其余尚未结束的输入任务发出取消请求，再返回或传播所选结果；余任务终结及其 `defer` 完成不是返回的前置条件。`Task.all` 的成功分支仍等全部输入任务成功，按输入顺序返回结果。

余任务保持原有 scope 与执行器归属，由执行器继续持有并驱动至终态。聚合调用不将 scope 成员脱离所属 scope；scope 退出前仍等待全部成员终结和清理完成。因此，调用返回后的代码可以与余任务的取消和清理重叠，而 scope 出口仍是完整退场边界。合作式取消不保证抢占或固定返回时限，整个程序对宿主报告完成仍遵守执行器的收尾规则。当前等待者自己的取消展开规则不变。

已返回的聚合结果不由余任务的后续结局追溯改写；余任务的后续错误、panic 和清理失败仍按原有归属与各自通道处理，不能因聚合调用先返回而丢弃。这里规定返回与清理的时机，不补定下述 `Task.firstSuccess` 的混合结果仲裁，也不改变 `Task.timeout` 的规则。

前三者接受同构的 `Array<Task<T, E>>`，不隐式擦除异构结果或错误类型。类型不同的任务用 scope 加逐个等待。`TaskFailures<E>` 只汇总业务失败任务的调试名称、位置以及错误值 E，不包含 `PanicInfo` 或 `Cancelled`；scope 的位置是实际成员注册序号，库聚合函数的位置是输入位置。具体字段名不由上述语义记法定义。panic 始终沿独立通道传播；`Task.firstSuccess` 的成功与 panic 竞争及全部未成功时的混合选择须在共同执行接口中另行冻结，不能从业务汇总类型反推。

### 10.4 `Task<T, E>` 句柄

`go f(...)` 得到 `Task<T, E>`；`T` 是返回类型，`E` 是声明错误类型，默认 `Never`。句柄 API：

| 方法 | 类型 | 说明 |
|--|--|--|
| `t.cancel()` | `fn()` | 幂等地请求合作式取消 |
| `t.poll()` | `fn() -> TaskResult<T, E>?` | 非阻塞；未完成时返回 `null` |
| `t.awaitResult()` | `fn() -> TaskResult<T, E>` | 挂起等待终态，以值报告而不重抛子任务的错误或 panic |

```xray @id=coro-task-handle @fragment
var t = go fetch(url)
if (t.poll() == null) { print("running") }
var outcome = t.awaitResult()
```

`TaskResult<T, E>` 只有四种终态：`Success(T)`、`Error(E)`、`Panic(PanicInfo)`、`Cancelled`。括号表示载荷类型，不是构造语法或字段名声明；未完成由 `poll()` 的外层 Optional 表达，超时由 `Task.timeout` 的错误表达。`awaitResult()` 返回终态不意味着当前等待者免于取消展开。

跨 Task 发布和重复观察要求 T、E 可复制且 Sendable，PanicInfo 的完整可达图也必须拥有式、可复制且 Sendable；仅把绑定设为只读不满足这个要求。任务保留其终态，每个观察者取得独立逻辑快照；值复制与身份引用各遵守其拥有规则，不能借用已退出的执行器帧。

**取消语义**：只在挂起点检查，包括 `await`、Channel、`select`、`Task.yieldNow()`、sleep 和 IO。被取消的任务执行 `defer` 并展开，`catch (e)` 与 `catch panic` 均不能捕获，也不把这种取消写进任务函数的错误签名。用 `await` 或聚合函数等待它的一方按 §10.3 同样进入取消展开；`awaitResult()` 与 `poll()` 以 `TaskResult.Cancelled` 报告而不传播。取消展开到达逻辑根任务时程序结束：独立程序在 stderr 打印 `[Uncaught Cancellation]` 诊断并以退出码 1 退出（与 §8 的未捕获错误、panic 一致），嵌入宿主只结束本次执行。

循环后向跳转不是取消检查点。长循环可以通过 `Task.isCancelled()` 主动检查并收尾，或插入 `Task.yieldNow()` 提供调度与取消机会。运行时心跳或看门狗的观测不改变这些语言级检查点；取消请求也不保证能抢占不挂起的 native/FFI 调用。

### 10.5 Channel

```ebnf
ChannelType ::= 'Channel' '<' Type '>'
ChannelNew  ::= 'Channel' ('<' Type '>')? '(' Expression ')'
```

Channel 以稳定的 `const` 绑定命名；其类型来自受审计的同步能力 registry，因此 `send`/`recv` 可修改受保护的内部状态：

```xray @id=channel-decl-variants
const ch  = Channel<i64>(10)    // 有缓冲，capacity = 10
const ch0 = Channel<i64>(0)     // 无缓冲（同步握手）
const cha: Channel<i64> = Channel(3)   // 元素类型由类型上下文给出
```

构造时必须能确定元素类型：写 `Channel<T>(n)`，或由类型上下文给出；不从后续 `send` 推断。

**API**（注意全部为 **camelCase**）：

| 方法 | 签名 | 行为 |
|--|--|--|
| `send(v)` | `(T) -> ()` | 阻塞发送；满则等待消费者；channel 已关闭时 panic（`E0702`） |
| `recv()` | `() -> Recv<T>` | 阻塞接收；关闭且缓冲为空时返回 `Recv.Closed` |
| `recvOr(default)` | `(T) -> T` | 阻塞接收；收到值时直接返回 `T`，关闭且缓冲为空时返回 `default`，不分配 `Recv<T>` 包装 |
| `trySend(v)` | `(T) -> SendResult` | 非阻塞发送；返回 `Sent` / `Full` / `Closed` |
| `tryRecv()` | `() -> Recv<T>` | 非阻塞接收；空时返回 `Recv.Empty` |
| `sendTimeout(v, ms)` | `(T, i64) -> SendResult` | 带超时发送；超时返回 `SendResult.Timeout` |
| `recvTimeout(ms)` | `(i64) -> Recv<T>` | 带超时接收；超时返回 `Recv.Timeout` |
| `close()` | `() -> ()` | 关闭 channel；幂等 |
| `isClosed` | `bool`（属性） | channel 是否已关闭 |

**超时语义**：`ms` 必须是 `0..86_400_000` 范围内的 `i64`；越界在 Channel 操作前产生 `NumericRange`。零毫秒只尝试一次队列操作。正毫秒从首次尝试记录单一单调截止时间，反复等待或重新登记不能延长期限；宿主必须提供有效时钟，读数失败、为零或倒退产生 `HostError`，不冒充超时。发送关闭返回 `SendResult.Closed`，接收关闭且排空返回 `Recv.Closed`。receiver、消息和毫秒表达式按源码顺序各求值一次，Unit 消息也保留求值副作用；等待期间复用原值。peer 完成与 timeout/cancel 的首个原子终态决定结果，已提交交付不可撤回。两方法保守视为可能挂起，零毫秒不改变这一静态效果。

```xray @id=channel-basic-ops
const ch = Channel<i64>(10)
ch.send(42)                             // 阻塞发送
var v = match (ch.recv()) {
    Recv.Value { value } -> value
    Recv.Closed -> -1
    _ -> -1
}

var sent = ch.trySend(99)               // SendResult.Sent / Full / Closed
match (ch.tryRecv()) {
    Recv.Value { value: next } -> print(next)
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

**载荷所有权**：`Channel<T>` 要求 copyable、storable、Sendable 的 `T`。发送普通值保持逻辑副本，Array 后续修改相互隔离；class 和同步句柄保持其共享身份，不复制对象身份，也不以 `const` 代替 Sendable 证明。

Channel 在类型位置写作 `Channel<T>`，可用于函数参数、字段和返回类型：

```xray @id=channel-param-type
fn producer(ch: Channel<i64>) {
    ch.send(42)
}
```

**语义**：
- **MPMC**（多生产者多消费者）。
- 有缓冲 ch：满则发送方挂起，空则接收方挂起。
- 无缓冲 ch：发送/接收必须同时握手（rendezvous）。
- 关闭后：`send` 产生 `E0702` panic——向已关闭的 channel 发送是编程错误，与 Go 一致；需要把关闭当作正常结果处理时用 `trySend` 或 `sendTimeout`，它们返回 `SendResult.Closed`。`recv` 返回剩余 buffered value 的 `Recv.Value { value: v }`，取完后返回 `Recv.Closed`；`recvOr(default)` 返回剩余 buffered value，取完后返回 `default`；`tryRecv` 在空且未关闭时返回 `Recv.Empty`。
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
const ch1 = Channel<i64>(2)
const ch2 = Channel<i64>(2)

select {
    msg from ch1 -> { print("got from ch1:", msg) }      // 接收分支
    msg from ch2 -> { print("got from ch2:", msg) }      // 接收分支
    100  to   ch1 -> { print("sent 100 to ch1") }        // 发送分支
    _ -> { print("no channel ready") }                   // 默认分支（非阻塞）
}
```

**语义**：
- 接收分支 `name from ch -> body`：在 ch 有数据时被选中，并把 `Recv.Value { value: name }` 的 payload 绑定到 `name`。
- 发送分支 `value to ch -> body`：等价于 `ch.send(value)`，但仅在 ch 有空间时被选中；向已关闭 channel 的发送分支被选中时与 `send` 相同，产生 `E0702` panic；`value` 与 `ch.send` 遵守同一 transfer plan——execution-local heap 值必须显式写成 `copy(v)` 或 `move v`。
- 默认分支 `_ -> body`：当前无任何分支就绪时立即执行；**省略默认分支**会让 select 阻塞直到某个分支就绪。
- 多个分支同时就绪时**随机**选择一个（与 Go 一致）。

### 10.7 `scope` — 结构化并发

`scope` 只表示结构化并发；普通词法作用域直接写 `{ ... }`。scope 子任务是在该块中、不含嵌套函数正文的 `go` 所启动的任务。块内没有这样的 `go` 时编译报错并提示改用普通块。

```ebnf
ScopeStmt ::= 'scope' ('(' 'isolated' ')')? Block
TryScopeStmt ::= 'try' ScopeStmt
```

两种 scope 都是语句，不返回聚合值；退出前等待所有成员任务终结和清理完成。会传播业务错误时写 `try scope`，或者将 scope 放入真实 `try { }` 的保护区域。`try scope` 只标记 scope 退出处的隐式传播，不建立保护区域；主体内会抛业务错误的调用仍须有 `try` / `try?` / `try!`，或处于外层真实 try 主体的词法保护区域。保护范围不进入嵌套函数或闭包，也不包含该 try 的同级 handler。

| 形式 | 子任务失败（含 panic）时 | 结束时 |
|--|--|--|
| 默认 `scope` | 取消其余兄弟任务 | 有 panic 时传播首个成功发布的 panic；否则传播首个成功发布的主体或子任务业务错误 |
| `scope(isolated)` | 兄弟继续执行 | 有 panic 时沿 panic 通道传播；否则优先直接传播主体业务错误，没有主体业务错误时才以 `TaskFailures<E>` 汇总子业务错误 |

设 B 为主体经过局部 catch 后仍会逃逸的业务错误集合（包括 handler 自己抛出的错误），E 为直接 go 子任务声明错误类型的 union。默认 scope 的完整业务错误类型为 `B | E`；isolated 为 `B | TaskFailures<E>`，其中 E=Never 时不产生子业务汇总通道。B、E 都为 Never 时，两种 scope 均免除 scope 传播标记；可能 panic 不改变该规则。这个空通道规则不为所有类型位置定义 `TaskFailures<Never> = Never`。`try?` / `try!` 可以消去相应业务逃逸，不能捕获 panic。最终完整逃逸集合仍须属于所在函数声明，写 try 不扩大签名。

正常块末仅 join 成员；主体 Error、Panic 或向外 return/break/continue 时，两种 scope 都取消并 drain 已接管成员。isolated 的子失败本身不取消兄弟。待履行的正常控制转移必须拥有其值并保活主体帧和有效借用；只有 drain 后没有更高优先级失败才履行，不能用 return 吞掉子 panic。多个 panic 选择首个成功发布者，不承诺墙钟顺序；其余失败保留原始载荷用于次级诊断并按唯一拥有责任释放。isolated 主体错误直接传播时，子业务错误仅作次级诊断，不隐式包装主体错误。

```xray @id=coro-scope @check
fn worker(n: i64) -> i64 { return n + 1 }
fn main() {
    var x = 1
    { var x = 10; print(x) }
    print(x)
    scope {
        go worker(1)
        go worker(2)
    }
}
main()
```

```xray @id=coro-isolated-scope @check
enum WorkerErr { Failed }
fn worker(ok: bool) throws(WorkerErr) -> i64 {
    if (!ok) { throw WorkerErr.Failed }
    return 1
}
fn main() throws(TaskFailures<WorkerErr>) {
    try scope(isolated) {
        go(name: "first") worker(false)
        go(name: "second") worker(true)
    }
}
try main()
```

scope 自身引起的子任务取消不算失败，独立取消的成员也不被包装成业务错误或汇总项。父任务已在合法挂起检查点进入取消展开时，scope 取消并等待子任务，然后继续不可捕获的父取消；成员失败仅作次级诊断。单纯取消请求不直接改变帧、恢复位置或终态。默认 scope 因成员失败而取消其余成员后，主体若在等待其中某个成员时进入取消展开（§10.3），展开止于 scope 边界：scope 照常取消并 drain 其余成员，再按上表传播首个失败；这次取消不越过 scope 向外传播。scope 的隐式等待不引入取消展开。cleanup 逃逸 panic 始终遵守 §8.2.5 的终止规则。需要结果时保留句柄逐个等待或使用 §10.3 的库函数。scope 外的 `go` 是独立任务。

### 10.8 `move` — 跨协程所有权转移

```ebnf
MoveExpr ::= 'move' Identifier
```

`move` 是消费源动作（不是 `go` 的选项），可用于初始化、赋值、返回和调用实参。

**判定规则见 §2.14**：`move` 要求可重绑定的局部 `var` 根同时满足四条证据轴——绑定存活、根唯一、能力可变、无存活借用。语法上只接受标识符：`move x.field`、`move arr[i]`、`move f()` 都是语法错误。move 后原变量在编译期标记为**已 moved**，再次引用是编译错误；被拒绝的 move 不污染源状态。`const` 能力不能作为可变 owner 的 `move` 源。

跨协程边界的转移读的是同一套判定，因此「协程之间不共享可变图」这条保证依赖 §2.14 的唯一性证据，而不是另一套并发专用规则。

```xray @id=coro-move-transfer
var buf = Array<u8>(1024 * 1024)

// 移交给协程
var t = go fn(b: Array<u8>) -> i64 {
    return process(b)
}(move buf)
// 编译错误：buf has been moved
// print(len(buf))

// 移交给 channel
const ch = Channel<Array<u8>>(1)
var payload = Array<u8>(4096)
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
| `Atomic<T>` | 无锁原子包装 | 对 `i64`/`f64`/`bool` 提供 C11 原子操作 |
| `sync.Mutex<T>` / `sync.RwLock<T>` | 协程域锁 | 需显式 `import sync`；等待时挂起协程，不阻塞 worker；不得在 `sys.Thread` 线程体中使用 |
| `sys.OsMutex` / `sys.OsRwLock` / `sys.OsCondvar` 等 | OS 线程域锁 | 需显式 `import sys`；阻塞当前 OS 线程，适合 `sys.Thread`、运行时组件和短临界区 |

> **设计说明**：xray 不在 prelude 暴露裸 `Mutex`/`RwLock`。默认推荐 `Channel`、`const` 同步句柄、`move` 与 `Atomic<T>`；确需锁时必须通过 `sync` 或 `sys` 显式选定执行域，避免把协程挂起锁和 OS 阻塞锁混用。

#### `Atomic<T>` — 无锁原子类型

`Atomic<T>` 包装 `i64`、`f64` 或 `bool`，在系统堆上分配，底层使用 C11 原子指令，无需锁即可跨协程安全读写。

**声明约束**：`Atomic<T>` 句柄以 `const` 命名；其原子方法来自受审计的同步内部可变能力。

```xray
const counter = Atomic(0)         // Atomic<i64>
const flag = Atomic(false)        // Atomic<bool>
const rate = Atomic(3.14)         // Atomic<f64>
```

**方法一览**（完整签名见 §14.19）：

| 方法 | 说明 |
|---|---|
| `load(ord?)` | 原子读取 |
| `store(val, ord?)` | 原子写入 |
| `add(val, ord?)` / `sub(val, ord?)` | 原子加减（i64/f64） |
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

复制 `Atomic` 句柄共享同一个原子对象；每次构造产生一个独立对象，同一程序的不同实例不共享模块初始化产生的对象。值原子操作保证无锁（lock-free），不保证 wait-free，也不以互斥锁实现。

| 操作 | 合法内存序 |
|---|---|
| load | Relaxed / Acquire / SeqCst |
| store | Relaxed / Release / SeqCst |
| add/sub/fetchAdd/fetchSub/swap/toggle/compareExchange | 五种 Ordering |

省略 ordering 时使用 `SeqCst`。只接受 prelude 的 `Ordering` 值，同名的用户 enum 是不同类型，不能作为内存序。编译期已知的非法内存序（例如 `load(Ordering.Release)`）是编译错误；运行时传入的非法内存序在接收者和全部实参按原顺序求值后、任何原子读写之前终止本次执行，不修改原子状态，`catch` 与 `catch panic` 都不能捕获。

compareExchange 是 strong CAS，公开结果 `(T, bool)` 是真正 Tuple，不可见虚假失败。单个 ordering 决定成功/失败序：

| ordering | 成功 | 失败 |
|---|---|---|
| Relaxed | Relaxed | Relaxed |
| Acquire | Acquire | Acquire |
| Release | Release | Relaxed |
| AcquireRelease | AcquireRelease | Acquire |
| SeqCst | SeqCst | SeqCst |

失败时返回实际观察到的旧值且不写入；成功时返回写入前的旧值。CAS 成功之后才发生的失败或取消不回滚这次写入。

i64 的加减按 §2.3.1 模 2^64 环绕。f64 的构造、load、store、swap 和 compareExchange 保留完整 binary64 位型；compareExchange 在观察比较时先规范化 NaN 再比较位型：正零与负零不同，任意 NaN 符号和载荷按同一正 canonical quiet NaN 匹配。该观察规则不要求普通复制、参数和存储逐次规范化。这不改变普通浮点 `==` 的规则。f64 的加减按 §2.3.2 的舍入规则计算，与其他写入竞争时按最新值重算直到成功。`toString()` 读取一次 `SeqCst` 快照。

目标平台不能为所需宽度提供无锁原子操作时，含 `Atomic` 的程序在构建或加载时被明确拒绝，不退化为加锁实现。

> `Ordering` 只描述**单个原子操作**的内存序，不足以推导程序行为。这些值与 Channel、`go`、`await`、`scope`、`const` 发布等语言级同步边如何共同构成 happens-before，定义在 §16.9；不写 `Ordering` 的普通并发代码同样受 §16.9 约束。

```xray
const counter = Atomic(0)
counter.store(42, Ordering.Release)
var val = counter.load(Ordering.Acquire)
```


### 10.10 `Task.yieldNow()` — 让出 CPU

```ebnf
TaskYieldCall ::= 'Task' '.' 'yieldNow' '(' ')'
```

```xray @id=coro-yield-loop @fragment
for (i in 0..1000) {
    do_chunk(i)
    Task.yieldNow()                // 主动 safepoint，让其他协程有机会跑
}
```

`Task.yieldNow()` 是协作式调度让出点，等价于显式 safepoint，让调度器有机会运行其他协程并响应取消。`yield expr` 已专用于生成器产值；裸 `yield` 被拒绝。

此调用不接收值参数或类型实参，结果为 unit；普通函数不需要 async 标注。词法绑定若遮蔽 `Task`，仍按该绑定解析，不能偷偷调用调度原语。

两者共用词根但不是同一种挂起：`Task.yieldNow()` 让出给调度器（可换 OS 线程、是取消点、沿调用边向调用方传播），`yield expr` 对称转移回驱动方（以上三条都不成立）。逐条对照见 §3.16.1。生成器的挂起与关闭规则见 §3.16.2–§3.16.3。

### 10.11 并发安全模型

xray 用类型系统在编译期消除数据竞争。准确的表述是 §16.9.5 的定理：**不含 `unsafe` 块、且不使用 §16.9.5 所列逃逸口的程序不存在数据竞争，其行为等价于某个顺序一致的交错执行（SC-DRF）**。因此在安全子集内可以按"语句交错"推理，不需要理解 happens-before 细节；本节的规则表就是这条定理的强制手段。逃逸口清单与语言级同步边定义在 §16.9。

下列规则由编译器强制：

| 规则 | 强制 |
|--|--|
| 所有跨 execution 边界消费同一个 provenance-based capture plan | ✅ |
| execution-local graph 必须显式 `copy` 或从局部 `var` 执行 `move` | ✅ |
| 模块只读值可保留 module owner；模块可变状态不得直接跨界 | ✅ |
| 已发布 `const` 值与受审计同步句柄可按 verified plan 跨协程传递/捕获 | ✅ |
| `move` 只适用于普通局部 `var` 的显式所有权转移 | ✅ |
| Channel 跨协程传值 | ✅ |
| `Atomic<T>` 以 `const` 命名，只有受审计方法可执行同步内部修改 | ✅ |

**编译期不覆盖的部分**：
- Channel 不会隐式复制可变 class 引用；无法证明唯一转移或 const 发布时编译失败，应显式 `copy` 或 `move`。
- §16.9.5 的逃逸口（`unsafe`、`Array.mutPtr()`、`mem.*` 裸内存、`sys.Thread` 线程体、`CFn` 回调体）不在定理的保证范围内，其数据竞争后果是实现定义的。

#### 10.11.1 模型之外：`unsafe` 与 C 回调

上表的保证覆盖 Xray 值的所有权与能力判定。有三处**位于模型之外**，此时义务转移给使用者，`unsafe` 不放宽任何一条已有规则、也不提供任何替代保证：

- **`mem.volatileLoad` / `volatileStore` / `fence` / `pageAlloc`** 操作的是裸内存，没有所有权根、没有引用计数、也没有别名判定。编译器对这些地址不做任何假设，也不为它们提供 §2.14 的任何证据。用它们构造的共享结构，其数据竞争自由由使用者负责证明。
- **`Ptr<T>` / `MutPtr<T>`** 有借用跟踪（§2.4.2），但跟踪只到 owner 的作用域为止；解引用的地址有效性、对齐与别名正确性由 `unsafe` 块的作者承担。
- **`CFn` C 回调可能在任意 OS 线程上发生**，包括运行时并不管理的线程。回调体只能触及：传入的标量与裸指针、分配计划为共享的值（§16.3.1）、以及通过 `Channel` 转移进来的唯一根。回调体**不得**触及调用方协程的 coroutine-local 对象图——那些对象的引用计数在非原子带，跨线程接触即内存损坏。回调体也不得假定存在当前协程、当前 worker 或可用的调度器。

这三处是模型的**边界**而不是漏洞：它们在源码上都必须显式写出（`unsafe` 块、`mem` 模块导入、`CFn` 声明），因此"哪些代码不受编译期保护"始终可以从源码审计出来。

### 10.12 逻辑根任务与可达运行时能力

程序只有一个逻辑根任务，即入口模块的顶层代码。不可挂起的函数始终按普通调用执行，不隐式增加协程上下文、帧、安全点或当前任务查询。

- 普通（非根）函数结束时不等待它用 `go` 启动的任务；`scope` 的结构化等待按 §10.7。
- 丢弃最后一个 `Task` 句柄不取消、不阻塞该任务，任务照常运行到终态。
- 根任务的正文结束后，程序先把所有已经启动的任务运行到终态，再对外报告完成；普通子任务的失败不改写根任务的结果。
- 剩余任务全部在等待、且没有任何外部事件能唤醒它们时，程序以错误结束，不会无限挂起。
- 跨 Task 边界发布的完整终态都必须满足拥有式与 Sendable 规则，不只检查成功值 `T`：业务错误按其 enum 全部变体的载荷检查，而不是只看实际抛出的变体；泛型错误类型须在定义处同时约束为 `Error` 与 `Sendable`；`PanicInfo` 的完整可达图同样须可复制且 Sendable。
- `class` 默认不是 Sendable。显式写 `implements Sendable` 的 class，其全部字段都须是 Sendable，构造后字段不可重新绑定；复制这样的对象保留其身份。普通的同名接口不能冒充 Sendable。

runtime capability 只从 executable entry、manifest C export 等最终 artifact roots 传播。不可达的 `go` / `await` / Channel helper 不会迫使产物链接 scheduler、timer、netpoll 或 hosted runtime。

Hosted target 按 verified entry plan 选择 NONE / SINGLE / MULTI scheduler。Freestanding target 若可达代码只需要 core，则保持零 coroutine runtime；若需要 task、frame、submit、park/wake、timer、interrupt completion 或 executor pump，target manifest 必须提供版本化 provider ABI 及所需 hooks，缺失能力在生成或链接前硬失败。provider 是 target/build 契约，不引入 `async main`、`static main` 或 freestanding 专用源语言关键字。
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 10. Concurrency and Coroutines

xray's concurrency model is **goroutine-style coroutines + channels + strong static guarantees**. The design makes `go worker(args)` use the same explicit argument and ownership boundary as an ordinary call, while the **compiler guarantees no data race** within the language's safe subset. The precise form of that guarantee, its boundary, and the synchronisation edges each construct in this section establishes are defined in §16.9.

### 10.1 Coroutine model

| Dimension | Language guarantee |
|--|--|
| Function colouring | None. Ordinary functions carry no `async` / `suspend` marker; the compiler infers whether each function may suspend. Ordinary calls never depend on that inference for legality; only places that explicitly require non-suspension (function literals of a `nosuspend fn` type, `defer` bodies, and so on) check it |
| Scheduling | Cooperative: switches happen only at suspension points (`await`, channel operations, `select`, `Task.yieldNow()`, sleep, IO). Preemption is not guaranteed; a loop backedge is neither a yield point nor a cancellation checkpoint |
| Parallelism | Coroutines may run in parallel on several OS threads (M:N) and may move to another thread between two suspension points; programs must not rely on single-threaded ordering |
| Stack model | Stackless: only functions that may suspend keep state that lives across a suspension point; every other function runs as an ordinary call. Suspension cannot cross a C callback, so `CFn` callbacks must be `nosuspend` (§2.8) |

A function type without the `nosuspend` prefix may suspend (§2.8). A long loop without a suspension point neither yields the CPU to other coroutines on the same thread nor observes cancellation; call `Task.yieldNow()` or check `Task.isCancelled()` where needed (§10.4, §10.10). Worker thread counts, frame layout and creation cost are implementation properties that this specification does not promise.

A call through an interface value or an overridable method may suspend if any implementation the program can reach there may suspend. Adding one suspending implementation can therefore make a distant, previously valid `nosuspend` context fail, and the diagnostic names that implementation. When an interface requirement or class method carries a package-level `no_suspend` declaration promise (§5.2.11), every implementation must prove at its own definition that it does not suspend, and calls through that requirement never suspend. Where the set of implementations is open (interfaces exported from dynamic libraries, replaceable entries, external implementations), the call is treated as possibly suspending.

### 10.2 `go` — start a coroutine

```ebnf
GoExpr   ::= 'go' GoOptions? CallExpr
GoOptions ::= '(' GoOption (',' GoOption)* ')'
GoOption ::= 'name' ':' StringLiteral
```

`go` is an **expression** returning a `Task<T, E>` handle. It accepts only a call expression; inline logic must be an immediately invoked lambda:

```xray @id=coro-go-forms @fragment
// Form 1: call an existing function
var t1 = go worker(0, channel)

// Inline logic: a lambda must still form a complete call with explicit arguments
var t2 = go fn(d: JSON.Object) -> i64 {
    return d.value * 2
}(payload)

// Parameterless inline logic is still a zero-argument lambda call; go { ... } does not exist
var t3 = go fn() -> i64 {
    return compute()
}()

// Optional debugging name
var named = go(name: "worker-1") worker(1, channel)
```

**Pass according to the declaration**: copyable Sendable arguments retain owned logical copies, without requiring `copy(...)` or unique backing. A noncopyable Sendable resource transfers only to a consuming parameter; optional `move resource` marks that same transfer, not a `go` option. Task results must be copyable and Sendable; resources are not Task results:

```xray @id=coro-move-argument @check
var data = [10]
var task = go fn(d: Array<i64>) -> i64 {
    return d[0] + 1
}(data)            // retain a logical copy; the caller can still use data
```

`go { ... }` is not syntax. The single call form guarantees that every entry passes through the same argument evaluation, capture plan, and cross-coroutine transfer plan. Use a lambda call for inline logic and handle arguments through their declared READ or consuming mode:

```xray
var n = 10
var task = go fn(x: i64) -> i64 {
    return x + 1
}(n)
```

**Semantics**:
- Every `go` yields `Task<T, E>` where `T` is the return type and `E` is the declared error type, defaulting to `Never`. A Unit result stays Unit, not `null`.
- The runtime schedules the coroutine; it may run in parallel with the code that started it (§10.1).
- `go(name: ...)` only sets the debugging name and does not affect scheduling order.
- Uncaught exceptions are stored in the `Task` and rethrown when `await` is called.
- Arguments, owned captures and results must satisfy Sendable by their actual types. Ordinary generics are checked by constraints at the definition and reverified after Checked specialization. Copyable values retain declared logical copies, stopping at class and synchronization identities. Array qualification depends on every element; const, COW, reference counts and a single worker grant no additional Sendable authority. Ref, Slice/MutSlice, Cell and non-Sendable values cannot cross this boundary. Noncopyable Sendable resources transfer only through consuming parameters and do not enter ordinary copyable captures or Task results.
- An ordinary outer `var` may never be captured by a `go` closure, for either reads or writes. This rule does not depend on coroutine count or scheduling. Mutable state shared across coroutines must flow through audited `Channel`, `Atomic`, or `sync` handles; direct captured mutation is a compile error.

### 10.3 `await` — wait for a result

```ebnf
AwaitExpr ::= 'await' UnaryExpr
```

`await` accepts only `Task<T, E>` and produces `T` on success. Waiting may suspend without blocking an OS thread. When `E = Never`, `await t` throws no business error and takes no `try`; otherwise the propagation marker order is `try await t`, and inside a protected `try { }` region `await t` needs no additional marker.

```xray @id=coro-await-forms @fragment
var job = go compute(1)            // Task<i64, Never>
var n = await job                  // no business error, no try

var task = go fetch(url)           // fetch declares throws(FetchErr)
var result = try await task

var tasks = [go fetch(a), go fetch(b), go fetch(c)]
var results = try Task.all(tasks)
var first = try Task.first(tasks)
var firstOk = try Task.firstSuccess(tasks)
var bounded = try Task.timeout(task, 1000)
```

- The business error type is exactly the Task's `E`; no inference from the spawn site or worker body is needed. Cancellation is not a business error and never enters this type.
- Child enum errors use the value-error channel; child panic uses the panic channel. A successful `null` is a real Optional result, not cancellation or failure.
- Task results must be copyable and Sendable. Repeated waits produce logical value copies while preserving class and synchronization identities. Failure to copy a result does not consume or modify the task's terminal outcome.
- **Waiting on a cancelled task**: when the awaited task ends `Cancelled`, the waiter itself enters cancellation unwinding at that `await` (§10.4). It runs its defers, neither `catch (e)` nor `catch panic` intercepts the unwinding, the waiter also ends `Cancelled`, and the same rule then applies to whoever waits on it. Cancellation therefore propagates up the chain of waiters and never enters any function's error signature. A waiter whose own cancellation was already requested unwinds at that suspension point under §10.4 as well.
- To observe cancellation without propagating it, use `t.awaitResult()` or `t.poll()`, which report it as the value `TaskResult.Cancelled` (§10.4).

Multiple-task waits use suspending library functions without an `await` prefix:

| Function | Result | Business error type | Other tasks |
|--|--|--|--|
| `Task.all(ts)` | `Array<T>` in input order | `E` | Request cancellation before propagating failure; do not wait for cleanup |
| `Task.first(ts)` | Value or failure of the first task to finish | `E` | Request cancellation before returning or propagating; do not wait for cleanup |
| `Task.firstSuccess(ts)` | First successful value | `TaskFailures<E>` if all fail | Request cancellation before returning or propagating; do not wait for cleanup |
| `Task.timeout(t, ms)` | `T` | `E \| Timeout` | Cancel `t` on timeout |

When an input task ends `Cancelled` and that cancellation was not requested by the aggregate call itself, the waiting rule above makes the caller enter cancellation unwinding: `Task.all` does so when any input is cancelled, `Task.first` when the first input to finish is a cancellation, and `Task.timeout` when `t` is cancelled before the timeout. How cancelled inputs take part in the outcome of `Task.firstSuccess` is frozen together with its mixed-outcome arbitration below.

After selecting an outcome under its own rules, `Task.first`, `Task.firstSuccess`, and the failure branch of `Task.all` request cancellation of the remaining unfinished input tasks before returning or propagating the selected outcome. Their return does not wait for those tasks to terminate or finish their defers. Successful `Task.all` still waits for every input to succeed and returns results in input order.

Remaining tasks retain their original scope and executor ownership. The executor continues to own and drive them to terminal states. An aggregate call does not detach members from their scopes; a scope still waits for all members to terminate and finish cleanup before exit. Code after the aggregate call may therefore overlap cancellation and cleanup, while scope exit remains a complete lifetime boundary. Cooperative cancellation guarantees neither preemption nor a fixed return deadline. External program completion still follows the executor shutdown rules. Cancellation unwinding of the current waiter is unchanged.

Later outcomes of the remaining tasks do not retroactively replace an aggregate result already returned. Their subsequent errors, panic and cleanup failures continue through their original owners and applicable channels; early aggregate return cannot discard them. This rule specifies return and cleanup timing. It does not settle the mixed-outcome arbitration for `Task.firstSuccess` described below or change `Task.timeout`.

The first three functions accept homogeneous `Array<Task<T, E>>`, without implicit erasure of heterogeneous result or error types. Use a scope and individual waits for different types. `TaskFailures<E>` records only business failures: task debug names, positions, and error values E. It contains neither `PanicInfo` nor `Cancelled`. A scope position is the actual member registration ordinal; a library aggregate position is the input position. This semantic notation does not define concrete field names. Panic always uses its independent channel. Success-versus-panic races in `Task.firstSuccess` and mixed outcomes when none succeeds require separate joint execution-interface decisions; they cannot be inferred from the business aggregate type.

### 10.4 The `Task<T, E>` handle

`go f(...)` yields `Task<T, E>` where `T` is the return type and `E` is the declared error type, defaulting to `Never`. Handle API:

| Method | Type | Meaning |
|--|--|--|
| `t.cancel()` | `fn()` | Idempotently request cooperative cancellation |
| `t.poll()` | `fn() -> TaskResult<T, E>?` | Nonblocking; `null` while unfinished |
| `t.awaitResult()` | `fn() -> TaskResult<T, E>` | Suspend until terminal; report child errors and panic as values |

```xray @id=coro-task-handle @fragment
var t = go fetch(url)
if (t.poll() == null) { print("running") }
var outcome = t.awaitResult()
```

`TaskResult<T, E>` has exactly four terminal variants: `Success(T)`, `Error(E)`, `Panic(PanicInfo)`, and `Cancelled`. Parentheses describe payload types, not construction syntax or field names. The outer Optional of `poll()` represents unfinished work; `Task.timeout` represents timeout as an error. Returning a terminal outcome does not exempt the waiter from its own cancellation unwinding.

Task publication and repeated observation require copyable, Sendable T and E. The entire reachable PanicInfo graph must also be owned, copyable and Sendable; a read-only binding alone is insufficient. The task retains its terminal outcome, and each observer receives an independent logical snapshot. Value copies and identity references follow their ownership rules without borrowing a retired executor frame.

**Cancellation** is checked only at suspension points, including `await`, Channel operations, `select`, `Task.yieldNow()`, sleep and IO. A cancelled task runs its defers and unwinds. Neither `catch (e)` nor `catch panic` intercepts this, and it does not enter the task function's error signature. A task that waits on it with `await` or an aggregate function enters cancellation unwinding as well (§10.3); `awaitResult()` and `poll()` report `TaskResult.Cancelled` without propagating it. When cancellation unwinding reaches the logical root task the program ends: a standalone program prints an `[Uncaught Cancellation]` diagnostic to stderr and exits with code 1 (matching uncaught errors and panic in §8), while an embedding host ends only the current execution.

Loop backedges are not cancellation checkpoints. Long loops may check `Task.isCancelled()` and finish cooperatively, or call `Task.yieldNow()` to allow scheduling and cancellation. Runtime heartbeat or watchdog observations do not change these language-level checkpoints; a cancellation request does not guarantee preemption of a non-suspending native/FFI call.

### 10.5 Channel

```ebnf
ChannelType ::= 'Channel' '<' Type '>'
ChannelNew  ::= 'Channel' ('<' Type '>')? '(' Expression ')'
```

Channels use stable `const` bindings. Their audited synchronization capability permits `send`/`recv` to mutate protected internal state:

```xray @id=channel-decl-variants
const ch  = Channel<i64>(10)    // buffered, capacity = 10
const ch0 = Channel<i64>(0)     // unbuffered (synchronous handshake)
const cha: Channel<i64> = Channel(3)   // element type from the type context
```

The element type must be known at construction: write `Channel<T>(n)` or provide a type context. It is never inferred from a later `send`.

**API** (note that all method names are **camelCase**):

| Method | Signature | Behaviour |
|--|--|--|
| `send(v)` | `(T) -> ()` | Blocking send; waits for a consumer when full; panics (`E0702`) if the channel is closed |
| `recv()` | `() -> Recv<T>` | Blocking receive; returns `Recv.Closed` when closed and drained |
| `recvOr(default)` | `(T) -> T` | Blocking receive; returns the payload directly, or `default` when closed and drained, without allocating a `Recv<T>` wrapper |
| `trySend(v)` | `(T) -> SendResult` | Non-blocking send; returns `Sent` / `Full` / `Closed` |
| `tryRecv()` | `() -> Recv<T>` | Non-blocking receive; returns `Recv.Empty` when empty |
| `sendTimeout(v, ms)` | `(T, i64) -> SendResult` | Send with timeout; timeout returns `SendResult.Timeout` |
| `recvTimeout(ms)` | `(i64) -> Recv<T>` | Receive with timeout; timeout returns `Recv.Timeout` |
| `close()` | `() -> ()` | Close the channel; idempotent |
| `isClosed` | `bool` (property) | Whether the channel is closed |

**Timeout semantics**: `ms` must be an `i64` in `0..86_400_000`; an out-of-range value raises `NumericRange` before the Channel operation. Zero milliseconds performs one queue attempt. A positive duration establishes one monotonic deadline at the first attempt; repeated waits or registration cannot extend it. The host must supply a valid clock; a failed, zero or backward reading raises `HostError` rather than becoming a timeout. A closed send returns `SendResult.Closed`; a closed and drained receive returns `Recv.Closed`. Evaluate the receiver, message and duration once in source order, including Unit message side effects, and retain those values across waits. The first atomic terminal transition arbitrates peer completion against timeout/cancellation; a committed delivery cannot be withdrawn. Both methods conservatively may suspend, including a statically zero duration.

```xray @id=channel-basic-ops
const ch = Channel<i64>(10)
ch.send(42)                             // blocking send
var v = match (ch.recv()) {
    Recv.Value { value } -> value
    Recv.Closed -> -1
    _ -> -1
}

var sent = ch.trySend(99)               // SendResult.Sent / Full / Closed
match (ch.tryRecv()) {
    Recv.Value { value: next } -> print(next)
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

**Payload ownership**: `Channel<T>` requires copyable, storable, Sendable `T`. Sending an ordinary value preserves a logical copy, including independent later Array mutations. Class and synchronization handles retain their shared identity; sending does not clone identity, and `const` never substitutes for Sendable evidence.

In type position, a channel is written as `Channel<T>` and may be used in function parameters, fields, and return types:

```xray @id=channel-param-type
fn producer(ch: Channel<i64>) {
    ch.send(42)
}
```

**Semantics**:
- **MPMC** (multi-producer, multi-consumer).
- Buffered channel: senders suspend when full; receivers suspend when empty.
- Unbuffered channel: send and receive must rendezvous (synchronous handshake).
- After close: `send` panics with `E0702`. Sending on a closed channel is a programming error, as in Go; use `trySend` or `sendTimeout`, which return `SendResult.Closed`, when closure is an expected outcome. `recv` returns remaining buffered values as `Recv.Value { value: v }`, then `Recv.Closed`; `recvOr(default)` returns remaining buffered values, then `default`; `tryRecv` returns `Recv.Empty` when empty and not closed.
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
const ch1 = Channel<i64>(2)
const ch2 = Channel<i64>(2)

select {
    msg from ch1 -> { print("got from ch1:", msg) }      // receive arm
    msg from ch2 -> { print("got from ch2:", msg) }      // receive arm
    100  to   ch1 -> { print("sent 100 to ch1") }        // send arm
    _ -> { print("no channel ready") }                   // default arm (non-blocking)
}
```

**Semantics**:
- Receive arm `name from ch -> body`: selected when ch has data, and binds the `Recv.Value { value: name }` payload to `name`.
- Send arm `value to ch -> body`: equivalent to `ch.send(value)`, but selected only when `ch` has capacity; a selected send arm on a closed channel panics with `E0702`, exactly like `send`; `value` follows the same transfer plan as `ch.send` — an execution-local heap value must be written as explicit `copy(v)` or `move v`.
- Default arm `_ -> body`: runs immediately when no arm is ready; **omitting the default arm** makes `select` block until an arm becomes ready.
- When multiple arms are ready at the same time, one is selected **randomly** (matching Go).

### 10.7 `scope` — structured concurrency

`scope` means structured concurrency only; use `{ ... }` for ordinary lexical scope. Member tasks are started by `go` within the block, excluding nested function bodies. A block with no such `go` is a compile error with a suggestion to use an ordinary block.

```ebnf
ScopeStmt ::= 'scope' ('(' 'isolated' ')')? Block
TryScopeStmt ::= 'try' ScopeStmt
```

Both forms are statements without an aggregate value and wait for all members to terminate and finish cleanup before exit. A business-error-producing scope requires `try scope` or an enclosing real protected `try { }` region. `try scope` marks only the implicit exit propagation and creates no protected region: throwing calls in its body still require `try` / `try?` / `try!` or lexical protection by an outer real try body. Protection excludes nested function and closure bodies and that try statement's sibling handlers.

| Form | On child failure (including panic) | On exit |
|--|--|--|
| Default `scope` | Cancel the remaining siblings | Propagate the first successfully published panic if present; otherwise the first successfully published body or child business error |
| `scope(isolated)` | Let siblings continue | Propagate panic on its channel if present; otherwise propagate a body business error directly, or aggregate child business errors as `TaskFailures<E>` when no body business error exists |

Let B be the business errors escaping the body after local catches, including errors raised by handlers, and E the union of the declared error types of direct go children. Default scope has complete business error type `B | E`; isolated scope has `B | TaskFailures<E>`, with no child business aggregate channel when E=Never. Both forms need no scope propagation marker when B and E are Never; possible panic does not change this rule. This empty-channel rule does not define `TaskFailures<Never> = Never` in every type position. `try?` / `try!` can remove business escape, but cannot catch panic. The full escaping set must still fit the enclosing function's declaration; try does not enlarge its signature.

Normal block completion only joins members. Body Error, Panic or an outward return/break/continue cancels and drains accepted members in both forms. An isolated child failure alone does not cancel siblings. A pending normal control transfer must own its value and retain the body frame and valid loans; it is fulfilled only after draining finds no higher-priority failure, so return cannot swallow child panic. Multiple panics select the first successful publisher without promising wall-clock order. Other failures retain their original payloads for secondary diagnostics and release under unique ownership. When an isolated body error propagates directly, child business errors are secondary diagnostics; the body error is not implicitly wrapped.

```xray @id=coro-scope @check
fn worker(n: i64) -> i64 { return n + 1 }
fn main() {
    var x = 1
    { var x = 10; print(x) }
    print(x)
    scope {
        go worker(1)
        go worker(2)
    }
}
main()
```

```xray @id=coro-isolated-scope @check
enum WorkerErr { Failed }
fn worker(ok: bool) throws(WorkerErr) -> i64 {
    if (!ok) { throw WorkerErr.Failed }
    return 1
}
fn main() throws(TaskFailures<WorkerErr>) {
    try scope(isolated) {
        go(name: "first") worker(false)
        go(name: "second") worker(true)
    }
}
try main()
```

Scope-induced child cancellation is not a failure, and independently cancelled members are not wrapped as business errors or aggregate items. Once the parent enters cancellation unwinding at a legal suspension checkpoint, scope cancels and drains members, then continues uncatchable parent cancellation; member failures are secondary diagnostics. A cancellation request alone does not mutate frames, resume positions or terminal state. After a default scope cancels its remaining members because one member failed, a body that enters cancellation unwinding while waiting on one of those members (§10.3) unwinds only to the scope boundary: the scope still cancels and drains the remaining members and then propagates the first failure under the table above; that cancellation never propagates past the scope. Implicit scope waiting starts no cancellation unwinding. Panic escaping cleanup always follows the terminal rule in §8.2.5. Keep handles for individual waits or use §10.3 library functions when values are needed. A `go` outside a scope starts an independent task.

### 10.8 `move` — cross-coroutine ownership transfer

```ebnf
MoveExpr ::= 'move' Identifier
```

`move` is a consuming source action (not a `go` option) accepted in initializers, assignments, returns, and call arguments.

**The decision procedure is §2.14**: `move` requires a rebindable local `var` root that satisfies all four evidence axes at once — binding live, root unique, capability mutable, no live loan. Syntactically it accepts an identifier only: `move x.field`, `move arr[i]`, and `move f()` are syntax errors. After `move`, the variable is statically marked **moved**, and any later reference is a compile error; a rejected move does not poison the source state. A `const` capability is not a mutable-owner move source.

A transfer across a coroutine boundary reads the same decision procedure, so the guarantee that coroutines do not share a mutable graph rests on the uniqueness evidence in §2.14, not on a separate concurrency-only rule.

```xray @id=coro-move-transfer
var buf = Array<u8>(1024 * 1024)

// hand off to a coroutine
var t = go fn(b: Array<u8>) -> i64 {
    return process(b)
}(move buf)
// compile error: buf has been moved
// print(len(buf))

// hand off to a channel
const ch = Channel<Array<u8>>(1)
var payload = Array<u8>(4096)
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
| `Atomic<T>` | Lock-free atomic wrapper | C11 atomic operations for `i64`/`f64`/`bool` |
| `sync.Mutex<T>` / `sync.RwLock<T>` | Coroutine-domain locks | Require explicit `import sync`; wait by suspending a coroutine, not by blocking a worker; not allowed in `sys.Thread` bodies |
| `sys.OsMutex` / `sys.OsRwLock` / `sys.OsCondvar`, etc. | OS-thread-domain locks | Require explicit `import sys`; block the current OS thread, suitable for `sys.Thread`, runtime components, and short critical sections |

> **Design note**: xray does not expose bare `Mutex`/`RwLock` in the prelude. Prefer `Channel`, stable `const` synchronization handles, `move`, and `Atomic<T>` by default. When a lock is required, choose the execution domain explicitly through `sync` or `sys` so coroutine-suspending locks are not confused with OS-thread-blocking locks.

#### `Atomic<T>` — lock-free atomic type

`Atomic<T>` wraps `i64`, `f64`, or `bool`, allocated on the system heap, using C11 atomic instructions for lock-free cross-coroutine reads and writes.

**Declaration constraint**: name an `Atomic<T>` handle with `const`; its atomic methods come from an audited synchronized interior-mutation capability.

```xray
const counter = Atomic(0)         // Atomic<i64>
const flag = Atomic(false)        // Atomic<bool>
const rate = Atomic(3.14)         // Atomic<f64>
```

**Method overview** (full signatures in §14.19):

| Method | Description |
|---|---|
| `load(ord?)` | Atomic read |
| `store(val, ord?)` | Atomic write |
| `add(val, ord?)` / `sub(val, ord?)` | Atomic add/subtract (i64/f64) |
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

Copies of an `Atomic` handle share one atomic object; each construction creates an independent object, and different instances of one program do not share objects created by module initialization. Value atomic operations are guaranteed lock-free, not wait-free, and are never implemented with a mutex.

| Operation | Legal memory orders |
|---|---|
| load | Relaxed / Acquire / SeqCst |
| store | Relaxed / Release / SeqCst |
| add/sub/fetchAdd/fetchSub/swap/toggle/compareExchange | All five Ordering variants |

Omitting the ordering selects `SeqCst`. Only prelude `Ordering` values are accepted; a user enum with the same name is a different type and cannot act as a memory order. A memory order known to be invalid at compile time (for example `load(Ordering.Release)`) is a compile error. An invalid order supplied at run time ends the current execution after the receiver and all arguments are evaluated in source order and before any atomic read or write; atomic state is unchanged, and neither `catch` nor `catch panic` intercepts it.

compareExchange is strong CAS and returns a real `(T, bool)` Tuple without visible spurious failure. One ordering determines the success/failure pair:

| ordering | Success | Failure |
|---|---|---|
| Relaxed | Relaxed | Relaxed |
| Acquire | Acquire | Acquire |
| Release | Release | Relaxed |
| AcquireRelease | AcquireRelease | Acquire |
| SeqCst | SeqCst | SeqCst |

On failure the operation returns the value actually observed and writes nothing; on success it returns the value before the write. A failure or cancellation that happens after a successful CAS does not roll the write back.

i64 addition and subtraction wrap modulo 2^64 as in §2.3.1. f64 construction, load, store, swap and compareExchange preserve all binary64 bits; compareExchange normalizes NaNs at the observed comparison before comparing bits: positive and negative zero differ, while every NaN sign/payload matches the same positive canonical quiet NaN. This observation rule does not canonicalize ordinary copying, parameters or storage on every step. This does not change ordinary floating-point `==`. f64 addition and subtraction round as in §2.3.2 and, when racing other writes, recompute from the latest value until they succeed. `toString()` reads one `SeqCst` snapshot.

When the target cannot provide lock-free atomics of the required width, a program that uses `Atomic` is rejected at build or load time; it never falls back to a locking implementation.

> `Ordering` describes the memory order of **one atomic operation**; that alone is not enough to derive program behaviour. How these values combine with the language-level synchronisation edges of channels, `go`, `await`, `scope` and `const` publication into happens-before is defined in §16.9, which also governs ordinary concurrent code that never mentions `Ordering`.

```xray
const counter = Atomic(0)
counter.store(42, Ordering.Release)
var val = counter.load(Ordering.Acquire)
```


### 10.10 `Task.yieldNow()` — yield the CPU

```ebnf
TaskYieldCall ::= 'Task' '.' 'yieldNow' '(' ')'
```

```xray @id=coro-yield-loop @fragment
for (i in 0..1000) {
    do_chunk(i)
    Task.yieldNow()                // explicit safepoint, lets other coroutines run
}
```

`Task.yieldNow()` is a cooperative scheduling point, equivalent to an explicit safepoint where the scheduler can run other coroutines and observe cancellation. `yield expr` is reserved for generator value production; bare `yield` is rejected.

The call accepts no value or type arguments and returns unit; ordinary functions need no async annotation. A lexical binding shadowing `Task` is resolved normally and cannot silently invoke the scheduling primitive.

The two share a word root but are not the same suspension: `Task.yieldNow()` yields to the scheduler (can migrate OS threads, is a cancellation point, propagates to callers along call edges), while `yield expr` transfers symmetrically back to the driver (none of those hold). See §3.16.1 for the point-by-point comparison. Generator suspension and close follow §3.16.2–§3.16.3.

### 10.11 Concurrency safety model

xray eliminates data races at compile time through the type system. The precise statement is the theorem in §16.9.5: **a program containing no `unsafe` block and using none of the escape hatches listed in §16.9.5 has no data race, and its behaviour is equivalent to some sequentially consistent interleaving (SC-DRF)**. Inside the safe subset you may therefore reason in terms of interleaved statements without needing the happens-before detail; the rules below are how that theorem is enforced. The escape-hatch list and the language-level synchronisation edges are defined in §16.9.

The compiler enforces:

| Rule | Enforced |
|--|--|
| Every cross-execution boundary consumes the same provenance-based capture plan | ✅ |
| An execution-local graph requires explicit `copy`, or `move` from a local `var` | ✅ |
| Module-readonly values may retain the module owner; module-mutable state may not cross directly | ✅ |
| Published `const` values and audited synchronization handles may cross through a verified plan | ✅ |
| `move` only applies to explicit ownership transfer of ordinary local `var` values | ✅ |
| Channels for cross-coroutine values | ✅ |
| `Atomic<T>` uses a stable `const` binding and only audited methods mutate internal state | ✅ |

**What compile-time enforcement does not cover**:
- Channels never copy a mutable class reference implicitly. If uniqueness transfer or const publication cannot be proven, compilation fails; use explicit `copy` or `move`.
- The escape hatches of §16.9.5 (`unsafe`, `Array.mutPtr()`, raw `mem.*` memory, `sys.Thread` bodies, `CFn` callback bodies) sit outside the theorem's guarantee; a data race through them has implementation-defined consequences.

#### 10.11.1 Outside the model: `unsafe` and C callbacks

The guarantees above cover ownership and capability decisions for Xray values. Three places sit **outside the model**, where the obligation moves to the author. `unsafe` relaxes none of the existing rules and supplies no substitute guarantee:

- **`mem.volatileLoad` / `volatileStore` / `fence` / `pageAlloc`** operate on raw memory: no ownership root, no reference count, no aliasing decision. The compiler assumes nothing about those addresses and produces none of the §2.14 evidence for them. Data-race freedom of any shared structure built from them is the author's proof obligation.
- **`Ptr<T>` / `MutPtr<T>`** are borrow-tracked (§2.4.2), but the tracking reaches only as far as the owner's scope. Address validity, alignment, and aliasing correctness at the dereference belong to the author of the `unsafe` block.
- **A `CFn` C callback may run on any OS thread**, including threads the runtime does not manage. A callback body may reach only: the scalars and raw pointers passed in, values whose allocation plan is shared (§16.3.1), and a unique root transferred in through a `Channel`. It must **not** reach the calling coroutine's coroutine-local object graph — those objects' reference counts are in the non-atomic band, and touching them from another thread is memory corruption. A callback body must also not assume a current coroutine, a current worker, or an available scheduler.

These three are the model's **boundary**, not holes in it: each has to be written explicitly in source (an `unsafe` block, a `mem` import, a `CFn` declaration), so which code runs outside compile-time protection is always auditable from the source.

### 10.12 Logical root task and reachable runtime capabilities

A program has one logical root task: the top-level code of its entry module. A function that cannot suspend always runs as an ordinary call and never gains a hidden coroutine context, frame, safepoint or current-task query.

- An ordinary (non-root) function does not wait for the tasks it started with `go` when it returns; structured waiting in `scope` follows §10.7.
- Dropping the last `Task` handle neither cancels nor blocks the task; it runs to its terminal state.
- After the root task's body ends, the program runs every task already started to its terminal state before it reports completion; an ordinary child failure does not replace the root outcome.
- When every remaining task is waiting and no external event can wake any of them, the program ends with an error instead of hanging.
- The complete terminal outcome published across a Task boundary must satisfy the owned and Sendable rules, not only the success value `T`: a business error is checked across the payloads of every variant of its enum, not only the variant actually thrown; a generic error type must be constrained to both `Error` and `Sendable` at its definition; and the entire reachable `PanicInfo` graph must be copyable and Sendable.
- A `class` is not Sendable by default. A class that explicitly writes `implements Sendable` must have only Sendable fields, and those fields cannot be rebound after construction; copying such an object preserves its identity. An ordinary interface with the same name cannot stand in for Sendable.

Runtime capabilities propagate only from final artifact roots such as the executable entry and manifest-selected C exports. An unreachable helper containing `go`, `await`, or Channel operations does not force the artifact to link a scheduler, timer, netpoll, or hosted runtime.

A hosted target selects a NONE / SINGLE / MULTI scheduler from the verified entry plan. A freestanding target remains coroutine-runtime-free when reachable code needs only core. If reachable code needs task/frame allocation, submit, park/wake, timer, interrupt completion, or an executor pump, the target manifest must provide a versioned provider ABI and the required hooks; missing capabilities fail before generation or linking. The provider is a target/build contract and introduces no `async main`, `static main`, or freestanding-only source-language keyword.
<!-- /xr-spec:en -->

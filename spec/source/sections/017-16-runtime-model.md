---
id: spec.16_runtime_model
order: 017
---

<!-- xr-spec:cn -->
---

## 16. 运行时模型 (Runtime Model)

> 真值源：`src/runtime/`、`src/vm/`、`src/runtime/mem/`、`docs/rules/architecture.md`。

### 16.1 值表示

值表示属于实现。本规范不规定值的位布局、类型标签、装箱方式或字符串的物理存储；VM 寄存器与原生代码的拆箱表示由实现分别选择。语言可观察的只有值的静态类型、值语义与身份（§2.14），以及本章规定的回收与同步合同。

### 16.2 跨任务边界

可复制且 Sendable 的值跨任务边界时保留逻辑副本：发送方与接收方此后各自修改，互不影响，不要求写 `copy` 或 `move`。Sendable class 与同步句柄跨界时共享身份，同步由对象自身保证。非 Sendable 的值在编译期拒绝（§7.4、§10.2）。因此任务之间没有隐式共享的可变状态，共享可变状态只经同步句柄。

物理上，跨界的值在提升点由发布方提升到共享带，已在共享带的值再次跨界是 O(1)（§16.3.1、§16.9.4）。对不含身份的值，实现也可以改用复制；两种做法都不可观察。

### 16.3 对象生命周期与回收

- 普通对象由编译器插入的引用计数回收；最后一个强引用释放时立即进入 RC 销毁路径。本地带对象使用非原子计数，共享带对象使用原子计数（§16.3.1）；模块与运行时对象按各自 owner 的生命周期管理。
- **循环引用不由运行时回收**：回收只有一条规则 —— 对象在最后一个强引用释放的那一刻死亡。环由三层处理：编译期类型图证明大多数程序根本不产生环（L0）；`weak` 字段是唯一的显式断环机制（L1）；任务回收域在任务结束时断开剩余的环，为它们设定泄漏上界（L2）。开发期检测器只报告、不回收。
- **任务回收域（L2）**：每个任务拥有一个回收域。
  - 回收域只登记身份对象（class 实例、闭包环境、被闭包捕获的 `var` 的 cell），并且只登记 L0 判定可能成环的类型。L0 在特化之后按具体类型判断；函数类型字段、闭包捕获与尚未确定的泛型参数按“能到达任何 class”处理，无法判断时登记。
  - 字符串、Array、Map、Set 的存储与 struct、enum 值一律不登记，即使它们处在环的路径上。值类型的存储不能指向自己，所以每个环都至少经过一个身份对象。
  - 无环对象仍在最后一个强引用处立即回收。任务结束时，先释放该任务的全部局部根，再清空每个仍存活的已登记对象的出边（字段与它直接拥有的元素）；之后的释放由普通引用计数完成，回收域不直接释放对象。因此**未被引用计数回收的对象（引用环），其泄漏范围不超过所属任务的生命周期**。
  - 提升遍历同时把对象移出源任务的回收域（§16.3.1）。共享带上的环（例如 Channel 缓冲区里的消息持有同一通道）与主任务长期持有的环不受 L2 约束，可能留到进程结束。
- **宿主拥有式结果的关闭边界**：执行 owner 在所有 Call/Task 排空并撤去内部根后，处置没有外部 owned 根可达的执行残余图，并把其余完整图逻辑移交同一个宿主残余 owner。逻辑脱离不要求搬动物理地址或更换分配器与累计账本；它也不授予跨执行共享权限。宿主保留的值仍可按原类型和权限读、复制与修改。只要该宿主残余 owner 还有任意外部强根，后来形成或失去外根的环都留到 owner 结束；不得在每次释放时部分收环。最后一个外部根释放后，整体处置剩余图。普通 RC 归零仍立即析构，OPEN 执行域不扫描或收环。
- **这条性质在纯 RC 语言里并不常见**：Rust 的 Rc/RefCell 环与 Swift 的强引用环都会泄漏到进程结束，因为两者的引用计数对象都分配在进程级堆上，没有一个比进程更小的、可以整体丢弃的边界。Xray 的任务回收域提供了这个边界，于是「忘了标 `weak`」的代价从**永久泄漏**降为**有界泄漏**。这不是环回收，是生命周期封顶。
- Xray 没有并发 tracing GC，也没有环收集器；函数调用与后向跳转处不保留任何 GC hook。
- **用户可见 introspection**：`runtime.liveBytes()` / `runtime.liveObjects()` / `runtime.info()` 报告当前任务回收域（无当前任务时回退到主任务的域）的 live-memory 视图（`import runtime`；`mem` 模块只承载裸内存能力）。这些统计不保证跨后端一致（§16.8）。
- 只登记可能成环的身份对象，以及关闭时清空出边的规则，当前实现资格见 §17。

#### 16.3.1 引用计数的两个带

引用计数只有一个字段，用**符号**区分两个带：

| 带 | 编码 | 可达范围 | 原子性 |
|--|--|--|--|
| 本地带 | 正数 | 只被一个任务可达 | 非原子 |
| 共享带 | 负数（`N` 个引用存为 `-N`） | 可被多个任务可达 | 原子 |

热路径的符号测试把每个对象路由到正确的路径：本地带的加一、减一是普通读写，共享带走原子操作。不可变的常驻对象存 sticky 值，永不参与计数。新分配的对象在本地带。

**带只在提升点单向改变，由发布方完成。** 值在另一个任务能看到之前（§16.9.4 的提升点），发布方把它可达的本地带对象翻到共享带；遍历遇到已在共享带的对象就停，因此已共享的值再次跨界是 O(1)。运行时维持两条不变量：

1. 本地带对象只被一个任务可达；
2. 共享带对象只引用共享带对象。

**共享带的对象不写。** 值类型对象（Array、Map、Set、string 的存储）需要独占的修改时：计数为 −1（只有这一个引用），就先把这一个对象降回本地带，它引用的对象不动；否则照常写时复制，副本在本地带。修改路径本来就要读计数判断唯一，降带不增加检查。实现内部的缓存（例如字符串的哈希缓存）同样不在共享带对象上写，要么在提升时算好，要么用原子写。Sendable class 构造后字段不能重绑，本来就满足这一条；`weak` handle 随所属对象一起提升，在共享带上的读取与清空用原子操作。

**实现的自由。** 对不含身份的值，实现可以用复制代替提升，例如图小于某个阈值时；整图唯一的移动值也可以留在本地带。两者都不可观察。身份类型跨界时必须共享。

- 共享带对象可能由任何任务释放，分配器须支持跨线程释放。
- 逃逸分析把发布当作逃逸，栈上对象不会被提升。

真正的 OS 线程（`sys.Thread.spawn` 体、`CFn` 回调）适用同一套规则，没有任何放宽：线程体与回调可达的值都在提升点进入共享带（§16.9.4）。

两带编码、提升与降带的当前实现资格见 §17。

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
os.platform()             // "darwin" / "linux" / "windows"
os.arch()                 // "arm64" / "x64" / "x86" / "ppc64" / "riscv64"
os.sep()                  // 路径分隔符
os.eol()                  // 行尾
```

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

### 16.8 对象回收与析构时机契约

**回收点是精确的，且没有例外**：对象在它的最后一个强引用被释放的那一刻回收。VM 与 AOT 在这一点上给出相同的答案。

- 这条承诺之所以能是全称的，是因为回收只有引用计数这一个机制。运行时没有环收集器、没有 tracing GC、没有任何"某次 GC 时"的不确定时点（任务 247）。
- 唯一不由 RC 回收的形态是**引用环**，而它同样是确定的：环不由运行时收集，其泄漏范围由任务回收域封顶（§16.3 的 L2）；任务结束时断开已登记对象的出边不是环收集。环由编译期类型图预防（L0）、`weak` 显式断开（L1），开发期检测器只报告、不回收。
- 仍**不提供**用户可见的确定性析构（destructor / finalizer / `Drop`）表面：程序不得依赖某个类型的析构钩子是否存在、以何顺序运行、或运行在哪个线程。精确的是**回收时刻**，不是"你能挂一段代码上去"。

**绑定与临时值的释放时机**决定最后一个强引用何时释放：

1. **作用域结束。** 局部绑定在离开作用域的每条边上释放它持有的引用：正常落出、`return`、`break`、`continue`、错误传播、panic 展开与取消，与 `defer` 是同一组边。先执行该作用域的 `defer`，再按声明逆序释放绑定。循环体每结束一轮，就是一次作用域结束。重新赋值时释放旧值。
2. **已交出的不再释放。** 有两种情况：显式写了 `move x`；或者在每条路径上，绑定的最后一次使用都落在取得持有权的位置（初始化或赋值给另一个绑定、存入字段或容器、`move` 参数、返回、逃逸闭包捕获、`go` 实参、Channel 发送）。这时绑定不再持有值。只在部分路径上交出时，其余路径照常在作用域结束释放。这项分类按源码控制流完成，VM 与原生一致。
3. **纯值可以更早。** 类型不含身份时，值可以在最后一次使用之后的任意位置释放。含身份指类型可达 class、同步句柄、Channel、Task、不可复制资源或函数值；泛型在特化后判断，无法判断时按含身份处理。这是优化，不影响合法性和输出；`runtime.liveBytes()` 等统计因此不保证跨后端一致。
4. **临时值。** 没有名字的中间值在所在完整语句结束时释放，交给新所有者的除外；纯值可以更早。
5. **参数与捕获。** READ 与 `ref` 参数是借用，不释放；`move` 参数相当于在函数入口声明的局部绑定；闭包捕获的值随闭包释放；`go` 实参由任务持有。

只有含身份的值，其释放时机才能经 `weak` 或 Channel 等身份对象观察到。第 2 条使 `move x` 与隐式移动的结果相同（§2.14.5）。这些规则的当前实现资格见 §17。

唯一保证确定性、且跨后端（VM / AOT）一致的资源清理机制是 **`defer`**：它在所属作用域退出时按 LIFO 顺序执行，与对象回收时机无关。需要确定性释放外部资源（文件 / 句柄 / 锁）的代码必须使用 `defer`，而非依赖对象析构。

**`defer` 在每一条跨越其所属词法作用域的退出边上都执行**，包括：正常落出、`return`、`break`、`continue`、值错误向外传播、panic 展开，以及**协程被取消**。前端把每个注册点编译为程序点相关的静态 cleanup frontier；没有闭包、回调对象或动态运行时栈。取消不是一次外部击杀：可挂起帧仅保存定长 cleanup 区域标识与深度，调度器在该协程的 worker 上调用生成的静态 frontier，清理完成后才标记为已取消；`task.cancel()` 也因此不会在清理完成前让 `await` 观察到取消结果。

为了让这条保证是**全称**的，defer 体**不得抵达调度器挂起点，也不得创建任务**（`E0392`，见 §4.9 / §8.3.1）。挂起无法保证静态 cleanup 区域在帧离开前完成；创建任务则会让工作逃逸清理边界。静态分析对直接、传递与动态调用保守拒绝，VM/AOT 还在任务创建或调度侧效之前以 `E0444` 失败关闭。清理期间取消被屏蔽，因此清理本身不会被取消打断。

> 演进说明：当确定性析构（RAII / `Drop`）被正式纳入语言时，本节将升级为**确定性回收契约**（明确析构点与顺序），并由跨后端差分测试逐字节守门。在此之前，"回收时机 / finalizer 行为"被显式声明为实现定义的非确定项。

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
| 4 | `go` 启动 | `go expr` 处对实参、捕获与 `move` 转移的求值，以及它们的提升（§16.9.4），happens-before 协程体的第一条语句 |
| 5 | Task 完成 → 终态观察 | 任务体的最后一个动作，happens-before 等待者通过 `await t`、`awaitResult()` 或 `poll()` 成功观察到该终态后的操作；`poll()` 返回 `null` 不建立这条完成边，也不驱动子任务 |
| 6 | scope 退出 | scope 内全部子任务的完成，happens-before scope 块之后的语句。`Task.all` / `Task.first` / `Task.firstSuccess` 对它们实际观察到的每个终态建立第 5 条边；返回时仍在运行的余任务不建立边 |
| 7 | `const` 发布 | 顶层 `const` 的初始化与发布，happens-before 任何执行体对该 const 的首次读取 |
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

#### 16.9.4 引用计数提升规则

新分配的对象在本地带，使用非原子引用计数（§16.3.1）。一个值在另一个任务能看到之前，发布方**必须**已把它可达的本地带对象提升到共享带；否则并发 retain/release 是数据竞争，后果是堆损坏。

提升点是穷举的。每次提升遍历值可达的本地带对象，遇到已在共享带的对象即停：

| 提升点 | 说明 |
|---|---|
| `go` 的实参与捕获 | 在子任务的第一条语句之前完成（边 4） |
| Channel 发送，含 `select` 的发送分支 | 含 `move` 发送。接收方取得的对象仍在共享带，第一次需要独占的修改按 §16.3.1 降带或写时复制 |
| Task 与 `scope` 的结果 | 经由边 5 / 边 6 传出 |
| 存入同步句柄 | `sync.Mutex`、`sync.RwLock` 在离开临界区时重新发布槽的内容；`sync.WorkQueue` 等容器在入队时 |
| 模块 `const` 发布 | 经由边 7 |
| `sys.Thread` 线程体与 `CFn` 回调可达的值 | 见 §16.9.5：这条通道当前不在安全子集内 |

`string` 的存储与其他值类型对象遵守同一规则，不是例外。

#### 16.9.5 数据竞争自由的保证与边界

**定理（安全子集的 DRF）**：不含 `unsafe` 块、且不使用下方"逃逸口"能力的程序，不存在数据竞争；其行为等价于某个顺序一致（sequentially consistent）的交错执行。这就是 SC-DRF：在安全子集里可以按"语句交错"来推理，不需要理解本节的 happens-before 细节。

**逃逸口（穷举）**：以下能力可以引入数据竞争。它们的后果是**实现定义的，可能包含内存损坏**，证明责任在使用者：

- `unsafe` 块内的一切原始内存访问
- `Array.mutPtr()` 产生的可写原始指针（指针作为**实参**跨协程边界时不经过 Sendable 检查，也不经过提升）
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

> Source of truth: `src/runtime/`, `src/vm/`, `src/runtime/mem/`, `docs/rules/architecture.md`.

### 16.1 Value Representation

Value representation belongs to the implementation. This specification fixes no bit layout, type tag, boxing scheme or physical string storage; the VM's registers and native code each choose their own unboxed representation. What the language can observe is a value's static type, its value semantics and its identity (§2.14), together with the reclamation and synchronisation contracts of this chapter.

### 16.2 Cross-Task Boundaries

A copyable, Sendable value crossing a task boundary keeps a logical copy: sender and receiver may then each mutate their own value without affecting the other, and neither `copy` nor `move` is required. A Sendable class or a synchronisation handle crosses with its identity shared, and the object itself provides the synchronisation. A value that is not Sendable is rejected at compile time (§7.4, §10.2). Tasks therefore share no mutable state implicitly; shared mutable state goes only through synchronisation handles.

Physically, the publisher promotes a crossing value to the shared band at a promotion point, and a value already in the shared band crosses again in O(1) (§16.3.1, §16.9.4). For a value without identity the implementation may copy instead; neither choice is observable.

### 16.3 Object Lifetime and Reclamation

- Ordinary objects are reclaimed by compiler-inserted reference counting and enter the RC destruction path as soon as their last strong reference is released. Local-band objects use non-atomic counts, shared-band objects atomic counts (§16.3.1); module and runtime objects follow their respective owners' lifetimes.
- **Cycles are not reclaimed at runtime**: reclamation has exactly one rule — an object dies when its last strong reference goes. Cycles are handled in three layers instead: a compile-time type graph proves most programs never form one (L0), a `weak` field is the sole explicit way to break one (L1), and the task's reclamation domain breaks whatever remains when the task ends, bounding the leak (L2). The development-mode detector reports cycles; it never collects them.
- **Task reclamation domain (L2)**: every task owns one reclamation domain.
  - The domain registers only identity objects (class instances, closure environments, and the cells of `var` bindings captured by closures), and only of the types that L0 judges may form a cycle. L0 decides on concrete types after specialization; function-typed fields, closure captures and generic parameters not yet fixed count as "may reach any class", and an undecidable case is registered.
  - The storage of strings, Arrays, Maps and Sets, and struct and enum values, are never registered, even when they lie on a cycle's path. Value-type storage cannot point to itself, so every cycle passes through at least one identity object.
  - Acyclic objects still die immediately at their last strong reference. When the task ends, the domain first releases all of the task's local roots, then clears the outgoing edges (fields and directly owned elements) of every registered object still alive; ordinary reference counting performs the releases that follow, and the domain never frees an object directly. Thus **an object reference counting did not reclaim — a cycle — leaks no further than the lifetime of the task that built it**.
  - The promotion walk also removes objects from the source task's domain (§16.3.1). A cycle in the shared band (for example a message in a Channel buffer that holds the same channel) and a cycle held long-term by the main task are outside L2 and may remain until the process exits.
- **Closure boundary for host-owned results**: after all Calls and Tasks have drained and internal roots are withdrawn, the execution owner disposes its residual graph unreachable from external owned roots and logically transfers the remaining complete graph to one host residual owner. Logical detachment need not move physical addresses or replace the allocator and cumulative ledger; it grants no cross-execution sharing permission. Retained host values remain readable, copyable, and mutable under their original types and permissions. While any external strong root of that host residual owner remains, cycles formed or orphaned later remain until the owner ends; individual releases must not collect a subset of those cycles. The last external root ends the owner and disposes the complete remaining graph. Ordinary RC zero still destroys immediately, and an OPEN execution domain neither scans nor collects cycles.
- **This property is unusual among pure reference-counted languages**: an Rc/RefCell cycle in Rust, or a strong-reference cycle in Swift, leaks until the process exits — both allocate refcounted objects on a process-wide heap, with no boundary smaller than the process that can be dropped wholesale. Xray's task reclamation domain is that boundary, so forgetting a `weak` costs a **bounded** leak rather than a permanent one. This caps the lifetime; it is not cycle collection.
- Xray has no concurrent tracing GC and no cycle collector; no GC hooks remain at function calls or backward branches.
- **User-visible introspection**: `runtime.liveBytes()` / `runtime.liveObjects()` / `runtime.info()` report the current task's reclamation domain, falling back to the main task's domain when no task is current (`import runtime`; the `mem` module carries raw-memory capabilities only). These statistics are not guaranteed to agree across backends (§16.8).
- See §17 for the current implementation qualification of registering only possibly cyclic identity objects and of clearing their edges at close.

#### 16.3.1 The two reference-count bands

There is one reference-count field, and its **sign** selects between two bands:

| Band | Encoding | Reachable from | Atomicity |
|--|--|--|--|
| local | positive | exactly one task | non-atomic |
| shared | negative (`N` references stored as `-N`) | possibly several tasks | atomic |

A sign test on the hot path routes each object to the right path: increments and decrements in the local band are plain loads and stores, while the shared band uses atomic operations. Immortal objects store a sticky value and never participate in counting. A newly allocated object is in the local band.

**A band changes only at a promotion point, in one direction, by the publisher.** Before a value becomes visible to another task (the promotion points of §16.9.4), the publisher flips the local-band objects reachable from it to the shared band; the walk stops at any object already in the shared band, so a value that is already shared crosses again in O(1). The runtime maintains two invariants:

1. A local-band object is reachable from exactly one task.
2. A shared-band object references only shared-band objects.

**Shared-band objects are not written.** When a value-type object (the storage of an Array, Map, Set or string) needs an exclusive mutation and its count is −1 (this is the only reference), that one object is first demoted to the local band while the objects it references stay where they are; otherwise the mutation copies on write as usual, and the copy is in the local band. The mutation path reads the count to test uniqueness anyway, so demotion adds no check. Internal caches (such as a string's hash cache) are not written on shared-band objects either: they are computed at promotion or written atomically. The fields of a Sendable class cannot be rebound after construction, which already satisfies this rule; a `weak` handle is promoted together with its owning object, and reads and clears of it in the shared band are atomic.

**Implementation freedom.** For a value without identity the implementation may copy instead of promoting, for example below a size threshold, and a moved value whose whole graph is unique may stay in the local band. Neither choice is observable. Identity types must be shared when they cross.

- A shared-band object may be released by any task, so the allocator must support cross-thread release.
- Escape analysis treats publication as an escape, so a stack object is never promoted.

Real OS threads (a `sys.Thread.spawn` body, a `CFn` callback) follow the same rules with no relaxation: every value reachable from a thread body or a callback enters the shared band at a promotion point (§16.9.4).

See §17 for the current implementation qualification of the two-band encoding, promotion and demotion.

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
os.platform()             // "darwin" / "linux" / "windows"
os.arch()                 // "arm64" / "x64" / "x86" / "ppc64" / "riscv64"
os.sep()                  // path separator
os.eol()                  // end-of-line
```

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

### 16.8 Object Reclamation & Finalizer Timing Contract

**The reclamation point is exact, with no exceptions**: an object is reclaimed the moment its last strong reference is released. VM and AOT give the same answer here.

- The promise can be universal because reference counting is the only reclamation mechanism. There is no cycle collector, no tracing GC, and no indeterminate "some GC point" at runtime (task 247).
- The one shape RC does not reclaim is a **reference cycle**, and that too is determinate: cycles are not collected at runtime, and what they cost is capped by the task reclamation domain (L2 in §16.3). Clearing the outgoing edges of registered objects when a task ends is not cycle collection. Cycles are prevented by the compile-time type graph (L0) and broken explicitly with `weak` (L1); the development detector reports them and never reclaims.
- What is still **not** offered is a user-visible deterministic destructor (destructor / finalizer / `Drop`) surface: a program must not depend on whether a type has a finalization hook, in what order such hooks run, or on which thread. What is exact is the **moment of reclamation**, not the ability to attach code to it.

**Release timing of bindings and temporaries** decides when the last strong reference goes:

1. **Scope end.** A local binding releases its reference on every edge that leaves its scope: normal fallthrough, `return`, `break`, `continue`, error propagation, panic unwinding and cancellation, the same set of edges as `defer`. The scope's `defer` bodies run first, then the bindings are released in reverse declaration order. Each finished loop iteration is a scope end. Reassignment releases the old value.
2. **What was handed over is not released again.** This holds in two cases: the source wrote `move x`, or on every path the binding's last use is a position that takes ownership (initializing or assigning another binding, storing into a field or container, a `move` parameter, a return, an escaping closure capture, a `go` argument, a Channel send). The binding then no longer holds the value. Where it is handed over on only some paths, the other paths release it at scope end as usual. The classification follows source control flow, identically in the VM and native code.
3. **Pure values may go earlier.** A value whose type has no identity may be released anywhere after its last use. A type has identity when it can reach a class, a synchronisation handle, a Channel, a Task, a noncopyable resource or a function value; a generic is judged after specialization, and an undecidable case counts as having identity. This is an optimization that changes neither legality nor output; statistics such as `runtime.liveBytes()` are therefore not guaranteed to agree across backends.
4. **Temporaries.** An unnamed intermediate value is released at the end of its full statement unless it is handed to a new owner; a pure value may go earlier.
5. **Parameters and captures.** READ and `ref` parameters are borrows and are not released; a `move` parameter acts as a local binding declared at function entry; values captured by a closure are released with the closure; `go` arguments are held by the task.

Only for values with identity can release timing be observed, through `weak` or identity objects such as a Channel. Rule 2 gives `move x` and an implicit move the same result (§2.14.5). See §17 for the current implementation qualification of these rules.

The only deterministic, cross-backend (VM / AOT) consistent cleanup mechanism is **`defer`**, which runs at owning-scope exit in LIFO order, independent of object reclamation timing. Code that must deterministically release external resources (files / handles / locks) must use `defer` rather than relying on object finalization.

**`defer` runs on every exit edge that crosses its owning lexical scope**: normal fallthrough, `return`, `break`, `continue`, outward value-error propagation, panic unwinding, and **coroutine cancellation**. The frontend compiles each registration point into a program-point-sensitive static cleanup frontier; there is no closure, callback object, or dynamic runtime stack. Cancellation is not an external kill: a suspendable frame stores only fixed-layout cleanup-region identifiers and depth, and the scheduler invokes the generated static frontier on that coroutine's worker before marking it cancelled. `task.cancel()` correspondingly does not let an `await` observe cancellation before cleanup has run.

For this guarantee to be **total**, a defer body must **not reach a scheduler suspension point or create a task** (`E0392`, see §4.9 / §8.3.1). Suspension cannot guarantee completion of the static cleanup region before the frame leaves; task creation would let work escape the cleanup boundary. Static analysis rejects direct, transitive, and unresolved dynamic cases conservatively, and VM/AOT fail closed with `E0444` before a task-creation or scheduling side effect. Cancellation is masked while a defer body runs, so cleanup itself cannot be interrupted by cancellation.

> Evolution note: once deterministic destruction (RAII / `Drop`) is formally added to the language, this section will be upgraded to a **deterministic reclamation contract** (specifying destruction points and order), gated byte-for-byte by cross-backend differential tests. Until then, "reclamation timing / finalizer behavior" is explicitly declared an implementation-defined, non-deterministic aspect.

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
| 4 | `go` spawn | Evaluation of the arguments, captures and `move` transfers at the `go expr`, and their promotion (§16.9.4), happens-before the first statement of the coroutine body |
| 5 | Task completion → terminal observation | The task body's last action happens-before operations after a waiter successfully observes that terminal outcome through `await t`, `awaitResult()`, or `poll()`; a `null` poll establishes no completion edge and does not drive the child |
| 6 | scope exit | Completion of every child task in a scope happens-before the statements after the scope block. `Task.all` / `Task.first` / `Task.firstSuccess` establish edge 5 for every terminal outcome they actually observe; remaining tasks still running when they return establish no edge |
| 7 | `const` publication | Initialisation and publication of a top-level `const` happens-before any agent's first read of that const |
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
- Object reclamation or an RC reaching zero: although the reclamation point is exact (§16.8), it establishes no ordering between agents and so cannot carry synchronisation.
- The sysmon heartbeat and the `XRAY_SYSMON_CANCEL_MS` cancellation flag.
- Time: neither `time.sleep` nor any timeout expiry establishes ordering.

#### 16.9.4 Reference-count promotion rules

A newly allocated object is in the local band and uses non-atomic reference counting (§16.3.1). Before a value becomes visible to another task, the publisher **must** already have promoted the local-band objects reachable from it to the shared band; otherwise concurrent retain/release is a data race whose consequence is heap corruption.

The promotion points are exhaustive. Each promotion walks the local-band objects reachable from the value and stops at any object already in the shared band:

| Promotion point | Notes |
|---|---|
| `go` arguments and captures | Completed before the child task's first statement (edge 4) |
| A Channel send, including a `select` send arm | Including a `move` send. The objects the receiver obtains are still in the shared band; their first exclusive mutation demotes or copies on write as §16.3.1 describes |
| Task and `scope` results | Delivered through edge 5 / edge 6 |
| A store into a synchronisation handle | `sync.Mutex` and `sync.RwLock` republish the slot content when the critical section ends; containers such as `sync.WorkQueue` promote on enqueue |
| Publication of a module `const` | Through edge 7 |
| Values reachable from a `sys.Thread` body or a `CFn` callback | See §16.9.5: that channel is currently outside the safe subset |

String storage follows this same rule as every other value-type object; it is not an exception.

#### 16.9.5 The data-race-freedom guarantee and its boundary

**Theorem (DRF for the safe subset)**: a program containing no `unsafe` block and using none of the escape hatches below has no data race, and its behaviour is equivalent to some sequentially consistent interleaving. That is SC-DRF: inside the safe subset you may reason in terms of interleaved statements and never need the happens-before detail in this section.

**Escape hatches (exhaustive)**: the following capabilities can introduce a data race. Their consequences are **implementation-defined and may include memory corruption**; the proof obligation is the user's:

- Any raw memory access inside an `unsafe` block
- The writable raw pointer from `Array.mutPtr()` (as an **argument** a pointer crosses a coroutine boundary without Sendable checking or promotion)
- `mem.volatileLoad` / `volatileStore` / `fence` / `pageAlloc`
- The body of a `sys.Thread`
- The body of a `CFn` C callback (which may be invoked on any thread)

> This is the **precise form** of the claim that xray eliminates data races at compile time: a theorem with a stated boundary, not an unconditional assertion. Shrinking the escape-hatch list is a goal of the language's evolution, and each shrink must come with the mechanism that brings that capability inside the guarantee.

#### 16.9.6 Obligations on the compiler

A synchronisation edge is not only a runtime promise; it also constrains optimisation. The `:sync` column of `xisa/xi/ops.def` declares the edge each Xi operation establishes (`none` / `acquire` / `release` / `acq-rel` / `seq-cst`). Where the edge's strength is chosen at run time by an `Ordering` argument, the declaration is the **strongest** edge that operation can carry — a fail-closed upper bound.

From this follows one hard rule for every pass: **alias disjointness is not a licence to reorder**. Two memory operations must not be moved across an operation that carries a `:sync` edge or that may suspend, even when TBAA proves them disjoint. `xi_op_is_ordering_barrier()` is the single answer to that question.
<!-- /xr-spec:en -->

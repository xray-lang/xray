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

- **Descriptor（8 字节）**：`tag: byte`、`flags: byte`、`heap_type: u16`、`ext: u32`。`tag` 是类型判定的唯一入口；`heap_type` 只在 `tag == PTR` 时表示堆对象类型。
- **Payload（8 字节）**：`i64` / `double` / 指针三选一，按 `tag` 解释。
- **无 NaN-boxing / 无指针低位标记**：整数保留完整 64 位；对象引用是普通堆指针，类型信息在 descriptor 中。
- **字符串不是值级 SSO**：`string` 始终是 `XrString` 堆对象，字符数据存放在对象内的 `data[]` flexible array 中。运行期短串默认协程本地无锁分配，仅字面量/符号、显式 `intern()` 与 map/set 键驻留全局池；跨协程边界（channel send、`go` 实参、task/scope 结果）按需提升为共享原子 RC。这些都是对象层存储策略，不改变 `XrValue` 表示。

| 值类型 | 内部表示 |
|--|--|
| `int` | `XR_TAG_I64` + 64-bit signed payload |
| `float` | `XR_TAG_F64` + IEEE-754 double payload |
| `bool` | `XR_TAG_BOOL` + `0/1` payload |
| `rune` | `XR_TAG_RUNE` + Unicode scalar payload |
| `null` | `XR_TAG_NULL` + zero payload |
| `string` | `XR_TAG_PTR` + `XR_TSTRING` + `XrString*` |
| `Array<byte>` | `XR_TAG_PTR` + `XR_TARRAY`，元素布局为 byte |
| 其他对象 | `XR_TAG_PTR` + heap type + heap pointer |

Typed array 元素布局是容器元数据的一部分。`Array<rune>` 使用 `XR_ELEM_RUNE`，数据区是连续 `uint32_t[]` Unicode scalar；load 时重新装箱为 `XR_TAG_RUNE`，store 时拒绝非 `rune` 值，因此不会与 `Array<u32>` 混淆。

### 16.2 语义所有权域

存储由**两个正交的轴**描述，任何一处混用都会得出错误结论：

- **语义所有权域**（本节）：谁拥有这块存储、它活多久、跨执行边界时按什么规则迁移。这是**语义契约**。
- **后端物化**（§16.2.1）：后端实际把它放在哪（内联、栈、静态数据、某个堆）。这是**实现选择**。

> 真值源：`src/base/xstorage.h` 的 `XrSemanticStorageDomain`、`XrBackendMaterialization`、`XrTransferAction`。

一个值恰好属于六个语义域之一：

| 域 | 归属与生命周期 | 引用计数 | 典型来源 |
|--|--|--|--|
| **`EXEC_LOCAL`** | 当前执行体（协程）独占；随该执行体的堆一起结束 | 非原子 | 普通局部对象、字面量构造的容器与实例 |
| **`TRANSFERABLE`** | 唯一所有权，可跨执行边界**整体移交**；移交后源侧不再可用 | 非原子 | `move` 的目标、跨协程传递的唯一值 |
| **`CONST_SHARED`** | 已发布的不可变根，多个执行体并发只读共享 | 原子 | 发布后的 `const` 根、`freeze` 的结果 |
| **`SYNC_SHARED`** | 受审计的并发句柄，多个执行体并发访问，同步由句柄自身保证 | 原子 | channel、mutex 等并发原语持有的值 |
| **`MODULE_STATIC`** | 归属模块，活到模块卸载；**默认不提供并发安全性** | 按模块 owner | 顶层 `const` / `var` |
| **`FOREIGN`** | 由 Xray 之外的分配器拥有；运行时不管理其生命周期 | 无 | FFI 指针、外部缓冲 |

**域决定并发可见性与回收责任，不决定物理位置。** 例如 `EXEC_LOCAL` 的值可能被内联进持有者、放在栈上、或分配在执行堆上——三种物化都不改变"它随该协程结束"这一契约。

#### 16.2.1 后端物化

后端在满足语义域契约的前提下，为每个值选择一种物化：

| 物化 | 含义 |
|--|--|
| `INLINE` | 内联在持有者的存储里，无独立地址身份 |
| `STACK` | 函数帧上；随帧退出而失效，不可增长、不可释放 |
| `STATIC_DATA` | 只读静态数据段（consteval 结果、字面量池） |
| `EXEC_HEAP` | 当前执行体的堆（`XrCoroHeap`）；引用计数回收 |
| `SYSTEM_HEAP` | 系统堆；跨执行体可见的对象走这里 |
| `SROA` | 被标量替换后拆散到寄存器，聚合体本身不再具体存在 |
| `EXTERNAL` | 外部分配器持有 |
| `INVALID` | 未决定；出现在最终计划里即为内部错误 |

> **改变物化不得授予更强的语义域或迁移能力。** 把一个 `EXEC_LOCAL` 的值物化到 `SYSTEM_HEAP` 不会让它变得可以跨执行体共享；反过来，把 `CONST_SHARED` 的值物化到栈上是非法的——它的域要求地址在共享期内稳定。物化是语义域的**下游**，永远不是理由。

#### 16.2.2 跨执行边界的域迁移

值跨越执行边界（闭包捕获进 `go`、送入 channel、作为 task 结果返回）时，由其**源域**与可变性决定一个迁移动作。这是编译期判定，不是运行时降级：

| 动作 | 何时选用 | 语义 |
|--|--|--|
| `INLINE_COPY` | 标量、且非可变捕获 | 按值复制，两侧此后独立 |
| `CONST_SHARE` | 源域为 `CONST_SHARED` | 共享同一不可变根，原子引用计数 |
| `SYNC_SHARE` | 源域为 `SYNC_SHARED`，或捕获形态本身是并发句柄 | 共享同一句柄，同步由句柄保证 |
| `MOVE_UNIQUE` | 源域为 `TRANSFERABLE` | 整体移交所有权，源侧此后不可用 |
| `EXPLICIT_COPY` | **仅**源码写了 `copy(...)` | 物化一个独立的值再迁移 |
| `MODULE_READ` | 源域为 `MODULE_STATIC` 且不可变 | 只读引用模块绑定 |
| **`REJECT`** | **以上都不成立** | **编译错误** |

`REJECT` 是这张表里最重要的一项：**跨执行边界没有隐式深拷贝兜底**。可变的模块绑定、可变或被重新赋值的普通捕获、没有 const/sync/move 证据的托管值，全部是编译错误而不是"悄悄拷一份"。需要独立副本时，源码必须显式写 `copy(...)`。

### 16.3 对象生命周期与回收

- 默认由编译器插入的 **per-coroutine reference counting** 回收普通局部对象；最后一个强引用释放时立即进入 RC 销毁路径。共享对象使用 atomic RC，模块/运行时对象按各自 owner 的生命周期管理。
- **循环引用回收**：编译器标记可能形成环的类型；Bacon–Rajan trial-deletion collector 处理相应的 coroutine-local 强引用环。显式入口是 `runtime.collectCycles()`，候选根数量达到自适应阈值时也会自动触发。
- **collector 边界**：cycle collector 跳过 const/sync shared domain、runtime-managed 和 Region 对象。函数调用与后向跳转处保留的 tracing-GC hook 当前为空操作；Xray 没有并发 tracing GC。
- **用户可见 introspection**：`runtime.liveBytes()` / `runtime.liveObjects()` / `runtime.info()` 报告当前 coroutine heap（无当前 coroutine 时回退到 main coroutine）的 live-memory 视图（`import runtime`；`mem` 模块只承载裸内存能力）。

#### 16.3.1 引用计数的两个带

引用计数只有一个字段，用**符号**区分两个带：

| 带 | 编码 | 使用者 | 原子性 |
|--|--|--|--|
| coroutine-local | 正数 | 普通局部对象图 | 非原子（relaxed 快路径） |
| shared | 负数（`N` 个引用存为 `-N`） | const 发布值、同步句柄、模块/运行时对象 | 原子 |

热路径的符号测试把每个对象自动路由到正确的路径，因此两个带共存不需要额外分支。不可变的常驻对象存 sticky 值，永不参与计数。

**对象落在哪个带是编译期决定的**，由该分配点的存储计划确定（§2.13.2 的能力轴 + 分配域），不是运行时转换：一个存活对象不会在跨边界时被静默改带。这条是承重的——运行时改带需要与并发的非原子读改写同步，而那正是非原子带成立的前提所排除的。

因此跨执行边界只有两种合法形态：

1. **共享**：值的分配计划本身就是 const-shared 或 sync-shared，从创建起就在原子带。`const` 发布值、`Channel` / `Atomic` / `Semaphore` 等受审计句柄属于此类。
2. **转移**：`move` 把唯一根交出去，任一时刻只有一个所有者，因此非原子计数依然正确。

**第 2 条的正确性直接依赖 §2.13 的唯一性证据。** 如果一个仍有别名或存活借用的根被转移出去，两个执行上下文会同时对同一个非原子计数做读改写——那不是数据竞争而是内存损坏（重复释放或提前释放）。这就是 §2.13 把闭包捕获与堆存储都算作证据的原因：所有权判定是引用计数正确性的前提，不只是一条编程纪律。

真正的 OS 线程（`sys.Thread.spawn` 体、`CFn` 回调）适用同一套规则，且没有任何放宽：线程体只能触及分配计划为共享的值，或通过 `Channel` 转移进来的唯一根。

详见 `src/runtime/mem/`、`src/shared/xr_obj_header.h`。

### 16.4 协程调度

- M:N 调度（M OS 线程 × N 协程）。
- **work-stealing**：空闲 worker 从其他 worker 队列偷任务。
- **协作式抢占**：协程在 safepoint 让出（非强制抢占）。
- **公平性**：单一 runnable 队列配合本地 run-next、全局注入队列和 work-stealing；调度顺序不暴露用户级优先级。
- **栈管理**：VM 的 register/frame stack 在需要时扩容，扩容可能搬迁底层存储；运行时以 slot offset 重建指针。

详见 `src/coro/` 与 `src/vm/xvm_coro_backend.c`。

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
os.arch                   // 常量："arm64" / "x64" / "x86" / "ppc64" / "riscv64"
os.sep                    // 常量：路径分隔符
os.eol                    // 常量：行尾
os.sleep(100)             // 休眠毫秒数（与 `time.sleep` 等价）
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
    code: int                   // 错误码（从 "E0xxx: ..." 前缀自动解析，默认 0）
    data: Json                  // 运行时故障的结构化附加数据；无数据时为 JSON null

    constructor(message: string = "", cause: PanicInfo? = null)
    fn toString() -> string
}
```

用户级可恢复错误不使用 `PanicInfo`：`throw` 语句只接受 enum 变体值（见 §8.1.1）；非 enum 错误值在编译期被拒绝（错误码 `E0370`）。

栈展开（仅 panic 通道）：VM `xvm_unwind_stack()` 按 try-table 查找 `catch panic` handler，逐帧释放局部、执行 defer，到达 handler 后跳转。可恢复错误走值返回通道，不展开栈。详见 §8。

### 16.7 值返回错误通道运行时

`throw <enum>` 将枚举值写入帧的 `pending_error` 槽位，设置错误标志位并返回。调用方通过 `OP_ERR_CHECK` 检测标志位，决定进入 `catch` handler 或继续向上返回。该通道不展开栈、不分配 `PanicInfo`；在需要传播或捕获错误的调用边界，正常路径只经过可预测的错误标志分支。

### 16.8 对象回收与析构时机契约

xray **当前不提供用户可见的确定性析构（destructor / finalizer / `Drop`）**。对象的**回收时机**不属于可观察语义契约，由实现定义，且不同执行后端不同：

- 回收可能发生在"最后一个引用消失时"、"某次 GC 时"、或"进程退出时"中的任意一种，VM 与 AOT 的回收时机**不保证一致**。
- 程序**不得依赖**：(a) 对象在某个确定时刻被回收；(b) 任何析构 / finalizer 是否运行、运行顺序或运行所在线程。

唯一保证确定性、且跨后端（VM / AOT）一致的资源清理机制是 **`defer`**：它在所属作用域退出时按 LIFO 顺序执行，与对象回收时机无关。需要确定性释放外部资源（文件 / 句柄 / 锁）的代码必须使用 `defer`，而非依赖对象析构。

**`defer` 在每一条退出边上都执行**，包括：正常返回、`throw` 展开、panic 展开，以及**协程被取消**。取消不是一次外部击杀：被取消的协程若仍欠着 defer 链，会被交回调度器、在自己的 worker 上恢复一次、展开完成后才被标记为已取消；`task.cancel()` 也因此不会在清理完成前让 `await` 观察到取消结果。

为了让这条保证是**全称**的，defer 体**不得抵达调度器挂起点**（`E0392`，见 §2.14.4）。defer 体运行在一个正在离开的帧上，没有可供挂起后恢复的位置；允许它挂起就意味着允许清理执行到一半被丢弃，那样 `defer` 就不再是确定性机制。清理期间取消被屏蔽，因此清理本身不会被取消打断。

> 演进说明：当确定性析构（RAII / `Drop`）被正式纳入语言时，本节将升级为**确定性回收契约**（明确析构点与顺序），并由跨后端差分测试逐字节守门。在此之前，"回收时机 / finalizer 行为"被显式声明为实现定义的非确定项。

### 16.9 并发内存模型

> 真值源：`src/coro/xchannel.c`、`src/coro/xtask.c`、`src/coro/xtask_await.c`、`xisa/xi/ops.def` 的 `:sync` 列、`contracts/memory-model.md`。

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
| 4 | `go` 启动 | `go expr` 处对实参、capture plan 与 `move` 转移的求值，happens-before 协程体的第一条语句 |
| 5 | Task 完成 → await | 任务体的最后一个动作，happens-before 任何 `await t` / `awaitResult()` / `awaitTimeout()` 返回之后、或观测到 `t.done == true`、`t.poll()` 返回终态、`t.status` 为终态之后发生的一切 |
| 6 | scope 退出 | scope 内全部子任务的完成，happens-before scope 块之后的语句。`await all` / `any` / `anySuccess` 按其各自的完成条件建立同样的边 |
| 7 | `const` publish / seal | 顶层 `const` 的初始化与 seal，happens-before 任何执行体对该 const 根的首次读取 |
| 8 | Atomic release / acquire | `store(v, Release)` synchronizes-with 读到 v 的 `load(Acquire)`；RMW 构成 release sequence；`SeqCst` 操作另外参与一个单一全序。`AcquireRelease` 同时具备两侧 |
| 9 | 协程域锁 | `sync.Mutex` / `sync.RwLock`：一次 unlock synchronizes-with 后续成功的 lock |
| 10 | OS 线程域原语 | `sys.OsMutex` / `sys.OsRwLock` / `sys.OsCondvar` / `sys.OsBarrier`：同上；`sys.OsOnce` 的初始化 happens-before 所有观测到其已完成的调用 |
| 11 | `sys.Thread` | `spawn` 之前发生的一切 happens-before 线程体；线程体 happens-before `join` 返回之后 |
| 12 | 协程迁移 | 协程在 worker A 上挂起、在 worker B 上恢复：挂起前发生的一切 happens-before 恢复后发生的一切。这条边由运行时保证，用户代码看不见它，但**没有它，work-stealing 下的任何顺序推理都不成立** |
| 13 | `select` | 被选中的分支建立其对应 channel 操作的边（第 1 / 2 / 3 条） |

#### 16.9.3 非边（穷举地否定）

以下行为**不**建立 happens-before，不得用于同步：

- `Coro.yield()`：它是调度让出点，不是同步点。
- `codegen.compilerFence()`：只阻止有内存效果的操作跨越该编译器调度点，不是 CPU 屏障，不建立 happens-before，不能修复数据竞争（见 §12）。
- `Ordering.Relaxed` 的原子操作：保证该位置的访问不撕裂，不建立与其他位置的顺序关系。
- 对象回收 / RC 归零 / cycle collector 运行：回收时机不属于可观察语义（§16.8），因此不能承载顺序关系。
- sysmon 心跳与 `XRAY_SYSMON_CANCEL_MS` 取消标记。
- 时间：`time.sleep` 与任何超时到期都不建立顺序关系。

#### 16.9.4 引用计数提升规则

普通局部对象使用**协程本地非原子 RC**（§16.3）。一个对象一旦可以被两个执行体同时到达，其引用计数**必须**已经提升为原子 RC；否则并发 retain/release 是数据竞争，后果是堆损坏。

提升点是穷举的，每个提升点对可达图**深度生效**（对象自身及其可达的全部 managed 成员）：

| 提升点 | 说明 |
|---|---|
| Channel `send` 的值 | 含 `move` 发送；接收方独占后仍保持原子 RC |
| `go` / `scope` 的实参与 capture plan 成员 | 由 capture plan 静态确定 |
| task / scope 的结果值 | 经由边 5 / 边 6 传出 |
| `const` publish / seal 的可达图 | 一次性提升，之后图为深只读 |
| 跨 `sys.Thread` 边界或 `CFn` 回调可达的一切 | 见 §16.9.5：这条通道当前不在安全子集内 |

`string` 的提升在 §16.1 另有描述（短串协程本地、跨界按需提升为共享原子 RC）；那是同一规则在 `XrString` 上的实例，不是例外。

#### 16.9.5 数据竞争自由的保证与边界

**定理（安全子集的 DRF）**：不含 `unsafe` 块、且不使用下方"逃逸口"能力的程序，不存在数据竞争；其行为等价于某个顺序一致（sequentially consistent）的交错执行。这就是 SC-DRF：在安全子集里可以按"语句交错"来推理，不需要理解本节的 happens-before 细节。

**逃逸口（穷举）**：以下能力可以引入数据竞争。它们的后果是**实现定义的，可能包含内存损坏**，证明责任在使用者：

- `unsafe` 块内的一切原始内存访问
- `Array.mutPtr()` 产生的可写原始指针（指针作为**实参**跨协程边界时不经过 capture plan 检查）
- `mem.volatileLoad` / `volatileStore` / `fence` / `pageAlloc`
- `sys.Thread` 的线程体
- `CFn` C 回调体（可在任意线程被调用）

> 这是本语言"编译期消除数据竞争"这一说法的**准确形式**：它是一个带明确边界的定理，不是无条件断言。缩小逃逸口清单是语言演进的目标之一；每次缩小都必须同时给出该能力被纳入保证的机制。

#### 16.9.6 对编译器的约束

同步边不只是运行时承诺，它同时约束优化。`xisa/xi/ops.def` 的 `:sync` 列为每个 Xi 操作声明它建立的边（`none` / `acquire` / `release` / `acq-rel` / `seq-cst`）；当边的强度由运行期 `Ordering` 实参决定时，声明的是该操作可能承载的**最强**边，即 fail-closed 上界。

由此得到一条对所有 pass 的硬性规则：**别名不相交不构成重排许可**。两个内存操作即使 TBAA 证明不相交，也不得跨越携带 `:sync` 边或可挂起的操作移动。该规则由 `xi_op_is_ordering_barrier()` 统一回答，并由 `contracts/memory-model.md` 冻结。
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 16. Runtime Model

> Source of truth: `src/runtime/`, `src/vm/`, `src/runtime/mem/`, `docs/rules/architecture.md`.

### 16.1 Value Representation

Xray values are uniformly represented as `XrValue`. The current implementation requires a 64-bit platform and uses a **16-byte tagged struct-of-union**:

- **Descriptor (8 bytes)**: `tag: byte`, `flags: byte`, `heap_type: u16`, and `ext: u32`. The `tag` is the single entry point for type dispatch; `heap_type` is meaningful only when `tag == PTR`.
- **Payload (8 bytes)**: one of `i64`, `double`, or pointer, interpreted by the tag.
- **No NaN-boxing / no low-bit pointer tagging**: integers keep the full 64-bit payload; object references are ordinary heap pointers, with type metadata in the descriptor.
- **Strings are not value-level SSO**: `string` is always an `XrString` heap object, with bytes stored inside the object's `data[]` flexible array. Runtime short strings are coroutine-local with lock-free allocation by default; literals/symbols, explicit `intern()`, and map/set keys use the global pool. Cross-execution storage is selected from verified context at construction/publication time; a boundary never copies or promotes the payload implicitly. These are object-storage policies and do not change the `XrValue` representation.

| Value type | Internal representation |
|--|--|
| `int` | `XR_TAG_I64` + 64-bit signed payload |
| `float` | `XR_TAG_F64` + IEEE-754 double payload |
| `bool` | `XR_TAG_BOOL` + `0/1` payload |
| `rune` | `XR_TAG_RUNE` + Unicode scalar payload |
| `null` | `XR_TAG_NULL` + zero payload |
| `string` | `XR_TAG_PTR` + `XR_TSTRING` + `XrString*` |
| `Array<byte>` | `XR_TAG_PTR` + `XR_TARRAY`, with byte element layout |
| Other objects | `XR_TAG_PTR` + heap type + heap pointer |

Typed-array element layout is part of the container metadata. `Array<rune>` uses `XR_ELEM_RUNE`; its data area is a contiguous `uint32_t[]` of Unicode scalars. Loads re-box values as `XR_TAG_RUNE`, and stores reject non-`rune` values, so it cannot be confused with `Array<u32>`.

### 16.2 Semantic Ownership Domains

Storage is described by **two orthogonal axes**. Conflating them yields wrong conclusions:

- **Semantic ownership domain** (this section): who owns the storage, how long it lives, and what rule governs its migration across an execution boundary. This is a **semantic contract**.
- **Backend materialization** (§16.2.1): where the backend actually puts it (inline, stack, static data, some heap). This is an **implementation choice**.

> Source of truth: `XrSemanticStorageDomain`, `XrBackendMaterialization`, and `XrTransferAction` in `src/base/xstorage.h`.

Every value belongs to exactly one of six semantic domains:

| Domain | Ownership and lifetime | Reference counting | Typical origin |
|--|--|--|--|
| **`EXEC_LOCAL`** | Owned exclusively by the current execution (coroutine); ends with that execution's heap | Non-atomic | Ordinary local objects; containers and instances built from literals |
| **`TRANSFERABLE`** | Unique ownership that can be **handed over whole** across an execution boundary; unusable on the source side afterwards | Non-atomic | The target of a `move`; unique values passed between coroutines |
| **`CONST_SHARED`** | A published immutable root, read concurrently by several executions | Atomic | A published `const` root; the result of `freeze` |
| **`SYNC_SHARED`** | An audited concurrency handle accessed by several executions, where the handle itself provides the synchronization | Atomic | Values held by channels, mutexes, and other concurrency primitives |
| **`MODULE_STATIC`** | Owned by the module, alive until it unloads; **not concurrency-safe by default** | Per module owner | Top-level `const` / `var` |
| **`FOREIGN`** | Owned by an allocator outside Xray; the runtime does not manage its lifetime | None | FFI pointers, external buffers |

**A domain fixes concurrency visibility and reclamation responsibility, not physical location.** An `EXEC_LOCAL` value may be inlined into its holder, placed on the stack, or allocated on the execution heap — none of the three changes the contract that it ends with that coroutine.

#### 16.2.1 Backend Materialization

Subject to the semantic domain's contract, the backend picks one materialization per value:

| Materialization | Meaning |
|--|--|
| `INLINE` | Inlined into the holder's storage, with no separate address identity |
| `STACK` | On the function frame; dies with the frame, cannot grow, cannot be freed |
| `STATIC_DATA` | Read-only static data (consteval results, literal pools) |
| `EXEC_HEAP` | The current execution's heap (`XrCoroHeap`), reclaimed by reference counting |
| `SYSTEM_HEAP` | The system heap; objects visible across executions live here |
| `SROA` | Scalar-replaced into registers; the aggregate no longer exists concretely |
| `EXTERNAL` | Held by an external allocator |
| `INVALID` | Undecided; reaching a final plan is an internal error |

> **Changing materialization must never grant a stronger semantic domain or transfer capability.** Materializing an `EXEC_LOCAL` value on the `SYSTEM_HEAP` does not make it shareable across executions; conversely, materializing a `CONST_SHARED` value on the stack is invalid, because its domain requires a stable address for as long as it is shared. Materialization is **downstream** of the semantic domain, never a justification.

#### 16.2.2 Domain Migration Across Execution Boundaries

When a value crosses an execution boundary (captured into `go`, sent through a channel, returned as a task result), its **source domain** and mutability select a transfer action. This is decided at compile time; it is not a runtime downgrade:

| Action | Selected when | Semantics |
|--|--|--|
| `INLINE_COPY` | Scalar, and not a mutable capture | Copied by value; the two sides are independent afterwards |
| `CONST_SHARE` | Source domain is `CONST_SHARED` | Shares the same immutable root under atomic reference counting |
| `SYNC_SHARE` | Source domain is `SYNC_SHARED`, or the capture is itself a concurrency handle | Shares the same handle; the handle provides synchronization |
| `MOVE_UNIQUE` | Source domain is `TRANSFERABLE` | Ownership is handed over whole; the source is unusable afterwards |
| `EXPLICIT_COPY` | **Only** where the source wrote `copy(...)` | Materializes an independent value, then migrates it |
| `MODULE_READ` | Source domain is `MODULE_STATIC` and immutable | Reads the module binding by reference |
| **`REJECT`** | **None of the above holds** | **Compile error** |

`REJECT` is the most important row: **there is no implicit deep-copy fallback across an execution boundary.** A mutable module binding, an ordinary capture that is mutable or reassigned, and a managed value with no const/sync/move evidence are all compile errors rather than a silent copy. Where an independent copy is wanted, the source must say `copy(...)`.

### 16.3 Object Lifetime and Reclamation

- Ordinary local objects use compiler-inserted **per-coroutine reference counting** and enter the RC destruction path as soon as their last strong reference is released. Shared objects use atomic RC; module and runtime objects follow their respective owners' lifetimes.
- **Cycle collection**: the compiler marks types that may form cycles, and a Bacon–Rajan trial-deletion collector handles the corresponding coroutine-local strong-reference cycles. The explicit entrypoint is `runtime.collectCycles()`; collection also starts automatically when the potential-root count reaches an adaptive threshold.
- **Collector boundary**: the cycle collector skips the const/synchronized shared domain, runtime-managed, and Region objects. The former tracing-GC hooks at function calls and backward branches are currently no-ops; Xray has no concurrent tracing GC.
- **User-visible introspection**: `runtime.liveBytes()` / `runtime.liveObjects()` / `runtime.info()` report the current coroutine heap's live-memory view, falling back to the main coroutine when no coroutine is current (`import runtime`; the `mem` module carries raw-memory capabilities only).

#### 16.3.1 The two reference-count bands

There is one reference-count field, and its **sign** selects between two bands:

| Band | Encoding | Used by | Atomicity |
|--|--|--|--|
| coroutine-local | positive | ordinary local object graphs | non-atomic (relaxed fast path) |
| shared | negative (`N` references stored as `-N`) | published const values, synchronization handles, module and runtime objects | atomic |

A sign test on the hot path routes each object to the right path automatically, so the two bands coexist without an extra branch. Immortal objects store a sticky value and never participate in counting.

**Which band an object lands in is decided at compile time**, by the storage plan of its allocation site (the capability axis of §2.13.2 plus the allocation domain); it is not a runtime conversion, and a live object is never silently re-banded when it crosses a boundary. That is load-bearing: re-banding at runtime would have to synchronize with concurrent non-atomic read-modify-writes, which is exactly what the non-atomic band assumes cannot happen.

Only two shapes therefore cross an execution boundary:

1. **Sharing**: the value's allocation plan is const-shared or sync-shared, so it is in the atomic band from creation. Published `const` values and audited handles such as `Channel`, `Atomic`, and `Semaphore` are of this kind.
2. **Transfer**: `move` hands over a unique root. Exactly one owner exists at any moment, so a non-atomic count stays correct.

**The correctness of the second shape rests directly on the uniqueness evidence in §2.13.** If a root that still has an alias or a live loan were transferred out, two execution contexts would read-modify-write one non-atomic counter — which is not a data race but memory corruption (a double free or a premature free). That is why §2.13 counts closure captures and heap stores as evidence: the ownership decision is a precondition for reference-count correctness, not merely a programming discipline.

Real OS threads (a `sys.Thread.spawn` body, a `CFn` callback) follow the same rules with no relaxation: a thread body may reach only values whose allocation plan is shared, or a unique root transferred in through a `Channel`.

See `src/runtime/mem/` and `src/shared/xr_obj_header.h` for details.

### 16.4 Coroutine Scheduling

- M:N scheduling (M OS threads × N coroutines).
- **work-stealing**: idle workers steal tasks from other workers' queues.
- **Cooperative preemption**: coroutines yield at safepoints (no forced preemption).
- **Fairness**: a single runnable queue works with local run-next, global injection, and work-stealing; scheduling order does not expose user-level priorities.
- **Stack management**: VM register/frame stacks grow on demand and may relocate their backing storage; the runtime re-derives pointers from slot offsets.

See `src/coro/` and `src/vm/xvm_coro_backend.c` for details.

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
os.arch                   // constant: "arm64" / "x64" / "x86" / "ppc64" / "riscv64"
os.sep                    // constant: path separator
os.eol                    // constant: end-of-line
os.sleep(100)             // sleep in milliseconds (equivalent to `time.sleep`)
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
    code: int                   // error code (auto-parsed from "E0xxx: ..." prefix; default 0)
    data: Json                  // structured data for a runtime fault; JSON null when absent

    constructor(message: string = "", cause: PanicInfo? = null)
    fn toString() -> string
}
```

Recoverable user-level errors do not use `PanicInfo`: a `throw` statement accepts enum variant values only (see §8.1.1); non-enum error values are rejected at compile time (error code `E0370`).

Stack unwinding (panic channel only): the VM's `xvm_unwind_stack()` walks the try-table to find `catch panic` handlers, releasing locals frame by frame and running `defer` along the way before jumping to the handler. Recoverable errors use the value-return channel and never unwind the stack. See §8 for details.

### 16.7 Value-return Error Channel Runtime

`throw <enum>` writes the enum value into the frame's `pending_error` slot, sets the error flag, and returns. The caller detects the flag via `OP_ERR_CHECK` and either enters a `catch` handler or continues returning upward. This channel performs no stack unwinding and allocates no `PanicInfo`; at call boundaries that may propagate or catch errors, the happy path goes through only a predictable error-flag branch.

### 16.8 Object Reclamation & Finalizer Timing Contract

xray **currently exposes no user-visible deterministic destructor (destructor / finalizer / `Drop`)**. The **timing** of object reclamation is NOT part of the observable semantic contract; it is implementation-defined and differs across execution backends:

- Reclamation may occur at "the moment the last reference disappears", "some GC point", or "process exit"; VM and AOT reclamation timing is **not guaranteed to agree**.
- Programs **must not depend on**: (a) an object being reclaimed at any particular moment; (b) whether any destructor / finalizer runs, in what order, or on which thread.

The only deterministic, cross-backend (VM / AOT) consistent cleanup mechanism is **`defer`**, which runs at owning-scope exit in LIFO order, independent of object reclamation timing. Code that must deterministically release external resources (files / handles / locks) must use `defer` rather than relying on object finalization.

**`defer` runs on every exit edge**: a normal return, `throw` unwinding, panic unwinding, and **coroutine cancellation**. Cancellation is not an external kill: a cancelled coroutine that still owes a defer chain is handed back to the scheduler, resumed once on its own worker, and marked cancelled only after it has unwound. `task.cancel()` correspondingly does not let an `await` observe the cancellation before that cleanup has run.

For this guarantee to be **total**, a defer body must **not reach a scheduler suspension point** (`E0392`, see §2.14.4). A defer body runs on a frame that is already leaving and has no place to park and resume; allowing it to suspend would allow cleanup to be dropped halfway, and `defer` would stop being deterministic. Cancellation is masked while a defer body runs, so the cleanup itself cannot be interrupted by a cancellation.

> Evolution note: once deterministic destruction (RAII / `Drop`) is formally added to the language, this section will be upgraded to a **deterministic reclamation contract** (specifying destruction points and order), gated byte-for-byte by cross-backend differential tests. Until then, "reclamation timing / finalizer behavior" is explicitly declared an implementation-defined, non-deterministic aspect.

### 16.9 Concurrency Memory Model

> Source of truth: `src/coro/xchannel.c`, `src/coro/xtask.c`, `src/coro/xtask_await.c`, the `:sync` column of `xisa/xi/ops.def`, `contracts/memory-model.md`.

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
| 4 | `go` spawn | Evaluation of the arguments, capture plan and `move` transfers at the `go expr` happens-before the first statement of the coroutine body |
| 5 | Task completion → await | The task body's last action happens-before everything after any `await t` / `awaitResult()` / `awaitTimeout()` return, or after observing `t.done == true`, a terminal `t.poll()` result, or a terminal `t.status` |
| 6 | scope exit | Completion of every child task in a scope happens-before the statements after the scope block. `await all` / `any` / `anySuccess` establish the same edge under their respective completion conditions |
| 7 | `const` publish / seal | Initialisation and sealing of a top-level `const` happens-before any agent's first read of that const root |
| 8 | Atomic release / acquire | `store(v, Release)` synchronizes-with the `load(Acquire)` that reads v; RMWs form a release sequence; `SeqCst` operations additionally participate in one single total order. `AcquireRelease` carries both sides |
| 9 | Coroutine-domain locks | `sync.Mutex` / `sync.RwLock`: an unlock synchronizes-with a later successful lock |
| 10 | OS-thread-domain primitives | `sys.OsMutex` / `sys.OsRwLock` / `sys.OsCondvar` / `sys.OsBarrier`: as above; a `sys.OsOnce` initialisation happens-before every call that observes it as complete |
| 11 | `sys.Thread` | Everything before `spawn` happens-before the thread body; the thread body happens-before everything after `join` returns |
| 12 | Coroutine migration | A coroutine suspended on worker A and resumed on worker B: everything before the suspension happens-before everything after the resumption. The runtime guarantees this edge; user code never names it, but **without it no ordering argument holds under work-stealing at all** |
| 13 | `select` | The selected arm establishes the edge of its channel operation (edges 1 / 2 / 3) |

#### 16.9.3 Non-edges (exhaustively denied)

The following establish **no** happens-before and must not be used to synchronise:

- `Coro.yield()`: a scheduling yield point, not a synchronisation point.
- `codegen.compilerFence()`: it only prevents memory-effecting operations from moving across that compiler scheduling point. It is not a CPU fence, establishes no happens-before, and cannot repair a data race (see §12).
- Atomic operations with `Ordering.Relaxed`: they keep that location's accesses from tearing; they order nothing with respect to other locations.
- Object reclamation, an RC reaching zero, or a cycle-collector run: reclamation timing is not observable semantics (§16.8), so it cannot carry an ordering relation.
- The sysmon heartbeat and the `XRAY_SYSMON_CANCEL_MS` cancellation flag.
- Time: neither `time.sleep` nor any timeout expiry establishes ordering.

#### 16.9.4 Reference-count promotion rules

Ordinary local objects use **coroutine-local, non-atomic RC** (§16.3). Once an object can be reached by two agents, its reference count **must** already have been promoted to atomic RC; otherwise concurrent retain/release is a data race whose consequence is heap corruption.

The promotion points are exhaustive, and each applies to the reachable graph **deeply** (the object and every managed member reachable from it):

| Promotion point | Notes |
|---|---|
| The value of a Channel `send` | Including a `move` send; the value stays atomically counted after the receiver takes exclusive ownership |
| `go` / `scope` arguments and capture-plan members | Determined statically by the capture plan |
| Task / scope result values | Delivered through edge 5 / edge 6 |
| The reachable graph of a `const` publish / seal | Promoted once; the graph is deeply read-only afterwards |
| Everything reachable across a `sys.Thread` boundary or from a `CFn` callback | See §16.9.5: that channel is currently outside the safe subset |

`string` promotion is described separately in §16.1 (short strings are coroutine-local, promoted to shared atomic RC on demand when they cross a boundary). That is this same rule instantiated for `XrString`, not an exception to it.

#### 16.9.5 The data-race-freedom guarantee and its boundary

**Theorem (DRF for the safe subset)**: a program containing no `unsafe` block and using none of the escape hatches below has no data race, and its behaviour is equivalent to some sequentially consistent interleaving. That is SC-DRF: inside the safe subset you may reason in terms of interleaved statements and never need the happens-before detail in this section.

**Escape hatches (exhaustive)**: the following capabilities can introduce a data race. Their consequences are **implementation-defined and may include memory corruption**; the proof obligation is the user's:

- Any raw memory access inside an `unsafe` block
- The writable raw pointer from `Array.mutPtr()` (as an **argument** a pointer crosses a coroutine boundary without passing through capture-plan checking)
- `mem.volatileLoad` / `volatileStore` / `fence` / `pageAlloc`
- The body of a `sys.Thread`
- The body of a `CFn` C callback (which may be invoked on any thread)

> This is the **precise form** of the claim that xray eliminates data races at compile time: a theorem with a stated boundary, not an unconditional assertion. Shrinking the escape-hatch list is a goal of the language's evolution, and each shrink must come with the mechanism that brings that capability inside the guarantee.

#### 16.9.6 Obligations on the compiler

A synchronisation edge is not only a runtime promise; it also constrains optimisation. The `:sync` column of `xisa/xi/ops.def` declares the edge each Xi operation establishes (`none` / `acquire` / `release` / `acq-rel` / `seq-cst`). Where the edge's strength is chosen at run time by an `Ordering` argument, the declaration is the **strongest** edge that operation can carry — a fail-closed upper bound.

From this follows one hard rule for every pass: **alias disjointness is not a licence to reorder**. Two memory operations must not be moved across an operation that carries a `:sync` edge or that may suspend, even when TBAA proves them disjoint. `xi_op_is_ordering_barrier()` is the single answer to that question, and `contracts/memory-model.md` freezes it.
<!-- /xr-spec:en -->

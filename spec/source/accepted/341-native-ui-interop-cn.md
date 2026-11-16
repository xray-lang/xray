# 已接受目标规则：341 - 原生 UI 互操作：主线程域、闭包回调与零拷贝边界

> 2026-11-16 从任务文档迁入统一规范源。此处维护已接受的目标定义；原任务只维护实施、依赖、理由与验收记录。
> 迁入不表示实现完成。下面保留的示例是目标验收材料，不能据此声称当前编译器、VM 或 AOT 已支持。历史修订和已明确被取代的规则保持历史身份；不能将旧版本重新激活。
> 实施任务：[341-native-ui-interop-cn](../../../../xray-docs/tasks/341-native-ui-interop-cn.md)。

## 4. 语言设计

本节的拼写都是暂定的，裁决时一并确定（§14）。

### 4.1 主线程域与根执行流效应（已裁决）

**概念。** 进程的主线程是运行入口代码的 OS 线程。主线程域就是根执行流，包括三类执行：根任务（入口顶层代码）、主线程任务（`go(main)` 启动的任务，以及挂起的事件处理体，见 §4.2）、主线程回调。它们都只在主线程上运行，共用根回收域和非原子计数带。

**沿用已有的根执行流效应，不新增效应位。** 规范 §17.38 已经为每个函数推导 `requires_root` 与 `unresolved`，`go` 的目标需要根或无法确定时拒绝；来源包括可变模块槽、非 Sendable 模块常量、模块初始化，以及原生函数（目前只有 `sys.onSignal`）。本方案只做两件事：契约为 `main` 的 extern 函数成为根来源；入口计划可达这类来源时，根执行流钉在主线程上。界面调用于是与模块状态遵守同一条规则、报同一种错，一个事实只有一个生产者。下文的"`main` 效应"就是这个根事实，裁决过程与模拟见[决策记录](../../../../xray-docs/analysis/341-decision-1-main-thread-model-2026-11-09-cn.md)。

推导规则（用户一般不写标注）：

1. 来源：调用契约为 `main` 的 extern 函数；调用 `main fn`（即 `ROOT_REQUIRED`）类型的函数值；调用需要根的函数；读写模块可变状态等现有来源；经接口或可覆写方法的调用，按全部候选实现的并集推导（同 Task328 A5），能确定接收者实际类型时按实际类型判断（配套 2）。
2. 允许出现的位置：根任务及其同步调用的函数、模块初始化、`main fn` 的函数体、主线程任务的函数体。
3. 禁止出现的位置（编译错误，诊断要给出从该位置到第一个 `main` 来源的调用链）：`go` 的目标、`sys.Thread` 线程体、`sendable fn` 与 `CFn` 的函数体、未写 `main` 前缀的逃逸函数值。
4. 不逃逸参数（Task340）按调用点的实参推导：`items.forEach(x -> label.add(x))` 不需要任何标注。
5. 主线程域里的句柄类型永远不是 Sendable，不能 `move` 到别的执行，也不能被 `go` 的闭包捕获。含有这类句柄的 class 和 struct 随之不是 Sendable。规范 §10.11 的现有捕获规则已经保证这一点，不需要新机制。

**裁决附带的四项配套**（缺一项都不能交付界面代码）：

1. **"不需要根执行流"的函数类型写法**：`sendable fn`，对应现有的 `ROOT_NONE` 标志。现在普通显式 `fn` 类型默认是无法确定，后台任务把纯闭包存进数组再调用会被误拒，且没有写法可修（决策记录 s6）。这一项不依赖 UI，现有根效应已经需要它，可以提前实施。
2. **按实际类型判断根事实**：接收者就在本函数构造、或类型是 `final class` 时，不再取全部覆写的并集。决策记录 s4 中，别处一个子类的覆写碰了界面，后台只用基类的代码就被拒绝；去掉子类或改用泛型约束都通过。诊断要指出是哪个覆写带来的根要求。
3. **诊断给出 `go` 表达式的位置和完整原因链**。现在只有一句"GO target requires the current instance root execution"，隔几层调用时找不到原因。编译器已经记录原因链（`XrXirRootCauseTrace`），规范 §17.38 把它列为未完成项。
4. **修掉现有的内部错误**：把非 Sendable 类实例或需要根的函数值交给 `go`，现在报"constructed XIR failed checking"而不是正规诊断（[known_bugs](../../../../xray-docs/known_bugs.md) 2026-11-09 条目）。

**`main fn` 类型前缀。** 与 `nosuspend fn`、`escaping fn` 并列，是现有 `ROOT_REQUIRED` 标志的源码写法（拼写在第 7 项定，可能改叫 `root fn`）。`main fn()` 的值只能在允许 `main` 的位置调用，函数体可以有 `main` 效应。闭包字面量的类型由期望类型给出，所以写在 `main fn` 位置的闭包直接得到这个类型；已有的普通函数值可以传给 `main fn` 位置（它不碰主线程，在主线程上调用是安全的），反方向拒绝，与规范 §2.8.1 中"不抛错的函数可赋给抛错函数类型"同一方向。函数类型嵌套在别的函数类型里时不做这种放宽，与 Task340 第 9 条一致。用户只在少数地方会写到它，例如把处理函数存进字段：`var handlers: Array<main fn()>`。

**为什么这样是安全的。** 主线程域的值只能从主线程域的代码到达：非 Sendable 规则挡住了跨执行传递，`main` 效应挡住了在别的线程上调用碰主线程的代码。因此主线程域的对象图只有一个线程在访问，非原子引用计数是正确的，界面代码也进入了规范 §16.9.5 数据竞争自由定理的保证范围。§16.9.5 说过缩小逃逸口清单是语言演进的目标，本方案把主线程回调从逃逸口中移出，只留下 `CFn`。

**根任务钉住。** 入口计划里可达 `main` 契约的原生来源时，根任务整个生命周期钉在主线程上，主线程不参加窃取，只运行主线程域的执行（§5.1）。不可达时，调度方式与现在相同，不使用主线程域的程序不付任何代价。

**主线程任务是根执行流内部的任务。** 现在 `go` 的目标不能需要根；`go(main)` 是新增的唯一例外：它启动的任务属于根执行流，只在主线程上调度，所以目标可以需要根，参数与捕获可以是主线程域的值。多个主线程任务因此会在挂起点交错访问同一批模块变量与对象；2026-11-13 定下的规则是其前提：借用的根不在当前帧时不得跨挂起点（[决策记录](../../../../xray-docs/analysis/313-decision-borrows-across-suspension-2026-11-13-cn.md)）。

**主线程任务的写法（第 7 项已裁决，见[决策记录](../../../../xray-docs/analysis/341-decision-7-spellings-and-method-aliases-2026-11-10-cn.md)）：**

| 写法 | 说明 |
|---|---|
| `go(main) f(args)` | 复用规范 §10.2 的 `go` 选项列表；只能在主线程域里写；返回 `Task<T, E>`，`E` 来自 `f` 的签名。`main` 只在 `go` 的选项位置识别 |
| `Task.isMain() -> bool` | 当前是否在主线程域里，诊断用 |

不新建 `mainthread` 模块：模块名不能叫 `main`，会与语料中 777 个 `fn main(` 撞车；复用 `go` 也与 Kotlin `launch(Dispatchers.Main)` 同一思路。

`go` 的含义不变：总是启动可以在任何线程上运行的任务，捕获规则照旧。所以 UI 代码的模式很简单：界面逻辑在主线程域里写，耗时的计算 `go` 出去，用 `await` 拿结果或用 Channel 拿进度。后台任务无法直接改界面，编译器会拒绝。

**`main` 与 `pinned`（2026-11-10）。** 主线程以外也有只能在固定线程上调用的库：OpenGL 与 CUDA 的当前上下文、Linux 网络命名空间、COM 单线程套间、JNI、嵌入 Python。[Task335](../../../../xray-docs/tasks/335-coroutine-execution-and-scheduler-cn.md) §6.13 为此引入 `pinned` 修饰与 `go(pinned) f()`：被钉住的任务独占一条 OS 线程，结束后线程销毁。线程要求因此分三级：无 ⊂ `pinned` ⊂ `main`。`main` 就是钉在进程主线程上，根任务与 `go(main)` 任务都满足 `pinned`；`pinned` 任务不属于根执行流，不能调用 `main` 来源，也不能访问模块可变状态。推导与诊断沿用本节的根执行流机制。

### 4.2 回调

extern 签名里的函数类型参数，在 C 一侧降级为（函数指针，上下文指针），必要时再加一个销毁通知函数。参数顺序不同的 C API 由生成的 shim 适配。按 C 库对回调的使用方式分四种：

| 种类 | Xray 写法 | C 库怎么用它 | 闭包与捕获 | 运行时成本 |
|---|---|---|---|---|
| 同步 | `fn(A) -> R`（Task340 默认不逃逸） | 只在这次调用返回前、在同一线程上调用 | 任意闭包，可以捕获借用；环境放在调用方的栈上，不分配 | 函数指针调用加一个直接调用；AOT 可以为调用点生成专门的蹦床 |
| 主线程 | `escaping main fn(A) -> R` | 存起来，以后在主线程上调用 | 任意捕获，不要求 Sendable；闭包装箱，引用计数 | 一次线程本地标志检查，一次间接调用 |
| 任意线程 | `escaping sendable fn(A) -> R`（`sendable` 即 `ROOT_NONE`，并隐含 `nosuspend`） | 存起来，可能在任何线程上调用（音频、文件监视） | 按 `go` 的捕获规则；函数体不挂起，也不能有 `main` 效应 | 同上，不做线程检查 |
| 裸函数指针 | `CFn<fn(A) -> R>` | 没有上下文参数的 C API（`qsort`、`atexit`） | 模块级、非捕获（规范现状） | 无额外成本 |

**主线程回调的生命周期。** 闭包装箱后，上下文指针持有一个引用。什么时候释放由契约给出，三选一：

- `notified`：C 库在不再需要时调销毁通知（GLib 的 `GDestroyNotify`，Qt 信号连接随发送者销毁）；
- 随句柄：释放绑在某个句柄参数上，句柄释放或失效时一起释放（适合没有销毁通知、回调随对象存活的 API）；
- `permanent`：存活到进程结束（`sapp_run` 的帧回调）。

**线程检查。** 主线程回调的蹦床先读一个线程本地标志，不在主线程就终止进程并报告是哪个回调。这只会在 C 库违反自己的契约时发生，是诊断不是语义。

**挂起的处理体（D4，已裁决）。** 主线程回调的函数体可能挂起时（里面有 `await`、Channel 收发等），蹦床把它作为主线程任务启动：同步运行到第一个挂起点，然后把控制权还给 C；之后在主线程上由主执行器恢复。这与 C# 的 `async void` 事件处理、Swift 的 `Task { @MainActor in … }` 相同。函数体不挂起时就是普通调用，不创建任务。

Xray 的挂起是推导出来的：一个深层辅助函数加了一次 Channel 发送，处理函数就会从"执行完再还给 C"变成"中途还给 C"。2026-11-09 用户采用加护栏的做法（H1+），事件循环模拟见[决策记录](../../../../xray-docs/analysis/341-decision-4-suspending-handlers-2026-11-09-cn.md)：

1. 任务归注册该回调的句柄所有，句柄被销毁或失效时请求取消，任务在下一个挂起点执行 `defer` 并结束；注册在全局入口（例如帧回调）上的任务归根执行流所有。模拟中处理到一半关窗，不加这条会在恢复后访问已销毁的控件。
2. 借来的回调参数（C 侧事件对象、`Slice` 等）在第一个可能的挂起点之后不能使用，编译错误指出挂起来源的调用链。这是现有借用规则的应用，参数的主人是已经返回的 C 栈帧。模拟中"在 `await` 之后才接收按键"和"只改了 `log()` 使其可能挂起"在 H1 下都是未定义行为，加这条后都在编译期拒绝。
3. 要给 C 返回值的回调必须不挂起，可能挂起是编译错误。
4. 处理函数任务在第一个挂起点之后的未捕获 panic 按根任务处理：打印 `[Uncaught Panic]` 并结束程序。

不采用"处理函数必须不挂起、显式 `go(main)`"（H2）：它对无害的改动（按钮处理函数调用的辅助函数变成可能挂起）也要求重写，关窗的情况还要开发者自己记得给作用域。

**错误。** 回调的函数体不能抛业务错误（规范对 `CFn` 已有此要求，推广到全部四种），要在体内处理。panic 见 §4.5。

### 4.3 原生句柄

在 `extern` 块里声明句柄类型。句柄在 Xray 一侧是有身份的引用（按 class 的方式复制引用），在 C 一侧是一个指针。

```xray
unsafe extern "C" {
    handle Image release sg_destroy_image          // 拥有式
    main handle Widget tracked release qx_delete   // 主线程、可失效；未被收养时由 Xray 释放
    main handle Label: Widget                      // 句柄子类型：Label 可以当 Widget 用
}
```

**拥有式（`release f`）。** 最后一个 Xray 引用释放的那一刻调用 `f`，时机与规范 §16.8 的回收点完全一致。这不是用户可见的析构：只能挂一个 C 函数，不能挂 Xray 代码，`f` 不能回调进 Xray（契约上不允许 `reenters`）。带 `main` 的句柄只会在主线程上释放，因为它到达不了别的线程。

**可失效式（`tracked`）。** 对象的生命周期归 C/C++（父子树、窗口关闭）。shim 为每个对象分配一个小令牌 `{对象指针, 计数}`，对象销毁时把指针清零：Qt 用 `destroyed` 信号，GTK 用 weak ref，wxWidgets 用 `wxTrackable`。Xray 的句柄持有令牌；每次调用先读一次令牌指针，为空就以明确的诊断 panic（"Widget 已被销毁"）。成本是一次读和一次分支。

**收养（`adopt`）。** 参数写 `adopt w: Widget` 表示调用后由 C 一侧拥有 `w`（Qt 的 `setParent`、`addWidget`）。没被收养的可失效句柄在最后一个 Xray 引用释放时调用 `release`（例如 `deleteLater`）；被收养后只做失效检查，不再释放。PySide 的所有权转移规则与此相同。

**环。** 按钮的点击回调捕获按钮自身，是 Xray 闭包和 C++ 对象之间的环。对可失效句柄，回调随 C++ 对象一起释放（`notified` 或随句柄），C++ 对象由父子树决定何时删除，所以环会随窗口关闭而解开。拥有式句柄上注册的回调捕获句柄自身时会形成真正的环，按 Task328 A13 由开发期环检测器报告，用 `weak` 字段断开；不为此增加运行时环收集。

句柄的引用计数在 Xray 一侧，属于普通对象（主线程域里是非原子带），不经过 C。

### 4.4 边界类型

| Xray 参数 | C 一侧 | 成本 | 说明 |
|---|---|---|---|
| 定宽整数、浮点、`bool`、`usize`/`isize` | 对应标量 | 0 | 现状 |
| 绑定了原生布局的 `struct`（按值） | 对应 C 结构体（按值） | 0 | 布局由 manifest 的 `[[native.layout]]` 绑定并做静态断言（规范现有机制）；取消"不按值传聚合"的限制 |
| `ref T`，`T` 是原生布局的标量或结构体 | `T*`，直接指向这个位置 | 0 | 调用期间按 `ref` 的独占借用规则保护；类字段走动态独占检查（规范 §2.14.4）。纯输出参数由包装层用已初始化的局部变量传 `ref`，不再需要 `Buffer` 和 `assumeInitialized` |
| `string`（只读） | `(const char *, size_t)` | 两次读 | 零拷贝；字符串对象本来就是连续的 UTF-8 |
| `CStr` | `const char *`，NUL 结尾 | 一次读和一次分支 | Xray 字符串尾部已有 NUL，零拷贝；字符串内部含 NUL 时 panic（创建字符串时已经扫描 UTF-8，顺便记一位，不额外扫描） |
| `Utf16` | `(const char16_t *, size_t)` | 一次转码 | 转码进线程本地暂存区，热身后不分配；用自己的向量化转码器 |
| `Slice<T>` / `MutSlice<T>` | `(T *, size_t)` | 0 | 按规范 §2.4.2 的借用规则，调用期间不会重新分配 |
| 句柄 | 指针（可失效式先检查令牌） | 0 或一次读和一次分支 | 见 §4.3 |
| 函数类型 | （函数指针，上下文[，销毁通知]） | 见 §4.2 | |
| 返回 `CStr` | 立即复制成 Xray 字符串 | 一次分配 | C 把缓冲区交给调用方时，契约写 `release f`，复制后释放 |

**模板字符串直接进暂存区。** 模板字符串或拼接表达式直接作为只读字符串参数（`string`、`CStr`、`Utf16`）传给 extern 函数时，编译器把它格式化进线程本地的暂存区，不创建字符串对象，也没有引用计数。暂存区是后进先出的：`reenters` 调用里的回调再格式化，会叠在上面，回到调用点时一起弹出。求值顺序不变：各段先按顺序求值，再格式化，再调用。`imgui.text("FPS ${fps}")` 这样每帧几百次的调用因此零分配。这是性能上最重要的一项：§1.7 的字符串基准显示，剩下的差距主要在分配本身。

### 4.5 重入与 panic

**重入。** 契约写 `reenters` 的 extern 调用（Qt 的 setter、`SendMessage`，以及收同步回调的函数）在分析里当作"可能调用任何已注册主线程回调"的未知调用：

- 优化不能跨过它假设堆上的值不变（与调用任意闭包相同）；
- 借用：被闭包以共享单元捕获的变量本来就不能产生视图（规范 §2.4.2 第 5 条）；类字段的 `ref` 借用走动态独占检查（§2.14.4）；把共享单元捕获的变量 `ref` 给 `reenters` 调用时也做动态独占检查。回调里再访问同一个位置，会以明确的诊断 panic，不会出现别名写；
- 不写 `reenters` 的 extern 调用（ImGui、SDL3 的绝大多数函数）不能调用 Xray 回调，生成的 shim 在调试构建里断言这一点。

**panic 不穿过 C 帧。** Task336 规定不能假定 longjmp 或原生展开可以穿过宿主帧。回调蹦床接住函数体里的 panic，存进主线程域的"待抛 panic"槽，按契约给 C 返回默认值，然后返回。最外层的 extern 调用一返回，调用点检查这个槽（一次读和一次分支），不为空就在 Xray 里重新抛出，带着原始位置。宿主循环不返回的情形（`exec()`），适配器的 `drain` 发现有待抛 panic 时请求循环退出，`exec()` 返回后在调用点抛出。C++ 异常由 shim 全部接住，按契约转成错误码，包装层再转成 `throws`。

### 4.6 extern 契约写在签名上（已裁决）

规范 §5.2.9 现在要求每个 extern 声明的完整类型合同写在 `xray.toml` 的 NativePackagePlan 里。本方案把**签名事实**挪回源码，**构建事实**留在 manifest。2026-11-09 用户采用这一放法（P2），模拟与数据见[决策记录](../../../../xray-docs/analysis/341-decision-2-extern-contract-placement-2026-11-09-cn.md)：现有 manifest 契约在解析器之外没有读取者；xray-ports 四个包的 manifest 22,864 行中 99.2% 是从源码签名复述的逐符号样板，954 个契约全部写着 `blocking = "never"`，包括会阻塞的 `pread`；同一组 13 个界面函数，P2 的写法量约为 P1 的八分之一。

连带决定：manifest 的符号映射改为可选，只在 Xray 名与 C 名不同时写（953 个真实符号没有一个改名）；删除 NativePackagePlan 的契约 schema 与解析，xray-ports 的 manifest 去掉契约部分，不留兼容；修饰的默认值在实现时确定，原则是漏写时安全（例如未写的函数按可能阻塞处理）。

| 写在源码 extern 签名上 | 留在 manifest |
|---|---|
| 参数和返回类型、`ref`、句柄、回调种类与生命周期（`notified`、`permanent`）、`adopt`；函数修饰：`safe`、`main`、`reenters`、`blocks` | 库、目标、符号改名、源码与目标文件、编译选项、`lang`（`c`、`c++`、`objc`）、frameworks、pkg-config、哈希、许可证、原生布局绑定 |

理由：这些事实决定调用点能不能编译、有什么效应，是 HIR 和效应检查要读的，也是模块接口摘要（339 M5 的 H6 缓存）的一部分。放在 manifest 里，读的人要在两个文件之间对照，检查器也要从另一个来源取同一个事实，违反"一个事实一个生产者"。

**`safe` 条目（已裁决）。** 块写成 `unsafe extern "C" { … }`，表示声明者对这些契约负责。签名里只用 §4.4 中受检查的类型（不含 `Ptr`、`MutPtr`、`CFn`）的条目可以标 `safe`，调用时不写 `unsafe`。这与 Rust 2024 的 `unsafe extern` 块加 `safe fn` 相同。2026-11-09 用户采用这一做法（U2），证据见[决策记录](../../../../xray-docs/analysis/341-decision-3-safe-extern-entries-2026-11-09-cn.md)：按类型自动判定安全不成立，xray-ports 中 81 个只含标量的 extern 把地址当整数传；现行"每次调用都写 `unsafe`"在 xray-ports 里留下 2,160 个只装 extern 调用的 `unsafe` 块和 898 个只为包住 `unsafe` 的单行包装函数；多一层包装在 VM 下每次调用多约 5.0 ns，原生被内联。规则：

1. 只接受 `unsafe extern "C"`；普通 `extern "C"` 是编译错误，不留兼容。
2. `safe` 条目的签名里不允许出现 `Ptr`、`MutPtr`、`CFn`；这只挡住明显的错误，担保仍靠声明者。
3. `safe` 担保内存安全，并担保在 `main` 契约之外可以从任意线程调用。
4. 非 `safe` 条目的调用仍写在 `unsafe { }` 里。
5. 规范 §16.9.5 的逃逸口改为"`unsafe` 块与 `unsafe extern` 声明"。

好处有两个：生成的绑定可以把 extern 函数直接作为 API 暴露，不必再包一层；VM 调用时少一次解释执行的转发。方法写法（`label.setText(...)`）用下面的方法别名，不需要包装层。

```xray
// 由 xray bind 生成，使用者不直接看到
unsafe extern "C" {
    main handle Widget tracked release qx_delete
    main handle Label: Widget
    main handle PushButton: Widget
    main handle VBox

    main safe fn qx_label_new(text: Utf16) -> Label
    main safe fn qx_label_set_text(w: Label, text: Utf16) reenters
    main safe fn qx_button_new(text: Utf16) -> PushButton
    main safe fn qx_button_on_clicked(w: PushButton, f: escaping main fn() notified)
    main safe fn qx_vbox_add(box: VBox, adopt w: Widget) reenters
}
```

**句柄上的方法别名（第 7 项已裁决，见[决策记录](../../../../xray-docs/analysis/341-decision-7-spellings-and-method-aliases-2026-11-10-cn.md)）。** 规范没有扩展方法或 `impl` 块，方法一律写在类型体里，所以句柄的方法写在 `handle` 的类型体里，用一行别名指向同一 `unsafe extern` 块里已经声明的函数：

```xray
unsafe extern "C" {
    main safe fn qx_label_set_text(w: Label, text: Utf16) reenters
    main safe fn qx_label_text(w: Label) -> string
    main handle Label: Widget {
        setText = qx_label_set_text      // 接收者就是第一个参数，契约照搬原声明
        text = qx_label_text
    }
}

label.setText("${count}")                // 用户代码
```

1. 被指向的函数，第一个参数类型必须是这个句柄类型（或其父类型），它成为方法的接收者。
2. 方法照搬原声明的全部契约：`main`、`safe`、`reenters`、`blocks`，以及参数和返回类型。
3. 句柄子类型继承父类型的方法别名；C++ 的虚函数分派在 shim 里完成。
4. 不写 C 符号字符串，符号映射仍按 §4.6 的规则（只在改名时写进 manifest）。

不用包装类：每个控件会多一个对象，句柄与包装对象各有身份和引用计数，使收养与可失效规则复杂化；VM 下每次调用还多约 5.0 ns。

### 4.7 写起来是什么样

**即时模式（Dear ImGui + SDL3，首发的第一方包）：**

```xray
import "xray/ui/app" as app
import "xray/ui/imgui" as imgui

var speed: f32 = 1.0
var name = "world"
var clicks = 0

app.run("Demo", 800, 600, () -> {             // escaping main fn()：每帧在主线程调用一次
    imgui.window("Controls", () -> {           // 不逃逸闭包：AOT 内联，不分配
        imgui.text("Hello, ${name}! ${app.fps()} FPS")   // 格式化进暂存区，不分配
        imgui.sliderFloat("speed", ref speed, 0.0, 10.0) // ref → float*，直接指向 speed
        imgui.inputText("name", ref name)
        if (imgui.button("Click")) {
            clicks += 1
        }
    })
})
```

`imgui.window` 由包装层用不逃逸闭包保证 `Begin`/`End` 成对。

**保留模式（wxWidgets，第二个官方包）：** 按 Task331 G9 只用显式方法，不用属性；方法来自 §4.6 的方法别名。

```xray
import "xray/wx" as wx

fn download(url: string) -> string {          // 普通函数，可以在任何线程上运行
    // ……
    return "done"
}

const window = wx.Frame("示例", 480, 320)
const label = wx.StaticText(window, "0")
const add = wx.Button(window, "加一")
const fetch = wx.Button(window, "下载")
var count = 0

add.onClicked(() -> {                         // escaping main fn()
    count += 1
    label.setText("${count}")                 // 格式化后转码进 UTF-16 暂存区
})

fetch.onClicked(() -> {                       // 体内有 await：成为主线程任务，先把控制权还给 wx
    fetch.setEnabled(false)
    const t = go download("https://example.com")   // 后台任务
    label.setText(await t)                    // 恢复时仍在主线程
    fetch.setEnabled(true)
})

window.setLayout(wx.column([label, add, fetch]))
window.show()
wx.run()
```

**进度回传：**

```xray
const bar = wx.Gauge(window, 100)            // 进度条；与上例一样加进布局
fetch.onClicked(() -> {
    const progress = Channel<i64>(16)
    const t = go scan("/data", progress)      // scan 每处理一批就 send 一次，结束时 close
    var n = progress.recvOr(-1)
    while (n >= 0) {
        bar.setValue(n)                       // 主线程任务里等 Channel，界面不卡
        n = progress.recvOr(-1)
    }
    label.setText("共 ${await t} 个文件")
})
```

**任意线程回调：**

```xray
const levels = Channel<f32>(64)
audio.start((samples: Slice<f32>) -> {        // escaping sendable fn：在音频线程上调用
    levels.trySend(peak(samples))             // 不能挂起，所以用 trySend；只能碰 Sendable 的值
})
```

**编译器会拒绝的写法：**

```xray
fn refresh() {
    label.setText("x")                        // refresh 推导出 main 效应
}
const t = go refresh()                        // 错误：go 的目标有 main 效应
                                              //   refresh → label.setText → qx_label_set_text（main）

go fn() { label.setText("x") }()              // 错误：Label 不是 Sendable，不能被 go 的闭包捕获

var saved: fn() = () -> label.setText("y")   // 错误：局部变量上的函数类型可以逃逸，闭包有 main 效应，类型应写 main fn()
```

## 14. 裁决

1. **已裁决（2026-11-09）：采用 A**，界面调用并入已有的根执行流效应，带 §4.1 的四项配套；不采用运行时断言（B）和能力令牌（C）。模拟方法、25 个输入与结果见[决策记录](../../../../xray-docs/analysis/341-decision-1-main-thread-model-2026-11-09-cn.md)。
2. **已裁决（2026-11-09）：采用 P2**，签名事实写在 extern 声明上，manifest 只留构建事实，符号映射只在改名时写；删除 manifest 契约 schema，不留兼容。证据见[决策记录](../../../../xray-docs/analysis/341-decision-2-extern-contract-placement-2026-11-09-cn.md)。
3. **已裁决（2026-11-09）：采用 U2**，只接受 `unsafe extern` 块，逐项写 `safe` 的条目调用时不写 `unsafe`；不按类型自动判定安全。证据见[决策记录](../../../../xray-docs/analysis/341-decision-3-safe-extern-entries-2026-11-09-cn.md)。
4. **已裁决（2026-11-09）：采用 H1+**，会挂起的处理函数自动成为主线程任务，加任务归句柄所有、借来的参数挂起后不可用、要返回值的回调不挂起三道护栏；处理函数任务的未捕获 panic 按根任务处理。模拟见[决策记录](../../../../xray-docs/analysis/341-decision-4-suspending-handlers-2026-11-09-cn.md)。
5. **已裁决（2026-11-10）：采用 C1**，C++ 只经生成的 C shim，后端只生成 C；发布构建把 shim 与生成的 C 静态链接并开 ThinLTO（C++ 库本身按其发布形态链接，例如 Qt 仍是动态库），开发构建用动态绑定包。测量见[决策记录](../../../../xray-docs/analysis/341-decision-5-cpp-through-c-shims-2026-11-10-cn.md)。
6. **已裁决（2026-11-10）**：首发第一方包是 Dear ImGui + SDL3（窗口、输入、输入法与渲染都用 SDL3，取代 sokol_app + sokol_gfx）；wxWidgets 定为第二个官方包；Qt6 与 GTK4 只作为生成器的验证对象。实施前用真实中文输入法验收 ImGui 的输入体验。实测见[决策记录](../../../../xray-docs/analysis/341-decision-6-first-party-ui-packages-2026-11-10-cn.md)。
7. **已裁决（2026-11-10）**：保留 `main fn`、`sendable fn`、`safe`、`main`、`reenters`、`blocks`、`handle`、`tracked`、`release`、`adopt`、`notified`、`permanent`、`CStr`、`Utf16`；主线程任务写作 `go(main) f()`，另加 `Task.isMain()`，不新建 `mainthread` 模块；句柄上的方法用类型体里的方法别名（§4.6）。依据见[决策记录](../../../../xray-docs/analysis/341-decision-7-spellings-and-method-aliases-2026-11-10-cn.md)。

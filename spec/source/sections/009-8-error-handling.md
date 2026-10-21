---
id: spec.8_error_handling
order: 009
---

<!-- xr-spec:cn -->
---

## 8. 错误处理 (Error Handling)

> 值语义与单一 XIR 实现入口：`src/xir/xxir_verify.c`、`src/xir/xxir_error_value.inc.c`、`src/xir/xxir_call.c`。catch、效应与 panic/defer 的迁移状态见 Task 316；旧 analyzer/IR/VM 代码作为迁移参考，不构成第二条目标执行管线。

### 8.0 设计哲学：值返回 + panic 边界

Xray 的错误处理分为两个严格分离的通道：

| 通道 | 语法 | 用途 |
|--|--|--|
| 值错误 | `throws(E)`、`throw`、`try`、`catch` | 业务错误与可恢复失败 |
| panic | `catch panic` | 越界、除零等运行时故障 |

- **错误是值**：错误载荷是 enum 值；传播不把它转换为 `PanicInfo`。
- **签名决定错误类型**：省略 `throws` 表示 `Never`，即保证不抛业务错误；`throws(E)` 声明允许逃逸的错误类型，裸 `throws` 等价于 `throws(Error)`。签名和函数类型规则见 §2.2、§5.2。
- **检查是局部的**：每个函数按自身正文和被调用签名检查错误子集，不分析被调函数体、不做全程序错误推导。没有错误子句的具名函数不会自动获得抛错签名；闭包规则见 §5.2。
- **传播点可见**：可能抛错的调用必须在 `try`、`try?`、`try!` 表达式或 `try { }` 的保护区域内（§8.1.6）。
- **panic 不进入签名**：不抛业务错误的函数仍可能 panic；`try?` 不会吞掉 panic。
- **`defer` 替代 `finally`**：清理绑定最近的真实 `{}` 块，清理体必须静态证明没有逃逸的业务错误（`E0387`，§8.3.1）。

本章给出 Task329 的目标语言规则；示例的执行资格以冒烟门结果为准。规范生成通过不代表 `throws`、新 Task 形状或新诊断已经实现。

### 8.1 值返回错误通道

#### 8.1.1 `throw` 语句

`throw expr` 抛出一个枚举错误值。其实际值必须是枚举变体；静态类型可以是具体 enum、内置 `Error`，或定义处已证明 `Error` 约束的普通类型参数：

```xray @check
enum AppErr { NotFound, InvalidInput { message: string } }

throw AppErr.NotFound                       // ✅ 简单枚举变体
throw AppErr.InvalidInput { message: "bad format" } // ✅ 带载荷的 ADT 枚举变体
```

抛出后行为：

```
抛出点 → 交付拥有独立所有权的错误值 → 沿调用栈返回 → 执行每条跨域边上的静态 cleanup 区域 → catch 处理 → 否则继续返回 → 顶层诊断
```

- 不展开栈帧（不同于传统异常的 unwind）
- 正常路径不分配对象、不展开栈；在需传播或捕获错误的调用边界只经过可预测的错误标志分支
- 未捕获的顶层错误打印 `[Uncaught Error] <enum value>`，退出码 = 1

调试器对值返回错误的 caught／uncaught 分类以当前逻辑执行流的同步调用链为边界：选中本流中的 catch 为 caught，错误逃出真实 Instance 根入口或当前 Task 根入口为 uncaught。Task 保存的错误随后经另一执行流 await 再抛出时，在该 await 所在流重新分类；这不把 Task 失败等同于整个程序失败。观察点位于实际 throw 交付、handler 和清理效果执行之前。分类要求完整的 Source 路由事实及经过认证的执行边界；缺失事实或未经认证的 native 边界保持未知，不能据此报告 uncaught。

同源混合调试 Program 必须接收生成器发布的现行观察协议描述符，核对所选 native 回调及帧偏移并拥有复制注册元数据；缺失或旧协议在执行前拒绝。协议同时提供 Source 原点的指令前观察及普通 CALL 准备后的已解析 callee、指令序号和 invoke 位。接收方拥有这些原始事实后，仍须以实际 child ENTER、完整 executor binding、frame、thread 和深度证明调用已获准；不得读取祖先执行状态或借出祖先 CallView。完整认证 Source 路由及原点覆盖下，all、caught 和 uncaught 可沿真实 VM/native 同步链分类；递归使用实际 frame 身份，Task 根边界不跨越到未来 await 的接收者。原点覆盖不完整时拒绝 all，handler 路由不完整时拒绝 caught／uncaught，配置失败均保留旧过滤值。已经准入的 all 遇到缺失 handler 路由时仍报告 UNKNOWN，不猜测未捕获。生成 native 的停点位于原始 Source throw 对应指令执行之前，包括降为局部跳转或词法错误退出的 throw；继续执行时该指令只运行一次，传播返回和内部 dispatcher 不被当作新的 Source throw。拥有型异常快照保留真实 native 帧身份、Source 指令和枚举值，不暴露 native locals 或执行状态，也不保证后续 cleanup 或 handler 一定完成。canonical Source launch 可由受信任 host provider 返回同证明的 native ProgramSpec、现行描述符、选择及 code lease；失败关闭且不回退 VM，接收成功后只有最后真实代码 owner 退休才释放 lease。允许全部选择 native，不要求空闲 VM 条目。restart 重新接收，失败保留旧停点；detach 后的目标由独立 owner 与原 code lease 驱动，adapter 销毁不卸载代码。JSON 栈只投影快照已有的认证 Source 位置，不添加 native locals 或机器地址。native 指令断点、具有 native 初始化器或入口的启动暂停和全 native 的手动暂停请求明确不支持；任意 native 源码单步、动态库 CLI 载入及其完整安全资格仍未授予。

#### 8.1.2 `try` / `catch`

```xray @fragment
enum IOErr { Timeout, Refused { reason: string } }

try {
    connect(host)
} catch (e) {
    match (e) {
        IOErr.Timeout -> log("timeout"),
        IOErr.Refused { reason } -> log("refused: " + reason),
    }
}
```

**执行顺序**：

1. 执行 `try` 块
2. 若有 `throw` 逃出，按声明顺序逐一尝试匹配 `catch` 子句；首个匹配者执行其块体
3. 无 `catch` 匹配时，错误继续沿调用栈向上传播

**类型化 catch 与多 catch 子句**：

`catch` 变量可带类型注解 `catch (e: T)`，运行时用 `is T` 判断枚举类型是否匹配。支持多个 `catch` 子句，按声明顺序匹配：

```xray @fragment
enum NetErr { Timeout, Refused }
enum DbErr { ConnLost, QueryFailed { query: string } }

try {
    riskyIO()
} catch (e: NetErr) {
    log("network:", e)
} catch (e: DbErr) {
    log("database:", e)
} catch (e) {
    log("unexpected:", e)
}
```

**规则**：
- 无类型注解的 `catch (e)` 是 catch-all，匹配所有错误值，绑定的静态类型为内置 `Error`
- 有类型注解的 `catch (e: SomeErr)` 仅当错误值 `is SomeErr` 为真时匹配，其中 `SomeErr` 是具体 enum 错误类型
- 多个 `catch` 子句按声明顺序匹配，首个匹配者执行
- 若所有类型化子句均不匹配且没有 catch-all，错误继续向上传播
- 一个 `try` 语句 **必须**至少跟随 `catch`（普通 `catch` 或 `catch panic`）

内置 `Error` 是只容纳 enum 的持有值类型。enum 到 Error 的值转换保留实际名义身份、泛型实参、活动变体与载荷；复制、保存、返回和重新抛出均遵守相同值所有权规则。Error 不提供任意字段、构造或可见性权限，也不隐式证明 `Sendable`。读取具体变体载荷前必须完成有权限的类型窄化。普通泛型转换在定义处按约束检查，特化后复验，不能等调用处看到具体 enum 才补充证明。

catch 绑定仅在对应 handler 内可见，绑定及其值语义部分只读；需要重新赋值时先用 `var local = e` 创建局部逻辑副本。所有 handler 都接受完整静态检查，即使 try 块没有错误前驱，名称、类型、泛型约束、可见性、初始化和只读错误仍必须拒绝。不可执行的构造器 handler 从 try 入口的初始化状态开始检查，其内部写入不能成为正常后继已初始化的证据；handler 内部分支和循环遵循通常的必定/可能初始化规则。handler 的 return 仍参与所在函数的返回类型推断。

**enum variant pattern catch**：

普通 `catch` 的头部始终写在括号内。括号内是标识符且后跟 `:` 或 `)` 时为绑定（`catch (e)`、`catch (e: T)`），否则为 enum variant pattern：

```xray @fragment
enum DbErr { ConnLost, Timeout, QueryFailed { query: string, code: i64 } }

try {
    runQuery()
} catch (DbErr.QueryFailed { query, code: 500..=599 }) {
    log("server rejected:", query)
} catch (DbErr.ConnLost, DbErr.Timeout) {
    retry()
}
```

- 每个备选都是 `Enum.Variant` 或 `Enum.Variant { 字段 }`，可带模块限定；字段子模式遵循 §6（字面量、范围、嵌套 enum、`_` 与绑定）。payload variant 必须写出花括号，`{}` 只测试 tag；unit variant 不能写 `{}`。
- 逗号分隔的备选必须属于同一个 enum 类型。首个备选确定该 handler 的 enum 类型：Error 不携带静态类型实参，所以泛型 enum 必须在首个备选写全类型实参（如 `Event<string>.Failed { message }`）；其余备选须命名同一声明，省略的类型实参取自该类型，显式写出的必须完全相同。
- 先判断错误值是否属于该 enum 类型，再按源码顺序测试备选。类型、变体、字面量或范围任何一项不匹配时，继续尝试下一个 `catch` 子句；全部不匹配时原错误值不变地继续传播。pattern handler 不要求穷举，也不会产生匹配失败（`E0442`）。
- 各备选必须绑定同名、同静态类型的变量。pattern 绑定只读，仅在该 handler 内可见，与 catch 绑定规则相同。
- 括号内单个标识符是 catch-all 绑定，但大写开头标识符与裸 `_` 除外。单个大写开头标识符如 `catch (NetErr)` 报 `E0334`，提示改成 `catch (e: NetErr)`；它既不是类型过滤，也不建立同名绑定。`_`、字面量、范围，以及元组、对象、数组和 `is` 类型模式都不能作为 catch 头部。
- 普通 `catch` 不能省略头部；只有 `catch panic` 可以省略。
- 泛型 enum 目标在定义处按约束检查。不可执行的 pattern handler 同样完整检查名称、类型、约束、可见性、字段与绑定。

```xray @error=E0334
enum NetErr { Failed }
fn work() throws(NetErr) { throw NetErr.Failed }
fn main() {
    try { work() } catch (NetErr) { print("failed") } // error: use catch (e: NetErr)
}
try main()
```

#### 8.1.3 重抛与错误转换

`catch` 块内可重抛原错误或抛出不同枚举的新错误：

```xray @fragment
enum LowErr { Fail }
enum HighErr { Upstream }

try {
    lowLevelCall()
} catch (e: LowErr) {
    log("low-level failed")
    throw HighErr.Upstream
}
```

#### 8.1.4 错误枚举设计推荐

定义业务错误推荐使用 ADT enum，载荷携带上下文信息：

```xray @fragment
enum HttpErr {
    NotFound { message: string },
    ServerError { code: i64, message: string },
    Timeout,
}

enum ParseErr {
    Empty,
    InvalidChar { value: string, offset: i64 },
    Overflow,
}

fn fetchUser(id: i64) throws(HttpErr) -> User {
    if (id <= 0) { throw HttpErr.NotFound { message: "user not found" } }
    // ...
}

try {
    var user = fetchUser(-1)
} catch (e: HttpErr) {
    match (e) {
        HttpErr.NotFound { message: msg } -> log("404:", msg),
        HttpErr.ServerError { code, message: msg } -> log(string(code) + ":", msg),
        HttpErr.Timeout -> log("timeout"),
    }
}
```

ADT enum 可让 `match` 在编译期检查错因穷举性。

#### 8.1.5 错误的协程边界

`go f(...)` 把 `f` 的返回类型与声明错误类型保存在 `Task<T, E>` 中；省略错误参数时 `E = Never`。启动本身不向调用方传播子任务的业务错误，独立子任务失败也不自动取消父任务。

- `try await t` 等待结果，成功时返回 `T`，业务错误按 `E | Cancelled` 传播；子任务 panic 仍走 panic 通道。
- `t.awaitResult()` 将终态作为四变体 `TaskResult<T, E>` 返回，业务错误不会从该方法重抛；等待者自身的取消仍按取消展开处理。
- 默认 scope 在子任务失败时取消兄弟；isolated 保留兄弟执行，仅把子任务业务错误汇总为 `TaskFailures<E>`。两者都等待全部成员结束后退出，panic 始终走独立通道，不进入汇总。主体错误集合 B 与直接子任务错误集合 E 分别组成 `B | E` 和 `B | TaskFailures<E>`；B、E 都为 `Never` 时，两种 scope 均免除传播标记。`try scope` 只标记退出处传播，不免除主体内调用的标记。详见 §10.7。

```xray @fragment
var task = go fetch(url)
var value = try await task
var outcome = task.awaitResult()
```

等待取消的子任务得到类型化的 `Cancelled`；当前任务自身被取消时执行 `defer` 后展开，不能被任何 catch 捕获，也不写入当前函数签名（§10.4）。

#### 8.1.6 `try` 表达式与局部错误检查

`try expr` 传播表达式的业务错误；`try? expr` 将业务错误转成 `null`；`try! expr` 在业务错误出现时触发携带该错误值的 panic。后三者都不建立 catch 块。

一个标记覆盖右侧整个非赋值表达式中的调用；括号、参数分隔符和语句结束限定范围。`try?` 与 `??` 连用必须写 `(try? expr) ?? fallback`。等待顺序固定为 `try await t`。

```xray @run
enum ParseErr { Empty }
fn parse(s: string) throws(ParseErr) -> i64 {
    if (len(s) == 0) { throw ParseErr.Empty }
    return 42
}
fn forward(s: string) throws(ParseErr) -> i64 { return try parse(s) }
fn main() {
    print(try forward("ok"))
    print((try? parse("")) ?? 8080)
    print(try! parse("ok"))
}
try main()
```
```output
42
8080
42
```

`try { }` 保护区域内的调用视为已标记；区域不延伸到闭包、嵌套函数或该 try 语句自身的 catch handler。嵌套 handler 仍可处于外层 try 的保护区域内。catch 覆盖的错误从保护块的错误集合中扣除，其余错误与 handler 自身的逃逸错误必须包含在所在函数的声明中。`throw` 直接产生错误，不另加 `try`。模块顶层和程序入口 `main` 隐式允许任意错误，但传播调用仍需标记。这项入口许可不免除显式调用点的检查；模块顶层调用可能抛错的入口时仍写 `try main()`。

缺少传播标记和签名未声明错误是两类不同诊断（§18.2 的已登记编号，实现待接通）：

```xray @error=E0330
enum Failure { Bad }
fn fail() throws(Failure) { throw Failure.Bad }
fn main() { fail() } // error: missing try
try main()
```

```xray @error=E0331
enum Failure { Bad }
fn fail() throws(Failure) { throw Failure.Bad }
fn caller() { try fail() } // error: undeclared escaping error
fn main() { caller() }
try main()
```

### 8.2 Panic 通道

#### 8.2.1 什么是 panic

panic 表示**程序 bug 或运行时不变量违背**，不应用于业务逻辑：

- 数组越界访问
- 整数除以零
- 不完整的 match（non-exhaustive）
- 空引用解引用
- 运行时类型断言失败

panic 通过有限的栈展开传播，生成 `PanicInfo` 对象携带堆栈信息。

#### 8.2.2 `catch panic`

`catch panic` 捕获 panic 通道的运行时故障：

```xray @fragment
try {
    var arr: Array<i64> = [1, 2, 3]
    var v = arr[10]                          // 越界 → panic
} catch panic {
    log("runtime fault caught")
}

try {
    var n = 10 / 0                           // 除零 → panic
} catch panic {
    log("division by zero caught")
}
```

**`catch` 与 `catch panic` 可共存**，分别处理两个通道：

```xray @check
enum AppErr { InvalidArg }

fn process(input: i64) throws(AppErr) {
    if (input < 0) { throw AppErr.InvalidArg }
}

fn main() {
    try {
        process(-1)
    } catch (e: AppErr) {
        print("business error")              // 值返回通道
    } catch panic {
        print("runtime fault!")              // panic 通道
    }
}
main()
```

**绑定与作用域规则**：每个 `try` 至多有一个 `catch panic`，因为它无条件匹配该通道。可省略头部，或写 `catch panic (p)` / `catch panic (p: PanicInfo)`；注解必须是 `PanicInfo`，不是运行时类型过滤器。绑定只读且仅在 handler 内可见，复制或返回该引用遵守 class 的身份与所有权规则，不自动获得 `Sendable`。handler 只保护对应的 try 主体，不保护同级 handler；handler 中的新 panic 继续寻找外层处理器。普通 enum 错误不能进入 panic handler，panic 也不能进入普通 catch。

#### 8.2.3 `PanicInfo` 类

`PanicInfo` 现在**仅用于 panic 通道**。运行时故障发生时，VM 自动构造 `PanicInfo` 对象：

```xray @fragment
class PanicInfo {
    message: string             // 人类可读消息（如 "index out of bounds"）
    stack: Array<string>        // 自动捕获的调用栈
    cause: PanicInfo?           // 链式 cause
    code: i64                   // 错误码
    data: JSON.Value            // 附加数据；无数据时为 JSON null

    init(message: string = "", cause: PanicInfo? = null)
    fn toString() -> string
}
```

用户代码一般不直接构造 `PanicInfo`——业务错误用 `throw <enum>`。

**在 `catch panic` 中读取 panic 信息**：`catch panic (p)` 会把 `PanicInfo` 对象绑定到 `p`，可读取 `message`、`code`、`stack` 等字段：

```xray @run
fn main() {
    var arr: Array<i64> = [1, 2, 3]
    try {
        print(arr[10])                       // 越界 → panic
    } catch panic (p) {
        print("message:", p.message)         // array index out of range: 10 (length 3)
        print("code:", p.code)               // 430
    }
}

try main()
```
```output
message: array index out of range: 10 (length 3)
code: 430
```

#### 8.2.4 未捕获 panic 的顶层诊断

未被任何 `catch panic` 捕获的 panic 到达顶层时，打印一行到 stderr 并以退出码 1 结束：

```
[Uncaught Panic] E<code>: <message>
```

例如除零打印 `[Uncaught Panic] E0420: division by zero`，越界打印
`[Uncaught Panic] E0430: array index out of range: 9 (length 3)`。

**后端契约**：VM 与 AOT（`xray build --native`）对同一故障产生**逐字节相同**的可观测结果——退出码、stdout、以及这一行 stderr（错误码 + 消息均取自同一套共享格式化器）。契约内容仅限于此。

**不属于契约的呈现细节**：

- **调用栈**是 opt-in 诊断，默认不打印。设 `XRAY_BACKTRACE=1` 时，VM 在报告行后追加 `Stack trace:` 帧列表；AOT 原生路径不携带 unwind 状态，故不追加。让栈保持默认关闭，正是两个后端默认输出得以一致的原因。
- **颜色**按 stderr 是否为 TTY 决定：交互终端下 `[Uncaught Panic]` 标签为粗红色，管道/重定向时为纯文本。

> 对照值返回通道（§8.1.1）：未捕获的顶层 error 打印 `[Uncaught Error] <enum value>`，退出码同为 1。两个通道的前缀风格一致（`[Uncaught Error]` / `[Uncaught Panic]`）。

#### 8.2.5 清理期间的 panic

panic 可在 `defer` 内由内部 `catch panic` 捕获；一旦逃逸清理边界，立即不可捕获地终止，退出码为 70。正常返回、在途业务错误、在途 panic 和取消清理四种背景遵守同一规则，不只在 double panic 时终止。

终止诊断保留触发 panic 的原始编号和消息，并附清理上下文；已有的在途错误或 panic 另行报告，不覆盖、不吞掉其载荷。普通业务错误不能用新的错误 wrapper 绕过 §8.3.1 的静态规则。违反不抛错签名的坏产物不是合法业务错误传播。

编译期求值使用独立宿主边界：清理 panic 使该编译作业以 `CLEANUP_PANIC` 失败并释放资源，不能退出编译器进程。上述是目标行为；现有执行路径仍需迁移诊断和验证，不能把定稿当作已实现。

### 8.3 `defer` — 资源清理

`defer` 是块作用域的清理语句，在所属块退出时**保证执行**（无论正常结束、`break` / `continue`、`return`、`throw`、还是 panic）。语法见 §4.9。

cleanup 块在退出时读取外部绑定。被读取的可移动 owner 在块退出前不得被 `move` 或返回，否则报 `E0382`；需要注册时值时必须先创建显式副本。这个 owner-lifetime 规则与下述错误通道规则相互独立。

```xray @fragment
fn fetch(url: string) throws(IoErr | FetchErr) -> string {
    var conn = try open(url)
    defer { conn.close() }                   // 无论后续如何，conn 一定关闭

    var data = try conn.read()
    if (len(data) == 0) {
        throw FetchErr.Empty                 // defer 仍执行
    }
    return data
}
```

**规则**：
- `defer` 绑定最近的真实 `{}` 块；函数体顶层 `defer` 仍在函数退出时执行
- 同一块可有多个 `defer`，按 **LIFO** 顺序执行
- `defer` 在块正常结束、`break`、`continue`、`return`、`throw`、panic 展开时均执行
- 循环体中的 `defer` 每轮迭代退出时执行
- `defer` 体不得让错误逃逸（见 §8.3.1）
- `defer` 体不得直接或传递地挂起或创建任务（`E0392`）；动态未知调用在侧效前以 `E0444` 失败关闭
- `return` 及跳出 cleanup 边界的 `break` / `continue` 非法（`E0395`）；cleanup 内部循环的本地跳转合法
- 每个注册点降低为程序点相关的静态 cleanup frontier；不生成闭包、回调对象或动态 defer 栈

#### 8.3.1 `defer` 与错误

`defer` 是资源清理边，不能将业务错误向外传播或替换在途错误。

**规则 D1（静态）**：清理体的逃逸错误类型必须是 `Never`，否则报 `E0387`。检查依据清理体和调用签名；间接函数值的 `throws(E)` 同样参与检查，不以运行时兜底接受未知错误。省略 `throws` 的清理 API 本身保证不抛业务错误。

**规则 D2（满足方式）**：调用不抛错的清理 API，或在清理体内完整处理错误。下面的正例与负例分开给出：

```xray @id=defer-must-not-throw @run
enum CloseErr { Failed }
fn close() throws(CloseErr) { throw CloseErr.Failed }
fn main() {
    defer {
        try { close() } catch (e: CloseErr) { print("close failed") }
    }
    print("body")
}
try main()
```
```output
body
close failed
```

```xray @error=E0387
enum CloseErr { Failed }
fn close() throws(CloseErr) { throw CloseErr.Failed }
fn main() {
    defer { try close() } // error: error escapes cleanup
}
try main()
```

不再定义业务错误逃出 defer 的运行时 D3 兜底。静态不抛业务错误不等于不会 panic；清理期间 panic 的独立边界见 §8.2.5。运行时迁移仍需验证，不由本条静态规则自动证明。

### 8.4 Optional 与错误处理

`T?` 表示语言项 `Optional<T>`，每层有独立的存在标签，不拆成含 null 成员的 union。`T | null` 只在语法边界改写成 `T?`；一般的 `Optional<Optional<T>>` 不自动展平（§2.5）。Optional 表达无错因的有值/无值；需要错因时用 `throws(E)` 与 `throw`。

`try?` 有专门的结果类型规则：操作数类型不是 Optional 时得到 `T?`；已经是 `Optional<U>` 时保持原类型，不增加外层，也不递归展平现有层次。业务错误映射为该结果的 `null`，成功值原样返回。它不捕获 panic，也不捕获取消展开。

```xray @run
enum LookupErr { Failed }
fn lookup(ok: bool) throws(LookupErr) -> i64? {
    if (!ok) { throw LookupErr.Failed }
    return 7
}
fn main() {
    var found: i64? = try? lookup(true)
    print(found ?? 0)
    print((try? lookup(false)) ?? 0)
}
try main()
```
```output
7
0
```

### 8.5 决策树：选择哪种机制

按"**调用方对失败的处理需求**"选择：

```
失败需要被调用方处理吗？
│
├─ 不需要（致命、不可恢复、运行时 bug）
│   ↓
│   panic（由运行时自动触发；catch panic 用于最外层兜底）
│
├─ 需要，且失败有结构化错因
│   ↓
│   throw <enum>，catch 处理（低开销值返回通道）
│
├─ 需要，但失败只表示"没值"，无错因意义
│   ↓
│   T? + ?? / ?.
│
├─ 需要，且函数有 ≥3 种正常状态
│   ↓
│   用户 ADT enum 直接作为返回类型
│
└─ 需要返回多个对等值（不是"成功/失败"二元）
    ↓
    tuple (a, b, ...)
```

完整对照：

| 场景 | 推荐 | 例 |
|--|--|--|
| 业务错误、可恢复失败 | `throw <enum>` + `catch` | `throw ParseErr.Empty` |
| 字典查找、可选字段 | `T?` | `map.get(k) -> Value?` |
| 运行时故障兜底 | `catch panic` | 数组越界、除零 |
| 多分支结果 | enum | `nextEvent() -> NetEvent` |
| 主结果 + 元数据 | tuple | `parse(s) -> (Ast, i64)` |

### 8.6 常用模式

#### 模式 1：enum 错误 + defer 资源清理

```xray @run
enum ConnErr { Refused, Timeout }

// 用一个极简的 "连接" 替身让示例自成一体。
class Conn {
    alive: bool
    init(alive: bool) { this.alive = alive }
    isAlive() -> bool { return this.alive }
    close() { print("closed") }
}

fn fetchData(alive: bool) throws(ConnErr) -> string {
    var conn = Conn(alive)
    defer { conn.close() }             // 无论成功或抛错都会执行
    if (!conn.isAlive()) { throw ConnErr.Timeout }
    return "payload"
}

fn main() {
    try {
        print(fetchData(true))         // => closed 然后 payload
    } catch (e: ConnErr) {
        print("connection error")
    }
}

try main()
```
```output
closed
payload
```

#### 模式 2：throw + catch 用于库 API

```xray @run
enum ConfigErr { Missing { field: string } }

fn requirePort(cfg: JSON.Object) throws(ConfigErr) {
    if (!cfg.containsKey("port")) { throw ConfigErr.Missing { field: "port" } }
    print("port:", cfg["port"])
}

fn main() {
    try {
        requirePort(JSON.parseObject("{\"port\": 8080}"))   // => port: 8080
        requirePort(JSON.parseObject("{}"))                  // 抛出 ConfigErr.Missing
    } catch (e: ConfigErr) {
        match (e) {
            ConfigErr.Missing { field: f } -> print("missing field:", f), // => missing field: port
        }
    }
}

try main()
```
```output
port: 8080
missing field: port
```

#### 模式 3：`??` 提供默认值

```xray @fragment
var port = config?.port ?? 8080
var user = db.findUser(id) ?? guestUser
```

#### 模式 4：catch panic 兜底运行时故障

```xray @check
fn safeDivide(a: i64, b: i64) -> string {
    try {
        return string(a / b)
    } catch panic {
        return "error: division by zero"
    }
}
```

### 8.7 完整可运行示例

以下为自包含的目标语言示例，`output` 给出期望标准输出；是否已支持须由 VM/AOT 冒烟门验证。

#### 示例 1：`throw` / `catch` / `match`

```xray @run
enum ParseErr { Empty, BadChar { value: string } }

fn parseDigit(s: string) throws(ParseErr) -> i64 {
    if (len(s) == 0) { throw ParseErr.Empty }
    if (s == "x") { throw ParseErr.BadChar { value: s } }
    return 42
}

fn main() {
    try {
        print(parseDigit(""))
    } catch (e: ParseErr) {
        match (e) {
            ParseErr.Empty -> print("empty input"),        // => empty input
            ParseErr.BadChar { value: c } -> print("bad char:", c),
        }
    }
}

try main()
```
```output
empty input
```

#### 示例 2：`defer` 的 LIFO 顺序与异常路径

```xray @run
enum E { Boom }

fn work() throws(E) {
    defer { print("defer 1") }
    defer { print("defer 2") }
    print("body")
    throw E.Boom                             // 抛错时 defer 仍会执行
}

fn main() {
    try { work() } catch (e) { print("caught") }
}

try main()
```
```output
body
defer 2
defer 1
caught
```


#### 示例 3：`catch panic` 兜底并读取故障信息

```xray @run
fn main() {
    var arr: Array<i64> = [1, 2, 3]
    try {
        print(arr[10])
    } catch panic (p) {
        print("message:", p.message)         // => message: array index out of range: 10 (length 3)
        print("code:", p.code)               // => code: 430
    }
}

try main()
```
```output
message: array index out of range: 10 (length 3)
code: 430
```
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 8. Error Handling

> Value semantics and single-XIR implementation entry points: `src/xir/xxir_verify.c`, `src/xir/xxir_error_value.inc.c`, and `src/xir/xxir_call.c`. Task 316 tracks catch, effects, and panic/defer migration; the previous analyzer/IR/VM code is migration reference material, not a second target execution pipeline.

### 8.0 Design philosophy: value-return + panic boundary

Xray separates two channels:

| Channel | Syntax | Purpose |
|--|--|--|
| Value errors | `throws(E)`, `throw`, `try`, `catch` | Business errors and recoverable failures |
| Panic | `catch panic` | Runtime faults such as bounds errors and division by zero |

- **Errors are values**: error payloads are enum values; propagation does not turn them into `PanicInfo`.
- **Signatures determine error types**: omitting `throws` means `Never`, a guarantee of no business errors. `throws(E)` declares the permitted escaping error type; bare `throws` means `throws(Error)`. See §2.2 and §5.2 for signatures and callable types.
- **Checking is local**: each function is checked using its body and callee signatures, without inspecting callee bodies or whole-program error inference. A named function without a clause never acquires an inferred throwing signature; see §5.2 for closures.
- **Propagation is visible**: potentially throwing calls must occur within a `try`, `try?`, or `try!` expression or the protected region of `try { }` (§8.1.6).
- **Panic stays outside signatures**: a non-throwing function may still panic; `try?` never absorbs panic.
- **`defer` replaces `finally`**: cleanup belongs to the nearest real `{}` block and must statically have no escaping business errors (`E0387`, §8.3.1).

This chapter specifies the target language rules of Task329. Smoke results determine example execution support; successful document generation does not establish implementation of `throws`, the new Task shape, or the new diagnostics.

### 8.1 Value-return error channel

#### 8.1.1 `throw` statement

`throw expr` raises an enum error value. Its actual value must be an enum variant; its static type may be a concrete enum, the built-in `Error`, or an ordinary type parameter whose `Error` constraint is proved at definition:

```xray @check
enum AppErr { NotFound, InvalidInput { message: string } }

throw AppErr.NotFound                       // ✅ simple enum variant
throw AppErr.InvalidInput { message: "bad format" } // ✅ ADT enum variant with payload
```

After a throw:

```
throw point → deliver an independently owned error value → return up the call stack → run static cleanup regions on every crossed scope edge → catch handles → otherwise keep returning → top-level diagnostic
```

- No stack frame unwinding (unlike traditional exception unwinding)
- No object allocation and no stack unwinding on the happy path; call boundaries that may propagate or catch errors go through only a predictable error-flag branch
- Unhandled top-level errors print `[Uncaught Error] <enum value>`, exit code = 1

Debugger caught/uncaught classification of value-return errors uses the synchronous call chain of the current logical execution flow. Selection of a catch in that flow is caught; escape from the authenticated Instance root or current Task root is uncaught. A stored Task error rethrown by await in another flow is classified again in that awaiting flow. A Task failure therefore does not imply whole-program failure. Observation precedes actual throw delivery, handler execution, and cleanup effects. Classification requires complete Source route facts and authenticated execution boundaries; missing facts or unauthenticated native boundaries remain unknown and cannot establish uncaught.

A same-proof mixed debug Program must receive the generator's current observation descriptor, verify selected native callbacks and frame offsets, and own copied registration metadata; missing or old protocols are rejected before execution. The protocol provides pre-instruction Source origins and resolved callee, instruction ordinal and invoke facts after ordinary CALL preparation. The receiver owns those primitives and still requires actual child ENTER, complete executor binding, frame identity, thread and depth to prove admission. It neither reads ancestor execution state nor lends an ancestor CallView. Complete authenticated Source routes and origin coverage support all, caught and uncaught across the real synchronous VM/native chain. Recursion uses actual frame identities, and Task roots do not cross to a future await receiver. Incomplete origin coverage rejects all; incomplete handler routes reject caught/uncaught. Failed configuration preserves the previous filters. An admitted all filter retains UNKNOWN for a missing handler route rather than guessing uncaught. Generated-native stops precede the instruction for the original Source throw, including local jumps and lexical error exits. Continuation executes that instruction once; propagated returns and the internal dispatcher are not new Source throws. An owned exception snapshot retains native frame identity, Source instruction and enum value without native locals or execution state, and does not guarantee subsequent cleanup or handler completion. Canonical Source launch may receive a same-proof native ProgramSpec, current descriptor, selection and code lease from a trusted host provider. Failures are closed without VM fallback; after successful receiving only the last real code owner releases the lease. All entries may select native without an unused VM entry. Restart receives anew and preserves the old stop on failure. Detached execution uses its independent owner and original code lease after adapter disposal. JSON stacks project only authenticated Source coordinates already in the snapshot, without native locals or machine addresses. Native instruction breakpoints, startup pauses with a native initializer or entry and manual pauses in a native-only program are explicitly unsupported. Arbitrary native source stepping, dynamic-library CLI loading and full safety qualification are not granted.

#### 8.1.2 `try` / `catch`

```xray @fragment
enum IOErr { Timeout, Refused { reason: string } }

try {
    connect(host)
} catch (e) {
    match (e) {
        IOErr.Timeout -> log("timeout"),
        IOErr.Refused { reason } -> log("refused: " + reason),
    }
}
```

**Execution order**:

1. Run the `try` block.
2. If a `throw` escapes, try each `catch` clause in declaration order; the first match runs its body.
3. If no `catch` matches, the error keeps propagating up the call stack.

**Typed catch and multiple catch clauses**:

A `catch` variable may be typed `catch (e: T)`; the runtime uses `is T` to test the enum type. Multiple `catch` clauses are matched in declaration order:

```xray @fragment
enum NetErr { Timeout, Refused }
enum DbErr { ConnLost, QueryFailed { query: string } }

try {
    riskyIO()
} catch (e: NetErr) {
    log("network:", e)
} catch (e: DbErr) {
    log("database:", e)
} catch (e) {
    log("unexpected:", e)
}
```

**Rules**:
- An untyped `catch (e)` is the catch-all and matches any error value; its binding has the built-in static type `Error`.
- A typed `catch (e: SomeErr)` matches only when the error value satisfies `is SomeErr`, where `SomeErr` is a concrete enum error type.
- Multiple `catch` clauses are tried in declaration order; the first match wins.
- If every typed clause fails and there is no catch-all, the error continues propagating.
- A `try` statement **must** be followed by at least one of `catch` or `catch panic`.

The built-in `Error` is an owned value type containing only enums. Converting an enum to Error preserves its concrete nominal identity, generic arguments, active variant, and payload; copying, storing, returning, and rethrowing obey the same value-ownership rules. Error grants no arbitrary field, construction, or visibility authority and does not implicitly prove `Sendable`. Accessing a concrete variant payload requires authorized type narrowing. Ordinary generic conversion is checked against constraints at definition and rechecked after specialization; a concrete enum at a call site cannot supply a missing definition-time proof.

Catch bindings are visible only in their handler, and both the binding and its value-semantic portion are read-only. Create a local logical copy with `var local = e` when rebinding is needed. Every handler undergoes complete static checking even when the try body has no error predecessor: invalid names, types, generic constraints, visibility, initialization, and read-only writes must still be rejected. An unexecutable constructor handler is checked from the initialization state at try entry; writes inside it cannot establish initialization in the normal successor. Branches and loops within the handler follow the ordinary must/may initialization rules. Handler returns still participate in the enclosing function's return-type inference.

**Enum variant pattern catch**:

An ordinary `catch` header is always parenthesized. Inside the parentheses, an identifier followed by `:` or `)` is a binding (`catch (e)`, `catch (e: T)`); anything else is an enum variant pattern:

```xray @fragment
enum DbErr { ConnLost, Timeout, QueryFailed { query: string, code: i64 } }

try {
    runQuery()
} catch (DbErr.QueryFailed { query, code: 500..=599 }) {
    log("server rejected:", query)
} catch (DbErr.ConnLost, DbErr.Timeout) {
    retry()
}
```

- Every alternative is `Enum.Variant` or `Enum.Variant { fields }`, optionally module-qualified; field sub-patterns follow §6 (literals, ranges, nested enums, `_`, and bindings). A payload variant must write its braces, and `{}` tests only the tag; a unit variant cannot use `{}`.
- Comma-separated alternatives must belong to one enum type. The first alternative determines the handler's enum type. An Error carries no static type arguments, so a generic enum must state all of its type arguments there (for example `Event<string>.Failed { message }`); later alternatives must name the same declaration, take omitted type arguments from that type, and must repeat explicit ones exactly.
- The handler first tests whether the error value has that enum type, then tries the alternatives in source order. A type, variant, literal, or range mismatch continues with the next `catch` clause; if none matches, the original error keeps propagating unchanged. A pattern handler needs no exhaustiveness and never raises a match failure (`E0442`).
- All alternatives must bind the same names with exactly the same static types. Pattern bindings are read-only and visible only in their handler, like a catch binding.
- A single identifier is a catch-all binding except for uppercase-leading names and bare `_`. A single uppercase-leading identifier such as `catch (NetErr)` reports `E0334` with a suggestion to write `catch (e: NetErr)`; it is neither a type filter nor a binding. `_`, literals, ranges, tuples, objects, arrays and `is` type patterns are not catch heads.
- An ordinary `catch` cannot omit its header; only `catch panic` may.
- Generic enum targets are checked against their constraints at definition. An unexecutable pattern handler still receives complete name, type, constraint, visibility, field, and binding checks.

```xray @error=E0334
enum NetErr { Failed }
fn work() throws(NetErr) { throw NetErr.Failed }
fn main() {
    try { work() } catch (NetErr) { print("failed") } // error: use catch (e: NetErr)
}
try main()
```

#### 8.1.3 Rethrowing and error conversion

A `catch` block may rethrow the original error or throw a different enum variant:

```xray @fragment
enum LowErr { Fail }
enum HighErr { Upstream }

try {
    lowLevelCall()
} catch (e: LowErr) {
    log("low-level failed")
    throw HighErr.Upstream
}
```

#### 8.1.4 Recommended enum error design

Define business errors as ADT enums with context-carrying payloads:

```xray @fragment
enum HttpErr {
    NotFound { message: string },
    ServerError { code: i64, message: string },
    Timeout,
}

enum ParseErr {
    Empty,
    InvalidChar { value: string, offset: i64 },
    Overflow,
}

fn fetchUser(id: i64) throws(HttpErr) -> User {
    if (id <= 0) { throw HttpErr.NotFound { message: "user not found" } }
    // ...
}

try {
    var user = fetchUser(-1)
} catch (e: HttpErr) {
    match (e) {
        HttpErr.NotFound { message: msg } -> log("404:", msg),
        HttpErr.ServerError { code, message: msg } -> log(string(code) + ":", msg),
        HttpErr.Timeout -> log("timeout"),
    }
}
```

ADT enums allow `match` to check exhaustiveness at compile time.

#### 8.1.5 Errors and coroutine boundaries

`go f(...)` records the return and declared error types of `f` in `Task<T, E>`; the default error argument is `Never`. Spawning does not propagate the child's business errors to the caller, and an independent child failure does not automatically cancel its parent.

- `try await t` waits for `T` and propagates business errors as `E | Cancelled`; child panic stays on the panic channel.
- `t.awaitResult()` returns the four-variant `TaskResult<T, E>` without rethrowing business errors. Cancellation of the waiter itself still unwinds that waiter.
- Default scope cancels siblings on child failure; isolated scope lets siblings continue and aggregates only child business errors as `TaskFailures<E>`. Both drain all members before exit. Panic always uses its independent channel and never enters the aggregate. Body errors B and direct-child errors E form `B | E` and `B | TaskFailures<E>` respectively; both forms need no propagation marker when B and E are `Never`. `try scope` marks exit propagation without marking calls in its body. See §10.7.

```xray @fragment
var task = go fetch(url)
var value = try await task
var outcome = task.awaitResult()
```

Waiting on a cancelled child produces typed `Cancelled`. Cancellation of the current task runs its defers and unwinds; no catch can intercept it and it is not part of that function's error signature (§10.4).

#### 8.1.6 `try` expressions and local error checking

`try expr` propagates business errors; `try? expr` converts them to `null`; `try! expr` turns a business error into a panic carrying that error value. None of these expressions creates a catch block.

One marker covers calls in the entire following non-assignment expression. Parentheses, argument separators and statement endings bound its extent. Combine `try?` with `??` as `(try? expr) ?? fallback`. Marked waits always use `try await t`.

```xray @run
enum ParseErr { Empty }
fn parse(s: string) throws(ParseErr) -> i64 {
    if (len(s) == 0) { throw ParseErr.Empty }
    return 42
}
fn forward(s: string) throws(ParseErr) -> i64 { return try parse(s) }
fn main() {
    print(try forward("ok"))
    print((try? parse("")) ?? 8080)
    print(try! parse("ok"))
}
try main()
```
```output
42
8080
42
```

Calls in the protected `try { }` region count as marked. The region excludes closure bodies, nested functions and that try statement's own catch handlers. A nested handler can still belong to an outer try's protected region. Subtract the errors covered by catch clauses; the remainder and errors escaping handlers must fit the enclosing function's declaration. `throw` produces an error directly and needs no extra `try`. Module top level and the program entry `main` implicitly permit any error but still require propagation markers. This entry permission does not exempt explicit calls; a module-level call to a potentially throwing entry still uses `try main()`.

A missing marker and an undeclared escaping error are distinct diagnostics (registered numbers in §18.2, with implementation pending):

```xray @error=E0330
enum Failure { Bad }
fn fail() throws(Failure) { throw Failure.Bad }
fn main() { fail() } // error: missing try
try main()
```

```xray @error=E0331
enum Failure { Bad }
fn fail() throws(Failure) { throw Failure.Bad }
fn caller() { try fail() } // error: undeclared escaping error
fn main() { caller() }
try main()
```

### 8.2 Panic channel

#### 8.2.1 What is a panic

A panic represents a **program bug or runtime invariant violation**, not business logic:

- Array out-of-bounds access
- Integer division by zero
- Non-exhaustive match
- Null reference dereference
- Runtime type assertion failure

Panics propagate via limited stack unwinding and generate `PanicInfo` objects with stack traces.

#### 8.2.2 `catch panic`

`catch panic` catches runtime faults from the panic channel:

```xray @fragment
try {
    var arr: Array<i64> = [1, 2, 3]
    var v = arr[10]                          // OOB → panic
} catch panic {
    log("runtime fault caught")
}

try {
    var n = 10 / 0                           // division by zero → panic
} catch panic {
    log("division by zero caught")
}
```

**`catch` and `catch panic` can coexist**, handling both channels:

```xray @check
enum AppErr { InvalidArg }

fn process(input: i64) throws(AppErr) {
    if (input < 0) { throw AppErr.InvalidArg }
}

fn main() {
    try {
        process(-1)
    } catch (e: AppErr) {
        print("business error")              // value-return channel
    } catch panic {
        print("runtime fault!")              // panic channel
    }
}
main()
```

**Binding and scope rules**: A `try` has at most one `catch panic`, since it matches that channel unconditionally. Its header may be omitted or written as `catch panic (p)` / `catch panic (p: PanicInfo)`; the annotation must be `PanicInfo`, not a runtime type filter. The binding is read-only and scoped to the handler. Copying or returning the reference follows class identity and ownership rules and does not grant `Sendable`. A handler protects its try body, not sibling handlers; a new panic in a handler searches the enclosing handlers. Ordinary enum errors never enter a panic handler, and panics never enter ordinary catch handlers.

#### 8.2.3 The `PanicInfo` class

`PanicInfo` is now **used only by the panic channel**. The VM constructs `PanicInfo` objects automatically on runtime faults:

```xray @fragment
class PanicInfo {
    message: string             // human-readable message (e.g. "index out of bounds")
    stack: Array<string>        // automatically captured call stack
    cause: PanicInfo?           // chained cause
    code: i64                   // error code
    data: JSON.Value            // additional data; JSON null when absent

    init(message: string = "", cause: PanicInfo? = null)
    fn toString() -> string
}
```

User code generally does not construct `PanicInfo` directly — use `throw <enum>` for business errors.

**Reading panic details in `catch panic`**: `catch panic (p)` binds the `PanicInfo` object to `p`, so you can read `message`, `code`, `stack`, and the other fields:

```xray @run
fn main() {
    var arr: Array<i64> = [1, 2, 3]
    try {
        print(arr[10])                       // out of bounds → panic
    } catch panic (p) {
        print("message:", p.message)         // array index out of range: 10 (length 3)
        print("code:", p.code)               // 430
    }
}

try main()
```
```output
message: array index out of range: 10 (length 3)
code: 430
```

#### 8.2.4 Top-level diagnostic for an uncaught panic

A panic that reaches the top level uncaught by any `catch panic` prints one line
to stderr and exits with code 1:

```
[Uncaught Panic] E<code>: <message>
```

For example, division by zero prints `[Uncaught Panic] E0420: division by zero`,
and an out-of-bounds write prints
`[Uncaught Panic] E0430: array index out of range: 9 (length 3)`.

**Backend contract**: the VM and AOT (`xray build --native`) produce
**byte-identical** observable results for the same fault — exit code, stdout, and
this stderr line (both the code and the message come from the same shared
formatters). The contract covers exactly that surface.

**Presentation details, outside the contract**:

- The **stack trace** is an opt-in diagnostic, off by default. With
  `XRAY_BACKTRACE=1` the VM appends a `Stack trace:` frame list after the report
  line; the AOT native path carries no unwind state and appends nothing. Keeping
  the trace off by default is exactly what lets the two backends' default output
  agree.
- **Colour** follows whether stderr is a TTY: the `[Uncaught Panic]` tag is bold
  red on an interactive terminal and plain text when piped or redirected.

> Compare the value-return channel (§8.1.1): an uncaught top-level error prints
> `[Uncaught Error] <enum value>` and likewise exits 1. The two channels share
> the `[Uncaught Error]` / `[Uncaught Panic]` prefix style.

#### 8.2.5 Panic during cleanup

A panic may be caught by a handler inside its `defer`. Once it escapes the cleanup boundary, execution terminates immediately and uncatchably with exit status 70. The same rule applies during normal return, a business error in flight, another panic in flight, or cancellation cleanup; termination is not limited to double panic.

The terminal diagnostic preserves the triggering panic's original code and message and adds cleanup context. Any error or panic already in flight is reported separately without replacing or discarding its payload. A new business-error wrapper cannot bypass the static rule in §8.3.1. Malformed artifacts violating a non-throwing signature are not valid business-error propagation.

Compile-time evaluation has an independent host boundary: a cleanup panic fails that compilation job with `CLEANUP_PANIC` and releases its resources without terminating the compiler process. This is target behavior; the existing execution path still needs diagnostic migration and validation.

### 8.3 `defer` — resource cleanup

`defer` is a block-scoped cleanup statement guaranteed to run when the owning block exits (whether by fallthrough, `break` / `continue`, `return`, `throw`, or panic). Syntax: see §4.9.

A cleanup block reads its outer bindings at exit. A movable owner read by the block may not be moved or returned before the block exits (`E0382`); code that needs a registration-time value must create an explicit copy first. This owner-lifetime rule is independent of the error-channel rules below.

```xray @fragment
fn fetch(url: string) throws(IoErr | FetchErr) -> string {
    var conn = try open(url)
    defer { conn.close() }                   // conn is guaranteed to close

    var data = try conn.read()
    if (len(data) == 0) {
        throw FetchErr.Empty                 // defer still runs
    }
    return data
}
```

**Rules**:
- `defer` belongs to the nearest real `{}` block; a top-level function-body `defer` still runs when the function exits
- Multiple `defer`s in the same block run in **LIFO** order
- `defer` executes on block fallthrough, `break`, `continue`, `return`, `throw`, and panic unwinding
- A `defer` in a loop body runs as each iteration exits
- No error may escape a `defer` body (see §8.3.1)
- A `defer` body must not directly or transitively suspend or create a task (`E0392`); an unresolved dynamic call fails closed with `E0444` before the side effect
- `return` and any `break` / `continue` that crosses the cleanup boundary are illegal (`E0395`); a local jump within a cleanup-owned loop is legal
- Each registration point lowers to a program-point-sensitive static cleanup frontier; no closure, callback object, or dynamic defer stack is generated

#### 8.3.1 `defer` and errors

A `defer` is a cleanup edge. It cannot propagate business errors or replace an error already in flight.

**Rule D1 (static)**: the escaping error type of the cleanup body must be `Never`, otherwise report `E0387`. Checking uses the cleanup body and callee signatures, including `throws(E)` on indirect callable values. Unknown errors are not accepted through a runtime fallback. A cleanup API without `throws` guarantees no business errors.

**Rule D2 (satisfaction)**: use non-throwing cleanup APIs or handle their errors completely inside the cleanup. The valid and invalid examples are separate:

```xray @id=defer-must-not-throw @run
enum CloseErr { Failed }
fn close() throws(CloseErr) { throw CloseErr.Failed }
fn main() {
    defer {
        try { close() } catch (e: CloseErr) { print("close failed") }
    }
    print("body")
}
try main()
```
```output
body
close failed
```

```xray @error=E0387
enum CloseErr { Failed }
fn close() throws(CloseErr) { throw CloseErr.Failed }
fn main() {
    defer { try close() } // error: error escapes cleanup
}
try main()
```

There is no runtime D3 fallback for escaping business errors. Statically non-throwing does not mean panic-free; the independent cleanup-panic boundary is specified in §8.2.5. Runtime migration still needs validation and is not proved by this static rule.

### 8.4 Optional and error handling

`T?` denotes the language item `Optional<T>`; every layer has an independent presence tag, without decomposing into a union with null. `T | null` rewrites to `T?` only at the syntax boundary. Ordinary `Optional<Optional<T>>` does not flatten (§2.5). Use Optional for presence or absence without a failure reason; use `throws(E)` and `throw` when the reason matters.

`try?` has a specific result rule: a non-Optional operand of type `T` yields `T?`; an operand already typed `Optional<U>` keeps that type, without adding an outer layer or recursively flattening existing layers. Business errors become `null` and successful values are preserved. It catches neither panic nor cancellation unwinding.

```xray @run
enum LookupErr { Failed }
fn lookup(ok: bool) throws(LookupErr) -> i64? {
    if (!ok) { throw LookupErr.Failed }
    return 7
}
fn main() {
    var found: i64? = try? lookup(true)
    print(found ?? 0)
    print((try? lookup(false)) ?? 0)
}
try main()
```
```output
7
0
```

### 8.5 Decision tree: which mechanism to choose

Choose by "**how the caller has to handle the failure**":

```
Does the caller need to handle the failure?
│
├─ No (fatal / unrecoverable / program bug)
│   ↓
│   panic (triggered automatically by the runtime; catch panic for outer fallback)
│
├─ Yes, with structured causes
│   ↓
│   throw <enum>, catch to handle (low-overhead value-return channel)
│
├─ Yes, but the failure simply means "no value"
│   ↓
│   T? + ?? / ?.
│
├─ Yes, and the function has ≥3 normal states
│   ↓
│   User ADT enum directly as the return type
│
└─ Yes, returning multiple co-equal values (not success/failure)
    ↓
    tuple (a, b, ...)
```

Reference table:

| Case | Recommended | Example |
|--|--|--|
| Business errors, recoverable failures | `throw <enum>` + `catch` | `throw ParseErr.Empty` |
| Map lookup, optional fields | `T?` | `map.get(k) -> Value?` |
| Runtime fault fallback | `catch panic` | Array OOB, division by zero |
| Multi-branch result | enum | `nextEvent() -> NetEvent` |
| Primary result + metadata | tuple | `parse(s) -> (Ast, i64)` |

### 8.6 Common patterns

#### Pattern 1: enum errors + defer for resource cleanup

```xray @run
enum ConnErr { Refused, Timeout }

// A tiny stand-in "connection" so the example runs on its own.
class Conn {
    alive: bool
    init(alive: bool) { this.alive = alive }
    isAlive() -> bool { return this.alive }
    close() { print("closed") }
}

fn fetchData(alive: bool) throws(ConnErr) -> string {
    var conn = Conn(alive)
    defer { conn.close() }             // runs whether we succeed or throw
    if (!conn.isAlive()) { throw ConnErr.Timeout }
    return "payload"
}

fn main() {
    try {
        print(fetchData(true))         // => closed, then payload
    } catch (e: ConnErr) {
        print("connection error")
    }
}

try main()
```
```output
closed
payload
```

#### Pattern 2: throw + catch for library APIs

```xray @run
enum ConfigErr { Missing { field: string } }

fn requirePort(cfg: JSON.Object) throws(ConfigErr) {
    if (!cfg.containsKey("port")) { throw ConfigErr.Missing { field: "port" } }
    print("port:", cfg["port"])
}

fn main() {
    try {
        requirePort(JSON.parseObject("{\"port\": 8080}"))   // => port: 8080
        requirePort(JSON.parseObject("{}"))                  // throws ConfigErr.Missing
    } catch (e: ConfigErr) {
        match (e) {
            ConfigErr.Missing { field: f } -> print("missing field:", f), // => missing field: port
        }
    }
}

try main()
```
```output
port: 8080
missing field: port
```

#### Pattern 3: `??` for default values

```xray @fragment
var port = config?.port ?? 8080
var user = db.findUser(id) ?? guestUser
```

#### Pattern 4: catch panic for runtime fault fallback

```xray @check
fn safeDivide(a: i64, b: i64) -> string {
    try {
        return string(a / b)
    } catch panic {
        return "error: division by zero"
    }
}
```

### 8.7 Worked Examples

These self-contained target-language examples state expected stdout in `output` blocks; VM/AOT smoke gates must establish execution support.

#### Example 1: `throw` / `catch` / `match`

```xray @run
enum ParseErr { Empty, BadChar { value: string } }

fn parseDigit(s: string) throws(ParseErr) -> i64 {
    if (len(s) == 0) { throw ParseErr.Empty }
    if (s == "x") { throw ParseErr.BadChar { value: s } }
    return 42
}

fn main() {
    try {
        print(parseDigit(""))
    } catch (e: ParseErr) {
        match (e) {
            ParseErr.Empty -> print("empty input"),        // => empty input
            ParseErr.BadChar { value: c } -> print("bad char:", c),
        }
    }
}

try main()
```
```output
empty input
```

#### Example 2: `defer` order (LIFO) on the error path

```xray @run
enum E { Boom }

fn work() throws(E) {
    defer { print("defer 1") }
    defer { print("defer 2") }
    print("body")
    throw E.Boom                             // defers still run when throwing
}

fn main() {
    try { work() } catch (e) { print("caught") }
}

try main()
```
```output
body
defer 2
defer 1
caught
```


#### Example 3: `catch panic` with fault details

```xray @run
fn main() {
    var arr: Array<i64> = [1, 2, 3]
    try {
        print(arr[10])
    } catch panic (p) {
        print("message:", p.message)         // => message: array index out of range: 10 (length 3)
        print("code:", p.code)               // => code: 430
    }
}

try main()
```
```output
message: array index out of range: 10 (length 3)
code: 430
```
<!-- /xr-spec:en -->

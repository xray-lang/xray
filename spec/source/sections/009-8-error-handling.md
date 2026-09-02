---
id: spec.8_error_handling
order: 009
---

<!-- xr-spec:cn -->
---

## 8. 错误处理 (Error Handling)

> 真值源：`src/frontend/analyzer/xanalyzer_errorset.c`、`src/ir/xi_lower_stmt.c`、`src/vm/xvm_dispatch_exception.inc.c`、`src/runtime/object/xpanic_info.c`、`stdlib/types/panic_info.xr`。

### 8.0 设计哲学：值返回 + panic 边界

Xray 的错误处理分为两个严格分离的通道：

| 通道 | 语法 | 适用场景 | 运行时开销 |
|--|--|--|--|
| **值返回通道**（`throw <enum>` / `try` / `catch`） | 业务错误、可恢复失败 | **低开销**（不分配 `PanicInfo`、不 unwind；需传播/捕获错误的调用边界只有可预测分支） |
| **panic 通道**（`catch panic`） | 运行时故障（越界、除零、不完整 match） | 有限栈展开 |

设计原则：

- **错误是值**：`throw <enum>` 把枚举值写入返回通道，不展开栈、不分配 PanicInfo 对象。
- **panic 不是错误**：panic 表示程序 bug 或运行时不变量违背，不应用于业务逻辑。
- **函数签名不标 `throws`**：xray 不引入 Java/Swift 的受检异常语义。错误通过 throw/catch 值返回通道处理。
- **错误集合不进入函数类型**：具体错误 enum/variant 集合仍由 analyzer effect database 维护；函数类型只携带内部三态 throw-effect bit（`UNKNOWN` / `MAY_THROW` / `NO_THROW`），供安全约束和构造性代码生成消费。
- **no-throw 始终推导**：需要冻结 no-throw 保证时使用 `xray verify` 合同；未知或不完整证明按 may-throw 处理。
- **`defer` 替代 `finally`**：xray 没有 `finally` 关键字，资源清理统一用**块作用域**的 `defer`（绑定最近的真实 `{}` 块，见 §4.9 / §8.3）。
- **清理边不是错误传播边**：`defer` 体不得让错误逃逸，该约束由编译期规则强制（`E0387`），见 §8.3.1。

### 8.1 值返回错误通道

#### 8.1.1 `throw` 语句

`throw expr` 抛出一个枚举错误值。`expr` 必须是枚举类型的变体值：

```xray
enum AppErr { NotFound, InvalidInput { message: string } }

throw AppErr.NotFound                       // ✅ 简单枚举变体
throw AppErr.InvalidInput { message: "bad format" } // ✅ 带载荷的 ADT 枚举变体
```

抛出后行为：

```
抛出点 → 写入 pending_error → 沿调用栈返回 → 执行每条跨域边上的静态 cleanup 区域 → catch 处理 → 否则继续返回 → 顶层诊断
```

- 不展开栈帧（不同于传统异常的 unwind）
- 正常路径不分配对象、不展开栈；在需传播或捕获错误的调用边界只经过可预测的错误标志分支
- 未捕获的顶层错误打印 `[Uncaught Error] <enum value>`，退出码 = 1

#### 8.1.2 `try` / `catch`

```xray
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

```xray
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
- 无类型注解的 `catch (e)` 是 catch-all，匹配所有错误值
- 有类型注解的 `catch (e: SomeErr)` 仅当错误值 `is SomeErr` 为真时匹配，其中 `SomeErr` 是具体 enum 错误类型
- 多个 `catch` 子句按声明顺序匹配，首个匹配者执行
- 若所有类型化子句均不匹配且没有 catch-all，错误继续向上传播
- 一个 `try` **必须**至少跟随 `catch`（普通 `catch` 或 `catch panic`）

`catch` 也可以直接使用 enum variant pattern。unit variant 写作
`catch NetErr.Timeout { ... }`，其中唯一一对花括号是 catch 块体；payload variant 写作
`catch DbErr.QueryFailed { query } { ... }`，第一对花括号是具名 pattern，第二对是块体。
这个边界只由 brace-group 数量决定，不查询 variant schema，也不允许 unit pattern 写 `{}`。

#### 8.1.3 重抛与错误转换

`catch` 块内可重抛原错误或抛出不同枚举的新错误：

```xray
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

```xray
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

fn fetchUser(id: i64) -> User {
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

值返回通道的错误**不自动跨协程传播**。子协程内未捕获的错误：

- 子协程终止，错误记录到 `coro.error`
- 父协程**不**自动感知

传递子协程错误的方式：

1. **Channel 显式传递**：

```xray
enum WorkerErr { Failed { message: string } }
const err_ch = Channel<string>(1)

go fn() {
    try {
        riskyWork()
        err_ch.send("ok")
    } catch (e) {
        err_ch.send("error")
    }
}()

var result = match (err_ch.recv()) {
    Recv.Value { value: v } -> v
    _ -> "error"
}
if (result != "ok") { log("worker failed") }
```

2. **`linked go call()`**（见 §10.2）：独立启动的子 Task 失败会取消其父 Task 与关联子树；调用方仍可通过返回的 Task 观察精确错误。

3. **结构化并发 `linked scope`**（推荐，见 §10.5）：scope 内任一子协程的错误自动传播给父 scope，按正确的通道路由（值返回通道的 enum 错误通过 `catch` 捕获）。

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

```xray
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

```xray
enum AppErr { InvalidArg }

try {
    process(input)
} catch (e: AppErr) {
    log("business error:", e)                // 值返回通道
} catch panic {
    log("runtime fault!")                    // panic 通道
}
```

#### 8.2.3 `PanicInfo` 类

`PanicInfo` 现在**仅用于 panic 通道**。运行时故障发生时，VM 自动构造 `PanicInfo` 对象：

```xray
class PanicInfo {
    message: string             // 人类可读消息（如 "index out of bounds"）
    stack: Array<string>        // 自动捕获的调用栈
    cause: PanicInfo?           // 链式 cause
    code: i64                   // 错误码
    data: JSON.Value            // 附加数据；无数据时为 JSON null

    constructor(message: string = "", cause: PanicInfo? = null)
    fn toString() -> string
}
```

用户代码一般不直接构造 `PanicInfo`——业务错误用 `throw <enum>`。

**在 `catch panic` 中读取 panic 信息**：`catch panic (p)` 会把 `PanicInfo` 对象绑定到 `p`，可读取 `message`、`code`、`stack` 等字段：

```xray
fn main() {
    var arr: Array<i64> = [1, 2, 3]
    try {
        print(arr[10])                       // 越界 → panic
    } catch panic (p) {
        print("message:", p.message)         // array index out of range: 10 (length 3)
        print("code:", p.code)               // 430
    }
}

main()
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

### 8.3 `defer` — 资源清理

`defer` 是块作用域的清理语句，在所属块退出时**保证执行**（无论正常结束、`break` / `continue`、`return`、`throw`、还是 panic）。语法见 §4.9。

cleanup 块在退出时读取外部绑定。被读取的可移动 owner 在块退出前不得被 `move` 或返回，否则报 `E0382`；需要注册时值时必须先创建显式副本。这个 owner-lifetime 规则与下述错误通道规则相互独立。

```xray
fn fetch(url: string) -> string {
    var conn = open(url)
    defer { conn.close() }                   // 无论后续如何，conn 一定关闭

    var data = conn.read()
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

`defer` 是**资源清理边**，不是错误传播边。清理路径失败意味着资源状态已不可知，因此语言既不允许清理错误覆盖在途错误（Go 模型），也不允许静默吞掉它。

**规则 D1（静态，规范性）**：若 `defer` 块的推断逃逸错误集**非空**，编译器报 `E0387`。

与 §8.0 的 throw-effect bit 不同，D1 **不是** fail-closed：xray 没有用户可书写的 no-throw 标注，若"无法证明不抛"即报错，作者面对间接调用、高阶内建方法、尚未登记契约的原生成员时将无从消解。无法证明的那部分交给规则 D3 的运行时兜底——这正是分层的意义。若将来引入用户可写的 no-throw 标注，D1 可收紧为"必须被证明"，D3 随之变为构造上不可达。

```xray @id=defer-must-not-throw
fn close(c: Conn) { throw IoErr.Closed }

fn bad(c: Conn) {
    defer { close(c) }                       // ❌ E0387：错误会逃出 cleanup
}

fn good(c: Conn) {
    defer {                                  // ✅ 在 defer 体内自行处理
        try { close(c) } catch (e) { log.warn("close failed") }
    }
}
```

**规则 D2（满足方式）**：在 `defer` 体内用 `try` / `catch` 消化错误，或调用不抛错的清理 API。这是唯一合法形式——它强制作者回答"清理失败了怎么办"。

**规则 D3（运行时兜底，规范性）**：若错误仍从 `defer` 体逃逸——D1 未能静态判定的间接调用，或 `defer` 体内发生的 panic——运行时以 `E0443` **终止进程**，退出码 `70`：

- 该终止**不可捕获**——`catch` 与 `catch panic` 都不拦截它。在 `defer` 体**内部**被自己 `catch` 住的错误或 panic 不算逃逸，属规则 D2 的正常写法。
- 在途错误既不被替换也不被抑制；终止诊断报告逃逸的错误，并在存在在途错误时一并报告（枚举错误值不携带 message，此时显示 `<no message>`）。
- VM 与 AOT 后端在此行为上必须逐字一致（含退出码与诊断文本）。

**为什么不采用 Go 的"取代"语义**：Go 的 defer 要改写错误必须显式赋值给具名返回值，是可见的、局部的。xray 的函数签名不标 `throws`、调用点也无标记（§8.0），隐式的错误替换在源码上将完全不可见，且多个 `defer` 同时抛出时还需要一套替换链规则。fail-fast 是唯一既有定义、又不隐藏信息、又不引入额外规则的选项。

### 8.4 Optional 与错误处理

`T?` 是 `T | null` 的语法糖，用于"二态：有值/无值"场景。详见 §2.5。与错误处理的关系：

- **不带错因的失败**：用 `T?`（如字典查找返回 `null` 表示"无此键"）
- 与 `??`（默认值）/ `?.`（可选链）/ `e!`（强制解包）配合
- 不要把 `T?` 当通用错误返回——需要错因时用 `throw <enum>` + `catch`

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

```xray
enum ConnErr { Refused, Timeout }

// 用一个极简的 "连接" 替身让示例自成一体。
class Conn {
    alive: bool
    constructor(alive: bool) { this.alive = alive }
    isAlive() -> bool { return this.alive }
    close() { print("closed") }
}

fn fetchData(alive: bool) -> string {
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

main()
```

#### 模式 2：throw + catch 用于库 API

```xray
enum ConfigErr { Missing { field: string } }

fn requirePort(cfg: JSON.Object) {
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

main()
```

#### 模式 3：`??` 提供默认值

```xray
var port = config?.port ?? 8080
var user = db.findUser(id) ?? guestUser
```

#### 模式 4：catch panic 兜底运行时故障

```xray
fn safeDivide(a: i64, b: i64) -> string {
    try {
        return string(a / b)
    } catch panic {
        return "error: division by zero"
    }
}
```

### 8.7 完整可运行示例

以下均为自包含、可直接运行并通过 `xray check` 验证的完整程序（注释标注了真实输出）。

#### 示例 1：`throw` / `catch` / `match`

```xray
enum ParseErr { Empty, BadChar { value: string } }

fn parseDigit(s: string) -> i64 {
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

main()
```

#### 示例 2：`defer` 的 LIFO 顺序与异常路径

```xray
enum E { Boom }

fn work() {
    defer { print("defer 1") }
    defer { print("defer 2") }
    print("body")
    throw E.Boom                             // 抛错时 defer 仍会执行
}

fn main() {
    try { work() } catch (e) { print("caught") }
}

main()
```

输出（`defer` 按 LIFO 逆序执行）：

```
body
defer 2
defer 1
caught
```

#### 示例 3：`catch panic` 兜底并读取故障信息

```xray
fn main() {
    var arr: Array<i64> = [1, 2, 3]
    try {
        print(arr[10])
    } catch panic (p) {
        print("message:", p.message)         // => message: array index out of range: 10 (length 3)
        print("code:", p.code)               // => code: 430
    }
}

main()
```
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 8. Error Handling

> Source of truth: `src/frontend/analyzer/xanalyzer_errorset.c`, `src/ir/xi_lower_stmt.c`, `src/vm/xvm_dispatch_exception.inc.c`, `src/runtime/object/xpanic_info.c`, and `stdlib/types/panic_info.xr`.

### 8.0 Design philosophy: value-return + panic boundary

Xray's error handling is split into two strictly separated channels:

| Channel | Syntax | Use case | Runtime cost |
|--|--|--|--|
| **Value-return channel** (`throw <enum>` / `try` / `catch`) | Business errors, recoverable failures | **Low overhead** (no `PanicInfo` allocation and no unwind; only a predictable branch at call boundaries that may propagate or catch errors) |
| **Panic channel** (`catch panic`) | Runtime faults (OOB, division by zero, non-exhaustive match) | Limited stack unwinding |

Design principles:

- **Errors are values**: `throw <enum>` writes an enum value into the return channel — no stack unwinding, no PanicInfo allocation.
- **Panics are not errors**: a panic signals a program bug or runtime invariant violation, not business logic.
- **No `throws` in function signatures**: xray does not adopt Java/Swift-style checked exceptions. Errors are handled via the throw/catch value-return channel.
- **Error sets are not part of function types**: concrete error enum/variant sets remain in the analyzer effect database. A function type carries only the internal three-state throw-effect bit (`UNKNOWN` / `MAY_THROW` / `NO_THROW`) used by safety constraints and constructive code generation.
- **No-throw is always inferred**: use an `xray verify` contract to freeze a no-throw guarantee; unknown or incomplete evidence is treated as may-throw.
- **`defer` replaces `finally`**: xray has no `finally` keyword; resource cleanup uses **block-scoped** `defer` (bound to the nearest real `{}` block, see §4.9 / §8.3).
- **A cleanup edge is not an error-propagation edge**: no error may escape a `defer` body. The constraint is enforced at compile time (`E0387`), see §8.3.1.

### 8.1 Value-return error channel

#### 8.1.1 `throw` statement

`throw expr` raises an enum error value. `expr` must be a variant of an enum type:

```xray
enum AppErr { NotFound, InvalidInput { message: string } }

throw AppErr.NotFound                       // ✅ simple enum variant
throw AppErr.InvalidInput { message: "bad format" } // ✅ ADT enum variant with payload
```

After a throw:

```
throw point → write to pending_error → return up the call stack → run static cleanup regions on every crossed scope edge → catch handles → otherwise keep returning → top-level diagnostic
```

- No stack frame unwinding (unlike traditional exception unwinding)
- No object allocation and no stack unwinding on the happy path; call boundaries that may propagate or catch errors go through only a predictable error-flag branch
- Unhandled top-level errors print `[Uncaught Error] <enum value>`, exit code = 1

#### 8.1.2 `try` / `catch`

```xray
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

```xray
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
- An untyped `catch (e)` is the catch-all and matches any error value.
- A typed `catch (e: SomeErr)` matches only when the error value satisfies `is SomeErr`, where `SomeErr` is a concrete enum error type.
- Multiple `catch` clauses are tried in declaration order; the first match wins.
- If every typed clause fails and there is no catch-all, the error continues propagating.
- A `try` **must** be followed by at least one of `catch` or `catch panic`.

`catch` may also use an enum variant pattern directly. A unit variant is written as
`catch NetErr.Timeout { ... }`, where the only brace group is the catch body. A payload variant is
written as `catch DbErr.QueryFailed { query } { ... }`: the first braces are the named pattern and
the second braces are the body. Brace-group count alone determines this boundary; the parser does
not query the variant schema, and a unit pattern still cannot use `{}`.

#### 8.1.3 Rethrowing and error conversion

A `catch` block may rethrow the original error or throw a different enum variant:

```xray
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

```xray
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

fn fetchUser(id: i64) -> User {
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

Value-return errors **do not propagate across coroutines automatically**. An uncaught error in a child coroutine:

- Terminates the child, error recorded in `coro.error`
- The parent is **not** notified automatically

Ways to pass child coroutine errors:

1. **Explicit Channel**:

```xray
enum WorkerErr { Failed { message: string } }
const err_ch = Channel<string>(1)

go fn() {
    try {
        riskyWork()
        err_ch.send("ok")
    } catch (e) {
        err_ch.send("error")
    }
}()

var result = match (err_ch.recv()) {
    Recv.Value { value: v } -> v
    _ -> "error"
}
if (result != "ok") { log("worker failed") }
```

2. **`linked go call()`** (see §10.2): failure of a standalone child Task cancels its parent Task and linked subtree; the returned Task still exposes the precise error to its caller.

3. **Structured concurrency `linked scope`** (recommended, see §10.5): an error from any child in the scope propagates to the parent scope automatically, routed through the correct channel (value-return enum errors via `catch`, panics via `catch panic`).

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

```xray
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

```xray
enum AppErr { InvalidArg }

try {
    process(input)
} catch (e: AppErr) {
    log("business error:", e)                // value-return channel
} catch panic {
    log("runtime fault!")                    // panic channel
}
```

#### 8.2.3 The `PanicInfo` class

`PanicInfo` is now **used only by the panic channel**. The VM constructs `PanicInfo` objects automatically on runtime faults:

```xray
class PanicInfo {
    message: string             // human-readable message (e.g. "index out of bounds")
    stack: Array<string>        // automatically captured call stack
    cause: PanicInfo?           // chained cause
    code: i64                   // error code
    data: JSON.Value            // additional data; JSON null when absent

    constructor(message: string = "", cause: PanicInfo? = null)
    fn toString() -> string
}
```

User code generally does not construct `PanicInfo` directly — use `throw <enum>` for business errors.

**Reading panic details in `catch panic`**: `catch panic (p)` binds the `PanicInfo` object to `p`, so you can read `message`, `code`, `stack`, and the other fields:

```xray
fn main() {
    var arr: Array<i64> = [1, 2, 3]
    try {
        print(arr[10])                       // out of bounds → panic
    } catch panic (p) {
        print("message:", p.message)         // array index out of range: 10 (length 3)
        print("code:", p.code)               // 430
    }
}

main()
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

### 8.3 `defer` — resource cleanup

`defer` is a block-scoped cleanup statement guaranteed to run when the owning block exits (whether by fallthrough, `break` / `continue`, `return`, `throw`, or panic). Syntax: see §4.9.

A cleanup block reads its outer bindings at exit. A movable owner read by the block may not be moved or returned before the block exits (`E0382`); code that needs a registration-time value must create an explicit copy first. This owner-lifetime rule is independent of the error-channel rules below.

```xray
fn fetch(url: string) -> string {
    var conn = open(url)
    defer { conn.close() }                   // conn is guaranteed to close

    var data = conn.read()
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

`defer` is a **resource-cleanup edge**, not an error-propagation edge. A failure on the cleanup path means the resource state is no longer known, so the language neither lets a cleanup error overwrite an in-flight error (the Go model) nor silently swallows it.

**Rule D1 (static, normative)**: if the inferred escaping error set of a `defer` block is **non-empty**, the compiler reports `E0387`.

Unlike the throw-effect bit in §8.0, D1 is **not** fail-closed: xray has no user-writable no-throw annotation, so rejecting everything that cannot be proven non-throwing would leave an author facing an indirect call, a higher-order builtin, or a native member whose contract is not yet written with no way to discharge the obligation. What cannot be proven is left to rule D3's runtime backstop — that is what the layering is for. Should a user-writable no-throw annotation ever be added, D1 can tighten to "must be proven" and D3 becomes unreachable by construction.

```xray @id=defer-must-not-throw
fn close(c: Conn) { throw IoErr.Closed }

fn bad(c: Conn) {
    defer { close(c) }                       // ❌ E0387: error escapes cleanup
}

fn good(c: Conn) {
    defer {                                  // ✅ handle it inside the defer body
        try { close(c) } catch (e) { log.warn("close failed") }
    }
}
```

**Rule D2 (how to satisfy it)**: absorb the error inside the `defer` body with `try` / `catch`, or call a cleanup API that does not throw. This is the only legal form — it forces the author to answer "what happens when cleanup fails?".

**Rule D3 (runtime backstop, normative)**: if an error still escapes a `defer` body — an indirect call D1 could not decide statically, or a panic raised inside the body — the runtime **terminates the process** with `E0443` and exit status `70`:

- The termination is **not catchable** — neither `catch` nor `catch panic` intercepts it. An error or panic the `defer` body catches **itself** has not escaped; that is the ordinary form rule D2 prescribes.
- The in-flight error is neither replaced nor suppressed; the diagnostic reports the escaping error, and the in-flight one alongside it when there is one (an enum error value carries no message, and shows as `<no message>`).
- The VM and AOT backends must agree verbatim on this behaviour, including exit code and diagnostic text.

**Why not Go's "replace" semantics**: in Go, rewriting the error from a defer requires an explicit assignment to a named return value — visible and local. xray puts `throws` in neither the signature nor the call site (§8.0), so an implicit error replacement would be entirely invisible in the source, and multiple simultaneously-throwing `defer`s would additionally need a replacement-chain rule. Fail-fast is the only option that is defined, hides nothing, and adds no further rules.

### 8.4 Optional and error handling

`T?` is sugar for `T | null` and fits the binary "value or absent" case. See §2.5. Relation to error handling:

- **Failure with no cause**: `T?` (e.g. map lookup returns `null` for "key not found").
- Pairs with `??` (default value) / `?.` (optional chain) / `e!` (force unwrap).
- Do not use `T?` as a generic error return — if a cause is needed, use `throw <enum>` + `catch`.

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

```xray
enum ConnErr { Refused, Timeout }

// A tiny stand-in "connection" so the example runs on its own.
class Conn {
    alive: bool
    constructor(alive: bool) { this.alive = alive }
    isAlive() -> bool { return this.alive }
    close() { print("closed") }
}

fn fetchData(alive: bool) -> string {
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

main()
```

#### Pattern 2: throw + catch for library APIs

```xray
enum ConfigErr { Missing { field: string } }

fn requirePort(cfg: JSON.Object) {
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

main()
```

#### Pattern 3: `??` for default values

```xray
var port = config?.port ?? 8080
var user = db.findUser(id) ?? guestUser
```

#### Pattern 4: catch panic for runtime fault fallback

```xray
fn safeDivide(a: i64, b: i64) -> string {
    try {
        return string(a / b)
    } catch panic {
        return "error: division by zero"
    }
}
```

### 8.7 Worked Examples

These are self-contained programs that run as-is and pass `xray check` (comments show the real output).

#### Example 1: `throw` / `catch` / `match`

```xray
enum ParseErr { Empty, BadChar { value: string } }

fn parseDigit(s: string) -> i64 {
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

main()
```

#### Example 2: `defer` order (LIFO) on the error path

```xray
enum E { Boom }

fn work() {
    defer { print("defer 1") }
    defer { print("defer 2") }
    print("body")
    throw E.Boom                             // defers still run when throwing
}

fn main() {
    try { work() } catch (e) { print("caught") }
}

main()
```

Output (`defer` runs in LIFO order):

```
body
defer 2
defer 1
caught
```

#### Example 3: `catch panic` with fault details

```xray
fn main() {
    var arr: Array<i64> = [1, 2, 3]
    try {
        print(arr[10])
    } catch panic (p) {
        print("message:", p.message)         // => message: array index out of range: 10 (length 3)
        print("code:", p.code)               // => code: 430
    }
}

main()
```
<!-- /xr-spec:en -->

---
id: spec.8_error_handling
order: 009
---

<!-- xr-spec:cn -->
---

## 8. 错误处理 (Error Handling)

> 真值源：`src/vm/xvm_dispatch_exception.inc.c`、`src/vm/xvm_dispatch_misc.inc.c`、`src/runtime/object/xexception.c`、`stdlib/prelude/prelude.c`。

### 8.0 设计哲学：值返回 + panic 边界

Xray 的错误处理分为两个严格分离的通道：

| 通道 | 语法 | 适用场景 | 运行时开销 |
|--|--|--|--|
| **值返回通道**（`throw <enum>` / `try` / `catch`） | 业务错误、可恢复失败 | **零开销**（正常路径无额外指令） |
| **panic 通道**（`catch panic`） | 运行时故障（越界、除零、不完整 match） | 有限栈展开 |

设计原则：

- **错误是值**：`throw <enum>` 把枚举值写入返回通道，不展开栈、不分配 Exception 对象。
- **panic 不是错误**：panic 表示程序 bug 或运行时不变量违背，不应用于业务逻辑。
- **函数签名不标 `throws`**：xray 不引入 Java/Swift 的受检异常语义。错误通过 throw/catch 值返回通道处理。
- **`defer` 替代 `finally`**：xray 没有 `finally` 关键字，资源清理统一用函数作用域的 `defer`（Go 模型）。

### 8.1 值返回错误通道

#### 8.1.1 `throw` 表达式

`throw expr` 抛出一个枚举错误值。`expr` 必须是枚举类型的变体值：

```xray
enum AppErr { NotFound, InvalidInput(string) }

throw AppErr.NotFound                       // ✅ 简单枚举变体
throw AppErr.InvalidInput("bad format")     // ✅ 带载荷的 ADT 枚举变体
```

抛出后行为：

```
抛出点 → 写入 pending_error → 沿调用栈返回 → 沿途执行 defer → catch 处理 → 否则继续返回 → 顶层诊断
```

- 不展开栈帧（不同于传统异常的 unwind）
- 正常路径零开销，仅在错误路径有条件跳转成本
- 未捕获的顶层错误打印 `[Uncaught Error] <enum value>`，退出码 = 1

#### 8.1.2 `try` / `catch`

```xray
enum IOErr { Timeout, Refused(string) }

try {
    connect(host)
} catch (e) {
    match (e) {
        IOErr.Timeout -> log("timeout"),
        IOErr.Refused(reason) -> log("refused: " + reason),
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
enum DbErr { ConnLost, QueryFailed(string) }

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
- 有类型注解的 `catch (e: EnumType)` 仅当错误值 `is EnumType` 为真时匹配
- 多个 `catch` 子句按声明顺序匹配，首个匹配者执行
- 若所有类型化子句均不匹配且没有 catch-all，错误继续向上传播
- 一个 `try` **必须**至少跟随 `catch`（普通 `catch` 或 `catch panic`）

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
    NotFound(string),
    ServerError(int, string),
    Timeout,
}

enum ParseErr {
    Empty,
    InvalidChar(string, int),
    Overflow,
}

fn fetchUser(id: int) -> User {
    if (id <= 0) { throw HttpErr.NotFound("user not found") }
    // ...
}

try {
    let user = fetchUser(-1)
} catch (e: HttpErr) {
    match (e) {
        HttpErr.NotFound(msg) -> log("404:", msg),
        HttpErr.ServerError(code, msg) -> log(string(code) + ":", msg),
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
enum WorkerErr { Failed(string) }
const err_ch = Channel<string>(1)

go {
    try {
        riskyWork()
        err_ch.send("ok")
    } catch (e) {
        err_ch.send("error")
    }
}

let result = match (err_ch.recv()) {
    Recv.Value(v) -> v
    _ -> "error"
}
if (result != "ok") { log("worker failed") }
```

2. **结构化并发 `linked scope`**（推荐，见 §10.5）：子协程的错误自动传播给父 scope，按正确的通道路由（值返回通道的 enum 错误通过 `catch` 捕获）。

### 8.2 Panic 通道

#### 8.2.1 什么是 panic

panic 表示**程序 bug 或运行时不变量违背**，不应用于业务逻辑：

- 数组越界访问
- 整数除以零
- 不完整的 match（non-exhaustive）
- 空引用解引用
- 运行时类型断言失败

panic 通过有限的栈展开传播，生成 `Exception` 对象携带堆栈信息。

#### 8.2.2 `catch panic`

`catch panic` 捕获 panic 通道的运行时故障：

```xray
try {
    let arr: Array<int> = [1, 2, 3]
    let v = arr[10]                          // 越界 → panic
} catch panic {
    log("runtime fault caught")
}

try {
    let n = 10 / 0                           // 除零 → panic
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

#### 8.2.3 `Exception` 类

`Exception` 现在**仅用于 panic 通道**。运行时故障发生时，VM 自动构造 `Exception` 对象：

```xray
@native
class Exception {
    message: string             // 人类可读消息（如 "index out of bounds"）
    stack: Array<string>        // 自动捕获的调用栈
    cause: Exception?           // 链式 cause
    code: int                   // 错误码
    data: Json?                 // 附加数据

    constructor(message: string = "", cause: Exception? = null)
    fn toString() -> string
}
```

用户代码一般不直接构造 `Exception`——业务错误用 `throw <enum>`。

### 8.3 `defer` — 资源清理

`defer` 是函数作用域的清理语句，在函数退出时**保证执行**（无论正常返回、`throw`、还是 panic）。语法见 §4.9。

```xray
fn fetch(url: string) -> string {
    let conn = open(url)
    defer conn.close()                       // 无论后续如何，conn 一定关闭

    let data = conn.read()
    if (data.isEmpty()) {
        throw FetchErr.Empty                 // defer 仍执行
    }
    return data
}
```

**规则**：
- `defer` 是函数作用域（Go 模型），在函数退出时按 **LIFO** 顺序执行
- 单个函数可有多个 `defer`，按逆序执行
- `defer` 在 `throw`、`return`、panic 时均执行
- `defer` 块内不应抛出错误（行为未定义）

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
│   throw <enum>，catch 处理（零开销值返回通道）
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
| 主结果 + 元数据 | tuple | `parse(s) -> (Ast, int)` |

### 8.6 常用模式

#### 模式 1：enum 错误 + defer 资源清理

```xray
enum ConnErr { Refused, Timeout, Reset }

fn fetchData(host: string) -> string {
    let conn = connect(host)
    defer conn.close()

    if (!conn.isAlive()) { throw ConnErr.Timeout }
    return conn.read()
}

fn main() {
    try {
        let data = fetchData("api.example.com")
        print(data)
    } catch (e: ConnErr) {
        match (e) {
            ConnErr.Refused -> print("connection refused"),
            ConnErr.Timeout -> print("timeout"),
            ConnErr.Reset -> print("connection reset"),
        }
    }
}

main()
```

#### 模式 2：throw + catch 用于库 API

```xray
enum ConfigErr { BadJson(string), BadField(string) }

fn parseConfig(text: string) -> Config {
    let json = parseJson(text)
    let port = json["port"].toInt()
    if (port == null) { throw ConfigErr.BadField("port") }
    return Config(port: port!)
}

fn main() {
    try {
        let cfg = parseConfig(configText)
        startServer(cfg)
    } catch (e: ConfigErr) {
        match (e) {
            ConfigErr.BadJson(msg) -> print("invalid JSON:", msg),
            ConfigErr.BadField(f) -> print("bad field:", f),
        }
    }
}

main()
```

#### 模式 3：`??` 提供默认值

```xray
let port = config?.port ?? 8080
let user = db.findUser(id) ?? guestUser
```

#### 模式 4：catch panic 兜底运行时故障

```xray
fn safeDivide(a: int, b: int) -> string {
    try {
        return string(a / b)
    } catch panic {
        return "error: division by zero"
    }
}
```
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 8. Error Handling

> Source of truth: `src/vm/xvm_dispatch_exception.inc.c`, `src/vm/xvm_dispatch_misc.inc.c`, `src/runtime/object/xexception.c`, `stdlib/prelude/prelude.c`.

### 8.0 Design philosophy: value-return + panic boundary

Xray's error handling is split into two strictly separated channels:

| Channel | Syntax | Use case | Runtime cost |
|--|--|--|--|
| **Value-return channel** (`throw <enum>` / `try` / `catch`) | Business errors, recoverable failures | **Zero overhead** (no extra instructions on the happy path) |
| **Panic channel** (`catch panic`) | Runtime faults (OOB, division by zero, non-exhaustive match) | Limited stack unwinding |

Design principles:

- **Errors are values**: `throw <enum>` writes an enum value into the return channel — no stack unwinding, no Exception allocation.
- **Panics are not errors**: a panic signals a program bug or runtime invariant violation, not business logic.
- **No `throws` in function signatures**: xray does not adopt Java/Swift-style checked exceptions. Errors are handled via the throw/catch value-return channel.
- **`defer` replaces `finally`**: xray has no `finally` keyword; resource cleanup uses function-scoped `defer` (Go model).

### 8.1 Value-return error channel

#### 8.1.1 `throw` expression

`throw expr` raises an enum error value. `expr` must be a variant of an enum type:

```xray
enum AppErr { NotFound, InvalidInput(string) }

throw AppErr.NotFound                       // ✅ simple enum variant
throw AppErr.InvalidInput("bad format")     // ✅ ADT enum variant with payload
```

After a throw:

```
throw point → write to pending_error → return up the call stack → run defer on the way → catch handles → otherwise keep returning → top-level diagnostic
```

- No stack frame unwinding (unlike traditional exception unwinding)
- Zero overhead on the happy path; only a conditional branch on the error path
- Unhandled top-level errors print `[Uncaught Error] <enum value>`, exit code = 1

#### 8.1.2 `try` / `catch`

```xray
enum IOErr { Timeout, Refused(string) }

try {
    connect(host)
} catch (e) {
    match (e) {
        IOErr.Timeout -> log("timeout"),
        IOErr.Refused(reason) -> log("refused: " + reason),
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
enum DbErr { ConnLost, QueryFailed(string) }

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
- A typed `catch (e: EnumType)` matches only when the error value satisfies `is EnumType`.
- Multiple `catch` clauses are tried in declaration order; the first match wins.
- If every typed clause fails and there is no catch-all, the error continues propagating.
- A `try` **must** be followed by at least one of `catch` or `catch panic`.

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
    NotFound(string),
    ServerError(int, string),
    Timeout,
}

enum ParseErr {
    Empty,
    InvalidChar(string, int),
    Overflow,
}

fn fetchUser(id: int) -> User {
    if (id <= 0) { throw HttpErr.NotFound("user not found") }
    // ...
}

try {
    let user = fetchUser(-1)
} catch (e: HttpErr) {
    match (e) {
        HttpErr.NotFound(msg) -> log("404:", msg),
        HttpErr.ServerError(code, msg) -> log(string(code) + ":", msg),
        HttpErr.Timeout -> log("timeout"),
    }
}
```

ADT enums let `match` check exhaustiveness at compile time.

#### 8.1.5 Errors and coroutine boundaries

Value-return errors **do not propagate across coroutines automatically**. An uncaught error in a child coroutine:

- Terminates the child, error recorded in `coro.error`
- The parent is **not** notified automatically

Ways to pass child coroutine errors:

1. **Explicit Channel**:

```xray
enum WorkerErr { Failed(string) }
const err_ch = Channel<string>(1)

go {
    try {
        riskyWork()
        err_ch.send("ok")
    } catch (e) {
        err_ch.send("error")
    }
}

let result = match (err_ch.recv()) {
    Recv.Value(v) -> v
    _ -> "error"
}
if (result != "ok") { log("worker failed") }
```

2. **Structured concurrency `linked scope`** (recommended, see §10.5): child errors propagate to the parent scope automatically, routed through the correct channel (value-return enum errors via `catch`, panics via `catch panic`).

### 8.2 Panic channel

#### 8.2.1 What is a panic

A panic represents a **program bug or runtime invariant violation**, not business logic:

- Array out-of-bounds access
- Integer division by zero
- Non-exhaustive match
- Null reference dereference
- Runtime type assertion failure

Panics propagate via limited stack unwinding and generate `Exception` objects with stack traces.

#### 8.2.2 `catch panic`

`catch panic` catches runtime faults from the panic channel:

```xray
try {
    let arr: Array<int> = [1, 2, 3]
    let v = arr[10]                          // OOB → panic
} catch panic {
    log("runtime fault caught")
}

try {
    let n = 10 / 0                           // division by zero → panic
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

#### 8.2.3 The `Exception` class

`Exception` is now **used only by the panic channel**. The VM constructs `Exception` objects automatically on runtime faults:

```xray
@native
class Exception {
    message: string             // human-readable message (e.g. "index out of bounds")
    stack: Array<string>        // automatically captured call stack
    cause: Exception?           // chained cause
    code: int                   // error code
    data: Json?                 // additional data

    constructor(message: string = "", cause: Exception? = null)
    fn toString() -> string
}
```

User code generally does not construct `Exception` directly — use `throw <enum>` for business errors.

### 8.3 `defer` — resource cleanup

`defer` is a function-scoped cleanup statement guaranteed to run when the function exits (whether by normal return, `throw`, or panic). Syntax: see §4.9.

```xray
fn fetch(url: string) -> string {
    let conn = open(url)
    defer conn.close()                       // conn is guaranteed to close

    let data = conn.read()
    if (data.isEmpty()) {
        throw FetchErr.Empty                 // defer still runs
    }
    return data
}
```

**Rules**:
- `defer` is function-scoped (Go model), runs when the function exits in **LIFO** order
- Multiple `defer`s in the same function run in reverse order
- `defer` executes on `throw`, `return`, and panic
- `defer` blocks should not throw errors (behaviour is undefined)

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
│   throw <enum>, catch to handle (zero-overhead value-return channel)
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
| Primary result + metadata | tuple | `parse(s) -> (Ast, int)` |

### 8.6 Common patterns

#### Pattern 1: enum errors + defer for resource cleanup

```xray
enum ConnErr { Refused, Timeout, Reset }

fn fetchData(host: string) -> string {
    let conn = connect(host)
    defer conn.close()

    if (!conn.isAlive()) { throw ConnErr.Timeout }
    return conn.read()
}

fn main() {
    try {
        let data = fetchData("api.example.com")
        print(data)
    } catch (e: ConnErr) {
        match (e) {
            ConnErr.Refused -> print("connection refused"),
            ConnErr.Timeout -> print("timeout"),
            ConnErr.Reset -> print("connection reset"),
        }
    }
}

main()
```

#### Pattern 2: throw + catch for library APIs

```xray
enum ConfigErr { BadJson(string), BadField(string) }

fn parseConfig(text: string) -> Config {
    let json = parseJson(text)
    let port = json["port"].toInt()
    if (port == null) { throw ConfigErr.BadField("port") }
    return Config(port: port!)
}

fn main() {
    try {
        let cfg = parseConfig(configText)
        startServer(cfg)
    } catch (e: ConfigErr) {
        match (e) {
            ConfigErr.BadJson(msg) -> print("invalid JSON:", msg),
            ConfigErr.BadField(f) -> print("bad field:", f),
        }
    }
}

main()
```

#### Pattern 3: `??` for default values

```xray
let port = config?.port ?? 8080
let user = db.findUser(id) ?? guestUser
```

#### Pattern 4: catch panic for runtime fault fallback

```xray
fn safeDivide(a: int, b: int) -> string {
    try {
        return string(a / b)
    } catch panic {
        return "error: division by zero"
    }
}
```
<!-- /xr-spec:en -->

---
id: spec.4_statements
order: 005
---

<!-- xr-spec:cn -->
---

## 4. 语句 (Statements)

> 真值源：`src/frontend/parser/xparse_stmt.c`、`src/frontend/parser/xast_nodes_stmt.h`。

Xray 语句以 `\n` 或 `;` 分隔；语句末尾的 `;` 在大多数位置可省略，仅 `for` 循环的初始化/条件/步进三段必须用 `;` 分隔。

### 4.1 表达式语句与块

```ebnf
ExprStmt   ::= Expression (';' | LineBreak)
IncDecStmt ::= Identifier ('++' | '--') (';' | LineBreak)
Block      ::= '{' Statement* '}'
```

```xray
foo()                  // 表达式语句
x = 1                  // 赋值表达式作为语句
x++                    // 自增语句；不产生表达式值
{                      // 块
    let y = 2
    y + 1              // 表达式但结果被丢弃
}
```

`++` / `--` 是纯语句或 `for` 步进项，只能写作 `name++` / `name--`。它们等价于 `name = name + 1` / `name = name - 1`，没有返回值；`let y = x++`、`f(x++)`、`a[i++]`、`return x++` 等表达式位置均编译失败。

**注**：块**不是表达式**——它没有值。如果需要从块求值，用 `match` 或包装成立即调用函数。

### 4.2 `if` / `else`

```ebnf
IfStmt ::= 'if' '(' Expression ')' Block ElseIfChain? ElseClause?
ElseIfChain ::= ('else' 'if' '(' Expression ')' Block)+
ElseClause  ::= 'else' Block
```

```xray @id=stmt-if
if (x > 0) {
    print("positive")
} else if (x == 0) {
    print("zero")
} else {
    print("negative")
}
```

**约束**：
- 条件**必须**用括号包裹（与 Go/Rust 不同）。
- 条件必须是 `bool` 或 `T?`（`T != bool`）存在性检查；`bool?` 与裸 `int` / `string` / 集合等均为编译错误（见 §2.3.3）。
- 分支体必须是块 `{...}`，**不允许**单语句省略括号。
- `if` 不是表达式；要表达式形式用三元 `? :` 或 `match`。

### 4.3 `while`

```ebnf
LoopLabel ::= Identifier ':'
WhileStmt ::= LoopLabel? 'while' '(' Expression ')' Block
```

```xray @id=stmt-while
let i = 0
while (i < 10) {
    print(i)
    i++
}
```

无 `do-while` 形式。

### 4.4 `for`（C 风格）与 `for-in`

#### C 风格 `for`

```ebnf
ForStmt ::= LoopLabel? 'for' '(' ForInit? ';' Expression? ';' ForStep? ')' Block
ForInit ::= VarDecl | ExprStmt
ForStep ::= Expression | Identifier ('++' | '--')
```

```xray @id=stmt-for-c
for (let i = 0; i < 10; i++) {
    print(i)
}
for (let j = 100; j > 90; j--) {
    print(j)
}
```

- `ForInit` 中声明的变量作用域限于循环体。
- 步进项里的 `i++` / `i--` 必须是整个 step；若要多个更新，写在循环体末尾。
- 三个部分都可省略：`for (;;)` 是无限循环。

#### `for-in` 单变量

```ebnf
ForInStmt ::= LoopLabel? 'for' '(' Identifier 'in' Expression ')' Block
```

```xray @id=stmt-for-in
for (item in [1, 2, 3]) { print(item) }
for (i in 0..n) { print(i) }                  // 范围迭代（半开区间）
for (ch in "hello") { print(ch) }             // 字符串字符（按 codepoint）
for (key in someMap) { print(key) }           // Map 单变量 → key
for (key in someJson) { print(key) }          // Json 单变量 → key
for (day in Color) { print(day.name) }        // 枚举迭代（按声明顺序）
for (_ in 0..n) { count++ }                   // 占位符忽略
```

#### `for-in` 双变量解构

xray 支持两种等价的双变量形式：

```ebnf
ForInPairStmt ::= LoopLabel? 'for' '(' Identifier ',' Identifier 'in' Expression ')' Block
              |  LoopLabel? 'for' '(' '(' Identifier ',' Identifier ')' 'in' Expression ')' Block
```

```xray @id=stmt-for-pairs
// 形式 A：直接两标识符（更常见）
for (k, v in someMap) { print("${k}=${v}") }     // Map → (key, value)
for (i, e in someArray) { print("${i}: ${e}") }  // Array → (index, element)
for (i, c in "hello") { print("${i}:${c}") }     // string → (index, char)

// 形式 B：元组括号包裹（与 .entries() 配合）
for ((i, e) in someArray.entries()) { print("${i}=${e}") }
for ((i, c) in "hi".entries()) { print("${i}-${c}") }
```

迭代来源与产出对应关系：

| 集合类型 | 单变量产出 | 双变量产出 |
|---|---|---|
| `Array<T>` / `T[]` | element | (index, element) |
| `Map<K, V>` | key | (key, value) |
| `Json` | key (string) | (key, value) |
| `string` | char (1-codepoint string) | (index, char) |
| `Range`（`a..b`） | int | — |
| Enum 类型 | EnumValue | — |
| 自定义 `Iterator<T>` | T | — |

#### 自定义迭代器

实现 `iterator()` 方法返回 `Iterator<T>` 协议对象（含 `hasNext()` 和 `next()`）即可在 `for-in` 中使用。详见 §14.15。

### 4.5 `match` 语句

```ebnf
MatchStmt ::= 'match' '(' Expression ')' '{' MatchArm (','? MatchArm)* ','? '}'
MatchArm  ::= Pattern ('if' '(' Expression ')')? '->' (Expression | Block)
```

**关键语法**：
- 被匹配的表达式**必须**用括号包裹：`match (x) {...}`。
- 分支之间的逗号**可选**——同一个 match 中可以混用（不写更常见）。
- 守卫条件 `if` 后的表达式必须用括号：`n if (n > 0)`。

```xray @id=stmt-match
match (x) { 1 -> print("one"), _ -> print("other") }

match (action) {
    "start" -> {
        log.info("starting")
        start_engine()
    }
    "stop" -> stop_engine()
    _ -> log.warn("unknown")
}
```

`match` 既可作语句也可作表达式（详见 §3.13）；当作表达式时分支体必须是单一表达式或块的最后一个表达式。

模式细节见 [§6](#6-模式-patterns)。

### 4.6 `break` / `continue`

```xray @id=stmt-break-continue
break                  // 跳出最内层循环
continue               // 进入最内层循环的下一次迭代
break outer            // 跳出标签为 outer 的循环
continue outer         // 进入标签为 outer 的循环的下一次迭代

outer: for (i in 0..10) {
    for (j in 0..10) {
        if (j == 3) { continue outer }
        if (i * j > 20) { break outer }
    }
}
```

**约束**：
- 必须在 `while` / `for` 内部；否则编译错误 `E0304` / `E0305`。
- `match` 内部的 `break` / `continue` **不**作用于 `match`，而是跳出包裹 `match` 的循环。
- 循环标签写作 `label: for (...)` 或 `label: while (...)`，只能标在循环上；`label:` 后接非循环语句是编译错误。
- `break label` / `continue label` 必须引用当前活跃的外层循环标签；未知标签或同一活跃循环栈中重复标签是编译错误。
- 无标签 `continue` 作用于最内层循环；带标签 `continue` 作用于目标循环：`while` 重新检查条件，C 风格 `for` 执行 step 后再检查条件，`for-in` 进入下一项。

### 4.7 `return`

```ebnf
ReturnStmt ::= 'return' ReturnValue? (';' | LineBreak)
ReturnValue ::= Expression | '(' Expression (',' Expression)+ ')'
```

```xray @id=stmt-return
fn done() {
    return                 // 隐式返回 ()（Unit）
}

fn answer() -> int {
    return 42
}

fn pair(a: int, b: int) -> (int, int) {
    return (a, b)          // 多返回值，必须用括号包裹元组
}
```

> **注意**：多返回值必须用元组形式 `return (a, b)`；裸逗号 `return a, b` 是编译错误（`E0801`）。

**约束**：
- 只能在函数体内（含闭包）；顶层 return 是编译错误 `E0306`。
- 返回值类型必须与函数声明的返回类型兼容。

### 4.8 `throw` / `try` / `catch`

```ebnf
ThrowStmt     ::= 'throw' Expression

TryStmt       ::= 'try' Block CatchClause+
CatchClause   ::= 'catch' '(' Identifier (':' Type)? ')' Block
                | 'catch' 'panic' '(' Identifier ')' Block
```

```xray @id=stmt-try
enum AppError { NotFound, Timeout(ms: int) }

// 可恢复错误：enum 值经值返回通道传播，由 catch (e) 捕获
try { throw AppError.NotFound } catch (e) {
    match (e) {
        AppError.NotFound -> log.error("not found"),
        AppError.Timeout(ms) -> log.error("timeout after ${ms}ms")
    }
}

// catch panic (p) 是运行时故障（除零、越界、expr!）的独立边界
try { risky() } catch panic (p) {
    log.error("fault:", p)
}

throw AppError.NotFound                      // 值返回错误通道
// 没有 finally；用 defer 做确定性清理（见 §4.9）
```

**语义**：
- `try` 必须至少跟一个 `catch` 或 `catch panic` 子句。
- `catch (e)` 捕获经值返回通道传播的可恢复错误（用户 `throw <enum>`）；用 `match (e)` 解构错误值。
- `catch panic (p)` 捕获运行时故障（除零、越界、`expr!`、`assert`），与可恢复错误严格分离。
- `throw` 的操作数是错误值（通常为 enum），经值返回通道传播：不分配 `Exception`、不展开栈；需要传播或捕获错误的调用边界只经过可预测分支。
- 没有 `finally`：用 `defer`（§4.9）做确定性清理。
- 完整错误语义见 [§8](#8-错误处理-error-handling)。

### 4.9 `defer`

```ebnf
DeferStmt ::= 'defer' (Expression | Block)
```

```xray @id=stmt-defer
fn read_file(path: string) -> string {
    let f = open(path)
    defer f.close()                  // 函数返回前必执行
    return f.readAll()
}

fn process() {
    defer {                          // 块形式
        log.info("done")
        cleanup()
    }
    do_work()
}
```

**语义**：
- `defer` 绑定到包含它的**最近真实块** `{ ... }`。函数体本身也是块，因此写在函数体顶层的 `defer` 仍在函数退出前执行。
- **LIFO**：同一块内多个 `defer` 按声明的逆序执行。
- **必执行**：所属块正常结束，或通过 `break`、`continue`、`return`、值错误传播、panic 展开退出时都执行。
- 循环体内的 `defer` 每轮迭代结束时执行，不会堆积到函数尾。
- `defer` 是 Xray 唯一的确定性清理机制（取代其他语言的 `finally`）：它绑定词法块退出边，而不是整个函数的单一栈尾。
- `defer` 中抛出的错误会**取代**当前正在传播的错误（参考 Go 语义）。

### 4.10 内置打印函数

`print` / `dump` 是**内置全局函数**（非关键字，详见 §13.1），列于此处便于查阅：

```xray @id=stmt-print-dump
print("hello")                 // 自动追加换行
print("a:", a, "b:", b)        // 多参用空格分隔
dump(some_obj)                 // 调试输出，含类型信息与结构布局
```

**行为说明**：
- 接受任意类型与任意数量参数（变长）；每个参数自动调用其 `toString()` 或内置格式化。
- 输出到 stdout；不参与异常机制。
- 多参时以单空格分隔。
- `print` 默认会追加换行（与 C/Python 不同，与回归测试一致）。
- `dump` 用于调试，输出格式包含类型标注与对象内部结构。
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 4. Statements

> Source of truth: `src/frontend/parser/xparse_stmt.c`, `src/frontend/parser/xast_nodes_stmt.h`.

Xray statements are separated by `\n` or `;`; the trailing `;` is optional in most positions, only the three sections of a `for` loop (init / cond / step) require `;` separators.

### 4.1 Expression Statements and Blocks

```ebnf
ExprStmt   ::= Expression (';' | LineBreak)
IncDecStmt ::= Identifier ('++' | '--') (';' | LineBreak)
Block      ::= '{' Statement* '}'
```

```xray
foo()                  // expression statement
x = 1                  // assignment expression as a statement
x++                    // increment statement; produces no expression value
{                      // block
    let y = 2
    y + 1              // expression with discarded result
}
```

`++` / `--` are pure statements or `for` step items, and can only be written as `name++` / `name--`. They are equivalent to `name = name + 1` / `name = name - 1` and produce no value; expression-position uses such as `let y = x++`, `f(x++)`, `a[i++]`, and `return x++` are compile errors.

**Note**: a block is **not an expression** — it has no value. To get a value out of a block, use `match` or wrap it in an immediately-invoked function.

### 4.2 `if` / `else`

```ebnf
IfStmt ::= 'if' '(' Expression ')' Block ElseIfChain? ElseClause?
ElseIfChain ::= ('else' 'if' '(' Expression ')' Block)+
ElseClause  ::= 'else' Block
```

```xray @id=stmt-if
if (x > 0) {
    print("positive")
} else if (x == 0) {
    print("zero")
} else {
    print("negative")
}
```

**Constraints**:
- The condition **must** be parenthesized (unlike Go/Rust).
- The condition must be `bool` or nullable presence `T?` (`T != bool`); bare `bool?`, `int`, `string`, collections, etc. are compile errors (see §2.3.3).
- Branch bodies must be blocks `{...}`; **no** single-statement-without-braces form.
- `if` is not an expression; for an expression form use the ternary `? :` or `match`.

### 4.3 `while`

```ebnf
LoopLabel ::= Identifier ':'
WhileStmt ::= LoopLabel? 'while' '(' Expression ')' Block
```

```xray @id=stmt-while
let i = 0
while (i < 10) {
    print(i)
    i++
}
```

There is no `do-while` form.

### 4.4 `for` (C-style) and `for-in`

#### C-style `for`

```ebnf
ForStmt ::= LoopLabel? 'for' '(' ForInit? ';' Expression? ';' ForStep? ')' Block
ForInit ::= VarDecl | ExprStmt
ForStep ::= Expression | Identifier ('++' | '--')
```

```xray @id=stmt-for-c
for (let i = 0; i < 10; i++) {
    print(i)
}
for (let j = 100; j > 90; j--) {
    print(j)
}
```

- Variables declared in `ForInit` are scoped to the loop body.
- `i++` / `i--` in the step position must be the entire step; put multiple updates at the end of the loop body.
- All three sections may be omitted: `for (;;)` is an infinite loop.

#### Single-variable `for-in`

```ebnf
ForInStmt ::= LoopLabel? 'for' '(' Identifier 'in' Expression ')' Block
```

```xray @id=stmt-for-in
for (item in [1, 2, 3]) { print(item) }
for (i in 0..n) { print(i) }                  // range iteration (half-open)
for (ch in "hello") { print(ch) }             // string characters (by codepoint)
for (key in someMap) { print(key) }           // single variable over Map → key
for (key in someJson) { print(key) }          // single variable over Json → key
for (day in Color) { print(day.name) }        // enum iteration (declaration order)
for (_ in 0..n) { count++ }                   // discard with placeholder
```

#### Two-variable `for-in` destructuring

Xray supports two equivalent two-variable forms:

```ebnf
ForInPairStmt ::= LoopLabel? 'for' '(' Identifier ',' Identifier 'in' Expression ')' Block
              |  LoopLabel? 'for' '(' '(' Identifier ',' Identifier ')' 'in' Expression ')' Block
```

```xray @id=stmt-for-pairs
// Form A: two bare identifiers (more common)
for (k, v in someMap) { print("${k}=${v}") }     // Map → (key, value)
for (i, e in someArray) { print("${i}: ${e}") }  // Array → (index, element)
for (i, c in "hello") { print("${i}:${c}") }     // string → (index, char)

// Form B: tuple-parenthesized (pairs well with .entries())
for ((i, e) in someArray.entries()) { print("${i}=${e}") }
for ((i, c) in "hi".entries()) { print("${i}-${c}") }
```

Iteration source / yield mapping:

| Collection type | Single-variable yield | Two-variable yield |
|---|---|---|
| `Array<T>` / `T[]` | element | (index, element) |
| `Map<K, V>` | key | (key, value) |
| `Json` | key (string) | (key, value) |
| `string` | char (1-codepoint string) | (index, char) |
| `Range` (`a..b`) | int | — |
| Enum type | EnumValue | — |
| Custom `Iterator<T>` | T | — |

#### Custom iterators

Implement an `iterator()` method that returns an `Iterator<T>` protocol object (with `hasNext()` and `next()`) and the value becomes usable in `for-in`. See §14.15.

### 4.5 `match` Statement

```ebnf
MatchStmt ::= 'match' '(' Expression ')' '{' MatchArm (','? MatchArm)* ','? '}'
MatchArm  ::= Pattern ('if' '(' Expression ')')? '->' (Expression | Block)
```

**Key syntax**:
- The matched expression **must** be parenthesized: `match (x) {...}`.
- Commas between arms are **optional** — both styles can be mixed in the same `match` (omitting commas is more common).
- Guard expressions following `if` must be parenthesized: `n if (n > 0)`.

```xray @id=stmt-match
match (x) { 1 -> print("one"), _ -> print("other") }

match (action) {
    "start" -> {
        log.info("starting")
        start_engine()
    }
    "stop" -> stop_engine()
    _ -> log.warn("unknown")
}
```

`match` may serve as either a statement or an expression (see §3.13); when used as an expression, the arm body must be a single expression or end with one as the last expression of a block.

For pattern details see [§6](#6-patterns).

### 4.6 `break` / `continue`

```xray @id=stmt-break-continue
break                  // exit the innermost loop
continue               // proceed to the innermost loop's next iteration
break outer            // exit the loop labeled outer
continue outer         // proceed to the next iteration of the loop labeled outer

outer: for (i in 0..10) {
    for (j in 0..10) {
        if (j == 3) { continue outer }
        if (i * j > 20) { break outer }
    }
}
```

**Constraints**:
- Must appear inside a `while` / `for`; otherwise the compile errors `E0304` / `E0305`.
- `break` / `continue` inside a `match` does **not** affect `match` itself; it exits the enclosing loop.
- Loop labels are written as `label: for (...)` or `label: while (...)` and may only annotate loops; `label:` before a non-loop statement is a compile error.
- `break label` / `continue label` must refer to an active enclosing loop label; unknown labels and duplicate labels in the active loop stack are compile errors.
- Unlabeled `continue` targets the innermost loop. Labeled `continue` targets the named loop: `while` rechecks its condition, C-style `for` runs its step before rechecking, and `for-in` advances to the next item.

### 4.7 `return`

```ebnf
ReturnStmt ::= 'return' ReturnValue? (';' | LineBreak)
ReturnValue ::= Expression | '(' Expression (',' Expression)+ ')'
```

```xray @id=stmt-return
fn done() {
    return                 // implicitly returns () (Unit)
}

fn answer() -> int {
    return 42
}

fn pair(a: int, b: int) -> (int, int) {
    return (a, b)          // multi-value return must wrap a tuple in parens
}
```

> **Note**: multi-value returns must use the tuple form `return (a, b)`; the bare-comma form `return a, b` is the compile error `E0801`.

**Constraints**:
- Allowed only inside a function body (including closures); a top-level `return` is the compile error `E0306`.
- The returned value's type must be compatible with the function's declared return type.

### 4.8 `throw` / `try` / `catch`

```ebnf
ThrowStmt     ::= 'throw' Expression

TryStmt       ::= 'try' Block CatchClause+
CatchClause   ::= 'catch' '(' Identifier (':' Type)? ')' Block
                | 'catch' 'panic' '(' Identifier ')' Block
```

```xray @id=stmt-try
enum AppError { NotFound, Timeout(ms: int) }

// Recoverable errors: enum values flow through the value-return channel,
// caught by catch (e)
try { throw AppError.NotFound } catch (e) {
    match (e) {
        AppError.NotFound -> log.error("not found"),
        AppError.Timeout(ms) -> log.error("timeout after ${ms}ms")
    }
}

// catch panic (p) is a separate boundary for runtime faults
// (div-by-zero, out-of-bounds, expr!)
try { risky() } catch panic (p) {
    log.error("fault:", p)
}

throw AppError.NotFound                      // value-return error channel
// There is no finally; use defer for deterministic cleanup (see §4.9)
```

**Semantics**:
- A `try` must be followed by at least one `catch` or `catch panic` clause.
- `catch (e)` catches recoverable errors propagated through the value-return channel (a user `throw <enum>`); use `match (e)` to destructure the error value.
- `catch panic (p)` catches runtime faults (div-by-zero, out-of-bounds, `expr!`, `assert`), strictly separated from recoverable errors.
- The `throw` operand is an error value (typically an enum) propagated through the value-return channel: no `Exception` allocation, no stack unwinding, and only a predictable branch at call boundaries that may propagate or catch errors.
- There is no `finally`: use `defer` (§4.9) for deterministic cleanup.
- For full error semantics see [§8](#8-error-handling).

### 4.9 `defer`

```ebnf
DeferStmt ::= 'defer' (Expression | Block)
```

```xray @id=stmt-defer
fn read_file(path: string) -> string {
    let f = open(path)
    defer f.close()                  // always runs before the function returns
    return f.readAll()
}

fn process() {
    defer {                          // block form
        log.info("done")
        cleanup()
    }
    do_work()
}
```

**Semantics**:
- A `defer` belongs to the nearest enclosing real block `{ ... }`. A function body is a block, so a top-level function-body `defer` still runs before the function exits.
- **LIFO**: multiple `defer` statements in the same block run in reverse declaration order.
- **Always executes**: runs when the owning block falls through or exits by `break`, `continue`, `return`, value-error propagation, or panic unwinding.
- A `defer` inside a loop body runs at the end of each iteration, not at the end of the function.
- `defer` is Xray's only deterministic-cleanup mechanism (replacing other languages' `finally`): it is bound to lexical block exits, not to a single function tail.
- An error thrown inside a `defer` body **replaces** any in-flight error (Go-style semantics).

### 4.10 Built-in Print Functions

`print` / `dump` are **built-in global functions** (not keywords; see §13.1), listed here for convenience:

```xray @id=stmt-print-dump
print("hello")                 // auto-appends a newline
print("a:", a, "b:", b)        // multiple arguments separated by spaces
dump(some_obj)                 // debug output, with type info and structure
```

**Behavior**:
- Accepts any type and any number of arguments (variadic); each argument is automatically converted via its `toString()` or built-in formatter.
- Output goes to stdout; not part of the exception mechanism.
- Multiple arguments are separated by single spaces.
- `print` appends a newline by default (different from C/Python; consistent with regression tests).
- `dump` is for debugging; output includes type tags and internal structure.
<!-- /xr-spec:en -->

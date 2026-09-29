---
id: spec.6_patterns
order: 007
---

<!-- xr-spec:cn -->
---

## 6. 模式 (Patterns)

> 真值源：`src/frontend/parser/xparse_match.c`、`src/frontend/analyzer/xanalyzer_visitor_pattern.c`、`src/ir/xi_lower_expr.c` / `xi_lower_stmt.c` 与 VM/AOT 的 match lowering。

模式出现在 `match` 表达式/语句、`var` / `const` 解构，以及括号内的 `catch` 头部（仅 enum variant pattern，见 §8.1.2）中。

### 6.1 字面量模式

```xray
match (x) {
    0 -> "zero"
    3.14 -> "pi"
    "hello" -> "greeting"
    true -> "yes"
    null -> "nothing"
    _ -> "other"
}
```

- 匹配使用与 `==` 相同的语义。
- `null` 模式只匹配 `null` 本身。

### 6.2 范围模式 `a..b` / `a..=b`

```xray
match (age) {
    0..13 -> "child"
    13..20 -> "teen"
    20..=65 -> "adult"
    _ -> "senior"
}
```

- `a..b` 为半开区间 `[a, b)`；`a..=b` 为闭区间 `[a, b]`。
- 仅整数。

### 6.3 枚举模式

#### 6.3.1 简单变体（无 payload）

```xray
match (color) {
    Color.Red   -> "red",
    Color.Green -> "green",
    Color.Blue  -> "blue",
}
```

- 需完整限定 `EnumName.Variant`。

#### 6.3.2 ADT 变体（带 payload）解构

被匹配值只求值一次。分支绑定为不可变值，作用域仅覆盖该分支的 guard 和分支体；同一模式不能重复绑定同名变量。绑定遵守普通值复制与所有权规则，不获得写回被匹配值的权限，跨挂起和闭包逃逸仍须保留有效载荷。泛型变体模式省略的类型实参来自被匹配值的具体静态类型，不补足声明处缺失的约束。

ADT 变体模式按名称选择需要观察的 payload 字段。未列字段自动忽略，`{}` 只检查 payload variant 的 tag：

```xray
match (event) {
    NetEvent.Connected                         -> print("connected"),
    NetEvent.Disconnected { reason }           -> print("by:", reason),
    NetEvent.DataReceived { bytes }            -> process(bytes),
    NetEvent.Error { code, message: msg }      -> log.error(code, msg),
}

// 同名 binding shorthand
match (opt) {
    Option.Some { value: v } -> print("got:", v),
    Option.None    -> print("nothing"),
}

// 只列需要观察的字段
match (event) {
    NetEvent.Error { code } if (code >= 500) -> throw NetErr.ServerFault { code: code },
    _                                        -> continue,
}

// 嵌套解构
match (msg) {
    Option.Some { value: NetEvent.DataReceived { bytes } } -> process(bytes),
    Option.None                                           -> skip(),
    _                                                     -> skip(),
}
```

#### 6.3.3 穷举性检查

`match` 一个 ADT enum 时，编译器执行**穷举性分析**：

- 若所有变体的全部载荷取值均被无 guard 的模式覆盖（含无 guard 的 `_` 兜底），通过；只列出变体名但对子字段施加可失败模式，不足以覆盖整个变体
- 若漏写某变体，编译报错 `E0371 XR_ERR_ANALYZE_MATCH_NOT_EXHAUSTIVE`，并提示缺失的变体名

```xray
enum NetEvent {
    Connected,
    Disconnected { reason: string },
    DataReceived { bytes: Array<u8> },
    Error { code: i64, message: string },
}

match (event) {
    NetEvent.Connected                     -> "ok",
    NetEvent.Disconnected { reason: r }    -> "down: ${r}",
    // ❌ E0371: 缺失变体 DataReceived 和 Error；可加 `_ -> ...` 兜底
}
```

> 简单枚举（无 payload）与 ADT enum 均**强制**穷举；无 guard 的 `_` 兜底分支覆盖所有剩余值；带 guard 的分支不计作无条件覆盖。对非 enum 变量（如 `i64`）不强制。

### 6.4 类型模式 `is T`

```xray
match (value) {
    is i64 n -> "i64: ${n}"       // 绑定窄化值
    is string -> "a string"
    is User u -> "user: ${u.name}"
    _ -> "unknown"
}
```

- 检查动态类型；可选绑定窄化变量。

### 6.5 守卫条件 `if`

```xray
match (x) {
    n if (n > 0 && n < 10) -> "small positive"
    n if (n < 0) -> "negative"
    _ -> "other"
}
```

- 守卫表达式必须是 `bool`，nullable 值须显式比较（见 §2.3.3），与 `if` / `while` 条件规则一致。
- 失败时继续尝试下一分支。

### 6.6 多值模式

```xray
match (x) {
    1, 2, 3 -> "small"
    Color.Red, Color.Yellow -> "warm"
    _ -> "other"
}
```

- 按源码顺序尝试备选，首次成功后不再尝试该 arm 的其他备选。
- 所有备选必须绑定相同的名称集合，且每个同名绑定的确切静态类型相同；绑定不可变，属于该 arm 的共同作用域。
- 成功备选的绑定合并后，arm 的 guard 只求值一次；guard 失败进入下一 arm，不重试当前 arm 的剩余备选。
- 无 guard 的备选共同贡献穷举覆盖；有 guard 的整个 arm 不贡献无条件覆盖。

### 6.7 通配符 `_`

- 匹配任意值，不绑定变量。
- 通常作为最后的 default 分支。
- 解构中可用于跳过位置：`var [_, b, _] = arr`。

### 6.8 变量绑定模式

```xray
match (http_status) {
    200 -> "ok"
    code if (code >= 400) -> "error: ${code}"
    code -> "other: ${code}"
}
```

- 裸 `Identifier` 总匹配并绑定为值。

### 6.9 解构模式

```xray
var [a, b, c] = some_array
var (q, r) = divmod(17, 5)
var { name, age } = user
```

详见 §5.1.5。`match` 支持 tuple、ADT variant、对象与数组结构解构：

```xray
match (p) {
    { x, y } -> ...           // 对象字段解构（簡寫绑定）
    { name: n, age } -> ...   // 对象字段重命名 + 簡寫混用
    [a, b, ..rest] -> ...     // 数组解构，`..rest` 捕获尾部为新数组
    [_, mid, _] -> ...        // 元素位通配
}
```

- 对象模式匹配静态类型已知包含这些字段的 exact structural object；schema-less JSON 请先用 `JSON.get/require` 或 `JSON.decodeObject<T>` 提交结构。字段子模式可为可反驳模式（如 `{ mode: 2 }`）。
- 数组模式按**长度**匹配：无 `..rest` 时要求长度恰等于元素数，有 `..rest` 时要求长度 ≥ 元素数。元素子模式只能是绑定或通配（`..rest` 之外的元素不做按值测试——元素越界读取会陷入 panic）；需要按元素值判断时改用 `if` 守卫。
- or-pattern `|` 暂不支持（逗号多值已覆盖等价能力）。

### 6.10 穷举性与匹配失败

- 对 enum 表达式的 `match` 强制穷举（错误码 `E0371`，见 §6.3.3）。
- 其他类型不强制；运行时无分支匹配 → 抛 `PanicInfo` 错误码 `E0442`（见 §18.x）。
- 建议总是提供 `_` 兜底。
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 6. Patterns

> Source of truth: `src/frontend/parser/xparse_match.c`, `src/frontend/analyzer/xanalyzer_visitor_pattern.c`, `src/ir/xi_lower_expr.c` / `xi_lower_stmt.c`, and the VM/AOT match lowerings.

Patterns appear in `match` expressions/statements, in `var` / `const` destructuring, and in parenthesized `catch` headers (enum variant patterns only, see §8.1.2).

### 6.1 Literal Patterns

```xray
match (x) {
    0 -> "zero"
    3.14 -> "pi"
    "hello" -> "greeting"
    true -> "yes"
    null -> "nothing"
    _ -> "other"
}
```

- Matching uses the same semantics as `==`.
- The `null` pattern matches only `null` itself.

### 6.2 Range Patterns `a..b` / `a..=b`

```xray
match (age) {
    0..13 -> "child"
    13..20 -> "teen"
    20..=65 -> "adult"
    _ -> "senior"
}
```

- `a..b` is the half-open interval `[a, b)`; `a..=b` is the inclusive interval `[a, b]`.
- Integer-only.

### 6.3 Enum Patterns

#### 6.3.1 Simple variants (no payload)

```xray
match (color) {
    Color.Red   -> "red",
    Color.Green -> "green",
    Color.Blue  -> "blue",
}
```

- Must be fully qualified as `EnumName.Variant`.

#### 6.3.2 ADT variant (with payload) destructuring

The scrutinee is evaluated once. Pattern bindings are immutable values scoped to the arm's guard and body; a pattern cannot bind the same name twice. Ordinary copy and ownership rules apply, with no authority to write back into the scrutinee. Payload lifetime remains valid across suspension and escaping closures. Generic variant patterns take omitted type arguments from the scrutinee's static type without supplying missing definition-site constraints.

ADT variant patterns select payload fields by name. Omitted fields are ignored, and `{}` tests only the tag of a payload variant:

```xray
match (event) {
    NetEvent.Connected                         -> print("connected"),
    NetEvent.Disconnected { reason }           -> print("by:", reason),
    NetEvent.DataReceived { bytes }            -> process(bytes),
    NetEvent.Error { code, message: msg }      -> log.error(code, msg),
}

// Same-name binding shorthand
match (opt) {
    Option.Some { value: v } -> print("got:", v),
    Option.None    -> print("nothing"),
}

// List only fields that are observed
match (event) {
    NetEvent.Error { code } if (code >= 500) -> throw NetErr.ServerFault { code: code },
    _                                        -> continue,
}

// Nested destructuring
match (msg) {
    Option.Some { value: NetEvent.DataReceived { bytes } } -> process(bytes),
    Option.None                                           -> skip(),
    _                                                     -> skip(),
}
```

#### 6.3.3 Exhaustiveness check

When `match` is performed on an ADT enum, the compiler runs **exhaustiveness analysis**:

- The check passes when unguarded patterns cover every payload value of every variant (including an unguarded `_` catch-all). Naming a variant while restricting its fields with refutable patterns does not cover that entire variant.
- If a variant is missed, compilation fails with `E0371 XR_ERR_ANALYZE_MATCH_NOT_EXHAUSTIVE`, naming the missing variants.

```xray
enum NetEvent {
    Connected,
    Disconnected { reason: string },
    DataReceived { bytes: Array<u8> },
    Error { code: i64, message: string },
}

match (event) {
    NetEvent.Connected                     -> "ok",
    NetEvent.Disconnected { reason: r }    -> "down: ${r}",
    // ❌ E0371: missing variants DataReceived and Error; add `_ -> ...` as catch-all
}
```

> Both simple enums (no payload) and ADT enums **require** exhaustiveness; an unguarded `_` catch-all covers every remaining value; guarded arms do not count as unconditional coverage. Non-enum operands (such as `i64`) are not subject to the check.

### 6.4 Type Patterns `is T`

```xray
match (value) {
    is i64 n -> "i64: ${n}"       // bind the narrowed value
    is string -> "a string"
    is User u -> "user: ${u.name}"
    _ -> "unknown"
}
```

- Tests the dynamic type; optionally binds a narrowed variable.

### 6.5 Guard Conditions `if`

```xray
match (x) {
    n if (n > 0 && n < 10) -> "small positive"
    n if (n < 0) -> "negative"
    _ -> "other"
}
```

- The guard must be `bool`; nullable values require an explicit comparison (see §2.3.3), identical to `if` / `while` condition rules.
- On guard failure, matching falls through to the next arm.

### 6.6 Multi-value Patterns

```xray
match (x) {
    1, 2, 3 -> "small"
    Color.Red, Color.Yellow -> "warm"
    _ -> "other"
}
```

- Alternatives are tried in source order; the first success skips the remaining alternatives of that arm.
- Every alternative must bind the same names with exactly the same static types. Bindings are immutable and belong to the common arm scope.
- After joining the successful bindings, the arm guard runs once. A false guard advances to the next arm without retrying the remaining alternatives of the current arm.
- Unguarded alternatives contribute coverage together; a guarded arm contributes no unconditional coverage.

### 6.7 Wildcard `_`

- Matches any value without binding.
- Typically used as the trailing default arm.
- Usable in destructuring to skip positions: `var [_, b, _] = arr`.

### 6.8 Variable-binding Patterns

```xray
match (http_status) {
    200 -> "ok"
    code if (code >= 400) -> "error: ${code}"
    code -> "other: ${code}"
}
```

- A bare `Identifier` always matches and binds the value.

### 6.9 Destructuring Patterns

```xray
var [a, b, c] = some_array
var (q, r) = divmod(17, 5)
var { name, age } = user
```

See §5.1.5 for details. Within `match`, tuple, ADT-variant, object, and array destructuring are supported:

```xray
match (p) {
    { x, y } -> ...           // object field destructure (shorthand binding)
    { name: n, age } -> ...   // field rename + shorthand mixed
    [a, b, ..rest] -> ...     // array destructure; `..rest` captures the tail as a new array
    [_, mid, _] -> ...        // element-position wildcards
}
```

- An object pattern matches an exact structural object whose static type contains the named fields. For schema-less JSON, first commit the structure with `JSON.get/require` or `JSON.decodeObject<T>`. Field sub-patterns may be refutable (e.g. `{ mode: 2 }`).
- An array pattern matches by **length**: without `..rest`, the length must equal the element count; with `..rest`, the length must be ≥ the element count. Element sub-patterns may only be bindings or wildcards (non-rest elements are not value-tested — an out-of-bounds element read traps); use an `if` guard to test element values.
- The or-pattern `|` is not supported (comma-separated multi-values cover the equivalent need).

### 6.10 Exhaustiveness and Match Failure

- `match` over an enum expression is exhaustive (error code `E0371`, see §6.3.3).
- Other operand types are not enforced; if no arm matches at runtime, an `PanicInfo` with code `E0442` is raised (see §18.x).
- Always providing a `_` fallback is recommended.
<!-- /xr-spec:en -->

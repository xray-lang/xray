---
id: spec.6_patterns
order: 007
---

<!-- xr-spec:cn -->
---

## 6. 模式 (Patterns)

> 真值源：`src/frontend/parser/xparse_match.c`、`src/runtime/value/x_value_match.c`。

模式出现在 `match` 表达式/语句与 `let` 解构中。

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

### 6.2 范围模式 `a..b`

```xray
match (age) {
    0..13 -> "child"
    13..20 -> "teen"
    20..65 -> "adult"
    _ -> "senior"
}
```

- 半开区间 `[a, b)`，仅整数。

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

ADT 变体的模式可解构 payload 字段（按位置或按字段名）：

```xray
// 位置解构
match (event) {
    NetEvent.Connected            -> print("connected"),
    NetEvent.Disconnected(reason) -> print("by:", reason),
    NetEvent.DataReceived(b)      -> process(b),
    NetEvent.Error(code, msg)     -> log.error(code, msg),
}

// Option 模式（位置）
match (opt) {
    Option.Some(v) -> print("got:", v),
    Option.None    -> print("nothing"),
}

// 通配符跳过 payload 中不关心的字段
match (event) {
    NetEvent.Error(code, _) if (code >= 500) -> throw NetErr.ServerFault(code),
    _                                         -> continue,
}

// 嵌套解构
match (msg) {
    Option.Some(NetEvent.DataReceived(bytes)) -> process(bytes),
    Option.None                               -> skip(),
    _                                         -> skip(),
}
```

#### 6.3.3 穷举性检查

`match` 一个 ADT enum 时，编译器执行**穷举性分析**：

- 若所有变体都被覆盖（含 `_` 兜底），通过
- 若漏写某变体，编译报错 `E0371 XR_ERR_ANALYZE_MATCH_NOT_EXHAUSTIVE`，并提示缺失的变体名

```xray
enum NetEvent {
    Connected,
    Disconnected(reason: string),
    DataReceived(bytes: Bytes),
    Error(code: int, message: string),
}

match (event) {
    NetEvent.Connected            -> "ok",
    NetEvent.Disconnected(r)      -> "down: ${r}",
    // ❌ E0371: 缺失变体 DataReceived 和 Error；可加 `_ -> ...` 兜底
}
```

> 简单枚举（无 payload）与 ADT enum 均**强制**穷举；只要包含 `_` 兜底分支即可跳过检查。对非 enum 变量（如 `int`）不强制。

### 6.4 类型模式 `is T`

```xray
match (value) {
    is int n -> "int: ${n}"       // 绑定窄化值
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

- 守卫表达式位于 `if (...)` 括号内，按 truthy/falsy 上下文求值（见 §2.3.3，与 `if` / `while` 一致）；推荐显式使用 `bool` 表达式以提高可读性。
- 失败时继续尝试下一分支。

### 6.6 多值模式

```xray
match (x) {
    1, 2, 3 -> "small"
    Color.Red, Color.Yellow -> "warm"
    _ -> "other"
}
```

- 任一子模式匹配即成功。

### 6.7 通配符 `_`

- 匹配任意值，不绑定变量。
- 通常作为最后的 default 分支。
- 解构中可用于跳过位置：`let [_, b, _] = arr`。

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
let [a, b, c] = some_array
let (q, r) = divmod(17, 5)
let { name, age } = user
```

详见 §5.1.5。`match` 中当前支持 tuple 与 ADT variant 解构；对象/数组结构解构不属于当前 `match` 模式语法。

### 6.10 穷举性与匹配失败

- 对 enum 表达式的 `match` 强制穷举（错误码 `E0371`，见 §6.3.3）。
- 其他类型不强制；运行时无分支匹配 → 抛 `Exception` 错误码 `E0442`（见 §18.x）。
- 建议总是提供 `_` 兜底。
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 6. Patterns

> Source of truth: `src/frontend/parser/xparse_match.c`, `src/runtime/value/x_value_match.c`.

Patterns appear in `match` expressions/statements and in `let` destructuring.

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

### 6.2 Range Patterns `a..b`

```xray
match (age) {
    0..13 -> "child"
    13..20 -> "teen"
    20..65 -> "adult"
    _ -> "senior"
}
```

- Half-open interval `[a, b)`; integer-only.

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

ADT variant patterns may destructure payload fields (positionally or by name):

```xray
// Positional destructuring
match (event) {
    NetEvent.Connected            -> print("connected"),
    NetEvent.Disconnected(reason) -> print("by:", reason),
    NetEvent.DataReceived(b)      -> process(b),
    NetEvent.Error(code, msg)     -> log.error(code, msg),
}

// Option patterns (positional)
match (opt) {
    Option.Some(v) -> print("got:", v),
    Option.None    -> print("nothing"),
}

// Wildcards skip payload fields you don't care about
match (event) {
    NetEvent.Error(code, _) if (code >= 500) -> throw NetErr.ServerFault(code),
    _                                         -> continue,
}

// Nested destructuring
match (msg) {
    Option.Some(NetEvent.DataReceived(bytes)) -> process(bytes),
    Option.None                               -> skip(),
    _                                         -> skip(),
}
```

#### 6.3.3 Exhaustiveness check

When `match` is performed on an ADT enum, the compiler runs **exhaustiveness analysis**:

- If every variant is covered (including `_` as a catch-all), the check passes.
- If a variant is missed, compilation fails with `E0371 XR_ERR_ANALYZE_MATCH_NOT_EXHAUSTIVE`, naming the missing variants.

```xray
enum NetEvent {
    Connected,
    Disconnected(reason: string),
    DataReceived(bytes: Bytes),
    Error(code: int, message: string),
}

match (event) {
    NetEvent.Connected            -> "ok",
    NetEvent.Disconnected(r)      -> "down: ${r}",
    // ❌ E0371: missing variants DataReceived and Error; add `_ -> ...` as catch-all
}
```

> Both simple enums (no payload) and ADT enums **require** exhaustiveness; including a `_` catch-all suffices to skip the check. Non-enum operands (such as `int`) are not subject to the check.

### 6.4 Type Patterns `is T`

```xray
match (value) {
    is int n -> "int: ${n}"       // bind the narrowed value
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

- The guard expression sits inside `if (...)` parentheses and is evaluated under truthy/falsy context (see §2.3.3, identical to `if` / `while`); explicit `bool` expressions are recommended for readability.
- On guard failure, matching falls through to the next arm.

### 6.6 Multi-value Patterns

```xray
match (x) {
    1, 2, 3 -> "small"
    Color.Red, Color.Yellow -> "warm"
    _ -> "other"
}
```

- Any sub-pattern matching is a success.

### 6.7 Wildcard `_`

- Matches any value without binding.
- Typically used as the trailing default arm.
- Usable in destructuring to skip positions: `let [_, b, _] = arr`.

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
let [a, b, c] = some_array
let (q, r) = divmod(17, 5)
let { name, age } = user
```

See §5.1.5 for details. Within `match`, tuple and ADT-variant destructuring are currently supported; structural object/array destructuring is not part of the current `match` pattern syntax.

### 6.10 Exhaustiveness and Match Failure

- `match` over an enum expression is exhaustive (error code `E0371`, see §6.3.3).
- Other operand types are not enforced; if no arm matches at runtime, an `Exception` with code `E0442` is raised (see §18.x).
- Always providing a `_` fallback is recommended.
<!-- /xr-spec:en -->

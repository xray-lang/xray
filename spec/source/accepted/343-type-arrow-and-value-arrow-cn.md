# 已接受目标规则：343 - `->` 只写类型，`=>` 写值映射

> 2026-11-16 从任务文档迁入统一规范源。此处维护已接受的目标定义；原任务只维护实施、依赖、理由与验收记录。
> 迁入不表示实现完成。下面保留的示例是目标验收材料，不能据此声称当前编译器、VM 或 AOT 已支持。历史修订和已明确被取代的规则保持历史身份；不能将旧版本重新激活。
> 实施任务：[343-type-arrow-and-value-arrow-cn](../../../../xray-docs/tasks/343-type-arrow-and-value-arrow-cn.md)。

## 3. 语义

### 3.1 规则

1. **类型箭头 `->`**：只出现在函数与方法声明的返回类型、函数类型 `fn(T) -> R`（含 `nosuspend fn`、`escaping fn` 等修饰形式）、fn lambda `fn(x: T) -> R { ... }` 的返回类型中。多返回值 `-> (A, B)` 不变。
2. **值箭头 `=>`**：
   - 箭头 lambda：`x => expr`、`(x, y) => expr`、`(x: T) => expr`、`(x: ref T) => { ... }`；
   - `match` 分支：`Pattern => expr`、`Pattern if (cond) => { ... }`；分支之间仍按 G14 写逗号；
   - `select` 分支：`v from ch => { ... }`、`v to ch => { ... }`、`after ms => { ... }`、`_ => { ... }`。
3. **块与字面量**：`=>` 之后的 `{` 一律是块；要直接得到 Map 字面量写 `x => ({"a": 1})`。
4. **结合性与范围**：箭头 lambda 的函数体向右延伸到包含它的表达式结束，`=>` 右结合，`x => y => x + y` 是返回 lambda 的 lambda。与现行 `->` 的结合与范围规则相同，只换记号。
5. **词法**：`=>` 是单一记号，按最长匹配识别；与 `>=`、`==` 不冲突。
6. **两种 lambda 的分工不变**：箭头 lambda 不写返回类型，靠推断或期望的函数类型；需要写明返回类型、泛型参数或 `throws` 时用 fn lambda。

### 3.2 正例

```xray
fn make() -> fn(i64) -> i64 {
    return x => x * 2
}
const add = (a: i64, b: i64) => a + b
const twice = fn(x: i64) -> i64 { return x * 2 }
var f: fn(i64) -> i64 = x => x + 1

const label = match (n) {
    0 => "zero",
    n if (n < 0) => "negative",
    _ => "positive",
}

const curried = x => y => x + y
const toMap = (k: string) => ({k: 1})
```

```xray
select {
    v from ch => { print(v) }
    after 100 => { print("timeout") }
}
```

### 3.3 反例

```xray
const g = x -> x + 1              // 错误：lambda 用 `=>`：x => x + 1
fn h() => i64 { return 1 }        // 错误：返回类型用 `->`：fn h() -> i64
var k: fn(i64) => i64 = x => x    // 错误：函数类型用 `->`：fn(i64) -> i64
const r = match (n) { 0 -> "z", _ => "o" }   // 错误：match 分支用 `=>`
const m = x => {"a": 1}           // 错误：`=>` 后的 `{` 是块；Map 字面量写 x => ({"a": 1})
```

### 3.4 诊断

| 情形 | 文本 |
|---|---|
| 值位置写了 `->` | ``use `=>` for lambdas, match arms and select arms; `->` only introduces a type`` |
| 类型位置写了 `=>` | ``use `->` before a return type or in a function type; `=>` maps a value`` |
| 箭头 lambda 写返回类型 | 沿用现有诊断，修法改为 `fn(x: T) -> R { ... }` 或 `var f: fn(T) -> R = x => ...` |

诊断位置指向错误的箭头本身，并附可机械应用的修法。

---
id: spec.13_built_in_functions
order: 014
---

<!-- xr-spec:cn -->
---

## 13. 内置函数 (Built-in Functions)

> 真值源：`src/ir/xi_lower_expr.c`、`src/vm/xvm_dispatch_*.inc.c`、`src/runtime/object/builtins/`、`src/frontend/analyzer/xanalyzer_builtins.c`。

不需要 `import` 即可使用的全局函数和内置构造/静态函数。下列表格中的 `value` 表示“任意运行时值”，不是一个可写的 `any` 类型；Xray 源码中已没有 `any` 类型。

### 13.1 I/O 与调试

| 函数 | 签名 | 说明 |
|--|--|--|
| `print` | `(...values) -> ()` | 输出到 stdout，自动追加换行；多参以空格分隔 |
| `dump` | `(value, indent?) -> ()` | 结构化调试输出 |

### 13.2 类型转换

| 函数 | 签名 | 说明 |
|--|--|--|
| `int(x)` | `(value) -> int` | 转为 int；字符串解析失败抛异常 |
| `float(x)` | `(value) -> float` | 转为 float |
| `string(x)` | `(value) -> string` | 转为字符串 |
| `bool(x)` | `(value) -> bool` | 转为 bool；规则见 §2.4.1 |
| `chr(n)` | `(int) -> string` | Unicode 码点转单字符字符串 |
| `copy(x)` | `(T) -> T` | 深拷贝，保留运行时类型 |

### 13.3 类型检查

| 函数 / 表达式 | 签名 | 说明 |
|---|---|---|
| `typeof(x)` | `(value) -> string` | 返回运行时类型名字符串 |
| `x is T` | 表达式 | 运行时类型检查，分析器可做类型窄化 |

```xray @id=builtin-typeof-is
let x = 42
print(typeof(x))                // "int"
print(x is int)                 // true
print(typeof(x) == "int")       // true
```

### 13.4 协程

协程启动和等待是语法而不是全局函数：`go`、`await`、`await all`、`await any`、`await anySuccess`。休眠使用 `time.sleep(ms)`。

### 13.5 断言（测试用）

| 函数 | 签名 | 说明 |
|---|---|---|
| `assert(cond, msg?)` | `(bool, string?) -> ()` | `cond` 为 false 时抛异常 |
| `assert_true(cond)` | `(bool) -> ()` | 等价 `assert(cond)` |
| `assert_false(cond)` | `(bool) -> ()` | 等价 `assert(!cond)` |
| `assert_eq(a, b)` | `(T, T) -> ()` | 深相等断言 |
| `assert_ne(a, b)` | `(T, T) -> ()` | 深不等断言 |
| `assert_throws(fn)` | `(fn) -> ()` | 期望函数抛异常 |

### 13.6 容器构造与静态函数

| 函数 | 说明 |
|--|--|
| `Array()` / `Array(n)` / `Array(n, value)` | 创建空数组、指定长度数组或填充值数组 |
| `Array.from(iterable)` | 从 string / Array / Set / Map 创建数组 |
| `Array.range(start, end)` | 创建闭区间整数数组 `[start, ..., end]` |
| `Array.withCapacity(n)` | 创建 length=0、capacity=n 的数组 |
| `Map()` | 创建空 Map |
| `Map.from(entries)` | 从 `[key, value]` pair 数组创建 Map |
| `Map.from(keys, values)` | 从键数组和值数组创建 Map |
| `Set()` / `Set(array)` | 创建空 Set 或从数组创建 Set |
| `Set.from(iterable)` | 从 string / Array / Set 创建 Set |
| `Set.range(start, end)` | 创建闭区间整数 Set |

BigInt 使用 `123n` 字面量或 `int.toBigInt()`；Json 使用 `Json.parse` / `Json.stringify`；DateTime 使用 `datetime` 模块工厂函数。
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 13. Built-in Functions

> Source of truth: `src/ir/xi_lower_expr.c`, `src/vm/xvm_dispatch_*.inc.c`, `src/runtime/object/builtins/`, `src/frontend/analyzer/xanalyzer_builtins.c`.

These global functions and built-in constructor/static functions are usable without any `import`. In the tables below, `value` denotes "any runtime value" — it is **not** a writable `any` type; xray no longer has an `any` type in the source language.

### 13.1 I/O and Debugging

| Function | Signature | Description |
|--|--|--|
| `print` | `(...values) -> ()` | print to stdout, automatically appending a newline; multiple arguments are separated by spaces |
| `dump` | `(value, indent?) -> ()` | structured debug output |

### 13.2 Type Conversion

| Function | Signature | Description |
|--|--|--|
| `int(x)` | `(value) -> int` | convert to int; throws if string parsing fails |
| `float(x)` | `(value) -> float` | convert to float |
| `string(x)` | `(value) -> string` | convert to string |
| `bool(x)` | `(value) -> bool` | convert to bool; rules in §2.4.1 |
| `chr(n)` | `(int) -> string` | Unicode code point → single-character string |
| `copy(x)` | `(T) -> T` | deep copy, preserving runtime type |

### 13.3 Type Checking

| Function / expression | Signature | Description |
|---|---|---|
| `typeof(x)` | `(value) -> string` | returns the runtime type-name string |
| `x is T` | expression | runtime type check; the analyzer may narrow types |

```xray @id=builtin-typeof-is
let x = 42
print(typeof(x))                // "int"
print(x is int)                 // true
print(typeof(x) == "int")       // true
```

### 13.4 Coroutines

Coroutine launch and waiting are syntax, not global functions: `go`, `await`, `await all`, `await any`, `await anySuccess`. For sleeping, use `time.sleep(ms)`.

### 13.5 Assertions (for testing)

| Function | Signature | Description |
|---|---|---|
| `assert(cond, msg?)` | `(bool, string?) -> ()` | throws when `cond` is false |
| `assert_true(cond)` | `(bool) -> ()` | equivalent to `assert(cond)` |
| `assert_false(cond)` | `(bool) -> ()` | equivalent to `assert(!cond)` |
| `assert_eq(a, b)` | `(T, T) -> ()` | deep-equal assertion |
| `assert_ne(a, b)` | `(T, T) -> ()` | deep-not-equal assertion |
| `assert_throws(fn)` | `(fn) -> ()` | expects the function to throw |

### 13.6 Container Constructors and Static Functions

| Function | Description |
|--|--|
| `Array()` / `Array(n)` / `Array(n, value)` | create an empty array, an array of given length, or a value-filled array |
| `Array.from(iterable)` | create an array from a string / Array / Set / Map |
| `Array.range(start, end)` | inclusive integer array `[start, ..., end]` |
| `Array.withCapacity(n)` | array with `length=0` and `capacity=n` |
| `Map()` | empty Map |
| `Map.from(entries)` | Map from `[key, value]` pair array |
| `Map.from(keys, values)` | Map from key array and value array |
| `Set()` / `Set(array)` | empty Set or Set from an array |
| `Set.from(iterable)` | Set from a string / Array / Set |
| `Set.range(start, end)` | inclusive integer Set |

BigInt uses the `123n` literal or `int.toBigInt()`; Json uses `Json.parse` / `Json.stringify`; DateTime uses factory functions in the `datetime` module.
<!-- /xr-spec:en -->

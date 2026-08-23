---
id: spec.13_built_in_functions
order: 014
---

<!-- xr-spec:cn -->
---

## 13. 内置函数 (Built-in Functions)

> 真值源：`src/ir/xi_lower_expr.c`、`src/vm/xvm_dispatch_*.inc.c`、`src/runtime/object/builtins/`、`src/frontend/analyzer/xanalyzer_builtins.c`。

不需要 `import` 即可使用的全局函数和内置构造/静态函数。下列表格中的 `value` 表示“任意运行时值”，只是文档占位符，不是源码中的类型名。

### 13.1 I/O 与调试

| 函数 | 签名 | 说明 |
|--|--|--|
| `print` | `(...values) -> ()` | 输出到 stdout，自动追加换行；多参以空格分隔 |
| `dump` | `(value, indent?) -> ()` | 结构化调试输出 |
| `len` | `(value) -> i64` | 查询实现 `Lengthable` 的 string、容器、Range、Slice 等长度；`JSON.Value` 不实现 `Lengthable` |

### 13.2 类型转换

| 函数 | 签名 | 说明 |
|--|--|--|
| `x as T` | `numeric -> T` | 在 12 个 exact scalar 类型间做显式数值转换；`T` 必须是 `i8` / `i16` / `i32` / `i64` / `u8` / `u16` / `u32` / `u64` / `f32` / `f64` / `isize` / `usize` |
| `i64.parse(s)` | `(string) -> i64` | 严格解析十进制整数；失败抛异常 |
| `i64.tryParse(s)` | `(string) -> i64?` | 严格解析十进制整数；失败返回 `null` |
| `f64.parse(s)` | `(string) -> f64` | 严格解析十进制浮点数；失败抛异常 |
| `f64.tryParse(s)` | `(string) -> f64?` | 严格解析十进制浮点数；失败返回 `null` |
| `string(x)` | `(value) -> string` | 转为字符串；`rune` 转为单 scalar 字符串 |
| `bool(x)` | `(value) -> bool` | 转为 bool；规则见 §2.3.3 |
| `rune(n)` | `(i64) -> rune` | 从整数构造 Unicode scalar；surrogate 或越界值抛异常 |
| `chr(n)` | `(i64) -> string` | Unicode 码点转单 scalar 字符串 |
| `copy(x)` | `(value) -> fresh value` | 显式深拷贝；普通值保留类型形状，借用的 `Slice<T>` / view 则返回独立 owner `Array<T>` |

`i64.parse` / `i64.tryParse` / `f64.parse` / `f64.tryParse` 都解析整个字符串：允许首尾空白与前导符号，其余必须符合十进制文法；`f64` 额外接受小数部分和指数，整数与小数部分合计有一位数字即可（`.5` 与 `1.` 都能解析）。尾部残留、十六进制、`inf` / `nan` 以及越界整数都解析失败。`parse` 用 typed error channel 报错，`tryParse` 返回 `null`。数值之间的转换只能写成显式 `as`，文本解析不经过数值 cast。

### 13.3 类型检查

| 函数 / 表达式 | 签名 | 说明 |
|---|---|---|
| `typeOf(x)` | `(value) -> Type` | 返回稳定 TypeId / `Type.xxx` 值 |
| `typeName(x)` | `(value) -> string` | 返回调试/日志用类型名字符串 |
| `typeName<T>()` | `() -> string` | 返回静态类型 `T` 的名称 |
| `x is T` | 表达式 | 运行时类型检查，分析器可做类型窄化 |

全局只读环境值不是函数：`process`（入口参数/文件/目录信息）、`__file__`、`__dir__`。它们由真实文件/项目入口初始化；纯 `eval` 场景中 `process` 可为 `null`。

```xray @id=builtin-typeOf-is
var x = 42
print(typeOf(x) == Type.i64)    // true
print(typeName(x))              // "i64"
print(x is i64)                 // true
// typeOf(x) == "i64"           // compile error: use Type.i64 or typeName(x)
```

### 13.4 协程

协程启动和等待是语法而不是全局函数：`go`、`await`、`await all`、`await any`、`await anySuccess`。休眠使用 `time.sleep(ms)`。

### 13.5 断言（测试用）

| 函数 | 签名 | 说明 |
|---|---|---|
| `assert(cond, msg?)` | `(bool, string?) -> ()` | `cond` 为 false 时抛异常 |
| `assertEqual(a, b, msg?)` | `(T, T, string?) -> ()` | 同一静态类型的值深相等 |
| `assertThrows(action, msg?)` | `(() -> any, string?) -> ()` | 仅期望 typed error |
| `assertPanics(action, msg?)` | `(() -> any, string?) -> ()` | 仅期望 panic |

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

BigInt 使用 `123n` 字面量或 `i64.toBigInt()`；JSON 使用 `JSON.parse<T>` / `JSON.parseObject` / `JSON.value` / `JSON.stringify`；DateTime 使用 `datetime` 模块工厂函数。
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 13. Built-in Functions

> Source of truth: `src/ir/xi_lower_expr.c`, `src/vm/xvm_dispatch_*.inc.c`, `src/runtime/object/builtins/`, `src/frontend/analyzer/xanalyzer_builtins.c`.

These global functions and built-in constructor/static functions are usable without any `import`. In the tables below, `value` denotes "any runtime value"; it is a documentation placeholder rather than a writable source-language type.

### 13.1 I/O and Debugging

| Function | Signature | Description |
|--|--|--|
| `print` | `(...values) -> ()` | print to stdout, automatically appending a newline; multiple arguments are separated by spaces |
| `dump` | `(value, indent?) -> ()` | structured debug output |
| `len` | `(value) -> i64` | length of strings, containers, Range, Slice, and other `Lengthable` values; `JSON.Value` is not `Lengthable` |

### 13.2 Type Conversion

| Function | Signature | Description |
|--|--|--|
| `x as T` | `numeric -> T` | explicit numeric conversion among the 12 exact scalar types; `T` is `i8` / `i16` / `i32` / `i64` / `u8` / `u16` / `u32` / `u64` / `f32` / `f64` / `isize` / `usize` |
| `i64.parse(s)` | `(string) -> i64` | strict decimal integer parse; throws on failure |
| `i64.tryParse(s)` | `(string) -> i64?` | strict decimal integer parse; returns `null` on failure |
| `f64.parse(s)` | `(string) -> f64` | strict decimal floating-point parse; throws on failure |
| `f64.tryParse(s)` | `(string) -> f64?` | strict decimal floating-point parse; returns `null` on failure |
| `string(x)` | `(value) -> string` | convert to string; `rune` converts to a one-scalar string |
| `bool(x)` | `(value) -> bool` | convert to bool; rules in §2.3.3 |
| `rune(n)` | `(i64) -> rune` | construct a Unicode scalar from an integer; surrogate and out-of-range values throw |
| `chr(n)` | `(i64) -> string` | Unicode code point → one-scalar string |
| `copy(x)` | `(value) -> fresh value` | explicit deep copy; ordinary values preserve their type shape, while a borrowed `Slice<T>` / view returns an independent owner `Array<T>` |

`i64.parse` / `i64.tryParse` / `f64.parse` / `f64.tryParse` consume the whole string. Surrounding whitespace and a leading sign are accepted; `f64` additionally accepts a fractional part and exponent, with at least one digit across the integer and fractional parts (`.5` and `1.` are valid). Trailing residue, hexadecimal input, `inf` / `nan`, and out-of-range integers fail. `parse` reports failure through the typed error channel; `tryParse` returns `null`. Numeric conversions use explicit `as`; text parsing is not a numeric cast.

### 13.3 Type Checking

| Function / expression | Signature | Description |
|---|---|---|
| `typeOf(x)` | `(value) -> Type` | returns a stable TypeId / `Type.xxx` value |
| `typeName(x)` | `(value) -> string` | returns the debug/logging type-name string |
| `typeName<T>()` | `() -> string` | returns the name of static type `T` |
| `x is T` | expression | runtime type check; the analyzer may narrow types |

The global read-only environment values are not functions: `process` (entry arguments/file/directory), `__file__`, and `__dir__`. They are initialized for a real file/project entry; `process` may be `null` in a pure `eval` context.

```xray @id=builtin-typeOf-is
var x = 42
print(typeOf(x) == Type.i64)    // true
print(typeName(x))              // "i64"
print(x is i64)                 // true
// typeOf(x) == "i64"           // compile error: use Type.i64 or typeName(x)
```

### 13.4 Coroutines

Coroutine launch and waiting are syntax, not global functions: `go`, `await`, `await all`, `await any`, `await anySuccess`. For sleeping, use `time.sleep(ms)`.

### 13.5 Assertions (for testing)

| Function | Signature | Description |
|---|---|---|
| `assert(cond, msg?)` | `(bool, string?) -> ()` | throws when `cond` is false |
| `assertEqual(a, b, msg?)` | `(T, T, string?) -> ()` | deep equality for one static type |
| `assertThrows(action, msg?)` | `(() -> any, string?) -> ()` | expects only a typed error |
| `assertPanics(action, msg?)` | `(() -> any, string?) -> ()` | expects only a panic |

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

BigInt uses the `123n` literal or `i64.toBigInt()`; JSON uses `JSON.parse<T>` / `JSON.parseObject` / `JSON.value` / `JSON.stringify`; DateTime uses factory functions in the `datetime` module.
<!-- /xr-spec:en -->

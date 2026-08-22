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
| `len` | `(value) -> int` | 查询实现 `Lengthable` 的 string、容器、Range、Slice 等长度；`JSON.Value` 不实现 `Lengthable` |

### 13.2 类型转换

| 函数 | 签名 | 说明 |
|--|--|--|
| `int(x)` | `(value) -> int` | 转为 int；`rune` 转为 Unicode scalar code point；字符串解析失败抛异常 |
| `float(x)` | `(value) -> float` | 转为 float |
| `string(x)` | `(value) -> string` | 转为字符串；`rune` 转为单 scalar 字符串 |
| `bool(x)` | `(value) -> bool` | 转为 bool；规则见 §2.3.3 |
| `rune(n)` | `(int) -> rune` | 从整数构造 Unicode scalar；surrogate 或越界值抛异常 |
| `chr(n)` | `(int) -> string` | Unicode 码点转单 scalar 字符串 |
| `copy(x)` | `(value) -> fresh value` | 显式深拷贝；普通值保留类型形状，借用的 `Slice<T>` / view 则返回独立 owner `Array<T>` |

`int(s)` / `float(s)` 解析整个字符串，不被文法接受的输入一律抛异常：允许首尾空白与前导符号，其余必须是十进制数字；`float` 额外接受小数部分和指数，整数与小数部分合计有一位数字即可（`.5` 与 `1.` 都能解析）。尾部残留是解析失败而不是前缀解析，因此 `int("12abc")` 抛异常而不是得到 `12`；十六进制与 `inf` / `nan` 写法同样拒绝，整数超出 `int` 范围也是拒绝而不是饱和。这与 `strconv.parseInt` / `strconv.parseFloat` 已有的文法一致（§15.8）。

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
print(typeOf(x) == Type.int)    // true
print(typeName(x))              // "int"
print(x is int)                 // true
// typeOf(x) == "int"           // compile error: use Type.int or typeName(x)
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

BigInt 使用 `123n` 字面量或 `int.toBigInt()`；JSON 使用 `JSON.parse<T>` / `JSON.parseObject` / `JSON.value` / `JSON.stringify`；DateTime 使用 `datetime` 模块工厂函数。
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
| `len` | `(value) -> int` | length of strings, containers, Range, Slice, and other `Lengthable` values; `JSON.Value` is not `Lengthable` |

### 13.2 Type Conversion

| Function | Signature | Description |
|--|--|--|
| `int(x)` | `(value) -> int` | convert to int; `rune` converts to its Unicode scalar code point; throws if string parsing fails |
| `float(x)` | `(value) -> float` | convert to float |
| `string(x)` | `(value) -> string` | convert to string; `rune` converts to a one-scalar string |
| `bool(x)` | `(value) -> bool` | convert to bool; rules in §2.3.3 |
| `rune(n)` | `(int) -> rune` | construct a Unicode scalar from an integer; surrogate and out-of-range values throw |
| `chr(n)` | `(int) -> string` | Unicode code point → one-scalar string |
| `copy(x)` | `(value) -> fresh value` | explicit deep copy; ordinary values preserve their type shape, while a borrowed `Slice<T>` / view returns an independent owner `Array<T>` |

`int(s)` and `float(s)` parse the whole string, and anything the grammar does not accept throws: surrounding whitespace and a leading sign are allowed, and the rest must be decimal digits; `float` also accepts a fractional part and an exponent, and needs only one digit across the integer and fractional parts (`.5` and `1.` both parse). Trailing residue is a parse failure rather than a prefix parse, so `int("12abc")` throws instead of yielding `12`; hex and the `inf` / `nan` spellings are rejected as well, and an integer outside the `int` range is rejected rather than saturated. This is the grammar `strconv.parseInt` / `strconv.parseFloat` already enforce (§15.8).

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
print(typeOf(x) == Type.int)    // true
print(typeName(x))              // "int"
print(x is int)                 // true
// typeOf(x) == "int"           // compile error: use Type.int or typeName(x)
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

BigInt uses the `123n` literal or `int.toBigInt()`; JSON uses `JSON.parse<T>` / `JSON.parseObject` / `JSON.value` / `JSON.stringify`; DateTime uses factory functions in the `datetime` module.
<!-- /xr-spec:en -->

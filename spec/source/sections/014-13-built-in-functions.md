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
| `print` | `(...values) -> ()` | 输出到 stdout，追加恰好一个换行；多参以恰好一个空格分隔；`print()` 只输出换行。一次调用是一个不可分割的组：所有参数先从左到右各求值一次，再全部渲染，最后一次性写出 |
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
| `assert(cond, msg?)` | `(cond: bool, msg: string = "") -> ()` | false 产生 assertion panic |
| `assertEqual<T: Equal>(a, b, msg?)` | `(a: T, b: T, msg: string = "") -> ()` | 普通 TYPE 参数，在定义处证明 Equal 的同T比较 |
| `assertThrows(action, msg?)` | `(action: fn() -> R, msg: string = "") -> ()` | 仅期望 action 调用的 typed error |
| `assertPanics(action, msg?)` | `(action: fn() -> R, msg: string = "") -> ()` | 仅期望 action 调用的 panic |

`msg?` 只表示可以省略，不表示 `string?`；显式 null、可空字符串及非字符串均非法。
条件/action 表达式和消息按源序各求值一次，成功路径也求值消息；省略使用唯一默认参数机制。
条件断言失败时，`PanicInfo.message` 精确保存已求值的 `msg` 字节与长度；省略消息得到空字符串。
错误码、源码位置及显示前缀由诊断层单独呈现，不拼入该消息，不截断或按 C 字符串终止符读取。
`R` 是 builtin 从已检查 callable 签名取得的精确结果变量，不是普通源码泛型形参或 `any`。
Unit action 类型写 `fn()`，零 payload，不创建普通结果槽；其它结果须满足已准入的
可复制、可保存结果合同。正常返回先销毁结果恰好一次，再产生缺失期望的 assertion panic。
视图及不可复制结果须待各自返回合同准入，不放宽普通泛型 Unit 或存储规则。

保护域只包括 action 调用及清理，action/msg 表达式本身的失败不在域内。typed error/panic
不同；OOM、LIMIT、取消及 provider/ABI/准入失败不能使断言成功。未知 callable 效应保持未知，
不凭具体实例函数体补承诺；词法绑定优先于 builtin 名称。assertEqual 的完整能力及类型域
按定义处约束合同逐族准入，无约束 T 不在特化时获得深比较权限。普通相等与容器键等价保持
区别；含 NaN 的数组即使共享 backing 也遵守逐元素关系，memcmp 须有额外 bytewise 证明。
首个有限 Equal 域为 bool、八种定宽整数、f32/f64、string 和递归 Array<T: Equal>，
详见 §9.2/§17.33；普通 ==、!= 与本断言使用同一关系和证明。Equal 不授自反性或键等价，
也不是 Equatable 别名。a、b、msg 各按源序求值一次，成功也求值消息；仅正常比较得到 false
才触发 assertion panic，比较 OOM/LIMIT 保持资源通道。完整相等能力域与正式实现资格仍未完成。
本节是声明合同，不代表 §17 尚未准入的断言执行族已实现。

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
| `print` | `(...values) -> ()` | print to stdout, appending exactly one newline; arguments are separated by exactly one space; `print()` writes just the newline. One call is one indivisible group: every argument is evaluated left to right exactly once, then all of them are rendered, then the group is written in a single act |
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
| `assert(cond, msg?)` | `(cond: bool, msg: string = "") -> ()` | false produces an assertion panic |
| `assertEqual<T: Equal>(a, b, msg?)` | `(a: T, b: T, msg: string = "") -> ()` | ordinary TYPE parameter with definition-site Equal proof for same-T comparison |
| `assertThrows(action, msg?)` | `(action: fn() -> R, msg: string = "") -> ()` | expects only the action call's typed error |
| `assertPanics(action, msg?)` | `(action: fn() -> R, msg: string = "") -> ()` | expects only the action call's panic |

`msg?` denotes omission, not `string?`; explicit null, nullable strings and other types are invalid.
The condition/action and message expressions are each evaluated once in source order, including
success. Omission uses the single parameter-default mechanism. `R` is the builtin's exact result
variable from the checked callable signature, not an ordinary source generic parameter or `any`.
A Unit action has type `fn()` with zero payload and no ordinary result slot. Other results require
the admitted copyable, storable result contract. A normal result is destroyed exactly once before
the missing expected failure produces an assertion panic. View and noncopyable results require
their own return contracts; ordinary generic Unit and storage rules are not relaxed.

When a condition assertion fails, `PanicInfo.message` preserves the exact bytes and length of the
evaluated `msg`; an omitted message is the empty string. Diagnostics render the error code,
source location and display prefix separately, without adding them to the message, truncating
its bytes or treating a C string terminator as its boundary.

The protected region covers only the action call and cleanup, not evaluation of action/message
expressions. Typed errors and panics differ; OOM, LIMIT, cancellation and provider/ABI/admission
failures cannot satisfy an expectation. Unknown callable effects remain unknown, rather than being
inferred from a concrete instance's body. Lexical bindings precede builtin names. The complete
assertEqual capability/type domain is admitted by definition-site constraints, not by granting an
unconstrained T deep comparison authority during specialization. Ordinary equality differs from
container key equivalence. Arrays containing NaN preserve element-wise equality even with shared
backing; memcmp needs an additional bytewise proof. The first finite Equal domain is bool,
the eight fixed-width integers, f32/f64, string and recursive Array<T: Equal>; see §9.2/§17.33.
Ordinary ==, != and this assertion use the same relation and proof. Equal promises neither
reflexivity nor key equivalence, and is not an Equatable alias. Evaluate a, b and msg once in
source order, including success; only a successful false comparison triggers an assertion panic,
while comparison OOM/LIMIT retains its resource channel. The complete domain and implementation
qualification remain open; this section does not claim that the assertion execution family
missing from §17 has been implemented.

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

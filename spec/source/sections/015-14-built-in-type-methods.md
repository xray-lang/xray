---
id: spec.14_built_in_type_methods
order: 015
---

<!-- xr-spec:cn -->
---

## 14. 内置类型方法 (Built-in Type Methods)

> 真值源：prelude / analyzer / runtime 中的内置类型注册与方法定义。
> MCP knowledge 只消费生成后的 analyzer metadata，不独立维护内置类型方法签名。

本节给出每种类型的**方法索引**（按主题分组）。具体签名、参数说明、行为细节以实现代码为准。

### 14.1 `int` 方法

| 方法 | 签名 | 说明 |
|--|--|--|
| `abs()` | `() -> int` | 绝对值 |
| `toString()` | `() -> string` | 十进制字符串 |
| `toBigInt()` | `() -> BigInt` | 转 BigInt |
| `toFloat()` | `() -> float` | 转 float |
| `toHex()` | `() -> string` | 十六进制字符串 |
| `max(other)` / `min(other)` | `(int) -> int` | 双值最值 |
| `floor()` / `ceil()` / `round()` | `() -> int` | 对 int 返回自身 |
| `sqrt()` | `() -> float` | 平方根 |
| `pow(exp)` | `(float) -> float` | 幂运算 |
| `checkedAdd(other)` / `checkedSub(other)` / `checkedMul(other)` | `(int) -> int?` | 溢出返回 `null` |
| `saturatingAdd(other)` / `saturatingSub(other)` / `saturatingMul(other)` | `(int) -> int` | 溢出饱和到 `int` 边界 |
| `wrappingAdd(other)` / `wrappingSub(other)` / `wrappingMul(other)` | `(int) -> int` | 显式二补码环绕 |

`abs()` 遵循整数环绕语义：`(-9223372036854775807 - 1).abs()` 返回自身。`toHex()` 对负数使用带符号前缀，例如 `-0x8000000000000000`。

### 14.2 `float` 方法

| 方法 | 签名 | 说明 |
|--|--|--|
| `abs()` | `() -> float` | 绝对值 |
| `toString()` | `() -> string` | 字符串化 |
| `toFixed(decimals?)` | `(int?) -> string` | 固定位数小数字符串 |
| `toInt()` | `() -> int` | 转 int |
| `floor()` / `ceil()` / `round()` | `() -> int` | 取整 |
| `sqrt()` | `() -> float` | 平方根 |
| `pow(exp)` | `(float) -> float` | 幂运算 |
| `isNaN()` | `() -> bool` | 是否为 IEEE NaN |

### 14.3 `BigInt` 方法

| 方法 | 签名 | 说明 |
|--|--|--|
| `abs()` | `() -> BigInt` | 绝对值 |
| `toString()` | `() -> string` | 字符串化 |
| `sign()` | `() -> int` | -1 / 0 / 1 |
| `isZero()` / `isNegative()` / `isPositive()` | `() -> bool` | 符号判断 |
| `toInt()` | `() -> int?` | 无法表示时返回 null |
| `toFloat()` | `() -> float` | 转 float |

### 14.4 `bool` 方法

| 方法 | 签名 | 说明 |
|--|--|--|
| `toString()` | `() -> string` | 返回 `"true"` 或 `"false"` |

### 14.5 `string` 方法

| 成员 | 类型 / 说明 |
|--|--|
| `length` | 字符串长度属性 |
| `charAt(i)` | 返回指定位置字符 |
| `charCodeAt(i)` | 返回码点 |
| `concat(...others)` | 拼接字符串 |
| `includes(s)` | 是否包含子串 |
| `indexOf(s)` / `lastIndexOf(s)` | 查找子串 |
| `slice(start, end?)` / `substring(start, end?)` / `substr(start, len?)` | 子串 |
| `toLowerCase()` / `toUpperCase()` | 大小写转换 |
| `trim()` / `trimStart()` / `trimEnd()` | 去空白 |
| `split(sep, limit?)` | 分割为 `Array<string>` |
| `replace(from, to)` / `replaceAll(from, to)` | 替换 |
| `repeat(n)` | 重复 |
| `startsWith(s)` / `endsWith(s)` | 前缀/后缀判断 |
| `padStart(len, pad?)` / `padEnd(len, pad?)` | 填充 |
| `match(pattern)` | 正则匹配 |
| `iterator()` / `entriesIterator()` / `entries()` | 迭代协议 |

`slice(start, end?)` 使用与切片表达式相同的半开区间和负索引规则：负索引先按 `length + index` 从末尾计数，再夹到 `[0, length]`。

### 14.6 `Bytes`

`Bytes` 是 prelude 类型，构造由 `Bytes(n)` / `Bytes(n, fill)` 等内置路径处理。字符串转换和编码类操作优先使用 `encoding` / `base64` 模块。当前没有单独的 `stdlib/types/bytes.xr` 声明；工具不要假设存在完整 Array 同构 API。

### 14.7 `Array<T>` 方法

| 成员 | 类型/说明 |
|--|--|
| `length` | `int` 属性 |
| `arr[i]` / `arr[i] = v` | 下标读写 |
| `push(x)` / `pop()` | 尾部增删 |
| `shift()` / `unshift(x)` | 头部增删 |
| `slice(start?, end?)` | 切片 |
| `splice(start, deleteCount, ...items)` | 原地增删 |
| `concat(...arrays)` | 拼接 |
| `indexOf(x)` / `includes(x)` | 查找 |
| `join(sep?)` | 拼接为字符串 |
| `reverse()` / `sort(cmp?)` | 原地重排 |
| `map(fn)` / `filter(fn)` / `reduce(fn, init)` | 函数式处理 |
| `forEach(fn)` / `find(fn)` / `findIndex(fn)` / `every(fn)` / `some(fn)` | 遍历与谓词 |
| `flat(depth?)` / `fill(v, start?, end?)` / `copyWithin(target, start, end?)` | 数组工具 |
| `iterator()` / `entriesIterator()` / `entries()` | 迭代协议 |

`slice(start?, end?)` 使用与切片表达式相同的半开区间和负索引规则；返回独立数组，原数组不变。

### 14.8 `Map<K, V>` 方法

| 成员 | 类型/说明 |
|--|--|
| `length` | `int` 属性 |
| `m[k]` / `m[k] = v` | 下标读写 |
| `get(k)` / `set(k, v)` | 读取/写入 |
| `has(k)` / `delete(k)` / `clear()` | 查询与删除 |
| `keys()` / `values()` / `entries()` | 返回键、值、键值对 |
| `forEach(fn)` | 遍历 |
| `iterator()` / `entriesIterator()` | 迭代协议 |

**Map 字面量**：`#{"k1": v1, "k2": v2}` 或 `#{}`；使用 `:`，靠 `#` 前缀区别于 Object/Json 字面量。

### 14.9 `Set<T>` 方法

| 成员 | 类型/说明 |
|--|--|
| `length` | `int` 属性 |
| `add(x)` / `has(x)` / `delete(x)` | 插入、查询、删除 |
| `clear()` | 清空 |
| `values()` | 返回 `Array<T>` |
| `forEach(fn)` | 遍历 |
| `iterator()` | 迭代协议 |

**Set 字面量**：`#[1, 2, 3]` 或 `#[]`。

### 14.10 `Channel<T>` 方法

| 成员 | 类型/说明 |
|--|--|
| `send(v)` | 阻塞发送；channel 已关闭时抛异常 |
| `recv()` | 阻塞接收，返回 `Recv<T>`；关闭且缓冲为空时为 `Recv.Closed` |
| `trySend(v)` | 非阻塞发送，返回 `SendResult` |
| `tryRecv()` | 非阻塞接收，返回 `Recv<T>`；空时为 `Recv.Empty` |
| `sendTimeout(v, ms)` | 带超时发送，返回 `SendResult`；超时为 `SendResult.Timeout` |
| `recvTimeout(ms)` | 带超时接收，返回 `Recv<T>`；超时为 `Recv.Timeout` |
| `close()` | 关闭 channel |
| `isClosed` / `isClosed()` | 关闭状态；运行时属性和方法均支持 |

`Recv.Value(v)` 中的 `v` 就是 channel payload，因此 `Channel<int?>` 可以区分真实的 `Recv.Value(null)` 和 `Recv.Closed`。

### 14.11 `Json`

`Json` 是动态结构化数据类型。普通字段访问使用 `j.field` / `j["field"]`；通用查询和编解码通过 `Json` 静态函数完成，避免与用户字段名冲突。

| 静态函数 | 说明 |
|--|--|
| `Json.keys(obj)` / `Json.values(obj)` / `Json.entries(obj)` | Object 字段枚举 |
| `Json.has(obj, key)` | 字段存在性 |
| `Json.get(obj, key, default?)` | 字段读取，不存在返回 default 或 null |
| `Json.size(obj)` | 字段数量 |
| `Json.isEmpty(obj)` | 是否为空 |
| `Json.parse(s)` / `Json.tryParse(s)` / `Json.isValid(s)` | JSON 解析与校验 |
| `Json.stringify(value, indent?)` | 序列化 |

**字面量**：`{ name: "alice", age: 30 }`，动态类型为 `Json`。如需 sealed 对象，用 `type T = { name: string, age: int }` 标注。

### 14.12 `Range`

`a..b` 是半开区间 `[a, b)`，用于表达式和 `for-in`。常见成员为 `start`、`end`、`length`、`includes(x)`、`toArray()`、`toString()`。

### 14.13 `DateTime`

通过 `import datetime` 获得工厂函数：`now`、`utc`、`create`、`createUTC`、`fromTimestamp`、`fromTimestampMs`、`parse`、`offset`。`DateTime` 实例由 prelude 注册，无需 import 类型名。

| 成员 | 类型/说明 |
|--|--|
| `year` / `month` / `day` | 日期分量属性 |
| `hour` / `minute` / `second` / `millisecond` | 时间分量属性 |
| `weekday` / `yearday` / `timestamp` | 派生属性 |
| `toString()` / `format(pattern?)` / `toISOString()` | 格式化 |
| `add(amount, unit)` / `diff(other, unit?)` | 日期运算 |
| `toUTC()` / `toLocal()` | 时区转换 |
| `isBefore(other)` / `isAfter(other)` / `equals(other)` | 比较 |
| `isLeapYear()` / `daysInMonth()` | 日历查询 |

### 14.14 `Regex`

| 方法 | 说明 |
|--|--|
| `test(s)` | 是否匹配 |
| `find(s)` | 首个匹配 |
| `findAll(s)` | 所有匹配 |
| `replace(s, replacement)` | 替换 |
| `split(s)` | 分割 |

### 14.15 `StringBuilder`

| 方法 | 说明 |
|--|--|
| `length` | 当前长度属性 |
| `append(s)` | 追加并返回自身 |
| `toString()` | 输出字符串 |
| `clear()` | 清空并返回自身 |

### 14.16 `Exception`

内置 `Exception` 类包含 `message`、`stack`、`cause`、`code`、`data` 字段，构造函数 `constructor(message: string = "", cause: Exception? = null)`，以及 `toString()`。

### 14.17 `Task<T>` / `EnumValue` / `EnumType`

`Task<T>` 属性：`done`、`status`；方法：`cancel()`、`poll()`、`awaitResult()`、`awaitTimeout(ms)`。`poll()` 和显式等待方法返回 `TaskResult<T>`，plain `await task` 成功时返回 `T`，失败或取消时走异常路径。`EnumValue` 属性：`name`、`value`、`ordinal`，方法：`toString()`。`EnumType` 属性：`name`、`memberCount`，方法：`getMember(name)`。

### 14.18 其他 prelude 类型（`Logger` / `NetConn` / `NetListener`）

这些类型由 prelude 注册，实例由 `log` / `net` 等模块工厂函数构造。完整运行时能力以对应 stdlib 模块为准。

### 14.19 `Atomic<T>` 方法

`Atomic<T>` 包装 `int`、`float` 或 `bool`，提供无锁原子操作。必须声明为 `shared const`，禁止 `move`。

| 方法 | 签名 | 说明 |
|--|--|--|
| `load(ord?)` | `(Ordering?) -> T` | 原子读取当前值 |
| `store(val, ord?)` | `(T, Ordering?) -> ()` | 原子写入 |
| `add(val, ord?)` | `(T, Ordering?) -> ()` | 原子加 |
| `sub(val, ord?)` | `(T, Ordering?) -> ()` | 原子减 |
| `fetchAdd(val, ord?)` | `(T, Ordering?) -> T` | 原子加并返回旧值 |
| `fetchSub(val, ord?)` | `(T, Ordering?) -> T` | 原子减并返回旧值 |
| `swap(val, ord?)` | `(T, Ordering?) -> T` | 原子交换，返回旧值 |
| `compareExchange(expected, desired, ord?)` | `(T, T, Ordering?) -> (T, bool)` | CAS，返回 `(旧值, 是否成功)` |
| `toggle(ord?)` | `(Ordering?) -> bool` | 原子取反（仅 bool），返回旧值 |
| `toString()` | `() -> string` | 返回当前值的字符串表示 |

`ord?` 参数接受 `Ordering` 枚举，默认 `Ordering.SeqCst`。详见 §10.9。
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 14. Built-in Type Methods

> Source of truth: prelude / analyzer / runtime built-in type registration and method definitions.
> MCP knowledge only consumes the generated analyzer metadata; it does not maintain its own copy of built-in method signatures.

This section is a **method index** for each type (grouped by topic). Concrete signatures, parameter descriptions, and behavioral details are governed by the implementation source.

### 14.1 `int` Methods

| Method | Signature | Description |
|--|--|--|
| `abs()` | `() -> int` | absolute value |
| `toString()` | `() -> string` | decimal string |
| `toBigInt()` | `() -> BigInt` | convert to BigInt |
| `toFloat()` | `() -> float` | convert to float |
| `toHex()` | `() -> string` | hexadecimal string |
| `max(other)` / `min(other)` | `(int) -> int` | binary max/min |
| `floor()` / `ceil()` / `round()` | `() -> int` | for `int`, returns self |
| `sqrt()` | `() -> float` | square root |
| `pow(exp)` | `(float) -> float` | power |
| `checkedAdd(other)` / `checkedSub(other)` / `checkedMul(other)` | `(int) -> int?` | returns `null` on overflow |
| `saturatingAdd(other)` / `saturatingSub(other)` / `saturatingMul(other)` | `(int) -> int` | clamps overflow to the `int` boundary |
| `wrappingAdd(other)` / `wrappingSub(other)` / `wrappingMul(other)` | `(int) -> int` | explicit two's-complement wrap |

`abs()` follows integer wrap semantics: `(-9223372036854775807 - 1).abs()` returns itself. `toHex()` keeps a sign prefix for negative values, for example `-0x8000000000000000`.

### 14.2 `float` Methods

| Method | Signature | Description |
|--|--|--|
| `abs()` | `() -> float` | absolute value |
| `toString()` | `() -> string` | string conversion |
| `toFixed(decimals?)` | `(int?) -> string` | fixed-decimal string |
| `toInt()` | `() -> int` | convert to int |
| `floor()` / `ceil()` / `round()` | `() -> int` | rounding |
| `sqrt()` | `() -> float` | square root |
| `pow(exp)` | `(float) -> float` | power |
| `isNaN()` | `() -> bool` | whether the value is IEEE NaN |

### 14.3 `BigInt` Methods

| Method | Signature | Description |
|--|--|--|
| `abs()` | `() -> BigInt` | absolute value |
| `toString()` | `() -> string` | string conversion |
| `sign()` | `() -> int` | -1 / 0 / 1 |
| `isZero()` / `isNegative()` / `isPositive()` | `() -> bool` | sign predicates |
| `toInt()` | `() -> int?` | returns null when not representable as `int` |
| `toFloat()` | `() -> float` | convert to float |

### 14.4 `bool` Methods

| Method | Signature | Description |
|--|--|--|
| `toString()` | `() -> string` | returns `"true"` or `"false"` |

### 14.5 `string` Methods

| Member | Type / Description |
|--|--|
| `length` | string-length property |
| `charAt(i)` | character at the given index |
| `charCodeAt(i)` | code point at the given index |
| `concat(...others)` | concatenate strings |
| `includes(s)` | substring containment test |
| `indexOf(s)` / `lastIndexOf(s)` | substring search |
| `slice(start, end?)` / `substring(start, end?)` / `substr(start, len?)` | substrings |
| `toLowerCase()` / `toUpperCase()` | case conversion |
| `trim()` / `trimStart()` / `trimEnd()` | whitespace trimming |
| `split(sep, limit?)` | split into `Array<string>` |
| `replace(from, to)` / `replaceAll(from, to)` | replacement |
| `repeat(n)` | repeat |
| `startsWith(s)` / `endsWith(s)` | prefix/suffix check |
| `padStart(len, pad?)` / `padEnd(len, pad?)` | padding |
| `match(pattern)` | regex match |
| `iterator()` / `entriesIterator()` / `entries()` | iteration protocol |

`slice(start, end?)` uses the same half-open range and negative-index rules as slice expressions: a negative index is first converted as `length + index`, then clamped into `[0, length]`.

### 14.6 `Bytes`

`Bytes` is a prelude type; construction is handled via builtin paths such as `Bytes(n)` / `Bytes(n, fill)`. String conversion and encoding-related operations should prefer the `encoding` / `base64` modules. There is currently no separate `stdlib/types/bytes.xr` declaration; tooling should not assume a complete Array-isomorphic API.

### 14.7 `Array<T>` Methods

| Member | Type / Description |
|--|--|
| `length` | `int` property |
| `arr[i]` / `arr[i] = v` | indexed read/write |
| `push(x)` / `pop()` | tail insert/remove |
| `shift()` / `unshift(x)` | head insert/remove |
| `slice(start?, end?)` | slicing |
| `splice(start, deleteCount, ...items)` | in-place insert/remove |
| `concat(...arrays)` | concatenation |
| `indexOf(x)` / `includes(x)` | search |
| `join(sep?)` | concatenate into a string |
| `reverse()` / `sort(cmp?)` | in-place reorder |
| `map(fn)` / `filter(fn)` / `reduce(fn, init)` | functional helpers |
| `forEach(fn)` / `find(fn)` / `findIndex(fn)` / `every(fn)` / `some(fn)` | traversal and predicates |
| `flat(depth?)` / `fill(v, start?, end?)` / `copyWithin(target, start, end?)` | array utilities |
| `iterator()` / `entriesIterator()` / `entries()` | iteration protocol |

`slice(start?, end?)` uses the same half-open range and negative-index rules as slice expressions; it returns an independent array and leaves the original array unchanged.

### 14.8 `Map<K, V>` Methods

| Member | Type / Description |
|--|--|
| `length` | `int` property |
| `m[k]` / `m[k] = v` | indexed read/write |
| `get(k)` / `set(k, v)` | read/write |
| `has(k)` / `delete(k)` / `clear()` | query and remove |
| `keys()` / `values()` / `entries()` | keys, values, key/value pairs |
| `forEach(fn)` | traversal |
| `iterator()` / `entriesIterator()` | iteration protocol |

**Map literal**: `#{"k1": v1, "k2": v2}` or `#{}`; entries use `:`, distinguished from Object/Json literals by the `#` prefix.

### 14.9 `Set<T>` Methods

| Member | Type / Description |
|--|--|
| `length` | `int` property |
| `add(x)` / `has(x)` / `delete(x)` | insert, query, remove |
| `clear()` | empty the set |
| `values()` | returns `Array<T>` |
| `forEach(fn)` | traversal |
| `iterator()` | iteration protocol |

**Set literal**: `#[1, 2, 3]` or `#[]`.

### 14.10 `Channel<T>` Methods

| Member | Type / Description |
|--|--|
| `send(v)` | blocking send; throws if the channel is closed |
| `recv()` | blocking receive, returns `Recv<T>`; closed and drained is `Recv.Closed` |
| `trySend(v)` | non-blocking send, returns `SendResult` |
| `tryRecv()` | non-blocking receive, returns `Recv<T>`; empty is `Recv.Empty` |
| `sendTimeout(v, ms)` | timed send, returns `SendResult`; timeout is `SendResult.Timeout` |
| `recvTimeout(ms)` | timed receive, returns `Recv<T>`; timeout is `Recv.Timeout` |
| `close()` | close the channel |
| `isClosed` / `isClosed()` | closed state; both runtime property and method are supported |

`Recv.Value(v)` carries the channel payload, so `Channel<int?>` can distinguish a real `Recv.Value(null)` from `Recv.Closed`.

### 14.11 `Json`

`Json` is a dynamic structured-data type. Ordinary field access uses `j.field` / `j["field"]`; generic queries and serialization go through `Json` static functions to avoid colliding with user field names.

| Static function | Description |
|--|--|
| `Json.keys(obj)` / `Json.values(obj)` / `Json.entries(obj)` | enumerate object fields |
| `Json.has(obj, key)` | field existence |
| `Json.get(obj, key, default?)` | field read; returns `default` or `null` if absent |
| `Json.size(obj)` | number of fields |
| `Json.isEmpty(obj)` | emptiness predicate |
| `Json.parse(s)` / `Json.tryParse(s)` / `Json.isValid(s)` | JSON parsing and validation |
| `Json.stringify(value, indent?)` | serialization |

**Literal**: `{ name: "alice", age: 30 }` has dynamic type `Json`. For sealed objects, annotate with `type T = { name: string, age: int }`.

### 14.12 `Range`

`a..b` is the half-open interval `[a, b)`, used in expressions and `for-in`. Common members: `start`, `end`, `length`, `includes(x)`, `toArray()`, `toString()`.

### 14.13 `DateTime`

The `import datetime` module provides factory functions: `now`, `utc`, `create`, `createUTC`, `fromTimestamp`, `fromTimestampMs`, `parse`, `offset`. `DateTime` instances are registered by the prelude, so the type name need not be imported.

| Member | Type / Description |
|--|--|
| `year` / `month` / `day` | date-component properties |
| `hour` / `minute` / `second` / `millisecond` | time-component properties |
| `weekday` / `yearday` / `timestamp` | derived properties |
| `toString()` / `format(pattern?)` / `toISOString()` | formatting |
| `add(amount, unit)` / `diff(other, unit?)` | date arithmetic |
| `toUTC()` / `toLocal()` | timezone conversion |
| `isBefore(other)` / `isAfter(other)` / `equals(other)` | comparison |
| `isLeapYear()` / `daysInMonth()` | calendar queries |

### 14.14 `Regex`

| Method | Description |
|--|--|
| `test(s)` | match predicate |
| `find(s)` | first match |
| `findAll(s)` | all matches |
| `replace(s, replacement)` | replacement |
| `split(s)` | split |

### 14.15 `StringBuilder`

| Method | Description |
|--|--|
| `length` | current length property |
| `append(s)` | append and return self |
| `toString()` | output string |
| `clear()` | empty and return self |

### 14.16 `Exception`

The built-in `Exception` class has fields `message`, `stack`, `cause`, `code`, `data`, the constructor `constructor(message: string = "", cause: Exception? = null)`, and `toString()`.

### 14.17 `Task<T>` / `EnumValue` / `EnumType`

`Task<T>` properties: `done`, `status`; methods: `cancel()`, `poll()`, `awaitResult()`, `awaitTimeout(ms)`. `poll()` and explicit wait methods return `TaskResult<T>`; plain `await task` returns `T` on success and uses the exception path for failure or cancellation. `EnumValue` properties: `name`, `value`, `ordinal`; methods: `toString()`. `EnumType` properties: `name`, `memberCount`; methods: `getMember(name)`.

### 14.18 Other Prelude Types (`Logger` / `NetConn` / `NetListener`)

These types are registered by the prelude; instances are constructed by factory functions in modules such as `log` / `net`. The complete runtime capability follows the corresponding stdlib module.

### 14.19 `Atomic<T>` Methods

`Atomic<T>` wraps `int`, `float`, or `bool` with lock-free atomic operations. Must be declared as `shared const`; `move` is prohibited.

| Method | Signature | Description |
|--|--|--|
| `load(ord?)` | `(Ordering?) -> T` | Atomically read the current value |
| `store(val, ord?)` | `(T, Ordering?) -> ()` | Atomic write |
| `add(val, ord?)` | `(T, Ordering?) -> ()` | Atomic add |
| `sub(val, ord?)` | `(T, Ordering?) -> ()` | Atomic subtract |
| `fetchAdd(val, ord?)` | `(T, Ordering?) -> T` | Atomic add, returning old value |
| `fetchSub(val, ord?)` | `(T, Ordering?) -> T` | Atomic subtract, returning old value |
| `swap(val, ord?)` | `(T, Ordering?) -> T` | Atomic swap, returns old value |
| `compareExchange(expected, desired, ord?)` | `(T, T, Ordering?) -> (T, bool)` | CAS, returns `(old_value, success)` |
| `toggle(ord?)` | `(Ordering?) -> bool` | Atomic negate (bool only), returns old value |
| `toString()` | `() -> string` | Returns string representation of current value |

The `ord?` parameter accepts an `Ordering` enum; defaults to `Ordering.SeqCst`. See §10.9.
<!-- /xr-spec:en -->

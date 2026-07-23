---
id: spec.14_built_in_type_methods
order: 015
---

<!-- xr-spec:cn -->
---

## 14. 内置类型方法 (Built-in Type Methods)

> 真值源：prelude / analyzer / runtime 中的内置类型注册与方法定义。
> MCP knowledge 只消费生成后的 analyzer metadata，不独立维护内置类型方法签名。

本节按主题汇总每种内置类型的方法、签名和行为。

### 14.1 `int` 方法

| 方法 | 签名 | 说明 |
|--|--|--|
| `abs()` | `() -> int` | 绝对值 |
| `toString()` | `() -> string` | 十进制字符串 |
| `toBigInt()` | `() -> BigInt` | 转 BigInt |
| `toFloat()` | `() -> float` | 转 float |
| `toHex()` | `() -> string` | 十六进制字符串 |
| `max(other)` / `min(other)` | `(int) -> int` | 双值最值 |
| `sqrt()` | `() -> float` | 平方根 |
| `pow(exp)` | `(float) -> float` | 幂运算 |
| `checkedAdd(other)` / `checkedSub(other)` / `checkedMul(other)` | `(int) -> int?` | 溢出返回 `null` |
| `saturatingAdd(other)` / `saturatingSub(other)` / `saturatingMul(other)` | `(int) -> int` | 溢出饱和到 `int` 边界 |
| `wrappingAdd(other)` / `wrappingSub(other)` / `wrappingMul(other)` | `(int) -> int` | 显式二补码环绕 |
| `addOverflows(other)` / `subOverflows(other)` / `mulOverflows(other)` | `(int) -> bool` | 仅报告有符号溢出（要结果用 `checked*`） |
| `popcount()` | `() -> int` | 二补码位表示中置位的个数 |
| `leadingZeros()` / `trailingZeros()` | `() -> int` | 前导/后缀零比特数（`0` 返回 `64`） |
| `byteswap()` | `() -> int` | 反转字节序 |
| `rotateLeft(n)` / `rotateRight(n)` | `(int) -> int` | 循环移位（`n` 按模 64） |

`abs()` 遵循整数环绕语义：`(-9223372036854775807 - 1).abs()` 返回自身。`toHex()` 对负数使用带符号前缀，例如 `-0x8000000000000000`。位运算与溢出谓词在 VM 与 AOT 中具有相同语义。

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

### 14.4.1 `rune` 方法

| 方法 | 签名 | 说明 |
|--|--|--|
| `toString()` | `() -> string` | 返回单 Unicode scalar 字符串 |
| `toUInt32()` | `() -> uint32` | 返回 Unicode scalar code point |
| `isLetter()` | `() -> bool` | 是否为 Unicode 字母 |
| `isNumber()` | `() -> bool` | 是否为 Unicode 数字 |
| `isAlphanumeric()` | `() -> bool` | 是否为字母或数字 |
| `isWhitespace()` | `() -> bool` | 是否为空白字符 |

`rune` 是独立原始类型，不继承整数方法；需要码点时显式使用 `toUInt32()`。

### 14.5 `string` 方法

| 成员 | 类型 / 说明 |
|--|--|
| `len(s)` | O(1) Unicode scalar 数量 |
| `bytes()` / `copyBytes()` | 借用的 `Slice<byte>` / 独立的 `Array<byte>` |
| `runes()` | `Iterator<rune>`；裸 `for (r in s)` 使用相同语义 |
| `string.fromRune(r)` | 从一个 Unicode scalar 构造字符串 |
| `string.fromUtf8(bytes)` | 复制并严格验证 `Slice<byte>`；非法 UTF-8 抛 `Utf8Error.InvalidUtf8` |
| `string.fromUtf8Lossy(bytes)` | 复制 `Slice<byte>`，非法序列替换为 U+FFFD |
| `string.join(parts, separator?)` | 拼接 `Array<string>` |
| `contains(s)` | 是否包含子串 |
| `indexOf(s, start?)` / `lastIndexOf(s)` | 返回 rune ordinal |
| `slice(start, end?)` | 按 rune ordinal 取得独立 string；范围必须合法 |
| `sliceBytes(start, end)` | 按 byte offset 切片；边界非法时抛 `StringSliceError.InvalidByteRange` |
| `split(sep, limit?)` | 分割为 `Array<string>` |
| `replace(from, to)` / `replaceAll(from, to)` | 替换 |
| `repeat(n)` | 重复 |
| `startsWith(s)` / `endsWith(s)` | 前缀/后缀判断 |
| `toString()` | 返回自身 |

string 不支持整数下标或 slice operator；显式使用 `s.runes().nth(i)`、`s.bytes()[i]` 或 `s.slice(start, end)`。字符串拼接使用 `+`；大小写、去空白、填充和反转等 Unicode 文本操作属于 `text` 模块。

### 14.6 `Array<byte>`

`Array<byte>` 是可直接使用的 `Array` 具体化，构造由 `Array<byte>(n)` / `Array<byte>(n, fill)` 等内置路径处理。它的 `toString()` 与所有 Array 一样返回容器格式；文本解码必须显式使用 `string.fromUtf8(bytes[:])` 或 `string.fromUtf8Lossy(bytes[:])`。当前没有单独的 `stdlib/types/bytes.xr` 声明；工具不要把它当成另一套与 Array 同构的独立 API。

### 14.7 `Array<T>` 方法

| 成员 | 类型/说明 |
|--|--|
| `len(arr)` | `int` 全局查询 |
| `capacity` / `arr[i]` / `arr[i] = v` | 容量属性与下标读写；也可使用 `get(i)` / `set(i, v)` |
| `push(x)` / `pop()` | 尾部增删 |
| `shift()` / `unshift(x)` | 头部增删 |
| `concat(...arrays)` | 拼接 |
| `indexOf(x)` / `contains(x)` | 查找 |
| `join(sep?)` | 拼接为字符串 |
| `reverse()` / `sort(cmp?)` | 原地重排 |
| `map(fn)` / `filter(fn)` / `reduce(fn, init)` | 函数式处理 |
| `forEach(fn)` / `find(fn)` / `findIndex(fn)` / `every(fn)` / `some(fn)` | 遍历与谓词 |
| `fill(v, start?, end?)` / `clear()` | 填充或清空 |
| `reserve(capacity)` / `resize(length, fill)` | 容量与长度管理 |
| `ptr()` / `mutPtr()` | 显式底层指针视图 |
| `toString()` | 容器字符串表示 |
| `iterator()` / `entriesIterator()` / `entries()` | 迭代协议 |

Array 没有 `slice()` / `splice()` / `flat()` / `copyWithin()` 方法。`arr[start:end]` 产生借用的 `Slice<T>`，必须有显式目标类型并遵守借用生命周期；需要独立数据时使用 `copy(arr[start:end])`。

### 14.8 `Map<K, V>` 方法

| 成员 | 类型/说明 |
|--|--|
| `len(m)` | `int` 全局查询 |
| `m[k]` / `m[k] = v` | 下标读写 |
| `get(k)` / `set(k, v)` | `get` 在缺失时返回 `null`；`set` 写入 |
| `containsKey(k)` / `containsValue(v)` / `delete(k)` / `clear()` | 查询与删除 |
| `keys()` / `values()` / `entries()` | 返回键、值、键值对 |
| `forEach(fn)` | 遍历 |
| `iterator()` / `entriesIterator()` | 迭代协议 |

**Map 字面量**：`#{"k1": v1, "k2": v2}` 或 `#{}`；使用 `:`，靠 `#` 前缀区别于 Record/Json 对象字面量。

`m[k]` 要求键存在；缺失键触发运行时错误 `E0431`。需要可选读取时使用 `m.get(k)`。

### 14.9 `Set<T>` 方法

| 成员 | 类型/说明 |
|--|--|
| `len(set)` | `int` 全局查询 |
| `add(x)` / `contains(x)` / `delete(x)` | 插入、查询、删除 |
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
| `recvOr(default)` | 接收 payload；没有值时返回给定默认值 |
| `trySend(v)` | 非阻塞发送，返回 `SendResult` |
| `tryRecv()` | 非阻塞接收，返回 `Recv<T>`；空时为 `Recv.Empty` |
| `sendTimeout(v, ms)` | 带超时发送，返回 `SendResult`；超时为 `SendResult.Timeout` |
| `recvTimeout(ms)` | 带超时接收，返回 `Recv<T>`；超时为 `Recv.Timeout` |
| `close()` | 关闭 channel |
| `capacity` / `isClosed` | 容量和关闭状态属性 |

`Recv.Value(v)` 中的 `v` 就是 channel payload，因此 `Channel<int?>` 可以区分真实的 `Recv.Value(null)` 和 `Recv.Closed`。

### 14.11 `Json`

`Json` 是动态结构化数据类型。普通字段访问使用 `j.field` / `j["field"]`；通用查询和编解码通过 `Json` 静态函数完成，避免与用户字段名冲突。

| 静态函数 | 说明 |
|--|--|
| `Json.keys(obj)` / `Json.values(obj)` / `Json.entries(obj)` | Object 字段枚举 |
| `Json.containsKey(obj, key)` | 字段存在性 |
| `Json.get(obj, key, default?)` | 字段读取，不存在返回 default 或 null |
| `len(obj)` | Object / Array / String variant 的元素数量；scalar 抛 TypeError |
| `Json.parse(s)` / `Json.tryParse(s)` / `Json.isValid(s)` | JSON 解析与校验 |
| `Json.encode(value)` | 显式 typed value → Json 边界转换 |
| `Json.stringify(value, indent?)` | 序列化 |

**字面量**：`{ name: "alice", age: 30 }` 默认是 sealed `Record`。显式写 `var j: Json = {...}` 时才是动态 Json object；typed value 进入 JSON 边界使用 `Json.encode(value)`。

### 14.12 `Range`

`a..b` 是半开区间 `[a, b)`，`a..=b` 是闭区间 `[a, b]`；两种范围都可用于表达式、`for-in` 和 `match` 范围模式。

| 成员 | 说明 |
|--|--|
| `start` / `end` | 起点与声明的终点 |
| `contains(x)` | 按半开或闭区间语义判断 `x` 是否在范围内 |
| `toArray()` | 按迭代顺序生成独立的 `Array<int>` |
| `toString()` | 返回 `a..b` 或 `a..=b` 形式的字符串 |
| `len(range)` | 返回范围中的元素数量 |

```xray @id=range-members
var pages = 1..=3
print(pages.start)          // 1
print(pages.end)            // 3
print(pages.contains(3))    // true
print(len(pages))           // 3
print(pages.toArray())      // [1, 2, 3]

var empty = 5..5
print(len(empty))           // 0
```

### 14.13 `DateTime`

通过 `import datetime` 获得工厂函数：`now`、`utc`、`create`、`createUTC`、`fromTimestamp`、`fromTimestampMs`、`parse`、`offset`。`DateTime` 不是 prelude 类型；需要类型名时使用 `import { DateTime } from datetime`。

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
| `findText(s)` / `findGroup(s, index)` | 首个匹配文本 / 捕获组文本 |
| `replace(s, replacement)` | 替换 |
| `split(s, limit?)` | 分割 |

### 14.15 `StringBuilder`

| 方法 | 说明 |
|--|--|
| `len(builder)` | 当前 rune 数量 |
| `append(s)` | 追加并返回自身 |
| `toString()` | 输出字符串 |
| `clear()` | 清空并返回自身 |

### 14.16 `PanicInfo`

内置 `PanicInfo` 类包含 `message`、`stack`、`cause`、`code`、`data` 字段，构造函数 `constructor(message: string = "", cause: PanicInfo? = null)`，以及 `toString()`。

### 14.17 `Task<T>` 与 enum 值

`Task<T>` 属性：`done`、`status`；方法：`cancel()`、`poll()`、`awaitResult()`、`awaitTimeout(ms)`。`poll()` 和显式等待方法返回 `TaskResult<T>`：`Success(T)`、`Failed(PanicInfo)`、`Cancelled`、`Timeout`、`Pending`。plain `await task` 成功时返回 `T`，失败或取消时走对应错误/panic 路径；unique mutable 结果由编译器自动执行单次 take，不使用 `await (move task)`。enum 值提供冷路径属性 `name`、`ordinal` 与方法 `toString()`。

### 14.18 线程与同步 handle

`Thread<T>` 是 prelude handle 类型，公开 `done` 属性以及 `join()`、`detach()` 方法。导入 `sys` 后，使用 `sys.Thread.spawn(body)` 或 `sys.Thread.spawn(ThreadOptions{...}, body)` 创建 OS 线程，并对返回的 handle 调用 `join()` 或 `detach()`。`CountdownLatch`、`EventCount`、`ResultGroup`、`Semaphore`、`WorkQueue` 等类型从 `sync` 模块导入；`Logger` 从 `log` 模块导入；连接与监听器类型从 `net` 模块导入。

### 14.19 `Atomic<T>` 方法

`Atomic<T>` 包装 `int`、`float` 或 `bool`，提供无锁原子操作。句柄以 `const` 命名；受审计原子方法提供同步内部修改。

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

This section summarizes the methods, signatures, and behavior of each built-in type by topic.

### 14.1 `int` Methods

| Method | Signature | Description |
|--|--|--|
| `abs()` | `() -> int` | absolute value |
| `toString()` | `() -> string` | decimal string |
| `toBigInt()` | `() -> BigInt` | convert to BigInt |
| `toFloat()` | `() -> float` | convert to float |
| `toHex()` | `() -> string` | hexadecimal string |
| `max(other)` / `min(other)` | `(int) -> int` | binary max/min |
| `sqrt()` | `() -> float` | square root |
| `pow(exp)` | `(float) -> float` | power |
| `checkedAdd(other)` / `checkedSub(other)` / `checkedMul(other)` | `(int) -> int?` | returns `null` on overflow |
| `saturatingAdd(other)` / `saturatingSub(other)` / `saturatingMul(other)` | `(int) -> int` | clamps overflow to the `int` boundary |
| `wrappingAdd(other)` / `wrappingSub(other)` / `wrappingMul(other)` | `(int) -> int` | explicit two's-complement wrap |
| `addOverflows(other)` / `subOverflows(other)` / `mulOverflows(other)` | `(int) -> bool` | reports signed overflow only (use `checked*` for the value) |
| `popcount()` | `() -> int` | number of set bits in the two's-complement representation |
| `leadingZeros()` / `trailingZeros()` | `() -> int` | leading/trailing zero bit count (`0` yields `64`) |
| `byteswap()` | `() -> int` | reverses the byte order |
| `rotateLeft(n)` / `rotateRight(n)` | `(int) -> int` | bit rotation (`n` taken modulo 64) |

`abs()` follows integer wrap semantics: `(-9223372036854775807 - 1).abs()` returns itself. `toHex()` keeps a sign prefix for negative values, for example `-0x8000000000000000`. Bit-manipulation methods and overflow predicates have the same semantics in VM and AOT builds.

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

### 14.4.1 `rune` Methods

| Method | Signature | Description |
|--|--|--|
| `toString()` | `() -> string` | return a one-Unicode-scalar string |
| `toUInt32()` | `() -> uint32` | return the Unicode scalar code point |
| `isLetter()` | `() -> bool` | whether the scalar is a Unicode letter |
| `isNumber()` | `() -> bool` | whether the scalar is a Unicode number |
| `isAlphanumeric()` | `() -> bool` | whether the scalar is a letter or number |
| `isWhitespace()` | `() -> bool` | whether the scalar is whitespace |

`rune` is an independent primitive type and does not inherit integer methods; use `toUInt32()` explicitly when the code point is needed.

### 14.5 `string` Methods

| Member | Type / Description |
|--|--|
| `len(s)` | O(1) Unicode scalar count |
| `bytes()` / `copyBytes()` | borrowed `Slice<byte>` / independent `Array<byte>` |
| `runes()` | `Iterator<rune>`; bare `for (r in s)` has the same semantics |
| `string.fromRune(r)` | constructs a string from one Unicode scalar |
| `string.fromUtf8(bytes)` | copies and strictly validates a `Slice<byte>`; invalid UTF-8 throws `Utf8Error.InvalidUtf8` |
| `string.fromUtf8Lossy(bytes)` | copies a `Slice<byte>`, replacing invalid sequences with U+FFFD |
| `string.join(parts, separator?)` | joins an `Array<string>` |
| `contains(s)` | substring containment test |
| `indexOf(s, start?)` / `lastIndexOf(s)` | return rune ordinals |
| `slice(start, end?)` | independent rune-ordinal slice; the range must be valid |
| `sliceBytes(start, end)` | slice by byte offset; invalid boundaries throw `StringSliceError.InvalidByteRange` |
| `split(sep, limit?)` | split into `Array<string>` |
| `replace(from, to)` / `replaceAll(from, to)` | replacement |
| `repeat(n)` | repeat |
| `startsWith(s)` / `endsWith(s)` | prefix/suffix check |
| `toString()` | return self |

Strings do not support integer indexing or the slice operator; use `s.runes().nth(i)`, `s.bytes()[i]`, or `s.slice(start, end)` explicitly. Concatenation uses `+`; Unicode text transforms such as case conversion, trimming, padding, and reversal belong to the `text` module.

### 14.6 `Array<byte>`

`Array<byte>` is a directly available specialization of `Array`; construction is handled via builtin paths such as `Array<byte>(n)` / `Array<byte>(n, fill)`. Its `toString()` uses the same container formatting as every Array; decode text explicitly with `string.fromUtf8(bytes[:])` or `string.fromUtf8Lossy(bytes[:])`. There is currently no separate `stdlib/types/bytes.xr` declaration; tooling should not treat it as a second, Array-isomorphic API surface.

### 14.7 `Array<T>` Methods

| Member | Type / Description |
|--|--|
| `len(arr)` | global `int` query |
| `capacity` / `arr[i]` / `arr[i] = v` | capacity field and indexed read/write; `get(i)` / `set(i, v)` are also available |
| `push(x)` / `pop()` | tail insert/remove |
| `shift()` / `unshift(x)` | head insert/remove |
| `concat(...arrays)` | concatenation |
| `indexOf(x)` / `contains(x)` | search |
| `join(sep?)` | concatenate into a string |
| `reverse()` / `sort(cmp?)` | in-place reorder |
| `map(fn)` / `filter(fn)` / `reduce(fn, init)` | functional helpers |
| `forEach(fn)` / `find(fn)` / `findIndex(fn)` / `every(fn)` / `some(fn)` | traversal and predicates |
| `fill(v, start?, end?)` / `clear()` | fill or clear |
| `reserve(capacity)` / `resize(length, fill)` | capacity and length management |
| `ptr()` / `mutPtr()` | explicit low-level pointer views |
| `toString()` | container representation |
| `iterator()` / `entriesIterator()` / `entries()` | iteration protocol |

Array has no `slice()` / `splice()` / `flat()` / `copyWithin()` methods. `arr[start:end]` produces a borrowed `Slice<T>` whose target type must be explicit and whose lifetime follows the borrow; use `copy(arr[start:end])` for independent data.

### 14.8 `Map<K, V>` Methods

| Member | Type / Description |
|--|--|
| `len(m)` | global `int` query |
| `m[k]` / `m[k] = v` | indexed read/write |
| `get(k)` / `set(k, v)` | `get` returns `null` when absent; `set` writes |
| `containsKey(k)` / `containsValue(v)` / `delete(k)` / `clear()` | query and remove |
| `keys()` / `values()` / `entries()` | keys, values, key/value pairs |
| `forEach(fn)` | traversal |
| `iterator()` / `entriesIterator()` | iteration protocol |

**Map literal**: `#{"k1": v1, "k2": v2}` or `#{}`; entries use `:`, distinguished from Record/Json object literals by the `#` prefix.

`m[k]` requires the key to exist; a missing key raises runtime error `E0431`. Use `m.get(k)` for optional lookup.

### 14.9 `Set<T>` Methods

| Member | Type / Description |
|--|--|
| `len(set)` | global `int` query |
| `add(x)` / `contains(x)` / `delete(x)` | insert, query, remove |
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
| `recvOr(default)` | receives a payload, or returns the supplied default when none is available |
| `trySend(v)` | non-blocking send, returns `SendResult` |
| `tryRecv()` | non-blocking receive, returns `Recv<T>`; empty is `Recv.Empty` |
| `sendTimeout(v, ms)` | timed send, returns `SendResult`; timeout is `SendResult.Timeout` |
| `recvTimeout(ms)` | timed receive, returns `Recv<T>`; timeout is `Recv.Timeout` |
| `close()` | close the channel |
| `capacity` / `isClosed` | capacity and closed-state fields |

`Recv.Value(v)` carries the channel payload, so `Channel<int?>` can distinguish a real `Recv.Value(null)` from `Recv.Closed`.

### 14.11 `Json`

`Json` is a dynamic structured-data type. Ordinary field access uses `j.field` / `j["field"]`; generic queries and serialization go through `Json` static functions to avoid colliding with user field names.

| Static function | Description |
|--|--|
| `Json.keys(obj)` / `Json.values(obj)` / `Json.entries(obj)` | enumerate object fields |
| `Json.containsKey(obj, key)` | field existence |
| `Json.get(obj, key, default?)` | field read; returns `default` or `null` if absent |
| `len(obj)` | element count for Object / Array / String variants; scalar values throw TypeError |
| `Json.parse(s)` / `Json.tryParse(s)` / `Json.isValid(s)` | JSON parsing and validation |
| `Json.encode(value)` | explicit typed value → Json boundary conversion |
| `Json.stringify(value, indent?)` | serialization |

**Literal**: `{ name: "alice", age: 30 }` defaults to sealed `Record`. It becomes a dynamic Json object only with an explicit `Json` annotation such as `var j: Json = {...}`; use `Json.encode(value)` when a typed value crosses a JSON boundary.

### 14.12 `Range`

`a..b` is the half-open interval `[a, b)`, while `a..=b` is the inclusive interval `[a, b]`. Both forms work in expressions, `for-in`, and range patterns in `match`.

| Member | Description |
|--|--|
| `start` / `end` | The start and the declared endpoint |
| `contains(x)` | Tests membership using the range's half-open or inclusive semantics |
| `toArray()` | Produces an independent `Array<int>` in iteration order |
| `toString()` | Returns an `a..b` or `a..=b` string |
| `len(range)` | Returns the number of elements in the range |

```xray @id=range-members-en
var pages = 1..=3
print(pages.start)          // 1
print(pages.end)            // 3
print(pages.contains(3))    // true
print(len(pages))           // 3
print(pages.toArray())      // [1, 2, 3]

var empty = 5..5
print(len(empty))           // 0
```

### 14.13 `DateTime`

The `datetime` module provides factory functions through `import datetime`: `now`, `utc`, `create`, `createUTC`, `fromTimestamp`, `fromTimestampMs`, `parse`, and `offset`. `DateTime` is not a prelude type; import it explicitly with `import { DateTime } from datetime` when the name is used as a type.

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
| `findText(s)` / `findGroup(s, index)` | first matched text / capture-group text |
| `replace(s, replacement)` | replacement |
| `split(s, limit?)` | split |

### 14.15 `StringBuilder`

| Method | Description |
|--|--|
| `len(builder)` | current rune count |
| `append(s)` | append and return self |
| `toString()` | output string |
| `clear()` | empty and return self |

### 14.16 `PanicInfo`

The built-in `PanicInfo` class has fields `message`, `stack`, `cause`, `code`, `data`, the constructor `constructor(message: string = "", cause: PanicInfo? = null)`, and `toString()`.

### 14.17 `Task<T>` and Enum Values

`Task<T>` properties: `done`, `status`; methods: `cancel()`, `poll()`, `awaitResult()`, `awaitTimeout(ms)`. `poll()` and explicit wait methods return `TaskResult<T>` as `Success(T)`, `Failed(PanicInfo)`, `Cancelled`, `Timeout`, or `Pending`. Plain `await task` returns `T` on success and uses the matching error/panic path for failure or cancellation; a unique mutable result is taken once automatically, with no `await (move task)` form. Enum values provide the cold-path `name`, `ordinal`, and `toString()` surface.

### 14.18 Thread and Synchronization Handles

`Thread<T>` is a prelude handle type with the `done` field and the `join()` / `detach()` methods. After importing `sys`, create an OS thread with `sys.Thread.spawn(body)` or `sys.Thread.spawn(ThreadOptions{...}, body)`, then call `join()` or `detach()` on the returned handle. Import `CountdownLatch`, `EventCount`, `ResultGroup`, `Semaphore`, and `WorkQueue` from `sync`; import `Logger` from `log`; and import connection and listener types from `net`.

### 14.19 `Atomic<T>` Methods

`Atomic<T>` wraps `int`, `float`, or `bool` with lock-free atomic operations. Name the handle with `const`; audited atomic methods provide synchronized interior mutation.

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

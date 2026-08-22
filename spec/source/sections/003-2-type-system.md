---
id: spec.2_type_system
order: 003
---

<!-- xr-spec:cn -->
---

## 2. 类型系统 (Type System)

> 真值源：`src/runtime/value/xtype.h`（XrType 定义）、`src/runtime/value/xtype.c`、`src/frontend/parser/xparse_type.c`（语法）、`src/frontend/analyzer/xtype_ref_resolve.c`（解析）、`stdlib/prelude/builtin_symbols.def`（内置类型表）。

### 2.1 概述

Xray 是静态类型语言；每个表达式在编译期有确定类型。类型系统的核心特性：

1. **类型推断**：变量声明几乎不用写类型；分析器从初始值/上下文推导。
2. **Nullable 分离**：`T` 永不为 `null`；`T?` 是 `T | null` 的语法糖。
3. **Union 类型**：`A | B | ...`（最多 6 个成员）。
4. **泛型单态化**：泛型定义在构建期按具体类型特化，同时保留名义类型身份。
5. **三个职责清晰的类型域**：`class` / `struct` / `interface` 按**名义**兼容（显式 `implements`，无隐式实现）；`structural object` 按**结构**兼容且普通类型位置字段集精确，泛型约束位置可表达最小字段集；`JSON.Value` 是 schema-less 递归数据交换值域。JSON 标量可零物化 widening，复合 typed value 必须经 `JSON.value` 显式编码。
6. **最小类型身份**：`typeOf` / `typeName` / `is` / `as`；默认没有运行时 `Reflect` 模块。

### 2.2 类型分类

| 类别 | 示例 |
|--|--|
| Primitive | `i64`、`f64`、`bool`、`string`、`rune`、`()`（Unit，无返回值） |
| 精确整数 | `i8`、`i16`、`i32`、`i64`、`u8`..`u64` |
| 精确浮点 | `f32`、`f64` |
| 容器 | `Array<T>`、`Map<K,V>`、`Set<T>`、`Channel<T>`；`Array<u8>` 是连续字节元素的 `Array` 特化 |
| 定长布局 | `[T; N]` |
| 借用视图 | `Slice<T>`（不拥有数据，受借用生命周期约束，见 §2.4.2） |
| Prelude 特殊类型/命名空间 | `JSON`（含 `JSON.Value` / `JSON.Object`）、`BigInt`、`Range`、`Regex`、`StringBuilder`、`Atomic<T>`、`Path`、`Thread<T>`、`NetConn`、`NetListener`、`Os*` 同步类型 |
| 模块导出类型 | `DateTime`、`Logger`、`Plan`、`Mutex<T>` 等；必须从定义它们的模块显式 import |
| 错误处理 prelude | `PanicInfo`（见 §8） |
| Nullable | `T?` |
| Union | `A \| B \| ...` |
| Tuple | `(T1, T2, ...)` |
| Function | `fn(T1, T2) -> R` |
| FFI / C ABI | `Ptr<T>`、`MutPtr<T>`、`CFn<fn(T) -> R>`、`usize`、`isize` |
| Class / Struct / Interface | 用户定义（nominal） |
| Enum | 用户定义（含 ADT enum，见 §5.6） |
| Type alias | `type Name = SomeType`、`type Name<T> = SomeType` |

<!-- xr-builtin-registry:begin -->

#### 2.2.1 内置符号登记表

下表由 `stdlib/prelude/builtin_symbols.def` 生成，是无需 import 即可命名的符号全集。编译器、LSP 与本表读同一份真值源；表外的大写名字必须来自 import 或用户声明。

**内置类型**

| 符号 | 构造 |
|--|--|
| `Array<T>` | prelude |
| `Atomic<T>` | prelude |
| `BigInt` | prelude |
| `CFn<T>` | 解析器内建 |
| `Channel<T>` | prelude |
| `CoroLocal<T>` | 解析器内建 |
| `EnumPayloadField<T>` | 解析器内建 |
| `EnumPayloads<T>` | 解析器内建 |
| `EnumVariant<T>` | 解析器内建 |
| `EnumVariants<T>` | 解析器内建 |
| `Iterator<T>` | prelude |
| `JSON.Decodable` | 解析器内建 |
| `JSON.Encodable` | 解析器内建 |
| `JSON.Object` | 解析器内建 |
| `JSON.Path` | 解析器内建 |
| `JSON.PathSegment` | 解析器内建 |
| `JSON.UnknownFields` | 解析器内建 |
| `JSON.Value` | 解析器内建 |
| `JSON.WithRest<T>` | 解析器内建 |
| `Map<K, V>` | prelude |
| `MutPtr<T>` | 解析器内建 |
| `NetConn` | prelude |
| `NetListener` | prelude |
| `OsBarrier` | prelude |
| `OsCondvar` | prelude |
| `OsMutex` | prelude |
| `OsOnce` | prelude |
| `OsRwLock` | prelude |
| `PanicInfo` | prelude |
| `Path` | prelude |
| `Ptr<T>` | 解析器内建 |
| `Range` | prelude |
| `Regex` | prelude |
| `Set<T>` | prelude |
| `Slice<T>` | prelude |
| `StringBuilder` | prelude |
| `Task<T>` | 解析器内建 |
| `Thread<T>` | prelude |

**内置 enum**

| 符号 | 变体 |
|--|--|
| `Ordering` | `Relaxed` \| `Acquire` \| `Release` \| `AcquireRelease` \| `SeqCst` |
| `Endian` | `Native` \| `LE` \| `BE` |
| `Utf8Error` | `InvalidUtf8` |
| `StringSliceError` | `InvalidByteRange` |
| `CompressionError` | `InvalidData` |
| `CryptoError` | `InvalidLength` |
| `Recv<T>` | `Value` \| `Empty` \| `Timeout` \| `Closed` |
| `SendResult` | `Sent` \| `Full` \| `Timeout` \| `Closed` |
| `TaskResult<T>` | `Success` \| `Failed` \| `Cancelled` \| `Timeout` \| `Pending` |
| `TaskStatus` | `Pending` \| `Running` \| `Success` \| `Failed` \| `Cancelled` |

**内置约束接口**

| 符号 |
|--|
| `Callable<T>` |
| `Closeable` |
| `Comparable` |
| `Equatable` |
| `Error` |
| `Hashable` |
| `Indexable<K, V>` |
| `Iterable<T>` |
| `Lengthable` |
| `Stringable` |

**故意不提供的名字**

| 符号 | 诊断提示（编译器原文） |
|--|--|
| `Self` | Xray has no 'Self' type; write the declaring type's own name, e.g. operator==(other: Token) -> bool |
| `Box` | 'Box' is not a built-in type; indirect recursive data through a class node or a container slot such as Array<T> |

<!-- xr-builtin-registry:end -->

### 2.3 基本类型

#### 2.3.1 整数类型

| 类型 | 范围 | 默认角色 |
|--|--|--|
| `i8` | `[-128, 127]` | — |
| `i16` | `[-32768, 32767]` | — |
| `i32` | `[-2³¹, 2³¹-1]` | — |
| `i64` | `[-2⁶³, 2⁶³-1]` | 默认整数类型 |
| `u8`..`u64` | 无符号对应 | — |

- 无唯一数值上下文的整数字面量默认 `i64`；有唯一整数上下文时直接取得该类型且必须落在其范围内（`var x: i8 = 200` 编译拒绝）。有唯一浮点上下文时也直接取得该浮点类型，但整数值必须能被它精确表示。
- 算术：二补码环绕语义（wrap on overflow），不区分 debug / release 构建。同类型整数运算保留该类型并按其宽度环绕（`u8 + u8 -> u8`）；同符号异宽整数使用唯一的较宽类型。不同符号、固定宽度与 `isize`/`usize`、整数与浮点之间不做隐式提升；移位结果取左操作数类型。
- 静态类型为 `u8`..`u64` 的值在 `print`、`string(x)`、模板字符串、字符串拼接和顺序比较中按无符号解释；例如静态 `u64` 的位型 `0xffff_ffff_ffff_ffff` 显示为 `18446744073709551615`，且大于 `0`。
- `i64` 的 `checkedAdd` / `checkedSub` / `checkedMul` 在溢出时返回 `null`；`saturating*` 饱和到 `i64` 边界；`wrapping*` 显式执行默认二补码环绕。
- 已定型表达式不能通过赋值隐式窄化、改变符号或改变目标相关宽度；这些转换必须显式写 `as`。显式整数转换按目标位宽取模并以目标类型的二补码位型解释。
- 动态擦除后的 `XrValue` 只保存整数 payload，不保存有符号性或位宽；跨过 `JSON.Value` / 动态容器等边界后，超过 `i64` 正范围的 `u64` 值在格式化和顺序比较中的行为不保证保留无符号语义。需要无符号语义时保持适当的 exact 无符号静态类型（`u8` / `u16` / `u32` / `u64`）。

#### 2.3.2 浮点类型

| 类型 | 标准 |
|--|--|
| `f32` | IEEE-754 单精度 |
| `f64` | IEEE-754 双精度；默认浮点类型 |

字面量默认 `f64`。

#### 2.3.3 `bool`

`true` / `false`，独立类型，与数值类型**不可隐式互转**（不能 `var x: i64 = true`，也不能 `var b: bool = 1`）。

**条件表达式规则**（`if` / `while` / `for` 条件 / 三元 `?:` / `match` 守卫）：

| 条件类型 | 是否允许 | 语义 |
|---|---|---|
| `bool` | 允许 | 直接布尔判断 |
| `T?` 且 `T != bool` | 允许 | 仅判断是否为 `null`（不检查内容是否“空”） |
| `bool?` | 编译错误 | 三态歧义；写 `flag == true` / `flag != null` / `flag ?? false` |
| `i64` / `f64` / `string` / `rune` / 集合 / 对象 | 编译错误 | 必须写显式比较，如 `n != 0`、`len(s) != 0` |

`&&` / `||` / `!` 的操作数必须是 `bool`；不要把 `T?` 直接放进 `&&` / `||`。

```xray @id=types-explicit-conditions
var ok = true
if (ok) { }

var user: User? = findUser()
if (user) {              // 存在性：仅检查 null
    print(user.name)     // 此分支 user 窄化为 User
}

var flag: bool? = maybeFlag()
if (flag == true) { }    // OK
if (flag != null) { }    // OK
// if (flag) { }         // 编译错误：裸 bool? 不能作条件

var s = ""
if (len(s) != 0) { }     // OK
// if (s) { }            // 编译错误
```

#### 2.3.4 `string`

不可变且始终合法的 UTF-8 字符串。`len(s)` 以 O(1) 返回 Unicode scalar 数量，`len(s.bytes())` 以 O(1) 返回 UTF-8 byte 数量。默认迭代元素是 `rune`；整数下标与 slice operator 均不适用于 string，显式访问见 §14.5。

底层使用引用计数（ARC）；运行期短串默认协程本地（无锁分配），字面量/符号、显式 `intern()` 与 map/set 键走全局驻留池，跨协程边界按需提升为共享。

#### 2.3.5 `rune`

`rune` 表示一个 Unicode scalar value（有效范围 `U+0000..U+10FFFF`，排除 surrogate 区间 `U+D800..U+DFFF`）。它是独立的原始类型，**不是**数值类型，也**不是** `u32` 的别名。

```xray
var a: rune = 'a'
var zh = '中'
var smile = '\u{1F600}'
print(typeName(a))        // "rune"
print(smile.toUInt32())   // 128512
```

- rune 字面量必须恰好包含一个 Unicode scalar；空字面量、多 scalar 字面量和 surrogate 字面量都是编译错误。
- `rune` 不参与算术、位运算或窄整数赋值：`'a' + 1`、`var n: u32 = 'a'` 都会在分析期拒绝。
- 显式转换：`i64(c)` 得到 scalar code point；`rune(n)` 从整数构造 rune 并验证 scalar 合法性；`string(c)` / `c.toString()` 得到单 scalar 字符串。
- 常用方法见 §14.4.1。

#### 2.3.6 Unit `()`（无返回值）

xray 用 **0-元组 `()`** 表示"无返回值"（Unit 类型）：

```xray
fn log(msg: string) -> () { print(msg) }   // 显式 Unit 返回
fn ping() { print("pong") }                  // 省略返回类型 = ()
var r: () = log("hi")                        // 允许；r 是 Unit 值
```

- 一个函数省略返回类型等同于 `-> ()`。
- `void` 不是类型名：写 `fn f() -> void` 会被拒绝（`E0804`）；无返回值使用 `-> ()` 或省略返回类型。

#### 2.3.7 FFI 标量与 C ABI 边界类型

xray 的 C FFI 使用一组显式边界类型，避免把普通 xray 对象隐式解释成 C 数据：

| 类型 | C ABI 含义 | 备注 |
|--|--|--|
| `usize` | `size_t` | 宽度由编译目标决定；不得按宿主机 `u64` 代用 |
| `isize` | `ptrdiff_t` / 平台有符号宽度 | 宽度由编译目标决定；不得按宿主机 `i64` 代用 |
| `Ptr<T>` | `const void *` 边界值 | 只读裸指针；`T` 用于 xray 端解引用/索引宽度 |
| `MutPtr<T>` | `void *` 边界值 | 可写裸指针；可传给需要 `Ptr<T>` 的位置 |
| `CFn<fn(A, B) -> R>` | C ABI 函数指针 | 用于把 xray 函数作为 C 回调传入 `extern "C"` 函数 |

裸指针值可以安全地保存、传递、比较和用 `offset(i)` 做按元素宽度缩放的指针偏移；真正读写外部内存必须写在 `unsafe { }` 内：

```xray
extern "C" {
    fn malloc(n: usize) -> MutPtr<u8>
    fn free(p: MutPtr<u8>)
}

var p = unsafe { malloc(4) }
unsafe {
    p[0] = 42
    print(p.deref())
    free(p)
}
```

`Ptr<T>` 只能读取，写入必须使用 `MutPtr<T>`；`unsafe` 不会绕过这个类型规则。裸指针访问不做空指针或边界检查，调用方必须保证地址、生命周期、对齐和别名规则正确。

`usize` / `isize` 在 FFI 调用、`mem.load/store<T>`、manifest 绑定的 C layout 和生成 C 中使用同一份目标 ABI 标量描述。VM、AOT 与布局 introspection 必须采用编译目标的宽度和对齐；交叉编译时不得读取构建宿主机的 `sizeof(size_t)` 作为语义。

`CFn<fn(...) -> ...>` 不是普通 xray 闭包类型。当前 VM/AOT 后端支持把模块级、非捕获、签名精确匹配的 xray 函数传给 C；捕获闭包、匿名函数和 extern 函数本身不能作为 `CFn` 回调实参。

### 2.4 复合类型

#### 2.4.1 `Array<T>`

有序可变数组。详见 §14.7。

```xray @id=types-array
var a: Array<i64> = [1, 2, 3]
var b = [1, 2, 3]                // 推断为 Array<i64>
var c: Array<string> = []         // 显式空数组
```

`Array<T>` 的 `T` 必须能在编译期确定。空 `[]` 在无类型标注时是编译错误：`Empty array '[]' requires a type annotation`。

`Array<rune>` 保留 `rune` 元素身份：读出时得到 `rune`，写入时只接受 `rune`。

#### 2.4.1.1 定长数组 `[T; N]`

`[T; N]` 是定长布局数组类型，表示 `N` 个 `T` 元素。`N` 是类型的一部分，必须能在分析期求值为正的编译期整数表达式；当前支持整数字面量、`const` 整数标识符、括号、一元 `-`/`~`，以及整数算术/位运算。当前后端编码上限为 65535 个元素。

定长数组可用于 struct inline 字段和局部变量，并支持 struct、嵌套定长数组和引用容器等元素类型，因此可以递归组合：

```xray
var bytes: [u8; 4] = [1, 2, 3, 4]
var zero: [u8; 64] = [0; 64]
var names: [string; 2] = ["a", "b"]
var blocks: [[u8; 2]; 2] = [[1, 2], [3, 4]]
```

有目标类型的数组字面量初始化 `[T; N]` 时必须 exact-length；重复初始化 `[value; N]` 的 `N` 同样是正的编译期整数表达式，并且必须和目标类型长度一致。无上下文的普通数组字面量仍推断为动态 `Array<T>`；无上下文的 `[value; N]` 推断为 `[T; N]`。

定长数组支持 `len(array)`、索引读取、索引写入、`ref`/`in` 参数传递，以及目标类型为 `Slice<T>` 时通过切片产生借用视图：

```xray
var data: [u8; 4] = [5, 6, 7, 8]
var view: Slice<u8> = data[1:4]
view[1] = 99
```

```xray
struct Packet {
    magic: [u8; 4]
    payload: [u8; 128]
}

var key: [u8; 4] = [1, 2, 3, 4]
key[1] = 9

fn first(packet: Packet) -> u8 {
    return packet.magic[0]
}
```

`[T; N]` 与 `Array<T>` 语义不同：

- `[T; N]`：定长、值语义、固定布局，适合 struct inline 字段、局部小缓冲、FFI/freestanding 数据。
- `Array<T>`：动态长度、可增长、堆上容器。
- `Slice<T>`：借用连续存储的视图，不拥有数据（见 §2.4.2）。

旧的 `[N]T` 语法不属于 Xray 语言。

#### 2.4.2 `Slice<T>`

> 真值源：`stdlib/prelude/builtin_symbols.def`（prelude 内置符号注册）、`src/frontend/analyzer/xanalyzer_visitor_stmt.c`（借用跟踪与失效检查）、`src/frontend/analyzer/xa_memory_effect_db.h`（失效判据）、`src/frontend/analyzer/xanalyzer_visitor_decl.c`（返回视图契约）。

`Slice<T>` 是**借用视图**：它描述另一个值所拥有的一段连续元素存储，自身不拥有数据、不参与引用计数、不可放入任何长生命周期存储。它是 prelude 类型（`GENERIC_1`），可以在任何类型注解里直接写出。

##### 构造

视图只能由以下三种来源产生，且**必须有显式目标类型**——没有目标类型时切片表达式是编译错误：

| 来源 | 结果 | 说明 |
|--|--|--|
| `array[start:end]` | `Slice<T>` | owner 是 `Array<T>` |
| `fixedArray[start:end]` | `Slice<T>` | owner 是 `[T; N]` |
| `str.bytes()` | `Slice<u8>` | owner 是 `string` 的 UTF-8 字节存储 |

```xray
var arr: Array<i64> = [10, 20, 30, 40]
var view: Slice<i64> = arr[1:3]      // OK：借用 arr
var all: Slice<i64> = arr[:]         // 全长视图，不是拷贝
var bad = arr[1:3]                   // E0365：切片结果需要显式目标类型
```

owner 必须是**具名的局部变量、参数或 receiver 上的字段路径**。不能借用临时值：

```xray
var view: Slice<u8> = makeBytes()[0:2]   // E0384：不能从临时 owner 创建视图
```

##### 能力

- `len(view)` 取长度；`view[i]` 读元素；`view[i] = v` 写元素，**直接写入 owner 的存储**。
- `view[a:b]` 可以再切片，结果仍借用同一个 owner。
- 视图**没有成员方法**，也没有 `.length`。
- `const Slice<T>` 与 `const` owner 派生出的视图是只读的，写入是编译错误。

```xray @id=types-slice
fn main() {
    var arr: Array<i64> = [10, 20, 30, 40]
    var view: Slice<i64> = arr[1:3]        // 借用视图，不是拷贝
    view[1] = 31
    print(arr[2])                          // 31 —— 写穿到 owner
    arr[1] = 21
    print(view[0])                         // 21 —— owner 的元素写入对视图立即可见
    var owned: Array<i64> = copy(arr[1:3]) // 独立的 Array<T>
    arr.push(50)                           // OK：此处没有存活的视图
    print(len(owned))
}

main()
```

##### 借用规则

设视图 `v` 借用 owner `o`。在 `v` 的**存活期**内：

1. **元素写允许**：`o[i] = x` 合法。元素写不改变 `o` 的存储地址，视图仍然有效。
2. **失效操作拒绝**（`E0382`）：任何可能使 `o` 的元素存储重新定位、缩短或整体失效的操作都被拒绝。判据不是方法名白名单，而是被调用函数的 memory effect：地址稳定性（`ADDRESS_STABLE` / `MAY_RELOCATE`）、长度收缩（`NEVER_SHORTENS` / `MAY_SHORTEN`）、视图失效（`NEVER_INVALIDATES` / `INVALIDATES_VIEWS`）。`o.push(x)`、重新给 `o` 赋值、`move o`、`freeze o`、`return o` 都属于此类。
3. **存活期按最后一次使用判定**（非词法生命周期）：借用在 `v` 的最后一次使用处结束，而不是在词法块末尾。因此紧随其后的 owner 变更是合法的。
4. **不得逃逸**（`E0383`）：视图不得离开 owner 的作用域。以下位置一律拒绝——函数返回值（除非满足下面的返回契约）、class/struct/structural object 字段、Array / Map / Set / tuple / JSON.Value / enum payload 元素、闭包捕获、generator `yield`、模块级绑定、泛型类/结构体的类型实参、`go` 或 channel 等跨执行边界的传递、通过 `as` 擦除类型。

```xray
fn ok() {
    var bytes: Array<u8> = [1, 2]
    var view: Slice<u8> = bytes[:]
    print(len(view))                 // view 的最后一次使用
    bytes.push(3)                    // OK：借用已结束（规则 3）
}

fn rejected() {
    var bytes: Array<u8> = [1, 2]
    var view: Slice<u8> = bytes[:]
    bytes.push(3)                    // E0382：view 仍存活
    print(len(view))
}
```

##### 跨函数：返回视图契约

函数可以返回 `Slice<T>`，**当且仅当**返回的视图有**唯一可推断的来源**：某一个参数、receiver，或静态存储。编译器为这样的函数记录一份返回视图契约（来源 + 参数下标），调用点据此把结果继续记在原 owner 的账上，借用规则在调用者一侧照常生效。

来源不唯一、或借自函数的局部值时，是编译错误 `E0384`：

```xray
fn tail(data: Slice<u8>, start: i64) -> Slice<u8> {
    return data[start:]              // OK：唯一来源是参数 data
}

fn bad(a: Slice<u8>, b: Slice<u8>, useA: bool) -> Slice<u8> {
    if (useA) {
        return a
    }
    return b                         // E0384：多来源
}

fn alsoBad() -> Slice<i64> {
    var local: Array<i64> = [1, 2]
    return local[:]                  // E0384：借自局部值
}
```

##### 逃生口：`copy`

需要让数据活过 owner，或需要把它放进长生命周期存储时，用 `copy` 把视图物化为独立的 owner：

```xray
var owned: Array<i64> = copy(arr[1:3])   // 独立的 Array<T>，与 arr 无关
```

`copy(slice)` 的结果类型是 `Array<T>`，不是 `Slice<T>`；它是唯一能把借用数据变成拥有数据的构造。

##### 与其他借用形式的关系

`ref` 参数与 `Ptr<T>` / `MutPtr<T>` 共用同一套借用跟踪与同一组错误码：`E0382`（owner 在借用存活期内失效）、`E0383`（借用逃逸）、`E0384`（借用来源不稳定或不唯一）。`unsafe` 块不放宽其中任何一条。

#### 2.4.3 `Map<K, V>`

哈希字典，**保持插入顺序**。详见 §14.8。

**Map 字面量**必须用 `#{ ... }` 前缀，分隔符用 `:`；`JSON.Object` 使用同一个 Map 字面量：

```xray @id=types-map
var m: Map<string, i64> = #{"a": 1, "b": 2}
var m2 = #{"a": 1, "b": 2}
var empty = #{}                                     // 空 Map

m["c"] = 3                                          // 添加/修改
var v = m["a"]                                      // 取值；键不存在时 panic E0431
var maybe = m.get("missing")                        // 安全查询；不存在返回 null
```

| 字面量形式 | 类型 | 用途 |
|---|---|---|
| `{ key: value }`（无前缀） | exact `structural object` | 见 §2.4.7 |
| `#{ "k": v }`（`#` 前缀 + `:`） | `Map<K, V>`（哈希字典） | 本节 |
| `#{}` | `Map<K, V>`（空） | 显式空 Map |
| `[]` | `Array<T>` | 数组 |
| `#[]` | `Set<T>` | 集合 |

`K` 必须满足 `Hashable`（详见 §9.2）：通常是 `i64`、`f64`、`string`、`bool`、`enum`、`BigInt`，或同时提供 `operator==` 与 `hash() -> i64` 的自定义类型（`operator==` 的参数类型写该类型自己的名字，Xray 没有 `Self` 类型）。泛型键类型必须显式写成 `K: Hashable`。

#### 2.4.4 `Set<T>`

去重集合。详见 §14.9。

```xray @id=types-set
var s: Set<i64> = #[1, 2, 3]
```

#### 2.4.5 `Channel<T>`

协程间通信通道。命名通道句柄使用稳定 `const` 绑定；其同步内部可变能力来自受审计 registry（见 §10.5）。

```xray @id=types-channel
const ch: Channel<i64> = Channel<i64>(10)
```

#### 2.4.6 `Array<u8>`

类型化字节缓冲。语义等价 `Array<u8>`，但底层是连续内存。

```xray
var buf = Array<u8>(1024)
var init = Array<u8>([72, 101, 108, 108, 111])
```

#### 2.4.7 静态结构对象、`JSON.Value` 与对象字面量

裸对象字面量永远形成字段集精确的静态结构对象，用于普通业务对象、options 和多字段返回值。运行期未知键由 `Map` 承担；JSON object 的标准拼写 `JSON.Object` 是 `Map<string, JSON.Value>` 的纯别名。`JSON` 是无需 import 的 prelude 命名空间，不是值类型。

**对象字面量** `{ field: value, ... }` 与 Map 字面量的关键区别：

```xray @id=types-json-object
// 结构对象字面量：标识符或静态字符串 key + 冒号 ':'
var user = { name: "Bob", age: 25 }       // exact 对象形状
typeName(user)                            // "object"
user.name              // 类型: string；direct ordinal
user["name"]           // 与 user.name 完全等价；同一 direct ordinal

// 字段简写：当字段名与变量名相同
var name = "Alice"
var age = 30
var user = { name, age }                  // 等价 { name: name, age: age }

// Map 字面量：`#{}` 前缀 + `:`
var m = #{"k1": 1, "k2": 2}           // 类型: Map<string, i64>
var data: JSON.Object = #{"name": "Alice", "age": 30}
data["traceId"] = "req-1"               // 动态键只属于 Map
```

**对照表**：

| 写法 | 类型 | 备注 |
|---|---|---|
| `{ name: "x", age: 1 }` | exact 匿名对象形状 | 标识符或字符串 key 后跟 `:` |
| `{ x: y }`（`x` 是字段名，`y` 是变量名） | exact 匿名对象形状 | 字段简写 `{ x }` 等价 `{ x: x }`，仅裸 key |
| `#{"a": 1}` | `Map<K, V>` | `#` 前缀消歧，分隔符用 `:` |
| `var j: JSON.Object = #{"a": 1}` | `Map<string, JSON.Value>` | schema-less JSON object；可遍历、增删动态键 |
| `Point{x: 1.0, y: 2.0}` | `Point`（struct） | 类型名 + `{...}` 字面量 |

**对象形状类型**：裸对象字面量和 `type T = {...}` 都是 exact 对象形状；访问或赋值未声明字段是编译错误。结构宽度只出现在泛型约束满足关系中：`T: { name: string }` 表示 T 至少包含该字段，调用点仍按实参的具体 exact shape 单态化。尾部 `...` 不属于类型语法。

```xray
type User = { name: string, age: i64 }

var u: User = { name: "Alice", age: 30 }
print(u.name)         // OK
// u.extra = "x"      // 编译错误：sealed type User has no field 'extra'

var u2 = { name: "Alice", age: 30 }      // exact 对象形状
// u2.extra = "x"     // 编译错误

type Named = { name: string }
fn showName<T: Named>(value: T) {
    print(value.name)                     // 约束只暴露已声明字段
    print(value["name"])                  // 与 .name 相同
    // print(value.age)                    // 编译错误：隐藏字段不可见
}
showName(u)                               // OK：u 的 concrete exact shape 更宽

var j: JSON.Object = #{"name": "Alice", "age": 30}
j["extra"] = "x"                        // OK：Map 动态键
var encoded: JSON.Value = JSON.value(u)  // 复合值显式进入 JSON 值域
```

**乘积类型的职责划分**：

| | 身份 | 字段集 | 用户可定义方法 | 用途 |
|---|---|---|---|---|
| 对象形状 | 结构化 | exact；泛型约束可要求最小字段集 | **否** | options、多字段返回值、普通业务数据 |
| `JSON.Object` | Map 别名 | 任意字符串键，运行期确定 | Map API | schema-less JSON object |
| `struct` | 名义 | 声明的，编译期检查 | 是 | 值语义、固定布局、FFI 聚合、数学类型 |
| `class` | 名义 | 声明的，编译期检查 | 是 | 引用语义、继承、封装 |

结构对象字段可承载任意满足普通类型规则的静态值，包括函数引用、Map 或 class 实例；用户不能在结构对象类型上声明方法。能存储不等于能序列化：`JSON.value` / `JSON.stringify` 在编译期要求 `JSON.Encodable`，函数字段等不支持类型会被拒绝。

#### 2.4.8 `BigInt`

任意精度整数。见 §14.8。

#### 2.4.9 `Range`

`Range` 表示整数区间，由 `a..b` 或 `a..=b` 产生：

- `a..b` 是半开区间 `[a, b)`，不包含终点 `b`。
- `a..=b` 是闭区间 `[a, b]`，包含终点 `b`。

```xray @id=types-range
var halfOpen = 1..4       // 1, 2, 3
var inclusive = 1..=4     // 1, 2, 3, 4

print(len(halfOpen))              // 3
print(inclusive.contains(4))      // true
print(inclusive.toArray())        // [1, 2, 3, 4]

for (i in 3..=5) {
    print(i)
}
```

范围可用于 `for-in`、`match` 范围模式以及集合查询。完整表达式语义见 §3.9，成员见 §14.12。

#### 2.4.10 `DateTime` / `Regex` / `StringBuilder`

`Regex` 与 `StringBuilder` 是 prelude 类型。`DateTime` 不是 prelude 名字，必须通过 `import { DateTime } from datetime`（或其它显式 import）进入当前作用域。成员索引见 §14。

### 2.5 可空类型

`T?` 是 `T | null` 的语法糖。

```xray @id=types-nullable
var x: i64? = null      // OK
var y: i64? = 42        // OK
var z: i64 = null       // 编译错误：null 不是 i64
```

`JSON.Value` 本身包含 `null`，因此 `JSON.Value?` 与 `JSON.Value | null` 是语义重复并在解析阶段报错。解析失败使用 typed error enum 通过 `throw`/`catch` 值返回通道传播；若失败必须作为普通数据保存或返回，则使用领域 ADT 或含显式状态字段的 structural object。不要引入全局 `Result<T,E>`。

**可空原始类型一等公民**：`i64?` / `f64?` / `bool?` 与其它 `T?` 一样是合法类型，泛型与容器会自然产生它们（如 `Map<string, bool>.get(k) -> bool?`、`fn find<T>(...) -> T?` 在 `T = bool` 时）。它们以 tagged 表示承载 `null`，因此 `null` 值在 `print` / `string()` / 字符串拼接中统一显示为 `"null"`（不是底层数值 `0`），VM 与 AOT 一致。

> `bool?` 是三态（`true` / `false` / `null`）。它合法，但**不能直接作条件**（裸 `if (b)` where `b: bool?` 是编译错误，见 §5 / 任务 128）；需显式写 `b == true` / `b != null` / `b ?? false`。

#### 解包

```xray
// 1. 空合并
var v = x ?? 0

// 2. 可选链（整链短路，结果可空）
var city = user?.address.city

// 3. 强制解包
var v: i64 = x!           // 若 x 为 null，运行时 panic NullError

// 4. 流敏感收窄（完整规则见 §2.13）
if (x != null) {
    print(x + 1)          // 此分支内 x 已收窄为 i64
}
if (x is i64) {
    print(x + 1)
}
```

### 2.6 Union 类型

```xray @id=types-union-basic
var v: i64 | string = 42
v = "hello"             // OK
```

约束：
- 最多 **6 个成员**（编译期检查；超限 → 错误）。
- 成员互不为彼此的子类型（否则会被规范化）。
- **成员必须在运行期可判别**：动态擦除后的值只保留 i64 / f64 两个族，因此 union 至多包含**一个整数族成员**和**一个浮点族成员**。`i16 | i32`、`f32 | f64` 是同一个运行期类型的两个静态名字，`is` / `match` 无法区分，赋值也说不出存的是哪一个 —— 声明即报 `E0390`。
- 处理 union 值需用 `match` 或 `is` 窄化：

```xray
var v: i64 | string = ...
match v {
    is i64    -> print("i64: ${v}"),
    is string -> print("str: ${v}"),
}
```

**成员选择**：union 值携带它被赋值时选定的成员。选择规则是确定的，与成员书写顺序无关：

- 源类型与某个成员**完全相同** → 选该成员。
- 否则选唯一一个可经 §2.10.1 隐式转换到达的成员。可判别性规则保证每个数值族至多一个成员，所以这个成员至多有一个。
- 数值字面量按自己的族选：整数字面量优先选整数族成员，union 没有整数族成员时选浮点族成员（与"整数字面量定型进唯一浮点上下文"一致）；浮点字面量选浮点族成员。字面量必须能被目标精确表示。

```xray
var a: i64 | f64 = 1        // 精确匹配 i64
var b: i64 | f64 = 1.0      // 精确匹配 f64
var c: i32 | string = 7       // 唯一整数族成员：i32
var d: f32 | string = 1.5     // 唯一浮点族成员：f32
```

`is T` 检查的是运行期值：对定宽数值类型，它问的是"该值能否被 `T` 精确表示"——擦除后的值不保存位宽，这是唯一可回答的形式。在一个合法 union 内，选定成员总能通过它自己的 `is`，其余成员一定失败，因此各分支互斥。

**特殊化**：
- `i64 | null` 规范化为 `i64?`。
- `T?` 出现在 union 时：`i64? | string` 实际等价 `i64 | string | null`，规范化为 `(i64 | string)?`。

### 2.7 元组类型

xray 的元组**是头等公民**——可以作为任意值出现、作为字段保存、嵌套。

```xray @id=types-tuple
// 字面量
var t = (1, 2, 3)                 // 类型推断为 (i64, i64, i64)
var h = (10, "hi", true)          // 异构元组
var single = (99,)                // 单元素元组：注意尾逗号

// 类型注解
var p: (i64, string) = (7, "ok")

// 字段访问：.N（N 是编译期常量整数下标）
var first = t.0                   // 1
var mid   = t.1                   // 2
var nest  = ((1, 2), (3, 4))
var a     = nest.0.0              // 1
var b     = nest.1.1              // 4

// 函数返回与解构
fn divmod(a: i64, b: i64) -> (i64, i64) { return (a / b, a % b) }
var (q, r) = divmod(17, 5)        // tuple destructure

// 泛型
fn pair<A, B>(a: A, b: B) -> (A, B) { return (a, b) }
var p2 = pair(1, "x")             // (i64, string)
```

**注意事项**：

- **单元素元组**必须用尾逗号 `(x,)`——不带逗号的 `(x)` 是分组括号（普通表达式）。
- 字段访问 `t.N` 中 N **必须是字面量整数**；用变量或字符串访问是编译错误 `XR_ERR_ANALYZE_TUPLE_FIELD_NAME` / `_RANGE`。
- 元组**不可变**：`t.0 = v` 是编译错误。修改必须重新构造。

#### 完整可运行示例

```xray
fn main() {
    var pair = (1, "hello")
    print(pair.0)   // => 1
    print(pair.1)   // => hello
    var (a, b) = pair    // 解构
    print(a)        // => 1
    print(b)        // => hello
}

main()
```

### 2.8 类型别名

```xray @id=types-alias
type Result = i64 | string
type Mapper = fn(i64) -> i64
type Point = { x: f64, y: f64 }
type Pair<T> = { first: T, second: T }
type Mapper2<T, U> = fn(T) -> U
```

别名是**纯语法**等价，不产生新类型，也不产生运行时元数据或 AOT 分支。泛型别名在使用处按类型实参做语法代入：

```xray
var p: Pair<i64> = { first: 1, second: 2 }  // 等价于 { first: i64, second: i64 }
var f: Mapper2<i64, string> = (n) -> string(n)
```

泛型别名形参只允许名字列表（`<T, U>`）；不带约束。需要约束时应放在使用该别名的泛型函数、class / struct / enum / interface 声明上。别名可前向引用，但循环别名（包括递归对象别名）是编译错误。

**函数类型语法**：函数类型以 `fn` 引导，写作 `fn(T1, T2) -> R`；参数可带 `ref` / `move` mode（如 `fn(ref i64) -> i64`）。返回 `unit` 时省略箭头段，写作 `fn(T)` / `fn()`——类型位置**不允许**显式写 `-> ()`。这与函数声明位有意不对称：声明位 `fn f() -> ()` 与 `fn f()` 都合法（§5.2），而类型出现在参数、泛型实参、容器元素等密集内联位置，只保留一种拼写。类型位置的裸 `(` 只表示元组或分组括号，与表达式侧和 §2.7 一致——出现逗号才是元组，`(T)` 是分组；因此可空函数类型直接写作 `(fn() -> i64)?`。`CFn<...>` 内同样使用 `fn` 拼写（`CFn<fn(A, B) -> R>`，见 §3.12）。

### 2.9 类型推断

详见 §7.4。简述：

```xray @id=types-inference
var x = 1               // x: i64
var y = 1.5             // y: f64
var z = "hello"         // z: string
var a = [1, 2, 3]       // a: Array<i64>
var m = #{"a": 1}    // m: Map<string, i64>
var p = { name: "A" }   // p: { name: string } —— 结构化对象类型
var f = (x: i64) -> x   // f: fn(i64) -> i64 —— 箭头参数显式、返回类型推断
```

### 2.10 类型兼容性与转换

#### 2.10.1 隐式转换

| 源 | 目标 | 允许与条件 |
|--|--|--|
| `T` | `T` | ✅ identity |
| `i8 → i16 → i32 → i64` | 链上更宽的类型 | ✅ 无损加宽 |
| `u8 → u16 → u32 → u64` | 链上更宽的类型 | ✅ 无损加宽 |
| `f32` | `f64` | ✅ 无损加宽 |
| 整数字面量 | 唯一整数 / 浮点上下文 | ✅ 直接定型；目标整数可表示，或目标浮点精确可表示 |
| 已定型整数 | 不同符号、较窄或 `isize`/`usize` 与固定宽度之间 | ❌ 必须显式 `as` |
| 已定型整数 | 任意浮点类型 | ❌ 必须显式 `as` |
| 已定型浮点 | 任意整数类型或 `f64 → f32` | ❌ 必须显式 `as` |
| `T` | `T?` | ✅ |
| `i64` / `f64` / `string` / `bool` / `null` | `JSON.Value` | ✅ JSON 标量零物化 widening |
| 复合类型 | `JSON.Value` | ❌ 必须写 `JSON.value(value)`，并满足 `JSON.Encodable` |
| `Map<string, JSON.Value>` | `JSON.Object` | ✅ 纯别名 identity |
| `null` | `T?` | ✅ |
| Subtype | Supertype（class）| ✅ |
| 字段集不同的 exact 对象形状 | exact 对象形状 | ❌ exact target 要求精确字段集 |
| exact 对象形状 | 泛型约束 `T: { a: A }` | ✅ 约束满足关系；不是普通赋值转换 |

> **exact / width-constraint 规则**：普通类型位置要求**精确字段集**——源字段名集合必须与目标一致；目标中类型可空的字段允许缺省。泛型约束位置的结构类型表示“至少包含这些字段”，同名字段类型保持不变；它不产生运行时宽度对象或动态字典。
> ```xray
> type User = { name: string }
> var full = { name: "A", age: 18 }
> // var u: User = full            // 编译错误 E0352：extra field 'age'
> fn nameOf<T: User>(value: T) -> string { return value.name }
> print(nameOf(full))              // OK：T 保留 full 的 concrete exact shape
>
> type Opt = { name: string, age: i64? }
> var o: Opt = { name: "A" }       // OK：age 可空，允许缺省
> ```

> **静态对象与 JSON 动态边界**：已知 schema 使用 exact target 与 `JSON.parse<T>`；schema-less object 使用 `JSON.Object` / Map。typed 复合值通过 `JSON.value(value)` 显式编码，动态 object 通过 `JSON.decodeObject<T>(object)` 显式验证回 typed target。

#### 2.10.2 显式 `as`

```xray @id=types-cast
var n = x as i64        // 失败抛 TypeError
var n = x as i64?       // 失败返回 null（安全转换）
```

适用于：
- 数值之间（含 `JSON.Value → i64`，运行时检查）。
- `JSON.Value` 到标量类型的运行期检查；复合 typed 解码使用 `JSON.decode<T>` / `JSON.decodeObject<T>`。
- 父类 → 子类（向下转）。

数值 `as` 的结果与主机 C 编译器、优化级别和 VM/AOT 后端无关：整数到整数按目标位宽取模，并按目标有符号性解释同一位型；整数到浮点以及 `f64 → f32` 使用 IEEE-754 round-to-nearest, ties-to-even，溢出产生带原符号的无穷大，NaN 规范化为 Xray 的 canonical quiet NaN；浮点到整数向零截断，NaN、无穷大或超出目标范围时抛 `XR_ERR_OVERFLOW` (E0422)，消息为 `numeric conversion is out of range`。

`expr as T?` 仅用于可能失败的动态 / 结构化转换；它不是数值 checked-cast 语法。数值转换必须写 `expr as T`，并遵循上面的确定性规则。

#### 2.10.3 `is` 检查

```xray
if (v is User) {
    // 编译器在此分支窄化 v 的类型为 User
}
```

仅作类型守卫；不改变值。

### 2.11 typeOf / typeName / Type 枚举

```xray
typeOf(value)     // 返回 Type 枚举值（i64 表示）
typeName(value)   // 返回类型名字符串
```

`Type` 枚举成员：

`Type.i64`、`Type.f64`、`Type.string`、`Type.bool`、`Type.null`、
`Type.Array`、`Type.Map`、`Type.Set`、`Type.Channel`、
`Type.function`、`Type.class`、`Type.struct`、`Type.enum`、`Type.module`、`Type.bigint`、...

使用 `typeName(value)` 可以取得具体值的调试类型名。

### 2.12 元数据与类型身份边界

Xray 默认只保留最小类型身份层：

- `typeOf(x)` 返回稳定的 `Type` / `TypeId`，适合分支、`match` 和 analyzer narrowing。
- `typeName(x)` 返回调试/日志用的类型名字符串，是冷路径能力。
- 名义类型判断使用 `x is T` / `x as T`，不要通过字符串比较类型名。
- 字段/方法/构造器遍历不属于默认运行时能力；序列化、inspect、RPC schema 等结构化元数据由 `@derive(...)` 或编译期工具显式生成。

运行时类型查询使用 `typeOf(value)`、`typeName(value)` 和 `TypeId`。反射元数据不会暴露为可遍历、可调用的对象图。

### 2.13 流敏感类型收窄

> 真值源：`src/frontend/analyzer/xanalyzer_flow.c`（事实算子与控制流传播）、`src/frontend/analyzer/xanalyzer_visitor_expr.c`（引用点查询与 `E0379`）。

**收窄（narrowing）**指在特定控制流位置上把一个绑定的**静态类型**收紧为其声明类型的子类型。收窄结果参与成员查找、重载解析、`match` 穷尽性判定与**代码生成**，因此收窄是**语义**而非诊断优化：对同一程序，VM 与 AOT 两条后端必须得到相同的收窄结果。

本节规则编号 `N-1` … `N-13` 是规范性条文，每条在 `tests/compile_errors/narrowing/` 或 `tests/regression/16_narrowing/` 下有对应的一致性用例。

#### 2.13.1 可收窄主体

**N-1** 只有**简单绑定**可被收窄：局部 `var` / `const` 绑定，以及函数参数的裸标识符。

**N-2** 以下位置**不可收窄**，即使对其做了空检查或 `is` 检查：字段访问 `p.f`、`this.f`、索引 `a[i]`、元组分量 `t.0`、调用结果 `f()`，以及任何其它非标识符表达式。对这些位置的检查只决定该检查表达式自身的类型，不影响后续对同一写法的访问。

> 理由：只有简单绑定的全部写入点可在函数内被静态枚举。字段与元素可经别名、其它协程或方法调用改变；收窄它们需要引入位置（place）等价与别名失效分析，其代价与不确定性超过收益。

**N-3** 需要对不可收窄位置做流敏感处理时，先提取一个局部绑定：

```xray @id=narrowing-extract-binding
class Address { city: string
    constructor(city: string) { this.city = city } }
class User { address: Address?
    constructor(address: Address?) { this.address = address } }

fn show(u: User) {
    var addr = u.address          // 提取为简单绑定
    if (addr != null) {
        print(addr.city)          // OK：addr 已收窄为 Address
    }
}
```

#### 2.13.2 事实与算子

**N-4** 条件表达式在两个方向上产生**事实**：true 事实与 false 事实。下表是完整清单，未列出的形态两个方向都不产生事实。表中 `x` 表示可收窄主体（N-1），`e` 表示任意条件表达式。

| 条件形态 | true 分支 | false 分支 |
|--|--|--|
| `x` | 去掉 `null` | 无事实（`0` / `""` / `false` 同样为假）|
| `!e` | `e` 的 false 事实 | `e` 的 true 事实 |
| `(e)` | `e` 的 true 事实 | `e` 的 false 事实 |
| `x == null` / `null == x` | 只保留 `null` | 去掉 `null` |
| `x != null` / `null != x` | 去掉 `null` | 只保留 `null` |
| `x is T` | 与 `T` 相交 | 去掉与 `T` 相交的部分 |
| `typeOf(x) == Type.K` | 保留 kind 为 `K` 的成员 | 去掉 kind 为 `K` 的成员 |
| `typeOf(x) != Type.K` | 去掉 kind 为 `K` 的成员 | 保留 kind 为 `K` 的成员 |
| `e1 && e2` | `e1` 与 `e2` 的 true 事实依次施加 | 无事实 |
| `e1 \|\| e2` | 无事实 | `e1` 与 `e2` 的 false 事实依次施加 |

**N-5（短路继承）** `e1 && e2` 的 `e2` 在 `e1` 的 true 事实下分析；`e1 \|\| e2` 的 `e2` 在 `e1` 的 false 事实下分析。由于 `T?` 不能直接作条件（§2.5），这条规则使下面两种写法成为处理可空值的标准形式：

```xray @id=narrowing-short-circuit
fn check(a: string?) -> bool {
    if (a != null && len(a) > 0) { return true }     // e2 在 a: string 下分析
    if (a == null || len(a) == 0) { return false }   // e2 在 a: string 下分析
    return true
}
```

**N-6（相交语义）** `x is T` 的 true 方向：声明类型为 union 时保留与 `T` 相交的成员（无成员相交 → `never`）；非 union 时若可交则为 `T`，否则为 `never`。false 方向对称地移除相交部分。`x is T` 对基元类型、命名类型、泛型实例一视同仁。

#### 2.13.3 传播

**N-7** 事实在以下位置生效：`if` 的 then / else 体、条件表达式 `c ? a : b` 的两支、`while` / `for` 的循环体入口与循环之后（false 事实）、`&&` / `||` 的右操作数、`assert(c)` 之后的后继代码。

**N-8（早返回）** 当分支必然终止（`return` / `throw` / `break` / `continue`），其后的代码继承该条件的相反方向事实：

```xray @id=narrowing-early-return
fn nameLen(s: string?) -> i64 {
    if (s == null) { return 0 }
    return len(s)                 // 此处 s 已收窄为 string
}
```

**N-9（合流）** 合流点的类型是各前驱路径类型的并集；不可达前驱贡献 `never` 并被并集吸收。

**N-10（循环）** 循环头的类型是入口边与所有回边的并集。回边上若存在对该绑定的赋值，则该回边贡献被赋值表达式的类型；若一条回边回到循环头时**没有**经过任何赋值，则绑定的值未变，该回边不向并集贡献任何类型。因此循环体内对该绑定的赋值会使下一轮迭代的条件收窄按合流后的类型重新计算，而循环前建立的收窄在循环体不写该绑定时保持有效：

```xray @id=narrowing-loop
fn drain(first: string?) {
    var cur = first
    while (cur != null) {         // 循环体入口 cur: string
        print(cur)
        cur = null                // 回边贡献 null，循环头重新合流
    }
}
```

#### 2.13.4 失效

**N-11** 收窄在以下事件失效：

1. **赋值** / 复合赋值 / `++` / `--`：绑定的静态类型重置为被赋值表达式的静态类型；
2. 作为 `ref` 实参传出：重置为声明类型；
3. `move x` 之后：绑定不可用（见 §10）；
4. **在任意闭包体内被赋值**的绑定：在整个函数体内不可收窄（闭包何时运行不可知）。该规则与闭包在源码中的位置无关——写在收窄点之后同样生效。诊断会指出该原因，修法是改用不被写入的新绑定；
5. 普通函数调用**不**使收窄失效——N-1 / N-2 保证可收窄主体不可能被被调用方改写。

**N-11.1（函数体边界）** 每个函数体（具名函数、方法、闭包）拥有独立的流图：外层的收窄事实**不**进入闭包体，闭包体内的事实也不流出。闭包内需要收窄时在闭包内重新检查。

```xray @id=narrowing-invalidation
fn f(a: string?) {
    if (a != null) {
        print(len(a))             // OK
        a = null                  // 赋值使收窄失效
        print(a ?? "")            // 必须重新解包
    }
}
```

#### 2.13.5 收窄与 null 诊断

**N-12** 当接收者的静态类型仍可能为 `null`（含恒为 `null` 的类型）时，成员访问、索引、调用、算术、`len()` 与迭代都报 `E0379`（`XR_ERR_ANALYZE_POSSIBLY_NULL`），见 §18.2。三个例外：`==` / `!=`（与 `null` 比较正是收窄的入口）、`&&` / `||`（其操作数按条件检查），以及**字符串拼接**——`s + x` 中任一侧为 `string` 时，`null` 按 §2.5 渲染为 `"null"`。

**N-13** 解包途径共三条，均在 N-4 之外独立生效：

- `x!`：静态去掉 `null`；运行期若为 `null` 则 panic（`NullError`），**不是未定义行为**；
- `x ?? d`：结果类型为 `x` 去 `null` 后与 `d` 的并集；
- `x?.f`：可选链，**整链短路**——链上任意一段为 `null` 时整个后缀链求值为 `null`，结果类型为可空（见 §3.6）。
### 2.14 所有权、别名与借用

> 真值源：`src/frontend/analyzer/xa_ownership.h`（证据轴与判定结构）、`src/frontend/analyzer/xanalyzer_visitor_expr.c`（`move` 判定）、`src/frontend/analyzer/xanalyzer_visitor_stmt.c`（别名与借用跟踪）、`src/ir/xi_source_move_verify.c`（Xi 层独立复核）。

Xray 不要求写生命周期，也不提供借用检查器语法。但**所有权是有定义的**：`move`、`copy`、`ref`、`Slice<T>`、跨协程传递都读同一套判定，本节把它写出来。

#### 2.14.1 所有权根

**所有权根**是一个可独立回收的堆对象图的入口。`Array` / `Map`（含 `JSON.Object`）/ `Set` / `JSON.Value` 的复合 arm / `structural object` / class 实例 / 唯一结果 `Task<T>` 各有自己的根；标量、`string`、`Slice<T>`、裸指针、值 struct、定长数组**没有**根——它们要么按值复制，要么是借用视图。

只有拥有根的绑定才谈得上所有权转移。对没有根的值写 `move` 是编译错误（`E0391`：`move is not meaningful for value type`）。

#### 2.14.2 四条独立证据轴

每个绑定在每个程序点上带四条**互相独立**的证据，合法的所有权操作要求四条同时成立：

| 轴 | 回答的问题 | 取值 |
|--|--|--|
| **绑定状态** | 这个名字现在能用吗 | `UNINITIALIZED` / `LIVE` / `MOVED` / `MAYBE_MOVED` / `UNKNOWN` |
| **根别名** | 还有别的引用指向同一个根吗 | `UNIQUE` / `LOCAL_ALIASED` / `ESCAPED` / `ALIAS_UNKNOWN` |
| **能力** | 允许做什么 | `MUTABLE` / `CONST` / `SYNC_INTERIOR_MUTABLE` / `UNKNOWN` |
| **借用** | 有存活的借出吗 | 一组 loan：Slice 视图 / 裸指针借用 / 闭包捕获 |

分成四条是有意的：绑定状态是控制流事实，别名是对象图事实，能力是权限，借用是有起止的 place 事实。它们不能互相推导，也不能互相替代。

**默认 fail-closed**：任何一轴无法给出肯定证据时，答案是拒绝，不是放行。这也是为什么一个来源不明的调用结果不能被 `move`——编译器没有它的别名证据。

#### 2.14.3 别名如何产生与终止

| 动作 | 对根别名的影响 | 能否恢复 |
|--|--|--|
| `var b = a` | `LOCAL_ALIASED` | 能。`b` 最后一次使用后，根重新是 `UNIQUE` |
| `arr.push(a)` / `obj.f = a` / `m[k] = a` / `[a]` / `#{k: a}` / `Enum.V(a)` | `ESCAPED` | **不能**。函数内分析看不到那个槽何时被覆盖 |
| 来源不明的调用结果 | `ALIAS_UNKNOWN` | 不能 |
| `copy(a)` | 不影响 `a`；结果是新的 `UNIQUE` 根 | — |
| `move a` | `a` 变为 `MOVED`；根随之转移 | — |

**存活期按最后一次使用判定**（非词法生命周期），与 §2.4.2 的借用规则一致。因此下面第一段合法、第二段不合法：

```xray
fn ok() {
    var buf = [1, 2, 3]
    var alias = buf
    print(len(alias))          // alias 的最后一次使用
    consume(move buf)          // OK：别名已结束
}

fn rejected() {
    var buf = [1, 2, 3]
    var alias = buf
    consume(move buf)          // E0391：strong alias 'alias' remains live
    print(len(alias))
}
```

`ESCAPED` 是终态，这一点是刻意的：把引用写进堆图之后，谁还持有它已经不是这个函数能回答的问题。需要转移时用 `copy(a)`。

#### 2.14.4 借用如何产生与终止

三种借用形式共用同一份 loan 记录、同一套非词法存活期判定、同一组错误码（`E0382` / `E0383` / `E0384`）：

| 形式 | 借出者 | 存活期 |
|--|--|--|
| `Slice<T>` 视图 | 视图绑定 | 到视图绑定的最后一次使用 |
| `Ptr<T>` / `MutPtr<T>` | 指针绑定 | 到指针绑定的最后一次使用 |
| **闭包捕获** | 闭包绑定 | 到闭包绑定的最后一次使用 |

普通同步闭包**按引用捕获**外层绑定，因此捕获是一次借用，不是拷贝：

```xray
fn rejected() {
    var buf = [1, 2, 3]
    const peek = fn() -> i64 { return len(buf) }
    go consume(move buf)       // E0382：closure capture 'peek' is active
    print(peek())
}
```

只作为**调用实参**出现的闭包字面量通常不产生存活借用——它随调用结束，不可能活过调用：

```xray
fn ok() {
    var buf: Array<i64> = []
    items.forEach(fn(x: i64) { buf.push(x) })   // 调用边界内的捕获
    consume(move buf)                            // OK
}
```

例外是被调方**保留或逃逸**该形参时——此时闭包活过调用，它按引用捕获的根随之逃逸（`OWN-E-ESCAPED-ROOT`）。判据是被调方的形参效应摘要，不是语法形状。

存活的借用禁止对 owner 做失效操作，`move` 是其中一种（`E0382`）。

#### 2.14.5 `move` 的完整条件

`move x` 要求 `x` 是**可重绑定的局部 `var` 根**，且：

1. 绑定状态是 `LIVE`（不是已 moved、可能已 moved 或未知）；
2. 根别名是 `UNIQUE`；
3. 能力是 `MUTABLE`（`const` 与同步句柄不可 move）；
4. 没有存活的 loan；
5. 存储计划完整（编译器已为这个根解出分配域）；
6. 不在会重复执行的循环里消费循环外声明的绑定。

`move` 只接受**标识符**：`move x.field`、`move arr[i]`、`move f()` 都是语法错误。字段与元素没有独立的所有权根——它们的根是容器本身，转移其中一格会让容器处于部分转移状态，这个状态没有表示。需要取出一格时，先 `copy`，或让容器本身成为 move 源。

move 成功后源绑定在编译期标记为已 moved，再次引用是编译错误。**被拒绝的 move 不污染源状态**：诊断之后 `x` 仍然可用。

拒绝原因在诊断里具名，便于定位是哪一轴失败：

| 原因 | 含义 |
|--|--|
| `OWN-E-LIVE-ALIAS` | 存在存活的局部强别名 |
| `OWN-E-ESCAPED-ROOT` | 根已写入堆图 |
| `OWN-E-UNKNOWN-CALL` | 唯一性证据不完整（来源不明的调用结果） |
| `OWN-E-STORAGE-PLAN` | 存储/所有权计划不完整 |
| `OWN-E-LIVE-LOAN` | 存在存活借用（Slice 视图 / 裸指针 / 闭包捕获） |

#### 2.14.6 值拷贝与 managed 字段

值 struct 按值复制。为了让「按值复制」始终是完整语义，**struct 字段的类型是受限的**：只允许标量、`string`、裸指针、定长数组、以及其他值 struct。`Array` / `Map` / `Set` / `JSON.Value` / class 实例**不能**作为 struct 字段（`E0352`）。

因此不存在「struct 值拷贝携带可变 managed 字段」的情形，也就不需要在浅拷贝与深拷贝之间做选择。唯一的 managed 字段是 `string`，而 `string` 不可变：共享它不产生任何可观察差异，唯一性判定也不受影响。

需要在聚合里放可变图时用 class——class 是引用类型，赋值创建的是别名，按 §2.14.3 的规则处理。

### 2.15 完整可运行示例

以下为自包含、可运行并通过 `xray check` 验证的完整程序（注释标注真实输出）。

数组：

```xray
fn main() {
    var nums = [1, 2, 3]
    nums.push(4)
    print(nums)          // => [1, 2, 3, 4]
    print(len(nums))     // => 4
    var doubled = nums.map(fn(x: i64) -> i64 { return x * 2 })
    print(doubled)       // => [2, 4, 6, 8]
    var evens = nums.filter(fn(x: i64) -> bool { return x % 2 == 0 })
    print(evens)         // => [2, 4]
}

main()
```

Map 与 Set：

```xray
fn main() {
    var scores = #{"alice": 95, "bob": 88}
    scores.set("carol", 77)
    print(scores.get("alice") ?? 0)   // => 95
    print(len(scores))                 // => 3

    var seen = Set<i64>()
    seen.add(1)
    seen.add(2)
    seen.add(2)
    print(len(seen))          // => 2
    print(seen.contains(1))   // => true
}

main()
```

可空类型与 `??`：

```xray
fn main() {
    var name: string? = null
    print(name ?? "anonymous")   // => anonymous
    var city: string? = "NYC"
    print(city ?? "unknown")     // => NYC
}

main()
```

<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 2. Type System

> Source of truth: `src/runtime/value/xtype.h` (`XrType` definition), `src/runtime/value/xtype.c`, `src/frontend/parser/xparse_type.c` (syntax), `src/frontend/analyzer/xtype_ref_resolve.c` (resolution), `stdlib/prelude/builtin_symbols.def` (built-in type table).

### 2.1 Overview

Xray is statically typed; every expression has a determined type at compile time. Core features of the type system:

1. **Type inference**: variable declarations rarely require type annotations; the analyzer infers from the initializer / context.
2. **Nullable separation**: `T` is never `null`; `T?` is sugar for `T | null`.
3. **Union types**: `A | B | ...` (up to 6 members).
4. **Monomorphized generics**: generic definitions are specialized at build time while keeping nominal type identity.
5. **Three type domains with distinct responsibilities**: `class` / `struct` / `interface` are **nominal** (explicit `implements`, no implicit conformance); `structural object` is structural and exact in ordinary type positions, while generic constraint positions may require a minimum field set; `JSON.Value` is the schema-less recursive data-exchange domain. JSON scalars widen without materialization, while typed composites require explicit `JSON.value` encoding.
6. **Minimal type identity**: `typeOf`, `typeName`, `is`, and `as`; there is no default runtime `Reflect` module.

### 2.2 Type Categories

| Category | Examples |
|--|--|
| Primitive | `i64`, `f64`, `bool`, `string`, `rune`, `()` (Unit, no return value) |
| Sized integers | `i8`, `i16`, `i32`, `i64`, `u8`..`u64` |
| Sized floats | `f32`, `f64` |
| Containers | `Array<T>`, `Map<K,V>`, `Set<T>`, `Channel<T>`; `Array<u8>` is the contiguous-byte specialization of `Array` |
| Fixed layout | `[T; N]` |
| Borrowed view | `Slice<T>` (owns no data; constrained by borrow lifetimes, see §2.4.2) |
| Special prelude types/namespaces | `JSON` (including `JSON.Value` / `JSON.Object`), `BigInt`, `Range`, `Regex`, `StringBuilder`, `Atomic<T>`, `Path`, `Thread<T>`, `NetConn`, `NetListener`, and the `Os*` synchronization types |
| Module-exported types | `DateTime`, `Logger`, `Plan`, `Mutex<T>`, and others; these require explicit imports from their defining modules |
| Error-handling prelude | `PanicInfo` (see §8) |
| Nullable | `T?` |
| Union | `A \| B \| ...` |
| Tuple | `(T1, T2, ...)` |
| Function | `fn(T1, T2) -> R` |
| FFI / C ABI | `Ptr<T>`, `MutPtr<T>`, `CFn<fn(T) -> R>`, `usize`, `isize` |
| Class / Struct / Interface | user-defined (nominal) |
| Enum | user-defined (incl. ADT enum, see §5.6) |
| Type alias | `type Name = SomeType`, `type Name<T> = SomeType` |

<!-- xr-builtin-registry:begin -->

#### 2.2.1 Built-in symbol registry

Generated from `stdlib/prelude/builtin_symbols.def`, this is the complete set of names available without an import. The compiler, the LSP and this table read the same source of truth; any capitalized name outside it comes from an import or a user declaration.

**Built-in types**

| Symbol | Construction |
|--|--|
| `Array<T>` | prelude |
| `Atomic<T>` | prelude |
| `BigInt` | prelude |
| `CFn<T>` | resolver built-in |
| `Channel<T>` | prelude |
| `CoroLocal<T>` | resolver built-in |
| `EnumPayloadField<T>` | resolver built-in |
| `EnumPayloads<T>` | resolver built-in |
| `EnumVariant<T>` | resolver built-in |
| `EnumVariants<T>` | resolver built-in |
| `Iterator<T>` | prelude |
| `JSON.Decodable` | resolver built-in |
| `JSON.Encodable` | resolver built-in |
| `JSON.Object` | resolver built-in |
| `JSON.Path` | resolver built-in |
| `JSON.PathSegment` | resolver built-in |
| `JSON.UnknownFields` | resolver built-in |
| `JSON.Value` | resolver built-in |
| `JSON.WithRest<T>` | resolver built-in |
| `Map<K, V>` | prelude |
| `MutPtr<T>` | resolver built-in |
| `NetConn` | prelude |
| `NetListener` | prelude |
| `OsBarrier` | prelude |
| `OsCondvar` | prelude |
| `OsMutex` | prelude |
| `OsOnce` | prelude |
| `OsRwLock` | prelude |
| `PanicInfo` | prelude |
| `Path` | prelude |
| `Ptr<T>` | resolver built-in |
| `Range` | prelude |
| `Regex` | prelude |
| `Set<T>` | prelude |
| `Slice<T>` | prelude |
| `StringBuilder` | prelude |
| `Task<T>` | resolver built-in |
| `Thread<T>` | prelude |

**Built-in enums**

| Symbol | Variants |
|--|--|
| `Ordering` | `Relaxed` \| `Acquire` \| `Release` \| `AcquireRelease` \| `SeqCst` |
| `Endian` | `Native` \| `LE` \| `BE` |
| `Utf8Error` | `InvalidUtf8` |
| `StringSliceError` | `InvalidByteRange` |
| `CompressionError` | `InvalidData` |
| `CryptoError` | `InvalidLength` |
| `Recv<T>` | `Value` \| `Empty` \| `Timeout` \| `Closed` |
| `SendResult` | `Sent` \| `Full` \| `Timeout` \| `Closed` |
| `TaskResult<T>` | `Success` \| `Failed` \| `Cancelled` \| `Timeout` \| `Pending` |
| `TaskStatus` | `Pending` \| `Running` \| `Success` \| `Failed` \| `Cancelled` |

**Built-in constraint interfaces**

| Symbol |
|--|
| `Callable<T>` |
| `Closeable` |
| `Comparable` |
| `Equatable` |
| `Error` |
| `Hashable` |
| `Indexable<K, V>` |
| `Iterable<T>` |
| `Lengthable` |
| `Stringable` |

**Deliberately absent names**

| Symbol | Diagnostic hint |
|--|--|
| `Self` | Xray has no 'Self' type; write the declaring type's own name, e.g. operator==(other: Token) -> bool |
| `Box` | 'Box' is not a built-in type; indirect recursive data through a class node or a container slot such as Array<T> |

<!-- xr-builtin-registry:end -->

### 2.3 Primitive Types

#### 2.3.1 Integer Types

| Type | Range | Default role |
|--|--|--|
| `i8` | `[-128, 127]` | — |
| `i16` | `[-32768, 32767]` | — |
| `i32` | `[-2³¹, 2³¹-1]` | — |
| `i64` | `[-2⁶³, 2⁶³-1]` | default integer type |
| `u8`..`u64` | unsigned counterparts | — |

- An integer literal without a unique numeric context defaults to `i64`; in a unique integer context it directly acquires that type and must fit its range (`var x: i8 = 200` is rejected at compile time). In a unique floating context it directly acquires that floating type, but its integer value must be exactly representable.
- Arithmetic uses two's-complement wrap-around semantics (no debug/release distinction). Operations on the same integer type keep that type and wrap at its width (`u8 + u8 -> u8`); different widths with the same signedness use the unique wider type. There is no implicit promotion across signedness, between fixed-width integers and `isize`/`usize`, or between integers and floats; shift results keep the left operand's type.
- Values with static type `u8`..`u64` are interpreted as unsigned by `print`, `string(x)`, template strings, string concatenation, and ordering comparisons; for example, a static `u64` bit pattern of `0xffff_ffff_ffff_ffff` formats as `18446744073709551615` and compares greater than `0`.
- `i64.checkedAdd` / `checkedSub` / `checkedMul` return `null` on overflow; `saturating*` clamps to the `i64` boundary; `wrapping*` explicitly performs the default two's-complement wrap.
- An already-typed expression cannot be implicitly narrowed, change signedness, or cross a target-dependent width at assignment. Such conversions require an explicit `as`. Explicit integer conversion reduces modulo the target width and interprets the resulting two's-complement bit pattern as the target type.
- After dynamic erasure, `XrValue` stores only the integer payload, not signedness or width. Across `JSON.Value` / dynamic-container boundaries, `u64` values above the positive `i64` range are not guaranteed to keep unsigned formatting or ordering semantics. Keep the value in the appropriate exact unsigned static type (`u8`, `u16`, `u32`, or `u64`) when unsigned semantics are required.

#### 2.3.2 Floating-Point Types

| Type | Standard |
|--|--|
| `f32` | IEEE-754 single precision |
| `f64` | IEEE-754 double precision; default floating type |

Literals default to `f64`.

#### 2.3.3 `bool`

`true` / `false`, a standalone type. **No implicit conversion** to/from numeric types (cannot write `var x: i64 = true` or `var b: bool = 1`).

**Condition expression rules** (`if` / `while` / `for` conditions / ternary `?:` / `match` guards):

| Condition type | Allowed | Meaning |
|---|---|---|
| `bool` | yes | direct boolean test |
| `T?` with `T != bool` | yes | null presence only (content emptiness is **not** checked) |
| `bool?` | compile error | tri-state ambiguity; write `flag == true` / `flag != null` / `flag ?? false` |
| `i64` / `f64` / `string` / `rune` / collections / objects | compile error | use explicit comparisons such as `n != 0`, `len(s) != 0` |

Operands of `&&` / `||` / `!` must be `bool`; do not place `T?` directly into `&&` / `||`.

```xray @id=types-explicit-conditions
var ok = true
if (ok) { }

var user: User? = findUser()
if (user) {              // presence: null check only
    print(user.name)     // user is narrowed to User here
}

var flag: bool? = maybeFlag()
if (flag == true) { }    // OK
if (flag != null) { }    // OK
// if (flag) { }         // compile error: bare bool? cannot be a condition

var s = ""
if (len(s) != 0) { }     // OK
// if (s) { }            // compile error
```

#### 2.3.4 `string`

Immutable strings that always contain valid UTF-8. `len(s)` returns the Unicode scalar count in O(1), and `len(s.bytes())` returns the UTF-8 byte count in O(1). Default iteration yields `rune`; integer indexing and the slice operator do not apply to strings. See §14.5 for explicit access.

Internally uses ARC; runtime short strings are coroutine-local by default (lock-free allocation), while literals/symbols, explicit `intern()`, and map/set keys use the global intern pool. Cross-execution strings consume a verified storage plan; the boundary does not promote or copy them implicitly.

#### 2.3.5 `rune`

`rune` represents one Unicode scalar value (valid range `U+0000..U+10FFFF`, excluding the surrogate range `U+D800..U+DFFF`). It is an independent primitive type, **not** a numeric type and **not** an alias of `u32`.

```xray
var a: rune = 'a'
var zh = '中'
var smile = '\u{1F600}'
print(typeName(a))        // "rune"
print(smile.toUInt32())   // 128512
```

- A rune literal must contain exactly one Unicode scalar; empty literals, multi-scalar literals, and surrogate literals are compile errors.
- `rune` does not participate in arithmetic, bitwise operations, or narrow-integer assignment: `'a' + 1` and `var n: u32 = 'a'` are rejected by the analyzer.
- Explicit conversions: `i64(c)` returns the scalar code point; `rune(n)` constructs a rune from an integer and validates that it is a legal scalar; `string(c)` / `c.toString()` returns a one-scalar string.
- Common methods are listed in §14.4.1.

#### 2.3.6 Unit `()` (no return value)

Xray uses the **0-tuple `()`** to represent "no return value" (the Unit type):

```xray
fn log(msg: string) -> () { print(msg) }   // explicit Unit return
fn ping() { print("pong") }                  // omitted return type = ()
var r: () = log("hi")                        // allowed; r is a Unit value
```

- A function omitting its return type is equivalent to `-> ()`.
- `void` is not a type name: `fn f() -> void` is rejected (`E0804`); use `-> ()` or omit the return type to indicate no return value.

#### 2.3.7 FFI Scalars and C ABI Boundary Types

Xray's C FFI uses explicit boundary types so ordinary xray objects are not implicitly interpreted as C data:

| Type | C ABI meaning | Notes |
|--|--|--|
| `usize` | `size_t` | width comes from the compilation target; it must not be substituted with the host's `u64` |
| `isize` | `ptrdiff_t` / platform signed width | width comes from the compilation target; it must not be substituted with the host's `i64` |
| `Ptr<T>` | `const void *` boundary value | read-only raw pointer; `T` gives the xray-side dereference/index width |
| `MutPtr<T>` | `void *` boundary value | mutable raw pointer; assignable where `Ptr<T>` is expected |
| `CFn<fn(A, B) -> R>` | C ABI function pointer | passes an xray function as a C callback argument to an `extern "C"` function |

Raw pointer values may be stored, passed, compared, and offset with `offset(i)` using element-width scaling in safe code; actually reading or writing foreign memory must be inside `unsafe { }`:

```xray
extern "C" {
    fn malloc(n: usize) -> MutPtr<u8>
    fn free(p: MutPtr<u8>)
}

var p = unsafe { malloc(4) }
unsafe {
    p[0] = 42
    print(p.deref())
    free(p)
}
```

`Ptr<T>` is read-only; writes require `MutPtr<T>`. `unsafe` does not bypass that type rule. Raw pointer access performs no null or bounds checks, so the caller must guarantee address validity, lifetime, alignment, and aliasing correctness.

`usize` / `isize` use one target-ABI scalar descriptor across FFI calls, `mem.load/store<T>`, extern-layout fields, and generated C. The VM, AOT backend, and layout introspection must use the compilation target's width and alignment; cross-compilation never derives language semantics from the build host's `sizeof(size_t)`.

`CFn<fn(...) -> ...>` is not an ordinary xray closure type. The current VM/AOT backends support passing module-level, noncapturing xray functions with an exact signature match to C; capturing closures, anonymous functions, and extern functions themselves cannot be used as `CFn` callback arguments.

### 2.4 Composite Types

#### 2.4.1 `Array<T>`

Ordered mutable array. See §14.7.

```xray @id=types-array
var a: Array<i64> = [1, 2, 3]
var b = [1, 2, 3]                // inferred as Array<i64>
var c: Array<string> = []         // explicit empty array
```

The `T` in `Array<T>` must be determinable at compile time. An empty `[]` without a type annotation is a compile error: `Empty array '[]' requires a type annotation`.

`Array<rune>` preserves the `rune` element identity: reads return `rune`, and writes accept only `rune`.

#### 2.4.1.1 Fixed Arrays `[T; N]`

`[T; N]` is a fixed-layout array type for `N` elements of type `T`. `N` is part of the type and must evaluate during analysis to a positive compile-time integer expression. The current expression subset includes integer literals, `const` integer identifiers, grouping, unary `-`/`~`, and integer arithmetic/bitwise operators. The current backend encoding limit is 65535 elements.

Fixed arrays work as inline struct fields and local variables. They support struct, nested fixed-array, and reference-container element types, so fixed arrays compose recursively:

```xray
var bytes: [u8; 4] = [1, 2, 3, 4]
var zero: [u8; 64] = [0; 64]
var names: [string; 2] = ["a", "b"]
var blocks: [[u8; 2]; 2] = [[1, 2], [3, 4]]
```

A target-typed array literal that initializes `[T; N]` must have the exact length; repeat initialization `[value; N]` uses the same positive compile-time integer expression rule and must also match the target length. A normal array literal without context still infers dynamic `Array<T>`; `[value; N]` without context infers `[T; N]`.

Fixed arrays support `len(array)`, indexed reads, indexed writes, `ref`/`in` parameter passing, and target-typed slicing into `Slice<T>`:

```xray
var data: [u8; 4] = [5, 6, 7, 8]
var view: Slice<u8> = data[1:4]
view[1] = 99
```

```xray
struct Packet {
    magic: [u8; 4]
    payload: [u8; 128]
}

var key: [u8; 4] = [1, 2, 3, 4]
key[1] = 9

fn first(packet: Packet) -> u8 {
    return packet.magic[0]
}
```

`[T; N]` has different semantics from `Array<T>`:

- `[T; N]`: fixed length, value semantics, fixed layout; suited for inline struct fields, local small buffers, and FFI/freestanding data.
- `Array<T>`: dynamic length, growable, heap-backed container.
- `Slice<T>`: borrowed view over contiguous storage; it does not own data (see §2.4.2).

The old `[N]T` syntax is not part of the Xray language.

#### 2.4.2 `Slice<T>`

> Source of truth: `stdlib/prelude/builtin_symbols.def` (prelude built-in symbol registration), `src/frontend/analyzer/xanalyzer_visitor_stmt.c` (borrow tracking and invalidation checks), `src/frontend/analyzer/xa_memory_effect_db.h` (invalidation criteria), `src/frontend/analyzer/xanalyzer_visitor_decl.c` (returned-view contract).

`Slice<T>` is a **borrowed view**: it denotes a run of contiguous element storage owned by another value. It owns no data, does not participate in reference counting, and cannot be placed in any long-lived storage. It is a prelude type (`GENERIC_1`) and may be written directly in any type annotation.

##### Construction

A view can only arise from the three sources below, and it **requires an explicit target type** — a slice expression without one is a compile error:

| Source | Result | Notes |
|--|--|--|
| `array[start:end]` | `Slice<T>` | the owner is an `Array<T>` |
| `fixedArray[start:end]` | `Slice<T>` | the owner is a `[T; N]` |
| `str.bytes()` | `Slice<u8>` | the owner is the string's UTF-8 byte storage |

```xray
var arr: Array<i64> = [10, 20, 30, 40]
var view: Slice<i64> = arr[1:3]      // OK: borrows arr
var all: Slice<i64> = arr[:]         // full-length view, not a copy
var bad = arr[1:3]                   // E0365: a slice result needs an explicit target type
```

The owner must be a **named local, a parameter, or a field path rooted at one**. Temporaries cannot be borrowed:

```xray
var view: Slice<u8> = makeBytes()[0:2]   // E0384: cannot create a view from a temporary owner
```

##### Capabilities

- `len(view)` for the length; `view[i]` reads an element; `view[i] = v` writes one, **straight into the owner's storage**.
- `view[a:b]` reslices; the result still borrows the same owner.
- A view has **no member methods** and no `.length`.
- A `const Slice<T>`, and any view derived from a `const` owner, is read-only; writing through it is a compile error.

```xray @id=types-slice
fn main() {
    var arr: Array<i64> = [10, 20, 30, 40]
    var view: Slice<i64> = arr[1:3]        // borrowed view, not a copy
    view[1] = 31
    print(arr[2])                          // 31 — the write goes through to the owner
    arr[1] = 21
    print(view[0])                         // 21 — element writes on the owner are visible here
    var owned: Array<i64> = copy(arr[1:3]) // an independent Array<T>
    arr.push(50)                           // OK: no view is live at this point
    print(len(owned))
}

main()
```

##### Borrow rules

Let view `v` borrow owner `o`. While `v` is **live**:

1. **Element writes are allowed**: `o[i] = x` is legal. An element write does not move the owner's storage, so the view stays valid.
2. **Invalidating operations are rejected** (`E0382`): any operation that may relocate, shorten, or otherwise invalidate `o`'s element storage. The criterion is not a method-name allowlist but the callee's memory effect: address stability (`ADDRESS_STABLE` / `MAY_RELOCATE`), shortening (`NEVER_SHORTENS` / `MAY_SHORTEN`), and view invalidation (`NEVER_INVALIDATES` / `INVALIDATES_VIEWS`). `o.push(x)`, reassigning `o`, `move o`, `freeze o`, and `return o` all fall in this class.
3. **Liveness ends at the last use** (non-lexical lifetimes): the borrow ends at `v`'s last use, not at the end of the enclosing block, so an owner mutation placed after that point is legal.
4. **No escape** (`E0383`): a view must not outlive the owner's scope. All of the following are rejected — function return values (unless the return contract below is satisfied), class / struct / structural object fields, Array / Map / Set / tuple / JSON.Value / enum-payload elements, closure captures, generator `yield`, module-level bindings, type arguments of a generic class or struct, crossing an execution boundary via `go` or a channel, and erasing the type with `as`.

```xray
fn ok() {
    var bytes: Array<u8> = [1, 2]
    var view: Slice<u8> = bytes[:]
    print(len(view))                 // last use of view
    bytes.push(3)                    // OK: the borrow already ended (rule 3)
}

fn rejected() {
    var bytes: Array<u8> = [1, 2]
    var view: Slice<u8> = bytes[:]
    bytes.push(3)                    // E0382: view is still live
    print(len(view))
}
```

##### Across functions: the returned-view contract

A function may return `Slice<T>` **if and only if** the returned view has a **uniquely inferable source**: one specific parameter, the receiver, or static storage. The compiler records a returned-view contract (source kind plus parameter index) for such a function; the call site charges the result back to the original owner, so the borrow rules keep applying on the caller's side.

A non-unique source, or a view borrowed from one of the function's own locals, is a compile error (`E0384`):

```xray
fn tail(data: Slice<u8>, start: i64) -> Slice<u8> {
    return data[start:]              // OK: the unique source is the parameter data
}

fn bad(a: Slice<u8>, b: Slice<u8>, useA: bool) -> Slice<u8> {
    if (useA) {
        return a
    }
    return b                         // E0384: multiple sources
}

fn alsoBad() -> Slice<i64> {
    var local: Array<i64> = [1, 2]
    return local[:]                  // E0384: borrowed from a local
}
```

##### The escape hatch: `copy`

When the data must outlive the owner, or must go into long-lived storage, use `copy` to materialize the view into an independent owner:

```xray
var owned: Array<i64> = copy(arr[1:3])   // an independent Array<T>, unrelated to arr
```

`copy(slice)` has result type `Array<T>`, not `Slice<T>`; it is the only construct that turns borrowed data into owned data.

##### Relationship to other borrows

`ref` parameters and `Ptr<T>` / `MutPtr<T>` share the same borrow tracking and the same error codes: `E0382` (owner invalidated while a borrow is live), `E0383` (borrow escapes), `E0384` (borrow source unstable or not unique). An `unsafe` block relaxes none of them.

#### 2.4.3 `Map<K, V>`

Hash table that **preserves insertion order**. See §14.8.

**Map literals** must use the `#{ ... }` prefix with `:` separators; `JSON.Object` uses the same Map literal:

```xray @id=types-map
var m: Map<string, i64> = #{"a": 1, "b": 2}
var m2 = #{"a": 1, "b": 2}
var empty = #{}                                     // empty Map

m["c"] = 3                                          // insert / update
var v = m["a"]                                      // lookup; a missing key panics with E0431
var maybe = m.get("missing")                        // safe lookup; returns null if absent
```

| Literal form | Type | Purpose |
|---|---|---|
| `{ key: value }` (no prefix) | exact `structural object` | see §2.4.7 |
| `#{ "k": v }` (`#` prefix + `:`) | `Map<K, V>` (hash table) | this section |
| `#{}` | `Map<K, V>` (empty) | explicit empty Map |
| `[]` | `Array<T>` | array |
| `#[]` | `Set<T>` | set |

`K` must satisfy `Hashable` (see §9.2): typically `i64`, `f64`, `string`, `bool`, `enum`, `BigInt`, or a custom type that provides both `operator==` and `hash() -> i64` (the parameter type of `operator==` is spelled as the type's own name; Xray has no `Self` type). Generic key types must be explicitly constrained as `K: Hashable`.

#### 2.4.4 `Set<T>`

Deduplicated collection. See §14.9.

```xray @id=types-set
var s: Set<i64> = #[1, 2, 3]
```

#### 2.4.5 `Channel<T>`

Inter-coroutine communication channel. A named channel uses a stable `const` binding; its synchronized interior-mutation capability comes from the audited registry (see §10.5).

```xray @id=types-channel
const ch: Channel<i64> = Channel<i64>(10)
```

#### 2.4.6 `Array<u8>`

Typed byte buffer. Semantically equivalent to `Array<u8>`, but stored as contiguous memory.

```xray
var buf = Array<u8>(1024)
var init = Array<u8>([72, 101, 108, 108, 111])
```

#### 2.4.7 Static Structural Objects, `JSON.Value`, and Object Literals

A bare object literal always forms a static structural object with an exact field set. It is used for ordinary business objects, options, and multi-field returns. Runtime-unknown keys belong to `Map`; the canonical JSON-object spelling `JSON.Object` is a pure alias of `Map<string, JSON.Value>`. `JSON` is a prelude namespace that needs no import, not a value type.

The key difference between an **object literal** `{ field: value, ... }` and a Map literal:

```xray @id=types-json-object
// Structural-object literal: identifier or static string key + colon ':'
var user = { name: "Bob", age: 25 }       // exact object shape
typeName(user)                            // "object"
user.name              // type: string; direct ordinal
user["name"]           // exactly equivalent to user.name; same direct ordinal

// Field shorthand: when a field name matches a variable name
var name = "Alice"
var age = 30
var user = { name, age }                  // equivalent to { name: name, age: age }

// Map literal: `#{}` prefix + `:`
var m = #{"k1": 1, "k2": 2}           // type: Map<string, i64>
var data: JSON.Object = #{"name": "Alice", "age": 30}
data["traceId"] = "req-1"               // only Map has dynamic keys
```

**Comparison**:

| Form | Type | Notes |
|---|---|---|
| `{ name: "x", age: 1 }` | exact anonymous object shape | identifier or string key followed by `:` |
| `{ x: y }` (`x` is field name, `y` is variable) | exact anonymous object shape | shorthand `{ x }` equivalent to `{ x: x }`; bare key only |
| `#{"a": 1}` | `Map<K, V>` | `#` prefix disambiguates; separator `:` |
| `var j: JSON.Object = #{"a": 1}` | `Map<string, JSON.Value>` | schema-less JSON object; supports iteration and dynamic-key mutation |
| `Point{x: 1.0, y: 2.0}` | `Point` (struct) | type name + `{...}` literal |

**Object-shape types**: bare object literals and `type T = {...}` are exact object shapes, so accessing or assigning an undeclared field is a compile error. Structural width exists only in generic constraint satisfaction: `T: { name: string }` means that T contains at least that field, while each call is still monomorphized for the argument's concrete exact shape. A trailing `...` is not type syntax.

```xray
type User = { name: string, age: i64 }

var u: User = { name: "Alice", age: 30 }
print(u.name)         // OK
// u.extra = "x"      // compile error: sealed type User has no field 'extra'

var u2 = { name: "Alice", age: 30 }      // exact object shape
// u2.extra = "x"     // compile error

type Named = { name: string }
fn showName<T: Named>(value: T) {
    print(value.name)                     // the constraint exposes declared fields only
    print(value["name"])                  // identical to .name
    // print(value.age)                    // compile error: hidden field
}
showName(u)                               // OK: u has a wider concrete exact shape

var j: JSON.Object = #{"name": "Alice", "age": 30}
j["extra"] = "x"                        // OK: a Map dynamic key
var encoded: JSON.Value = JSON.value(u)  // composites enter JSON explicitly
```

**Responsibilities of product types**:

| | Identity | Field set | User-defined methods | Use for |
|---|---|---|---|---|
| object shape | structural | exact; a generic constraint may require a minimum field set | **no** | options, multi-field returns, ordinary business data |
| `JSON.Object` | Map alias | arbitrary string keys resolved at run time | Map API | schema-less JSON objects |
| `struct` | nominal | declared, checked at compile time | yes | value semantics, fixed layout, FFI aggregates, math types |
| `class` | nominal | declared, checked at compile time | yes | reference semantics, inheritance, encapsulation |

Structural-object fields may carry any statically typed value allowed by the ordinary type rules, including function references, Maps, and class instances; users cannot declare methods on a structural-object type. Storage does not imply serialization: `JSON.value` and `JSON.stringify` require `JSON.Encodable` at compile time, so unsupported types such as function fields are rejected.

#### 2.4.8 `BigInt`

Arbitrary-precision integer. See §14.8.

#### 2.4.9 `Range`

`Range` represents an integer interval and is produced by `a..b` or `a..=b`:

- `a..b` is the half-open interval `[a, b)` and excludes `b`.
- `a..=b` is the inclusive interval `[a, b]` and includes `b`.

```xray @id=types-range-en
var halfOpen = 1..4       // 1, 2, 3
var inclusive = 1..=4     // 1, 2, 3, 4

print(len(halfOpen))              // 3
print(inclusive.contains(4))      // true
print(inclusive.toArray())        // [1, 2, 3, 4]

for (i in 3..=5) {
    print(i)
}
```

Ranges work with `for-in`, range patterns in `match`, and collection queries. See §3.9 for expression semantics and §14.12 for members.

#### 2.4.10 `DateTime` / `Regex` / `StringBuilder`

`Regex` and `StringBuilder` are prelude types. `DateTime` is not a prelude name; bring it into scope with `import { DateTime } from datetime` (or another explicit import). See §14 for the member index.

### 2.5 Nullable Types

`T?` is sugar for `T | null`.

```xray @id=types-nullable
var x: i64? = null      // OK
var y: i64? = 42        // OK
var z: i64 = null       // compile error: null is not i64
```

`JSON.Value` intrinsically includes `null`, so `JSON.Value?` and `JSON.Value | null` are redundant and rejected during parsing. Parse failures use typed error enums propagated through the `throw`/`catch` value-return channel. When failure must be stored or returned as ordinary data, use a domain ADT or a structural object with an explicit status field. Do not introduce a global `Result<T,E>`.

**Nullable primitives are first-class**: `i64?` / `f64?` / `bool?` are ordinary `T?` types and arise naturally from generics and containers (e.g. `Map<string, bool>.get(k) -> bool?`, or `fn find<T>(...) -> T?` at `T = bool`). They carry `null` in the tagged representation, so a `null` value renders as `"null"` in `print` / `string()` / string concatenation (never as the raw payload `0`), identically in the VM and AOT.

> `bool?` is tri-state (`true` / `false` / `null`). It is legal but **cannot be used directly as a condition** (a bare `if (b)` where `b: bool?` is a compile error; see §5 / task 128); write `b == true` / `b != null` / `b ?? false`.

#### Unwrapping

```xray
// 1. Null coalescing
var v = x ?? 0

// 2. Optional chaining (whole-chain short-circuit, nullable result)
var city = user?.address.city

// 3. Force unwrap
var v: i64 = x!           // panics with NullError at runtime if x is null

// 4. Flow-sensitive narrowing (full rules in §2.13)
if (x != null) {
    print(x + 1)          // x is narrowed to i64 in this branch
}
if (x is i64) {
    print(x + 1)
}
```

### 2.6 Union Types

```xray @id=types-union-basic
var v: i64 | string = 42
v = "hello"             // OK
```

Constraints:
- Up to **6 members** (checked at compile time; over the limit → error).
- Members must not be subtypes of each other (otherwise normalized).
- **Members must be discriminable at run time**: a dynamically erased value keeps only its i64 or f64 family, so a union carries at most **one integer-family member** and at most **one f64-family member**. `i16 | i32` and `f32 | f64` are one runtime type wearing two static names — `is` and `match` cannot tell them apart, and an assignment could not say which one it stored. Declaring one is `E0390`.
- Working with a union value requires `match` or `is`-based narrowing:

```xray
var v: i64 | string = ...
match v {
    is i64    -> print("i64: ${v}"),
    is string -> print("str: ${v}"),
}
```

**Member selection**: a union value carries the member selected where it was assigned. The rule is deterministic and independent of the order the members were written:

- The source type is **exactly** one member → that member is selected.
- Otherwise the single member reachable by an implicit conversion from §2.10.1 is selected. Discriminability guarantees at most one member per numeric family, so at most one member qualifies.
- A numeric literal selects within its own family: an integer literal prefers the integer-family member, falling back to the f64-family member when the union has none (matching "an integer literal types into a unique floating context"); a f64 literal selects the f64-family member. The literal must be exactly representable in the selected member.

```xray
var a: i64 | f64 = 1        // exact match: i64
var b: i64 | f64 = 1.0      // exact match: f64
var c: i32 | string = 7       // sole integer-family member: i32
var d: f32 | string = 1.5     // sole f64-family member: f32
```

`is T` asks about the runtime value: for a fixed-width numeric type it asks whether the value is exactly representable in `T`, because an erased value does not carry its width and that is the only answerable form. Inside a well-formed union the selected member always passes its own `is` and every other member always fails, so the arms are mutually exclusive.

**Special cases**:
- `i64 | null` normalizes to `i64?`.
- When `T?` appears in a union: `i64? | string` is effectively `i64 | string | null`, normalized to `(i64 | string)?`.

### 2.7 Tuple Types

Xray's tuples are **first-class** — they may appear as any value, be stored as fields, and nest.

```xray @id=types-tuple
// Literals
var t = (1, 2, 3)                 // type inferred as (i64, i64, i64)
var h = (10, "hi", true)          // heterogeneous tuple
var single = (99,)                // single-element tuple: note trailing comma

// Type annotation
var p: (i64, string) = (7, "ok")

// Field access: .N (N is a compile-time constant integer index)
var first = t.0                   // 1
var mid   = t.1                   // 2
var nest  = ((1, 2), (3, 4))
var a     = nest.0.0              // 1
var b     = nest.1.1              // 4

// Function return and destructuring
fn divmod(a: i64, b: i64) -> (i64, i64) { return (a / b, a % b) }
var (q, r) = divmod(17, 5)        // tuple destructure

// Generic
fn pair<A, B>(a: A, b: B) -> (A, B) { return (a, b) }
var p2 = pair(1, "x")             // (i64, string)
```

**Notes**:

- A **single-element tuple** must use a trailing comma `(x,)` — `(x)` without a comma is a grouping parenthesis (a plain expression).
- In field access `t.N`, N **must be an integer literal**; using a variable or string is the compile error `XR_ERR_ANALYZE_TUPLE_FIELD_NAME` / `_RANGE`.
- Tuples are **immutable**: `t.0 = v` is a compile error. To modify, build a new tuple.

#### Worked Examples

```xray
fn main() {
    var pair = (1, "hello")
    print(pair.0)   // => 1
    print(pair.1)   // => hello
    var (a, b) = pair    // destructuring
    print(a)        // => 1
    print(b)        // => hello
}

main()
```

### 2.8 Type Aliases

```xray @id=types-alias
type Result = i64 | string
type Mapper = fn(i64) -> i64
type Point = { x: f64, y: f64 }
type Pair<T> = { first: T, second: T }
type Mapper2<T, U> = fn(T) -> U
```

Aliases are **purely syntactic** equivalences; they do not introduce new types,
runtime metadata, or AOT branches. A generic alias is substituted at its use
site:

```xray
var p: Pair<i64> = { first: 1, second: 2 }  // equivalent to { first: i64, second: i64 }
var f: Mapper2<i64, string> = (n) -> string(n)
```

Generic alias parameters are a name list only (`<T, U>`); constraints are not
part of type-alias syntax. Put constraints on the generic function, class /
struct / enum / interface that uses the alias. Aliases may be forward
referenced, but cyclic aliases, including recursive object aliases, are compile
errors.

**Function-type syntax.** A function type is led by `fn`, written
`fn(T1, T2) -> R`; parameters may carry a `ref` / `move` mode (e.g.
`fn(ref i64) -> i64`). When the return is `unit` the arrow segment is omitted,
written `fn(T)` / `fn()` — an explicit `-> ()` is **not** allowed in type
position. This is a deliberate asymmetry with the declaration position, where
both `fn f() -> ()` and `fn f()` are valid (§5.2); a type appears in dense
inline positions (parameters, generic arguments, container elements) and keeps
a single spelling. A bare `(` in type position is only a tuple or a grouping,
matching the expression side and §2.7 — a comma makes a tuple, `(T)` groups — so
a nullable function type is written directly as `(fn() -> i64)?`. `CFn<...>`
uses the same `fn` spelling (`CFn<fn(A, B) -> R>`, see §3.12).

### 2.9 Type Inference

See §7.4 for details. In summary:

```xray @id=types-inference
var x = 1               // x: i64
var y = 1.5             // y: f64
var z = "hello"         // z: string
var a = [1, 2, 3]       // a: Array<i64>
var m = #{"a": 1}    // m: Map<string, i64>
var p = { name: "A" }   // p: { name: string } — structured object type
var f = (x: i64) -> x   // f: fn(i64) -> i64 — explicit parameter, inferred return
```

### 2.10 Type Compatibility and Conversion

#### 2.10.1 Implicit Conversion

| From | To | Allowed and condition |
|--|--|--|
| `T` | `T` | ✅ identity |
| `i8 → i16 → i32 → i64` | a wider type on the chain | ✅ lossless widening |
| `u8 → u16 → u32 → u64` | a wider type on the chain | ✅ lossless widening |
| `f32` | `f64` | ✅ lossless widening |
| Integer literal | a unique integer / floating context | ✅ direct typing; the integer target represents it, or the floating target represents it exactly |
| Typed integer | another signedness, a narrower type, or fixed-width ↔ `isize`/`usize` | ❌ explicit `as` required |
| Typed integer | any floating type | ❌ explicit `as` required |
| Typed f64 | any integer type or `f64 → f32` | ❌ explicit `as` required |
| `T` | `T?` | ✅ |
| `i64` / `f64` / `string` / `bool` / `null` | `JSON.Value` | ✅ zero-materialization JSON scalar widening |
| composite type | `JSON.Value` | ❌ `JSON.value(value)` required, and the type must satisfy `JSON.Encodable` |
| `Map<string, JSON.Value>` | `JSON.Object` | ✅ pure-alias identity |
| `null` | `T?` | ✅ |
| Subtype | Supertype (class) | ✅ |
| exact object shape with a different field set | exact object shape | ❌ an exact target requires an exact field set |
| exact object shape | generic constraint `T: { a: A }` | ✅ constraint satisfaction; not an ordinary assignment conversion |

> **Exact / width-constraint rule**: an ordinary type position requires an **exact field set** — source field names must match the target; nullable target fields may be omitted. A structural type in generic constraint position means "contains at least these fields", with invariant types for common fields. It creates neither a runtime width object nor a dynamic dictionary.
> ```xray
> type User = { name: string }
> var full = { name: "A", age: 18 }
> // var u: User = full            // compile error E0352: extra field 'age'
> fn nameOf<T: User>(value: T) -> string { return value.name }
> print(nameOf(full))              // OK: T retains full's concrete exact shape
>
> type Opt = { name: string, age: i64? }
> var o: Opt = { name: "A" }       // OK: age is nullable and may be omitted
> ```

> **Static objects and the dynamic JSON boundary**: known schemas use exact targets and `JSON.parse<T>`; schema-less objects use `JSON.Object` / Map. Typed composites are encoded explicitly with `JSON.value(value)`, and a dynamically processed object is validated back to a typed target with `JSON.decodeObject<T>(object)`.

#### 2.10.2 Explicit `as`

```xray @id=types-cast
var n = x as i64        // throws TypeError on failure
var n = x as i64?       // returns null on failure (safe cast)
```

Applies to:
- Between numeric types (including `JSON.Value → i64`, checked at runtime).
- Runtime checks from `JSON.Value` to scalar types; composite typed decoding uses `JSON.decode<T>` / `JSON.decodeObject<T>`.
- Parent → child (downcast).

Numeric `as` is independent of the host C compiler, optimization level, and VM/AOT backend: integer-to-integer conversion reduces modulo the target width and interprets the same bit pattern with the target signedness; integer-to-f64 and `f64 → f32` use IEEE-754 round-to-nearest, ties-to-even, overflow produces signed infinity, and NaN is normalized to Xray's canonical quiet NaN; f64-to-integer truncates toward zero and throws `XR_ERR_OVERFLOW` (E0422), with message `numeric conversion is out of range`, for NaN, infinity, or a value outside the target range.

`expr as T?` is reserved for fallible dynamic / structural conversion; it is not a numeric checked-cast form. Numeric conversions use `expr as T` and follow the deterministic rules above.

#### 2.10.3 `is` Check

```xray
if (v is User) {
    // In this branch the compiler narrows v's type to User
}
```

Acts only as a type guard; does not change the value.

### 2.11 typeOf / typeName / Type Enum

```xray
typeOf(value)     // returns a Type enum value (an i64 representation)
typeName(value)   // returns the type name as a string
```

`Type` enum members:

`Type.i64`, `Type.f64`, `Type.string`, `Type.bool`, `Type.null`,
`Type.Array`, `Type.Map`, `Type.Set`, `Type.Channel`,
`Type.function`, `Type.class`, `Type.struct`, `Type.enum`, `Type.module`, `Type.bigint`, ...

Use `typeName(value)` to obtain the concrete debug name of a value's type.

### 2.12 Metadata and Type Identity Boundary

Xray keeps only the minimal type identity layer by default:

- `typeOf(x)` returns a stable `Type` / `TypeId` for branches, `match`, and analyzer narrowing.
- `typeName(x)` returns a debug/logging type-name string and is a cold-path capability.
- Nominal type checks use `x is T` / `x as T`; do not compare type-name strings.
- Field, method, and constructor enumeration is not a default runtime capability. Structured metadata for serialization, inspect, RPC schema, and similar use cases is generated explicitly by `@derive(...)` or compile-time tooling.

Runtime type queries use `typeOf(value)`, `typeName(value)`, and `TypeId`. Reflection metadata is not exposed as a traversable or callable object graph.

### 2.13 Flow-Sensitive Type Narrowing

> Source of truth: `src/frontend/analyzer/xanalyzer_flow.c` (fact operators and control-flow propagation), `src/frontend/analyzer/xanalyzer_visitor_expr.c` (reference-site query and `E0379`).

**Narrowing** tightens the **static type** of a binding at a specific control-flow position to a subtype of its declared type. Narrowed types feed member lookup, overload resolution, `match` exhaustiveness, and **code generation**, so narrowing is **semantics**, not a diagnostic optimization: for the same program the VM and AOT backends must compute identical narrowing results.

Rules `N-1` … `N-13` in this section are normative. Each has a conformance case under `tests/compile_errors/narrowing/` or `tests/regression/16_narrowing/`.

#### 2.13.1 Narrowable Subjects

**N-1** Only **simple bindings** narrow: local `var` / `const` bindings and bare parameter identifiers.

**N-2** The following positions **never narrow**, even after a null check or an `is` check: field access `p.f`, `this.f`, index `a[i]`, tuple component `t.0`, call results `f()`, and any other non-identifier expression. A check on such a position determines the type of the check expression itself only; it does not affect later accesses spelled the same way.

> Rationale: only a simple binding has all of its write sites statically enumerable within the function. Fields and elements can change through an alias, another coroutine, or a method call; narrowing them would require place equivalence plus alias invalidation analysis, whose cost and uncertainty exceed the benefit.

**N-3** To get flow sensitivity for a non-narrowable position, extract a local binding first:

```xray @id=narrowing-extract-binding
class Address { city: string
    constructor(city: string) { this.city = city } }
class User { address: Address?
    constructor(address: Address?) { this.address = address } }

fn show(u: User) {
    var addr = u.address          // extract into a simple binding
    if (addr != null) {
        print(addr.city)          // OK: addr is narrowed to Address
    }
}
```

#### 2.13.2 Facts and Operators

**N-4** A condition expression produces **facts** in two directions: a true fact and a false fact. The table below is the complete list; forms not listed produce no facts in either direction. `x` is a narrowable subject (N-1); `e` is any condition expression.

| Condition form | True branch | False branch |
|--|--|--|
| `x` | remove `null` | no fact (`0` / `""` / `false` are falsy too) |
| `!e` | false fact of `e` | true fact of `e` |
| `(e)` | true fact of `e` | false fact of `e` |
| `x == null` / `null == x` | keep only `null` | remove `null` |
| `x != null` / `null != x` | remove `null` | keep only `null` |
| `x is T` | intersect with `T` | remove the part intersecting `T` |
| `typeOf(x) == Type.K` | keep members of kind `K` | remove members of kind `K` |
| `typeOf(x) != Type.K` | remove members of kind `K` | keep members of kind `K` |
| `e1 && e2` | true facts of `e1` then `e2` | no fact |
| `e1 \|\| e2` | no fact | false facts of `e1` then `e2` |

**N-5 (short-circuit inheritance)** In `e1 && e2`, `e2` is analyzed under the true fact of `e1`; in `e1 || e2`, `e2` is analyzed under the false fact of `e1`. Because `T?` cannot be used directly as a condition (§2.5), this rule makes the two forms below the standard way to work with nullable values:

```xray @id=narrowing-short-circuit
fn check(a: string?) -> bool {
    if (a != null && len(a) > 0) { return true }     // e2 analyzed with a: string
    if (a == null || len(a) == 0) { return false }   // e2 analyzed with a: string
    return true
}
```

**N-6 (intersection semantics)** True direction of `x is T`: when the declared type is a union, keep the members that intersect `T` (none → `never`); otherwise the result is `T` when the two intersect and `never` when they do not. The false direction symmetrically removes the intersecting part. `x is T` treats primitive types, named types, and generic instances alike.

#### 2.13.3 Propagation

**N-7** Facts take effect in: the then / else body of `if`, both arms of a conditional expression `c ? a : b`, the loop-body entry and the code after `while` / `for` (false fact), the right operand of `&&` / `||`, and the code following `assert(c)`.

**N-8 (early exit)** When a branch necessarily terminates (`return` / `throw` / `break` / `continue`), the code after it inherits the opposite-direction fact:

```xray @id=narrowing-early-return
fn nameLen(s: string?) -> i64 {
    if (s == null) { return 0 }
    return len(s)                 // s is narrowed to string here
}
```

**N-9 (join)** The type at a join point is the union of the types on all predecessor paths; unreachable predecessors contribute `never` and are absorbed by the union.

**N-10 (loops)** The type at a loop header is the union of the entry edge and every back edge. A back edge that assigns the binding contributes the assigned expression's type; a back edge that reaches the header **without** an assignment leaves the value unchanged and contributes nothing to the union. An assignment inside the loop body therefore makes the next iteration re-derive the condition narrowing from the joined type, while a narrowing established before the loop survives a body that never writes the binding:

```xray @id=narrowing-loop
fn drain(first: string?) {
    var cur = first
    while (cur != null) {         // cur is string at loop-body entry
        print(cur)
        cur = null                // back edge contributes null; header re-joins
    }
}
```

#### 2.13.4 Invalidation

**N-11** Narrowing is invalidated by:

1. **assignment** / compound assignment / `++` / `--`: the static type resets to the static type of the assigned expression;
2. being passed as a `ref` argument: resets to the declared type;
3. `move x`: the binding becomes unusable (§10);
4. being **assigned inside any closure body**: the binding does not narrow anywhere in the function body, because when the closure runs is unknowable. The rule does not depend on where the closure appears — one written after the narrowing site suppresses it just the same. The diagnostic names this cause; the fix is a fresh binding that is never written;
5. an ordinary function call does **not** invalidate narrowing — N-1 / N-2 guarantee a narrowable subject cannot be written by a callee.

**N-11.1 (function-body boundary)** Every function body — named function, method, or closure — owns its flow graph: facts from the enclosing body do **not** enter a closure body, and facts inside a closure do not escape it. Re-check inside the closure when narrowing is needed there.

```xray @id=narrowing-invalidation
fn f(a: string?) {
    if (a != null) {
        print(len(a))             // OK
        a = null                  // assignment invalidates the narrowing
        print(a ?? "")            // must be unwrapped again
    }
}
```

#### 2.13.5 Narrowing and Null Diagnostics

**N-12** When the static type of a receiver can still be `null` (including a type that is always `null`), member access, indexing, calls, arithmetic, `len()`, and iteration all report `E0379` (`XR_ERR_ANALYZE_POSSIBLY_NULL`); see §18.2. Three exceptions: `==` / `!=`, which is how narrowing starts; `&&` / `||`, whose operands are checked as conditions; and **string concatenation** — when either side of `+` is a `string`, a null operand renders as `"null"` per §2.5.

**N-13** There are exactly three unwrapping forms, each independent of N-4:

- `x!`: statically removes `null`; panics (`NullError`) at run time when the value is `null` — this is **not** undefined behavior;
- `x ?? d`: the result type is the union of `x` without `null` and `d`;
- `x?.f`: optional chaining, **whole-chain short-circuit** — when any link is `null` the entire postfix chain evaluates to `null`, and the result type is nullable (§3.6).
### 2.14 Ownership, Aliasing, and Loans

> Truth source: `src/frontend/analyzer/xa_ownership.h` (evidence axes and decision structures), `src/frontend/analyzer/xanalyzer_visitor_expr.c` (`move` decision), `src/frontend/analyzer/xanalyzer_visitor_stmt.c` (alias and loan tracking), `src/ir/xi_source_move_verify.c` (independent Xi-level re-check).

Xray has no lifetime syntax and no borrow-checker annotations. Ownership is nonetheless **defined**: `move`, `copy`, `ref`, `Slice<T>`, and every cross-coroutine transfer read one decision procedure, and this section states it.

#### 2.14.1 Ownership roots

An **ownership root** is the entry point of a heap object graph that can be reclaimed on its own. `Array`, `Map` (including `JSON.Object`), `Set`, composite `JSON.Value` arms, `structural object`, class instances, and a unique-result `Task<T>` each have their own root. Scalars, `string`, `Slice<T>`, raw pointers, value structs, and fixed arrays have **no** root: they are copied by value or they are borrowed views.

Only a binding that owns a root can transfer ownership. Writing `move` on a rootless value is a compile error (`E0391`: `move is not meaningful for value type`).

#### 2.14.2 Four independent evidence axes

At every program point a binding carries four **mutually independent** pieces of evidence. A legal ownership operation requires all four at once:

| Axis | Question it answers | Values |
|--|--|--|
| **Binding state** | Is this name usable now | `UNINITIALIZED` / `LIVE` / `MOVED` / `MAYBE_MOVED` / `UNKNOWN` |
| **Root aliasing** | Does another reference reach the same root | `UNIQUE` / `LOCAL_ALIASED` / `ESCAPED` / `ALIAS_UNKNOWN` |
| **Capability** | What is permitted | `MUTABLE` / `CONST` / `SYNC_INTERIOR_MUTABLE` / `UNKNOWN` |
| **Loans** | Is anything borrowed out | a set of loans: Slice views / raw pointer borrows / closure captures |

The split is deliberate: binding state is a CFG fact, aliasing is an object-graph fact, capability is a permission, and a loan is a bounded place fact. None of the four can be derived from or substituted for another.

**Fail-closed by default**: when an axis cannot produce positive evidence, the answer is rejection, not permission. That is why the result of a call with unknown provenance cannot be moved — the compiler has no aliasing evidence for it.

#### 2.14.3 How aliases are created and end

| Action | Effect on root aliasing | Recoverable |
|--|--|--|
| `var b = a` | `LOCAL_ALIASED` | Yes. After `b`'s last use the root is `UNIQUE` again |
| `arr.push(a)` / `obj.f = a` / `m[k] = a` / `[a]` / `#{k: a}` / `Enum.V(a)` | `ESCAPED` | **No.** Function-local analysis cannot see that slot being overwritten |
| Result of a call with unknown provenance | `ALIAS_UNKNOWN` | No |
| `copy(a)` | `a` unaffected; the result is a fresh `UNIQUE` root | — |
| `move a` | `a` becomes `MOVED`; the root moves with it | — |

Liveness is decided by **last use**, not by lexical scope, matching the borrow rules in §2.4.2. So the first function below is legal and the second is not:

```xray
fn ok() {
    var buf = [1, 2, 3]
    var alias = buf
    print(len(alias))          // alias's last use
    consume(move buf)          // OK: the alias has ended
}

fn rejected() {
    var buf = [1, 2, 3]
    var alias = buf
    consume(move buf)          // E0391: strong alias 'alias' remains live
    print(len(alias))
}
```

`ESCAPED` being terminal is deliberate: once a reference is written into a heap graph, who still holds it is no longer a question this function can answer. Use `copy(a)` when a transfer is needed anyway.

#### 2.14.4 How loans are created and end

Three loan forms share one loan record, one non-lexical liveness rule, and one set of error codes (`E0382` / `E0383` / `E0384`):

| Form | Borrower | Live until |
|--|--|--|
| `Slice<T>` view | the view binding | that binding's last use |
| `Ptr<T>` / `MutPtr<T>` | the pointer binding | that binding's last use |
| **Closure capture** | the closure binding | that binding's last use |

An ordinary synchronous closure captures an outer binding **by reference**, so a capture is a loan and not a copy:

```xray
fn rejected() {
    var buf = [1, 2, 3]
    const peek = fn() -> i64 { return len(buf) }
    go consume(move buf)       // E0382: closure capture 'peek' is active
    print(peek())
}
```

A closure literal that appears only as a **call argument** usually creates no live loan: it ends with the call and cannot outlive it.

```xray
fn ok() {
    var buf: Array<i64> = []
    items.forEach(fn(x: i64) { buf.push(x) })   // capture bounded by the call
    consume(move buf)                            // OK
}
```

The exception is a callee that **retains or escapes** that parameter: the closure then outlives the call, and every root it captured by reference escapes with it (`OWN-E-ESCAPED-ROOT`). The decision reads the callee's parameter effect summary, not the syntactic shape.

A live loan forbids invalidating operations on the owner, and `move` is one of them (`E0382`).

#### 2.14.5 The full conditions for `move`

`move x` requires `x` to be a **rebindable local `var` root**, and:

1. binding state is `LIVE` (not moved, not maybe-moved, not unknown);
2. root aliasing is `UNIQUE`;
3. capability is `MUTABLE` (`const` values and synchronization handles cannot be moved);
4. no loan is live;
5. the storage plan is complete (the compiler has resolved an allocation domain for the root);
6. the consumed binding is not declared outside a loop that would run the `move` again.

`move` accepts an **identifier** only: `move x.field`, `move arr[i]`, and `move f()` are syntax errors. A field or an element has no ownership root of its own — its root is the container — and transferring one slot would leave the container partially moved, a state with no representation. To take one slot out, `copy` it, or make the container itself the move source.

After a successful move the source binding is statically marked moved, and any later reference is a compile error. **A rejected move does not poison the source**: after the diagnostic, `x` is still usable.

Rejection reasons are named in the diagnostic so the failing axis is identifiable:

| Reason | Meaning |
|--|--|
| `OWN-E-LIVE-ALIAS` | a local strong alias is still live |
| `OWN-E-ESCAPED-ROOT` | the root was written into a heap graph |
| `OWN-E-UNKNOWN-CALL` | uniqueness evidence is incomplete (call result with unknown provenance) |
| `OWN-E-STORAGE-PLAN` | the storage / ownership plan is incomplete |
| `OWN-E-LIVE-LOAN` | a loan is live (Slice view / raw pointer / closure capture) |

#### 2.14.6 Value copies and managed fields

A value struct is copied by value. So that "copied by value" is always the complete semantics, **struct field types are restricted**: only scalars, `string`, raw pointers, fixed arrays, and other value structs are allowed. `Array`, `Map`, `Set`, `JSON.Value`, and class instances **cannot** be struct fields (`E0352`).

A struct value copy therefore never carries a mutable managed field, and there is no shallow-versus-deep choice to make. The one managed field type is `string`, and `string` is immutable: sharing it produces no observable difference and does not affect the uniqueness decision.

Use a class when an aggregate needs a mutable graph. A class is a reference type, so assignment creates an alias and §2.14.3 governs it.

### 2.15 Worked Examples

Self-contained programs that run as-is and pass `xray check` (comments show the real output).

Arrays:

```xray
fn main() {
    var nums = [1, 2, 3]
    nums.push(4)
    print(nums)          // => [1, 2, 3, 4]
    print(len(nums))     // => 4
    var doubled = nums.map(fn(x: i64) -> i64 { return x * 2 })
    print(doubled)       // => [2, 4, 6, 8]
    var evens = nums.filter(fn(x: i64) -> bool { return x % 2 == 0 })
    print(evens)         // => [2, 4]
}

main()
```

Maps and Sets:

```xray
fn main() {
    var scores = #{"alice": 95, "bob": 88}
    scores.set("carol", 77)
    print(scores.get("alice") ?? 0)   // => 95
    print(len(scores))                 // => 3

    var seen = Set<i64>()
    seen.add(1)
    seen.add(2)
    seen.add(2)
    print(len(seen))          // => 2
    print(seen.contains(1))   // => true
}

main()
```

Nullable types with `??`:

```xray
fn main() {
    var name: string? = null
    print(name ?? "anonymous")   // => anonymous
    var city: string? = "NYC"
    print(city ?? "unknown")     // => NYC
}

main()
```

<!-- /xr-spec:en -->

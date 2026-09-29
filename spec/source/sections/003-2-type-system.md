---
id: spec.2_type_system
order: 003
---

<!-- xr-spec:cn -->
---

## 2. 类型系统 (Type System)

> 类型语法与内置身份锚点：`src/frontend/parser/xparse_type.c`、`stdlib/prelude/builtin_symbols.def`。本章定义语言合同；既有运行时或 analyzer 的表示不是值语义合法性的来源。当前新 XIR 实现资格见 §17。

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
| 容器 | `Array<T>`、`Map<K,V>`、`Set<T>`、`Channel<T>` |
| 定长布局 | `[T; N]` |
| 借用视图 | `Slice<T>` / `MutSlice<T>`（共享只读 / 独占可写，不拥有元素，见 §2.4.2） |
| Prelude 特殊类型/命名空间 | `JSON`（含 `JSON.Value` / `JSON.Object`）、`BigInt`、`Range`、`Regex`、`StringBuilder`、`Atomic<T>`、`Path`、`Thread<T>`、`Os*` 同步类型 |
| 模块导出类型 | `DateTime`、`Logger`、`NetConn`、`NetListener`、`Plan`、`Mutex<T>` 等；必须从定义它们的模块显式 import |
| 错误处理 prelude | `PanicInfo`（见 §8） |
| Nullable | `T?` |
| Union | `A \| B \| ...` |
| Tuple | `(T1, T2, ...)` |
| Function | `fn(T1, T2) -> R` |
| FFI / C ABI | `Ptr<T>`、`MutPtr<T>`、`CFn<fn(T) -> R>`、`usize`、`isize` |
| Class / Struct / Interface | 用户定义（nominal） |
| Enum | 用户定义（含 ADT enum，见 §5.6） |
| Type alias | `type Name = SomeType`、`type Name<T> = SomeType` |

内置登记表是实现投影，不是新 XIR 声明族的准入清单。§2.4.2 规定的 `MutSlice` 尚未列入该表；完整视图注册与准入仍待对应声明族实现，不能把规范中的类型说明计作当前实现覆盖。

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
| `OsBarrier` | prelude |
| `OsCondvar` | prelude |
| `OsMutex` | prelude |
| `OsOnce` | prelude |
| `OsRwLock` | prelude |
| `PanicInfo` | prelude |
| `Path` | prelude |
| `Ptr<T>` | 解析器内建 |
| `Range` | prelude |
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
| `NumberParseError` | `InvalidSyntax` \| `OutOfRange` |
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
- 整数除法向零截断，非零余数与被除数同号；除数为零报错。任一有符号整数类型的 `MIN / -1` 按其位宽环绕为 `MIN`，`MIN % -1` 为零。
- `i64` 的 `checkedAdd` / `checkedSub` / `checkedMul` 在溢出时返回 `null`；`saturating*` 饱和到 `i64` 边界；`wrapping*` 显式执行默认二补码环绕。
- 已定型表达式不能通过赋值隐式窄化、改变符号或改变目标相关宽度；这些转换必须显式写 `as`。显式整数转换按目标位宽取模并以目标类型的二补码位型解释。
- 动态擦除后的 `XrValue` 只保存整数 payload，不保存有符号性或位宽；跨过 `JSON.Value` / 动态容器等边界后，超过 `i64` 正范围的 `u64` 值在格式化和顺序比较中的行为不保证保留无符号语义。需要无符号语义时保持适当的 exact 无符号静态类型（`u8` / `u16` / `u32` / `u64`）。

#### 2.3.2 浮点类型

| 类型 | 标准 |
|--|--|
| `f32` | IEEE-754 单精度 |
| `f64` | IEEE-754 双精度；默认浮点类型 |

字面量默认 `f64`。

浮点 `+ - * /` 每次在操作数的共同精度按 round-to-nearest, ties-to-even 舍入；`f32` 与 `f64` 混合时先无损加宽到 `f64`。保留次正规数并渐进下溢，溢出为带符号无穷大。除零遵循 IEEE 规则，不触发整数除零错误；无效操作与 NaN 结果统一为正 canonical quiet NaN。禁止隐式融合或重排操作，不依赖或改变宿主浮点舍入状态与异常标志。浮点 `%` 与位运算不成立，已定型整数与浮点混合仍须显式转换。

#### 2.3.3 `bool`

`true` / `false`，独立类型，与数值类型**不可隐式互转**（不能 `var x: i64 = true`，也不能 `var b: bool = 1`）。

**条件表达式规则**（`if` / `while` / `for` 条件 / 三元 `?:` / `match` 守卫）：

| 条件类型 | 是否允许 | 语义 |
|---|---|---|
| `bool` | 允许 | 直接布尔判断 |
| `T?` 且 `T != bool` | 编译错误 | 显式写 `value != null` 检查存在性 |
| `bool?` | 编译错误 | 三态歧义；写 `flag == true` / `flag != null` / `flag ?? false` |
| `i64` / `f64` / `string` / `rune` / 集合 / 对象 | 编译错误 | 必须写显式比较，如 `n != 0`、`len(s) != 0` |

`&&` / `||` / `!` 的操作数必须是 `bool`；不要把 `T?` 直接放进 `&&` / `||`。

```xray @id=types-explicit-conditions
var ok = true
if (ok) { }

var user: User? = findUser()
if (user != null) {      // 显式存在性检查
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

`Array<T>` 是有序、可增长的值类型。赋值、保存参数和返回值保留独立的逻辑副本；实现可以共享元素存储，但经一个可写绑定修改或追加元素不得改变其他逻辑副本。复制沿值语义部分递归，在 class 或同步身份处只复制引用，不复制或冻结所引用的对象。方法见 §14.7，所有权规则见 §2.14。

```xray @id=types-array
var a: Array<i64> = [1, 2, 3]
var b = a                        // b 是逻辑副本
b[0] = 9                         // a[0] 仍为 1
var c: Array<string> = []         // 显式空数组
```

`T` 必须在编译期确定，且可复制、可保存；借用视图和不可复制资源不能作为普通 Array 元素。空 `[]` 需要类型上下文，无上下文时是编译错误：`Empty array '[]' requires a type annotation`。非 nullable Array 必须显式初始化，不从未初始化存储推定为空数组。

读出元素产生类型为 `T` 的逻辑副本；写入按 `T` 检查，只采用该类型已允许的赋值转换。`Array<T>` 不变：元素可转换不意味着整个数组可隐式转换，例如 `Array<i8>` 不能整体隐式转换为 `Array<i64>`。`Array<rune>` 保留 `rune` 身份，读写均按 `rune` 检查。

容器本身只要求存储所需能力，不要求所有 `T` 都可比较、排序、哈希或字符串化。条件方法的约束必须由相应声明明确给出；具体实例恰好提供某成员不能补足普通泛型定义处的约束。

本节规定语言合同，不表示所有 Array 方法、聚合元素或借用视图已获新 XIR 源入口准入；当前实现资格见 §17。未准入的声明族必须明确拒绝。

#### 2.4.1.1 定长数组 `[T; N]`

`[T; N]` 是定长布局数组类型，表示 `N` 个 `T` 元素。`N` 是类型的一部分，必须能在分析期求值为正的编译期整数表达式；当前支持整数字面量、`const` 整数标识符、括号、一元 `-`/`~`，以及整数算术/位运算。当前后端编码上限为 65535 个元素。

定长数组可用于 struct inline 字段和局部变量，并支持 struct、嵌套定长数组和拥有式容器等元素类型，因此可以递归组合：

```xray
var bytes: [u8; 4] = [1, 2, 3, 4]
var zero: [u8; 64] = [0; 64]
var names: [string; 2] = ["a", "b"]
var blocks: [[u8; 2]; 2] = [[1, 2], [3, 4]]
```

有目标类型的数组字面量初始化 `[T; N]` 时必须 exact-length；重复初始化 `[value; N]` 的 `N` 同样是正的编译期整数表达式，并且必须和目标类型长度一致。无上下文的普通数组字面量仍推断为动态 `Array<T>`；无上下文的 `[value; N]` 推断为 `[T; N]`。

定长数组支持 `len(array)`、索引读取、可写 place 上的索引写入和 `ref` 参数传递。借用视图按 §2.4.2 区分共享 `Slice<T>` 与独占 `MutSlice<T>`：

```xray
var data: [u8; 4] = [5, 6, 7, 8]
var view: MutSlice<u8> = data[1:4]
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
- `Array<T>`：动态长度、可增长的值类型；元素存储可在堆上共享并按需分离。
- `Slice<T>` / `MutSlice<T>`：借用连续存储的共享 / 独占视图，不拥有元素（见 §2.4.2）。

旧的 `[N]T` 语法不属于 Xray 语言。

#### 2.4.2 `Slice<T>` / `MutSlice<T>`

`Slice<T>` 是共享只读借用，`MutSlice<T>` 是独占可写借用。它们描述另一个值拥有的连续元素存储，自身不拥有元素；借用始终依附可证明的 owner 和访问权限。以下规则定义视图合同，不表示新 XIR 源入口已经支持视图声明族；其完整准入与验证见 §17。

##### 构造与访问

切片使用显式目标视图类型。`array[start:end]` 或 `fixedArray[start:end]` 可形成共享视图；形成 `MutSlice<T>` 还要求 owner 是独占可写 place，并在视图产生前完成必要的 COW 分离。字符串字节视图 `str.bytes()` 是 `Slice<u8>`，不能用于修改字符串。

`len(view)` 读取长度，`view[i]` 读取元素；只有 `MutSlice<T>` 允许 `view[i] = value`，写入对应 owner 的逻辑值。视图不提供成员方法或 `.length`。从 `const` owner 派生的视图只能只读；不得用尚未冻结的 type-position `const T` 重新定义可写视图。

owner 必须是可证明存活的具名局部 place、参数或 receiver 字段路径，不能借用临时值。嵌套投影保护所属根及必要路径。class 字段借用须保活对象并动态检查独占；只有局部无冲突证明才可省去动态检查。

`MutSlice<T>` 赋值转移借用，普通调用暂时重借用。`MutSlice<T>` 到 `Slice<T>` 是共享重借用；派生的只读视图存活期间，原可写视图不能写入。子切片也必须遵守同一借用权限。

```xray @id=types-slice
fn main() {
    var arr: Array<i64> = [10, 20, 30, 40]
    var view: Slice<i64> = arr[1:3]
    print(view[0])                         // 20；view 的最后一次使用
    var writable: MutSlice<i64> = arr[1:3]
    writable[1] = 31                       // writable 的最后一次使用
    print(arr[2])                          // 31
    var owned: Array<i64> = copy(arr[1:3]) // 物化拥有式副本
    arr.push(50)                           // 没有存活借用
    print(len(owned))                      // 2
}

main()
```

##### 借用规则

设视图 `v` 借用 owner `o`。借用存活期按最后一次使用而非词法块末尾判定：

1. `Slice<T>` 存活期间不得修改 `o`，包括元素写入；不能因为一次写入可能不改变物理地址就放行。
2. `MutSlice<T>` 存活期间不得经 owner 或其他路径读取、写入或复制 `o`。只有当前独占视图及合法重借用可访问该存储。
3. 重新绑定、销毁、消费或其他使借用失效的操作被拒绝（`E0382`）。操作效应必须反映 COW 分离与重定位；不得把 Array 元素替换一概标为地址稳定。
4. 视图仅可用于局部、参数、返回及 tuple / Optional 包装；包装继承全部借用限制。视图不得进入普通集合、用户聚合、模块存储或 `go` 等跨执行边界，不得借 `as` 擦除限制（`E0383`）。
5. 被普通闭包以共享 cell 捕获的变量不能产生视图。捕获视图的闭包可以立即调用；跨参数传递要求显式、已冻结的借用调用合同，保证不保存、不返回且调用结束前完成。被调方实现推断不能代替该公开合同，也不因闭包字面量写在实参位置就认定不逃逸。
6. 普通调用中的借用可以跨挂起，但须始终保活 owner 并维持访问权限。帧地址稳定不能代替这些证明；生成器首版不得保存借用参数。

```xray
fn rejected() {
    var bytes: Array<u8> = [1, 2]
    var view: Slice<u8> = bytes[:]
    bytes[0] = 3                    // E0382：共享借用仍存活
    print(view[0])
}
```

##### 跨函数：返回来源合同

返回视图的来源首版只由签名确定：整个签名必须恰有一个视图输入或 `ref` 输入候选；`ref` receiver 也计入候选。零候选或多个候选均拒绝（`E0384`），即使函数体始终返回某一个参数；不得从函数体或静态存储特例推导跨模块来源。返回局部值的视图同样非法。

tuple / Optional 中的每个返回视图都依附该唯一候选，并单独验证可写性；可写返回不能来自只读候选。返回两个 `MutSlice` 还要求显式的不重叠构造合同，普通用户函数不能只凭下标算式建立该保证。

```xray
fn tail(data: Slice<u8>, start: i64) -> Slice<u8> {
    return data[start:]              // 唯一候选为 data
}

fn rejected(a: Slice<u8>, b: Slice<u8>) -> Slice<u8> {
    return a                        // E0384：签名中有两个候选
}
```

##### 物化拥有式副本

`copy(view)` 将借用数据物化为 `Array<T>`，其元素遵循 §2.14 的逻辑复制与身份边界；class 元素仍引用同一个对象。结果不再借用原 owner。此用途不裁决 `copy(array)` 的公开拼写，也不使其成为普通 Array 赋值的前置条件。

`ref` 参数和 `Ptr<T>` / `MutPtr<T>` 同样必须满足来源、权限、存活与不逃逸要求。相关诊断为 `E0382`（存活借用冲突）、`E0383`（借用逃逸）、`E0384`（来源不稳定或不唯一）；`unsafe` 不放宽这些要求。

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

整数as操作数独立于目标定型；直接正字面量（允许括号）超过i64最大值但不超过u64最大值时，保留u64源幅度后再按目标取模。`18446744073709551615 as i8`为-1；`200 as i8`仍按显式转换环绕，不要求字面量先满足i8范围。

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
| `x: bool` | 无类型窄化事实 | 无类型窄化事实 |
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
3. 成功消费 `x` 之后：绑定不可用（见 §2.14.5）；
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
### 2.14 值、身份与借用

本节定义声明类型、参数模式及函数内数据流共同决定的语言合同。存储布局、引用计数和 COW 是实现机制，不能反向改变源程序合法性。Built→Checked 检查这些规则；特化在 Checked 上完成并复验，Lowered 才选择具体复制、分离和释放操作。完整声明族的实现资格见 §17，规格冻结不等于实现完成。

#### 2.14.1 逻辑值与身份边界

Array / Map / Set / string、可复制 struct、enum、tuple 和匿名记录按值复制。复制获得独立的逻辑值；实现允许共享不可变存储或采用 COW，但经一个副本修改值语义部分不得污染其他副本。调用返回的 Array 也遵守同一合同，不要求调用方分析被调方函数体以证明其存储唯一。

class 实例和同步对象具有身份。复制其引用仍指向同一个对象；递归复制聚合时在这个身份边界停止。因而复制含 class 元素的 Array 不复制 class 对象，修改对象状态可经其他引用观察到；替换某个 Array 元素则仅影响该数组的逻辑值。

`const` 固定绑定及其值语义部分，不冻结整个可达图。`const xs = [Counter()]` 禁止经 `xs` 改写或追加数组元素，但不冻结 Counter 的身份状态；对象方法与同步 API 仍受各自合同控制。是否能跨执行共享由完整类型的 Sendable 性质决定，不能由 `const` 或底层原子引用计数推导。

#### 2.14.2 初始化、权限与借用事实

每个程序点都必须分别证明：

| 事实 | 必须证明的条件 |
|--|--|
| 初始化和消费状态 | 读取前已完整初始化；需要持有权的操作不得使用已消费或可能已消费的值 |
| place 权限 | 写入目标可写；只读参数和 `const` 的值语义部分不能直接写入 |
| 值与声明合同 | 类型、复制或消费能力、参数模式、保存与返回权限符合声明 |
| 活动借用 | 访问不与仍存活的共享或独占借用冲突，借用来源和存活期有效 |

初始化与消费状态是控制流事实，权限来自绑定和声明，借用保护具体 place 及必要路径；这些证明不能互相代替。缺少真实所需证明必须拒绝，但 Array backing 是否唯一不是普通赋值、保存副本、返回或修改的语言准入条件。非 nullable Array 仍须显式初始化（§5.1.1）。

#### 2.14.3 普通复制与存储共享

`var b = a`、保存只读参数、返回 Array，以及把可复制 Array 存入另一个合法容器或聚合，均保留逻辑副本，不建立对源数组可变存储的语言级别名。它们不会把源绑定标为永久 `ESCAPED`，也不要求其他副本先结束存活期才能修改源值。底层存储的唯一性只可用于省略分离或复制的优化。

```xray
var a = [1, 2, 3]
var b = a
const snapshot = a
b[0] = 9
a.push(a[0])                 // 先求值实参，再建立 ref receiver 的独占访问
print(snapshot[0])          // 1
print(len(snapshot))        // 3
print(b[0])                 // 9
print(len(a))               // 4
```

复制必须取得可独立释放的结果责任；失败不得发布部分初始化的结果。自赋值、读取自身元素后写回或追加均须维持这一责任，不能因物理共享造成重复释放或使用已释放存储。求值顺序遵循 §3.0 E2/E5；复制或自动移动优化不能改变既定错误、析构或外部副作用顺序。

#### 2.14.4 place 借用与捕获

`ref` 是独占可写 place 借用。调用先按 E2 求值 receiver 和全部实参，再在进入被调方前建立借用；存活期间不得通过其他路径冲突访问借出的 place。赋值仍按 E5 求值位置子表达式、索引、右值，再存储，不因 COW 实现更改顺序。

`Slice` 共享借用与 `MutSlice` 独占借用遵循 §2.4.2；class 字段借用还须保活对象并检查动态独占。底层 backing 共享不能成为取消这些检查的理由。反过来，普通 Array 逻辑副本不因共享 backing 自动成为借用。

普通闭包的外层 `var` 捕获共享 cell，`const` 与可复制非视图的普通 READ 参数按值捕获。共享 cell 的身份不能伪装成不可逃逸的临时 loan；其存储和释放责任必须显式保留。被共享 cell 捕获的变量不能产生视图。视图捕获与借用回调的准入遵循 §2.4.2 的显式合同，不能从被调方实现或实参语法形状猜测。

#### 2.14.5 消费与尚未准入的表面

不可复制资源的消费转移持有权和最终析构责任，调用是否消费由声明的参数或 receiver 模式决定。对这类转移，`move x` 是可选的显式标记；源绑定成功消费后不可再用，未被所有路径重新初始化的循环再次消费也必须拒绝。只读与 `ref` 借用不消费持有权。被拒绝的操作不能把有效源状态错误地标为已消费。

可复制类型（包括 Array）不能声明 `move` 参数或 receiver。可复制值是否另行允许独立的 `move x` 表达式、普通值 `copy(array)` 是否保留公开拼写，以及 type-position `const T` 的身份和转换，均不在本已冻结子集内，须在准入前单独确定。这些待定表面不阻塞普通 Array 复制；不得继承旧唯一根、堆逃逸终态或整个图深冻结规则作为默认答案。

#### 2.14.6 struct 字段的复制与初始化

可复制 struct 按字段的声明类型复制：Array / Map / Set 等容器字段保留逻辑副本，string 保持不可变值语义，class 和同步字段只复制身份引用。不得仅因字段是 Array、其他拥有式容器或 class 引用而拒绝该 struct；也不能把带 class 字段的 struct 描述成整个可达图都独立。

字段仍须满足普通存储、初始化和可复制性要求。视图不能进入普通用户聚合；不可复制字段或载荷要求所属 struct / enum 也不可复制，其声明族另行准入。含非 nullable Array 的字段不能无条件零初始化，默认构造须满足 §5 的字段规则。

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

> Type-syntax and built-in identity anchors: `src/frontend/parser/xparse_type.c` and `stdlib/prelude/builtin_symbols.def`. This chapter defines language contracts; existing runtime or analyzer representations do not determine value-semantic legality. See §17 for current new XIR implementation qualification.

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
| Borrowed view | `Slice<T>` / `MutSlice<T>` (shared read-only / exclusive writable; no element ownership, see §2.4.2) |
| Special prelude types/namespaces | `JSON` (including `JSON.Value` / `JSON.Object`), `BigInt`, `Range`, `Regex`, `StringBuilder`, `Atomic<T>`, `Path`, `Thread<T>`, and the `Os*` synchronization types |
| Module-exported types | `DateTime`, `Logger`, `NetConn`, `NetListener`, `Plan`, `Mutex<T>`, and others; these require explicit imports from their defining modules |
| Error-handling prelude | `PanicInfo` (see §8) |
| Nullable | `T?` |
| Union | `A \| B \| ...` |
| Tuple | `(T1, T2, ...)` |
| Function | `fn(T1, T2) -> R` |
| FFI / C ABI | `Ptr<T>`, `MutPtr<T>`, `CFn<fn(T) -> R>`, `usize`, `isize` |
| Class / Struct / Interface | user-defined (nominal) |
| Enum | user-defined (incl. ADT enum, see §5.6) |
| Type alias | `type Name = SomeType`, `type Name<T> = SomeType` |

The built-in registry is an implementation projection, not the new XIR declaration-family admission list. The `MutSlice` contract in §2.4.2 is not yet represented in this registry. Full view registration and admission remain implementation work for that declaration family; describing the type does not count as current implementation coverage.

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
| `OsBarrier` | prelude |
| `OsCondvar` | prelude |
| `OsMutex` | prelude |
| `OsOnce` | prelude |
| `OsRwLock` | prelude |
| `PanicInfo` | prelude |
| `Path` | prelude |
| `Ptr<T>` | resolver built-in |
| `Range` | prelude |
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
| `NumberParseError` | `InvalidSyntax` \| `OutOfRange` |
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
- Integer division truncates toward zero; a nonzero remainder has the dividend's sign. A zero divisor faults. For every signed integer type, `MIN / -1` wraps to that type's `MIN`, and `MIN % -1` is zero.
- `i64.checkedAdd` / `checkedSub` / `checkedMul` return `null` on overflow; `saturating*` clamps to the `i64` boundary; `wrapping*` explicitly performs the default two's-complement wrap.
- An already-typed expression cannot be implicitly narrowed, change signedness, or cross a target-dependent width at assignment. Such conversions require an explicit `as`. Explicit integer conversion reduces modulo the target width and interprets the resulting two's-complement bit pattern as the target type.
- After dynamic erasure, `XrValue` stores only the integer payload, not signedness or width. Across `JSON.Value` / dynamic-container boundaries, `u64` values above the positive `i64` range are not guaranteed to keep unsigned formatting or ordering semantics. Keep the value in the appropriate exact unsigned static type (`u8`, `u16`, `u32`, or `u64`) when unsigned semantics are required.

#### 2.3.2 Floating-Point Types

| Type | Standard |
|--|--|
| `f32` | IEEE-754 single precision |
| `f64` | IEEE-754 double precision; default floating type |

Literals default to `f64`.

Floating `+ - * /` rounds each operation at the common operand precision,
nearest with ties to even; a mixed f32/f64 pair first widens f32 exactly to
f64. Subnormals use gradual underflow and overflow produces signed infinity.
Division by zero follows IEEE rules rather than raising an integer division
fault. Invalid operations and NaN results produce the positive canonical quiet
NaN. Operations are not implicitly fused or reassociated and neither depend on
nor alter host rounding state or exception flags. Floating remainder and bitwise
operations reject; already typed integer/float mixing still requires an explicit cast.

#### 2.3.3 `bool`

`true` / `false`, a standalone type. **No implicit conversion** to/from numeric types (cannot write `var x: i64 = true` or `var b: bool = 1`).

**Condition expression rules** (`if` / `while` / `for` conditions / ternary `?:` / `match` guards):

| Condition type | Allowed | Meaning |
|---|---|---|
| `bool` | yes | direct boolean test |
| `T?` with `T != bool` | compile error | write `value != null` to test presence explicitly |
| `bool?` | compile error | tri-state ambiguity; write `flag == true` / `flag != null` / `flag ?? false` |
| `i64` / `f64` / `string` / `rune` / collections / objects | compile error | use explicit comparisons such as `n != 0`, `len(s) != 0` |

Operands of `&&` / `||` / `!` must be `bool`; do not place `T?` directly into `&&` / `||`.

```xray @id=types-explicit-conditions
var ok = true
if (ok) { }

var user: User? = findUser()
if (user != null) {      // explicit presence check
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

`Array<T>` is an ordered, growable value type. Assignment, saving a parameter, and returning a value preserve distinct logical copies. Implementations may share element storage, but mutation or append through one writable binding must not change other logical copies. Copying follows value-semantic components and copies only references at class or synchronized identity boundaries; it neither copies nor freezes the referenced objects. See §14.7 for methods and §2.14 for ownership.

```xray @id=types-array
var a: Array<i64> = [1, 2, 3]
var b = a                        // b is a logical copy
b[0] = 9                         // a[0] is still 1
var c: Array<string> = []         // explicit empty array
```

`T` must be determined at compile time and must be copyable and storable. Borrowed views and noncopyable resources cannot be ordinary Array elements. An empty `[]` requires a type context; without one it is a compile error: `Empty array '[]' requires a type annotation`. A non-nullable Array requires explicit initialization; uninitialized storage is not an empty array.

Reading an element produces a logical copy of type `T`. Writes are checked against `T`, using only assignment conversions already allowed for that type. `Array<T>` is invariant: an element conversion does not permit an implicit conversion of the whole array, such as `Array<i8>` to `Array<i64>`. `Array<rune>` preserves `rune` identity for both reads and writes.

The container itself requires only the capabilities needed for storage; it does not require every `T` to support comparison, sorting, hashing, or string conversion. Conditional method requirements must be explicit in the corresponding declaration. Members incidentally present on a concrete instance cannot supply missing constraints at an ordinary generic definition.

This section defines the language contract, not implementation admission for every Array method, aggregate element, or borrowed view in the new XIR source entry. See §17 for current implementation qualification. Unadmitted declaration families must be rejected explicitly.

#### 2.4.1.1 Fixed Arrays `[T; N]`

`[T; N]` is a fixed-layout array type for `N` elements of type `T`. `N` is part of the type and must evaluate during analysis to a positive compile-time integer expression. The current expression subset includes integer literals, `const` integer identifiers, grouping, unary `-`/`~`, and integer arithmetic/bitwise operators. The current backend encoding limit is 65535 elements.

Fixed arrays work as inline struct fields and local variables. They support struct, nested fixed-array, and owned-container element types, so fixed arrays compose recursively:

```xray
var bytes: [u8; 4] = [1, 2, 3, 4]
var zero: [u8; 64] = [0; 64]
var names: [string; 2] = ["a", "b"]
var blocks: [[u8; 2]; 2] = [[1, 2], [3, 4]]
```

A target-typed array literal that initializes `[T; N]` must have the exact length; repeat initialization `[value; N]` uses the same positive compile-time integer expression rule and must also match the target length. A normal array literal without context still infers dynamic `Array<T>`; `[value; N]` without context infers `[T; N]`.

Fixed arrays support `len(array)`, indexed reads, indexed writes to writable places, and `ref` parameters. Borrowed views distinguish shared `Slice<T>` from exclusive `MutSlice<T>` as specified in §2.4.2:

```xray
var data: [u8; 4] = [5, 6, 7, 8]
var view: MutSlice<u8> = data[1:4]
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
- `Array<T>`: a growable value type with dynamic length; element storage may be shared on the heap and detached when needed.
- `Slice<T>` / `MutSlice<T>`: shared / exclusive borrowed views over contiguous storage; they do not own elements (see §2.4.2).

The old `[N]T` syntax is not part of the Xray language.

#### 2.4.2 `Slice<T>` / `MutSlice<T>`

`Slice<T>` is a shared read-only borrow; `MutSlice<T>` is an exclusive writable borrow. They describe contiguous element storage owned by another value and do not own the elements. A borrow remains attached to a proven owner and access permission. These are view contracts, not a claim that the new XIR source entry already admits the view declaration family; see §17 for implementation qualification and validation.

##### Construction and access

Slicing uses an explicit target view type. `array[start:end]` and `fixedArray[start:end]` may form shared views. Forming a `MutSlice<T>` additionally requires an exclusively writable owner place and completion of any necessary COW detachment before publishing the view. The string byte view `str.bytes()` is a `Slice<u8>` and cannot mutate the string.

`len(view)` reads the length and `view[i]` reads an element. Only `MutSlice<T>` permits `view[i] = value`, which updates the owner's logical value. Views have no member methods or `.length`. A view derived from a `const` owner is read-only; the unsettled type-position `const T` spelling must not redefine writable views.

An owner must be a named local place, parameter, or receiver field path with a proven lifetime; a temporary cannot be borrowed. Nested projections protect their root and the necessary access path. Borrowing a class field must retain the object and check exclusive access dynamically. A local proof of no conflict may eliminate that dynamic check.

Assignment of a `MutSlice<T>` transfers its borrow; ordinary calls temporarily reborrow it. Conversion from `MutSlice<T>` to `Slice<T>` is a shared reborrow. The original writable view cannot write while the derived read-only view is live. Subslices obey the same borrow permissions.

```xray @id=types-slice
fn main() {
    var arr: Array<i64> = [10, 20, 30, 40]
    var view: Slice<i64> = arr[1:3]
    print(view[0])                         // 20; last use of view
    var writable: MutSlice<i64> = arr[1:3]
    writable[1] = 31                       // last use of writable
    print(arr[2])                          // 31
    var owned: Array<i64> = copy(arr[1:3]) // materialize an owned copy
    arr.push(50)                           // no live borrow
    print(len(owned))                      // 2
}

main()
```

##### Borrow rules

Let view `v` borrow owner `o`. Borrow liveness ends at last use, not at the end of the lexical block:

1. While a `Slice<T>` is live, `o` must not be mutated, including element writes. The possibility that a write preserves a physical address does not permit it.
2. While a `MutSlice<T>` is live, `o` must not be read, written, or copied through the owner or another path. Only the exclusive view and its valid reborrows may access that storage.
3. Rebinding, destruction, consumption, or other invalidation of a live borrow is rejected (`E0382`). Operation effects must account for COW detachment and relocation; Array element replacement is not unconditionally address-stable.
4. Views are allowed only in locals, parameters, returns, and tuple / Optional wrappers. Wrappers inherit every borrow restriction. Views cannot enter ordinary collections, user aggregates, module storage, or execution boundaries such as `go`, and `as` cannot erase these restrictions (`E0383`).
5. A variable captured by an ordinary closure through a shared cell cannot produce a view. A closure capturing a view may be called immediately. Passing it as a parameter requires an explicit, frozen borrowing-call contract that prevents saving or returning it and guarantees completion before the call ends. Inference from the callee body cannot replace that public contract; a closure literal in argument position does not by itself prove no escape.
6. A borrow in an ordinary call may survive suspension only while the owner remains alive and access permissions hold. Frame address stability is not such a proof. Generators initially must not save borrowed parameters.

```xray
fn rejected() {
    var bytes: Array<u8> = [1, 2]
    var view: Slice<u8> = bytes[:]
    bytes[0] = 3                    // E0382: shared borrow is still live
    print(view[0])
}
```

##### Across functions: return-source contracts

The initial return-source rule uses the signature alone: the entire signature must contain exactly one view input or `ref` input candidate. A `ref` receiver counts as a candidate. Zero or multiple candidates are rejected (`E0384`), even if the body always returns one particular parameter. Neither body inference nor a static-storage exception supplies a cross-module source contract. Returning a view of a local value is also invalid.

Each returned view inside a tuple / Optional attaches to that unique candidate and is checked separately for write permission. A writable result cannot originate from a read-only candidate. Returning two `MutSlice` values additionally requires an explicit non-overlapping construction contract; ordinary user functions cannot establish it merely from index arithmetic.

```xray
fn tail(data: Slice<u8>, start: i64) -> Slice<u8> {
    return data[start:]              // data is the unique candidate
}

fn rejected(a: Slice<u8>, b: Slice<u8>) -> Slice<u8> {
    return a                        // E0384: two signature candidates
}
```

##### Materializing an owned copy

`copy(view)` materializes borrowed data as an `Array<T>` whose elements follow the logical-copy and identity-boundary rules in §2.14. Class elements still reference the same objects. The result no longer borrows the original owner. This use does not decide the public `copy(array)` spelling and does not make it a prerequisite for ordinary Array assignment.

`ref` parameters and `Ptr<T>` / `MutPtr<T>` must also satisfy source, permission, lifetime, and non-escape requirements. The relevant diagnostics are `E0382` (live-borrow conflict), `E0383` (borrow escape), and `E0384` (unstable or non-unique source). `unsafe` relaxes none of them.

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

An integer as operand is typed independently of the target. A direct positive literal (parentheses allowed) above the i64 maximum and at most the u64 maximum retains its u64 source magnitude before reducing to the target width. `18446744073709551615 as i8` is -1; `200 as i8` still wraps explicitly rather than first range-checking the literal as i8.

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
| `x: bool` | no type-narrowing fact | no type-narrowing fact |
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
3. after successful consumption of `x`: the binding becomes unusable (§2.14.5);
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
### 2.14 Values, Identity, and Borrows

This section defines language contracts determined by declared types, parameter modes, and intraprocedural dataflow. Layout, reference counting, and COW are implementation mechanisms and must not change source legality. Built→Checked checks these rules; specialization operates on Checked and is rechecked before Lowered selects concrete copy, detachment, and release operations. See §17 for implementation qualification of complete declaration families. Freezing a specification is not completion of its implementation.

#### 2.14.1 Logical values and identity boundaries

Array / Map / Set / string, copyable structs, enums, tuples, and anonymous records copy by value. A copy is a distinct logical value. Implementations may share immutable storage or use COW, but mutation of value-semantic components through one copy must not change another. An Array returned by a call obeys the same contract; callers need not inspect the callee body to prove storage uniqueness.

Class instances and synchronized objects have identity. A copied reference still names the same object; recursive aggregate copying stops at that boundary. Copying an Array of class elements therefore does not copy the class objects. Object-state mutation may be visible through other references, while replacing an Array element affects only that array's logical value.

`const` fixes the binding and its value-semantic components, not the entire reachable graph. `const xs = [Counter()]` prevents replacing or appending array elements through `xs` but does not freeze Counter's identity state. Object methods and synchronized APIs retain their own contracts. Sharing across execution boundaries depends on the complete type's Sendable property; neither `const` nor atomic reference counting proves it.

#### 2.14.2 Initialization, permissions, and borrow facts

At each program point the following must be proved separately:

| Fact | Required condition |
|--|--|
| Initialization and consumption state | A read requires complete initialization; operations needing ownership cannot use a consumed or possibly consumed value |
| Place permission | A write requires a writable destination; READ parameters and the value-semantic components of `const` cannot be written directly |
| Value and declaration contract | Type, copy or consumption capability, parameter mode, storage, and return permissions satisfy the declaration |
| Active borrows | Access does not conflict with a live shared or exclusive borrow, whose source and lifetime remain valid |

Initialization and consumption are control-flow facts, permissions come from bindings and declarations, and borrows protect specific places and necessary paths. These proofs cannot substitute for each other. Missing a required proof is an error, but uniqueness of an Array backing store is not a language admission condition for ordinary assignment, saving a copy, returning, or mutation. Non-nullable Arrays still require explicit initialization (§5.1.1).

#### 2.14.3 Ordinary copies and storage sharing

`var b = a`, saving a READ parameter, returning an Array, and storing a copyable Array in another permitted container or aggregate preserve logical copies. They do not create a language-level alias to the source array's mutable storage, do not permanently mark its binding `ESCAPED`, and do not require other copies to die before the source can be mutated. Backing-store uniqueness may only optimize away detachment or copying.

```xray
var a = [1, 2, 3]
var b = a
const snapshot = a
b[0] = 9
a.push(a[0])                 // evaluate arguments before exclusive receiver access
print(snapshot[0])          // 1
print(len(snapshot))        // 3
print(b[0])                 // 9
print(len(a))               // 4
```

A copy must acquire independently releasable result ownership; failure must not publish a partially initialized result. Self-assignment and reading an element before replacing or appending it must preserve this responsibility, without double release or access to freed storage caused by physical sharing. Evaluation follows §3.0 E2/E5. Copy or automatic-move optimizations must not change the established order of errors, destruction, or external side effects.

#### 2.14.4 Place borrows and captures

`ref` is an exclusive writable place borrow. E2 evaluates the receiver and all arguments before establishing the borrow and entering the callee. Conflicting access through another path is forbidden while it is live. Assignment still follows E5: location subexpressions, index, right-hand side, then storage. COW implementation does not change that order.

Shared `Slice` and exclusive `MutSlice` borrows follow §2.4.2. Class-field borrows additionally retain the object and check dynamic exclusivity. Sharing a backing store cannot remove these checks. Conversely, ordinary Array logical copies do not become borrows merely because they share backing storage.

Ordinary closures capture outer `var` bindings through shared cells; `const` and ordinary READ parameters of copyable non-view types are captured by value. Shared-cell identity must not be disguised as a temporary non-escaping loan; its storage and release responsibilities remain explicit. Variables captured through shared cells cannot produce views. View captures and borrowing callbacks require the explicit contracts in §2.4.2, not guesses from a callee body or argument syntax.

#### 2.14.5 Consumption and unadmitted surfaces

Consumption of a noncopyable resource transfers ownership and final destruction responsibility. Whether a call consumes is determined by the declared parameter or receiver mode; `move x` optionally marks such a transfer explicitly. Successful consumption makes the source binding unusable. Consuming it again in a loop without reinitialization on every relevant path must be rejected. READ and `ref` borrows do not consume ownership. A rejected operation must not incorrectly mark a valid source as consumed.

Copyable types, including Array, cannot declare `move` parameters or receivers. Whether a standalone `move x` expression is allowed for copyable values, whether ordinary `copy(array)` retains a public spelling, and the identity and conversions of type-position `const T` are outside this frozen subset and require separate decisions before admission. These open surfaces do not block ordinary Array copying. Old unique-root, terminal heap-escape, or whole-graph deep-freeze rules must not supply default answers.

#### 2.14.6 Struct-field copying and initialization

A copyable struct copies fields according to their declared types. Array / Map / Set and other container fields preserve logical copies, string retains immutable value semantics, and class or synchronized fields copy identity references only. A struct must not be rejected merely because it contains an Array, another owned container, or a class reference. Nor may a struct with class fields be described as a wholly independent reachable graph.

Fields still satisfy ordinary storage, initialization, and copyability requirements. Views cannot enter ordinary user aggregates. A noncopyable field or payload requires its containing struct / enum to be noncopyable, with that declaration family admitted separately. A non-nullable Array field cannot be unconditionally zero-initialized; default construction must obey §5's field rules.

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

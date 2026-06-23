---
id: spec.2_type_system
order: 003
---

<!-- xr-spec:cn -->
---

## 2. 类型系统 (Type System)

> 真值源：`src/runtime/value/xtype.h`（XrType 定义）、`src/runtime/value/xtype.c`、`src/frontend/parser/xparse_type.c`（语法）、`src/frontend/analyzer/xtype_ref_resolve.c`（解析）、`stdlib/prelude/prelude_types.def`（内置类型表）。

### 2.1 概述

Xray 是静态类型语言；每个表达式在编译期有确定类型。类型系统的核心特性：

1. **类型推断**：变量声明几乎不用写类型；分析器从初始值/上下文推导。
2. **Nullable 分离**：`T` 永不为 `null`；`T?` 是 `T | null` 的语法糖。
3. **Union 类型**：`A | B | ...`（最多 6 个成员）。
4. **Generic reified**：泛型类型参数运行时可反射。
5. **Structural Json + Nominal class**：Json 对象按字段结构兼容（duck typing），class 按名义兼容。
6. **运行时反射**：`typeof` / `Reflect.*` API。

### 2.2 类型分类

| 类别 | 示例 |
|--|--|
| Primitive | `int`、`float`、`bool`、`string`、`()`（Unit，无返回值） |
| 精确整数 | `int8`、`int16`、`int32`、`int64`、`uint8`..`uint64` |
| 精确浮点 | `float32`、`float64` |
| 容器 | `Array<T>`、`Map<K,V>`、`Set<T>`、`Channel<T>`、`Bytes`（即 `Array<uint8>`） |
| 特殊 | `Json`、`BigInt`、`Range`、`DateTime`、`Regex`、`StringBuilder`、`Logger`、`NetConn`、`NetListener` |
| 错误处理 prelude | `Exception`（见 §8） |
| 弱引用容器 | `WeakMap`、`WeakSet` |
| Nullable | `T?` |
| Union | `A \| B \| ...` |
| Tuple | `(T1, T2, ...)` |
| Function | `fn(T1, T2) -> R` |
| FFI / C ABI | `RawPtr<T>`、`RawMut<T>`、`CFn<(T) -> R>`、`uintsize`、`intsize` |
| Class / Struct / Interface | 用户定义（nominal） |
| Enum | 用户定义（含 ADT enum，见 §5.6） |
| Type alias | `type Name = SomeType`、`type Name<T> = SomeType` |

### 2.3 基本类型

#### 2.3.1 整数类型

| 类型 | 范围 | 别名 |
|--|--|--|
| `int8` | `[-128, 127]` | — |
| `int16` | `[-32768, 32767]` | — |
| `int32` | `[-2³¹, 2³¹-1]` | — |
| `int64` | `[-2⁶³, 2⁶³-1]` | `int`（默认整数类型）|
| `uint8`..`uint64` | 无符号对应 | — |

- 字面量默认 `int`；可被上下文窄化（如赋给 `int32` 变量），但直接字面量必须落在目标范围内（`let x: int8 = 200` 编译拒绝）。
- 算术：二补码环绕语义（wrap on overflow），不区分 debug / release 构建。同宽窄整数运算保留该宽度并按该宽度环绕（`uint8 + uint8 -> uint8`）；异宽窄整数运算塌回 `int`；移位运算结果取左操作数宽度。
- 静态类型为 `uint8`..`uint64` 的值在 `print`、`string(x)`、模板字符串、字符串拼接和顺序比较中按无符号解释；例如静态 `uint64` 的位型 `0xffff_ffff_ffff_ffff` 显示为 `18446744073709551615`，且大于 `0`。
- `int` 的 `checkedAdd` / `checkedSub` / `checkedMul` 在溢出时返回 `null`；`saturating*` 饱和到 `int` 边界；`wrapping*` 显式执行默认二补码环绕。
- 非字面量表达式写入窄整数目标时按目标类型窄化并环绕，例如 `let x: uint8 = 255 + 1` 得到 `0`。
- 动态擦除后的 `XrValue` 只保存整数 payload，不保存有符号性或位宽；跨过 `any` / Json / 动态容器等边界后，超过 `int64` 正范围的 `uint64` 值在格式化和顺序比较中的行为不保证保留无符号语义。需要无符号语义时保持静态 `uintN` 类型。

#### 2.3.2 浮点类型

| 类型 | 标准 |
|--|--|
| `float32` | IEEE-754 单精度 |
| `float64` | IEEE-754 双精度；`float` 的别名 |

字面量默认 `float`。

#### 2.3.3 `bool`

`true` / `false`，独立类型，与数值类型**不可隐式互转**（不能 `let x: int = true`，也不能 `let b: bool = 1`）。

**条件表达式规则**（`if` / `while` / `for` 条件 / 三元 `?:` / `match` 守卫）：

| 条件类型 | 是否允许 | 语义 |
|---|---|---|
| `bool` | 允许 | 直接布尔判断 |
| `T?` 且 `T != bool` | 允许 | 仅判断是否为 `null`（不检查内容是否“空”） |
| `bool?` | 编译错误 | 三态歧义；写 `flag == true` / `flag != null` / `flag ?? false` |
| `int` / `float` / `string` / 集合 / 对象 | 编译错误 | 必须写显式比较，如 `n != 0`、`!s.isEmpty()` |

`&&` / `||` / `!` 的操作数必须是 `bool`；不要把 `T?` 直接放进 `&&` / `||`。

```xray @id=types-explicit-conditions
let ok = true
if (ok) { }

let user: User? = findUser()
if (user) {              // 存在性：仅检查 null
    print(user.name)     // 此分支 user 窄化为 User
}

let flag: bool? = maybeFlag()
if (flag == true) { }    // OK
if (flag != null) { }    // OK
// if (flag) { }         // 编译错误：裸 bool? 不能作条件

let s = ""
if (!s.isEmpty()) { }    // OK
// if (s) { }            // 编译错误
```

#### 2.3.4 `string`

不可变 UTF-8 字符串。支持 `length`、索引、切片、丰富方法集（见 §14.2）。

底层使用引用计数（ARC）+ 字符串驻留（interning）优化。

#### 2.3.5 Unit `()`（无返回值）

xray 用 **0-元组 `()`** 表示"无返回值"（Unit 类型）：

```xray
fn log(msg: string) -> () { print(msg) }   // 显式 Unit 返回
fn ping() { print("pong") }                  // 省略返回类型 = ()
let r: () = log("hi")                        // 允许；r 是 Unit 值
```

- 一个函数省略返回类型等同于 `-> ()`。
- `void` 不是类型名：写 `fn f() -> void` 会被拒绝（`E0804`）；无返回值使用 `-> ()` 或省略返回类型。

#### 2.3.6 FFI 标量与 C ABI 边界类型

xray 的 C FFI 使用一组显式边界类型，避免把普通 xray 对象隐式解释成 C 数据：

| 类型 | C ABI 含义 | 备注 |
|--|--|--|
| `uintsize` | `size_t` | 当前支持目标上为 `uint64` |
| `intsize` | `ptrdiff_t` / 平台有符号宽度 | 当前支持目标上为 `int64` |
| `RawPtr<T>` | `const void *` 边界值 | 只读裸指针；`T` 用于 xray 端解引用/索引宽度 |
| `RawMut<T>` | `void *` 边界值 | 可写裸指针；可传给需要 `RawPtr<T>` 的位置 |
| `CFn<(A, B) -> R>` | C ABI 函数指针 | 用于把 xray 函数作为 C 回调传入 `@extern` 函数 |

裸指针值可以安全地保存、传递、比较和用 `offset(i)` 做按元素宽度缩放的指针偏移；真正读写外部内存必须写在 `unsafe { }` 内：

```xray
@extern("C") fn malloc(n: uintsize) -> RawMut<uint8>
@extern("C") fn free(p: RawMut<uint8>)

let p = unsafe { malloc(4) }
unsafe {
    p[0] = 42
    print(p.deref())
    free(p)
}
```

`RawPtr<T>` 只能读取，写入必须使用 `RawMut<T>`；`unsafe` 不会绕过这个类型规则。裸指针访问不做空指针或边界检查，调用方必须保证地址、生命周期、对齐和别名规则正确。

`CFn<(...) -> ...>` 不是普通 xray 闭包类型。当前 VM/AOT 后端支持把模块级、非捕获、签名精确匹配的 xray 函数传给 C；捕获闭包、匿名函数和 `@extern` 函数本身不能作为 `CFn` 回调实参。

### 2.4 复合类型

#### 2.4.1 `Array<T>`

有序可变数组。详见 §14.1。

```xray @id=types-array
let a: Array<int> = [1, 2, 3]
let b = [1, 2, 3]                // 推断为 Array<int>
let c: Array<string> = []         // 显式空数组
```

`Array<T>` 的 `T` 必须能在编译期确定。空 `[]` 在无类型标注时是编译错误：`Empty array '[]' requires a type annotation`。

#### 2.4.2 `Map<K, V>`

哈希字典，**保持插入顺序**。详见 §14.7。

**Map 字面量**必须用 `#{ ... }` 前缀，分隔符用 `:`（与 Json 一致，靠 `#` 前缀消歧）：

```xray @id=types-map
let m: Map<string, int> = #{"a": 1, "b": 2}
let m2 = #{"a": 1, "b": 2}
let empty = #{}                                     // 空 Map

m["c"] = 3                                          // 添加/修改
let v = m["a"]                                      // 取值；不存在返回 null
```

| 字面量形式 | 类型 | 用途 |
|---|---|---|
| `{ key: value }`（无前缀） | `Json` / `Object`（结构化） | 见 §2.4.6 |
| `#{ "k": v }`（`#` 前缀 + `:`） | `Map<K, V>`（哈希字典） | 本节 |
| `#{}` | `Map<K, V>`（空） | 显式空 Map |
| `[]` | `Array<T>` | 数组 |
| `#[]` | `Set<T>` | 集合 |

`K` 必须满足 `Hashable`（详见 §9.2）：通常是 `int`、`float`、`string`、`bool`、`enum`、`BigInt`，或提供 `operator==(other: Self) -> bool` 与 `hash() -> int` 的自定义类型。泛型键类型必须显式写成 `K: Hashable`。

#### 2.4.3 `Set<T>`

去重集合。详见 §14.4。

```xray @id=types-set
let s: Set<int> = #[1, 2, 3]
```

#### 2.4.4 `Channel<T>`

协程间通信通道。**必须**用 `const` 声明（见 §10.5）。

```xray @id=types-channel
const ch: Channel<int> = new Channel<int>(10)
```

#### 2.4.5 `Bytes`

类型化字节缓冲。语义等价 `Array<uint8>`，但底层是连续内存。

```xray
let buf = new Bytes(1024)
let init = new Bytes([72, 101, 108, 108, 111])
```

#### 2.4.6 `Json` 与对象字面量

`Json` 是 xray 的**动态结构化数据类型**——可以装载 JSON 等价的任意结构。详见 §14.10 与 §2.10。

**对象字面量** `{ field: value, ... }` 与 Map 字面量的关键区别：

```xray @id=types-json-object
// Object/Json 字面量：标识符或字符串 key + 冒号 ':'
let data: Json = { name: "Alice", tags: ["a", "b"], age: 30 }
let user = { name: "Bob", age: 25 }       // 默认类型为 Json
data.name              // 类型: Json（字段访问返回 Json）
data["name"]           // 等价

// 字段简写：当字段名与变量名相同
let name = "Alice"
let age = 30
let user = { name, age }                  // 等价 { name: name, age: age }

// Map 字面量：`#{}` 前缀 + `:`
let m = #{"k1": 1, "k2": 2}           // 类型: Map<string, int>
```

**对照表**：

| 写法 | 类型 | 备注 |
|---|---|---|
| `{ name: "x", age: 1 }` | `Json` / `Object` | 标识符或字符串 key 后跟 `:` |
| `{ x: y }`（`x` 是字段名，`y` 是变量名） | `Json` / `Object` | 字段简写 `{ x }` 等价 `{ x: x }`，仅裸 key |
| `#{"a": 1}` | `Map<K, V>` | `#` 前缀消歧，分隔符用 `:` |
| `Point{x: 1.0, y: 2.0}` | `Point`（struct） | 类型名 + `{...}` 字面量 |

**密封（sealed）对象类型**：通过 `type` 别名为对象类型起名后，类型成为 sealed——访问/赋值未声明字段是编译错误：

```xray
type User = { name: string, age: int }

let u: User = { name: "Alice", age: 30 }
print(u.name)         // OK
// u.extra = "x"      // 编译错误：sealed type User has no field 'extra'

// 不指定类型则为动态 Json
let u2 = { name: "Alice", age: 30 }      // Json（可动态扩展）
u2.extra = "x"        // OK（Json 是动态的）
```

#### 2.4.7 `BigInt`

任意精度整数。见 §14.8。

#### 2.4.8 `Range`

由 `..` 运算符产生。见 §3.12。

#### 2.4.9 `DateTime` / `Regex` / `StringBuilder`

详见 §14。

#### 2.4.10 `WeakMap` / `WeakSet`

`WeakMap` 的键、`WeakSet` 的元素必须是堆对象；弱引用不阻止 GC 回收。弱集合不提供会长期持有元素的遍历回调。

### 2.5 可空类型

`T?` 是 `T | null` 的语法糖。

```xray @id=types-nullable
let x: int? = null      // OK
let y: int? = 42        // OK
let z: int = null       // 编译错误：null 不是 int
```

**可空原始类型一等公民**：`int?` / `float?` / `bool?` 与其它 `T?` 一样是合法类型，泛型与容器会自然产生它们（如 `Map<string, bool>.get(k) -> bool?`、`fn find<T>(...) -> T?` 在 `T = bool` 时）。它们以 tagged 表示承载 `null`，因此 `null` 值在 `print` / `string()` / 字符串拼接中统一显示为 `"null"`（不是底层数值 `0`），VM 与 AOT 一致。

> `bool?` 是三态（`true` / `false` / `null`）。它合法，但**不能直接作条件**（裸 `if (b)` where `b: bool?` 是编译错误，见 §5 / 任务 128）；需显式写 `b == true` / `b != null` / `b ?? false`。

#### 解包

```xray
// 1. 空合并
let v = x ?? 0

// 2. 可选链
let len = name?.length    // 若 name 为 null，结果为 null

// 3. 强制解包
let v: int = x!           // 若 x 为 null，运行时抛 NullError

// 4. is 检查
if (x is int) {
    // 此分支内 x 类型窄化为 int
    print(x + 1)
}
```

### 2.6 Union 类型

```xray @id=types-union-basic
let v: int | string = 42
v = "hello"             // OK
```

约束：
- 最多 **6 个成员**（编译期检查；超限 → 错误）。
- 成员互不为彼此的子类型（否则会被规范化）。
- 处理 union 值需用 `match` 或 `is` 窄化：

```xray
let v: int | string = ...
match v {
    is int    -> print("int: ${v}"),
    is string -> print("str: ${v}"),
}
```

**特殊化**：
- `int | null` 规范化为 `int?`。
- `T?` 出现在 union 时：`int? | string` 实际等价 `int | string | null`，规范化为 `(int | string)?`。

### 2.7 元组类型

xray 的元组**是头等公民**——可以作为任意值出现、作为字段保存、嵌套。

```xray @id=types-tuple
// 字面量
let t = (1, 2, 3)                 // 类型推断为 (int, int, int)
let h = (10, "hi", true)          // 异构元组
let single = (99,)                // 单元素元组：注意尾逗号

// 类型注解
let p: (int, string) = (7, "ok")

// 字段访问：.N（N 是编译期常量整数下标）
let first = t.0                   // 1
let mid   = t.1                   // 2
let nest  = ((1, 2), (3, 4))
let a     = nest.0.0              // 1
let b     = nest.1.1              // 4

// 函数返回与解构
fn divmod(a: int, b: int) -> (int, int) { return (a / b, a % b) }
let (q, r) = divmod(17, 5)        // tuple destructure

// 泛型
fn pair<A, B>(a: A, b: B) -> (A, B) { return (a, b) }
let p2 = pair(1, "x")             // (int, string)
```

**注意事项**：

- **单元素元组**必须用尾逗号 `(x,)`——不带逗号的 `(x)` 是分组括号（普通表达式）。
- 字段访问 `t.N` 中 N **必须是字面量整数**；用变量或字符串访问是编译错误 `XR_ERR_ANALYZE_TUPLE_FIELD_NAME` / `_RANGE`。
- 元组**不可变**：`t.0 = v` 是编译错误。修改必须重新构造。

### 2.8 类型别名

```xray @id=types-alias
type Result = int | string
type Mapper = (int) -> int
type Point = { x: float, y: float }
type Pair<T> = { first: T, second: T }
type Mapper2<T, U> = (T) -> U
```

别名是**纯语法**等价，不产生新类型，也不产生运行时元数据或 AOT 分支。泛型别名在使用处按类型实参做语法代入：

```xray
let p: Pair<int> = { first: 1, second: 2 }  // 等价于 { first: int, second: int }
let f: Mapper2<int, string> = (n) -> string(n)
```

泛型别名形参只允许名字列表（`<T, U>`）；不带约束。需要约束时应放在使用该别名的泛型函数、class / struct / enum / interface 声明上。别名可前向引用，但循环别名（包括递归对象别名）是编译错误。

### 2.9 类型推断

详见 §7.4。简述：

```xray @id=types-inference
let x = 1               // x: int
let y = 1.5             // y: float
let z = "hello"         // z: string
let a = [1, 2, 3]       // a: Array<int>
let m = #{"a": 1}    // m: Map<string, int>
let p = { name: "A" }   // p: { name: string } —— 结构化对象类型
let f = (x: int) -> x   // f: (int) -> int —— 箭头参数必须标注
```

### 2.10 类型兼容性与转换

#### 2.10.1 隐式转换

| 源 | 目标 | 允许 |
|--|--|--|
| `int` | `float` | ✅ |
| `int8` | `int` (= `int64`) | ✅ |
| `T` | `T?` | ✅ |
| `T` | `Json`（如果 T 是 Json 兼容） | ✅ |
| `null` | `T?` | ✅ |
| Subtype | Supertype（class）| ✅ |
| 子集对象类型 | 超集对象类型 | ❌（结构化兼容是 superset → subset） |

> **结构化兼容方向**（duck typing）：字段更多的类型可赋给字段更少的类型。
> ```xray
> type User = { name: string }
> let full = { name: "A", age: 18 }
> let u: User = full       // OK：full 是 User 的超集
> ```

#### 2.10.2 显式 `as`

```xray @id=types-cast
let n = x as int        // 失败抛 TypeError
let n = x as int?       // 失败返回 null（安全转换）
```

适用于：
- 数值之间（含 `Json → int`，运行时检查）。
- `Json → User`（结构化 narrowing）。
- 父类 → 子类（向下转）。

#### 2.10.3 `is` 检查

```xray
if (v is User) {
    // 编译器在此分支窄化 v 的类型为 User
}
```

仅作类型守卫；不改变值。

### 2.11 typeof / typename / Type 枚举

```xray
typeof(value)     // 返回 Type 枚举值（int 表示）
typename(value)   // 返回类型名字符串
```

`Type` 枚举成员：

`Type.int`、`Type.float`、`Type.string`、`Type.bool`、`Type.null`、
`Type.Array`、`Type.Map`、`Type.Set`、`Type.Channel`、`Type.Json`、
`Type.function`、`Type.class`、`Type.struct`、`Type.enum`、`Type.module`、`Type.bigint`、...

完整列表见 `stdlib/types/enum.xr` / `src/runtime/value/xtype.h`。

### 2.12 运行时反射

`Reflect` 模块（内置）：

```xray
Reflect.getType(obj)        // 获取类型信息（Json）
Reflect.typeOf(obj)         // 获取类型名（string）
Reflect.isInstance(obj, cls)// 是否某类实例
Reflect.fieldCount(obj)     // 字段数量
Reflect.getAllTypes()       // 所有已注册类型
```

详见 §13 与 §14。
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 2. Type System

> Source of truth: `src/runtime/value/xtype.h` (`XrType` definition), `src/runtime/value/xtype.c`, `src/frontend/parser/xparse_type.c` (syntax), `src/frontend/analyzer/xtype_ref_resolve.c` (resolution), `stdlib/prelude/prelude_types.def` (built-in type table).

### 2.1 Overview

Xray is statically typed; every expression has a determined type at compile time. Core features of the type system:

1. **Type inference**: variable declarations rarely require type annotations; the analyzer infers from the initializer / context.
2. **Nullable separation**: `T` is never `null`; `T?` is sugar for `T | null`.
3. **Union types**: `A | B | ...` (up to 6 members).
4. **Reified generics**: generic type parameters are reflectable at runtime.
5. **Structural Json + Nominal class**: Json objects are field-structure compatible (duck typing); classes are nominally compatible.
6. **Runtime reflection**: `typeof` / `Reflect.*` APIs.

### 2.2 Type Categories

| Category | Examples |
|--|--|
| Primitive | `int`, `float`, `bool`, `string`, `()` (Unit, no return value) |
| Sized integers | `int8`, `int16`, `int32`, `int64`, `uint8`..`uint64` |
| Sized floats | `float32`, `float64` |
| Containers | `Array<T>`, `Map<K,V>`, `Set<T>`, `Channel<T>`, `Bytes` (equivalent to `Array<uint8>`) |
| Special | `Json`, `BigInt`, `Range`, `DateTime`, `Regex`, `StringBuilder`, `Logger`, `NetConn`, `NetListener` |
| Error-handling prelude | `Exception` (see §8) |
| Weak containers | `WeakMap`, `WeakSet` |
| Nullable | `T?` |
| Union | `A \| B \| ...` |
| Tuple | `(T1, T2, ...)` |
| Function | `fn(T1, T2) -> R` |
| FFI / C ABI | `RawPtr<T>`, `RawMut<T>`, `CFn<(T) -> R>`, `uintsize`, `intsize` |
| Class / Struct / Interface | user-defined (nominal) |
| Enum | user-defined (incl. ADT enum, see §5.6) |
| Type alias | `type Name = SomeType`, `type Name<T> = SomeType` |

### 2.3 Primitive Types

#### 2.3.1 Integer Types

| Type | Range | Alias |
|--|--|--|
| `int8` | `[-128, 127]` | — |
| `int16` | `[-32768, 32767]` | — |
| `int32` | `[-2³¹, 2³¹-1]` | — |
| `int64` | `[-2⁶³, 2⁶³-1]` | `int` (default integer type) |
| `uint8`..`uint64` | unsigned counterparts | — |

- Literals default to `int`; the type may be narrowed by context (e.g., assigned to an `int32` variable), but a direct literal must fit the target range (`let x: int8 = 200` is rejected at compile time).
- Arithmetic uses two's-complement wrap-around semantics (no debug/release distinction). Same-width narrow integer operations keep that width and wrap at that width (`uint8 + uint8 -> uint8`); mixed narrow widths collapse back to `int`; shift results take the left operand's width.
- Values with static type `uint8`..`uint64` are interpreted as unsigned by `print`, `string(x)`, template strings, string concatenation, and ordering comparisons; for example, a static `uint64` bit pattern of `0xffff_ffff_ffff_ffff` formats as `18446744073709551615` and compares greater than `0`.
- `int.checkedAdd` / `checkedSub` / `checkedMul` return `null` on overflow; `saturating*` clamps to the `int` boundary; `wrapping*` explicitly performs the default two's-complement wrap.
- Non-literal expressions written into narrow integer targets are narrowed with target-type wrap-around, so `let x: uint8 = 255 + 1` evaluates to `0`.
- After dynamic erasure, `XrValue` stores only the integer payload, not signedness or width. Across `any` / Json / dynamic-container boundaries, `uint64` values above the positive `int64` range are not guaranteed to keep unsigned formatting or ordering semantics. Keep the value statically typed as `uintN` when unsigned semantics are required.

#### 2.3.2 Floating-Point Types

| Type | Standard |
|--|--|
| `float32` | IEEE-754 single precision |
| `float64` | IEEE-754 double precision; alias of `float` |

Literals default to `float`.

#### 2.3.3 `bool`

`true` / `false`, a standalone type. **No implicit conversion** to/from numeric types (cannot write `let x: int = true` or `let b: bool = 1`).

**Condition expression rules** (`if` / `while` / `for` conditions / ternary `?:` / `match` guards):

| Condition type | Allowed | Meaning |
|---|---|---|
| `bool` | yes | direct boolean test |
| `T?` with `T != bool` | yes | null presence only (content emptiness is **not** checked) |
| `bool?` | compile error | tri-state ambiguity; write `flag == true` / `flag != null` / `flag ?? false` |
| `int` / `float` / `string` / collections / objects | compile error | use explicit comparisons such as `n != 0`, `!s.isEmpty()` |

Operands of `&&` / `||` / `!` must be `bool`; do not place `T?` directly into `&&` / `||`.

```xray @id=types-explicit-conditions
let ok = true
if (ok) { }

let user: User? = findUser()
if (user) {              // presence: null check only
    print(user.name)     // user is narrowed to User here
}

let flag: bool? = maybeFlag()
if (flag == true) { }    // OK
if (flag != null) { }    // OK
// if (flag) { }         // compile error: bare bool? cannot be a condition

let s = ""
if (!s.isEmpty()) { }    // OK
// if (s) { }            // compile error
```

#### 2.3.4 `string`

Immutable UTF-8 strings. Supports `length`, indexing, slicing, and a rich method set (see §14.2).

Internally uses ARC + string interning optimizations.

#### 2.3.5 Unit `()` (no return value)

Xray uses the **0-tuple `()`** to represent "no return value" (the Unit type):

```xray
fn log(msg: string) -> () { print(msg) }   // explicit Unit return
fn ping() { print("pong") }                  // omitted return type = ()
let r: () = log("hi")                        // allowed; r is a Unit value
```

- A function omitting its return type is equivalent to `-> ()`.
- `void` is not a type name: `fn f() -> void` is rejected (`E0804`); use `-> ()` or omit the return type to indicate no return value.

#### 2.3.6 FFI Scalars and C ABI Boundary Types

Xray's C FFI uses explicit boundary types so ordinary xray objects are not implicitly interpreted as C data:

| Type | C ABI meaning | Notes |
|--|--|--|
| `uintsize` | `size_t` | `uint64` on the currently supported targets |
| `intsize` | `ptrdiff_t` / platform signed width | `int64` on the currently supported targets |
| `RawPtr<T>` | `const void *` boundary value | read-only raw pointer; `T` gives the xray-side dereference/index width |
| `RawMut<T>` | `void *` boundary value | mutable raw pointer; assignable where `RawPtr<T>` is expected |
| `CFn<(A, B) -> R>` | C ABI function pointer | passes an xray function as a C callback argument to an `@extern` function |

Raw pointer values may be stored, passed, compared, and offset with `offset(i)` using element-width scaling in safe code; actually reading or writing foreign memory must be inside `unsafe { }`:

```xray
@extern("C") fn malloc(n: uintsize) -> RawMut<uint8>
@extern("C") fn free(p: RawMut<uint8>)

let p = unsafe { malloc(4) }
unsafe {
    p[0] = 42
    print(p.deref())
    free(p)
}
```

`RawPtr<T>` is read-only; writes require `RawMut<T>`. `unsafe` does not bypass that type rule. Raw pointer access performs no null or bounds checks, so the caller must guarantee address validity, lifetime, alignment, and aliasing correctness.

`CFn<(...) -> ...>` is not an ordinary xray closure type. The current VM/AOT backends support passing module-level, noncapturing xray functions with an exact signature match to C; capturing closures, anonymous functions, and `@extern` functions themselves cannot be used as `CFn` callback arguments.

### 2.4 Composite Types

#### 2.4.1 `Array<T>`

Ordered mutable array. See §14.1.

```xray @id=types-array
let a: Array<int> = [1, 2, 3]
let b = [1, 2, 3]                // inferred as Array<int>
let c: Array<string> = []         // explicit empty array
```

The `T` in `Array<T>` must be determinable at compile time. An empty `[]` without a type annotation is a compile error: `Empty array '[]' requires a type annotation`.

#### 2.4.2 `Map<K, V>`

Hash table that **preserves insertion order**. See §14.7.

**Map literals** must use the `#{ ... }` prefix with `:` separators (consistent with Json; disambiguated by the `#` prefix):

```xray @id=types-map
let m: Map<string, int> = #{"a": 1, "b": 2}
let m2 = #{"a": 1, "b": 2}
let empty = #{}                                     // empty Map

m["c"] = 3                                          // insert / update
let v = m["a"]                                      // lookup; returns null if absent
```

| Literal form | Type | Purpose |
|---|---|---|
| `{ key: value }` (no prefix) | `Json` / `Object` (structural) | see §2.4.6 |
| `#{ "k": v }` (`#` prefix + `:`) | `Map<K, V>` (hash table) | this section |
| `#{}` | `Map<K, V>` (empty) | explicit empty Map |
| `[]` | `Array<T>` | array |
| `#[]` | `Set<T>` | set |

`K` must satisfy `Hashable` (see §9.2): typically `int`, `float`, `string`, `bool`, `enum`, `BigInt`, or a custom type that provides `operator==(other: Self) -> bool` and `hash() -> int`. Generic key types must be explicitly constrained as `K: Hashable`.

#### 2.4.3 `Set<T>`

Deduplicated collection. See §14.4.

```xray @id=types-set
let s: Set<int> = #[1, 2, 3]
```

#### 2.4.4 `Channel<T>`

Inter-coroutine communication channel. **Must** be declared `const` (see §10.5).

```xray @id=types-channel
const ch: Channel<int> = new Channel<int>(10)
```

#### 2.4.5 `Bytes`

Typed byte buffer. Semantically equivalent to `Array<uint8>`, but stored as contiguous memory.

```xray
let buf = new Bytes(1024)
let init = new Bytes([72, 101, 108, 108, 111])
```

#### 2.4.6 `Json` and Object Literals

`Json` is xray's **dynamic structured data type** — it can hold any JSON-equivalent structure. See §14.10 and §2.10.

The key difference between an **object literal** `{ field: value, ... }` and a Map literal:

```xray @id=types-json-object
// Object/Json literal: identifier or string key + colon ':'
let data: Json = { name: "Alice", tags: ["a", "b"], age: 30 }
let user = { name: "Bob", age: 25 }       // default type is Json
data.name              // type: Json (field access returns Json)
data["name"]           // equivalent

// Field shorthand: when a field name matches a variable name
let name = "Alice"
let age = 30
let user = { name, age }                  // equivalent to { name: name, age: age }

// Map literal: `#{}` prefix + `:`
let m = #{"k1": 1, "k2": 2}           // type: Map<string, int>
```

**Comparison**:

| Form | Type | Notes |
|---|---|---|
| `{ name: "x", age: 1 }` | `Json` / `Object` | identifier or string key followed by `:` |
| `{ x: y }` (`x` is field name, `y` is variable) | `Json` / `Object` | shorthand `{ x }` equivalent to `{ x: x }`; bare key only |
| `#{"a": 1}` | `Map<K, V>` | `#` prefix disambiguates; separator `:` |
| `Point{x: 1.0, y: 2.0}` | `Point` (struct) | type name + `{...}` literal |

**Sealed object types**: once an object type is named via `type`, it becomes sealed — accessing or assigning an undeclared field is a compile error:

```xray
type User = { name: string, age: int }

let u: User = { name: "Alice", age: 30 }
print(u.name)         // OK
// u.extra = "x"      // compile error: sealed type User has no field 'extra'

// Without a type annotation, the literal is dynamic Json
let u2 = { name: "Alice", age: 30 }      // Json (dynamically extensible)
u2.extra = "x"        // OK (Json is dynamic)
```

#### 2.4.7 `BigInt`

Arbitrary-precision integer. See §14.8.

#### 2.4.8 `Range`

Produced by the `..` operator. See §3.12.

#### 2.4.9 `DateTime` / `Regex` / `StringBuilder`

See §14 for details.

#### 2.4.10 `WeakMap` / `WeakSet`

Keys of `WeakMap` and elements of `WeakSet` must be heap objects; weak references do not prevent GC reclamation. Weak collections do not provide long-lived traversal callbacks that would retain elements.

### 2.5 Nullable Types

`T?` is sugar for `T | null`.

```xray @id=types-nullable
let x: int? = null      // OK
let y: int? = 42        // OK
let z: int = null       // compile error: null is not int
```

**Nullable primitives are first-class**: `int?` / `float?` / `bool?` are ordinary `T?` types and arise naturally from generics and containers (e.g. `Map<string, bool>.get(k) -> bool?`, or `fn find<T>(...) -> T?` at `T = bool`). They carry `null` in the tagged representation, so a `null` value renders as `"null"` in `print` / `string()` / string concatenation (never as the raw payload `0`), identically in the VM and AOT.

> `bool?` is tri-state (`true` / `false` / `null`). It is legal but **cannot be used directly as a condition** (a bare `if (b)` where `b: bool?` is a compile error; see §5 / task 128); write `b == true` / `b != null` / `b ?? false`.

#### Unwrapping

```xray
// 1. Null coalescing
let v = x ?? 0

// 2. Optional chaining
let len = name?.length    // null if name is null

// 3. Force unwrap
let v: int = x!           // throws NullError at runtime if x is null

// 4. `is` check
if (x is int) {
    // In this branch x is narrowed to int
    print(x + 1)
}
```

### 2.6 Union Types

```xray @id=types-union-basic
let v: int | string = 42
v = "hello"             // OK
```

Constraints:
- Up to **6 members** (checked at compile time; over the limit → error).
- Members must not be subtypes of each other (otherwise normalized).
- Working with a union value requires `match` or `is`-based narrowing:

```xray
let v: int | string = ...
match v {
    is int    -> print("int: ${v}"),
    is string -> print("str: ${v}"),
}
```

**Special cases**:
- `int | null` normalizes to `int?`.
- When `T?` appears in a union: `int? | string` is effectively `int | string | null`, normalized to `(int | string)?`.

### 2.7 Tuple Types

Xray's tuples are **first-class** — they may appear as any value, be stored as fields, and nest.

```xray @id=types-tuple
// Literals
let t = (1, 2, 3)                 // type inferred as (int, int, int)
let h = (10, "hi", true)          // heterogeneous tuple
let single = (99,)                // single-element tuple: note trailing comma

// Type annotation
let p: (int, string) = (7, "ok")

// Field access: .N (N is a compile-time constant integer index)
let first = t.0                   // 1
let mid   = t.1                   // 2
let nest  = ((1, 2), (3, 4))
let a     = nest.0.0              // 1
let b     = nest.1.1              // 4

// Function return and destructuring
fn divmod(a: int, b: int) -> (int, int) { return (a / b, a % b) }
let (q, r) = divmod(17, 5)        // tuple destructure

// Generic
fn pair<A, B>(a: A, b: B) -> (A, B) { return (a, b) }
let p2 = pair(1, "x")             // (int, string)
```

**Notes**:

- A **single-element tuple** must use a trailing comma `(x,)` — `(x)` without a comma is a grouping parenthesis (a plain expression).
- In field access `t.N`, N **must be an integer literal**; using a variable or string is the compile error `XR_ERR_ANALYZE_TUPLE_FIELD_NAME` / `_RANGE`.
- Tuples are **immutable**: `t.0 = v` is a compile error. To modify, build a new tuple.

### 2.8 Type Aliases

```xray @id=types-alias
type Result = int | string
type Mapper = (int) -> int
type Point = { x: float, y: float }
type Pair<T> = { first: T, second: T }
type Mapper2<T, U> = (T) -> U
```

Aliases are **purely syntactic** equivalences; they do not introduce new types,
runtime metadata, or AOT branches. A generic alias is substituted at its use
site:

```xray
let p: Pair<int> = { first: 1, second: 2 }  // equivalent to { first: int, second: int }
let f: Mapper2<int, string> = (n) -> string(n)
```

Generic alias parameters are a name list only (`<T, U>`); constraints are not
part of type-alias syntax. Put constraints on the generic function, class /
struct / enum / interface that uses the alias. Aliases may be forward
referenced, but cyclic aliases, including recursive object aliases, are compile
errors.

### 2.9 Type Inference

See §7.4 for details. In summary:

```xray @id=types-inference
let x = 1               // x: int
let y = 1.5             // y: float
let z = "hello"         // z: string
let a = [1, 2, 3]       // a: Array<int>
let m = #{"a": 1}    // m: Map<string, int>
let p = { name: "A" }   // p: { name: string } — structured object type
let f = (x: int) -> x   // f: (int) -> int — arrow parameters require annotation
```

### 2.10 Type Compatibility and Conversion

#### 2.10.1 Implicit Conversion

| From | To | Allowed |
|--|--|--|
| `int` | `float` | ✅ |
| `int8` | `int` (= `int64`) | ✅ |
| `T` | `T?` | ✅ |
| `T` | `Json` (if T is Json-compatible) | ✅ |
| `null` | `T?` | ✅ |
| Subtype | Supertype (class) | ✅ |
| Subset object type | Superset object type | ❌ (structural compatibility goes superset → subset) |

> **Structural compatibility direction** (duck typing): a type with more fields is assignable to a type with fewer fields.
> ```xray
> type User = { name: string }
> let full = { name: "A", age: 18 }
> let u: User = full       // OK: full is a superset of User
> ```

#### 2.10.2 Explicit `as`

```xray @id=types-cast
let n = x as int        // throws TypeError on failure
let n = x as int?       // returns null on failure (safe cast)
```

Applies to:
- Between numeric types (including `Json → int`, checked at runtime).
- `Json → User` (structural narrowing).
- Parent → child (downcast).

#### 2.10.3 `is` Check

```xray
if (v is User) {
    // In this branch the compiler narrows v's type to User
}
```

Acts only as a type guard; does not change the value.

### 2.11 typeof / typename / Type Enum

```xray
typeof(value)     // returns a Type enum value (an int representation)
typename(value)   // returns the type name as a string
```

`Type` enum members:

`Type.int`, `Type.float`, `Type.string`, `Type.bool`, `Type.null`,
`Type.Array`, `Type.Map`, `Type.Set`, `Type.Channel`, `Type.Json`,
`Type.function`, `Type.class`, `Type.struct`, `Type.enum`, `Type.module`, `Type.bigint`, ...

Full list: see `stdlib/types/enum.xr` / `src/runtime/value/xtype.h`.

### 2.12 Runtime Reflection

The `Reflect` module (built in):

```xray
Reflect.getType(obj)        // get type info (Json)
Reflect.typeOf(obj)         // get the type name (string)
Reflect.isInstance(obj, cls)// whether obj is an instance of cls
Reflect.fieldCount(obj)     // number of fields
Reflect.getAllTypes()       // all registered types
```

See §13 and §14 for more.
<!-- /xr-spec:en -->

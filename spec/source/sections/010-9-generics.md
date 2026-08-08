---
id: spec.9_generics
order: 010
---

<!-- xr-spec:cn -->
---

## 9. 泛型 (Generics)

> 真值源：`src/frontend/analyzer/xtype_ref_resolve.c`、`xanalyzer_mono.c`、`xanalyzer_builtin_interfaces.c` 与 `src/runtime/value/xtype_generic.c`。

### 9.1 类型参数语法 `<T>`

```ebnf
TypeParams ::= '<' TypeParam (',' TypeParam)* '>'
TypeParam  ::= Identifier (':' ConstraintList)?
ConstraintList ::= Type ('&' Type)*               // 交叉约束用 '&' 连接
TypeArgs   ::= '<' Type (',' Type)* '>'
AliasTypeParams ::= '<' Identifier (',' Identifier)* ','? '>'
```

```xray @id=generics-basic
// 泛型函数
fn identity<T>(x: T) -> T {
    return x
}

var a = identity<int>(42)
var b = identity("hello")               // 推断 T=string

// 泛型类
class Box<T> {
    value: T
    constructor(v: T) { this.value = v }
    get() -> T { return this.value }
}

var b1 = Box<int>(42)
var b2 = Box<string>("hi")

// 多参数泛型
class Pair<K, V> {
    key: K
    value: V
    constructor(k: K, v: V) {
        this.key = k; this.value = v
    }
}

// 泛型接口
interface Comparable<T> {
    compareTo(other: T) -> int
}

// 泛型 type alias：透明语法替换，不产生新类型
type PairAlias<T> = { first: T, second: T }
```

`type` 别名的泛型形参使用 `AliasTypeParams`：只允许名字列表，不支持约束。别名使用处会把类型实参直接代入别名 RHS，例如 `PairAlias<int>` 等价于 `{ first: int, second: int }`。这一步发生在编译期，不产生运行时元数据、单态化实例或 AOT 分支；循环别名会被拒绝。

### 9.2 类型约束：`<T: Constraint>` 与交叉约束 `&`

xray 的约束语法统一用冒号 `:`，多个约束用 `&` 连接（读作“同时满足”）。**不使用** Java/TS 的 `extends` / `implements` 作为约束关键字。

```xray @id=generics-constraints
// 单一约束
fn first<T: Comparable>(a: T, b: T) -> T {
    return a
}

// 多个约束（交叉）——T 必须同时满足 Comparable、Hashable、Stringable
fn passThrough<T: Comparable & Hashable & Stringable>(x: T) -> T {
    return x
}

// 多个类型参数，每个独立约束
fn pickValue<K: Hashable, V>(k: K, v: V) -> V {
    return v
}
```

**内置约束接口**：

| 接口 | 含义 |
|---|---|
| `Comparable` | 可用 `<` `<=` `>` `>=` 比较；int/float/string/Comparable 实现者 |
| `Hashable` | 可作为 `Map` 键或 `Set` 元素；内置 `int` / `float` / `string` / `bool` / `enum` / `BigInt` 默认满足，用户类型必须同时提供 `operator==` 与 `hash() -> int`（签名见下） |
| `Stringable` | 可调 `.toString()`；几乎所有内置类型默认实现 |
| `Iterable<T>` | 通过 iterator 协议被 `for-in` 遍历；Array、Slice、Map（含 `JSON.Object`）、Set、string、Range、生成器返回的 `Iterator<T>` 与自定义 `iterator()` 满足此约束。`JSON.Value` 不可直接迭代。`Channel<T>` 虽可用 `for-in` 接收，但走专用接收循环而非 iterator 协议，不满足此约束。unit-only enum 的 `for (value in E)` 与 concrete enum 的 `E.variants` 是编译期有限域语法，不使 enum 满足 `Iterable<T>`，也不能替代泛型 `Iterable<T>` 约束 |

`Hashable` 是静态契约：具体 class / struct / enum 用作 `Map<K, V>` 的键、`Set<T>` 的元素，或声明 `implements Hashable` 时，编译器必须看到非 `static`、非 `private` 的 `operator==` 与 `hash() -> int`。`operator==` 的参数类型必须**写成声明它的那个类型自己的名字**——Xray 没有 `Self` 类型，写 `Self` 会得到诊断 `E0365`：

```xray @id=generics-hashable-contract
class Token implements Hashable {
    value: int

    constructor(value: int) { this.value = value }

    // 参数写 Token，不写 Self
    operator==(other: Token) -> bool { return this.value == other.value }

    hash() -> int { return this.value }
}

var counts: Map<Token, int> = #{}
counts.set(Token(7), 99)
```

只提供旧式 `hashCode()` 不满足契约；只提供 `==` 或只提供 `hash()` 也会编译失败。若键/元素是类型参数，类型参数本身必须显式声明 `: Hashable`，例如 `fn f<K: Hashable>(m: Map<K, int>)`。

#### `where` 子句

约束也可以写在签名之后。`where` 是**同一机制的另一种拼写**，不是第二套规则：它把约束追加到 `<T: C>` 填的那张列表，因此两种写法由同一条路径检查（`E0358`），并且在同一个参数上**取交集**而非互相覆盖。

```ebnf
WhereClause ::= 'where' WhereItem (',' WhereItem)*
WhereItem   ::= Identifier ':' ConstraintList
```

```xray @id=generics-where-clause
// 约束列表长时，写在后面不会把参数挤出一行
fn maxOf<T>(a: T, b: T) -> T where T: Comparable {
    if (a > b) { return a }
    return b
}

// 内联与 where 在同一参数上取交集：T 必须同时满足 Comparable 与 Stringable
fn describe<T: Comparable>(a: T, b: T) -> T where T: Stringable { ... }

class Registry<K, V> where K: Hashable { ... }
struct Holder<T> where T: Comparable { ... }
interface Seq<T> where T: Comparable { ... }
enum Wrap<T> where T: Comparable { ... }
```

`where` 只能约束该声明自身的类型参数；命名其他标识符，或在没有类型参数的声明上使用，都是编译错误。

#### 键等价关系

哈希容器按**键等价关系**存取，它与 `==` 运算符是两个关系：

- 键等价必须**自反、对称、传递**。自反性是容器不变量：存进去的键必须能被它自己找回来，否则插入不再覆盖、查找不再命中、删除不再回收。
- `a == b` 蕴含 `a` 与 `b` 键等价（反之不成立）。
- 键等价蕴含 `hash(a) == hash(b)`。

内置 `float` 的 `==` 是 IEEE 语义，对 NaN 不自反，所以它的键等价额外规定：**所有 NaN 是同一个键**，`-0.0` 与 `+0.0` 是同一个键。于是 `nan == nan` 仍为 `false`，而 `m[nan] = v` 之后 `m[nan]` 一定取得到。

按"值是否在其中"提问的操作走键等价关系，不走 `==`：`Map` 的 `containsKey` / `containsValue` / 下标读写 / `delete`，`Set` 的 `add` / `contains` / `delete`，以及 `Array` 的 `indexOf` / `contains`。

用户类型的 `operator==` 直接充当它自己的键等价关系，因此**它必须自反**。带浮点字段的类型若原样转发 IEEE 比较，就会把上面的不变量带回来。

**当前限制**：
- 不支持**高阶类型**（`F<_>` 作为参数）——见 §9.6.1，这是明确不提供，不是暂缓。
- 不支持默认类型参数（`<T = int>`）。
- `where` 只接受与内联约束相同的表达力（`T: A & B`）；不支持对关联类型或嵌套类型的约束（`where T.Item: Hashable`），因为关联类型本身不存在。
- 同一个类型参数列表中不得出现重名参数（`<T, T>`）。
- 接口实现仍需**显式 `implements`**（在类声明位置，不是约束位置，详见 §5.4）。

### 9.3 类型推断与显式实例化

#### 类型推断

```xray @id=generics-inference
identity(42)                    // T 推断为 int
Box("hello")                // T 推断为 string
Pair("key", 100)            // K=string, V=int
```

推断算法是**双向推断**：
- 从参数推断（调用位置实参类型 → 类型参数）。
- 从返回值推断（上下文期望类型 → 类型参数）。

#### 显式实例化

在推断失败或需要明确时：

```xray @id=generics-explicit-instantiation
var empty = Array<int>()              // 无元素可推
var m = Map<string, int>()
var result = identity<float>(0)            // 泛型实参提供唯一上下文，0 直接定型为 float
```

### 9.4 特化与 monomorphization

**实现策略**：构建期 monomorphization（单态化）。**具体类型实参元组即实例身份**，函数泛型与 class / struct 泛型适用同一条规则。

- **实例身份**：`identity<string>` 与 `identity<MyClass>` 是两个实例，`Box<string>` 与 `Box<MyClass>` 也是两个实例——即使它们的运行时表示同为 PTR。前端不按表示合并，因为 duck-typed 的泛型体要针对具体类型实参解析 `x.foo()`：在解析完成之前，两个 ABI 等价的实例并不可互换。
- **代码共享是 AOT 决策，不是前端决策**：体积合并发生在解析之后的后端计划里（`generic-body-plan` / `generic-code-size-plan` 证据行，按体积阈值决定 `share_canonical_body`），并且带证据。前端保持精确身份，后端负责体积。
- 名字修饰（name mangling）：`identity<int>` → `identity$i64`，`Pair<string, int>` → `Pair$str$i64`。修饰名承载实例身份，因此不得丢失任何类型实参。
- 编译期严格类型检查保证安全；冷路径类型名元数据可在启用 names/debug profile 时保留具体类型参数显示信息。

> 真值源：`src/frontend/analyzer/xanalyzer_mono.c`（单态化 pass）、`xanalyzer_mono.h`（API）。

#### 单态化预算

两个预算防的是两类不同的风险，互不可替代：

| 预算 | 值 | 防什么 | 超限 |
|---|:---:|---|---|
| `XR_MONO_MAX_DEPTH` | 128 | **嵌套深度**。特化体可以再实例化别的泛型（`Router<int>` 构造 `RouteMatch<int>` 构造 `Map<string, int>`），因此展开是一个不动点迭代。多态递归（`fn f<T>() { f<Box<T>>() }`）让该迭代发散，而深度是唯一能识别它的量——每一轮都产生真正全新的类型元组，去重与计数都无法把发散和合法的广度区分开 | `E0389` |
| `XR_MONO_MAX_INSTANCES` | 16384 | **广度**。每个实例克隆一份完整声明，因此这是编译期内存兜底，不是语言规则。取值远高于任何现实程序 | `E0387` |

**超限一律是硬错误，绝不静默降级。** 把调用留在泛型状态会在 `xray verify` 的 `forbid=["box"]` 合同下面重新引入装箱，而合同刚刚"证明"了它不存在——这类不可见的去优化正是版本化 effect 合同要排除的东西。

`E0389` 的诊断会打印完整实例化链（`a$i64 -> b$Box_i64 -> ...`），否则报出的类型是用户从未写过、也无法检索的。

**性能影响**：
- 单态化让 AOT 在 I64 / F64 / BOOL 等值表示上生成无装箱 fast path。
- 逐类型特化会增加代码和元数据体积（大致按“类型组合数 × 声明体积”增长），换来精确布局、调试类型名保真和按类型特化；体积回收由上述 AOT 共享计划按阈值完成。
- 内置特化容器（`Array<int>`、`Array<byte>`）进一步避免装箱开销。
- 跨模块泛型在构建期 whole-program / LTO 阶段展开；提供泛型定义的库必须保留可分析的 IR/AST 形态，不能只发布不透明预编译产物。

**高阶函数的错误效应特化**：回调参数默认是 effect-polymorphic。单态化会按实参回调的 throw-effect summary 选择 `NO_THROW` 或 `MAY_THROW` 版本，使已证明 no-throw 的回调路径不生成无用 error-check；未知动态目标保守进入 may-throw 版本。需要强保证的高阶调用边界使用 `xray verify` 合同，证明不足即拒绝。

**特性状态**（使用 §0.4.3 的状态标记）：

| 特性 | 状态 | 说明 |
|---|---|---|
| `where` 子句 | **稳定** | 见 §9.2 |
| 声明点方差（`out T` / `in T`） | **未实现** | 有前置依赖，见 §9.6 |
| 默认类型参数（`<T = int>`） | **未实现** | 语法当前是错误，不是被忽略 |
| 高阶类型（HKT） | **明确不提供** | 与全程序单态化冲突，见 §9.6.1 |

### 9.5 协议（duck typing）与名义类型

#### 名义类型为主

xray 的接口实现需**显式 `implements`**——这与 Go 的"隐式接口实现"不同。

```xray @id=generics-nominal-interface
interface Drawable { draw() -> () }

class Square implements Drawable {        // 必须显式 implements
    draw() { print("square") }
}

class Wrong {
    draw() { print("wrong") }
}

fn render(d: Drawable) { d.draw() }
render(Square())     // OK
// render(Wrong())   // 编译错误：Wrong 不是 Drawable
```

#### 结构化对象

仅 `object literal` 与 `type T = {...}` 是结构化匹配。结构化匹配要求**精确字段集**（详见 §2.10.1）：既不能多也不能少，只有类型可空的字段允许缺省。

```xray
type Point = { x: float, y: float }

fn describe(p: Point) { ... }

describe({ x: 1.0, y: 2.0 })          // OK：字段集精确匹配
describe({ x: 1.0, y: 2.0, z: 3.0 })  // 编译错误 E0356：extra field 'z'
describe({ x: 1.0 })                  // 编译错误 E0356：missing field 'y'
```

### 9.6 方差（Variance）

**状态：未实现**（声明点方差标注 `out T` / `in T`）。当前行为是完整且健全的基线，不是占位：

- 容器类型：**不变**（`Array<Dog>` 不是 `Array<Animal>` 的子类型）。
- 函数类型：参数逆变、返回值协变（标准规则）。

**为什么不在本轮提供**：方差是在子类型关系之上定规则，因此它有一个前置依赖——结构化类型的宽度方向必须先定死（见 §2.10.1 的精确字段集规则）。在子类型关系本身尚未收敛时引入声明点方差，会把一个未定的语义再乘以一层，且不可向后兼容地修补。不变性是安全的、AOT 友好的、可随时放宽的起点。

### 9.6.1 高阶类型（HKT）

**状态：明确不提供**（不是"暂缓"）。Xray 不支持类型构造器参数（其它语言里写作 `F<_>` 的那种 functor 抽象）。

**为什么是永久决定**：HKT 与全程序单态化在根本上冲突。对类型构造器抽象意味着实例集合在编译期不再有限可枚举，实现只能退回字典传递或类型擦除——两者都会重新引入 Xray 的整条 AOT 路线（无装箱表示、精确布局、`xray verify` 的 shape 合同）明确要消除的间接层。这与"轻量脚本语言"的定位也不一致。

需要类似抽象能力时，使用 interface + 具体类型参数（`interface Mappable { map(f: (T) -> U) -> Self<U> }` 这类签名同样不提供），或在调用点用具体实例化。

### 9.7 泛型与类型身份

由于 monomorphization，每个具体实例化都有独立的类/函数定义。运行时类型判断使用名义身份，调试输出通过 `typeName` 的冷路径名字表提供：

```xray @id=generics-type-identity
class Container<T> {
    items: Array<T>
}
var c = Container<int>()
print(c is Container<int>)     // true
print(typeName(c))             // "Container<int>" when type names are enabled
```

结构化字段/方法元数据不会由默认运行时自动提供；需要 inspect/serialization 等能力时应使用显式 derive 或编译期生成。
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 9. Generics

> Source of truth: `src/frontend/analyzer/xtype_ref_resolve.c`, `xanalyzer_mono.c`, `xanalyzer_builtin_interfaces.c`, and `src/runtime/value/xtype_generic.c`.

### 9.1 Type Parameter Syntax `<T>`

```ebnf
TypeParams ::= '<' TypeParam (',' TypeParam)* '>'
TypeParam  ::= Identifier (':' ConstraintList)?
ConstraintList ::= Type ('&' Type)*               // intersection constraints joined by '&'
TypeArgs   ::= '<' Type (',' Type)* '>'
AliasTypeParams ::= '<' Identifier (',' Identifier)* ','? '>'
```

```xray @id=generics-basic
// Generic function
fn identity<T>(x: T) -> T {
    return x
}

var a = identity<int>(42)
var b = identity("hello")               // T inferred as string

// Generic class
class Box<T> {
    value: T
    constructor(v: T) { this.value = v }
    get() -> T { return this.value }
}

var b1 = Box<int>(42)
var b2 = Box<string>("hi")

// Multi-parameter generic
class Pair<K, V> {
    key: K
    value: V
    constructor(k: K, v: V) {
        this.key = k; this.value = v
    }
}

// Generic interface
interface Comparable<T> {
    compareTo(other: T) -> int
}

// Generic type alias: transparent syntax substitution, not a new type
type PairAlias<T> = { first: T, second: T }
```

Generic `type` aliases use `AliasTypeParams`: only a name list is allowed, with
no constraints. At each use site, the type arguments are substituted directly
into the alias RHS, so `PairAlias<int>` is equivalent to `{ first: int, second:
int }`. This happens at compile time and creates no runtime metadata,
monomorphization instance, or AOT branch; cyclic aliases are rejected.

### 9.2 Type Constraints: `<T: Constraint>` and Intersection Constraints `&`

Xray's constraint syntax uses a colon `:` uniformly, with multiple constraints joined by `&` (read as "must satisfy simultaneously"). It **does not use** Java/TS `extends` / `implements` as constraint keywords.

```xray @id=generics-constraints
// Single constraint
fn first<T: Comparable>(a: T, b: T) -> T {
    return a
}

// Multiple constraints (intersection) — T must satisfy Comparable, Hashable, and Stringable
fn passThrough<T: Comparable & Hashable & Stringable>(x: T) -> T {
    return x
}

// Multiple type parameters, each independently constrained
fn pickValue<K: Hashable, V>(k: K, v: V) -> V {
    return v
}
```

**Built-in constraint interfaces**:

| Interface | Meaning |
|---|---|
| `Comparable` | usable with `<` `<=` `>` `>=`; int/float/string and types implementing `Comparable` |
| `Hashable` | usable as a `Map` key or `Set` element; built-in `int` / `float` / `string` / `bool` / `enum` / `BigInt` satisfy it by default, and user types must provide both `operator==` and `hash() -> int` (signature below) |
| `Stringable` | callable via `.toString()`; almost every built-in type implements it by default |
| `Iterable<T>` | usable through the iterator protocol in `for-in`; Array, Slice, Map (including `JSON.Object`), Set, string, Range, the `Iterator<T>` a generator returns, and types with a custom `iterator()` satisfy this constraint. `JSON.Value` is not directly iterable. `Channel<T>` is receivable with `for-in` but drives a dedicated receive loop instead of the iterator protocol, so it does not satisfy it. Unit-only `for (value in E)` and concrete `E.variants` are compile-time finite-domain forms; they do not make an enum satisfy `Iterable<T>` and cannot stand in for a generic `Iterable<T>` constraint |

`Hashable` is a static contract: when a concrete class / struct / enum is used as a `Map<K, V>` key, a `Set<T>` element, or declares `implements Hashable`, the compiler must see a non-`static`, non-`private` `operator==` and `hash() -> int`. The parameter type of `operator==` must be **spelled as the name of the declaring type itself** — Xray has no `Self` type, and writing `Self` produces diagnostic `E0365`:

```xray @id=generics-hashable-contract
class Token implements Hashable {
    value: int

    constructor(value: int) { this.value = value }

    // the parameter is spelled Token, not Self
    operator==(other: Token) -> bool { return this.value == other.value }

    hash() -> int { return this.value }
}

var counts: Map<Token, int> = #{}
counts.set(Token(7), 99)
```

Providing only one of `==` or `hash()` is a compile error. If the key/element is a type parameter, that parameter itself must be explicitly constrained, for example `fn f<K: Hashable>(m: Map<K, int>)`.

#### `where` clauses

Constraints may also be written after the signature. `where` is **another spelling of the same mechanism**, not a second set of rules: it appends to the very list `<T: C>` fills, so both forms are checked by one path (`E0358`) and they **intersect** on a shared parameter rather than overriding each other.

```ebnf
WhereClause ::= 'where' WhereItem (',' WhereItem)*
WhereItem   ::= Identifier ':' ConstraintList
```

```xray @id=generics-where-clause
// A long constraint list after the signature keeps the parameters on one line
fn maxOf<T>(a: T, b: T) -> T where T: Comparable {
    if (a > b) { return a }
    return b
}

// Inline and where intersect on T: it must satisfy Comparable and Stringable
fn describe<T: Comparable>(a: T, b: T) -> T where T: Stringable { ... }

class Registry<K, V> where K: Hashable { ... }
struct Holder<T> where T: Comparable { ... }
interface Seq<T> where T: Comparable { ... }
enum Wrap<T> where T: Comparable { ... }
```

A `where` clause may only constrain type parameters of its own declaration; naming any other identifier, or using `where` on a declaration with no type parameters, is a compile error.

#### The key relation

Hash containers store and retrieve by a **key equivalence relation**, which is a different relation from the `==` operator:

- Key equivalence must be **reflexive, symmetric, and transitive**. Reflexivity is a container invariant: a stored key must find itself, or insert stops replacing, lookup stops hitting, and delete stops reclaiming.
- `a == b` implies `a` and `b` are key-equivalent (the converse does not hold).
- Key equivalence implies `hash(a) == hash(b)`.

Built-in `float` compares with IEEE `==`, which is not reflexive on NaN, so its key relation adds: **all NaNs are one key**, and `-0.0` is the same key as `+0.0`. `nan == nan` therefore stays `false`, while `m[nan] = v` followed by `m[nan]` always finds the value.

Operations that ask "is this value in here" use the key relation rather than `==`: `Map`'s `containsKey` / `containsValue` / subscript read and write / `delete`, `Set`'s `add` / `contains` / `delete`, and `Array`'s `indexOf` / `contains`.

A user type's `operator==` serves as its own key relation, so **it must be reflexive**. A type with float fields that forwards IEEE comparison unchanged reintroduces the invariant break described above.

**Current limitations**:
- **Higher-kinded types** (`F<_>` as a parameter) are not supported — see §9.6.1; this is an explicit non-goal, not a deferral.
- Default type parameters (`<T = int>`) are not supported.
- `where` accepts exactly the expressiveness of an inline constraint (`T: A & B`); constraints on associated or nested types (`where T.Item: Hashable`) are not supported, because associated types do not exist.
- Duplicate names in one type-parameter list (`<T, T>`) are rejected.
- Interface implementation still requires **explicit `implements`** at the class declaration site (not at the constraint site; see §5.4).

### 9.3 Type Inference and Explicit Instantiation

#### Type inference

```xray @id=generics-inference
identity(42)                    // T inferred as int
Box("hello")                // T inferred as string
Pair("key", 100)            // K=string, V=int
```

The inference algorithm is **bidirectional**:
- From arguments (call-site argument types → type parameters).
- From the return type (contextual expected type → type parameters).

#### Explicit instantiation

When inference fails or precision is needed:

```xray @id=generics-explicit-instantiation
var empty = Array<int>()              // no element to infer from
var m = Map<string, int>()
var result = identity<float>(0)            // the type argument supplies a unique context; 0 is directly typed as float
```

### 9.4 Specialization and Monomorphization

**Implementation strategy**: build-time monomorphization. **The concrete type-argument tuple is the instance identity**, and the same rule applies to generic functions and to generic classes / structs alike.

- **Instance identity**: `identity<string>` and `identity<MyClass>` are two instances, and so are `Box<string>` and `Box<MyClass>` — even though both use the PTR runtime representation. The frontend never merges by representation, because a duck-typed generic body resolves `x.foo()` against the concrete type argument: until that resolution is done, two ABI-equivalent instances are not interchangeable.
- **Code sharing is an AOT decision, not a frontend one**: size-driven merging happens after resolution, in the backend plan (`generic-body-plan` / `generic-code-size-plan` evidence rows decide `share_canonical_body` against a size threshold), and it carries evidence. The frontend keeps identity exact; the backend owns size.
- Name mangling: `identity<int>` → `identity$i64`, `Pair<string, int>` → `Pair$str$i64`. The mangled name *is* the instance identity, so it must never drop a type argument.
- Strict compile-time type checking ensures safety; cold-path type-name metadata may retain concrete type-parameter display information when the names/debug profile enables it.

> Source of truth: `src/frontend/analyzer/xanalyzer_mono.c` (monomorphization pass), `xanalyzer_mono.h` (API).

#### Monomorphization budgets

Two budgets guard two different risks, and they are not interchangeable:

| Budget | Value | Guards | On breach |
|---|:---:|---|---|
| `XR_MONO_MAX_DEPTH` | 128 | **Nesting**. A specialized body may instantiate further generics (`Router<int>` building `RouteMatch<int>` building `Map<string, int>`), so expansion is a fixpoint. Polymorphic recursion (`fn f<T>() { f<Box<T>>() }`) makes that fixpoint diverge, and depth is the only quantity that can detect it: every round produces a genuinely new type tuple, so neither dedup nor a counter can tell divergence from legitimate breadth | `E0389` |
| `XR_MONO_MAX_INSTANCES` | 16384 | **Breadth**. Each instance clones a whole declaration, so this is a compile-time memory backstop rather than a language rule. It sits far above any realistic program | `E0387` |

**Exceeding a budget is always a hard error, never a silent downgrade.** Leaving a call generic would reintroduce boxing underneath an `xray verify` `forbid=["box"]` contract that just "proved" it absent — exactly the kind of invisible de-optimization versioned effect contracts exist to rule out.

The `E0389` diagnostic prints the full instantiation chain (`a$i64 -> b$Box_i64 -> ...`); without it the reported type is one the user never wrote and cannot search for.

**Performance impact**:
- Monomorphization lets AOT generate unboxed fast paths for I64 / F64 / BOOL value representations.
- Per-type specialization grows code and metadata size roughly with "type combinations x declaration size"; this buys exact layout, faithful debug type names, and per-type specialization. Size is recovered by the AOT sharing plan above, against its threshold.
- Built-in specialized containers (`Array<int>`, `Array<byte>`) further avoid boxing overhead.
- Cross-module generics are expanded during build-time whole-program / LTO analysis. Libraries that expose generic definitions must ship analyzable IR/AST form rather than only opaque precompiled artifacts.

**Error-effect specialization for higher-order functions**: callback parameters are effect-polymorphic by default. Monomorphization selects a `NO_THROW` or `MAY_THROW` version from the argument callback's throw-effect summary, so a callback proven no-throw does not generate unnecessary error checks; an unknown dynamic target conservatively selects the may-throw version. Strong guarantees at higher-order call boundaries use `xray verify` contracts and reject incomplete proof.

**Feature status** (using the §0.4.3 status markers):

| Feature | Status | Notes |
|---|---|---|
| `where` clauses | **Stable** | see §9.2 |
| Declaration-site variance (`out T` / `in T`) | **Unimplemented** | has a prerequisite, see §9.6 |
| Default type parameters (`<T = int>`) | **Unimplemented** | the syntax is currently an error, not silently ignored |
| Higher-kinded types (HKT) | **Explicitly not provided** | conflicts with whole-program monomorphization, see §9.6.1 |

### 9.5 Protocols (Duck Typing) vs. Nominal Typing

#### Nominal typing dominates

Xray's interface implementations require **explicit `implements`** — unlike Go's "implicit interface implementation".

```xray @id=generics-nominal-interface
interface Drawable { draw() -> () }

class Square implements Drawable {        // explicit implements required
    draw() { print("square") }
}

class Wrong {
    draw() { print("wrong") }
}

fn render(d: Drawable) { d.draw() }
render(Square())     // OK
// render(Wrong())   // compile error: Wrong is not Drawable
```

#### Structural objects

Only `object literal` and `type T = {...}` use structural matching. Structural matching requires an **exact field set** (see §2.10.1): neither extra nor missing fields, except that fields whose declared type admits null may be omitted.

```xray
type Point = { x: float, y: float }

fn describe(p: Point) { ... }

describe({ x: 1.0, y: 2.0 })          // OK: exact field set
describe({ x: 1.0, y: 2.0, z: 3.0 })  // compile error E0356: extra field 'z'
describe({ x: 1.0 })                  // compile error E0356: missing field 'y'
```

### 9.6 Variance

**Status: Unimplemented** (declaration-site variance annotations `out T` / `in T`). The current behavior is a complete and sound baseline, not a placeholder:

- Container types: **invariant** (`Array<Dog>` is not a subtype of `Array<Animal>`).
- Function types: parameters contravariant, return values covariant (the standard rule).

**Why not in this round**: variance states rules *on top of* the subtype relation, so it has a prerequisite — the width direction for structural types must be settled first (see the exact-field-set rule in §2.10.1). Introducing declaration-site variance while the subtype relation itself has not converged multiplies an undecided semantics by another layer, and it cannot be patched backward-compatibly afterwards. Invariance is the safe, AOT-friendly starting point, and it can be relaxed at any time.

### 9.6.1 Higher-Kinded Types (HKT)

**Status: explicitly not provided** — a non-goal, not a deferral. Xray has no type-constructor parameters (the functor abstraction other languages spell `F<_>`).

**Why this is permanent**: HKT is fundamentally at odds with whole-program monomorphization. Abstracting over a type constructor means the instance set is no longer finitely enumerable at compile time, leaving only dictionary passing or type erasure — and both reintroduce exactly the indirection that Xray's AOT line (unboxed representations, exact layout, `xray verify` shape contracts) exists to remove. It is also inconsistent with the lightweight-scripting-language positioning.

Where similar abstraction is wanted, use an interface with concrete type parameters (signatures like `interface Mappable { map(f: (T) -> U) -> Self<U> }` are likewise not provided), or instantiate concretely at the call site.

### 9.7 Generics and Type Identity

Because of monomorphization, every concrete instantiation has its own class/function definition. Runtime checks use nominal identity, and debug output goes through `typeName`'s cold-path name table:

```xray @id=generics-type-identity
class Container<T> {
    items: Array<T>
}
var c = Container<int>()
print(c is Container<int>)     // true
print(typeName(c))             // "Container<int>" when type names are enabled
```

Structured field/method metadata is not provided automatically by the default runtime; use explicit derive or compile-time generation for inspect/serialization use cases.
<!-- /xr-spec:en -->

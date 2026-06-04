---
id: spec.9_generics
order: 010
---

<!-- xr-spec:cn -->
---

## 9. 泛型 (Generics)

> 真值源：`src/frontend/analyzer/xanalyzer_generic.c`、`src/frontend/analyzer/xanalyzer_subtype.c`。

### 9.1 类型参数语法 `<T>`

```ebnf
TypeParams ::= '<' TypeParam (',' TypeParam)* '>'
TypeParam  ::= Identifier (':' ConstraintList)?
ConstraintList ::= Type ('&' Type)*               // 交叉约束用 '&' 连接
TypeArgs   ::= '<' Type (',' Type)* '>'
```

```xray @id=generics-basic
// 泛型函数
fn identity<T>(x: T) -> T {
    return x
}

let a = identity<int>(42)
let b = identity("hello")               // 推断 T=string

// 泛型类
class Box<T> {
    value: T
    constructor(v: T) { this.value = v }
    get() -> T { return this.value }
}

let b1 = new Box<int>(42)
let b2 = new Box<string>("hi")

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
```

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

**内置约束接口**（详见 §14.14）：

| 接口 | 含义 |
|---|---|
| `Comparable` | 可用 `<` `<=` `>` `>=` 比较；int/float/string/Comparable 实现者 |
| `Hashable` | 可作为 `Map` / `Set` 的键；int/float/string/bool/enum/Hashable 实现者 |
| `Stringable` | 可调 `.toString()`；几乎所有内置类型默认实现 |
| `Iterable<T>` | 可被 `for-in` 遭历；Array、Map、Json、string、Range、enum、自定义 `iterator()` |

**当前限制**：
- 约束只能位于类型参数后，不支持 where 子句。
- 不支持**高阶类型**（`F<_>` 作为参数）。
- 不支持默认类型参数（`<T = int>`）。
- 接口实现仍需**显式 `implements`**（在类声明位置，不是约束位置，详见 §5.4）。

### 9.3 类型推断与显式实例化

#### 类型推断

```xray @id=generics-inference
identity(42)                    // T 推断为 int
new Box("hello")                // T 推断为 string
new Pair("key", 100)            // K=string, V=int
```

推断算法是**双向推断**：
- 从参数推断（调用位置实参类型 → 类型参数）。
- 从返回值推断（上下文期望类型 → 类型参数）。

#### 显式实例化

在推断失败或需要明确时：

```xray @id=generics-explicit-instantiation
let empty = new Array<int>()              // 无元素可推
let m = new Map<string, int>()
let result = identity<float>(0)            // 0 默认 int，强制 float
```

### 9.4 特化与 monomorphization

**实现策略**：编译期 monomorphization（单态化）。

- 编译器收集所有泛型函数/类的具体类型实例化点，为每个类型组合生成专用 AST 克隆并编译为独立字节码。
- 名字修饰（name mangling）：`identity<int>` → `identity$i64`，`Pair<string, int>` → `Pair$str$i64`。
- 按表示分组共享（rep-sharing）：同为指针表示的类型共用同一份特化（最多 3 版本：I64 / F64 / PTR）。
- 编译期严格类型检查保证安全；运行时保留具体类型参数信息供 `Reflect.typeOf` 使用。

> 真值源：`src/frontend/analyzer/xanalyzer_mono.c`（单态化 pass）、`xanalyzer_mono.h`（API）。

**性能影响**：
- 单态化的泛型函数可被 JIT / AOT 直接优化为原生类型操作（无装箱）。
- 内置特化容器（`Array<int>`、`Bytes`）进一步避免装箱开销。
- 编译体积随实例化组合数线性增长；上限 `XR_MONO_MAX_INSTANCES` 防止爆炸。

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
render(new Square())     // OK
// render(new Wrong())   // 编译错误：Wrong 不是 Drawable
```

#### 结构化对象

仅`object literal` 与 `type T = {...}` 是结构化匹配：

```xray
type Point = { x: float, y: float }

fn describe(p: Point) { ... }

describe({ x: 1.0, y: 2.0 })   // OK：字面量结构匹配
describe({ x: 1.0, y: 2.0, z: 3.0 })  // 编译错误：sealed 类型多了字段
```

### 9.6 方差（Variance）

当前不支持显式方差标注（`out T` / `in T`）。默认行为：
- 容器类型：**不变**（`Array<Dog>` 不是 `Array<Animal>` 的子类型）。
- 函数类型：参数逆变、返回值协变（标准规则）。

### 9.7 泛型与运行时反射

由于 monomorphization，每个具体实例化在运行时都有独立的类/函数定义，且保留了类型参数信息：

```xray @id=generics-reflection
class Container<T> {
    items: Array<T>
}
let c = new Container<int>()
print(Reflect.typeOf(c))       // "Container<int>"
```

对具体值的类型检查使用 `is` / `as`。
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 9. Generics

> Source of truth: `src/frontend/analyzer/xanalyzer_generic.c`, `src/frontend/analyzer/xanalyzer_subtype.c`.

### 9.1 Type Parameter Syntax `<T>`

```ebnf
TypeParams ::= '<' TypeParam (',' TypeParam)* '>'
TypeParam  ::= Identifier (':' ConstraintList)?
ConstraintList ::= Type ('&' Type)*               // intersection constraints joined by '&'
TypeArgs   ::= '<' Type (',' Type)* '>'
```

```xray @id=generics-basic
// Generic function
fn identity<T>(x: T) -> T {
    return x
}

let a = identity<int>(42)
let b = identity("hello")               // T inferred as string

// Generic class
class Box<T> {
    value: T
    constructor(v: T) { this.value = v }
    get() -> T { return this.value }
}

let b1 = new Box<int>(42)
let b2 = new Box<string>("hi")

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
```

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

**Built-in constraint interfaces** (see §14.14 for details):

| Interface | Meaning |
|---|---|
| `Comparable` | usable with `<` `<=` `>` `>=`; int/float/string and types implementing `Comparable` |
| `Hashable` | usable as a `Map` / `Set` key; int/float/string/bool/enum and types implementing `Hashable` |
| `Stringable` | callable via `.toString()`; almost every built-in type implements it by default |
| `Iterable<T>` | usable in `for-in`; Array, Map, Json, string, Range, enum, types with custom `iterator()` |

**Current limitations**:
- Constraints may only follow type parameters; there is no `where` clause.
- **Higher-kinded types** (`F<_>` as a parameter) are not supported.
- Default type parameters (`<T = int>`) are not supported.
- Interface implementation still requires **explicit `implements`** at the class declaration site (not at the constraint site; see §5.4).

### 9.3 Type Inference and Explicit Instantiation

#### Type inference

```xray @id=generics-inference
identity(42)                    // T inferred as int
new Box("hello")                // T inferred as string
new Pair("key", 100)            // K=string, V=int
```

The inference algorithm is **bidirectional**:
- From arguments (call-site argument types → type parameters).
- From the return type (contextual expected type → type parameters).

#### Explicit instantiation

When inference fails or precision is needed:

```xray @id=generics-explicit-instantiation
let empty = new Array<int>()              // no element to infer from
let m = new Map<string, int>()
let result = identity<float>(0)            // 0 defaults to int; force float
```

### 9.4 Specialization and Monomorphization

**Implementation strategy**: compile-time monomorphization.

- The compiler collects all concrete instantiation sites of generic functions/classes, generates a dedicated AST clone for each type combination, and compiles each into independent bytecode.
- Name mangling: `identity<int>` → `identity$i64`, `Pair<string, int>` → `Pair$str$i64`.
- Sharing by representation (rep-sharing): types with the same pointer representation share a single specialization (at most three versions: I64 / F64 / PTR).
- Strict compile-time type checking ensures safety; concrete type-parameter information is retained at runtime for `Reflect.typeOf`.

> Source of truth: `src/frontend/analyzer/xanalyzer_mono.c` (monomorphization pass), `xanalyzer_mono.h` (API).

**Performance impact**:
- Monomorphized generic functions can be optimized directly by JIT / AOT into native-typed operations (no boxing).
- Built-in specialized containers (`Array<int>`, `Bytes`) further avoid boxing overhead.
- Compiled binary size grows linearly with the number of instantiation combinations; the ceiling `XR_MONO_MAX_INSTANCES` prevents explosion.

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
render(new Square())     // OK
// render(new Wrong())   // compile error: Wrong is not Drawable
```

#### Structural objects

Only `object literal` and `type T = {...}` use structural matching:

```xray
type Point = { x: float, y: float }

fn describe(p: Point) { ... }

describe({ x: 1.0, y: 2.0 })   // OK: literal matches structurally
describe({ x: 1.0, y: 2.0, z: 3.0 })  // compile error: sealed type rejects extra fields
```

### 9.6 Variance

Explicit variance annotations (`out T` / `in T`) are not currently supported. Default behavior:
- Container types: **invariant** (`Array<Dog>` is not a subtype of `Array<Animal>`).
- Function types: parameters contravariant, return values covariant (the standard rule).

### 9.7 Generics and Runtime Reflection

Because of monomorphization, every concrete instantiation has its own class/function definition at runtime, with type-parameter information retained:

```xray @id=generics-reflection
class Container<T> {
    items: Array<T>
}
let c = new Container<int>()
print(Reflect.typeOf(c))       // "Container<int>"
```

Type checks on concrete values use `is` / `as`.
<!-- /xr-spec:en -->

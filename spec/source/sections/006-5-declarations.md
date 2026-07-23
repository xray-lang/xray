---
id: spec.5_declarations
order: 006
---

<!-- xr-spec:cn -->
---

## 5. 声明 (Declarations)

> 真值源：`src/frontend/parser/xparse_decl.c`、`src/frontend/parser/xast_nodes_decl.h`、`src/frontend/analyzer/xanalyzer_visitor.c`。

### 5.1 `var` / `const`

```ebnf
VarDecl ::= 'var' Binding
ConstDecl ::= 'const' Binding
Binding ::= Pattern (':' Type)? ('=' Expression)?
Pattern ::= Identifier
         | '[' BindingPattern (',' BindingPattern)* ','? ']'    // array destructure
         | '(' BindingPattern (',' BindingPattern)+ ','? ')'    // tuple destructure
         | '{' ObjectBinding (',' ObjectBinding)* ','? '}'      // object destructure
ObjectBinding ::= Identifier (':' Identifier)?
```

#### 5.1.1 `var` — 可变绑定

```xray @id=decl-var
var x = 1                         // 类型推断为 int
var name: string = "Alice"        // 显式类型
var count: int                    // 仅声明无初值：使用零值
var maybeName: string?            // OK：默认 null
var empty: string = ""            // string 必须显式初始化
```

- 可重新赋值。
- 必须有初值**或**类型标注；否则编译错误 `E0303`。
- 无初值只允许 **default-initializable** 类型：数值类型默认 `0` / `0.0`，`bool` 默认 `false`，`()` 默认 unit，`T?` 默认 `null`，struct 仅当所有字段都可默认初始化时允许。
- 非 nullable 的 `string`、class instance、`Array` / `Map` / `Set`、`Channel`、`Task`、function / closure、interface / union 等必须显式初始化。

#### 5.1.2 `const` — 不可变绑定

```xray @id=decl-const
const PI = 3.14159
const MAX_LEN: int = 1024
```

- **必须**有初值。
- 不能重新赋值（编译错误 `E0303`）。
- 类型可推断或显式标注。
- `const` 和 `var` 一样，每条声明绑定一个名字或解构模式。多个独立名字使用多条声明；相关值可用 `const (a, b) = pair` 解构。
- 对 managed/aggregate 值，`const name: T` 推导并持有 `const T` 能力：字段、索引和嵌套投影深只读；`var name: const T` 则允许名字重绑，但不开放图内修改。
- `const T` 可用于任意 type position。不可变标量上的 `const` 与原类型等价；managed/aggregate 上的 `const T` 是独立 type identity。
- 新鲜构造可直接进入 `var` 的可变域或 `const` 的只读域。已有可变唯一图进入 `const` 必须显式 `move` 或 `copy`，不存在隐式冻结或隐藏复制。
- `Channel`、`Atomic`、`Mutex` 等受审计同步句柄以 `const` 命名；编译器把它们规范化为内部同步共享能力，其受审计方法仍可改变同步保护的内部状态。
- 新鲜可变图由编译器推断唯一所有权，不需要存储修饰符。`move` 要求源根唯一且无存活 alias/loan，成功后使源绑定失效；`copy` 保留源并显式构造独立图。

```xray @id=decl-capability
const channel = Channel<int>(16)
const counter = Atomic(0)

var source = [1, 2, 3]
var moved = move source       // 转移同一根；source 此后不可用
const snapshot = copy(moved)  // 显式构造深只读独立图
var current: const Config = loadConfig()
```

详见 [§10.11](#1011-并发安全模型)。

#### 5.1.3 解构绑定

```xray @id=decl-destructuring
// 数组解构
var [a, b, c] = [1, 2, 3]
var [first, , third] = [10, 20, 30]         // 跳过元素

// 元组解构（多返回值）
var (q, r) = divmod(17, 5)

// 对象解构（按字段名提取，可重命名本地绑定）
var { name, age } = { name: "Alice", age: 30 }
var { name: localName, age } = { name: "Alice", age: 30 }
```

约束：
- 解构变量数必须匹配（除 rest 模式外）。
- 对象解构字段名必须是 `Identifier`；`field: localName` 只改变本地绑定名，不改变被读取的字段名。

### 5.2 `fn` 函数声明

```ebnf
FnDecl ::= AttrList? 'fn' Identifier TypeParams? '(' ParamList? ')' ReturnType? Block
ParamList ::= Param (',' Param)*
Param     ::= Identifier ':' ParamType ('=' DefaultValue)?
            | '...' Identifier ':' Type
ParamType ::= ParamMode? Type
ParamMode ::= 'ref' | 'move'
ReturnType ::= '->' Type
            |  '->' '(' Type (',' Type)+ ')'   // 元组返回
TypeParams ::= '<' Identifier (',' Identifier)* '>'
AttrList ::= ('@' Identifier ('(' AttrArgList? ')')?)*
```

#### 5.2.1 基本形式

```xray @id=decl-fn-basic
fn add(a: int, b: int) -> int {
    return a + b
}

fn greet(name: string) -> () {         // 显式 Unit
    print("Hi ${name}")
}

fn echo(x: int) {                       // 省略返回类型 = ()
    print(x)
}
```

**关键**：
- 参数**必须**带类型标注（与箭头函数一致）。
- 返回类型省略 = `()`（Unit）；推荐显式标注以增强可读性。
- 函数体必须是块。

#### 5.2.2 默认参数值

```xray @id=decl-fn-default
fn connect(host: string, port: int = 8080, tls: bool = false) {
    print(host, port, tls)
}

connect("localhost")              // port=8080, tls=false
connect("localhost", 443)         // tls=false
connect("localhost", 443, true)
```

- 默认值在**调用点**求值：省略某个尾部参数时，编译器在该调用点补入默认表达式，按实参顺序、每次省略调用各求值一次。
- 显式传 `null` 就是传入 `null`，**不会**触发默认值（默认值只在参数被省略时使用）。
- 有默认值的参数必须在尾部连续出现。
- 默认参数只作用于**具名函数/方法/构造器的直接调用**。通过函数值（函数类型变量）的间接调用不携带默认表达式，必须传入全部实参。

#### 5.2.3 多返回值

```xray @id=decl-fn-multi-return
fn divmod(a: int, b: int) -> (int, int) {
    return (a / b, a % b)
}

var (q, r) = divmod(17, 5)
var result = divmod(10, 3)        // result 类型 (int, int)
```

**约束**：
- 返回类型用括号包裹元组：`(int, bool)`。
- 单返回值不写括号：`: int`。
- `return (a, b)` 必须带括号；裸逗号 `return a, b` 是编译错误（`E0801`）。

#### 5.2.4 参数模式

普通参数默认提供只读 capability；只有写借用和所有权交接需要显式模式：
`name: ref T`、`name: move T`。

```xray @id=decl-fn-param-modes
fn length_sq(v: Vec2) -> float {
    // v 默认只读；具体 ABI 可按值或按只读地址传递
    return v.x * v.x + v.y * v.y
}

fn translate(v: ref Vec2, dx: float, dy: float) -> () {
    // v 是可变引用（修改对调用方可见）
    v.x += dx
    v.y += dy
}

fn submit(job: move Job) -> () {
    queue.store(move job)
}

translate(ref point, 1.0, 2.0)
submit(move pending)
submit(makeJob())
```

| 参数模式 | 语义 |
|--|--|
| 无（READ） | 只读 capability；callee 不得修改 caller 的 mutable graph |
| `ref` | 独占可写 place 借用；调用点必须写 `ref place` |
| `move` | 取得唯一 owner；既有 lvalue 调用点必须写 `move value`，fresh value 与 `copy(value)` 可直接传入 |

普通输出使用返回值、tuple、struct 或 `Result`。C ABI 输出位置使用 `MutPtr<T>`，
不把输出参数模式引入普通 Xray 函数。

#### 5.2.5 rest 参数

```xray @id=decl-fn-rest
fn sum(...nums: int) -> int {
    var total = 0
    for (n in nums) { total += n }
    return total
}

sum(1, 2, 3)        // total = 6
```

- rest 参数在参数列表**最后**。
- 类型 `...T` 内部实际是 `Array<T>`。
- 只能有一个 rest 参数。

#### 5.2.6 函数提升

```xray
main()                       // OK：函数声明被提升

fn main() { ... }
```

- 顶层 `fn` 声明被提升到当前作用域顶部。
- `var f = (x: int) -> x`（赋值给变量的箭头函数）**不**提升。

#### 5.2.7 尾递归优化

Xi 优化器会把可证明的自尾调用改写为循环；VM 也有常量栈空间的 tail-call opcode。不要把这一点理解为所有后端、所有间接/互递归调用的通用常量栈保证：构造调用和无法证明安全的调用仍按普通调用执行。详见 [§17](#17-编译流水线-compilation-pipeline)。

```xray
fn factorial(n: int, acc: int = 1) -> int {
    if (n <= 1) { return acc }
    return factorial(n - 1, acc * n)     // 尾调用：自动优化为循环
}
```

#### 5.2.8 程序入口

xray **没有隐式 `main` 入口**：脚本/模块从顶层开始顺序执行，遇到 `fn` 声明被提升注册，遇到表达式或语句被立即执行。

```xray
// hello.xr
print("loading")          // 顶层语句，立即执行
fn greet() { print("hi") }
greet()                   // 必须显式调用
```

- `fn main()` 没有任何特殊含义；如需手动调用，写 `main()`。
- 顶层不允许 `return`（编译错误 `E0306`）。
- 多文件项目的入口由 `xray.toml` 的 `[project]`（或 package manifest 的 `[package]`）中的 `main` 字段指定，例如 `main = "src/main.xr"`；对应文件按上述脚本规则执行。

#### 5.2.9 `extern "C"` C FFI 声明块

`extern "C"` 块只声明共享 C ABI 的外部**函数符号**。外部函数没有 Xray 函数体，调用点必须显式写在 `unsafe { }` 内：

```xray
extern "C" {
    fn malloc(n: usize) -> MutPtr<byte>
    fn free(p: MutPtr<byte>)
    fn cos(x: f64) -> f64
}

var p = unsafe { malloc(4) }
unsafe {
    p[0] = 42
    print(cos(0.0))
    free(p)
}
```

库、目标、symbol rename、原生源码/对象、typed flags、hash 与许可证都属于 `xray.toml` 的 NativePackagePlan，不写在普通源码中。`extern "C"` 不允许 `dylib/link` 子句、函数体、data/global，也不允许在块内声明 struct/union/packed/flex。

C ABI 聚合使用普通 Xray struct，并由 manifest 的 `[[native.layout]]` 绑定 C header/type。构建器生成 `sizeof`、`_Alignof` 与 `offsetof` 静态断言；任何不一致都在链接前失败：

```xray
import mem

struct CHeader {
    tag: u8
    count: u32
}

print(mem.sizeOf<CHeader>())
print(mem.alignOf<CHeader>())
print(mem.offsetOf<CHeader>("count"))
```

规则：
- ABI 字符串必须显式写出；当前唯一支持值是 `"C"`。
- extern 块内函数只能声明签名，不能带 `{ }` 函数体；普通函数必须带块体。
- 每个声明必须在 NativePackagePlan 中有唯一 symbol mapping 与完整 typed contract；shipping package 缺合同、hash、目标或来源信息时 fail closed。
- C 输出指针先写入 raw `Buffer`。成功路径仅可在 `unsafe` 中调用 `mem.assumeInitialized<T>(move buffer)`；编译器要求 exact size/alignment、完整 output validity、success-path dominance 与 header layout evidence。失败/partial write 不能物化为 `T`。
- 普通 Xray 函数没有 output parameter mode，返回值统一写 `return value`，不写 `return move value`。
- 每个编译目标只有一份 canonical target data layout。Analyzer、VM、AOT、Slice/layout 查询与 header verifier共用 size/alignment/field-offset 结果。
- 跨 VM/AOT 后端已收口的边界类型包括 `bool`、精确整数、`f32` / `f64`、`usize` / `isize`、`Ptr<T>`、`MutPtr<T>`，以及 `()` 返回。
- C 回调参数必须写成 `CFn<(A, B) -> R>`，不能使用普通 xray 函数类型 `(A, B) -> R`。
- 当前 `CFn` 实参必须是模块级、非捕获、签名精确匹配的 xray 函数；匿名函数、捕获闭包和 extern 函数本身会被拒绝。

```xray
extern "C" {
    fn bsearch(
        key: Ptr<byte>,
        base: Ptr<byte>,
        count: usize,
        size: usize,
        cmp: CFn<(Ptr<byte>, Ptr<byte>) -> i32>
    ) -> Ptr<byte>
}

fn zeroCmp(a: Ptr<byte>, b: Ptr<byte>) -> i32 {
    return 0
}

// zeroCmp 是模块级非捕获函数，可作为 CFn 回调。
```

#### 5.2.10 Manifest C ABI 导出

模块级 Xray 函数在源码中保持普通 `fn`；是否导出 C ABI、导出符号与可见性都由 `xray.toml` 的 typed export plan 指定：

```xray
fn add(a: i32, b: i32) -> i32 {
    return a + b
}

print(add(19, 23))        // Xray 内部仍是普通函数调用
```

```toml
[[export.c]]
xray = "add"
symbol = "xr_add_i32"
visibility = "default"
header = true
```

规则：
- `xray` 必须唯一解析到模块级、有函数体的普通函数；方法、匿名函数、嵌套函数与 extern 声明不能成为导出目标。
- `symbol` 必须是非空 C identifier；同一 AOT bundle 的 Xray target 与 C symbol 均不得重复。
- 当前支持的导出边界类型是 `bool`、精确整数、`f32` / `f64`、`usize` / `isize`、`Ptr<T>`、`MutPtr<T>`，以及 `()` 返回。
- 当前不直接导出 Xray managed value 或 by-value aggregate；与 C 共享结构体内存时通过 `Ptr<T>` / `MutPtr<T>` 传递地址。
- `xray build --native --c-header FILE` 为 `header = true` 的 export 生成 C 原型；`--shared` 只接受无需 runtime 初始化的 scalar/raw-pointer 边界。
- export plan 只选择导出目标，不绕过 ABI verifier，也不改变 VM 语义或普通 Xray 调用。

#### 5.2.11 推导 effect 与 `xray verify` 合同

分配、错误集、挂起、阻塞、panic/abort 与 AOT residue 全部由编译器推导，不由源码注解声明；需要发布或性能门禁时，把要求写入版本化合同：

```toml
version = 1

[[function]]
symbol = "math.addOne"
scope = "semantic"
requires = ["no_semantic_alloc", "no_throw", "no_suspend"]

[[function]]
symbol = "codec.hotAdd"
scope = "backend"
backend = "aot"
target = "aarch64-apple-darwin"
profile = "release"
requires = ["no_runtime_heap", "no_throw", "no_suspend"]

[function.shape]
forbid = ["runtime_dispatch", "box", "bounds_in_loop", "lane_spill"]
allow = []
```

运行 `xray verify --contract perf-contracts.toml`。合同只验证已有语义/effect 与目标产物形状，不授予优化许可，也不能改变运行语义。semantic 合同可与目标无关；backend/shape 合同必须写出具体 backend、target 与 profile。动态调用、native unknown、缺失 symbol 或不完整证明均 fail closed，并报告 witness。

#### 完整可运行示例

闭包捕获与高阶函数：

```xray
fn apply(f: (int) -> int, x: int) -> int {
    return f(x)
}

fn main() {
    var base = 10
    var addBase = fn(x: int) -> int { return x + base }   // 闭包捕获 base
    print(addBase(5))            // => 15
    print(apply(addBase, 7))     // => 17（函数作为参数传入）
}

main()
```

多返回值（元组）：

```xray
fn divmod(a: int, b: int) -> (int, int) {
    return (a / b, a % b)
}

fn main() {
    var (q, r) = divmod(17, 5)
    print(q)   // => 3
    print(r)   // => 2
}

main()
```

### 5.3 `class` 声明

```ebnf
ClassDecl ::= 'final'? 'class' Identifier TypeParams?
              ('extends' Identifier TypeArgs?)?
              ('implements' Identifier TypeArgs? (',' Identifier TypeArgs?)*)?
              '{' ClassMember* '}'
ClassMember ::= FieldDecl | MethodDecl | ConstructorDecl | StaticBlock
FieldDecl ::= Modifier* Identifier ':' Type ('=' Expression)?
MethodDecl ::= Modifier* Identifier '(' ParamList? ')' ReturnType? Block
            |  Modifier* 'operator' OpToken '(' ParamList? ')' ReturnType? Block
ConstructorDecl ::= 'constructor' '(' ParamList? ')' Block          // 参数类型可省
Modifier ::= 'private' | 'protected' | 'static' | 'const'
```

> **关于默认公开可见性和自动覆写**：
>
> - 公开是**默认可见性**——所有未带 `private` / `protected` 的字段/方法都是公开的；语言没有 `public` 修饰符。
> - 可见性受**编译期强制**：从类外访问 `private` / `protected` 成员、或从非子类访问 `protected` 成员，均报 `E0377`。
> - 覆写由编译器自动推导：子类实例方法与父类链中非私有实例方法同名同签时即为覆写。
> - 用户不可写 `override` / `abstract` / `final method`；同名不同签、字段/方法隐藏、静态方法隐藏均为编译错误。
>
> 标准库和回归测试一致采用"省略默认修饰符"风格。

#### 5.3.1 基本类

```xray @id=decl-class-basic
class Animal {
    name: string                       // 字段
    private _age: int = 0              // 私有字段，可有默认值

    constructor(name: string) {
        this.name = name
    }

    speak() -> string {
        return "..."
    }

    static create(name: string) -> Animal {
        return Animal(name)
    }
}

var a = Animal("Rex")
print(a.speak())
print(Animal.create("Bob").name)
```

#### 5.3.2 继承

```xray @id=decl-class-inheritance
class Dog extends Animal {
    constructor(name: string) {
        super(name)                    // **必须**首语句（仅限派生类）
    }

    speak() -> string {                  // 同名同签：自动覆写
        return "woof"
    }
}
```

**约束**：
- 派生类构造器**第一行**必须是 `super(...)`（除非未声明构造器）；否则编译错误。
- 不能在 `super(...)` 之前访问 `this`。
- **重写父类方法不需要任何关键字**——只要子类出现同名同签实例方法即自动重写。
- 同名不同签不是重载，也不是隐藏；必须改名或使用默认参数 / 命名工厂。
- 父类标 `final class` 则不可继承。
- `super.method()` 可在重写的方法体内调用被屏蔽的父类方法。

#### 5.3.3 修饰符

| 修饰符 | 适用 | 语义 |
|--|--|--|
| （无） | 字段/方法 | 默认 public——公开可见 |
| `private` | 字段/方法 | 仅声明类内部可访问（含同类其它实例）；子类与外部访问均报 `E0377` |
| `protected` | 字段/方法 | 声明类及其子类内部可访问；外部访问报 `E0377` |
| `static` | 字段/方法 | 类级别，不属于实例；调用为 `ClassName.method()` |
| `const` | 字段 | 不可变字段——只能在声明类的构造器中经 `this` 赋值一次，之后重写报 `E0378` |
| `final` | 类声明前缀 | `final class C` 禁止继承；`final` 不用于字段或方法 |

**修饰符可组合**：`private const secret: string = "key123"`、`protected static counter: int = 0`。

> `const` = 不可变字段/绑定，`final class` = 禁止继承。字段不可变只用 `const`；对字段或方法写 `final` 会报错。

#### 5.3.4 构造器

```xray
class Point {
    x: float
    y: float
    constructor(x: float, y: float) {
        this.x = x
        this.y = y
    }
}

// 参数类型可省（从同名字段推断）
class Vector2 {
    x: float
    y: float
    constructor(x, y) {         // 等价于显式写 (x: float, y: float)
        this.x = x
        this.y = y
    }
}
```

- 关键字 `constructor`（不是 `init` 也不是与类同名）。
- 一个类**只有一个构造器**（不支持构造器重载）；要多种创建方式用 `static` 工厂方法。
- 构造器参数**类型可省**——若参数名与字段同名，从字段类型自动推断；其他情况推断为调用位点的实参类型。
- 构造器隐式返回 `this`（编译期注入）。
- 派生类构造器必须首行调 `super(...)`。
- struct 可以**没有**构造器（`Point()` 创建隐式零值实例，后续手动赋值；详见 §5.4）。

#### 5.3.5 运算符重载

```xray
class Vec2 {
    x: float
    y: float

    constructor(x: float, y: float) {
        this.x = x; this.y = y
    }

    operator+(other: Vec2) -> Vec2 {
        return Vec2(this.x + other.x, this.y + other.y)
    }

    operator==(other: Vec2) -> bool {
        return this.x == other.x && this.y == other.y
    }

    operator[](index: int) -> float {
        if (index == 0) { return this.x }
        return this.y
    }
}
```

**可重载的运算符**（完整列表，源自 `xparse_oop.c`）：

| 类别 | 运算符 | 参数数 | 备注 |
|--|--|--|--|
| 二元算术 | `+` `-` `*` `/` `%` | 1 | `-` 单参数视为一元负号 |
| 位运算 | `&` `\|` `^` `<<` `>>` | 1 | |
| 比较 | `==` `!=` `<` `<=` `>` `>=` | 1 | 一般成对实现 `==`/`!=`、`<`/`<=`/`>`/`>=` |
| 下标 | `[]`（getter）`[]=`（setter） | 1 / 2 | setter 是 `(index, value)` |
| 一元 | `!` `~` `++` `--` | 0 | |
| 复合赋值 | `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` | 1 | |

```xray
class Counter {
    n: int = 0
    operator++() -> Counter { this.n = this.n + 1; return this }
    operator+=(other: int) -> Counter { this.n = this.n + other; return this }
    operator[](i: int) -> int { return this.n + i }
    operator[]=(i: int, v: int) { this.n = v - i }
}
```

**不能**重载：`&&` `\|\|` `=` `?.` `?[` `?:` `??` `,` `.`

#### 5.3.6 自定义迭代器

实现 `iterator()` 返回带 `hasNext() -> bool` 和 `next() -> T?` 的对象即可启用 `for-in`。详见 §14.15。

#### 5.3.7 完整可运行示例

以下为自包含、可运行并通过 `xray check` 验证的完整程序（注释标注真实输出）。

继承与自动覆写：

```xray
class Animal {
    name: string
    constructor(name: string) { this.name = name }
    speak() -> string { return "..." }
}

class Dog extends Animal {
    constructor(name: string) { super(name) }
    speak() -> string { return "woof" }   // 同名同签：自动覆写
}

fn main() {
    var d = Dog("Rex")
    print(d.name)      // => Rex
    print(d.speak())   // => woof
}

main()
```

运算符重载（用具名变量调用）：

```xray
class Vec2 {
    x: int
    y: int
    constructor(x: int, y: int) { this.x = x; this.y = y }
    operator+(other: Vec2) -> Vec2 {
        return Vec2(this.x + other.x, this.y + other.y)
    }
}

fn main() {
    var a = Vec2(1, 2)
    var b = Vec2(3, 4)
    var sum = a + b
    print(sum.x)   // => 4
    print(sum.y)   // => 6
}

main()
```

### 5.4 `struct` 声明

```ebnf
StructDecl ::= 'struct' Identifier TypeParams?
               ('implements' Identifier (',' Identifier)*)?
               '{' StructMember* '}'
```

```xray @id=decl-struct-point
struct Point {
    x: float
    y: float

    magnitude_sq() -> float {
        return this.x * this.x + this.y * this.y
    }
}

// 两种创建方式
var p = Point()                  // 默认构造（字段为零值）后逐个赋值
p.x = 3.0
p.y = 4.0

var q = Point{x: 3.0, y: 4.0}        // struct 字面量：类型名 + { field: value }
var pt = Point{x: 1.0, y: 2.0}

// 值语义：赋值与传参都是拷贝
var b = q                            // b 是 q 的独立拷贝
b.x = 99.0
// q.x 仍为 3.0
```

**与 `class` 的差异**：

| 维度 | `class` | `struct` |
|--|--|--|
| 内存语义 | 引用类型（堆） | 值类型（栈或内联） |
| 赋值/传参 | 共享引用 | **拷贝**（`var b = a` 生产独立副本） |
| 继承 | 支持 `extends` | **不支持**继承 |
| `implements` | ✅ | ✅ |
| 泛型 | ✅ | ✅ |
| `static` / `private` / `protected` / `const` | ✅ | ✅ |
| 运算符重载 | ✅ | ✅ |
| 构造器 | `constructor(...)` | **可省略**：`Point()` 生成零值实例 |
| 字面量 | 无 | `TypeName{field: value, ...}` |

**适用场景**：
- 数学类型（Vec2/Vec3/Quat/Color）
- 短生命周期值（迭代器状态、临时元组替代）
- 性能敏感、希望避免堆分配的数据

#### 5.4.1 值语义示例

`struct` 是值类型，赋值与传参都会拷贝：

```xray
struct Point {
    x: int
    y: int
}

fn main() {
    var p = Point{x: 3, y: 4}
    var q = p            // struct 赋值是拷贝
    q.x = 99
    print(p.x)           // => 3（不受影响）
    print(q.x)           // => 99
}

main()
```

### 5.5 `interface` 与 `implements`

xray 接口实现是**显式声明的**（与 Go 的隐式实现不同）：类 / struct 必须用 `implements` 列出实现的接口。

```ebnf
InterfaceDecl ::= 'interface' Identifier TypeParams?
                  ('extends' NamedType (',' NamedType)*)?
                  '{' InterfaceMember* '}'
InterfaceMember ::= Identifier '(' ParamList? ')' ReturnType?       // 方法签名
                 |  ('const')? Identifier ':' Type                   // 属性签名（可加 const 表示只读）
```

```xray @id=decl-interface-shape
interface Shape {
    area() -> float
    perimeter() -> float
}

// 接口方法返回类型可省略（默认 ()）
interface Greeter {
    greet(name: string)             // 等价于 greet(name: string) -> ()
    log()                           // 无参无返回
}

class Circle implements Shape {
    radius: float
    constructor(r: float) { this.radius = r }
    area() -> float { return 3.14 * this.radius * this.radius }
    perimeter() -> float { return 6.28 * this.radius }
}

// 实现多个接口
class Logger implements Shape, Greeter {
    radius: float
    constructor(r: float) { this.radius = r }
    area() -> float { return 3.14 * this.radius * this.radius }
    perimeter() -> float { return 6.28 * this.radius }
    greet(name: string) { print("hello,", name) }
    log() { print("logging") }
}

fn describe(s: Shape) -> string {
    return "area=${s.area()}, perimeter=${s.perimeter()}"
}
```

**约束**：

- 接口可继承其他接口（`extends`）；支持泛型 `interface Container<T>` 与受约束 `interface Stats<T: Numeric>`。
- 类 / struct 用 `implements I1, I2, ...` 声明实现一个或多个接口（**显式**，不存在隐式实现）。
- 实现类**必须**提供所有接口成员（方法同名同参同返回；属性同名同类型）。
- 接口方法声明中的**返回类型可省略**（默认 `()`）。
- 接口方法默认 `abstract`（无方法体）。
- 接口可声明**属性签名**（`length: int`、`const id: int`）；实现类必须有相应字段。
- 实现类可以提供额外的方法（接口仅定义最小集）。

```xray
// 属性签名 + 接口继承
interface HasLength {
    length: int
}
interface SizedCollection<T> extends HasLength {
    first() -> T
}

class Buffer implements SizedCollection<int> {
    length: int                       // 实现属性签名
    private data: Array<int>
    constructor(n: int) {
        this.length = n
        this.data = []
    }
    first() -> int { return this.data[0] }
}
```

#### 完整可运行示例

接口 + `implements` + 多态：

```xray
interface Shape {
    area() -> float
}

class Circle implements Shape {
    r: float
    constructor(r: float) { this.r = r }
    area() -> float { return 3.14159 * this.r * this.r }
}

fn main() {
    var s: Shape = Circle(2.0)
    print(s.area())   // => 12.56636
}

main()
```

### 5.6 `enum` 声明

xray 的 `enum` 是**安全 tagged aggregate**：每个值包含编译器分配的声明顺序 tag，并且只有当前 tag 对应的 payload 可读。无 payload 的简单 enum 只是 payload size 为 0 的 tagged aggregate；带 payload 的 enum 是同一模型下的安全 sum type。

```ebnf
EnumDecl       ::= 'enum' Identifier TypeParams?
                   ('implements' NamedType (',' NamedType)*)?
                   '{' EnumVariant (',' EnumVariant)* ','? EnumMethod* '}'
EnumVariant    ::= Identifier VariantPayload?
EnumMethod     ::= 'static'? 'fn' Identifier TypeParams? '(' ParamList? ')' ReturnType? Block
VariantPayload ::= '(' VariantField (',' VariantField)* ')'
VariantField   ::= (Identifier ':')? Type
```

> 变体声明必须排在前面（逗号分隔），方法声明排在所有变体之后（无逗号，靠块边界分隔，与 `class` 内方法一致）。详见 §5.6.7。

#### 5.6.1 简单枚举（0-payload enum）

```xray @id=decl-enum-simple
enum Color { Red, Green, Blue }

Color.Red.ordinal     // 0
Color.Blue.ordinal    // 2
Color.Red.name        // "Red"
Color.Red.toString()  // "Color.Red"

enum HttpStatus {
    OK,
    NotFound,
    InternalError

    fn code() -> int {
        return match (this) {
            HttpStatus.OK -> 200,
            HttpStatus.NotFound -> 404,
            HttpStatus.InternalError -> 500
        }
    }
}
```

enum variant 使用声明顺序形成稳定的 `ordinal`，不声明额外 backing value。协议数值、字符串符号等外部表示通过 `const`、方法或显式转换函数表达。

#### 5.6.2 Payload enum

变体名后跟括号声明 payload 字段（位置参数或具名字段）：

```xray @id=decl-enum-payload
enum Option<T> {
    Some(T),
    None,
}

enum NetEvent {
    Connected,
    Disconnected(reason: string),
    DataReceived(bytes: Array<byte>),
    Error(code: int, message: string),
}

enum Expr {
    Number(int),
    Binary(op: string, left: Box<Expr>, right: Box<Expr>),
    Call(name: string, args: Array<Expr>),
}
```

直接按值递归的 enum payload 会导致无限大小，必须编译拒绝。递归数据结构需要显式间接化，例如 `Box<Expr>`、class 节点或引用容器槽。

#### 5.6.3 构造与解构

构造：

```xray
var c = Color.Red
var r1 = Option.Some(42)                            // 位置 payload
var e1 = NetEvent.DataReceived(bytes: b)            // 具名 payload，可写字段名
var e2 = NetEvent.Error(404, "not found")           // 也可省略字段名按位置传
var e3 = NetEvent.Connected                         // 无 payload 变体不写括号
```

解构（match）：

```xray @id=decl-enum-match
match (event) {
    NetEvent.Connected            -> print("connected"),
    NetEvent.Disconnected(reason) -> print("by:", reason),
    NetEvent.DataReceived(b)      -> process(b),
    NetEvent.Error(code, msg)     -> log.error(code, msg),
}
```

详见 §6.3。

#### 5.6.4 enum 值 API

实例属性（作用在枚举值上）：

```xray @id=decl-enum-properties
Color.Red.name        // "Red"          变体名 (string)
Color.Red.ordinal     // 0              声明顺序 tag (int，从 0)
Color.Red.toString()  // "Color.Red"    "<EnumName>.<VariantName>" 格式
```

enum 值提供 `name`、`ordinal` 与 `toString()`。它们不提供 `value`、`rawValue`、`fromName` 或 `fromOrdinal` 等隐式 backing-value/reflection API。

#### 5.6.5 遍历

仅由无 payload 变体组成的具体 enum 可以直接迭代实际 enum 值；任意具体 enum 都可通过 `.variants` 迭代声明级描述符：

```xray @id=decl-enum-iteration
for (color in Color) {
    print(color.name)                     // color: Color；Red、Green、Blue
}

for (variant in NetEvent.variants) {
    print(variant.ordinal)                // variant: EnumVariant<NetEvent>
    print(variant.name)
    for (field in variant.payloads) {
        print(field.name)                 // field: EnumPayloadField<NetEvent>
        print(field.type)                 // int：具体字段类型的 canonical TypeId
    }
}
```

两种循环语法相似，但产出类型有意不同：

| 表达式 | 适用范围 | 循环变量类型 | 语义 |
|---|---|---|---|
| `E` | 仅 unit-only 具体 enum | `E` | 按声明顺序产出实际 enum 值 |
| `E.variants` | 任意具体 enum | `EnumVariant<E>` | 按声明顺序产出只读变体描述符 |
| `variant.payloads` | 一个 `EnumVariant<E>` | `EnumPayloadField<E>` | 按声明顺序产出只读 payload 字段描述符 |

若 `E` 含任一 payload 变体，`for (value in E)` 是编译错误，诊断会建议 `E.variants`；编译器不会虚构 payload 值。`for (variant in Color.variants)` 对 unit-only enum 也完全合法，但 `variant` 仍是描述符而不是 `Color` 值。循环本身不打印任何内容；只有循环体显式执行的副作用会产生输出。

descriptor API 是封闭白名单：

| 类型 | 属性 / 操作 |
|---|---|
| `EnumVariants<E>` | `length: int`、检查边界的 `[index] -> EnumVariant<E>`、`for-in` |
| `EnumVariant<E>` | `ordinal: int`、`name: string`、`payloadCount: int`、`isUnit: bool`、`payloads: EnumPayloads<E>` |
| `EnumPayloads<E>` | `length: int`、检查边界的 `[index] -> EnumPayloadField<E>`、`for-in` |
| `EnumPayloadField<E>` | `index: int`、`name: string`、`type: int`（canonical TypeId） |

命名 payload 字段的 `name` 是源码声明名；位置 payload 字段没有声明名，其 `name` 确定为 `""`，不使用 `null`，因此 descriptor 表面保持非空 `string` 类型。

这些类型不可由用户构造，描述符不可调用，也不提供从名字/ordinal 构造 enum 值的入口。越界索引按普通 checked index 失败。descriptor 不进入 C ABI，FFI 边界会编译拒绝。

该能力是编译器静态类型域，不是 `Iterable` 协议实现：直接循环和不逃逸 descriptor 在 VM/AOT 中以 ordinal/index 标量降低，不分配数组或 iterator。只有 descriptor 流入 `any`、擦除 union、泛型存储、容器、闭包或跨协程通道等需要身份的边界时才物化不可变 box；`.name`、payload schema 与 type token 由使用证据分别保留，未使用的 cold sidecar 可裁剪。

unit-only enum 的实际值在 typed 路径中同样只携带 ordinal；一旦该值跨入 tagged/擦除边界，静态 sidecar 必须保留 enum 名与全部 case 名，使边界后的 `.name`、`toString()`、相等性和通用字符串格式化与 VM 语义一致。仍保持 typed 的 enum 不生成该 sidecar。

泛型时必须知道具体 enum layout，例如 `Option<int>.variants` 合法；未约束类型参数 `E.variants` 不合法。别名、导入和跨模块编译保留同一声明顺序与具体类型替换。

#### 5.6.6 反查（从值到成员）

默认不支持 `Enum(value)` 或从 backing value 反查 enum。协议解析应写成显式函数：

```xray
fn statusFromCode(code: int) -> HttpStatus? {
    if (code == 200) { return HttpStatus.OK }
    if (code == 404) { return HttpStatus.NotFound }
    if (code == 500) { return HttpStatus.InternalError }
    return null
}
```

#### 5.6.7 enum 方法

`enum` 体内可定义实例方法和静态方法，语法与 `class` 内的方法一致（不引入 `impl` 关键字）。实例方法在所有变体上可调用；方法体内通过 `match (this)` 区分变体行为：

```xray
enum Shape {
    Circle(radius: float),
    Rect(w: float, h: float),
    Triangle(a: float, b: float, c: float)

    fn area() -> float {
        return match (this) {
            Shape.Circle(r)     -> 3.14159 * r * r,
            Shape.Rect(w, h)    -> w * h,
            Shape.Triangle(a, b, c) -> {
                var s = (a + b + c) / 2.0
                return (s * (s-a) * (s-b) * (s-c)).sqrt()
            },
        }
    }

    fn isRound() -> bool {
        return match (this) {
            Shape.Circle(_) -> true,
            _               -> false,
        }
    }
}

var s = Shape.Circle(radius: 1.0)
print(s.area())          // 3.14159
print(s.isRound())       // true
```

静态方法使用 `static fn`，常用于工厂、查表和 enum 相关 helper；静态方法没有 `this`：

```xray
enum Color {
    Red, Green, Blue

    static fn fromInt(v: int) -> Color {
        if (v == 1) { return Color.Red }
        if (v == 2) { return Color.Green }
        return Color.Blue
    }

    fn label() -> string {
        return this.name
    }
}

print(Color.fromInt(2).label())     // "Green"
```

> 注意 `Triangle(...)` 后没有逗号——最后一个变体与方法块之间用空白分隔（trailing comma 允许但不强制）。

**规则**：

- 方法语法与 `class` 内方法一致：`fn name(params) -> ReturnType { body }` 或 `static fn name(params) -> ReturnType { body }`
- 方法体内 `this` 的静态类型是 enum 自身（如 `Option<T>`），需要 `match (this)` 才能取出变体 payload
- 静态方法没有 `this`；调用形式是 `EnumName.method(args...)`
- **不**支持 `constructor`（变体语法本身就是构造器）
- **不**支持继承（`enum E extends ...` 是非法）；如需共享行为，用接口实现（`enum E implements Iface`）或顶层函数
- 简单枚举（无 payload）也可定义方法，但方法体内 `this` 是该 enum 的值，可用 `==` 直接比较：
  ```xray
  enum Color {
      Red, Green, Blue

      fn isWarm() -> bool { return this == Color.Red }
  }
  ```
- 方法**不能**和变体名同名

> 此设计与 Java enum / Swift enum / Kotlin sealed class 一致。Rust 的 `impl` 块在 xray 中**不**引入——xray 的方法定义统一在类型体内。

### 5.7 `type` 别名

```ebnf
TypeAliasDecl ::= 'type' Identifier AliasTypeParams? '=' Type
AliasTypeParams ::= '<' Identifier (',' Identifier)* ','? '>'
```

```xray
type Outcome = int | string                          // union 别名
type Mapper = (int) -> int                              // 函数类型别名
type Point = { x: float, y: float }                  // 结构化对象别名（sealed）
type Pair<T> = { first: T, second: T }                // 泛型别名
```

**语义**：
- 别名是**纯语法**替换，不产生新名义类型。
- 泛型别名在使用处做类型实参代入；代入发生在编译期，不引入运行时表示或 AOT 分支。
- 泛型别名形参只允许名字列表；不支持约束。约束应写在使用该别名的泛型声明上。
- `type Point = {...}` 的对象类型在使用此别名标注时**密封**：未声明的字段访问/赋值是编译错误。
- `type T = Json` 等于 `Json`（不密封）。
- 别名可前向引用，但**禁止循环别名**。

详见 [§2.4.6](#246-json) 与 [§2.8](#28-类型别名)。

### 5.8 `import` / `export`

详见 [§11](#11-模块系统-modules)。语法要点：

```xray
// stdlib / 第三方包：裸标识符，可自动生成别名
import time
import http
import alice/utils as utils

// 文件路径或目录路径：字符串，可显式 `as`，否则从路径尾段推导别名
import "./modules/mod_a.xr" as a
import "../utils/string_utils.xr" as utils
import "models/user" as user

// 命名 import：支持 quoted path 或裸模块名，成员可用 `as` 重命名
import { readFile, writeFile as write } from io
import { publicFn } from "./modules/mod_a.xr"

// 导出
export fn publicFn() -> string { return "hi" }
export const VERSION = "1.0"
export { name1, name2 as alias } from "./other"
export * from "./other"
```

**xray 不支持** JavaScript 默认导入 `import name from "module"`。使用 `import "module" as name`、`import module` 或 `import { name } from module`。

完整规则、路径解析、可见性细则见 [§11 模块系统](#11-模块系统-modules)。
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 5. Declarations

> Source of truth: `src/frontend/parser/xparse_decl.c`, `src/frontend/parser/xast_nodes_decl.h`, `src/frontend/analyzer/xanalyzer_visitor.c`.

### 5.1 `var` / `const`

```ebnf
VarDecl ::= 'var' Binding
ConstDecl ::= 'const' Binding
Binding ::= Pattern (':' Type)? ('=' Expression)?
Pattern ::= Identifier
         | '[' BindingPattern (',' BindingPattern)* ','? ']'    // array destructure
         | '(' BindingPattern (',' BindingPattern)+ ','? ')'    // tuple destructure
         | '{' ObjectBinding (',' ObjectBinding)* ','? '}'      // object destructure
ObjectBinding ::= Identifier (':' Identifier)?
```

#### 5.1.1 `var` — mutable binding

```xray @id=decl-var
var x = 1                         // type inferred as int
var name: string = "Alice"        // explicit type
var count: int                    // no initializer: zero value used
var maybeName: string?            // OK: defaults to null
var empty: string = ""            // string requires an explicit initializer
```

- Reassignable.
- Must have an initializer **or** a type annotation; otherwise compile error `E0303`.
- Omitted initializers are allowed only for **default-initializable** types: numeric types default to `0` / `0.0`, `bool` defaults to `false`, `()` defaults to unit, `T?` defaults to `null`, and structs are allowed only when every field is default-initializable.
- Non-nullable `string`, class instances, `Array` / `Map` / `Set`, `Channel`, `Task`, function / closure, interface / union, and similar reference-like values require an explicit initializer.

#### 5.1.2 `const` — immutable binding

```xray @id=decl-const
const PI = 3.14159
const MAX_LEN: int = 1024
```

- Initializer is **required**.
- Cannot be reassigned (compile error `E0303`).
- The type may be inferred or annotated explicitly.
- Like `var`, each `const` declaration binds one name or destructuring pattern. Use separate declarations for independent names, or destructure related values with `const (a, b) = pair`.
- For managed/aggregate values, `const name: T` infers and holds the `const T` capability: fields, indexes, and nested projections are deeply read-only. `var name: const T` permits rebinding the name without granting graph mutation.
- `const T` is accepted in every type position. `const` on an immutable scalar is identical to the base type; `const T` on a managed/aggregate value is a distinct type identity.
- Fresh construction may target either a mutable `var` domain or a read-only `const` domain. An existing mutable unique graph entering `const` requires explicit `move` or `copy`; there is no implicit freeze or hidden copy.
- Audited synchronization handles such as `Channel`, `Atomic`, and `Mutex` are named with `const`. The compiler normalizes them to an internal synchronized shared capability whose audited methods may still mutate protected internal state.
- The compiler infers unique ownership for fresh mutable graphs; no storage modifier is required. `move` requires a unique root with no live alias/loan and invalidates the source binding on success; `copy` preserves the source and explicitly constructs an independent graph.

```xray @id=decl-capability
const channel = Channel<int>(16)
const counter = Atomic(0)

var source = [1, 2, 3]
var moved = move source       // transfer the same root; source is now invalid
const snapshot = copy(moved)  // explicitly construct an independent read-only graph
var current: const Config = loadConfig()
```

See [§10.11](#1011-concurrency-safety-model).

#### 5.1.3 Destructuring bindings

```xray @id=decl-destructuring
// array destructuring
var [a, b, c] = [1, 2, 3]
var [first, , third] = [10, 20, 30]         // skip elements

// tuple destructuring (multi-return)
var (q, r) = divmod(17, 5)

// object destructuring (extract by field name; local binding may be renamed)
var { name, age } = { name: "Alice", age: 30 }
var { name: localName, age } = { name: "Alice", age: 30 }
```

Constraints:
- The number of destructured bindings must match (except with rest patterns).
- Object destructuring field names must be `Identifier`s; `field: localName` changes only the local binding name, not the field being read.

### 5.2 `fn` function declaration

```ebnf
FnDecl ::= AttrList? 'fn' Identifier TypeParams? '(' ParamList? ')' ReturnType? Block
ParamList ::= Param (',' Param)*
Param     ::= Identifier ':' ParamType ('=' DefaultValue)?
            | '...' Identifier ':' Type
ParamType ::= ParamMode? Type
ParamMode ::= 'ref' | 'move'
ReturnType ::= '->' Type
            |  '->' '(' Type (',' Type)+ ')'   // tuple return
TypeParams ::= '<' Identifier (',' Identifier)* '>'
AttrList ::= ('@' Identifier ('(' AttrArgList? ')')?)*
```

#### 5.2.1 Basic form

```xray @id=decl-fn-basic
fn add(a: int, b: int) -> int {
    return a + b
}

fn greet(name: string) -> () {         // explicit Unit
    print("Hi ${name}")
}

fn echo(x: int) {                       // omitted return type = ()
    print(x)
}
```

**Key points**:
- Parameters **must** carry type annotations (consistent with arrow functions).
- An omitted return type means `()` (Unit); explicit annotation is recommended for readability.
- The function body must be a block.

#### 5.2.2 Default parameter values

```xray @id=decl-fn-default
fn connect(host: string, port: int = 8080, tls: bool = false) {
    print(host, port, tls)
}

connect("localhost")              // port=8080, tls=false
connect("localhost", 443)         // tls=false
connect("localhost", 443, true)
```

- Default values are evaluated at the **call site**: when a trailing argument is omitted, the compiler completes the call with the default expression, evaluating it once per omitting call in argument order.
- Passing an explicit `null` passes `null`; it does **not** trigger the default (defaults are used only when the argument is omitted).
- Parameters with default values must appear consecutively at the tail of the parameter list.
- Default arguments apply only to **direct calls of a named function/method/constructor**. A call through a function value (a function-typed variable) carries no default expressions and must pass every argument.

#### 5.2.3 Multiple return values

```xray @id=decl-fn-multi-return
fn divmod(a: int, b: int) -> (int, int) {
    return (a / b, a % b)
}

var (q, r) = divmod(17, 5)
var result = divmod(10, 3)        // result has type (int, int)
```

**Constraints**:
- The return type wraps the tuple in parentheses: `(int, bool)`.
- A single return value omits the parentheses: `: int`.
- `return (a, b)` requires the parentheses; bare comma `return a, b` is a compile error (`E0801`).

#### 5.2.4 Parameter modes

Ordinary parameters provide a read-only capability by default. Only writable borrowing and
ownership transfer have explicit modes: `name: ref T` and `name: move T`.

```xray @id=decl-fn-param-modes
fn length_sq(v: Vec2) -> float {
    // v is read-only; the ABI may pass a small value or a read-only address
    return v.x * v.x + v.y * v.y
}

fn translate(v: ref Vec2, dx: float, dy: float) -> () {
    // v is a mutable reference (changes are visible to the caller)
    v.x += dx
    v.y += dy
}

fn submit(job: move Job) -> () {
    queue.store(move job)
}

translate(ref point, 1.0, 2.0)
submit(move pending)
submit(makeJob())
```

| Parameter mode | Semantics |
|--|--|
| none (READ) | Read-only capability; the callee cannot mutate the caller's mutable graph |
| `ref` | Exclusive writable place loan; the call site must write `ref place` |
| `move` | Transfer of the unique owner; an existing lvalue requires `move value`, while a fresh value or `copy(value)` can be passed directly |

Ordinary outputs use return values, tuples, structs, or `Result`. C ABI output locations use
`MutPtr<T>` rather than an output parameter mode in ordinary Xray functions.

#### 5.2.5 Rest parameters

```xray @id=decl-fn-rest
fn sum(...nums: int) -> int {
    var total = 0
    for (n in nums) { total += n }
    return total
}

sum(1, 2, 3)        // total = 6
```

- The rest parameter must be **last**.
- The actual internal type of `...T` is `Array<T>`.
- Only one rest parameter is allowed.

#### 5.2.6 Function hoisting

```xray
main()                       // OK: the function declaration is hoisted

fn main() { ... }
```

- Top-level `fn` declarations are hoisted to the top of the current scope.
- `var f = (x: int) -> x` (an arrow function bound to a variable) is **not** hoisted.

#### 5.2.7 Tail-call optimization

The Xi optimizer rewrites proven self-tail calls into loops, and the VM also has constant-stack tail-call opcodes. This is not a blanket constant-stack guarantee for every back end or every indirect/mutually-recursive call: constructors and calls that cannot be proven safe remain ordinary calls. See [§17](#17-compilation-pipeline).

```xray
fn factorial(n: int, acc: int = 1) -> int {
    if (n <= 1) { return acc }
    return factorial(n - 1, acc * n)     // tail call: optimized to a loop
}
```

#### 5.2.8 Program entry point

xray has **no implicit `main` entry**: scripts/modules execute their top level in declaration order. `fn` declarations are hoisted and registered; expressions and statements run immediately.

```xray
// hello.xr
print("loading")          // top-level statement, runs immediately
fn greet() { print("hi") }
greet()                   // must be called explicitly
```

- `fn main()` has no special meaning; call `main()` explicitly if desired.
- Top-level `return` is forbidden (compile error `E0306`).
- Multi-file projects specify the entry with the `main` field under `[project]` (or `[package]` for a package manifest), for example `main = "src/main.xr"`; that file follows the script execution rules above.

#### 5.2.9 `extern "C"` C FFI Declaration Blocks

An `extern "C"` block declares external **function symbols** that share the C ABI. External functions have no Xray body, and calls must appear explicitly inside `unsafe { }`:

```xray
extern "C" {
    fn malloc(n: usize) -> MutPtr<byte>
    fn free(p: MutPtr<byte>)
    fn cos(x: f64) -> f64
}

var p = unsafe { malloc(4) }
unsafe {
    p[0] = 42
    print(cos(0.0))
    free(p)
}
```

Libraries, targets, symbol renames, native sources/objects, typed flags, hashes, and licenses belong to the NativePackagePlan in `xray.toml`, not ordinary source. An `extern "C"` block does not accept `dylib`/`link` clauses, bodies, data/globals, structs, unions, packed layouts, or flexible members.

C ABI aggregates use ordinary Xray structs bound to a C header/type by `[[native.layout]]`. Before linking, the builder compiles static assertions for `sizeof`, `_Alignof`, and every `offsetof`; any mismatch fails closed:

```xray
import mem

struct CHeader {
    tag: u8
    count: u32
}

print(mem.sizeOf<CHeader>())
print(mem.alignOf<CHeader>())
print(mem.offsetOf<CHeader>("count"))
```

Rules:
- The ABI string is mandatory; `"C"` is currently the only supported value.
- Functions inside an extern block declare signatures only and cannot have `{ }` bodies.
- Every declaration must have a unique symbol mapping and a complete typed contract in the NativePackagePlan. A shipping package with missing contract, hash, target, or provenance is rejected.
- C output pointers write raw `Buffer` storage first. A success path may materialize `T` only with `unsafe { mem.assumeInitialized<T>(move buffer) }`, after exact size/alignment, complete output validity, success-path dominance, and header-layout evidence are proven. Partial or failed writes never produce `T`.
- Ordinary Xray functions have no output parameter mode. Returns always use `return value`, never `return move value`.
- Each target has one canonical data layout shared by the analyzer, VM, AOT, Slice/layout queries, and header verifier.
- The aligned VM/AOT boundary types are `bool`, sized integers, `f32` / `f64`, `usize` / `isize`, `Ptr<T>`, `MutPtr<T>`, and `()` returns.
- C callbacks use `CFn<(A, B) -> R>`, not ordinary Xray function types. A `CFn` value must be a module-level, noncapturing Xray function with an exact signature match.

```xray
extern "C" {
    fn bsearch(
        key: Ptr<byte>,
        base: Ptr<byte>,
        count: usize,
        size: usize,
        cmp: CFn<(Ptr<byte>, Ptr<byte>) -> i32>
    ) -> Ptr<byte>
}

fn zeroCmp(a: Ptr<byte>, b: Ptr<byte>) -> i32 {
    return 0
}

// zeroCmp is a module-level noncapturing function and can be used as a CFn callback.
```

#### 5.2.10 Manifest C ABI Exports

A module-level Xray function remains an ordinary `fn` in source. Its C ABI export name and visibility are selected by the typed export plan in `xray.toml`:

```xray
fn add(a: i32, b: i32) -> i32 {
    return a + b
}
```

```toml
[[export.c]]
xray = "add"
symbol = "xr_add_i32"
visibility = "default"
header = true
```

The Xray target must resolve uniquely to a module-level function with a body; methods, closures, nested functions, and extern declarations are rejected. Xray targets and C symbols must be unique. The plan selects exports but never bypasses the ABI verifier or changes VM/ordinary-call semantics. `xray build --native --c-header FILE` emits prototypes for entries with `header = true`; current shared exports are restricted to scalar/raw-pointer boundaries that need no runtime initialization.

#### 5.2.11 Inferred Effects and `xray verify` Contracts

Allocation, error sets, suspension, blocking, panic/abort, and AOT residue are always inferred by the compiler. Source functions do not declare them with effect or zero-cost attributes. Publication and performance gates live in a versioned contract:

```toml
version = 1

[[function]]
symbol = "math.addOne"
scope = "semantic"
requires = ["no_semantic_alloc", "no_throw", "no_suspend"]

[[function]]
symbol = "codec.hotAdd"
scope = "backend"
backend = "aot"
target = "aarch64-apple-darwin"
profile = "release"
requires = ["no_runtime_heap", "no_throw", "no_suspend"]

[function.shape]
forbid = ["runtime_dispatch", "box", "bounds_in_loop", "lane_spill"]
allow = []
```

Run `xray verify --contract perf-contracts.toml`. A contract checks existing semantic/effect evidence and target artifact shape; it never grants optimization permission or changes runtime semantics. A semantic contract may be target-independent, while backend/shape contracts require concrete backend, target, and profile values. Dynamic calls, native unknowns, missing symbols, and incomplete proofs fail closed with a witness.

#### Worked Examples

Closure capture and higher-order functions:

```xray
fn apply(f: (int) -> int, x: int) -> int {
    return f(x)
}

fn main() {
    var base = 10
    var addBase = fn(x: int) -> int { return x + base }   // closure captures base
    print(addBase(5))            // => 15
    print(apply(addBase, 7))     // => 17 (function passed as an argument)
}

main()
```

Multiple return values (a tuple):

```xray
fn divmod(a: int, b: int) -> (int, int) {
    return (a / b, a % b)
}

fn main() {
    var (q, r) = divmod(17, 5)
    print(q)   // => 3
    print(r)   // => 2
}

main()
```

### 5.3 `class` declaration

```ebnf
ClassDecl ::= 'final'? 'class' Identifier TypeParams?
              ('extends' Identifier TypeArgs?)?
              ('implements' Identifier TypeArgs? (',' Identifier TypeArgs?)*)?
              '{' ClassMember* '}'
ClassMember ::= FieldDecl | MethodDecl | ConstructorDecl | StaticBlock
FieldDecl ::= Modifier* Identifier ':' Type ('=' Expression)?
MethodDecl ::= Modifier* Identifier '(' ParamList? ')' ReturnType? Block
            |  Modifier* 'operator' OpToken '(' ParamList? ')' ReturnType? Block
ConstructorDecl ::= 'constructor' '(' ParamList? ')' Block          // parameter types may be omitted
Modifier ::= 'private' | 'protected' | 'static' | 'const'
```

> **About default public visibility and automatic overrides**:
>
> - Public is the **default visibility**—every field/method without `private` / `protected` is public; the language has no `public` modifier.
> - Visibility is **compile-time enforced**: accessing a `private` / `protected` member from outside the class, or a `protected` member from a non-subclass, reports `E0377`.
> - Overrides are inferred by the compiler: a subclass instance method overrides a non-private parent-chain instance method when name and signature match exactly.
> - Interfaces express abstract contracts, same-name same-signature methods override automatically, and same-name different-signature methods or member hiding are compile errors.
>
> The standard library and the regression tests consistently use the "omit the default modifier" style.

#### 5.3.1 Basic class

```xray @id=decl-class-basic
class Animal {
    name: string                       // field
    private _age: int = 0              // private field with default value

    constructor(name: string) {
        this.name = name
    }

    speak() -> string {
        return "..."
    }

    static create(name: string) -> Animal {
        return Animal(name)
    }
}

var a = Animal("Rex")
print(a.speak())
print(Animal.create("Bob").name)
```

#### 5.3.2 Inheritance

```xray @id=decl-class-inheritance
class Dog extends Animal {
    constructor(name: string) {
        super(name)                    // **must** be the first statement (derived classes only)
    }

    speak() -> string {                  // same name and signature: automatic override
        return "woof"
    }
}
```

**Constraints**:
- A derived class constructor's **first statement** must be `super(...)` (unless no constructor is declared); otherwise it is a compile error.
- `this` must not be accessed before `super(...)`.
- **Overriding requires no keyword**—any subclass instance method with the same name and signature automatically overrides the parent.
- Same-name different-signature methods are not overloads or hiding; rename the method or use default arguments / named factories.
- A `final class` cannot be inherited.
- `super.method()` invokes the shadowed parent method from inside an override.

#### 5.3.3 Modifiers

| Modifier | Applies to | Semantics |
|--|--|--|
| (none) | field/method | Default public—externally visible |
| `private` | field/method | Accessible only inside the declaring class (including other instances of the same class); subclass and external access report `E0377` |
| `protected` | field/method | Accessible inside the declaring class and its subclasses; external access reports `E0377` |
| `static` | field/method | Class-level, not part of an instance; called as `ClassName.method()` |
| `const` | field | Immutable field—assignable once via `this` in the declaring class's constructor; later writes report `E0378` |
| `final` | class declaration prefix | `final class C` cannot be inherited; `final` is not used on fields or methods |

**Modifiers may combine**: `private const secret: string = "key123"`, `protected static counter: int = 0`.

> `const` = immutable field/binding, `final class` = cannot be inherited. Immutable fields use `const` only; writing `final` on a field or method is an error.

#### 5.3.4 Constructors

```xray
class Point {
    x: float
    y: float
    constructor(x: float, y: float) {
        this.x = x
        this.y = y
    }
}

// Parameter types may be omitted (inferred from same-named fields)
class Vector2 {
    x: float
    y: float
    constructor(x, y) {         // equivalent to (x: float, y: float)
        this.x = x
        this.y = y
    }
}
```

- The keyword is `constructor` (not `init`, not the class name).
- A class has **at most one constructor** (no overloading); multiple creation paths use `static` factory methods.
- Constructor parameters **may omit their types**—if a parameter shares a name with a field, the type is inferred from that field; otherwise it is inferred from the call-site argument type.
- The constructor implicitly returns `this` (compiler-injected).
- Derived class constructors must call `super(...)` first.
- A `struct` may have **no** constructor (`Point()` produces a zero-initialized instance which is then assigned manually; see §5.4).

#### 5.3.5 Operator overloading

```xray
class Vec2 {
    x: float
    y: float

    constructor(x: float, y: float) {
        this.x = x; this.y = y
    }

    operator+(other: Vec2) -> Vec2 {
        return Vec2(this.x + other.x, this.y + other.y)
    }

    operator==(other: Vec2) -> bool {
        return this.x == other.x && this.y == other.y
    }

    operator[](index: int) -> float {
        if (index == 0) { return this.x }
        return this.y
    }
}
```

**Overloadable operators** (full list, source: `xparse_oop.c`):

| Category | Operators | Arity | Notes |
|--|--|--|--|
| Binary arithmetic | `+` `-` `*` `/` `%` | 1 | A unary `-` with no parameters acts as unary minus |
| Bitwise | `&` `\|` `^` `<<` `>>` | 1 | |
| Comparison | `==` `!=` `<` `<=` `>` `>=` | 1 | Typically implement `==`/`!=` and `<`/`<=`/`>`/`>=` as pairs |
| Indexing | `[]` (getter), `[]=` (setter) | 1 / 2 | The setter is `(index, value)` |
| Unary | `!` `~` `++` `--` | 0 | |
| Compound assignment | `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` | 1 | |

```xray
class Counter {
    n: int = 0
    operator++() -> Counter { this.n = this.n + 1; return this }
    operator+=(other: int) -> Counter { this.n = this.n + other; return this }
    operator[](i: int) -> int { return this.n + i }
    operator[]=(i: int, v: int) { this.n = v - i }
}
```

**Cannot** be overloaded: `&&` `\|\|` `=` `?.` `?[` `?:` `??` `,` `.`

#### 5.3.6 Custom iterators

Implement `iterator()` returning an object with `hasNext() -> bool` and `next() -> T?` to enable `for-in`. See §14.15.

#### 5.3.7 Worked Examples

Self-contained programs that run as-is and pass `xray check` (comments show the real output).

Inheritance with automatic override:

```xray
class Animal {
    name: string
    constructor(name: string) { this.name = name }
    speak() -> string { return "..." }
}

class Dog extends Animal {
    constructor(name: string) { super(name) }
    speak() -> string { return "woof" }   // same name/signature: auto-override
}

fn main() {
    var d = Dog("Rex")
    print(d.name)      // => Rex
    print(d.speak())   // => woof
}

main()
```

Operator overloading (call with named values):

```xray
class Vec2 {
    x: int
    y: int
    constructor(x: int, y: int) { this.x = x; this.y = y }
    operator+(other: Vec2) -> Vec2 {
        return Vec2(this.x + other.x, this.y + other.y)
    }
}

fn main() {
    var a = Vec2(1, 2)
    var b = Vec2(3, 4)
    var sum = a + b
    print(sum.x)   // => 4
    print(sum.y)   // => 6
}

main()
```

### 5.4 `struct` declaration

```ebnf
StructDecl ::= 'struct' Identifier TypeParams?
               ('implements' Identifier (',' Identifier)*)?
               '{' StructMember* '}'
```

```xray @id=decl-struct-point
struct Point {
    x: float
    y: float

    magnitude_sq() -> float {
        return this.x * this.x + this.y * this.y
    }
}

// Two creation styles
var p = Point()                  // default-construct (zero-valued fields), then assign
p.x = 3.0
p.y = 4.0

var q = Point{x: 3.0, y: 4.0}        // struct literal: TypeName + { field: value }
var pt = Point{x: 1.0, y: 2.0}

// Value semantics: assignment and parameter passing copy
var b = q                            // b is an independent copy of q
b.x = 99.0
// q.x is still 3.0
```

**Differences from `class`**:

| Dimension | `class` | `struct` |
|--|--|--|
| Memory model | Reference type (heap) | Value type (stack or inlined) |
| Assign / pass | Shared reference | **Copy** (`var b = a` produces an independent copy) |
| Inheritance | Supports `extends` | **No** inheritance |
| `implements` | ✅ | ✅ |
| Generics | ✅ | ✅ |
| `static` / `private` / `protected` / `const` | ✅ | ✅ |
| Operator overload | ✅ | ✅ |
| Constructor | `constructor(...)` | **Optional**: `Point()` yields a zero-valued instance |
| Literal | none | `TypeName{field: value, ...}` |

**When to use**:
- Math types (Vec2/Vec3/Quat/Color)
- Short-lived values (iterator state, ad-hoc tuples)
- Performance-sensitive data where heap allocation should be avoided

#### 5.4.1 Value-semantics example

A `struct` is a value type: assignment and argument passing copy it.

```xray
struct Point {
    x: int
    y: int
}

fn main() {
    var p = Point{x: 3, y: 4}
    var q = p            // struct assignment copies
    q.x = 99
    print(p.x)           // => 3 (unchanged)
    print(q.x)           // => 99
}

main()
```

### 5.5 `interface` and `implements`

xray's interface implementation is **explicit** (unlike Go's structural implementation): a class/struct must list interfaces with `implements`.

```ebnf
InterfaceDecl ::= 'interface' Identifier TypeParams?
                  ('extends' NamedType (',' NamedType)*)?
                  '{' InterfaceMember* '}'
InterfaceMember ::= Identifier '(' ParamList? ')' ReturnType?       // method signature
                 |  ('const')? Identifier ':' Type                   // property signature (`const` for read-only)
```

```xray @id=decl-interface-shape
interface Shape {
    area() -> float
    perimeter() -> float
}

// Interface method return types may be omitted (default ())
interface Greeter {
    greet(name: string)             // same as greet(name: string) -> ()
    log()                           // no parameters, no return value
}

class Circle implements Shape {
    radius: float
    constructor(r: float) { this.radius = r }
    area() -> float { return 3.14 * this.radius * this.radius }
    perimeter() -> float { return 6.28 * this.radius }
}

// Implement multiple interfaces
class Logger implements Shape, Greeter {
    radius: float
    constructor(r: float) { this.radius = r }
    area() -> float { return 3.14 * this.radius * this.radius }
    perimeter() -> float { return 6.28 * this.radius }
    greet(name: string) { print("hello,", name) }
    log() { print("logging") }
}

fn describe(s: Shape) -> string {
    return "area=${s.area()}, perimeter=${s.perimeter()}"
}
```

**Constraints**:

- Interfaces may extend other interfaces (`extends`); generics (`interface Container<T>`) and constraints (`interface Stats<T: Numeric>`) are supported.
- Classes/structs use `implements I1, I2, ...` to declare implementation of one or more interfaces (**explicit**; structural implementation is not supported).
- The implementing type **must** provide every interface member (matching name/parameters/return type for methods; matching name/type for properties).
- **Return types in interface method declarations may be omitted** (default `()`).
- Interface methods are `abstract` by default (no body).
- Interfaces may declare **property signatures** (`length: int`, `const id: int`); the implementing type must provide a corresponding field.
- Implementing types may add additional methods (the interface defines the minimum surface).

```xray
// property signatures + interface inheritance
interface HasLength {
    length: int
}
interface SizedCollection<T> extends HasLength {
    first() -> T
}

class Buffer implements SizedCollection<int> {
    length: int                       // implements the property signature
    private data: Array<int>
    constructor(n: int) {
        this.length = n
        this.data = []
    }
    first() -> int { return this.data[0] }
}
```

#### Worked Examples

Interface + `implements` + polymorphism:

```xray
interface Shape {
    area() -> float
}

class Circle implements Shape {
    r: float
    constructor(r: float) { this.r = r }
    area() -> float { return 3.14159 * this.r * this.r }
}

fn main() {
    var s: Shape = Circle(2.0)
    print(s.area())   // => 12.56636
}

main()
```

### 5.6 `enum` declaration

xray's `enum` is a **safe tagged aggregate**: each value contains a compiler-assigned declaration-order tag, and only the payload for the active tag may be read. A payload-free enum is just a tagged aggregate whose variants have payload size 0; payload enums are the same model used as a safe sum type.

```ebnf
EnumDecl       ::= 'enum' Identifier TypeParams?
                   ('implements' NamedType (',' NamedType)*)?
                   '{' EnumVariant (',' EnumVariant)* ','? EnumMethod* '}'
EnumVariant    ::= Identifier VariantPayload?
EnumMethod     ::= 'static'? 'fn' Identifier TypeParams? '(' ParamList? ')' ReturnType? Block
VariantPayload ::= '(' VariantField (',' VariantField)* ')'
VariantField   ::= (Identifier ':')? Type
```

> Variant declarations come first (comma-separated); method declarations follow all variants (no commas, separated by block boundaries — same convention as `class` member methods). See §5.6.7.

#### 5.6.1 Simple enums (0-payload enum)

```xray @id=decl-enum-simple
enum Color { Red, Green, Blue }

Color.Red.ordinal     // 0
Color.Blue.ordinal    // 2
Color.Red.name        // "Red"
Color.Red.toString()  // "Color.Red"

enum HttpStatus {
    OK,
    NotFound,
    InternalError

    fn code() -> int {
        return match (this) {
            HttpStatus.OK -> 200,
            HttpStatus.NotFound -> 404,
            HttpStatus.InternalError -> 500
        }
    }
}
```

Enum variants use declaration order for their stable `ordinal` and do not declare a separate backing value. Express protocol numbers, string symbols, and similar external representations with `const`, methods, or explicit conversion functions.

#### 5.6.2 Payload enum

A variant name may be followed by parentheses declaring payload fields (positional or named):

```xray @id=decl-enum-payload
enum Option<T> {
    Some(T),
    None,
}

enum NetEvent {
    Connected,
    Disconnected(reason: string),
    DataReceived(bytes: Array<byte>),
    Error(code: int, message: string),
}

enum Expr {
    Number(int),
    Binary(op: string, left: Box<Expr>, right: Box<Expr>),
    Call(name: string, args: Array<Expr>),
}
```

A directly recursive enum payload would have infinite size and must be rejected at compile time. Recursive data structures need explicit indirection such as `Box<Expr>`, class nodes, or reference-container slots.

#### 5.6.3 Construction and destructuring

Construction:

```xray
var c = Color.Red
var r1 = Option.Some(42)                            // positional payload
var e1 = NetEvent.DataReceived(bytes: b)            // named payload, field name allowed
var e2 = NetEvent.Error(404, "not found")           // field name omitted, positional
var e3 = NetEvent.Connected                         // payload-free variant: no parentheses
```

Destructuring (match):

```xray @id=decl-enum-match
match (event) {
    NetEvent.Connected            -> print("connected"),
    NetEvent.Disconnected(reason) -> print("by:", reason),
    NetEvent.DataReceived(b)      -> process(b),
    NetEvent.Error(code, msg)     -> log.error(code, msg),
}
```

See §6.3.

#### 5.6.4 Enum value API

Instance properties (act on the enum value):

```xray @id=decl-enum-properties
Color.Red.name        // "Red"          variant name (string)
Color.Red.ordinal     // 0              declaration-order tag (int, zero-based)
Color.Red.toString()  // "Color.Red"    "<EnumName>.<VariantName>" format
```

Enum values provide `name`, `ordinal`, and `toString()`. They do not expose implicit backing-value/reflection APIs such as `value`, `rawValue`, `fromName`, or `fromOrdinal`.

#### 5.6.5 Iteration

A concrete enum containing only payload-free variants can be iterated directly as actual enum values. Every concrete enum can expose declaration metadata through `.variants`:

```xray @id=decl-enum-iteration
for (color in Color) {
    print(color.name)                     // color: Color; Red, Green, Blue
}

for (variant in NetEvent.variants) {
    print(variant.ordinal)                // variant: EnumVariant<NetEvent>
    print(variant.name)
    for (field in variant.payloads) {
        print(field.name)                 // field: EnumPayloadField<NetEvent>
        print(field.type)                 // int: canonical TypeId for the concrete field type
    }
}
```

The two loop forms deliberately yield different types:

| Expression | Availability | Loop variable | Meaning |
|---|---|---|---|
| `E` | concrete unit-only enum | `E` | actual enum values in declaration order |
| `E.variants` | any concrete enum | `EnumVariant<E>` | read-only variant descriptors in declaration order |
| `variant.payloads` | an `EnumVariant<E>` | `EnumPayloadField<E>` | read-only payload-field descriptors in declaration order |

If `E` contains any payload variant, `for (value in E)` is a compile error and the diagnostic recommends `E.variants`; the compiler never invents payload values. `for (variant in Color.variants)` is also valid for a unit-only enum, but `variant` remains a descriptor, not a `Color` value. A loop does not print by itself; only explicit effects in its body produce output.

The descriptor API is a closed whitelist:

| Type | Properties / operations |
|---|---|
| `EnumVariants<E>` | `length: int`, checked `[index] -> EnumVariant<E>`, and `for-in` |
| `EnumVariant<E>` | `ordinal: int`, `name: string`, `payloadCount: int`, `isUnit: bool`, `payloads: EnumPayloads<E>` |
| `EnumPayloads<E>` | `length: int`, checked `[index] -> EnumPayloadField<E>`, and `for-in` |
| `EnumPayloadField<E>` | `index: int`, `name: string`, `type: int` (canonical TypeId) |

For a named payload field, `name` is its source declaration name. A positional payload field has no declared name and deterministically reports `""`, not `null`, so the descriptor surface keeps a non-null `string` type.

Users cannot construct these types, descriptors are not callable, and they do not provide name/ordinal-to-value construction. Out-of-range access fails like other checked indexing. Descriptors have no C ABI and are rejected at FFI boundaries.

This facility is a compiler-recognized static type domain, not an `Iterable` conformance. Direct loops and non-escaping descriptors lower to ordinal/index scalars in VM and AOT without allocating an array or iterator. An immutable box is materialized only when a descriptor crosses an identity-requiring boundary such as `any`, an erased union, generic storage, a container, a closure, or a cross-coroutine channel. Use evidence independently retains `.name`, payload-schema, and type-token metadata; unused cold sidecars remain strippable.

An actual unit-only enum value likewise carries only its ordinal on typed paths. Once it crosses a tagged or erased boundary, its immutable static sidecar must retain the enum name and every case name so later `.name`, `toString()`, equality, and generic string formatting remain VM-equivalent. Enums that stay typed emit no such sidecar.

Generic code must identify a concrete enum layout. `Option<int>.variants` is valid; `E.variants` on an unconstrained type parameter is not. Aliases, imports, and separate compilation preserve declaration order and concrete type substitution.

#### 5.6.6 Reverse lookup (value to member)

`Enum(value)` and reverse lookup from backing values are not supported by default. Protocol parsing should be written as an explicit function:

```xray
fn statusFromCode(code: int) -> HttpStatus? {
    if (code == 200) { return HttpStatus.OK }
    if (code == 404) { return HttpStatus.NotFound }
    if (code == 500) { return HttpStatus.InternalError }
    return null
}
```

#### 5.6.7 Enum methods

Instance and static methods may be defined inside `enum` bodies with the same syntax as `class` methods (no `impl` keyword is introduced). Instance methods are callable on every variant; method bodies typically `match (this)` to dispatch on the variant:

```xray
enum Shape {
    Circle(radius: float),
    Rect(w: float, h: float),
    Triangle(a: float, b: float, c: float)

    fn area() -> float {
        return match (this) {
            Shape.Circle(r)     -> 3.14159 * r * r,
            Shape.Rect(w, h)    -> w * h,
            Shape.Triangle(a, b, c) -> {
                var s = (a + b + c) / 2.0
                return (s * (s-a) * (s-b) * (s-c)).sqrt()
            },
        }
    }

    fn isRound() -> bool {
        return match (this) {
            Shape.Circle(_) -> true,
            _               -> false,
        }
    }
}

var s = Shape.Circle(radius: 1.0)
print(s.area())          // 3.14159
print(s.isRound())       // true
```

Static methods use `static fn` and are useful for factories, lookup helpers, and enum-scoped utilities. Static methods do not have `this`:

```xray
enum Color {
    Red, Green, Blue

    static fn fromInt(v: int) -> Color {
        if (v == 1) { return Color.Red }
        if (v == 2) { return Color.Green }
        return Color.Blue
    }

    fn label() -> string {
        return this.name
    }
}

print(Color.fromInt(2).label())     // "Green"
```

> Note that `Triangle(...)` is not followed by a comma — the last variant is separated from the method block by whitespace (a trailing comma is allowed but not required).

**Rules**:

- Method syntax matches `class` methods: `fn name(params) -> ReturnType { body }` or `static fn name(params) -> ReturnType { body }`.
- Inside a method, the static type of `this` is the enum itself (e.g. `Option<T>`); use `match (this)` to extract a variant's payload.
- Static methods have no `this`; call them as `EnumName.method(args...)`.
- `constructor` is **not** supported (variant syntax already serves as the constructor).
- Inheritance is **not** supported (`enum E extends ...` is illegal); for shared behaviour use interface implementation (`enum E implements Iface`) or top-level functions.
- Simple (payload-free) enums may also define methods; inside such methods `this` is the enum value and can be compared directly with `==`:
  ```xray
  enum Color {
      Red, Green, Blue

      fn isWarm() -> bool { return this == Color.Red }
  }
  ```
- Methods **may not** share a name with a variant.

> This design matches Java enums, Swift enums, and Kotlin sealed classes. Rust's `impl` blocks are **not** introduced in xray—xray defines methods inside the type body uniformly.

### 5.7 `type` aliases

```ebnf
TypeAliasDecl ::= 'type' Identifier AliasTypeParams? '=' Type
AliasTypeParams ::= '<' Identifier (',' Identifier)* ','? '>'
```

```xray
type Outcome = int | string                          // union alias
type Mapper = (int) -> int                           // function-type alias
type Point = { x: float, y: float }                  // structural object alias (sealed)
type Pair<T> = { first: T, second: T }                // generic alias
```

**Semantics**:
- An alias is **purely a syntactic** substitution; it does not introduce a new nominal type.
- A generic alias substitutes type arguments at the use site; substitution happens at compile time and introduces no runtime representation or AOT branch.
- Generic alias parameters are only a name list; constraints are not supported. Put constraints on the generic declaration that uses the alias.
- A `type Point = {...}` object alias is **sealed** when used as an annotation: accessing or assigning an undeclared field is a compile error.
- `type T = Json` equals `Json` (not sealed).
- Aliases may be referenced before their declaration but **must not be cyclic**.

See [§2.4.6](#246-json) and [§2.8](#28-type-aliases).

### 5.8 `import` / `export`

See [§11](#11-modules). Syntax highlights:

```xray
// stdlib / third-party packages: bare identifiers; alias auto-derived
import time
import http
import alice/utils as utils

// File path or directory path: string, with explicit `as` or alias derived from the trailing segment
import "./modules/mod_a.xr" as a
import "../utils/string_utils.xr" as utils
import "models/user" as user

// Named import: supports quoted paths or bare module names; members may be renamed with `as`
import { readFile, writeFile as write } from io
import { publicFn } from "./modules/mod_a.xr"

// Exports
export fn publicFn() -> string { return "hi" }
export const VERSION = "1.0"
export { name1, name2 as alias } from "./other"
export * from "./other"
```

**xray does not support** the JavaScript default-import form `import name from "module"`. Use `import "module" as name`, `import module`, or `import { name } from module`.

For full rules, path resolution, and visibility details see [§11 Modules](#11-modules).
<!-- /xr-spec:en -->

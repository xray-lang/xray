---
id: spec.5_declarations
order: 006
---

<!-- xr-spec:cn -->
---

## 5. 声明 (Declarations)

> 真值源：`src/frontend/parser/xparse_decl.c`、`src/frontend/parser/xast_nodes_decl.h`、`src/frontend/analyzer/xanalyzer_visitor.c`。

### 5.1 `var` / `const` / `shared` / `owned`

```ebnf
VarDecl ::= 'var' Binding
ConstDecl ::= 'const' Binding
SharedDecl ::= 'shared' Identifier (':' Type)? '=' Expression
OwnedDecl ::= 'owned' Identifier (':' Type)? '=' Expression
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

#### 5.1.3 `shared` — 共享身份绑定

```xray @id=decl-shared
shared CONFIG = { host: "localhost", port: 8080 }
shared PRIMES = [2, 3, 5, 7, 11]
shared counter = Atomic(0)
```

- 由 **shared/system owner** 直接物化并持有稳定共享身份；具体堆布局不是语言语义。
- 绑定名不可重新赋值，也不能作为 `move` 源。
- 可被 `go` 闭包捕获，也可作为实参跨协程传递；对象本身是否可安全并发修改由类型语义决定。
- `Atomic`、`Channel`、`Semaphore`、`WorkQueue` 等同步/并发句柄必须通过 `shared` 创建命名。

详见 [§10.11](#1011-并发安全模型)。

#### 5.1.4 `owned` — 唯一所有身份绑定

```xray @id=decl-owned
owned buffer = Array<byte>(1024)

var source = [1, 2, 3]
owned moved = move source       // 转移现有引用图
owned cloned = copy(moved)      // 或显式深拷贝
```

- `owned` 只能声明局部单名绑定，必须带初始化器；模块级 `owned` 和解构 `owned` 均被拒绝。
- 绑定由 system owner 记录为唯一可变身份；绑定名本身不可重新赋值，但所拥有对象可以按类型规则原地修改。
- 新构造的引用值可直接初始化。来自已有引用绑定的值必须显式写 `move source` 或 `copy(source)`，避免产生未声明的别名。
- 所有权可通过 `move` 转给另一个 `owned`/`shared` 绑定、协程边界或返回值；移动后原绑定不可再使用。切片、裸指针等借用必须在移动前结束。
- `owned` 是存储声明，不是类型修饰符，也不能带声明 attribute。

#### 5.1.5 解构绑定

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
ParamMode ::= 'in' | 'ref' | 'out'
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

参数模式写在冒号之后、类型之前：`name: in T`、`name: ref T`、`name: out T`。

```xray @id=decl-fn-param-modes
fn length_sq(v: in Vec2) -> float {
    // v 是只读引用（不拷贝，不可修改）
    return v.x * v.x + v.y * v.y
}

fn translate(v: ref Vec2, dx: float, dy: float) -> () {
    // v 是可变引用（修改对调用方可见）
    v.x += dx
    v.y += dy
}
```

| 参数模式 | 语义 |
|--|--|
| 无 | 按值传递（struct 拷贝） |
| `in` | 按只读引用传递（不拷贝、不可写） |
| `ref` | 按可变引用传递（不拷贝、可写、修改可见）；调用点必须写 `ref place` |
| `out` | 按输出位置传递；调用点必须写 `out place`，callee 必须在正常返回前写入 |

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

`extern "C"` 块声明共享 C ABI 的外部函数与原生聚合布局。可选库契约只参与外部函数的符号解析。外部函数没有 xray 函数体，调用点必须显式写在 `unsafe { }` 内：

```xray
extern "C" {
    fn malloc(n: uintsize) -> MutPtr<byte>
    fn free(p: MutPtr<byte>)
}
extern "C" dylib("m") {
    fn cos(x: float64) -> float64
}

var p = unsafe { malloc(4) }
unsafe {
    p[0] = 42
    print(cos(0.0))
    free(p)
}
```

同一语法也可声明由 C 拥有的 struct / union 布局；这些声明是布局身份，不是可构造的 xray 值类型：

```xray
import mem

extern "C" {
    struct CHeader {
        tag: uint8
        count: uint32
        next: MutPtr<byte>
        samples: [uint16; 3]
        payload: flex uint8
    }

    union CWord {
        word: uint32
        bytes: [uint8; 4]
    }
}

print(mem.sizeOf<CHeader>())
print(mem.alignOf<CHeader>())
print(mem.offsetOf<CHeader>("next"))
```

外部地址可在 `unsafe` 中用 `mem.view<T>(ptr)` 投影为该 layout 的 typed C view。投影不分配、不复制，也不创建 xray 对象；字段读写直接作用于外部存储，并继续遵守 `Ptr` / `MutPtr` 的只读/可写区分：

```xray
var header = unsafe { mem.view<CHeader>(rawHeader) }
print(header.count)
unsafe { header.count = 4 }
```

规则：
- ABI 字符串必须显式写出；当前唯一支持值是 `"C"`。
- `dylib("name-or-path")` 指定符号所在动态库；`link("name")` 指定 AOT 系统链接名。二者都进入统一 typed FFI descriptor，未指定时从默认进程/系统路径解析。
- extern 块内函数只能声明签名，不能带 `{ }` 函数体；普通函数必须带块体。
- 需要从模块发布的外部函数或 layout 必须在 extern 块内直接写 `export fn`、`export struct`、`export union` 或 `export packed struct`；已删除的同模块 post-hoc `export { Name }` 不可用于补发布。
- extern 块内可声明 `struct`、`union` 与 `packed struct`。布局字段只能使用 C ABI 标量、`Ptr<T>` / `MutPtr<T>`、标量定长数组，或另一个 extern layout；普通 xray struct、class、`string`、nullable 与其他 managed 类型均被拒绝。
- extern layout 不允许泛型、接口、方法、字段修饰符或字段初始化器；按值嵌套的聚合必须也在 extern 块中声明。
- `flex T` 表示真正的 C flexible array member，只能出现在 extern struct 的最后一个字段，并且之前至少有一个固定字段；extern union 与普通 xray struct 均不允许 `flex`。`sizeOf` 返回已按 struct alignment 补齐的 header size，`offsetOf` 可查询 flexible tail 的起始偏移；tail 不携带隐式长度。
- extern layout 不能用 `T(...)` 或 struct literal 构造为 xray 值；它只定义由外部内存承载的原生布局。`mem.sizeOf<T>()`、`mem.alignOf<T>()` 与 `mem.offsetOf<T>(field)` 使用该原生布局表。
- 每个编译目标只有一份 canonical target data layout。Analyzer、VM、AOT、`mem.view` 和 layout introspection 共用同一份 size/alignment/field-offset 结果；嵌套、packed、union、fixed array 与 flexible tail 均不得由后端再次推导。
- extern layout 随 bytecode 确定性序列化，并绑定目标 ABI fingerprint。加载器必须拒绝 ABI 不匹配、残缺、越界、循环或带尾随垃圾的 layout payload，不能回退到宿主机布局。
- 跨 VM/AOT 后端已收口的边界类型包括 `bool`、精确整数、`float32` / `float64`、`uintsize` / `intsize`、`Ptr<T>`、`MutPtr<T>`，以及 `()` 返回。
- C 回调参数必须写成 `CFn<(A, B) -> R>`，不能使用普通 xray 函数类型 `(A, B) -> R`。
- 当前 `CFn` 实参必须是模块级、非捕获、签名精确匹配的 xray 函数；匿名函数、捕获闭包和 extern 函数本身会被拒绝。

```xray
extern "C" {
    fn bsearch(
        key: Ptr<byte>,
        base: Ptr<byte>,
        count: uintsize,
        size: uintsize,
        cmp: CFn<(Ptr<byte>, Ptr<byte>) -> int32>
    ) -> Ptr<byte>
}

fn zeroCmp(a: Ptr<byte>, b: Ptr<byte>) -> int32 {
    return 0
}

// zeroCmp 是模块级非捕获函数，可作为 CFn 回调。
```

#### 5.2.10 `@c_export` AOT C ABI 导出

`@c_export("symbol")` 把一个模块级 xray 函数额外暴露为 AOT C ABI wrapper。它不改变 xray 源码内的普通函数调用语义；VM 执行该文件时仍把函数当作普通 xray 函数运行，AOT codegen 在生成的 native 产物中额外输出指定 C 符号。

```xray
@c_export("xr_add_i32")
fn add(a: int32, b: int32) -> int32 {
    return a + b
}

print(add(19, 23))        // xray 内部仍是普通函数调用
```

规则：
- `@c_export` 只能标注模块级 `fn` 声明；不能标注 class、struct、方法、匿名函数或嵌套函数。
- `@c_export` 函数必须有 xray 函数体，不能同时是 extern 函数。
- 字符串参数必须是非空 C identifier；该字符串就是导出的 C 符号名。
- 同一个 AOT bundle 中每个 `@c_export` 符号名必须唯一；重复符号是编译错误。
- 当前支持的导出边界类型是 `bool`、精确整数、`float32` / `float64`、`uintsize` / `intsize`、`Ptr<T>`、`MutPtr<T>`，以及 `()` 返回。
- 当前不导出 xray 管理值（如 `string`、class instance、Array/Map/Set、普通 closure）或 by-value aggregate；需要与 C 共享结构体内存时，先通过 `Ptr<T>` / `MutPtr<T>` 传递地址。
- `@c_export` 定义函数 ABI wrapper；`xray build --native --c-header FILE` 可为这些 wrapper 生成 C 原型头文件，`xray build --native --shared --c-header FILE` 可生成 native shared library 和匹配头文件。
- `--shared` 当前只支持无需 Xray runtime 初始化的 scalar / raw pointer 导出；runtime-backed 特性、managed ownership、aggregate by-value 和初始化/关闭策略仍由后续 FFI 任务定义。

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

enum 值提供 `name`、`ordinal` 与 `toString()`。需要遍历全部 case 时，应使用显式生成 metadata 的 `CaseIterable` 风格能力。

#### 5.6.5 遍历

enum 默认不可 `for-in` 遍历：

```xray @id=decl-enum-iteration
for (c in Color) { print(c.name) }        // 编译错误
```

原因是 case 列表属于可裁剪 metadata，不应成为每个 enum 的默认 runtime 对象能力。需要 case iteration 时应显式 opt-in，由编译器生成只读 case 表。

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

### 5.1 `var` / `const` / `shared` / `owned`

```ebnf
VarDecl ::= 'var' Binding
ConstDecl ::= 'const' Binding
SharedDecl ::= 'shared' Identifier (':' Type)? '=' Expression
OwnedDecl ::= 'owned' Identifier (':' Type)? '=' Expression
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

#### 5.1.3 `shared` — shared identity binding

```xray @id=decl-shared
shared CONFIG = { host: "localhost", port: 8080 }
shared PRIMES = [2, 3, 5, 7, 11]
shared counter = Atomic(0)
```

- Materialized directly under the **shared/system owner** as a stable shared identity; the concrete heap layout is not language semantics.
- The binding name cannot be reassigned and cannot be used as a `move` source.
- It may be captured by `go` closures and passed across coroutine boundaries directly; concurrent mutation safety comes from the value's own type semantics.
- Synchronization/concurrency handles such as `Atomic`, `Channel`, `Semaphore`, and `WorkQueue` must be created with `shared`.

See [§10.11](#1011-concurrency-safety-model).

#### 5.1.4 `owned` — unique-ownership identity binding

```xray @id=decl-owned
owned buffer = Array<byte>(1024)

var source = [1, 2, 3]
owned moved = move source       // transfer an existing reference graph
owned cloned = copy(moved)      // or make an explicit deep copy
```

- `owned` is a local, single-name declaration and requires an initializer; module-level and destructuring `owned` declarations are rejected.
- The system owner records the binding as a unique mutable identity. The name itself is not reassignable, while the owned object may be mutated according to its type.
- A freshly constructed reference value may initialize it directly. A value taken from an existing reference binding must be written as `move source` or `copy(source)` so that no undeclared alias is created.
- Ownership may be transferred with `move` to another `owned`/`shared` binding, across an execution boundary, or through a return value. The source cannot be used after the move, and borrows such as slices/raw pointers must have ended first.
- `owned` is a storage declaration, not a type modifier, and it cannot carry declaration attributes.

#### 5.1.5 Destructuring bindings

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
ParamMode ::= 'in' | 'ref' | 'out'
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

Parameter modes are written after the colon and before the type: `name: in T`, `name: ref T`, `name: out T`.

```xray @id=decl-fn-param-modes
fn length_sq(v: in Vec2) -> float {
    // v is a read-only reference (no copy, not writable)
    return v.x * v.x + v.y * v.y
}

fn translate(v: ref Vec2, dx: float, dy: float) -> () {
    // v is a mutable reference (changes are visible to the caller)
    v.x += dx
    v.y += dy
}
```

| Parameter mode | Semantics |
|--|--|
| (none) | Pass by value (struct copy) |
| `in` | Pass by read-only reference (no copy, not writable) |
| `ref` | Pass by mutable reference (no copy, writable, observable to caller); the call site must write `ref place` |
| `out` | Pass an output place; the call site must write `out place`, and the callee must write before normal return |

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

An `extern "C"` block declares external functions and native aggregate layouts that share a C ABI. An optional library contract participates only in external-function symbol resolution. An external function has no xray function body, and call sites must be written explicitly inside `unsafe { }`:

```xray
extern "C" {
    fn malloc(n: uintsize) -> MutPtr<byte>
    fn free(p: MutPtr<byte>)
}
extern "C" dylib("m") {
    fn cos(x: float64) -> float64
}

var p = unsafe { malloc(4) }
unsafe {
    p[0] = 42
    print(cos(0.0))
    free(p)
}
```

The same syntax declares C-owned struct / union layouts. These declarations are layout identities, not constructible xray value types:

```xray
import mem

extern "C" {
    struct CHeader {
        tag: uint8
        count: uint32
        next: MutPtr<byte>
        samples: [uint16; 3]
        payload: flex uint8
    }

    union CWord {
        word: uint32
        bytes: [uint8; 4]
    }
}

print(mem.sizeOf<CHeader>())
print(mem.alignOf<CHeader>())
print(mem.offsetOf<CHeader>("next"))
```

Inside `unsafe`, an external address can be projected with `mem.view<T>(ptr)` as a typed C-layout view. The projection allocates and copies nothing and does not create an xray object; field reads and writes directly access foreign storage while preserving the readonly/mutable distinction of `Ptr` and `MutPtr`:

```xray
var header = unsafe { mem.view<CHeader>(rawHeader) }
print(header.count)
unsafe { header.count = 4 }
```

Rules:
- The ABI string is mandatory; `"C"` is currently the only supported value.
- `dylib("name-or-path")` selects a dynamic library, while `link("name")` selects an AOT system link name. Both feed the same typed FFI descriptor; without either, resolution uses the default process/system lookup path.
- Functions inside an extern block may only declare signatures and cannot have `{ }` bodies; ordinary functions require block bodies.
- An external function or layout published by the module must use direct visibility inside the extern block: `export fn`, `export struct`, `export union`, or `export packed struct`. The removed same-module post-hoc `export { Name }` form cannot publish it afterward.
- An extern block may declare `struct`, `union`, and `packed struct` layouts. Layout fields may contain only C ABI scalars, `Ptr<T>` / `MutPtr<T>`, fixed arrays of scalars, or another extern layout. Ordinary xray structs, classes, `string`, nullable types, and other managed types are rejected.
- Extern layouts cannot have generics, interfaces, methods, field modifiers, or field initializers. Any aggregate nested by value must itself be declared as an extern layout.
- `flex T` is a real C flexible array member. It may appear only as the last field of an extern struct and requires at least one preceding fixed field; extern unions and ordinary xray structs reject `flex`. `sizeOf` returns the header size padded to the struct alignment, while `offsetOf` can query the flexible tail's starting offset. The tail carries no implicit length.
- An extern layout cannot be constructed as an xray value with `T(...)` or a struct literal; it only describes native storage owned outside xray. `mem.sizeOf<T>()`, `mem.alignOf<T>()`, and `mem.offsetOf<T>(field)` consume the native layout table.
- Each compilation target has one canonical target data layout. The analyzer, VM, AOT backend, `mem.view`, and layout introspection share the same size/alignment/field-offset results; nested, packed, union, fixed-array, and flexible-tail layouts are never re-derived by individual backends.
- Extern layouts are serialized deterministically with bytecode and bound to a target-ABI fingerprint. The loader rejects ABI mismatches and truncated, out-of-range, cyclic, or trailing-garbage layout payloads; it never falls back to host layout.
- Boundary types that are aligned across the VM/AOT backends include `bool`, sized integers, `float32` / `float64`, `uintsize` / `intsize`, `Ptr<T>`, `MutPtr<T>`, and `()` returns.
- C callback parameters must use `CFn<(A, B) -> R>`, not the ordinary xray function type `(A, B) -> R`.
- A current `CFn` argument must be a module-level, noncapturing xray function with an exact signature match; anonymous functions, capturing closures, and extern functions themselves are rejected.

```xray
extern "C" {
    fn bsearch(
        key: Ptr<byte>,
        base: Ptr<byte>,
        count: uintsize,
        size: uintsize,
        cmp: CFn<(Ptr<byte>, Ptr<byte>) -> int32>
    ) -> Ptr<byte>
}

fn zeroCmp(a: Ptr<byte>, b: Ptr<byte>) -> int32 {
    return 0
}

// zeroCmp is a module-level noncapturing function and can be used as a CFn callback.
```

#### 5.2.10 `@c_export` AOT C ABI Exports

`@c_export("symbol")` additionally exposes a module-level xray function as an AOT C ABI wrapper. It does not change ordinary xray call semantics; the VM still runs the function as a normal xray function, while AOT codegen emits the requested C symbol in the generated native artifact.

```xray
@c_export("xr_add_i32")
fn add(a: int32, b: int32) -> int32 {
    return a + b
}

print(add(19, 23))        // still an ordinary xray call inside xray
```

Rules:
- `@c_export` may only annotate a module-level `fn` declaration; it cannot annotate classes, structs, methods, anonymous functions, or nested functions.
- A `@c_export` function must have an xray function body and cannot also be an extern function.
- The string argument must be a non-empty C identifier; that string is the exported C symbol name.
- Each `@c_export` symbol name must be unique within one AOT bundle; duplicate symbols are compile errors.
- Currently supported export boundary types are `bool`, sized integers, `float32` / `float64`, `uintsize` / `intsize`, `Ptr<T>`, `MutPtr<T>`, and `()` returns.
- Managed xray values such as `string`, class instances, Array/Map/Set, ordinary closures, and by-value aggregates are not exported directly today. To share struct memory with C, pass an address through `Ptr<T>` / `MutPtr<T>`.
- `@c_export` defines the function ABI wrapper; `xray build --native --c-header FILE` can emit a C prototype header for those wrappers, and `xray build --native --shared --c-header FILE` can emit a native shared library with a matching header.
- `--shared` currently supports only scalar / raw pointer exports that do not require Xray runtime initialization; runtime-backed features, managed ownership, aggregate by-value, and initialization/shutdown policy remain future FFI work.

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

Enum values provide `name`, `ordinal`, and `toString()`. Code that needs to iterate all cases should use an explicit generated-metadata capability in the style of `CaseIterable`.

#### 5.6.5 Iteration

Enums are not iterable by default:

```xray @id=decl-enum-iteration
for (c in Color) { print(c.name) }        // compile error
```

The case list is strippable metadata and should not become a default runtime object capability for every enum. Case iteration should be explicit opt-in and compiler-generated when needed.

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

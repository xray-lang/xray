---
id: spec.3_expressions
order: 004
---

<!-- xr-spec:cn -->
---

## 3. 表达式 (Expressions)

> 真值源：`src/frontend/parser/xparse_expr.c`、`src/frontend/parser/xast_types.h` 的 `AST_BINARY_*` / `AST_UNARY_*` / `AST_TERNARY` / `AST_*` 等节点。

### 3.1 优先级与结合性

完整优先级表（自**高至低**；同级运算符按结合性分组）：

| 级 | 运算符 | 结合性 | 说明 |
|--|--|--|--|
| 17 | `(...)` `[...]` `.x` `?.x` `?[...]` `f()` `e!` | 左 | 后缀：分组、索引、成员、可选链、调用、强制解包 |
| 16 | 前缀 `-` `+` `!` `~` `move` `await` `go` `unsafe` | 右 | 一元前缀 + 协程/FFI 边界操作 |
| 15 | `as` `is` | 左 | 类型转换 / 检查（`as T?` 安全形式靠目标类型可空，非独立 `as?` 运算符） |
| 14 | `*` `/` `%` | 左 | 乘除取模 |
| 13 | `+` `-` | 左 | 加减 |
| 12 | `<<` `>>` | 左 | 移位 |
| 11 | `<` `<=` `>` `>=` | 左 | 关系比较 |
| 10 | `==` `!=` | 左 | 相等比较 |
| 9 | `&` | 左 | 位与 |
| 8 | `^` | 左 | 位异或 |
| 7 | `\|` | 左 | 位或（亦用于 union 类型） |
| 6 | `&&` | 左 | 逻辑与（短路） |
| 5 | `\|\|` | 左 | 逻辑或（短路） |
| 4 | `??` | 左 | 空值合并 |
| 3 | `..` `..=` | 左 | 范围 |
| 2 | `? :` | 右 | 三元 |
| 1 | `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` | 右 | 赋值与复合赋值 |
| 0 | `,`（仅 `match` 多值、参数列表等特定位置）| — | 不是真正运算符 |

实现：`src/frontend/parser/xparse_expr.c` 的 Pratt-parser 风格。

### 3.2 一元表达式

```ebnf
UnaryExpr ::= ('-' | '+' | '!' | '~') UnaryExpr
            | 'move' UnaryExpr
            | 'await' ('all' | 'any')? UnaryExpr
            | 'go' (Block | PostfixExpr)
            | 'unsafe' Block
            | PostfixExpr
```

| 运算符 | 适用类型 | 结果类型 | 备注 |
|--|--|--|--|
| `-x` | 数值 | 同 | 负号；浮点 NaN 保留 |
| `+x` | 数值 | 同 | 标识，几乎无用 |
| `!x` | `bool` | `bool` | 逻辑非；**不接受非 bool**（不像 JS） |
| `~x` | 整数 | 同 | 按位取反 |
`++` / `--` **不是表达式**：`let y = x++`、`f(x++)`、`a[i++]`、`return x++` 等表达式位置均编译报错。语句级自增/自减见 §4.1。

#### `unsafe { }`

`unsafe { ... }` 是显式 FFI/裸指针边界表达式。块内允许调用 `@extern` 函数、读取/写入 `RawPtr<T>` / `RawMut<T>` 指向的外部内存，以及调用需要裸指针解引用的 `deref()`。

```xray
@extern("C") fn malloc(n: uintsize) -> RawMut<uint8>
@extern("C") fn free(p: RawMut<uint8>)

let p = unsafe { malloc(1) }      // 块的最后一个表达式作为结果
unsafe {
    p[0] = 7                      // RawMut 写入必须在 unsafe 内
    print(p.deref())              // 解引用必须在 unsafe 内
    free(p)                       // @extern 调用必须在 unsafe 内
}
```

`unsafe` 不改变表达式的结果类型；多语句块的最后一个表达式语句产生块值，否则结果为 `()`。`unsafe` 也不关闭普通类型检查：`RawPtr<T>` 仍不可写，`RawMut<T>` 才能写入；空指针、越界、生命周期和对齐由调用方负责。

### 3.3 二元表达式

```ebnf
BinaryExpr ::= UnaryExpr (BinOp UnaryExpr)*
BinOp ::= '+' | '-' | '*' | '/' | '%'
       | '&' | '|' | '^' | '<<' | '>>'
       | '<' | '<=' | '>' | '>='
       | '==' | '!='
       | '&&' | '||'
       | '??'
```

#### 3.3.1 算术运算符

| 运算符 | int×int | float×float | int×float | string | 其他 |
|--|--|--|--|--|--|
| `+` | int | float | float（int→float 提升） | 字符串拼接 | ❌ |
| `-` | int | float | float | ❌ | ❌ |
| `*` | int | float | float | ❌ | ❌ |
| `/` | int（整除） | float | float | ❌ | ❌ |
| `%` | int | ❌ | ❌ | ❌ | ❌ |

**特殊语义**：
- `int / 0` → 运行时抛 `XR_ERR_DIV_BY_ZERO` (E0420)。
- `int % 0` → 运行时抛 `XR_ERR_MOD_BY_ZERO` (E0421)。
- 结果类型为 `float`/`float32` 的除法遵循 IEEE-754：`1.0 / 0.0` 产生 `+inf`，`-1.0 / 0.0` 产生 `-inf`，`0.0 / 0.0` 产生 `NaN`；可用 `x.isNaN()` 或 `math.isNaN(x)` 检测 NaN。
- `%` 仅接受整数操作数；静态类型包含 float 的求模（如 `5.0 % 2.0`）在分析期编译错误。运行时 `XR_ERR_TYPE_MISMATCH` (E0404) 仅作为动态兜底。
- 整数溢出：见 §2.3.1。
- 字符串 `+ string` 是 O(n) 拼接；密集拼接请用 `StringBuilder`。
- `char` 是独立的 Unicode scalar 类型，不参与算术；需要码点时显式写 `int(c)`。

#### 3.3.2 位运算

`&` `|` `^` `~` `<<` `>>`

- 仅作用于整数类型。
- 移位计数取模 64（与 C 不同：xray 总是定义的）。
- `>>` 是**算术右移**（保留符号位）。无符号类型用对应的 `uintN`。
- bool 不参与位运算（用 `&&` `||`）。
- `char` 不参与位运算；需要码点时显式写 `int(c)`。

#### 3.3.3 比较运算符

| 运算符 | 语义 |
|--|--|
| `==` | 值相等。`int` 与 `float` 可比较（int→float 隐式）。字符串按内容比较。class/struct 使用 `==` 重载或默认 identity。 |
| `!=` | `==` 的反 |
| `<` `<=` `>` `>=` | 数值与字符串支持；其他类型默认不支持（可通过 `operator<` 重载启用）。 |

**与 JS 的区别**：xray 的 `==` **不**做 string↔number 转换；只做数值之间的 int↔float 提升。

#### 3.3.4 逻辑运算符

`&&` `||`：

- 两操作数**必须**是 `bool` 类型（编译期检查）。
- 短路求值：`false && X` 不求值 `X`；`true || X` 不求值 `X`。
- 结果类型 `bool`（不像 JS 返回操作数）。

#### 3.3.5 空值合并 `??`

```xray @id=expr-null-coalesce
let v = nullable_expr ?? default_value
```

- 当 `nullable_expr` 为 `null` 时返回 `default_value`，否则返回 `nullable_expr` 本身。
- **短路**：`default_value` 只在前者为 null 时求值。
- 类型推导：若 `nullable_expr: T?` 且 `default_value: T`，结果类型 `T`（非空）。
- 仅作用于可空类型；对 `T`（非空）使用 `??` 是编译警告/错误。

### 3.4 赋值与复合赋值

```ebnf
AssignExpr ::= LValue AssignOp Expression
LValue ::= Identifier | MemberAccess | IndexAccess
AssignOp ::= '=' | '+=' | '-=' | '*=' | '/=' | '%='
           | '&=' | '|=' | '^=' | '<<=' | '>>='
```

**语义**：
- 赋值是**表达式**，结果是赋值后的值（可链式：`a = b = 0`）。
- `x op= y` 等价于 `x = x op y`，但 `x` 只求值一次（重要：`obj.f += 1` 不会调用 `f` 的 getter 两次）。
- 不能赋值给 `const`（编译错误 `E0303`）。
- 不能赋值给 `shared const`（同上）。

**特殊**：
- 函数参数中的 `in T` 修饰符使参数变只读，对其赋值是编译错误。
- 数组/Map 字面量字段：`a[i] = v` 调用 `operator[]=` 或内置 setter。

### 3.5 三元 `? :`

```ebnf
TernaryExpr ::= LogicOrExpr ('?' Expression ':' Expression)?
```

```xray
let max = a > b ? a : b
```

- **右结合**：`a ? b : c ? d : e` = `a ? b : (c ? d : e)`。
- 条件必须是 `bool`。
- 两分支类型统一：取共同超类型（或 union）。

### 3.6 空合并 `??` 与可选链 `?.` / `?[`

详见 §3.3.5（`??`）与下方（`?.` / `?[`）。

#### 可选链 `?.` / `?[`

```ebnf
OptionalChain ::= Primary ('?.' Identifier | '?.' '(' ArgList? ')' | '?[' Expr ']')+
```

```xray @id=expr-optional-chain
let len = name?.length          // null 时返回 null
let item = arr?[0]              // 可选索引
let value = callback?.(input)   // 可选函数调用
```

**语义**：
- 若 `?.` 或 `?[` 左侧为 `null`，整个表达式短路返回 `null`。
- `?.` 用于属性访问、方法调用和函数调用：`obj?.prop`、`obj?.method()`、`func?.(args)`。
- `?[` 用于索引访问：`arr?[0]`。与普通索引 `arr[0]` 对称，只需在 `[` 前加 `?`。
- `func?.(args)` 在函数值为 `null` 时不求值实参，直接返回 `null`。
- **传播**：`a?.b.c.d` 中，若 `a` 为 null，整个链返回 null；中间 `.` 不重新检查。
- 结果类型：原类型加 `?`（若已经 `?` 则保持）。

### 3.7 强制解包 `!`

> 完整错误处理语义见 §8。本节只列表达式语法与简要语义。

#### 强制解包 `expr!`

```xray
let v: int = nullable_int!      // null 时运行时抛 NullThrowError (E0410)
```

仅当编译期可确定 `expr` 是可空类型 (`T?`) 时合法；对非空类型 `T` 使用 `!` 是编译错误。

### 3.8 `as` / `is`

#### `is` 运行时类型检查

```ebnf
IsExpr ::= UnaryExpr 'is' Type
```

```xray
if (v is User) {
    // 此分支内 v 窄化为 User
    print(v.name)
}
```

- 结果类型 `bool`。
- **类型守卫**：分析器在分支内窄化 `v` 的静态类型。
- 适用于 union、可空、class 层级、`Json` 结构匹配。

#### `as` 类型转换

```ebnf
AsExpr ::= UnaryExpr 'as' Type
        |  UnaryExpr 'as' Type '?'
```

```xray
let n = v as int           // 失败抛 TypeError
let n = v as int?          // 失败返回 null（"as nullable" 安全形式）
```

| 形式 | 失败行为 | 用途 |
|--|--|--|
| `expr as T` | 抛 `XR_ERR_TYPE_MISMATCH` (E0404) | 必须成功的转换 |
| `expr as T?` | 返回 `null` | 不确定能否转换的尝试 |

**支持的转换**：
- 数值之间（`int → float` 不丢失，`float → int` 截断）。
- `Json → T`（运行时按 T 结构检查）。
- 父类 → 子类（运行时 instanceof）。
- Union 成员 → 具体成员。

### 3.9 范围 `..` / `..=` 与展开 `...`

#### 范围 `a..b` / `a..=b`

```ebnf
RangeExpr ::= AddExpr (('..' | '..=') AddExpr)?
```

```xray @id=expr-range
0..10                  // 0..10，左闭右开（包含 0，不包含 10）
0..=10                 // 0..=10，闭区间（包含 0 和 10）
let r = 1..100
let n = 10
for (i in 0..n) { print(i) }
for (i in 0..=n) { print(i) }
```

- 类型 `Range`（仅 int 范围）。
- `a..b` 是半开区间 `[a, b)`：`a` 包含、`b` 不包含。
- `a..=b` 是闭区间 `[a, b]`：两端都包含。
- `for-in`、`Range.includes`、`Range.length`、`Range.toArray()`、`match` 中的范围模式全部遵循对应端点语义。
- 主要用途：`for-in` 循环、模式匹配中的范围判定。

#### 展开 `...`

仅在以下位置使用：
- **函数 rest 参数声明**：`fn f(...args: int)`
- **函数调用展开**：`f(...args)`，展开源必须是静态 arity 已知的 tuple。
- **tuple 字面量展开**：`(head, ...tail)`，展开源必须是静态 arity 已知的 tuple。
- **数组字面量展开**：`[...a, x, ...b]`，展开源必须是数组，运行期拼接成新数组（O(n)）。
- **对象/record 字面量展开**：`{...base, x: 1}`，展开源必须是对象；字段合并成新对象，同名字段后者覆盖前者，结果字段集为各展开源字段与字面量字段的并集。

```xray @id=expr-spread-collections
let a = [1, 2]
let b = [3, 4]
let nums = [...a, 99, ...b]            // [1, 2, 99, 3, 4]

let base = { x: 1, y: 2 }
let point = { ...base, y: 20, z: 3 }   // { x: 1, y: 20, z: 3 }
```

### 3.10 字面量构造

#### Array `[...]`

```ebnf
ArrayLit  ::= '[' (ArrayElem (',' ArrayElem)* ','?)? ']'
ArrayElem ::= '...' Expr | Expr
```

```xray @id=expr-array-lit
let a = [1, 2, 3]
let empty: Array<int> = []
let mixed = [1, "hello"]    // 类型 Array<int | string>
```

#### Map `#{k: v, ...}` 与 `#{}`

```ebnf
MapLit   ::= '#{' (MapEntry (',' MapEntry)* ','?)? '}'
MapEntry ::= Expression ':' Expression
EmptyMap ::= '#{' '}'    // 注意：'#{' 是单个 token
```

```xray @id=expr-map-lit
let m = #{"a": 1, "b": 2}
let empty = #{}                           // 空 Map
```

**关键区别**：`{}` 始终是**Json / Object**；`#{}` 始终是 **Map**。两者都用 `:` 作键值分隔，靠 `#` 前缀区分。

#### Set `#[...]`

```ebnf
SetLit ::= '#[' (Expr (',' Expr)* ','?)? ']'
```

```xray @id=expr-set-lit
let s = #[1, 2, 3]
let empty = #[]
```

#### Object（结构化对象）`{ field: value, ... }`

```ebnf
ObjectLit  ::= '{' ObjectField (',' ObjectField)* ','? '}'
ObjectField ::= Identifier ':' Expr
              | Identifier            // shorthand: `{ x }` 等价 `{ x: x }`
              | '...' Expr            // spread: `{ ...base }` 合并字段
```

```xray @id=expr-object-lit
let p = { name: "Alice", age: 30 }
let users = "Bob"
let obj = { users }              // shorthand
```

- 默认推断为**可扩展**的结构化对象类型（见 §2.4.6 / §2.10 Json 行为）。
- 用 `type` 别名固化结构：`let u: User = {...}`（编译期检查字段集，密封）。

#### Bytes `Bytes(...)`

详见 §2.4.5 与 §14.5。

#### Channel `Channel<T>(buf?)`

```xray
const ch: Channel<int> = Channel<int>(10)
```

详见 §10.5。

### 3.11 调用 / 成员访问 / 索引 / 切片

#### 函数调用

```ebnf
CallExpr ::= Primary '(' ArgList? ')'
ArgList ::= Expr (',' Expr)* ','?
```

- 参数按位置传递；不支持命名参数。
- rest 参数收集多余参数到数组。
- 参数计数不匹配 → 编译错误 `E0307` / `E0450`。

#### 成员访问

```ebnf
MemberAccess ::= Primary '.' Identifier
```

```xray
obj.field
obj.method(args)
ClassName.staticMethod()
EnumName.MemberName
```

- 字段访问：编译期检查类型有此字段。
- 方法调用：解析为 invoke（带 IC 缓存优化）。
- 模块成员：`module.export_name`。
- 枚举成员：`Color.Red`。

#### 索引访问

```ebnf
IndexAccess ::= Primary '[' Expr ']'
```

```xray @id=expr-index-access
arr[0]
arr[0] = 10
map["key"]
str[i]                  // 返回 char
```

- `Array` 索引：`int`，越界抛 `E0430`。
- `Map` 索引：键类型；找不到键 → `E0431`。
- `string` 索引：按 Unicode scalar 下标访问，返回 `char`。
- 自定义类：通过 `operator[]` 重载。

#### 切片

```ebnf
Slice ::= Primary '[' Expr? ':' Expr? ']'
```

```xray @id=expr-slice
arr[1:4]                // 元素 [1,4)
arr[:3]                 // 前 3 个
arr[2:]                 // 从索引 2 到末尾
arr[:]                  // 全切片（浅拷贝）
str[0:5]                // 字符串切片
```

- 半开区间 `[start, end)`。
- `Array` 与 `string` 切片统一支持负索引：负数先按 `length + index` 从末尾计数，再夹到 `[0, length]`。
- 切片返回新对象，不修改原数组。

### 3.12 匿名函数与 Lambda

xray 有三种匿名函数语法，全部编译为相同的 `AST_FUNCTION_EXPR` 节点，语义完全等价，仅在简洁度和适用场景上有区别。

```ebnf
AnonFunction ::= BareLambda | ArrowLambda | FnExpression
BareLambda   ::= Identifier '->' (Expression | Block)
ArrowLambda  ::= '(' ArrowParams? ')' '->' (Expression | Block)
ArrowParams  ::= ArrowParam (',' ArrowParam)*
ArrowParam   ::= Identifier (':' Type)?      // 类型可省略，由上下文推断
FnExpression ::= 'fn' GenericParams? '(' Params ')' ('->' Type)? Block
```

```xray @id=expr-lambda-forms
// ── 裸 lambda：最简洁，仅限调用参数位置 ──
arr.map(x -> x * 2)
arr.filter(x -> x % 2 == 0)

// ── 箭头 lambda：任意位置，支持多参数和参数类型注解 ──
let sum = arr.reduce((acc, x) -> acc + x, 0)    // 无类型
let double = (x: int) -> x * 2                   // 有类型
let add = (a: int, b: int) -> a + b              // 多参数

// ── fn 表达式：多语句体、返回类型注解、泛型参数 ──
let inc = fn(x: int) -> int {
    let y = x + 1
    return y
}
let identity = fn<T>(x: T) -> T { return x }     // 泛型
```

**三种形式的选择指南**：

| 形式 | 语法 | 适用场景 |
|------|------|----------|
| 裸 lambda | `x -> expr` | 单参数回调，最简洁 |
| 箭头 lambda | `(x, y) -> expr` | 多参数、需参数类型注解、或非调用参数位置 |
| fn 表达式 | `fn(x: T) -> R { ... }` | 多语句体、返回类型注解、泛型参数 |

**关键规则**：
- **裸 lambda**（`x -> expr`）：仅限**调用参数位置**，单参数无括号。参数类型由被调函数签名或容器元素类型推断。
- **箭头 lambda**（`(x) -> expr`、`(x, y) -> expr`）：任意位置可用。参数类型可省略，由上下文推断；推断失败时报 E0365。箭头 lambda **不支持返回类型注解**；需要显式返回类型时，用 `fn(x: T) -> R { ... }`，或给绑定写函数类型：`let f: (T) -> R = (x) -> ...`。
- **fn 表达式**（`fn(x: T) { ... }`）：任意位置可用。支持泛型参数 `fn<T>(...)`、返回类型注解 `-> T`、多语句体。
- 单表达式形式 `-> expr` 自动 `return`。
- 块形式 `-> { ... }` 或 `{ ... }` 用显式 `return`。
- 捕获规则：见 §7.4 闭包捕获。**`go` 协程闭包对 `let` 变量的捕获是编译错误**——必须显式 `shared const`、`move`、或参数传递。

### 3.13 `match` 表达式

```ebnf
MatchExpr ::= 'match' Expr '{' MatchArm (',' MatchArm)* ','? '}'
MatchArm ::= Pattern ('if' Expr)? '->' Expression
```

```xray @id=expr-match
let result = match (x) {
    1 -> "one",
    2, 3, 4 -> "few",                 // 多值
    10..20 -> "teen",                 // 范围
    n if (n > 100) -> "big",          // 守卫
    Color.Red -> "red",               // 枚举
    is User -> "a user",              // 类型模式
    _ -> "default"                    // 通配符
}
```

**语义**：
- 自上而下匹配第一个成功的分支。
- 所有分支表达式必须返回相同类型（或 union）。
- **穷举性**：对 enum 变量（ADT 与简单枚举）编译器强制穷举。对其他表达式不强制，运行时无匹配抛 `PanicInfo(E0442)`。
- 模式详见 [§6](#6-模式-patterns)。

### 3.14 构造表达式

```ebnf
ConstructExpr ::= Identifier TypeArgs? '(' ArgList? ')'
```

构造与普通函数调用同形：`TypeName(args)`。没有 `new` 关键字——写出 `new`（如 `new Point(...)`）是编译错误，提示删除 `new`。

```xray @id=expr-new
let p = Point(1.0, 2.0)
let arr = Array<int>()
let ch = Channel<int>(10)
let m = Map<string, int>()
```

**用于**：
- 类与 struct 实例化（`TypeName(args)`）。
- 容器内置类型构造（`Array`/`Map`/`Set`/`Channel`/`Bytes`/`StringBuilder` 等，同样是 `TypeName(args)`）。
- 消歧由 analyzer 按符号种类判定：类型名构造，函数名调用（命名约定：类型大写、函数小写）。

**与字面量的关系**：
```xray @id=expr-literal-constructor-relation
let a = [1, 2, 3]              // 等价 Array<int>() + push
let m = #{}                    // 等价 Map<...>()
let p = Point{x: 1, y: 2}      // struct literal
```

### 3.15 字符串插值

详见 §1.6.5。简要：

```xray @id=expr-string-interpolation
"Hello, ${name}! Age: ${user.age + 1}"
```

- `${...}` 内任意表达式（含函数调用、对象访问、算术）。
- `${...}` 内的字符串字面量可使用与外层模板相同的引号；lexer 按表达式大括号深度匹配，并跳过内层字符串 / raw string / char 字面量。
- 表达式类型必须可转为字符串（实现 `toString()` 或为基本类型）。

### 3.16 `yield` 语句

```xray @id=expr-yield
yield expr                  // 生成器产出一个值并挂起
```

`yield expr` 只能出现在声明返回 `Iterator<T>` 的生成器函数体内。第一次调用生成器函数不会立即执行函数体，而是返回一个惰性 `Iterator<T>`；`for-in` 通过 `hasNext()` / `next()` 拉取，每次 `yield expr` 产出一个 `T` 并暂停到下一次拉取。

协作让出 CPU 不再使用裸 `yield`；使用 `Coro.yield()`（见 §10.10）。裸 `yield` 是语法错误。
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 3. Expressions

> Source of truth: `src/frontend/parser/xparse_expr.c`, AST node types in `src/frontend/parser/xast_types.h` such as `AST_BINARY_*` / `AST_UNARY_*` / `AST_TERNARY` / `AST_*`.

### 3.1 Precedence and Associativity

Full precedence table (highest → lowest; operators at the same level share associativity):

| Level | Operators | Assoc. | Description |
|--|--|--|--|
| 17 | `(...)` `[...]` `.x` `?.x` `?[...]` `f()` `e!` | left | postfix: grouping, index, member, optional chain, call, force unwrap |
| 16 | prefix `-` `+` `!` `~` `move` `await` `go` `unsafe` | right | unary prefix + coroutine/FFI boundary operators |
| 15 | `as` `is` | left | type cast / check (`as T?` is the safe form via a nullable target type, not a separate `as?` operator) |
| 14 | `*` `/` `%` | left | multiplication / division / modulo |
| 13 | `+` `-` | left | addition / subtraction |
| 12 | `<<` `>>` | left | shifts |
| 11 | `<` `<=` `>` `>=` | left | relational |
| 10 | `==` `!=` | left | equality |
| 9 | `&` | left | bitwise AND |
| 8 | `^` | left | bitwise XOR |
| 7 | `\|` | left | bitwise OR (also union types) |
| 6 | `&&` | left | logical AND (short-circuit) |
| 5 | `\|\|` | left | logical OR (short-circuit) |
| 4 | `??` | left | null coalescing |
| 3 | `..` `..=` | left | range |
| 2 | `? :` | right | ternary |
| 1 | `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` | right | assignment and compound assignment |
| 0 | `,` (only in `match` multi-value arms, argument lists, etc.) | — | not a real operator |

Implementation: Pratt-parser style in `src/frontend/parser/xparse_expr.c`.

### 3.2 Unary Expressions

```ebnf
UnaryExpr ::= ('-' | '+' | '!' | '~') UnaryExpr
            | 'move' UnaryExpr
            | 'await' ('all' | 'any')? UnaryExpr
            | 'go' (Block | PostfixExpr)
            | 'unsafe' Block
            | PostfixExpr
```

| Operator | Applicable types | Result type | Notes |
|--|--|--|--|
| `-x` | numeric | same | negation; preserves float NaN |
| `+x` | numeric | same | identity, almost never useful |
| `!x` | `bool` | `bool` | logical not; **rejects non-bool** (unlike JS) |
| `~x` | integer | same | bitwise complement |
`++` / `--` are **not expressions**: expression-position uses such as `let y = x++`, `f(x++)`, `a[i++]`, and `return x++` are compile errors. Statement-level increment/decrement is specified in §4.1.

#### `unsafe { }`

`unsafe { ... }` is an explicit FFI/raw-pointer boundary expression. Inside the block, xray permits calls to `@extern` functions, reads/writes through `RawPtr<T>` / `RawMut<T>` foreign memory, and `deref()` calls that dereference raw pointers.

```xray
@extern("C") fn malloc(n: uintsize) -> RawMut<uint8>
@extern("C") fn free(p: RawMut<uint8>)

let p = unsafe { malloc(1) }      // the final expression is the block result
unsafe {
    p[0] = 7                      // RawMut writes must be inside unsafe
    print(p.deref())              // dereference must be inside unsafe
    free(p)                       // @extern calls must be inside unsafe
}
```

`unsafe` does not change the expression's result type; in a multi-statement block, the trailing expression statement yields the block value, otherwise the result is `()`. `unsafe` also does not disable ordinary type checking: `RawPtr<T>` is still read-only, and writes require `RawMut<T>`; null pointers, bounds, lifetimes, and alignment remain the caller's responsibility.

### 3.3 Binary Expressions

```ebnf
BinaryExpr ::= UnaryExpr (BinOp UnaryExpr)*
BinOp ::= '+' | '-' | '*' | '/' | '%'
       | '&' | '|' | '^' | '<<' | '>>'
       | '<' | '<=' | '>' | '>='
       | '==' | '!='
       | '&&' | '||'
       | '??'
```

#### 3.3.1 Arithmetic Operators

| Operator | int×int | float×float | int×float | string | other |
|--|--|--|--|--|--|
| `+` | int | float | float (int→float promotion) | string concatenation | ❌ |
| `-` | int | float | float | ❌ | ❌ |
| `*` | int | float | float | ❌ | ❌ |
| `/` | int (truncating) | float | float | ❌ | ❌ |
| `%` | int | ❌ | ❌ | ❌ | ❌ |

**Special semantics**:
- `int / 0` → throws `XR_ERR_DIV_BY_ZERO` (E0420) at runtime.
- `int % 0` → throws `XR_ERR_MOD_BY_ZERO` (E0421) at runtime.
- Division whose result type is `float`/`float32` follows IEEE-754: `1.0 / 0.0` produces `+inf`, `-1.0 / 0.0` produces `-inf`, and `0.0 / 0.0` produces `NaN`; use `x.isNaN()` or `math.isNaN(x)` to test NaN.
- `%` accepts integer operands only; modulo with a static type that contains float (e.g. `5.0 % 2.0`) is a compile-time analyzer error. Runtime `XR_ERR_TYPE_MISMATCH` (E0404) remains only as a dynamic fallback.
- Integer overflow: see §2.3.1.
- `string + string` is O(n) concatenation; for heavy concatenation use `StringBuilder`.
- `char` is an independent Unicode scalar type and does not participate in arithmetic; use `int(c)` explicitly when the code point is needed.

#### 3.3.2 Bitwise Operators

`&` `|` `^` `~` `<<` `>>`

- Apply only to integer types.
- Shift counts are taken modulo 64 (unlike C: always defined in xray).
- `>>` is an **arithmetic right shift** (preserves the sign bit). For unsigned shifts, use the corresponding `uintN`.
- `bool` does not participate in bitwise operations (use `&&` `||`).
- `char` does not participate in bitwise operations; use `int(c)` explicitly when the code point is needed.

#### 3.3.3 Comparison Operators

| Operator | Semantics |
|--|--|
| `==` | value equality. `int` and `float` are comparable (with int→float implicit promotion). Strings compare by content. class/struct uses `==` overload or default identity. |
| `!=` | inverse of `==` |
| `<` `<=` `>` `>=` | supported by numbers and strings; other types are unsupported by default (enable via `operator<` overload). |

**Difference vs. JS**: xray's `==` **does not** do string↔number conversion; only the numeric int↔float promotion.

#### 3.3.4 Logical Operators

`&&` `||`:

- Both operands **must** be `bool` (checked at compile time).
- Short-circuit evaluation: `false && X` does not evaluate `X`; `true || X` does not evaluate `X`.
- Result type is `bool` (unlike JS, which returns one of the operands).

#### 3.3.5 Null Coalescing `??`

```xray @id=expr-null-coalesce
let v = nullable_expr ?? default_value
```

- Returns `default_value` when `nullable_expr` is `null`; otherwise returns `nullable_expr` itself.
- **Short-circuit**: `default_value` is evaluated only when the left side is null.
- Type inference: if `nullable_expr: T?` and `default_value: T`, the result type is `T` (non-null).
- Applies only to nullable types; using `??` on a non-null `T` is a compile warning/error.

### 3.4 Assignment and Compound Assignment

```ebnf
AssignExpr ::= LValue AssignOp Expression
LValue ::= Identifier | MemberAccess | IndexAccess
AssignOp ::= '=' | '+=' | '-=' | '*=' | '/=' | '%='
           | '&=' | '|=' | '^=' | '<<=' | '>>='
```

**Semantics**:
- Assignment is an **expression**; its result is the assigned value (chainable: `a = b = 0`).
- `x op= y` is equivalent to `x = x op y`, but `x` is evaluated only once (important: `obj.f += 1` does not call `f`'s getter twice).
- Cannot assign to a `const` (compile error `E0303`).
- Cannot assign to `shared const` (same as above).

**Special cases**:
- A function parameter with the `in T` modifier is read-only; assignment is a compile error.
- Array/Map field assignment: `a[i] = v` calls `operator[]=` or the built-in setter.

### 3.5 Ternary `? :`

```ebnf
TernaryExpr ::= LogicOrExpr ('?' Expression ':' Expression)?
```

```xray
let max = a > b ? a : b
```

- **Right-associative**: `a ? b : c ? d : e` = `a ? b : (c ? d : e)`.
- The condition must be `bool`.
- The two branches share a unified type (taken as the common supertype or a union).

### 3.6 Null Coalescing `??` and Optional Chaining `?.` / `?[`

See §3.3.5 (`??`) and below (`?.` / `?[`).

#### Optional chaining `?.` / `?[`

```ebnf
OptionalChain ::= Primary ('?.' Identifier | '?.' '(' ArgList? ')' | '?[' Expr ']')+
```

```xray @id=expr-optional-chain
let len = name?.length          // returns null when name is null
let item = arr?[0]              // optional index
let value = callback?.(input)   // optional function call
```

**Semantics**:
- If the LHS of `?.` or `?[` is `null`, the entire expression short-circuits to `null`.
- `?.` is for property access, method calls, and function calls: `obj?.prop`, `obj?.method()`, `func?.(args)`.
- `?[` is for index access: `arr?[0]`. Symmetric with regular indexing `arr[0]` — just add `?` before `[`.
- `func?.(args)` does not evaluate its arguments when the function value is `null`; it returns `null` directly.
- **Propagation**: in `a?.b.c.d`, if `a` is null the whole chain returns null; intermediate `.` operations are not re-checked.
- Result type: the original type plus `?` (already-nullable types remain unchanged).

### 3.7 Force Unwrap `!`

> Full error-handling semantics are in §8. This section only lists the expression syntax and brief semantics.

#### Force unwrap `expr!`

```xray
let v: int = nullable_int!      // throws NullThrowError (E0410) at runtime when null
```

Legal only when `expr` is known to be a nullable type (`T?`) at compile time; using `!` on a non-null `T` is a compile error.

### 3.8 `as` / `is`

#### `is` runtime type check

```ebnf
IsExpr ::= UnaryExpr 'is' Type
```

```xray
if (v is User) {
    // v is narrowed to User in this branch
    print(v.name)
}
```

- Result type: `bool`.
- **Type guard**: the analyzer narrows the static type of `v` inside the branch.
- Applies to union, nullable, class hierarchies, and `Json` structural matching.

#### `as` type cast

```ebnf
AsExpr ::= UnaryExpr 'as' Type
        |  UnaryExpr 'as' Type '?'
```

```xray
let n = v as int           // throws TypeError on failure
let n = v as int?          // returns null on failure (the "as nullable" safe form)
```

| Form | Failure behavior | Use case |
|--|--|--|
| `expr as T` | throws `XR_ERR_TYPE_MISMATCH` (E0404) | a cast that must succeed |
| `expr as T?` | returns `null` | a cast that may or may not succeed |

**Supported conversions**:
- Between numeric types (`int → float` lossless, `float → int` truncating).
- `Json → T` (runtime structural check against `T`).
- Parent → child (runtime `instanceof`).
- Union member → concrete member.

### 3.9 Range `..` / `..=` and Spread `...`

#### Range `a..b` / `a..=b`

```ebnf
RangeExpr ::= AddExpr (('..' | '..=') AddExpr)?
```

```xray @id=expr-range
0..10                  // 0..10, left-closed right-open (includes 0, excludes 10)
0..=10                 // 0..=10, closed interval (includes both 0 and 10)
let r = 1..100
let n = 10
for (i in 0..n) { print(i) }
for (i in 0..=n) { print(i) }
```

- Type: `Range` (int ranges only).
- `a..b` is the half-open interval `[a, b)`: `a` is included, `b` is not.
- `a..=b` is the inclusive interval `[a, b]`: both endpoints are included.
- `for-in`, `Range.includes`, `Range.length`, `Range.toArray()`, and range patterns in `match` all use the corresponding endpoint semantics.
- Primary uses: `for-in` loops, range checks in pattern matching.

#### Spread `...`

Allowed in the following positions only:
- **Function rest parameter declaration**: `fn f(...args: int)`
- **Function call spread**: `f(...args)`; the spread source must be a tuple whose arity is statically known.
- **Tuple literal spread**: `(head, ...tail)`; the spread source must be a tuple whose arity is statically known.
- **Array literal spread**: `[...a, x, ...b]`; the spread source must be an array. The result is a new array built by runtime concatenation (O(n)).
- **Object/record literal spread**: `{...base, x: 1}`; the spread source must be an object. Fields are merged into a new object; on a name clash the later field wins, and the result field set is the union of every source's fields and the literal fields.

```xray @id=expr-spread-collections
let a = [1, 2]
let b = [3, 4]
let nums = [...a, 99, ...b]            // [1, 2, 99, 3, 4]

let base = { x: 1, y: 2 }
let point = { ...base, y: 20, z: 3 }   // { x: 1, y: 20, z: 3 }
```

### 3.10 Literal Construction

#### Array `[...]`

```ebnf
ArrayLit  ::= '[' (ArrayElem (',' ArrayElem)* ','?)? ']'
ArrayElem ::= '...' Expr | Expr
```

```xray @id=expr-array-lit
let a = [1, 2, 3]
let empty: Array<int> = []
let mixed = [1, "hello"]    // type Array<int | string>
```

#### Map `#{k: v, ...}` and `#{}`

```ebnf
MapLit   ::= '#{' (MapEntry (',' MapEntry)* ','?)? '}'
MapEntry ::= Expression ':' Expression
EmptyMap ::= '#{' '}'    // note: '#{' is a single token
```

```xray @id=expr-map-lit
let m = #{"a": 1, "b": 2}
let empty = #{}                           // empty Map
```

**Key distinction**: `{}` is always a **Json / Object**; `#{}` is always a **Map**. Both use `:` between key and value; the `#` prefix is the disambiguator.

#### Set `#[...]`

```ebnf
SetLit ::= '#[' (Expr (',' Expr)* ','?)? ']'
```

```xray @id=expr-set-lit
let s = #[1, 2, 3]
let empty = #[]
```

#### Object (structured object) `{ field: value, ... }`

```ebnf
ObjectLit  ::= '{' ObjectField (',' ObjectField)* ','? '}'
ObjectField ::= Identifier ':' Expr
              | Identifier            // shorthand: `{ x }` ≡ `{ x: x }`
              | '...' Expr            // spread: `{ ...base }` merges fields
```

```xray @id=expr-object-lit
let p = { name: "Alice", age: 30 }
let users = "Bob"
let obj = { users }              // shorthand
```

- Defaults to an **extensible** structured object type (see §2.4.6 / §2.10 Json behavior).
- Fix the structure with a `type` alias: `let u: User = {...}` (compile-time field check, sealed).

#### Bytes `Bytes(...)`

See §2.4.5 and §14.5.

#### Channel `Channel<T>(buf?)`

```xray
const ch: Channel<int> = Channel<int>(10)
```

See §10.5.

### 3.11 Calls / Member Access / Indexing / Slicing

#### Function call

```ebnf
CallExpr ::= Primary '(' ArgList? ')'
ArgList ::= Expr (',' Expr)* ','?
```

- Arguments are passed positionally; named arguments are not supported.
- A rest parameter collects extra arguments into an array.
- Argument-count mismatch → compile error `E0307` / `E0450`.

#### Member access

```ebnf
MemberAccess ::= Primary '.' Identifier
```

```xray
obj.field
obj.method(args)
ClassName.staticMethod()
EnumName.MemberName
```

- Field access: compile-time check that the type has the field.
- Method call: resolved to invoke (with IC cache optimization).
- Module member: `module.export_name`.
- Enum member: `Color.Red`.

#### Index access

```ebnf
IndexAccess ::= Primary '[' Expr ']'
```

```xray @id=expr-index-access
arr[0]
arr[0] = 10
map["key"]
str[i]                  // returns char
```

- `Array` indexing: `int`; out-of-bounds throws `E0430`.
- `Map` indexing: key type; missing key → `E0431`.
- `string` indexing: addresses Unicode scalar positions and returns `char`.
- User classes: via `operator[]` overload.

#### Slice

```ebnf
Slice ::= Primary '[' Expr? ':' Expr? ']'
```

```xray @id=expr-slice
arr[1:4]                // elements [1, 4)
arr[:3]                 // first 3
arr[2:]                 // from index 2 to the end
arr[:]                  // full slice (shallow copy)
str[0:5]                // string slice
```

- Half-open interval `[start, end)`.
- `Array` and `string` slicing share the same negative-index rule: a negative index is first converted as `length + index`, then clamped into `[0, length]`.
- Slicing returns a new object; the original array is not modified.

### 3.12 Anonymous Functions and Lambdas

Xray has three anonymous-function forms, all compiled to the same `AST_FUNCTION_EXPR` node with fully equivalent semantics; they differ only in conciseness and applicable position.

```ebnf
AnonFunction ::= BareLambda | ArrowLambda | FnExpression
BareLambda   ::= Identifier '->' (Expression | Block)
ArrowLambda  ::= '(' ArrowParams? ')' '->' (Expression | Block)
ArrowParams  ::= ArrowParam (',' ArrowParam)*
ArrowParam   ::= Identifier (':' Type)?      // type optional, inferred from context
FnExpression ::= 'fn' GenericParams? '(' Params ')' ('->' Type)? Block
```

```xray @id=expr-lambda-forms
// ── Bare lambda: most concise, restricted to call-argument position ──
arr.map(x -> x * 2)
arr.filter(x -> x % 2 == 0)

// ── Arrow lambda: any position, supports multi-param and parameter type annotation ──
let sum = arr.reduce((acc, x) -> acc + x, 0)    // no type
let double = (x: int) -> x * 2                   // typed
let add = (a: int, b: int) -> a + b              // multi-param

// ── fn expression: multi-statement body, return-type annotation, generics ──
let inc = fn(x: int) -> int {
    let y = x + 1
    return y
}
let identity = fn<T>(x: T) -> T { return x }     // generic
```

**Choosing among the three**:

| Form | Syntax | Suitable for |
|------|------|----------|
| Bare lambda | `x -> expr` | single-parameter callbacks, most concise |
| Arrow lambda | `(x, y) -> expr` | multi-param, parameter type annotation, or non-call positions |
| fn expression | `fn(x: T) -> R { ... }` | multi-statement body, return-type annotation, generics |

**Key rules**:
- **Bare lambda** (`x -> expr`): restricted to **call-argument position**; the single parameter is unparenthesized. The parameter type is inferred from the callee signature or the container element type.
- **Arrow lambda** (`(x) -> expr`, `(x, y) -> expr`): usable in any position. Parameter types may be omitted and inferred from context; inference failure raises `E0365`. Arrow lambdas **do not support return-type annotations**; use `fn(x: T) -> R { ... }`, or annotate the binding as a function type: `let f: (T) -> R = (x) -> ...`.
- **fn expression** (`fn(x: T) { ... }`): usable in any position. Supports generic parameters `fn<T>(...)`, return-type annotation `-> T`, and a multi-statement body.
- Single-expression form `-> expr` implicitly `return`s.
- Block form `-> { ... }` or `{ ... }` uses an explicit `return`.
- Capture rules: see §7.4 closure capture. **A `go` coroutine closure cannot capture `let` variables** — pass them explicitly via `shared const`, `move`, or parameters.

### 3.13 `match` Expression

```ebnf
MatchExpr ::= 'match' Expr '{' MatchArm (',' MatchArm)* ','? '}'
MatchArm ::= Pattern ('if' Expr)? '->' Expression
```

```xray @id=expr-match
let result = match (x) {
    1 -> "one",
    2, 3, 4 -> "few",                 // multi-value
    10..20 -> "teen",                 // range
    n if (n > 100) -> "big",          // guard
    Color.Red -> "red",               // enum
    is User -> "a user",              // type pattern
    _ -> "default"                    // wildcard
}
```

**Semantics**:
- Matches top-down, taking the first successful arm.
- All arm expressions must yield the same type (or a union).
- **Exhaustiveness**: for enum scrutinees (ADT and simple enums), the compiler enforces exhaustiveness. Otherwise it is not enforced, and an unmatched value at runtime throws `PanicInfo(E0442)`.
- For pattern details see [§6](#6-patterns).

### 3.14 Construction expressions

```ebnf
ConstructExpr ::= Identifier TypeArgs? '(' ArgList? ')'
```

Construction looks just like a function call: `TypeName(args)`. There is no `new` keyword—writing `new` (e.g. `new Point(...)`) is a compile error that tells you to delete it.

```xray @id=expr-new
let p = Point(1.0, 2.0)
let arr = Array<int>()
let ch = Channel<int>(10)
let m = Map<string, int>()
```

**Used for**:
- Class and struct instantiation (`TypeName(args)`).
- Constructing built-in container types (`Array` / `Map` / `Set` / `Channel` / `Bytes` / `StringBuilder`, etc.; also `TypeName(args)`).
- Disambiguation is by symbol kind in the analyzer: type names construct, function names call (naming convention: types capitalized, functions lowercase).

**Relation to literals**:
```xray @id=expr-literal-constructor-relation
let a = [1, 2, 3]              // equivalent to Array<int>() + push
let m = #{}                    // equivalent to Map<...>()
let p = Point{x: 1, y: 2}      // struct literal
```

### 3.15 String Interpolation

See §1.6.5. In brief:

```xray @id=expr-string-interpolation
"Hello, ${name}! Age: ${user.age + 1}"
```

- `${...}` accepts any expression (calls, object access, arithmetic).
- Embedded string literals inside `${...}` may use the same quote as the outer template; the lexer matches expression braces by depth and skips nested strings / raw strings / char literals.
- The expression's type must be convertible to a string (implement `toString()` or be a primitive).

### 3.16 `yield` Statement

```xray @id=expr-yield
yield expr                  // produce one generator value and suspend
```

`yield expr` is only valid inside a generator function declared to return `Iterator<T>`. Calling a generator function does not immediately execute its body; it returns a lazy `Iterator<T>`. `for-in` pulls through `hasNext()` / `next()`, and each `yield expr` produces one `T` before suspending until the next pull.

Cooperative CPU yielding no longer uses bare `yield`; use `Coro.yield()` (see §10.10). Bare `yield` is a syntax error.
<!-- /xr-spec:en -->

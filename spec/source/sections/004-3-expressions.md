---
id: spec.3_expressions
order: 004
---

<!-- xr-spec:cn -->
---

## 3. 表达式 (Expressions)

> 真值源：`src/frontend/parser/xparse_expr.c`、`src/frontend/parser/xast_types.h` 的 `AST_BINARY_*` / `AST_UNARY_*` / `AST_TERNARY` / `AST_*` 等节点。

### 3.0 求值顺序

> 真值源：`src/frontend/canonical/xcanon.c`、`src/ir/xi_lower_expr.c`。本节定义 §16.9 内存模型中 sequenced-before 关系的来源；优先级（§3.1）只决定表达式的**结构**，本节决定其**求值时序**，两者是独立的规则。

xray 的求值顺序**完全确定**：语言不存在未指定（unspecified）或未定义（undefined）的求值顺序，VM 与 AOT 后端必须产生完全相同的副作用序列。

这不是保守取舍而是必要条件：差分测试以"同一程序在两条后端上逐字节相同"为正确性判据，一旦承认某处顺序未指定，该处的后端分歧就不再算缺陷，判据本身随之失效。该一致性由 `tests/diff/cases/semantics/evaluation_order/` 门禁强制。

**E1（总则）**：表达式按**从左到右、深度优先**求值。子表达式在其父表达式使用其值之前完成求值，包含全部副作用。

**E2（函数调用）**：`callee(a1, …, an)` 依次求值被调表达式 `callee`（成员调用时为 receiver）→ `a1` → … → `an`，随后进入调用。借用在全部实参求值完成之后、进入被调函数之前建立。缺省参数在调用点按声明顺序求值，且排在全部显式实参之后。

**E3（二元运算符）**：左操作数完全求值（含副作用）先于右操作数开始求值。唯一例外是短路运算符（E4）。

**E4（短路点）**：`&&`、`||`、`??`、`?:`、`?.`、`?[` 是语言中**全部**的短路点。除此之外没有任何操作数会被条件性跳过求值。

**E5（赋值）**：`place = rhs` 依次求值 place 的位置子表达式（receiver 或数组表达式 → 索引表达式）→ `rhs` → 执行存储。位置子表达式先于右值，与 C# / Java 一致，与 Rust 相反。

**E6（复合赋值）**：`place op= rhs` 等价于 `place = place op rhs`，**但 place 的每个子表达式只求值一次**。求值顺序为：place 子表达式 → 读取 place → `rhs` → 计算 → 写回 place。`x++` / `x--` 等价于 `x += 1` / `x -= 1`，遵循同一规则。复合赋值的目标限于变量与成员访问（见 §3.4）。

**E7（字面量）**：Array / Set / tuple 元素按源码顺序求值；Map 与对象字面量按源码顺序逐项求值，每项内 key 先于 value；展开 `...` 在其出现位置按序求值。

**E8（字符串插值）**：插值片段按源码从左到右求值。

**E9（match）**：scrutinee 先于任何 arm 求值，且只求值一次。arm 按源码顺序尝试；`if` 守卫只在其所属模式已匹配时求值。

**E10（切片与范围）**：`a[lo:hi]` 依次求值 a → lo → hi；`lo..hi` 与 `lo..=hi` 依次求值 lo → hi。

**E11（语句）**：语句按源码顺序求值。`defer` 在注册处求值其捕获，执行时机见 §4.9。

```xray @id=eval-order-rules
fn t(tag: string, v: int) -> int { print(tag); return v }
fn add(a: int, b: int) -> int { return a + b }
fn pick(tag: string) -> fn(int, int) -> int { print(tag); return add }

class Counter {
    hits: int = 0
}

fn mk(tag: string) -> Counter { print(tag); return Counter() }

// E2：被调表达式先于实参
var sum = pick("callee")(t("arg1", 1), t("arg2", 2))   // callee, arg1, arg2

// E5：place 子表达式先于右值
var arr = [0, 0, 0]
arr[t("index", 0)] = t("value", 5)                     // index, value

// E6：复杂 receiver 只求值一次
mk("receiver").hits += t("delta", 1)                   // receiver, delta
```

### 3.1 优先级与结合性

完整优先级表（自**高至低**；同级运算符按结合性分组）：

| 级 | 运算符 | 结合性 | 说明 |
|--|--|--|--|
| 17 | `(...)` `[...]` `.x` `?.x` `?[...]` `f()` `e!` | 左 | 后缀：分组、索引、成员、可选链、调用、强制解包 |
| 16 | 前缀 `-` `+` `!` `~` `move` `await` `go` `unsafe` `comptime` | 右 | 一元前缀 + 协程/FFI/编译期边界操作 |
| 15 | `as` `is` | 左 | 类型转换 / 检查（`as T?` 安全形式靠目标类型可空，非独立 `as?` 运算符） |
| 14 | `*` `/` `%` | 左 | 乘除取模 |
| 13 | `+` `-` | 左 | 加减 |
| 12 | `<<` `>>` | 左 | 移位 |
| 11 | `..` `..=` | 无 | 范围：端点是算术表达式，故 `0..n+1` = `0..(n+1)`；非结合，`a..b..c` 是语法错误 |
| 10 | `<` `<=` `>` `>=` | 左 | 关系比较 |
| 9 | `==` `!=` | 左 | 相等比较 |
| 8 | `&` | 左 | 位与 |
| 7 | `^` | 左 | 位异或 |
| 6 | `\|` | 左 | 位或（亦用于 union 类型） |
| 5 | `&&` | 左 | 逻辑与（短路） |
| 4 | `\|\|` | 左 | 逻辑或（短路） |
| 3 | `??` | 左 | 空值合并 |
| 2 | `? :` | 右 | 三元 |
| 1 | `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` | 右 | 赋值与复合赋值 |
| 0 | `,`（仅 `match` 多值、参数列表等特定位置）| — | 不是真正运算符 |


### 3.2 一元表达式

```ebnf
UnaryExpr ::= ('-' | '+' | '!' | '~') UnaryExpr
            | 'move' UnaryExpr
            | 'await' ('all' | 'any' | 'anySuccess')? UnaryExpr
            | 'go' (Block | PostfixExpr)
            | 'unsafe' Block
            | 'comptime' (Expression | Block)
            | PostfixExpr
```

| 运算符 | 适用类型 | 结果类型 | 备注 |
|--|--|--|--|
| `-x` | 数值 | 同 | 负号；浮点 NaN 保留 |
| `+x` | 数值 | 同 | 标识，几乎无用 |
| `!x` | `bool` | `bool` | 逻辑非；**不接受非 bool**（不像 JS） |
| `~x` | 整数 | 同 | 按位取反 |
`++` / `--` **不是表达式**：`var y = x++`、`f(x++)`、`a[i++]`、`return x++` 等表达式位置均编译报错。语句级自增/自减见 §4.1。

#### `unsafe { }`

`unsafe { ... }` 是显式 FFI/裸指针边界表达式。块内允许调用 `extern "C"` 函数、读取/写入 `Ptr<T>` / `MutPtr<T>` 指向的外部内存，以及调用需要裸指针解引用的 `deref()`。

```xray
extern "C" {
    fn malloc(n: usize) -> MutPtr<byte>
    fn free(p: MutPtr<byte>)
}

var p = unsafe { malloc(1) }      // 块的最后一个表达式作为结果
unsafe {
    p[0] = 7                      // MutPtr 写入必须在 unsafe 内
    print(p.deref())              // 解引用必须在 unsafe 内
    free(p)                       // extern 调用必须在 unsafe 内
}
```

`unsafe` 不改变表达式的结果类型；多语句块的最后一个表达式语句产生块值，否则结果为 `()`。`unsafe` 也不关闭普通类型检查：`Ptr<T>` 仍不可写，`MutPtr<T>` 才能写入；空指针、越界、生命周期和对齐由调用方负责。

#### `comptime expr` / `comptime { ... }`

`comptime` 要求表达式或块在分析阶段求值；失败是编译错误，不会退回运行期。常量表达式不限于整数：当前支持标量常量、TypeId、定长数组、tuple、struct 聚合及其成员/索引访问，以及可求值的一元/二元运算。固定数组长度等静态整数位置仍要求最终值是正整数。

```xray
const SCALE = comptime 8 * 4
var buf: [byte; comptime SCALE + 2] = [0; SCALE + 2]
```

`comptime { ... }` 已实现受限解释执行。块支持局部 `const`/`var`、局部变量赋值和复合赋值、`if`/`while`、C 风格 `for`、定长数组 `for-in`、循环内带标签或不带标签的 `break`/`continue`、`compile_assert(...)` 与 `compile_error(...)`。块只产生编译期副作用并在运行时被擦除；需要把值带出块时使用 `return <consteval-expression>`。当前函数调用不属于 consteval-safe 表达式，unsupported 语句会在分析期拒绝。

```xray
const TABLE_SIZE = comptime 4 * 8

comptime {
    var sum = comptime 0
    for (var i = 0; i < 4; i += 1) {
        sum += i
    }
    compile_assert(sum == 6, "comptime loop")
}
```

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

| 运算符 | 同类整数 | 同类浮点 | 可无损加宽的数值 | 整数×浮点 | string / 其他 |
|--|--|--|--|--|--|
| `+` | 原整数类型 | 原浮点类型 | 唯一较宽类型 | ❌（需显式 `as`） | `string + string` 拼接；其他 ❌ |
| `-` | 原整数类型 | 原浮点类型 | 唯一较宽类型 | ❌（需显式 `as`） | ❌ |
| `*` | 原整数类型 | 原浮点类型 | 唯一较宽类型 | ❌（需显式 `as`） | ❌ |
| `/` | 原整数类型（整除） | 原浮点类型 | 唯一较宽类型 | ❌（需显式 `as`） | ❌ |
| `%` | 原整数类型 | ❌ | 唯一较宽整数类型 | ❌ | ❌ |

“可无损加宽”仅指同符号整数链和 `f32 → f64`。一个直接数值字面量可由另一已定型操作数取得唯一上下文；两个已定型操作数不存在 C 风格 usual arithmetic conversions。

**特殊语义**：
- `int / 0` → 运行时抛 `XR_ERR_DIV_BY_ZERO` (E0420)。
- `int % 0` → 运行时抛 `XR_ERR_MOD_BY_ZERO` (E0421)。
- 结果类型为 `float`/`f32` 的除法遵循 IEEE-754：`1.0 / 0.0` 产生 `+inf`，`-1.0 / 0.0` 产生 `-inf`，`0.0 / 0.0` 产生 `NaN`；可用 `x.isNaN()` 或 `math.isNaN(x)` 检测 NaN。
- `%` 仅接受整数操作数；静态类型包含 float 的求模（如 `5.0 % 2.0`）在分析期编译错误。运行时 `XR_ERR_TYPE_MISMATCH` (E0404) 仅作为动态兜底。
- 整数溢出：见 §2.3.1。
- 字符串 `+ string` 是 O(n) 拼接；密集拼接请用 `StringBuilder`。
- `rune` 是独立的 Unicode scalar 类型，不参与算术；需要码点时显式写 `int(c)`。

#### 3.3.2 位运算

`&` `|` `^` `~` `<<` `>>`

- 仅作用于整数类型。
- 移位计数取模 64（与 C 不同：xray 总是定义的）。
- `>>` 是**算术右移**（保留符号位）。无符号类型用对应的 `uintN`。
- bool 不参与位运算（用 `&&` `||`）。
- `rune` 不参与位运算；需要码点时显式写 `int(c)`。

#### 3.3.3 比较运算符

| 运算符 | 语义 |
|--|--|
| `==` | 值相等。数值操作数必须同型或存在唯一无损共同类型；整数与浮点、不同符号整数之间必须先显式转换。字符串按内容比较。class/struct 使用 `==` 重载或默认 identity。 |
| `!=` | `==` 的反 |
| `<` `<=` `>` `>=` | 数值与字符串支持；其他类型默认不支持（可通过 `operator<` 重载启用）。 |

**与 JS / C 的区别**：xray 的 `==` 不做 string↔number 转换，也不做整数↔浮点或符号变化的隐式提升。

#### 3.3.4 逻辑运算符

`&&` `||`：

- 两操作数**必须**是 `bool` 类型（编译期检查）。
- 短路求值：`false && X` 不求值 `X`；`true || X` 不求值 `X`。
- 结果类型 `bool`（不像 JS 返回操作数）。
- **收窄继承（§2.13 N-5）**：`e1 && e2` 的 `e2` 在 `e1` 的 true 事实下分析，`e1 || e2` 的 `e2` 在 `e1` 的 false 事实下分析。因为 `T?` 不能直接作条件，`a != null && a.f` 与 `a == null || a.f` 是处理可空值的标准写法。

#### 3.3.5 空值合并 `??`

```xray @id=expr-null-coalesce
var v = nullable_expr ?? default_value
```

- 当 `nullable_expr` 为 `null` 时返回 `default_value`，否则返回 `nullable_expr` 本身。
- **短路**：`default_value` 只在前者为 null 时求值。
- 类型推导：若 `nullable_expr: T?` 且 `default_value: T`，结果类型 `T`（非空）。
- 仅作用于可空类型；对 `T`（非空）使用 `??` 是编译警告/错误。

### 3.4 赋值与复合赋值

```ebnf
AssignExpr   ::= AssignLValue '=' Expression
               | CompoundLValue CompoundOp Expression
AssignLValue ::= Identifier | MemberAccess | IndexAccess
CompoundLValue ::= Identifier | MemberAccess
CompoundOp   ::= '+=' | '-=' | '*=' | '/=' | '%='
               | '&=' | '|=' | '^=' | '<<=' | '>>='
```

**语义**：
- 赋值是**表达式**，结果是赋值后的值（可链式：`a = b = 0`）。
- `x op= y` 等价于 `x = x op y`，但 `x` 的每个子表达式只求值一次（重要：`obj.f += 1` 不会调用 `f` 的 getter 两次；`mk().f += 1` 只调用 `mk()` 一次）。完整时序见 §3.0 E6。
- **复合赋值的目标不含索引**：`a[i] += v` 是编译错误，写作 `a[i] = a[i] + v`。简单赋值 `a[i] = v` 不受此限。
- 不能赋值给 `const`（编译错误 `E0303`）。

**特殊**：
- 默认参数是只读借用；`ref` 参数才允许通过 place 修改，`move` 参数消费源所有权。
- 数组/Map 字面量字段：`a[i] = v` 调用 `operator[]=` 或内置 setter。

### 3.5 三元 `? :`

```ebnf
TernaryExpr ::= LogicOrExpr ('?' Expression ':' Expression)?
```

```xray
var max = a > b ? a : b
```

- **右结合**：`a ? b : c ? d : e` = `a ? b : (c ? d : e)`。
- 条件必须是 `bool`。
- 两分支类型统一：取共同超类型（或 union）。
- **收窄（§2.13 N-7）**：条件的 true 事实在 `then` 分支生效，false 事实在 `else` 分支生效，与 `if` 一致：`a != null ? len(a) : 0` 合法。

### 3.6 空合并 `??` 与可选链 `?.` / `?[`

详见 §3.3.5（`??`）与下方（`?.` / `?[`）。

#### 可选链 `?.` / `?[`

```ebnf
OptionalChain ::= Primary ('?.' Identifier | '?.' '(' ArgList? ')' | '?[' Expr ']')+
```

```xray @id=expr-optional-chain
var city = user?.address        // 可选属性访问
var item = arr?[0]              // 可选索引
var value = callback?.(input)   // 可选函数调用
var road = user?.address.street // 整链短路：user 为 null 时结果为 null
```

**语义**：
- 若 `?.` 或 `?[` 左侧为 `null`，**整条后缀链**短路返回 `null`，链上后续的 `.` / `[` / `(` 一律不求值。
- `?.` 用于属性访问、方法调用和函数调用：`obj?.prop`、`obj?.method()`、`func?.(args)`。
- `?[` 用于索引访问：`arr?[0]`。与普通索引 `arr[0]` 对称，只需在 `[` 前加 `?`。
- `func?.(args)` 在函数值为 `null` 时不求值实参，直接返回 `null`。
- **整链短路**：`a?.b.c.d` 等价于「`a` 为 `null` 时结果为 `null`，否则求值 `a.b.c.d`」。链中每个 `?.` 都是一个短路点；短路点之后的 `.` 不再需要 `?`，也**不会**产生 `E0379`——因为它只在前缀非 null 时求值。
- 若链中某一段自身返回可空值（`a?.b` 中 `b: T?`），则对该值继续用 `.` 访问仍需 `?.`：短路只覆盖 `?.` 左侧为 null 的情形，不覆盖右侧结果为 null 的情形。
- 结果类型：整条链的类型加 `?`（若已经 `?` 则保持）。§2.13 N-13。

### 3.7 强制解包 `!`

> 完整错误处理语义见 §8。本节只列表达式语法与简要语义。

#### 强制解包 `expr!`

```xray
var v: int = nullable_int!      // null 时运行时抛 NullThrowError (E0410)
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
- **类型守卫**：`v` 是简单绑定时，分析器按 §2.13 N-4 / N-6 在 true 分支把 `v` 收窄为与 `T` 的交，在 false 分支移除该交集。
- 适用于 union、可空、class 层级、`Json` 结构匹配。
- **定宽数值类型**：动态擦除后的值只保留 i64 / f64 两个族，不保留位宽，因此 `v is i32` 问的是"该值能否被 `i32` 精确表示"——这是擦除后唯一可回答的形式。`is int` / `is float` 对整个族恒为真；`v as T?` 用同一个判定，不通过时返回 `null`。

#### `as` 类型转换

```ebnf
AsExpr ::= UnaryExpr 'as' Type
        |  UnaryExpr 'as' Type '?'
```

```xray
var n = v as int           // 失败抛 TypeError
var n = v as int?          // 失败返回 null（"as nullable" 安全形式）
```

| 形式 | 失败行为 | 用途 |
|--|--|--|
| `expr as T` | 抛 `XR_ERR_TYPE_MISMATCH` (E0404) | 必须成功的转换 |
| `expr as T?` | 返回 `null` | 不确定能否成功的动态 / 结构化转换；不适用于数值转换 |

**支持的转换**：
- 数值之间：整数到整数按目标位宽取模后按目标有符号性解释；整数到浮点和 `f64 → f32` 使用 IEEE-754 round-to-nearest, ties-to-even；浮点到整数向零截断，NaN、无穷大或越界抛 `XR_ERR_OVERFLOW` (E0422)。数值转换只使用 `expr as T`，不用 nullable 形式。
- `Json → T`（运行时按 T 结构检查）。
- 父类 → 子类（运行时 instanceof）。
- Union 成员 → 具体成员。

### 3.9 范围 `..` / `..=` 与展开 `...`

#### 范围 `a..b` / `a..=b`

```ebnf
RangeExpr ::= ShiftExpr (('..' | '..=') ShiftExpr)?
```

```xray @id=expr-range
var halfOpen = 0..10   // 左闭右开（包含 0，不包含 10）
var closed = 0..=10    // 闭区间（包含 0 和 10）
var r = 1..100
var n = 10
for (i in 0..n) { print(i) }
for (i in 0..=n) { print(i) }
for (i in 0..n+1) { print(i) }   // 端点先算：0..(n+1)
```

- 类型 `Range`（仅 int 范围）。
- **优先级（见 §3.1）**：`..` / `..=` 比所有算术运算符（`* / % + - << >>`）都松，故端点先结合——`0..n+1` 是 `0..(n+1)`，`1..2*3` 是 `1..(2*3)`；它比比较与逻辑运算符紧，故 `0..n == 0..m` 是 `(0..n) == (0..m)`。
- **非结合**：范围不能链式书写，`a..b..c` 是语法错误；确需嵌套时给端点加括号。
- `a..b` 是半开区间 `[a, b)`：`a` 包含、`b` 不包含。
- `a..=b` 是闭区间 `[a, b]`：两端都包含。
- `for-in`、`Range.contains`、`len(range)`、`Range.toArray()`、`match` 中的范围模式全部遵循对应端点语义。
- 主要用途：`for-in` 循环、模式匹配中的范围判定。

#### 展开 `...`

仅在以下位置使用：
- **函数 rest 参数声明**：`fn f(...args: int)`
- **函数调用展开**：`f(...args)`，展开源必须是静态 arity 已知的 tuple。
- **tuple 字面量展开**：`(head, ...tail)`，展开源必须是静态 arity 已知的 tuple。
- **数组字面量展开**：`[...a, x, ...b]`，展开源必须是数组，运行期拼接成新数组（O(n)）。
- **对象字面量展开**：`{...base, x: 1}`，展开源必须是对象；字段合并成新对象，同名字段后者覆盖前者，结果字段集为各展开源字段与字面量字段的并集。

```xray @id=expr-spread-collections
var a = [1, 2]
var b = [3, 4]
var nums = [...a, 99, ...b]            // [1, 2, 99, 3, 4]

var base = { x: 1, y: 2 }
var point = { ...base, y: 20, z: 3 }   // { x: 1, y: 20, z: 3 }
```

### 3.10 字面量构造

#### Array `[...]`

```ebnf
ArrayLit  ::= '[' (ArrayElem (',' ArrayElem)* ','?)? ']'
ArrayElem ::= '...' Expr | Expr
```

```xray @id=expr-array-lit
var a = [1, 2, 3]
var empty: Array<int> = []
var mixed = [1, "hello"]    // 类型 Array<int | string>
```

#### Map `#{k: v, ...}` 与 `#{}`

```ebnf
MapLit   ::= '#{' (MapEntry (',' MapEntry)* ','?)? '}'
MapEntry ::= Expression ':' Expression
EmptyMap ::= '#{' '}'    // 注意：'#{' 是单个 token
```

```xray @id=expr-map-lit
var m = #{"a": 1, "b": 2}
var empty = #{}                           // 空 Map
```

**关键区别**：`{}` 始终是**Json / Object**；`#{}` 始终是 **Map**。两者都用 `:` 作键值分隔，靠 `#` 前缀区分。

#### Set `#[...]`

```ebnf
SetLit ::= '#[' (Expr (',' Expr)* ','?)? ']'
```

```xray @id=expr-set-lit
var s = #[1, 2, 3]
var empty = #[]
```

#### Object（结构化对象）`{ field: value, ... }`

```ebnf
ObjectLit  ::= '{' ObjectField (',' ObjectField)* ','? '}'
ObjectField ::= Identifier ':' Expr
              | Identifier            // shorthand: `{ x }` 等价 `{ x: x }`
              | '...' Expr            // spread: `{ ...base }` 合并字段
```

```xray @id=expr-object-lit
var p = { name: "Alice", age: 30 }
var users = "Bob"
var obj = { users }              // shorthand
```

- 默认推断为 exact 对象形状（见 §2.4.7），字段集和字段 offset 在编译期固定，适合 AOT 快路径。
- 只有显式 `Json` 期望类型时才按动态 Json object literal 解释；typed value 进入 JSON 边界使用 `Json.encode(value)`。
- 用 `type` 别名命名 structural object：`var u: User = {...}`（编译期检查字段集，密封）。

#### Array<byte> `Array<byte>(...)`

详见 §2.4.6 与 §14.5。

#### Channel `Channel<T>(buf?)`

```xray
const ch: Channel<int> = Channel<int>(10)
```

详见 §10.5。

### 3.11 调用 / 成员访问 / 索引 / 切片

#### 函数调用

```ebnf
CallExpr ::= Primary '(' ArgList? ')'
ArgList ::= CallArg (',' CallArg)* ','?
CallArg ::= ('ref' | 'out')? Expr
```

- 参数按位置传递；不支持命名参数。
- `ref` / `out` 参数必须在调用点重复写同名 marker，并传入可寻址 place；普通 `in`/值参数不写调用点 marker。
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
var bytes: Slice<byte> = text.bytes()
bytes[i]                // 显式 byte 视图索引
```

- `Array` 索引：`int`，越界抛 `E0430`。
- `Map` 索引：键类型；找不到键 → `E0431`。
- `string` 整数索引：编译错误；使用 `runes().nth(i)` 或 `bytes()[i]` 显式选择单位。
- 自定义类：通过 `operator[]` 重载。

#### 切片

```ebnf
Slice ::= Primary '[' Expr? ':' Expr? ']'
```

```xray @id=expr-slice
arr[1:4]                // 元素 [1,4)
arr[:3]                 // 前 3 个
arr[2:]                 // 从索引 2 到末尾
arr[:]                  // 全长视图（不是拷贝）
var view: Slice<int> = arr[1:4]
```

- 半开区间 `[start, end)`。
- Array 切片支持负索引：负数先按 `len(array) + index` 从末尾计数，再夹到合法范围。
- string 不支持 slice operator；使用严格 rune ordinal 的 `s.slice(start, end)`。
- 切片求值为目标类型为 `Slice<T>` 的**借用视图**，不复制元素。包括 `arr[:]` 在内的所有形式都是视图：通过视图写入直接改写 owner 的存储，owner 的元素写入也对视图立即可见。需要独立数据时写 `copy(arr[1:4])`。
- 切片是一次借用：owner 在视图存活期间受 §2.4.2 的借用规则约束，视图本身不得逃逸出 owner 的作用域。完整规则见 §2.4.2。

### 3.12 匿名函数与 Lambda

xray 只有两个匿名函数概念：**arrow lambda** 与 **fn lambda**。单个、无标注的只读参数可省略括号，因此 `x -> expr` 与 `(x) -> expr` 是同一概念的两种括号拼写，而不是两种函数。两类都编译为相同的 `AST_FUNCTION_EXPR` 节点；差异在源代码可携带的签名信息。

```ebnf
AnonFunction  ::= ArrowLambda | FnExpression
ArrowLambda   ::= Identifier '->' ArrowBody
                | '(' ArrowParams? ')' '->' ArrowBody
ArrowBody     ::= Expression | Block
ArrowParams   ::= ArrowParam (',' ArrowParam)* ','?
ArrowParam    ::= Identifier (':' ParamMode? Type)?
ParamMode     ::= 'ref' | 'move'
FnExpression  ::= 'fn' GenericParams? '(' ArrowParams? ')' ('->' Type)? Block
```

```xray @id=expr-lambda-forms
// ── arrow lambda：每个参数都可独立标注或推断 ──
arr.map(x -> x * 2)
var add = (x: int, y) -> x + y

// 表达式体隐式返回；ref 只授予可写借用，不会自动写回
var calculate = (x: ref int) -> x * 2
var doubleInPlace = (x: ref int) -> { x = x * 2 }

// ── fn lambda：显式返回类型、泛型或完整参数契约 ──
var inc = fn(x: int) -> int {
    var y = x + 1
    return y
}
var identity = fn<T>(x: T) -> T { return x }     // 泛型
```

**选择规则**：

| 概念 | 语法 | 能力与选择条件 |
|------|------|----------------|
| arrow lambda | `x -> expr` / `(x: T, y) -> expr` / `(x: ref T) -> { ... }` | 参数可逐个标注；返回类型始终推断；表达式体隐式返回，块体显式返回 |
| fn lambda | `fn(x: T) -> R { ... }` / `fn<T>(...) { ... }` | 需要显式返回类型、泛型或完整声明式签名时选择 |

**关键规则**：
- arrow 参数的类型标注相互独立，可以混写 `(x: int, y) -> ...`；未标注参数综合绑定类型、调用点签名、容器元素类型和函数体约束推断。推断不足时报 `E0365`，可直接标注该参数、标注绑定、补充调用点类型上下文，或改写为显式签名的 `fn`。
- 参数 mode 使用统一的后缀标注：`x: ref T`、`x: move T`。`(ref x) -> ...`、默认值和 rest 参数不是 arrow 语法；默认值和 rest 参数属于具名函数声明。
- arrow 没有返回类型位置。`(x: T): R -> ...` 与 `(x: T) -> R { ... }` 都是错误；显式返回类型必须写成 `fn(x: T) -> R { ... }`。
- `-> expr` 隐式返回表达式值。`-> { ... }` 支持多条语句，值返回必须写 `return value`；正常落到块尾返回 `()`，块尾表达式没有特殊的隐式返回语义。
- `ref` 表示调用者提供可写借用，不表示自动写回：`(x: ref int) -> x * 2` 只读取并返回乘积，调用者的值不变；`(x: ref int) -> { x = x * 2 }` 原地修改调用者并返回 `()`。需要“修改并返回”时，在块中赋值后显式 `return x`。
- 工具可对“源码显式写了 `ref`、函数体确定未修改该参数”的匿名函数给出非强制 hint。期望函数契约要求 `ref`、mode 来自推断或效果分析不完整时不提示；该提示不改变类型检查或运行时语义。
- 捕获规则：见 §7.4。`go` 协程闭包消费统一的 provenance-based capture plan：inline、已发布 const 值与受审计同步句柄可直接捕获；execution-local graph、module-mutable state 和生命周期不足的 view/pointer 会被拒绝，必须通过参数显式 `copy(...)` / `move`。

### 3.13 `match` 表达式

```ebnf
MatchExpr ::= 'match' Expr '{' MatchArm (',' MatchArm)* ','? '}'
MatchArm ::= Pattern ('if' Expr)? '->' Expression
```

```xray @id=expr-match
var result = match (x) {
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

构造与普通函数调用同形：`TypeName(args)`。`new` 是保留字，不构成合法表达式。

```xray @id=expr-new
var p = Point(1.0, 2.0)
var arr = Array<int>()
const ch = Channel<int>(10)
var m = Map<string, int>()
```

**用于**：
- 类与 struct 实例化（`TypeName(args)`）。
- 容器内置类型构造（`Array`/`Map`/`Set`/`Channel`/`Array<byte>`/`StringBuilder` 等，同样是 `TypeName(args)`）。
- 消歧由 analyzer 按符号种类判定：类型名构造，函数名调用（命名约定：类型大写、函数小写）。

**与字面量的关系**：
```xray @id=expr-literal-constructor-relation
var a = [1, 2, 3]              // 等价 Array<int>() + push
var m = #{}                    // 等价 Map<...>()
var p = Point{x: 1, y: 2}      // struct literal
```

### 3.15 字符串插值

详见 §1.6.5。简要：

```xray @id=expr-string-interpolation
var greeting = "Hello, ${name}! Age: ${user.age + 1}"
```

- `${...}` 内任意表达式（含函数调用、对象访问、算术）。
- `${...}` 内的字符串字面量可使用与外层模板相同的引号；lexer 按表达式大括号深度匹配，并跳过内层字符串 / raw string / rune 字面量。
- 表达式类型必须可转为字符串（实现 `toString()` 或为基本类型）。

### 3.16 `yield` 语句

```xray @id=expr-yield
yield expr                  // 生成器产出一个值并挂起
```

`yield expr` 只能出现在声明返回 `Iterator<T>` 的生成器函数体内。第一次调用生成器函数不会立即执行函数体，而是返回一个惰性 `Iterator<T>`（协议见 §5.3.6）；`for-in` 通过 `hasNext()` / `next()` 拉取，每次 `yield expr` 产出一个 `T` 并暂停到下一次拉取。

协作让出 CPU 使用 `Coro.yield()`（见 §10.10）；裸 `yield` 不是表达式。

#### 3.16.1 两种挂起是不同的语义

`yield expr` 与 `Coro.yield()` 共用词根，但**不是同一种挂起**，二者在调用方能观察到的每一个性质上都不同：

| | `yield expr` | `Coro.yield()` |
|---|---|---|
| 转移目标 | 对称转移回驱动该生成器的迭代器 | 非对称让出给调度器 |
| 是否可能换 OS 线程 | 否 | 是（work-stealing） |
| 是否是取消检查点 | **否** | 是（§10.5） |
| 是否向调用方传播 | **否**——驱动生成器不会挂起驱动方自己的帧 | 是，沿调用边传递 |
| 效应契约 | `no_suspend` 拒绝，`no_reschedule` 接受 | 两者都拒绝 |

因此 `xray verify` 把挂起拆成两个独立维度：`no_reschedule` 断言"不进调度器、不是取消点、不迁移线程"，`no_suspend` 是更强的"控制流从不离开本帧"。纯生成器满足前者而不满足后者；**只是驱动生成器的函数两者都满足**。

#### 3.16.2 生成器体内不得抵达调度器

生成器帧由驱动它的一方恢复，而非由调度器恢复。因此生成器函数体内**不得出现任何调度器挂起点**，包括：`await`、`select`、`scope` 块、`Coro.yield()`、Channel 的阻塞方法、`time.sleep()`，以及对任何可能挂起的函数的调用。违反时报 `E0385`。

该规则是**效应规则而非语法规则**：它按 §8 的效应产品逐层传播，因此调用一个内部 `await` 的函数同样被拒绝。证据不完整时**失败关闭**——生成器体内通过未解析的函数值（如函数类型参数）发起调用会被拒绝，因为该目标可能挂起。这与 `sys.Thread.spawn` 体和 parallel 回调的边界规则一致。

> 该限制是有意的作用域决定，不是实现缺口：本版本的生成器是拉取式迭代器，不是异步流。

#### 3.16.3 提前放弃的生成器不再执行任何代码

`for-in` 通过 `break` / `return` / 抛错提前离开，或迭代器被丢弃时，生成器帧**不会被恢复**，其函数体的剩余部分永不执行。回收时机不属于可观察语义契约（§16.8）。

因此**生成器体内不得使用 `defer`**（`E0386`）：`defer` 是本语言唯一的确定性清理机制，而在此处它可能永远不执行，一个可能不运行的清理比没有清理更危险。需要清理时，由调用方持有资源并作为参数传入。
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 3. Expressions

> Source of truth: `src/frontend/parser/xparse_expr.c`, AST node types in `src/frontend/parser/xast_types.h` such as `AST_BINARY_*` / `AST_UNARY_*` / `AST_TERNARY` / `AST_*`.

### 3.0 Evaluation Order

> Source of truth: `src/frontend/canonical/xcanon.c`, `src/ir/xi_lower_expr.c`. This section defines where the sequenced-before relation of the §16.9 memory model comes from. Precedence (§3.1) fixes only the **structure** of an expression; this section fixes **when** its parts are evaluated. They are independent rules.

xray's evaluation order is **fully determined**: the language has no unspecified and no undefined evaluation order, and the VM and AOT backends must produce exactly the same sequence of side effects.

This is a requirement rather than a conservative preference. Differential testing uses "the same program is byte-identical on both backends" as its correctness criterion; the moment an order is declared unspecified, a backend divergence at that point stops being a defect and the criterion itself stops working. The `tests/diff/cases/semantics/evaluation_order/` gate enforces the agreement.

**E1 (general rule)**: expressions are evaluated **left to right, depth first**. A subexpression is fully evaluated, side effects included, before its parent expression uses its value.

**E2 (calls)**: `callee(a1, …, an)` evaluates the callee expression (the receiver for a method call) → `a1` → … → `an`, and then enters the call. Borrows are established after every argument has been evaluated and before the callee is entered. Default arguments are evaluated at the call site in declaration order, after all explicit arguments.

**E3 (binary operators)**: the left operand is fully evaluated, side effects included, before evaluation of the right operand begins. The short-circuit operators (E4) are the only exception.

**E4 (short-circuit points)**: `&&`, `||`, `??`, `?:`, `?.` and `?[` are **all** of the language's short-circuit points. No other operand is ever conditionally skipped.

**E5 (assignment)**: `place = rhs` evaluates the place's location subexpressions (receiver, or array expression → index expression) → `rhs` → the store. Place before value, as in C# and Java, and unlike Rust.

**E6 (compound assignment)**: `place op= rhs` is equivalent to `place = place op rhs`, **except that each subexpression of the place is evaluated exactly once**. The order is: place subexpressions → read the place → `rhs` → compute → write the place back. `x++` / `x--` are equivalent to `x += 1` / `x -= 1` and follow the same rule. Compound assignment targets are restricted to variables and member accesses (see §3.4).

**E7 (literals)**: Array, Set and tuple elements are evaluated in source order; Map and object literals are evaluated entry by entry in source order, key before value within an entry; a spread `...` is evaluated in sequence at the position where it appears.

**E8 (string interpolation)**: interpolated fragments are evaluated left to right in source order.

**E9 (match)**: the scrutinee is evaluated before any arm, exactly once. Arms are tried in source order; an `if` guard is evaluated only once its pattern has matched.

**E10 (slices and ranges)**: `a[lo:hi]` evaluates a → lo → hi; `lo..hi` and `lo..=hi` evaluate lo → hi.

**E11 (statements)**: statements are evaluated in source order. A `defer` evaluates its captures where it is registered; for when the body runs see §4.9.

```xray @id=eval-order-rules
fn t(tag: string, v: int) -> int { print(tag); return v }
fn add(a: int, b: int) -> int { return a + b }
fn pick(tag: string) -> fn(int, int) -> int { print(tag); return add }

class Counter {
    hits: int = 0
}

fn mk(tag: string) -> Counter { print(tag); return Counter() }

// E2: the callee is evaluated before the arguments
var sum = pick("callee")(t("arg1", 1), t("arg2", 2))   // callee, arg1, arg2

// E5: place subexpressions are evaluated before the right-hand side
var arr = [0, 0, 0]
arr[t("index", 0)] = t("value", 5)                     // index, value

// E6: a complex receiver is evaluated exactly once
mk("receiver").hits += t("delta", 1)                   // receiver, delta
```

### 3.1 Precedence and Associativity

Full precedence table (highest → lowest; operators at the same level share associativity):

| Level | Operators | Assoc. | Description |
|--|--|--|--|
| 17 | `(...)` `[...]` `.x` `?.x` `?[...]` `f()` `e!` | left | postfix: grouping, index, member, optional chain, call, force unwrap |
| 16 | prefix `-` `+` `!` `~` `move` `await` `go` `unsafe` `comptime` | right | unary prefix + coroutine/FFI/compile-time boundary operators |
| 15 | `as` `is` | left | type cast / check (`as T?` is the safe form via a nullable target type, not a separate `as?` operator) |
| 14 | `*` `/` `%` | left | multiplication / division / modulo |
| 13 | `+` `-` | left | addition / subtraction |
| 12 | `<<` `>>` | left | shifts |
| 11 | `..` `..=` | none | range: endpoints are arithmetic, so `0..n+1` is `0..(n+1)`; non-associative, `a..b..c` is a syntax error |
| 10 | `<` `<=` `>` `>=` | left | relational |
| 9 | `==` `!=` | left | equality |
| 8 | `&` | left | bitwise AND |
| 7 | `^` | left | bitwise XOR |
| 6 | `\|` | left | bitwise OR (also union types) |
| 5 | `&&` | left | logical AND (short-circuit) |
| 4 | `\|\|` | left | logical OR (short-circuit) |
| 3 | `??` | left | null coalescing |
| 2 | `? :` | right | ternary |
| 1 | `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` | right | assignment and compound assignment |
| 0 | `,` (only in `match` multi-value arms, argument lists, etc.) | — | not a real operator |


### 3.2 Unary Expressions

```ebnf
UnaryExpr ::= ('-' | '+' | '!' | '~') UnaryExpr
            | 'move' UnaryExpr
            | 'await' ('all' | 'any' | 'anySuccess')? UnaryExpr
            | 'go' (Block | PostfixExpr)
            | 'unsafe' Block
            | 'comptime' (Expression | Block)
            | PostfixExpr
```

| Operator | Applicable types | Result type | Notes |
|--|--|--|--|
| `-x` | numeric | same | negation; preserves float NaN |
| `+x` | numeric | same | identity, almost never useful |
| `!x` | `bool` | `bool` | logical not; **rejects non-bool** (unlike JS) |
| `~x` | integer | same | bitwise complement |
`++` / `--` are **not expressions**: expression-position uses such as `var y = x++`, `f(x++)`, `a[i++]`, and `return x++` are compile errors. Statement-level increment/decrement is specified in §4.1.

#### `unsafe { }`

`unsafe { ... }` is an explicit FFI/raw-pointer boundary expression. Inside the block, xray permits calls to `extern "C"` functions, reads/writes through `Ptr<T>` / `MutPtr<T>` foreign memory, and `deref()` calls that dereference raw pointers.

```xray
extern "C" {
    fn malloc(n: usize) -> MutPtr<byte>
    fn free(p: MutPtr<byte>)
}

var p = unsafe { malloc(1) }      // the final expression is the block result
unsafe {
    p[0] = 7                      // MutPtr writes must be inside unsafe
    print(p.deref())              // dereference must be inside unsafe
    free(p)                       // extern calls must be inside unsafe
}
```

`unsafe` does not change the expression's result type; in a multi-statement block, the trailing expression statement yields the block value, otherwise the result is `()`. `unsafe` also does not disable ordinary type checking: `Ptr<T>` is still read-only, and writes require `MutPtr<T>`; null pointers, bounds, lifetimes, and alignment remain the caller's responsibility.

#### `comptime expr` / `comptime { ... }`

`comptime` requires an expression or block to evaluate during analysis; failure is a compile error and never falls back to runtime. Constant expressions are not limited to integers: the current evaluator supports scalar constants, TypeIds, fixed arrays, tuples, struct aggregates and their member/index accesses, plus const-evaluable unary and binary operations. Static integer positions such as fixed-array lengths still require a positive integer result.

```xray
const SCALE = comptime 8 * 4
var buf: [byte; comptime SCALE + 2] = [0; SCALE + 2]
```

`comptime { ... }` has a restricted interpreter. A block supports local `const`/`var` declarations, local assignments and compound assignments, `if`/`while`, C-style `for`, fixed-array `for-in`, labeled or unlabeled `break`/`continue` inside loops, `compile_assert(...)`, and `compile_error(...)`. A statement block is erased from runtime; use `return <consteval-expression>` when the block must produce a value. Function calls are not currently consteval-safe, and unsupported statements are rejected during analysis.

```xray
const TABLE_SIZE = comptime 4 * 8

comptime {
    var sum = comptime 0
    for (var i = 0; i < 4; i += 1) {
        sum += i
    }
    compile_assert(sum == 6, "comptime loop")
}
```

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

| Operator | same-kind integers | same-kind floats | losslessly widenable numeric | integer×float | string / other |
|--|--|--|--|--|--|
| `+` | original integer type | original floating type | unique wider type | ❌ (explicit `as` required) | `string + string` concatenates; other ❌ |
| `-` | original integer type | original floating type | unique wider type | ❌ (explicit `as` required) | ❌ |
| `*` | original integer type | original floating type | unique wider type | ❌ (explicit `as` required) | ❌ |
| `/` | original integer type (truncating) | original floating type | unique wider type | ❌ (explicit `as` required) | ❌ |
| `%` | original integer type | ❌ | unique wider integer type | ❌ | ❌ |

“Losslessly widenable” means only a same-signed integer chain or `f32 → f64`. A direct numeric literal may acquire the unique context of the other already-typed operand; two already-typed operands never use C-style usual arithmetic conversions.

**Special semantics**:
- `int / 0` → throws `XR_ERR_DIV_BY_ZERO` (E0420) at runtime.
- `int % 0` → throws `XR_ERR_MOD_BY_ZERO` (E0421) at runtime.
- Division whose result type is `float`/`f32` follows IEEE-754: `1.0 / 0.0` produces `+inf`, `-1.0 / 0.0` produces `-inf`, and `0.0 / 0.0` produces `NaN`; use `x.isNaN()` or `math.isNaN(x)` to test NaN.
- `%` accepts integer operands only; modulo with a static type that contains float (e.g. `5.0 % 2.0`) is a compile-time analyzer error. Runtime `XR_ERR_TYPE_MISMATCH` (E0404) remains only as a dynamic fallback.
- Integer overflow: see §2.3.1.
- `string + string` is O(n) concatenation; for heavy concatenation use `StringBuilder`.
- `rune` is an independent Unicode scalar type and does not participate in arithmetic; use `int(c)` explicitly when the code point is needed.

#### 3.3.2 Bitwise Operators

`&` `|` `^` `~` `<<` `>>`

- Apply only to integer types.
- Shift counts are taken modulo 64 (unlike C: always defined in xray).
- `>>` is an **arithmetic right shift** (preserves the sign bit). For unsigned shifts, use the corresponding `uintN`.
- `bool` does not participate in bitwise operations (use `&&` `||`).
- `rune` does not participate in bitwise operations; use `int(c)` explicitly when the code point is needed.

#### 3.3.3 Comparison Operators

| Operator | Semantics |
|--|--|
| `==` | value equality. Numeric operands must have the same type or a unique lossless common type; integer-vs-float and different-signedness integers require an explicit conversion first. Strings compare by content. class/struct uses `==` overload or default identity. |
| `!=` | inverse of `==` |
| `<` `<=` `>` `>=` | supported by numbers and strings; other types are unsupported by default (enable via `operator<` overload). |

**Difference vs. JS / C**: xray's `==` does not perform string↔number conversion, integer↔float promotion, or implicit signedness changes.

#### 3.3.4 Logical Operators

`&&` `||`:

- Both operands **must** be `bool` (checked at compile time).
- Short-circuit evaluation: `false && X` does not evaluate `X`; `true || X` does not evaluate `X`.
- Result type is `bool` (unlike JS, which returns one of the operands).
- **Narrowing inheritance (§2.13 N-5)**: in `e1 && e2`, `e2` is analyzed under the true fact of `e1`; in `e1 || e2`, `e2` is analyzed under the false fact of `e1`. Since `T?` cannot be used directly as a condition, `a != null && a.f` and `a == null || a.f` are the standard forms for nullable values.

#### 3.3.5 Null Coalescing `??`

```xray @id=expr-null-coalesce
var v = nullable_expr ?? default_value
```

- Returns `default_value` when `nullable_expr` is `null`; otherwise returns `nullable_expr` itself.
- **Short-circuit**: `default_value` is evaluated only when the left side is null.
- Type inference: if `nullable_expr: T?` and `default_value: T`, the result type is `T` (non-null).
- Applies only to nullable types; using `??` on a non-null `T` is a compile warning/error.

### 3.4 Assignment and Compound Assignment

```ebnf
AssignExpr   ::= AssignLValue '=' Expression
               | CompoundLValue CompoundOp Expression
AssignLValue ::= Identifier | MemberAccess | IndexAccess
CompoundLValue ::= Identifier | MemberAccess
CompoundOp   ::= '+=' | '-=' | '*=' | '/=' | '%='
               | '&=' | '|=' | '^=' | '<<=' | '>>='
```

**Semantics**:
- Assignment is an **expression**; its result is the assigned value (chainable: `a = b = 0`).
- `x op= y` is equivalent to `x = x op y`, but each subexpression of `x` is evaluated exactly once (important: `obj.f += 1` does not call `f`'s getter twice, and `mk().f += 1` calls `mk()` once). For the full ordering see §3.0 E6.
- **Compound assignment does not accept an index target**: `a[i] += v` is a compile error; write `a[i] = a[i] + v`. Plain assignment `a[i] = v` is unaffected.
- Cannot assign to a `const` (compile error `E0303`).

**Special cases**:
- A default parameter is a read-only borrow; only `ref` permits mutation through a place, while `move` consumes source ownership.
- Array/Map field assignment: `a[i] = v` calls `operator[]=` or the built-in setter.

### 3.5 Ternary `? :`

```ebnf
TernaryExpr ::= LogicOrExpr ('?' Expression ':' Expression)?
```

```xray
var max = a > b ? a : b
```

- **Right-associative**: `a ? b : c ? d : e` = `a ? b : (c ? d : e)`.
- The condition must be `bool`.
- The two branches share a unified type (taken as the common supertype or a union).
- **Narrowing (§2.13 N-7)**: the true fact of the condition applies to the then arm and the false fact to the else arm, exactly as in `if` — `a != null ? len(a) : 0` is legal.

### 3.6 Null Coalescing `??` and Optional Chaining `?.` / `?[`

See §3.3.5 (`??`) and below (`?.` / `?[`).

#### Optional chaining `?.` / `?[`

```ebnf
OptionalChain ::= Primary ('?.' Identifier | '?.' '(' ArgList? ')' | '?[' Expr ']')+
```

```xray @id=expr-optional-chain
var city = user?.address        // optional property access
var item = arr?[0]              // optional index
var value = callback?.(input)   // optional function call
var road = user?.address.street // whole-chain short-circuit: null when user is null
```

**Semantics**:
- If the LHS of `?.` or `?[` is `null`, the **entire postfix chain** short-circuits to `null`; every later `.` / `[` / `(` in the chain is skipped.
- `?.` is for property access, method calls, and function calls: `obj?.prop`, `obj?.method()`, `func?.(args)`.
- `?[` is for index access: `arr?[0]`. Symmetric with regular indexing `arr[0]` — just add `?` before `[`.
- `func?.(args)` does not evaluate its arguments when the function value is `null`; it returns `null` directly.
- **Whole-chain short-circuit**: `a?.b.c.d` means "`null` when `a` is `null`, otherwise evaluate `a.b.c.d`". Every `?.` is a short-circuit point; a `.` after a short-circuit point needs no `?` and does **not** raise `E0379`, because it is evaluated only when the prefix is non-null.
- When a link itself yields a nullable value (`b: T?` in `a?.b`), continuing with `.` on that value still requires `?.`: the short-circuit covers a null LHS of `?.`, not a null result on its RHS.
- Result type: the type of the whole chain plus `?` (already-nullable types remain unchanged). §2.13 N-13.

### 3.7 Force Unwrap `!`

> Full error-handling semantics are in §8. This section only lists the expression syntax and brief semantics.

#### Force unwrap `expr!`

```xray
var v: int = nullable_int!      // throws NullThrowError (E0410) at runtime when null
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
- **Type guard**: when `v` is a simple binding, the analyzer narrows it to its intersection with `T` in the true branch and removes that intersection in the false branch, per §2.13 N-4 / N-6.
- Applies to union, nullable, class hierarchies, and `Json` structural matching.
- **Fixed-width numeric types**: a dynamically erased value keeps only its i64 or f64 family, not its width, so `v is i32` asks whether the value is exactly representable in `i32` — the only form the erased value can answer. `is int` / `is float` hold for the whole family. `v as T?` uses the same predicate and yields `null` when it does not hold.

#### `as` type cast

```ebnf
AsExpr ::= UnaryExpr 'as' Type
        |  UnaryExpr 'as' Type '?'
```

```xray
var n = v as int           // throws TypeError on failure
var n = v as int?          // returns null on failure (the "as nullable" safe form)
```

| Form | Failure behavior | Use case |
|--|--|--|
| `expr as T` | throws `XR_ERR_TYPE_MISMATCH` (E0404) | a cast that must succeed |
| `expr as T?` | returns `null` | a fallible dynamic / structural cast; not a numeric conversion |

**Supported conversions**:
- Between numeric types: integer-to-integer reduces modulo the target width and is interpreted with the target signedness; integer-to-float and `f64 → f32` use IEEE-754 round-to-nearest, ties-to-even; float-to-integer truncates toward zero and throws `XR_ERR_OVERFLOW` (E0422) for NaN, infinity, or an out-of-range value. Numeric conversion uses only `expr as T`, never the nullable form.
- `Json → T` (runtime structural check against `T`).
- Parent → child (runtime `instanceof`).
- Union member → concrete member.

### 3.9 Range `..` / `..=` and Spread `...`

#### Range `a..b` / `a..=b`

```ebnf
RangeExpr ::= ShiftExpr (('..' | '..=') ShiftExpr)?
```

```xray @id=expr-range
var halfOpen = 0..10   // left-closed right-open (includes 0, excludes 10)
var closed = 0..=10    // closed interval (includes both 0 and 10)
var r = 1..100
var n = 10
for (i in 0..n) { print(i) }
for (i in 0..=n) { print(i) }
for (i in 0..n+1) { print(i) }   // endpoint binds first: 0..(n+1)
```

- Type: `Range` (int ranges only).
- **Precedence (see §3.1)**: `..` / `..=` bind looser than every arithmetic operator (`* / % + - << >>`), so endpoints group first — `0..n+1` is `0..(n+1)` and `1..2*3` is `1..(2*3)`; they bind tighter than comparison and logical operators, so `0..n == 0..m` is `(0..n) == (0..m)`.
- **Non-associative**: ranges do not chain; `a..b..c` is a syntax error. Parenthesize an endpoint if a nested range is intended.
- `a..b` is the half-open interval `[a, b)`: `a` is included, `b` is not.
- `a..=b` is the inclusive interval `[a, b]`: both endpoints are included.
- `for-in`, `Range.contains`, `len(range)`, `Range.toArray()`, and range patterns in `match` all use the corresponding endpoint semantics.
- Primary uses: `for-in` loops, range checks in pattern matching.

#### Spread `...`

Allowed in the following positions only:
- **Function rest parameter declaration**: `fn f(...args: int)`
- **Function call spread**: `f(...args)`; the spread source must be a tuple whose arity is statically known.
- **Tuple literal spread**: `(head, ...tail)`; the spread source must be a tuple whose arity is statically known.
- **Array literal spread**: `[...a, x, ...b]`; the spread source must be an array. The result is a new array built by runtime concatenation (O(n)).
- **Object-literal spread**: `{...base, x: 1}`; the spread source must be an object. Fields are merged into a new object; on a name clash the later field wins, and the result field set is the union of every source's fields and the literal fields.

```xray @id=expr-spread-collections
var a = [1, 2]
var b = [3, 4]
var nums = [...a, 99, ...b]            // [1, 2, 99, 3, 4]

var base = { x: 1, y: 2 }
var point = { ...base, y: 20, z: 3 }   // { x: 1, y: 20, z: 3 }
```

### 3.10 Literal Construction

#### Array `[...]`

```ebnf
ArrayLit  ::= '[' (ArrayElem (',' ArrayElem)* ','?)? ']'
ArrayElem ::= '...' Expr | Expr
```

```xray @id=expr-array-lit
var a = [1, 2, 3]
var empty: Array<int> = []
var mixed = [1, "hello"]    // type Array<int | string>
```

#### Map `#{k: v, ...}` and `#{}`

```ebnf
MapLit   ::= '#{' (MapEntry (',' MapEntry)* ','?)? '}'
MapEntry ::= Expression ':' Expression
EmptyMap ::= '#{' '}'    // note: '#{' is a single token
```

```xray @id=expr-map-lit
var m = #{"a": 1, "b": 2}
var empty = #{}                           // empty Map
```

**Key distinction**: `{}` is always a **Json / Object**; `#{}` is always a **Map**. Both use `:` between key and value; the `#` prefix is the disambiguator.

#### Set `#[...]`

```ebnf
SetLit ::= '#[' (Expr (',' Expr)* ','?)? ']'
```

```xray @id=expr-set-lit
var s = #[1, 2, 3]
var empty = #[]
```

#### Object (structured object) `{ field: value, ... }`

```ebnf
ObjectLit  ::= '{' ObjectField (',' ObjectField)* ','? '}'
ObjectField ::= Identifier ':' Expr
              | Identifier            // shorthand: `{ x }` ≡ `{ x: x }`
              | '...' Expr            // spread: `{ ...base }` merges fields
```

```xray @id=expr-object-lit
var p = { name: "Alice", age: 30 }
var users = "Bob"
var obj = { users }              // shorthand
```

- Defaults to an exact object shape (see §2.4.7); the field set and offsets are fixed at compile time for AOT fast paths.
- It is interpreted as a dynamic Json object literal only under an explicit `Json` expected type; use `Json.encode(value)` when a typed value crosses a JSON boundary.
- Name the structural object with a `type` alias: `var u: User = {...}` (compile-time field check, sealed).

#### Array<byte> `Array<byte>(...)`

See §2.4.6 and §14.5.

#### Channel `Channel<T>(buf?)`

```xray
const ch: Channel<int> = Channel<int>(10)
```

See §10.5.

### 3.11 Calls / Member Access / Indexing / Slicing

#### Function call

```ebnf
CallExpr ::= Primary '(' ArgList? ')'
ArgList ::= CallArg (',' CallArg)* ','?
CallArg ::= ('ref' | 'out')? Expr
```

- Arguments are passed positionally; named arguments are not supported.
- A `ref` or `out` parameter repeats the same marker at the call site and must receive an addressable place; ordinary `in`/value parameters have no call-site marker.
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
var bytes: Slice<byte> = text.bytes()
bytes[i]                // explicit byte-view index
```

- `Array` indexing: `int`; out-of-bounds throws `E0430`.
- `Map` indexing: key type; missing key → `E0431`.
- Integer indexing a `string` is a compile error; use `runes().nth(i)` or `bytes()[i]` to select the unit explicitly.
- User classes: via `operator[]` overload.

#### Slice

```ebnf
Slice ::= Primary '[' Expr? ':' Expr? ']'
```

```xray @id=expr-slice
arr[1:4]                // elements [1, 4)
arr[:3]                 // first 3
arr[2:]                 // from index 2 to the end
arr[:]                  // full-length view (not a copy)
var view: Slice<int> = arr[1:4]
```

- Half-open interval `[start, end)`.
- Array slicing supports negative indices: a negative index is converted using `len(array) + index` and then clamped to the valid range.
- Strings do not support the slice operator; use strict rune-ordinal `s.slice(start, end)`.
- A slice expression evaluates to a **borrowed view** of type `Slice<T>`, selected by its target type; no elements are copied. Every form, including `arr[:]`, is a view: writing through the view writes straight into the owner's storage, and element writes on the owner are immediately visible through the view. Use `copy(arr[1:4])` when you need independent data.
- A slice expression is a borrow: while the view is live the owner is constrained by the borrow rules in §2.4.2, and the view itself must not escape the owner's scope. See §2.4.2 for the full rules.

### 3.12 Anonymous Functions and Lambdas

Xray has only two anonymous-function concepts: **arrow lambdas** and **fn lambdas**. A single unannotated read parameter may omit parentheses, so `x -> expr` and `(x) -> expr` are two parenthesizations of one concept, not two function kinds. Both compile to the same `AST_FUNCTION_EXPR` node; they differ in the signature information that source code may carry.

```ebnf
AnonFunction  ::= ArrowLambda | FnExpression
ArrowLambda   ::= Identifier '->' ArrowBody
                | '(' ArrowParams? ')' '->' ArrowBody
ArrowBody     ::= Expression | Block
ArrowParams   ::= ArrowParam (',' ArrowParam)* ','?
ArrowParam    ::= Identifier (':' ParamMode? Type)?
ParamMode     ::= 'ref' | 'move'
FnExpression  ::= 'fn' GenericParams? '(' ArrowParams? ')' ('->' Type)? Block
```

```xray @id=expr-lambda-forms
// ── Arrow lambda: each parameter may be annotated or inferred independently ──
arr.map(x -> x * 2)
var add = (x: int, y) -> x + y

// Expression bodies return implicitly; ref only grants a writable loan
var calculate = (x: ref int) -> x * 2
var doubleInPlace = (x: ref int) -> { x = x * 2 }

// ── fn lambda: explicit return type, generics, or a full parameter contract ──
var inc = fn(x: int) -> int {
    var y = x + 1
    return y
}
var identity = fn<T>(x: T) -> T { return x }     // generic
```

**Selection rule**:

| Concept | Syntax | Capability and selection rule |
|---------|--------|-------------------------------|
| Arrow lambda | `x -> expr` / `(x: T, y) -> expr` / `(x: ref T) -> { ... }` | Parameters may be annotated independently; the return type is always inferred; expression bodies return implicitly and block bodies explicitly |
| fn lambda | `fn(x: T) -> R { ... }` / `fn<T>(...) { ... }` | Use for an explicit return type, generics, or a full declaration-style signature |

**Key rules**:
- Arrow parameter annotations are independent, so `(x: int, y) -> ...` is valid. Each unannotated parameter is inferred from binding types, callee signatures, container element types, and body constraints. Failed inference raises `E0365`; annotate that parameter or its binding, provide call-site context, or use a fully explicit `fn` signature.
- Parameter modes use the shared postfix annotation syntax: `x: ref T` and `x: move T`. `(ref x) -> ...`, default parameters, and rest parameters are not arrow syntax; defaults and rest parameters belong to named function declarations.
- An arrow lambda has no return-type position. Both `(x: T): R -> ...` and `(x: T) -> R { ... }` are errors; an explicit return type requires `fn(x: T) -> R { ... }`.
- `-> expr` implicitly returns the expression value. `-> { ... }` supports multiple statements; value returns require `return value`, normal fallthrough returns `()`, and a final expression has no special implicit-return semantics.
- `ref` means that the caller supplies a writable loan; it does not mean automatic writeback. `(x: ref int) -> x * 2` only reads and returns a product, leaving the caller unchanged. `(x: ref int) -> { x = x * 2 }` mutates the caller in place and returns `()`. To mutate and return, assign in a block and then write `return x`.
- Tooling may emit a non-mandatory hint when an anonymous-function parameter is explicitly written as `ref` and complete effect analysis proves it is never mutated. The hint is suppressed when an expected callable contract requires `ref`, the mode was inferred, or effect analysis is incomplete; it never changes type checking or runtime semantics.
- Capture rules: see §7.4. A `go` closure consumes the unified provenance-based capture plan: inline values, published const values, and audited synchronization handles may be captured directly; execution-local graphs, module-mutable state, and views/pointers with insufficient lifetime are rejected and must cross as explicit `copy(...)` / `move` arguments.

### 3.13 `match` Expression

```ebnf
MatchExpr ::= 'match' Expr '{' MatchArm (',' MatchArm)* ','? '}'
MatchArm ::= Pattern ('if' Expr)? '->' Expression
```

```xray @id=expr-match
var result = match (x) {
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

Construction has the same form as a function call: `TypeName(args)`. `new` is reserved and does not form an expression.

```xray @id=expr-new
var p = Point(1.0, 2.0)
var arr = Array<int>()
const ch = Channel<int>(10)
var m = Map<string, int>()
```

**Used for**:
- Class and struct instantiation (`TypeName(args)`).
- Constructing built-in container types (`Array` / `Map` / `Set` / `Channel` / `Array<byte>` / `StringBuilder`, etc.; also `TypeName(args)`).
- Disambiguation is by symbol kind in the analyzer: type names construct, function names call (naming convention: types capitalized, functions lowercase).

**Relation to literals**:
```xray @id=expr-literal-constructor-relation
var a = [1, 2, 3]              // equivalent to Array<int>() + push
var m = #{}                    // equivalent to Map<...>()
var p = Point{x: 1, y: 2}      // struct literal
```

### 3.15 String Interpolation

See §1.6.5. In brief:

```xray @id=expr-string-interpolation
var greeting = "Hello, ${name}! Age: ${user.age + 1}"
```

- `${...}` accepts any expression (calls, object access, arithmetic).
- Embedded string literals inside `${...}` may use the same quote as the outer template; the lexer matches expression braces by depth and skips nested strings / raw strings / rune literals.
- The expression's type must be convertible to a string (implement `toString()` or be a primitive).

### 3.16 `yield` Statement

```xray @id=expr-yield
yield expr                  // produce one generator value and suspend
```

`yield expr` is only valid inside a generator function declared to return `Iterator<T>`. Calling a generator function does not immediately execute its body; it returns a lazy `Iterator<T>` (protocol in §5.3.6). `for-in` pulls through `hasNext()` / `next()`, and each `yield expr` produces one `T` before suspending until the next pull.

Cooperative CPU yielding uses `Coro.yield()` (see §10.10); bare `yield` is not an expression.

#### 3.16.1 The two suspensions are different semantics

`yield expr` and `Coro.yield()` share a word root but are **not the same kind of suspension**. They differ in every property a caller can observe:

| | `yield expr` | `Coro.yield()` |
|---|---|---|
| Transfer target | symmetric transfer back to the iterator driving the generator | asymmetric yield to the scheduler |
| Can migrate OS threads | no | yes (work stealing) |
| Is a cancellation checkpoint | **no** | yes (§10.5) |
| Propagates to the caller | **no** — driving a generator does not suspend the driver's own frame | yes, along call edges |
| Effect contract | rejected by `no_suspend`, accepted by `no_reschedule` | rejected by both |

`xray verify` therefore treats suspension as two independent dimensions: `no_reschedule` asserts "never reaches the scheduler, is not a cancellation point, cannot migrate threads", and `no_suspend` is the stronger "control never leaves this frame". A pure generator satisfies the first but not the second; **a function that merely drives a generator satisfies both**.

#### 3.16.2 A generator body must not reach the scheduler

A generator frame is resumed by whoever drives it, never by the scheduler. A generator function body therefore **must not contain any scheduler suspension point**: `await`, `select`, a `scope` block, `Coro.yield()`, the blocking channel methods, `time.sleep()`, or a call to any function that may suspend. Violations report `E0385`.

The rule is an **effect rule, not a syntactic one**: it composes through the §8 effect product, so calling a function that awaits internally is rejected the same way. Incomplete evidence **fails closed** — a generator that calls through an unresolved function value (such as a function-typed parameter) is rejected, because that target may suspend. This matches the boundary rules for `sys.Thread.spawn` bodies and parallel callbacks.

> The restriction is a deliberate scope decision, not an implementation gap: generators in this version are pull-driven iterators, not async streams.

#### 3.16.3 An abandoned generator runs no further code

When `for-in` leaves early through `break` / `return` / a thrown error, or the iterator is discarded, the generator frame is **not resumed** and the rest of its body never executes. Reclamation timing is not part of the observable semantic contract (§16.8).

A generator body therefore **must not use `defer`** (`E0386`). `defer` is the language's only deterministic cleanup mechanism, and here it might never run; a cleanup that may not run is more dangerous than no cleanup. When cleanup is required, the caller owns the resource and passes it in.
<!-- /xr-spec:en -->

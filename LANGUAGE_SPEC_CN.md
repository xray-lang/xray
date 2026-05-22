# Xray 语言参考手册

> 版本：基于 `xray` v0.7.1 源码（截止 2026-05-21）
> 性质：语言规范与参考手册。本文档是描述 xray 语言**实际行为**的真值源。
> 实现：所有语义以 `xray` 当前主仓代码为准；本文档与代码不一致以代码为准并视为本文档需更新。
> 受众：xray 编写者、IDE / AI 工具实现者、编译器内部贡献者。

## 目录

- [0. 前言](#0-前言)
- [1. 词法结构 (Lexical Structure)](#1-词法结构-lexical-structure)
- [2. 类型系统 (Type System)](#2-类型系统-type-system)
- [3. 表达式 (Expressions)](#3-表达式-expressions)
- [4. 语句 (Statements)](#4-语句-statements)
- [5. 声明 (Declarations)](#5-声明-declarations)
- [6. 模式 (Patterns)](#6-模式-patterns)
- [7. 作用域与名字解析 (Scoping)](#7-作用域与名字解析-scoping)
- [8. 错误处理 (Error Handling)](#8-错误处理-error-handling)
- [9. 泛型 (Generics)](#9-泛型-generics)
- [10. 并发与协程 (Concurrency)](#10-并发与协程-concurrency)
- [11. 模块系统 (Modules)](#11-模块系统-modules)
- [12. 测试 (Testing)](#12-测试-testing)
- [13. 内置函数 (Built-in Functions)](#13-内置函数-built-in-functions)
- [14. 内置类型方法 (Built-in Type Methods)](#14-内置类型方法-built-in-type-methods)
- [15. 标准库概览 (Standard Library)](#15-标准库概览-standard-library)
- [16. 运行时模型 (Runtime Model)](#16-运行时模型-runtime-model)
- [17. 编译流水线 (Compilation Pipeline)](#17-编译流水线-compilation-pipeline)
- [18. 错误码参考 (Error Code Reference)](#18-错误码参考-error-code-reference)
- [附录 A. EBNF 语法](#附录-a-ebnf-语法)
- [附录 B. 关键字索引](#附录-b-关键字索引)
- [附录 C. 操作符索引](#附录-c-操作符索引)
- [附录 D. 标准库模块索引](#附录-d-标准库模块索引)
- [附录 E. 与其他语言的差异](#附录-e-与其他语言的差异)
- [附录 F. 词汇表](#附录-f-词汇表)

---

## 0. 前言

### 0.1 关于本规范

本文档是 Xray 编程语言的**参考手册**（reference manual），描述语言的词法、语法、类型系统、语义、并发模型、运行时与标准库的接口。它的目标是：

1. 让人类阅读后能写出合法且行为可预测的 xray 代码；
2. 作为编译器、分析器、IDE、AI 助手、文档生成器等工具的**结构化真值源**；
3. 与 `xray` 主仓的实际实现保持一致——任何不一致都视为本文档或代码的 bug。

**本手册不是教程**。完整的入门材料见 xray 官网与 `demos/` 目录。

### 0.2 版本约定

本手册版本号与 `xray` 主仓 (`CMakeLists.txt` 的 `project(Xray VERSION x.y.z)`) 严格一致。重大破坏性改动以章节内的"自 vX.Y.Z 起"标注。

xray 当前处于 v0.x 阶段。**不承诺任何向后兼容**。规范每次发版可能引入破坏性改动。

### 0.3 语言设计哲学

Xray 是一个**轻量级静态类型脚本语言，原生支持并发**。设计目标：

| 维度 | 取舍 |
|--|--|
| **类型** | 静态类型 + 类型推断；变量声明几乎不需要写类型标注，但类型系统在编译期完全可见 |
| **并发** | 内置 M:N 协程（go / await / Channel / scope / select），并发安全在编译期由"显式共享"规则保证 |
| **运行模式** | VM 解释 / JIT / AOT 三档，对开发者透明；语义在三种模式下严格一致 |
| **错误处理** | 异常机制（throw / try / catch / finally）+ `Result<T,E>` + 可空类型（T?）+ defer 资源管理 |
| **元编程** | 注解（`@test` / `@native` / `@deprecated`）+ 运行时反射（Reflect）+ 泛型 reified |
| **互操作** | C ABI 内置；stdlib 模块可由 C 编写并通过 `XR_DEFINE_BUILTIN` 暴露 |

设计参考来源：TypeScript（类型推断 + nullable）、Go（结构化并发 + Channel）、Rust（所有权语义的轻量版 move）、Swift（协议 + 可空链）。**Xray 不是其中任何一者的克隆**。

### 0.4 阅读约定

#### 0.4.1 语法记法

本文档使用一种轻量化的 EBNF 风格描述语法：

| 符号 | 含义 |
|--|--|
| `Term` | 非终结符（capitalised） |
| `'literal'` | 字面 token |
| `A B` | 序列 |
| `A \| B` | 选择 |
| `A?` | 可选 |
| `A*` | 零次或多次 |
| `A+` | 一次或多次 |
| `(A)` | 分组 |

完整 EBNF 见 [附录 A](#附录-a-ebnf-语法)。

#### 0.4.2 源码引用

本文档大量引用 xray 主仓源码作为真值源。引用格式：

```
路径:行号
```

例：`src/frontend/lexer/xkeywords.def`、`src/frontend/parser/xast_types.h:42-58`。

#### 0.4.3 状态标记

| 标记 | 含义 |
|--|--|
| **稳定** | 默认状态；行为不会无预警变化 |
| **实验性** | 实现存在但可能改变 |
| **保留** | 关键字/语法已识别但当前禁用 |
| **未实现** | spec 中描述但代码暂未支持，应显式标注实现状态 |

#### 0.4.4 错误码引用

错误码使用 `E0xxx` 格式（如 `E0101`），完整列表见 [第 18 章](#18-错误码参考-error-code-reference)。源码定义在 `src/runtime/xerror_codes.h` 与 `src/runtime/xerror.h`。

---

## 1. 词法结构 (Lexical Structure)

> 真值源：`src/frontend/lexer/xlex.h`（token 枚举）、`src/frontend/lexer/xkeywords.def`（关键字表，63 条）、`src/frontend/lexer/xlex.c`（扫描器实现）。

### 1.1 字符编码

xray 源文件**必须**是 UTF-8 编码。所有源码处理（包括字符串字面量、标识符、注释）按 UTF-8 字节序列进行；非 ASCII 字符仅在字符串字面量、注释、原始字符串内部允许（标识符暂只支持 ASCII，见 §1.4）。

文件可选 UTF-8 BOM（`EF BB BF`）；扫描器跳过开头的 BOM。

### 1.2 行结尾与空白

行结尾识别 `\n`（Unix）与 `\r\n`（Windows）。`\r` 单独出现视为非法字符。

**空白字符**：空格 (`U+0020`)、水平制表符 (`U+0009`)、行结尾。空白用于分隔 token，不传递语义（**异常**：泛型语境下连续 `>>` 的拆分依赖空白上下文）。

### 1.3 注释

xray 支持两种注释，**均不嵌套**：

```xray
// 行注释，从 // 到行尾
/* 块注释，
   可跨行；
   不支持嵌套：内部出现 /* 不开启新层级 */
```

注释可出现在任何空白能出现的地方。注释会被收集为 **trivia**，供 formatter 与 LSP 使用（见 `src/frontend/parser/xtrivia.*`），但不参与语法分析。

文档注释（与普通注释无语法差异）：约定以 `///` 或 `/** */` 开头，用于工具识别。当前编译器不强制此约定。

### 1.4 标识符

```ebnf
Identifier ::= IdentStart IdentCont*
IdentStart ::= 'a'..'z' | 'A'..'Z' | '_'
IdentCont  ::= IdentStart | '0'..'9'
```

仅 ASCII。最大长度受编译器限制（约 255 字节）。

**保留约束**：标识符不能与保留关键字相同（见 §1.5）；可与**上下文敏感关键字**相同（如 `from`、`to`、`default`、`ref`、`move`、`linked`、`supervisor`、`after` 可作为普通标识符）。

字符 `_` 是**专用通配符 token**，不是普通标识符：

- 在 `match` 模式中表示**通配符**（见 §6.7）。
- 在 `for-in` 中可用于忽略键或值：`for (_, v in m) { ... }`。
- 在解构绑定中可用于忽略位置：`let (a, _) = (1, 2)`。
- **不能**作为 `let _ = expr`、函数参数名或被引用的变量名；编译器会报"expected variable name"。
- 多下划线名（如 `__tmp`）是普通标识符。

### 1.5 关键字

xray 共 **63 个保留关键字**，源码真值表见 `src/frontend/lexer/xkeywords.def`。关键字按用途分组：

#### 1.5.1 声明与流程控制

| 关键字 | 用途 |
|--|--|
| `let` | 可变变量声明 |
| `const` | 不可变变量声明 |
| `shared` | 跨协程共享修饰符（与 `const`/`let` 组合） |
| `fn` | 函数声明 |
| `return` | 函数返回 |
| `yield` | 协程让出（语句形式）|
| `if` `else` | 条件分支 |
| `while` | 循环 |
| `for` `in` | 循环（C 风格 + for-in） |
| `break` `continue` | 循环控制 |
| `match` | 模式匹配 |

#### 1.5.2 面向对象与类型

| 关键字 | 用途 |
|--|--|
| `class` `struct` | 类/结构体声明 |
| `extends` | 类继承 |
| `interface` `implements` | 接口声明/实现 |
| `enum` | 枚举声明 |
| `type` | 类型别名 |
| `new` | 实例化 |
| `this` `super` | 自我/父类引用 |
| `constructor` | 构造器 |
| `static` `private` `public` | 可见性修饰符（`public` 是**默认**，几乎从不显式写出） |
| `abstract` `final` `override` | 类/方法修饰符（`override` 是**可选**——重写父类方法不要求显式标注） |
| `operator` | 运算符重载 |
| `is` `as` | 运行时类型检查 / 转换 |

#### 1.5.3 异常处理

`try` `catch` `finally` `throw`

#### 1.5.4 模块系统

`import` `export`

#### 1.5.5 协程与并发

`go` `await` `select` `defer` `scope`

#### 1.5.6 类型名（保留）

`int` `int8` `int16` `int32` `int64` `uint8` `uint16` `uint32` `uint64`
`float` `float32` `float64` `bool` `string` `unknown`

> **注意**：以下名字**不是**词法关键字，而是 `prelude` 自动引入的内置类型符号：
> `Array` · `BigInt` · `Bytes` · `Channel` · `DateTime` · `Exception` · `Json` · `Logger` · `Map` · `NetConn` · `NetListener` · `Range` · `Regex` · `Set` · `StringBuilder`。
> `Result<T, E>` 是当前错误处理与 `catch!` 路径使用的内置 ADT enum，也可直接使用。
> 它们可被用户类同名覆盖（局部 shadow），但通常无须 import 即可使用。

#### 1.5.7 字面量关键字

`true` `false` `null`

#### 1.5.8 上下文敏感关键字

不在 lexer 关键字表中，由 parser 按位置识别。**可以**作为普通标识符使用：

| Token | 出现位置 |
|--|--|
| `from` | `select` 的接收分支 (`x from ch`)；也用于命名 import / re-export (`import { x } from "module"`) |
| `to` | `select` 的发送分支 (`value to ch`) |
| `default` | 保留，当前未启用 |
| `cancelled` | `cancelled()` 协程取消检查（实际上是 builtin 函数）|
| `ref` | 函数参数修饰符 (`fn f(p: ref T)`) |
| `move` | 所有权转移 (`move x`) |
| `linked` | `linked go` / `linked scope` 修饰符 |
| `supervisor` | `supervisor scope` 修饰符 |
| `after` | `select` 的超时分支 (`after 1000 -> ...`) |

### 1.6 字面量

#### 1.6.1 整数字面量

```ebnf
IntLiteral ::= DecLit | HexLit | OctLit | BinLit
DecLit ::= Digit (Digit | '_')*
HexLit ::= '0x' HexDigit (HexDigit | '_')*
OctLit ::= '0o' OctDigit (OctDigit | '_')*
BinLit ::= '0b' BinDigit (BinDigit | '_')*
```

- 千位分隔符 `_` 仅用于可读性，可出现在数字之间任意位置。
- 字面量默认类型为 `int`（= `int64`）。后缀 `n` 转为 `BigInt`（见 §1.6.3）。
- 范围：`int64` 表示范围 `[-(2^63), 2^63 - 1]`；溢出在编译期检测。

```xray
42
0xFF
0b1010
0o77
1_000_000      // 一百万
```

#### 1.6.2 浮点字面量

```ebnf
FloatLiteral ::= Digit+ '.' Digit* Exp?
              | Digit+ Exp
              | '.' Digit+ Exp?
Exp ::= ('e' | 'E') ('+' | '-')? Digit+
```

字面量类型为 `float`（= `float64`，IEEE-754 双精度）。

```xray
3.14
1.0e10
2.5E-3
.5             // 等价 0.5
```

#### 1.6.3 BigInt 字面量

```ebnf
BigIntLiteral ::= (DecLit | HexLit | OctLit | BinLit) 'n'
```

```xray
123n
0xFFn
0b1010n
```

任意精度整数，运算永不溢出。类型见 §14.8。

#### 1.6.4 布尔与 null 字面量

```xray
true
false
null
```

- `true` / `false`：类型 `bool`。
- `null`：类型 `null`（语义上是所有可空类型 `T?` 的零值）。

#### 1.6.5 字符串字面量

xray 支持两类字符串字面量：**带转义** 和 **原始字符串**。两者均使用双引号或单引号，且均支持 `${...}` 插值。反引号字符串不属于当前语法——lexer 直接报错。

##### 普通字符串（双引号 / 单引号）

```ebnf
StringLiteral ::= '"' StrChar* '"' | "'" StrChar* "'"
StrChar ::= 任何非引号、非反斜杠、非换行符
          | EscapeSeq
          | Interpolation
EscapeSeq ::= '\' ('"' | "'" | '\\' | 'n' | 't' | 'r' | '0'
                  | 'x' HexDigit{2}
                  | 'u' HexDigit{4}
                  | 'u{' HexDigit{1,6} '}')
Interpolation ::= '${' Expression '}'
```

- 双引号 / 单引号**完全等价**——都支持转义、`${...}` 插值。
- 字符串可跨行；行结尾包含在字符串中。
- 包含插值的字面量在 lexer 内部产出 `TK_TEMPLATE_STRING`；不含插值的产出 `TK_LITERAL_STRING`。

```xray
"hello"
'world'
"Hello, ${name}! ${1 + 2}"
'tab\there\nnewline'
"\u4F60\u597D"        // "你好"
"\u{1F600}"            // emoji
```

**插值表达式内禁止再嵌套未转义的引号字符**（lexer 限制）。

##### 原始字符串（`r` 前缀）

```ebnf
RawString ::= 'r' ('"' RawChar* '"' | "'" RawChar* "'")
RawChar ::= 任何非引号字符（包括 `\`，不做转义处理）
```

- **不**处理任何转义（`\n`、`\t` 等保持原样）。
- 仍然支持 `${...}` 插值。
- 标识符 `r` 单独使用时仍为普通标识符（`TK_NAME`），仅当后紧接引号才识别为原始字符串前缀。

```xray
r"C:\path\to\file"          // 字面量包含两个反斜杠
r'C:\Users\${USER}'         // 反斜杠不转义，但 ${USER} 仍插值
```

##### 反引号字符串（非法）

源码 lexer 显式拒绝反引号字符串。如需模板，使用普通双 / 单引号 + `${...}`。

#### 1.6.6 正则字面量

```ebnf
RegexLiteral ::= '/' RegexBody '/' RegexFlag*
RegexFlag ::= 'g' | 'i' | 'm' | 's'
```

```xray
/[a-z]+/i
/\d+\.\d+/g
```

- flags：`g`（全局）、`i`（忽略大小写）、`m`（多行）、`s`（dot 匹配换行）。
- 实现：见 `stdlib/regex`。
- **歧义消解**：当 `/` 出现在能接受一元 `/` 的位置（如紧跟 `=`、`,`、`(`、操作符），扫描器识别为正则；其他位置识别为除法。

### 1.7 操作符与 Token

完整 token 表（按类别）：

#### 1.7.1 标点

| Token | 用途 |
|--|--|
| `(` `)` | 分组、调用、参数列表 |
| `{` `}` | 块、对象字面量 |
| `[` `]` | 数组字面量、索引 |
| `,` | 分隔符 |
| `.` | 成员访问 |
| `:` | 类型注解、map kv、ternary |
| `;` | for 循环分隔（其他位置可选） |
| `?` | 可空类型、ternary |
| `@` | 注解标记 (`@test`) |

#### 1.7.2 算术

`+` `-` `*` `/` `%`

#### 1.7.3 位运算

`&` `|` `^` `~` `<<` `>>`

#### 1.7.4 比较

`==` `!=` `===` `!==` `<` `<=` `>` `>=`

- `==` `!=`：值相等（隐式数值转换：int→float）
- `===` `!==`：严格相等（类型+值；无转换）
- `<` 等：数字、字符串支持；其他类型不支持

#### 1.7.5 逻辑

`&&` `||` `!`

短路求值。

#### 1.7.6 赋值

`=` `+=` `-=` `*=` `/=` `%=` `&=` `|=` `^=` `<<=` `>>=`

#### 1.7.7 自增自减

`++` `--`

仅支持**后缀**形式 `x++` / `x--`；前缀 `++x` / `--x` 编译报错。详见 §3.2。

#### 1.7.8 类型相关

| Token | 用途 |
|--|--|
| `?` | 可空类型 (`T?`)、ternary、可选链前缀 |
| `?.` | 可选链 (`obj?.prop`) |
| `??` | 空值合并 (`a ?? b`) |
| `!` | 强制解包（后缀，`expr!`）/ 逻辑非（前缀） |
| `\|` | union 类型 (`int \| string`) / 位或 |
| `->` | 统一箭头：函数返回类型、函数类型、闭包、`match` / `select` 分支 |
| `...` | rest / spread |
| `..` | 范围 (`0..10`) |
| `is` | 运行时类型检查 |
| `as` | 类型转换 |

`!` 的歧义在 parser 阶段消解：紧跟表达式末尾且无空白时识别为强制解包；前缀位置识别为逻辑非。

#### 1.7.9 集合字面量起始符

| Token | 用途 |
|--|--|
| `#{` | 空 Map 字面量 |
| `#[` | Set 字面量起始 |

例：

```xray
let empty_map = #{}
let primes = #[2, 3, 5, 7]
```

#### 1.7.10 模式

| Token | 用途 |
|--|--|
| `_` | `match` 通配符 |

#### 1.7.11 操作符优先级

完整优先级表见 [§3.1](#31-优先级与结合性)。

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
| 错误处理 prelude | `Exception`、`Result<T, E>`（见 §8） |
| 弱引用容器 | `WeakMap`、`WeakSet` |
| Nullable | `T?` |
| Union | `A \| B \| ...` |
| Tuple | `(T1, T2, ...)` |
| Function | `fn(T1, T2) -> R` |
| Class / Struct / Interface | 用户定义（nominal） |
| Enum | 用户定义（含 ADT enum，见 §5.6） |
| Type alias | `type Name = SomeType` |

### 2.3 基本类型

#### 2.3.1 整数类型

| 类型 | 范围 | 别名 |
|--|--|--|
| `int8` | `[-128, 127]` | — |
| `int16` | `[-32768, 32767]` | — |
| `int32` | `[-2³¹, 2³¹-1]` | — |
| `int64` | `[-2⁶³, 2⁶³-1]` | `int`（默认整数类型）|
| `uint8`..`uint64` | 无符号对应 | — |

- 字面量默认 `int`；可被上下文窄化（如赋给 `int32` 变量）。
- 算术：二补码环绕语义（wrap on overflow），不区分 debug / release 构建。

#### 2.3.2 浮点类型

| 类型 | 标准 |
|--|--|
| `float32` | IEEE-754 单精度 |
| `float64` | IEEE-754 双精度；`float` 的别名 |

字面量默认 `float`。

#### 2.3.3 `bool`

`true` / `false`，独立类型，与数值类型**不可隐式互转**（不能 `let x: int = true`，也不能 `let b: bool = 1`）。

**truthy / falsy 上下文**（仅作用于 `if` / `while` / `?:` / `??` / `&&` / `||` 等控制流位置，**不**改变变量类型）：

| 值 | 视作 |
|---|---|
| `false`、`null`、`0`、`0.0`、`""`、`Bytes(0)`、空数组 / 空 Map | **falsy** |
| 其他一切（包括 `0.0001`、非空字符串/集合、对象引用） | **truthy** |

```xray
let x: int? = maybe_int()
if (x) {                  // truthy 上下文：x 既不是 null 也不是 0 时进入
    print(x + 1)          // 此分支中 x 被窄化为 int
}

let s: string = ""
if (s) { ... } else { ... }    // falsy：进入 else

let m: Map<string, int> = #{}
if (m) { ... }                  // falsy：空 Map

let a: int? = null
let b = a ?? 0                  // null 合并：b = 0
```

**注意**：`x is T` 和 `x != null` 等显式比较是首选，truthy/falsy 主要用于简洁的"存在性"判断（如 `if (user)`）。

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

### 2.4 复合类型

#### 2.4.1 `Array<T>`

有序可变数组。详见 §14.1。

```xray
let a: Array<int> = [1, 2, 3]
let b = [1, 2, 3]                // 推断为 Array<int>
let c: Array<string> = []         // 显式空数组
```

`Array<T>` 的 `T` 必须能在编译期确定。空 `[]` 在无类型标注时是编译错误：`Empty array '[]' requires a type annotation`。

#### 2.4.2 `Map<K, V>`

哈希字典，**保持插入顺序**。详见 §14.7。

**Map 字面量**必须用 `#{ ... }` 前缀，分隔符用 `:`（与 Json 一致，靠 `#` 前缀消歧）：

```xray
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

`K` 必须实现 `Hashable`（详见 §14.14）：通常是 `int`、`string`、`bool`、`enum`、或自定义实现 `Hashable` 的类。

#### 2.4.3 `Set<T>`

去重集合。详见 §14.4。

```xray
let s: Set<int> = #[1, 2, 3]
```

#### 2.4.4 `Channel<T>`

协程间通信通道。**必须**用 `const` 声明（见 §10.5）。

```xray
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

```xray
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

```xray
let x: int? = null      // OK
let y: int? = 42        // OK
let z: int = null       // 编译错误：null 不是 int
```

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

```xray
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

```xray
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

```xray
type Result = int | string
type Mapper = (int) -> int
type Point = { x: float, y: float }
```

别名是**纯语法**等价，不产生新类型。

### 2.9 类型推断

详见 §7.4。简述：

```xray
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

```xray
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

---

## 3. 表达式 (Expressions)

> 真值源：`src/frontend/parser/xparse_expr.c`、`src/frontend/parser/xast_types.h` 的 `AST_BINARY_*` / `AST_UNARY_*` / `AST_TERNARY` / `AST_*` 等节点。

### 3.1 优先级与结合性

完整优先级表（自**高至低**；同级运算符按结合性分组）：

| 级 | 运算符 | 结合性 | 说明 |
|--|--|--|--|
| 17 | `(...)` `[...]` `.x` `?.x` `f()` `e!` | 左 | 后缀：分组、索引、成员、可选链、调用、强制解包 |
| 16 | 前缀 `-` `+` `!` `~` `new` `move` `await` `go` | 右 | 一元前缀 + 协程操作（`++` / `--` 仅后缀） |
| 15 | `as` `is` | 左 | 类型转换 / 检查（`as T?` 安全形式靠目标类型可空，非独立 `as?` 运算符） |
| 14 | `*` `/` `%` | 左 | 乘除取模 |
| 13 | `+` `-` | 左 | 加减 |
| 12 | `<<` `>>` | 左 | 移位 |
| 11 | `<` `<=` `>` `>=` | 左 | 关系比较 |
| 10 | `==` `!=` `===` `!==` | 左 | 相等比较 |
| 9 | `&` | 左 | 位与 |
| 8 | `^` | 左 | 位异或 |
| 7 | `\|` | 左 | 位或（亦用于 union 类型） |
| 6 | `&&` | 左 | 逻辑与（短路） |
| 5 | `\|\|` | 左 | 逻辑或（短路） |
| 4 | `??` | 左 | 空值合并 |
| 3 | `..` | 左 | 范围 |
| 2 | `? :` | 右 | 三元 |
| 1 | `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` | 右 | 赋值与复合赋值 |
| 0 | `,`（仅 `match` 多值、参数列表等特定位置）| — | 不是真正运算符 |

实现：`src/frontend/parser/xparse_expr.c` 的 Pratt-parser 风格。

### 3.2 一元表达式

```ebnf
UnaryExpr ::= ('-' | '+' | '!' | '~') UnaryExpr
            | 'new' Identifier TypeArgs? '(' ArgList? ')'
            | 'move' UnaryExpr
            | 'await' ('all' | 'any')? UnaryExpr
            | 'go' (Block | PostfixExpr)
            | 'try?' UnaryExpr
            | 'try!' UnaryExpr
            | PostfixExpr
```

| 运算符 | 适用类型 | 结果类型 | 备注 |
|--|--|--|--|
| `-x` | 数值 | 同 | 负号；浮点 NaN 保留 |
| `+x` | 数值 | 同 | 标识，几乎无用 |
| `!x` | `bool` | `bool` | 逻辑非；**不接受非 bool**（不像 JS） |
| `~x` | 整数 | 同 | 按位取反 |
| `x++` `x--` | 整数 | 同 | 后缀自增/减；返回**修改后**的值（与 C/Java 不同，本质上是 `x = x + 1` 的赋值结果） |

**自增/减语义**：
- xray **只提供后缀** `x++` / `x--`，不支持前缀 `++x` / `--x`（编译错误："prefix ++/-- not supported, use postfix form"）。
- 等价于赋值 `x = x + 1` / `x = x - 1`；表达式值即赋值后的新值。
- 不能内联在二元表达式中（如 `a + x++`、`f(x++)`、`x++ + 1`）；解析器会报"++/-- must be standalone statement"。允许 `let y = x++`（赋值左侧紧邻），此时 `y` 取到的是修改后的值。
- 仅作用于左值（变量、字段、索引）。
- 浮点数**不支持** `++`/`--`（编译错误）。

### 3.3 二元表达式

```ebnf
BinaryExpr ::= UnaryExpr (BinOp UnaryExpr)*
BinOp ::= '+' | '-' | '*' | '/' | '%'
       | '&' | '|' | '^' | '<<' | '>>'
       | '<' | '<=' | '>' | '>='
       | '==' | '!=' | '===' | '!=='
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
| `%` | int | float（IEEE remainder） | float | ❌ | ❌ |

**特殊语义**：
- `int / 0` → 运行时抛 `XR_ERR_DIV_BY_ZERO` (E0420)。
- `int % 0` → 运行时抛 `XR_ERR_MOD_BY_ZERO` (E0421)。
- `float / 0.0` → `+inf` / `-inf` / `NaN`（IEEE-754 语义，**不抛**异常）。
- 整数溢出：见 §2.3.1。
- 字符串 `+ string` 是 O(n) 拼接；密集拼接请用 `StringBuilder`。

#### 3.3.2 位运算

`&` `|` `^` `~` `<<` `>>`

- 仅作用于整数类型。
- 移位计数取模 64（与 C 不同：xray 总是定义的）。
- `>>` 是**算术右移**（保留符号位）。无符号类型用对应的 `uintN`。
- bool 不参与位运算（用 `&&` `||`）。

#### 3.3.3 比较运算符

| 运算符 | 语义 |
|--|--|
| `==` | 值相等。`int` 与 `float` 可比较（int→float 隐式）。字符串按内容比较。class/struct 使用 `==` 重载或默认 identity。 |
| `!=` | `==` 的反 |
| `===` | **严格**相等：类型与值都必须相等；无任何隐式转换。 |
| `!==` | `===` 的反 |
| `<` `<=` `>` `>=` | 数值与字符串支持；其他类型默认不支持（可通过 `operator<` 重载启用）。 |

**与 JS 的区别**：xray 的 `==` **不**做 string↔number 转换；只做数值之间的 int↔float 提升。

#### 3.3.4 逻辑运算符

`&&` `||`：

- 两操作数**必须**是 `bool` 类型（编译期检查）。
- 短路求值：`false && X` 不求值 `X`；`true || X` 不求值 `X`。
- 结果类型 `bool`（不像 JS 返回操作数）。

#### 3.3.5 空值合并 `??`

```xray
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

### 3.6 空合并 `??` 与可选链 `?.`

详见 §3.3.5（`??`）与下方（`?.`）。

#### 可选链 `?.`

```ebnf
OptionalChain ::= Primary ('?.' (Identifier | '[' Expr ']'))+
```

```xray
let len = name?.length          // null 时返回 null
let item = arr?.[0]             // 可选索引
```

**语义**：
- 若 `?.` 左侧为 `null`，整个表达式短路返回 `null`。
- **传播**：`a?.b.c.d` 中，若 `a` 为 null，整个链返回 null；中间 `.` 不重新检查。
- 结果类型：原类型加 `?`（若已经 `?` 则保持）。
- 当前 `?.` 支持属性访问与索引访问；函数可选调用 `func?.()` 不属于当前语法。

### 3.7 强制解包 `!` / `try?` / `try!` / `catch!`

> 完整错误处理语义见 §8。本节只列表达式语法与简要语义。

#### 强制解包 `expr!`

```xray
let v: int = nullable_int!      // null 时运行时抛 NullThrowError (E0410)
```

仅当编译期可确定 `expr` 是可空类型 (`T?`) 时合法；对非空类型 `T` 使用 `!` 是编译错误。

#### `try?` 表达式 —— 失败转 null

```ebnf
TryOptional ::= 'try?' Expression
```

`try? e` 把任何失败折叠为 `null`，丢弃错因：

| `e` 的静态类型 | `try? e` 的类型 | 行为 |
|--|--|--|
| `Result<T, E>` | `T?` | `Err` → `null`；`Ok(v)` → `v` |
| `T`（普通类型，调用可能抛异常） | `T?` | 抛异常 → `null`；正常 → `v` |
| `T?` | `T?` | 透传 |

```xray
let n: int? = try? parseInt(input)         // Result.Err → null
let v: Json? = try? http.get(url).json()   // 抛异常 → null
```

#### `try!` 表达式 —— 早退或跨轨升级

```ebnf
TryForce ::= 'try!' Expression
```

`try!` 后面的表达式静态类型**必须**是 `Result<T, E>` 或 `T?`（其它类型编译报错 `E0821`）。行为按表达式类型 + 当前函数返回类型双重 dispatch：

| `e` 类型 | 当前函数返回 | 失败行为 | 成功值 |
|--|--|--|--|
| `Result<T, E>` | `Result<_, E>` | 早退 `return Result.Err(e)` | `v` |
| `Result<T, E>` | 其它 | `throw e`（要求 `E` 是 `Exception` 派生） | `v` |
| `T?` | `_?` | 早退 `return null` | `v` |
| `T?` | 其它 | `throw new NullThrowError(...)` | `v` |

```xray
fn pipeline(s: string) -> Result<int, ParseError> {
    let n = try! parseInt(s)      // Err 早退 Err
    return Result.Ok(n + 1)
}
```

完整说明见 §8.3.1。**`try!` 不是异常调用的强制仪式**——xray 不要求会抛异常的调用前必须写 `try!`。

#### `catch!` 块表达式 —— 异常块凝结成 Result

```ebnf
CatchBlock ::= 'catch!' Block
```

`catch! { ... }` 把代码块中可能抛出的任何异常包装为 `Result<T, Exception>`：

```xray
let r: Result<int, Exception> = catch! {
    let x = riskyOp()
    let y = another(x)
    return y + 1                   // 块内的 return 是"从 catch! 块返回"
}
```

- 块内 `return v` 是从 `catch!` 块返回（不影响外层函数）
- 块的最后一个表达式作为 `Ok(v)` 的值
- 块内**逃出的任何异常**包装为 `Err(e)`，`e` 静态类型恒为 `Exception`
- 不支持类型过滤——需要时用手写 `try/catch`

完整说明见 §8.3.3。

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

### 3.9 范围 `..` 与展开 `...`

#### 范围 `a..b`

```ebnf
RangeExpr ::= AddExpr ('..' AddExpr)?
```

```xray
0..10                  // 0..10，左闭右开（包含 0，不包含 10）
let r = 1..100
for (i in 0..n) { ... }
```

- 类型 `Range`（仅 int 范围）。
- 半开区间 `[a, b)`：`a` 包含、`b` 不包含。`for-in`、`Range.includes`、`Range.length`、`Range.toArray()`、`match` 中的 `a..b` 模式全部遵循同一语义。
- 主要用途：`for-in` 循环、模式匹配中的范围判定。
- 当前不提供闭区间语法（`a..=b`）；如需"包含 b"，写 `a..(b+1)`。

#### 展开 `...`

仅在以下位置使用：
- **函数 rest 参数声明**：`fn f(...args: int)`
- **函数调用展开**：`f(...args)`，展开源必须是静态 arity 已知的 tuple。
- **tuple 字面量展开**：`(head, ...tail)`，展开源必须是静态 arity 已知的 tuple。

### 3.10 字面量构造

#### Array `[...]`

```ebnf
ArrayLit ::= '[' (Expr (',' Expr)* ','?)? ']'
```

```xray
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

```xray
let m = #{"a": 1, "b": 2}
let empty = #{}                           // 空 Map
```

**关键区别**：`{}` 始终是**Json / Object**；`#{}` 始终是 **Map**。两者都用 `:` 作键值分隔，靠 `#` 前缀区分。

#### Set `#[...]`

```ebnf
SetLit ::= '#[' (Expr (',' Expr)* ','?)? ']'
```

```xray
let s = #[1, 2, 3]
let empty = #[]
```

#### Object（结构化对象）`{ field: value, ... }`

```ebnf
ObjectLit  ::= '{' ObjectField (',' ObjectField)* ','? '}'
ObjectField ::= Identifier ':' Expr
              | Identifier            // shorthand: `{ x }` 等价 `{ x: x }`
```

```xray
let p = { name: "Alice", age: 30 }
let users = "Bob"
let obj = { users }              // shorthand
```

- 默认推断为**可扩展**的结构化对象类型（见 §2.4.6 / §2.10 Json 行为）。
- 用 `type` 别名固化结构：`let u: User = {...}`（编译期检查字段集，密封）。

#### Bytes `new Bytes(...)`

详见 §2.4.5 与 §14.5。

#### Channel `new Channel<T>(buf?)`

```xray
const ch: Channel<int> = new Channel<int>(10)
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

```xray
arr[0]
arr[0] = 10
map["key"]
str[i]                  // 返回单字符字符串
```

- `Array` 索引：`int`，越界抛 `E0430`。
- `Map` 索引：键类型；找不到键 → `E0431`。
- `string` 索引：返回长度为 1 的字符串（**不是** char/int）。
- 自定义类：通过 `operator[]` 重载。

#### 切片

```ebnf
Slice ::= Primary '[' Expr? ':' Expr? ']'
```

```xray
arr[1:4]                // 元素 [1,4)
arr[:3]                 // 前 3 个
arr[2:]                 // 从索引 2 到末尾
arr[:]                  // 全切片（浅拷贝）
str[0:5]                // 字符串切片
```

- 半开区间 `[start, end)`。
- `string` 切片支持负索引，负数从末尾计数；`Array` 切片中负 `start` 夹到 `0`，负 `end` 视为数组长度。
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

```xray
// ── 裸 lambda：最简洁，仅限调用参数位置 ──
arr.map(x -> x * 2)
arr.filter(x -> x % 2 == 0)

// ── 箭头 lambda：任意位置，支持多参数和类型注解 ──
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
| 箭头 lambda | `(x, y) -> expr` | 多参数、需类型注解、或非调用参数位置 |
| fn 表达式 | `fn(x: T) -> R { ... }` | 多语句体、返回类型注解、泛型参数 |

**关键规则**：
- **裸 lambda**（`x -> expr`）：仅限**调用参数位置**，单参数无括号。参数类型由被调函数签名或容器元素类型推断。
- **箭头 lambda**（`(x) -> expr`、`(x, y) -> expr`）：任意位置可用。参数类型可省略，由上下文推断；推断失败时报 E0365。
- **fn 表达式**（`fn(x: T) { ... }`）：任意位置可用。支持泛型参数 `fn<T>(...)`、返回类型注解 `-> T`、多语句体。
- 单表达式形式 `-> expr` 自动 `return`。
- 块形式 `-> { ... }` 或 `{ ... }` 用显式 `return`。
- 捕获规则：见 §7.4 闭包捕获。**`go` 协程闭包对 `let` 变量的捕获是编译错误**——必须显式 `shared const`、`move`、或参数传递。

### 3.13 `match` 表达式

```ebnf
MatchExpr ::= 'match' Expr '{' MatchArm (',' MatchArm)* ','? '}'
MatchArm ::= Pattern ('if' Expr)? '->' Expression
```

```xray
let result = match x {
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
- **穷举性**：对 enum 变量（ADT 与简单枚举）编译器强制穷举。对其他表达式不强制，运行时无匹配抛 `Exception(E0442)`。
- 模式详见 [§6](#6-模式-patterns)。

### 3.14 `new`

```ebnf
NewExpr ::= 'new' Identifier TypeArgs? '(' ArgList? ')'
```

```xray
let p = new Point(1.0, 2.0)
let arr = new Array<int>()
let ch = new Channel<int>(10)
let m = new Map<string, int>()
```

**用于**：
- 类与 struct 实例化。
- 容器内置类型构造（`Array`/`Map`/`Set`/`Channel`/`Bytes`/`StringBuilder` 等）。

**与字面量的关系**：
```xray
let a = [1, 2, 3]              // 等价 new Array<int>() + push
let m = #{}                    // 等价 new Map<...>()
let p = Point{x: 1, y: 2}      // struct literal
```

### 3.15 字符串插值

详见 §1.6.5。简要：

```xray
"Hello, ${name}! Age: ${user.age + 1}"
```

- `${...}` 内任意表达式（含函数调用、对象访问、算术）。
- 嵌套字符串字面量需转义引号或换用单引号外层。
- 表达式类型必须可转为字符串（实现 `toString()` 或为基本类型）。

### 3.16 `yield` 语句

```xray
yield                       // 让出执行权
```

**当前实现**：仅支持无值语句形式，让协程让出 CPU（类似 Go 的 `runtime.Gosched()`）。

详见 §10.10。

---

## 4. 语句 (Statements)

> 真值源：`src/frontend/parser/xparse_stmt.c`、`src/frontend/parser/xast_nodes_stmt.h`。

Xray 语句以 `\n` 或 `;` 分隔；语句末尾的 `;` 在大多数位置可省略，仅 `for` 循环的初始化/条件/步进三段必须用 `;` 分隔。

### 4.1 表达式语句与块

```ebnf
ExprStmt ::= Expression (';' | LineBreak)
Block    ::= '{' Statement* '}'
```

```xray
foo()                  // 表达式语句
x = 1                  // 赋值表达式作为语句
{                      // 块
    let y = 2
    y + 1              // 表达式但结果被丢弃
}
```

**注**：块**不是表达式**——它没有值。如果需要从块求值，用 `match` 或包装成立即调用函数。

### 4.2 `if` / `else`

```ebnf
IfStmt ::= 'if' '(' Expression ')' Block ElseIfChain? ElseClause?
ElseIfChain ::= ('else' 'if' '(' Expression ')' Block)+
ElseClause  ::= 'else' Block
```

```xray
if (x > 0) {
    print("positive")
} else if (x == 0) {
    print("zero")
} else {
    print("negative")
}
```

**约束**：
- 条件**必须**用括号包裹（与 Go/Rust 不同）。
- 条件按 truthy/falsy 上下文求值（见 §2.3.3）；推荐使用显式 `bool` 表达式或 `x != null` / `x is T` 等比较以提高可读性。
- 分支体必须是块 `{...}`，**不允许**单语句省略括号。
- `if` 不是表达式；要表达式形式用三元 `? :` 或 `match`。

### 4.3 `while`

```ebnf
WhileStmt ::= 'while' '(' Expression ')' Block
```

```xray
let i = 0
while (i < 10) {
    print(i)
    i++
}
```

无 `do-while` 形式。

### 4.4 `for`（C 风格）与 `for-in`

#### C 风格 `for`

```ebnf
ForStmt ::= 'for' '(' ForInit? ';' Expression? ';' ForStep? ')' Block
ForInit ::= VarDecl | ExprStmt
ForStep ::= Expression (',' Expression)*
```

```xray
for (let i = 0; i < 10; i++) {
    print(i)
}
for (let j = 100; j > 90; j--) {
    print(j)
}
```

- `ForInit` 中声明的变量作用域限于循环体。
- 三个部分都可省略：`for (;;)` 是无限循环。

#### `for-in` 单变量

```ebnf
ForInStmt ::= 'for' '(' Identifier 'in' Expression ')' Block
```

```xray
for (item in [1, 2, 3]) { print(item) }
for (i in 0..n) { print(i) }                  // 范围迭代（半开区间）
for (ch in "hello") { print(ch) }             // 字符串字符（按 codepoint）
for (key in someMap) { print(key) }           // Map 单变量 → key
for (key in someJson) { print(key) }          // Json 单变量 → key
for (day in Color) { print(day.name) }        // 枚举迭代（按声明顺序）
for (_ in 0..n) { count++ }                   // 占位符忽略
```

#### `for-in` 双变量解构

xray 支持两种等价的双变量形式：

```ebnf
ForInPairStmt ::= 'for' '(' Identifier ',' Identifier 'in' Expression ')' Block
              |  'for' '(' '(' Identifier ',' Identifier ')' 'in' Expression ')' Block
```

```xray
// 形式 A：直接两标识符（更常见）
for (k, v in someMap) { print("${k}=${v}") }     // Map → (key, value)
for (i, e in someArray) { print("${i}: ${e}") }  // Array → (index, element)
for (i, c in "hello") { print("${i}:${c}") }     // string → (index, char)

// 形式 B：元组括号包裹（与 .entries() 配合）
for ((i, e) in someArray.entries()) { print("${i}=${e}") }
for ((i, c) in "hi".entries()) { print("${i}-${c}") }
```

迭代来源与产出对应关系：

| 集合类型 | 单变量产出 | 双变量产出 |
|---|---|---|
| `Array<T>` / `T[]` | element | (index, element) |
| `Map<K, V>` | key | (key, value) |
| `Json` | key (string) | (key, value) |
| `string` | char (1-codepoint string) | (index, char) |
| `Range`（`a..b`） | int | — |
| Enum 类型 | EnumValue | — |
| 自定义 `Iterator<T>` | T | — |

#### 自定义迭代器

实现 `iterator()` 方法返回 `Iterator<T>` 协议对象（含 `hasNext()` 和 `next()`）即可在 `for-in` 中使用。详见 §14.15。

### 4.5 `match` 语句

```ebnf
MatchStmt ::= 'match' '(' Expression ')' '{' MatchArm (','? MatchArm)* ','? '}'
MatchArm  ::= Pattern ('if' '(' Expression ')')? '->' (Expression | Block)
```

**关键语法**：
- 被匹配的表达式**必须**用括号包裹：`match (x) {...}`。
- 分支之间的逗号**可选**——同一个 match 中可以混用（不写更常见）。
- 守卫条件 `if` 后的表达式必须用括号：`n if (n > 0)`。

```xray
match (x) { 1 -> print("one"), _ -> print("other") }

match (action) {
    "start" -> {
        log.info("starting")
        start_engine()
    }
    "stop" -> stop_engine()
    _ -> log.warn("unknown")
}
```

`match` 既可作语句也可作表达式（详见 §3.13）；当作表达式时分支体必须是单一表达式或块的最后一个表达式。

模式细节见 [§6](#6-模式-patterns)。

### 4.6 `break` / `continue`

```xray
break                  // 跳出最内层循环
continue               // 进入下一次循环
```

**约束**：
- 必须在 `while` / `for` 内部；否则编译错误 `E0304` / `E0305`。
- `match` 内部的 `break` / `continue` **不**作用于 `match`，而是跳出包裹 `match` 的循环。
- **无标签** break/continue（不像 Java/Rust）。

### 4.7 `return`

```ebnf
ReturnStmt ::= 'return' ReturnValue? (';' | LineBreak)
ReturnValue ::= Expression | '(' Expression (',' Expression)+ ')'
```

```xray
return                 // 隐式返回 ()（Unit）
return 42
return (a, b)          // 多返回值，必须用括号包裹元组
```

> **注意**：多返回值必须用元组形式 `return (a, b)`；裸逗号 `return a, b` 是编译错误（`E0801`）。

**约束**：
- 只能在函数体内（含闭包）；顶层 return 是编译错误 `E0306`。
- 返回值类型必须与函数声明的返回类型兼容。

### 4.8 `throw` / `try` / `catch` / `finally`

```ebnf
ThrowStmt ::= 'throw' Expression

TryStmt       ::= 'try' Block CatchClause* FinallyClause?
CatchClause   ::= 'catch' '(' Identifier (':' Type)? ')' Block
FinallyClause ::= 'finally' Block
```

```xray
try { throw new Exception("inline") } catch (e) { print(e.message) }

try {
    risky()
} catch (e) {
    log.error("failed:", e.message)
} finally {
    cleanup()
}

throw new Exception("error message")        // ✅ Exception 派生
throw new HttpError(500, "internal")        // ✅ 自定义 Exception 子类
// throw "msg"                              // ❌ E0370: 必须是 Exception 派生
```

**语义**：
- `try` 必须至少跟 `catch` 或 `finally` 之一。
- `catch (e)` 中 `e` 类型默认 `Exception`；可带类型注解 `catch (e: HttpError)` 实现类型过滤；多个 `catch` 子句按声明顺序匹配。
- `finally` **保证执行**，无论是否抛异常或 return。
- `throw` 操作数静态类型必须是 `Exception` 派生（`E0370`）。
- 完整异常语义见 [§8](#8-错误处理-error-handling)。

### 4.9 `defer`

```ebnf
DeferStmt ::= 'defer' (Expression | Block)
```

```xray
fn read_file(path: string) -> string {
    let f = open(path)
    defer f.close()                  // 函数返回前必执行
    return f.readAll()
}

fn process() {
    defer {                          // 块形式
        log.info("done")
        cleanup()
    }
    do_work()
}
```

**语义**：
- 在**函数作用域结束**时执行（不是块作用域，与 Swift 不同）。
- **LIFO**：多个 `defer` 按声明的逆序执行。
- **必执行**：即使函数因异常退出也执行（类似 `finally`）。
- 与 `finally` 的区别：`defer` 绑定函数作用域，`finally` 绑定 `try` 块。
- `defer` 中的异常会**取代**当前正在传播的异常（参考 Go 语义）。

### 4.10 内置打印函数

`print` / `dump` 是**内置全局函数**（非关键字，详见 §13.1），列于此处便于查阅：

```xray
print("hello")                 // 自动追加换行
print("a:", a, "b:", b)        // 多参用空格分隔
dump(some_obj)                 // 调试输出，含类型信息与结构布局
```

**行为说明**：
- 接受任意类型与任意数量参数（变长）；每个参数自动调用其 `toString()` 或内置格式化。
- 输出到 stdout；不参与异常机制。
- 多参时以单空格分隔。
- `print` 默认会追加换行（与 C/Python 不同，与回归测试一致）。
- `dump` 用于调试，输出格式包含类型标注与对象内部结构。

---

## 5. 声明 (Declarations)

> 真值源：`src/frontend/parser/xparse_decl.c`、`src/frontend/parser/xast_nodes_decl.h`、`src/frontend/analyzer/xanalyzer_visitor.c`。

### 5.1 `let` / `const` / `shared`

```ebnf
VarDecl ::= ('let' | 'const' | 'shared' ('const' | 'let')) Binding (',' Binding)*
Binding ::= Pattern (':' Type)? ('=' Expression)?
Pattern ::= Identifier
         | '[' BindingPattern (',' BindingPattern)* ','? ']'    // array destructure
         | '(' BindingPattern (',' BindingPattern)+ ','? ')'    // tuple destructure
         | '{' Identifier (',' Identifier)* ','? '}'            // object destructure
```

#### 5.1.1 `let` — 可变绑定

```xray
let x = 1                         // 类型推断为 int
let name: string = "Alice"        // 显式类型
let count: int                    // 仅声明无初值：使用零值
```

- 可重新赋值。
- 必须有初值**或**类型标注；否则编译错误 `E0303`。
- 无初值的情况下，初始值为类型的零值（`int` → `0`、`string` → `""`、`bool` → `false`、`T?` → `null`）。

#### 5.1.2 `const` — 不可变绑定

```xray
const PI = 3.14159
const MAX_LEN: int = 1024
```

- **必须**有初值。
- 不能重新赋值（编译错误 `E0303`）。
- 类型可推断或显式标注。

#### 5.1.3 `shared const` — 跨协程不可变共享

```xray
shared const CONFIG = { host: "localhost", port: 8080 }
shared const PRIMES = [2, 3, 5, 7, 11]
```

- 存储在**全局堆**，refcount 管理。
- 跨协程**零拷贝**只读访问。
- 是 `go` 闭包**唯一**能合法捕获的可变作用域之外的变量种类（其他必须走参数传递或 `move`）。

#### 5.1.4 `shared let` — 跨协程可变独占

```xray
shared let buffer = new Bytes(1024)
```

- **Move 语义**：必须用 `move` 显式转移所有权。
- 不能被 `go` 闭包捕获（必须 `move`）。
- `move` 之后访问 → 编译错误。

详见 [§10.11](#1011-并发安全模型)。

#### 5.1.5 解构绑定

```xray
// 数组解构
let [a, b, c] = [1, 2, 3]
let [first, , third] = [10, 20, 30]         // 跳过元素

// 元组解构（多返回值）
let (q, r) = divmod(17, 5)

// 对象解构（仅按名提取，**不**支持重命名）
let { name, age } = { name: "Alice", age: 30 }
```

约束：
- 解构变量数必须匹配（除 rest 模式外）。
- 对象解构只接受 `Identifier` 列表，**不支持** `{ name: localName }` 风格的重命名。

### 5.2 `fn` 函数声明

```ebnf
FnDecl ::= AttrList? Modifier* 'fn' Identifier TypeParams? '(' ParamList? ')' ReturnType? Block
ParamList ::= Param (',' Param)*
Param     ::= Modifier* Identifier ':' Type ('=' DefaultValue)?
            | '...' Identifier ':' Type
Modifier  ::= 'in' | 'ref'
ReturnType ::= '->' Type
            |  '->' '(' Type (',' Type)+ ')'   // 元组返回
TypeParams ::= '<' Identifier (',' Identifier)* '>'
AttrList ::= ('@' Identifier ('(' AttrArgList? ')')?)*
```

#### 5.2.1 基本形式

```xray
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

```xray
fn connect(host: string, port: int = 8080, tls: bool = false) {
    print(host, port, tls)
}

connect("localhost")              // port=8080, tls=false
connect("localhost", 443)         // tls=false
connect("localhost", 443, true)
```

- 默认值在被调函数入口求值；调用方省略参数时传入 `null`，函数入口用默认表达式替换该 `null`。
- 有默认值的参数必须在尾部连续出现。

#### 5.2.3 多返回值

```xray
fn divmod(a: int, b: int) -> (int, int) {
    return (a / b, a % b)
}

let (q, r) = divmod(17, 5)
let result = divmod(10, 3)        // result 类型 (int, int)
```

**约束**：
- 返回类型用括号包裹元组：`(int, bool)`。
- 单返回值不写括号：`: int`。
- `return (a, b)` 必须带括号；裸逗号 `return a, b` 是编译错误（`E0801`）。

#### 5.2.4 参数修饰符

仅适用于 **`struct` 值类型参数**。

```xray
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

| 修饰符 | 语义 |
|--|--|
| 无 | 按值传递（struct 拷贝） |
| `in` | 按只读引用传递（不拷贝、不可写） |
| `ref` | 按可变引用传递（不拷贝、可写、修改可见） |

#### 5.2.5 rest 参数

```xray
fn sum(...nums: int) -> int {
    let total = 0
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
- `let f = (x: int) -> x`（赋值给变量的箭头函数）**不**提升。

#### 5.2.7 尾递归优化

编译器自动识别 accumulator 风格的尾递归并转为循环（避免栈溢出）。详见 [§17](#17-编译流水线-compilation-pipeline)。

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
- 多文件项目的入口由 `xray.toml` 的 `entry` 字段指定，对应文件按上述脚本规则执行。

### 5.3 `class` 声明

```ebnf
ClassDecl ::= Modifier* 'class' Identifier TypeParams?
              ('extends' Identifier TypeArgs?)?
              ('implements' Identifier TypeArgs? (',' Identifier TypeArgs?)*)?
              '{' ClassMember* '}'
ClassMember ::= FieldDecl | MethodDecl | ConstructorDecl | StaticBlock
FieldDecl ::= Modifier* Identifier ':' Type ('=' Expression)?
MethodDecl ::= Modifier* Identifier '(' ParamList? ')' ReturnType? Block
            |  Modifier* 'operator' OpToken '(' ParamList? ')' ReturnType? Block
ConstructorDecl ::= 'constructor' '(' ParamList? ')' Block          // 参数类型可省
Modifier ::= 'private' | 'public' | 'static' | 'final' | 'abstract' | 'override'
```

> **关于 `public` 和 `override`**：这两个修饰符**在词法层是合法关键字**，但实际编码风格中**几乎从不使用**：
>
> - `public` 是**默认可见性**——所有未带 `private` 的字段/方法都是公开的，因此显式写 `public` 是冗余的。
> - `override` 是**可选**——重写父类方法只要同名同参就自动覆盖，不要求显式 `override` 标注。
>
> 标准库和回归测试一致采用"省略默认修饰符"风格。

#### 5.3.1 基本类

```xray
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
        return new Animal(name)
    }
}

let a = new Animal("Rex")
print(a.speak())
print(Animal.create("Bob").name)
```

#### 5.3.2 继承

```xray
class Dog extends Animal {
    constructor(name: string) {
        super(name)                    // **必须**首语句（仅限派生类）
    }

    override speak() -> string {         // override 可选，但推荐写出
        return "woof"
    }
}
```

**约束**：
- 派生类构造器**第一行**必须是 `super(...)`（除非未声明构造器）；否则编译错误。
- 不能在 `super(...)` 之前访问 `this`。
- **重写父类方法不需要任何关键字**——只要子类出现同名同参的方法即自动重写（`override` 修饰符存在但**可选**）。
- 父类标 `final class` 则不可继承。
- 父类方法标 `final` 则不可重写。
- 父类方法标 `abstract` 则子类**必须**实现（除非子类也是 `abstract`）。
- `super.method()` 可在重写的方法体内调用被屏蔽的父类方法。

#### 5.3.3 修饰符

| 修饰符 | 适用 | 语义 |
|--|--|--|
| （无） | 字段/方法 | 默认 public——公开可见 |
| `public` | 字段/方法 | **冗余**——与默认相同；实际从不写出 |
| `private` | 字段/方法 | 仅类内部可访问；子类不能直接访问，但可通过父类公开方法间接访问 |
| `static` | 字段/方法 | 类级别，不属于实例；调用为 `ClassName.method()` |
| `final` | 类/方法/字段 | 类：禁止继承；方法：禁止重写；字段：初始化后不可修改 |
| `abstract` | 类/方法 | 不可实例化 / 必须由子类实现 |
| `override` | 方法 | **可选**——重写不要求显式标注；写了仅作文档作用 |

**修饰符可组合**：`private final secret: string = "key123"`、`static final pi() -> float`、`private static counter: int = 0`。

xray **没有** `protected` 修饰符——子类通过父类公开方法间接访问私有字段即可。

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
- struct 可以**没有**构造器（`new Point()` 创建隐式零值实例，后续手动赋值；详见 §5.4）。

#### 5.3.5 运算符重载

```xray
class Vec2 {
    x: float
    y: float

    constructor(x: float, y: float) {
        this.x = x; this.y = y
    }

    operator+(other: Vec2) -> Vec2 {
        return new Vec2(this.x + other.x, this.y + other.y)
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

**不能**重载：`&&` `\|\|` `=` `?.` `?:` `??` `,` `.`

#### 5.3.6 自定义迭代器

实现 `iterator()` 返回带 `hasNext() -> bool` 和 `next() -> T?` 的对象即可启用 `for-in`。详见 §14.15。

### 5.4 `struct` 声明

```ebnf
StructDecl ::= 'struct' Identifier TypeParams?
               ('implements' Identifier (',' Identifier)*)?
               '{' StructMember* '}'
```

```xray
struct Point {
    x: float
    y: float

    magnitude_sq() -> float {
        return this.x * this.x + this.y * this.y
    }
}

// 两种创建方式
let p = new Point()                  // 默认构造（字段为零值）后逐个赋值
p.x = 3.0
p.y = 4.0

let q = Point{x: 3.0, y: 4.0}        // struct 字面量：类型名 + { field: value }
let pt = Point{x: 1.0, y: 2.0}

// 值语义：赋值与传参都是拷贝
let b = q                            // b 是 q 的独立拷贝
b.x = 99.0
// q.x 仍为 3.0
```

**与 `class` 的差异**：

| 维度 | `class` | `struct` |
|--|--|--|
| 内存语义 | 引用类型（堆） | 值类型（栈或内联） |
| 赋值/传参 | 共享引用 | **拷贝**（`let b = a` 生产独立副本） |
| 继承 | 支持 `extends` | **不支持**继承 |
| `implements` | ✅ | ✅ |
| 泛型 | ✅ | ✅ |
| `static` / `private` / `final` | ✅ | ✅ |
| 运算符重载 | ✅ | ✅ |
| 构造器 | `constructor(...)` | **可省略**：`new Point()` 生成零值实例 |
| 字面量 | 无 | `TypeName{field: value, ...}` |

**适用场景**：
- 数学类型（Vec2/Vec3/Quat/Color）
- 短生命周期值（迭代器状态、临时元组替代）
- 性能敏感、希望避免堆分配的数据

### 5.5 `interface` 与 `implements`

xray 接口实现是**显式声明的**（与 Go 的隐式实现不同）：类 / struct 必须用 `implements` 列出实现的接口。

```ebnf
InterfaceDecl ::= 'interface' Identifier TypeParams?
                  ('extends' NamedType (',' NamedType)*)?
                  '{' InterfaceMember* '}'
InterfaceMember ::= Identifier '(' ParamList? ')' ReturnType?       // 方法签名
                 |  ('const')? Identifier ':' Type                   // 属性签名（可加 const 表示只读）
```

```xray
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

xray 的 `enum` 是**代数数据类型 (Algebraic Data Type)**：每个变体可以是无 payload 的简单标签（C 风格枚举），也可以**携带类型化的 payload 数据**（ADT 风格）。两者可在同一个 enum 中混用。

```ebnf
EnumDecl       ::= 'enum' Identifier TypeParams?
                   ('implements' NamedType (',' NamedType)*)?
                   '{' EnumVariant (',' EnumVariant)* ','? EnumMethod* '}'
EnumVariant    ::= Identifier VariantPayload?
                |  Identifier '=' BackingValue                // 简单枚举的显式 backing value
EnumMethod     ::= 'fn' Identifier TypeParams? '(' ParamList? ')' ReturnType? Block
VariantPayload ::= '(' VariantField (',' VariantField)* ')'
VariantField   ::= (Identifier ':')? Type
BackingValue   ::= IntLiteral | FloatLiteral | StringLiteral | BoolLiteral
```

> 变体声明必须排在前面（逗号分隔），方法声明排在所有变体之后（无逗号，靠块边界分隔，与 `class` 内方法一致）。详见 §5.6.7。

#### 5.6.1 简单枚举（无 payload）

```xray
enum Color { Red, Green, Blue }
Color.Red.value     // 0
Color.Blue.value    // 2

enum HttpStatus {
    OK = 200,
    NotFound = 404,
    InternalError = 500,
}

enum Direction { North = "N", South = "S", East = "E", West = "W" }
enum Flag      { On = true, Off = false }
enum Pi        { Approximate = 3.14, Better = 3.14159 }
```

简单枚举的所有成员必须使用相同 backing type（全 int / 全 float / 全 string / 全 bool）；混合类型编译错误 `XR_ERR_ANALYZE_ENUM_MIXED_TYPE`。

#### 5.6.2 ADT 枚举（带 payload）

变体名后跟括号声明 payload 字段（位置参数或具名字段）：

```xray
// 位置 payload
enum Result<T, E> {
    Ok(T),
    Err(E),
}

// 具名字段 payload（推荐：可读性更好）
enum NetEvent {
    Connected,
    Disconnected(reason: string),
    DataReceived(bytes: Bytes),
    Error(code: int, message: string),
}

// 状态机
enum ConnState {
    Idle,
    Connecting(retry: int),
    Connected(peer: string, since: int),
    Failed(reason: string),
}

// AST 节点
enum Expr {
    Number(int),
    Binary(op: string, left: Expr, right: Expr),
    Call(name: string, args: Array<Expr>),
}
```

**ADT 与简单枚举的区别**：

| 特性 | 简单枚举 | ADT 枚举 |
|------|--|--|
| 携带数据 | ❌ | ✅ 每变体独立的字段集 |
| `.value` / `.ordinal` | ✅ | 仅对无 payload 的变体可用 |
| backing value (`= 200`) | ✅ | ❌ 不能与 payload 混用 |
| 泛型 | ❌ | ✅ `enum Result<T, E> { ... }` |
| match 解构 | 仅按值 | 按变体 + 解构 payload |
| `for-in` 遍历 | ✅ 按声明顺序 | ❌ 含 payload 时无意义 |
| 内存表示 | 整数/字符串值 | tag + payload |

混合：一个 enum 可以同时含有"无 payload"和"带 payload"的变体（见上面的 `NetEvent` / `ConnState`）。

#### 5.6.3 构造与解构

构造：

```xray
let c = Color.Red                                   // 简单
let r1 = Result.Ok(42)                              // 位置 payload
let e1 = NetEvent.DataReceived(bytes: b)            // 具名 payload，可写字段名
let e2 = NetEvent.Error(404, "not found")           // 也可省略字段名按位置传
let e3 = NetEvent.Connected                         // 无 payload 变体不写括号
```

解构（match）：

```xray
match (event) {
    NetEvent.Connected            -> print("connected"),
    NetEvent.Disconnected(reason) -> print("by:", reason),
    NetEvent.DataReceived(b)      -> process(b),
    NetEvent.Error(code, msg)     -> log.error(code, msg),
}
```

详见 §6.3。

#### 5.6.4 简单枚举的 Member API

仅适用于**无 payload** 的变体（含 ADT 中的"纯标签"变体）。

实例属性（作用在枚举值上）：

```xray
Color.Red.name        // "Red"          变体名 (string)
Color.Red.value       // 0              backing value
Color.Red.ordinal     // 0              声明顺序索引 (int，从 0)
Color.Red.toString()  // "Color.Red"    "<EnumName>.<VariantName>" 格式
```

类静态属性/方法：

```xray
Color.memberCount     // 3              简单变体总数 (int)
Color.getMember(0)    // Color.Red      按 ordinal 取
```

含 payload 的 ADT 变体**不**支持 `.value` / `.ordinal` / `getMember`，但仍可调用 `.name` 与 `toString()`（后者会带 payload 摘要，如 `Result.Ok(42)`）。

#### 5.6.5 遍历

简单枚举可被 `for-in` 按声明顺序遍历：

```xray
for (c in Color) { print(c.name) }        // "Red" "Green" "Blue"
```

含 payload 的 ADT enum **不**支持直接 `for-in`——遍历"所有可能值"无意义（`Result<int, string>` 有无穷多个）。

#### 5.6.6 反查（从值到成员）

简单整数枚举编译器优化反查（Tier 1/2 contiguous/sparse；其他类型走线性扫描）。ADT 变体不支持反查。

#### 5.6.7 enum 实例方法

`enum` 体内可定义实例方法，语法与 `class` 内的方法完全一致（不引入 `impl` 关键字）。方法在所有变体上可调用；方法体内通过 `match (this)` 区分变体行为：

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
                let s = (a + b + c) / 2.0
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

let s = Shape.Circle(radius: 1.0)
print(s.area())          // 3.14159
print(s.isRound())       // true
```

> 注意 `Triangle(...)` 后没有逗号——最后一个变体与方法块之间用空白分隔（trailing comma 允许但不强制）。

**规则**：

- 方法语法与 `class` 内方法一致：`fn name(params) -> ReturnType { body }`
- 方法体内 `this` 的静态类型是 enum 自身（如 `Result<T, E>`），需要 `match (this)` 才能取出变体 payload
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
- 静态方法目前**不支持**（如需"工厂方法"请用顶层函数）

> 此设计与 Java enum / Swift enum / Kotlin sealed class 一致。Rust 的 `impl` 块在 xray 中**不**引入——xray 的方法定义统一在类型体内。

### 5.7 `type` 别名

```ebnf
TypeAliasDecl ::= 'type' Identifier TypeParams? '=' Type
```

```xray
type Outcome = int | string                          // union 别名（不要与 prelude 的 Result 重名）
type Mapper = fn(int) -> int                            // 函数类型别名
type Point = { x: float, y: float }                  // 结构化对象别名（sealed）
```

**语义**：
- 别名是**纯语法**替换，不产生新名义类型。
- `type Point = {...}` 的对象类型在使用此别名标注时**密封**：未声明的字段访问/赋值是编译错误。
- `type T = Json` 等于 `Json`（不密封）。
- 别名可前向引用，但**禁止循环别名**。
- 当前 `type` 别名不带类型参数；泛型抽象使用泛型函数、泛型 class / struct / enum / interface。

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
export publicFn, VERSION                    // 后置 export 已声明标识符列表
export { name1, name2 as alias } from "./other"
export * from "./other"
```

**xray 不支持** JavaScript 默认导入 `import name from "module"`。使用 `import "module" as name`、`import module` 或 `import { name } from module`。

完整规则、路径解析、可见性细则见 [§11 模块系统](#11-模块系统-modules)。

---

## 6. 模式 (Patterns)

> 真值源：`src/frontend/parser/xparse_match.c`、`src/runtime/value/x_value_match.c`。

模式出现在 `match` 表达式/语句与 `let` 解构中。

### 6.1 字面量模式

```xray
match (x) {
    0 -> "zero"
    3.14 -> "pi"
    "hello" -> "greeting"
    true -> "yes"
    null -> "nothing"
    _ -> "other"
}
```

- 匹配使用与 `==` 相同的语义。
- `null` 模式只匹配 `null` 本身。

### 6.2 范围模式 `a..b`

```xray
match (age) {
    0..13 -> "child"
    13..20 -> "teen"
    20..65 -> "adult"
    _ -> "senior"
}
```

- 半开区间 `[a, b)`，仅整数。

### 6.3 枚举模式

#### 6.3.1 简单变体（无 payload）

```xray
match (color) {
    Color.Red   -> "red",
    Color.Green -> "green",
    Color.Blue  -> "blue",
}
```

- 需完整限定 `EnumName.Variant`。

#### 6.3.2 ADT 变体（带 payload）解构

ADT 变体的模式可解构 payload 字段（按位置或按字段名）：

```xray
// 位置解构
match (event) {
    NetEvent.Connected            -> print("connected"),
    NetEvent.Disconnected(reason) -> print("by:", reason),
    NetEvent.DataReceived(b)      -> process(b),
    NetEvent.Error(code, msg)     -> log.error(code, msg),
}

// Result 模式（位置）
match (result) {
    Result.Ok(v)  -> print("got:", v),
    Result.Err(e) -> print("failed:", e),
}

// 通配符跳过 payload 中不关心的字段
match (event) {
    NetEvent.Error(code, _) if (code >= 500) -> throw new Exception("server error: ${code}"),
    _                                         -> continue,
}

// 嵌套解构
match (msg) {
    Result.Ok(NetEvent.DataReceived(bytes)) -> process(bytes),
    Result.Err(e)                            -> log.error(e),
    _                                         -> skip(),
}
```

#### 6.3.3 穷举性检查

`match` 一个 ADT enum 时，编译器执行**穷举性分析**：

- 若所有变体都被覆盖（含 `_` 兜底），通过
- 若漏写某变体，编译报错 `E0371 XR_ERR_ANALYZE_MATCH_NOT_EXHAUSTIVE`，并提示缺失的变体名

```xray
enum NetEvent {
    Connected,
    Disconnected(reason: string),
    DataReceived(bytes: Bytes),
    Error(code: int, message: string),
}

match (event) {
    NetEvent.Connected            -> "ok",
    NetEvent.Disconnected(r)      -> "down: ${r}",
    // ❌ E0371: 缺失变体 DataReceived 和 Error；可加 `_ -> ...` 兜底
}
```

> 简单枚举（无 payload）与 ADT enum 均**强制**穷举；只要包含 `_` 兜底分支即可跳过检查。对非 enum 变量（如 `int`）不强制。

### 6.4 类型模式 `is T`

```xray
match (value) {
    is int n -> "int: ${n}"       // 绑定窄化值
    is string -> "a string"
    is User u -> "user: ${u.name}"
    _ -> "unknown"
}
```

- 检查动态类型；可选绑定窄化变量。

### 6.5 守卫条件 `if`

```xray
match (x) {
    n if (n > 0 && n < 10) -> "small positive"
    n if (n < 0) -> "negative"
    _ -> "other"
}
```

- 守卫表达式位于 `if (...)` 括号内，按 truthy/falsy 上下文求值（见 §2.3.3，与 `if` / `while` 一致）；推荐显式使用 `bool` 表达式以提高可读性。
- 失败时继续尝试下一分支。

### 6.6 多值模式

```xray
match (x) {
    1, 2, 3 -> "small"
    Color.Red, Color.Yellow -> "warm"
    _ -> "other"
}
```

- 任一子模式匹配即成功。

### 6.7 通配符 `_`

- 匹配任意值，不绑定变量。
- 通常作为最后的 default 分支。
- 解构中可用于跳过位置：`let [_, b, _] = arr`。

### 6.8 变量绑定模式

```xray
match (http_status) {
    200 -> "ok"
    code if (code >= 400) -> "error: ${code}"
    code -> "other: ${code}"
}
```

- 裸 `Identifier` 总匹配并绑定为值。

### 6.9 解构模式

```xray
let [a, b, c] = some_array
let (q, r) = divmod(17, 5)
let { name, age } = user
```

详见 §5.1.5。`match` 中当前支持 tuple 与 ADT variant 解构；对象/数组结构解构不属于当前 `match` 模式语法。

### 6.10 穷举性与匹配失败

- 对 enum 表达式的 `match` 强制穷举（错误码 `E0371`，见 §6.3.3）。
- 其他类型不强制；运行时无分支匹配 → 抛 `Exception` 错误码 `E0442`（见 §18.x）。
- 建议总是提供 `_` 兜底。

---

## 7. 作用域与名字解析 (Scoping)

> 真值源：`src/frontend/analyzer/xanalyzer_scope.c`、`src/frontend/analyzer/xanalyzer_capture.c`。

### 7.1 词法作用域与提升

Xray 采用**词法作用域**：名字的可见性由源代码结构决定。

**作用域类型**：

| 作用域 | 触发 | 示例 |
|--|--|--|
| 模块 | 每个 `.xr` 文件 | 顶层 `let` `fn` `class` |
| 函数 / 闭包 | `fn` / 箭头函数进入 | 参数 + 函数体 |
| 块 | `{...}` | `if` `while` `for` `match` 分支体 |
| `scope` 块 | `scope { ... }` 关键字 | 显式词法作用域 + 结构化并发（见 §10.7） |
| `for` 头 | `for (let i=0; ...)` | `i` 仅循环体可见 |
| `catch` 参数 | `catch (e)` | `e` 仅 catch 体可见 |
| 类体 | `class` 定义 | 字段、方法 |

**提升规则**：

- 顶层 `fn` `class` `struct` `interface` `enum` `type` **提升**至当前作用域顶部——可在定义前引用。
- `let` / `const` **不提升**——必须在定义后使用。
- 同名重复声明：同作用域内 2 个同名变量 → 编译错误（嵌套作用域可 shadow）。

```xray
main()                    // OK：使用提升后的 fn
fn main() { ... }

let y = x                 // 错误：x 未声明
let x = 10
```

#### Shadow 规则

嵌套块可 shadow 外层同名变量：

```xray
let x = 1
{
    let x = "hello"           // shadow：OK
    print(x)                 // "hello"
}
print(x)                     // 1
```

### 7.2 闭包捕获语义

闭包捕获外层作用域的变量为 **upvalue**。

#### 普通同步闭包

默认按 **引用捕获**：

```xray
fn make_counter() -> (() -> int) {
    let count = 0
    return fn() -> int {
        count += 1                  // 修改外层 count
        return count
    }
}

let c = make_counter()
print(c())      // 1
print(c())      // 2
```

- 闭包与原变量**共享**。
- 外层作用域退出后，被闭包引用的变量会被 GC 保活（提升到堆）。

#### 闭包优化

编译器会分析 upvalue：
- 仅读 → 可能隐式复制（避免闭包转换）。
- 读写 → 提升为闭包 box。
- 详见 §17.5。

### 7.3 所有权与 move

Xray **不**是全面 ownership/borrow checker 语言（不像 Rust）。但在**跨协程数据传递**中使用 move 语义：

```xray
shared let big_buffer = new Bytes(1024 * 1024)

let t = go fn(b: Bytes) -> int {
    return process(b)
}(big_buffer)             // 编译错误：shared let 不能直接传递，必须 move

let t2 = go fn(b: Bytes) -> int {
    return process(b)
}(move big_buffer)        // OK：所有权转移

print(big_buffer.length)  // 编译错误：move 后访问
```

**move 使用场景**：`move` 作为**实参前缀**出现在调用位置（参见 §10.8）：

- `go f(move x)`、`go fn(...){...}(move x)`：把所有权转给协程。
- `ch.send(move data)`：跨协程发送时转移所有权（避免拷贝）。
- 普通函数调用 `f(move x)`：把所有权传入函数（被调函数独占）。

### 7.4 协程数据传递规则（避免数据竞争）

"保证编译期消除数据竞争"是 xray 并发模型的核心设计原则。

`go` 启动的协程**不能直接捕获**外层作用域的可变变量；数据必须通过**参数传递**进入协程。普通变量自动深拷贝；shared 变量按下表区分：

| 变量种类 | 跨协程传递规则 |
|---|---|
| 普通 `let` / `const`（局部） | 作为实参传递时**自动深拷贝**；不能被闭包捕获修改 |
| 函数参数 | ✅ 完全自由（已经是拷贝 / move 进来的） |
| `shared const` | ✅ 跨协程零拷贝只读共享（可被闭包捕获） |
| `shared let` | ⚠️ 必须用 `move` 实参前缀转移所有权；move 后原变量在编译期不可访问 |
| `Channel<T>` | ✅ 可被闭包捕获（生命周期由 channel 自身管理） |
| `this` / 闭包 upvalue（可变） | ❌ 不能跨协程；必须通过参数显式传递 |
| 全局 `import` 的函数/类 | ✅ 不可变定义，可自由引用 |

```xray
let local = 0
go { local += 1 }                        // ❌ 编译错误：不能捕获可变局部变量
```

#### 正确姿势

```xray
// 方法 1：作为参数传值（普通变量自动深拷贝）
let arr = [1, 2, 3]
let t = go fn(data: Array<int>) -> int {
    data.push(4)            // 拷贝上修改，不影响原值
    return data.length
}(arr)
print(arr)                  // [1, 2, 3] 未变

// 方法 2：shared const 零拷贝只读（可被捕获）
shared const config = { rate: 100 }
let t2 = go fn(c: Json) -> int {
    return c.rate
}(config)

// 方法 3：move 转移所有权
shared let big = new Bytes(1024)
let t3 = go fn(b: Bytes) -> int {
    return process(b)
}(move big)
// big 在此处不可访问

// 方法 4：Channel 通信（可被捕获）
shared const ch = new Channel<int>(10)
let t4 = go fn(c: Channel<int>) -> int {
    return c.recv()
}(ch)
ch.send(42)
```

### 7.5 GC 与对象生命周期

Xray 采用多层内存管理：

| 存储 | 机制 | 释放时机 |
|--|--|--|
| 全局堆（`shared const`） | refcount | refcount 变 0 |
| 局部堆（一般对象） | Mark-Sweep GC | 不可达时 |
| 栈（`struct` 值、本地） | RAII | 作用域退出 |
| Arena（底层临时分配） | 批量释放 | arena 结束 |

**GC 观察点**：
- 默认 incremental Mark-Sweep。
- Mark 阶段从根集（全局、栈、寄存器）遍历。
- Sweep 阶段释放未标记对象。
- GC 需要 GC-safepoint；指令列表中成为 safepoint 的点包括函数调用、后向跳转、显式 `gc.collect()`。

**写屏障**与**代际 GC** 设计：见 `docs/rules/gc-memory.md`。

---

## 8. 错误处理 (Error Handling)

> 真值源：`src/runtime/error/xerror.c`、`src/vm/xvm_exception.c`、`stdlib/types/exception.xr`、`stdlib/types/result.xr`。

### 8.0 设计哲学：双轨制

Xray 同时提供两套互补的错误处理机制：

| 机制 | 适用场景 | 失败可见性 |
|--|--|--|
| **异常**（`throw` / `try` / `catch`） | 真正罕见、跨多层传播；致命错误；框架/顶层兜底 | 隐式（不污染中间层签名） |
| **Result**（`Result<T, E>` enum） | 库 API 的明确失败模式；调用方必须穷举处理；可序列化错误 | 显式（编译期可见） |

两套机制**职责互补、无重叠**：

- 函数签名**不**标 `throws`（不引入 Java/Swift 风格的"必须声明可抛异常"语义）。需要让调用方编译期看到失败可能时，请用 `Result<T, E>` 作为返回类型。
- `Exception` 是异常轨的统一基类（见 §8.1.4）；`Result<T, E>` 是 prelude enum（见 §8.2）。
- 三个糖关键字 `try!` / `try?` / `catch!` 负责两轨之间的桥接（见 §8.3）。

### 8.1 异常机制

#### 8.1.1 `throw` 表达式

`throw expr` 抛出异常。**`expr` 的静态类型必须是 `Exception` 或其派生类**。其它类型在编译期被拒绝（错误码 `E0370` `XR_ERR_ANALYZE_THROW_NON_EXCEPTION`）：

```xray
throw new Exception("oops")                 // ✅
throw new HttpError(404, "not found")       // ✅ 自定义 Exception 子类
throw "oops"                                // ❌ E0370: throw 必须是 Exception 派生
throw 42                                    // ❌ E0370
throw { code: 500 }                         // ❌ E0370
throw null                                  // ❌ E0370（静态类型为 null）
```

> **设计说明**：xray 早期允许 `throw` 任意值（字符串、整数、对象等）；自当前版本起，参考 Python 3 / Java / Swift 收紧为"必须 Exception 派生"。这与 xray "无 `any` 类型"的设计原则自洽——`catch (e)` 中 `e` 的静态类型恒为 `Exception`，工具链可稳定提供补全和类型分析。

抛出后行为：

```
抛出点 → 调用栈向上展开 → 沿途执行 defer / finally → catch 处理 → 否则继续向上 → 协程终止
```

未捕获的顶层异常会终止当前协程：

- 子协程：默认打印堆栈到 stderr，协程终止；**不**自动通知父协程（异常**不跨协程传播**，见 §8.1.6）
- 主协程：进程退出码 = 1

#### 8.1.2 `try` / `catch` / `finally`

```xray
try {
    risky()
} catch (e) {
    log.error(e.message)
} finally {
    cleanup()
}
```

**执行顺序**：

1. 执行 `try` 块
2. 若有异常逃出，按声明顺序逐一尝试匹配 `catch` 子句；首个匹配者执行其块体
3. 无论是否异常，都执行 `finally` 块（保证执行）
4. 若没有 `catch`，异常在 `finally` 之后继续向上传播

**类型化 catch 与多 catch 子句**：

`catch` 变量可带类型注解 `catch (e: T)`，运行时用 `is T` 判断是否匹配。支持多个 `catch` 子句，按声明顺序匹配，首个匹配者执行：

```xray
try {
    riskyIO()
} catch (e: HttpError) {
    log.error("HTTP:", e.statusCode)
} catch (e: DbError) {
    log.error("DB:", e.query)
} catch (e) {
    log.error("unexpected:", e.message)
}
```

**规则**：
- 无类型注解的 `catch (e)` 是"catch-all"，匹配所有异常；`e` 静态类型为 `Exception`。
- 有类型注解的 `catch (e: T)` 仅当异常值 `is T` 为真时匹配；`e` 静态类型为 `T`。
- 多个 `catch` 子句按声明顺序匹配，首个匹配者执行。
- 若所有类型化子句均不匹配且没有 catch-all，异常被自动重抛。
- 一个 `try` **必须**至少跟随 `catch` 或 `finally` 之一。

#### 8.1.3 重抛

`catch` 块内可重抛原异常或新异常：

```xray
try {
    fetch(url)
} catch (e) {
    log.error("network failed:", e.message)
    throw new ServiceError("upstream unavailable", e)  // 用 cause 链传递原因
}
```

#### 8.1.4 `Exception` 类

`Exception` 是 prelude 的内置类（声明：`stdlib/types/exception.xr`），可直接 `new`：

```xray
@native
class Exception {
    message: string             // 人类可读消息
    stack: Array<string>        // 自动 capture 的调用栈，每帧一行格式化字符串
    cause: Exception?           // 链式 cause
    code: int                   // 错误码（从 "E0xxx: ..." 前缀自动解析，默认 0）
    data: Json?                 // throw 非异常值时原始值被包装在此

    constructor(message: string = "", cause: Exception? = null)
    fn toString() -> string
}
```

字段语义：

- `message`：人类可读的错误消息（构造时传入）
- `stack`：`Array<string>`。构造时为空数组；`throw` 控制流经过其他帧时逐步 push 一行格式化帧描述（如 `"at f (line N)"`）。用户可遍历、join、取长度；可读可写但运行时不依赖用户修改。
- `cause`：可选的原因异常，用于错误链
- `code`：整形错误码。VM 报错时在 message 前缀 `"E0xxx: ..."` ，primitive ctor 解析该前缀并写入 code；用户手动 throw 时 code=0
- `data`：`Json?`。`throw <非Exception值>` 被运行时包装为 Exception 时，原始值放在此处；catch 可从 `e.data` 取回

构造方式：

```xray
throw new Exception("connection refused")
throw new Exception("upstream failed", originalErr)
```

#### 8.1.5 自定义 `Exception` 子类

定义业务错误推荐继承 `Exception`：

```xray
class HttpError extends Exception {
    statusCode: int
    constructor(statusCode: int, message: string, cause: Exception? = null) {
        super(message, cause)
        this.statusCode = statusCode
    }
}

class DbError extends Exception {
    table: string
    constructor(table: string, message: string) {
        super(message)
        this.table = table
    }
}

throw new HttpError(404, "not found")

try {
    fetchUser(id)
} catch (e: HttpError) {
    log.error("http", e.statusCode, e.message)
}
```

继承自 `Exception` 的子类自动获得 `message` / `stack` / `cause` 与 `toString()`，并可附加任意业务字段。

#### 8.1.6 异常的协程边界

异常**不跨协程传播**。子协程内未捕获的异常：

- 子协程立即终止
- 默认行为：将异常的 `toString()` + `stack` 打印到 stderr
- 父协程**不**自动感知

需要传递子协程错误时，用 Channel 显式传递：

```xray
const err_ch = Channel<Exception?>(1)

go {
    try {
        risky()
        err_ch.send(null)
    } catch (e) {
        err_ch.send(e)
    }
}

let err = err_ch.recv()
if (err != null) { log.error(err.message) }
```

或使用结构化并发的 `scope` 块（见 §10.5），让 scope 自动传播子协程异常给父协程。

#### 8.1.7 `defer`

`defer` 是资源清理语句，在所属作用域退出时**保证执行**（不论是否有异常）。语法见 §4.9。与 `try/finally` 关系：

- 二者可以混用
- 单个作用域多个 `defer` 按 **LIFO** 顺序执行
- 必发顺序（栈展开时）：
  1. 内层 `try` 的 `finally` 依次执行
  2. 当前作用域的 `defer` 按 LIFO 执行
  3. 控制权返回调用者（或异常继续向上传播）

```xray
fn fetch(url: string) -> string {
    let conn = open(url)
    defer conn.close()                       // 无论后续如何，conn 一定关闭

    try {
        return conn.read()
    } catch (e) {
        log.error(e.message)
        throw e                              // 重抛；defer 仍会执行
    }
}
```

### 8.2 `Result<T, E>`

#### 8.2.1 类型与构造

`Result<T, E>` 是 prelude 的内置 ADT enum（声明：`stdlib/types/result.xr`）：

```xray
@native
enum Result<T, E> {
    Ok(T),
    Err(E),
}
```

构造与解构：

```xray
let r1: Result<int, ParseError> = Result.Ok(42)
let r2: Result<int, ParseError> = Result.Err(ParseError.Empty)

match (r1) {
    Result.Ok(v)  -> print("got:", v),
    Result.Err(e) -> print("failed:", e),
}
```

#### 8.2.2 错误类型 `E` 的选择

`E` 可以是任意类型；推荐风格：

| 错误形态 | E 类型 | 例子 |
|--|--|--|
| 多种可枚举的失败原因 | 用户定义的 ADT enum | `enum ParseError { Empty, NotNumber(s: string), Overflow }` |
| 单一字符串错因 | `string` | `Result<int, string>` |
| 异常对象（与异常轨桥接） | `Exception` 派生 | `Result<T, Exception>` |

**强烈推荐用 ADT enum**——可让 `match` 在编译期检查错因穷举性。

#### 8.2.3 `Result` 上的方法

`Result<T, E>` 是带方法的 enum（见 §5.6.7）。完整签名（声明：`stdlib/types/result.xr`）：

```xray
@native
enum Result<T, E> {
    Ok(T),
    Err(E)

    fn isOk() -> bool
    fn isErr() -> bool

    fn ok() -> T?                                          // Ok(v) -> v；Err -> null
    fn err() -> E?                                         // Err(e) -> e；Ok -> null

    fn unwrap() -> T                                       // Err 时 throw（要求 E 是 Exception 派生）
    fn unwrapOr(default: T) -> T                           // Err 时返回 default
    fn unwrapOrElse(handler: (E) -> T) -> T                // Err 时由 handler 计算

    fn map<U>(transform: (T) -> U) -> Result<U, E>         // 转换 Ok 内的值
    fn mapErr<F>(transform: (E) -> F) -> Result<T, F>      // 转换 Err 内的值
    fn andThen<U>(transform: (T) -> Result<U, E>) -> Result<U, E>  // 链式（flatMap）
}
```

`map` / `mapErr` 是"不破壳换内里"的快捷方式，避免每次 `match` 套话：

```xray
// 不用 map：5 行
let r2: Result<float, ParseError>
match (parseInt(s)) {
    Result.Ok(n)  -> r2 = Result.Ok(n.toFloat()),
    Result.Err(e) -> r2 = Result.Err(e),
}

// 用 map：1 行
let r2 = parseInt(s).map(n -> n.toFloat())
```

#### 8.2.4 错误类型转换：必须显式 `mapErr`

跨层 `Result` 组合时，**不**自动转换 Err 类型。调用方必须显式 `.mapErr(...)`：

```xray
fn loadConfig(text: string) -> Result<Config, ConfigError> {
    let json = try! parseJson(text).mapErr(e -> ConfigError.BadJson(e))
    //                              ^^^^^^^^ 必须显式：ParseError → ConfigError
    let port = try! json["port"].toInt().mapErr(e -> ConfigError.BadField("port", e))
    return Result.Ok(Config(port: port))
}
```

每个 `.mapErr(...)` 是一行**人类可读的错误升级路径**——优于 Rust `From::from` 的隐式转换。

### 8.3 桥接：`try!` / `try?` / `catch!`

三个糖关键字覆盖所有两轨之间的桥接，同时也是同轨道内的早退糖。

#### 8.3.1 `try! e` —— 早退或跨轨升级

`try!` 后面的表达式静态类型**必须**是 `Result<T, E>` 或 `T?`。其它类型编译报错（`E0821 XR_ERR_TRY_BANG_BAD_OPERAND`）。

行为按 `e` 类型 + 当前函数返回类型双重 dispatch：

| `e` 类型 | 当前函数返回 | 行为 |
|--|--|--|
| `Result<T, E>` | `Result<_, E>` | `Err(e)` → `return Result.Err(e)`；`Ok(v)` → `v` |
| `Result<T, E>` | 其它 | `Err(e)` → `throw e`（要求 `E` 是 `Exception` 派生，否则 `E0822`）；`Ok(v)` → `v` |
| `T?` | `_?` | `null` → `return null`；`v` → `v` |
| `T?` | 其它 | `null` → `throw new NullThrowError("try! on null")`；`v` → `v` |

例子：

```xray
// 同轨道早退
fn parsePair(s: string) -> Result<(int, int), ParseError> {
    let parts = s.split(",")
    if (parts.length != 2) return Result.Err(ParseError.Empty)
    let a = try! parseInt(parts[0])              // Err 早退 Err
    let b = try! parseInt(parts[1])
    return Result.Ok((a, b))
}

// 跨轨道升级（Result → 异常）
fn dangerous(s: string) -> int {
    let n = try! parseInt(s)                     // Err 抛异常
    return n * 2
}

// Optional 早退
fn lookupTwo(m: Map<string, int>, k1: string, k2: string) -> int? {
    let v1 = try! m.get(k1)                      // null 早退 null
    let v2 = try! m.get(k2)
    return v1 + v2
}
```

> **`try!` 不是异常调用的强制仪式**——xray **不**像 Swift 那样要求每个会抛的调用前都写 `try`。`try!` 只用于 `Result` / `Optional` 的早退或升级。

#### 8.3.2 `try? e` —— 失败转 null

`try?` 把任何失败统一转为 `null`，丢弃错因。

| `e` 类型 | `try? e` 类型 | 行为 |
|--|--|--|
| `Result<T, E>` | `T?` | `Err` → `null`（错因丢弃）；`Ok(v)` → `v` |
| 普通可抛调用，类型 `T` | `T?` | 抛异常 → `null`；正常 → `v` |
| `T?` | `T?` | 透传 |

```xray
let n: int? = try? parseInt(s)              // Err 转 null
let v: Json? = try? http.get(url).json()    // 抛异常转 null
```

`try?` 适合"调用方不关心错因，只想要默认值"的场景，常与 `??` 搭配：

```xray
let port = (try? parseConfig(text).map(c -> c.port)) ?? 8080
```

#### 8.3.3 `catch! { ... }` —— 异常块凝结成 Result

`catch!` 把一段可能抛异常的代码块凝结成 `Result<T, Exception>`：

```xray
let r: Result<Server, Exception> = catch! {
    let cfg = loadConfig(path)
    let conn = openDb(cfg.dbUrl)
    return startServer(cfg, conn)
}

match (r) {
    Result.Ok(s)  -> s.run(),
    Result.Err(e) -> log.error("startup failed:", e.message),
}
```

规则：

- 块的最后一个表达式或 `return v` 作为 `Ok(v)` 的值
- 块内**逃出的任何异常**被捕获并包装为 `Err(e)`
- `e` 的静态类型为 `Exception`（即结果恒为 `Result<T, Exception>`）
- 块内 `return v` 是"从 `catch!` 块返回"，**不是**从外层函数返回
- `defer` 在块内仍按规则执行
- 不支持类型过滤——需要过滤特定异常请用手写 `try` / `catch`

#### 8.3.4 `Result.unwrap()` —— Result 升级为异常

`unwrap()` 是反方向桥接：把 `Err(e)` 变成 `throw e`：

```xray
let cfg = loadConfig(text).unwrap()         // Err 时抛异常（要求 E 是 Exception 派生）
```

若 `E` 不是 `Exception` 派生，编译期报错（建议使用 `unwrapOr(...)` 或 `match`）。

#### 8.3.5 桥接矩阵

```
                   ┌──────────────────────────────────────────┐
                   │  ↓ 异常轨 (throw / catch)                 │
                   │                                          │
       catch! { } ─┤◄── 异常 → Result<T, Exception>          │
                   │                                          │
       try? expr  ─┤◄── 异常 → T? (丢弃错因)                 │
                   │                                          │
       try { } catch (e) { ... }    完整异常控制流          │
                   └──────────────────────────────────────────┘
                                    ▲
                                    │  unwrap / unwrapOr / try!
                                    │
                   ┌──────────────────────────────────────────┐
                   │  ↑ Result 轨 (Result<T, E>)              │
                   │                                          │
       try! result ─┤── 同轨道早退 / 跨轨抛异常              │
                   │                                          │
       try? result ─┤── Err → null (丢弃错因)                │
                   │                                          │
       result.ok()  ─── Result<T,E> → T?                     │
                   └──────────────────────────────────────────┘
```

### 8.4 Optional 与错误处理

`T?` 是 `T | null` 的语法糖，用于"二态：有值/无值"场景。详见 §2.5。与错误处理的关系：

- **不带错因的失败**：用 `T?` 比 `Result<T, ()>` 更简洁
- 与 `try!` / `try?` 协同（见 §8.3.1 / §8.3.2）
- 与 `??`（默认值）/ `?.`（可选链）/ `e!`（强制解包）配合
- 不要把 `T?` 当通用错误返回——需要错因时用 `Result<T, E>`

### 8.5 决策树：选择哪种机制

按"**调用方对失败的处理需求**"选择：

```
失败需要被调用方处理吗？
│
├─ 不需要（致命、不可恢复、跨多层兜底）
│   ↓
│   throw / try-catch
│
├─ 需要，且失败有结构化错因，调用方应穷举处理
│   ↓
│   Result<T, E>，E 用 ADT enum
│
├─ 需要，但失败只表示"没值"，无错因意义
│   ↓
│   T? + ?? / ?. / try?
│
├─ 需要，且函数有 ≥3 种正常状态（不只是"成功/失败"）
│   ↓
│   用户 ADT enum 直接作为返回类型
│
└─ 需要返回多个对等值（不是"成功/失败"二元）
    ↓
    tuple (a, b, ...)
```

完整对照：

| 场景 | 推荐 | 例 |
|--|--|--|
| 解析、解码、状态转换 | `Result<T, E>` | `parseInt(s) -> Result<int, ParseError>` |
| 字典查找、可选字段 | `T?` | `map.get(k) -> Value?` |
| IO、网络、不可恢复 | `throw` + 顶层 `catch` | `readFile(p) -> Bytes`（可能抛 IOError） |
| 多分支结果 | enum | `nextEvent() -> NetEvent` |
| 主结果 + 元数据 | tuple | `parse(s) -> (Ast, int)` // ast + 已读字节数 |

### 8.6 常用模式

#### 模式 1：库 API 用 Result，业务边界用异常

```xray
// 库层：Result
fn parseConfig(text: string) -> Result<Config, ConfigError> {
    let json = try! parseJson(text).mapErr(e -> ConfigError.BadJson(e))
    return Result.Ok(Config(port: json["port"].toInt().unwrap()))
}

// 业务层：组合 Result
fn loadConfig(path: string) -> Result<Config, ConfigError> {
    let text = readFile(path)                    // 可能抛 IOError（异常）
    return parseConfig(text)
}

// 顶层：异常 catch 兜底（必须显式调用，没有自动 main）
fn main() {
    try {
        let cfg = loadConfig("app.toml").unwrap()
        startServer(cfg)
    } catch (e: IOError) {
        log.error("config missing:", e.message)
        exit(1)
    } catch (e: ConfigError) {
        log.error("bad config:", e)
        exit(2)
    }
}

main()                                     // 显式调用入口
```

#### 模式 2：异常块凝结成 Result（用于 RPC / 序列化）

```xray
fn handleRequest(req: Request) -> Response {
    let r: Result<Json, Exception> = catch! {
        let user = db.query(req.userId)         // 可能抛 DbError
        return user.toJson()                    // 可能抛 SerializeError
    }
    match (r) {
        Result.Ok(json) -> Response.success(json),
        Result.Err(e)   -> Response.error(500, e.message),
    }
}
```

#### 模式 3：`try?` + `??` 提供默认值

```xray
let port = (try? parseConfig(text).map(c -> c.port)) ?? 8080
let user = (try? db.findUser(id)) ?? guestUser
```

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

```xray
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

```xray
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

```xray
identity(42)                    // T 推断为 int
new Box("hello")                // T 推断为 string
new Pair("key", 100)            // K=string, V=int
```

推断算法是**双向推断**：
- 从参数推断（调用位置实参类型 → 类型参数）。
- 从返回值推断（上下文期望类型 → 类型参数）。

#### 显式实例化

在推断失败或需要明确时：

```xray
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

```xray
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

```xray
class Container<T> {
    items: Array<T>
}
let c = new Container<int>()
print(Reflect.typeOf(c))       // "Container<int>"
```

对具体值的类型检查使用 `is` / `as`。

---

## 10. 并发与协程 (Concurrency)

> 真值源：`src/runtime/coro/xcoro_*.c`、`src/runtime/sync/xchannel.c`、`src/runtime/sync/xscope.c`、`docs/rules/design-principles.md`。

xray 的并发是**协程 (goroutine 风格) + Channel + 强静态约束**。设计目标：写 `go { ... }` 就和写普通函数一样简单，但**编译期保证不发生数据竞争**。

### 10.1 协程模型

| 维度 | 选择 |
|--|--|
| 调度模型 | M:N（用户态协程 + 多 OS 线程） |
| 调度策略 | 协作式（GC-safepoint）+ work-stealing |
| 栈模型 | Segmented stack（按需扩展） |
| 创建开销 | ~微秒级，KB 级初始栈 |
| 上下文切换 | 用户态切栈，无 syscall |

协程默认分布在多个 worker 线程上；运行时根据 CPU 核数自动设置 GOMAXPROCS 风格的并行度。

### 10.2 `go` — 启动协程

```ebnf
GoExpr ::= 'go' (Block | CallExpr | LambdaExpr CallArgs?)
```

`go` 是**表达式**，返回 `Task<T>` 句柄。三种形式都合法：

```xray
// 形式 1：调用一个已声明的函数
let t1 = go worker(0, channel)

// 形式 2：调用一个 lambda 字面量（用于内联逻辑+捕获参数）
let t2 = go fn(d: Json) -> int {
    return d.value * 2
}(payload)

// 形式 3：块形式（隐式包装为零参 lambda）
let t3 = go {
    return compute()
}
```

**move 在参数位置**：跨协程转移所有权通过参数前缀 `move` 实现，**不是** `go` 的选项：

```xray
shared let data = { value: 10 }
let task = go fn(d: Json) -> int {
    return d.value + 1
}(move data)        // 把 data 的所有权移交给协程；之后 data 不可访问
```

**语义**：
- 每个 `go` 表达式都返回一个 `Task<T>`，其中 `T` 是被调函数的返回类型；返回 `()` 的函数对应 `Task<null>`。
- 协程在闲置 worker 线程中调度（M:N）。
- 协程内**未捕获**异常存在 `Task` 中，由 `await` 时重抛。
- 普通局部变量（非 `shared`、非 `move`）传给 `go` 时**自动深拷贝**；`shared const` 零拷贝共享；`shared let` 必须 `move`。

### 10.3 `await` — 等待结果

```ebnf
AwaitExpr ::= 'await' Expression
           |  'await' 'all' Expression       // 等待全部完成
           |  'await' 'any' Expression       // 等待任一完成
```

```xray
// 单 task
let task = go fetch("https://example.com")
let result = await task                    // 让出当前协程直到 task 完成

// await all：等待全部完成，返回结果数组（与输入顺序一致）
let t1 = go compute(2)
let t2 = go compute(3)
let t3 = go compute(4)
let results: Array<int> = await all [t1, t2, t3]
// 也可直接对变量使用，无需中括号
let tasks = [t1, t2, t3]
let results2: Array<int> = await all tasks

// await any：等待任一完成，返回该任务结果；其他任务继续运行
let first = await any [t1, t2, t3]

// await anySuccess：跳过失败任务，等待第一个成功的任务
let firstOk = await anySuccess [t1, t2, t3]
```

**语义**：

- `await` 仅作用于 `Task<T>` 类型；其他类型为编译错误。
- 当前协程**让出**直到目标完成（不阻塞 OS 线程）。
- 异常传播：
  - `await t` 重抛 t 抛出的异常。
  - `await all` 中任一任务抛异常即整体抛异常（其余任务会被取消）。
  - `await any` 仅当**全部失败**时抛异常；只要有一个完成，返回该任务结果。
  - `await anySuccess` 类似 `await any`，但**跳过**抛异常的任务，只等成功完成的。
- `all` / `any` / `anySuccess` 在 `await` 后面是**上下文关键字**，仅在此位置生效。

### 10.4 `Task<T>` 句柄

`go expr` 返回 `Task<T>`，其中 `T` 为被调函数的返回类型。Task 句柄支持：

| 方法 / 属性 | 类型 | 说明 |
|--|--|--|
| `t.done` | `bool`（属性） | 任务是否已完成（成功、失败或取消） |
| `t.cancelled` | `bool`（属性） | 任务是否被取消过 |
| `t.result` | `Json`（属性） | 任务返回值；未完成或失败时为 `null` |
| `t.error` | `string?`（属性） | 任务的异常消息；未失败时为 `null` |
| `t.cancel()` | `() -> ()` | 请求取消任务（合作式） |

```xray
let t = go fetch(url)
if (!t.done) { /* 还在跑 */ }
let r = await t

// 直接读取属性（无需 await）
print(t.done, t.cancelled, t.result, t.error)
```

**取消语义**：`cancel()` 设置取消标志；协程在下一个 safepoint（GC 检查点、Channel 操作、`await`、`yield`）检测到标志后抛出取消异常。`await` 已取消的 task 返回 `null`，`t.cancelled` 变为 `true`。

### 10.5 Channel

```ebnf
ChannelType ::= 'Channel' '<' Type '>'
ChannelNew  ::= 'new' 'Channel' ('<' Type '>')? '(' Expression ')'
```

Channel 通常以 `shared const` 声明（生命周期跨协程，引用语义）：

```xray
shared const ch  = new Channel<int>(10)    // 有缓冲，capacity = 10
shared const ch0 = new Channel<int>(0)     // 无缓冲（同步握手）
shared const cha = new Channel(3)          // 元素类型从首次 send 推断
```

**API**（注意全部为 **camelCase**）：

| 方法 | 签名 | 行为 |
|--|--|--|
| `send(v)` | `(T) -> ()` | 阻塞发送；满则等待消费者；channel 已关闭时抛异常 |
| `recv()` | `() -> T?` | 阻塞接收；空则等待生产者；channel 已关闭且缓冲为空时返回 `null` |
| `trySend(v)` | `(T) -> bool` | 非阻塞：成功返回 `true`，满或已关闭返回 `false` |
| `tryRecv()` | `() -> (T, bool)` | 非阻塞，返回**多值**：`(value, ok)`；空或已关闭时 `ok=false` |
| `sendTimeout(v, ms)` | `(T, int) -> bool` | 带超时发送；超时返回 `false` |
| `recvTimeout(ms)` | `(int) -> (T, bool)` | 带超时接收；超时返回 `ok=false` |
| `close()` | `() -> ()` | 关闭 channel；幂等 |
| `isClosed` | `bool`（属性） | channel 是否已关闭 |

```xray
shared const ch = new Channel<int>(10)
ch.send(42)                             // 阻塞发送
let v = ch.recv()                       // 阻塞接收（关闭并取空后返回 null）

let sent = ch.trySend(99)               // 非阻塞发送：true / false
let (next, ok) = ch.tryRecv()           // 非阻塞接收：值 + 成功标志
if (ok) { print(next) }

ch.close()
```

**send/recv 与 `move`**：发送大对象时用 `ch.send(move payload)` 转移所有权，避免拷贝；接收方独占。

Channel 在类型位置写作 `Channel<T>`，可用于函数参数、字段和返回类型：

```xray
fn producer(ch: Channel<int>) {
    ch.send(42)
}
```

**语义**：
- **MPMC**（多生产者多消费者）。
- 有缓冲 ch：满则发送方挂起，空则接收方挂起。
- 无缓冲 ch：发送/接收必须同时握手（rendezvous）。
- 关闭后：`send` 抛异常；`recv` 返回剩余值，剩余取完后返回 `null`；`tryRecv` 返回 `(zero, false)`。

### 10.6 `select`

`select` 在多个 channel 操作中多路复用；非阻塞分支用 `_` 占位。

```ebnf
SelectStmt ::= 'select' '{' SelectArm+ '}'
SelectArm  ::= RecvArm | SendArm | TimeoutArm | DefaultArm
RecvArm    ::= Identifier 'from' Expression '->' Block
SendArm    ::= Expression 'to' Expression '->' Block
TimeoutArm ::= 'after' Expression '->' Block
DefaultArm ::= '_' '->' Block
```

```xray
shared const ch1 = new Channel<int>(2)
shared const ch2 = new Channel<int>(2)

select {
    msg from ch1 -> { print("got from ch1:", msg) }      // 接收分支
    msg from ch2 -> { print("got from ch2:", msg) }      // 接收分支
    100  to   ch1 -> { print("sent 100 to ch1") }        // 发送分支
    _ -> { print("no channel ready") }                   // 默认分支（非阻塞）
}
```

**语义**：
- 接收分支 `name from ch -> body`：等价于 `name = ch.recv()`，但仅在 ch 有数据时被选中。
- 发送分支 `value to ch -> body`：等价于 `ch.send(value)`，但仅在 ch 有空间时被选中。
- 默认分支 `_ -> body`：当前无任何分支就绪时立即执行；**省略默认分支**会让 select 阻塞直到某个分支就绪。
- 多个分支同时就绪时**随机**选择一个（与 Go 一致）。

### 10.7 `scope` 块（结构化并发 / 词法作用域）

`scope` 是**语句关键字**，建立一个新的词法作用域块。它服务两个目的：

1. **纯词法作用域**：与 C/Rust `{ ... }` 局部块一致，块内 `let` 不影响外层同名变量。
2. **结构化并发**（语义增强）：在 `scope` 块内 `go` 启动的协程，块退出前**自动等待**全部完成或取消。

```ebnf
ScopeStmt          ::= 'scope' Block
LinkedScopeStmt    ::= 'linked' 'scope' Block          // 兄弟失败 → 取消所有 + 重抛
SupervisorScopeExpr ::= 'supervisor' 'scope' Block     // 收集所有错误，返回 Array<string>
```

```xray
// 词法作用域用途
let x = 1
scope {
    let x = 10            // shadow 外层 x，块内有效
    print(x)              // 10
}
print(x)                  // 1

// 结构化并发用途（与 go 配合）
scope {
    go worker_a()
    go worker_b()
    // scope 块退出前，等待 a/b 全部完成；任一抛异常不影响兄弟
}
```

**三种 scope 变体**：

| 形式 | 子协程抛异常时的行为 | 返回值 |
|---|---|---|
| `scope { ... }` | 不取消兄弟；异常不向外传播（每个 task 独立） | 无（语句形式） |
| `linked scope { ... }` | **取消所有兄弟**协程，并向外**重抛**最先抛出的异常 | 无 |
| `supervisor scope { ... }` | **收集**所有失败子协程的异常消息，子协程之间互不影响 | `Array<string>`（错误列表；可为空表示全部成功） |

```xray
// linked scope：失败传播
try {
    linked scope {
        go ok_worker()
        go failing_worker()         // 抛异常
    }
} catch (e) {
    print("caught:", e)              // 命中此分支
}

// supervisor scope：收集错误
let errors = supervisor scope {
    go failing("error1")
    go failing("error2")
    go ok()
}
print(errors.length)                 // 2（只统计失败的）
```

**通用语义**：
- `scope` 不是函数调用，也不需要 import；是关键字块语句。
- 三种形式都在块退出前等待所有 `go` 启动的子协程完成。

### 10.8 `move` — 跨协程所有权转移

```ebnf
MoveExpr ::= 'move' Identifier        // 仅出现在调用参数位置
```

`move` 是**实参修饰前缀**（不是 `go` 的选项）。它把 `shared let` 变量的所有权从当前作用域转移到被调函数（包括 `go` 启动的协程、`ch.send()` 等）。move 后原变量在编译期被标记为**已 moved**，再次引用是编译错误。

```xray
shared let buf = new Bytes(1024 * 1024)

// 移交给协程
let t = go fn(b: Bytes) -> int {
    return process(b)
}(move buf)
// 编译错误：buf has been moved
// print(buf.length)

// 移交给 channel
shared const ch = new Channel<Bytes>(1)
shared let payload = new Bytes(4096)
ch.send(move payload)
// 编译错误：payload has been moved
```

详见 §7.3、§7.4 关于 shared 变量的捕获规则。

### 10.9 同步原语

xray 的默认并发模型偏向**消息传递 + 不可变共享**——通过 `shared const`、`Channel`、`move`、`scope` 已能在编译期消除大部分数据竞争，因此**不**鼓励使用裸 Mutex/锁。

如确需互斥锁/原子操作，运行时层面提供：

| 原语 | 形态 | 说明 |
|---|---|---|
| Channel(1) | 单元素 channel | 互斥的最佳实践（通过 send/recv 模拟 lock/unlock） |
| `shared let` + `move` | 编译期独占 | 跨协程独占，无运行时开销 |

> **设计说明**：xray 不在标准库中暴露 `Mutex`/`RwLock`/`Atomic*` 等通用并发原语；如未来引入，将作为 unstable 模块单独开放（参见 `docs/known_bugs.md` 与未来设计 RFC）。


### 10.10 `yield` — 让出 CPU

```ebnf
YieldStmt ::= 'yield'
```

```xray
for (i in 0..1000) {
    do_chunk(i)
    yield                       // 主动 safepoint，让其他协程有机会跑
}
```

**当前实现**：作为语句使用，等价 Go 的 `runtime.Gosched()`；不支持带值 `yield`。

### 10.11 并发安全模型

xray 通过类型系统**编译期消除大部分数据竞争**：

| 规则 | 强制 |
|--|--|
| `go` 闭包不能捕获普通 `let` 局部变量 | ✅ |
| `shared const` 跨协程零拷贝只读 | ✅ |
| `shared let` 必须 `move` 才能跨协程 | ✅ |
| Channel 跨协程传值 | ✅ |
| 共享可变状态必须显式 Mutex | 文档约定 |

**仍可能存在数据竞争**（运行时检测，非编译期）：
- 在 Channel 中发送可变 class 引用（接收方可能与发送方同时修改）— 建议总是发送 `shared const` / `Bytes` / 不可变对象 / `move` 移交。

---

## 11. 模块系统 (Modules)

> 真值源：`src/module/xmodule.c`、`src/module/xmodule_resolve.c`、`src/frontend/parser/xparse_import.c`。

### 11.1 模块定义

- 每个 `.xr` 文件是一个模块。
- 模块名 = 文件名（去除 `.xr` 后缀）。
- 模块路径反映目录结构：`src/utils/string.xr` → `utils.string`。

### 11.2 项目结构

```
my_project/
├── xray.toml              # 包清单（包名、依赖、入口）
├── src/
│   ├── main.xr            # 入口
│   ├── utils.xr
│   └── lib/
│       └── helper.xr
├── tests/
│   └── test_utils.xr
└── docs/
```

`xray.toml` 示例：

```toml
[package]
name = "my_project"
version = "0.1.0"
entry = "src/main.xr"

[dependencies]
http = "1.0"
json = "0.2"

[dev-dependencies]
test = "1.0"
```

### 11.3 `import` 语法

```ebnf
ImportStmt ::= 'import' ImportMembers 'from' ImportModule
            |  'import' ImportModule ('as' Identifier)?
ImportMembers ::= '{' ImportMember (',' ImportMember)* ','? '}'
ImportMember  ::= Identifier ('as' Identifier)?
ImportModule  ::= StringLiteral | ModuleName
ModuleName    ::= Identifier ('/' Identifier)?
```

```xray
// 1. stdlib：裸标识符；没有 `as` 时别名等于模块名
import time
import datetime
import http as httpClient

// 2. 第三方包：owner/name 形式
import alice/utils
import bob/http_client as httpClient

// 3. 文件路径或目录路径：字符串字面量，可显式 alias，也可从路径尾段推导
import "./modules/mod_a.xr" as a
import "../utils/string_utils.xr" as utils
import "models/user" as user

// 4. 命名 import：成员可重命名；`from` 后可接字符串路径或裸模块名
import { readFile, writeFile as write } from io
import { publicFn } from "./modules/mod_a.xr"
```

**不支持** JavaScript 默认导入 `import name from "module"`。在 Xray 中使用 `import "module" as name`、`import module` 或 `import { name } from module`。

**解析算法**（按优先级）：
1. **stdlib 命名解析**：裸标识符 `import time` → 内置 stdlib 模块表。
2. **相对路径**：`"./xxx.xr"` 与 `"../xxx.xr"` 相对当前文件解析。
3. **项目根目录路径**：不以 `./` 或 `../` 开头的 quoted path 作为项目目录 import。
4. **第三方包**：`owner/name` 由 `xray.toml` 的 `[dependencies]` 解析。

**真值源**：`xparse_import.c` 与 `xmodule_resolve_path()`。

### 11.4 `export` 与可见性

xray 支持三种 export 形式：

```ebnf
ExportStmt ::= 'export' Declaration                              // 直接 export 声明
            |  'export' Identifier                               // export 已声明的标识符
            |  'export' '{' ExportSpec (',' ExportSpec)* '}' 'from' StringLiteral
            |  'export' '*' 'from' StringLiteral
ExportSpec ::= Identifier ('as' Identifier)?
```

```xray
// 1. 直接 export 声明
export fn helper() { return }
export class MyClass {
    value: int
    constructor() { this.value = 1 }
}
export const VERSION = "1.0"

// 2. export 已声明的标识符（用于内部先声明、最后统一暴露）
fn _helper() -> string { return "..." }
fn publicFn() -> string { return _helper() }
export publicFn

// 3. 重导出（带可选重命名）
export { getUser, getUserAge as getAge } from "./user"

// 4. 通配重导出（把另一个模块的全部 export 转出）
export * from "./product"
```

- 未标 `export` 的声明仅模块内可见（**私有**）。
- 模块的内部状态（`let _x`, `const _VERSION`, `fn _helper`）在不同模块中互不冲突，即使同名。
- 重导出与通配重导出常用于 `index.xr` 聚合子模块的公开 API。

### 11.5 命名约定

- 模块名 `snake_case`：`http_client.xr` / `string_utils.xr`。
- 公开符号 `camelCase` 或 `PascalCase`（类/接口）。
- 内部符号约定前缀 `_`：`_internal_helper`。

### 11.6 循环依赖

xray **禁止**循环依赖。模块加载时建立 DAG；检测到循环 → 编译错误。

### 11.7 native 模块

C 层暴露的模块（如 `time`、`http`、`os`）通过 native ABI 注册：

```c
// C 端
XRAY_API void register_time_module(xray_vm_t* vm) {
    xray_module_t* m = xray_module_create(vm, "time");
    xray_module_add_fn(m, "now", time_now);
    xray_module_add_fn(m, "sleep", time_sleep);
    xray_module_register(vm, m);
}
```

xray 端用法相同：

```xray
import time
let t = time.now()
time.sleep(100)
```

详见 `docs/rules/architecture.md` 的"native 模块注册"章节。

---

## 12. 测试系统 (Testing)

> 真值源：`src/app/cli/xcli_test.c`、`stdlib/xray/test.xr`、`docs/testing-spec.md`。

### 12.1 测试声明：`@test` 注解

xray 用 **`@test` 注解**标注测试函数，**不**通过 `test("...")` 函数调用形式。

```ebnf
TestDecl ::= '@test' FnDecl
```

```xray
@test
fn test_addition() {
    assert_eq(1 + 1, 2)
}

@test
fn test_with_assertions() {
    let result = compute()
    assert_eq(result, 42)
    assert(result > 0)
}
```

**语义**：
- `@test` 标注的函数会被 `xray test` 自动发现并运行；普通函数不会。
- 测试函数命名约定：`test_xxx`（snake_case），描述性命名。
- 测试函数无参数无返回值；通过 assert 系列函数表达预期。
- 同一文件可包含**任意数量**的 `@test` 函数；它们按声明顺序运行。

### 12.2 测试入口

测试文件约定：
- 与被测代码同目录或 `tests/regression/` 目录下。
- 文件名形如 `XXXX_topic.xr`（四位数字编号 + 主题）。

运行：

```bash
xray test                                  # 运行所有测试
xray test tests/regression/01_literals/    # 整个分组
xray test tests/regression/01_literals/0100_int_basic.xr   # 单文件
```

### 12.3 断言 API

xray 把断言函数作为**全局内置**（不需 `import test`）。完整签名见 [§13.5](#135-断言测试用)。

| 函数 | 语义 |
|--|--|
| `assert(cond, msg?)` | `cond` 为 false 时抛异常 |
| `assert_eq(a, b)` | `a == b` 失败时输出两值 |
| `assert_ne(a, b)` | `a != b` |
| `assert_true(cond)` / `assert_false(cond)` | 等价 `assert(cond)` / `assert(!cond)` |
| `assert_throws(fn)` | 期望 `fn()` 抛异常 |

> **命名一致性**：所有断言函数为 `snake_case`（`assert_eq`，不是 `assertEq`）。

### 12.4 异步测试

`@test` 函数体内可使用 `go` / `await` / `await all` / `await any`：

```xray
@test
fn test_async_fetch() {
    let task = go fetch_data("http://...")
    let result = await task
    assert_eq(result.status, 200)
}
```

### 12.5 注解（Attributes）总览

xray 的注解前缀为 `@`，紧接标识符。当前 parser 仅识别**三种**注解（源码：`xparse_decl.c:xr_parse_single_attribute`）：

| 注解 | 适用 | 说明 |
|---|---|---|
| `@test` | 函数 | 标记为测试函数；接受可选参数：`@test(skip)` 跳过、`@test(timeout: 30)` 超时设置 |
| `@native` | class / struct / fn | 声明 native 实现，方法体由 C 提供；用于 stdlib 类型声明 |
| `@deprecated` | 任意声明 | 弃用警告；可选消息：`@deprecated("use X instead")` |

```xray
@test                                 // 标记测试
fn test_basic() { return }

@test(skip)                           // 跳过此测试
fn test_wip() { return }

@native                               // C 实现
class Array<T> {
    length: int
    push(v: T)
    // 无方法体——由 src/runtime/object/xarray_methods.c 提供
}

@deprecated("use newAPI() instead")
fn oldAPI() { return }
```

> 不存在的注解（用户代码不要使用）：`@before_each` / `@after_all` / `@async` / `@override` 等——这些会触发"unknown attribute name"错误。

### 12.6 `xray run` / `xray test` / `xray repl`

| 命令 | 用途 |
|--|--|
| `xray run main.xr` | 执行主程序 |
| `xray test` | 运行测试套件 |
| `xray repl` | 启动 REPL |
| `xray build --aot` | AOT 编译 |
| `xray fmt` | 格式化 |

---

## 13. 内置函数 (Built-in Functions)

> 真值源：`src/ir/xi_lower_expr.c`、`src/vm/xvm_dispatch_*.inc.c`、`src/runtime/object/builtins/`、`src/frontend/analyzer/xanalyzer_builtins.c`。

不需要 `import` 即可使用的全局函数和内置构造/静态函数。下列表格中的 `value` 表示“任意运行时值”，不是一个可写的 `any` 类型；Xray 源码中已没有 `any` 类型。

### 13.1 I/O 与调试

| 函数 | 签名 | 说明 |
|--|--|--|
| `print` | `(...values) -> ()` | 输出到 stdout，自动追加换行；多参以空格分隔 |
| `dump` | `(value, indent?) -> ()` | 结构化调试输出 |

### 13.2 类型转换

| 函数 | 签名 | 说明 |
|--|--|--|
| `int(x)` | `(value) -> int` | 转为 int；字符串解析失败抛异常 |
| `float(x)` | `(value) -> float` | 转为 float |
| `string(x)` | `(value) -> string` | 转为字符串 |
| `bool(x)` | `(value) -> bool` | 转为 bool；规则见 §2.4.1 |
| `chr(n)` | `(int) -> string` | Unicode 码点转单字符字符串 |
| `copy(x)` | `(T) -> T` | 深拷贝，保留运行时类型 |

### 13.3 类型检查

| 函数 / 表达式 | 签名 | 说明 |
|---|---|---|
| `typeof(x)` | `(value) -> string` | 返回运行时类型名字符串 |
| `x is T` | 表达式 | 运行时类型检查，分析器可做类型窄化 |

```xray
let x = 42
print(typeof(x))                // "int"
print(x is int)                 // true
print(typeof(x) == "int")       // true
```

### 13.4 协程

协程启动和等待是语法而不是全局函数：`go`、`await`、`await all`、`await any`、`await anySuccess`。休眠使用 `time.sleep(ms)`。

### 13.5 断言（测试用）

| 函数 | 签名 | 说明 |
|---|---|---|
| `assert(cond, msg?)` | `(bool, string?) -> ()` | `cond` 为 false 时抛异常 |
| `assert_true(cond)` | `(bool) -> ()` | 等价 `assert(cond)` |
| `assert_false(cond)` | `(bool) -> ()` | 等价 `assert(!cond)` |
| `assert_eq(a, b)` | `(T, T) -> ()` | 深相等断言 |
| `assert_ne(a, b)` | `(T, T) -> ()` | 深不等断言 |
| `assert_throws(fn)` | `(fn) -> ()` | 期望函数抛异常 |

### 13.6 容器构造与静态函数

| 函数 | 说明 |
|--|--|
| `Array()` / `Array(n)` / `Array(n, value)` | 创建空数组、指定长度数组或填充值数组 |
| `Array.from(iterable)` | 从 string / Array / Set / Map 创建数组 |
| `Array.range(start, end)` | 创建闭区间整数数组 `[start, ..., end]` |
| `Array.withCapacity(n)` | 创建 length=0、capacity=n 的数组 |
| `Map()` | 创建空 Map |
| `Map.from(entries)` | 从 `[key, value]` pair 数组创建 Map |
| `Map.from(keys, values)` | 从键数组和值数组创建 Map |
| `Set()` / `Set(array)` | 创建空 Set 或从数组创建 Set |
| `Set.from(iterable)` | 从 string / Array / Set 创建 Set |
| `Set.range(start, end)` | 创建闭区间整数 Set |

BigInt 使用 `123n` 字面量或 `int.toBigInt()`；Json 使用 `Json.parse` / `Json.stringify`；DateTime 使用 `datetime` 模块工厂函数。

---

## 14. 内置类型方法 (Built-in Type Methods)

> 真值源：prelude / analyzer / runtime 中的内置类型注册与方法定义。
> MCP knowledge 只消费生成后的 analyzer metadata，不独立维护内置类型方法签名。

本节给出每种类型的**方法索引**（按主题分组）。具体签名、参数说明、行为细节以实现代码为准。

### 14.1 `int` 方法

| 方法 | 签名 | 说明 |
|--|--|--|
| `abs()` | `() -> int` | 绝对值 |
| `toString()` | `() -> string` | 十进制字符串 |
| `toBigInt()` | `() -> BigInt` | 转 BigInt |
| `toFloat()` | `() -> float` | 转 float |
| `toHex()` | `() -> string` | 十六进制字符串 |
| `max(other)` / `min(other)` | `(int) -> int` | 双值最值 |
| `floor()` / `ceil()` / `round()` | `() -> int` | 对 int 返回自身 |
| `sqrt()` | `() -> float` | 平方根 |
| `pow(exp)` | `(float) -> float` | 幂运算 |

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

### 14.5 `string` 方法

| 成员 | 类型 / 说明 |
|--|--|
| `length` | 字符串长度属性 |
| `charAt(i)` | 返回指定位置字符 |
| `charCodeAt(i)` | 返回码点 |
| `concat(...others)` | 拼接字符串 |
| `includes(s)` | 是否包含子串 |
| `indexOf(s)` / `lastIndexOf(s)` | 查找子串 |
| `slice(start, end?)` / `substring(start, end?)` / `substr(start, len?)` | 子串 |
| `toLowerCase()` / `toUpperCase()` | 大小写转换 |
| `trim()` / `trimStart()` / `trimEnd()` | 去空白 |
| `split(sep, limit?)` | 分割为 `Array<string>` |
| `replace(from, to)` / `replaceAll(from, to)` | 替换 |
| `repeat(n)` | 重复 |
| `startsWith(s)` / `endsWith(s)` | 前缀/后缀判断 |
| `padStart(len, pad?)` / `padEnd(len, pad?)` | 填充 |
| `match(pattern)` | 正则匹配 |
| `iterator()` / `entriesIterator()` / `entries()` | 迭代协议 |

### 14.6 `Bytes`

`Bytes` 是 prelude 类型，构造由 `Bytes(n)` / `Bytes(n, fill)` 等内置路径处理。字符串转换和编码类操作优先使用 `encoding` / `base64` 模块。当前没有单独的 `stdlib/types/bytes.xr` 声明；工具不要假设存在完整 Array 同构 API。

### 14.7 `Array<T>` 方法

| 成员 | 类型/说明 |
|--|--|
| `length` | `int` 属性 |
| `arr[i]` / `arr[i] = v` | 下标读写 |
| `push(x)` / `pop()` | 尾部增删 |
| `shift()` / `unshift(x)` | 头部增删 |
| `slice(start?, end?)` | 切片 |
| `splice(start, deleteCount, ...items)` | 原地增删 |
| `concat(...arrays)` | 拼接 |
| `indexOf(x)` / `includes(x)` | 查找 |
| `join(sep?)` | 拼接为字符串 |
| `reverse()` / `sort(cmp?)` | 原地重排 |
| `map(fn)` / `filter(fn)` / `reduce(fn, init)` | 函数式处理 |
| `forEach(fn)` / `find(fn)` / `findIndex(fn)` / `every(fn)` / `some(fn)` | 遍历与谓词 |
| `flat(depth?)` / `fill(v, start?, end?)` / `copyWithin(target, start, end?)` | 数组工具 |
| `iterator()` / `entriesIterator()` / `entries()` | 迭代协议 |

### 14.8 `Map<K, V>` 方法

| 成员 | 类型/说明 |
|--|--|
| `length` | `int` 属性 |
| `m[k]` / `m[k] = v` | 下标读写 |
| `get(k)` / `set(k, v)` | 读取/写入 |
| `has(k)` / `delete(k)` / `clear()` | 查询与删除 |
| `keys()` / `values()` / `entries()` | 返回键、值、键值对 |
| `forEach(fn)` | 遍历 |
| `iterator()` / `entriesIterator()` | 迭代协议 |

**Map 字面量**：`#{"k1": v1, "k2": v2}` 或 `#{}`；使用 `:`，靠 `#` 前缀区别于 Object/Json 字面量。

### 14.9 `Set<T>` 方法

| 成员 | 类型/说明 |
|--|--|
| `length` | `int` 属性 |
| `add(x)` / `has(x)` / `delete(x)` | 插入、查询、删除 |
| `clear()` | 清空 |
| `values()` | 返回 `Array<T>` |
| `forEach(fn)` | 遍历 |
| `iterator()` | 迭代协议 |

**Set 字面量**：`#[1, 2, 3]` 或 `#[]`。

### 14.10 `Channel<T>` 方法

| 成员 | 类型/说明 |
|--|--|
| `send(v)` | 阻塞发送；channel 已关闭时抛异常 |
| `recv()` | 阻塞接收；关闭且缓冲为空时返回 `null` |
| `trySend(v)` | 非阻塞发送，返回 bool |
| `tryRecv()` | 非阻塞接收，返回 `(T, bool)` |
| `sendTimeout(v, ms)` | 带超时发送，超时或关闭返回 false |
| `recvTimeout(ms)` | 带超时接收，返回 `(T, bool)` |
| `close()` | 关闭 channel |
| `isClosed` / `isClosed()` | 关闭状态；运行时属性和方法均支持 |

> `stdlib/types/channel.xr` 仍声明 `closed` 属性，但运行时符号表和 VM 分派使用 `isClosed`；该声明漂移已记录为已知问题。

### 14.11 `Json`

`Json` 是动态结构化数据类型。普通字段访问使用 `j.field` / `j["field"]`；通用查询和编解码通过 `Json` 静态函数完成，避免与用户字段名冲突。

| 静态函数 | 说明 |
|--|--|
| `Json.keys(obj)` / `Json.values(obj)` / `Json.entries(obj)` | Object 字段枚举 |
| `Json.has(obj, key)` | 字段存在性 |
| `Json.get(obj, key, default?)` | 字段读取，不存在返回 default 或 null |
| `Json.size(obj)` | 字段数量 |
| `Json.isEmpty(obj)` | 是否为空 |
| `Json.parse(s)` / `Json.tryParse(s)` / `Json.isValid(s)` | JSON 解析与校验 |
| `Json.stringify(value, indent?)` | 序列化 |

**字面量**：`{ name: "alice", age: 30 }`，动态类型为 `Json`。如需 sealed 对象，用 `type T = { name: string, age: int }` 标注。

### 14.12 `Range`

`a..b` 是半开区间 `[a, b)`，用于表达式和 `for-in`。常见成员为 `start`、`end`、`length`、`includes(x)`、`toArray()`、`toString()`。

### 14.13 `DateTime`

通过 `import datetime` 获得工厂函数：`now`、`utc`、`create`、`createUTC`、`fromTimestamp`、`fromTimestampMs`、`parse`、`offset`。`DateTime` 实例由 prelude 注册，无需 import 类型名。

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
| `replace(s, replacement)` | 替换 |
| `split(s)` | 分割 |

### 14.15 `StringBuilder`

| 方法 | 说明 |
|--|--|
| `length` | 当前长度属性 |
| `append(s)` | 追加并返回自身 |
| `toString()` | 输出字符串 |
| `clear()` | 清空并返回自身 |

### 14.16 `Exception`

内置 `Exception` 类包含 `message`、`stack`、`cause`、`code`、`data` 字段，构造函数 `constructor(message: string = "", cause: Exception? = null)`，以及 `toString()`。

### 14.17 `Task<T>` / `EnumValue` / `EnumType`

`Task<T>` 属性：`done`、`cancelled`、`result`、`error`；方法：`cancel()`。`EnumValue` 属性：`name`、`value`、`ordinal`，方法：`toString()`。`EnumType` 属性：`name`、`memberCount`，方法：`getMember(name)`。

### 14.18 其他 prelude 类型（`Logger` / `NetConn` / `NetListener`）

这些类型由 prelude 注册，实例由 `log` / `net` 等模块工厂函数构造。完整运行时能力以对应 stdlib 模块为准。

---

## 15. 标准库概览 (Standard Library)

> 真值源：标准库实现与 analyzer builtin metadata。
> MCP knowledge 通过 `xray builtin-dump` 获取 API 签名并在生成时注入模块知识卡片。
> 详见 [附录 D stdlib 模块索引](#d-stdlib-模块索引)。

> **真实 native 模块清单**（22 个，源码：`stdlib/<module>/*.c`）：
>
> `base64`、`cluster`、`compress`、`crypto`、`csv`、`datetime`、`encoding`、`gc`、`http`、`io`、`log`、`math`、`net`、`os`、`path`、`regex`、`time`、`toml`、`url`、`ws`、`xml`、`yaml`。
>
> 不需要 import 的内置类型由 prelude 注册（`Array` `Map` `Set` `Json` `Channel` `Bytes` `BigInt` `StringBuilder` `Exception` `Regex` `Logger` `NetConn` `NetListener` 等）；`Result<T, E>` 是错误处理路径使用的内置 ADT enum。详见 §1.5.6 / §2.2。

### 15.1 文件 IO 与系统

| 模块 | 主题 | 关键 API |
|--|--|--|
| `io` | 文件 IO + 文件系统 | `readFile` `writeFile` `exists` `mkdir` `remove` `readdir` `stat` `stdin` `stdout` `stderr` |
| `path` | 路径操作 | `join` `dirname` `basename` `extname` `normalize` `isAbsolute` `resolve` `relative` `parse` `format` |
| `os` | 操作系统接口 | `getenv` `setenv` `environ` `exit` `getpid` `getcwd` `chdir` `hostname` `tmpdir` `homedir` `cpuCount` `sleep` `exec`；常量 `platform` `arch` `sep` `eol` |

> xray **没有**独立的 `fs` 模块，文件系统操作在 `io` 中；进程参数 / 进程信息走全局 `process` 对象（`process.args` / `process.file` / `process.dir`，见 §16.5），不在 `os` 中。
> `os.platform` / `os.arch` / `os.sep` / `os.eol` 是**常量字符串**，不带括号；其余 `os.*` 是函数调用。

### 15.2 网络

| 模块 | 主题 | 关键 API |
|--|--|--|
| `net` | TCP / UDP / TLS socket + DNS | `listen` `dial` `lookup` `Socket` `Listener` `hasTLS` |
| `http` | HTTP / HTTPS 客户端 + 服务端 + HTTP/2 | `get` `post` `request` `Server` `urlEncode` `urlDecode` |
| `ws` | WebSocket | 客户端/服务端连接 |
| `url` | URL 解析与构造 | `parse` `format` `parseQuery` `buildQuery` `encode` `decode` |

> DNS 查询通过 `net.lookup(host)` 完成；没有独立的 `dns` 模块。

### 15.3 数据格式

| 模块 | 主题 |
|--|--|
| `yaml` | YAML |
| `toml` | TOML |
| `xml` | XML |
| `csv` | CSV |
| `base64` | Base64 编/解 |
| `encoding` | hex / UTF-8 等通用编码（不含 Base64，base64 在自身模块） |

> JSON 编解码**不在**单独的 `json` 模块；通过内置类型 `Json` 的静态方法 `Json.parse(s)` / `Json.stringify(v)` 使用（无需 import；见 §14.10）。

### 15.4 加密与哈希

| 模块 | 关键 API |
|--|--|
| `crypto` | `md5` `sha1` `sha256` `sha512` `hmac` `aes` `rsa` 等；详细 API 详见 stdlib 源码 |

> stdlib **没有**独立的 `random` 模块；如需伪随机数请使用 `crypto` 模块的随机源或 `math` 模块的工具函数。

### 15.5 压缩

| 模块 | 关键 API |
|--|--|
| `compress` | `gzip` / `gunzip`、`deflate` / `inflate` 等 |

### 15.6 时间

| 模块 | 关键 API |
|--|--|
| `time` | `now()` `monotonic()` `sleep(ms)` `Duration` |
| `datetime` | `DateTime` / `Date` / `Time` 解析、格式化（详见 §14.12） |

### 15.7 数学

| 模块 | 关键 API |
|--|--|
| `math` | `sin` `cos` `tan` `log` `pow` `sqrt` `floor` `ceil` `round` `abs` `min` `max` 等；常量 `PI` / `E` / `MAX_INT` / `MIN_INT` |

### 15.8 文本

| 模块 | 关键 API |
|--|--|
| `regex` | `compile(pattern)` 返回 `Regex`；详见 §14.13。也支持 `/pattern/flags` 字面量 |

> stdlib **没有** `strconv` 模块；字符串 ↔ 数值转换使用内置函数 `int(s)` / `float(s)` / `string(n)`（见 §13.2）。

### 15.9 日志与诊断

| 模块 | 关键 API |
|--|--|
| `log` | `debug` / `info` / `warn` / `error` / `fatal` / `child()`、source 位置开关、异步写入模式 |
| `gc` | `collect()` `isrunning()` `count()` `state()` `stats()` |

### 15.10 分布式

| 模块 | 主题 |
|--|--|
| `cluster` | 节点发现、健康检查、Topic 消息总线（见 stdlib/cluster/）|

### 15.11 测试

`@test` 注解 + 全局 `assert*` 函数即可，**不需要**额外的 `test` 模块（见 §12）。

### 15.12 已**不存在**的模块

文档中可能引用过、但当前 stdlib 中**确实没有**的模块（避免误导）：

`fs` · `process` · `dns` · `random` · `strconv` · `sync` · `runtime` · `json`

这些功能或者归入其他模块（见上面各小节注），或者尚未实现。

> **完整索引**：见[附录 D](#d-stdlib-模块索引)。

---

## 16. 运行时模型 (Runtime Model)

> 真值源：`src/runtime/`、`src/vm/`、`docs/rules/gc-memory.md`、`docs/rules/architecture.md`。

### 16.1 值表示

xray 值统一用 `xray_value_t` 表示。布局策略：

- **NaN-boxing**（在 64 位平台）：double 编码用未使用的 NaN 表示空间存放小整数、bool、指针标记。
- **指针标记**：低位 tag 区分对象类型。
- **对象引用**：堆对象通过 tagged pointer 引用；当前 GC 不移动对象。

| 值类型 | 内部表示 |
|--|--|
| `int` | 53-bit immediate（NaN-box） |
| `float` | 双精度直接存放 |
| `bool` | tag |
| `null` | 单一全局值 |
| `string` | 堆对象 + 短字符串内联（≤ 7 字节） |
| `Bytes` | 堆对象 + capacity/length |
| 其他对象 | 堆指针 |

### 16.2 内存分配

| 区域 | 用途 |
|--|--|
| **系统堆** | C `malloc/free`，用于 native 数据结构 |
| **全局堆** | `shared const` / `shared let`，refcount GC |
| **协程堆** | 每协程独立的 Mark-Sweep GC 堆 |
| **栈** | `struct` 值、局部 immediate、函数帧 |
| **Arena** | parser 临时分配、frame allocation |

### 16.3 GC 模型

- 默认 **per-coroutine Mark-Sweep / Immix Mark-Region**。
- **三色标记**：white（未访问）/ gray（待扫）/ black（已扫）。
- **写屏障**：对象字段、模块字段、静态字段、容器写入 GC 值时触发；Sticky Immix 使用后向屏障维护 young/old 关系。
- **GC-safepoint**：函数调用、后向跳转、显式 `gc.collect()`。

详见 `docs/rules/gc-memory.md`。

### 16.4 协程调度

- M:N 调度（M OS 线程 × N 协程）。
- **work-stealing**：空闲 worker 从其他 worker 队列偷任务。
- **协作式抢占**：协程在 safepoint 让出（非强制抢占）。
- **栈管理**：segmented stack 按需扩展。

详见 `src/runtime/coro/`。

### 16.5 进程级全局访问

- `process`（全局内置，无需 import）：进程自身信息。
- `os`（需 `import os`）：操作系统、环境、进程控制。

```xray
// 进程自身信息 — 全局对象
process.file              // 当前脚本路径（与 __file__ 等价）
process.args              // Array<string>，进程命令行参数
process.dir               // 脚本所在目录（与 __dir__ 等价）

// OS / 环境 — 需 import
import os
os.getenv("PATH")         // 读取环境变量 -> string?
os.environ()              // 获取全部环境变量 -> Map<string, string>
os.exit(0)                // 退出进程
os.getpid()               // 进程 ID
os.getcwd()               // 当前工作目录
os.hostname()             // 主机名
os.tmpdir()               // 临时目录
os.platform               // 常量："darwin" / "linux" / "windows"
os.arch                   // 常量："arm64" / "x64" / "x86"
os.sep                    // 常量：路径分隔符
os.eol                    // 常量：行尾
os.sleep(100)             // 休眠毫秒数（与 `time.sleep` 等价）
```

> **命名约定**：`os.*` 以 POSIX 函数名为主（`getenv` / `getcwd` / `getpid`）；不随 Node.js。Node 风格的 `process.env` 映射不提供，请用 `os.getenv(name)` / `os.environ()`。

详见 `stdlib/os/`。

### 16.6 异常运行时

内置 `Exception` 类是 prelude 类型（声明：`stdlib/types/exception.xr`），用户可直接 `new` 或继承：

```xray
@native
class Exception {
    message: string             // 人类可读消息
    stack: Array<string>        // 自动 capture 的调用栈，每帧一行格式化字符串
    cause: Exception?           // 链式 cause
    code: int                   // 错误码（从 "E0xxx: ..." 前缀自动解析，默认 0）
    data: Json?                 // throw 非异常值时原始值被包装在此

    constructor(message: string = "", cause: Exception? = null)
    fn toString() -> string
}
```

`throw` 表达式的操作数静态类型必须是 `Exception` 或其派生类（见 §8.1.1）；其它类型在编译期被拒绝（错误码 `E0370`）。VM 抛出的运行时错误也使用此 `Exception` 类型。

栈展开：VM `xvm_unwind_stack()` 按 try-table 查找 catch handler，逐帧释放局部、执行 finally / defer，到达 catch 后跳转。详见 §8。

### 16.7 Result 运行时

`Result<T, E>` 是 prelude 的 ADT enum（见 §8.2 / §5.6.2）。运行时表示：tag (1 字节) + payload。`Result.Ok(v)` 与 `Result.Err(e)` 是值对象，`match` 解构在 IR 层 lower 为 tag-test + payload-extract。

由于 `Result` 在异常零路径上无成本（不抛、不展开栈），使用 Result 的代码路径性能等同于 tagged-union。

---

## 17. 编译流水线 (Compilation Pipeline)

> 真值源：`src/frontend/`、`src/vm/`、`src/jit/`、`src/aot/`、`docs/rules/architecture.md`。

### 17.1 阶段总览

```
源码 (.xr)
    ↓ lexer
Token Stream
    ↓ parser
AST
    ↓ analyzer (语义分析、类型检查、scope/capture/generic)
Typed AST
    ↓ ssa-gen
SSA IR
    ↓ optimize（const fold、DCE、inline、TCO、escape analysis）
Optimized SSA
    ↓ codegen
Bytecode  →  AOT (machine code)
    ↓ VM
    ↓ Profiler → JIT (machine code)
执行
```

### 17.2 词法分析 (Lexer)

- 真值源：`src/frontend/lexer/xlexer.c`。
- 输出 `XrToken` 流，每个 token 含 `kind`、`value`、`pos(line, col)`。
- 处理：字符串插值（产生 `${...}` 拼接序列）、原始字符串、正则字面量。

### 17.3 语法分析 (Parser)

- 真值源：`src/frontend/parser/`（分文件：expr、stmt、decl、match）。
- 风格：手写 Pratt parser（表达式）+ 递归下降（声明 / 语句）。
- 错误恢复：遇到错误后跳到下一同步点（`;` `}` `)`），尽量继续解析。
- 输出：`XrAstNode*` 根（即 module）。

### 17.4 语义分析 (Analyzer)

- 真值源：`src/frontend/analyzer/xanalyzer_*.c`（按主题拆分）。
- **作用域**：嵌套符号表、变量解析、shadowing 检查。
- **类型检查**：双向类型推断、union 收窄、Json 结构匹配。
- **泛型**：monomorphization、约束检查、调用点重写。
- **闭包分析**：upvalue 标记、`go` 闭包捕获禁令。
- **错误码**：`XR_ERR_ANALYZE_*` 系列。

### 17.5 SSA 与优化

- 真值源：`src/ir/xi_opt*.c`、`src/ir/xi_pass.h`、`src/jit/`。
- **常量折叠**：编译期求值。
- **DCE**（dead-code elimination）：删除未使用代码。
- **inlining**：小函数内联。
- **TCO**（tail-call optimization）：accumulator 风格尾递归转循环。
- **escape analysis**：栈分配 vs 堆分配决策。

### 17.6 字节码与 VM

- 真值源：`src/vm/`、`include/xray_opcodes.h`。
- 寄存器栈混合 VM。
- IC（inline cache）加速属性访问与方法分派。

### 17.7 JIT 与 AOT

- **JIT**（运行时）：热函数被 profiler 选中后 → 编译为本地机器码。源码：`src/jit/`。
- **AOT**（提前）：`xray build --aot` → 整个模块编译为 native binary。源码：`src/aot/`。
- 共享 SSA IR；后端选择不同（解释 / JIT / AOT）。

---

## 18. 错误码参考 (Error Code Reference)

> 真值源：`src/runtime/xerror_codes.h`、`src/runtime/xerror.h`。

> xray 有**两套错误码系统**：
>
> - 数值码（`xerror_codes.h` 中的 `#define`）：lexer / parser / VM 运行时使用，按区间分布。
> - 枚举码（`xerror.h` 中的 `XrErrorCode` 枚举）：分析器（type/binding/closure）使用，按区间分布。
>
> 下表列出**主要**错误码；详细的全列表与触发条件以源码为准。错误抛出时携带的 `error.name` 字段与下表"名称"列对应。

### 错误码分类（数值码）

| 范围 | 类别 |
|--|--|
| `E0101`-`E0199` | 词法错误 (Lexer) |
| `E0201`-`E0299` | 语法错误 (Syntax) |
| `E0301`-`E0399` | 编译错误 (Compile) |
| `E0401`-`E0499` | 运行时错误 (Runtime) |
| `E0501`-`E0599` | 模块错误 (Module) |
| `E0801`-`E0899` | 禁止写法 (Rejected Syntax) |

### 18.1 词法错误

| 码 | 名称 | 描述 |
|--|--|--|
| `E0101` | `XR_ERR_LEX_INVALID_CHAR` | 非法字符 |
| `E0102` | `XR_ERR_LEX_UNTERMINATED_STR` | 字符串未闭合 |
| `E0103` | `XR_ERR_LEX_INVALID_NUMBER` | 数字字面量格式错误 |
| `E0104` | `XR_ERR_LEX_INVALID_ESCAPE` | 非法转义序列 |

### 18.2 语法错误 (Syntax)

| 码 | 名称 | 描述 |
|--|--|--|
| `E0201` | `XR_ERR_SYN_UNEXPECTED_TOKEN` | 未预期的 token |
| `E0202` | `XR_ERR_SYN_EXPECTED_EXPR` | 缺少表达式 |
| `E0203` | `XR_ERR_SYN_EXPECTED_STMT` | 缺少语句 |
| `E0204` | `XR_ERR_SYN_UNCLOSED_PAREN` | 未闭合 `(` |
| `E0205` | `XR_ERR_SYN_UNCLOSED_BRACE` | 未闭合 `{` |
| `E0206` | `XR_ERR_SYN_UNCLOSED_BRACKET` | 未闭合 `[` |
| `E0207` | `XR_ERR_SYN_INVALID_ASSIGN` | 非法赋值目标（如赋值给字面量） |

### 18.3 编译期 / 名字解析错误

数值码（基础）：

| 码 | 名称 | 描述 |
|--|--|--|
| `E0301` | `XR_ERR_CMP_UNDEFINED_VAR` | 未定义名字 |
| `E0302` | `XR_ERR_CMP_REDEFINED_VAR` | 重复声明 |
| `E0303` | `XR_ERR_CMP_CONST_ASSIGN` | 赋值给 `const` |
| `E0304` | `XR_ERR_CMP_INVALID_BREAK` | `break` 不在循环内 |
| `E0305` | `XR_ERR_CMP_INVALID_CONTINUE` | `continue` 不在循环内 |
| `E0306` | `XR_ERR_CMP_INVALID_RETURN` | `return` 不在函数内 |
| `E0307` | `XR_ERR_CMP_TOO_MANY_PARAMS` | 参数数量超过限制 |
| `E0308` | `XR_ERR_CMP_TOO_MANY_LOCALS` | 局部变量数量超过限制 |

分析器枚举码（`XrErrorCode`，定义在 `xerror.h` 350+ 段）：

| 枚举名 | 描述 |
|--|--|
| `XR_ERR_ANALYZE_UNDEFINED_VAR` | 未声明变量 |
| `XR_ERR_ANALYZE_TYPE_MISMATCH` | 类型不可赋值 |
| `XR_ERR_ANALYZE_CONST_ASSIGN` | 不能给 `const` 赋值 |
| `XR_ERR_ANALYZE_NOT_CALLABLE` | 值不可调用 |
| `XR_ERR_ANALYZE_WRONG_ARG_COUNT` | 参数数量不匹配 |
| `XR_ERR_ANALYZE_ARG_TYPE` | 参数类型不匹配 |
| `XR_ERR_ANALYZE_GENERIC_COUNT` | 类型参数数量错误 |
| `XR_ERR_ANALYZE_GENERIC_CONSTRAINT` | 类型实参不满足约束 |
| `XR_ERR_ANALYZE_SUPER_FIRST` | 派生类构造器首行不是 `super(...)` |
| `XR_ERR_ANALYZE_SUPER_THIS` | `super(...)` 之前访问 `this` |
| `XR_ERR_ANALYZE_SUPER_REQUIRED` | 派生类未调 `super()` |
| `XR_ERR_ANALYZE_SUPER_INVALID` | 非派生类使用 `super()` |
| `XR_ERR_ANALYZE_CLOSURE_CAPTURE` | 协程闭包捕获了不安全变量 |
| `XR_ERR_ANALYZE_AWAIT_TYPE` | `await` 操作数不是 `Task` |
| `XR_ERR_ANALYZE_MISSING_TYPE` | 变量需要类型注解或初始化器 |
| `XR_ERR_ANALYZE_ENUM_MIXED_TYPE` | enum 成员 backing type 混合 |
| `XR_ERR_ANALYZE_INTERFACE_NOT_IMPLEMENTED` | 类未实现声明的接口 |
| `XR_ERR_ANALYZE_TUPLE_FIELD_NAME` | 用非数字 key 访问 tuple |
| `XR_ERR_ANALYZE_TUPLE_FIELD_RANGE` | tuple 字段下标越界 |

### 18.4 运行时错误 (Runtime)

#### 类型与方法 (E040x-E041x)

| 码 | 名称 | 描述 |
|--|--|--|
| `E0401` | `XR_ERR_TYPE_NO_PROPERTY` | 类型上不存在该属性 |
| `E0402` | `XR_ERR_TYPE_NO_INDEX` | 类型不可索引 |
| `E0403` | `XR_ERR_TYPE_NO_CALL` | 值不可调用 |
| `E0404` | `XR_ERR_TYPE_MISMATCH` | 类型不匹配 |
| `E0405` | `XR_ERR_TYPE_NO_METHOD` | 类型上不存在该方法 |
| `E0406` | `XR_ERR_TYPE_NO_OPERATOR` | 类型不支持该运算符 |

#### Null 相关 (E041x)

| 码 | 名称 | 描述 |
|--|--|--|
| `E0410` | `XR_ERR_NULL_PROPERTY` | 对 null 访问属性 |
| `E0411` | `XR_ERR_NULL_INDEX` | 对 null 索引 |
| `E0412` | `XR_ERR_NULL_CALL` | 对 null 调用 |

#### 算术 (E042x)

| 码 | 名称 | 描述 |
|--|--|--|
| `E0420` | `XR_ERR_DIV_BY_ZERO` | 整数除零 |
| `E0421` | `XR_ERR_MOD_BY_ZERO` | 整数求模零 |
| `E0422` | `XR_ERR_OVERFLOW` | 整数溢出 |

#### 索引/键 (E043x)

| 码 | 名称 | 描述 |
|--|--|--|
| `E0430` | `XR_ERR_INDEX_OUT_OF_BOUNDS` | 数组 / 字符串 / Bytes 越界 |
| `E0431` | `XR_ERR_KEY_NOT_FOUND` | Map 键不存在 |

#### 内存与栈 (E044x)

| 码 | 名称 | 描述 |
|--|--|--|
| `E0440` | `XR_ERR_STACK_OVERFLOW` | 栈溢出 |
| `E0441` | `XR_ERR_OUT_OF_MEMORY` | 内存不足 |

#### 调用参数 (E045x)

| 码 | 名称 | 描述 |
|--|--|--|
| `E0450` | `XR_ERR_WRONG_ARG_COUNT` | 实参数量不匹配 |
| `E0451` | `XR_ERR_INVALID_ARG_TYPE` | 实参类型不匹配 |

#### 协程 (E046x)

| 码 | 名称 | 描述 |
|--|--|--|
| `E0460` | `XR_ERR_CORO_DEAD` | 在已死的协程上操作 |
| `E0461` | `XR_ERR_CORO_CANCELLED` | 协程被取消 |

### 18.5 模块错误 (Module)

| 码 | 名称 | 描述 |
|--|--|--|
| `E0501` | `XR_ERR_MOD_NOT_FOUND` | 找不到模块 |
| `E0502` | `XR_ERR_MOD_LOAD_FAILED` | 模块加载失败（IO / 解析错误） |
| `E0503` | `XR_ERR_MOD_NO_EXPORT` | import 的名字未被 export |
| `E0504` | `XR_ERR_MOD_CIRCULAR` | 循环依赖 |

### 18.6 禁止写法 (Rejected Syntax)

> parser 遇到下列写法时直接报错，并给出正确替代方案。

| 码 | 名称 | 禁止写法 | 正确写法 |
|--|--|--|--|
| `E0801` | `XR_ERR_SYN_RETURN_MULTI_REMOVED` | `return a, b` | `return (a, b)` |
| `E0802` | `XR_ERR_SYN_LET_MULTI_REMOVED` | `let x, y = ...` | `let (x, y) = ...` |
| `E0803` | `XR_ERR_SYN_FOR_FLAT_REMOVED` | `for k, v in m`（裸 KV） | `for (k, v in m)` |
| `E0804` | `XR_ERR_SYN_VOID_REMOVED` | `-> void` | `-> ()` 或省略返回类型 |

### 18.7 错误处理与 Result (E082x)

| 码 | 名称 | 描述 |
|--|--|--|
| `E0820` | `XR_ERR_THROW_NOT_EXCEPTION` | 已合并到 `E0370`（见 §8.1.1）；代码仅保留在错误码表中以免重复分配 |
| `E0821` | `XR_ERR_TRY_BANG_BAD_OPERAND` | `try!` 操作数不是 `Result<T,E>` 或 `T?` |
| `E0822` | `XR_ERR_TRY_BANG_NON_EXCEPTION_ERR` | `try!` 跨轨升级时 `E` 不是 `Exception` 派生 |
| `E0823` | `XR_ERR_MATCH_NOT_EXHAUSTIVE` | 已合并到 `E0371`（见 §6.3.3）；代码仅保留在错误码表中以免重复分配 |
| `E0824` | `XR_ERR_UNWRAP_NON_EXCEPTION_ERR` | `Result<T, E>.unwrap()` 中 `E` 不是 `Exception` 派生 |

### 18.8 错误对象结构

VM 抛出的运行时错误使用 prelude `Exception` 类（声明：`stdlib/types/exception.xr`）：

```xray
@native
class Exception {
    message: string             // 人类可读消息，含错误码与上下文
    stack: Array<string>        // 自动 capture 的调用栈，每帧一行格式化字符串
    cause: Exception?           // 链式 cause
    code: int                   // 错误码（从 "E0xxx: ..." 前缀自动解析，默认 0）
    data: Json?                 // throw 非异常值时原始值被包装在此

    constructor(message: string = "", cause: Exception? = null)
    fn toString() -> string
}
```

`throw` 操作数的静态类型**必须**是 `Exception` 派生（见 §8.1.1 / `E0370`）。如需结构化错误，继承 `Exception` 添加业务字段：

```xray
class HttpError extends Exception {
    statusCode: int
    constructor(statusCode: int, message: string, cause: Exception? = null) {
        super(message, cause)
        this.statusCode = statusCode
    }
}
```

或使用 ADT enum + `Result<T, E>` 表达可枚举的失败模式（见 §8.2）。

---

## 附录 A. EBNF 语法

> 真值源：`src/frontend/parser/xparse_*.c`。本附录给出整理后的紧凑 EBNF；具体冲突由 parser 实现决议。

### A.1 词法层

```ebnf
SourceFile ::= Statement*

Comment ::= '//' [^\n]*
         |  '/*' .* '*/'

Identifier ::= IdStart IdContinue*
IdStart    ::= 'a'..'z' | 'A'..'Z' | '_'
IdContinue ::= IdStart | '0'..'9'

IntLiteral   ::= DecimalInt | HexInt | BinInt | OctInt
DecimalInt   ::= DecimalDigit ('_'? DecimalDigit)*
HexInt       ::= '0x' HexDigit ('_'? HexDigit)*
BinInt       ::= '0b' ('0' | '1') ('_'? ('0' | '1'))*
OctInt       ::= '0o' ('0'..'7') ('_'? ('0'..'7'))*

FloatLiteral ::= DecimalInt '.' DecimalInt? Exponent?
              |  DecimalInt Exponent
Exponent     ::= ('e' | 'E') ('+' | '-')? DecimalDigit+

BigIntLiteral ::= DecimalInt 'n'

StringLiteral ::= '"' StringChar* '"'
                | "'" StringChar* "'"
RawStringLiteral ::= 'r' '"' [^"]* '"'
RegexLiteral ::= '/' RegexBody '/' RegexFlags?

BoolLiteral ::= 'true' | 'false'
NullLiteral ::= 'null'
```

### A.2 类型

```ebnf
Type ::= UnionType
UnionType ::= IntersectionType ('|' IntersectionType)*
IntersectionType ::= NullableType
NullableType ::= PrimaryType '?'?
PrimaryType ::= NamedType | FunctionType | TupleType | ObjectType
NamedType   ::= QualifiedIdent TypeArgs?
FunctionType ::= '(' TypeList? ')' '->' Type
TupleType   ::= '(' Type (',' Type)+ ')'
ObjectType  ::= '{' FieldList? '}'
FieldList   ::= ObjectField (',' ObjectField)* ','?
ObjectField ::= Identifier ':' Type
QualifiedIdent ::= Identifier ('.' Identifier)*
TypeArgs    ::= '<' Type (',' Type)* '>'
TypeList    ::= Type (',' Type)*
```

### A.3 表达式

```ebnf
Expression ::= AssignExpr
AssignExpr ::= TernaryExpr (AssignOp Expression)?
AssignOp   ::= '=' | '+=' | '-=' | '*=' | '/=' | '%='
            |  '&=' | '|=' | '^=' | '<<=' | '>>='

TernaryExpr ::= LogicOrExpr ('?' Expression ':' Expression)?
LogicOrExpr ::= LogicAndExpr ('||' LogicAndExpr)*
            |   NullCoalesce
LogicAndExpr ::= BitOrExpr ('&&' BitOrExpr)*
NullCoalesce ::= LogicAndExpr ('??' LogicAndExpr)*
BitOrExpr   ::= BitXorExpr ('|' BitXorExpr)*
BitXorExpr  ::= BitAndExpr ('^' BitAndExpr)*
BitAndExpr  ::= EqualityExpr ('&' EqualityExpr)*
EqualityExpr ::= RelationalExpr (('==' | '!=' | '===' | '!==') RelationalExpr)*
RelationalExpr ::= ShiftExpr (('<' | '<=' | '>' | '>=') ShiftExpr)*
ShiftExpr   ::= AdditiveExpr (('<<' | '>>') AdditiveExpr)*
AdditiveExpr ::= MultiplicativeExpr (('+' | '-') MultiplicativeExpr)*
MultiplicativeExpr ::= TypeOpExpr (('*' | '/' | '%') TypeOpExpr)*
TypeOpExpr  ::= UnaryExpr (('as' | 'is') Type)*           // 安全转换写为 `x as T?`，T? 是可空类型
RangeExpr   ::= AdditiveExpr ('..' AdditiveExpr)?

UnaryExpr ::= ('-' | '+' | '!' | '~' | '++' | '--') UnaryExpr
           |  'new' QualifiedIdent TypeArgs? '(' ArgList? ')'
           |  'move' UnaryExpr
           |  'await' ('all' | 'any' | 'anySuccess')? UnaryExpr
           |  'go' (Block | PostfixExpr)
           |  'try?' UnaryExpr
           |  'try!' UnaryExpr
           |  PostfixExpr

PostfixExpr ::= Primary PostfixOp*
PostfixOp   ::= '(' ArgList? ')'              // call
             |  '.' Identifier                 // member
             |  '?.' (Identifier | '(' ArgList? ')')  // optional chain
             |  '[' Expression ']'             // index
             |  '[' Expression? ':' Expression? ']'  // slice
             |  '!'                            // force unwrap
             |  '++' | '--'                    // postfix inc/dec

Primary ::= IntLiteral | FloatLiteral | BigIntLiteral
         |  StringLiteral | RawStringLiteral | RegexLiteral
         |  BoolLiteral | NullLiteral
         |  Identifier
         |  ArrayLit | MapLit | SetLit | ObjectLit
         |  ArrowFunction
         |  MatchExpr
         |  TryExpr
         |  CatchBlock
         |  '(' Expression ')'
         |  '(' Expression (',' Expression)+ ')'  // tuple

ArrayLit ::= '[' (Expression (',' Expression)* ','?)? ']'
MapLit   ::= '#{' (MapEntry (',' MapEntry)* ','?)? '}'
MapEntry ::= Expression ':' Expression
SetLit   ::= '#[' (Expression (',' Expression)* ','?)? ']'
ObjectLit ::= '{' (ObjectFieldExpr (',' ObjectFieldExpr)* ','?)? '}'
ObjectFieldExpr ::= Identifier ':' Expression | Identifier

ArrowFunction ::= '(' ArrowParams? ')' '->' (Expression | Block)
ArrowParams ::= ArrowParam (',' ArrowParam)*
ArrowParam  ::= Identifier ':' Type
// Note: arrow closures cannot declare an explicit return type;
// use `fn(p: T) -> R { ... }` or annotate the binding (`let f: (T) -> R = ...`) instead.

MatchExpr ::= 'match' '(' Expression ')' '{' MatchArm (','? MatchArm)* ','? '}'
MatchArm  ::= Pattern ('if' '(' Expression ')')? '->' (Expression | Block)

TryExpr     ::= 'try?' Expression | 'try!' Expression
CatchBlock  ::= 'catch!' Block

ThrowExpr   ::= 'throw' Expression                // operand 静态类型必须是 Exception 派生（E0370）

ArgList ::= Expression (',' Expression)*
```

### A.4 模式

```ebnf
Pattern ::= LiteralPattern
         |  RangePattern
         |  EnumPattern
         |  TypePattern
         |  WildcardPattern
         |  BindingPattern
         |  MultiPattern

LiteralPattern  ::= IntLiteral | FloatLiteral | StringLiteral | BoolLiteral | NullLiteral
RangePattern    ::= Expression '..' Expression
EnumPattern     ::= QualifiedIdent VariantPayloadPattern?    // ADT enum payload 解构
VariantPayloadPattern ::= '(' Pattern (',' Pattern)* ')'
TypePattern     ::= 'is' Type Identifier?
WildcardPattern ::= '_'
BindingPattern  ::= Identifier
MultiPattern    ::= Pattern (',' Pattern)+
```

### A.5 语句

```ebnf
Statement ::= ExprStmt
           |  VarDecl
           |  FnDecl
           |  ClassDecl
           |  StructDecl
           |  InterfaceDecl
           |  EnumDecl
           |  TypeAliasDecl
           |  ImportDecl
           |  ExportDecl
           |  IfStmt
           |  WhileStmt
           |  ForStmt
           |  ForInStmt
           |  ForInPairStmt
           |  MatchStmt
           |  ScopeStmt
           |  SelectStmt
           |  ReturnStmt
           |  BreakStmt
           |  ContinueStmt
           |  ThrowStmt
           |  TryStmt
           |  DeferStmt
           |  YieldStmt
           |  Block
           // \u6ce8\uff1aprint/dump \u4f5c\u4e3a\u51fd\u6570\u8c03\u7528\u5305\u542b\u5728 ExprStmt \u4e2d\uff1bgo \u662f\u8868\u8fbe\u5f0f\uff08GoExpr\uff09

ExprStmt ::= Expression (';' | LineBreak)
Block    ::= '{' Statement* '}'

IfStmt    ::= 'if' '(' Expression ')' Block ('else' 'if' '(' Expression ')' Block)* ('else' Block)?
WhileStmt ::= 'while' '(' Expression ')' Block
ForStmt   ::= 'for' '(' VarDecl? ';' Expression? ';' Expression (',' Expression)* ? ')' Block
ForInStmt ::= 'for' '(' Identifier 'in' Expression ')' Block
ForInPairStmt ::= 'for' '(' Identifier ',' Identifier 'in' Expression ')' Block
             |  'for' '(' '(' Identifier ',' Identifier ')' 'in' Expression ')' Block
MatchStmt ::= 'match' '(' Expression ')' '{' MatchArm (','? MatchArm)* ','? '}'

ReturnStmt   ::= 'return' (Expression | '(' Expression (',' Expression)+ ')')?
BreakStmt    ::= 'break'
ContinueStmt ::= 'continue'

ThrowStmt ::= 'throw' Expression
TryStmt   ::= 'try' Block CatchClause? FinallyClause?
CatchClause ::= 'catch' ('(' Identifier (':' Type)? ')')? Block
FinallyClause ::= 'finally' Block

DeferStmt ::= 'defer' (Expression | Block)

// print 是普通全局函数调用，语法上属于 ExprStmt。

// go 是表达式，返回 Task<T>。不作为独立语句类别出现（封装在 ExprStmt 中）。

ScopeStmt ::= 'scope' Block            // 词法作用域 + 结构化并发

SelectStmt ::= 'select' '{' SelectArm+ '}'
SelectArm  ::= Identifier 'from' Expression '->' Block      // 接收
            |  Expression 'to' Expression '->' Block        // 发送
            |  'after' Expression '->' Block                // 超时
            |  '_' '->' Block                                // 默认

YieldStmt ::= 'yield'
```

### A.6 声明

```ebnf
VarDecl ::= ('let' | 'const' | 'shared' ('const' | 'let')) Binding (',' Binding)*
Binding ::= BindingPattern (':' Type)? ('=' Expression)?
BindingPattern ::= Identifier
                |  '[' BindingPattern (',' BindingPattern)* ','? ']'
                |  '(' BindingPattern (',' BindingPattern)+ ','? ')'
                |  '{' Identifier (',' Identifier)* ','? '}'

FnDecl ::= AttrList? Modifier* 'fn' Identifier TypeParams? '(' ParamList? ')' ReturnType? Block
ParamList ::= Param (',' Param)*
Param     ::= Modifier* Identifier ':' Type ('=' Expression)?
           |  '...' Identifier ':' Type
ReturnType ::= '->' Type | '->' '(' Type (',' Type)+ ')'
Modifier  ::= 'in' | 'ref' | 'private' | 'public' | 'static' | 'final' | 'abstract' | 'override'
              // public/override 合法但实际从不使用（默认/隐式行为）

TypeParams ::= '<' TypeParam (',' TypeParam)* '>'
TypeParam  ::= Identifier (':' Type ('&' Type)*)?         // 约束用 ':' ，多约束用 '&'

ClassDecl ::= Modifier* 'class' Identifier TypeParams?
              ('extends' NamedType)?
              ('implements' NamedType (',' NamedType)*)?
              '{' ClassMember* '}'
ClassMember ::= FieldDecl | MethodDecl | ConstructorDecl
FieldDecl ::= Modifier* Identifier ':' Type ('=' Expression)?
MethodDecl ::= Modifier* Identifier '(' ParamList? ')' ReturnType? Block
            |  Modifier* 'operator' OperatorToken '(' ParamList? ')' ReturnType? Block
ConstructorDecl ::= 'constructor' '(' ParamList? ')' Block

StructDecl ::= 'struct' Identifier TypeParams?
               ('implements' NamedType (',' NamedType)*)?
               '{' ClassMember* '}'

InterfaceDecl ::= 'interface' Identifier TypeParams?
                  ('extends' NamedType (',' NamedType)*)?
                  '{' InterfaceMember* '}'
InterfaceMember ::= Identifier '(' ParamList? ')' ReturnType?

EnumDecl       ::= 'enum' Identifier TypeParams?
                   ('implements' NamedType (',' NamedType)*)?
                   '{' EnumVariant (',' EnumVariant)* ','? EnumMethod* '}'
EnumVariant    ::= Identifier VariantPayload?
                |  Identifier '=' BackingValue                  // 简单枚举（无 payload）
EnumMethod     ::= 'fn' Identifier TypeParams? '(' ParamList? ')' ReturnType? Block
VariantPayload ::= '(' VariantField (',' VariantField)* ')'
VariantField   ::= (Identifier ':')? Type
BackingValue   ::= IntLiteral | FloatLiteral | StringLiteral | BoolLiteral

TypeAliasDecl ::= 'type' Identifier TypeParams? '=' Type

ImportDecl ::= 'import' ImportMembers 'from' ImportModule
            |  'import' ImportModule ('as' Identifier)?
ExportDecl ::= 'export' Declaration                                         // 直接导出声明
            |  'export' Identifier                                          // 导出已声明标识符
            |  'export' '*' 'from' StringLiteral                            // 转发导出
ImportMembers ::= '{' ImportMember (',' ImportMember)* ','? '}'
ImportMember  ::= Identifier ('as' Identifier)?
ImportModule  ::= StringLiteral | Identifier ('/' Identifier)?

AttrList ::= ('@' Identifier ('(' ArgList? ')')?)*

OperatorToken ::= '+' | '-' | '*' | '/' | '%'
               |  '&' | '|' | '^'
               |  '==' | '!=' | '<' | '<=' | '>' | '>='
               |  '[]' | '[]='
               |  '!' | '~'
```

> 注：以上 EBNF 为指导性整理。precedence、associativity、消歧由 parser 实现决议；遇到歧义请以 `src/frontend/parser/xparse_*.c` 为准。

---

## 附录 B. 关键字索引

完整 63 个关键字按字母排序见 [§1.5](#15-关键字)。

| 关键字 | 节 |
|--|--|
| `abstract` | §5.3 |
| `as` | §3.8 |
| `await` | §10.3 |
| `bool` | §2.3.3 |
| `break` | §4.6 |
| `catch` | §8 |
| `class` | §5.3 |
| `const` | §5.1 |
| `constructor` | §5.3 |
| `continue` | §4.6 |
| `defer` | §4.9 |
| `else` | §4.2 |
| `enum` | §5.6 |
| `export` | §5.8 |
| `extends` | §5.3 |
| `false` | §1.6.4 |
| `final` | §5.3 |
| `finally` | §8 |
| `float` `float32` `float64` | §2.3.2 |
| `fn` | §5.2 |
| `for` | §4.4 |
| `go` | §10.2 |
| `if` | §4.2 |
| `implements` | §5.5 |
| `import` | §5.8 |
| `in` | §4.4 |
| `int` `int8`..`int64` | §2.3.1 |
| `interface` | §5.5 |
| `is` | §3.8 |
| `let` | §5.1 |
| `match` | §3.13 / §4.5 |
| `new` | §3.14 |
| `null` | §1.6.4 |
| `operator` | §5.3 |
| `override` | §5.3 |
| `private` `public` | §5.3 |
| `return` | §4.7 |
| `scope` | §10.7 |
| `select` | §10.6 |
| `shared` | §5.1 / §10.11 |
| `static` | §5.3 |
| `string` | §2.3.4 |
| `struct` | §5.4 |
| `super` | §5.3 |
| `this` | §5.3 |
| `throw` | §8 |
| `true` | §1.6.4 |
| `try` | §8 |
| `type` | §5.7 |
| `uint8`..`uint64` | §2.3.1 |
| `unknown` | §2.2（编译器内部）|
| `while` | §4.3 |
| `yield` | §3.16 / §10.10 |

---

## 附录 C. 操作符索引

完整操作符按用途见 [§1.7](#17-操作符与-token)。详细优先级见 [§3.1](#31-优先级与结合性)。

| 类别 | 操作符 |
|--|--|
| 算术 | `+` `-` `*` `/` `%` |
| 位运算 | `&` `\|` `^` `~` `<<` `>>` |
| 比较 | `==` `!=` `===` `!==` `<` `<=` `>` `>=` |
| 逻辑 | `&&` `\|\|` `!` |
| 赋值 | `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` |
| 其他 | `..` `??` `?.` `!` `->` |

---

## 附录 D. 标准库模块索引

完整 22 个 native 模块见 [§15](#15-标准库概览-standard-library)。

| 模块 | 用途 |
|--|--|
| `base64` | Base64 编解码 |
| `cluster` | 分布式集群 |
| `compress` | 压缩（gzip/zlib/deflate） |
| `crypto` | 加密散列 |
| `csv` | CSV 解析/序列化 |
| `datetime` | 日期时间 |
| `encoding` | 字符编码转换 |
| `gc` | GC 控制 |
| `http` | HTTP/REST |
| `io` | 文件 I/O |
| `log` | 结构化日志 |
| `math` | 数学函数 |
| `net` | TCP/UDP/TLS |
| `os` | 操作系统 |
| `path` | 路径操作 |
| `regex` | 正则 |
| `time` | 时间/计时器/sleep |
| `toml` | TOML 解析 |
| `url` | URL 解析/构造 |
| `ws` | WebSocket |
| `xml` | XML 解析 |
| `yaml` | YAML 解析 |

---

## 附录 E. 与其他语言的差异速查

xray 在开发过程中借鉴了现有语言的许多优秀设计，但还是有显著差异。

### E.1 vs JavaScript / TypeScript

| 维度 | JS/TS | xray |
|--|--|--|
| 静态类型 | TS 可选 | **强制**（除 `Json` 是动态） |
| 数值 | 仅 `number`（双精度） | `int` `float` `BigInt` 严格区分 |
| 真值转换 | truthy / falsy | truthy / falsy（与 JS 相近）但 `bool` 类型本身不接受 int/null 隐式赋值 |
| `===` 与 `==` | `===` 强、`==` 弱（string↔number 自动转） | `==`/`!=` 为值相等（仅数值 int↔float 提升）；`===`/`!==` 为类型和值都严格相等 |
| 闭包捕获 | 引用 | 引用（默认）；`go` 闭包严格受限 |
| 对象 | 动态字段 | 默认动态；带 `type T = {...}` 注解后 sealed |
| import | ES Module | 自有 import 语法（含 stdlib 无引号形式） |
| 并发 | 异步/Promise | 协程 + Channel |

### E.2 vs Go

| 维度 | Go | xray |
|--|--|--|
| 类型系统 | 简单 + interface 隐式 | 较丰富 + 显式 `implements` |
| 错误处理 | 多返回值 + `err != nil` | 异常 + `Result<T,E>` + `try?`/`try!` |
| 协程 | `go func() {}`（语句） | `go expr`（表达式，返回 `Task<T>`） |
| 等待结果 | 无直接等价（通过 channel/WaitGroup） | `await t`、`await all [...]`、`await any [...]` |
| Channel | 内置 `chan T`，`<-` 操作符 | `Channel<T>` 类，方法 `send`/`recv`/`trySend`/`tryRecv` |
| select 分支 | `case x := <-ch:` / `case ch <- v:` / `default:` | `x from ch ->` / `v to ch ->` / `after ms ->` / `_ ->` |
| GC | 三色并发 | per-coroutine Mark-Sweep / Immix |
| 类与继承 | 无（仅 struct + interface） | class 支持继承 |
| 泛型 | 1.18+ 有 | 有，monomorphization + 运行时 reified |

### E.3 vs Rust

| 维度 | Rust | xray |
|--|--|--|
| 内存安全 | borrow checker 全面 | 仅跨协程用 `move`；其他用 GC |
| 错误 | `Result<T, E>` | 异常 + `Result<T,E>` |
| 类型推断 | Hindley-Milner 强 | 双向推断 |
| trait | 完整 | 类似 `interface`，少功能 |
| 性能 | 接近 C | VM/JIT，热路径接近 native |
| 编译期 | macro / const | 简单常量折叠 |

### E.4 vs Python

| 维度 | Python | xray |
|--|--|--|
| 类型 | 动态（可选 hint） | 静态 |
| GIL | 有 | 无（M:N 协程） |
| 字符串 | unicode str | utf-8 string |
| 缩进 | 强制 | 自由（用 `{}`） |
| 类 | 动态属性 | 静态字段 |
| 性能 | CPython 慢 | JIT 后接近 V8/JVM |

### E.5 vs Swift

| 维度 | Swift | xray |
|--|--|--|
| 可空 `?` | 有 | 有 |
| `!` 解包 | 有 | 有 |
| `try?` / `try!` 语义 | `try?` 折叠为 nil；`try!` 失败 abort | `try?` 折叠为 null；`try!` **重抛原异常**（不 abort） |
| struct vs class | 值/引用 | 值/引用 |
| 协议 | 有强 | `interface` 较弱 |
| 并发 | actor + async/await | 协程 + Channel + `go`/`await all`/`scope` |

---

## 附录 F. 词汇表

| 术语 | 定义 |
|--|--|
| **AOT** | Ahead-of-Time 编译：构建时预编译为机器码 |
| **AST** | Abstract Syntax Tree：源码解析后的中间表示 |
| **Arena** | 批量分配器：所有分配同时释放 |
| **Bytes** | 字节缓冲类型（见 §2.4.5） |
| **Channel** | 类型化的协程通信管道（见 §10.5） |
| **closure** | 闭包：捕获外层变量的函数 |
| **coroutine** | 协程：用户态可暂停/恢复的执行流 |
| **defer** | 延迟执行：函数退出前执行（见 §4.9） |
| **enum** | 枚举类型（见 §5.6） |
| **GC** | Garbage Collector：垃圾回收 |
| **GC-safepoint** | GC 安全点：可安全开始 GC 的指令位置 |
| **goroutine** | xray 中称作协程 (coroutine)，启动语法 `go {...}` |
| **hoisting** | 提升：声明在使用前被隐式定义 |
| **IC** | Inline Cache：内联缓存（属性访问/方法分派优化） |
| **interface** | 接口（见 §5.5） |
| **JIT** | Just-In-Time 编译：运行时编译热路径 |
| **lvalue / rvalue** | 左值（可赋值）/ 右值（仅值） |
| **monomorphization** | 单态化：泛型实例化为多个具体类型版本（xray 不做） |
| **move** | 所有权转移：跨协程时强制（见 §7.3） |
| **NaN-boxing** | 用 IEEE-754 NaN 的位空间存放标记值 |
| **nullable** | 可空类型 `T?`：值可以为 null |
| **pattern** | 模式：用于 `match` 与解构（见 §6） |
| **scope** | 作用域 |
| **shared** | 跨协程共享的存储类（见 §5.1.3） |
| **SSA** | Static Single Assignment：每个变量只赋值一次的 IR |
| **struct** | 值类型类（见 §5.4） |
| **TCO** | Tail-Call Optimization：尾调用优化 |
| **trait** | Rust 术语；xray 用 `interface` |
| **truthy** | 真值：控制流中非 `false` / `null` / `0` / `""` / 空集合的值视为真（见 §2.3.3） |
| **monomorphization** | 单态化：泛型类型参数在编译期特化为具体类型，运行时保留类型信息 |
| **union** | 联合类型 `A \| B` |
| **upvalue** | 闭包捕获的外层变量 |
| **VM** | Virtual Machine：xray 字节码虚拟机 |
| **write barrier** | 写屏障：GC 在指针更新时插入的钩子 |

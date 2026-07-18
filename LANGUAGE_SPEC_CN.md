# Xray 语言参考手册

> 版本：基于 `xray` v0.9.0 源码（校对日期 2026-07-18）
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
- [18. 错误码 (Error Codes)](#18-错误码-error-codes)
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
| **运行模式** | 字节码 VM 与 `xray build --native` AOT 两条路径；当前没有 JIT，跨后端语义由差分测试守门 |
| **错误处理** | 值返回错误通道（throw / try / catch + enum 错误）+ panic 边界（catch panic）+ 可空类型（T?）+ defer 资源管理 |
| **元编程** | 注解（`@test` / `@native` / `@deprecated`）+ 编译期/derive 元数据 + 泛型单态化 |
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

> 真值源：`src/frontend/lexer/xlex.h`（token 枚举）、`src/frontend/lexer/xkeywords.def`（关键字表，66 条）、`src/frontend/lexer/xlex.c`（扫描器实现）。

### 1.1 字符编码

xray 源文件**必须**是合法 UTF-8。scanner 在产生首个 token 前对整个输入做严格 UTF-8 校验；字符串、注释和标识符都可包含非 ASCII 字符（标识符规则见 §1.4）。

文件可选 UTF-8 BOM（`EF BB BF`）；扫描器跳过开头的 BOM。

### 1.2 行结尾与空白

行结尾以 `\n` 计数；Windows `\r\n` 因 `\r` 被当作横向空白而正常工作。单独的 `\r` 也会被跳过，但**不会**增加行号或触发智能分号，因此不应当作源码换行使用。

**空白字符**：空格 (`U+0020`)、水平制表符 (`U+0009`)、行结尾。空白用于分隔 token，不传递语义（**异常**：泛型语境下连续 `>>` 的拆分依赖空白上下文）。

### 1.3 注释

xray 支持两种注释：行注释不嵌套，块注释支持嵌套：

```xray
// 行注释，从 // 到行尾
/* 块注释，
   可跨行；
   支持 /* 嵌套 */ 到任意合理深度 */
```

注释可出现在任何空白能出现的地方。formatter 与 LSP 可以读取注释 trivia；trivia 不参与语法分析。

文档注释（与普通注释无语法差异）：约定以 `///` 或 `/** */` 开头，用于工具识别。当前编译器不强制此约定。

### 1.4 标识符

```ebnf
Identifier ::= IdentStart IdentCont*
IdentStart ::= 'a'..'z' | 'A'..'Z' | '_' | Utf8NonAsciiByteSequence
IdentCont  ::= IdentStart | '0'..'9'
```

输入先经过严格 UTF-8 校验；随后 scanner 把非 ASCII UTF-8 字节序列作为标识符的一部分。因此 `中文`、`café` 都是合法标识符。当前 scanner 不做 Unicode XID 分类或 NFC 规范化，视觉等价但编码不同的名字仍是不同标识符。

**保留约束**：标识符不能与保留关键字相同（见 §1.5）；可与**上下文敏感关键字**相同（如 `from`、`to`、`default`、`ref`、`move`、`linked`、`supervisor`、`after` 可作为普通标识符）。

字符 `_` 是**专用通配符 token**，不是普通标识符：

- 在 `match` 模式中表示**通配符**（见 §6.7）。
- 在 `for-in` 中可用于忽略键或值：`for (_, v in m) { ... }`。
- 在解构绑定中可用于忽略位置：`var (a, _) = (1, 2)`。
- **不能**作为 `var _ = expr`、函数参数名或被引用的变量名；编译器会报"expected variable name"。
- 多下划线名（如 `__tmp`）是普通标识符。

### 1.5 关键字

xray 共 **66 个保留关键字**，按用途分组如下：

#### 1.5.1 声明与流程控制

| 关键字 | 用途 |
|--|--|
| `var` | 可变变量声明 |
| `const` | 不可变变量声明 |
| `shared` | 共享身份绑定（由 shared/system owner 持有，词法作用域照常） |
| `owned` | 唯一的 system-owned 可变身份绑定；转移使用 `move` |
| `comptime` | 强制编译期求值的表达式前缀 |
| `fn` | 函数声明 |
| `return` | 函数返回 |
| `yield` | 生成器产值语句 |
| `if` `else` | 条件分支 |
| `while` | 循环 |
| `for` `in` | 循环（C 风格 + for-in） |
| `break` `continue` | 循环控制 |
| `match` | 模式匹配 |

#### 1.5.2 面向对象与类型

| 关键字 | 用途 |
|--|--|
| `class` `struct` | 类/结构体声明 |
| `packed` `union` | FFI 布局声明 |
| `extends` | 类继承 |
| `interface` `implements` | 接口声明/实现 |
| `enum` | 枚举声明 |
| `type` | 类型别名 |
| `new` | 保留字；对象构造统一写 `T(...)`（见 §3.14） |
| `this` `super` | 自我/父类引用 |
| `constructor` | 构造器 |
| `static` `private` `protected` | 类/成员修饰符；公开是默认语义，没有 `public` 关键字 |
| `const` | 不可变字段/绑定修饰符 |
| `final` | `final class` 禁止继承 |
| `operator` | 运算符重载 |
| `is` `as` | 运行时类型检查 / 转换 |

`abstract` 与 `override` 不是关键字；它们在普通表达式位置可作为标识符。类的抽象约束通过接口表达，同名同签名方法自动覆写，不需要成员修饰符。

#### 1.5.3 错误处理

`try` `catch` `throw` `defer`

#### 1.5.4 模块系统

`import` `export`

#### 1.5.5 协程与并发

`go` `await` `select` `defer` `scope` `unsafe`

`parallel` 是需要显式 import 的标准库模块名，不是词法关键字。

#### 1.5.6 类型名（保留）

`int` `int8` `int16` `int32` `int64` `byte` `uint8` `uint16` `uint32` `uint64`
`float` `float32` `float64` `bool` `string` `rune`

类型注解中写 `unknown` 会被解析器拒绝；它不是词法关键字，表达式位置仍可作为普通标识符使用。

> **注意**：以下名字**不是**词法关键字，而是 `stdlib/prelude/prelude_types.def` 自动引入的类型符号：
> `Array` · `Atomic` · `BigInt` · `Channel` · `Json` · `Map` · `NetConn` · `NetListener` · `OsBarrier` · `OsCondvar` · `OsMutex` · `OsOnce` · `OsRwLock` · `PanicInfo` · `Path` · `Range` · `Regex` · `Set` · `StringBuilder` · `Thread`。
> `Array<byte>` 是 `Array` 的特化而不是独立名字。`DateTime`、`Logger` 等模块类型必须从对应模块显式 import。

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
| `ref` | 参数模式与调用授权 (`fn f(p: ref T)` / `f(ref p)`) |
| `out` | 输出参数模式与调用授权 (`fn f(p: out T)` / `f(out p)`) |
| `move` | 所有权转移 (`move x`) |
| `linked` | `linked go` / `linked scope` 修饰符 |
| `supervisor` | `supervisor scope` 修饰符 |
| `after` | `select` 的超时分支 (`after 1000 -> ...`) |
| `panic` | `catch panic (p)` 的 panic 通道边界 |

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
- 当整数字面量直接出现在窄整数上下文（变量初始化、赋值、参数、返回值、集合元素等）时，字面量值必须落在目标类型范围内；例如 `var x: int8 = 200` 是编译错误。非字面量表达式在写入窄整数目标时仍按目标宽度窄化并环绕，见 §2.3.1。

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

xray 支持两类字符串字面量：**带转义** 和 **原始字符串**。字符串只使用双引号；单引号专用于 `rune` 字面量。反引号字符串不属于当前语法——lexer 直接报错。

##### 普通字符串（双引号）

```ebnf
StringLiteral ::= '"' StrChar* '"'
StrChar ::= 任何非双引号、非反斜杠、非换行符
          | EscapeSeq
          | Interpolation
EscapeSeq ::= '\' ('"' | "'" | '\\' | 'n' | 't' | 'r' | '0'
                  | 'x' HexDigit{2}
                  | 'u' HexDigit{4}
                  | 'u{' HexDigit{1,6} '}')
Interpolation ::= '${' Expression '}'
```

- 字符串可跨行；行结尾包含在字符串中。
- 包含插值的字面量在 lexer 内部产出 `TK_TEMPLATE_STRING`；不含插值的产出 `TK_LITERAL_STRING`。
- `${...}` 内按表达式模式扫描：大括号按深度配对，内部字符串 / raw string / rune 字面量会被整体跳过，因此允许同种引号嵌套，例如 `"${m["k"]}"` 与 `"${"a}b"}"`。

```xray
"hello"
"Hello, ${name}! ${1 + 2}"
"tab\there\nnewline"
"\u4F60\u597D"        // "你好"
"\u{1F600}"            // emoji
```

插值表达式可以继续包含嵌套插值；内层字符串中的 `}` 不会结束外层 `${...}`。

##### 原始字符串（`r` 前缀）

```ebnf
RawString ::= 'r' '"' RawChar* '"'
RawChar ::= 任何非双引号字符（包括 `\`，不做转义处理）
```

- **不**处理任何转义（`\n`、`\t` 等保持原样）。
- 仍然支持 `${...}` 插值。
- 标识符 `r` 单独使用时仍为普通标识符（`TK_NAME`），仅当后紧接双引号才识别为原始字符串前缀。
- raw string 使用 `r"..."`；单引号字符串仍按普通转义规则解析。

```xray
r"C:\path\to\file"          // 字面量包含两个反斜杠
r"C:\Users\${USER}"         // 反斜杠不转义，但 ${USER} 仍插值
```

#### 1.6.6 `rune` 字面量

```ebnf
CharLiteral ::= "'" CharBody "'"
CharBody ::= UnicodeScalar | EscapeSeq | '\u{' HexDigit{1,6} '}'
```

- `'a'` 的类型是 `rune`，表示一个 Unicode scalar value。
- 合法范围为 `U+0000..U+10FFFF`，排除 surrogate `U+D800..U+DFFF`。
- 字面量必须恰好包含一个 scalar；`''`、`'ab'`、`'🇨🇳'`、`'é'` 均编译失败。
- 支持 `'\n'`、`'\t'`、`'\r'`、`'\0'`、`'\''`、`'\\'`、`'\u{1F600}'` 等转义。
- rune 字面量不支持 `${...}` 插值。

```xray
var a: rune = 'a'
var zh: rune = '中'
var smile: rune = '\u{1F600}'
```

##### 字符串插值

字符串模板使用普通双引号和 `${...}` 插值。

#### 1.6.7 正则字面量

```ebnf
RegexLiteral ::= '/' RegexBody '/' RegexFlag*
RegexFlag ::= 'g' | 'i' | 'm' | 's'
```

```xray
/[a-z]+/i
/\d+\.\d+/g
```

- flags：`g`（全局）、`i`（忽略大小写）、`m`（多行）、`s`（dot 匹配换行）。
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

`==` `!=` `<` `<=` `>` `>=`

- `==` `!=`：值相等（隐式数值转换：int→float）
- `<` 等：数字、字符串支持；其他类型不支持

#### 1.7.5 逻辑

`&&` `||` `!`

短路求值。

#### 1.7.6 赋值

`=` `+=` `-=` `*=` `/=` `%=` `&=` `|=` `^=` `<<=` `>>=`

#### 1.7.7 自增自减

`++` `--`

仅支持**语句级后缀**形式 `x++` / `x--`；前缀 `++x` / `--x`、以及表达式位置的 `x++` / `x--` 均编译报错。详见 §4.1。

#### 1.7.8 类型相关

| Token | 用途 |
|--|--|
| `?` | 可空类型 (`T?`)、ternary、可选链前缀 |
| `?.` | 可选链属性/方法 (`obj?.prop`, `obj?.method()`) |
| `?[` | 可选链索引 (`arr?[0]`) |
| `??` | 空值合并 (`a ?? b`) |
| `!` | 强制解包（后缀，`expr!`）/ 逻辑非（前缀） |
| `\|` | union 类型 (`int \| string`) / 位或 |
| `->` | 统一箭头：函数返回类型、函数类型、闭包、`match` / `select` 分支 |
| `...` | rest / spread |
| `..` | 半开范围 (`0..10`) |
| `..=` | 闭区间范围 (`0..=10`) |
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
var empty_map = #{}
var primes = #[2, 3, 5, 7]
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
4. **泛型单态化**：泛型定义在构建期按具体类型特化，同时保留名义类型身份。
5. **Structural Json + Nominal class**：Json 对象按字段结构兼容（duck typing），class 按名义兼容。
6. **最小类型身份**：`typeOf` / `typeName` / `is` / `as`；默认没有运行时 `Reflect` 模块。

### 2.2 类型分类

| 类别 | 示例 |
|--|--|
| Primitive | `int`、`float`、`bool`、`string`、`rune`、`()`（Unit，无返回值） |
| 精确整数 | `int8`、`int16`、`int32`、`int64`、`byte`..`uint64` |
| 精确浮点 | `float32`、`float64` |
| 容器 | `Array<T>`、`Map<K,V>`、`Set<T>`、`Channel<T>`；`Array<byte>` 是连续字节元素的 `Array` 特化 |
| 定长布局 | `[T; N]` |
| Prelude 特殊类型 | `Json`、`BigInt`、`Range`、`Regex`、`StringBuilder`、`Atomic<T>`、`Path`、`Thread<T>`、`NetConn`、`NetListener`、`Os*` 同步类型 |
| 模块导出类型 | `DateTime`、`Logger`、`Plan`、`Mutex<T>` 等；必须从定义它们的模块显式 import |
| 错误处理 prelude | `PanicInfo`（见 §8） |
| 弱引用容器 | `WeakMap`、`WeakSet` |
| Nullable | `T?` |
| Union | `A \| B \| ...` |
| Tuple | `(T1, T2, ...)` |
| Function | `fn(T1, T2) -> R` |
| FFI / C ABI | `Ptr<T>`、`MutPtr<T>`、`CFn<(T) -> R>`、`uintsize`、`intsize` |
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
| `byte`..`uint64` | 无符号对应 | — |

- 字面量默认 `int`；可被上下文窄化（如赋给 `int32` 变量），但直接字面量必须落在目标范围内（`var x: int8 = 200` 编译拒绝）。
- 算术：二补码环绕语义（wrap on overflow），不区分 debug / release 构建。同宽窄整数运算保留该宽度并按该宽度环绕（`byte + byte -> byte`）；异宽窄整数运算塌回 `int`；移位运算结果取左操作数宽度。
- 静态类型为 `byte`..`uint64` 的值在 `print`、`string(x)`、模板字符串、字符串拼接和顺序比较中按无符号解释；例如静态 `uint64` 的位型 `0xffff_ffff_ffff_ffff` 显示为 `18446744073709551615`，且大于 `0`。
- `int` 的 `checkedAdd` / `checkedSub` / `checkedMul` 在溢出时返回 `null`；`saturating*` 饱和到 `int` 边界；`wrapping*` 显式执行默认二补码环绕。
- 非字面量表达式写入窄整数目标时按目标类型窄化并环绕，例如 `var x: byte = 255 + 1` 得到 `0`。
- 动态擦除后的 `XrValue` 只保存整数 payload，不保存有符号性或位宽；跨过 `any` / Json / 动态容器等边界后，超过 `int64` 正范围的 `uint64` 值在格式化和顺序比较中的行为不保证保留无符号语义。需要无符号语义时保持静态 `uintN` 类型。

#### 2.3.2 浮点类型

| 类型 | 标准 |
|--|--|
| `float32` | IEEE-754 单精度 |
| `float64` | IEEE-754 双精度；`float` 的别名 |

字面量默认 `float`。

#### 2.3.3 `bool`

`true` / `false`，独立类型，与数值类型**不可隐式互转**（不能 `var x: int = true`，也不能 `var b: bool = 1`）。

**条件表达式规则**（`if` / `while` / `for` 条件 / 三元 `?:` / `match` 守卫）：

| 条件类型 | 是否允许 | 语义 |
|---|---|---|
| `bool` | 允许 | 直接布尔判断 |
| `T?` 且 `T != bool` | 允许 | 仅判断是否为 `null`（不检查内容是否“空”） |
| `bool?` | 编译错误 | 三态歧义；写 `flag == true` / `flag != null` / `flag ?? false` |
| `int` / `float` / `string` / `rune` / 集合 / 对象 | 编译错误 | 必须写显式比较，如 `n != 0`、`len(s) != 0` |

`&&` / `||` / `!` 的操作数必须是 `bool`；不要把 `T?` 直接放进 `&&` / `||`。

```xray
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

`rune` 表示一个 Unicode scalar value（有效范围 `U+0000..U+10FFFF`，排除 surrogate 区间 `U+D800..U+DFFF`）。它是独立的原始类型，**不是**数值类型，也**不是** `uint32` 的别名。

```xray
var a: rune = 'a'
var zh = '中'
var smile = '\u{1F600}'
print(typeName(a))        // "rune"
print(smile.toUInt32())   // 128512
```

- rune 字面量必须恰好包含一个 Unicode scalar；空字面量、多 scalar 字面量和 surrogate 字面量都是编译错误。
- `rune` 不参与算术、位运算或窄整数赋值：`'a' + 1`、`var n: uint32 = 'a'` 都会在分析期拒绝。
- 显式转换：`int(c)` 得到 scalar code point；`rune(n)` 从整数构造 rune 并验证 scalar 合法性；`string(c)` / `c.toString()` 得到单 scalar 字符串。
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
| `uintsize` | `size_t` | 宽度由编译目标决定；不得按宿主机 `uint64` 代用 |
| `intsize` | `ptrdiff_t` / 平台有符号宽度 | 宽度由编译目标决定；不得按宿主机 `int64` 代用 |
| `Ptr<T>` | `const void *` 边界值 | 只读裸指针；`T` 用于 xray 端解引用/索引宽度 |
| `MutPtr<T>` | `void *` 边界值 | 可写裸指针；可传给需要 `Ptr<T>` 的位置 |
| `CFn<(A, B) -> R>` | C ABI 函数指针 | 用于把 xray 函数作为 C 回调传入 `extern "C"` 函数 |

裸指针值可以安全地保存、传递、比较和用 `offset(i)` 做按元素宽度缩放的指针偏移；真正读写外部内存必须写在 `unsafe { }` 内：

```xray
extern "C" {
    fn malloc(n: uintsize) -> MutPtr<byte>
    fn free(p: MutPtr<byte>)
}

var p = unsafe { malloc(4) }
unsafe {
    p[0] = 42
    print(p.deref())
    free(p)
}
```

`Ptr<T>` 只能读取，写入必须使用 `MutPtr<T>`；`unsafe` 不会绕过这个类型规则。裸指针访问不做空指针或边界检查，调用方必须保证地址、生命周期、对齐和别名规则正确。

`uintsize` / `intsize` 在 FFI 调用、`mem.load/store<T>`、extern layout 字段和生成 C 中使用同一份目标 ABI 标量描述。VM、AOT 与布局 introspection 必须采用编译目标的宽度和对齐；交叉编译时不得读取构建宿主机的 `sizeof(size_t)` 作为语义。

`CFn<(...) -> ...>` 不是普通 xray 闭包类型。当前 VM/AOT 后端支持把模块级、非捕获、签名精确匹配的 xray 函数传给 C；捕获闭包、匿名函数和 extern 函数本身不能作为 `CFn` 回调实参。

### 2.4 复合类型

#### 2.4.1 `Array<T>`

有序可变数组。详见 §14.7。

```xray
var a: Array<int> = [1, 2, 3]
var b = [1, 2, 3]                // 推断为 Array<int>
var c: Array<string> = []         // 显式空数组
```

`Array<T>` 的 `T` 必须能在编译期确定。空 `[]` 在无类型标注时是编译错误：`Empty array '[]' requires a type annotation`。

`Array<rune>` 保留 `rune` 元素身份：读出时得到 `rune`，写入时只接受 `rune`。

#### 2.4.1.1 定长数组 `[T; N]`

`[T; N]` 是定长布局数组类型，表示 `N` 个 `T` 元素。`N` 是类型的一部分，必须能在分析期求值为正的编译期整数表达式；当前支持整数字面量、`const` 整数标识符、括号、一元 `-`/`~`，以及整数算术/位运算。当前后端编码上限为 65535 个元素。

定长数组可用于 struct inline 字段和局部变量，并支持 struct、嵌套定长数组和引用容器等元素类型，因此可以递归组合：

```xray
var bytes: [byte; 4] = [1, 2, 3, 4]
var zero: [byte; 64] = [0; 64]
var names: [string; 2] = ["a", "b"]
var blocks: [[byte; 2]; 2] = [[1, 2], [3, 4]]
```

有目标类型的数组字面量初始化 `[T; N]` 时必须 exact-length；重复初始化 `[value; N]` 的 `N` 同样是正的编译期整数表达式，并且必须和目标类型长度一致。无上下文的普通数组字面量仍推断为动态 `Array<T>`；无上下文的 `[value; N]` 推断为 `[T; N]`。

定长数组支持 `len(array)`、索引读取、索引写入、`ref`/`in` 参数传递，以及目标类型为 `Slice<T>` 时通过切片产生借用视图：

```xray
var data: [byte; 4] = [5, 6, 7, 8]
var view: Slice<byte> = data[1:4]
view[1] = 99
```

```xray
struct Packet {
    magic: [byte; 4]
    payload: [byte; 128]
}

var key: [byte; 4] = [1, 2, 3, 4]
key[1] = 9

fn first(packet: Packet) -> byte {
    return packet.magic[0]
}
```

`[T; N]` 与 `Array<T>` 语义不同：

- `[T; N]`：定长、值语义、固定布局，适合 struct inline 字段、局部小缓冲、FFI/freestanding 数据。
- `Array<T>`：动态长度、可增长、堆上容器。
- `Slice<T>`：借用连续存储的视图，不拥有数据。

旧的 `[N]T` 语法不属于 Xray 语言。

#### 2.4.2 `Map<K, V>`

哈希字典，**保持插入顺序**。详见 §14.8。

**Map 字面量**必须用 `#{ ... }` 前缀，分隔符用 `:`（与 Record / Json 对象一致，靠 `#` 前缀消歧）：

```xray
var m: Map<string, int> = #{"a": 1, "b": 2}
var m2 = #{"a": 1, "b": 2}
var empty = #{}                                     // 空 Map

m["c"] = 3                                          // 添加/修改
var v = m["a"]                                      // 取值；键不存在时 panic E0431
var maybe = m.get("missing")                        // 安全查询；不存在返回 null
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

去重集合。详见 §14.9。

```xray
var s: Set<int> = #[1, 2, 3]
```

#### 2.4.4 `Channel<T>`

协程间通信通道。命名通道句柄**必须**用 `shared` 创建（见 §10.5）。

```xray
shared ch: Channel<int> = Channel<int>(10)
```

#### 2.4.5 `Array<byte>`

类型化字节缓冲。语义等价 `Array<byte>`，但底层是连续内存。

```xray
var buf = Array<byte>(1024)
var init = Array<byte>([72, 101, 108, 108, 111])
```

#### 2.4.6 `Record` / `Json` 与对象字面量

裸对象字面量默认推断为 sealed structural `Record`，用于普通业务对象、options 和多字段返回值。`Json` 是显式 opt-in 的 JSON 值域类型：它用于外部数据交换边界，可以装载 JSON 等价的任意结构，并且本身包含 `null`。

**对象字面量** `{ field: value, ... }` 与 Map 字面量的关键区别：

```xray
// Record/Json 对象字面量：标识符或字符串 key + 冒号 ':'
var data: Json = { name: "Alice", tags: ["a", "b"], age: 30 }
var user = { name: "Bob", age: 25 }       // 默认类型为 sealed Record
typeName(user)                            // "Record"
data.name              // 类型: Json（字段访问返回 Json）
data["name"]           // 等价

// 字段简写：当字段名与变量名相同
var name = "Alice"
var age = 30
var user = { name, age }                  // 等价 { name: name, age: age }

// Map 字面量：`#{}` 前缀 + `:`
var m = #{"k1": 1, "k2": 2}           // 类型: Map<string, int>
```

**对照表**：

| 写法 | 类型 | 备注 |
|---|---|---|
| `{ name: "x", age: 1 }` | sealed anonymous `Record` | 标识符或字符串 key 后跟 `:` |
| `var j: Json = { name: "x" }` | `Json` object | 只有显式 `Json` 期望类型时按动态 Json 解释 |
| `{ x: y }`（`x` 是字段名，`y` 是变量名） | sealed anonymous `Record` | 字段简写 `{ x }` 等价 `{ x: x }`，仅裸 key |
| `#{"a": 1}` | `Map<K, V>` | `#` 前缀消歧，分隔符用 `:` |
| `Point{x: 1.0, y: 2.0}` | `Point`（struct） | 类型名 + `{...}` 字面量 |

**Record 类型**：裸对象字面量和 `type T = {...}` 都是 Record。默认 Record 是 sealed——访问/赋值未声明字段是编译错误；需要 JSON 边界时显式标注 `Json` 或调用 `Json.encode(value)`。

```xray
type User = { name: string, age: int }

var u: User = { name: "Alice", age: 30 }
print(u.name)         // OK
// u.extra = "x"      // 编译错误：sealed type User has no field 'extra'

var u2 = { name: "Alice", age: 30 }      // sealed Record
// u2.extra = "x"     // 编译错误

var j: Json = { name: "Alice", age: 30 } // 动态 Json object
j.extra = "x"        // OK（Json 是动态的）
```

#### 2.4.7 `BigInt`

任意精度整数。见 §14.8。

#### 2.4.8 `Range`

`Range` 表示整数区间，由 `a..b` 或 `a..=b` 产生：

- `a..b` 是半开区间 `[a, b)`，不包含终点 `b`。
- `a..=b` 是闭区间 `[a, b]`，包含终点 `b`。

```xray
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

#### 2.4.9 `DateTime` / `Regex` / `StringBuilder`

`Regex` 与 `StringBuilder` 是 prelude 类型。`DateTime` 不是 prelude 名字，必须通过 `import { DateTime } from datetime`（或其它显式 import）进入当前作用域。成员索引见 §14。

#### 2.4.10 `WeakMap` / `WeakSet`

`WeakMap` 的键、`WeakSet` 的元素必须是堆对象；弱引用不会延长对象的生命周期。弱集合不提供会长期持有元素的遍历回调。

### 2.5 可空类型

`T?` 是 `T | null` 的语法糖。

```xray
var x: int? = null      // OK
var y: int? = 42        // OK
var z: int = null       // 编译错误：null 不是 int
```

`Json` 本身包含 `null`，因此 `Json?` 与 `Json | null` 是语义重复并在解析阶段报错。解析失败使用 typed error enum 通过 `throw`/`catch` 值返回通道传播；若失败必须作为普通数据保存或返回，则使用领域 ADT 或含显式状态字段的 Record。不要引入全局 `Result<T,E>`。

**可空原始类型一等公民**：`int?` / `float?` / `bool?` 与其它 `T?` 一样是合法类型，泛型与容器会自然产生它们（如 `Map<string, bool>.get(k) -> bool?`、`fn find<T>(...) -> T?` 在 `T = bool` 时）。它们以 tagged 表示承载 `null`，因此 `null` 值在 `print` / `string()` / 字符串拼接中统一显示为 `"null"`（不是底层数值 `0`），VM 与 AOT 一致。

> `bool?` 是三态（`true` / `false` / `null`）。它合法，但**不能直接作条件**（裸 `if (b)` where `b: bool?` 是编译错误，见 §5 / 任务 128）；需显式写 `b == true` / `b != null` / `b ?? false`。

#### 解包

```xray
// 1. 空合并
var v = x ?? 0

// 2. 可选链
var nameLen = name == null ? null : len(name!)

// 3. 强制解包
var v: int = x!           // 若 x 为 null，运行时抛 NullError

// 4. is 检查
if (x is int) {
    // 此分支内 x 类型窄化为 int
    print(x + 1)
}
```

### 2.6 Union 类型

```xray
var v: int | string = 42
v = "hello"             // OK
```

约束：
- 最多 **6 个成员**（编译期检查；超限 → 错误）。
- 成员互不为彼此的子类型（否则会被规范化）。
- 处理 union 值需用 `match` 或 `is` 窄化：

```xray
var v: int | string = ...
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
var t = (1, 2, 3)                 // 类型推断为 (int, int, int)
var h = (10, "hi", true)          // 异构元组
var single = (99,)                // 单元素元组：注意尾逗号

// 类型注解
var p: (int, string) = (7, "ok")

// 字段访问：.N（N 是编译期常量整数下标）
var first = t.0                   // 1
var mid   = t.1                   // 2
var nest  = ((1, 2), (3, 4))
var a     = nest.0.0              // 1
var b     = nest.1.1              // 4

// 函数返回与解构
fn divmod(a: int, b: int) -> (int, int) { return (a / b, a % b) }
var (q, r) = divmod(17, 5)        // tuple destructure

// 泛型
fn pair<A, B>(a: A, b: B) -> (A, B) { return (a, b) }
var p2 = pair(1, "x")             // (int, string)
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
type Pair<T> = { first: T, second: T }
type Mapper2<T, U> = (T) -> U
```

别名是**纯语法**等价，不产生新类型，也不产生运行时元数据或 AOT 分支。泛型别名在使用处按类型实参做语法代入：

```xray
var p: Pair<int> = { first: 1, second: 2 }  // 等价于 { first: int, second: int }
var f: Mapper2<int, string> = (n) -> string(n)
```

泛型别名形参只允许名字列表（`<T, U>`）；不带约束。需要约束时应放在使用该别名的泛型函数、class / struct / enum / interface 声明上。别名可前向引用，但循环别名（包括递归对象别名）是编译错误。

### 2.9 类型推断

详见 §7.4。简述：

```xray
var x = 1               // x: int
var y = 1.5             // y: float
var z = "hello"         // z: string
var a = [1, 2, 3]       // a: Array<int>
var m = #{"a": 1}    // m: Map<string, int>
var p = { name: "A" }   // p: { name: string } —— 结构化对象类型
var f = (x: int) -> x   // f: (int) -> int —— 箭头参数必须标注
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
> var full = { name: "A", age: 18 }
> var u: User = full       // OK：full 是 User 的超集
> ```

#### 2.10.2 显式 `as`

```xray
var n = x as int        // 失败抛 TypeError
var n = x as int?       // 失败返回 null（安全转换）
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

### 2.11 typeOf / typeName / Type 枚举

```xray
typeOf(value)     // 返回 Type 枚举值（int 表示）
typeName(value)   // 返回类型名字符串
```

`Type` 枚举成员：

`Type.int`、`Type.float`、`Type.string`、`Type.bool`、`Type.null`、
`Type.Array`、`Type.Map`、`Type.Set`、`Type.Channel`、`Type.Json`、
`Type.function`、`Type.class`、`Type.struct`、`Type.enum`、`Type.module`、`Type.bigint`、...

使用 `typeName(value)` 可以取得具体值的调试类型名。

### 2.12 元数据与类型身份边界

Xray 默认只保留最小类型身份层：

- `typeOf(x)` 返回稳定的 `Type` / `TypeId`，适合分支、`match` 和 analyzer narrowing。
- `typeName(x)` 返回调试/日志用的类型名字符串，是冷路径能力。
- 名义类型判断使用 `x is T` / `x as T`，不要通过字符串比较类型名。
- 字段/方法/构造器遍历不属于默认运行时能力；序列化、inspect、RPC schema 等结构化元数据由 `@derive(...)` 或编译期工具显式生成。

运行时类型查询使用 `typeOf(value)`、`typeName(value)` 和 `TypeId`。反射元数据不会暴露为可遍历、可调用的对象图。

---

## 3. 表达式 (Expressions)

> 真值源：`src/frontend/parser/xparse_expr.c`、`src/frontend/parser/xast_types.h` 的 `AST_BINARY_*` / `AST_UNARY_*` / `AST_TERNARY` / `AST_*` 等节点。

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
    fn malloc(n: uintsize) -> MutPtr<byte>
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

```xray
var v = nullable_expr ?? default_value
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
- 不能赋值给 `shared`（同上）。

**特殊**：
- 函数参数中的 `in T` 修饰符使参数变只读，对其赋值是编译错误。
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

### 3.6 空合并 `??` 与可选链 `?.` / `?[`

详见 §3.3.5（`??`）与下方（`?.` / `?[`）。

#### 可选链 `?.` / `?[`

```ebnf
OptionalChain ::= Primary ('?.' Identifier | '?.' '(' ArgList? ')' | '?[' Expr ']')+
```

```xray
var nameLen = name == null ? null : len(name!)
var item = arr?[0]              // 可选索引
var value = callback?.(input)   // 可选函数调用
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
- **类型守卫**：分析器在分支内窄化 `v` 的静态类型。
- 适用于 union、可空、class 层级、`Json` 结构匹配。

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

```xray
0..10                  // 0..10，左闭右开（包含 0，不包含 10）
0..=10                 // 0..=10，闭区间（包含 0 和 10）
var r = 1..100
var n = 10
for (i in 0..n) { print(i) }
for (i in 0..=n) { print(i) }
```

- 类型 `Range`（仅 int 范围）。
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
- **对象/record 字面量展开**：`{...base, x: 1}`，展开源必须是对象；字段合并成新对象，同名字段后者覆盖前者，结果字段集为各展开源字段与字面量字段的并集。

```xray
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

```xray
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

```xray
var m = #{"a": 1, "b": 2}
var empty = #{}                           // 空 Map
```

**关键区别**：`{}` 始终是**Json / Object**；`#{}` 始终是 **Map**。两者都用 `:` 作键值分隔，靠 `#` 前缀区分。

#### Set `#[...]`

```ebnf
SetLit ::= '#[' (Expr (',' Expr)* ','?)? ']'
```

```xray
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

```xray
var p = { name: "Alice", age: 30 }
var users = "Bob"
var obj = { users }              // shorthand
```

- 默认推断为 sealed structural `Record`（见 §2.4.6），字段集和字段 offset 在编译期固定，适合 AOT 快路径。
- 只有显式 `Json` 期望类型时才按动态 Json object literal 解释；typed value 进入 JSON 边界使用 `Json.encode(value)`。
- 用 `type` 别名命名 Record：`var u: User = {...}`（编译期检查字段集，密封）。

#### Array<byte> `Array<byte>(...)`

详见 §2.4.5 与 §14.5。

#### Channel `Channel<T>(buf?)`

```xray
shared ch: Channel<int> = Channel<int>(10)
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

```xray
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

```xray
arr[1:4]                // 元素 [1,4)
arr[:3]                 // 前 3 个
arr[2:]                 // 从索引 2 到末尾
arr[:]                  // 全切片（浅拷贝）
var view: Slice<int> = arr[1:4]
```

- 半开区间 `[start, end)`。
- Array 切片支持负索引：负数先按 `len(array) + index` 从末尾计数，再夹到合法范围。
- string 不支持 slice operator；使用严格 rune ordinal 的 `s.slice(start, end)`。
- 切片是目标类型为 `Slice<T>` 的 scoped borrowed view，不修改 owner。

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

// ── 箭头 lambda：任意位置，支持多参数和参数类型注解 ──
var sum = arr.reduce((acc, x) -> acc + x, 0)    // 无类型
var double = (x: int) -> x * 2                   // 有类型
var add = (a: int, b: int) -> a + b              // 多参数

// ── fn 表达式：多语句体、返回类型注解、泛型参数 ──
var inc = fn(x: int) -> int {
    var y = x + 1
    return y
}
var identity = fn<T>(x: T) -> T { return x }     // 泛型
```

**三种形式的选择指南**：

| 形式 | 语法 | 适用场景 |
|------|------|----------|
| 裸 lambda | `x -> expr` | 单参数回调，最简洁 |
| 箭头 lambda | `(x, y) -> expr` | 多参数、需参数类型注解、或非调用参数位置 |
| fn 表达式 | `fn(x: T) -> R { ... }` | 多语句体、返回类型注解、泛型参数 |

**关键规则**：
- **裸 lambda**（`x -> expr`）：仅限**调用参数位置**，单参数无括号。参数类型由被调函数签名或容器元素类型推断。
- **箭头 lambda**（`(x) -> expr`、`(x, y) -> expr`）：任意位置可用。参数类型可省略，由上下文推断；推断失败时报 E0365。箭头 lambda **不支持返回类型注解**；需要显式返回类型时，用 `fn(x: T) -> R { ... }`，或给绑定写函数类型：`var f: (T) -> R = (x) -> ...`。
- **fn 表达式**（`fn(x: T) { ... }`）：任意位置可用。支持泛型参数 `fn<T>(...)`、返回类型注解 `-> T`、多语句体。
- 单表达式形式 `-> expr` 自动 `return`。
- 块形式 `-> { ... }` 或 `{ ... }` 用显式 `return`。
- 捕获规则：见 §7.4。`go` 协程闭包消费统一的 provenance-based capture plan：inline、module-readonly 与 shared identity 可直接捕获；execution-local graph、module-mutable state 和生命周期不足的 view/pointer 会被拒绝，必须通过参数显式 `copy(...)` / `move` 或改用 `shared`。

### 3.13 `match` 表达式

```ebnf
MatchExpr ::= 'match' Expr '{' MatchArm (',' MatchArm)* ','? '}'
MatchArm ::= Pattern ('if' Expr)? '->' Expression
```

```xray
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

```xray
var p = Point(1.0, 2.0)
var arr = Array<int>()
shared ch = Channel<int>(10)
var m = Map<string, int>()
```

**用于**：
- 类与 struct 实例化（`TypeName(args)`）。
- 容器内置类型构造（`Array`/`Map`/`Set`/`Channel`/`Array<byte>`/`StringBuilder` 等，同样是 `TypeName(args)`）。
- 消歧由 analyzer 按符号种类判定：类型名构造，函数名调用（命名约定：类型大写、函数小写）。

**与字面量的关系**：
```xray
var a = [1, 2, 3]              // 等价 Array<int>() + push
var m = #{}                    // 等价 Map<...>()
var p = Point{x: 1, y: 2}      // struct literal
```

### 3.15 字符串插值

详见 §1.6.5。简要：

```xray
"Hello, ${name}! Age: ${user.age + 1}"
```

- `${...}` 内任意表达式（含函数调用、对象访问、算术）。
- `${...}` 内的字符串字面量可使用与外层模板相同的引号；lexer 按表达式大括号深度匹配，并跳过内层字符串 / raw string / rune 字面量。
- 表达式类型必须可转为字符串（实现 `toString()` 或为基本类型）。

### 3.16 `yield` 语句

```xray
yield expr                  // 生成器产出一个值并挂起
```

`yield expr` 只能出现在声明返回 `Iterator<T>` 的生成器函数体内。第一次调用生成器函数不会立即执行函数体，而是返回一个惰性 `Iterator<T>`；`for-in` 通过 `hasNext()` / `next()` 拉取，每次 `yield expr` 产出一个 `T` 并暂停到下一次拉取。

协作让出 CPU 使用 `Coro.yield()`（见 §10.10）；裸 `yield` 不是表达式。

---

## 4. 语句 (Statements)

> 真值源：`src/frontend/parser/xparse_stmt.c`、`src/frontend/parser/xast_nodes_stmt.h`。

Xray 语句以 `\n` 或 `;` 分隔；语句末尾的 `;` 在大多数位置可省略，仅 `for` 循环的初始化/条件/步进三段必须用 `;` 分隔。

### 4.1 表达式语句与块

```ebnf
ExprStmt   ::= Expression (';' | LineBreak)
IncDecStmt ::= Identifier ('++' | '--') (';' | LineBreak)
Block      ::= '{' Statement* '}'
```

```xray
foo()                  // 表达式语句
x = 1                  // 赋值表达式作为语句
x++                    // 自增语句；不产生表达式值
{                      // 块
    var y = 2
    y + 1              // 表达式但结果被丢弃
}
```

`++` / `--` 是纯语句或 `for` 步进项，只能写作 `name++` / `name--`。它们等价于 `name = name + 1` / `name = name - 1`，没有返回值；`var y = x++`、`f(x++)`、`a[i++]`、`return x++` 等表达式位置均编译失败。

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
- 条件必须是 `bool` 或 `T?`（`T != bool`）存在性检查；`bool?` 与裸 `int` / `string` / 集合等均为编译错误（见 §2.3.3）。
- 分支体必须是块 `{...}`，**不允许**单语句省略括号。
- `if` 不是表达式；要表达式形式用三元 `? :` 或 `match`。

### 4.3 `while`

```ebnf
LoopLabel ::= Identifier ':'
WhileStmt ::= LoopLabel? 'while' '(' Expression ')' Block
```

```xray
var i = 0
while (i < 10) {
    print(i)
    i++
}
```

无 `do-while` 形式。

### 4.4 `for`（C 风格）与 `for-in`

#### C 风格 `for`

```ebnf
ForStmt ::= LoopLabel? 'for' '(' ForInit? ';' Expression? ';' ForStep? ')' Block
ForInit ::= VarDecl | ExprStmt
ForStep ::= Expression | Identifier ('++' | '--')
```

```xray
for (var i = 0; i < 10; i++) {
    print(i)
}
for (var j = 100; j > 90; j--) {
    print(j)
}
```

- `ForInit` 中声明的变量作用域限于循环体。
- 步进项里的 `i++` / `i--` 必须是整个 step；若要多个更新，写在循环体末尾。
- 三个部分都可省略：`for (;;)` 是无限循环。

#### `for-in` 单变量

```ebnf
ForInStmt ::= LoopLabel? 'for' '(' Identifier 'in' Expression ')' Block
```

```xray
for (item in [1, 2, 3]) { print(item) }
for (i in 0..n) { print(i) }                  // 范围迭代（半开区间）
for (ch in "hello") { print(ch) }             // 字符串字符（按 Unicode scalar）
for (key in someMap) { print(key) }           // Map 单变量 → key
for (key in someJson) { print(key) }          // Json 单变量 → key
// for (day in Color) { ... }                 // 编译错误：enum 类型本身不可迭代
for (_ in 0..n) { count++ }                   // 占位符忽略
```

#### `for-in` 双变量解构

xray 支持两种等价的双变量形式：

```ebnf
ForInPairStmt ::= LoopLabel? 'for' '(' Identifier ',' Identifier 'in' Expression ')' Block
              |  LoopLabel? 'for' '(' '(' Identifier ',' Identifier ')' 'in' Expression ')' Block
```

```xray
// 形式 A：直接两标识符（更常见）
for (k, v in someMap) { print("${k}=${v}") }     // Map → (key, value)
for (i, e in someArray) { print("${i}: ${e}") }  // Array → (index, element)
for (i, c in "hello") { print("${i}:${c}") }     // string → (index, rune)

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
| `string` | `rune` | (index, rune) |
| `Range`（`a..b`） | int | — |
| Enum 类型 | **不可迭代**；需要编译器生成或用户提供的显式 case 表 | — |
| 自定义 `Iterator<T>` | T | — |

#### 自定义迭代器

实现 `iterator()` 方法返回 `Iterator<T>` 协议对象（含 `hasNext()` 和 `next()`）即可在 `for-in` 中使用。详见 §5.3.6。

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
continue               // 进入最内层循环的下一次迭代
break outer            // 跳出标签为 outer 的循环
continue outer         // 进入标签为 outer 的循环的下一次迭代

outer: for (i in 0..10) {
    for (j in 0..10) {
        if (j == 3) { continue outer }
        if (i * j > 20) { break outer }
    }
}
```

**约束**：
- 必须在 `while` / `for` 内部；否则编译错误 `E0304` / `E0305`。
- `match` 内部的 `break` / `continue` **不**作用于 `match`，而是跳出包裹 `match` 的循环。
- 循环标签写作 `label: for (...)` 或 `label: while (...)`，只能标在循环上；`label:` 后接非循环语句是编译错误。
- `break label` / `continue label` 必须引用当前活跃的外层循环标签；未知标签或同一活跃循环栈中重复标签是编译错误。
- 无标签 `continue` 作用于最内层循环；带标签 `continue` 作用于目标循环：`while` 重新检查条件，C 风格 `for` 执行 step 后再检查条件，`for-in` 进入下一项。

### 4.7 `return`

```ebnf
ReturnStmt ::= 'return' ReturnValue? (';' | LineBreak)
ReturnValue ::= Expression | '(' Expression (',' Expression)+ ')'
```

```xray
fn done() {
    return                 // 隐式返回 ()（Unit）
}

fn answer() -> int {
    return 42
}

fn pair(a: int, b: int) -> (int, int) {
    return (a, b)          // 多返回值，必须用括号包裹元组
}
```

> **注意**：多返回值必须用元组形式 `return (a, b)`；裸逗号 `return a, b` 是编译错误（`E0801`）。

**约束**：
- 只能在函数体内（含闭包）；顶层 return 是编译错误 `E0306`。
- 返回值类型必须与函数声明的返回类型兼容。

### 4.8 `throw` / `try` / `catch`

```ebnf
ThrowStmt     ::= 'throw' Expression

TryStmt       ::= 'try' Block CatchClause+
CatchClause   ::= 'catch' '(' Identifier (':' Type)? ')' Block
                | 'catch' 'panic' '(' Identifier ')' Block
```

```xray
enum AppError { NotFound, Timeout(ms: int) }

// 可恢复错误：enum 值经值返回通道传播，由 catch (e) 捕获
try { throw AppError.NotFound } catch (e) {
    match (e) {
        AppError.NotFound -> log.error("not found"),
        AppError.Timeout(ms) -> log.error("timeout after ${ms}ms")
    }
}

// catch panic (p) 是运行时故障（除零、越界、expr!）的独立边界
try { risky() } catch panic (p) {
    log.error("fault:", p)
}

throw AppError.NotFound                      // 值返回错误通道
// 没有 finally；用 defer 做确定性清理（见 §4.9）
```

**语义**：
- `try` 必须至少跟一个 `catch` 或 `catch panic` 子句。
- `catch (e)` 捕获经值返回通道传播的可恢复错误（用户 `throw <enum>`）；用 `match (e)` 解构错误值。
- `catch panic (p)` 捕获运行时故障（除零、越界、`expr!`、`assert`），与可恢复错误严格分离。
- `throw` 的操作数是错误值（通常为 enum），经值返回通道传播：不分配 `PanicInfo`、不展开栈；需要传播或捕获错误的调用边界只经过可预测分支。
- 没有 `finally`：用 `defer`（§4.9）做确定性清理。
- 完整错误语义见 [§8](#8-错误处理-error-handling)。

### 4.9 `defer`

```ebnf
DeferStmt ::= 'defer' (Expression | Block)
```

```xray
fn read_file(path: string) -> string {
    var f = open(path)
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
- `defer` 绑定到包含它的**最近真实块** `{ ... }`。函数体本身也是块，因此写在函数体顶层的 `defer` 仍在函数退出前执行。
- **LIFO**：同一块内多个 `defer` 按声明的逆序执行。
- **必执行**：所属块正常结束，或通过 `break`、`continue`、`return`、值错误传播、panic 展开退出时都执行。
- 循环体内的 `defer` 每轮迭代结束时执行，不会堆积到函数尾。
- `defer` 是 Xray 唯一的确定性清理机制（取代其他语言的 `finally`）：它绑定词法块退出边，而不是整个函数的单一栈尾。
- `defer` 中抛出的错误会**取代**当前正在传播的错误（参考 Go 语义）。

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

```xray
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

```xray
const PI = 3.14159
const MAX_LEN: int = 1024
```

- **必须**有初值。
- 不能重新赋值（编译错误 `E0303`）。
- 类型可推断或显式标注。
- `const` 和 `var` 一样，每条声明绑定一个名字或解构模式。多个独立名字使用多条声明；相关值可用 `const (a, b) = pair` 解构。

#### 5.1.3 `shared` — 共享身份绑定

```xray
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

```xray
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

```xray
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

- 默认值在**调用点**求值：省略某个尾部参数时，编译器在该调用点补入默认表达式，按实参顺序、每次省略调用各求值一次。
- 显式传 `null` 就是传入 `null`，**不会**触发默认值（默认值只在参数被省略时使用）。
- 有默认值的参数必须在尾部连续出现。
- 默认参数只作用于**具名函数/方法/构造器的直接调用**。通过函数值（函数类型变量）的间接调用不携带默认表达式，必须传入全部实参。

#### 5.2.3 多返回值

```xray
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

| 参数模式 | 语义 |
|--|--|
| 无 | 按值传递（struct 拷贝） |
| `in` | 按只读引用传递（不拷贝、不可写） |
| `ref` | 按可变引用传递（不拷贝、可写、修改可见）；调用点必须写 `ref place` |
| `out` | 按输出位置传递；调用点必须写 `out place`，callee 必须在正常返回前写入 |

#### 5.2.5 rest 参数

```xray
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
        return Animal(name)
    }
}

var a = Animal("Rex")
print(a.speak())
print(Animal.create("Bob").name)
```

#### 5.3.2 继承

```xray
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

```xray
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

```xray
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

```xray
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

```xray
Color.Red.name        // "Red"          变体名 (string)
Color.Red.ordinal     // 0              声明顺序 tag (int，从 0)
Color.Red.toString()  // "Color.Red"    "<EnumName>.<VariantName>" 格式
```

enum 值提供 `name`、`ordinal` 与 `toString()`。需要遍历全部 case 时，应使用显式生成 metadata 的 `CaseIterable` 风格能力。

#### 5.6.5 遍历

enum 默认不可 `for-in` 遍历：

```xray
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
export publicFn, VERSION                    // 后置 export 已声明标识符列表
export { name1, name2 as alias } from "./other"
export * from "./other"
```

**xray 不支持** JavaScript 默认导入 `import name from "module"`。使用 `import "module" as name`、`import module` 或 `import { name } from module`。

完整规则、路径解析、可见性细则见 [§11 模块系统](#11-模块系统-modules)。

---

## 6. 模式 (Patterns)

> 真值源：`src/frontend/parser/xparse_match.c`、`src/frontend/analyzer/xanalyzer_visitor_pattern.c`、`src/ir/xi_lower_expr.c` / `xi_lower_stmt.c` 与 VM/AOT 的 match lowering。

模式出现在 `match` 表达式/语句与 `var` / `const` 解构中。

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

### 6.2 范围模式 `a..b` / `a..=b`

```xray
match (age) {
    0..13 -> "child"
    13..20 -> "teen"
    20..=65 -> "adult"
    _ -> "senior"
}
```

- `a..b` 为半开区间 `[a, b)`；`a..=b` 为闭区间 `[a, b]`。
- 仅整数。

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

// Option 模式（位置）
match (opt) {
    Option.Some(v) -> print("got:", v),
    Option.None    -> print("nothing"),
}

// 通配符跳过 payload 中不关心的字段
match (event) {
    NetEvent.Error(code, _) if (code >= 500) -> throw NetErr.ServerFault(code),
    _                                         -> continue,
}

// 嵌套解构
match (msg) {
    Option.Some(NetEvent.DataReceived(bytes)) -> process(bytes),
    Option.None                               -> skip(),
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
    DataReceived(bytes: Array<byte>),
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

- 守卫表达式必须是 `bool` 或 `T?` 存在性检查（见 §2.3.3），与 `if` / `while` 条件规则一致。
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
- 解构中可用于跳过位置：`var [_, b, _] = arr`。

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
var [a, b, c] = some_array
var (q, r) = divmod(17, 5)
var { name, age } = user
```

详见 §5.1.5。`match` 支持 tuple、ADT variant、对象与数组结构解构：

```xray
match (p) {
    { x, y } -> ...           // 对象字段解构（簡寫绑定）
    { name: n, age } -> ...   // 对象字段重命名 + 簡寫混用
    [a, b, ..rest] -> ...     // 数组解构，`..rest` 捕获尾部为新数组
    [_, mid, _] -> ...        // 元素位通配
}
```

- 对象模式匹配任意带这些字段的对象/Json；字段读取对缺失字段安全（为 `null`）。字段子模式可为可反驳模式（如 `{ mode: 2 }`）。
- 数组模式按**长度**匹配：无 `..rest` 时要求长度恰等于元素数，有 `..rest` 时要求长度 ≥ 元素数。元素子模式只能是绑定或通配（`..rest` 之外的元素不做按值测试——元素越界读取会陷入 panic）；需要按元素值判断时改用 `if` 守卫。
- or-pattern `|` 暂不支持（逗号多值已覆盖等价能力）。

### 6.10 穷举性与匹配失败

- 对 enum 表达式的 `match` 强制穷举（错误码 `E0371`，见 §6.3.3）。
- 其他类型不强制；运行时无分支匹配 → 抛 `PanicInfo` 错误码 `E0442`（见 §18.x）。
- 建议总是提供 `_` 兜底。

---

## 7. 作用域与名字解析 (Scoping)

> 真值源：`src/frontend/analyzer/xanalyzer_symbol.c`、`xanalyzer_escape.c`、`xaddressability.c`、`xanalyzer_visitor_stmt.c` 与 `src/analysis/xglobal_producer.c` 的 capture/storage plan。

### 7.1 词法作用域与提升

Xray 采用**词法作用域**：名字的可见性由源代码结构决定。

**作用域类型**：

| 作用域 | 触发 | 示例 |
|--|--|--|
| 模块 | 每个 `.xr` 文件 | 顶层 `var` `const` `shared` `fn` `class`（模块级 `owned` 被拒绝） |
| 函数 / 闭包 | `fn` / 箭头函数进入 | 参数 + 函数体 |
| 块 | `{...}` | `if` `while` `for` `match` 分支体 |
| `scope` 块 | `scope { ... }` 关键字 | 显式词法作用域 + 结构化并发（见 §10.7） |
| `for` 头 | `for (var i=0; ...)` | `i` 仅循环体可见 |
| `catch` 参数 | `catch (e)` | `e` 仅 catch 体可见 |
| 类体 | `class` 定义 | 字段、方法 |

**提升规则**：

- 顶层 `fn` `class` `struct` `interface` `enum` `type` **提升**至当前作用域顶部——可在定义前引用。
- `var` / `const` / `shared` / 局部 `owned` **不提升**——必须在定义后使用。
- 同名重复声明：同作用域内 2 个同名变量 → 编译错误（嵌套作用域可 shadow）。

```xray
main()                    // OK：使用提升后的 fn
fn main() { ... }

var y = x                 // 错误：x 未声明
var x = 10
```

#### Shadow 规则

嵌套块可 shadow 外层同名变量：

```xray
var x = 1
{
    var x = "hello"           // shadow：OK
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
    var count = 0
    return fn() -> int {
        count += 1                  // 修改外层 count
        return count
    }
}

var c = make_counter()
print(c())      // 1
print(c())      // 2
```

- 闭包与原变量**共享**。
- 外层作用域退出后，被捕获变量由闭包 cell / upvalue 与相应引用计数继续保活。

#### 闭包优化

编译器会分析 upvalue：
- 仅读 → 可能隐式复制（避免闭包转换）。
- 读写 → 提升为闭包 box。
- 详见 §17.5。

### 7.3 所有权与 move

Xray **不**是全面 ownership/borrow checker 语言（不像 Rust）。但在**跨协程数据传递**中使用 move 语义：

```xray
var big_buffer = Array<byte>(1024 * 1024)

var t = go fn(b: Array<byte>) -> int {
    return process(b)
}(big_buffer)             // 编译错误：owned heap 值不能裸跨协程传递

var t2 = go fn(b: Array<byte>) -> int {
    return process(b)
}(move big_buffer)        // OK：所有权转移

print(len(big_buffer))    // 编译错误：move 后访问
```

**move 使用场景**：`move` 是一元所有权转移表达式，常见于实参、初始化器与返回值（参见 §5.1.4 / §10.8）：

- `go f(move x)`、`go fn(...){...}(move x)`：把所有权转给协程。
- `ch.send(move data)`：跨协程发送时转移所有权（避免拷贝）。
- 普通函数调用 `f(move x)`：把所有权传入函数（被调函数独占）。
- `owned dst = move src` / `shared dst = move src` / `return move src`：把局部 `var` 或 `owned` 身份转给新的 owner 或调用方。

### 7.4 协程数据传递规则（避免数据竞争）

"保证编译期消除数据竞争"是 xray 并发模型的核心设计原则。

所有跨 execution 边界（`go` 闭包、`go` 实参、Channel send、deferred task、线程入口和导出 callback）消费同一个已验证 capture plan。合法性由值的 **storage owner、provenance、可变性与类型表示**共同决定，不由 `var` / `const` 关键字单独决定：

| Capture action | 适用值 | 边界行为 |
|---|---|---|
| inline value | 标量、不可变小值 | 直接复制位表示 |
| deep copy | 显式 `copy(x)` 的 owned graph | 在目标 execution owner 中物化独立图 |
| move | 显式 `move x` 的可转移局部 `var` 或 `owned` | 转移所有权并静态废弃源绑定 |
| module readonly | 已冻结并发布的模块只读值 | 保留模块只读 owner，不复制到 root/task heap |
| shared ref | `shared`、Channel、Task、Atomic 等稳定共享身份 | 保留 shared/system owner 的引用 |
| reject | execution-local graph、可变 module state、悬垂 slice/pointer/upvalue | 编译错误并报告 owner/provenance 与所需显式动作 |

因此，局部 `const` 若只含 inline 值可以直接跨界；若它仍指向 execution-local graph，则仍需显式 `copy(...)`，且因为 `const` 不能作为 move 源，不能写成 `move constValue`。模块级 `const` 只有在完成冻结与发布后才属于 module readonly；模块级 `var` 属于 module mutable，并不因“全局可见”而自动线程安全。

```xray
var local = 0
go { local += 1 }                        // ❌ 编译错误：不能捕获可变局部变量
```

#### 正确姿势

```xray
// 方法 1：显式复制 owned graph
var arr = [1, 2, 3]
var t = go fn(data: Array<int>) -> int {
    data.push(4)            // 拷贝上修改，不影响原值
    return len(data)
}(copy(arr))
print(arr)                  // [1, 2, 3] 未变

// 方法 2：shared 零拷贝只读（可被捕获）
shared config = { rate: 100 }
var t2 = go fn(c: Json) -> int {
    return c.rate
}(config)

// 方法 3：move 转移所有权
var big = Array<byte>(1024)
var t3 = go fn(b: Array<byte>) -> int {
    return process(b)
}(move big)
// big 在此处不可访问

// 方法 4：Channel 通信（可被捕获）
shared ch = Channel<int>(10)
var t4 = go fn(c: Channel<int>) -> int {
    return match (c.recv()) {
        Recv.Value(v) -> v
        _ -> 0
    }
}(ch)
ch.send(42)
```

### 7.5 内存管理与对象生命周期

Xray 采用多层内存管理：

| 存储 | 机制 | 释放时机 |
|--|--|--|
| 模块只读存储（顶层 `const`） | consteval rodata，或 module allocator 初始化后 freeze + publish | 模块卸载 |
| 模块可变存储（顶层 `var`） | module owner；默认不具备并发安全性 | 模块卸载 |
| 共享存储（`shared`） | shared/system owner + 原子引用计数 | 最后共享引用释放 |
| coroutine-local heap（一般局部对象） | per-coroutine heap + 编译器插入的引用计数 + Bacon–Rajan cycle collector | 最后强引用释放时立即回收；强引用环由 cycle collector 回收；coroutine 结束时批量释放剩余 Region 块和大对象 |
| 栈（`struct` 值、本地） | 词法存储期 | 作用域退出；语言没有用户可见的确定性析构 / `Drop` hook |
| Arena（底层临时分配） | 批量释放 | arena 结束 |

**内存观察点**：
- 普通局部对象由编译器插入 retain/drop；最后一个强引用释放时进入 RC 销毁路径。
- 编译器只把可能形成引用环的类型标为 cycle candidate；相应对象在 RC 降低但仍存活时进入候选根集合。
- cycle collector 由显式 `runtime.collectCycles()` 或候选根数量达到自适应阈值触发。
- cycle collector 只遍历 coroutine-local RC 边，并跳过 shared/atomic、runtime-managed 和 Region 对象；它不是并发 tracing GC。

---

## 8. 错误处理 (Error Handling)

> 真值源：`src/frontend/analyzer/xanalyzer_errorset.c`、`src/ir/xi_lower_stmt.c`、`src/vm/xvm_dispatch_exception.inc.c`、`src/runtime/object/xpanic_info.c`、`stdlib/types/panic_info.xr`。

### 8.0 设计哲学：值返回 + panic 边界

Xray 的错误处理分为两个严格分离的通道：

| 通道 | 语法 | 适用场景 | 运行时开销 |
|--|--|--|--|
| **值返回通道**（`throw <enum>` / `try` / `catch`） | 业务错误、可恢复失败 | **低开销**（不分配 `PanicInfo`、不 unwind；需传播/捕获错误的调用边界只有可预测分支） |
| **panic 通道**（`catch panic`） | 运行时故障（越界、除零、不完整 match） | 有限栈展开 |

设计原则：

- **错误是值**：`throw <enum>` 把枚举值写入返回通道，不展开栈、不分配 PanicInfo 对象。
- **panic 不是错误**：panic 表示程序 bug 或运行时不变量违背，不应用于业务逻辑。
- **函数签名不标 `throws`**：xray 不引入 Java/Swift 的受检异常语义。错误通过 throw/catch 值返回通道处理。
- **`defer` 替代 `finally`**：xray 没有 `finally` 关键字，资源清理统一用函数作用域的 `defer`（Go 模型）。

### 8.1 值返回错误通道

#### 8.1.1 `throw` 语句

`throw expr` 抛出一个枚举错误值。`expr` 必须是枚举类型的变体值：

```xray
enum AppErr { NotFound, InvalidInput(string) }

throw AppErr.NotFound                       // ✅ 简单枚举变体
throw AppErr.InvalidInput("bad format")     // ✅ 带载荷的 ADT 枚举变体
```

抛出后行为：

```
抛出点 → 写入 pending_error → 沿调用栈返回 → 沿途执行 defer → catch 处理 → 否则继续返回 → 顶层诊断
```

- 不展开栈帧（不同于传统异常的 unwind）
- 正常路径不分配对象、不展开栈；在需传播或捕获错误的调用边界只经过可预测的错误标志分支
- 未捕获的顶层错误打印 `[Uncaught Error] <enum value>`，退出码 = 1

#### 8.1.2 `try` / `catch`

```xray
enum IOErr { Timeout, Refused(string) }

try {
    connect(host)
} catch (e) {
    match (e) {
        IOErr.Timeout -> log("timeout"),
        IOErr.Refused(reason) -> log("refused: " + reason),
    }
}
```

**执行顺序**：

1. 执行 `try` 块
2. 若有 `throw` 逃出，按声明顺序逐一尝试匹配 `catch` 子句；首个匹配者执行其块体
3. 无 `catch` 匹配时，错误继续沿调用栈向上传播

**类型化 catch 与多 catch 子句**：

`catch` 变量可带类型注解 `catch (e: T)`，运行时用 `is T` 判断枚举类型是否匹配。支持多个 `catch` 子句，按声明顺序匹配：

```xray
enum NetErr { Timeout, Refused }
enum DbErr { ConnLost, QueryFailed(string) }

try {
    riskyIO()
} catch (e: NetErr) {
    log("network:", e)
} catch (e: DbErr) {
    log("database:", e)
} catch (e) {
    log("unexpected:", e)
}
```

**规则**：
- 无类型注解的 `catch (e)` 是 catch-all，匹配所有错误值
- 有类型注解的 `catch (e: SomeErr)` 仅当错误值 `is SomeErr` 为真时匹配，其中 `SomeErr` 是具体 enum 错误类型
- 多个 `catch` 子句按声明顺序匹配，首个匹配者执行
- 若所有类型化子句均不匹配且没有 catch-all，错误继续向上传播
- 一个 `try` **必须**至少跟随 `catch`（普通 `catch` 或 `catch panic`）

#### 8.1.3 重抛与错误转换

`catch` 块内可重抛原错误或抛出不同枚举的新错误：

```xray
enum LowErr { Fail }
enum HighErr { Upstream }

try {
    lowLevelCall()
} catch (e: LowErr) {
    log("low-level failed")
    throw HighErr.Upstream
}
```

#### 8.1.4 错误枚举设计推荐

定义业务错误推荐使用 ADT enum，载荷携带上下文信息：

```xray
enum HttpErr {
    NotFound(string),
    ServerError(int, string),
    Timeout,
}

enum ParseErr {
    Empty,
    InvalidChar(string, int),
    Overflow,
}

fn fetchUser(id: int) -> User {
    if (id <= 0) { throw HttpErr.NotFound("user not found") }
    // ...
}

try {
    var user = fetchUser(-1)
} catch (e: HttpErr) {
    match (e) {
        HttpErr.NotFound(msg) -> log("404:", msg),
        HttpErr.ServerError(code, msg) -> log(string(code) + ":", msg),
        HttpErr.Timeout -> log("timeout"),
    }
}
```

ADT enum 可让 `match` 在编译期检查错因穷举性。

#### 8.1.5 错误的协程边界

值返回通道的错误**不自动跨协程传播**。子协程内未捕获的错误：

- 子协程终止，错误记录到 `coro.error`
- 父协程**不**自动感知

传递子协程错误的方式：

1. **Channel 显式传递**：

```xray
enum WorkerErr { Failed(string) }
shared err_ch = Channel<string>(1)

go {
    try {
        riskyWork()
        err_ch.send("ok")
    } catch (e) {
        err_ch.send("error")
    }
}

var result = match (err_ch.recv()) {
    Recv.Value(v) -> v
    _ -> "error"
}
if (result != "ok") { log("worker failed") }
```

2. **结构化并发 `linked scope`**（推荐，见 §10.5）：子协程的错误自动传播给父 scope，按正确的通道路由（值返回通道的 enum 错误通过 `catch` 捕获）。

### 8.2 Panic 通道

#### 8.2.1 什么是 panic

panic 表示**程序 bug 或运行时不变量违背**，不应用于业务逻辑：

- 数组越界访问
- 整数除以零
- 不完整的 match（non-exhaustive）
- 空引用解引用
- 运行时类型断言失败

panic 通过有限的栈展开传播，生成 `PanicInfo` 对象携带堆栈信息。

#### 8.2.2 `catch panic`

`catch panic` 捕获 panic 通道的运行时故障：

```xray
try {
    var arr: Array<int> = [1, 2, 3]
    var v = arr[10]                          // 越界 → panic
} catch panic {
    log("runtime fault caught")
}

try {
    var n = 10 / 0                           // 除零 → panic
} catch panic {
    log("division by zero caught")
}
```

**`catch` 与 `catch panic` 可共存**，分别处理两个通道：

```xray
enum AppErr { InvalidArg }

try {
    process(input)
} catch (e: AppErr) {
    log("business error:", e)                // 值返回通道
} catch panic {
    log("runtime fault!")                    // panic 通道
}
```

#### 8.2.3 `PanicInfo` 类

`PanicInfo` 现在**仅用于 panic 通道**。运行时故障发生时，VM 自动构造 `PanicInfo` 对象：

```xray
@native
class PanicInfo {
    message: string             // 人类可读消息（如 "index out of bounds"）
    stack: Array<string>        // 自动捕获的调用栈
    cause: PanicInfo?           // 链式 cause
    code: int                   // 错误码
    data: Json                  // 附加数据；无数据时为 JSON null

    constructor(message: string = "", cause: PanicInfo? = null)
    fn toString() -> string
}
```

用户代码一般不直接构造 `PanicInfo`——业务错误用 `throw <enum>`。

### 8.3 `defer` — 资源清理

`defer` 是块作用域的清理语句，在所属块退出时**保证执行**（无论正常结束、`break` / `continue`、`return`、`throw`、还是 panic）。语法见 §4.9。

```xray
fn fetch(url: string) -> string {
    var conn = open(url)
    defer conn.close()                       // 无论后续如何，conn 一定关闭

    var data = conn.read()
    if (len(data) == 0) {
        throw FetchErr.Empty                 // defer 仍执行
    }
    return data
}
```

**规则**：
- `defer` 绑定最近的真实 `{}` 块；函数体顶层 `defer` 仍在函数退出时执行
- 同一块可有多个 `defer`，按 **LIFO** 顺序执行
- `defer` 在块正常结束、`break`、`continue`、`return`、`throw`、panic 展开时均执行
- 循环体中的 `defer` 每轮迭代退出时执行
- `defer` 块内不应抛出错误（行为未定义）

### 8.4 Optional 与错误处理

`T?` 是 `T | null` 的语法糖，用于"二态：有值/无值"场景。详见 §2.5。与错误处理的关系：

- **不带错因的失败**：用 `T?`（如字典查找返回 `null` 表示"无此键"）
- 与 `??`（默认值）/ `?.`（可选链）/ `e!`（强制解包）配合
- 不要把 `T?` 当通用错误返回——需要错因时用 `throw <enum>` + `catch`

### 8.5 决策树：选择哪种机制

按"**调用方对失败的处理需求**"选择：

```
失败需要被调用方处理吗？
│
├─ 不需要（致命、不可恢复、运行时 bug）
│   ↓
│   panic（由运行时自动触发；catch panic 用于最外层兜底）
│
├─ 需要，且失败有结构化错因
│   ↓
│   throw <enum>，catch 处理（低开销值返回通道）
│
├─ 需要，但失败只表示"没值"，无错因意义
│   ↓
│   T? + ?? / ?.
│
├─ 需要，且函数有 ≥3 种正常状态
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
| 业务错误、可恢复失败 | `throw <enum>` + `catch` | `throw ParseErr.Empty` |
| 字典查找、可选字段 | `T?` | `map.get(k) -> Value?` |
| 运行时故障兜底 | `catch panic` | 数组越界、除零 |
| 多分支结果 | enum | `nextEvent() -> NetEvent` |
| 主结果 + 元数据 | tuple | `parse(s) -> (Ast, int)` |

### 8.6 常用模式

#### 模式 1：enum 错误 + defer 资源清理

```xray
enum ConnErr { Refused, Timeout, Reset }

fn fetchData(host: string) -> string {
    var conn = connect(host)
    defer conn.close()

    if (!conn.isAlive()) { throw ConnErr.Timeout }
    return conn.read()
}

fn main() {
    try {
        var data = fetchData("api.example.com")
        print(data)
    } catch (e: ConnErr) {
        match (e) {
            ConnErr.Refused -> print("connection refused"),
            ConnErr.Timeout -> print("timeout"),
            ConnErr.Reset -> print("connection reset"),
        }
    }
}

main()
```

#### 模式 2：throw + catch 用于库 API

```xray
enum ConfigErr { BadJson(string), BadField(string) }

fn parseConfig(text: string) -> Config {
    var json = parseJson(text)
    var port = json["port"].toInt()
    if (port == null) { throw ConfigErr.BadField("port") }
    return Config(port: port!)
}

fn main() {
    try {
        var cfg = parseConfig(configText)
        startServer(cfg)
    } catch (e: ConfigErr) {
        match (e) {
            ConfigErr.BadJson(msg) -> print("invalid JSON:", msg),
            ConfigErr.BadField(f) -> print("bad field:", f),
        }
    }
}

main()
```

#### 模式 3：`??` 提供默认值

```xray
var port = config?.port ?? 8080
var user = db.findUser(id) ?? guestUser
```

#### 模式 4：catch panic 兜底运行时故障

```xray
fn safeDivide(a: int, b: int) -> string {
    try {
        return string(a / b)
    } catch panic {
        return "error: division by zero"
    }
}
```

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

```xray
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

**内置约束接口**：

| 接口 | 含义 |
|---|---|
| `Comparable` | 可用 `<` `<=` `>` `>=` 比较；int/float/string/Comparable 实现者 |
| `Hashable` | 可作为 `Map` 键或 `Set` 元素；内置 `int` / `float` / `string` / `bool` / `enum` / `BigInt` 默认满足，用户类型必须同时提供 `operator==(other: Self) -> bool` 与 `hash() -> int` |
| `Stringable` | 可调 `.toString()`；几乎所有内置类型默认实现 |
| `Iterable<T>` | 可被 `for-in` 遍历；Array、Map、Json、string、Range 与自定义 `iterator()`；enum 类型本身不可迭代 |

`Hashable` 是静态契约：具体 class / struct / enum 用作 `Map<K, V>` 的键、`Set<T>` 的元素，或声明 `implements Hashable` 时，编译器必须看到非 `static`、非 `private` 的 `operator==(other: Self) -> bool` 与 `hash() -> int`。只提供旧式 `hashCode()` 不满足契约；只提供 `==` 或只提供 `hash()` 也会编译失败。若键/元素是类型参数，类型参数本身必须显式声明 `: Hashable`，例如 `fn f<K: Hashable>(m: Map<K, int>)`。

**当前限制**：
- 约束只能位于类型参数后，不支持 where 子句。
- 不支持**高阶类型**（`F<_>` 作为参数）。
- 不支持默认类型参数（`<T = int>`）。
- 接口实现仍需**显式 `implements`**（在类声明位置，不是约束位置，详见 §5.4）。

### 9.3 类型推断与显式实例化

#### 类型推断

```xray
identity(42)                    // T 推断为 int
Box("hello")                // T 推断为 string
Pair("key", 100)            // K=string, V=int
```

推断算法是**双向推断**：
- 从参数推断（调用位置实参类型 → 类型参数）。
- 从返回值推断（上下文期望类型 → 类型参数）。

#### 显式实例化

在推断失败或需要明确时：

```xray
var empty = Array<int>()              // 无元素可推
var m = Map<string, int>()
var result = identity<float>(0)            // 0 默认 int，强制 float
```

### 9.4 特化与 monomorphization

**实现策略**：构建期 monomorphization（单态化），按泛型种类采用不同表示策略。

- **函数泛型**：编译器收集具体调用点，按运行时表示做 rep-sharing。当前表示组为 I64 / F64 / PTR / BOOL，同一函数最多生成 4 个表示版本；同为 PTR 表示的引用类型共享一份函数体，避免因引用类型数量导致代码体积爆炸。
- **class / struct 泛型**：逐具体类型组合完整单态化，按 mangled name 去重，不按 PTR 表示合并。`Box<string>` 与 `Box<MyClass>` 即使同为 PTR 表示也保留不同类型身份，以保证字段布局、调试类型名与名义类型语义精确。
- 名字修饰（name mangling）：`identity<int>` → `identity$i64`，`Pair<string, int>` → `Pair$str$i64`。
- 单态化实例总数受 `XR_MONO_MAX_INSTANCES = 256` 保护，防止递归/组合爆炸。
- 编译期严格类型检查保证安全；冷路径类型名元数据可在启用 names/debug profile 时保留具体类型参数显示信息。

> 真值源：`src/frontend/analyzer/xanalyzer_mono.c`（单态化 pass）、`xanalyzer_mono.h`（API）。

**性能影响**：
- 函数泛型 rep-sharing 让 AOT 在 I64 / F64 / BOOL 等值表示上生成无装箱 fast path，同时让引用类型共享 PTR 版本。
- class / struct 泛型不做 rep-sharing 会增加代码和元数据体积（大致按“类型组合数 × 类体积”增长），但换来精确布局、调试类型名保真和按类型特化；未来体积敏感场景可考虑对纯 PTR class 泛型增加显式 opt-in rep-sharing。
- 内置特化容器（`Array<int>`、`Array<byte>`）进一步避免装箱开销。
- 跨模块泛型在构建期 whole-program / LTO 阶段展开；提供泛型定义的库必须保留可分析的 IR/AST 形态，不能只发布不透明预编译产物。

**当前缓项**：
- 声明点方差标注（`out T` / `in T`）、默认类型参数和 `where` 子句本轮不提供；容器默认保持不变性，这是 AOT 友好且安全的基线。

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
render(Square())     // OK
// render(Wrong())   // 编译错误：Wrong 不是 Drawable
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

### 9.7 泛型与类型身份

由于 monomorphization，每个具体实例化都有独立的类/函数定义。运行时类型判断使用名义身份，调试输出通过 `typeName` 的冷路径名字表提供：

```xray
class Container<T> {
    items: Array<T>
}
var c = Container<int>()
print(c is Container<int>)     // true
print(typeName(c))             // "Container<int>" when type names are enabled
```

结构化字段/方法元数据不会由默认运行时自动提供；需要 inspect/serialization 等能力时应使用显式 derive 或编译期生成。

---

## 10. 并发与协程 (Concurrency)

> 真值源：`src/coro/xcoro*.c`、`src/coro/xtask*.c`、`src/coro/xchannel.c`、`src/coro/xscope*.c`、`src/frontend/analyzer/xanalyzer_escape.c` 与 `docs/rules/design-principles.md`。

xray 的并发是**协程 (goroutine 风格) + Channel + 强静态约束**。设计目标：写 `go { ... }` 就和写普通函数一样简单，但**编译期保证不发生数据竞争**。

### 10.1 协程模型

| 维度 | 选择 |
|--|--|
| 调度模型 | M:N（用户态协程 + 多 OS 线程） |
| 调度策略 | 协作式（后向跳转、Channel、`await`、`Coro.yield()` 等调度/挂起点）+ work-stealing |
| 栈模型 | Stackless（每协程独立 VM 值栈 + 帧数组，按需增长，无原生 C 栈） |
| 创建开销 | ~微秒级（初始 VM 值栈约 64 槽 + 4 帧，非原生栈） |
| 上下文切换 | VM 上下文切换（保存/恢复 VM 帧），无原生栈切换、无 syscall |

协程默认分布在多个 worker 线程上；运行时根据 CPU 核数自动设置 GOMAXPROCS 风格的并行度。

### 10.2 `go` — 启动协程

```ebnf
GoExpr   ::= 'go' GoOptions? (Block | CallExpr | LambdaExpr CallArgs?)
GoOptions ::= '(' GoOption (',' GoOption)* ')'
GoOption ::= 'name' ':' StringLiteral
```

`go` 是**表达式**，返回 `Task<T>` 句柄。三种形式都合法：

```xray
// 形式 1：调用一个已声明的函数
var t1 = go worker(0, channel)

// 形式 2：调用一个 lambda 字面量（用于内联逻辑 + 显式传参）
var t2 = go fn(d: Json) -> int {
    return d.value * 2
}(payload)

// 形式 3：块形式（隐式包装为零参 lambda）
var t3 = go {
    return compute()
}

// 可选调试名称
var named = go(name: "worker-1") worker(1, channel)
```

**move 在参数位置**：跨协程转移普通局部所有权通过参数前缀 `move` 实现，**不是** `go` 的选项；`shared` 绑定是稳定共享身份，不能 `move`：

```xray
var data = { value: 10 }
var task = go fn(d: Json) -> int {
    return d.value + 1
}(move data)        // 把 data 的所有权移交给协程；之后 data 不可访问
```

**块形式限制**：`go { ... }` 是隐式零参 lambda，没有参数列表，也不会绕过统一 capture plan。inline 值、已发布的 module-readonly 值和 shared/system owner 身份可直接捕获；execution-local graph、module-mutable 状态及生命周期不足的 view/pointer 会被拒绝。需要跨界复制或转移局部数据时，使用带参数的 lambda / 函数调用形式并显式写出 `copy(...)` / `move`：

```xray
var n = 10
var task = go fn(x: int) -> int {
    return x + 1
}(n)
```

**语义**：
- 每个 `go` 表达式都返回一个 `Task<T>`，其中 `T` 是被调函数的返回类型；返回 `()` 的函数对应 `Task<null>`。
- 协程在闲置 worker 线程中调度（M:N）。
- `go(name: ...)` 只设置调试名称，不影响调度顺序。
- 协程内**未捕获**异常存在 `Task` 中，由 `await` 时重抛。
- 跨协程传递需要隔离的 owned heap 值（`Array` / `Map` / `Set` / `Json` / `Array<byte>` / `StringBuilder` 等）必须显式 `copy(x)`、`move x` 或声明 `shared`，**裸传是编译错误**；标量、`string`、`shared`、Channel / Task / Atomic 等可直接传。`move` 只适用于可重新绑定的局部 `var` 值，`shared` 绑定不能被 move。`go` 实参与 `ch.send`、`select` 发送分支共用同一显式 transfer 规则，每次边界传递都能从源码看出复制、转移或共享语义。
- `go { ... }` 块形式等价于零参 lambda，只能使用符合协程捕获规则的外部状态；传参请用 `go fn(x: T) -> R { ... }(arg)` 或 `go worker(arg)`。

### 10.3 `await` — 等待结果

```ebnf
AwaitExpr ::= 'await' Expression
           |  'await' 'all' Expression       // 等待全部完成
           |  'await' 'any' Expression       // 等待任一完成
```

```xray
// 单 task
var task = go fetch("https://example.com")
var result = await task                    // 让出当前协程直到 task 完成

// await all：等待全部完成，返回结果数组（与输入顺序一致）
var t1 = go compute(2)
var t2 = go compute(3)
var t3 = go compute(4)
var results: Array<int> = await all [t1, t2, t3]
// 也可直接对变量使用，无需中括号
var tasks = [t1, t2, t3]
var results2: Array<int> = await all tasks

// await any：等待任一完成，返回该任务结果；其他任务继续运行
var first = await any [t1, t2, t3]

// await anySuccess：跳过失败任务，等待第一个成功的任务
var firstOk = await anySuccess [t1, t2, t3]
```

**语义**：

- `await` 仅作用于 `Task<T>` 类型；其他类型为编译错误。
- 当前协程**让出**直到目标完成（不阻塞 OS 线程）。
- 异常传播：
  - `await t` 重抛 t 抛出的异常。
  - `await t` 成功时返回 `T`；如果 `T` 是 `T?`，返回的 `null` 是任务真实结果，不代表取消或失败。
  - `await all` 中任一任务抛异常即整体抛异常（其余任务会被取消）。
  - `await any` 仅当**全部失败**时抛异常；只要有一个完成，返回该任务结果。
  - `await anySuccess` 类似 `await any`，但**跳过**抛异常的任务，只等成功完成的。
- `all` / `any` / `anySuccess` 在 `await` 后面是**上下文关键字**，仅在此位置生效。
- `await all` 的输入必须是同构任务集合：每个元素都必须是同一静态 `Task<T>` 类型，结果类型为 `Array<T>`。异构任务（如 `Task<int>` 与 `Task<string>` 混合）不会自动擦除或装箱；需要逐个 `await`，或在任务内部显式转换为统一 enum / union / Json 结果类型。

### 10.4 `Task<T>` 句柄

`go expr` 返回 `Task<T>`，其中 `T` 为被调函数的返回类型。Task 句柄支持：

| 方法 / 属性 | 类型 | 说明 |
|--|--|--|
| `t.done` | `bool`（属性） | 任务是否已完成（成功、失败或取消） |
| `t.status` | `TaskStatus`（属性） | `Pending` / `Running` / `Success` / `Failed` / `Cancelled` |
| `t.cancel()` | `() -> ()` | 请求取消任务（合作式） |
| `t.poll()` | `() -> TaskResult<T>` | 非阻塞观察；未完成返回 `TaskResult.Pending` |
| `t.awaitResult()` | `() -> TaskResult<T>` | 阻塞等待并返回状态结果，不重抛异常 |
| `t.awaitTimeout(ms)` | `(int) -> TaskResult<T>` | 阻塞到完成或超时，超时返回 `TaskResult.Timeout` |

```xray
var t = go fetch(url)
if (!t.done) { /* 还在跑 */ }
var r = await t

match t.poll() {
    TaskResult.Pending -> print("running")
    TaskResult.Success(value) -> print(value)
    TaskResult.Failed(err) -> print(err)
    TaskResult.Cancelled -> print("cancelled")
    TaskResult.Timeout -> print("timeout")
}
```

`TaskResult<T>` 的当前公开形状为 `Success(T)`、`Failed(PanicInfo)`、`Cancelled`、`Timeout`、`Pending`。运行时故障通过 `PanicInfo` 进入 `Failed`；业务 enum 错误仍走语言的错误通道，plain `await task` 会按对应错误路径传播，调用方需要状态值时使用显式 task handle 与 `awaitResult()` / `awaitTimeout(ms)`。

**取消语义**：`cancel()` 设置取消标志；协程在下一个调度/挂起检查点（后向跳转、Channel 操作、`await`、`Coro.yield()`）检测到标志后抛出取消异常。plain `await` 已取消的 task 会抛 `TaskCancelled`；需要状态值时使用 `awaitResult()` 或 `awaitTimeout(ms)`。

**看门狗策略**：运行时监控线程（sysmon）会观察 RUNNING 协程的心跳。纯 Xray 循环在后向跳转 safepoint 推进心跳，因此会被观测为持续进展；sysmon 主要用于发现长时间 native/FFI 或无 safepoint 区域卡住。如果心跳长时间冻结，默认行为是 **warn-only**：约 100ms 后打印一次 stuck warning，但不静默取消协程。强制取消是显式 opt-in：设置环境变量 `XRAY_SYSMON_CANCEL_MS=N`（`N > 0`，单位毫秒）后，心跳冻结超过该阈值的协程会被标记取消；未设置或设为 `0` 时保持仅告警。纯 CPU 长循环可在循环内插入 `Coro.yield()`，以改善调度公平性和取消响应性。

### 10.5 Channel

```ebnf
ChannelType ::= 'Channel' '<' Type '>'
ChannelNew  ::= 'Channel' ('<' Type '>')? '(' Expression ')'
```

Channel 通常以 `shared` 声明（生命周期跨协程，引用语义）：

```xray
shared ch  = Channel<int>(10)    // 有缓冲，capacity = 10
shared ch0 = Channel<int>(0)     // 无缓冲（同步握手）
shared cha = Channel(3)          // 元素类型从首次 send 推断
```

**API**（注意全部为 **camelCase**）：

| 方法 | 签名 | 行为 |
|--|--|--|
| `send(v)` | `(T) -> ()` | 阻塞发送；满则等待消费者；channel 已关闭时抛异常 |
| `recv()` | `() -> Recv<T>` | 阻塞接收；关闭且缓冲为空时返回 `Recv.Closed` |
| `recvOr(default)` | `(T) -> T` | 阻塞接收；收到值时直接返回 `T`，关闭且缓冲为空时返回 `default`，不分配 `Recv<T>` 包装 |
| `trySend(v)` | `(T) -> SendResult` | 非阻塞发送；返回 `Sent` / `Full` / `Closed` |
| `tryRecv()` | `() -> Recv<T>` | 非阻塞接收；空时返回 `Recv.Empty` |
| `sendTimeout(v, ms)` | `(T, int) -> SendResult` | 带超时发送；超时返回 `SendResult.Timeout` |
| `recvTimeout(ms)` | `(int) -> Recv<T>` | 带超时接收；超时返回 `Recv.Timeout` |
| `close()` | `() -> ()` | 关闭 channel；幂等 |
| `isClosed` | `bool`（属性） | channel 是否已关闭 |

```xray
shared ch = Channel<int>(10)
ch.send(42)                             // 阻塞发送
var v = match ch.recv() {
    Recv.Value(value) -> value
    Recv.Closed -> -1
    _ -> -1
}

var sent = ch.trySend(99)               // SendResult.Sent / Full / Closed
match ch.tryRecv() {
    Recv.Value(next) -> print(next)
    Recv.Empty -> print("empty")
    Recv.Closed -> print("closed")
    Recv.Timeout -> print("timeout")
}

ch.send(7)
ch.close()
for (msg in ch) {
    print(msg)
}

var value = ch.recvOr(-1)
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
- 关闭后：`send` 抛异常；`recv` 返回剩余 buffered value 的 `Recv.Value(v)`，取完后返回 `Recv.Closed`；`recvOr(default)` 返回剩余 buffered value，取完后返回 `default`；`tryRecv` 在空且未关闭时返回 `Recv.Empty`。
- `for (msg in ch)` 等价于阻塞接收直到 channel 关闭且 drained；循环变量类型为 `T`。Channel 不支持 key-value 迭代。

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
shared ch1 = Channel<int>(2)
shared ch2 = Channel<int>(2)

select {
    msg from ch1 -> { print("got from ch1:", msg) }      // 接收分支
    msg from ch2 -> { print("got from ch2:", msg) }      // 接收分支
    100  to   ch1 -> { print("sent 100 to ch1") }        // 发送分支
    _ -> { print("no channel ready") }                   // 默认分支（非阻塞）
}
```

**语义**：
- 接收分支 `name from ch -> body`：在 ch 有数据时被选中，并把 `Recv.Value(name)` 的 payload 绑定到 `name`。
- 发送分支 `value to ch -> body`：等价于 `ch.send(value)`，但仅在 ch 有空间时被选中；`value` 与 `ch.send` 遵守同一显式 transfer 规则——裸 owned heap 值必须写成 `copy(v)` / `move v` 或 `shared`。
- 默认分支 `_ -> body`：当前无任何分支就绪时立即执行；**省略默认分支**会让 select 阻塞直到某个分支就绪。
- 多个分支同时就绪时**随机**选择一个（与 Go 一致）。

### 10.7 `scope` 块（结构化并发 / 词法作用域）

`scope` 是**语句关键字**，建立一个新的词法作用域块。它服务两个目的：

1. **纯词法作用域**：与 C/Rust `{ ... }` 局部块一致，块内 `var` 不影响外层同名变量。
2. **结构化并发**（语义增强）：在 `scope` 块内 `go` 启动的协程，块退出前**自动等待**全部完成或取消。

```ebnf
ScopeStmt           ::= 'scope' Block
LinkedScopeStmt     ::= 'linked' 'scope' Block          // 兄弟失败 -> 取消所有 + 重抛
SupervisorScopeStmt ::= 'supervisor' 'scope' Block      // 等待所有子协程；语句形式
```

```xray
// 词法作用域用途
var x = 1
scope {
    var x = 10            // shadow 外层 x，块内有效
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
| `supervisor scope { ... }` | 等待所有子协程完成；子协程之间互不影响 | 无（语句形式） |

`supervisor scope` 会等待块内所有 `go` 子协程完成，但不返回聚合结果。需要观察某个子任务状态时，显式保留 task handle 并调用 `awaitResult()` 或 `awaitTimeout(ms)`；把 `supervisor scope` 当表达式使用会被编译器拒绝。

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

// supervisor scope：收集每个子协程的 outcome
var outcomes = supervisor scope {
    go failing("error1")
    go failing("error2")
    go ok()
}
print(len(outcomes))                 // 3（每个子协程一个 outcome）
```

**通用语义**：
- `scope` 不是函数调用，也不需要 import；是关键字块语句。
- 三种形式都在块退出前等待所有 `go` 启动的子协程完成。

### 10.8 `move` — 跨协程所有权转移

```ebnf
MoveExpr ::= 'move' Identifier        // 仅出现在调用参数位置
```

`move` 是**实参修饰前缀**（不是 `go` 的选项）。它把可重新绑定的局部 `var` 值所有权从当前作用域转移到被调函数（包括 `go` 启动的协程、`ch.send()` 等）。move 后原变量在编译期被标记为**已 moved**，再次引用是编译错误。`const` 和 `shared` 绑定都不能作为 `move` 源。

```xray
var buf = Array<byte>(1024 * 1024)

// 移交给协程
var t = go fn(b: Array<byte>) -> int {
    return process(b)
}(move buf)
// 编译错误：buf has been moved
// print(len(buf))

// 移交给 channel
shared ch = Channel<Array<byte>>(1)
var payload = Array<byte>(4096)
ch.send(move payload)
// 编译错误：payload has been moved
```

详见 §7.3、§7.4 关于 `var` / `shared` 的协程传递规则。

### 10.9 同步原语

xray 的默认并发模型偏向**消息传递 + 显式共享身份 + 显式所有权转移**——通过 `shared`、`Channel`、`move`、`scope` 把跨协程数据边界写在源码里，因此**不**鼓励使用裸 Mutex/锁。

如确需互斥锁/原子操作，运行时层面提供：

| 原语 | 形态 | 说明 |
|---|---|---|
| Channel(1) | 单元素 channel | 互斥的最佳实践（通过 send/recv 模拟 lock/unlock） |
| `shared` | 稳定共享身份 | 由 shared/system owner 持有，作用域仍按词法规则；并发可变安全由值的类型语义决定 |
| `Atomic<T>` | 无锁原子包装 | 对 `int`/`float`/`bool` 提供 C11 原子操作 |
| `sync.Mutex<T>` / `sync.RwLock<T>` | 协程域锁 | 需显式 `import sync`；等待时挂起协程，不阻塞 worker；不得在 `sys.Thread` 线程体中使用 |
| `sys.OsMutex` / `sys.OsRwLock` / `sys.OsCondvar` 等 | OS 线程域锁 | 需显式 `import sys`；阻塞当前 OS 线程，适合 `sys.Thread`、运行时组件和短临界区 |

> **设计说明**：xray 不在 prelude 暴露裸 `Mutex`/`RwLock`。默认推荐 `Channel`、`shared`、`move` 与 `Atomic<T>`；确需锁时必须通过 `sync` 或 `sys` 显式选定执行域，避免把协程挂起锁和 OS 阻塞锁混用。

#### `Atomic<T>` — 无锁原子类型

`Atomic<T>` 包装 `int`、`float` 或 `bool`，在系统堆上分配，底层使用 C11 原子指令，无需锁即可跨协程安全读写。

**声明约束**：`Atomic<T>` 变量必须声明为 `shared`，禁止 `move`。

```xray
shared counter = Atomic(0)         // Atomic<int>
shared flag = Atomic(false)        // Atomic<bool>
shared rate = Atomic(3.14)         // Atomic<float>
```

**方法一览**（完整签名见 §14.19）：

| 方法 | 说明 |
|---|---|
| `load(ord?)` | 原子读取 |
| `store(val, ord?)` | 原子写入 |
| `add(val, ord?)` / `sub(val, ord?)` | 原子加减（int/float） |
| `fetchAdd(val, ord?)` / `fetchSub(val, ord?)` | 原子加减并返回旧值 |
| `swap(val, ord?)` | 原子交换，返回旧值 |
| `compareExchange(expected, desired, ord?)` | CAS，返回 `(old, bool)` |
| `toggle(ord?)` | 原子取反（bool），返回旧值 |
| `toString()` | 返回当前值的字符串表示 |

#### `Ordering` 枚举

所有接受 `ord?` 参数的方法均可传入 `Ordering` 枚举指定内存序，默认 `SeqCst`（最强保证）。

```xray
enum Ordering {
    Relaxed,          // 无跨线程排序保证
    Acquire,          // 读屏障
    Release,          // 写屏障
    AcquireRelease,   // 读写屏障
    SeqCst,           // 顺序一致（默认）
}
```

`Ordering` 枚举由编译器自动注入（prelude），无需 import；底层 intrinsic 读取声明顺序 tag，不依赖用户可见 backing value。

```xray
shared counter = Atomic(0)
counter.store(42, Ordering.Release)
var val = counter.load(Ordering.Acquire)
```


### 10.10 `Coro.yield()` — 让出 CPU

```ebnf
CoroYieldCall ::= 'Coro' '.' 'yield' '(' ')'
```

```xray
for (i in 0..1000) {
    do_chunk(i)
    Coro.yield()                // 主动 safepoint，让其他协程有机会跑
}
```

`Coro.yield()` 是协作式调度让出点，等价于显式 safepoint，让调度器有机会运行其他协程并响应取消。`yield expr` 已专用于生成器产值；裸 `yield` 被拒绝。

### 10.11 并发安全模型

xray 通过类型系统**编译期消除大部分数据竞争**：

| 规则 | 强制 |
|--|--|
| 所有跨 execution 边界消费同一个 provenance-based capture plan | ✅ |
| execution-local graph 必须显式 `copy` 或从局部 `var` 执行 `move` | ✅ |
| 模块只读值可保留 module owner；模块可变状态不得直接跨界 | ✅ |
| `shared` 绑定可直接跨协程传递/捕获，且不能重新赋值或 `move` | ✅ |
| `move` 只适用于普通局部 `var` 的显式所有权转移 | ✅ |
| Channel 跨协程传值 | ✅ |
| `Atomic<T>` 必须声明为 `shared`，禁止 `move` | ✅ |

**仍可能存在数据竞争**（运行时检测，非编译期）：
- 在 Channel 中发送可变 class 引用（接收方可能与发送方同时修改）— 建议总是发送 `shared` / `Array<byte>` / 不可变对象 / `move` 移交。

### 10.12 逻辑根任务与可达运行时能力

程序语义只有一个逻辑 root task；物理实现由编译器从最终产物的可达 root 集合推导：纯同步入口使用 **ELIDED**，只启动子任务但自身不挂起时使用 **DESCRIPTOR**，入口或其可达调用发生挂起时使用 **RESUMABLE_FRAME**。普通不可挂起函数始终保留普通 ABI，不隐式增加 coroutine context、frame、safepoint 或 current-task 查询。

runtime capability 只从 executable entry、共享库导出和 `@c_export` 等最终 artifact roots 传播。不可达的 `go` / `await` / Channel helper 不会迫使产物链接 scheduler、timer、netpoll 或 hosted runtime。

Hosted target 按 verified entry plan 选择 NONE / SINGLE / MULTI scheduler。Freestanding target 若可达代码只需要 core，则保持零 coroutine runtime；若需要 task、frame、submit、park/wake、timer、interrupt completion 或 executor pump，target manifest 必须提供版本化 provider ABI 及所需 hooks，缺失能力在生成或链接前硬失败。provider 是 target/build 契约，不引入 `async main`、`static main` 或 freestanding 专用源语言关键字。

---

## 11. 模块系统 (Modules)

> 真值源：`src/module/xmodule.c`、`src/module/xmodule_resolver.c`、`src/module/xmodule_graph.c`、`src/frontend/parser/xparse_import.c`。

### 11.1 模块定义

- 每个 `.xr` 文件是一个模块。
- 模块名 = 文件名（去除 `.xr` 后缀）。
- 文件模块的规范身份由 resolver 归一化后的路径决定；源码中通常通过相对路径或包路径 import，不应依赖把目录分隔符展示成点号的推导规则。

### 11.2 项目结构

```
my_project/
├── xray.toml              # 包清单（包名、依赖、main）
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
main = "src/main.xr"

[dependencies]
local_utils = { path = "../local_utils" }
```

### 11.3 `import` 语法

`import` 声明**只能出现在模块顶层**。在函数、类或任何嵌套作用域内使用 `import` 会产生编译错误。

```ebnf
ImportStmt ::= 'import' ImportMembers 'from' ImportModule
            |  'import' ImportModule ('as' Identifier)?
ImportMembers ::= '{' ImportMember (',' ImportMember)* ','? '}'
ImportMember  ::= Identifier ('as' Identifier)?
ImportModule  ::= StringLiteral | ModuleName
ModuleName    ::= Identifier
```

```xray
// 1. stdlib：裸标识符；没有 `as` 时别名等于模块名
import time
import datetime
import http as httpClient

// 2. 第三方包：引号 + owner/name 形式
import "alice/utils"
import "bob/http_client" as httpClient

// 3. 文件路径或目录路径：字符串字面量，可显式 alias，也可从路径尾段推导（不含 .xr 扩展名）
import "./modules/mod_a" as a
import "../utils/string_utils" as utils
import "models/user" as user

// 4. 命名 import：成员可重命名；`from` 后可接字符串路径或裸模块名
import { readFile, writeFile as write } from io
import { publicFn } from "./modules/mod_a"
```

**不支持**：
- JavaScript 默认导入 `import name from "module"`。使用 `import "module" as name`、`import module` 或 `import { name } from module`。
- 动态导入 `import("module")`。所有导入必须是静态声明。

**解析算法**（按优先级）：
1. **stdlib 命名解析**：裸标识符 `import time` → 内置 stdlib 模块表。
2. **相对路径**：`"./xxx"` 与 `"../xxx"` 相对当前文件解析（自动补 `.xr` 扩展名或 `index.xr` 目录入口）。
3. **项目根目录路径**：不以 `./` 或 `../` 开头的 quoted path 作为项目目录 import。
4. **第三方包**：`"owner/name"` 由 `xray.toml` 的 `[dependencies]` 解析。

**Specifier 校验**（编译错误）：
- 禁止包含 `.xr` 扩展名：`import "./a.xr"` → 错误
- 禁止末尾斜杠：`import "./a/"` → 错误
- 禁止显式 `index`：`import "./a/index"` → 错误
- 禁止绝对路径：`import "/etc/foo"` → 错误

### 11.4 `export` 与可见性

`export` 声明**只能出现在模块顶层**。xray 支持三种 export 形式：

```ebnf
ExportStmt ::= 'export' Declaration                              // 直接 export 声明
            |  'export' '{' Identifier (',' Identifier)* '}'     // export 已声明的标识符
            |  'export' '{' ExportSpec (',' ExportSpec)* '}' 'from' StringLiteral
            |  'export' '*' 'from' StringLiteral
ExportSpec ::= Identifier ('as' Identifier)?
Declaration ::= FnDecl | ClassDecl | StructDecl | ConstDecl | TypeAlias
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
export { publicFn }

// 3. 重导出（带可选重命名）
export { getUser, getUserAge as getAge } from "./user"

// 4. 通配重导出（把另一个模块的全部 export 转出）
export * from "./product"
```

**限制**：
- 未标 `export` 的声明仅模块内可见（**私有**）。
- `export var` 不被支持。可变绑定不能跨模块共享，使用 `export const` 代替。
- 模块的内部状态在不同模块中互不冲突，即使同名。
- 重导出与通配重导出常用于 `index.xr` 聚合子模块的公开 API。

### 11.5 命名约定

- 模块名 `snake_case`：`http_client.xr` / `string_utils.xr`。
- 公开符号 `camelCase` 或 `PascalCase`（类/接口）。
- 内部符号约定前缀 `_`：`_internal_helper`。

### 11.6 编译期模块图与循环依赖

xray 在编译期构建完整的**模块依赖图**（DAG）：

1. 从入口文件开始，递归解析所有 `import` 声明，构建依赖图。
2. 对依赖图进行拓扑排序，确定模块初始化顺序。
3. 如果检测到**循环依赖**（SCC 大小 > 1 或自环），产生编译错误。
4. 按拓扑序从叶子模块（无依赖）到入口模块依次初始化。

选择性导入（`import { foo } from "./m"`）在编译期解析为固定的模块索引和导出槽位，运行时为 O(1) 索引访问，不涉及字符串查找。

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
var t = time.now()
time.sleep(100)
```

---

## 12. 测试系统 (Testing)

> 真值源：`src/app/cli/xcmd_test.c`、`src/api/xtest_runner.c`、`src/frontend/parser/xparse_decl.c` 与 analyzer 的全局 assertion builtin 表。

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
    var result = compute()
    assert_eq(result, 42)
    assert(result > 0)
}
```

**语义**：
- `@test` 标注的函数会被 `xray test` 自动发现并运行；普通函数不会。
- 测试函数命名约定：`test_xxx`（snake_case），描述性命名。
- 测试函数无参数无返回值；通过 assert 系列函数表达预期。
- 同一文件可包含**任意数量**的 `@test` 函数；单文件内按声明顺序运行。多个文件可用 `-j N` 并行，每个文件使用独立 isolate。

### 12.2 测试入口

`xray test` 不强制测试目录或文件名；目录输入会递归收集所有 `.xr` 文件，并按路径排序。仓库自己的 regression suite 使用 `tests/regression/XXXX_topic.xr` 只是项目约定。

运行：

```bash
xray test tests/                           # 必须显式给出至少一个文件或目录
xray test tests/regression/01_literals/    # 整个分组
xray test tests/regression/01_literals/0100_int_basic.xr   # 单文件
xray test -j 4 tests/                      # 文件级并行
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
    var task = go fetch_data("http://...")
    var result = await task
    assert_eq(result.status, 200)
}
```

### 12.5 注解（Attributes）总览

xray 的注解前缀为 `@`，真值表在 `xparse_decl.c:xr_parse_single_attribute`。测试 runner 识别以下测试注解：

| 注解 | 说明 |
|---|---|
| `@test` / `@test(skip)` / `@test(timeout: N)` | 测试、跳过测试、单测试超时秒数 |
| `@before_all` / `@after_all` | 单文件 suite 前后各执行一次 |
| `@before_each` / `@after_each` | 每个未跳过测试前后执行 |

其它注解包括：`@deprecated("...")`；内置声明专用的 `@native`；AOT/C ABI 的 `@c_export("sym")`、`@section("name")`、`@weak`、`@used`、`@naked`、`@interrupt("abi")`、`@no_alloc`；以及 `@derive(Inspect, Json, Eq, Hash, Clone)`（其中 `Hash` 要求同时 `Eq`）。外部 C 声明使用 `extern "C" ... {}` 块。

```xray
@test                                 // 标记测试
fn test_basic() { return }

@test(skip)                           // 跳过此测试
fn test_wip() { return }

@before_each
fn reset_fixture() { resetState() }

@deprecated("use newAPI() instead")
fn oldAPI() { return }
```

> `@async`、`@override`、`@beforeEach` 等不在当前表中，会触发 `unknown attribute name`。异步能力直接在测试体内使用 `go` / `await`。

### 12.6 `xray run` / `xray test` / `xray repl`

| 命令 | 用途 |
|--|--|
| `xray run main.xr` | 执行主程序 |
| `xray test` | 运行测试套件 |
| `xray repl` | 启动 REPL |
| `xray build --native main.xr` | AOT native 编译 |
| `xray fmt` | 格式化 |

---

## 13. 内置函数 (Built-in Functions)

> 真值源：`src/ir/xi_lower_expr.c`、`src/vm/xvm_dispatch_*.inc.c`、`src/runtime/object/builtins/`、`src/frontend/analyzer/xanalyzer_builtins.c`。

不需要 `import` 即可使用的全局函数和内置构造/静态函数。下列表格中的 `value` 表示“任意运行时值”，只是文档占位符，不是源码中的类型名。

### 13.1 I/O 与调试

| 函数 | 签名 | 说明 |
|--|--|--|
| `print` | `(...values) -> ()` | 输出到 stdout，自动追加换行；多参以空格分隔 |
| `dump` | `(value, indent?) -> ()` | 结构化调试输出 |
| `len` | `(value) -> int` | 查询实现 `Lengthable` 的 string、容器、Range、Slice、Json 等长度；不读取 `.length` |

### 13.2 类型转换

| 函数 | 签名 | 说明 |
|--|--|--|
| `int(x)` | `(value) -> int` | 转为 int；`rune` 转为 Unicode scalar code point；字符串解析失败抛异常 |
| `float(x)` | `(value) -> float` | 转为 float |
| `string(x)` | `(value) -> string` | 转为字符串；`rune` 转为单 scalar 字符串 |
| `bool(x)` | `(value) -> bool` | 转为 bool；规则见 §2.3.3 |
| `rune(n)` | `(int) -> rune` | 从整数构造 Unicode scalar；surrogate 或越界值抛异常 |
| `chr(n)` | `(int) -> string` | Unicode 码点转单 scalar 字符串 |
| `copy(x)` | `(value) -> owned value` | 显式深拷贝；普通值保留类型形状，借用的 `Slice<T>` / view 则返回独立 owner `Array<T>` |

### 13.3 类型检查

| 函数 / 表达式 | 签名 | 说明 |
|---|---|---|
| `typeOf(x)` | `(value) -> Type` | 返回稳定 TypeId / `Type.xxx` 值 |
| `typeName(x)` | `(value) -> string` | 返回调试/日志用类型名字符串 |
| `typeName<T>()` | `() -> string` | 返回静态类型 `T` 的名称 |
| `x is T` | 表达式 | 运行时类型检查，分析器可做类型窄化 |

分支提示 `likely(cond)` / `unlikely(cond)` 接受并原样返回 `bool`；它们只向优化器提供概率提示，不改变求值或短路语义。

全局只读环境值不是函数：`process`（入口参数/文件/目录信息）、`__file__`、`__dir__`。它们由真实文件/项目入口初始化；纯 `eval` 场景中 `process` 可为 `null`。

```xray
var x = 42
print(typeOf(x) == Type.int)    // true
print(typeName(x))              // "int"
print(x is int)                 // true
// typeOf(x) == "int"           // compile error: use Type.int or typeName(x)
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

BigInt 使用 `123n` 字面量或 `int.toBigInt()`；Json 使用 `Json.parse` / `Json.encode` / `Json.stringify`；DateTime 使用 `datetime` 模块工厂函数。

---

## 14. 内置类型方法 (Built-in Type Methods)

> 真值源：prelude / analyzer / runtime 中的内置类型注册与方法定义。
> MCP knowledge 只消费生成后的 analyzer metadata，不独立维护内置类型方法签名。

本节按主题汇总每种内置类型的方法、签名和行为。

### 14.1 `int` 方法

| 方法 | 签名 | 说明 |
|--|--|--|
| `abs()` | `() -> int` | 绝对值 |
| `toString()` | `() -> string` | 十进制字符串 |
| `toBigInt()` | `() -> BigInt` | 转 BigInt |
| `toFloat()` | `() -> float` | 转 float |
| `toHex()` | `() -> string` | 十六进制字符串 |
| `max(other)` / `min(other)` | `(int) -> int` | 双值最值 |
| `sqrt()` | `() -> float` | 平方根 |
| `pow(exp)` | `(float) -> float` | 幂运算 |
| `checkedAdd(other)` / `checkedSub(other)` / `checkedMul(other)` | `(int) -> int?` | 溢出返回 `null` |
| `saturatingAdd(other)` / `saturatingSub(other)` / `saturatingMul(other)` | `(int) -> int` | 溢出饱和到 `int` 边界 |
| `wrappingAdd(other)` / `wrappingSub(other)` / `wrappingMul(other)` | `(int) -> int` | 显式二补码环绕 |
| `addOverflows(other)` / `subOverflows(other)` / `mulOverflows(other)` | `(int) -> bool` | 仅报告有符号溢出（要结果用 `checked*`） |
| `popcount()` | `() -> int` | 二补码位表示中置位的个数 |
| `leadingZeros()` / `trailingZeros()` | `() -> int` | 前导/后缀零比特数（`0` 返回 `64`） |
| `byteswap()` | `() -> int` | 反转字节序 |
| `rotateLeft(n)` / `rotateRight(n)` | `(int) -> int` | 循环移位（`n` 按模 64） |

`abs()` 遵循整数环绕语义：`(-9223372036854775807 - 1).abs()` 返回自身。`toHex()` 对负数使用带符号前缀，例如 `-0x8000000000000000`。位运算与溢出谓词在 VM 与 AOT 中具有相同语义。

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
| `isNaN()` | `() -> bool` | 是否为 IEEE NaN |

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

### 14.4.1 `rune` 方法

| 方法 | 签名 | 说明 |
|--|--|--|
| `toString()` | `() -> string` | 返回单 Unicode scalar 字符串 |
| `toUInt32()` | `() -> uint32` | 返回 Unicode scalar code point |
| `isLetter()` | `() -> bool` | 是否为 Unicode 字母 |
| `isNumber()` | `() -> bool` | 是否为 Unicode 数字 |
| `isAlphanumeric()` | `() -> bool` | 是否为字母或数字 |
| `isWhitespace()` | `() -> bool` | 是否为空白字符 |

`rune` 是独立原始类型，不继承整数方法；需要码点时显式使用 `toUInt32()`。

### 14.5 `string` 方法

| 成员 | 类型 / 说明 |
|--|--|
| `len(s)` | O(1) Unicode scalar 数量 |
| `bytes()` / `copyBytes()` | 借用的 `Slice<byte>` / 独立的 `Array<byte>` |
| `runes()` | `Iterator<rune>`；裸 `for (r in s)` 使用相同语义 |
| `string.fromRune(r)` | 从一个 Unicode scalar 构造字符串 |
| `string.fromUtf8(bytes)` | 复制并严格验证 `Slice<byte>`；非法 UTF-8 抛 `Utf8Error.InvalidUtf8` |
| `string.fromUtf8Lossy(bytes)` | 复制 `Slice<byte>`，非法序列替换为 U+FFFD |
| `string.join(parts, separator?)` | 拼接 `Array<string>` |
| `contains(s)` | 是否包含子串 |
| `indexOf(s, start?)` / `lastIndexOf(s)` | 返回 rune ordinal |
| `slice(start, end?)` | 按 rune ordinal 取得 owned string；范围必须合法 |
| `sliceBytes(start, end)` | 按 byte offset 切片；边界非法时抛 `StringSliceError.InvalidByteRange` |
| `split(sep, limit?)` | 分割为 `Array<string>` |
| `replace(from, to)` / `replaceAll(from, to)` | 替换 |
| `repeat(n)` | 重复 |
| `startsWith(s)` / `endsWith(s)` | 前缀/后缀判断 |
| `toString()` | 返回自身 |

string 不支持整数下标或 slice operator；显式使用 `s.runes().nth(i)`、`s.bytes()[i]` 或 `s.slice(start, end)`。字符串拼接使用 `+`；大小写、去空白、填充和反转等 Unicode 文本操作属于 `text` 模块。

### 14.6 `Array<byte>`

`Array<byte>` 是可直接使用的 `Array` 具体化，构造由 `Array<byte>(n)` / `Array<byte>(n, fill)` 等内置路径处理。它的 `toString()` 与所有 Array 一样返回容器格式；文本解码必须显式使用 `string.fromUtf8(bytes[:])` 或 `string.fromUtf8Lossy(bytes[:])`。当前没有单独的 `stdlib/types/bytes.xr` 声明；工具不要把它当成另一套与 Array 同构的独立 API。

### 14.7 `Array<T>` 方法

| 成员 | 类型/说明 |
|--|--|
| `len(arr)` | `int` 全局查询 |
| `capacity` / `arr[i]` / `arr[i] = v` | 容量属性与下标读写；也可使用 `get(i)` / `set(i, v)` |
| `push(x)` / `pop()` | 尾部增删 |
| `shift()` / `unshift(x)` | 头部增删 |
| `concat(...arrays)` | 拼接 |
| `indexOf(x)` / `contains(x)` | 查找 |
| `join(sep?)` | 拼接为字符串 |
| `reverse()` / `sort(cmp?)` | 原地重排 |
| `map(fn)` / `filter(fn)` / `reduce(fn, init)` | 函数式处理 |
| `forEach(fn)` / `find(fn)` / `findIndex(fn)` / `every(fn)` / `some(fn)` | 遍历与谓词 |
| `fill(v, start?, end?)` / `clear()` | 填充或清空 |
| `reserve(capacity)` / `resize(length, fill)` | 容量与长度管理 |
| `ptr()` / `mutPtr()` | 显式底层指针视图 |
| `toString()` | 容器字符串表示 |
| `iterator()` / `entriesIterator()` / `entries()` | 迭代协议 |

Array 没有 `slice()` / `splice()` / `flat()` / `copyWithin()` 方法。`arr[start:end]` 产生借用的 `Slice<T>`，必须有显式目标类型并遵守借用生命周期；需要独立 owned 数据时使用 `copy(arr[start:end])`。

### 14.8 `Map<K, V>` 方法

| 成员 | 类型/说明 |
|--|--|
| `len(m)` | `int` 全局查询 |
| `m[k]` / `m[k] = v` | 下标读写 |
| `get(k)` / `set(k, v)` | `get` 在缺失时返回 `null`；`set` 写入 |
| `containsKey(k)` / `containsValue(v)` / `delete(k)` / `clear()` | 查询与删除 |
| `keys()` / `values()` / `entries()` | 返回键、值、键值对 |
| `forEach(fn)` | 遍历 |
| `iterator()` / `entriesIterator()` | 迭代协议 |

**Map 字面量**：`#{"k1": v1, "k2": v2}` 或 `#{}`；使用 `:`，靠 `#` 前缀区别于 Record/Json 对象字面量。

`m[k]` 要求键存在；缺失键触发运行时错误 `E0431`。需要可选读取时使用 `m.get(k)`。

### 14.9 `Set<T>` 方法

| 成员 | 类型/说明 |
|--|--|
| `len(set)` | `int` 全局查询 |
| `add(x)` / `contains(x)` / `delete(x)` | 插入、查询、删除 |
| `clear()` | 清空 |
| `values()` | 返回 `Array<T>` |
| `forEach(fn)` | 遍历 |
| `iterator()` | 迭代协议 |

**Set 字面量**：`#[1, 2, 3]` 或 `#[]`。

### 14.10 `Channel<T>` 方法

| 成员 | 类型/说明 |
|--|--|
| `send(v)` | 阻塞发送；channel 已关闭时抛异常 |
| `recv()` | 阻塞接收，返回 `Recv<T>`；关闭且缓冲为空时为 `Recv.Closed` |
| `recvOr(default)` | 接收 payload；没有值时返回给定默认值 |
| `trySend(v)` | 非阻塞发送，返回 `SendResult` |
| `tryRecv()` | 非阻塞接收，返回 `Recv<T>`；空时为 `Recv.Empty` |
| `sendTimeout(v, ms)` | 带超时发送，返回 `SendResult`；超时为 `SendResult.Timeout` |
| `recvTimeout(ms)` | 带超时接收，返回 `Recv<T>`；超时为 `Recv.Timeout` |
| `close()` | 关闭 channel |
| `capacity` / `isClosed` | 容量和关闭状态属性 |

`Recv.Value(v)` 中的 `v` 就是 channel payload，因此 `Channel<int?>` 可以区分真实的 `Recv.Value(null)` 和 `Recv.Closed`。

### 14.11 `Json`

`Json` 是动态结构化数据类型。普通字段访问使用 `j.field` / `j["field"]`；通用查询和编解码通过 `Json` 静态函数完成，避免与用户字段名冲突。

| 静态函数 | 说明 |
|--|--|
| `Json.keys(obj)` / `Json.values(obj)` / `Json.entries(obj)` | Object 字段枚举 |
| `Json.containsKey(obj, key)` | 字段存在性 |
| `Json.get(obj, key, default?)` | 字段读取，不存在返回 default 或 null |
| `len(obj)` | Object / Array / String variant 的元素数量；scalar 抛 TypeError |
| `Json.parse(s)` / `Json.tryParse(s)` / `Json.isValid(s)` | JSON 解析与校验 |
| `Json.encode(value)` | 显式 typed value → Json 边界转换 |
| `Json.stringify(value, indent?)` | 序列化 |

**字面量**：`{ name: "alice", age: 30 }` 默认是 sealed `Record`。显式写 `var j: Json = {...}` 时才是动态 Json object；typed value 进入 JSON 边界使用 `Json.encode(value)`。

### 14.12 `Range`

`a..b` 是半开区间 `[a, b)`，`a..=b` 是闭区间 `[a, b]`；两种范围都可用于表达式、`for-in` 和 `match` 范围模式。

| 成员 | 说明 |
|--|--|
| `start` / `end` | 起点与声明的终点 |
| `contains(x)` | 按半开或闭区间语义判断 `x` 是否在范围内 |
| `toArray()` | 按迭代顺序生成独立的 `Array<int>` |
| `toString()` | 返回 `a..b` 或 `a..=b` 形式的字符串 |
| `len(range)` | 返回范围中的元素数量 |

```xray
var pages = 1..=3
print(pages.start)          // 1
print(pages.end)            // 3
print(pages.contains(3))    // true
print(len(pages))           // 3
print(pages.toArray())      // [1, 2, 3]

var empty = 5..5
print(len(empty))           // 0
```

### 14.13 `DateTime`

通过 `import datetime` 获得工厂函数：`now`、`utc`、`create`、`createUTC`、`fromTimestamp`、`fromTimestampMs`、`parse`、`offset`。`DateTime` 不是 prelude 类型；需要类型名时使用 `import { DateTime } from datetime`。

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
| `findText(s)` / `findGroup(s, index)` | 首个匹配文本 / 捕获组文本 |
| `replace(s, replacement)` | 替换 |
| `split(s, limit?)` | 分割 |

### 14.15 `StringBuilder`

| 方法 | 说明 |
|--|--|
| `len(builder)` | 当前 rune 数量 |
| `append(s)` | 追加并返回自身 |
| `toString()` | 输出字符串 |
| `clear()` | 清空并返回自身 |

### 14.16 `PanicInfo`

内置 `PanicInfo` 类包含 `message`、`stack`、`cause`、`code`、`data` 字段，构造函数 `constructor(message: string = "", cause: PanicInfo? = null)`，以及 `toString()`。

### 14.17 `Task<T>` 与 enum 值

`Task<T>` 属性：`done`、`status`；方法：`cancel()`、`poll()`、`awaitResult()`、`awaitTimeout(ms)`。`poll()` 和显式等待方法返回 `TaskResult<T>`：`Success(T)`、`Failed(PanicInfo)`、`Cancelled`、`Timeout`、`Pending`。plain `await task` 成功时返回 `T`，失败或取消时走对应错误/panic 路径。enum 值提供冷路径属性 `name`、`ordinal` 与方法 `toString()`。

### 14.18 线程与同步 handle

`Thread<T>` 是 prelude handle 类型，公开 `done` 属性以及 `join()`、`detach()` 方法。导入 `sys` 后，使用 `sys.Thread.spawn(body)` 或 `sys.Thread.spawn(ThreadOptions{...}, body)` 创建 OS 线程，并对返回的 handle 调用 `join()` 或 `detach()`。`CountdownLatch`、`EventCount`、`ResultGroup`、`Semaphore`、`WorkQueue` 等类型从 `sync` 模块导入；`Logger` 从 `log` 模块导入；连接与监听器类型从 `net` 模块导入。

### 14.19 `Atomic<T>` 方法

`Atomic<T>` 包装 `int`、`float` 或 `bool`，提供无锁原子操作。必须声明为 `shared`，禁止 `move`。

| 方法 | 签名 | 说明 |
|--|--|--|
| `load(ord?)` | `(Ordering?) -> T` | 原子读取当前值 |
| `store(val, ord?)` | `(T, Ordering?) -> ()` | 原子写入 |
| `add(val, ord?)` | `(T, Ordering?) -> ()` | 原子加 |
| `sub(val, ord?)` | `(T, Ordering?) -> ()` | 原子减 |
| `fetchAdd(val, ord?)` | `(T, Ordering?) -> T` | 原子加并返回旧值 |
| `fetchSub(val, ord?)` | `(T, Ordering?) -> T` | 原子减并返回旧值 |
| `swap(val, ord?)` | `(T, Ordering?) -> T` | 原子交换，返回旧值 |
| `compareExchange(expected, desired, ord?)` | `(T, T, Ordering?) -> (T, bool)` | CAS，返回 `(旧值, 是否成功)` |
| `toggle(ord?)` | `(Ordering?) -> bool` | 原子取反（仅 bool），返回旧值 |
| `toString()` | `() -> string` | 返回当前值的字符串表示 |

`ord?` 参数接受 `Ordering` 枚举，默认 `Ordering.SeqCst`。详见 §10.9。

---

## 15. 标准库概览 (Standard Library)

> 真值源：`stdlib/defs/*.def`、纯 Xray `stdlib/<module>/<module>.xr` export、`stdlib/types/*.xr` native type 声明，以及合并这些来源的 `scripts/gen_api_inventory.py`。
> MCP knowledge 和 API inventory 使用 source-derived inventory；`xray builtin-dump` 只作为运行时 builtin 视图输入之一。
> 详见 [附录 D stdlib 模块索引](#d-stdlib-模块索引)。

> **真实 stdlib 模块清单**（28 个，源码：`stdlib/<module>/*.c` / `stdlib/<module>/*.xr`）：
>
> `base64`、`cluster`、`compress`、`crypto`、`csv`、`datetime`、`encoding`、`http`、`io`、`log`、`math`、`mem`、`net`、`os`、`parallel`、`path`、`regex`、`runtime`、`strconv`、`sync`、`sys`、`text`、`time`、`toml`、`url`、`ws`、`xml`、`yaml`。
>
> 不需要 import 的 prelude 类型为：`Array`、`Atomic`、`OsBarrier`、`BigInt`、`Channel`、`OsCondvar`、`PanicInfo`、`Json`、`Map`、`OsMutex`、`NetConn`、`NetListener`、`OsOnce`、`Path`、`Range`、`Regex`、`OsRwLock`、`Set`、`StringBuilder`、`Thread`。`Array<byte>` 是 `Array` 的具体化；`DateTime`、`Logger` 等模块类型需要从对应模块导入。详见 §1.5.6 / §2.2。

### 15.1 文件 IO 与系统

| 模块 | 主题 | 关键 API |
|--|--|--|
| `io` | 文件 IO + 文件系统 | `readFile` `writeFile` `readFileBytes` `writeFileBytes` `exists` `mkdir` `mkdirp` `remove` `readDir` `stat` `readStdin` |
| `path` | 路径操作 | `join` `dirname` `basename` `extname` `normalize` `isAbsolute` `resolve` `relative` `parse` `format` |
| `os` | 操作系统接口 | `getenv` `setenv` `environ` `exit` `getpid` `getcwd` `chdir` `hostname` `tmpdir` `homedir` `cpuCount` `sleep` `exec`；常量 `platform` `arch` `sep` `eol` |

> xray **没有**独立的 `fs` 模块，文件系统操作在 `io` 中；进程参数 / 进程信息走全局 `process` 对象（`process.args` / `process.file` / `process.dir`，见 §16.5），不在 `os` 中。
> `os.platform` / `os.arch` / `os.sep` / `os.eol` 是**常量字符串**，不带括号；其余 `os.*` 是函数调用。

### 15.2 网络

| 模块 | 主题 | 关键 API |
|--|--|--|
| `net` | TCP / UDP / TLS socket + DNS | `listen` `dial` `accept` `read` `readInto` `write` `writeBytes` `copy` `copyBidirectional` `setDeadline` `lastError` `lookup` `dialTLS` `NetConn` `NetListener` |
| `http` | HTTP / HTTPS 客户端 + 服务端 + HTTP/2 | `request` `h2Request` `listen` `router` `routeHandler` `requestText` `responseText` `parseResponseText` |
| `ws` | WebSocket | `connect` `serve` `send` `recv` `close` `parseFrame` `parseUrl` `parseUpgradeRequest` `clientHandshakeRequest` |
| `url` | URL 解析与构造 | `URL` `QueryParams` `parse` `format` `parseQuery` `encode` `decode` |

> DNS 查询通过 `net.lookup(host)` 完成；没有独立的 `dns` 模块。

#### 15.2.1 TCP 数据路径

`net` 的 TCP API 明确区分三类数据路径：

- `read(conn)` / `write(conn, data)`：消息型路径，把 payload 暴露为 Xray `string`，适合协议解析、文本处理和需要检查内容的逻辑。
- `readInto(conn, bytes, maxlen?)` / `writeBytes(conn, bytes)`：可复用 `Array<byte>` buffer 路径，适合二进制协议热路径，避免为每个包创建临时字符串。
- `copy(src, dst)` / `copyBidirectional(a, b)`：流式 native 路径，payload 保持在可复用 C buffer 中，适合 proxy、relay、`copy(conn, conn)` echo 和其他不需要语言层查看每个字节的高吞吐场景。

设计原则：raw stream 不应为了“经过语言层”而创建临时字符串；只有业务逻辑需要看数据时才使用字符串 API。

TCP 等待操作使用协程友好的 netpoll 路径。`setReadDeadline(conn, deadline)`、`setWriteDeadline(conn, deadline)`、`setDeadline(conn, deadline)` 和 `setAcceptDeadline(listener, deadline)` 接受 `time.monotonic()` 毫秒 deadline；超时后操作按自身返回形态返回 `null` 或 `-1`，并通过 `lastError(handle)` / `lastErrno(handle)` 暴露诊断原因。

`shutdownRead(conn)`、`shutdownWrite(conn)` 和 `shutdown(conn)` 暴露 TCP 半关闭语义。通用 proxy/relay 应优先使用 `copyBidirectional(a, b)`，它会在单向 EOF 后半关闭对端写侧，并返回两个方向的字节统计。

TLS client 路径通过 `dialTLS(host, port, timeout?)` 和 `upgradeTLS(conn, hostname, timeout?)` 提供；TLS read/write/copy 与 plain TCP 共享同一套 deadline、错误诊断和 typed handle 生命周期语义。

### 15.3 数据格式

| 模块 | 主题 |
|--|--|
| `yaml` | YAML |
| `toml` | TOML |
| `xml` | XML |
| `csv` | CSV |
| `base64` | Base64 编/解 |
| `encoding` | hex / UTF-8 等通用编码（不含 Base64，base64 在自身模块） |

> JSON 编解码**不在**单独的 `json` 模块；通过内置类型 `Json` 的静态方法 `Json.parse(s)` / `Json.encode(v)` / `Json.stringify(v)` 使用（无需 import；见 §14.11）。

### 15.4 加密与哈希

| 模块 | 关键 API |
|--|--|
| `crypto` | `md5` `sha1` `sha256` `sha512` `hmac` `encrypt` `decrypt` `randomBytes` `timingSafeEqual` `uuid` |

> stdlib **没有**独立的 `random` 模块；如需伪随机数请使用 `crypto` 模块的随机源或 `math` 模块的工具函数。

### 15.5 压缩

| 模块 | 关键 API |
|--|--|
| `compress` | `gzip` / `gunzip`、`deflate` / `inflate` 等 |

### 15.6 时间

| 模块 | 关键 API |
|--|--|
| `time` | `now()` `monotonic()` `clock()` `micros()` `nanos()` `sleep(ms)` `localOffset()` `localOffsetAt()` |
| `datetime` | `DateTime` 及 `now()` `utc()` `create()` `createUTC()` `fromTimestamp()` `parse()` 等工厂（详见 §14.13） |

### 15.7 数学

| 模块 | 关键 API |
|--|--|
| `math` | `sin` `cos` `tan` `log` `pow` `sqrt` `floor` `ceil` `round` `abs` `min` `max` 等；常量 `PI` / `E` / `MAX_INT` / `MIN_INT` |

### 15.8 文本

| 模块 | 关键 API |
|--|--|
| `regex` | `compile(pattern)` 返回 `Regex`；详见 §14.14。也支持 `/pattern/flags` 字面量 |
| `text` | `lower` `upper` `trim` `trimStart` `trimEnd` `padStart` `padEnd` `reverseRunes` `translate` |
| `strconv` | `parseInt` `parseFloat` |

内置转换 `int(s)` / `float(s)` / `string(n)` 仍可用于普通转换；需要带 radix / default 的解析接口时使用 `strconv`。

### 15.9 日志与诊断

| 模块 | 关键 API |
|--|--|
| `log` | `debug` / `info` / `warn` / `error` / `fatal` / `child()`、source 位置开关、异步写入模式 |
| `runtime` | `collectCycles()` `isCycleCollectionEnabled()` `liveBytes()` `liveObjects()` `info()` |
| `mem` | `alloc()` / `allocZeroed()` / `allocAligned()` 返回受管 `Buffer`；`pageAlloc()` / `pageFree()`；`copy()` / `move()` / `set()` / `compare()`；`volatileLoad()` / `volatileStore()`；`fence()` |
| `sync` | 协程域同步：`Mutex` `RwLock` `Once` `Barrier` `Condvar` `CachePadded` `fence()` 等，需显式 `import sync` |
| `sys` | OS / 线程底层接口：编译器定义的 `sys.Thread.spawn(...)` 与 `ThreadOptions`，以及 `ThreadLocal`、`OsMutex` `OsRwLock` `OsCondvar` `OsBarrier` `OsOnce`、process/dylib/pipe handle、`cpuCount()`、`sleepMs()`、`threadYield()`、`pinToCpu()`、`onSignal()` |

### 15.10 并行

`parallel` 导出 `forEach` 以及 `Plan` 抽象，用于结构化 CPU 并行工作；语言关键字表中没有 `parallel` 关键字，需显式导入模块。

### 15.11 分布式

| 模块 | 主题 |
|--|--|
| `cluster` | 节点发现、健康检查、Topic 消息总线（见 stdlib/cluster/）|

### 15.12 测试

`@test` 注解 + 全局 `assert*` 函数即可，**不需要**额外的 `test` 模块（见 §12）。

### 15.13 已**不存在**的模块

文档中可能引用过、但当前 stdlib 中**确实没有**的模块（避免误导）：

`fs` · `process` · `dns` · `random` · `json`

这些功能或者归入其他模块（见上面各小节注），或者尚未实现。

> **完整索引**：见[附录 D](#d-stdlib-模块索引)。

---

## 16. 运行时模型 (Runtime Model)

> 真值源：`src/runtime/`、`src/vm/`、`src/runtime/mem/`、`docs/rules/architecture.md`。

### 16.1 值表示

Xray 值统一用 `XrValue` 表示。当前实现要求 64 位平台，并采用 **16 字节 tagged struct-of-union**：

- **Descriptor（8 字节）**：`tag: byte`、`flags: byte`、`heap_type: uint16`、`ext: uint32`。`tag` 是类型判定的唯一入口；`heap_type` 只在 `tag == PTR` 时表示堆对象类型。
- **Payload（8 字节）**：`int64` / `double` / 指针三选一，按 `tag` 解释。
- **无 NaN-boxing / 无指针低位标记**：整数保留完整 64 位；对象引用是普通堆指针，类型信息在 descriptor 中。
- **字符串不是值级 SSO**：`string` 始终是 `XrString` 堆对象，字符数据存放在对象内的 `data[]` flexible array 中。运行期短串默认协程本地无锁分配，仅字面量/符号、显式 `intern()` 与 map/set 键驻留全局池；跨协程边界（channel send、`go` 实参、task/scope 结果）按需提升为共享原子 RC。这些都是对象层存储策略，不改变 `XrValue` 表示。

| 值类型 | 内部表示 |
|--|--|
| `int` | `XR_TAG_I64` + 64-bit signed payload |
| `float` | `XR_TAG_F64` + IEEE-754 double payload |
| `bool` | `XR_TAG_BOOL` + `0/1` payload |
| `rune` | `XR_TAG_RUNE` + Unicode scalar payload |
| `null` | `XR_TAG_NULL` + zero payload |
| `string` | `XR_TAG_PTR` + `XR_TSTRING` + `XrString*` |
| `Array<byte>` | `XR_TAG_PTR` + `XR_TARRAY`，元素布局为 byte |
| 其他对象 | `XR_TAG_PTR` + heap type + heap pointer |

Typed array 元素布局是容器元数据的一部分。`Array<rune>` 使用 `XR_ELEM_RUNE`，数据区是连续 `uint32_t[]` Unicode scalar；load 时重新装箱为 `XR_TAG_RUNE`，store 时拒绝非 `rune` 值，因此不会与 `Array<uint32>` 混淆。

### 16.2 内存分配

| 区域 | 用途 |
|--|--|
| **系统 owner** | runtime/native 数据结构；hosted 可使用 C allocator，freestanding 由 target hooks 提供 |
| **模块只读 owner** | consteval rodata，或 module allocator 初始化后 freeze + publish 的顶层 `const` |
| **模块可变 owner** | 顶层 `var`；生命周期属于模块，默认不提供并发安全性 |
| **shared/system owner** | `shared` 稳定共享身份与显式并发句柄，原子引用计数 |
| **coroutine owner** | 普通局部对象；由当前 coroutine 的 `XrCoroHeap` 分配并执行引用计数回收 |
| **栈** | `struct` 值、局部 immediate、函数帧 |
| **Arena** | parser 临时分配、frame allocation |

### 16.3 内存模型

- 默认由编译器插入的 **per-coroutine reference counting** 回收普通局部对象；最后一个强引用释放时立即进入 RC 销毁路径。共享对象使用 atomic RC，模块/运行时对象按各自 owner 的生命周期管理。
- **循环引用回收**：编译器标记可能形成环的类型；Bacon–Rajan trial-deletion collector 处理相应的 coroutine-local 强引用环。显式入口是 `runtime.collectCycles()`，候选根数量达到自适应阈值时也会自动触发。
- **collector 边界**：cycle collector 跳过 shared/atomic、runtime-managed 和 Region 对象。函数调用与后向跳转处保留的 tracing-GC hook 当前为空操作；Xray 没有并发 tracing GC。
- **用户可见 introspection**：`runtime.liveBytes()` / `runtime.liveObjects()` / `runtime.info()` 报告当前 coroutine heap（无当前 coroutine 时回退到 main coroutine）的 live-memory 视图（`import runtime`；`mem` 模块只承载裸内存能力）。

详见 `src/runtime/mem/`。

### 16.4 协程调度

- M:N 调度（M OS 线程 × N 协程）。
- **work-stealing**：空闲 worker 从其他 worker 队列偷任务。
- **协作式抢占**：协程在 safepoint 让出（非强制抢占）。
- **公平性**：单一 runnable 队列配合本地 run-next、全局注入队列和 work-stealing；调度顺序不暴露用户级优先级。
- **栈管理**：VM 的 register/frame stack 在需要时扩容，扩容可能搬迁底层存储；运行时以 slot offset 重建指针。

详见 `src/coro/` 与 `src/vm/xvm_coro_backend.c`。

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

### 16.6 Panic 运行时

内置 `PanicInfo` 类是 prelude 类型（声明：`stdlib/types/panic_info.xr`），仅属于 **panic 通道**。运行时故障（越界、除零、不完整 `match`、运行时不变量违背等）由 VM/AOT runtime 构造 `PanicInfo` 对象：

```xray
@native
class PanicInfo {
    message: string             // 人类可读消息
    stack: Array<string>        // 自动 capture 的调用栈，每帧一行格式化字符串
    cause: PanicInfo?           // 链式 cause
    code: int                   // 错误码（从 "E0xxx: ..." 前缀自动解析，默认 0）
    data: Json                  // 运行时故障的结构化附加数据；无数据时为 JSON null

    constructor(message: string = "", cause: PanicInfo? = null)
    fn toString() -> string
}
```

用户级可恢复错误不使用 `PanicInfo`：`throw` 语句只接受 enum 变体值（见 §8.1.1）；非 enum 错误值在编译期被拒绝（错误码 `E0370`）。

栈展开（仅 panic 通道）：VM `xvm_unwind_stack()` 按 try-table 查找 `catch panic` handler，逐帧释放局部、执行 defer，到达 handler 后跳转。可恢复错误走值返回通道，不展开栈。详见 §8。

### 16.7 值返回错误通道运行时

`throw <enum>` 将枚举值写入帧的 `pending_error` 槽位，设置错误标志位并返回。调用方通过 `OP_ERR_CHECK` 检测标志位，决定进入 `catch` handler 或继续向上返回。该通道不展开栈、不分配 `PanicInfo`；在需要传播或捕获错误的调用边界，正常路径只经过可预测的错误标志分支。

### 16.8 对象回收与析构时机契约

xray **当前不提供用户可见的确定性析构（destructor / finalizer / `Drop`）**。对象的**回收时机**不属于可观察语义契约，由实现定义，且不同执行后端不同：

- 回收可能发生在"最后一个引用消失时"、"某次 GC 时"、或"进程退出时"中的任意一种，VM 与 AOT 的回收时机**不保证一致**。
- 程序**不得依赖**：(a) 对象在某个确定时刻被回收；(b) 任何析构 / finalizer 是否运行、运行顺序或运行所在线程。

唯一保证确定性、且跨后端（VM / AOT）一致的资源清理机制是 **`defer`**：它在所属作用域退出时按 LIFO 顺序执行（含 panic 展开路径），与对象回收时机无关。需要确定性释放外部资源（文件 / 句柄 / 锁）的代码必须使用 `defer`，而非依赖对象析构。

> 演进说明：当确定性析构（RAII / `Drop`）被正式纳入语言时，本节将升级为**确定性回收契约**（明确析构点与顺序），并由跨后端差分测试逐字节守门。在此之前，"回收时机 / finalizer 行为"被显式声明为实现定义的非确定项。

---

## 17. 编译流水线 (Compilation Pipeline)

> 真值源：`src/frontend/`、`src/ir/`、`src/vm/`、`src/aot/`、`src/toolchain/`、`src/app/cli/xcmd_build.c`、`src/app/cli/xcmd_compile.c`。

### 17.1 两条执行路径

```
源码 (.xr) -> lexer -> AST -> analyzer / canonical evidence
                                  |-> bytecode compiler -> XrProto -> VM
                                  `-> Xi IR -> optimization -> C source -> host/cross C toolchain -> native binary
```

Xray 当前没有 JIT。默认 `xray run` 和默认 `xray build` 使用字节码/VM 路径；`xray build --native` 才选择 AOT 路径。两条路径共享 parser、analyzer、模块图与运行时语义，但输出表示和后端优化并不相同。

### 17.2 前端

- lexer：`src/frontend/lexer/xlex.c` / `xlex.h`，关键字表为 `xkeywords.def`；输入必须是合法 UTF-8。
- parser：`src/frontend/parser/xparse*.c`，手写递归下降与优先级表达式解析，输出 `AstNode` 模块树。
- analyzer：`src/frontend/analyzer/`，执行名称解析、类型/泛型/可见性/所有权/捕获/错误集检查。
- canonical evidence：`src/frontend/canonical/` 与 module/toolchain 层把跨模块事实收敛成供后端消费的稳定证据。

### 17.3 字节码与 VM

`src/frontend/codegen/xcompiler.c` 将已分析 AST 编译成 `XrProto` 字节码。opcode 的唯一清单是 `src/runtime/value/xopcode_def.h`，VM 实现在 `src/vm/`。VM 使用寄存器式指令布局，并包含属性/调用快路径以及 coroutine、错误通道、tail-call 等专用 opcode。

`xray compile file.xr` 默认生成 `.xrc`；`--format bytecode|c|header` 可显式选择序列化字节码或把字节码嵌入 C 源/头文件。这里的 `--format c` 是**字节码容器的 C 表示**，不是 native AOT C 后端。

字节码必须以确定性顺序序列化 extern 聚合布局及目标 ABI 指纹。加载器必须在执行前校验布局深度、递归环、字段范围、总大小、尾随数据和 ABI 一致性；任何损坏或目标不匹配都必须拒绝加载，不能回退到宿主机布局。

### 17.4 Xi IR 与优化

native 路径将程序 lowering 为 `src/ir/` 中的 Xi IR。优化流水线由 `xi_pipeline.c` / `xi_pass.h` 组织，当前实现包括 SCCP、DCE/CFG 简化、内联、devirtualization、tail-call、escape/ownership、循环优化、GVN/PRE、边界检查消除和向量化等 passes；具体启用集合由优化等级、目标和合法性检查决定，不能把某个 pass 的存在理解为每个程序都保证发生该优化。

### 17.5 Native AOT

`xray build --native file.xr` 经 `src/aot/` 的 prepare/verify/representation/container/link plan，把 Xi IR 生成 C，再调用 host、Clang 或 Zig C toolchain 编译链接。hosted native 仍链接 Xray runtime；`--profile freestanding` 使用受限的 freestanding capability 集。`--target`、`--toolchain`、`--cpu`、`--lto` 等选项以 `xray build --help` 为准。

native AOT 不是直接从 SSA 发射机器码，也不是 JIT；最终机器码由所选 C toolchain 产生。

---

## 18. 错误码 (Error Codes)

> 唯一真值源：`src/runtime/xerror_codes.h`。`XrErrorCode` 在 `src/runtime/xerror.h` 中是 `int`；用户可见格式为 `Exxxx`。编号可保留空洞，不得用文档中不存在的名称补齐。

### 18.1 Lexer 与 parser

| 代码 | C 名称 | 含义 |
|--|--|--|
| `E0101` | `XR_ERR_LEX_INVALID_CHAR` | 非法字符 |
| `E0102` | `XR_ERR_LEX_UNTERMINATED_STR` | 字符串未终止 |
| `E0103` | `XR_ERR_LEX_INVALID_NUMBER` | 非法数字字面量 |
| `E0104` | `XR_ERR_LEX_INVALID_ESCAPE` | 非法转义 |

#### Parser

| 代码 | C 名称 | 含义 |
|--|--|--|
| `E0201` | `XR_ERR_SYN_UNEXPECTED_TOKEN` | 意外 token |
| `E0202` | `XR_ERR_SYN_EXPECTED_EXPR` | 缺少表达式 |
| `E0203` | `XR_ERR_SYN_EXPECTED_STMT` | 缺少语句 |
| `E0204` | `XR_ERR_SYN_UNCLOSED_PAREN` | 圆括号未闭合 |
| `E0205` | `XR_ERR_SYN_UNCLOSED_BRACE` | 花括号未闭合 |
| `E0206` | `XR_ERR_SYN_UNCLOSED_BRACKET` | 方括号未闭合 |
| `E0207` | `XR_ERR_SYN_INVALID_ASSIGN` | 非法赋值目标或形式 |

### 18.2 编译与静态分析

| 代码 | C 名称 | 含义 |
|--|--|--|
| `E0301` | `XR_ERR_CMP_UNDEFINED_VAR` | 编译器阶段未定义变量 |
| `E0302` | `XR_ERR_CMP_REDEFINED_VAR` | 重复定义变量 |
| `E0303` | `XR_ERR_CMP_CONST_ASSIGN` | 给常量赋值 |
| `E0304` | `XR_ERR_CMP_INVALID_BREAK` | 非法 `break` |
| `E0305` | `XR_ERR_CMP_INVALID_CONTINUE` | 非法 `continue` |
| `E0306` | `XR_ERR_CMP_INVALID_RETURN` | 非法 `return` |
| `E0307` | `XR_ERR_CMP_TOO_MANY_PARAMS` | 参数过多 |
| `E0308` | `XR_ERR_CMP_TOO_MANY_LOCALS` | 局部变量过多 |
| `E0309` | `XR_ERR_CMP_TOO_MANY_CONSTANTS` | 常量过多 |
| `E0310` | `XR_ERR_CMP_TOO_MANY_UPVALUES` | upvalue 过多 |
| `E0311` | `XR_ERR_CMP_JUMP_TOO_LARGE` | 跳转偏移超限 |
| `E0321` | `XR_ERR_TYPE_NOT_CALLABLE` | 静态类型不可调用 |
| `E0322` | `XR_ERR_TYPE_NOT_INDEXABLE` | 静态类型不可下标 |
| `E0323` | `XR_ERR_TYPE_NOT_ITERABLE` | 静态类型不可迭代 |
| `E0324` | `XR_ERR_TYPE_INVALID_OPERAND` | 操作数类型非法 |

#### Analyzer

| 代码 | C 名称 | 含义 |
|--|--|--|
| `E0350` | `XR_ERR_ANALYZE` | 通用 analyzer 错误 |
| `E0351` | `XR_ERR_ANALYZE_UNDEFINED_VAR` | 名称未定义 |
| `E0352` | `XR_ERR_ANALYZE_TYPE_MISMATCH` | 类型不匹配 |
| `E0353` | `XR_ERR_ANALYZE_CONST_ASSIGN` | 静态检查发现常量赋值 |
| `E0354` | `XR_ERR_ANALYZE_NOT_CALLABLE` | 被调用值不可调用 |
| `E0355` | `XR_ERR_ANALYZE_WRONG_ARG_COUNT` | 实参数量错误 |
| `E0356` | `XR_ERR_ANALYZE_ARG_TYPE` | 实参类型错误 |
| `E0357` | `XR_ERR_ANALYZE_GENERIC_COUNT` | 泛型实参数量错误 |
| `E0358` | `XR_ERR_ANALYZE_GENERIC_CONSTRAINT` | 泛型约束不满足 |
| `E0359` | `XR_ERR_ANALYZE_SUPER_FIRST` | `super(...)` 不是首个构造动作 |
| `E0360` | `XR_ERR_ANALYZE_SUPER_THIS` | `super(...)` 前访问 `this` |
| `E0361` | `XR_ERR_ANALYZE_SUPER_REQUIRED` | 派生构造函数缺少 `super(...)` |
| `E0362` | `XR_ERR_ANALYZE_SUPER_INVALID` | 非派生类非法使用 `super` |
| `E0363` | `XR_ERR_ANALYZE_CLOSURE_CAPTURE` | 闭包捕获不安全 |
| `E0364` | `XR_ERR_ANALYZE_AWAIT_TYPE` | `await` 操作数类型非法 |
| `E0365` | `XR_ERR_ANALYZE_MISSING_TYPE` | 缺少可推断的类型或初始化器 |
| `E0367` | `XR_ERR_ANALYZE_INTERFACE_NOT_IMPLEMENTED` | 未完整实现 interface |
| `E0368` | `XR_ERR_ANALYZE_TUPLE_FIELD_NAME` | tuple 字段名非法 |
| `E0369` | `XR_ERR_ANALYZE_TUPLE_FIELD_RANGE` | tuple 字段越界 |
| `E0370` | `XR_ERR_ANALYZE_THROW_NON_EXCEPTION` | `throw` 操作数不是允许的 enum 错误值 |
| `E0371` | `XR_ERR_ANALYZE_MATCH_NOT_EXHAUSTIVE` | `match` 不穷尽 |
| `E0372` | `XR_ERR_ANALYZE_USED_BEFORE_ASSIGN` | 赋值前使用 |
| `E0373` | `XR_ERR_ANALYZE_TUPLE_IMMUTABLE` | 修改不可变 tuple |
| `E0374` | `XR_ERR_ANALYZE_OVERRIDE_MISMATCH` | override 契约不匹配 |
| `E0375` | `XR_ERR_ANALYZE_HASHABLE_CONTRACT` | Map/Set 元素缺少 hash/equality 契约 |
| `E0376` | `XR_ERR_ANALYZE_CONDITION_TYPE` | 条件类型非法 |
| `E0377` | `XR_ERR_ANALYZE_VISIBILITY` | 可见性违规 |
| `E0378` | `XR_ERR_ANALYZE_CONST_FIELD` | 修改 const 字段 |
| `E0379` | `XR_ERR_ANALYZE_POSSIBLY_NULL` | 可能为 null 的值被不安全使用 |

### 18.3 运行时

| 代码 | C 名称 | 含义 |
|--|--|--|
| `E0400` | `XR_ERR_RUNTIME` | 通用运行时错误 |
| `E0401` | `XR_ERR_TYPE_NO_PROPERTY` | 类型没有该属性 |
| `E0402` | `XR_ERR_TYPE_NO_INDEX` | 值不可下标 |
| `E0403` | `XR_ERR_TYPE_NO_CALL` | 值不可调用 |
| `E0404` | `XR_ERR_TYPE_MISMATCH` | 运行时类型不匹配 |
| `E0405` | `XR_ERR_TYPE_NO_METHOD` | 类型没有该方法 |
| `E0406` | `XR_ERR_TYPE_NO_OPERATOR` | 类型不支持该运算符 |

#### Null、算术与容器

| 代码 | C 名称 | 含义 |
|--|--|--|
| `E0410` | `XR_ERR_NULL_PROPERTY` | 对 null 取属性 |
| `E0411` | `XR_ERR_NULL_INDEX` | 对 null 下标 |
| `E0412` | `XR_ERR_NULL_CALL` | 调用 null |
| `E0413` | `XR_ERR_NULL_UNWRAP` | 强制解包 null |
| `E0420` | `XR_ERR_DIV_BY_ZERO` | 除零 |
| `E0421` | `XR_ERR_MOD_BY_ZERO` | 模零 |
| `E0422` | `XR_ERR_OVERFLOW` | 算术溢出 |
| `E0430` | `XR_ERR_INDEX_OUT_OF_BOUNDS` | 下标越界 |
| `E0431` | `XR_ERR_KEY_NOT_FOUND` | Map 键不存在 |

#### 系统、调用、coroutine 与 stdlib

| 代码 | C 名称 | 含义 |
|--|--|--|
| `E0440` | `XR_ERR_STACK_OVERFLOW` | 栈溢出 |
| `E0441` | `XR_ERR_OUT_OF_MEMORY` | 内存不足 |
| `E0442` | `XR_ERR_MATCH_FAILURE` | 运行时 match 失败 |
| `E0450` | `XR_ERR_WRONG_ARG_COUNT` | 运行时实参数量错误 |
| `E0451` | `XR_ERR_INVALID_ARG_TYPE` | 运行时实参类型错误 |
| `E0460` | `XR_ERR_CORO_DEAD` | 操作已结束 coroutine |
| `E0461` | `XR_ERR_CORO_CANCELLED` | coroutine 已取消 |
| `E0470` | `XR_ERR_JSON_PARSE` | JSON 解析失败 |
| `E0471` | `XR_ERR_JSON_INVALID` | JSON 值或操作非法 |
| `E0475` | `XR_ERR_REGEX_COMPILE` | regex 编译失败 |
| `E0476` | `XR_ERR_REGEX_PATTERN` | regex pattern 非法 |
| `E0480` | `XR_ERR_TLS_UNAVAILABLE` | TLS 能力不可用 |

### 18.4 模块、IO 与 coroutine

| 代码 | C 名称 | 含义 |
|--|--|--|
| `E0501` | `XR_ERR_MOD_NOT_FOUND` | 模块不存在 |
| `E0502` | `XR_ERR_MOD_LOAD_FAILED` | 模块加载失败 |
| `E0503` | `XR_ERR_MOD_NO_EXPORT` | 名称未导出 |
| `E0504` | `XR_ERR_MOD_CIRCULAR` | 循环模块依赖 |
| `E0601` | `XR_ERR_IO_FILE_NOT_FOUND` | 文件不存在 |
| `E0602` | `XR_ERR_IO_READ_FAILED` | 读取失败 |
| `E0603` | `XR_ERR_IO_WRITE_FAILED` | 写入失败 |
| `E0604` | `XR_ERR_IO_PERMISSION_DENIED` | IO 权限不足 |
| `E0701` | `XR_ERR_CORO_DEADLOCK` | coroutine 死锁 |
| `E0702` | `XR_ERR_CORO_CHANNEL_CLOSED` | channel 已关闭 |
| `E0703` | `XR_ERR_CORO_LIMIT_EXCEEDED` | coroutine 限额超出 |

### 18.5 语法引导与内部错误

| 代码 | C 名称 | 被拒形式 / 含义 |
|--|--|--|
| `E0801` | `XR_ERR_SYN_RETURN_MULTI_REMOVED` | `return a, b` 无效；元组返回写作 `return (a, b)` |
| `E0803` | `XR_ERR_SYN_FOR_FLAT_REMOVED` | 裸 key/value `for` 形式无效 |
| `E0804` | `XR_ERR_SYN_VOID_REMOVED` | `-> void` 无效；无返回值写作 `-> ()` 或省略 |
| `E0805` | `XR_ERR_SYN_PARAM_MODE_PREFIX_REMOVED` | 参数 mode 必须写在冒号与类型之间 |
| `E0806` | `XR_ERR_SYN_PARAM_MOVE_MODE_REMOVED` | `move` 是实参转移表达式，不是参数 mode |
| `E0807` | `XR_ERR_SYN_PARAM_MODE_COMBINED_REMOVED` | 非法组合参数 mode |
| `E0808` | `XR_ERR_SYN_PARAM_MODE_POSTFIX_REMOVED` | 参数 mode 不能写在类型之后 |
| `E0809` | `XR_ERR_SYN_CALL_IN_MARKER_REMOVED` | call-site `in` marker；普通 `in` 参数调用不写 marker |
| `E0900` | `XR_ERR_INTERNAL` | 内部错误 |
| `E0901` | `XR_ERR_NOT_IMPLEMENTED` | 尚未实现 |
| `E0999` | `XR_ERR_UNKNOWN` | 未知错误 |

运行时 panic 通道使用 prelude `PanicInfo`；用户级 `throw <enum>` 走值返回错误通道。二者的语义见 §8 与 §16，不应仅凭错误码区段推断传播机制。

---

## 附录 A. EBNF 语法

> 真值源：`src/frontend/parser/xparse_*.c`。本附录给出整理后的紧凑 EBNF；具体冲突由 parser 实现决议。

### A.1 词法层

```ebnf
SourceFile ::= Statement*

Comment ::= '//' [^\n]*
         |  '/*' .* '*/'

Identifier ::= IdStart IdContinue*
IdStart    ::= 'a'..'z' | 'A'..'Z' | '_' | NonAsciiUtf8Byte
IdContinue ::= IdStart | '0'..'9'
// 源文件先整体校验为 UTF-8；lexer 将任意非 ASCII UTF-8 字节视为 identifier 字节，当前不做 XID/NFC 归一化。

IntLiteral   ::= DecimalInt | HexInt | BinInt | OctInt
DecimalInt   ::= DecimalDigit ('_'? DecimalDigit)*
HexInt       ::= '0x' HexDigit ('_'? HexDigit)*
BinInt       ::= '0b' ('0' | '1') ('_'? ('0' | '1'))*
OctInt       ::= '0o' ('0'..'7') ('_'? ('0'..'7'))*

FloatLiteral ::= DecimalInt '.' DecimalInt? Exponent?
              |  DecimalInt Exponent
Exponent     ::= ('e' | 'E') ('+' | '-')? DecimalDigit+

BigIntLiteral ::= (DecimalInt | HexInt | BinInt | OctInt) 'n'

StringLiteral ::= '"' StringChar* '"'
RawStringLiteral ::= 'r' '"' [^"]* '"'
CharLiteral ::= "'" CharBody "'"
CharBody ::= UnicodeScalar | EscapeSeq | '\u{' HexDigit{1,6} '}'
RegexLiteral ::= '/' RegexBody '/' RegexFlag*
RegexFlag ::= 'i' | 'm' | 's' | 'g' | 'u'

BoolLiteral ::= 'true' | 'false'
NullLiteral ::= 'null'
```

### A.2 类型

```ebnf
Type ::= UnionType
UnionType ::= IntersectionType ('|' IntersectionType)*
IntersectionType ::= NullableType
NullableType ::= PrimaryType '?'?
PrimaryType ::= FFIPointerType | CFunctionType | NamedType | FunctionType | TupleType | ObjectType
FFIPointerType ::= ('Ptr' | 'MutPtr') '<' Type '>'
CFunctionType ::= 'CFn' '<' FunctionType '>'
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

TernaryExpr ::= NullCoalesceExpr ('?' Expression ':' Expression)?
NullCoalesceExpr ::= LogicOrExpr ('??' LogicOrExpr)*
LogicOrExpr ::= LogicAndExpr ('||' LogicAndExpr)*
LogicAndExpr ::= BitOrExpr ('&&' BitOrExpr)*
BitOrExpr   ::= BitXorExpr ('|' BitXorExpr)*
BitXorExpr  ::= BitAndExpr ('^' BitAndExpr)*
BitAndExpr  ::= EqualityExpr ('&' EqualityExpr)*
EqualityExpr ::= RelationalExpr (('==' | '!=') RelationalExpr)*
RelationalExpr ::= ShiftExpr ((('<' | '<=' | '>' | '>=') ShiftExpr) | (('as' | 'is') Type))*
ShiftExpr   ::= AdditiveExpr (('<<' | '>>') AdditiveExpr)*
AdditiveExpr ::= MultiplicativeExpr (('+' | '-') MultiplicativeExpr)*
MultiplicativeExpr ::= UnaryExpr (('*' | '/' | '%' | '..' | '..=') UnaryExpr)*
// parser 中 range 与乘除同一 precedence；安全转换写为 `x as T?`，T? 是可空类型。

UnaryExpr ::= ('-' | '+' | '!' | '~') UnaryExpr
           |  'move' UnaryExpr
           |  'await' ('all' | 'any' | 'anySuccess')? UnaryExpr
           |  'go' (Block | PostfixExpr)
           |  'unsafe' Block
           |  PostfixExpr

PostfixExpr ::= Primary PostfixOp*
PostfixOp   ::= '(' ArgList? ')'              // call
             |  '.' Identifier                 // member
             |  '?.' Identifier                 // optional chain property
             |  '?.' Identifier '(' ArgList? ')'  // optional chain method
             |  '?[' Expression ']'            // optional chain index
             |  '[' Expression ']'             // index
             |  '[' Expression? ':' Expression? ']'  // slice
             |  '!'                            // force unwrap

Primary ::= IntLiteral | FloatLiteral | BigIntLiteral
         |  StringLiteral | RawStringLiteral | CharLiteral | RegexLiteral
         |  BoolLiteral | NullLiteral
         |  Identifier
         |  ArrayLit | MapLit | SetLit | ObjectLit
         |  ArrowFunction
         |  ComptimeExpr
         |  MatchExpr
         |  '(' Expression ')'
         |  '(' Expression (',' Expression)+ ')'  // tuple

ArrayLit ::= '[' (ArrayElem (',' ArrayElem)* ','?)? ']'
ArrayElem ::= '...' Expression | Expression
MapLit   ::= '#{' (MapEntry (',' MapEntry)* ','?)? '}'
MapEntry ::= Expression ':' Expression
SetLit   ::= '#[' (Expression (',' Expression)* ','?)? ']'
ObjectLit ::= '{' (ObjectFieldExpr (',' ObjectFieldExpr)* ','?)? '}'
ObjectFieldExpr ::= Identifier ':' Expression | Identifier | '...' Expression

ArrowFunction ::= '(' ArrowParams? ')' '->' (Expression | Block)
ArrowParams ::= ArrowParam (',' ArrowParam)*
ArrowParam  ::= Identifier ':' Type
// Note: arrow closures cannot declare an explicit return type;
// use `fn(p: T) -> R { ... }` or annotate the binding (`var f: (T) -> R = ...`) instead.

ComptimeExpr ::= 'comptime' (Expression | Block)

MatchExpr ::= 'match' '(' Expression ')' '{' MatchArm (','? MatchArm)* ','? '}'
MatchArm  ::= Pattern ('if' '(' Expression ')')? '->' (Expression | Block)

ArgList ::= CallArg (',' CallArg)* ','?
CallArg ::= ('ref' | 'out') Expression
          | '...' Expression
          | Identifier '->' (Expression | Block)
          | '_'
          | Expression
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

LiteralPattern  ::= IntLiteral | FloatLiteral | StringLiteral | CharLiteral | BoolLiteral | NullLiteral
RangePattern    ::= Expression ('..' | '..=') Expression
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
           |  IncDecStmt
           |  VarDecl
           |  ConstDecl
           |  SharedDecl
           |  OwnedDecl
           |  FnDecl
           |  ExternBlock
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
IncDecStmt ::= Identifier ('++' | '--') (';' | LineBreak)
Block    ::= '{' Statement* '}'

IfStmt    ::= 'if' '(' Expression ')' Block ('else' 'if' '(' Expression ')' Block)* ('else' Block)?
LoopLabel ::= Identifier ':'
WhileStmt ::= LoopLabel? 'while' '(' Expression ')' Block
ForStmt   ::= LoopLabel? 'for' '(' VarDecl? ';' Expression? ';' (Expression | Identifier ('++' | '--'))? ')' Block
ForInStmt ::= LoopLabel? 'for' '(' Identifier 'in' Expression ')' Block
ForInPairStmt ::= LoopLabel? 'for' '(' Identifier ',' Identifier 'in' Expression ')' Block
             |  LoopLabel? 'for' '(' '(' Identifier ',' Identifier ')' 'in' Expression ')' Block
MatchStmt ::= 'match' '(' Expression ')' '{' MatchArm (','? MatchArm)* ','? '}'

ReturnStmt   ::= 'return' (Expression | '(' Expression (',' Expression)+ ')')?
BreakStmt    ::= 'break' Identifier?
ContinueStmt ::= 'continue' Identifier?

ThrowStmt ::= 'throw' Expression
TryStmt   ::= 'try' Block CatchClause+
CatchClause ::= 'catch' 'panic'? ('(' Identifier (':' Type)? ')')? Block

DeferStmt ::= 'defer' (Expression | Block)

// print 是普通全局函数调用，语法上属于 ExprStmt。

// go 是表达式，返回 Task<T>。不作为独立语句类别出现（封装在 ExprStmt 中）。

ScopeStmt ::= ('linked' | 'supervisor')? 'scope' Block

SelectStmt ::= 'select' '{' SelectArm+ '}'
SelectArm  ::= Identifier 'from' Expression '->' Block      // 接收
            |  Expression 'to' Expression '->' Block        // 发送
            |  'after' Expression '->' Block                // 超时
            |  '_' '->' Block                                // 默认

YieldStmt ::= 'yield' Expression
```

### A.6 声明

```ebnf
VarDecl ::= 'var' Binding
ConstDecl ::= 'const' Binding
SharedDecl ::= 'shared' Identifier (':' Type)? '=' Expression
OwnedDecl ::= 'owned' Identifier (':' Type)? '=' Expression
Binding ::= BindingPattern (':' Type)? ('=' Expression)?
BindingPattern ::= Identifier
                |  '[' BindingPattern (',' BindingPattern)* ','? ']'
                |  '(' BindingPattern (',' BindingPattern)+ ','? ')'
                |  '{' ObjectBinding (',' ObjectBinding)* ','? '}'
ObjectBinding ::= Identifier (':' Identifier)?

FnDecl ::= AttrList? Modifier* 'fn' Identifier TypeParams? '(' ParamList? ')' ReturnType? Block
ExternBlock ::= 'extern' StringLiteral ExternLibrary? '{' ExternDecl+ '}'
ExternLibrary ::= ('dylib' | 'link') '(' StringLiteral ')'
ExternDecl ::= ExternFnDecl | ExternLayoutDecl
ExternFnDecl ::= AttrList? 'fn' Identifier '(' ParamList? ')' ReturnType? ';'?
ExternLayoutDecl ::= ('packed'? 'struct' | 'union') Identifier '{' ExternLayoutField* '}'
ExternLayoutField ::= Identifier ':' ('flex' Type | Type) ';'?
ParamList ::= Param (',' Param)* ','?
Param     ::= Identifier ':' ParamType ('=' Expression)?
           |  '...' Identifier ':' Type
ParamType ::= ParamMode? Type
ParamMode ::= 'in' | 'ref' | 'out'
ReturnType ::= '->' Type | '->' '(' Type (',' Type)+ ')'
Modifier  ::= 'private' | 'protected' | 'static' | 'const'
              // 公开可见性是默认语义；final 只作为 class 前缀

TypeParams ::= '<' TypeParam (',' TypeParam)* ','? '>'
TypeParam  ::= Identifier (':' Type ('&' Type)*)?         // 约束用 ':' ，多约束用 '&'
AliasTypeParams ::= '<' Identifier (',' Identifier)* ','? '>'

ClassDecl ::= 'final'? 'class' Identifier TypeParams?
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
EnumMethod     ::= 'fn' Identifier TypeParams? '(' ParamList? ')' ReturnType? Block
VariantPayload ::= '(' VariantField (',' VariantField)* ')'
VariantField   ::= (Identifier ':')? Type
TypeAliasDecl ::= 'type' Identifier AliasTypeParams? '=' Type

ImportDecl ::= 'import' ImportMembers 'from' ImportModule
            |  'import' ImportModule ('as' Identifier)?
ExportDecl ::= 'export' Declaration                                         // 直接导出声明
            |  'export' Identifier                                          // 导出已声明标识符
            |  'export' '*' 'from' StringLiteral                            // 转发导出
ImportMembers ::= '{' ImportMember (',' ImportMember)* ','? '}'
ImportMember  ::= Identifier ('as' Identifier)?
ImportModule  ::= StringLiteral | Identifier ('/' Identifier)?

AttrList ::= ('@' Identifier ('(' ArgList? ')')?)*  // 例如 @c_export("sym")、@section(".text")

OperatorToken ::= '+' | '-' | '*' | '/' | '%'
               |  '&' | '|' | '^'
               |  '==' | '!=' | '<' | '<=' | '>' | '>='
               |  '[]' | '[]='
               |  '!' | '~'
```

> 注：以上 EBNF 为指导性整理。precedence、associativity、消歧由 parser 实现决议；遇到歧义请以 `src/frontend/parser/xparse_*.c` 为准。

---

## 附录 B. 关键字索引

以下 **66 个**关键字与 `src/frontend/lexer/xkeywords.def` 一一对应并按源码顺序（ASCII 字典序）排列。`move`、`ref`、`out`、`linked`、`supervisor`、`from`、`to`、`after`、`panic` 是上下文词，不在本表；`parallel` 是标准库模块名。

| 关键字 | 节 |
|--|--|
| `as` | §3.8 |
| `await` | §10.3 |
| `bool` | §2.3.3 |
| `break` | §4.6 |
| `byte` | §2.3.1 |
| `catch` | §8 |
| `class` | §5.3 |
| `comptime` | §3.2 |
| `const` | §5.1 |
| `constructor` | §5.3 |
| `continue` | §4.6 |
| `defer` | §4.9 |
| `else` | §4.2 |
| `enum` | §5.6 |
| `export` | §11 |
| `extends` | §5.3 |
| `false` | §1.6.4 |
| `final` | §5.3 |
| `float` | §2.3.2 |
| `float32` | §2.3.2 |
| `float64` | §2.3.2 |
| `fn` | §5.2 |
| `for` | §4.4 |
| `go` | §10.2 |
| `if` | §4.2 |
| `implements` | §5.5 |
| `import` | §11 |
| `in` | §4.4 |
| `int` | §2.3.1 |
| `int16` | §2.3.1 |
| `int32` | §2.3.1 |
| `int64` | §2.3.1 |
| `int8` | §2.3.1 |
| `interface` | §5.5 |
| `is` | §3.8 |
| `match` | §3.13 / §4.5 |
| `new` | §3.14 |
| `null` | §1.6.4 |
| `operator` | §5.3 |
| `owned` | §5.1 |
| `packed` | §5.2.9 |
| `private` | §5.3 |
| `protected` | §5.3 |
| `return` | §4.7 |
| `rune` | §2.3.5 |
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
| `uint16` | §2.3.1 |
| `uint32` | §2.3.1 |
| `uint64` | §2.3.1 |
| `uint8` | §2.3.1 |
| `union` | §5.2.9 |
| `unsafe` | §3.2 / §5.2 |
| `var` | §5.1 |
| `while` | §4.3 |
| `yield` | §3.16 |

---

## 附录 C. 操作符索引

完整操作符按用途见 [§1.7](#17-操作符与-token)。详细优先级见 [§3.1](#31-优先级与结合性)。

| 类别 | 操作符 |
|--|--|
| 算术 | `+` `-` `*` `/` `%` |
| 位运算 | `&` `\|` `^` `~` `<<` `>>` |
| 比较 | `==` `!=` `<` `<=` `>` `>=` |
| 逻辑 | `&&` `\|\|` `!` |
| 赋值 | `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` |
| 语句 | `++` `--` |
| 类型、范围与展开 | `?` `??` `?.` `?[` `!` `\|` `->` `...` `..` `..=` `is` `as` |

---

## 附录 D. 标准库模块索引

完整 28 个标准库模块（native、纯 Xray 或混合实现）见 [§15](#15-标准库概览-standard-library)。

| 模块 | 用途 |
|--|--|
| `base64` | Base64 编解码 |
| `cluster` | 分布式集群 |
| `compress` | 压缩（gzip/zlib/deflate） |
| `crypto` | 加密散列 |
| `csv` | CSV 解析/序列化 |
| `datetime` | 日期时间 |
| `encoding` | 字符编码转换 |
| `http` | HTTP/REST |
| `io` | 文件 I/O |
| `log` | 结构化日志 |
| `math` | 数学函数 |
| `mem` | 裸内存与 managed Buffer |
| `net` | TCP/UDP/TLS |
| `os` | 操作系统 |
| `parallel` | 结构化 CPU 并行 |
| `path` | 路径操作 |
| `regex` | 正则 |
| `runtime` | 运行时信息与 cycle collection |
| `strconv` | 字符串数值解析 |
| `sync` | 协程同步原语 |
| `sys` | OS 线程与底层同步接口 |
| `text` | Unicode 文本变换 |
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
| 条件 | truthy / falsy | 条件必须是 `bool`，或使用 nullable `T?` 的存在性；int/string 不做 truthy 转换 |
| 相等比较 | `===` 强、`==` 弱（string↔number 自动转） | 仅 `==`/`!=`；值相等只做数值 int↔float 提升，不提供 `===`/`!==` |
| 闭包捕获 | 引用 | 引用（默认）；`go` 闭包严格受限 |
| 对象 | 动态字段 | `{...}` 默认形成 sealed Record；动态对象需显式 `Json` 边界 |
| import | ES Module | 自有 import 语法（含 stdlib 无引号形式） |
| 并发 | 异步/Promise | 协程 + Channel |

### E.2 vs Go

| 维度 | Go | xray |
|--|--|--|
| 类型系统 | 简单 + interface 隐式 | 较丰富 + 显式 `implements` |
| 错误处理 | 多返回值 + `err != nil` | 值返回错误通道（`throw` / `catch`）+ `T?` |
| 协程 | `go func() {}`（语句） | `go expr`（表达式，返回 `Task<T>`） |
| 等待结果 | 无直接等价（通过 channel/WaitGroup） | `await t`、`await all [...]`、`await any [...]` |
| Channel | 内置 `chan T`，`<-` 操作符 | `Channel<T>` 类，方法 `send`/`recv`/`trySend`/`tryRecv` |
| select 分支 | `case x := <-ch:` / `case ch <- v:` / `default:` | `x from ch ->` / `v to ch ->` / `after ms ->` / `_ ->` |
| 内存管理 | 三色并发 tracing GC | coroutine-local 引用计数 + Bacon–Rajan cycle collector；shared/system 对象使用原子引用计数 |
| 类与继承 | 无（仅 struct + interface） | class 支持继承 |
| 泛型 | 1.18+ 有 | 有；按具体类型或后端表示单态化 |

### E.3 vs Rust

| 维度 | Rust | xray |
|--|--|--|
| 内存安全 | borrow checker 全面 | owned/shared/move 约束跨执行边界；`Slice` 等借用视图受静态生命周期限制 |
| 错误 | `Result<T, E>` | 值返回错误通道（`throw` / `catch`）|
| 类型推断 | Hindley-Milner 强 | 双向推断 |
| trait | 完整 | 类似 `interface`，少功能 |
| 性能 | 接近 C | 字节码 VM，或通过 C toolchain 的 native AOT |
| 编译期 | macro / const | `comptime` 受限求值子集 + optimizer 常量折叠 |

### E.4 vs Python

| 维度 | Python | xray |
|--|--|--|
| 类型 | 动态（可选 hint） | 静态 |
| GIL | 有 | 无（M:N 协程） |
| 字符串 | unicode str | utf-8 string |
| 缩进 | 强制 | 自由（用 `{}`） |
| 类 | 动态属性 | 静态字段 |
| 性能 | CPython 慢 | 字节码 VM；性能关键程序可用 `xray build --native` |

### E.5 vs Swift

| 维度 | Swift | xray |
|--|--|--|
| 可空 `?` | 有 | 有 |
| `!` 解包 | 有 | 有 |
| 错误处理 | `try?` 折叠为 nil；`try!` abort | `throw` / `catch` 值返回通道；`T?` + `??` |
| struct vs class | 值/引用 | 值/引用 |
| 协议 | 有强 | `interface` 较弱 |
| 并发 | actor + async/await | 协程 + Channel + `go`/`await all`/`scope` |

---

## 附录 F. 词汇表

| 术语 | 定义 |
|--|--|
| **AOT** | Ahead-of-Time 编译：Xi IR 生成 C，并由所选 C toolchain 在构建时产生 native binary |
| **AST** | Abstract Syntax Tree：源码解析后的中间表示 |
| **Arena** | 批量分配器：所有分配同时释放 |
| **Array<byte>** | 字节缓冲类型（见 §2.4.5） |
| **rune** | 单个 Unicode scalar value 的原始类型；不是数值类型，也不是 `uint32` 别名（见 §2.3.5） |
| **Channel** | 类型化的协程通信管道（见 §10.5） |
| **closure** | 闭包：捕获外层变量的函数 |
| **coroutine** | 协程：用户态可暂停/恢复的执行流 |
| **defer** | 延迟执行：函数退出前执行（见 §4.9） |
| **enum** | 枚举类型（见 §5.6） |
| **GC** | Garbage Collection 的泛称；Xray 没有 tracing GC，而以引用计数为主，并用 Bacon–Rajan cycle collector 回收 coroutine-local 强引用环 |
| **safepoint** | 调度器可检查抢占、取消或挂起状态的安全位置；当前 cycle collector 不由函数调用或后向跳转 safepoint 驱动 |
| **goroutine** | xray 中称作协程 (coroutine)，启动语法 `go {...}` |
| **hoisting** | 提升：声明在使用前被隐式定义 |
| **IC** | Inline Cache：内联缓存（属性访问/方法分派优化） |
| **interface** | 接口（见 §5.5） |
| **JIT** | Just-In-Time 编译；Xray 当前未实现 JIT |
| **lvalue / rvalue** | 左值（可赋值）/ 右值（仅值） |
| **monomorphization** | 单态化：泛型在构建期按具体类型/表示生成专门版本；函数泛型可按 I64 / F64 / PTR / BOOL 表示共享，class / struct 泛型按具体类型完整单态化 |
| **move** | 所有权转移：跨协程时强制（见 §7.3） |
| **nullable** | 可空类型 `T?`：值可以为 null |
| **pattern** | 模式：用于 `match` 与解构（见 §6） |
| **scope** | 作用域 |
| **shared** | 跨协程共享的存储类（见 §5.1.3） |
| **SSA** | Static Single Assignment：每个变量只赋值一次的 IR |
| **struct** | 值类型类（见 §5.4） |
| **TCO** | Tail-Call Optimization：尾调用优化 |
| **trait** | Rust 术语；xray 用 `interface` |
| **condition expression** | 控制流条件：必须是 `bool` 或 `T?` 存在性（`T != bool`）；见 §2.3.3 |
| **grapheme cluster** | 用户感知字符，可能由多个 Unicode scalar 组成；`len(string)` / rune 迭代按 Unicode scalar，不按 grapheme cluster |
| **union** | 联合类型 `A \| B` |
| **Unicode scalar value** | 合法 Unicode 码位，范围 `U+0000..U+10FFFF` 且不包含 surrogate 区间 `U+D800..U+DFFF` |
| **upvalue** | 闭包捕获的外层变量 |
| **VM** | Virtual Machine：xray 字节码虚拟机 |
| **write barrier** | 写屏障：GC 在指针更新时插入的钩子 |

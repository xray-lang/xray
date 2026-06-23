---
id: spec.1_lexical_structure
order: 002
---

<!-- xr-spec:cn -->
---

## 1. 词法结构 (Lexical Structure)

> 真值源：`src/frontend/lexer/xlex.h`（token 枚举）、`src/frontend/lexer/xkeywords.def`（关键字表，61 条）、`src/frontend/lexer/xlex.c`（扫描器实现）。

### 1.1 字符编码

xray 源文件**必须**是 UTF-8 编码。所有源码处理（包括字符串字面量、标识符、注释）按 UTF-8 字节序列进行；非 ASCII 字符仅在字符串字面量、注释、原始字符串内部允许（标识符暂只支持 ASCII，见 §1.4）。

文件可选 UTF-8 BOM（`EF BB BF`）；扫描器跳过开头的 BOM。

### 1.2 行结尾与空白

行结尾识别 `\n`（Unix）与 `\r\n`（Windows）。`\r` 单独出现视为非法字符。

**空白字符**：空格 (`U+0020`)、水平制表符 (`U+0009`)、行结尾。空白用于分隔 token，不传递语义（**异常**：泛型语境下连续 `>>` 的拆分依赖空白上下文）。

### 1.3 注释

xray 支持两种注释：行注释不嵌套，块注释支持嵌套：

```xray
// 行注释，从 // 到行尾
/* 块注释，
   可跨行；
   支持 /* 嵌套 */ 到任意合理深度 */
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

xray 共 **61 个保留关键字**，源码真值表见 `src/frontend/lexer/xkeywords.def`。关键字按用途分组：

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
| `static` `private` | 类/成员修饰符；公开是默认语义，没有 `public` 关键字 |
| `abstract` `final` `override` | 类/方法修饰符（`override` 是**可选**——重写父类方法不要求显式标注） |
| `operator` | 运算符重载 |
| `is` `as` | 运行时类型检查 / 转换 |

#### 1.5.3 错误处理

`try` `catch` `panic` `throw` `defer`

#### 1.5.4 模块系统

`import` `export`

#### 1.5.5 协程与并发

`go` `await` `select` `defer` `scope`

#### 1.5.6 类型名（保留）

`int` `int8` `int16` `int32` `int64` `uint8` `uint16` `uint32` `uint64`
`float` `float32` `float64` `bool` `string`

`unknown` 是编译器内部类型格子的显示名，不是词法关键字；用户代码中可作为普通标识符使用。

> **注意**：以下名字**不是**词法关键字，而是 `prelude` 自动引入的内置类型符号：
> `Array` · `BigInt` · `Bytes` · `Channel` · `DateTime` · `Exception` · `Json` · `Logger` · `Map` · `NetConn` · `NetListener` · `Range` · `Regex` · `Set` · `StringBuilder`。
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
- 当整数字面量直接出现在窄整数上下文（变量初始化、赋值、参数、返回值、集合元素等）时，字面量值必须落在目标类型范围内；例如 `let x: int8 = 200` 是编译错误。非字面量表达式在写入窄整数目标时仍按目标宽度窄化并环绕，见 §2.3.1。

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
let empty_map = #{}
let primes = #[2, 3, 5, 7]
```

#### 1.7.10 模式

| Token | 用途 |
|--|--|
| `_` | `match` 通配符 |

#### 1.7.11 操作符优先级

完整优先级表见 [§3.1](#31-优先级与结合性)。
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 1. Lexical Structure

> Source of truth: `src/frontend/lexer/xlex.h` (token enum), `src/frontend/lexer/xkeywords.def` (keyword table, 61 entries), `src/frontend/lexer/xlex.c` (scanner implementation).

### 1.1 Character Encoding

Xray source files **must** be encoded as UTF-8. All source processing (string literals, identifiers, comments) treats input as a UTF-8 byte sequence; non-ASCII characters are allowed only inside string literals, comments, and raw strings (identifiers are currently ASCII-only; see §1.4).

A UTF-8 BOM (`EF BB BF`) is optional; the scanner skips a leading BOM.

### 1.2 Line Endings and Whitespace

Line endings recognize `\n` (Unix) and `\r\n` (Windows). A standalone `\r` is treated as an illegal character.

**Whitespace**: space (`U+0020`), horizontal tab (`U+0009`), and line terminators. Whitespace separates tokens and carries no semantics (**exception**: in generic contexts, splitting consecutive `>>` depends on whitespace context).

### 1.3 Comments

Xray supports two kinds of comments: line comments do not nest; block comments nest:

```xray
// line comment, from // to end-of-line
/* block comment,
   may span lines;
   supports /* nested */ layers to any reasonable depth */
```

Comments may appear wherever whitespace is allowed. They are collected as **trivia** for formatters and LSP (see `src/frontend/parser/xtrivia.*`), but do not participate in syntactic analysis.

Doc comments (no syntactic difference from ordinary comments): conventionally `///` or `/** */` for tooling. The compiler does not currently enforce this convention.

### 1.4 Identifiers

```ebnf
Identifier ::= IdentStart IdentCont*
IdentStart ::= 'a'..'z' | 'A'..'Z' | '_'
IdentCont  ::= IdentStart | '0'..'9'
```

ASCII only. The maximum length is bounded by the compiler (about 255 bytes).

**Reservation rule**: identifiers cannot collide with reserved keywords (see §1.5); they **may** collide with **context-sensitive keywords** (such as `from`, `to`, `default`, `ref`, `move`, `linked`, `supervisor`, `after`).

The character `_` is a **dedicated wildcard token**, not an ordinary identifier:

- In `match` patterns it represents a **wildcard** (see §6.7).
- In `for-in`, it can ignore the key or the value: `for (_, v in m) { ... }`.
- In destructuring binding it can ignore positions: `let (a, _) = (1, 2)`.
- It **cannot** appear as `let _ = expr`, as a function-parameter name, or as a referenced variable; the compiler reports "expected variable name".
- Multi-underscore names (such as `__tmp`) are ordinary identifiers.

### 1.5 Keywords

Xray has **61 reserved keywords** in total; the authoritative source-of-truth table is in `src/frontend/lexer/xkeywords.def`. Keywords are grouped by purpose:

#### 1.5.1 Declarations and Control Flow

| Keyword | Purpose |
|--|--|
| `let` | mutable variable declaration |
| `const` | immutable variable declaration |
| `shared` | cross-coroutine shared modifier (combined with `const`/`let`) |
| `fn` | function declaration |
| `return` | function return |
| `yield` | coroutine yield (statement form) |
| `if` `else` | conditional branches |
| `while` | loop |
| `for` `in` | loops (C-style + for-in) |
| `break` `continue` | loop control |
| `match` | pattern matching |

#### 1.5.2 Object Orientation and Types

| Keyword | Purpose |
|--|--|
| `class` `struct` | class / struct declaration |
| `extends` | class inheritance |
| `interface` `implements` | interface declaration / implementation |
| `enum` | enum declaration |
| `type` | type alias |
| `new` | instantiation |
| `this` `super` | self / parent reference |
| `constructor` | constructor |
| `static` `private` | class/member modifiers; public visibility is the default and has no `public` keyword |
| `abstract` `final` `override` | class/method modifiers (`override` is **optional** — overriding a parent method does not require an explicit annotation) |
| `operator` | operator overloading |
| `is` `as` | runtime type check / cast |

#### 1.5.3 Error Handling

`try` `catch` `panic` `throw` `defer`

#### 1.5.4 Module System

`import` `export`

#### 1.5.5 Coroutines and Concurrency

`go` `await` `select` `defer` `scope`

#### 1.5.6 Type Names (reserved)

`int` `int8` `int16` `int32` `int64` `uint8` `uint16` `uint32` `uint64`
`float` `float32` `float64` `bool` `string`

`unknown` is the display name for a compiler-internal type lattice value, not a lexical keyword; user code may use it as an ordinary identifier.

> **Note**: the following names are **not** lexer keywords; they are built-in type symbols automatically introduced by the prelude:
> `Array` · `BigInt` · `Bytes` · `Channel` · `DateTime` · `Exception` · `Json` · `Logger` · `Map` · `NetConn` · `NetListener` · `Range` · `Regex` · `Set` · `StringBuilder`.
> They may be locally shadowed by user types of the same name, but typically need no import.

#### 1.5.7 Literal Keywords

`true` `false` `null`

#### 1.5.8 Context-sensitive Keywords

These are not in the lexer keyword table; the parser recognizes them by position. They **may** be used as ordinary identifiers:

| Token | Where it appears |
|--|--|
| `from` | `select` receive arm (`x from ch`); also in named import / re-export (`import { x } from "module"`) |
| `to` | `select` send arm (`value to ch`) |
| `default` | reserved, currently disabled |
| `cancelled` | `cancelled()` cancellation check (actually a builtin function) |
| `ref` | function parameter modifier (`fn f(p: ref T)`) |
| `move` | ownership transfer (`move x`) |
| `linked` | `linked go` / `linked scope` modifier |
| `supervisor` | `supervisor scope` modifier |
| `after` | `select` timeout arm (`after 1000 -> ...`) |

### 1.6 Literals

#### 1.6.1 Integer Literals

```ebnf
IntLiteral ::= DecLit | HexLit | OctLit | BinLit
DecLit ::= Digit (Digit | '_')*
HexLit ::= '0x' HexDigit (HexDigit | '_')*
OctLit ::= '0o' OctDigit (OctDigit | '_')*
BinLit ::= '0b' BinDigit (BinDigit | '_')*
```

- Digit separators `_` exist purely for readability and may appear anywhere between digits.
- Default literal type is `int` (= `int64`). The `n` suffix promotes to `BigInt` (see §1.6.3).
- Range: `int64` covers `[-(2^63), 2^63 - 1]`; overflow is detected at compile time.
- When an integer literal appears directly in a narrow-integer context (variable initialization, assignment, argument, return value, collection element, and similar sites), its value must fit the target type; for example, `let x: int8 = 200` is a compile-time error. Non-literal expressions written into narrow integer targets are still narrowed with target-width wrap-around; see §2.3.1.

```xray
42
0xFF
0b1010
0o77
1_000_000      // one million
```

#### 1.6.2 Floating-Point Literals

```ebnf
FloatLiteral ::= Digit+ '.' Digit* Exp?
              | Digit+ Exp
              | '.' Digit+ Exp?
Exp ::= ('e' | 'E') ('+' | '-')? Digit+
```

Literal type is `float` (= `float64`, IEEE-754 double precision).

```xray
3.14
1.0e10
2.5E-3
.5             // equivalent to 0.5
```

#### 1.6.3 BigInt Literals

```ebnf
BigIntLiteral ::= (DecLit | HexLit | OctLit | BinLit) 'n'
```

```xray
123n
0xFFn
0b1010n
```

Arbitrary-precision integers; arithmetic never overflows. See §14.8 for the type.

#### 1.6.4 Boolean and Null Literals

```xray
true
false
null
```

- `true` / `false`: type `bool`.
- `null`: type `null` (semantically the zero value of every nullable type `T?`).

#### 1.6.5 String Literals

Xray supports two flavors of string literals: **escaped** and **raw**. Both can be quoted with single or double quotes, and both support `${...}` interpolation. Backtick strings are not part of the current grammar — the lexer rejects them.

##### Plain strings (double / single quotes)

```ebnf
StringLiteral ::= '"' StrChar* '"' | "'" StrChar* "'"
StrChar ::= any character that is not a quote, backslash, or newline
          | EscapeSeq
          | Interpolation
EscapeSeq ::= '\' ('"' | "'" | '\\' | 'n' | 't' | 'r' | '0'
                  | 'x' HexDigit{2}
                  | 'u' HexDigit{4}
                  | 'u{' HexDigit{1,6} '}')
Interpolation ::= '${' Expression '}'
```

- Double and single quotes are **fully equivalent** — both support escapes and `${...}` interpolation.
- Strings may span multiple lines; line breaks are part of the string.
- Literals containing interpolation produce `TK_TEMPLATE_STRING` internally; literals without interpolation produce `TK_LITERAL_STRING`.

```xray
"hello"
'world'
"Hello, ${name}! ${1 + 2}"
'tab\there\nnewline'
"\u4F60\u597D"        // "你好"
"\u{1F600}"            // emoji
```

**Interpolation expressions cannot contain unescaped quote characters of the surrounding kind** (a lexer restriction).

##### Raw strings (`r` prefix)

```ebnf
RawString ::= 'r' ('"' RawChar* '"' | "'" RawChar* "'")
RawChar ::= any character except the closing quote (including `\`, which is not processed)
```

- **No** escape processing (`\n`, `\t`, etc. are kept as-is).
- `${...}` interpolation is still supported.
- The identifier `r` standing alone is still a regular identifier (`TK_NAME`); it is recognized as a raw-string prefix only when immediately followed by a quote.

```xray
r"C:\path\to\file"          // literal contains two backslashes
r'C:\Users\${USER}'         // backslash is not escaped, but ${USER} still interpolates
```

##### Backtick strings (illegal)

The lexer explicitly rejects backtick strings. For templates, use plain double / single quotes plus `${...}`.

#### 1.6.6 Regex Literals

```ebnf
RegexLiteral ::= '/' RegexBody '/' RegexFlag*
RegexFlag ::= 'g' | 'i' | 'm' | 's'
```

```xray
/[a-z]+/i
/\d+\.\d+/g
```

- Flags: `g` (global), `i` (case-insensitive), `m` (multi-line), `s` (dot matches newline).
- Implementation: see `stdlib/regex`.
- **Disambiguation**: when `/` appears in a position that can accept a unary `/` (e.g., right after `=`, `,`, `(`, an operator), the scanner treats it as a regex; elsewhere it is division.

### 1.7 Operators and Tokens

Full token table (by category):

#### 1.7.1 Punctuation

| Token | Use |
|--|--|
| `(` `)` | grouping, calls, parameter lists |
| `{` `}` | blocks, object literals |
| `[` `]` | array literals, indexing |
| `,` | separator |
| `.` | member access |
| `:` | type annotation, map kv, ternary |
| `;` | for-loop separator (optional elsewhere) |
| `?` | nullable type, ternary |
| `@` | attribute marker (`@test`) |

#### 1.7.2 Arithmetic

`+` `-` `*` `/` `%`

#### 1.7.3 Bitwise

`&` `|` `^` `~` `<<` `>>`

#### 1.7.4 Comparison

`==` `!=` `<` `<=` `>` `>=`

- `==` `!=`: value equality (with implicit numeric promotion: int→float).
- `<` etc.: supported by numbers and strings; not supported by other types.

#### 1.7.5 Logical

`&&` `||` `!`

Short-circuit evaluation.

#### 1.7.6 Assignment

`=` `+=` `-=` `*=` `/=` `%=` `&=` `|=` `^=` `<<=` `>>=`

#### 1.7.7 Increment / Decrement

`++` `--`

Only the **statement-level postfix** form `x++` / `x--` is supported; prefix `++x` / `--x` and expression-position `x++` / `x--` are compile errors. See §4.1.

#### 1.7.8 Type-related

| Token | Use |
|--|--|
| `?` | nullable type (`T?`), ternary, optional-chain prefix |
| `?.` | optional chain property/method (`obj?.prop`, `obj?.method()`) |
| `?[` | optional chain index (`arr?[0]`) |
| `??` | null coalescing (`a ?? b`) |
| `!` | force unwrap (postfix, `expr!`) / logical not (prefix) |
| `\|` | union type (`int \| string`) / bitwise or |
| `->` | unified arrow: function return type, function type, closures, `match` / `select` arms |
| `...` | rest / spread |
| `..` | half-open range (`0..10`) |
| `..=` | inclusive range (`0..=10`) |
| `is` | runtime type check |
| `as` | type cast |

`!` ambiguity is resolved at parse time: immediately after an expression and with no whitespace, it is force-unwrap; in prefix position, it is logical not.

#### 1.7.9 Collection-literal Starters

| Token | Use |
|--|--|
| `#{` | empty Map literal |
| `#[` | Set literal start |

Examples:

```xray
let empty_map = #{}
let primes = #[2, 3, 5, 7]
```

#### 1.7.10 Patterns

| Token | Use |
|--|--|
| `_` | `match` wildcard |

#### 1.7.11 Operator Precedence

The full precedence table is in [§3.1](#31-precedence-and-associativity).
<!-- /xr-spec:en -->

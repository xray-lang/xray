---
id: spec.1_lexical_structure
order: 002
---

<!-- xr-spec:cn -->
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
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 1. Lexical Structure

> Source of truth: `src/frontend/lexer/xlex.h` (token enum), `src/frontend/lexer/xkeywords.def` (keyword table, 66 entries), `src/frontend/lexer/xlex.c` (scanner implementation).

### 1.1 Character Encoding

Xray source files **must** be valid UTF-8. Before producing the first token, the scanner strictly validates the entire input. Strings, comments, and identifiers may all contain non-ASCII characters (see §1.4 for identifier rules).

A UTF-8 BOM (`EF BB BF`) is optional; the scanner skips a leading BOM.

### 1.2 Line Endings and Whitespace

Line numbers advance on `\n`. Windows `\r\n` works because `\r` is skipped as horizontal whitespace. A standalone `\r` is also skipped, but it does **not** advance the line counter or trigger smart-semicolon behavior and therefore should not be used as a source line break.

**Whitespace**: space (`U+0020`), horizontal tab (`U+0009`), and line terminators. Whitespace separates tokens and carries no semantics (**exception**: in generic contexts, splitting consecutive `>>` depends on whitespace context).

### 1.3 Comments

Xray supports two kinds of comments: line comments do not nest; block comments nest:

```xray
// line comment, from // to end-of-line
/* block comment,
   may span lines;
   supports /* nested */ layers to any reasonable depth */
```

Comments may appear wherever whitespace is allowed. Formatters and language servers may read comment trivia; trivia does not participate in parsing.

Doc comments (no syntactic difference from ordinary comments): conventionally `///` or `/** */` for tooling. The compiler does not currently enforce this convention.

### 1.4 Identifiers

```ebnf
Identifier ::= IdentStart IdentCont*
IdentStart ::= 'a'..'z' | 'A'..'Z' | '_' | Utf8NonAsciiByteSequence
IdentCont  ::= IdentStart | '0'..'9'
```

After strict input validation, the scanner accepts non-ASCII UTF-8 byte sequences as identifier content, so `中文` and `café` are valid identifiers. The current scanner does not apply Unicode XID classification or NFC normalization; visually equivalent but differently encoded names remain distinct.

**Reservation rule**: identifiers cannot collide with reserved keywords (see §1.5); they **may** collide with **context-sensitive keywords** (such as `from`, `to`, `default`, `ref`, `move`, `linked`, `supervisor`, `after`).

The character `_` is a **dedicated wildcard token**, not an ordinary identifier:

- In `match` patterns it represents a **wildcard** (see §6.7).
- In `for-in`, it can ignore the key or the value: `for (_, v in m) { ... }`.
- In destructuring binding it can ignore positions: `var (a, _) = (1, 2)`.
- It **cannot** appear as `var _ = expr`, as a function-parameter name, or as a referenced variable; the compiler reports "expected variable name".
- Multi-underscore names (such as `__tmp`) are ordinary identifiers.

### 1.5 Keywords

Xray has **66 reserved keywords** in total, grouped by purpose below:

#### 1.5.1 Declarations and Control Flow

| Keyword | Purpose |
|--|--|
| `var` | mutable variable declaration |
| `const` | immutable variable declaration |
| `shared` | shared identity binding (owned by the shared/system owner, ordinary lexical scope) |
| `owned` | unique system-owned mutable identity binding; transfer with `move` |
| `comptime` | expression prefix that forces compile-time evaluation |
| `fn` | function declaration |
| `return` | function return |
| `yield` | generator value-yield statement |
| `if` `else` | conditional branches |
| `while` | loop |
| `for` `in` | loops (C-style + for-in) |
| `break` `continue` | loop control |
| `match` | pattern matching |

#### 1.5.2 Object Orientation and Types

| Keyword | Purpose |
|--|--|
| `class` `struct` | class / struct declaration |
| `packed` `union` | FFI layout declarations |
| `extends` | class inheritance |
| `interface` `implements` | interface declaration / implementation |
| `enum` | enum declaration |
| `type` | type alias |
| `new` | reserved; construct objects with `T(...)` (see §3.14) |
| `this` `super` | self / parent reference |
| `constructor` | constructor |
| `static` `private` `protected` | class/member modifiers; public visibility is the default and has no `public` keyword |
| `const` | immutable field/binding modifier |
| `final` | `final class` cannot be inherited |
| `operator` | operator overloading |
| `is` `as` | runtime type check / cast |

`abstract` and `override` are not keywords and may be used as identifiers in ordinary expression positions. Interfaces express abstract contracts, and methods with the same name and signature override automatically without member modifiers.

#### 1.5.3 Error Handling

`try` `catch` `throw` `defer`

#### 1.5.4 Module System

`import` `export`

#### 1.5.5 Coroutines and Concurrency

`go` `await` `select` `defer` `scope` `unsafe`

`parallel` is an explicitly imported standard-library module name, not a lexical keyword.

#### 1.5.6 Type Names (reserved)

`int` `int8` `int16` `int32` `int64` `byte` `uint8` `uint16` `uint32` `uint64`
`float` `float32` `float64` `bool` `string` `rune`

Writing `unknown` in a type annotation is rejected by the parser; it is not a lexical keyword, and remains usable as an ordinary identifier in expression position.

> **Note**: the following names are **not** lexer keywords; `stdlib/prelude/prelude_types.def` introduces them automatically:
> `Array` · `Atomic` · `BigInt` · `Channel` · `Json` · `Map` · `NetConn` · `NetListener` · `OsBarrier` · `OsCondvar` · `OsMutex` · `OsOnce` · `OsRwLock` · `PanicInfo` · `Path` · `Range` · `Regex` · `Set` · `StringBuilder` · `Thread`.
> `Array<byte>` is an `Array` specialization, not a separate name. Module-owned types such as `DateTime` and `Logger` require explicit imports from their modules.

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
| `ref` | parameter mode and call-site authorization (`fn f(p: ref T)` / `f(ref p)`) |
| `out` | output parameter mode and call-site authorization (`fn f(p: out T)` / `f(out p)`) |
| `move` | ownership transfer (`move x`) |
| `linked` | `linked go` / `linked scope` modifier |
| `supervisor` | `supervisor scope` modifier |
| `after` | `select` timeout arm (`after 1000 -> ...`) |
| `panic` | panic-channel boundary in `catch panic (p)` |

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
- When an integer literal appears directly in a narrow-integer context (variable initialization, assignment, argument, return value, collection element, and similar sites), its value must fit the target type; for example, `var x: int8 = 200` is a compile-time error. Non-literal expressions written into narrow integer targets are still narrowed with target-width wrap-around; see §2.3.1.

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

Xray supports two flavors of string literals: **escaped** and **raw**. Strings use double quotes only; single quotes are reserved for `rune` literals. Backtick strings are not part of the current grammar — the lexer rejects them.

##### Plain strings (double quotes)

```ebnf
StringLiteral ::= '"' StrChar* '"'
StrChar ::= any character that is not a double quote, backslash, or newline
          | EscapeSeq
          | Interpolation
EscapeSeq ::= '\' ('"' | "'" | '\\' | 'n' | 't' | 'r' | '0'
                  | 'x' HexDigit{2}
                  | 'u' HexDigit{4}
                  | 'u{' HexDigit{1,6} '}')
Interpolation ::= '${' Expression '}'
```

- Strings may span multiple lines; line breaks are part of the string.
- Literals containing interpolation produce `TK_TEMPLATE_STRING` internally; literals without interpolation produce `TK_LITERAL_STRING`.
- `${...}` is scanned in expression mode: braces are matched by depth, and nested strings / raw strings / rune literals are skipped as a unit, so same-quote nesting is legal, for example `"${m["k"]}"` and `"${"a}b"}"`.

```xray
"hello"
"Hello, ${name}! ${1 + 2}"
"tab\there\nnewline"
"\u4F60\u597D"        // "你好"
"\u{1F600}"            // emoji
```

Interpolation expressions may themselves contain nested interpolation; `}` characters inside nested strings do not close the outer `${...}`.

##### Raw strings (`r` prefix)

```ebnf
RawString ::= 'r' '"' RawChar* '"'
RawChar ::= any character except double quote (including `\`, which is not processed)
```

- **No** escape processing (`\n`, `\t`, etc. are kept as-is).
- `${...}` interpolation is still supported.
- The identifier `r` standing alone is still a regular identifier (`TK_NAME`); it is recognized as a raw-string prefix only when immediately followed by a double quote.
- Raw strings use `r"..."`; single-quoted strings continue to use the ordinary escape rules.

```xray
r"C:\path\to\file"          // literal contains two backslashes
r"C:\Users\${USER}"         // backslash is not escaped, but ${USER} still interpolates
```

#### 1.6.6 `rune` Literals

```ebnf
CharLiteral ::= "'" CharBody "'"
CharBody ::= UnicodeScalar | EscapeSeq | '\u{' HexDigit{1,6} '}'
```

- `'a'` has type `rune` and represents one Unicode scalar value.
- The valid range is `U+0000..U+10FFFF`, excluding surrogates `U+D800..U+DFFF`.
- A literal must contain exactly one scalar; `''`, `'ab'`, `'🇨🇳'`, and `'é'` are compile errors.
- Escapes such as `'\n'`, `'\t'`, `'\r'`, `'\0'`, `'\''`, `'\\'`, and `'\u{1F600}'` are supported.
- Char literals do not support `${...}` interpolation.

```xray
var a: rune = 'a'
var zh: rune = '中'
var smile: rune = '\u{1F600}'
```

##### String interpolation

String templates use ordinary double quotes with `${...}` interpolation.

#### 1.6.7 Regex Literals

```ebnf
RegexLiteral ::= '/' RegexBody '/' RegexFlag*
RegexFlag ::= 'g' | 'i' | 'm' | 's'
```

```xray
/[a-z]+/i
/\d+\.\d+/g
```

- Flags: `g` (global), `i` (case-insensitive), `m` (multi-line), `s` (dot matches newline).
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
var empty_map = #{}
var primes = #[2, 3, 5, 7]
```

#### 1.7.10 Patterns

| Token | Use |
|--|--|
| `_` | `match` wildcard |

#### 1.7.11 Operator Precedence

The full precedence table is in [§3.1](#31-precedence-and-associativity).
<!-- /xr-spec:en -->

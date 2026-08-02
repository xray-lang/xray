# Xray Language Reference

> Version: based on the `xray` source tree version v0.9.0 (audited on 2026-07-18).
> Status: this is a reference manual for the implemented language. When this document and the implementation disagree, the implementation is authoritative and this document must be updated.
> Chinese version: [`LANGUAGE_SPEC_CN.md`](LANGUAGE_SPEC_CN.md).

## Table of Contents

- [0. Preface](#0-preface)
- [1. Lexical Structure](#1-lexical-structure)
- [2. Type System](#2-type-system)
- [3. Expressions](#3-expressions)
- [4. Statements](#4-statements)
- [5. Declarations](#5-declarations)
- [6. Patterns](#6-patterns)
- [7. Scoping and Name Resolution](#7-scoping-and-name-resolution)
- [8. Error Handling](#8-error-handling)
- [9. Generics](#9-generics)
- [10. Concurrency and Coroutines](#10-concurrency-and-coroutines)
- [11. Modules](#11-modules)
- [12. Testing](#12-testing)
- [13. Built-in Functions](#13-built-in-functions)
- [14. Built-in Type Methods](#14-built-in-type-methods)
- [15. Standard Library](#15-standard-library)
- [16. Runtime Model](#16-runtime-model)
- [17. Compilation Pipeline](#17-compilation-pipeline)
- [18. Error Codes](#18-error-codes)
- [Appendix A. EBNF](#appendix-a-ebnf)
- [Appendix B. Keyword Index](#appendix-b-keyword-index)
- [Appendix C. Operator Index](#appendix-c-operator-index)
- [Appendix D. Standard Library Module Index](#appendix-d-standard-library-module-index)
- [Appendix E. Differences from Other Languages](#appendix-e-differences-from-other-languages)
- [Appendix F. Glossary](#appendix-f-glossary)

---

## 0. Preface

### 0.1 About This Specification

This document is the **reference manual** for the Xray programming language. It describes the lexical structure, syntax, type system, semantics, concurrency model, runtime, and standard-library surface. Its goals are:

1. Allow a human reader to write valid Xray code with predictable behavior.
2. Serve as a **structured source of truth** for compilers, analyzers, IDEs, AI assistants, documentation generators, and other tooling.
3. Stay aligned with the actual implementation in the main `xray` repository — any divergence is a bug in either the document or the code.

**This manual is not a tutorial.** For introductory material, see the Xray website and the `demos/` directory.

### 0.2 Versioning

The version of this manual is kept in strict lock-step with the main `xray` repository (the `project(Xray VERSION x.y.z)` line in `CMakeLists.txt`). Major breaking changes are noted in-section as "since vX.Y.Z".

Xray is currently in the `v0.x` series. **No backward compatibility is promised.** Each release of the spec may introduce breaking changes.

### 0.3 Language Design Philosophy

Xray is a **lightweight statically typed scripting language with native concurrency support**. Design goals:

| Dimension | Choice |
|--|--|
| **Types** | Static typing + type inference; declarations rarely require explicit type annotations, but the type system is fully visible at compile time |
| **Concurrency** | Built-in M:N coroutines (go / await / Channel / scope / select); concurrency safety is enforced at compile time by the "explicit sharing" rules |
| **Execution** | Bytecode VM and the `xray build --native` AOT path; there is currently no JIT, and differential tests guard cross-backend semantics |
| **Error handling** | Value-return error channel (throw / try / catch + enum errors) + panic boundary (catch panic) + nullable types (T?) + `defer`-based resource management |
| **Metaprogramming** | Attributes (`@test` / `@deprecated` / `@derive`) + compile-time metadata + monomorphized generics |
| **Interop** | C ABI is built-in; stdlib modules can be authored in C and exposed via `XR_DEFINE_BUILTIN` |

Design influences: TypeScript (type inference + nullable), Go (structured concurrency + Channel), Rust (a lightweight take on ownership/`move`), Swift (protocols + optional chaining). **Xray is not a clone of any of them.**

### 0.4 Reading Conventions

#### 0.4.1 Grammar Notation

This document uses a lightweight EBNF-style notation:

| Symbol | Meaning |
|--|--|
| `Term` | non-terminal (capitalised) |
| `'literal'` | literal token |
| `A B` | sequence |
| `A \| B` | choice |
| `A?` | optional |
| `A*` | zero or more |
| `A+` | one or more |
| `(A)` | grouping |

The complete EBNF is in [Appendix A](#appendix-a-ebnf).

#### 0.4.2 Source-Code References

This document references the main `xray` repository extensively as its source of truth. Citation format:

```
path:lineno
```

Examples: `src/frontend/lexer/xkeywords.def`, `src/frontend/parser/xast_types.h:42-58`.

#### 0.4.3 Status Markers

| Marker | Meaning |
|--|--|
| **Stable** | default state; behavior will not change without notice |
| **Experimental** | implementation exists but may change |
| **Reserved** | keyword/syntax recognized but currently disabled |
| **Unimplemented** | described in the spec but not yet supported in code; must be marked explicitly |

#### 0.4.4 Error-Code References

Error codes use the `E0xxx` format (e.g., `E0101`); the full list is in [Chapter 18](#18-error-code-reference). Source-level definitions are in `src/runtime/xerror_codes.h` and `src/runtime/xerror.h`.

---

## 1. Lexical Structure

> Source of truth: `src/frontend/lexer/xlex.h` (token enum), `src/frontend/lexer/xkeywords.def` (keyword table, 66 entries), `src/frontend/lexer/xlex.c` (scanner implementation).

### 1.1 Character Encoding

Xray source files **must** be valid UTF-8. Before producing the first token, the scanner strictly validates the entire input. Strings, comments, and identifiers may all contain non-ASCII characters (see §1.4 for identifier rules).

A UTF-8 BOM (`EF BB BF`) is optional; the scanner skips a leading BOM.

### 1.2 Line Endings and Whitespace

Line numbers advance on `\n`. Windows `\r\n` works because `\r` is skipped as horizontal whitespace. A standalone `\r` is also skipped, but it does **not** advance the line counter or end a statement, and therefore should not be used as a source line break.

**Whitespace**: space (`U+0020`), horizontal tab (`U+0009`), and line terminators. Whitespace separates tokens and carries no semantics.

**The one whitespace-sensitive rule**: whether `<` opens a generic argument list depends on whether whitespace **precedes** it. `f<T>(x)` is a call with explicit type arguments; `f < T` is a comparison. Splitting consecutive `>` does **not** depend on whitespace: `Array<Array<int>>` and `Array<Array<int> >` are exactly equivalent, decided by grammatical position rather than spacing.

#### 1.2.1 Statement Boundaries

Xray does not require a trailing `;`: **a line ending terminates a statement**. When a line ending is reached while an expression is still open, the compiler decides whether the next line continues it or starts a new statement.

A line ending **terminates** the current expression if and only if all of the following hold:

1. The last consumed token can **end** an expression (identifier, literal, `)`, `]`, `}`, postfix `!`, `++`, `--`, `?`, a scalar type name); and
2. The first token of the new line can **begin** an expression; and
3. The parser is not inside `(` or `[` — no statement can begin inside a bracket group, so line endings are transparent there.

Conditions 1 and 2 are only ever both true for tokens that carry **both a prefix and an infix role**: `!` (logical not / force unwrap), `-` (negate / subtract), `/` (regex literal / divide), `++`, `--`, `(`, `[`. Hence:

```xray
var x = a
!b            // two statements; NOT var x = a!
```

When a line begins with a token that has **no prefix role** (`.`, `?`, `:`, `&&`, `||`, `+`, `*`, `as`, `is`, …) there is no ambiguity and continuation works as usual:

```xray
var ok = check(a)
    && check(b)      // continuation

var total = base +
            extra    // operator parked at end of line; continuation
```

To continue a line with `-` or `/`, move the operator to the end of the previous line or wrap the whole expression in `( )`:

```xray
var d = (total
    - used)          // line endings are transparent inside a bracket group
```

> The value of an expression statement is discarded, so **an expression statement with no effect is an error** (`E0208`). This pairs with the boundary rule above: a continuation line that was correctly split off does not become a silent no-op — it is reported immediately, with the rewrite suggested. The REPL is exempt, since it prints the value of a trailing bare expression and that value is therefore observed.

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

**Reservation rule**: identifiers cannot collide with reserved keywords (see §1.5); they **may** collide with **context-sensitive keywords** (such as `from`, `to`, `default`, `ref`, `move`, `linked`, `after`).

The character `_` is a **dedicated wildcard token**, not an ordinary identifier:

- In `match` patterns it represents a **wildcard** (see §6.7).
- In `for-in`, it can ignore the key or the value: `for (_, v in m) { ... }`.
- In destructuring binding it can ignore positions: `var (a, _) = (1, 2)`.
- It **cannot** appear as `var _ = expr`, as a function-parameter name, or as a referenced variable; the compiler reports "expected variable name".
- Multi-underscore names (such as `__tmp`) are ordinary identifiers.

### 1.5 Keywords

Xray has **64 reserved keywords** in total, grouped by purpose below:

#### 1.5.1 Declarations and Control Flow

| Keyword | Purpose |
|--|--|
| `var` | mutable variable declaration |
| `const` | stable binding declaration; deep-read-only capability in type position |
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

`int` `i8` `i16` `i32` `i64` `byte` `u8` `u16` `u32` `u64`
`float` `f32` `f64` `bool` `string` `rune`

Writing `unknown` in a type annotation is rejected by the parser; it is not a lexical keyword, and remains usable as an ordinary identifier in expression position.

> **Note**: the following names are **not** lexer keywords; `stdlib/prelude/builtin_symbols.def` introduces them automatically:
> `Array` · `Atomic` · `BigInt` · `Channel` · `Json` · `Map` · `NetConn` · `NetListener` · `OsBarrier` · `OsCondvar` · `OsMutex` · `OsOnce` · `OsRwLock` · `PanicInfo` · `Path` · `Range` · `Regex` · `Set` · `Slice` · `StringBuilder` · `Thread`.
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
| `move` | ownership transfer (`move x`) |
| `linked` | `linked go` / `linked scope` modifier |
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
- An integer literal without a unique numeric context defaults to `int` (= `i64`). The `n` suffix promotes to `BigInt` (see §1.6.3).
- Range: the default `int` context covers `[-(2^63), 2^63 - 1]`; overflow is detected at compile time.
- When an integer literal appears directly in a unique numeric context (variable initialization, assignment, argument, return value, collection element, or another already-typed numeric operand), it acquires the target type directly instead of first becoming `int` and then being converted. An integer target must represent the value; a floating target must represent it exactly. Otherwise compilation fails and an explicit `as` is required to express truncation, sign change, or rounding intent.

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

Literal type is `float` (= `f64`, IEEE-754 double precision).

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

Xray quoted literals use double quotes only; single quotes are reserved for `rune`, and backtick strings do not exist. The literal prefix, escape mode, and delimiter form are orthogonal dimensions; the unified rules follow below.

##### Inline escaped strings (Q = 1)

```ebnf
InlineEscapedString ::= '"' StrChar* '"'
StrChar ::= any character that is not a double quote, backslash, or newline
          | EscapeSeq
          | Interpolation
EscapeSeq ::= '\' ('"' | "'" | '\\' | 'n' | 't' | 'r' | '0'
                  | 'x' HexDigit{2}
                  | 'u' HexDigit{4}
                  | 'u{' HexDigit{1,6} '}')
Interpolation ::= '${' Expression '}'
```

- An inline literal cannot cross a physical line; use an escape or block form for line breaks.
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

##### Inline raw strings (`r` prefix, Q = 1)

```ebnf
InlineRawString ::= 'r' '"' RawChar* '"'
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

##### Unified prefixes and variable quote delimiters

```ebnf
QuotedLiteral ::= LiteralPrefix InlineQuoted | LiteralPrefix BlockQuoted
LiteralPrefix ::= '' | 'r' | 'b' | 'br' | 'c' | 'cr'
InlineQuoted ::= '"' InlinePayload* '"' | '""'
BlockQuoted ::= QuoteRun ImmediateLineEnding BlockBody BlockClose
QuoteRun ::= '"'{Q}                         // Q >= 3
BlockClose ::= LineStart Indent SameQuoteRun (LineEnding | EOF)
```

- No prefix / `r` produces a valid UTF-8 `string`; no prefix processes escapes, while `r` preserves backslashes literally.
- `b/br` produces `[byte; L]`; `c/cr` produces `[byte; L+1]` with an appended NUL. `b/c` processes escapes, while `br/cr` preserves raw bytes.
- `${...}` interpolates only in the no-prefix / `r` family. It is always ordinary payload bytes in `b/br/c/cr`.
- The only prefixes are no prefix, `r`, `b`, `br`, `c`, and `cr`; `rb/rc` are not aliases. A prefix must immediately precede the quote run.
- `c/cr` rejects every interior NUL after escape decoding, newline normalization, and margin removal.

```xray
"Hello, ${name}!"
r"C:\\path\\${name}"   // backslashes are raw; interpolation remains active
b"\\x89PNG"              // escaped [byte; 4]
br"${HOME}\\bin"         // raw bytes; `${HOME}` does not interpolate
c"puts"                   // [byte; 5], ending in the appended NUL
cr"C:\\assets"           // raw C bytes + appended NUL
```

One quote is the inline delimiter. Two consecutive quotes represent only an empty payload in the selected prefix family and never open a block:

```xray
"" r"" b"" br"" c"" cr""
```

Three or more consecutive quotes form a block delimiter. The opener must be followed immediately by LF or CRLF. The closer must start on its own line and may contain only “margin + exactly the opener's quote count + line ending or EOF”. No trailing whitespace, comment, comma, semicolon, or bracket is allowed on the closer line; subsequent tokens start on the next line.

```xray
const HTML = r"""
<div class="card">
  ${title}
</div>
"""

const SCRIPT = br""""
echo ${HOME}
"""
""""
```

The structural newline after the opener and before the closer is not part of the value. CRLF inside the body normalizes to LF. Spaces/tabs before the closer define the margin; every non-empty body line must begin with that exact byte prefix, which is removed from the payload. Tabs and spaces are not compared by visual columns.

Only a complete standalone quote-only line matching the closer shape ends the block; ordinary quote runs within body lines are content. If the body needs a line that conflicts with the current closer, the author or formatter increases `Q`. The formatter preserves the prefix and inline/block form but selects the smallest safe `Q >= 3`.

Interpolation expressions are scanned in expression mode with balanced braces; nested quoted literals and rune literals are skipped as units, so same-quote nesting is legal. The fixed-byte family never enters interpolation scanning.

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

- `==` `!=`: value equality; numeric operands must have the same type or require only same-signed integer widening / `f32 → f64`. An integer literal may directly acquire the other operand's numeric type.
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

`!` ambiguity is resolved at parse time by **line**, not by whitespace: after an expression on the same line it is force-unwrap; in prefix position (including at the start of a line) it is logical not. Whitespace plays no part — `a !b` and `a! b` are both "two statements on one line" and both require a separating `;`. For the cross-line rule see §1.2.1.

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

---

## 2. Type System

> Source of truth: `src/runtime/value/xtype.h` (`XrType` definition), `src/runtime/value/xtype.c`, `src/frontend/parser/xparse_type.c` (syntax), `src/frontend/analyzer/xtype_ref_resolve.c` (resolution), `stdlib/prelude/builtin_symbols.def` (built-in type table).

### 2.1 Overview

Xray is statically typed; every expression has a determined type at compile time. Core features of the type system:

1. **Type inference**: variable declarations rarely require type annotations; the analyzer infers from the initializer / context.
2. **Nullable separation**: `T` is never `null`; `T?` is sugar for `T | null`.
3. **Union types**: `A | B | ...` (up to 6 members).
4. **Monomorphized generics**: generic definitions are specialized at build time while keeping nominal type identity.
5. **Three disconnected compatibility domains**: `class` / `struct` / `interface` are **nominal** (explicit `implements`, no implicit conformance); `Record` is **structural with an exact field set** (sealed, no width subtyping); `Json` is an **open, run-time** data-exchange value domain. There is no implicit conversion between domains — only the two explicit bridges `Json.encode(value)` and `json as T`.
6. **Minimal type identity**: `typeOf`, `typeName`, `is`, and `as`; there is no default runtime `Reflect` module.

### 2.2 Type Categories

| Category | Examples |
|--|--|
| Primitive | `int`, `float`, `bool`, `string`, `rune`, `()` (Unit, no return value) |
| Sized integers | `i8`, `i16`, `i32`, `i64`, `byte`..`u64` |
| Sized floats | `f32`, `f64` |
| Containers | `Array<T>`, `Map<K,V>`, `Set<T>`, `Channel<T>`; `Array<byte>` is the contiguous-byte specialization of `Array` |
| Fixed layout | `[T; N]` |
| Borrowed view | `Slice<T>` (owns no data; constrained by borrow lifetimes, see §2.4.2) |
| Special prelude types | `Json`, `BigInt`, `Range`, `Regex`, `StringBuilder`, `Atomic<T>`, `Path`, `Thread<T>`, `NetConn`, `NetListener`, and the `Os*` synchronization types |
| Module-exported types | `DateTime`, `Logger`, `Plan`, `Mutex<T>`, and others; these require explicit imports from their defining modules |
| Error-handling prelude | `PanicInfo` (see §8) |
| Nullable | `T?` |
| Union | `A \| B \| ...` |
| Tuple | `(T1, T2, ...)` |
| Function | `fn(T1, T2) -> R` |
| FFI / C ABI | `Ptr<T>`, `MutPtr<T>`, `CFn<(T) -> R>`, `usize`, `isize` |
| Class / Struct / Interface | user-defined (nominal) |
| Enum | user-defined (incl. ADT enum, see §5.6) |
| Type alias | `type Name = SomeType`, `type Name<T> = SomeType` |

<!-- xr-builtin-registry:begin -->

#### 2.2.1 Built-in symbol registry

Generated from `stdlib/prelude/builtin_symbols.def`, this is the complete set of names available without an import. The compiler, the LSP and this table read the same source of truth; any capitalized name outside it comes from an import or a user declaration.

**Built-in types**

| Symbol | Construction |
|--|--|
| `Array<T>` | prelude |
| `Atomic<T>` | prelude |
| `BigInt` | prelude |
| `CFn<T>` | resolver built-in |
| `Channel<T>` | prelude |
| `CoroLocal<T>` | resolver built-in |
| `EnumPayloadField<T>` | resolver built-in |
| `EnumPayloads<T>` | resolver built-in |
| `EnumVariant<T>` | resolver built-in |
| `EnumVariants<T>` | resolver built-in |
| `Json` | prelude |
| `Map<K, V>` | prelude |
| `MutPtr<T>` | resolver built-in |
| `NetConn` | prelude |
| `NetListener` | prelude |
| `OsBarrier` | prelude |
| `OsCondvar` | prelude |
| `OsMutex` | prelude |
| `OsOnce` | prelude |
| `OsRwLock` | prelude |
| `PanicInfo` | prelude |
| `Path` | prelude |
| `Ptr<T>` | resolver built-in |
| `Range` | prelude |
| `Regex` | prelude |
| `Set<T>` | prelude |
| `Slice<T>` | prelude |
| `StringBuilder` | prelude |
| `Task<T>` | resolver built-in |
| `Thread<T>` | prelude |

**Built-in enums**

| Symbol | Variants |
|--|--|
| `Ordering` | `Relaxed` \| `Acquire` \| `Release` \| `AcquireRelease` \| `SeqCst` |
| `Endian` | `Native` \| `LE` \| `BE` |
| `Utf8Error` | `InvalidUtf8` |
| `StringSliceError` | `InvalidByteRange` |
| `CompressionError` | `InvalidData` |
| `CryptoError` | `InvalidLength` |
| `Recv<T>` | `Value` \| `Empty` \| `Timeout` \| `Closed` |
| `SendResult` | `Sent` \| `Full` \| `Timeout` \| `Closed` |
| `TaskResult<T>` | `Success` \| `Failed` \| `Cancelled` \| `Timeout` \| `Pending` |
| `TaskStatus` | `Pending` \| `Running` \| `Success` \| `Failed` \| `Cancelled` |

**Built-in constraint interfaces**

| Symbol |
|--|
| `Callable<T>` |
| `Closeable` |
| `Comparable` |
| `Equatable` |
| `Hashable` |
| `Indexable<K, V>` |
| `Iterable<T>` |
| `Iterator<T>` |
| `Lengthable` |
| `Stringable` |

**Deliberately absent names**

| Symbol | Diagnostic hint |
|--|--|
| `Self` | Xray has no 'Self' type; write the declaring type's own name, e.g. operator==(other: Token) -> bool |
| `Box` | 'Box' is not a built-in type; indirect recursive data through a class node or a container slot such as Array<T> |

<!-- xr-builtin-registry:end -->

### 2.3 Primitive Types

#### 2.3.1 Integer Types

| Type | Range | Alias |
|--|--|--|
| `i8` | `[-128, 127]` | — |
| `i16` | `[-32768, 32767]` | — |
| `i32` | `[-2³¹, 2³¹-1]` | — |
| `i64` | `[-2⁶³, 2⁶³-1]` | `int` (default integer type) |
| `byte`..`u64` | unsigned counterparts | — |

- An integer literal without a unique numeric context defaults to `int`; in a unique integer context it directly acquires that type and must fit its range (`var x: i8 = 200` is rejected at compile time). In a unique floating context it directly acquires that floating type, but its integer value must be exactly representable.
- Arithmetic uses two's-complement wrap-around semantics (no debug/release distinction). Operations on the same integer type keep that type and wrap at its width (`byte + byte -> byte`); different widths with the same signedness use the unique wider type. There is no implicit promotion across signedness, between fixed-width integers and `isize`/`usize`, or between integers and floats; shift results keep the left operand's type.
- Values with static type `byte`..`u64` are interpreted as unsigned by `print`, `string(x)`, template strings, string concatenation, and ordering comparisons; for example, a static `u64` bit pattern of `0xffff_ffff_ffff_ffff` formats as `18446744073709551615` and compares greater than `0`.
- `int.checkedAdd` / `checkedSub` / `checkedMul` return `null` on overflow; `saturating*` clamps to the `int` boundary; `wrapping*` explicitly performs the default two's-complement wrap.
- An already-typed expression cannot be implicitly narrowed, change signedness, or cross a target-dependent width at assignment. Such conversions require an explicit `as`. Explicit integer conversion reduces modulo the target width and interprets the resulting two's-complement bit pattern as the target type.
- After dynamic erasure, `XrValue` stores only the integer payload, not signedness or width. Across `any` / Json / dynamic-container boundaries, `u64` values above the positive `i64` range are not guaranteed to keep unsigned formatting or ordering semantics. Keep the value statically typed as `uintN` when unsigned semantics are required.

#### 2.3.2 Floating-Point Types

| Type | Standard |
|--|--|
| `f32` | IEEE-754 single precision |
| `f64` | IEEE-754 double precision; alias of `float` |

Literals default to `float`.

#### 2.3.3 `bool`

`true` / `false`, a standalone type. **No implicit conversion** to/from numeric types (cannot write `var x: int = true` or `var b: bool = 1`).

**Condition expression rules** (`if` / `while` / `for` conditions / ternary `?:` / `match` guards):

| Condition type | Allowed | Meaning |
|---|---|---|
| `bool` | yes | direct boolean test |
| `T?` with `T != bool` | yes | null presence only (content emptiness is **not** checked) |
| `bool?` | compile error | tri-state ambiguity; write `flag == true` / `flag != null` / `flag ?? false` |
| `int` / `float` / `string` / `rune` / collections / objects | compile error | use explicit comparisons such as `n != 0`, `len(s) != 0` |

Operands of `&&` / `||` / `!` must be `bool`; do not place `T?` directly into `&&` / `||`.

```xray
var ok = true
if (ok) { }

var user: User? = findUser()
if (user) {              // presence: null check only
    print(user.name)     // user is narrowed to User here
}

var flag: bool? = maybeFlag()
if (flag == true) { }    // OK
if (flag != null) { }    // OK
// if (flag) { }         // compile error: bare bool? cannot be a condition

var s = ""
if (len(s) != 0) { }     // OK
// if (s) { }            // compile error
```

#### 2.3.4 `string`

Immutable strings that always contain valid UTF-8. `len(s)` returns the Unicode scalar count in O(1), and `len(s.bytes())` returns the UTF-8 byte count in O(1). Default iteration yields `rune`; integer indexing and the slice operator do not apply to strings. See §14.5 for explicit access.

Internally uses ARC; runtime short strings are coroutine-local by default (lock-free allocation), while literals/symbols, explicit `intern()`, and map/set keys use the global intern pool. Cross-execution strings consume a verified storage plan; the boundary does not promote or copy them implicitly.

#### 2.3.5 `rune`

`rune` represents one Unicode scalar value (valid range `U+0000..U+10FFFF`, excluding the surrogate range `U+D800..U+DFFF`). It is an independent primitive type, **not** a numeric type and **not** an alias of `u32`.

```xray
var a: rune = 'a'
var zh = '中'
var smile = '\u{1F600}'
print(typeName(a))        // "rune"
print(smile.toUInt32())   // 128512
```

- A rune literal must contain exactly one Unicode scalar; empty literals, multi-scalar literals, and surrogate literals are compile errors.
- `rune` does not participate in arithmetic, bitwise operations, or narrow-integer assignment: `'a' + 1` and `var n: u32 = 'a'` are rejected by the analyzer.
- Explicit conversions: `int(c)` returns the scalar code point; `rune(n)` constructs a rune from an integer and validates that it is a legal scalar; `string(c)` / `c.toString()` returns a one-scalar string.
- Common methods are listed in §14.4.1.

#### 2.3.6 Unit `()` (no return value)

Xray uses the **0-tuple `()`** to represent "no return value" (the Unit type):

```xray
fn log(msg: string) -> () { print(msg) }   // explicit Unit return
fn ping() { print("pong") }                  // omitted return type = ()
var r: () = log("hi")                        // allowed; r is a Unit value
```

- A function omitting its return type is equivalent to `-> ()`.
- `void` is not a type name: `fn f() -> void` is rejected (`E0804`); use `-> ()` or omit the return type to indicate no return value.

#### 2.3.7 FFI Scalars and C ABI Boundary Types

Xray's C FFI uses explicit boundary types so ordinary xray objects are not implicitly interpreted as C data:

| Type | C ABI meaning | Notes |
|--|--|--|
| `usize` | `size_t` | width comes from the compilation target; it must not be substituted with the host's `u64` |
| `isize` | `ptrdiff_t` / platform signed width | width comes from the compilation target; it must not be substituted with the host's `i64` |
| `Ptr<T>` | `const void *` boundary value | read-only raw pointer; `T` gives the xray-side dereference/index width |
| `MutPtr<T>` | `void *` boundary value | mutable raw pointer; assignable where `Ptr<T>` is expected |
| `CFn<(A, B) -> R>` | C ABI function pointer | passes an xray function as a C callback argument to an `extern "C"` function |

Raw pointer values may be stored, passed, compared, and offset with `offset(i)` using element-width scaling in safe code; actually reading or writing foreign memory must be inside `unsafe { }`:

```xray
extern "C" {
    fn malloc(n: usize) -> MutPtr<byte>
    fn free(p: MutPtr<byte>)
}

var p = unsafe { malloc(4) }
unsafe {
    p[0] = 42
    print(p.deref())
    free(p)
}
```

`Ptr<T>` is read-only; writes require `MutPtr<T>`. `unsafe` does not bypass that type rule. Raw pointer access performs no null or bounds checks, so the caller must guarantee address validity, lifetime, alignment, and aliasing correctness.

`usize` / `isize` use one target-ABI scalar descriptor across FFI calls, `mem.load/store<T>`, extern-layout fields, and generated C. The VM, AOT backend, and layout introspection must use the compilation target's width and alignment; cross-compilation never derives language semantics from the build host's `sizeof(size_t)`.

`CFn<(...) -> ...>` is not an ordinary xray closure type. The current VM/AOT backends support passing module-level, noncapturing xray functions with an exact signature match to C; capturing closures, anonymous functions, and extern functions themselves cannot be used as `CFn` callback arguments.

### 2.4 Composite Types

#### 2.4.1 `Array<T>`

Ordered mutable array. See §14.7.

```xray
var a: Array<int> = [1, 2, 3]
var b = [1, 2, 3]                // inferred as Array<int>
var c: Array<string> = []         // explicit empty array
```

The `T` in `Array<T>` must be determinable at compile time. An empty `[]` without a type annotation is a compile error: `Empty array '[]' requires a type annotation`.

`Array<rune>` preserves the `rune` element identity: reads return `rune`, and writes accept only `rune`.

#### 2.4.1.1 Fixed Arrays `[T; N]`

`[T; N]` is a fixed-layout array type for `N` elements of type `T`. `N` is part of the type and must evaluate during analysis to a positive compile-time integer expression. The current expression subset includes integer literals, `const` integer identifiers, grouping, unary `-`/`~`, and integer arithmetic/bitwise operators. The current backend encoding limit is 65535 elements.

Fixed arrays work as inline struct fields and local variables. They support struct, nested fixed-array, and reference-container element types, so fixed arrays compose recursively:

```xray
var bytes: [byte; 4] = [1, 2, 3, 4]
var zero: [byte; 64] = [0; 64]
var names: [string; 2] = ["a", "b"]
var blocks: [[byte; 2]; 2] = [[1, 2], [3, 4]]
```

A target-typed array literal that initializes `[T; N]` must have the exact length; repeat initialization `[value; N]` uses the same positive compile-time integer expression rule and must also match the target length. A normal array literal without context still infers dynamic `Array<T>`; `[value; N]` without context infers `[T; N]`.

Fixed arrays support `len(array)`, indexed reads, indexed writes, `ref`/`in` parameter passing, and target-typed slicing into `Slice<T>`:

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

`[T; N]` has different semantics from `Array<T>`:

- `[T; N]`: fixed length, value semantics, fixed layout; suited for inline struct fields, local small buffers, and FFI/freestanding data.
- `Array<T>`: dynamic length, growable, heap-backed container.
- `Slice<T>`: borrowed view over contiguous storage; it does not own data (see §2.4.2).

The old `[N]T` syntax is not part of the Xray language.

#### 2.4.2 `Slice<T>`

> Source of truth: `stdlib/prelude/prelude_types.def` (prelude registration), `src/frontend/analyzer/xanalyzer_visitor_stmt.c` (borrow tracking and invalidation checks), `src/frontend/analyzer/xa_memory_effect_db.h` (invalidation criteria), `src/frontend/analyzer/xanalyzer_visitor_decl.c` (returned-view contract).

`Slice<T>` is a **borrowed view**: it denotes a run of contiguous element storage owned by another value. It owns no data, does not participate in reference counting, and cannot be placed in any long-lived storage. It is a prelude type (`GENERIC_1`) and may be written directly in any type annotation.

##### Construction

A view can only arise from the three sources below, and it **requires an explicit target type** — a slice expression without one is a compile error:

| Source | Result | Notes |
|--|--|--|
| `array[start:end]` | `Slice<T>` | the owner is an `Array<T>` |
| `fixedArray[start:end]` | `Slice<T>` | the owner is a `[T; N]` |
| `str.bytes()` | `Slice<byte>` | the owner is the string's UTF-8 byte storage |

```xray
var arr: Array<int> = [10, 20, 30, 40]
var view: Slice<int> = arr[1:3]      // OK: borrows arr
var all: Slice<int> = arr[:]         // full-length view, not a copy
var bad = arr[1:3]                   // E0365: a slice result needs an explicit target type
```

The owner must be a **named local, a parameter, or a field path rooted at one**. Temporaries cannot be borrowed:

```xray
var view: Slice<byte> = makeBytes()[0:2]   // E0384: cannot create a view from a temporary owner
```

##### Capabilities

- `len(view)` for the length; `view[i]` reads an element; `view[i] = v` writes one, **straight into the owner's storage**.
- `view[a:b]` reslices; the result still borrows the same owner.
- A view has **no member methods** and no `.length`.
- A `const Slice<T>`, and any view derived from a `const` owner, is read-only; writing through it is a compile error.

```xray
fn main() {
    var arr: Array<int> = [10, 20, 30, 40]
    var view: Slice<int> = arr[1:3]        // borrowed view, not a copy
    view[1] = 31
    print(arr[2])                          // 31 — the write goes through to the owner
    arr[1] = 21
    print(view[0])                         // 21 — element writes on the owner are visible here
    var owned: Array<int> = copy(arr[1:3]) // an independent Array<T>
    arr.push(50)                           // OK: no view is live at this point
    print(len(owned))
}

main()
```

##### Borrow rules

Let view `v` borrow owner `o`. While `v` is **live**:

1. **Element writes are allowed**: `o[i] = x` is legal. An element write does not move the owner's storage, so the view stays valid.
2. **Invalidating operations are rejected** (`E0382`): any operation that may relocate, shorten, or otherwise invalidate `o`'s element storage. The criterion is not a method-name allowlist but the callee's memory effect: address stability (`ADDRESS_STABLE` / `MAY_RELOCATE`), shortening (`NEVER_SHORTENS` / `MAY_SHORTEN`), and view invalidation (`NEVER_INVALIDATES` / `INVALIDATES_VIEWS`). `o.push(x)`, reassigning `o`, `move o`, `freeze o`, and `return o` all fall in this class.
3. **Liveness ends at the last use** (non-lexical lifetimes): the borrow ends at `v`'s last use, not at the end of the enclosing block, so an owner mutation placed after that point is legal.
4. **No escape** (`E0383`): a view must not outlive the owner's scope. All of the following are rejected — function return values (unless the return contract below is satisfied), class / struct / Record fields, Array / Map / Set / tuple / Json / enum-payload elements, closure captures, generator `yield`, module-level bindings, type arguments of a generic class or struct, crossing an execution boundary via `go` or a channel, and erasing the type with `as`.

```xray
fn ok() {
    var bytes: Array<byte> = [1, 2]
    var view: Slice<byte> = bytes[:]
    print(len(view))                 // last use of view
    bytes.push(3)                    // OK: the borrow already ended (rule 3)
}

fn rejected() {
    var bytes: Array<byte> = [1, 2]
    var view: Slice<byte> = bytes[:]
    bytes.push(3)                    // E0382: view is still live
    print(len(view))
}
```

##### Across functions: the returned-view contract

A function may return `Slice<T>` **if and only if** the returned view has a **uniquely inferable source**: one specific parameter, the receiver, or static storage. The compiler records a returned-view contract (source kind plus parameter index) for such a function; the call site charges the result back to the original owner, so the borrow rules keep applying on the caller's side.

A non-unique source, or a view borrowed from one of the function's own locals, is a compile error (`E0384`):

```xray
fn tail(data: Slice<byte>, start: int) -> Slice<byte> {
    return data[start:]              // OK: the unique source is the parameter data
}

fn bad(a: Slice<byte>, b: Slice<byte>, useA: bool) -> Slice<byte> {
    if (useA) {
        return a
    }
    return b                         // E0384: multiple sources
}

fn alsoBad() -> Slice<int> {
    var local: Array<int> = [1, 2]
    return local[:]                  // E0384: borrowed from a local
}
```

##### The escape hatch: `copy`

When the data must outlive the owner, or must go into long-lived storage, use `copy` to materialize the view into an independent owner:

```xray
var owned: Array<int> = copy(arr[1:3])   // an independent Array<T>, unrelated to arr
```

`copy(slice)` has result type `Array<T>`, not `Slice<T>`; it is the only construct that turns borrowed data into owned data.

##### Relationship to other borrows

`ref` parameters and `Ptr<T>` / `MutPtr<T>` share the same borrow tracking and the same error codes: `E0382` (owner invalidated while a borrow is live), `E0383` (borrow escapes), `E0384` (borrow source unstable or not unique). An `unsafe` block relaxes none of them.

#### 2.4.3 `Map<K, V>`

Hash table that **preserves insertion order**. See §14.8.

**Map literals** must use the `#{ ... }` prefix with `:` separators (consistent with Json; disambiguated by the `#` prefix):

```xray
var m: Map<string, int> = #{"a": 1, "b": 2}
var m2 = #{"a": 1, "b": 2}
var empty = #{}                                     // empty Map

m["c"] = 3                                          // insert / update
var v = m["a"]                                      // lookup; a missing key panics with E0431
var maybe = m.get("missing")                        // safe lookup; returns null if absent
```

| Literal form | Type | Purpose |
|---|---|---|
| `{ key: value }` (no prefix) | sealed `Record` (`Json` when the expected type is `Json`) | see §2.4.7 |
| `#{ "k": v }` (`#` prefix + `:`) | `Map<K, V>` (hash table) | this section |
| `#{}` | `Map<K, V>` (empty) | explicit empty Map |
| `[]` | `Array<T>` | array |
| `#[]` | `Set<T>` | set |

`K` must satisfy `Hashable` (see §9.2): typically `int`, `float`, `string`, `bool`, `enum`, `BigInt`, or a custom type that provides both `operator==` and `hash() -> int` (the parameter type of `operator==` is spelled as the type's own name; Xray has no `Self` type). Generic key types must be explicitly constrained as `K: Hashable`.

#### 2.4.4 `Set<T>`

Deduplicated collection. See §14.9.

```xray
var s: Set<int> = #[1, 2, 3]
```

#### 2.4.5 `Channel<T>`

Inter-coroutine communication channel. A named channel uses a stable `const` binding; its synchronized interior-mutation capability comes from the audited registry (see §10.5).

```xray
const ch: Channel<int> = Channel<int>(10)
```

#### 2.4.6 `Array<byte>`

Typed byte buffer. Semantically equivalent to `Array<byte>`, but stored as contiguous memory.

```xray
var buf = Array<byte>(1024)
var init = Array<byte>([72, 101, 108, 108, 111])
```

#### 2.4.7 `Record` / `Json` and Object Literals

Bare object literals default to sealed structural `Record`, for ordinary business objects, options, and multi-field returns. `Json` is an explicit opt-in JSON value-domain type: it is used at external data-exchange boundaries, can hold any JSON-equivalent structure, and intrinsically includes `null`.

The key difference between an **object literal** `{ field: value, ... }` and a Map literal:

```xray
// Record/Json object literal: identifier or string key + colon ':'
var data: Json = { name: "Alice", tags: ["a", "b"], age: 30 }
var user = { name: "Bob", age: 25 }       // default type is sealed Record
typeName(user)                            // "Record"
data.name              // type: Json (field access returns Json)
data["name"]           // equivalent

// Field shorthand: when a field name matches a variable name
var name = "Alice"
var age = 30
var user = { name, age }                  // equivalent to { name: name, age: age }

// Map literal: `#{}` prefix + `:`
var m = #{"k1": 1, "k2": 2}           // type: Map<string, int>
```

**Comparison**:

| Form | Type | Notes |
|---|---|---|
| `{ name: "x", age: 1 }` | sealed anonymous `Record` | identifier or string key followed by `:` |
| `var j: Json = { name: "x" }` | `Json` object | interpreted as dynamic Json only with an explicit `Json` expected type |
| `{ x: y }` (`x` is field name, `y` is variable) | sealed anonymous `Record` | shorthand `{ x }` equivalent to `{ x: x }`; bare key only |
| `#{"a": 1}` | `Map<K, V>` | `#` prefix disambiguates; separator `:` |
| `Point{x: 1.0, y: 2.0}` | `Point` (struct) | type name + `{...}` literal |

**Record types**: bare object literals and `type T = {...}` are Records. Records are sealed by default — accessing or assigning an undeclared field is a compile error. Use an explicit `Json` annotation or `Json.encode(value)` at JSON boundaries.

```xray
type User = { name: string, age: int }

var u: User = { name: "Alice", age: 30 }
print(u.name)         // OK
// u.extra = "x"      // compile error: sealed type User has no field 'extra'

var u2 = { name: "Alice", age: 30 }      // sealed Record
// u2.extra = "x"     // compile error

var j: Json = { name: "Alice", age: 30 } // dynamic Json object
j.extra = "x"        // OK (Json is dynamic)
```

**Responsibilities of the product types**: the line between the structural and nominal domains is whether a type carries methods.

| | Identity | Field set | User-defined methods | Use for |
|---|---|---|---|---|
| `Record` | structural | declared, exact at compile time | **no** | options, multi-field returns, ordinary business data |
| `Json` | value domain (no identity) | arbitrary, resolved at run time | **no** | external data-exchange boundaries |
| `struct` | nominal | declared, checked at compile time | yes | value semantics, fixed layout, FFI aggregates, math types |
| `class` | nominal | declared, checked at compile time | yes | reference semantics, inheritance, encapsulation |

**Normative commitment**: `Record` and `Json` are pure data shapes. Their fields **carry data only and can never hold a function**, and users cannot declare methods on them. Consequently `j.name` is always a field read, while a built-in member can only appear in call form `j.name()` — the two forms stay syntactically decidable, so a field name never contends with a built-in member name for the same expression. Use `struct` or `class` when behavior is required. `Json` also exposes its generic queries and conversions as static functions (`Json.keys(obj)`, see §14.11); prefer the static form whenever a field name may collide with a built-in member name.

#### 2.4.8 `BigInt`

Arbitrary-precision integer. See §14.8.

#### 2.4.9 `Range`

`Range` represents an integer interval and is produced by `a..b` or `a..=b`:

- `a..b` is the half-open interval `[a, b)` and excludes `b`.
- `a..=b` is the inclusive interval `[a, b]` and includes `b`.

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

Ranges work with `for-in`, range patterns in `match`, and collection queries. See §3.9 for expression semantics and §14.12 for members.

#### 2.4.10 `DateTime` / `Regex` / `StringBuilder`

`Regex` and `StringBuilder` are prelude types. `DateTime` is not a prelude name; bring it into scope with `import { DateTime } from datetime` (or another explicit import). See §14 for the member index.

### 2.5 Nullable Types

`T?` is sugar for `T | null`.

```xray
var x: int? = null      // OK
var y: int? = 42        // OK
var z: int = null       // compile error: null is not int
```

`Json` intrinsically includes `null`, so `Json?` and `Json | null` are redundant and rejected during parsing. Parse failures use typed error enums propagated through the `throw`/`catch` value-return channel. When failure must be stored or returned as ordinary data, use a domain ADT or a Record with an explicit status field. Do not introduce a global `Result<T,E>`.

**Nullable primitives are first-class**: `int?` / `float?` / `bool?` are ordinary `T?` types and arise naturally from generics and containers (e.g. `Map<string, bool>.get(k) -> bool?`, or `fn find<T>(...) -> T?` at `T = bool`). They carry `null` in the tagged representation, so a `null` value renders as `"null"` in `print` / `string()` / string concatenation (never as the raw payload `0`), identically in the VM and AOT.

> `bool?` is tri-state (`true` / `false` / `null`). It is legal but **cannot be used directly as a condition** (a bare `if (b)` where `b: bool?` is a compile error; see §5 / task 128); write `b == true` / `b != null` / `b ?? false`.

#### Unwrapping

```xray
// 1. Null coalescing
var v = x ?? 0

// 2. Optional chaining (whole-chain short-circuit, nullable result)
var city = user?.address.city

// 3. Force unwrap
var v: int = x!           // panics with NullError at runtime if x is null

// 4. Flow-sensitive narrowing (full rules in §2.13)
if (x != null) {
    print(x + 1)          // x is narrowed to int in this branch
}
if (x is int) {
    print(x + 1)
}
```

### 2.6 Union Types

```xray
var v: int | string = 42
v = "hello"             // OK
```

Constraints:
- Up to **6 members** (checked at compile time; over the limit → error).
- Members must not be subtypes of each other (otherwise normalized).
- **Members must be discriminable at run time**: a dynamically erased value keeps only its i64 or f64 family, so a union carries at most **one integer-family member** and at most **one float-family member**. `i16 | i32` and `f32 | float` are one runtime type wearing two static names — `is` and `match` cannot tell them apart, and an assignment could not say which one it stored. Declaring one is `E0390`.
- Working with a union value requires `match` or `is`-based narrowing:

```xray
var v: int | string = ...
match v {
    is int    -> print("int: ${v}"),
    is string -> print("str: ${v}"),
}
```

**Member selection**: a union value carries the member selected where it was assigned. The rule is deterministic and independent of the order the members were written:

- The source type is **exactly** one member → that member is selected.
- Otherwise the single member reachable by an implicit conversion from §2.10.1 is selected. Discriminability guarantees at most one member per numeric family, so at most one member qualifies.
- A numeric literal selects within its own family: an integer literal prefers the integer-family member, falling back to the float-family member when the union has none (matching "an integer literal types into a unique floating context"); a float literal selects the float-family member. The literal must be exactly representable in the selected member.

```xray
var a: int | float = 1        // exact match: int
var b: int | float = 1.0      // exact match: float
var c: i32 | string = 7       // sole integer-family member: i32
var d: f32 | string = 1.5     // sole float-family member: f32
```

`is T` asks about the runtime value: for a fixed-width numeric type it asks whether the value is exactly representable in `T`, because an erased value does not carry its width and that is the only answerable form. Inside a well-formed union the selected member always passes its own `is` and every other member always fails, so the arms are mutually exclusive.

**Special cases**:
- `int | null` normalizes to `int?`.
- When `T?` appears in a union: `int? | string` is effectively `int | string | null`, normalized to `(int | string)?`.

### 2.7 Tuple Types

Xray's tuples are **first-class** — they may appear as any value, be stored as fields, and nest.

```xray
// Literals
var t = (1, 2, 3)                 // type inferred as (int, int, int)
var h = (10, "hi", true)          // heterogeneous tuple
var single = (99,)                // single-element tuple: note trailing comma

// Type annotation
var p: (int, string) = (7, "ok")

// Field access: .N (N is a compile-time constant integer index)
var first = t.0                   // 1
var mid   = t.1                   // 2
var nest  = ((1, 2), (3, 4))
var a     = nest.0.0              // 1
var b     = nest.1.1              // 4

// Function return and destructuring
fn divmod(a: int, b: int) -> (int, int) { return (a / b, a % b) }
var (q, r) = divmod(17, 5)        // tuple destructure

// Generic
fn pair<A, B>(a: A, b: B) -> (A, B) { return (a, b) }
var p2 = pair(1, "x")             // (int, string)
```

**Notes**:

- A **single-element tuple** must use a trailing comma `(x,)` — `(x)` without a comma is a grouping parenthesis (a plain expression).
- In field access `t.N`, N **must be an integer literal**; using a variable or string is the compile error `XR_ERR_ANALYZE_TUPLE_FIELD_NAME` / `_RANGE`.
- Tuples are **immutable**: `t.0 = v` is a compile error. To modify, build a new tuple.

#### Worked Examples

```xray
fn main() {
    var pair = (1, "hello")
    print(pair.0)   // => 1
    print(pair.1)   // => hello
    var (a, b) = pair    // destructuring
    print(a)        // => 1
    print(b)        // => hello
}

main()
```

### 2.8 Type Aliases

```xray
type Result = int | string
type Mapper = (int) -> int
type Point = { x: float, y: float }
type Pair<T> = { first: T, second: T }
type Mapper2<T, U> = (T) -> U
```

Aliases are **purely syntactic** equivalences; they do not introduce new types,
runtime metadata, or AOT branches. A generic alias is substituted at its use
site:

```xray
var p: Pair<int> = { first: 1, second: 2 }  // equivalent to { first: int, second: int }
var f: Mapper2<int, string> = (n) -> string(n)
```

Generic alias parameters are a name list only (`<T, U>`); constraints are not
part of type-alias syntax. Put constraints on the generic function, class /
struct / enum / interface that uses the alias. Aliases may be forward
referenced, but cyclic aliases, including recursive object aliases, are compile
errors.

### 2.9 Type Inference

See §7.4 for details. In summary:

```xray
var x = 1               // x: int
var y = 1.5             // y: float
var z = "hello"         // z: string
var a = [1, 2, 3]       // a: Array<int>
var m = #{"a": 1}    // m: Map<string, int>
var p = { name: "A" }   // p: { name: string } — structured object type
var f = (x: int) -> x   // f: (int) -> int — arrow parameters require annotation
```

### 2.10 Type Compatibility and Conversion

#### 2.10.1 Implicit Conversion

| From | To | Allowed and condition |
|--|--|--|
| `T` | `T` (including `int`=`i64`, `float`=`f64`) | ✅ identity |
| `i8 → i16 → i32 → i64` | a wider type on the chain | ✅ lossless widening |
| `u8/byte → u16 → u32 → u64` | a wider type on the chain | ✅ lossless widening |
| `f32` | `f64` (=`float`) | ✅ lossless widening |
| Integer literal | a unique integer / floating context | ✅ direct typing; the integer target represents it, or the floating target represents it exactly |
| Typed integer | another signedness, a narrower type, or fixed-width ↔ `isize`/`usize` | ❌ explicit `as` required |
| Typed integer | any floating type | ❌ explicit `as` required |
| Typed float | any integer type or `f64 → f32` | ❌ explicit `as` required |
| `T` | `T?` | ✅ |
| `int` / `float` / `string` / `bool` / `null` | `Json` | ✅ JSON scalar enters the value domain |
| Any other type | `Json` | ❌ `Json.encode(value)` required |
| `null` | `T?` | ✅ |
| Subtype | Supertype (class) | ✅ |
| Record with a different field set | Record | ❌ a sealed Record requires an exact field set |

> **Width rule for sealed Records**: Record assignment requires an **exact field set** — the source field names must match the target's. Fields whose declared type admits null may be omitted; every other field can be neither missing nor extra. Xray has no width subtyping; both `superset → subset` and `subset → superset` are rejected.
> ```xray
> type User = { name: string }
> var full = { name: "A", age: 18 }
> // var u: User = full            // compile error E0352: extra field 'age'
>
> type Opt = { name: string, age: int? }
> var o: Opt = { name: "A" }       // OK: age is nullable and may be omitted
> ```

> **Record and Json are two disconnected semantic domains**: `Record` is a closed field set checked at compile time; `Json` is an open data-exchange value domain resolved at run time. There is no implicit conversion between them, only two explicit bridges:
> - `Json.encode(value)`: typed value → `Json`
> - `json as T` / `json as T?`: `Json` → a Record or other typed value (structural narrowing)

#### 2.10.2 Explicit `as`

```xray
var n = x as int        // throws TypeError on failure
var n = x as int?       // returns null on failure (safe cast)
```

Applies to:
- Between numeric types (including `Json → int`, checked at runtime).
- `Json → User` (structural narrowing).
- Parent → child (downcast).

Numeric `as` is independent of the host C compiler, optimization level, and VM/AOT backend: integer-to-integer conversion reduces modulo the target width and interprets the same bit pattern with the target signedness; integer-to-float and `f64 → f32` use IEEE-754 round-to-nearest, ties-to-even, overflow produces signed infinity, and NaN is normalized to Xray's canonical quiet NaN; float-to-integer truncates toward zero and throws `XR_ERR_OVERFLOW` (E0422), with message `numeric conversion is out of range`, for NaN, infinity, or a value outside the target range.

`expr as T?` is reserved for fallible dynamic / structural conversion; it is not a numeric checked-cast form. Numeric conversions use `expr as T` and follow the deterministic rules above.

#### 2.10.3 `is` Check

```xray
if (v is User) {
    // In this branch the compiler narrows v's type to User
}
```

Acts only as a type guard; does not change the value.

### 2.11 typeOf / typeName / Type Enum

```xray
typeOf(value)     // returns a Type enum value (an int representation)
typeName(value)   // returns the type name as a string
```

`Type` enum members:

`Type.int`, `Type.float`, `Type.string`, `Type.bool`, `Type.null`,
`Type.Array`, `Type.Map`, `Type.Set`, `Type.Channel`, `Type.Json`,
`Type.function`, `Type.class`, `Type.struct`, `Type.enum`, `Type.module`, `Type.bigint`, ...

Use `typeName(value)` to obtain the concrete debug name of a value's type.

### 2.12 Metadata and Type Identity Boundary

Xray keeps only the minimal type identity layer by default:

- `typeOf(x)` returns a stable `Type` / `TypeId` for branches, `match`, and analyzer narrowing.
- `typeName(x)` returns a debug/logging type-name string and is a cold-path capability.
- Nominal type checks use `x is T` / `x as T`; do not compare type-name strings.
- Field, method, and constructor enumeration is not a default runtime capability. Structured metadata for serialization, inspect, RPC schema, and similar use cases is generated explicitly by `@derive(...)` or compile-time tooling.

Runtime type queries use `typeOf(value)`, `typeName(value)`, and `TypeId`. Reflection metadata is not exposed as a traversable or callable object graph.

### 2.13 Flow-Sensitive Type Narrowing

> Source of truth: `src/frontend/analyzer/xanalyzer_flow.c` (fact operators and control-flow propagation), `src/frontend/analyzer/xanalyzer_visitor_expr.c` (reference-site query and `E0379`).

**Narrowing** tightens the **static type** of a binding at a specific control-flow position to a subtype of its declared type. Narrowed types feed member lookup, overload resolution, `match` exhaustiveness, and **code generation**, so narrowing is **semantics**, not a diagnostic optimization: for the same program the VM and AOT backends must compute identical narrowing results.

Rules `N-1` … `N-13` in this section are normative. Each has a conformance case under `tests/compile_errors/narrowing/` or `tests/regression/16_narrowing/`.

#### 2.13.1 Narrowable Subjects

**N-1** Only **simple bindings** narrow: local `var` / `const` bindings and bare parameter identifiers.

**N-2** The following positions **never narrow**, even after a null check or an `is` check: field access `p.f`, `this.f`, index `a[i]`, tuple component `t.0`, call results `f()`, and any other non-identifier expression. A check on such a position determines the type of the check expression itself only; it does not affect later accesses spelled the same way.

> Rationale: only a simple binding has all of its write sites statically enumerable within the function. Fields and elements can change through an alias, another coroutine, or a method call; narrowing them would require place equivalence plus alias invalidation analysis, whose cost and uncertainty exceed the benefit.

**N-3** To get flow sensitivity for a non-narrowable position, extract a local binding first:

```xray
class Address { city: string
    constructor(city: string) { this.city = city } }
class User { address: Address?
    constructor(address: Address?) { this.address = address } }

fn show(u: User) {
    var addr = u.address          // extract into a simple binding
    if (addr != null) {
        print(addr.city)          // OK: addr is narrowed to Address
    }
}
```

#### 2.13.2 Facts and Operators

**N-4** A condition expression produces **facts** in two directions: a true fact and a false fact. The table below is the complete list; forms not listed produce no facts in either direction. `x` is a narrowable subject (N-1); `e` is any condition expression.

| Condition form | True branch | False branch |
|--|--|--|
| `x` | remove `null` | no fact (`0` / `""` / `false` are falsy too) |
| `!e` | false fact of `e` | true fact of `e` |
| `(e)` | true fact of `e` | false fact of `e` |
| `x == null` / `null == x` | keep only `null` | remove `null` |
| `x != null` / `null != x` | remove `null` | keep only `null` |
| `x is T` | intersect with `T` | remove the part intersecting `T` |
| `typeOf(x) == Type.K` | keep members of kind `K` | remove members of kind `K` |
| `typeOf(x) != Type.K` | remove members of kind `K` | keep members of kind `K` |
| `e1 && e2` | true facts of `e1` then `e2` | no fact |
| `e1 \|\| e2` | no fact | false facts of `e1` then `e2` |

**N-5 (short-circuit inheritance)** In `e1 && e2`, `e2` is analyzed under the true fact of `e1`; in `e1 || e2`, `e2` is analyzed under the false fact of `e1`. Because `T?` cannot be used directly as a condition (§2.5), this rule makes the two forms below the standard way to work with nullable values:

```xray
fn check(a: string?) -> bool {
    if (a != null && len(a) > 0) { return true }     // e2 analyzed with a: string
    if (a == null || len(a) == 0) { return false }   // e2 analyzed with a: string
    return true
}
```

**N-6 (intersection semantics)** True direction of `x is T`: when the declared type is a union, keep the members that intersect `T` (none → `never`); otherwise the result is `T` when the two intersect and `never` when they do not. The false direction symmetrically removes the intersecting part. `x is T` treats primitive types, named types, and generic instances alike.

#### 2.13.3 Propagation

**N-7** Facts take effect in: the then / else body of `if`, both arms of a conditional expression `c ? a : b`, the loop-body entry and the code after `while` / `for` (false fact), the right operand of `&&` / `||`, and the code following `assert(c)`.

**N-8 (early exit)** When a branch necessarily terminates (`return` / `throw` / `break` / `continue`), the code after it inherits the opposite-direction fact:

```xray
fn nameLen(s: string?) -> int {
    if (s == null) { return 0 }
    return len(s)                 // s is narrowed to string here
}
```

**N-9 (join)** The type at a join point is the union of the types on all predecessor paths; unreachable predecessors contribute `never` and are absorbed by the union.

**N-10 (loops)** The type at a loop header is the union of the entry edge and every back edge. A back edge that assigns the binding contributes the assigned expression's type; a back edge that reaches the header **without** an assignment leaves the value unchanged and contributes nothing to the union. An assignment inside the loop body therefore makes the next iteration re-derive the condition narrowing from the joined type, while a narrowing established before the loop survives a body that never writes the binding:

```xray
fn drain(first: string?) {
    var cur = first
    while (cur != null) {         // cur is string at loop-body entry
        print(cur)
        cur = null                // back edge contributes null; header re-joins
    }
}
```

#### 2.13.4 Invalidation

**N-11** Narrowing is invalidated by:

1. **assignment** / compound assignment / `++` / `--`: the static type resets to the static type of the assigned expression;
2. being passed as a `ref` argument: resets to the declared type;
3. `move x`: the binding becomes unusable (§10);
4. being **assigned inside any closure body**: the binding does not narrow anywhere in the function body, because when the closure runs is unknowable. The rule does not depend on where the closure appears — one written after the narrowing site suppresses it just the same. The diagnostic names this cause; the fix is a fresh binding that is never written;
5. an ordinary function call does **not** invalidate narrowing — N-1 / N-2 guarantee a narrowable subject cannot be written by a callee.

**N-11.1 (function-body boundary)** Every function body — named function, method, or closure — owns its flow graph: facts from the enclosing body do **not** enter a closure body, and facts inside a closure do not escape it. Re-check inside the closure when narrowing is needed there.

```xray
fn f(a: string?) {
    if (a != null) {
        print(len(a))             // OK
        a = null                  // assignment invalidates the narrowing
        print(a ?? "")            // must be unwrapped again
    }
}
```

#### 2.13.5 Narrowing and Null Diagnostics

**N-12** When the static type of a receiver can still be `null` (including a type that is always `null`), member access, indexing, calls, arithmetic, `len()`, and iteration all report `E0379` (`XR_ERR_ANALYZE_POSSIBLY_NULL`); see §18.2. Three exceptions: `==` / `!=`, which is how narrowing starts; `&&` / `||`, whose operands are checked as conditions; and **string concatenation** — when either side of `+` is a `string`, a null operand renders as `"null"` per §2.5.

**N-13** There are exactly three unwrapping forms, each independent of N-4:

- `x!`: statically removes `null`; panics (`NullError`) at run time when the value is `null` — this is **not** undefined behavior;
- `x ?? d`: the result type is the union of `x` without `null` and `d`;
- `x?.f`: optional chaining, **whole-chain short-circuit** — when any link is `null` the entire postfix chain evaluates to `null`, and the result type is nullable (§3.6).
### 2.14 Ownership, Aliasing, and Loans

> Truth source: `src/frontend/analyzer/xa_ownership.h` (evidence axes and decision structures), `src/frontend/analyzer/xanalyzer_visitor_expr.c` (`move` decision), `src/frontend/analyzer/xanalyzer_visitor_stmt.c` (alias and loan tracking), `src/ir/xi_source_move_verify.c` (independent Xi-level re-check).

Xray has no lifetime syntax and no borrow-checker annotations. Ownership is nonetheless **defined**: `move`, `copy`, `ref`, `Slice<T>`, and every cross-coroutine transfer read one decision procedure, and this section states it.

#### 2.14.1 Ownership roots

An **ownership root** is the entry point of a heap object graph that can be reclaimed on its own. `Array`, `Map`, `Set`, `Json`, `Record`, class instances, and a unique-result `Task<T>` each have their own root. Scalars, `string`, `Slice<T>`, raw pointers, value structs, and fixed arrays have **no** root: they are copied by value or they are borrowed views.

Only a binding that owns a root can transfer ownership. Writing `move` on a rootless value is a compile error (`E0391`: `move is not meaningful for value type`).

#### 2.14.2 Four independent evidence axes

At every program point a binding carries four **mutually independent** pieces of evidence. A legal ownership operation requires all four at once:

| Axis | Question it answers | Values |
|--|--|--|
| **Binding state** | Is this name usable now | `UNINITIALIZED` / `LIVE` / `MOVED` / `MAYBE_MOVED` / `UNKNOWN` |
| **Root aliasing** | Does another reference reach the same root | `UNIQUE` / `LOCAL_ALIASED` / `ESCAPED` / `ALIAS_UNKNOWN` |
| **Capability** | What is permitted | `MUTABLE` / `CONST` / `SYNC_INTERIOR_MUTABLE` / `UNKNOWN` |
| **Loans** | Is anything borrowed out | a set of loans: Slice views / raw pointer borrows / closure captures |

The split is deliberate: binding state is a CFG fact, aliasing is an object-graph fact, capability is a permission, and a loan is a bounded place fact. None of the four can be derived from or substituted for another.

**Fail-closed by default**: when an axis cannot produce positive evidence, the answer is rejection, not permission. That is why the result of a call with unknown provenance cannot be moved — the compiler has no aliasing evidence for it.

#### 2.14.3 How aliases are created and end

| Action | Effect on root aliasing | Recoverable |
|--|--|--|
| `var b = a` | `LOCAL_ALIASED` | Yes. After `b`'s last use the root is `UNIQUE` again |
| `arr.push(a)` / `obj.f = a` / `m[k] = a` / `[a]` / `#{k: a}` / `Enum.V(a)` | `ESCAPED` | **No.** Function-local analysis cannot see that slot being overwritten |
| Result of a call with unknown provenance | `ALIAS_UNKNOWN` | No |
| `copy(a)` | `a` unaffected; the result is a fresh `UNIQUE` root | — |
| `move a` | `a` becomes `MOVED`; the root moves with it | — |

Liveness is decided by **last use**, not by lexical scope, matching the borrow rules in §2.4.2. So the first function below is legal and the second is not:

```xray
fn ok() {
    var buf = [1, 2, 3]
    var alias = buf
    print(len(alias))          // alias's last use
    consume(move buf)          // OK: the alias has ended
}

fn rejected() {
    var buf = [1, 2, 3]
    var alias = buf
    consume(move buf)          // E0391: strong alias 'alias' remains live
    print(len(alias))
}
```

`ESCAPED` being terminal is deliberate: once a reference is written into a heap graph, who still holds it is no longer a question this function can answer. Use `copy(a)` when a transfer is needed anyway.

#### 2.14.4 How loans are created and end

Three loan forms share one loan record, one non-lexical liveness rule, and one set of error codes (`E0382` / `E0383` / `E0384`):

| Form | Borrower | Live until |
|--|--|--|
| `Slice<T>` view | the view binding | that binding's last use |
| `Ptr<T>` / `MutPtr<T>` | the pointer binding | that binding's last use |
| **Closure capture** | the closure binding | that binding's last use |

An ordinary synchronous closure captures an outer binding **by reference**, so a capture is a loan and not a copy:

```xray
fn rejected() {
    var buf = [1, 2, 3]
    const peek = fn() -> int { return len(buf) }
    go consume(move buf)       // E0382: closure capture 'peek' is active
    print(peek())
}
```

A closure literal that appears only as a **call argument** usually creates no live loan: it ends with the call and cannot outlive it.

```xray
fn ok() {
    var buf: Array<int> = []
    items.forEach(fn(x: int) { buf.push(x) })   // capture bounded by the call
    consume(move buf)                            // OK
}
```

The exception is a callee that **retains or escapes** that parameter: the closure then outlives the call, and every root it captured by reference escapes with it (`OWN-E-ESCAPED-ROOT`). The decision reads the callee's parameter effect summary, not the syntactic shape.

A live loan forbids invalidating operations on the owner, and `move` is one of them (`E0382`).

#### 2.14.5 The full conditions for `move`

`move x` requires `x` to be a **rebindable local `var` root**, and:

1. binding state is `LIVE` (not moved, not maybe-moved, not unknown);
2. root aliasing is `UNIQUE`;
3. capability is `MUTABLE` (`const` values and synchronization handles cannot be moved);
4. no loan is live;
5. the storage plan is complete (the compiler has resolved an allocation domain for the root);
6. the consumed binding is not declared outside a loop that would run the `move` again.

`move` accepts an **identifier** only: `move x.field`, `move arr[i]`, and `move f()` are syntax errors. A field or an element has no ownership root of its own — its root is the container — and transferring one slot would leave the container partially moved, a state with no representation. To take one slot out, `copy` it, or make the container itself the move source.

After a successful move the source binding is statically marked moved, and any later reference is a compile error. **A rejected move does not poison the source**: after the diagnostic, `x` is still usable.

Rejection reasons are named in the diagnostic so the failing axis is identifiable:

| Reason | Meaning |
|--|--|
| `OWN-E-LIVE-ALIAS` | a local strong alias is still live |
| `OWN-E-ESCAPED-ROOT` | the root was written into a heap graph |
| `OWN-E-UNKNOWN-CALL` | uniqueness evidence is incomplete (call result with unknown provenance) |
| `OWN-E-STORAGE-PLAN` | the storage / ownership plan is incomplete |
| `OWN-E-LIVE-LOAN` | a loan is live (Slice view / raw pointer / closure capture) |

#### 2.14.6 Value copies and managed fields

A value struct is copied by value. So that "copied by value" is always the complete semantics, **struct field types are restricted**: only scalars, `string`, raw pointers, fixed arrays, and other value structs are allowed. `Array`, `Map`, `Set`, `Json`, and class instances **cannot** be struct fields (`E0352`).

A struct value copy therefore never carries a mutable managed field, and there is no shallow-versus-deep choice to make. The one managed field type is `string`, and `string` is immutable: sharing it produces no observable difference and does not affect the uniqueness decision.

Use a class when an aggregate needs a mutable graph. A class is a reference type, so assignment creates an alias and §2.14.3 governs it.

### 2.15 Worked Examples

Self-contained programs that run as-is and pass `xray check` (comments show the real output).

Arrays:

```xray
fn main() {
    var nums = [1, 2, 3]
    nums.push(4)
    print(nums)          // => [1, 2, 3, 4]
    print(len(nums))     // => 4
    var doubled = nums.map(fn(x: int) -> int { return x * 2 })
    print(doubled)       // => [2, 4, 6, 8]
    var evens = nums.filter(fn(x: int) -> bool { return x % 2 == 0 })
    print(evens)         // => [2, 4]
}

main()
```

Maps and Sets:

```xray
fn main() {
    var scores = #{"alice": 95, "bob": 88}
    scores.set("carol", 77)
    print(scores.get("alice") ?? 0)   // => 95
    print(len(scores))                 // => 3

    var seen = Set<int>()
    seen.add(1)
    seen.add(2)
    seen.add(2)
    print(len(seen))          // => 2
    print(seen.contains(1))   // => true
}

main()
```

Nullable types with `??`:

```xray
fn main() {
    var name: string? = null
    print(name ?? "anonymous")   // => anonymous
    var city: string? = "NYC"
    print(city ?? "unknown")     // => NYC
}

main()
```

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

```xray
fn t(tag: string, v: int) -> int { print(tag); return v }
fn add(a: int, b: int) -> int { return a + b }
fn pick(tag: string) -> (int, int) -> int { print(tag); return add }

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

```xray
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

```xray
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

```xray
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
- **Object/record literal spread**: `{...base, x: 1}`; the spread source must be an object. Fields are merged into a new object; on a name clash the later field wins, and the result field set is the union of every source's fields and the literal fields.

```xray
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

```xray
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

```xray
var m = #{"a": 1, "b": 2}
var empty = #{}                           // empty Map
```

**Key distinction**: `{}` is always a **Json / Object**; `#{}` is always a **Map**. Both use `:` between key and value; the `#` prefix is the disambiguator.

#### Set `#[...]`

```ebnf
SetLit ::= '#[' (Expr (',' Expr)* ','?)? ']'
```

```xray
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

```xray
var p = { name: "Alice", age: 30 }
var users = "Bob"
var obj = { users }              // shorthand
```

- Defaults to sealed structural `Record` (see §2.4.7); the field set and offsets are fixed at compile time for AOT fast paths.
- It is interpreted as a dynamic Json object literal only under an explicit `Json` expected type; use `Json.encode(value)` when a typed value crosses a JSON boundary.
- Name the Record with a `type` alias: `var u: User = {...}` (compile-time field check, sealed).

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

```xray
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

```xray
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

Xray has three anonymous-function forms, all compiled to the same `AST_FUNCTION_EXPR` node with fully equivalent semantics; they differ only in conciseness and applicable position.

```ebnf
AnonFunction ::= BareLambda | ArrowLambda | FnExpression
BareLambda   ::= Identifier '->' (Expression | Block)
ArrowLambda  ::= '(' ArrowParams? ')' '->' (Expression | Block)
ArrowParams  ::= ArrowParam (',' ArrowParam)*
ArrowParam   ::= Identifier (':' Type)?      // type optional, inferred from context
FnExpression ::= 'fn' GenericParams? '(' Params ')' ('->' Type)? Block
```

```xray
// ── Bare lambda: unparenthesized single parameter, usable in any expression position ──
arr.map(x -> x * 2)
arr.filter(x -> x % 2 == 0)
var double: (int) -> int = x -> x * 2

// ── Arrow lambda: supports multiple parameters and parameter type annotations ──
var sum = arr.reduce((acc, x) -> acc + x, 0)    // no type
var typedDouble = (x: int) -> x * 2              // typed
var add = (a: int, b: int) -> a + b              // multi-param

// ── fn expression: multi-statement body, return-type annotation, generics ──
var inc = fn(x: int) -> int {
    var y = x + 1
    return y
}
var identity = fn<T>(x: T) -> T { return x }     // generic
```

**Choosing among the three**:

| Form | Syntax | Suitable for |
|------|------|----------|
| Bare lambda | `x -> expr` | untyped single-parameter functions in any position |
| Arrow lambda | `(x, y) -> expr` | multiple parameters or parameter type annotations |
| fn expression | `fn(x: T) -> R { ... }` | multi-statement body, return-type annotation, generics |

**Key rules**:
- **Bare lambda** (`x -> expr`): usable in any expression position, with exactly one untyped, unparenthesized parameter. Its type is inferred from context such as the assignment target, return type, callee signature, or container element type.
- **Arrow lambda** (`(x) -> expr`, `(x, y) -> expr`): usable in any position. Parameter types may be omitted and inferred from context; inference failure raises `E0365`. Arrow lambdas **do not support return-type annotations**; use `fn(x: T) -> R { ... }`, or annotate the binding as a function type: `var f: (T) -> R = (x) -> ...`.
- **fn expression** (`fn(x: T) { ... }`): usable in any position. Supports generic parameters `fn<T>(...)`, return-type annotation `-> T`, and a multi-statement body.
- Single-expression form `-> expr` implicitly `return`s.
- Block form `-> { ... }` or `{ ... }` uses an explicit `return`.
- Capture rules: see §7.4. A `go` closure consumes the unified provenance-based capture plan: inline values, published const values, and audited synchronization handles may be captured directly; execution-local graphs, module-mutable state, and views/pointers with insufficient lifetime are rejected and must cross as explicit `copy(...)` / `move` arguments.

### 3.13 `match` Expression

```ebnf
MatchExpr ::= 'match' Expr '{' MatchArm (',' MatchArm)* ','? '}'
MatchArm ::= Pattern ('if' Expr)? '->' Expression
```

```xray
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

```xray
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
```xray
var a = [1, 2, 3]              // equivalent to Array<int>() + push
var m = #{}                    // equivalent to Map<...>()
var p = Point{x: 1, y: 2}      // struct literal
```

### 3.15 String Interpolation

See §1.6.5. In brief:

```xray
var greeting = "Hello, ${name}! Age: ${user.age + 1}"
```

- `${...}` accepts any expression (calls, object access, arithmetic).
- Embedded string literals inside `${...}` may use the same quote as the outer template; the lexer matches expression braces by depth and skips nested strings / raw strings / rune literals.
- The expression's type must be convertible to a string (implement `toString()` or be a primitive).

### 3.16 `yield` Statement

```xray
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

---

## 4. Statements

> Source of truth: `src/frontend/parser/xparse_stmt.c`, `src/frontend/parser/xast_nodes_stmt.h`.

Xray statements are separated by `\n` or `;`; the trailing `;` is optional in most positions, only the three sections of a `for` loop (init / cond / step) require `;` separators.

### 4.1 Expression Statements and Blocks

```ebnf
ExprStmt   ::= Expression (';' | LineBreak)
IncDecStmt ::= Identifier ('++' | '--') (';' | LineBreak)
Block      ::= '{' Statement* '}'
```

```xray
foo()                  // expression statement
x = 1                  // assignment expression as a statement
x++                    // increment statement; produces no expression value
{                      // block
    var y = 2
    y + 1              // expression with discarded result
}
```

`++` / `--` are pure statements or `for` step items, and can only be written as `name++` / `name--`. They are equivalent to `name = name + 1` / `name = name - 1` and produce no value; expression-position uses such as `var y = x++`, `f(x++)`, `a[i++]`, and `return x++` are compile errors.

**Note**: a block is **not an expression** — it has no value. To get a value out of a block, use `match` or wrap it in an immediately-invoked function.

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

**Constraints**:
- The condition **must** be parenthesized (unlike Go/Rust).
- The condition must be `bool` or nullable presence `T?` (`T != bool`); bare `bool?`, `int`, `string`, collections, etc. are compile errors (see §2.3.3).
- Branch bodies must be blocks `{...}`; **no** single-statement-without-braces form.
- `if` is not an expression; for an expression form use the ternary `? :` or `match`.

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

There is no `do-while` form.

### 4.4 `for` (C-style) and `for-in`

#### C-style `for`

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

- Variables declared in `ForInit` are scoped to the loop body.
- `i++` / `i--` in the step position must be the entire step; put multiple updates at the end of the loop body.
- All three sections may be omitted: `for (;;)` is an infinite loop.

#### Single-variable `for-in`

```ebnf
ForInStmt ::= LoopLabel? 'for' '(' Identifier 'in' Expression ')' Block
```

The `for-in` iteration variable is a fresh immutable binding for each iteration; a closure captures the value of that iteration's binding when the closure is created.

```xray
for (item in [1, 2, 3]) { print(item) }
for (i in 0..n) { print(i) }                  // range iteration (half-open)
for (ch in "hello") { print(ch) }             // string characters (by Unicode scalar)
for (key in someMap) { print(key) }           // single variable over Map → key
for (key in someJson) { print(key) }          // single variable over Json → key
for (color in Color) { print(color.name) }    // unit-only enum; yields Color values
for (variant in Event.variants) {             // any concrete enum; yields EnumVariant<Event>
    print(variant.name)
}
for (_ in 0..n) { count++ }                   // discard with placeholder
```

#### Two-variable `for-in` destructuring

Xray supports two equivalent two-variable forms:

```ebnf
ForInPairStmt ::= LoopLabel? 'for' '(' Identifier ',' Identifier 'in' Expression ')' Block
              |  LoopLabel? 'for' '(' '(' Identifier ',' Identifier ')' 'in' Expression ')' Block
```

```xray
// Form A: two bare identifiers (more common)
for (k, v in someMap) { print("${k}=${v}") }     // Map → (key, value)
for (i, e in someArray) { print("${i}: ${e}") }  // Array → (index, element)
for (i, c in "hello") { print("${i}:${c}") }     // string → (index, rune)

// Form B: tuple-parenthesized (pairs well with .entries())
for ((i, e) in someArray.entries()) { print("${i}=${e}") }
for ((i, c) in "hi".entries()) { print("${i}-${c}") }
```

Iteration source / yield mapping:

| Collection type | Single-variable yield | Two-variable yield |
|---|---|---|
| `Array<T>` / `T[]` | element | (index, element) |
| `Map<K, V>` | key | (key, value) |
| `Json` | key (string) | (key, value) |
| `string` | `rune` | (index, rune) |
| `Range` (`a..b`) | int | — |
| Concrete enum type `E` with unit-only variants | actual `E` values (declaration order) | — |
| Enum type `E` containing a payload variant | **compile error**; use `E.variants` | — |
| `EnumVariants<E>` | `EnumVariant<E>` descriptors (declaration order) | — |
| `EnumPayloads<E>` | `EnumPayloadField<E>` descriptors (declaration order) | — |
| Custom `Iterator<T>` | T | — |

Enum type domains and descriptor views are compiler-recognized static domains. They do not make enums conform to `Iterable`, and they do not implicitly construct arrays or iterator objects. `E` must have a compile-time-known concrete enum layout; `E.variants` on an unconstrained type parameter is rejected. See §5.6.5.

#### Custom iterators

Implement an `iterator()` method that returns an `Iterator<T>` protocol object (with `hasNext()` and `next()`) and the value becomes usable in `for-in`. See §5.3.6.

### 4.5 `match` Statement

```ebnf
MatchStmt ::= 'match' '(' Expression ')' '{' MatchArm (','? MatchArm)* ','? '}'
MatchArm  ::= Pattern ('if' '(' Expression ')')? '->' (Expression | Block)
```

**Key syntax**:
- The matched expression **must** be parenthesized: `match (x) {...}`.
- Commas between arms are **optional** — both styles can be mixed in the same `match` (omitting commas is more common).
- Guard expressions following `if` must be parenthesized: `n if (n > 0)`.

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

`match` may serve as either a statement or an expression (see §3.13); when used as an expression, the arm body must be a single expression or end with one as the last expression of a block.

For pattern details see [§6](#6-patterns).

### 4.6 `break` / `continue`

```xray
break                  // exit the innermost loop
continue               // proceed to the innermost loop's next iteration
break outer            // exit the loop labeled outer
continue outer         // proceed to the next iteration of the loop labeled outer

outer: for (i in 0..10) {
    for (j in 0..10) {
        if (j == 3) { continue outer }
        if (i * j > 20) { break outer }
    }
}
```

**Constraints**:
- Must appear inside a `while` / `for`; otherwise the compile errors `E0304` / `E0305`.
- `break` / `continue` inside a `match` does **not** affect `match` itself; it exits the enclosing loop.
- Loop labels are written as `label: for (...)` or `label: while (...)` and may only annotate loops; `label:` before a non-loop statement is a compile error.
- `break label` / `continue label` must refer to an active enclosing loop label; unknown labels and duplicate labels in the active loop stack are compile errors.
- Unlabeled `continue` targets the innermost loop. Labeled `continue` targets the named loop: `while` rechecks its condition, C-style `for` runs its step before rechecking, and `for-in` advances to the next item.

### 4.7 `return`

```ebnf
ReturnStmt ::= 'return' ReturnValue? (';' | LineBreak)
ReturnValue ::= Expression | '(' Expression (',' Expression)+ ')'
```

```xray
fn done() {
    return                 // implicitly returns () (Unit)
}

fn answer() -> int {
    return 42
}

fn pair(a: int, b: int) -> (int, int) {
    return (a, b)          // multi-value return must wrap a tuple in parens
}
```

> **Note**: multi-value returns must use the tuple form `return (a, b)`; the bare-comma form `return a, b` is the compile error `E0801`.

**Constraints**:
- Allowed only inside a function body (including closures); a top-level `return` is the compile error `E0306`.
- The returned value's type must be compatible with the function's declared return type.

### 4.8 `throw` / `try` / `catch`

```ebnf
ThrowStmt     ::= 'throw' Expression

TryStmt       ::= 'try' Block CatchClause+
CatchClause   ::= 'catch' '(' Identifier (':' Type)? ')' Block
                | 'catch' 'panic' '(' Identifier ')' Block
```

```xray
enum AppError { NotFound, Timeout(ms: int) }

// Recoverable errors: enum values flow through the value-return channel,
// caught by catch (e)
try { throw AppError.NotFound } catch (e) {
    match (e) {
        AppError.NotFound -> log.error("not found"),
        AppError.Timeout(ms) -> log.error("timeout after ${ms}ms")
    }
}

// catch panic (p) is a separate boundary for runtime faults
// (div-by-zero, out-of-bounds, expr!)
try { risky() } catch panic (p) {
    log.error("fault:", p)
}

throw AppError.NotFound                      // value-return error channel
// There is no finally; use defer for deterministic cleanup (see §4.9)
```

**Semantics**:
- A `try` must be followed by at least one `catch` or `catch panic` clause.
- `catch (e)` catches recoverable errors propagated through the value-return channel (a user `throw <enum>`); use `match (e)` to destructure the error value.
- `catch panic (p)` catches runtime faults (div-by-zero, out-of-bounds, `expr!`, `assert`), strictly separated from recoverable errors.
- The `throw` operand is an error value (typically an enum) propagated through the value-return channel: no `PanicInfo` allocation, no stack unwinding, and only a predictable branch at call boundaries that may propagate or catch errors.
- There is no `finally`: use `defer` (§4.9) for deterministic cleanup.
- For full error semantics see [§8](#8-error-handling).

### 4.9 `defer`

```ebnf
DeferStmt ::= 'defer' (CallExpr | Block)
```

```xray
fn read_file(path: string) -> string {
    var f = open(path)
    defer f.close()                  // always runs before the function returns
    return f.readAll()
}

fn process() {
    defer {                          // block form
        log.info("done")
        cleanup()
    }
    do_work()
}

fn snapshot_vs_reference() {
    var n = 1
    defer print("call", n)            // saves 1 at registration
    defer { print("block", n) }       // reads 2 at exit
    n = 2
}                                      // prints block 2, then call 1
```

**Semantics**:
- A `defer` body has exactly two forms: a call expression or a block. Assignments, member assignments, bare values, and every other non-call expression are syntax errors; write arbitrary cleanup as `defer { ... }`.
- The call form immediately evaluates and saves the callee receiver and every argument from left to right. Block exit only invokes that saved call. A dynamic callable that cannot be proven stable statically is rejected with `E0392`.
- The block form does not snapshot values. Its body runs at block exit and observes captured local bindings by reference at that time. Thus `defer conn.close()` saves the current `conn`, while `defer { conn.close() }` reads `conn` at exit.
- A movable owner referenced by a call snapshot or block capture is under a lexical loan until the owning block exits. Moving or returning it earlier reports `E0382`. Ordinary rebinding/mutation remains legal; the snapshot-versus-reference rule above determines which value the defer observes.
- A `defer` belongs to the nearest enclosing real block `{ ... }`. A function body is a block, so a top-level function-body `defer` still runs before the function exits.
- **LIFO**: multiple `defer` statements in the same block run in reverse declaration order.
- **Always executes**: runs when the owning block falls through or exits by `break`, `continue`, `return`, value-error propagation, or panic unwinding.
- A `defer` inside a loop body runs at the end of each iteration, not at the end of the function.
- `defer` is Xray's only deterministic-cleanup mechanism (replacing other languages' `finally`): it is bound to lexical block exits, not to a single function tail.
- **No error may escape a `defer` body**: `E0387` is reported when the deferred callable's inferred error set is non-empty; errors must be absorbed inside the `defer` body with `try` / `catch`. What static analysis cannot decide is backstopped at runtime by `E0443`. Full rules in §8.3.1.

### 4.10 Built-in Print Functions

`print` / `dump` are **built-in global functions** (not keywords; see §13.1), listed here for convenience:

```xray
print("hello")                 // auto-appends a newline
print("a:", a, "b:", b)        // multiple arguments separated by spaces
dump(some_obj)                 // debug output, with type info and structure
```

**Behavior**:
- Accepts any type and any number of arguments (variadic); each argument is automatically converted via its `toString()` or built-in formatter.
- Output goes to stdout; not part of the exception mechanism.
- Multiple arguments are separated by single spaces.
- `print` appends a newline by default (different from C/Python; consistent with regression tests).
- `dump` is for debugging; output includes type tags and internal structure.
### 4.11 Worked Examples

Combining `if` / `match` / `for-in` control flow:

```xray
fn classify(n: int) -> string {
    if (n < 0) { return "negative" }
    return match (n) {
        0 -> "zero"
        1..=9 -> "small"
        _ -> "large"
    }
}

fn main() {
    for (i in [-1, 0, 5, 100]) {
        print(classify(i))
    }
}

main()
```

Output:

```
negative
zero
small
large
```

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

```xray
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

```xray
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

```xray
const channel = Channel<int>(16)
const counter = Atomic(0)

var source = [1, 2, 3]
var moved = move source       // transfer the same root; source is now invalid
const snapshot = copy(moved)  // explicitly construct an independent read-only graph
var current: const Config = loadConfig()
```

See [§10.11](#1011-concurrency-safety-model).

#### 5.1.3 Destructuring bindings

```xray
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

```xray
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

```xray
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

```xray
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

```xray
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

```xray
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

**What a native unknown is**: a bodyless `extern "C"` declaration has no inferable Xray semantics, so the only admissible evidence about it is that symbol's `[native.symbol.contract]` in `xray.toml`. Fields the contract declares (`allocation`, `suspend`, and so on) enter the caller's effect conclusion as axioms; without a complete contract the corresponding semantic bits are marked unknown and any contract covering them fails closed. **An absent body is not a proof.**

**Inference coverage** (status: partially implemented): `requires` values backed by a real analysis pass today are `no_semantic_alloc`, `no_suspend`, `no_throw` (semantic scope) and `no_runtime_heap` (backend scope). The semantic effect bits behind `no_block`, `no_thread_block`, `no_panic`, and `no_abort` are computed by no pass, so `xray verify` rejects those four with a "no inference source" witness instead of granting them vacuously. They become accepted once their analyses land.

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

```xray
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

```xray
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

#### 5.3.6 `Iterator<T>` and `Iterable<T>`

`Iterator<T>` is a built-in prelude interface. It is the pull protocol behind `for-in` and the return type of a generator function (§3.16):

```xray
interface Iterator<T> {
    hasNext() -> bool       // is another element available; does not consume one
    next() -> T             // take the next element and advance
    nth(index: int) -> T    // advance index elements from the current position and return it
}

interface Iterable<T> {
    iterator() -> Iterator<T>
}
```

Rules:

- Any type implementing `iterator()` can be used with `for-in`; the built-in collections and `string` (by `rune`) already do.
- `for-in` guarantees only that it pulls through `hasNext()` / `next()`; nothing beyond the number and ordering of those calls is guaranteed.
- `next()` returns `T`, not `T?`: **exhaustion is not encoded in the return value**. The protocol is two-step — every `next()` / `nth()` must be preceded by a `hasNext()` that returned `true`. Calling `next()` after `hasNext()` is `false` violates the contract: the runtime reports it as an `E0432` panic rather than returning a zero value or a `null` that `T` forbids (§18.3).
- `Iterator<T>` is **single-use**: once exhausted, `hasNext()` stays `false` and it cannot be reset. Call `iterator()` again, or call the generator function again, to traverse again.
- Mutating the underlying collection during iteration is defined by that collection; the built-in collections invalidate their iterators (§14).
- `Iterator<T>` has **no** `close()`: abandoning an iterator early runs no cleanup (§3.16.3), which is why `defer` is rejected inside a generator body.

A generator function (one whose body uses `yield expr`) implements this interface automatically; it is never written by hand.

#### 5.3.7 Computed Properties

A field name followed by an accessor block declares a **computed property**: it has no storage slot, and reads and writes become accessor calls.

```xray
class Rect {
    _w: int
    _h: int
    constructor(w: int, h: int) { this._w = w; this._h = h }

    // Read-only: getter alone
    area: int { fn() { return this._w * this._h } }

    // Readable and writable: getter + setter
    width: int {
        fn() { return this._w }
        fn(v: int) { this._w = v }
    }
}

fn main() {
    var r = Rect(3, 4)
    print(r.area)       // => 12
    r.width = 10
    print(r.area)       // => 40
}
```

Rules:

- An accessor block contains only `fn` definitions. The one taking **no parameter** is the getter; the one taking **exactly one** is the setter. At most one of each; more than one parameter is a compile error.
- The getter's return type defaults to the declared property type, as does the setter's parameter type; both may be omitted.
- The property type is the type of a read. A property with only a getter is **read-only**; writing it reports `E0380` as an undeclared member.
- `obj.p` and `obj.p = v` are ordinary calls, **not** slot accesses: an accessor may compute and may have effects, and it costs one method call.
- A computed property satisfies an interface's property requirement — an interface does not distinguish a slot from an accessor.
- Accessors live in the class method table as `get:<name>` / `set:<name>`. That name contains `:`, which no identifier can, so it never collides with a declared method; this is an implementation detail, not writable syntax.

#### 5.3.8 Worked Examples

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

```xray
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

**Field rules for aggregate literals** (identical to sealed Records):

- Every field name in the literal must be declared by the type; an undeclared name is compile error `E0380`.
- Every declared field must be set. Only fields that carry a **declaration default** or whose **type admits null** may be omitted; any other omission is compile error `E0381`.
- Use `Point()` when a whole zero value is what you want; do not obtain zero values implicitly by omitting literal fields.
- **`union` is the exception**: its members share one storage location and exactly one is live, so a union literal sets **exactly one** member. Setting none (which member is live would be undefined) or several (the writes overwrite one another) is `E0381`.

```xray
struct Config {
    host: string
    port: int = 8080        // declaration default: omittable in a literal
    label: string?          // nullable: omittable in a literal
}

var c = Config{host: "localhost"}    // OK
// Config{host: "h", ports: 1}       // compile error E0380: no field 'ports'
// Config{port: 1}                   // compile error E0381: missing field 'host'
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

```xray
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

Enum variants use declaration order for their stable `ordinal` and do not declare a separate backing value. Express protocol numbers, string symbols, and similar external representations with `const`, methods, or explicit conversion functions.

#### 5.6.2 Payload enum

A variant name may be followed by parentheses declaring payload fields (positional or named):

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

// A recursive enum payload must be indirected through a class node
class ExprNode {
    expr: Expr
    constructor(expr: Expr) { this.expr = expr }
    get() -> Expr { return this.expr }
}

enum Expr {
    Number(int),
    Binary(op: string, left: ExprNode, right: ExprNode),
    Call(name: string, args: Array<Expr>),
}
```

A directly recursive enum payload would have infinite size and is rejected at compile time (`E0352`). Recursive data structures must use explicit indirection, in one of two forms:

- **Class node**: classes are reference types, so the field costs one pointer and `Binary(left: ExprNode, ...)` has a finite layout.
- **Container slot**: `Array<Expr>`, `Map<K, Expr>`, and other containers keep elements in their own storage, so the payload holds only the container handle.

`T?` is not indirection — nullable does not change the by-value layout of the payload, so `Binary(left: Expr?, ...)` is rejected by `E0352` as well.

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

```xray
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

```xray
Color.Red.name        // "Red"          variant name (string)
Color.Red.ordinal     // 0              declaration-order tag (int, zero-based)
Color.Red.toString()  // "Color.Red"    "<EnumName>.<VariantName>" format
```

Enum values provide `name`, `ordinal`, and `toString()`. They do not expose implicit backing-value/reflection APIs such as `value`, `rawValue`, `fromName`, or `fromOrdinal`.

#### 5.6.5 Iteration

A concrete enum containing only payload-free variants can be iterated directly as actual enum values. Every concrete enum can expose declaration metadata through `.variants`:

```xray
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

See [§2.4.7](#247-json) and [§2.8](#28-type-aliases).

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

---

## 6. Patterns

> Source of truth: `src/frontend/parser/xparse_match.c`, `src/frontend/analyzer/xanalyzer_visitor_pattern.c`, `src/ir/xi_lower_expr.c` / `xi_lower_stmt.c`, and the VM/AOT match lowerings.

Patterns appear in `match` expressions/statements and in `var` / `const` destructuring.

### 6.1 Literal Patterns

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

- Matching uses the same semantics as `==`.
- The `null` pattern matches only `null` itself.

### 6.2 Range Patterns `a..b` / `a..=b`

```xray
match (age) {
    0..13 -> "child"
    13..20 -> "teen"
    20..=65 -> "adult"
    _ -> "senior"
}
```

- `a..b` is the half-open interval `[a, b)`; `a..=b` is the inclusive interval `[a, b]`.
- Integer-only.

### 6.3 Enum Patterns

#### 6.3.1 Simple variants (no payload)

```xray
match (color) {
    Color.Red   -> "red",
    Color.Green -> "green",
    Color.Blue  -> "blue",
}
```

- Must be fully qualified as `EnumName.Variant`.

#### 6.3.2 ADT variant (with payload) destructuring

ADT variant patterns may destructure payload fields (positionally or by name):

```xray
// Positional destructuring
match (event) {
    NetEvent.Connected            -> print("connected"),
    NetEvent.Disconnected(reason) -> print("by:", reason),
    NetEvent.DataReceived(b)      -> process(b),
    NetEvent.Error(code, msg)     -> log.error(code, msg),
}

// Option patterns (positional)
match (opt) {
    Option.Some(v) -> print("got:", v),
    Option.None    -> print("nothing"),
}

// Wildcards skip payload fields you don't care about
match (event) {
    NetEvent.Error(code, _) if (code >= 500) -> throw NetErr.ServerFault(code),
    _                                         -> continue,
}

// Nested destructuring
match (msg) {
    Option.Some(NetEvent.DataReceived(bytes)) -> process(bytes),
    Option.None                               -> skip(),
    _                                         -> skip(),
}
```

#### 6.3.3 Exhaustiveness check

When `match` is performed on an ADT enum, the compiler runs **exhaustiveness analysis**:

- If every variant is covered (including `_` as a catch-all), the check passes.
- If a variant is missed, compilation fails with `E0371 XR_ERR_ANALYZE_MATCH_NOT_EXHAUSTIVE`, naming the missing variants.

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
    // ❌ E0371: missing variants DataReceived and Error; add `_ -> ...` as catch-all
}
```

> Both simple enums (no payload) and ADT enums **require** exhaustiveness; including a `_` catch-all suffices to skip the check. Non-enum operands (such as `int`) are not subject to the check.

### 6.4 Type Patterns `is T`

```xray
match (value) {
    is int n -> "int: ${n}"       // bind the narrowed value
    is string -> "a string"
    is User u -> "user: ${u.name}"
    _ -> "unknown"
}
```

- Tests the dynamic type; optionally binds a narrowed variable.

### 6.5 Guard Conditions `if`

```xray
match (x) {
    n if (n > 0 && n < 10) -> "small positive"
    n if (n < 0) -> "negative"
    _ -> "other"
}
```

- The guard must be `bool` or nullable presence `T?` (see §2.3.3), identical to `if` / `while` condition rules.
- On guard failure, matching falls through to the next arm.

### 6.6 Multi-value Patterns

```xray
match (x) {
    1, 2, 3 -> "small"
    Color.Red, Color.Yellow -> "warm"
    _ -> "other"
}
```

- Any sub-pattern matching is a success.

### 6.7 Wildcard `_`

- Matches any value without binding.
- Typically used as the trailing default arm.
- Usable in destructuring to skip positions: `var [_, b, _] = arr`.

### 6.8 Variable-binding Patterns

```xray
match (http_status) {
    200 -> "ok"
    code if (code >= 400) -> "error: ${code}"
    code -> "other: ${code}"
}
```

- A bare `Identifier` always matches and binds the value.

### 6.9 Destructuring Patterns

```xray
var [a, b, c] = some_array
var (q, r) = divmod(17, 5)
var { name, age } = user
```

See §5.1.5 for details. Within `match`, tuple, ADT-variant, object, and array destructuring are supported:

```xray
match (p) {
    { x, y } -> ...           // object field destructure (shorthand binding)
    { name: n, age } -> ...   // field rename + shorthand mixed
    [a, b, ..rest] -> ...     // array destructure; `..rest` captures the tail as a new array
    [_, mid, _] -> ...        // element-position wildcards
}
```

- An object pattern matches any object/Json carrying those fields; field reads are null-safe for a missing field. Field sub-patterns may be refutable (e.g. `{ mode: 2 }`).
- An array pattern matches by **length**: without `..rest`, the length must equal the element count; with `..rest`, the length must be ≥ the element count. Element sub-patterns may only be bindings or wildcards (non-rest elements are not value-tested — an out-of-bounds element read traps); use an `if` guard to test element values.
- The or-pattern `|` is not supported (comma-separated multi-values cover the equivalent need).

### 6.10 Exhaustiveness and Match Failure

- `match` over an enum expression is exhaustive (error code `E0371`, see §6.3.3).
- Other operand types are not enforced; if no arm matches at runtime, an `PanicInfo` with code `E0442` is raised (see §18.x).
- Always providing a `_` fallback is recommended.

---

## 7. Scoping and Name Resolution

> Source of truth: `src/frontend/analyzer/xanalyzer_symbol.c`, `xanalyzer_escape.c`, `xaddressability.c`, `xanalyzer_visitor_stmt.c`, and the capture/storage plans produced by `src/analysis/xglobal_producer.c`.

### 7.1 Lexical Scoping and Hoisting

Xray uses **lexical scoping**: a name's visibility is determined entirely by the source code structure.

**Scope kinds**:

| Scope | Triggered by | Example |
|--|--|--|
| Module | Each `.xr` file | top-level `var` `const` `fn` `class` |
| Function / closure | Entering `fn` / arrow function | parameters + function body |
| Block | `{...}` | `if` `while` `for` `match` arm body |
| `scope` block | `scope { ... }` keyword | explicit lexical scope + structured concurrency (see §10.7) |
| `for` header | `for (var i=0; ...)` | `i` is visible only within the loop body |
| `catch` parameter | `catch (e)` | `e` is visible only within the catch body |
| Class body | `class` definition | fields, methods |

**Hoisting rules**:

- Top-level `fn` `class` `struct` `interface` `enum` `type` are **hoisted** to the top of the current scope — they may be referenced before their textual definition.
- `var` / `const` are **not hoisted** — they must appear before any use.
- Duplicate names: declaring two same-named variables in the same scope is a compile error (nested scopes may shadow).

```xray
main()                    // OK: uses the hoisted fn
fn main() { ... }

var y = x                 // error: x is not declared
var x = 10
```

#### Shadow rules

A nested block may shadow a same-named variable in an outer scope:

```xray
var x = 1
{
    var x = "hello"           // shadow: OK
    print(x)                 // "hello"
}
print(x)                     // 1
```

### 7.2 Closure Capture Semantics

A closure captures variables from outer scopes as **upvalues**.

#### Plain synchronous closures

The default capture mode is **by reference**:

```xray
fn make_counter() -> (() -> int) {
    var count = 0
    return fn() -> int {
        count += 1                  // mutates the outer count
        return count
    }
}

var c = make_counter()
print(c())      // 1
print(c())      // 2
```

- The closure and the original variable **share state**.
- After the outer scope exits, a captured variable remains alive through its closure cell/upvalue and the corresponding reference counts.
- Every read/write capture has exactly one shared cell. Outer assignments and
  every capturing closure access that same cell; the cell and each newly stored
  value follow ordinary strong-reference transfers and releases, with no hidden
  keepalive rule.

#### Closure optimization

The compiler analyzes upvalues:
- read-only → may be implicitly copied (avoiding closure conversion).
- read/write → promoted to a closure box.
- See §17.5 for details.

### 7.3 Ownership and `move`

Xray is **not** a full ownership/borrow-checked language (unlike Rust). However, **cross-coroutine data transfer** uses move semantics:

```xray
var big_buffer = Array<byte>(1024 * 1024)

var t = go fn(b: Array<byte>) -> int {
    return process(b)
}(big_buffer)             // compile error: an execution-local heap value cannot cross bare

var t2 = go fn(b: Array<byte>) -> int {
    return process(b)
}(move big_buffer)        // OK: ownership transferred

print(len(big_buffer))    // compile error: accessed after move
```

**`move` usage**: `move` is a unary ownership-transfer expression commonly used in arguments and initializers (see §5.1.4 / §10.8):

- `go f(move x)`, `go fn(...){...}(move x)`: transfer ownership to the coroutine.
- `ch.send(move data)`: transfer ownership when sending across coroutines (avoiding a copy).
- Plain function call `f(move x)`: transfer ownership into the function (which becomes the sole owner).
- `var dst = move src` / `const dst = move src`: transfer a verifier-proven unique local `var` root to a new owner or const capability.
- Function returns are always written `return value`. The return edge terminates the current continuation, so the compiler automatically publishes owner-forward proof for a unique local or `move` parameter; `return move value` is not a public form.

### 7.4 Cross-Coroutine Data Transfer Rules (Race Avoidance)

"Statically eliminating data races at compile time" is a core design principle of xray's concurrency model.

Every cross-execution boundary (`go` closure, `go` argument, Channel send, deferred task, thread entry, and exported callback) consumes the same verified capture plan. Legality is determined jointly by the value's **storage owner, provenance, mutability, and type representation**, never by the `var` / `const` keyword alone:

| Capture action | Values | Boundary behavior |
|---|---|---|
| inline value | scalars and small immutable values passed as explicit `go` arguments | copy the bits directly |
| deep copy | execution-local graph under explicit `copy(x)` | materialize an independent graph in the destination storage domain |
| move | inferred-unique local `var` under explicit `move x` | transfer ownership and statically invalidate the source binding |
| module readonly | sealed and published module values | retain the module-readonly owner; do not copy into a root/task heap |
| synchronized ref | audited stable identities such as Channel, Task, and Atomic | retain a reference in the verified synchronized domain |
| reject | execution-local graphs, mutable module state, dangling slices/pointers/upvalues | compile error reporting the owner/provenance and required explicit action |

Consequently, a local `const` containing only inline values may cross directly. A managed/aggregate const graph may cross only when StoragePlan proves direct construction or O(1) publication seal; the boundary never copies implicitly. A module-level `const` becomes module-readonly only after seal-and-publish. A module-level `var` is module-mutable and is not made thread-safe merely by being globally visible.

`go` closures additionally follow one simple, stronger surface rule: **they may not capture any outer `var`, whether they only read it or mutate it, and whether the current program launches one coroutine or many.** Pass data as explicit `go` arguments and state `copy(...)`, `move`, or an audited synchronization capability at the boundary. Mutable state shared by multiple coroutines must flow through `Channel`, `Atomic`, or audited `sync` locks/handles. Directly capturing and modifying an ordinary `var` is a compile error, and `unsafe` does not relax this rule.

```xray
var local = 0
go fn() { local += 1 }()                 // ❌ compile error: cannot capture mutable local
```

#### Recommended patterns

```xray
// Pattern 1: explicitly copy an execution-local graph
var arr = [1, 2, 3]
var t = go fn(data: Array<int>) -> int {
    data.push(4)            // mutates the copy, original is unaffected
    return len(data)
}(copy(arr))
print(arr)                  // [1, 2, 3] unchanged

// Pattern 2: const, zero-copy read-only (capturable)
const config = { rate: 100 }
var t2 = go fn(c: Json) -> int {
    return c.rate
}(config)

// Pattern 3: move ownership
var big = Array<byte>(1024)
var t3 = go fn(b: Array<byte>) -> int {
    return process(b)
}(move big)
// big is inaccessible from this point

// Pattern 4: Channel communication (capturable)
const ch = Channel<int>(10)
var t4 = go fn(c: Channel<int>) -> int {
    return match (c.recv()) {
        Recv.Value(v) -> v
        _ -> 0
    }
}(ch)
ch.send(42)
```

### 7.5 Memory Management and Object Lifetimes

Xray uses a layered memory management strategy:

| Storage | Mechanism | Reclamation |
|--|--|--|
| Module-readonly storage (top-level `const`) | consteval rodata, or module allocator followed by seal + publish | at module unload |
| Module-mutable storage (top-level `var`) | module owner; not concurrency-safe by default | at module unload |
| Const/synchronized shared domain | verified const root or synchronized handle with root-only atomic reference counting | when the last cross-execution strong reference is released |
| Execution-local reclamation domain (ordinary local objects) | compiler-inserted reference counting plus a VM coroutine heap or AOT execution arena | immediately when the last strong reference is released; a reference cycle is not collected and the residual graph is disposed in bulk when the physical coroutine ends (§16.8) |
| Stack (`struct` values, locals) | lexical storage duration | scope exit; the language exposes no deterministic destructor / `Drop` hook |
| Arena (low-level temporary allocations) | bulk free | at arena end |

**Memory observation points**:
- The compiler inserts retain/drop operations for ordinary local objects; releasing the last strong reference enters the RC destruction path.
- The compiler marks only types that may form reference cycles as cycle candidates; the mark serves diagnostics and drives no runtime reclamation.
- Reference cycles are not collected at runtime: they are prevented statically (L0), broken explicitly with `weak` (L1), and bounded by bulk disposal of the execution-local reclamation domain (L2).
- No cycle-reclaiming mechanism exists at runtime — neither a concurrent tracing GC nor a cycle collector. A development build can enable a cycle detector, which only observes and reports; it never mutates the heap.

---

## 8. Error Handling

> Source of truth: `src/frontend/analyzer/xanalyzer_errorset.c`, `src/ir/xi_lower_stmt.c`, `src/vm/xvm_dispatch_exception.inc.c`, `src/runtime/object/xpanic_info.c`, and `stdlib/types/panic_info.xr`.

### 8.0 Design philosophy: value-return + panic boundary

Xray's error handling is split into two strictly separated channels:

| Channel | Syntax | Use case | Runtime cost |
|--|--|--|--|
| **Value-return channel** (`throw <enum>` / `try` / `catch`) | Business errors, recoverable failures | **Low overhead** (no `PanicInfo` allocation and no unwind; only a predictable branch at call boundaries that may propagate or catch errors) |
| **Panic channel** (`catch panic`) | Runtime faults (OOB, division by zero, non-exhaustive match) | Limited stack unwinding |

Design principles:

- **Errors are values**: `throw <enum>` writes an enum value into the return channel — no stack unwinding, no PanicInfo allocation.
- **Panics are not errors**: a panic signals a program bug or runtime invariant violation, not business logic.
- **No `throws` in function signatures**: xray does not adopt Java/Swift-style checked exceptions. Errors are handled via the throw/catch value-return channel.
- **Error sets are not part of function types**: concrete error enum/variant sets remain in the analyzer effect database. A function type carries only the internal three-state throw-effect bit (`UNKNOWN` / `MAY_THROW` / `NO_THROW`) used by safety constraints and constructive code generation.
- **No-throw is always inferred**: use an `xray verify` contract to freeze a no-throw guarantee; unknown or incomplete evidence is treated as may-throw.
- **`defer` replaces `finally`**: xray has no `finally` keyword; resource cleanup uses **block-scoped** `defer` (bound to the nearest real `{}` block, see §4.9 / §8.3).
- **A cleanup edge is not an error-propagation edge**: no error may escape a `defer` body. The constraint is enforced at compile time (`E0387`), see §8.3.1.

### 8.1 Value-return error channel

#### 8.1.1 `throw` statement

`throw expr` raises an enum error value. `expr` must be a variant of an enum type:

```xray
enum AppErr { NotFound, InvalidInput(string) }

throw AppErr.NotFound                       // ✅ simple enum variant
throw AppErr.InvalidInput("bad format")     // ✅ ADT enum variant with payload
```

After a throw:

```
throw point → write to pending_error → return up the call stack → run defer on the way → catch handles → otherwise keep returning → top-level diagnostic
```

- No stack frame unwinding (unlike traditional exception unwinding)
- No object allocation and no stack unwinding on the happy path; call boundaries that may propagate or catch errors go through only a predictable error-flag branch
- Unhandled top-level errors print `[Uncaught Error] <enum value>`, exit code = 1

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

**Execution order**:

1. Run the `try` block.
2. If a `throw` escapes, try each `catch` clause in declaration order; the first match runs its body.
3. If no `catch` matches, the error keeps propagating up the call stack.

**Typed catch and multiple catch clauses**:

A `catch` variable may be typed `catch (e: T)`; the runtime uses `is T` to test the enum type. Multiple `catch` clauses are matched in declaration order:

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

**Rules**:
- An untyped `catch (e)` is the catch-all and matches any error value.
- A typed `catch (e: SomeErr)` matches only when the error value satisfies `is SomeErr`, where `SomeErr` is a concrete enum error type.
- Multiple `catch` clauses are tried in declaration order; the first match wins.
- If every typed clause fails and there is no catch-all, the error continues propagating.
- A `try` **must** be followed by at least one of `catch` or `catch panic`.

#### 8.1.3 Rethrowing and error conversion

A `catch` block may rethrow the original error or throw a different enum variant:

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

#### 8.1.4 Recommended enum error design

Define business errors as ADT enums with context-carrying payloads:

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

ADT enums allow `match` to check exhaustiveness at compile time.

#### 8.1.5 Errors and coroutine boundaries

Value-return errors **do not propagate across coroutines automatically**. An uncaught error in a child coroutine:

- Terminates the child, error recorded in `coro.error`
- The parent is **not** notified automatically

Ways to pass child coroutine errors:

1. **Explicit Channel**:

```xray
enum WorkerErr { Failed(string) }
const err_ch = Channel<string>(1)

go fn() {
    try {
        riskyWork()
        err_ch.send("ok")
    } catch (e) {
        err_ch.send("error")
    }
}()

var result = match (err_ch.recv()) {
    Recv.Value(v) -> v
    _ -> "error"
}
if (result != "ok") { log("worker failed") }
```

2. **`linked go call()`** (see §10.2): failure of a standalone child Task cancels its parent Task and linked subtree; the returned Task still exposes the precise error to its caller.

3. **Structured concurrency `linked scope`** (recommended, see §10.5): an error from any child in the scope propagates to the parent scope automatically, routed through the correct channel (value-return enum errors via `catch`, panics via `catch panic`).

### 8.2 Panic channel

#### 8.2.1 What is a panic

A panic represents a **program bug or runtime invariant violation**, not business logic:

- Array out-of-bounds access
- Integer division by zero
- Non-exhaustive match
- Null reference dereference
- Runtime type assertion failure

Panics propagate via limited stack unwinding and generate `PanicInfo` objects with stack traces.

#### 8.2.2 `catch panic`

`catch panic` catches runtime faults from the panic channel:

```xray
try {
    var arr: Array<int> = [1, 2, 3]
    var v = arr[10]                          // OOB → panic
} catch panic {
    log("runtime fault caught")
}

try {
    var n = 10 / 0                           // division by zero → panic
} catch panic {
    log("division by zero caught")
}
```

**`catch` and `catch panic` can coexist**, handling both channels:

```xray
enum AppErr { InvalidArg }

try {
    process(input)
} catch (e: AppErr) {
    log("business error:", e)                // value-return channel
} catch panic {
    log("runtime fault!")                    // panic channel
}
```

#### 8.2.3 The `PanicInfo` class

`PanicInfo` is now **used only by the panic channel**. The VM constructs `PanicInfo` objects automatically on runtime faults:

```xray
class PanicInfo {
    message: string             // human-readable message (e.g. "index out of bounds")
    stack: Array<string>        // automatically captured call stack
    cause: PanicInfo?           // chained cause
    code: int                   // error code
    data: Json                  // additional data; JSON null when absent

    constructor(message: string = "", cause: PanicInfo? = null)
    fn toString() -> string
}
```

User code generally does not construct `PanicInfo` directly — use `throw <enum>` for business errors.

**Reading panic details in `catch panic`**: `catch panic (p)` binds the `PanicInfo` object to `p`, so you can read `message`, `code`, `stack`, and the other fields:

```xray
fn main() {
    var arr: Array<int> = [1, 2, 3]
    try {
        print(arr[10])                       // out of bounds → panic
    } catch panic (p) {
        print("message:", p.message)         // array index out of range: 10 (length 3)
        print("code:", p.code)               // 430
    }
}

main()
```

#### 8.2.4 Top-level diagnostic for an uncaught panic

A panic that reaches the top level uncaught by any `catch panic` prints one line
to stderr and exits with code 1:

```
[Uncaught Panic] E<code>: <message>
```

For example, division by zero prints `[Uncaught Panic] E0420: division by zero`,
and an out-of-bounds write prints
`[Uncaught Panic] E0430: array index out of range: 9 (length 3)`.

**Backend contract**: the VM and AOT (`xray build --native`) produce
**byte-identical** observable results for the same fault — exit code, stdout, and
this stderr line (both the code and the message come from the same shared
formatters). The contract covers exactly that surface.

**Presentation details, outside the contract**:

- The **stack trace** is an opt-in diagnostic, off by default. With
  `XRAY_BACKTRACE=1` the VM appends a `Stack trace:` frame list after the report
  line; the AOT native path carries no unwind state and appends nothing. Keeping
  the trace off by default is exactly what lets the two backends' default output
  agree.
- **Colour** follows whether stderr is a TTY: the `[Uncaught Panic]` tag is bold
  red on an interactive terminal and plain text when piped or redirected.

> Compare the value-return channel (§8.1.1): an uncaught top-level error prints
> `[Uncaught Error] <enum value>` and likewise exits 1. The two channels share
> the `[Uncaught Error]` / `[Uncaught Panic]` prefix style.

### 8.3 `defer` — resource cleanup

`defer` is a block-scoped cleanup statement guaranteed to run when the owning block exits (whether by fallthrough, `break` / `continue`, `return`, `throw`, or panic). Syntax: see §4.9.

The call form snapshots its receiver and arguments at registration; the block form reads captured bindings by reference at exit. A movable owner held by either form may not be moved or returned before the block exits (`E0382`). This owner-lifetime rule is independent of the error-channel rules below.

```xray
fn fetch(url: string) -> string {
    var conn = open(url)
    defer conn.close()                       // conn is guaranteed to close

    var data = conn.read()
    if (len(data) == 0) {
        throw FetchErr.Empty                 // defer still runs
    }
    return data
}
```

**Rules**:
- `defer` belongs to the nearest real `{}` block; a top-level function-body `defer` still runs when the function exits
- Multiple `defer`s in the same block run in **LIFO** order
- `defer` executes on block fallthrough, `break`, `continue`, `return`, `throw`, and panic unwinding
- A `defer` in a loop body runs as each iteration exits
- No error may escape a `defer` body (see §8.3.1)

#### 8.3.1 `defer` and errors

`defer` is a **resource-cleanup edge**, not an error-propagation edge. A failure on the cleanup path means the resource state is no longer known, so the language neither lets a cleanup error overwrite an in-flight error (the Go model) nor silently swallows it.

**Rule D1 (static, normative)**: if the inferred error set of the callable a `defer` defers is **non-empty**, the compiler reports `E0387`.

Unlike the throw-effect bit in §8.0, D1 is **not** fail-closed: xray has no user-writable no-throw annotation, so rejecting everything that cannot be proven non-throwing would leave an author facing an indirect call, a higher-order builtin, or a native member whose contract is not yet written with no way to discharge the obligation. What cannot be proven is left to rule D3's runtime backstop — that is what the layering is for. Should a user-writable no-throw annotation ever be added, D1 can tighten to "must be proven" and D3 becomes unreachable by construction.

```xray
fn close(c: Conn) { throw IoErr.Closed }

fn bad(c: Conn) {
    defer close(c)                           // ❌ E0387: the deferred target throws
}

fn good(c: Conn) {
    defer {                                  // ✅ handle it inside the defer body
        try { close(c) } catch (e) { log.warn("close failed") }
    }
}
```

**Rule D2 (how to satisfy it)**: absorb the error inside the `defer` body with `try` / `catch`, or call a cleanup API that does not throw. This is the only legal form — it forces the author to answer "what happens when cleanup fails?".

**Rule D3 (runtime backstop, normative)**: if an error still escapes a `defer` body — an indirect call D1 could not decide statically, or a panic raised inside the body — the runtime **terminates the process** with `E0443` and exit status `70`:

- The termination is **not catchable** — neither `catch` nor `catch panic` intercepts it. An error or panic the `defer` body catches **itself** has not escaped; that is the ordinary form rule D2 prescribes.
- The in-flight error is neither replaced nor suppressed; the diagnostic reports the escaping error, and the in-flight one alongside it when there is one (an enum error value carries no message, and shows as `<no message>`).
- The VM and AOT backends must agree verbatim on this behaviour, including exit code and diagnostic text.

**Why not Go's "replace" semantics**: in Go, rewriting the error from a defer requires an explicit assignment to a named return value — visible and local. xray puts `throws` in neither the signature nor the call site (§8.0), so an implicit error replacement would be entirely invisible in the source, and multiple simultaneously-throwing `defer`s would additionally need a replacement-chain rule. Fail-fast is the only option that is defined, hides nothing, and adds no further rules.

### 8.4 Optional and error handling

`T?` is sugar for `T | null` and fits the binary "value or absent" case. See §2.5. Relation to error handling:

- **Failure with no cause**: `T?` (e.g. map lookup returns `null` for "key not found").
- Pairs with `??` (default value) / `?.` (optional chain) / `e!` (force unwrap).
- Do not use `T?` as a generic error return — if a cause is needed, use `throw <enum>` + `catch`.

### 8.5 Decision tree: which mechanism to choose

Choose by "**how the caller has to handle the failure**":

```
Does the caller need to handle the failure?
│
├─ No (fatal / unrecoverable / program bug)
│   ↓
│   panic (triggered automatically by the runtime; catch panic for outer fallback)
│
├─ Yes, with structured causes
│   ↓
│   throw <enum>, catch to handle (low-overhead value-return channel)
│
├─ Yes, but the failure simply means "no value"
│   ↓
│   T? + ?? / ?.
│
├─ Yes, and the function has ≥3 normal states
│   ↓
│   User ADT enum directly as the return type
│
└─ Yes, returning multiple co-equal values (not success/failure)
    ↓
    tuple (a, b, ...)
```

Reference table:

| Case | Recommended | Example |
|--|--|--|
| Business errors, recoverable failures | `throw <enum>` + `catch` | `throw ParseErr.Empty` |
| Map lookup, optional fields | `T?` | `map.get(k) -> Value?` |
| Runtime fault fallback | `catch panic` | Array OOB, division by zero |
| Multi-branch result | enum | `nextEvent() -> NetEvent` |
| Primary result + metadata | tuple | `parse(s) -> (Ast, int)` |

### 8.6 Common patterns

#### Pattern 1: enum errors + defer for resource cleanup

```xray
enum ConnErr { Refused, Timeout }

// A tiny stand-in "connection" so the example runs on its own.
class Conn {
    alive: bool
    constructor(alive: bool) { this.alive = alive }
    isAlive() -> bool { return this.alive }
    close() { print("closed") }
}

fn fetchData(alive: bool) -> string {
    var conn = Conn(alive)
    defer conn.close()                 // runs whether we succeed or throw
    if (!conn.isAlive()) { throw ConnErr.Timeout }
    return "payload"
}

fn main() {
    try {
        print(fetchData(true))         // => closed, then payload
    } catch (e: ConnErr) {
        print("connection error")
    }
}

main()
```

#### Pattern 2: throw + catch for library APIs

```xray
enum ConfigErr { Missing(string) }

fn requirePort(cfg: Json) {
    if (!Json.containsKey(cfg, "port")) { throw ConfigErr.Missing("port") }
    print("port:", Json.get(cfg, "port"))
}

fn main() {
    try {
        requirePort(Json.parse("{\"port\": 8080}"))   // => port: 8080
        requirePort(Json.parse("{}"))                  // throws ConfigErr.Missing
    } catch (e: ConfigErr) {
        match (e) {
            ConfigErr.Missing(f) -> print("missing field:", f),   // => missing field: port
        }
    }
}

main()
```

#### Pattern 3: `??` for default values

```xray
var port = config?.port ?? 8080
var user = db.findUser(id) ?? guestUser
```

#### Pattern 4: catch panic for runtime fault fallback

```xray
fn safeDivide(a: int, b: int) -> string {
    try {
        return string(a / b)
    } catch panic {
        return "error: division by zero"
    }
}
```

### 8.7 Worked Examples

These are self-contained programs that run as-is and pass `xray check` (comments show the real output).

#### Example 1: `throw` / `catch` / `match`

```xray
enum ParseErr { Empty, BadChar(string) }

fn parseDigit(s: string) -> int {
    if (len(s) == 0) { throw ParseErr.Empty }
    if (s == "x") { throw ParseErr.BadChar(s) }
    return 42
}

fn main() {
    try {
        print(parseDigit(""))
    } catch (e: ParseErr) {
        match (e) {
            ParseErr.Empty -> print("empty input"),        // => empty input
            ParseErr.BadChar(c) -> print("bad char:", c),
        }
    }
}

main()
```

#### Example 2: `defer` order (LIFO) on the error path

```xray
enum E { Boom }

fn work() {
    defer print("defer 1")
    defer print("defer 2")
    print("body")
    throw E.Boom                             // defers still run when throwing
}

fn main() {
    try { work() } catch (e) { print("caught") }
}

main()
```

Output (`defer` runs in LIFO order):

```
body
defer 2
defer 1
caught
```

#### Example 3: `catch panic` with fault details

```xray
fn main() {
    var arr: Array<int> = [1, 2, 3]
    try {
        print(arr[10])
    } catch panic (p) {
        print("message:", p.message)         // => message: array index out of range: 10 (length 3)
        print("code:", p.code)               // => code: 430
    }
}

main()
```

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

```xray
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

```xray
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
| `Iterable<T>` | usable through the iterator protocol in `for-in`; Array, Slice, Map, Set, string, Json, Range, the `Iterator<T>` a generator returns, and types with a custom `iterator()` satisfy this constraint. `Channel<T>` is receivable with `for-in` but drives a dedicated receive loop instead of the iterator protocol, so it does not satisfy it. Unit-only `for (value in E)` and concrete `E.variants` are compile-time finite-domain forms; they do not make an enum satisfy `Iterable<T>` and cannot stand in for a generic `Iterable<T>` constraint |

`Hashable` is a static contract: when a concrete class / struct / enum is used as a `Map<K, V>` key, a `Set<T>` element, or declares `implements Hashable`, the compiler must see a non-`static`, non-`private` `operator==` and `hash() -> int`. The parameter type of `operator==` must be **spelled as the name of the declaring type itself** — Xray has no `Self` type, and writing `Self` produces diagnostic `E0365`:

```xray
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

```xray
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

```xray
identity(42)                    // T inferred as int
Box("hello")                // T inferred as string
Pair("key", 100)            // K=string, V=int
```

The inference algorithm is **bidirectional**:
- From arguments (call-site argument types → type parameters).
- From the return type (contextual expected type → type parameters).

#### Explicit instantiation

When inference fails or precision is needed:

```xray
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

```xray
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

```xray
class Container<T> {
    items: Array<T>
}
var c = Container<int>()
print(c is Container<int>)     // true
print(typeName(c))             // "Container<int>" when type names are enabled
```

Structured field/method metadata is not provided automatically by the default runtime; use explicit derive or compile-time generation for inspect/serialization use cases.

---

## 10. Concurrency and Coroutines

> Source of truth: `src/coro/xcoro*.c`, `src/coro/xtask*.c`, `src/coro/xchannel.c`, `src/coro/xscope*.c`, `src/frontend/analyzer/xanalyzer_escape.c`, and `docs/rules/design-principles.md`.

xray's concurrency model is **goroutine-style coroutines + channels + strong static guarantees**. The design makes `go worker(args)` use the same explicit argument and ownership boundary as an ordinary call, while the **compiler guarantees no data race** within the language's safe subset. The precise form of that guarantee, its boundary, and the synchronisation edges each construct in this section establishes are defined in §16.9.

### 10.1 Coroutine model

| Dimension | Choice |
|--|--|
| Scheduling model | M:N (user-space coroutines on multiple OS threads) |
| Scheduling policy | Cooperative (back edges, channels, `await`, `Coro.yield()`, and similar scheduling/suspension points) + work stealing |
| Stack model | Stackless (per-coroutine VM value stack + frame array, grows on demand, no native C stack) |
| Creation cost | ~microsecond (initial VM value stack ~64 slots + 4 frames, not a native stack) |
| Context switch | VM context switch (save/restore VM frames), no native stack switch, no syscall |

Coroutines are distributed across multiple worker threads by default; the runtime sets a Go-style `GOMAXPROCS` parallelism level based on the CPU core count.

### 10.2 `go` — start a coroutine

```ebnf
GoExpr   ::= 'linked'? 'go' GoOptions? CallExpr
GoOptions ::= '(' GoOption (',' GoOption)* ')'
GoOption ::= 'name' ':' StringLiteral
```

`go` is an **expression** returning a `Task<T>` handle. It accepts only a call expression; inline logic must be an immediately invoked lambda:

```xray
// Form 1: call an existing function
var t1 = go worker(0, channel)

// Inline logic: a lambda must still form a complete call with explicit arguments
var t2 = go fn(d: Json) -> int {
    return d.value * 2
}(payload)

// Parameterless inline logic is still a zero-argument lambda call; go { ... } does not exist
var t3 = go fn() -> int {
    return compute()
}()

// Optional debugging name
var named = go(name: "worker-1") worker(1, channel)
```

**`move` marks the source expression**: ownership transfer of an inferred-unique local root uses `move`, **not** a `go` option; a `const` capability is not a mutable-owner move source:

```xray
var data = { value: 10 }
var task = go fn(d: Json) -> int {
    return d.value + 1
}(move data)        // transfer data ownership to the coroutine; data is unusable afterwards
```

`go { ... }` is not syntax. The single call form guarantees that every entry passes through the same argument evaluation, capture plan, and cross-coroutine transfer plan. Use a lambda call for inline logic and spell `copy(...)` / `move` explicitly in argument positions:

```xray
var n = 10
var task = go fn(x: int) -> int {
    return x + 1
}(n)
```

**Semantics**:
- Every `go` expression returns a `Task<T>`, where `T` is the callee's return type; functions returning `()` correspond to `Task<null>`.
- Coroutines are scheduled on idle worker threads (M:N).
- `go(name: ...)` only sets the debugging name and does not affect scheduling order.
- Uncaught exceptions are stored in the `Task` and rethrown when `await` is called.
- Execution-local heap values (`Array` / `Map` / `Set` / `Json` / `Array<byte>` / `StringBuilder`, etc.) crossing a coroutine boundary must use explicit `copy(x)` or `move x`; **passing them bare is a compile error**. Scalars, `string`, published const values, and audited Channel / Task / Atomic handles pass directly. `move` requires a rebindable local `var` root proven unique with no live alias/loan. `go` arguments share the same transfer plan as `ch.send` and `select` send arms, so every boundary operation visibly states whether data is copied, moved, or capability-shared.
- `name` inside `go(name: ...)` is diagnostic/debug metadata only. The semantic modifier is the `linked go` prefix and is not a `GoOptions` entry.
- An ordinary outer `var` may never be captured by a `go` closure, for either reads or writes. This rule does not depend on coroutine count or scheduling. Mutable state shared across coroutines must flow through audited `Channel`, `Atomic`, or `sync` handles; direct captured mutation is a compile error.
- `linked go call()` still accepts only a call expression and returns an ordinary `Task<T>`. Outside a scope it attaches the child Task to the current parent Task: parent cancellation recursively cancels the child, child failure cancels the parent and its linked subtree, and a parent that has finished its own body still waits for linked children to terminate. Inside a scope, membership and failure propagation are owned solely by that scope's policy; no second parent-child relation is layered on top.

### 10.3 `await` — wait for a result

```ebnf
AwaitExpr ::= 'await' Expression
           |  'await' 'all' Expression       // wait for all to complete
           |  'await' 'any' Expression       // wait for any one to complete
```

```xray
// single task
var task = go fetch("https://example.com")
var result = await task                    // yields the current coroutine until task completes

// await all: wait for all, returns the result array (in input order)
var t1 = go compute(2)
var t2 = go compute(3)
var t3 = go compute(4)
var results: Array<int> = await all [t1, t2, t3]
// also works on a variable directly, no brackets needed
var tasks = [t1, t2, t3]
var results2: Array<int> = await all tasks

// await any: wait for the first to complete, return its result; the others keep running
var first = await any [t1, t2, t3]

// await anySuccess: skip failing tasks; wait for the first successful one
var firstOk = await anySuccess [t1, t2, t3]
```

**Semantics**:

- `await` only applies to `Task<T>`; other types are a compile error.
- Users always write `await task`, never `await (move task)`. If `T` is a unique mutable result, the compiler proves this await as the Task's one terminal take; a second await or later use of that single-owner task is a compile error. Const, synchronized-shared, and inline-copy results are observed according to their capability without a second await syntax.
- The current coroutine **yields** until the target completes (without blocking the OS thread).
- PanicInfo propagation:
  - `await t` re-raises `t`'s failure on the channel `t` raised it on. A `throw <enum>` inside the coroutine is re-raised as a value error, so the awaiting frame's `catch (e)` catches it and, with no catch, it propagates by value like any other throw. A panic inside the coroutine is re-raised as a panic and keeps unwinding.
  - `await t` therefore inherits the error set of the awaited coroutine's body, and a function containing such an `await` is fallible exactly when that body is. A task whose coroutine cannot be named at the await — read out of a collection, received as a parameter — is fail-closed: may-throw.
  - The same holds at a `linked scope`'s exit, which re-raises its first failed child in the enclosing frame on that child's channel.
  - The forms that report an outcome as a value — `awaitResult()`, `awaitTimeout(ms)` — never re-raise, and so never enter the enclosing function's error set.
  - On success, `await t` returns `T`; if `T` is nullable, a returned `null` is the task's real result, not a cancellation or failure marker.
  - `await all` throws if any task throws (the others are cancelled).
  - `await any` throws only when **every** task fails; if any one completes, its result is returned.
  - `await anySuccess` is similar to `await any` but **skips** throwing tasks, awaiting only the first successful one.
- `all` / `any` / `anySuccess` are **contextual keywords** after `await`; they apply only in this position.
- The input to `await all` must be homogeneous: every element must have the same static `Task<T>` type, and the result type is `Array<T>`. Heterogeneous tasks such as mixed `Task<int>` and `Task<string>` are not automatically erased or boxed; await them individually, or convert inside each task to a common enum / union / Json result type.

### 10.4 `Task<T>` handle

`go expr` returns `Task<T>`, where `T` is the callee's return type. Task handles support:

| Method / property | Type | Description |
|--|--|--|
| `t.done` | `bool` (property) | Whether the task has completed (success, failure, or cancellation) |
| `t.status` | `TaskStatus` (property) | `Pending` / `Running` / `Success` / `Failed` / `Cancelled` |
| `t.cancel()` | `() -> ()` | Request cooperative cancellation |
| `t.poll()` | `() -> TaskResult<T>` | Non-blocking observation; returns `TaskResult.Pending` while incomplete |
| `t.awaitResult()` | `() -> TaskResult<T>` | Waits and returns a status result without rethrowing |
| `t.awaitTimeout(ms)` | `(int) -> TaskResult<T>` | Waits until completion or timeout; timeout returns `TaskResult.Timeout` |

```xray
var t = go fetch(url)
if (!t.done) { /* still running */ }
var r = await t

match (t.poll()) {
    TaskResult.Pending -> print("running")
    TaskResult.Success(value) -> print(value)
    TaskResult.Failed(err) -> print(err)
    TaskResult.Cancelled -> print("cancelled")
    TaskResult.Timeout -> print("timeout")
}
```

The current public shape of `TaskResult<T>` is `Success(T)`, `Failed(PanicInfo)`, `Cancelled`, `Timeout`, and `Pending`. Runtime faults enter `Failed` as `PanicInfo`; business enum errors still use the language error channel, so plain `await task` propagates through the matching error path, and callers that need a status value keep an explicit task handle and call `awaitResult()` / `awaitTimeout(ms)`.

**Cancellation semantics**: `cancel()` sets the cancellation flag; the coroutine throws a cancellation exception at the next scheduling/suspension point (back edge, channel operation, `await`, `Coro.yield()`). Plain `await` on a cancelled task throws `TaskCancelled`; use `awaitResult()` or `awaitTimeout(ms)` when you want a status value.

**Watchdog policy**: the runtime monitor thread (sysmon) observes the heartbeat of RUNNING coroutines. Pure Xray loops advance the heartbeat at back-edge safepoints, so they are observed as making progress; sysmon is mainly for long native/FFI calls or no-safepoint regions that stop progressing. If a heartbeat stays frozen for too long, the default behavior is **warn-only**: a stuck warning is printed after roughly 100ms, but the coroutine is not silently cancelled. Forced cancellation is explicit opt-in: set `XRAY_SYSMON_CANCEL_MS=N` (`N > 0`, milliseconds) to mark a coroutine for cancellation after its heartbeat remains frozen past that threshold; unset or `0` keeps warn-only behavior. Long pure-CPU loops may insert `Coro.yield()` to improve scheduling fairness and cancellation responsiveness.

### 10.5 Channel

```ebnf
ChannelType ::= 'Channel' '<' Type '>'
ChannelNew  ::= 'Channel' ('<' Type '>')? '(' Expression ')'
```

Channels use stable `const` bindings. Their audited synchronization capability permits `send`/`recv` to mutate protected internal state:

```xray
const ch  = Channel<int>(10)    // buffered, capacity = 10
const ch0 = Channel<int>(0)     // unbuffered (synchronous handshake)
const cha = Channel(3)          // element type inferred from the first send
```

**API** (note that all method names are **camelCase**):

| Method | Signature | Behaviour |
|--|--|--|
| `send(v)` | `(T) -> ()` | Blocking send; waits for a consumer when full; throws if the channel is closed |
| `recv()` | `() -> Recv<T>` | Blocking receive; returns `Recv.Closed` when closed and drained |
| `recvOr(default)` | `(T) -> T` | Blocking receive; returns the payload directly, or `default` when closed and drained, without allocating a `Recv<T>` wrapper |
| `trySend(v)` | `(T) -> SendResult` | Non-blocking send; returns `Sent` / `Full` / `Closed` |
| `tryRecv()` | `() -> Recv<T>` | Non-blocking receive; returns `Recv.Empty` when empty |
| `sendTimeout(v, ms)` | `(T, int) -> SendResult` | Send with timeout; timeout returns `SendResult.Timeout` |
| `recvTimeout(ms)` | `(int) -> Recv<T>` | Receive with timeout; timeout returns `Recv.Timeout` |
| `close()` | `() -> ()` | Close the channel; idempotent |
| `isClosed` | `bool` (property) | Whether the channel is closed |

```xray
const ch = Channel<int>(10)
ch.send(42)                             // blocking send
var v = match (ch.recv()) {
    Recv.Value(value) -> value
    Recv.Closed -> -1
    _ -> -1
}

var sent = ch.trySend(99)               // SendResult.Sent / Full / Closed
match (ch.tryRecv()) {
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

**send/recv with `move`**: when sending a large object, use `ch.send(move payload)` to transfer ownership and avoid copying; the receiver becomes the sole owner.

In type position, a channel is written as `Channel<T>` and may be used in function parameters, fields, and return types:

```xray
fn producer(ch: Channel<int>) {
    ch.send(42)
}
```

**Semantics**:
- **MPMC** (multi-producer, multi-consumer).
- Buffered channel: senders suspend when full; receivers suspend when empty.
- Unbuffered channel: send and receive must rendezvous (synchronous handshake).
- After close: `send` throws; `recv` returns remaining buffered values as `Recv.Value(v)`, then `Recv.Closed`; `recvOr(default)` returns remaining buffered values, then `default`; `tryRecv` returns `Recv.Empty` when empty and not closed.
- `for (msg in ch)` is equivalent to blocking receive until the channel is closed and drained; the loop variable has type `T`. Channels do not support key-value iteration.

### 10.6 `select`

`select` multiplexes multiple channel operations. The non-blocking default branch uses `_`.

```ebnf
SelectStmt ::= 'select' '{' SelectArm+ '}'
SelectArm  ::= RecvArm | SendArm | TimeoutArm | DefaultArm
RecvArm    ::= Identifier 'from' Expression '->' Block
SendArm    ::= Expression 'to' Expression '->' Block
TimeoutArm ::= 'after' Expression '->' Block
DefaultArm ::= '_' '->' Block
```

```xray
const ch1 = Channel<int>(2)
const ch2 = Channel<int>(2)

select {
    msg from ch1 -> { print("got from ch1:", msg) }      // receive arm
    msg from ch2 -> { print("got from ch2:", msg) }      // receive arm
    100  to   ch1 -> { print("sent 100 to ch1") }        // send arm
    _ -> { print("no channel ready") }                   // default arm (non-blocking)
}
```

**Semantics**:
- Receive arm `name from ch -> body`: selected when ch has data, and binds the `Recv.Value(name)` payload to `name`.
- Send arm `value to ch -> body`: equivalent to `ch.send(value)`, but selected only when `ch` has capacity; `value` follows the same transfer plan as `ch.send` — an execution-local heap value must be written as explicit `copy(v)` or `move v`.
- Default arm `_ -> body`: runs immediately when no arm is ready; **omitting the default arm** makes `select` block until an arm becomes ready.
- When multiple arms are ready at the same time, one is selected **randomly** (matching Go).

### 10.7 `scope` (structured concurrency / lexical scope)

`scope` is a **statement keyword** that introduces a new lexical block. It serves two purposes:

1. **Pure lexical scope**: identical to a C/Rust `{ ... }` local block; `var` inside the block does not affect outer same-named variables.
2. **Structured concurrency** (semantic enhancement): coroutines started via `go` inside the block are **awaited automatically** before the block exits.

```ebnf
ScopeStmt       ::= 'scope' Block
LinkedScopeStmt ::= 'linked' 'scope' Block          // sibling failure -> cancel all + rethrow
```

```xray
// lexical scope use
var x = 1
scope {
    var x = 10            // shadow the outer x; in effect inside the block
    print(x)              // 10
}
print(x)                  // 1

// structured concurrency use (with go)
scope {
    go worker_a()
    go worker_b()
    // before the block exits, both a/b are awaited; an exception in either does not affect siblings
}
```

**Two scope variants**:

| Form | Behaviour when a child coroutine throws | Return value |
|---|---|---|
| `scope { ... }` | Siblings are not cancelled; exceptions do not propagate outward (each task is independent) | none (statement form) |
| `linked scope { ... }` | **Cancels all siblings** and **rethrows** the first exception outward | none |

Neither form returns an aggregate result. To observe a specific child status, keep an explicit task handle and call `awaitResult()` or `awaitTimeout(ms)`; to aggregate by result, use `await all` / `await any` / `await anySuccess`.

```xray
// linked scope: failure propagation
try {
    linked scope {
        go ok_worker()
        go failing_worker()         // throws
    }
} catch (e) {
    print("caught:", e)              // hits this branch
}

// scope: keep task handles and inspect each outcome after the block
var first: Task<int>?
var second: Task<int>?
var third: Task<int>?
scope {
    first = go failing("error1")
    second = go failing("error2")
    third = go ok()
}
var outcomes = [first!.awaitResult(), second!.awaitResult(), third!.awaitResult()]
print(len(outcomes))                 // 3 (one outcome per child)
```

**General semantics**:
- `scope` is not a function call and does not require an import; it is a keyword block statement.
- Both forms await every coroutine started by `go` inside the block before exiting.
- Both forms are statements only; neither may appear in an expression position.

### 10.8 `move` — cross-coroutine ownership transfer

```ebnf
MoveExpr ::= 'move' Identifier
```

`move` is a consuming source action (not a `go` option) accepted in initializers, assignments, returns, and call arguments.

**The decision procedure is §2.13**: `move` requires a rebindable local `var` root that satisfies all four evidence axes at once — binding live, root unique, capability mutable, no live loan. Syntactically it accepts an identifier only: `move x.field`, `move arr[i]`, and `move f()` are syntax errors. After `move`, the variable is statically marked **moved**, and any later reference is a compile error; a rejected move does not poison the source state. A `const` capability is not a mutable-owner move source.

A transfer across a coroutine boundary reads the same decision procedure, so the guarantee that coroutines do not share a mutable graph rests on the uniqueness evidence in §2.13, not on a separate concurrency-only rule.

```xray
var buf = Array<byte>(1024 * 1024)

// hand off to a coroutine
var t = go fn(b: Array<byte>) -> int {
    return process(b)
}(move buf)
// compile error: buf has been moved
// print(len(buf))

// hand off to a channel
const ch = Channel<Array<byte>>(1)
var payload = Array<byte>(4096)
ch.send(move payload)
// compile error: payload has been moved
```

See §7.3 and §7.4 for the coroutine transfer rules of `var` and `const` capabilities.

### 10.9 Synchronisation primitives

xray's default concurrency model favours **message passing + verified capability sharing + explicit ownership transfer**: `const`/synchronization capabilities, `Channel`, `move`, and `scope` make cross-coroutine data boundaries visible in source, so raw mutexes/locks are **discouraged**.

When mutual exclusion or atomic operations are unavoidable, the runtime provides:

| Primitive | Form | Description |
|---|---|---|
| Channel(1) | A single-element channel | The recommended mutex pattern (simulate lock/unlock via send/recv) |
| `const`/synchronized capability | Stable read-only value or synchronized identity | Ordinary graphs are deeply read-only; audited handles expose only capability-approved interior mutation |
| `Atomic<T>` | Lock-free atomic wrapper | C11 atomic operations for `int`/`float`/`bool` |
| `sync.Mutex<T>` / `sync.RwLock<T>` | Coroutine-domain locks | Require explicit `import sync`; wait by suspending a coroutine, not by blocking a worker; not allowed in `sys.Thread` bodies |
| `sys.OsMutex` / `sys.OsRwLock` / `sys.OsCondvar`, etc. | OS-thread-domain locks | Require explicit `import sys`; block the current OS thread, suitable for `sys.Thread`, runtime components, and short critical sections |

> **Design note**: xray does not expose bare `Mutex`/`RwLock` in the prelude. Prefer `Channel`, stable `const` synchronization handles, `move`, and `Atomic<T>` by default. When a lock is required, choose the execution domain explicitly through `sync` or `sys` so coroutine-suspending locks are not confused with OS-thread-blocking locks.

#### `Atomic<T>` — lock-free atomic type

`Atomic<T>` wraps `int`, `float`, or `bool`, allocated on the system heap, using C11 atomic instructions for lock-free cross-coroutine reads and writes.

**Declaration constraint**: name an `Atomic<T>` handle with `const`; its atomic methods come from an audited synchronized interior-mutation capability.

```xray
const counter = Atomic(0)         // Atomic<int>
const flag = Atomic(false)        // Atomic<bool>
const rate = Atomic(3.14)         // Atomic<float>
```

**Method overview** (full signatures in §14.19):

| Method | Description |
|---|---|
| `load(ord?)` | Atomic read |
| `store(val, ord?)` | Atomic write |
| `add(val, ord?)` / `sub(val, ord?)` | Atomic add/subtract (int/float) |
| `fetchAdd(val, ord?)` / `fetchSub(val, ord?)` | Atomic add/subtract returning old value |
| `swap(val, ord?)` | Atomic swap, returns old value |
| `compareExchange(expected, desired, ord?)` | CAS, returns `(old, bool)` |
| `toggle(ord?)` | Atomic negate (bool), returns old value |
| `toString()` | Returns string representation of current value |

#### `Ordering` enum

All methods accepting an `ord?` parameter take an `Ordering` enum to specify memory ordering. Default is `SeqCst` (strongest guarantee).

```xray
enum Ordering {
    Relaxed,          // No cross-thread ordering guarantee
    Acquire,          // Read barrier
    Release,          // Write barrier
    AcquireRelease,   // Read-write barrier
    SeqCst,           // Sequential consistency (default)
}
```

The `Ordering` enum is automatically injected by the compiler (prelude); no import is needed. Low-level intrinsics read the declaration-order tag and do not rely on user-visible backing values.

> `Ordering` describes the memory order of **one atomic operation**; that alone is not enough to derive program behaviour. How these values combine with the language-level synchronisation edges of channels, `go`, `await`, `scope` and `const` publication into happens-before is defined in §16.9, which also governs ordinary concurrent code that never mentions `Ordering`.

```xray
const counter = Atomic(0)
counter.store(42, Ordering.Release)
var val = counter.load(Ordering.Acquire)
```


### 10.10 `Coro.yield()` — yield the CPU

```ebnf
CoroYieldCall ::= 'Coro' '.' 'yield' '(' ')'
```

```xray
for (i in 0..1000) {
    do_chunk(i)
    Coro.yield()                // explicit safepoint, lets other coroutines run
}
```

`Coro.yield()` is a cooperative scheduling point, equivalent to an explicit safepoint where the scheduler can run other coroutines and observe cancellation. `yield expr` is reserved for generator value production; bare `yield` is rejected.

The two share a word root but are not the same suspension: `Coro.yield()` yields to the scheduler (can migrate OS threads, is a cancellation point, propagates to callers along call edges), while `yield expr` transfers symmetrically back to the driver (none of those hold). See §3.16.1 for the point-by-point comparison. **A generator body must not call `Coro.yield()`** (`E0385`, see §3.16.2).

### 10.11 Concurrency safety model

xray eliminates data races at compile time through the type system. The precise statement is the theorem in §16.9.5: **a program containing no `unsafe` block and using none of the escape hatches listed in §16.9.5 has no data race, and its behaviour is equivalent to some sequentially consistent interleaving (SC-DRF)**. Inside the safe subset you may therefore reason in terms of interleaved statements without needing the happens-before detail; the rules below are how that theorem is enforced. The escape-hatch list and the language-level synchronisation edges are defined in §16.9.

The compiler enforces:

| Rule | Enforced |
|--|--|
| Every cross-execution boundary consumes the same provenance-based capture plan | ✅ |
| An execution-local graph requires explicit `copy`, or `move` from a local `var` | ✅ |
| Module-readonly values may retain the module owner; module-mutable state may not cross directly | ✅ |
| Published `const` values and audited synchronization handles may cross through a verified plan | ✅ |
| `move` only applies to explicit ownership transfer of ordinary local `var` values | ✅ |
| Channels for cross-coroutine values | ✅ |
| `Atomic<T>` uses a stable `const` binding and only audited methods mutate internal state | ✅ |

**What compile-time enforcement does not cover**:
- Channels never copy a mutable class reference implicitly. If uniqueness transfer or const publication cannot be proven, compilation fails; use explicit `copy` or `move`.
- The escape hatches of §16.9.5 (`unsafe`, `Array.mutPtr()`, raw `mem.*` memory, `sys.Thread` bodies, `CFn` callback bodies) sit outside the theorem's guarantee; a data race through them has implementation-defined consequences.

#### 10.11.1 Outside the model: `unsafe` and C callbacks

The guarantees above cover ownership and capability decisions for Xray values. Three places sit **outside the model**, where the obligation moves to the author. `unsafe` relaxes none of the existing rules and supplies no substitute guarantee:

- **`mem.volatileLoad` / `volatileStore` / `fence` / `pageAlloc`** operate on raw memory: no ownership root, no reference count, no aliasing decision. The compiler assumes nothing about those addresses and produces none of the §2.13 evidence for them. Data-race freedom of any shared structure built from them is the author's proof obligation.
- **`Ptr<T>` / `MutPtr<T>`** are borrow-tracked (§2.4.2), but the tracking reaches only as far as the owner's scope. Address validity, alignment, and aliasing correctness at the dereference belong to the author of the `unsafe` block.
- **A `CFn` C callback may run on any OS thread**, including threads the runtime does not manage. A callback body may reach only: the scalars and raw pointers passed in, values whose allocation plan is shared (§16.3.1), and a unique root transferred in through a `Channel`. It must **not** reach the calling coroutine's coroutine-local object graph — those objects' reference counts are in the non-atomic band, and touching them from another thread is memory corruption. A callback body must also not assume a current coroutine, a current worker, or an available scheduler.

These three are the model's **boundary**, not holes in it: each has to be written explicitly in source (an `unsafe` block, a `mem` import, a `CFn` declaration), so which code runs outside compile-time protection is always auditable from the source.

### 10.12 Logical root task and reachable runtime capabilities

Program semantics expose one logical root task. Its physical representation is derived from the final artifact's reachable roots: a pure synchronous entry is **ELIDED**; an entry that only spawns children without suspending uses a **DESCRIPTOR**; an entry that suspends directly or transitively uses a **RESUMABLE_FRAME**. Ordinary non-suspendable functions keep the plain ABI, with no implicit coroutine context, frame, safepoint, or current-task lookup.

Runtime capabilities propagate only from final artifact roots such as the executable entry and manifest-selected C exports. An unreachable helper containing `go`, `await`, or Channel operations does not force the artifact to link a scheduler, timer, netpoll, or hosted runtime.

A hosted target selects a NONE / SINGLE / MULTI scheduler from the verified entry plan. A freestanding target remains coroutine-runtime-free when reachable code needs only core. If reachable code needs task/frame allocation, submit, park/wake, timer, interrupt completion, or an executor pump, the target manifest must provide a versioned provider ABI and the required hooks; missing capabilities fail before generation or linking. The provider is a target/build contract and introduces no `async main`, `static main`, or freestanding-only source-language keyword.

---

## 11. Modules

> Source of truth: `src/module/xmodule.c`, `src/module/xmodule_resolver.c`, `src/module/xmodule_graph.c`, `src/frontend/parser/xparse_import.c`.

### 11.1 Module Definition

- Each `.xr` file is one module.
- Module name = file name (with the `.xr` suffix removed).
- A file module's canonical identity comes from its resolver-normalized path. Source normally imports it through a relative or package path; code should not depend on a display-only dotted-directory derivation.

### 11.2 Project Layout

```
my_project/
├── xray.toml              # package manifest (name, dependencies, main)
├── src/
│   ├── main.xr            # entry
│   ├── utils.xr
│   └── lib/
│       └── helper.xr
├── tests/
│   └── test_utils.xr
└── docs/
```

`xray.toml` example:

```toml
[package]
name = "my_project"
version = "0.1.0"
main = "src/main.xr"

[dependencies]
local_utils = { path = "../local_utils" }
```

### 11.3 `import` Syntax

`import` declarations **must appear at the module top level**. Using `import` inside a function, class, or any nested scope is a compile error.

```ebnf
ImportStmt ::= 'import' ImportMembers 'from' ImportModule
            |  'import' ImportModule ('as' Identifier)?
ImportMembers ::= '{' ImportMember (',' ImportMember)* ','? '}'
ImportMember  ::= Identifier ('as' Identifier)?
ImportModule  ::= StringLiteral | ModuleName
ModuleName    ::= Identifier
```

```xray
// 1. stdlib: bare identifier; without `as`, the alias equals the module name
import time
import datetime
import http as httpClient

// 2. third-party packages: quoted owner/name form
import "alice/utils"
import "bob/http_client" as httpClient

// 3. file-path or directory-path: string literal, optional explicit alias, otherwise inferred from the trailing path segment (no .xr extension)
import "./modules/mod_a" as a
import "../utils/string_utils" as utils
import "models/user" as user

// 4. named imports: members may be renamed; the `from` operand may be a quoted path or a bare module name
import { readFile, writeFile as write } from io
import { publicFn } from "./modules/mod_a"
```

**Not supported**:
- JavaScript-style default import (`import name from "module"`). Use `import "module" as name`, `import module`, or `import { name } from module`.
- Dynamic import (`import("module")`). All imports must be static declarations.

**Resolution algorithm** (in priority order):
1. **stdlib name resolution**: a bare identifier `import time` → the built-in stdlib module table.
2. **Relative path**: `"./xxx"` and `"../xxx"` are resolved relative to the current file (auto-appends `.xr` extension or `index.xr` directory entry).
3. **Project-root path**: a quoted path that does not start with `./` or `../` is resolved as a project-relative directory import.
4. **Third-party packages**: `"owner/name"` is resolved through the `[dependencies]` section in `xray.toml`.

**Specifier validation** (compile errors):
- Including `.xr` extension: `import "./a.xr"` → error
- Trailing slash: `import "./a/"` → error
- Explicit `index`: `import "./a/index"` → error
- Absolute paths: `import "/etc/foo"` → error

### 11.4 `export` and Visibility

`export` visibility **must appear on a module-level declaration**. Cross-module re-exports remain declarations of their own:

```ebnf
Declaration ::= Attribute* Visibility? Modifier* DeclarationBody
Visibility  ::= 'export'
ReexportDecl ::= 'export' '{' ExportSpec (',' ExportSpec)* '}' 'from' StringLiteral
              | 'export' '*' 'from' StringLiteral
ExportSpec ::= Identifier ('as' Identifier)?
```

```xray
// 1. visibility belongs to the declaration
export fn helper() { return }
export final class MyClass {
    value: int
    constructor() { this.value = 1 }
}
export const VERSION = "1.0"

// 2. re-export (with optional renaming)
export { getUser, getUserAge as getAge } from "./user"

// 3. wildcard re-export (forward all exports of another module)
export * from "./product"
```

**Restrictions**:
- Declarations not marked `export` are **private** to the module.
- Post-hoc local `export { LocalName }` is removed; put `export` on the declaration.
- `export var` is not supported. Mutable bindings cannot be shared across modules; use `export const` instead.
- Internal state does not collide across modules even with the same name.
- Re-exports and wildcard re-exports are commonly used in `index.xr` to aggregate public APIs of submodules.

### 11.5 Naming Conventions

- Module names: `snake_case` (`http_client.xr` / `string_utils.xr`).
- Public symbols: `camelCase` or `PascalCase` (for classes / interfaces).
- Internal symbols: prefix with `_` (`_internal_helper`).

### 11.6 Compile-Time Module Graph and Circular Dependencies

Xray builds a complete **module dependency graph** (DAG) at compile time:

1. Starting from the entry file, all `import` declarations are recursively resolved to build the dependency graph.
2. The graph is topologically sorted to determine module initialization order.
3. If a **circular dependency** is detected (SCC size > 1 or self-loop), compilation fails with `E0504` and reports a concrete cycle path such as `a.xr -> b.xr -> a.xr`.
4. Modules are initialized in topological order, from leaf modules (no dependencies) to the entry module.

Selective imports (`import { foo } from "./m"`) are resolved at compile time to fixed module indices and export slots, resulting in O(1) indexed access at runtime with no string lookup.

Xray does not use partial module initialization or cache-based cycle breaking. A module that is still loading is not observable through `import`; mutual dependencies must be refactored into a DAG.

### 11.7 Native Modules

Modules exposed from the C layer (`time`, `http`, `os`, etc.) are registered through the native ABI:

```c
// C side
XRAY_API void register_time_module(xray_vm_t* vm) {
    xray_module_t* m = xray_module_create(vm, "time");
    xray_module_add_fn(m, "now", time_now);
    xray_module_add_fn(m, "sleep", time_sleep);
    xray_module_register(vm, m);
}
```

Usage from Xray code is identical:

```xray
import time
var t = time.now()
time.sleep(100)
```
### 11.8 Worked Examples

Selective import and namespace import from the standard library:

```xray
import { sha256 } from crypto
import math

fn main() {
    print(math.sqrt(144.0))   // => 12.0
    print(sha256("xray"))     // => 1a46e6a6... (SHA-256 digest)
}

main()
```

---

## 12. Testing

> Source of truth: `src/app/cli/xcmd_test.c`, `src/api/xtest_runner.c`, `src/frontend/parser/xparse_decl.c`, and the analyzer's global assertion-builtin table.

### 12.1 Declaring Tests: the `@test` Attribute

Xray marks test functions with the **`@test` attribute**, **not** with a `test("...")` function call.

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

**Semantics**:
- Functions annotated with `@test` are auto-discovered and run by `xray test`; ordinary functions are not.
- Test naming convention: `test_xxx` (snake_case), descriptive.
- Test functions take no parameters and return nothing; expectations are expressed via the `assert*` family.
- A file may contain **any number** of `@test` functions; they run in declaration order within that file. Multiple files may run in parallel with `-j N`, each in its own isolate.

### 12.2 Test Entry Points

`xray test` enforces no directory or filename convention. A directory argument is scanned recursively for every `.xr` file, sorted by path. This repository's `tests/regression/XXXX_topic.xr` form is only a project convention.

Run:

```bash
xray test tests/                           # at least one file or directory is required
xray test tests/regression/01_literals/    # run a whole group
xray test tests/regression/01_literals/0100_int_basic.xr   # single file
xray test -j 4 tests/                      # file-level parallelism
```

### 12.3 Assertion API

Xray provides assertion functions as **global builtins** (no `import test` needed). Full signatures are in [§13.5](#135-assertions-for-testing).

| Function | Semantics |
|--|--|
| `assert(cond, msg?)` | throws when `cond` is false |
| `assert_eq(a, b)` | prints both values when `a == b` fails |
| `assert_ne(a, b)` | `a != b` |
| `assert_true(cond)` / `assert_false(cond)` | equivalent to `assert(cond)` / `assert(!cond)` |
| `assert_throws(fn)` | expects `fn()` to throw |

> **Naming consistency**: all assertion functions are `snake_case` (`assert_eq`, not `assertEq`).

### 12.4 Async Tests

A `@test` function body may use `go` / `await` / `await all` / `await any`:

```xray
@test
fn test_async_fetch() {
    var task = go fetch_data("http://...")
    var result = await task
    assert_eq(result.status, 200)
}
```

### 12.5 Attribute Overview

Public Xray attributes come from one registry, exposed by `xray language attributes`. The test runner recognizes:

| Attribute | Description |
|---|---|
| `@test` / `@test(skip)` / `@test(timeout: N)` | test, skipped test, or per-test timeout in seconds |
| `@before_all` / `@after_all` | run once before/after the file's suite |
| `@before_each` / `@after_each` | run before/after every non-skipped test |

Other public attributes include `@deprecated("...")`, `@derive(Inspect, Json, Eq, Hash, Clone)` (`Hash` requires `Eq`), plus the stable, low-level AOT code-shape directives `@inline` / `@noinline`. The public surface is seven ordinary/test/metadata attributes plus two code-shape directives; the count is not a compatibility goal, while category and semantic orthogonality are constraints. The latter two apply only to functions or methods, take no arguments, and cannot annotate the same declaration together. They do not change language semantics, effects, or ABI: the VM ignores them, while AOT respectively prefers expansion or preservation of a native call boundary. They are only for low-level hot paths backed by real benchmarks and generated-code shape gates; they neither unlock ordinary optimization nor guarantee that every call edge is eligible.

`codegen.opaque(value)` and `codegen.compilerFence()` belong to the same expert-control layer but are standard-library intrinsics, not attributes. The former blocks native constant propagation while preserving an integer or pointer's exact static type, value, ownership, and provenance. The latter only prevents memory-effecting operations from moving across that compiler scheduling point. `compilerFence` is not a CPU memory fence, establishes no happens-before relation, and cannot repair a data race. Ordinary code does not need these controls; hard shape requirements are checked by `xray verify --contract`, not self-certified by the source request.

Effects, native identity, C exports, link symbols, and freestanding entries are inferred facts or typed manifest plans, not source attributes. External C functions use an `extern "C" ... {}` declaration block.

```xray
import codegen

@test                                 // mark as a test
fn test_basic() { return }

@test(skip)                           // skip this test
fn test_wip() { return }

@before_each
fn reset_fixture() { resetState() }

@deprecated("use newAPI() instead")
fn oldAPI() { return }

@inline
fn smallHotHelper(value: u64) -> u64 { return value ^ (value >> 33) }

@noinline
fn measuredDispatchBoundary(value: u64) -> u64 { return smallHotHelper(value) }

fn measuredKernel(value: u64) -> u64 {
    var hidden = codegen.opaque(value)
    codegen.compilerFence()
    return measuredDispatchBoundary(hidden)
}
```

> `@async`, `@override`, and camel-case spellings such as `@beforeEach` are not in the current table and trigger `unknown attribute name`. Use `go` / `await` directly in an async test body.

### 12.6 `xray run` / `xray test` / `xray repl`

| Command | Purpose |
|--|--|
| `xray run main.xr` | run the main program |
| `xray test` | run the test suite |
| `xray repl` | start the REPL |
| `xray build --native main.xr` | AOT native compile |
| `xray fmt` | format code |

---

## 13. Built-in Functions

> Source of truth: `src/ir/xi_lower_expr.c`, `src/vm/xvm_dispatch_*.inc.c`, `src/runtime/object/builtins/`, `src/frontend/analyzer/xanalyzer_builtins.c`.

These global functions and built-in constructor/static functions are usable without any `import`. In the tables below, `value` denotes "any runtime value"; it is a documentation placeholder rather than a writable source-language type.

### 13.1 I/O and Debugging

| Function | Signature | Description |
|--|--|--|
| `print` | `(...values) -> ()` | print to stdout, automatically appending a newline; multiple arguments are separated by spaces |
| `dump` | `(value, indent?) -> ()` | structured debug output |
| `len` | `(value) -> int` | length of strings, containers, Range, Slice, Json, and other `Lengthable` values; do not read `.length` |

### 13.2 Type Conversion

| Function | Signature | Description |
|--|--|--|
| `int(x)` | `(value) -> int` | convert to int; `rune` converts to its Unicode scalar code point; throws if string parsing fails |
| `float(x)` | `(value) -> float` | convert to float |
| `string(x)` | `(value) -> string` | convert to string; `rune` converts to a one-scalar string |
| `bool(x)` | `(value) -> bool` | convert to bool; rules in §2.3.3 |
| `rune(n)` | `(int) -> rune` | construct a Unicode scalar from an integer; surrogate and out-of-range values throw |
| `chr(n)` | `(int) -> string` | Unicode code point → one-scalar string |
| `copy(x)` | `(value) -> fresh value` | explicit deep copy; ordinary values preserve their type shape, while a borrowed `Slice<T>` / view returns an independent owner `Array<T>` |

### 13.3 Type Checking

| Function / expression | Signature | Description |
|---|---|---|
| `typeOf(x)` | `(value) -> Type` | returns a stable TypeId / `Type.xxx` value |
| `typeName(x)` | `(value) -> string` | returns the debug/logging type-name string |
| `typeName<T>()` | `() -> string` | returns the name of static type `T` |
| `x is T` | expression | runtime type check; the analyzer may narrow types |

The branch hints `likely(cond)` and `unlikely(cond)` accept and return the same `bool`; they inform optimization probability only and do not change evaluation or short-circuit semantics.

The global read-only environment values are not functions: `process` (entry arguments/file/directory), `__file__`, and `__dir__`. They are initialized for a real file/project entry; `process` may be `null` in a pure `eval` context.

```xray
var x = 42
print(typeOf(x) == Type.int)    // true
print(typeName(x))              // "int"
print(x is int)                 // true
// typeOf(x) == "int"           // compile error: use Type.int or typeName(x)
```

### 13.4 Coroutines

Coroutine launch and waiting are syntax, not global functions: `go`, `await`, `await all`, `await any`, `await anySuccess`. For sleeping, use `time.sleep(ms)`.

### 13.5 Assertions (for testing)

| Function | Signature | Description |
|---|---|---|
| `assert(cond, msg?)` | `(bool, string?) -> ()` | throws when `cond` is false |
| `assert_true(cond)` | `(bool) -> ()` | equivalent to `assert(cond)` |
| `assert_false(cond)` | `(bool) -> ()` | equivalent to `assert(!cond)` |
| `assert_eq(a, b)` | `(T, T) -> ()` | deep-equal assertion |
| `assert_ne(a, b)` | `(T, T) -> ()` | deep-not-equal assertion |
| `assert_throws(fn)` | `(fn) -> ()` | expects the function to throw |

### 13.6 Container Constructors and Static Functions

| Function | Description |
|--|--|
| `Array()` / `Array(n)` / `Array(n, value)` | create an empty array, an array of given length, or a value-filled array |
| `Array.from(iterable)` | create an array from a string / Array / Set / Map |
| `Array.range(start, end)` | inclusive integer array `[start, ..., end]` |
| `Array.withCapacity(n)` | array with `length=0` and `capacity=n` |
| `Map()` | empty Map |
| `Map.from(entries)` | Map from `[key, value]` pair array |
| `Map.from(keys, values)` | Map from key array and value array |
| `Set()` / `Set(array)` | empty Set or Set from an array |
| `Set.from(iterable)` | Set from a string / Array / Set |
| `Set.range(start, end)` | inclusive integer Set |

BigInt uses the `123n` literal or `int.toBigInt()`; Json uses `Json.parse` / `Json.encode` / `Json.stringify`; DateTime uses factory functions in the `datetime` module.

---

## 14. Built-in Type Methods

> Source of truth: prelude / analyzer / runtime built-in type registration and method definitions.
> MCP knowledge only consumes the generated analyzer metadata; it does not maintain its own copy of built-in method signatures.

This section summarizes the methods, signatures, and behavior of each built-in type by topic.

### 14.1 `int` Methods

| Method | Signature | Description |
|--|--|--|
| `abs()` | `() -> int` | absolute value |
| `toString()` | `() -> string` | decimal string |
| `toBigInt()` | `() -> BigInt` | convert to BigInt |
| `toFloat()` | `() -> float` | convert to float |
| `toHex()` | `() -> string` | hexadecimal string |
| `max(other)` / `min(other)` | `(int) -> int` | binary max/min |
| `sqrt()` | `() -> float` | square root |
| `pow(exp)` | `(float) -> float` | power |
| `checkedAdd(other)` / `checkedSub(other)` / `checkedMul(other)` | `(int) -> int?` | returns `null` on overflow |
| `saturatingAdd(other)` / `saturatingSub(other)` / `saturatingMul(other)` | `(int) -> int` | clamps overflow to the `int` boundary |
| `wrappingAdd(other)` / `wrappingSub(other)` / `wrappingMul(other)` | `(int) -> int` | explicit two's-complement wrap |
| `addOverflows(other)` / `subOverflows(other)` / `mulOverflows(other)` | `(int) -> bool` | reports signed overflow only (use `checked*` for the value) |
| `popcount()` | `() -> int` | number of set bits in the two's-complement representation |
| `leadingZeros()` / `trailingZeros()` | `() -> int` | leading/trailing zero bit count (`0` yields `64`) |
| `byteswap()` | `() -> int` | reverses the byte order |
| `rotateLeft(n)` / `rotateRight(n)` | `(int) -> int` | bit rotation (`n` taken modulo 64) |

`abs()` follows integer wrap semantics: `(-9223372036854775807 - 1).abs()` returns itself. `toHex()` keeps a sign prefix for negative values, for example `-0x8000000000000000`. Bit-manipulation methods and overflow predicates have the same semantics in VM and AOT builds.

### 14.2 `float` Methods

| Method | Signature | Description |
|--|--|--|
| `abs()` | `() -> float` | absolute value |
| `toString()` | `() -> string` | string conversion |
| `toFixed(decimals?)` | `(int?) -> string` | fixed-decimal string |
| `toInt()` | `() -> int` | convert to int |
| `floor()` / `ceil()` / `round()` | `() -> int` | rounding |
| `sqrt()` | `() -> float` | square root |
| `pow(exp)` | `(float) -> float` | power |
| `isNaN()` | `() -> bool` | whether the value is IEEE NaN |

### 14.3 `BigInt` Methods

| Method | Signature | Description |
|--|--|--|
| `abs()` | `() -> BigInt` | absolute value |
| `toString()` | `() -> string` | string conversion |
| `sign()` | `() -> int` | -1 / 0 / 1 |
| `isZero()` / `isNegative()` / `isPositive()` | `() -> bool` | sign predicates |
| `toInt()` | `() -> int?` | returns null when not representable as `int` |
| `toFloat()` | `() -> float` | convert to float |

### 14.4 `bool` Methods

| Method | Signature | Description |
|--|--|--|
| `toString()` | `() -> string` | returns `"true"` or `"false"` |

### 14.4.1 `rune` Methods

| Method | Signature | Description |
|--|--|--|
| `toString()` | `() -> string` | return a one-Unicode-scalar string |
| `toUInt32()` | `() -> u32` | return the Unicode scalar code point |
| `isLetter()` | `() -> bool` | whether the scalar is a Unicode letter |
| `isNumber()` | `() -> bool` | whether the scalar is a Unicode number |
| `isAlphanumeric()` | `() -> bool` | whether the scalar is a letter or number |
| `isWhitespace()` | `() -> bool` | whether the scalar is whitespace |

`rune` is an independent primitive type and does not inherit integer methods; use `toUInt32()` explicitly when the code point is needed.

### 14.5 `string` Methods

| Member | Type / Description |
|--|--|
| `len(s)` | O(1) Unicode scalar count |
| `bytes()` / `copyBytes()` | borrowed `Slice<byte>` / independent `Array<byte>` |
| `runes()` | `Iterator<rune>`; bare `for (r in s)` has the same semantics |
| `string.fromRune(r)` | constructs a string from one Unicode scalar |
| `string.fromUtf8(bytes)` | copies and strictly validates a `Slice<byte>`; invalid UTF-8 throws `Utf8Error.InvalidUtf8` |
| `string.fromUtf8Lossy(bytes)` | copies a `Slice<byte>`, replacing invalid sequences with U+FFFD |
| `string.join(parts, separator?)` | joins an `Array<string>` |
| `contains(s)` | substring containment test |
| `indexOf(s, start?)` / `lastIndexOf(s)` | return rune ordinals |
| `slice(start, end?)` | independent rune-ordinal slice; the range must be valid |
| `sliceBytes(start, end)` | slice by byte offset; invalid boundaries throw `StringSliceError.InvalidByteRange` |
| `split(sep, limit?)` | split into `Array<string>` |
| `replace(from, to)` / `replaceAll(from, to)` | replacement |
| `repeat(n)` | repeat |
| `startsWith(s)` / `endsWith(s)` | prefix/suffix check |
| `toString()` | return self |

Strings do not support integer indexing or the slice operator; use `s.runes().nth(i)`, `s.bytes()[i]`, or `s.slice(start, end)` explicitly. Concatenation uses `+`; Unicode text transforms such as case conversion, trimming, padding, and reversal belong to the `text` module.

### 14.6 `Array<byte>`

`Array<byte>` is a directly available specialization of `Array`; construction is handled via builtin paths such as `Array<byte>(n)` / `Array<byte>(n, fill)`. Its `toString()` uses the same container formatting as every Array; decode text explicitly with `string.fromUtf8(bytes[:])` or `string.fromUtf8Lossy(bytes[:])`. There is currently no separate `stdlib/types/bytes.xr` declaration; tooling should not treat it as a second, Array-isomorphic API surface.

### 14.7 `Array<T>` Methods

| Member | Type / Description |
|--|--|
| `len(arr)` | global `int` query |
| `capacity` / `arr[i]` / `arr[i] = v` | capacity field and indexed read/write; `get(i)` / `set(i, v)` are also available |
| `push(x)` / `pop()` | tail insert/remove |
| `shift()` / `unshift(x)` | head insert/remove |
| `concat(...arrays)` | concatenation |
| `indexOf(x)` / `contains(x)` | search |
| `join(sep?)` | concatenate into a string |
| `reverse()` / `sort(cmp?)` | in-place reorder |
| `map(fn)` / `filter(fn)` / `reduce(fn, init)` | functional helpers |
| `forEach(fn)` / `find(fn)` / `findIndex(fn)` / `every(fn)` / `some(fn)` | traversal and predicates |
| `fill(v, start?, end?)` / `clear()` | fill or clear |
| `reserve(capacity)` / `resize(length, fill)` | capacity and length management |
| `ptr()` / `mutPtr()` | explicit low-level pointer views |
| `toString()` | container representation |
| `iterator()` / `entriesIterator()` / `entries()` | iteration protocol |

Array has no `slice()` / `splice()` / `flat()` / `copyWithin()` methods. `arr[start:end]` produces a borrowed `Slice<T>` whose target type must be explicit and whose lifetime follows the borrow rules in §2.4.2; use `copy(arr[start:end])` for independent data.

### 14.8 `Map<K, V>` Methods

| Member | Type / Description |
|--|--|
| `len(m)` | global `int` query |
| `m[k]` / `m[k] = v` | indexed read/write |
| `get(k)` / `set(k, v)` | `get` returns `null` when absent; `set` writes |
| `containsKey(k)` / `containsValue(v)` / `delete(k)` / `clear()` | query and remove |
| `keys()` / `values()` / `entries()` | keys, values, key/value pairs |
| `forEach(fn)` | traversal |
| `iterator()` / `entriesIterator()` | iteration protocol |

**Map literal**: `#{"k1": v1, "k2": v2}` or `#{}`; entries use `:`, distinguished from Record/Json object literals by the `#` prefix.

`m[k]` requires the key to exist; a missing key raises runtime error `E0431`. Use `m.get(k)` for optional lookup.

The key position of a subscript is typed and checked against `K`, symmetrically with the value position against `V`: `m[1]` on a `Map<float, V>` is the float key `1.0`, not an int key stored in a float map. Key matching uses the key equivalence relation from §9.2, not `==`.

### 14.9 `Set<T>` Methods

| Member | Type / Description |
|--|--|
| `len(set)` | global `int` query |
| `add(x)` / `contains(x)` / `delete(x)` | insert, query, remove |
| `clear()` | empty the set |
| `values()` | returns `Array<T>` |
| `forEach(fn)` | traversal |
| `iterator()` | iteration protocol |

**Set literal**: `#[1, 2, 3]` or `#[]`.

### 14.10 `Channel<T>` Methods

| Member | Type / Description |
|--|--|
| `send(v)` | blocking send; throws if the channel is closed |
| `recv()` | blocking receive, returns `Recv<T>`; closed and drained is `Recv.Closed` |
| `recvOr(default)` | receives a payload, or returns the supplied default when none is available |
| `trySend(v)` | non-blocking send, returns `SendResult` |
| `tryRecv()` | non-blocking receive, returns `Recv<T>`; empty is `Recv.Empty` |
| `sendTimeout(v, ms)` | timed send, returns `SendResult`; timeout is `SendResult.Timeout` |
| `recvTimeout(ms)` | timed receive, returns `Recv<T>`; timeout is `Recv.Timeout` |
| `close()` | close the channel |
| `capacity` / `isClosed` | capacity and closed-state fields |

`Recv.Value(v)` carries the channel payload, so `Channel<int?>` can distinguish a real `Recv.Value(null)` from `Recv.Closed`.

### 14.11 `Json`

`Json` is a dynamic structured-data type. Ordinary field access uses `j.field` / `j["field"]`; generic queries and serialization go through `Json` static functions to avoid colliding with user field names.

| Static function | Description |
|--|--|
| `Json.keys(obj)` / `Json.values(obj)` / `Json.entries(obj)` | enumerate object fields |
| `Json.containsKey(obj, key)` | field existence |
| `Json.get(obj, key, default?)` | field read; returns `default` or `null` if absent |
| `len(obj)` | element count for Object / Array / String variants; scalar values throw TypeError |
| `Json.parse(s)` / `Json.tryParse(s)` / `Json.isValid(s)` | JSON parsing and validation |
| `Json.encode(value)` | explicit typed value → Json boundary conversion |
| `Json.stringify(value, indent?)` | serialization |

**Literal**: `{ name: "alice", age: 30 }` defaults to sealed `Record`. It becomes a dynamic Json object only with an explicit `Json` annotation such as `var j: Json = {...}`; use `Json.encode(value)` when a typed value crosses a JSON boundary.

### 14.12 `Range`

`a..b` is the half-open interval `[a, b)`, while `a..=b` is the inclusive interval `[a, b]`. Both forms work in expressions, `for-in`, and range patterns in `match`.

| Member | Description |
|--|--|
| `start` / `end` | The start and the declared endpoint |
| `contains(x)` | Tests membership using the range's half-open or inclusive semantics |
| `toArray()` | Produces an independent `Array<int>` in iteration order |
| `toString()` | Returns an `a..b` or `a..=b` string |
| `iterator()` | iteration protocol; yields the same elements as `toArray()`, lazily |
| `len(range)` | Returns the number of elements in the range |

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

The `datetime` module provides factory functions through `import datetime`: `now`, `utc`, `create`, `createUTC`, `fromTimestamp`, `fromTimestampMs`, `parse`, and `offset`. `DateTime` is not a prelude type; import it explicitly with `import { DateTime } from datetime` when the name is used as a type.

| Member | Type / Description |
|--|--|
| `year` / `month` / `day` | date-component properties |
| `hour` / `minute` / `second` / `millisecond` | time-component properties |
| `weekday` / `yearday` / `timestamp` | derived properties |
| `toString()` / `format(pattern?)` / `toISOString()` | formatting |
| `add(amount, unit)` / `diff(other, unit?)` | date arithmetic |
| `toUTC()` / `toLocal()` | timezone conversion |
| `isBefore(other)` / `isAfter(other)` / `equals(other)` | comparison |
| `isLeapYear()` / `daysInMonth()` | calendar queries |

### 14.14 `Regex`

| Method | Description |
|--|--|
| `test(s)` | match predicate |
| `find(s)` | first match |
| `findAll(s)` | all matches |
| `findText(s)` / `findGroup(s, index)` | first matched text / capture-group text |
| `replace(s, replacement)` | replacement |
| `split(s, limit?)` | split |

### 14.15 `StringBuilder`

| Method | Description |
|--|--|
| `len(builder)` | current rune count |
| `append(s)` | append and return self |
| `toString()` | output string |
| `clear()` | empty and return self |

### 14.16 `PanicInfo`

The built-in `PanicInfo` class has fields `message`, `stack`, `cause`, `code`, `data`, the constructor `constructor(message: string = "", cause: PanicInfo? = null)`, and `toString()`.

### 14.17 `Task<T>` and Enum Values

`Task<T>` properties: `done`, `status`; methods: `cancel()`, `poll()`, `awaitResult()`, `awaitTimeout(ms)`. `poll()` and explicit wait methods return `TaskResult<T>` as `Success(T)`, `Failed(PanicInfo)`, `Cancelled`, `Timeout`, or `Pending`. Plain `await task` returns `T` on success and uses the matching error/panic path for failure or cancellation; a unique mutable result is taken once automatically, with no `await (move task)` form. Enum values provide the cold-path `name`, `ordinal`, and `toString()` surface.

### 14.18 Thread and Synchronization Handles

`Thread<T>` is a prelude handle type with the `done` field and the `join()` / `detach()` methods. After importing `sys`, create an OS thread with `sys.Thread.spawn(body)` or `sys.Thread.spawn(ThreadOptions{...}, body)`, then call `join()` or `detach()` on the returned handle. Import `CountdownLatch`, `EventCount`, `ResultGroup`, `Semaphore`, and `WorkQueue` from `sync`; import `Logger` from `log`; and import connection and listener types from `net`.

### 14.19 `Atomic<T>` Methods

`Atomic<T>` wraps `int`, `float`, or `bool` with lock-free atomic operations. Name the handle with `const`; audited atomic methods provide synchronized interior mutation.

| Method | Signature | Description |
|--|--|--|
| `load(ord?)` | `(Ordering?) -> T` | Atomically read the current value |
| `store(val, ord?)` | `(T, Ordering?) -> ()` | Atomic write |
| `add(val, ord?)` | `(T, Ordering?) -> ()` | Atomic add |
| `sub(val, ord?)` | `(T, Ordering?) -> ()` | Atomic subtract |
| `fetchAdd(val, ord?)` | `(T, Ordering?) -> T` | Atomic add, returning old value |
| `fetchSub(val, ord?)` | `(T, Ordering?) -> T` | Atomic subtract, returning old value |
| `swap(val, ord?)` | `(T, Ordering?) -> T` | Atomic swap, returns old value |
| `compareExchange(expected, desired, ord?)` | `(T, T, Ordering?) -> (T, bool)` | CAS, returns `(old_value, success)` |
| `toggle(ord?)` | `(Ordering?) -> bool` | Atomic negate (bool only), returns old value |
| `toString()` | `() -> string` | Returns string representation of current value |

The `ord?` parameter accepts an `Ordering` enum; defaults to `Ordering.SeqCst`. See §10.9.

---

## 15. Standard Library Overview

> Source of truth: `stdlib/defs/*.def`, pure-Xray `stdlib/<module>/<module>.xr` exports, `stdlib/types/*.xr` native type declarations, and `scripts/gen_api_inventory.py`, which merges those sources.
> MCP knowledge and the API inventory use the source-derived inventory; `xray builtin-dump` is only one runtime builtin-view input.
> See [Appendix D — stdlib module index](#d-stdlib-module-index).

> **Authoritative stdlib module list** (28 modules; source: `stdlib/<module>/*.c` / `stdlib/<module>/*.xr`):
>
> `base64`, `cluster`, `compress`, `crypto`, `csv`, `datetime`, `encoding`, `http`, `io`, `log`, `math`, `mem`, `net`, `os`, `parallel`, `path`, `regex`, `runtime`, `strconv`, `sync`, `sys`, `text`, `time`, `toml`, `url`, `ws`, `xml`, `yaml`.
>
> The exact prelude type set is: `Array`, `Atomic`, `OsBarrier`, `BigInt`, `Channel`, `OsCondvar`, `PanicInfo`, `Json`, `Map`, `OsMutex`, `NetConn`, `NetListener`, `OsOnce`, `Path`, `Range`, `Regex`, `OsRwLock`, `Set`, `StringBuilder`, and `Thread`. `Array<byte>` is an `Array` specialization; module types such as `DateTime` and `Logger` must be imported. See §1.5.6 / §2.2.

### 15.1 File I/O and System

| Module | Topic | Key APIs |
|--|--|--|
| `io` | file I/O + filesystem | `readFile` `writeFile` `readFileBytes` `writeFileBytes` `exists` `mkdir` `mkdirp` `remove` `readDir` `stat` `readStdin` |
| `path` | path manipulation | `join` `dirname` `basename` `extname` `normalize` `isAbsolute` `resolve` `relative` `parse` `format` |
| `os` | OS interface | `getenv` `setenv` `environ` `exit` `getpid` `getcwd` `chdir` `hostname` `tmpdir` `homedir` `cpuCount` `sleep` `exec`; constants `platform` `arch` `sep` `eol` |

> Xray has **no** standalone `fs` module; filesystem operations live in `io`. Process arguments / process information are exposed through the global `process` object (`process.args` / `process.file` / `process.dir`, see §16.5), not `os`.
> `os.platform` / `os.arch` / `os.sep` / `os.eol` are **constant strings** (no parentheses); other `os.*` are function calls.

### 15.2 Networking

| Module | Topic | Key APIs |
|--|--|--|
| `net` | TCP / UDP / TLS sockets + DNS | `listen` `dial` `accept` `read` `readInto` `write` `writeBytes` `copy` `copyBidirectional` `setDeadline` `lastError` `lookup` `dialTLS` `NetConn` `NetListener` |
| `http` | HTTP / HTTPS client + server + HTTP/2 | `request` `h2Request` `listen` `router` `routeHandler` `requestText` `responseText` `parseResponseText` |
| `ws` | WebSocket | `connect` `serve` `send` `recv` `close` `parseFrame` `parseUrl` `parseUpgradeRequest` `clientHandshakeRequest` |
| `url` | URL parsing and construction | `URL` `QueryParams` `parse` `format` `parseQuery` `encode` `decode` |

> DNS lookups go through `net.lookup(host)`; there is no standalone `dns` module.

#### 15.2.1 TCP Data Paths

The `net` TCP API intentionally has three data paths:

- `read(conn)` / `write(conn, data)`: message path. Payload is exposed as an Xray `string`, suitable for protocol parsing, text handling, and logic that must inspect bytes.
- `readInto(conn, bytes, maxlen?)` / `writeBytes(conn, bytes)`: reusable `Array<byte>` buffer path for binary protocol hot loops without per-packet temporary strings.
- `copy(src, dst)` / `copyBidirectional(a, b)`: native stream path. Payload stays in a reusable C buffer, suitable for proxy, relay, `copy(conn, conn)` echo, and other high-throughput workloads that do not need to inspect every byte in Xray code.

Design rule: raw streams should not allocate temporary strings merely to pass through the language layer; use string APIs only when application logic needs the bytes.

TCP waiting operations use the coroutine-friendly netpoll path. `setReadDeadline(conn, deadline)`, `setWriteDeadline(conn, deadline)`, `setDeadline(conn, deadline)`, and `setAcceptDeadline(listener, deadline)` accept `time.monotonic()` millisecond deadlines; after a timeout, operations return their normal `null` or `-1` failure shape, and `lastError(handle)` / `lastErrno(handle)` expose diagnostic causes.

`shutdownRead(conn)`, `shutdownWrite(conn)`, and `shutdown(conn)` expose TCP half-close semantics. Generic proxy and relay code should prefer `copyBidirectional(a, b)`, which half-closes the opposite write side after one-way EOF and returns byte counts for both directions.

The TLS client path is provided by `dialTLS(host, port, timeout?)` and `upgradeTLS(conn, hostname, timeout?)`; TLS read/write/copy share the same deadline, diagnostic error, and typed-handle lifecycle semantics as plain TCP.

### 15.3 Data Formats

| Module | Topic |
|--|--|
| `yaml` | YAML |
| `toml` | TOML |
| `xml` | XML |
| `csv` | CSV |
| `base64` | Base64 encode / decode |
| `encoding` | hex / UTF-8 and other generic encodings (Base64 lives in its own module) |

> JSON encoding/decoding is **not** in a separate `json` module; use the built-in type `Json`'s static methods `Json.parse(s)` / `Json.encode(v)` / `Json.stringify(v)` (no import required; see §14.11).

### 15.4 Cryptography and Hashing

| Module | Key APIs |
|--|--|
| `crypto` | `md5` `sha1` `sha256` `sha512` `hmac` `encrypt` `decrypt` `randomBytes` `timingSafeEqual` `uuid` |

> stdlib has **no** standalone `random` module; for pseudo-random numbers use `crypto`'s random source or `math` utilities.

### 15.5 Compression

| Module | Key APIs |
|--|--|
| `compress` | `gzip` / `gunzip`, `deflate` / `inflate`, etc. |

### 15.6 Time

| Module | Key APIs |
|--|--|
| `time` | `now()` `monotonic()` `clock()` `micros()` `nanos()` `sleep(ms)` `localOffset()` `localOffsetAt()` |
| `datetime` | `DateTime` plus factories such as `now()` `utc()` `create()` `createUTC()` `fromTimestamp()` and `parse()` (see §14.13) |

### 15.7 Math

| Module | Key APIs |
|--|--|
| `math` | `sin` `cos` `tan` `log` `pow` `sqrt` `floor` `ceil` `round` `abs` `min` `max` etc.; constants `PI` / `E` / `MAX_INT` / `MIN_INT` |

### 15.8 Text

| Module | Key APIs |
|--|--|
| `regex` | `compile(pattern)` returns `Regex`; see §14.14. The `/pattern/flags` literal form is also supported |
| `text` | `lower` `upper` `trim` `trimStart` `trimEnd` `padStart` `padEnd` `reverseRunes` `translate` |
| `strconv` | `parseInt` `parseFloat` |

The built-ins `int(s)` / `float(s)` / `string(n)` remain available for ordinary conversions; use `strconv` when radix/default parsing controls are needed.

### 15.9 Logging and Diagnostics

| Module | Key APIs |
|--|--|
| `log` | `debug` / `info` / `warn` / `error` / `fatal` / `child()`, source-position toggles, async write mode |
| `runtime` | `liveBytes()` `liveObjects()` `info()` |
| `mem` | `alloc()` / `allocZeroed()` / `allocAligned()` return managed `Buffer`; `pageAlloc()` / `pageFree()`; `copy()` / `move()` / `set()` / `compare()`; `volatileLoad()` / `volatileStore()`; `fence()` |
| `sync` | coroutine-domain synchronization: `Mutex` `RwLock` `Once` `Barrier` `Condvar` `CachePadded` `fence()`, with explicit `import sync` |
| `sys` | low-level OS/thread surface: compiler-defined `sys.Thread.spawn(...)` with `ThreadOptions`, plus `ThreadLocal`, `OsMutex`, `OsRwLock`, `OsCondvar`, `OsBarrier`, `OsOnce`, process/dylib/pipe handles, `cpuCount()`, `sleepMs()`, `threadYield()`, `pinToCpu()`, and `onSignal()` |

### 15.10 Parallelism

`parallel` exports `forEach` and the `Plan` abstraction for structured CPU-parallel work. `parallel` is not a language keyword; import the module explicitly.

### 15.11 Distributed

| Module | Topic |
|--|--|
| `cluster` | node discovery, health checks, topic-based message bus (see `stdlib/cluster/`) |

### 15.12 Testing

The `@test` attribute together with the global `assert*` family is enough; **no** separate `test` module is needed (see §12).

### 15.13 Modules That **Do Not Exist**

Modules that may have been referenced historically but are **not** part of the current stdlib (to avoid confusion):

`fs` · `process` · `dns` · `random` · `json`

Their functionality has either moved into other modules (see the per-section notes above) or has not yet been implemented.

> **Full index**: see [Appendix D](#d-stdlib-module-index).

---

## 16. Runtime Model

> Source of truth: `src/runtime/`, `src/vm/`, `src/runtime/mem/`, `docs/rules/architecture.md`.

### 16.1 Value Representation

Xray values are uniformly represented as `XrValue`. The current implementation requires a 64-bit platform and uses a **16-byte tagged struct-of-union**:

- **Descriptor (8 bytes)**: `tag: byte`, `flags: byte`, `heap_type: u16`, and `ext: u32`. The `tag` is the single entry point for type dispatch; `heap_type` is meaningful only when `tag == PTR`.
- **Payload (8 bytes)**: one of `i64`, `double`, or pointer, interpreted by the tag.
- **No NaN-boxing / no low-bit pointer tagging**: integers keep the full 64-bit payload; object references are ordinary heap pointers, with type metadata in the descriptor.
- **Strings are not value-level SSO**: `string` is always an `XrString` heap object, with bytes stored inside the object's `data[]` flexible array. Runtime short strings are coroutine-local with lock-free allocation by default; literals/symbols, explicit `intern()`, and map/set keys use the global pool. Cross-execution storage is selected from verified context at construction/publication time; a boundary never copies or promotes the payload implicitly. These are object-storage policies and do not change the `XrValue` representation.

| Value type | Internal representation |
|--|--|
| `int` | `XR_TAG_I64` + 64-bit signed payload |
| `float` | `XR_TAG_F64` + IEEE-754 double payload |
| `bool` | `XR_TAG_BOOL` + `0/1` payload |
| `rune` | `XR_TAG_RUNE` + Unicode scalar payload |
| `null` | `XR_TAG_NULL` + zero payload |
| `string` | `XR_TAG_PTR` + `XR_TSTRING` + `XrString*` |
| `Array<byte>` | `XR_TAG_PTR` + `XR_TARRAY`, with byte element layout |
| Other objects | `XR_TAG_PTR` + heap type + heap pointer |

Typed-array element layout is part of the container metadata. `Array<rune>` uses `XR_ELEM_RUNE`; its data area is a contiguous `uint32_t[]` of Unicode scalars. Loads re-box values as `XR_TAG_RUNE`, and stores reject non-`rune` values, so it cannot be confused with `Array<u32>`.

### 16.2 Semantic Ownership Domains

Storage is described by **two orthogonal axes**. Conflating them yields wrong conclusions:

- **Semantic ownership domain** (this section): who owns the storage, how long it lives, and what rule governs its migration across an execution boundary. This is a **semantic contract**.
- **Backend materialization** (§16.2.1): where the backend actually puts it (inline, stack, static data, some heap). This is an **implementation choice**.

> Source of truth: `XrSemanticStorageDomain`, `XrBackendMaterialization`, and `XrTransferAction` in `src/base/xstorage.h`.

Every value belongs to exactly one of six semantic domains:

| Domain | Ownership and lifetime | Reference counting | Typical origin |
|--|--|--|--|
| **`EXEC_LOCAL`** | Owned exclusively by the current execution (coroutine); ends with that execution's heap | Non-atomic | Ordinary local objects; containers and instances built from literals |
| **`TRANSFERABLE`** | Unique ownership that can be **handed over whole** across an execution boundary; unusable on the source side afterwards | Non-atomic | The target of a `move`; unique values passed between coroutines |
| **`CONST_SHARED`** | A published immutable root, read concurrently by several executions | Atomic | A published `const` root; the result of `freeze` |
| **`SYNC_SHARED`** | An audited concurrency handle accessed by several executions, where the handle itself provides the synchronization | Atomic | Values held by channels, mutexes, and other concurrency primitives |
| **`MODULE_STATIC`** | Owned by the module, alive until it unloads; **not concurrency-safe by default** | Per module owner | Top-level `const` / `var` |
| **`FOREIGN`** | Owned by an allocator outside Xray; the runtime does not manage its lifetime | None | FFI pointers, external buffers |

**A domain fixes concurrency visibility and reclamation responsibility, not physical location.** An `EXEC_LOCAL` value may be inlined into its holder, placed on the stack, or allocated on the execution heap — none of the three changes the contract that it ends with that coroutine.

#### 16.2.1 Backend Materialization

Subject to the semantic domain's contract, the backend picks one materialization per value:

| Materialization | Meaning |
|--|--|
| `INLINE` | Inlined into the holder's storage, with no separate address identity |
| `STACK` | On the function frame; dies with the frame, cannot grow, cannot be freed |
| `STATIC_DATA` | Read-only static data (consteval results, literal pools) |
| `EXEC_HEAP` | The current execution's heap (`XrCoroHeap`), reclaimed by reference counting |
| `SYSTEM_HEAP` | The system heap; objects visible across executions live here |
| `SROA` | Scalar-replaced into registers; the aggregate no longer exists concretely |
| `EXTERNAL` | Held by an external allocator |
| `INVALID` | Undecided; reaching a final plan is an internal error |

> **Changing materialization must never grant a stronger semantic domain or transfer capability.** Materializing an `EXEC_LOCAL` value on the `SYSTEM_HEAP` does not make it shareable across executions; conversely, materializing a `CONST_SHARED` value on the stack is invalid, because its domain requires a stable address for as long as it is shared. Materialization is **downstream** of the semantic domain, never a justification.

#### 16.2.2 Domain Migration Across Execution Boundaries

When a value crosses an execution boundary (captured into `go`, sent through a channel, returned as a task result), its **source domain** and mutability select a transfer action. This is decided at compile time; it is not a runtime downgrade:

| Action | Selected when | Semantics |
|--|--|--|
| `INLINE_COPY` | Scalar, and not a mutable capture | Copied by value; the two sides are independent afterwards |
| `CONST_SHARE` | Source domain is `CONST_SHARED` | Shares the same immutable root under atomic reference counting |
| `SYNC_SHARE` | Source domain is `SYNC_SHARED`, or the capture is itself a concurrency handle | Shares the same handle; the handle provides synchronization |
| `MOVE_UNIQUE` | Source domain is `TRANSFERABLE` | Ownership is handed over whole; the source is unusable afterwards |
| `EXPLICIT_COPY` | **Only** where the source wrote `copy(...)` | Materializes an independent value, then migrates it |
| `MODULE_READ` | Source domain is `MODULE_STATIC` and immutable | Reads the module binding by reference |
| **`REJECT`** | **None of the above holds** | **Compile error** |

`REJECT` is the most important row: **there is no implicit deep-copy fallback across an execution boundary.** A mutable module binding, an ordinary capture that is mutable or reassigned, and a managed value with no const/sync/move evidence are all compile errors rather than a silent copy. Where an independent copy is wanted, the source must say `copy(...)`.

### 16.3 Object Lifetime and Reclamation

- Ordinary local objects use compiler-inserted **per-coroutine reference counting** and enter the RC destruction path as soon as their last strong reference is released. Shared objects use atomic RC; module and runtime objects follow their respective owners' lifetimes.
- **Cycles are not reclaimed at runtime**: reclamation has exactly one rule — an object dies when its last strong reference goes. Cycles are handled in three layers instead: a compile-time type graph proves most programs never form one (L0), a `weak` field is the sole explicit way to break one (L1), and bulk disposal of the execution-local reclamation domain bounds whatever remains (L2). The development-mode detector reports cycles; it never collects them.
- **Execution-local reclamation-domain boundary (L2)**: every physical coroutine owns one domain. The VM realizes it as a per-coroutine Region heap; hosted AOT uses an execution arena that registers still-live ordinary ARC allocations. Acyclic objects still die immediately at their last strong reference; when the coroutine ends, the domain disposes the complete residual graph. Thus **an object reference counting did not reclaim — a cycle — leaks no further than the lifetime of the coroutine that built it**. Cross-execution publication must first detach the complete owned graph of a shared or transferred root. Only `MODULE_STATIC` / `CONST_SHARED` / `SYNC_SHARED` domains and the main execution's root domain may live for the process lifetime.
- **This property is unusual among pure reference-counted languages**: an Rc/RefCell cycle in Rust, or a strong-reference cycle in Swift, leaks until the process exits — both allocate refcounted objects on a process-wide heap, with no boundary smaller than the process that can be dropped wholesale. Xray's execution-local reclamation domain is that boundary, so forgetting a `weak` costs a **bounded** leak rather than a permanent one. This caps the lifetime; it is not cycle collection.
- Xray has no concurrent tracing GC and no cycle collector; no GC hooks remain at function calls or backward branches.
- **User-visible introspection**: `runtime.liveBytes()` / `runtime.liveObjects()` / `runtime.info()` report the current execution-local reclamation domain (a VM coroutine heap or AOT execution arena), falling back to the main/root domain when no coroutine is current (`import runtime`; the `mem` module carries raw-memory capabilities only).

#### 16.3.1 The two reference-count bands

There is one reference-count field, and its **sign** selects between two bands:

| Band | Encoding | Used by | Atomicity |
|--|--|--|--|
| coroutine-local | positive | ordinary local object graphs | non-atomic (relaxed fast path) |
| shared | negative (`N` references stored as `-N`) | published const values, synchronization handles, module and runtime objects | atomic |

A sign test on the hot path routes each object to the right path automatically, so the two bands coexist without an extra branch. Immortal objects store a sticky value and never participate in counting.

**Which band an object lands in is decided at compile time**, by the storage plan of its allocation site (the capability axis of §2.13.2 plus the allocation domain); it is not a runtime conversion, and a live object is never silently re-banded when it crosses a boundary. That is load-bearing: re-banding at runtime would have to synchronize with concurrent non-atomic read-modify-writes, which is exactly what the non-atomic band assumes cannot happen.

Only two shapes therefore cross an execution boundary:

1. **Sharing**: the value's allocation plan is const-shared or sync-shared, so it is in the atomic band from creation. Published `const` values and audited handles such as `Channel`, `Atomic`, and `Semaphore` are of this kind.
2. **Transfer**: `move` hands over a unique root. Exactly one owner exists at any moment, so a non-atomic count stays correct.

**The correctness of the second shape rests directly on the uniqueness evidence in §2.13.** If a root that still has an alias or a live loan were transferred out, two execution contexts would read-modify-write one non-atomic counter — which is not a data race but memory corruption (a double free or a premature free). That is why §2.13 counts closure captures and heap stores as evidence: the ownership decision is a precondition for reference-count correctness, not merely a programming discipline.

Real OS threads (a `sys.Thread.spawn` body, a `CFn` callback) follow the same rules with no relaxation: a thread body may reach only values whose allocation plan is shared, or a unique root transferred in through a `Channel`.

See `src/runtime/mem/` and `src/shared/xr_obj_header.h` for details.

### 16.4 Coroutine Scheduling

- M:N scheduling (M OS threads × N coroutines).
- **work-stealing**: idle workers steal tasks from other workers' queues.
- **Cooperative preemption**: coroutines yield at safepoints (no forced preemption).
- **Fairness**: a single runnable queue works with local run-next, global injection, and work-stealing; scheduling order does not expose user-level priorities.
- **Stack management**: VM register/frame stacks grow on demand and may relocate their backing storage; the runtime re-derives pointers from slot offsets.

See `src/coro/` and `src/vm/xvm_coro_backend.c` for details.

### 16.5 Process-Level Global Access

- `process` (global builtin, no import required): self-process information.
- `os` (requires `import os`): operating system, environment, process control.

```xray
// Self-process information — global object
process.file              // current script path (equivalent to __file__)
process.args              // Array<string>, process command-line arguments
process.dir               // script directory (equivalent to __dir__)

// OS / environment — requires import
import os
os.getenv("PATH")         // read environment variable -> string?
os.environ()              // get all environment variables -> Map<string, string>
os.exit(0)                // exit the process
os.getpid()               // process ID
os.getcwd()               // current working directory
os.hostname()             // host name
os.tmpdir()               // temporary directory
os.platform               // constant: "darwin" / "linux" / "windows"
os.arch                   // constant: "arm64" / "x64" / "x86" / "ppc64" / "riscv64"
os.sep                    // constant: path separator
os.eol                    // constant: end-of-line
os.sleep(100)             // sleep in milliseconds (equivalent to `time.sleep`)
```

> **Naming convention**: `os.*` follows POSIX function names (`getenv` / `getcwd` / `getpid`); it does not track Node.js. Node-style `process.env` mapping is not provided — use `os.getenv(name)` / `os.environ()`.

See `stdlib/os/` for details.

### 16.6 Panic Runtime

The built-in `PanicInfo` class is a prelude type (declared in `stdlib/types/panic_info.xr`) and belongs only to the **panic channel**. Runtime faults (out-of-bounds, division by zero, non-exhaustive `match`, runtime invariant violations, and similar faults) are represented by `PanicInfo` objects constructed by the VM/AOT runtime:

```xray
class PanicInfo {
    message: string             // human-readable message
    stack: Array<string>        // automatically captured call stack, one formatted line per frame
    cause: PanicInfo?           // chained cause
    code: int                   // error code (auto-parsed from "E0xxx: ..." prefix; default 0)
    data: Json                  // structured data for a runtime fault; JSON null when absent

    constructor(message: string = "", cause: PanicInfo? = null)
    fn toString() -> string
}
```

Recoverable user-level errors do not use `PanicInfo`: a `throw` statement accepts enum variant values only (see §8.1.1); non-enum error values are rejected at compile time (error code `E0370`).

Stack unwinding (panic channel only): the VM's `xvm_unwind_stack()` walks the try-table to find `catch panic` handlers, releasing locals frame by frame and running `defer` along the way before jumping to the handler. Recoverable errors use the value-return channel and never unwind the stack. See §8 for details.

### 16.7 Value-return Error Channel Runtime

`throw <enum>` writes the enum value into the frame's `pending_error` slot, sets the error flag, and returns. The caller detects the flag via `OP_ERR_CHECK` and either enters a `catch` handler or continues returning upward. This channel performs no stack unwinding and allocates no `PanicInfo`; at call boundaries that may propagate or catch errors, the happy path goes through only a predictable error-flag branch.

### 16.8 Object Reclamation & Finalizer Timing Contract

**The reclamation point is exact, with no exceptions**: an object is reclaimed the moment its last strong reference is released. VM and AOT give the same answer here.

- The promise can be universal because reference counting is the only reclamation mechanism. There is no cycle collector, no tracing GC, and no indeterminate "some GC point" at runtime (task 247).
- The one shape RC does not reclaim is a **reference cycle**, and that too is determinate: cycles are not collected at runtime, and what they cost is capped by the execution-local reclamation-domain boundary (L2 in §16.3). Disposing the residual graph when its domain ends is not cycle collection. Cycles are prevented by the compile-time type graph (L0) and broken explicitly with `weak` (L1); the development detector reports them and never reclaims.
- What is still **not** offered is a user-visible deterministic destructor (destructor / finalizer / `Drop`) surface: a program must not depend on whether a type has a finalization hook, in what order such hooks run, or on which thread. What is exact is the **moment of reclamation**, not the ability to attach code to it.

The only deterministic, cross-backend (VM / AOT) consistent cleanup mechanism is **`defer`**, which runs at owning-scope exit in LIFO order, independent of object reclamation timing. Code that must deterministically release external resources (files / handles / locks) must use `defer` rather than relying on object finalization.

**`defer` runs on every exit edge**: a normal return, `throw` unwinding, panic unwinding, and **coroutine cancellation**. Cancellation is not an external kill: a cancelled coroutine that still owes a defer chain is handed back to the scheduler, resumed once on its own worker, and marked cancelled only after it has unwound. `task.cancel()` correspondingly does not let an `await` observe the cancellation before that cleanup has run.

For this guarantee to be **total**, a defer body must **not reach a scheduler suspension point** (`E0392`, see §2.14.4). A defer body runs on a frame that is already leaving and has no place to park and resume; allowing it to suspend would allow cleanup to be dropped halfway, and `defer` would stop being deterministic. Cancellation is masked while a defer body runs, so the cleanup itself cannot be interrupted by a cancellation.

> Evolution note: once deterministic destruction (RAII / `Drop`) is formally added to the language, this section will be upgraded to a **deterministic reclamation contract** (specifying destruction points and order), gated byte-for-byte by cross-backend differential tests. Until then, "reclamation timing / finalizer behavior" is explicitly declared an implementation-defined, non-deterministic aspect.

### 16.9 Concurrency Memory Model

> Source of truth: `src/coro/xchannel.c`, `src/coro/xtask.c`, `src/coro/xtask_await.c`, the `:sync` column of `xisa/xi/ops.def`, `contracts/memory-model.md`.

This section defines when accesses by two execution agents to the same memory location are **ordered**. The `Ordering` enum in §10.9 describes one atomic operation; it is not enough to derive program behaviour. This section is.

#### 16.9.1 Basic definitions

**Execution agent**: coroutines and OS threads are both execution agents. A coroutine may be moved between worker threads by the work-stealing scheduler during its lifetime and remains the same agent (see edge 12).

**Sequenced-before**: the total order of evaluation within one agent, given by §3.0 Evaluation Order. Because §3.0 leaves no order unspecified, this relation is total rather than partial.

**Synchronizes-with**: given by the edge table in §16.9.2. **That table is exhaustive**: no runtime behaviour outside it establishes synchronizes-with.

**Happens-before**: the transitive closure of (sequenced-before ∪ synchronizes-with).

**Conflicting accesses**: accesses by two agents to the same memory location, at least one of which is a write.

**Data race**: a pair of conflicting accesses with **no** happens-before between them in either direction, where the two are **not both** atomic operations on the same atomic object.

Reference-count updates are themselves memory accesses: concurrent retain/release on a non-atomic RC is a data race whose consequence is memory corruption, not merely a wrong number. The promotion rules are in §16.9.4.

#### 16.9.2 Synchronisation edges (exhaustive)

| # | Edge | Relation established |
|--:|---|---|
| 1 | Channel send → recv | Everything sequenced **before** the k-th successful `send(v)` happens-before everything sequenced **after** the `recv` / `recvOr` / `tryRecv` / `for-in` iteration that receives v. Buffered and unbuffered channels follow the same rule |
| 2 | Channel recv → later send (capacity edge) | On a channel of capacity N, completion of the k-th `recv` happens-before completion of the (k + N)-th `send`. This edge is what makes `Channel(N)` correct as a semaphore or rate limiter |
| 3 | Channel close | Everything before `close()` happens-before everything after any observation of `Recv.Closed` or `isClosed == true` |
| 4 | `go` spawn | Evaluation of the arguments, capture plan and `move` transfers at the `go expr` happens-before the first statement of the coroutine body |
| 5 | Task completion → await | The task body's last action happens-before everything after any `await t` / `awaitResult()` / `awaitTimeout()` return, or after observing `t.done == true`, a terminal `t.poll()` result, or a terminal `t.status` |
| 6 | scope exit | Completion of every child task in a scope happens-before the statements after the scope block. `await all` / `any` / `anySuccess` establish the same edge under their respective completion conditions |
| 7 | `const` publish / seal | Initialisation and sealing of a top-level `const` happens-before any agent's first read of that const root |
| 8 | Atomic release / acquire | `store(v, Release)` synchronizes-with the `load(Acquire)` that reads v; RMWs form a release sequence; `SeqCst` operations additionally participate in one single total order. `AcquireRelease` carries both sides |
| 9 | Coroutine-domain locks | `sync.Mutex` / `sync.RwLock`: an unlock synchronizes-with a later successful lock |
| 10 | OS-thread-domain primitives | `sys.OsMutex` / `sys.OsRwLock` / `sys.OsCondvar` / `sys.OsBarrier`: as above; a `sys.OsOnce` initialisation happens-before every call that observes it as complete |
| 11 | `sys.Thread` | Everything before `spawn` happens-before the thread body; the thread body happens-before everything after `join` returns |
| 12 | Coroutine migration | A coroutine suspended on worker A and resumed on worker B: everything before the suspension happens-before everything after the resumption. The runtime guarantees this edge; user code never names it, but **without it no ordering argument holds under work-stealing at all** |
| 13 | `select` | The selected arm establishes the edge of its channel operation (edges 1 / 2 / 3) |

#### 16.9.3 Non-edges (exhaustively denied)

The following establish **no** happens-before and must not be used to synchronise:

- `Coro.yield()`: a scheduling yield point, not a synchronisation point.
- `codegen.compilerFence()`: it only prevents memory-effecting operations from moving across that compiler scheduling point. It is not a CPU fence, establishes no happens-before, and cannot repair a data race (see §12).
- Atomic operations with `Ordering.Relaxed`: they keep that location's accesses from tearing; they order nothing with respect to other locations.
- Object reclamation, an RC reaching zero, or a cycle-collector run: reclamation timing is not observable semantics (§16.8), so it cannot carry an ordering relation.
- The sysmon heartbeat and the `XRAY_SYSMON_CANCEL_MS` cancellation flag.
- Time: neither `time.sleep` nor any timeout expiry establishes ordering.

#### 16.9.4 Reference-count promotion rules

Ordinary local objects use **coroutine-local, non-atomic RC** (§16.3). Once an object can be reached by two agents, its reference count **must** already have been promoted to atomic RC; otherwise concurrent retain/release is a data race whose consequence is heap corruption.

The promotion points are exhaustive, and each applies to the reachable graph **deeply** (the object and every managed member reachable from it):

| Promotion point | Notes |
|---|---|
| The value of a Channel `send` | Including a `move` send; the value stays atomically counted after the receiver takes exclusive ownership |
| `go` / `scope` arguments and capture-plan members | Determined statically by the capture plan |
| Task / scope result values | Delivered through edge 5 / edge 6 |
| The reachable graph of a `const` publish / seal | Promoted once; the graph is deeply read-only afterwards |
| Everything reachable across a `sys.Thread` boundary or from a `CFn` callback | See §16.9.5: that channel is currently outside the safe subset |

`string` promotion is described separately in §16.1 (short strings are coroutine-local, promoted to shared atomic RC on demand when they cross a boundary). That is this same rule instantiated for `XrString`, not an exception to it.

#### 16.9.5 The data-race-freedom guarantee and its boundary

**Theorem (DRF for the safe subset)**: a program containing no `unsafe` block and using none of the escape hatches below has no data race, and its behaviour is equivalent to some sequentially consistent interleaving. That is SC-DRF: inside the safe subset you may reason in terms of interleaved statements and never need the happens-before detail in this section.

**Escape hatches (exhaustive)**: the following capabilities can introduce a data race. Their consequences are **implementation-defined and may include memory corruption**; the proof obligation is the user's:

- Any raw memory access inside an `unsafe` block
- The writable raw pointer from `Array.mutPtr()` (as an **argument** a pointer crosses a coroutine boundary without passing through capture-plan checking)
- `mem.volatileLoad` / `volatileStore` / `fence` / `pageAlloc`
- The body of a `sys.Thread`
- The body of a `CFn` C callback (which may be invoked on any thread)

> This is the **precise form** of the claim that xray eliminates data races at compile time: a theorem with a stated boundary, not an unconditional assertion. Shrinking the escape-hatch list is a goal of the language's evolution, and each shrink must come with the mechanism that brings that capability inside the guarantee.

#### 16.9.6 Obligations on the compiler

A synchronisation edge is not only a runtime promise; it also constrains optimisation. The `:sync` column of `xisa/xi/ops.def` declares the edge each Xi operation establishes (`none` / `acquire` / `release` / `acq-rel` / `seq-cst`). Where the edge's strength is chosen at run time by an `Ordering` argument, the declaration is the **strongest** edge that operation can carry — a fail-closed upper bound.

From this follows one hard rule for every pass: **alias disjointness is not a licence to reorder**. Two memory operations must not be moved across an operation that carries a `:sync` edge or that may suspend, even when TBAA proves them disjoint. `xi_op_is_ordering_barrier()` is the single answer to that question, and `contracts/memory-model.md` freezes it.

---

## 17. Compilation Pipeline

> Sources of truth: `src/frontend/`, `src/ir/`, `src/vm/`, `src/aot/`, `src/toolchain/`, `src/app/cli/xcmd_build.c`, and `src/app/cli/xcmd_compile.c`.

### 17.1 Two Execution Paths

```
source (.xr) -> lexer -> AST -> analyzer / canonical evidence
                                  |-> bytecode compiler -> XrProto -> VM
                                  `-> Xi IR -> optimization -> C source -> host/cross C toolchain -> native binary
```

Xray currently has no JIT. `xray run` and default `xray build` use the bytecode/VM path; `xray build --native` selects AOT. Both paths share the parser, analyzer, module graph, and runtime semantics, but their output representations and backend optimizations differ.

### 17.2 Frontend

- Lexer: `src/frontend/lexer/xlex.c` / `xlex.h`, with `xkeywords.def` as the keyword table; input must be valid UTF-8.
- Parser: handwritten recursive-descent and precedence expression parsing in `src/frontend/parser/xparse*.c`, producing an `AstNode` module tree.
- Analyzer: `src/frontend/analyzer/` performs name, type, generic, visibility, ownership, capture, and error-set checks.
- Canonical evidence: `src/frontend/canonical/` plus the module/toolchain layer stabilize cross-module facts for backend consumers.

### 17.3 Bytecode and VM

`src/frontend/codegen/xcompiler.c` compiles analyzed AST into an `XrProto`. The single opcode list is `src/runtime/value/xopcode_def.h`; the VM lives in `src/vm/`. Its register-oriented instruction set includes property/call fast paths and dedicated coroutine, error-channel, and tail-call operations.

`xray compile file.xr` emits `.xrc` by default. `--format bytecode|c|header` selects serialized bytecode or a C source/header representation that embeds the bytecode. `--format c` here is **not** the native AOT C backend.

Bytecode must serialize extern aggregate layouts and the target ABI fingerprint in deterministic order. Before execution, the loader must validate layout depth, recursion cycles, field bounds, total size, trailing data, and ABI compatibility; corrupt or target-mismatched input is rejected rather than falling back to host layout.

### 17.4 Xi IR and Optimization

The native path lowers the program to Xi IR in `src/ir/`. `xi_pipeline.c` / `xi_pass.h` organize passes including SCCP, DCE/CFG simplification, inlining, devirtualization, tail-call rewriting, escape/ownership processing, loop transforms, GVN/PRE, bounds-check elimination, and vectorization. The enabled set depends on optimization level, target, and legality; the existence of a pass does not guarantee that every program is transformed by it.

### 17.5 Native AOT

`xray build --native file.xr` uses the prepare/verify/representation/container/link plans in `src/aot/`, generates C from Xi IR, and invokes a host, Clang, or Zig C toolchain. Hosted native binaries still link the Xray runtime; `--profile freestanding` uses a restricted freestanding capability set. Consult `xray build --help` for current `--target`, `--toolchain`, `--cpu`, `--lto`, and related options.

Native AOT does not emit machine code directly from SSA and is not a JIT; the selected C toolchain produces the final machine code.

---

## 18. Error Codes

> The single source of truth is `src/runtime/xerror_codes.h`. `XrErrorCode` is an `int` in `src/runtime/xerror.h`; user-facing rendering is `Exxxx`. Gaps are allowed and must not be filled with names absent from the header.

### 18.1 Lexer and Parser

| Code | C name | Meaning |
|--|--|--|
| `E0101` | `XR_ERR_LEX_INVALID_CHAR` | invalid character |
| `E0102` | `XR_ERR_LEX_UNTERMINATED_STR` | unterminated string |
| `E0103` | `XR_ERR_LEX_INVALID_NUMBER` | invalid numeric literal |
| `E0104` | `XR_ERR_LEX_INVALID_ESCAPE` | invalid escape |

#### Parser

| Code | C name | Meaning |
|--|--|--|
| `E0201` | `XR_ERR_SYN_UNEXPECTED_TOKEN` | unexpected token |
| `E0202` | `XR_ERR_SYN_EXPECTED_EXPR` | expression expected |
| `E0203` | `XR_ERR_SYN_EXPECTED_STMT` | statement expected |
| `E0204` | `XR_ERR_SYN_UNCLOSED_PAREN` | unclosed parenthesis |
| `E0205` | `XR_ERR_SYN_UNCLOSED_BRACE` | unclosed brace |
| `E0206` | `XR_ERR_SYN_UNCLOSED_BRACKET` | unclosed bracket |
| `E0207` | `XR_ERR_SYN_INVALID_ASSIGN` | invalid assignment target or form |
| `E0208` | `XR_ERR_SYN_EFFECTLESS_STMT` | expression statement has no effect; its result is discarded (see §1.2.1) |

### 18.2 Compilation and Static Analysis

| Code | C name | Meaning |
|--|--|--|
| `E0301` | `XR_ERR_CMP_UNDEFINED_VAR` | undefined variable at compiler stage |
| `E0302` | `XR_ERR_CMP_REDEFINED_VAR` | redefined variable |
| `E0303` | `XR_ERR_CMP_CONST_ASSIGN` | assignment to a constant |
| `E0304` | `XR_ERR_CMP_INVALID_BREAK` | invalid `break` |
| `E0305` | `XR_ERR_CMP_INVALID_CONTINUE` | invalid `continue` |
| `E0306` | `XR_ERR_CMP_INVALID_RETURN` | invalid `return` |
| `E0307` | `XR_ERR_CMP_TOO_MANY_PARAMS` | too many parameters |
| `E0308` | `XR_ERR_CMP_TOO_MANY_LOCALS` | too many locals |
| `E0309` | `XR_ERR_CMP_TOO_MANY_CONSTANTS` | too many constants |
| `E0310` | `XR_ERR_CMP_TOO_MANY_UPVALUES` | too many upvalues |
| `E0311` | `XR_ERR_CMP_JUMP_TOO_LARGE` | jump offset too large |
| `E0321` | `XR_ERR_TYPE_NOT_CALLABLE` | static type is not callable |
| `E0322` | `XR_ERR_TYPE_NOT_INDEXABLE` | static type is not indexable |
| `E0323` | `XR_ERR_TYPE_NOT_ITERABLE` | static type is not iterable |
| `E0324` | `XR_ERR_TYPE_INVALID_OPERAND` | invalid operand type |

#### Analyzer

| Code | C name | Meaning |
|--|--|--|
| `E0350` | `XR_ERR_ANALYZE` | generic analyzer error |
| `E0351` | `XR_ERR_ANALYZE_UNDEFINED_VAR` | undefined name |
| `E0352` | `XR_ERR_ANALYZE_TYPE_MISMATCH` | type mismatch |
| `E0353` | `XR_ERR_ANALYZE_CONST_ASSIGN` | analyzer-detected const assignment |
| `E0354` | `XR_ERR_ANALYZE_NOT_CALLABLE` | called value is not callable |
| `E0355` | `XR_ERR_ANALYZE_WRONG_ARG_COUNT` | wrong argument count |
| `E0356` | `XR_ERR_ANALYZE_ARG_TYPE` | wrong argument type |
| `E0357` | `XR_ERR_ANALYZE_GENERIC_COUNT` | wrong generic argument count |
| `E0358` | `XR_ERR_ANALYZE_GENERIC_CONSTRAINT` | generic constraint not satisfied |
| `E0359` | `XR_ERR_ANALYZE_SUPER_FIRST` | `super(...)` is not the first construction action |
| `E0360` | `XR_ERR_ANALYZE_SUPER_THIS` | `this` accessed before `super(...)` |
| `E0361` | `XR_ERR_ANALYZE_SUPER_REQUIRED` | derived constructor omits `super(...)` |
| `E0362` | `XR_ERR_ANALYZE_SUPER_INVALID` | invalid `super` in a non-derived class |
| `E0363` | `XR_ERR_ANALYZE_CLOSURE_CAPTURE` | unsafe closure capture |
| `E0364` | `XR_ERR_ANALYZE_AWAIT_TYPE` | invalid `await` operand type |
| `E0365` | `XR_ERR_ANALYZE_MISSING_TYPE` | no inferable type or initializer |
| `E0367` | `XR_ERR_ANALYZE_INTERFACE_NOT_IMPLEMENTED` | incomplete interface implementation |
| `E0368` | `XR_ERR_ANALYZE_TUPLE_FIELD_NAME` | invalid tuple field name |
| `E0369` | `XR_ERR_ANALYZE_TUPLE_FIELD_RANGE` | tuple field out of range |
| `E0370` | `XR_ERR_ANALYZE_THROW_NON_EXCEPTION` | `throw` operand is not an allowed enum error value |
| `E0371` | `XR_ERR_ANALYZE_MATCH_NOT_EXHAUSTIVE` | non-exhaustive `match` |
| `E0372` | `XR_ERR_ANALYZE_USED_BEFORE_ASSIGN` | use before assignment |
| `E0373` | `XR_ERR_ANALYZE_TUPLE_IMMUTABLE` | mutation of an immutable tuple |
| `E0374` | `XR_ERR_ANALYZE_OVERRIDE_MISMATCH` | override contract mismatch |
| `E0375` | `XR_ERR_ANALYZE_HASHABLE_CONTRACT` | Map/Set element lacks hash/equality contract |
| `E0376` | `XR_ERR_ANALYZE_CONDITION_TYPE` | invalid condition type |
| `E0377` | `XR_ERR_ANALYZE_VISIBILITY` | visibility violation |
| `E0378` | `XR_ERR_ANALYZE_CONST_FIELD` | mutation of a const field |
| `E0379` | `XR_ERR_ANALYZE_POSSIBLY_NULL` | unsafe use of a possibly-null value (trigger rules in §2.13 N-12) |
| `E0380` | `XR_ERR_ANALYZE_UNKNOWN_FIELD` | reading or setting a field / member the type does not declare |
| `E0381` | `XR_ERR_ANALYZE_MISSING_FIELD` | aggregate literal omits a required field |
| `E0382` | `XR_ERR_ANALYZE_BORROW_CONFLICT` | owner invalidated while a borrow (`Slice<T>` view, `ref`, or raw pointer) is live |
| `E0383` | `XR_ERR_ANALYZE_BORROW_ESCAPE` | a borrowed value escapes its owner's scope (return, field, container, closure capture, execution boundary) |
| `E0384` | `XR_ERR_ANALYZE_BORROW_SOURCE` | the borrow source is not a stable, uniquely inferable owner (temporary owner, multi-source return, borrowed from a local) |
| `E0385` | `XR_ERR_ANALYZE_GENERATOR_SUSPEND` | a generator body reaches a scheduler suspension point (`await` / `select` / `scope` / `Coro.yield()` / a blocking handle method / a suspending call), or the evidence is incomplete (a call through an unresolved function value); see §3.16.2 |
| `E0386` | `XR_ERR_ANALYZE_GENERATOR_DEFER` | `defer` inside a generator body; an abandoned generator is never resumed, so the cleanup may never run; see §3.16.3 |
| `E0387` | `XR_ERR_ANALYZE_DEFER_MAY_THROW` | the `defer` target throws (see §8.3.1) |
| `E0388` | `XR_ERR_ANALYZE_MONO_BUDGET` | the program exceeds the monomorphization instance budget (breadth); each instance clones a whole declaration; see §9.4 |
| `E0389` | `XR_ERR_ANALYZE_MONO_DEPTH` | monomorphization nested past the depth budget; polymorphic recursion (`f<T>` requesting `f<Box<T>>`) has no finite specialization and always reaches it; see §9.4 |
| `E0390` | `XR_ERR_ANALYZE_UNION_INDISCRIMINABLE` | union members are not discriminable at run time (two members of the same numeric family) |
| `E0391` | `XR_ERR_ANALYZE_MOVE_NOT_UNIQUE` | the uniqueness evidence `move` requires does not hold (live alias, published root, unknown provenance, incomplete storage plan) |
| `E0392` | `XR_ERR_ANALYZE_DEFER_SUSPEND` | a defer body reaches a scheduler suspension point; see §2.14.4 |

### 18.3 Runtime

| Code | C name | Meaning |
|--|--|--|
| `E0400` | `XR_ERR_RUNTIME` | generic runtime error |
| `E0401` | `XR_ERR_TYPE_NO_PROPERTY` | property absent on type |
| `E0402` | `XR_ERR_TYPE_NO_INDEX` | value is not indexable |
| `E0403` | `XR_ERR_TYPE_NO_CALL` | value is not callable |
| `E0404` | `XR_ERR_TYPE_MISMATCH` | runtime type mismatch |
| `E0405` | `XR_ERR_TYPE_NO_METHOD` | method absent on type |
| `E0406` | `XR_ERR_TYPE_NO_OPERATOR` | operator unsupported by type |

#### Null, Arithmetic, and Containers

| Code | C name | Meaning |
|--|--|--|
| `E0410` | `XR_ERR_NULL_PROPERTY` | property access on null |
| `E0411` | `XR_ERR_NULL_INDEX` | index on null |
| `E0412` | `XR_ERR_NULL_CALL` | call on null |
| `E0413` | `XR_ERR_NULL_UNWRAP` | force-unwrapping null |
| `E0420` | `XR_ERR_DIV_BY_ZERO` | division by zero |
| `E0421` | `XR_ERR_MOD_BY_ZERO` | modulo by zero |
| `E0422` | `XR_ERR_OVERFLOW` | arithmetic overflow or out-of-range numeric conversion (including NaN / infinity to integer) |
| `E0430` | `XR_ERR_INDEX_OUT_OF_BOUNDS` | index out of bounds |
| `E0431` | `XR_ERR_KEY_NOT_FOUND` | missing Map key |
| `E0432` | `XR_ERR_ITERATOR_EXHAUSTED` | `next()` / `nth()` on an exhausted `Iterator<T>`, violating the two-step pull protocol; see §5.3.6 |

#### System, Calls, Coroutines, and Stdlib

| Code | C name | Meaning |
|--|--|--|
| `E0440` | `XR_ERR_STACK_OVERFLOW` | stack overflow |
| `E0441` | `XR_ERR_OUT_OF_MEMORY` | out of memory |
| `E0442` | `XR_ERR_MATCH_FAILURE` | runtime match failure |
| `E0443` | `XR_ERR_DEFER_THROW` | an error escaped a `defer` body; uncatchable, terminates the process (see §8.3.1 rule D3) |
| `E0450` | `XR_ERR_WRONG_ARG_COUNT` | runtime argument-count mismatch |
| `E0451` | `XR_ERR_INVALID_ARG_TYPE` | runtime argument-type mismatch |
| `E0460` | `XR_ERR_CORO_DEAD` | operation on a dead coroutine |
| `E0461` | `XR_ERR_CORO_CANCELLED` | coroutine cancelled |
| `E0462` | `XR_ERR_TASK_ALREADY_TAKEN` | a Task's transferable result has already been taken and cannot be taken again |
| `E0470` | `XR_ERR_JSON_PARSE` | JSON parse failure |
| `E0471` | `XR_ERR_JSON_INVALID` | invalid JSON value or operation |
| `E0475` | `XR_ERR_REGEX_COMPILE` | regex compilation failure |
| `E0476` | `XR_ERR_REGEX_PATTERN` | invalid regex pattern |
| `E0480` | `XR_ERR_TLS_UNAVAILABLE` | TLS capability unavailable |

### 18.4 Modules, I/O, and Coroutines

| Code | C name | Meaning |
|--|--|--|
| `E0501` | `XR_ERR_MOD_NOT_FOUND` | module not found |
| `E0502` | `XR_ERR_MOD_LOAD_FAILED` | module load failed |
| `E0503` | `XR_ERR_MOD_NO_EXPORT` | name is not exported |
| `E0504` | `XR_ERR_MOD_CIRCULAR` | circular module dependency |
| `E0601` | `XR_ERR_IO_FILE_NOT_FOUND` | file not found |
| `E0602` | `XR_ERR_IO_READ_FAILED` | read failed |
| `E0603` | `XR_ERR_IO_WRITE_FAILED` | write failed |
| `E0604` | `XR_ERR_IO_PERMISSION_DENIED` | I/O permission denied |
| `E0701` | `XR_ERR_CORO_DEADLOCK` | coroutine deadlock |
| `E0702` | `XR_ERR_CORO_CHANNEL_CLOSED` | channel closed |
| `E0703` | `XR_ERR_CORO_LIMIT_EXCEEDED` | coroutine limit exceeded |

### 18.5 Syntax Guidance and Internal Errors

| Code | C name | Rejected form / meaning |
|--|--|--|
| `E0801` | `XR_ERR_SYN_RETURN_MULTI_REMOVED` | `return a, b` is invalid; return a tuple with `return (a, b)` |
| `E0802` | `XR_ERR_SYN_BINDING_CAPABILITY_REMOVED` | `owned` / `shared` binding syntax is invalid; use `var` or `const` — ownership and sharing are inferred from value capability and explicit copy/move boundaries |
| `E0803` | `XR_ERR_SYN_FOR_FLAT_REMOVED` | bare key/value `for` form is invalid |
| `E0804` | `XR_ERR_SYN_VOID_REMOVED` | `-> void` is invalid; use `-> ()` or omit the return type |
| `E0805` | `XR_ERR_SYN_PARAM_MODE_PREFIX_REMOVED` | parameter modes belong between the colon and the type |
| `E0806` | `XR_ERR_SYN_PARAM_MOVE_MODE_REMOVED` | `move` is an argument transfer expression, not a parameter mode |
| `E0807` | `XR_ERR_SYN_PARAM_MODE_COMBINED_REMOVED` | invalid combined parameter modes |
| `E0808` | `XR_ERR_SYN_PARAM_MODE_POSTFIX_REMOVED` | parameter modes cannot follow the type |
| `E0809` | `XR_ERR_SYN_CALL_IN_MARKER_REMOVED` | call-site `in` marker; ordinary `in` calls have no marker |
| `E0900` | `XR_ERR_INTERNAL` | internal error |
| `E0901` | `XR_ERR_NOT_IMPLEMENTED` | not implemented |
| `E0999` | `XR_ERR_UNKNOWN` | unknown error |

The runtime panic channel uses the prelude `PanicInfo`; user-level `throw <enum>` uses the value-return error channel. See §8 and §16 for propagation semantics; the numeric range alone does not determine the channel.

---

## Appendix A. EBNF Grammar

> Source of truth: `src/frontend/parser/xparse_*.c`. This appendix is a compact, curated EBNF; the parser implementation is the authoritative resolver of any conflicts.

### A.1 Lexical Layer

```ebnf
SourceFile ::= Statement*

Comment ::= '//' [^\n]*
         |  '/*' .* '*/'

Identifier ::= IdStart IdContinue*
IdStart    ::= 'a'..'z' | 'A'..'Z' | '_' | NonAsciiUtf8Byte
IdContinue ::= IdStart | '0'..'9'
// The source is first validated as UTF-8. The lexer accepts every non-ASCII UTF-8 byte in identifiers; it does not currently apply XID/NFC normalization.

IntLiteral   ::= DecimalInt | HexInt | BinInt | OctInt
DecimalInt   ::= DecimalDigit ('_'? DecimalDigit)*
HexInt       ::= '0x' HexDigit ('_'? HexDigit)*
BinInt       ::= '0b' ('0' | '1') ('_'? ('0' | '1'))*
OctInt       ::= '0o' ('0'..'7') ('_'? ('0'..'7'))*

FloatLiteral ::= DecimalInt '.' DecimalInt? Exponent?
              |  DecimalInt Exponent
Exponent     ::= ('e' | 'E') ('+' | '-')? DecimalDigit+

BigIntLiteral ::= (DecimalInt | HexInt | BinInt | OctInt) 'n'

QuotedLiteral ::= StringLiteral | FixedByteLiteral
StringLiteral ::= StringPrefix (InlineQuoted | BlockQuoted)
FixedByteLiteral ::= FixedBytePrefix (InlineQuoted | BlockQuoted)
StringPrefix ::= '' | 'r'
FixedBytePrefix ::= 'b' | 'br' | 'c' | 'cr'
InlineQuoted ::= '"' InlinePayload* '"' | '""'
BlockQuoted ::= QuoteRun ImmediateLineEnding BlockBody BlockClose
QuoteRun ::= '"'{Q}                         // Q >= 3
BlockClose ::= LineStart Indent SameQuoteRun (LineEnding | EOF)
CharLiteral ::= "'" CharBody "'"
CharBody ::= UnicodeScalar | EscapeSeq | '\u{' HexDigit{1,6} '}'
RegexLiteral ::= '/' RegexBody '/' RegexFlag*
RegexFlag ::= 'i' | 'm' | 's' | 'g' | 'u'

BoolLiteral ::= 'true' | 'false'
NullLiteral ::= 'null'
```

### A.2 Types

```ebnf
Type ::= UnionType
UnionType ::= IntersectionType ('|' IntersectionType)*
IntersectionType ::= NullableType
NullableType ::= PrimaryType '?'?
PrimaryType ::= ConstType | FFIPointerType | CFunctionType | NamedType | FunctionType | TupleType | ObjectType
ConstType ::= 'const' PrimaryType
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

### A.3 Expressions

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
RelationalExpr ::= RangeExpr ((('<' | '<=' | '>' | '>=') RangeExpr) | (('as' | 'is') Type))*
RangeExpr   ::= ShiftExpr (('..' | '..=') ShiftExpr)?
ShiftExpr   ::= AdditiveExpr (('<<' | '>>') AdditiveExpr)*
AdditiveExpr ::= MultiplicativeExpr (('+' | '-') MultiplicativeExpr)*
MultiplicativeExpr ::= UnaryExpr (('*' | '/' | '%') UnaryExpr)*
// range binds looser than every arithmetic operator, tighter than comparison, and is non-associative (a..b..c is a syntax error). A safe cast is `x as T?`, where T? is nullable.

UnaryExpr ::= ('-' | '+' | '!' | '~') UnaryExpr
           |  'move' UnaryExpr
           |  'await' ('all' | 'any' | 'anySuccess')? UnaryExpr
           |  GoExpr
           |  'unsafe' Block
           |  PostfixExpr

GoExpr    ::= 'linked'? 'go' GoOptions? CallExpr
GoOptions ::= '(' 'name' ':' StringLiteral ')'
CallExpr  ::= Primary PostfixOp* '(' ArgList? ')'

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
         |  QuotedLiteral | CharLiteral | RegexLiteral
         |  BoolLiteral | NullLiteral
         |  Identifier
         |  ArrayLit | MapLit | SetLit | ObjectLit
         |  BareLambda
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

BareLambda ::= Identifier '->' (Expression | Block)
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
          | '_'
          | Expression
```

### A.4 Patterns

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
EnumPattern     ::= QualifiedIdent VariantPayloadPattern?    // ADT enum payload destructuring
VariantPayloadPattern ::= '(' Pattern (',' Pattern)* ')'
TypePattern     ::= 'is' Type Identifier?
WildcardPattern ::= '_'
BindingPattern  ::= Identifier
MultiPattern    ::= Pattern (',' Pattern)+
```

### A.5 Statements

```ebnf
Statement ::= ExprStmt
           |  IncDecStmt
           |  VarDecl
           |  ConstDecl
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
           // Note: print/dump are calls inside ExprStmt; go is an expression (GoExpr)

// LineBreak is not a token: it is the outcome of deciding that a line ending
// terminates the statement here. That decision (previous token can end an
// expression + the new line's first token can begin one + not inside '(' / '[')
// is normative; see §1.2.1 for the full definition.
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

DeferStmt ::= 'defer' (CallExpr | Block)

// print is a normal global function call, syntactically an ExprStmt.

// go is an expression returning Task<T>. It is not a separate statement category (it appears wrapped in ExprStmt).

ScopeStmt ::= 'linked'? 'scope' Block

SelectStmt ::= 'select' '{' SelectArm+ '}'
SelectArm  ::= Identifier 'from' Expression '->' Block      // receive
            |  Expression 'to' Expression '->' Block        // send
            |  'after' Expression '->' Block                // timeout
            |  '_' '->' Block                                // default

YieldStmt ::= 'yield' Expression
```

### A.6 Declarations

```ebnf
Visibility ::= 'export'
VarDecl ::= 'var' Binding
ConstDecl ::= AttrList? Visibility? 'const' Binding
Binding ::= BindingPattern (':' Type)? ('=' Expression)?
BindingPattern ::= Identifier
                |  '[' BindingPattern (',' BindingPattern)* ','? ']'
                |  '(' BindingPattern (',' BindingPattern)+ ','? ')'
                |  '{' ObjectBinding (',' ObjectBinding)* ','? '}'
ObjectBinding ::= Identifier (':' Identifier)?

FnDecl ::= AttrList? Visibility? 'fn' Identifier TypeParams? '(' ParamList? ')' ReturnType? Block
ExternBlock ::= 'extern' '"C"' '{' ExternFnDecl+ '}'
ExternFnDecl ::= Visibility? 'fn' Identifier '(' ParamList? ')' ReturnType? ';'?
ParamList ::= Param (',' Param)* ','?
Param     ::= Identifier ':' ParamType ('=' Expression)?
           |  '...' Identifier ':' Type
ParamType ::= ParamMode? Type
ParamMode ::= 'ref' | 'move'
ReturnType ::= '->' Type | '->' '(' Type (',' Type)+ ')'
Modifier  ::= 'private' | 'protected' | 'static' | 'const'
              // public visibility is the default; final is only a class prefix

TypeParams ::= '<' TypeParam (',' TypeParam)* ','? '>'
TypeParam  ::= Identifier (':' Type ('&' Type)*)?         // constraints use ':', multiple use '&'
AliasTypeParams ::= '<' Identifier (',' Identifier)* ','? '>'

ClassDecl ::= AttrList? Visibility? 'final'? 'class' Identifier TypeParams?
              ('extends' NamedType)?
              ('implements' NamedType (',' NamedType)*)?
              '{' ClassMember* '}'
ClassMember ::= FieldDecl | MethodDecl | ConstructorDecl
FieldDecl ::= Modifier* Identifier ':' Type ('=' Expression)?
MethodDecl ::= Modifier* Identifier '(' ParamList? ')' ReturnType? Block
            |  Modifier* 'operator' OperatorToken '(' ParamList? ')' ReturnType? Block
ConstructorDecl ::= 'constructor' '(' ParamList? ')' Block

StructDecl ::= AttrList? Visibility? 'packed'? 'struct' Identifier TypeParams?
               ('implements' NamedType (',' NamedType)*)?
               '{' ClassMember* '}'

InterfaceDecl ::= Visibility? 'interface' Identifier TypeParams?
                  ('extends' NamedType (',' NamedType)*)?
                  '{' InterfaceMember* '}'
InterfaceMember ::= Identifier '(' ParamList? ')' ReturnType?

EnumDecl       ::= AttrList? Visibility? 'enum' Identifier TypeParams?
                   ('implements' NamedType (',' NamedType)*)?
                   '{' EnumVariant (',' EnumVariant)* ','? EnumMethod* '}'
EnumVariant    ::= Identifier VariantPayload?
EnumMethod     ::= 'fn' Identifier TypeParams? '(' ParamList? ')' ReturnType? Block
VariantPayload ::= '(' VariantField (',' VariantField)* ')'
VariantField   ::= (Identifier ':')? Type
BackingValue   ::= IntLiteral | FloatLiteral | StringLiteral | BoolLiteral

TypeAliasDecl ::= Visibility? 'type' Identifier AliasTypeParams? '=' Type

ImportDecl ::= 'import' ImportMembers 'from' ImportModule
            |  'import' ImportModule ('as' Identifier)?
ExportDecl ::= 'export' '{' ExportSpec (',' ExportSpec)* ','? '}' 'from' StringLiteral
            |  'export' '*' 'from' StringLiteral
ExportSpec ::= Identifier ('as' Identifier)?
ImportMembers ::= '{' ImportMember (',' ImportMember)* ','? '}'
ImportMember  ::= Identifier ('as' Identifier)?
ImportModule  ::= StringLiteral | Identifier ('/' Identifier)?

AttrList ::= PublicAttribute*
PublicAttribute ::= '@test' ('(' ('skip' | 'timeout' ':' IntegerLiteral) ')')?
                  | '@before_each' | '@after_each' | '@before_all' | '@after_all'
                  | '@deprecated' '(' StringLiteral ')'
                  | '@derive' '(' Identifier (',' Identifier)* ')'

OperatorToken ::= '+' | '-' | '*' | '/' | '%'
               |  '&' | '|' | '^'
               |  '==' | '!=' | '<' | '<=' | '>' | '>='
               |  '[]' | '[]='
               |  '!' | '~'
```

> Note: this EBNF is curated for guidance. Precedence, associativity, and disambiguation are determined by the parser implementation; in case of ambiguity, treat `src/frontend/parser/xparse_*.c` as authoritative.

---

## Appendix B. Keyword Index

These **66 keywords** correspond one-for-one with `src/frontend/lexer/xkeywords.def` and follow its ASCII lexical order. `move`, `ref`, `out`, `linked`, `from`, `to`, `after`, and `panic` are contextual words, not entries here; `parallel` is a standard-library module name.

| Keyword | Section |
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
| `f32` | §2.3.2 |
| `f64` | §2.3.2 |
| `fn` | §5.2 |
| `for` | §4.4 |
| `go` | §10.2 |
| `if` | §4.2 |
| `implements` | §5.5 |
| `import` | §11 |
| `in` | §4.4 |
| `int` | §2.3.1 |
| `i16` | §2.3.1 |
| `i32` | §2.3.1 |
| `i64` | §2.3.1 |
| `i8` | §2.3.1 |
| `interface` | §5.5 |
| `is` | §3.8 |
| `match` | §3.13 / §4.5 |
| `new` | §3.14 |
| `null` | §1.6.4 |
| `operator` | §5.3 |
| `packed` | §5.2.9 |
| `private` | §5.3 |
| `protected` | §5.3 |
| `return` | §4.7 |
| `rune` | §2.3.5 |
| `scope` | §10.7 |
| `select` | §10.6 |
| `static` | §5.3 |
| `string` | §2.3.4 |
| `struct` | §5.4 |
| `super` | §5.3 |
| `this` | §5.3 |
| `throw` | §8 |
| `true` | §1.6.4 |
| `try` | §8 |
| `type` | §5.7 |
| `u16` | §2.3.1 |
| `u32` | §2.3.1 |
| `u64` | §2.3.1 |
| `u8` | §2.3.1 |
| `union` | §5.2.9 |
| `unsafe` | §3.2 / §5.2 |
| `var` | §5.1 |
| `while` | §4.3 |
| `yield` | §3.16 |

---

## Appendix C. Operator Index

The complete operator listing organized by purpose is in [§1.7](#17-operators-and-tokens); detailed precedence is in [§3.1](#31-precedence-and-associativity).

| Group | Operators |
|--|--|
| Arithmetic | `+` `-` `*` `/` `%` |
| Bitwise | `&` `\|` `^` `~` `<<` `>>` |
| Comparison | `==` `!=` `<` `<=` `>` `>=` |
| Logical | `&&` `\|\|` `!` |
| Assignment | `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` |
| Statements | `++` `--` |
| Types, ranges, and spread | `?` `??` `?.` `?[` `!` `\|` `->` `...` `..` `..=` `is` `as` |

---

## Appendix D. Standard Library Module Index

The full set of 28 stdlib modules (native, pure Xray, or mixed) is documented in [§15](#15-standard-library-overview).

| Module | Purpose |
|--|--|
| `base64` | Base64 encode/decode |
| `cluster` | distributed cluster |
| `compress` | compression (gzip/zlib/deflate) |
| `crypto` | cryptographic hashes |
| `csv` | CSV parsing/serialization |
| `datetime` | date and time |
| `encoding` | character encoding conversion |
| `http` | HTTP/REST |
| `io` | file I/O |
| `log` | structured logging |
| `math` | math functions |
| `mem` | raw memory and managed Buffer |
| `net` | TCP/UDP/TLS |
| `os` | operating system |
| `parallel` | structured CPU parallelism |
| `path` | path manipulation |
| `regex` | regular expressions |
| `runtime` | runtime information and heap introspection |
| `strconv` | numeric string parsing |
| `sync` | coroutine synchronization primitives |
| `sys` | OS-thread and low-level synchronization surface |
| `text` | Unicode text transforms |
| `time` | time / timer / sleep |
| `toml` | TOML parsing |
| `url` | URL parsing/construction |
| `ws` | WebSocket |
| `xml` | XML parsing |
| `yaml` | YAML parsing |

---

## Appendix E. Differences from Other Languages

Xray draws inspiration from many existing languages but has notable differences worth highlighting.

### E.1 vs JavaScript / TypeScript

| Dimension | JS/TS | xray |
|--|--|--|
| Static typing | Optional in TS | **Mandatory** (`Json` is the only dynamic type) |
| Numerics | Single `number` (double) | `int`, `float`, `BigInt` strictly distinguished |
| Conditions | truthy / falsy | conditions must be `bool`, or nullable `T?` presence; int/string have no truthy conversion |
| Equality | `===` is strict, `==` is weak (string↔number coercion) | Only `==`/`!=`; value equality only promotes numeric int↔float, and `===`/`!==` are not operators |
| Closure capture | by reference | by reference (default); `go` closures are strictly restricted |
| Objects | dynamic fields | `{...}` creates a sealed Record by default; dynamic objects require an explicit `Json` boundary |
| import | ES Modules | xray-specific syntax (stdlib uses unquoted form) |
| Concurrency | async / Promise | coroutines + channels |

### E.2 vs Go

| Dimension | Go | xray |
|--|--|--|
| Type system | simple + implicit interfaces | richer + explicit `implements` |
| Error handling | multiple return values + `err != nil` | value-return error channel (`throw` / `catch`) + `T?` |
| Coroutines | `go func() {}` (statement) | `go expr` (expression returning `Task<T>`) |
| Awaiting | no direct equivalent (channels/WaitGroup) | `await t`, `await all [...]`, `await any [...]` |
| Channels | built-in `chan T`, `<-` operator | `Channel<T>` class with `send`/`recv`/`trySend`/`tryRecv` methods |
| `select` arms | `case x := <-ch:` / `case ch <- v:` / `default:` | `x from ch ->` / `v to ch ->` / `after ms ->` / `_ ->` |
| Memory management | concurrent tri-color tracing GC | coroutine-local reference counting, no cycle collector; cycles are handled by static proof, `weak`, and the coroutine-heap bound. Published const roots and synchronized handles use a verified shared domain |
| Classes / inheritance | none (struct + interface only) | classes with inheritance |
| Generics | since 1.18 | yes; monomorphized by concrete type or backend representation |

### E.3 vs Rust

| Dimension | Rust | xray |
|--|--|--|
| Memory safety | full borrow checker | inferred uniqueness, `move`, and const/synchronized capabilities across execution boundaries; borrowed views such as `Slice` have static lifetime restrictions (see §2.4.2) |
| Errors | `Result<T, E>` | value-return error channel (`throw` / `catch`) |
| Type inference | strong Hindley-Milner | bidirectional inference |
| Traits | full | similar to `interface`, fewer features |
| Performance | near C | bytecode VM, or native AOT through a C toolchain |
| Compile-time | macros / const | restricted `comptime` evaluator plus optimizer constant folding |

### E.4 vs Python

| Dimension | Python | xray |
|--|--|--|
| Typing | dynamic (optional hints) | static |
| GIL | yes | none (M:N coroutines) |
| Strings | unicode str | utf-8 string |
| Indentation | mandatory | free-form (`{}`) |
| Classes | dynamic attributes | static fields |
| Performance | CPython slow | bytecode VM; performance-sensitive programs may use `xray build --native` |

### E.5 vs Swift

| Dimension | Swift | xray |
|--|--|--|
| Optional `?` | yes | yes |
| `!` unwrap | yes | yes |
| Error handling | `try?` collapses to nil; `try!` aborts | `throw` / `catch` value-return channel; `T?` + `??` |
| struct vs class | value/reference | value/reference |
| Protocols | strong | `interface`, weaker |
| Concurrency | actors + async/await | coroutines + channels + `go`/`await all`/`scope` |

---

## Appendix F. Glossary

| Term | Definition |
|--|--|
| **AOT** | Ahead-of-Time compilation: Xi IR generates C and the selected C toolchain produces a native binary at build time |
| **AST** | Abstract Syntax Tree: intermediate representation produced by the parser |
| **Arena** | Bulk allocator: every allocation is freed together |
| **Array<byte>** | Byte buffer type (see §2.4.6) |
| **Slice<T>** | Borrowed view over contiguous element storage owned by another value; owns no data and is constrained by borrow lifetimes (see §2.4.2) |
| **borrow** | Access to another value's storage without taking ownership; `Slice<T>`, `ref` parameters, and `Ptr<T>` / `MutPtr<T>` are all borrows and share the `E0382` / `E0383` / `E0384` rules |
| **rune** | Primitive type for one Unicode scalar value; not numeric and not an alias of `u32` (see §2.3.5) |
| **Channel** | Typed inter-coroutine communication pipe (see §10.5) |
| **closure** | Function value that captures outer variables |
| **coroutine** | User-space, suspendable/resumable execution flow |
| **defer** | Deferred execution: runs before function exit (see §4.9) |
| **enum** | Enumeration type (see §5.6) |
| **GC** | Generic term for garbage collection; Xray has no tracing GC or cycle collector. Object death is driven only by reference counting, which makes its point exact; a cycle is not collected but ruled out statically, broken with a `weak` field, or bounded by its execution-local reclamation domain and disposed with the residual graph when the physical coroutine ends (§16.8) |
| **safepoint** | Safe location where the scheduler can observe preemption, cancellation, or suspension state |
| **goroutine** | Equivalent of an xray coroutine; launched by `go CallExpr`, with inline logic written as an immediately invoked lambda |
| **hoisting** | Implicit declaration of a name before its first use |
| **IC** | Inline Cache: optimization of property/method dispatch |
| **interface** | Interface type (see §5.5) |
| **JIT** | Just-In-Time compilation; Xray does not currently implement a JIT |
| **lvalue / rvalue** | Assignable left-hand-side value vs. value-only right-hand-side |
| **monomorphization** | Build-time specialization of generics into concrete type/representation versions; generic functions may share I64 / F64 / PTR / BOOL representation versions, while generic classes / structs are fully specialized by concrete type |
| **move** | Ownership transfer for an inferred-unique root with no live alias/loan (see §7.3) |
| **nullable** | A nullable type `T?` whose value may be `null` |
| **pattern** | A pattern used in `match` and destructuring (see §6) |
| **scope** | Lexical scope |
| **synchronized shared capability** | Internal compiler-granted capability for audited handles such as Channel/Atomic/Mutex; not a public storage modifier |
| **SSA** | Static Single Assignment: IR where each variable is assigned only once |
| **struct** | Value-type class (see §5.4) |
| **TCO** | Tail-Call Optimization |
| **trait** | Rust terminology; xray uses `interface` |
| **condition expression** | Control-flow condition: must be `bool` or nullable presence `T?` (`T != bool`); see §2.3.3 |
| **grapheme cluster** | User-perceived character that may contain multiple Unicode scalars; `len(string)` and rune iteration operate on Unicode scalars, not grapheme clusters |
| **union** | Union type `A \| B` |
| **Unicode scalar value** | Legal Unicode code point in `U+0000..U+10FFFF`, excluding the surrogate range `U+D800..U+DFFF` |
| **upvalue** | Outer variable captured by a closure |
| **VM** | Virtual Machine: xray bytecode VM |
| **write barrier** | Hook inserted by the GC on pointer updates |

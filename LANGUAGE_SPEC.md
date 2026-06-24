# Xray Language Reference

> Version: based on the `xray` source tree version v0.7.1 (audited on 2026-05-21).
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
| **Execution** | VM interpreter / JIT / AOT — all transparent to the developer; semantics are strictly identical across modes |
| **Error handling** | Value-return error channel (throw / try / catch + enum errors) + panic boundary (catch panic) + nullable types (T?) + `defer`-based resource management |
| **Metaprogramming** | Attributes (`@test` / `@native` / `@deprecated`) + runtime reflection (Reflect) + reified generics |
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

> Source of truth: `src/frontend/lexer/xlex.h` (token enum), `src/frontend/lexer/xkeywords.def` (keyword table, 63 entries), `src/frontend/lexer/xlex.c` (scanner implementation).

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

Xray has **63 reserved keywords** in total; the authoritative source-of-truth table is in `src/frontend/lexer/xkeywords.def`. Keywords are grouped by purpose:

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
| `new` | removed—kept as a keyword only for migration errors (construct with `T(...)`, see §3.14) |
| `this` `super` | self / parent reference |
| `constructor` | constructor |
| `static` `private` `protected` | class/member modifiers; public visibility is the default and has no `public` keyword |
| `const` | immutable field/binding modifier |
| `abstract` `final` `override` | class/method modifiers (`override` is **optional** — overriding a parent method does not require an explicit annotation; `final` only seals classes/methods) |
| `operator` | operator overloading |
| `is` `as` | runtime type check / cast |

#### 1.5.3 Error Handling

`try` `catch` `throw` `defer`

#### 1.5.4 Module System

`import` `export`

#### 1.5.5 Coroutines and Concurrency

`go` `await` `select` `defer` `scope` `unsafe`

#### 1.5.6 Type Names (reserved)

`int` `int8` `int16` `int32` `int64` `uint8` `uint16` `uint32` `uint64`
`float` `float32` `float64` `bool` `string` `char`

In type position, `unknown` is the built-in erased/unknown-value type name (for example, `TaskOutcome.Success(unknown)`); it is not a lexical keyword, and remains usable as an ordinary identifier in expression position.

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

Xray supports two flavors of string literals: **escaped** and **raw**. Strings use double quotes only; single quotes are reserved for `char` literals. Backtick strings are not part of the current grammar — the lexer rejects them.

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
- `${...}` is scanned in expression mode: braces are matched by depth, and nested strings / raw strings / char literals are skipped as a unit, so same-quote nesting is legal, for example `"${m["k"]}"` and `"${"a}b"}"`.

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
- `r'...'` has been removed; single quotes do not participate in raw strings.

```xray
r"C:\path\to\file"          // literal contains two backslashes
r"C:\Users\${USER}"         // backslash is not escaped, but ${USER} still interpolates
```

#### 1.6.6 `char` Literals

```ebnf
CharLiteral ::= "'" CharBody "'"
CharBody ::= UnicodeScalar | EscapeSeq | '\u{' HexDigit{1,6} '}'
```

- `'a'` has type `char` and represents one Unicode scalar value.
- The valid range is `U+0000..U+10FFFF`, excluding surrogates `U+D800..U+DFFF`.
- A literal must contain exactly one scalar; `''`, `'ab'`, `'🇨🇳'`, and `'é'` are compile errors.
- Escapes such as `'\n'`, `'\t'`, `'\r'`, `'\0'`, `'\''`, `'\\'`, and `'\u{1F600}'` are supported.
- Char literals do not support `${...}` interpolation.

```xray
let a: char = 'a'
let zh: char = '中'
let smile: char = '\u{1F600}'
```

##### Backtick strings (illegal)

The lexer explicitly rejects backtick strings. For templates, use plain double quotes plus `${...}`.

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

---

## 2. Type System

> Source of truth: `src/runtime/value/xtype.h` (`XrType` definition), `src/runtime/value/xtype.c`, `src/frontend/parser/xparse_type.c` (syntax), `src/frontend/analyzer/xtype_ref_resolve.c` (resolution), `stdlib/prelude/prelude_types.def` (built-in type table).

### 2.1 Overview

Xray is statically typed; every expression has a determined type at compile time. Core features of the type system:

1. **Type inference**: variable declarations rarely require type annotations; the analyzer infers from the initializer / context.
2. **Nullable separation**: `T` is never `null`; `T?` is sugar for `T | null`.
3. **Union types**: `A | B | ...` (up to 6 members).
4. **Reified generics**: generic type parameters are reflectable at runtime.
5. **Structural Json + Nominal class**: Json objects are field-structure compatible (duck typing); classes are nominally compatible.
6. **Runtime reflection**: `typeof` / `Reflect.*` APIs.

### 2.2 Type Categories

| Category | Examples |
|--|--|
| Primitive | `int`, `float`, `bool`, `string`, `char`, `()` (Unit, no return value) |
| Sized integers | `int8`, `int16`, `int32`, `int64`, `uint8`..`uint64` |
| Sized floats | `float32`, `float64` |
| Containers | `Array<T>`, `Map<K,V>`, `Set<T>`, `Channel<T>`, `Bytes` (equivalent to `Array<uint8>`) |
| Special | `Json`, `BigInt`, `Range`, `DateTime`, `Regex`, `StringBuilder`, `Logger`, `NetConn`, `NetListener` |
| Error-handling prelude | `Exception` (see §8) |
| Weak containers | `WeakMap`, `WeakSet` |
| Nullable | `T?` |
| Union | `A \| B \| ...` |
| Tuple | `(T1, T2, ...)` |
| Function | `fn(T1, T2) -> R` |
| FFI / C ABI | `RawPtr<T>`, `RawMut<T>`, `CFn<(T) -> R>`, `uintsize`, `intsize` |
| Class / Struct / Interface | user-defined (nominal) |
| Enum | user-defined (incl. ADT enum, see §5.6) |
| Type alias | `type Name = SomeType`, `type Name<T> = SomeType` |

### 2.3 Primitive Types

#### 2.3.1 Integer Types

| Type | Range | Alias |
|--|--|--|
| `int8` | `[-128, 127]` | — |
| `int16` | `[-32768, 32767]` | — |
| `int32` | `[-2³¹, 2³¹-1]` | — |
| `int64` | `[-2⁶³, 2⁶³-1]` | `int` (default integer type) |
| `uint8`..`uint64` | unsigned counterparts | — |

- Literals default to `int`; the type may be narrowed by context (e.g., assigned to an `int32` variable), but a direct literal must fit the target range (`let x: int8 = 200` is rejected at compile time).
- Arithmetic uses two's-complement wrap-around semantics (no debug/release distinction). Same-width narrow integer operations keep that width and wrap at that width (`uint8 + uint8 -> uint8`); mixed narrow widths collapse back to `int`; shift results take the left operand's width.
- Values with static type `uint8`..`uint64` are interpreted as unsigned by `print`, `string(x)`, template strings, string concatenation, and ordering comparisons; for example, a static `uint64` bit pattern of `0xffff_ffff_ffff_ffff` formats as `18446744073709551615` and compares greater than `0`.
- `int.checkedAdd` / `checkedSub` / `checkedMul` return `null` on overflow; `saturating*` clamps to the `int` boundary; `wrapping*` explicitly performs the default two's-complement wrap.
- Non-literal expressions written into narrow integer targets are narrowed with target-type wrap-around, so `let x: uint8 = 255 + 1` evaluates to `0`.
- After dynamic erasure, `XrValue` stores only the integer payload, not signedness or width. Across `any` / Json / dynamic-container boundaries, `uint64` values above the positive `int64` range are not guaranteed to keep unsigned formatting or ordering semantics. Keep the value statically typed as `uintN` when unsigned semantics are required.

#### 2.3.2 Floating-Point Types

| Type | Standard |
|--|--|
| `float32` | IEEE-754 single precision |
| `float64` | IEEE-754 double precision; alias of `float` |

Literals default to `float`.

#### 2.3.3 `bool`

`true` / `false`, a standalone type. **No implicit conversion** to/from numeric types (cannot write `let x: int = true` or `let b: bool = 1`).

**Condition expression rules** (`if` / `while` / `for` conditions / ternary `?:` / `match` guards):

| Condition type | Allowed | Meaning |
|---|---|---|
| `bool` | yes | direct boolean test |
| `T?` with `T != bool` | yes | null presence only (content emptiness is **not** checked) |
| `bool?` | compile error | tri-state ambiguity; write `flag == true` / `flag != null` / `flag ?? false` |
| `int` / `float` / `string` / `char` / collections / objects | compile error | use explicit comparisons such as `n != 0`, `!s.isEmpty()` |

Operands of `&&` / `||` / `!` must be `bool`; do not place `T?` directly into `&&` / `||`.

```xray
let ok = true
if (ok) { }

let user: User? = findUser()
if (user) {              // presence: null check only
    print(user.name)     // user is narrowed to User here
}

let flag: bool? = maybeFlag()
if (flag == true) { }    // OK
if (flag != null) { }    // OK
// if (flag) { }         // compile error: bare bool? cannot be a condition

let s = ""
if (!s.isEmpty()) { }    // OK
// if (s) { }            // compile error
```

#### 2.3.4 `string`

Immutable UTF-8 strings. `length` / `size`, indexing, and default iteration are expressed in Unicode scalar values: `s[i]` returns `char`, and slicing returns `string`. For the rich method set, see §14.5.

Internally uses ARC; runtime short strings are coroutine-local by default (lock-free allocation), while literals/symbols, explicit `intern()`, and map/set keys use the global intern pool, with strings promoted to shared on demand when crossing coroutine boundaries.

#### 2.3.5 `char`

`char` represents one Unicode scalar value (valid range `U+0000..U+10FFFF`, excluding the surrogate range `U+D800..U+DFFF`). It is an independent primitive type, **not** a numeric type and **not** an alias of `uint32`.

```xray
let a: char = 'a'
let zh = '中'
let smile = '\u{1F600}'
print(typeof(a))          // "char"
print(int(smile))         // 128512
```

- A char literal must contain exactly one Unicode scalar; empty literals, multi-scalar literals, and surrogate literals are compile errors.
- `char` does not participate in arithmetic, bitwise operations, or narrow-integer assignment: `'a' + 1` and `let n: uint32 = 'a'` are rejected by the analyzer.
- Explicit conversions: `int(c)` returns the scalar code point; `char(n)` constructs a char from an integer and validates that it is a legal scalar; `string(c)` / `c.toString()` returns a one-scalar string.
- Common methods are listed in §14.4.1.

#### 2.3.6 Unit `()` (no return value)

Xray uses the **0-tuple `()`** to represent "no return value" (the Unit type):

```xray
fn log(msg: string) -> () { print(msg) }   // explicit Unit return
fn ping() { print("pong") }                  // omitted return type = ()
let r: () = log("hi")                        // allowed; r is a Unit value
```

- A function omitting its return type is equivalent to `-> ()`.
- `void` is not a type name: `fn f() -> void` is rejected (`E0804`); use `-> ()` or omit the return type to indicate no return value.

#### 2.3.7 FFI Scalars and C ABI Boundary Types

Xray's C FFI uses explicit boundary types so ordinary xray objects are not implicitly interpreted as C data:

| Type | C ABI meaning | Notes |
|--|--|--|
| `uintsize` | `size_t` | `uint64` on the currently supported targets |
| `intsize` | `ptrdiff_t` / platform signed width | `int64` on the currently supported targets |
| `RawPtr<T>` | `const void *` boundary value | read-only raw pointer; `T` gives the xray-side dereference/index width |
| `RawMut<T>` | `void *` boundary value | mutable raw pointer; assignable where `RawPtr<T>` is expected |
| `CFn<(A, B) -> R>` | C ABI function pointer | passes an xray function as a C callback argument to an `@extern` function |

Raw pointer values may be stored, passed, compared, and offset with `offset(i)` using element-width scaling in safe code; actually reading or writing foreign memory must be inside `unsafe { }`:

```xray
@extern("C") fn malloc(n: uintsize) -> RawMut<uint8>
@extern("C") fn free(p: RawMut<uint8>)

let p = unsafe { malloc(4) }
unsafe {
    p[0] = 42
    print(p.deref())
    free(p)
}
```

`RawPtr<T>` is read-only; writes require `RawMut<T>`. `unsafe` does not bypass that type rule. Raw pointer access performs no null or bounds checks, so the caller must guarantee address validity, lifetime, alignment, and aliasing correctness.

`CFn<(...) -> ...>` is not an ordinary xray closure type. The current VM/AOT backends support passing module-level, noncapturing xray functions with an exact signature match to C; capturing closures, anonymous functions, and `@extern` functions themselves cannot be used as `CFn` callback arguments.

### 2.4 Composite Types

#### 2.4.1 `Array<T>`

Ordered mutable array. See §14.1.

```xray
let a: Array<int> = [1, 2, 3]
let b = [1, 2, 3]                // inferred as Array<int>
let c: Array<string> = []         // explicit empty array
```

The `T` in `Array<T>` must be determinable at compile time. An empty `[]` without a type annotation is a compile error: `Empty array '[]' requires a type annotation`.

`Array<char>` preserves the `char` element identity: reads return `char`, and writes accept only `char`. The implementation uses compact Unicode-scalar storage (`XR_ELEM_CHAR` / `uint32_t[]`) and does not degrade to `Array<uint32>`.

#### 2.4.2 `Map<K, V>`

Hash table that **preserves insertion order**. See §14.7.

**Map literals** must use the `#{ ... }` prefix with `:` separators (consistent with Json; disambiguated by the `#` prefix):

```xray
let m: Map<string, int> = #{"a": 1, "b": 2}
let m2 = #{"a": 1, "b": 2}
let empty = #{}                                     // empty Map

m["c"] = 3                                          // insert / update
let v = m["a"]                                      // lookup; returns null if absent
```

| Literal form | Type | Purpose |
|---|---|---|
| `{ key: value }` (no prefix) | `Json` / `Object` (structural) | see §2.4.6 |
| `#{ "k": v }` (`#` prefix + `:`) | `Map<K, V>` (hash table) | this section |
| `#{}` | `Map<K, V>` (empty) | explicit empty Map |
| `[]` | `Array<T>` | array |
| `#[]` | `Set<T>` | set |

`K` must satisfy `Hashable` (see §9.2): typically `int`, `float`, `string`, `bool`, `enum`, `BigInt`, or a custom type that provides `operator==(other: Self) -> bool` and `hash() -> int`. Generic key types must be explicitly constrained as `K: Hashable`.

#### 2.4.3 `Set<T>`

Deduplicated collection. See §14.4.

```xray
let s: Set<int> = #[1, 2, 3]
```

#### 2.4.4 `Channel<T>`

Inter-coroutine communication channel. **Must** be declared `const` (see §10.5).

```xray
const ch: Channel<int> = Channel<int>(10)
```

#### 2.4.5 `Bytes`

Typed byte buffer. Semantically equivalent to `Array<uint8>`, but stored as contiguous memory.

```xray
let buf = Bytes(1024)
let init = Bytes([72, 101, 108, 108, 111])
```

#### 2.4.6 `Json` and Object Literals

`Json` is xray's **dynamic structured data type** — it can hold any JSON-equivalent structure. See §14.10 and §2.10.

The key difference between an **object literal** `{ field: value, ... }` and a Map literal:

```xray
// Object/Json literal: identifier or string key + colon ':'
let data: Json = { name: "Alice", tags: ["a", "b"], age: 30 }
let user = { name: "Bob", age: 25 }       // default type is Json
data.name              // type: Json (field access returns Json)
data["name"]           // equivalent

// Field shorthand: when a field name matches a variable name
let name = "Alice"
let age = 30
let user = { name, age }                  // equivalent to { name: name, age: age }

// Map literal: `#{}` prefix + `:`
let m = #{"k1": 1, "k2": 2}           // type: Map<string, int>
```

**Comparison**:

| Form | Type | Notes |
|---|---|---|
| `{ name: "x", age: 1 }` | `Json` / `Object` | identifier or string key followed by `:` |
| `{ x: y }` (`x` is field name, `y` is variable) | `Json` / `Object` | shorthand `{ x }` equivalent to `{ x: x }`; bare key only |
| `#{"a": 1}` | `Map<K, V>` | `#` prefix disambiguates; separator `:` |
| `Point{x: 1.0, y: 2.0}` | `Point` (struct) | type name + `{...}` literal |

**Sealed object types**: once an object type is named via `type`, it becomes sealed — accessing or assigning an undeclared field is a compile error:

```xray
type User = { name: string, age: int }

let u: User = { name: "Alice", age: 30 }
print(u.name)         // OK
// u.extra = "x"      // compile error: sealed type User has no field 'extra'

// Without a type annotation, the literal is dynamic Json
let u2 = { name: "Alice", age: 30 }      // Json (dynamically extensible)
u2.extra = "x"        // OK (Json is dynamic)
```

#### 2.4.7 `BigInt`

Arbitrary-precision integer. See §14.8.

#### 2.4.8 `Range`

Produced by the `..` operator. See §3.12.

#### 2.4.9 `DateTime` / `Regex` / `StringBuilder`

See §14 for details.

#### 2.4.10 `WeakMap` / `WeakSet`

Keys of `WeakMap` and elements of `WeakSet` must be heap objects; weak references do not prevent GC reclamation. Weak collections do not provide long-lived traversal callbacks that would retain elements.

### 2.5 Nullable Types

`T?` is sugar for `T | null`.

```xray
let x: int? = null      // OK
let y: int? = 42        // OK
let z: int = null       // compile error: null is not int
```

**Nullable primitives are first-class**: `int?` / `float?` / `bool?` are ordinary `T?` types and arise naturally from generics and containers (e.g. `Map<string, bool>.get(k) -> bool?`, or `fn find<T>(...) -> T?` at `T = bool`). They carry `null` in the tagged representation, so a `null` value renders as `"null"` in `print` / `string()` / string concatenation (never as the raw payload `0`), identically in the VM and AOT.

> `bool?` is tri-state (`true` / `false` / `null`). It is legal but **cannot be used directly as a condition** (a bare `if (b)` where `b: bool?` is a compile error; see §5 / task 128); write `b == true` / `b != null` / `b ?? false`.

#### Unwrapping

```xray
// 1. Null coalescing
let v = x ?? 0

// 2. Optional chaining
let len = name?.length    // null if name is null

// 3. Force unwrap
let v: int = x!           // throws NullError at runtime if x is null

// 4. `is` check
if (x is int) {
    // In this branch x is narrowed to int
    print(x + 1)
}
```

### 2.6 Union Types

```xray
let v: int | string = 42
v = "hello"             // OK
```

Constraints:
- Up to **6 members** (checked at compile time; over the limit → error).
- Members must not be subtypes of each other (otherwise normalized).
- Working with a union value requires `match` or `is`-based narrowing:

```xray
let v: int | string = ...
match v {
    is int    -> print("int: ${v}"),
    is string -> print("str: ${v}"),
}
```

**Special cases**:
- `int | null` normalizes to `int?`.
- When `T?` appears in a union: `int? | string` is effectively `int | string | null`, normalized to `(int | string)?`.

### 2.7 Tuple Types

Xray's tuples are **first-class** — they may appear as any value, be stored as fields, and nest.

```xray
// Literals
let t = (1, 2, 3)                 // type inferred as (int, int, int)
let h = (10, "hi", true)          // heterogeneous tuple
let single = (99,)                // single-element tuple: note trailing comma

// Type annotation
let p: (int, string) = (7, "ok")

// Field access: .N (N is a compile-time constant integer index)
let first = t.0                   // 1
let mid   = t.1                   // 2
let nest  = ((1, 2), (3, 4))
let a     = nest.0.0              // 1
let b     = nest.1.1              // 4

// Function return and destructuring
fn divmod(a: int, b: int) -> (int, int) { return (a / b, a % b) }
let (q, r) = divmod(17, 5)        // tuple destructure

// Generic
fn pair<A, B>(a: A, b: B) -> (A, B) { return (a, b) }
let p2 = pair(1, "x")             // (int, string)
```

**Notes**:

- A **single-element tuple** must use a trailing comma `(x,)` — `(x)` without a comma is a grouping parenthesis (a plain expression).
- In field access `t.N`, N **must be an integer literal**; using a variable or string is the compile error `XR_ERR_ANALYZE_TUPLE_FIELD_NAME` / `_RANGE`.
- Tuples are **immutable**: `t.0 = v` is a compile error. To modify, build a new tuple.

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
let p: Pair<int> = { first: 1, second: 2 }  // equivalent to { first: int, second: int }
let f: Mapper2<int, string> = (n) -> string(n)
```

Generic alias parameters are a name list only (`<T, U>`); constraints are not
part of type-alias syntax. Put constraints on the generic function, class /
struct / enum / interface that uses the alias. Aliases may be forward
referenced, but cyclic aliases, including recursive object aliases, are compile
errors.

### 2.9 Type Inference

See §7.4 for details. In summary:

```xray
let x = 1               // x: int
let y = 1.5             // y: float
let z = "hello"         // z: string
let a = [1, 2, 3]       // a: Array<int>
let m = #{"a": 1}    // m: Map<string, int>
let p = { name: "A" }   // p: { name: string } — structured object type
let f = (x: int) -> x   // f: (int) -> int — arrow parameters require annotation
```

### 2.10 Type Compatibility and Conversion

#### 2.10.1 Implicit Conversion

| From | To | Allowed |
|--|--|--|
| `int` | `float` | ✅ |
| `int8` | `int` (= `int64`) | ✅ |
| `T` | `T?` | ✅ |
| `T` | `Json` (if T is Json-compatible) | ✅ |
| `null` | `T?` | ✅ |
| Subtype | Supertype (class) | ✅ |
| Subset object type | Superset object type | ❌ (structural compatibility goes superset → subset) |

> **Structural compatibility direction** (duck typing): a type with more fields is assignable to a type with fewer fields.
> ```xray
> type User = { name: string }
> let full = { name: "A", age: 18 }
> let u: User = full       // OK: full is a superset of User
> ```

#### 2.10.2 Explicit `as`

```xray
let n = x as int        // throws TypeError on failure
let n = x as int?       // returns null on failure (safe cast)
```

Applies to:
- Between numeric types (including `Json → int`, checked at runtime).
- `Json → User` (structural narrowing).
- Parent → child (downcast).

#### 2.10.3 `is` Check

```xray
if (v is User) {
    // In this branch the compiler narrows v's type to User
}
```

Acts only as a type guard; does not change the value.

### 2.11 typeof / typename / Type Enum

```xray
typeof(value)     // returns a Type enum value (an int representation)
typename(value)   // returns the type name as a string
```

`Type` enum members:

`Type.int`, `Type.float`, `Type.string`, `Type.bool`, `Type.null`,
`Type.Array`, `Type.Map`, `Type.Set`, `Type.Channel`, `Type.Json`,
`Type.function`, `Type.class`, `Type.struct`, `Type.enum`, `Type.module`, `Type.bigint`, ...

Full list: see `stdlib/types/enum.xr` / `src/runtime/value/xtype.h`.

### 2.12 Runtime Reflection

The `Reflect` module (built in):

```xray
Reflect.getType(obj)        // get type info (Json)
Reflect.typeOf(obj)         // get the type name (string)
Reflect.isInstance(obj, cls)// whether obj is an instance of cls
Reflect.fieldCount(obj)     // number of fields
Reflect.getAllTypes()       // all registered types
```

See §13 and §14 for more.

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

```xray
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

```xray
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

```xray
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

```xray
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

```xray
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

```xray
let m = #{"a": 1, "b": 2}
let empty = #{}                           // empty Map
```

**Key distinction**: `{}` is always a **Json / Object**; `#{}` is always a **Map**. Both use `:` between key and value; the `#` prefix is the disambiguator.

#### Set `#[...]`

```ebnf
SetLit ::= '#[' (Expr (',' Expr)* ','?)? ']'
```

```xray
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

```xray
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

```xray
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

```xray
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

```xray
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

```xray
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
- **Exhaustiveness**: for enum scrutinees (ADT and simple enums), the compiler enforces exhaustiveness. Otherwise it is not enforced, and an unmatched value at runtime throws `Exception(E0442)`.
- For pattern details see [§6](#6-patterns).

### 3.14 Construction expressions

```ebnf
ConstructExpr ::= Identifier TypeArgs? '(' ArgList? ')'
```

Construction looks just like a function call: `TypeName(args)`. There is no `new` keyword—writing `new` (e.g. `new Point(...)`) is a compile error that tells you to delete it.

```xray
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
```xray
let a = [1, 2, 3]              // equivalent to Array<int>() + push
let m = #{}                    // equivalent to Map<...>()
let p = Point{x: 1, y: 2}      // struct literal
```

### 3.15 String Interpolation

See §1.6.5. In brief:

```xray
"Hello, ${name}! Age: ${user.age + 1}"
```

- `${...}` accepts any expression (calls, object access, arithmetic).
- Embedded string literals inside `${...}` may use the same quote as the outer template; the lexer matches expression braces by depth and skips nested strings / raw strings / char literals.
- The expression's type must be convertible to a string (implement `toString()` or be a primitive).

### 3.16 `yield` Statement

```xray
yield                       // yield execution
```

**Current implementation**: only the value-less statement form is supported, letting the coroutine relinquish the CPU (analogous to Go's `runtime.Gosched()`).

See §10.10.

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
    let y = 2
    y + 1              // expression with discarded result
}
```

`++` / `--` are pure statements or `for` step items, and can only be written as `name++` / `name--`. They are equivalent to `name = name + 1` / `name = name - 1` and produce no value; expression-position uses such as `let y = x++`, `f(x++)`, `a[i++]`, and `return x++` are compile errors.

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
let i = 0
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
for (let i = 0; i < 10; i++) {
    print(i)
}
for (let j = 100; j > 90; j--) {
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

```xray
for (item in [1, 2, 3]) { print(item) }
for (i in 0..n) { print(i) }                  // range iteration (half-open)
for (ch in "hello") { print(ch) }             // string characters (by Unicode scalar)
for (key in someMap) { print(key) }           // single variable over Map → key
for (key in someJson) { print(key) }          // single variable over Json → key
for (day in Color) { print(day.name) }        // enum iteration (declaration order)
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
for (i, c in "hello") { print("${i}:${c}") }     // string → (index, char)

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
| `string` | `char` | (index, char) |
| `Range` (`a..b`) | int | — |
| Enum type | EnumValue | — |
| Custom `Iterator<T>` | T | — |

#### Custom iterators

Implement an `iterator()` method that returns an `Iterator<T>` protocol object (with `hasNext()` and `next()`) and the value becomes usable in `for-in`. See §14.15.

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
- The `throw` operand is an error value (typically an enum) propagated through the value-return channel: no `Exception` allocation, no stack unwinding, and only a predictable branch at call boundaries that may propagate or catch errors.
- There is no `finally`: use `defer` (§4.9) for deterministic cleanup.
- For full error semantics see [§8](#8-error-handling).

### 4.9 `defer`

```ebnf
DeferStmt ::= 'defer' (Expression | Block)
```

```xray
fn read_file(path: string) -> string {
    let f = open(path)
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
```

**Semantics**:
- A `defer` belongs to the nearest enclosing real block `{ ... }`. A function body is a block, so a top-level function-body `defer` still runs before the function exits.
- **LIFO**: multiple `defer` statements in the same block run in reverse declaration order.
- **Always executes**: runs when the owning block falls through or exits by `break`, `continue`, `return`, value-error propagation, or panic unwinding.
- A `defer` inside a loop body runs at the end of each iteration, not at the end of the function.
- `defer` is Xray's only deterministic-cleanup mechanism (replacing other languages' `finally`): it is bound to lexical block exits, not to a single function tail.
- An error thrown inside a `defer` body **replaces** any in-flight error (Go-style semantics).

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

---

## 5. Declarations

> Source of truth: `src/frontend/parser/xparse_decl.c`, `src/frontend/parser/xast_nodes_decl.h`, `src/frontend/analyzer/xanalyzer_visitor.c`.

### 5.1 `let` / `const` / `shared`

```ebnf
VarDecl ::= ('let' | 'const' | 'shared' ('const' | 'let')) Binding (',' Binding)*
Binding ::= Pattern (':' Type)? ('=' Expression)?
Pattern ::= Identifier
         | '[' BindingPattern (',' BindingPattern)* ','? ']'    // array destructure
         | '(' BindingPattern (',' BindingPattern)+ ','? ')'    // tuple destructure
         | '{' ObjectBinding (',' ObjectBinding)* ','? '}'      // object destructure
ObjectBinding ::= Identifier (':' Identifier)?
```

#### 5.1.1 `let` — mutable binding

```xray
let x = 1                         // type inferred as int
let name: string = "Alice"        // explicit type
let count: int                    // no initializer: zero value used
let maybeName: string?            // OK: defaults to null
let empty: string = ""            // string requires an explicit initializer
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

#### 5.1.3 `shared const` — cross-coroutine immutable shared

```xray
shared const CONFIG = { host: "localhost", port: 8080 }
shared const PRIMES = [2, 3, 5, 7, 11]
```

- Stored on the **global heap**, refcount-managed.
- Read-only **zero-copy** access across coroutines.
- The **only** kind of variable outside the local mutable scope that a `go` closure may legally capture (everything else must be passed explicitly or `move`d).

#### 5.1.4 `shared let` — cross-coroutine mutable, exclusive

```xray
shared let buffer = Bytes(1024)
```

- **Move semantics**: ownership must be transferred explicitly with `move`.
- Cannot be captured by a `go` closure (must be `move`d).
- Use after `move` is a compile error.

See [§10.11](#1011-concurrency-safety-model).

#### 5.1.5 Destructuring bindings

```xray
// array destructuring
let [a, b, c] = [1, 2, 3]
let [first, , third] = [10, 20, 30]         // skip elements

// tuple destructuring (multi-return)
let (q, r) = divmod(17, 5)

// object destructuring (extract by field name; local binding may be renamed)
let { name, age } = { name: "Alice", age: 30 }
let { name: localName, age } = { name: "Alice", age: 30 }
```

Constraints:
- The number of destructured bindings must match (except with rest patterns).
- Object destructuring field names must be `Identifier`s; `field: localName` changes only the local binding name, not the field being read.

### 5.2 `fn` function declaration

```ebnf
FnDecl ::= AttrList? Modifier* 'fn' Identifier TypeParams? '(' ParamList? ')' ReturnType? FnBody
ParamList ::= Param (',' Param)*
Param     ::= Modifier* Identifier ':' Type ('=' DefaultValue)?
            | '...' Identifier ':' Type
Modifier  ::= 'in' | 'ref'
ReturnType ::= '->' Type
            |  '->' '(' Type (',' Type)+ ')'   // tuple return
FnBody ::= Block
         | <empty>                              // only @extern functions may omit a body
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

let (q, r) = divmod(17, 5)
let result = divmod(10, 3)        // result has type (int, int)
```

**Constraints**:
- The return type wraps the tuple in parentheses: `(int, bool)`.
- A single return value omits the parentheses: `: int`.
- `return (a, b)` requires the parentheses; bare comma `return a, b` is a compile error (`E0801`).

#### 5.2.4 Parameter modifiers

Apply only to **`struct` value-type parameters**.

```xray
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

| Modifier | Semantics |
|--|--|
| (none) | Pass by value (struct copy) |
| `in` | Pass by read-only reference (no copy, not writable) |
| `ref` | Pass by mutable reference (no copy, writable, observable to caller) |

#### 5.2.5 Rest parameters

```xray
fn sum(...nums: int) -> int {
    let total = 0
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
- `let f = (x: int) -> x` (an arrow function bound to a variable) is **not** hoisted.

#### 5.2.7 Tail-call optimization

The compiler recognises accumulator-style tail recursion and rewrites it into a loop (avoiding stack overflow). See [§17](#17-compilation-pipeline).

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
- Multi-file projects specify the entry via the `entry` field of `xray.toml`; the corresponding file follows the script execution rules above.

#### 5.2.9 `@extern` C FFI Functions

`@extern("C")` declares an external C ABI function. An external function has no xray function body, and call sites must be written explicitly inside `unsafe { }`:

```xray
@extern("C") fn malloc(n: uintsize) -> RawMut<uint8>
@extern("C") fn free(p: RawMut<uint8>)
@extern("C") @dylib("m") fn cos(x: float64) -> float64

let p = unsafe { malloc(4) }
unsafe {
    p[0] = 42
    print(cos(0.0))
    free(p)
}
```

Rules:
- `@extern("C")` currently denotes the default C ABI; omitting the string is also treated as C ABI.
- `@dylib("name")` selects the dynamic library that provides the symbol; without it, resolution uses the default process/system lookup path.
- An `@extern` function may only declare its signature and cannot have a `{ }` body; every non-`@extern` function must have a block body.
- Boundary types that are aligned across the VM/AOT backends include `bool`, sized integers, `float32` / `float64`, `uintsize` / `intsize`, `RawPtr<T>`, `RawMut<T>`, and `()` returns.
- C callback parameters must use `CFn<(A, B) -> R>`, not the ordinary xray function type `(A, B) -> R`.
- A current `CFn` argument must be a module-level, noncapturing xray function with an exact signature match; anonymous functions, capturing closures, and `@extern` functions themselves are rejected.

```xray
@extern("C") fn bsearch(
    key: RawPtr<uint8>,
    base: RawPtr<uint8>,
    count: uintsize,
    size: uintsize,
    cmp: CFn<(RawPtr<uint8>, RawPtr<uint8>) -> int32>
) -> RawPtr<uint8>

fn zeroCmp(a: RawPtr<uint8>, b: RawPtr<uint8>) -> int32 {
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
- A `@c_export` function must have an xray function body and cannot also be an `@extern` function.
- The string argument must be a non-empty C identifier; that string is the exported C symbol name.
- Each `@c_export` symbol name must be unique within one AOT bundle; duplicate symbols are compile errors.
- Currently supported export boundary types are `bool`, sized integers, `float32` / `float64`, `uintsize` / `intsize`, `RawPtr<T>`, `RawMut<T>`, and `()` returns.
- Managed xray values such as `string`, class instances, Array/Map/Set, ordinary closures, and by-value aggregates are not exported directly today. To share struct memory with C, pass an address through `RawPtr<T>` / `RawMut<T>`.
- `@c_export` defines the function ABI wrapper; `xray build --native --c-header FILE` can emit a C prototype header for those wrappers, and `xray build --native --shared --c-header FILE` can emit a native shared library with a matching header.
- `--shared` currently supports only scalar / raw pointer exports that do not require Xray runtime initialization; runtime-backed features, managed ownership, aggregate by-value, and initialization/shutdown policy remain future FFI work.

### 5.3 `class` declaration

```ebnf
ClassDecl ::= Modifier* 'class' Identifier TypeParams?
              ('extends' Identifier TypeArgs?)?
              ('implements' Identifier TypeArgs? (',' Identifier TypeArgs?)*)?
              '{' ClassMember* '}'
ClassMember ::= FieldDecl | MethodDecl | ConstructorDecl | StaticBlock
FieldDecl ::= Modifier* Identifier ':' Type ('=' Expression)?
MethodDecl ::= Modifier* Identifier '(' ParamList? ')' ReturnType? Block
            |  Modifier* 'operator' OpToken '(' ParamList? ')' ReturnType? Block
ConstructorDecl ::= 'constructor' '(' ParamList? ')' Block          // parameter types may be omitted
Modifier ::= 'private' | 'protected' | 'static' | 'final' | 'const' | 'abstract' | 'override'
```

> **About default public visibility and `override`**:
>
> - Public is the **default visibility**—every field/method without `private` / `protected` is public; the language has no `public` modifier.
> - Visibility is **compile-time enforced**: accessing a `private` / `protected` member from outside the class, or a `protected` member from a non-subclass, reports `E0377`.
> - `override` is **optional**—an override happens automatically when the derived class declares a method with the same signature; an explicit `override` annotation is not required.
> - Once written, `override` is checked: the analyzer must find a same-name, same-signature instance method in the parent chain, or report compile error `E0374`.
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

let a = Animal("Rex")
print(a.speak())
print(Animal.create("Bob").name)
```

#### 5.3.2 Inheritance

```xray
class Dog extends Animal {
    constructor(name: string) {
        super(name)                    // **must** be the first statement (derived classes only)
    }

    override speak() -> string {         // override is optional but recommended
        return "woof"
    }
}
```

**Constraints**:
- A derived class constructor's **first statement** must be `super(...)` (unless no constructor is declared); otherwise it is a compile error.
- `this` must not be accessed before `super(...)`.
- **Overriding requires no keyword**—any subclass method with the same name and signature automatically overrides the parent (the `override` modifier is **optional**, but when present it must pass parent-chain signature checking).
- A `final class` cannot be inherited.
- A `final` method cannot be overridden.
- An `abstract` method **must** be implemented by subclasses (unless the subclass is also `abstract`).
- `super.method()` invokes the shadowed parent method from inside an override.

#### 5.3.3 Modifiers

| Modifier | Applies to | Semantics |
|--|--|--|
| (none) | field/method | Default public—externally visible |
| `private` | field/method | Accessible only inside the declaring class (including other instances of the same class); subclass and external access report `E0377` |
| `protected` | field/method | Accessible inside the declaring class and its subclasses; external access reports `E0377` |
| `static` | field/method | Class-level, not part of an instance; called as `ClassName.method()` |
| `const` | field | Immutable field—assignable once via `this` in the declaring class's constructor; later writes report `E0378` |
| `final` | class/method | Class: cannot be inherited. Method: cannot be overridden. **No longer applies to fields** (use `const` for immutable fields) |
| `abstract` | class/method | Cannot be instantiated / must be implemented by subclasses |
| `override` | method | **Optional but checked**—overrides do not require explicit annotation; when present, it must match a same-name, same-signature instance method in the parent chain |

**Modifiers may combine**: `private const secret: string = "key123"`, `static final pi() -> float`, `protected static counter: int = 0`.

> `const` = immutable field/binding, `final` = sealing a class/method (inheritance dimension). Their roles are separate: immutable fields use `const` only; writing `final` on a field is an error that suggests `const`.

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
let p = Point()                  // default-construct (zero-valued fields), then assign
p.x = 3.0
p.y = 4.0

let q = Point{x: 3.0, y: 4.0}        // struct literal: TypeName + { field: value }
let pt = Point{x: 1.0, y: 2.0}

// Value semantics: assignment and parameter passing copy
let b = q                            // b is an independent copy of q
b.x = 99.0
// q.x is still 3.0
```

**Differences from `class`**:

| Dimension | `class` | `struct` |
|--|--|--|
| Memory model | Reference type (heap) | Value type (stack or inlined) |
| Assign / pass | Shared reference | **Copy** (`let b = a` produces an independent copy) |
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

### 5.6 `enum` declaration

xray's `enum` is an **algebraic data type (ADT)**: each variant may be a payload-free tag (C-style enum) or carry typed payload data (ADT-style). Both styles can be mixed in the same enum.

```ebnf
EnumDecl       ::= 'enum' Identifier TypeParams?
                   ('implements' NamedType (',' NamedType)*)?
                   '{' EnumVariant (',' EnumVariant)* ','? EnumMethod* '}'
EnumVariant    ::= Identifier VariantPayload?
                |  Identifier '=' BackingValue                // explicit backing value for simple enums
EnumMethod     ::= 'static'? 'fn' Identifier TypeParams? '(' ParamList? ')' ReturnType? Block
VariantPayload ::= '(' VariantField (',' VariantField)* ')'
VariantField   ::= (Identifier ':')? Type
BackingValue   ::= IntLiteral | FloatLiteral | StringLiteral | BoolLiteral
```

> Variant declarations come first (comma-separated); method declarations follow all variants (no commas, separated by block boundaries — same convention as `class` member methods). See §5.6.7.

#### 5.6.1 Simple enums (no payload)

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

All members of a simple enum must use the same backing type (all `int`, all `float`, all `string`, or all `bool`). Mixed types are a compile error `XR_ERR_ANALYZE_ENUM_MIXED_TYPE`.

#### 5.6.2 ADT enums (with payload)

A variant name may be followed by parentheses declaring payload fields (positional or named):

```xray
// positional payload
enum Option<T> {
    Some(T),
    None,
}

// named-field payload (recommended for readability)
enum NetEvent {
    Connected,
    Disconnected(reason: string),
    DataReceived(bytes: Bytes),
    Error(code: int, message: string),
}

// state machine
enum ConnState {
    Idle,
    Connecting(retry: int),
    Connected(peer: string, since: int),
    Failed(reason: string),
}

// AST nodes
enum Expr {
    Number(int),
    Binary(op: string, left: Expr, right: Expr),
    Call(name: string, args: Array<Expr>),
}
```

**ADT vs. simple enums**:

| Feature | Simple enum | ADT enum |
|------|--|--|
| Carries data | ❌ | ✅ Each variant has its own field set |
| `.value` / `.ordinal` | ✅ | Available only on payload-free variants |
| Backing value (`= 200`) | ✅ | ❌ Cannot coexist with payloads |
| Generics | ❌ | ✅ `enum Option<T> { ... }` |
| `match` destructuring | By value only | By variant + payload destructuring |
| `for-in` iteration | ✅ In declaration order | ❌ Meaningless when payloads are present |
| Memory layout | Integer/string value | tag + payload |

Mixing: a single enum may contain both payload-free and payload-bearing variants (see `NetEvent` / `ConnState` above).

#### 5.6.3 Construction and destructuring

Construction:

```xray
let c = Color.Red                                   // simple
let r1 = Option.Some(42)                            // positional payload
let e1 = NetEvent.DataReceived(bytes: b)            // named payload, field name allowed
let e2 = NetEvent.Error(404, "not found")           // field name omitted, positional
let e3 = NetEvent.Connected                         // payload-free variant: no parentheses
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

#### 5.6.4 Member API for simple enums

Applies only to **payload-free** variants (including pure-tag variants inside an ADT enum).

Instance properties (act on the enum value):

```xray
Color.Red.name        // "Red"          variant name (string)
Color.Red.value       // 0              backing value
Color.Red.ordinal     // 0              declaration index (int, zero-based)
Color.Red.toString()  // "Color.Red"    "<EnumName>.<VariantName>" format
```

Class statics:

```xray
Color.memberCount     // 3              count of simple variants (int)
Color.getMember(0)    // Color.Red      lookup by ordinal
```

ADT variants with payloads do **not** support `.value` / `.ordinal` / `getMember`, but `.name` and `toString()` are still available (the latter includes a payload summary, e.g. `Option.Some(42)`).

#### 5.6.5 Iteration

Simple enums can be iterated with `for-in` in declaration order:

```xray
for (c in Color) { print(c.name) }        // "Red" "Green" "Blue"
```

ADT enums with payloads do **not** support direct `for-in`—iterating "all possible values" is meaningless (`Option<int>` has infinitely many).

#### 5.6.6 Reverse lookup (value to member)

Simple integer enums benefit from reverse-lookup optimization (Tier 1/2 contiguous/sparse; other types fall back to a linear scan). ADT variants do not support reverse lookup.

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
type Mapper = fn(int) -> int                         // function-type alias
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
export publicFn, VERSION                    // post-export of already-declared identifiers
export { name1, name2 as alias } from "./other"
export * from "./other"
```

**xray does not support** the JavaScript default-import form `import name from "module"`. Use `import "module" as name`, `import module`, or `import { name } from module`.

For full rules, path resolution, and visibility details see [§11 Modules](#11-modules).

---

## 6. Patterns

> Source of truth: `src/frontend/parser/xparse_match.c`, `src/runtime/value/x_value_match.c`.

Patterns appear in `match` expressions/statements and in `let` destructuring.

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
    DataReceived(bytes: Bytes),
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
- Usable in destructuring to skip positions: `let [_, b, _] = arr`.

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
let [a, b, c] = some_array
let (q, r) = divmod(17, 5)
let { name, age } = user
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
- Other operand types are not enforced; if no arm matches at runtime, an `Exception` with code `E0442` is raised (see §18.x).
- Always providing a `_` fallback is recommended.

---

## 7. Scoping and Name Resolution

> Source of truth: `src/frontend/analyzer/xanalyzer_scope.c`, `src/frontend/analyzer/xanalyzer_capture.c`.

### 7.1 Lexical Scoping and Hoisting

Xray uses **lexical scoping**: a name's visibility is determined entirely by the source code structure.

**Scope kinds**:

| Scope | Triggered by | Example |
|--|--|--|
| Module | Each `.xr` file | top-level `let` `fn` `class` |
| Function / closure | Entering `fn` / arrow function | parameters + function body |
| Block | `{...}` | `if` `while` `for` `match` arm body |
| `scope` block | `scope { ... }` keyword | explicit lexical scope + structured concurrency (see §10.7) |
| `for` header | `for (let i=0; ...)` | `i` is visible only within the loop body |
| `catch` parameter | `catch (e)` | `e` is visible only within the catch body |
| Class body | `class` definition | fields, methods |

**Hoisting rules**:

- Top-level `fn` `class` `struct` `interface` `enum` `type` are **hoisted** to the top of the current scope — they may be referenced before their textual definition.
- `let` / `const` are **not hoisted** — they must appear before any use.
- Duplicate names: declaring two same-named variables in the same scope is a compile error (nested scopes may shadow).

```xray
main()                    // OK: uses the hoisted fn
fn main() { ... }

let y = x                 // error: x is not declared
let x = 10
```

#### Shadow rules

A nested block may shadow a same-named variable in an outer scope:

```xray
let x = 1
{
    let x = "hello"           // shadow: OK
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
    let count = 0
    return fn() -> int {
        count += 1                  // mutates the outer count
        return count
    }
}

let c = make_counter()
print(c())      // 1
print(c())      // 2
```

- The closure and the original variable **share state**.
- After the outer scope exits, variables referenced by the closure are kept alive by the GC (promoted to the heap).

#### Closure optimization

The compiler analyzes upvalues:
- read-only → may be implicitly copied (avoiding closure conversion).
- read/write → promoted to a closure box.
- See §17.5 for details.

### 7.3 Ownership and `move`

Xray is **not** a full ownership/borrow-checked language (unlike Rust). However, **cross-coroutine data transfer** uses move semantics:

```xray
shared let big_buffer = Bytes(1024 * 1024)

let t = go fn(b: Bytes) -> int {
    return process(b)
}(big_buffer)             // compile error: shared let cannot be passed directly, must move

let t2 = go fn(b: Bytes) -> int {
    return process(b)
}(move big_buffer)        // OK: ownership transferred

print(big_buffer.length)  // compile error: accessed after move
```

**`move` usage**: `move` appears as an **argument prefix** at call sites (see §10.8):

- `go f(move x)`, `go fn(...){...}(move x)`: transfer ownership to the coroutine.
- `ch.send(move data)`: transfer ownership when sending across coroutines (avoiding a copy).
- Plain function call `f(move x)`: transfer ownership into the function (which becomes the sole owner).

### 7.4 Cross-Coroutine Data Transfer Rules (Race Avoidance)

"Statically eliminating data races at compile time" is a core design principle of xray's concurrency model.

A coroutine launched by `go` **cannot directly capture** mutable variables from the outer scope; data must enter the coroutine through **parameter passing**. Plain variables are deep-copied automatically; `shared` variables follow the rules below:

| Variable kind | Cross-coroutine transfer rule |
|---|---|
| Plain `let` / `const` (local) | **Deep-copied** automatically when passed as an argument; cannot be captured and mutated by closures |
| Function parameters | ✅ Fully free (already copied / moved in) |
| `shared const` | ✅ Zero-copy read-only sharing across coroutines (capturable by closures) |
| `shared let` | ⚠️ Must transfer ownership with a `move` argument prefix; the original variable becomes inaccessible after the move |
| `Channel<T>` | ✅ May be captured by closures (lifetime managed by the channel itself) |
| `this` / mutable closure upvalues | ❌ Cannot cross coroutines; must be passed explicitly through parameters |
| Globally imported functions/classes | ✅ Immutable definitions, freely referenceable |

```xray
let local = 0
go { local += 1 }                        // ❌ compile error: cannot capture mutable local
```

#### Recommended patterns

```xray
// Pattern 1: pass by value (plain variables are deep-copied)
let arr = [1, 2, 3]
let t = go fn(data: Array<int>) -> int {
    data.push(4)            // mutates the copy, original is unaffected
    return data.length
}(arr)
print(arr)                  // [1, 2, 3] unchanged

// Pattern 2: shared const, zero-copy read-only (capturable)
shared const config = { rate: 100 }
let t2 = go fn(c: Json) -> int {
    return c.rate
}(config)

// Pattern 3: move ownership
shared let big = Bytes(1024)
let t3 = go fn(b: Bytes) -> int {
    return process(b)
}(move big)
// big is inaccessible from this point

// Pattern 4: Channel communication (capturable)
shared const ch = Channel<int>(10)
let t4 = go fn(c: Channel<int>) -> int {
    return match (c.recv()) {
        Recv.Value(v) -> v
        _ -> 0
    }
}(ch)
ch.send(42)
```

### 7.5 GC and Object Lifetimes

Xray uses a layered memory management strategy:

| Storage | Mechanism | Reclamation |
|--|--|--|
| Global heap (`shared const`) | refcount | when refcount reaches 0 |
| Local heap (general objects) | reference counting + cycle collection | when the last reference is released; strong cycles are reclaimed by the cycle collector |
| Stack (`struct` values, locals) | RAII | when scope exits |
| Arena (low-level temporary allocations) | bulk free | at arena end |

**Memory observation points**:
- Default reclamation is reference-counted.
- Strong reference cycles are handled by the cycle collector at safepoints or explicit `mem.collectCycles()`.
- Memory safepoints in the instruction stream include function calls, backward branches, and explicit `mem.collectCycles()`.

Cycle collection and heap-layout design: see `src/runtime/mem/`.

---

## 8. Error Handling

> Source of truth: `src/vm/xvm_dispatch_exception.inc.c`, `src/vm/xvm_dispatch_misc.inc.c`, `src/runtime/object/xexception.c`, `stdlib/prelude/prelude.c`.

### 8.0 Design philosophy: value-return + panic boundary

Xray's error handling is split into two strictly separated channels:

| Channel | Syntax | Use case | Runtime cost |
|--|--|--|--|
| **Value-return channel** (`throw <enum>` / `try` / `catch`) | Business errors, recoverable failures | **Low overhead** (no `Exception` allocation and no unwind; only a predictable branch at call boundaries that may propagate or catch errors) |
| **Panic channel** (`catch panic`) | Runtime faults (OOB, division by zero, non-exhaustive match) | Limited stack unwinding |

Design principles:

- **Errors are values**: `throw <enum>` writes an enum value into the return channel — no stack unwinding, no Exception allocation.
- **Panics are not errors**: a panic signals a program bug or runtime invariant violation, not business logic.
- **No `throws` in function signatures**: xray does not adopt Java/Swift-style checked exceptions. Errors are handled via the throw/catch value-return channel.
- **`defer` replaces `finally`**: xray has no `finally` keyword; resource cleanup uses function-scoped `defer` (Go model).

### 8.1 Value-return error channel

#### 8.1.1 `throw` expression

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
- A typed `catch (e: EnumType)` matches only when the error value satisfies `is EnumType`.
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
    let user = fetchUser(-1)
} catch (e: HttpErr) {
    match (e) {
        HttpErr.NotFound(msg) -> log("404:", msg),
        HttpErr.ServerError(code, msg) -> log(string(code) + ":", msg),
        HttpErr.Timeout -> log("timeout"),
    }
}
```

ADT enums let `match` check exhaustiveness at compile time.

#### 8.1.5 Errors and coroutine boundaries

Value-return errors **do not propagate across coroutines automatically**. An uncaught error in a child coroutine:

- Terminates the child, error recorded in `coro.error`
- The parent is **not** notified automatically

Ways to pass child coroutine errors:

1. **Explicit Channel**:

```xray
enum WorkerErr { Failed(string) }
const err_ch = Channel<string>(1)

go {
    try {
        riskyWork()
        err_ch.send("ok")
    } catch (e) {
        err_ch.send("error")
    }
}

let result = match (err_ch.recv()) {
    Recv.Value(v) -> v
    _ -> "error"
}
if (result != "ok") { log("worker failed") }
```

2. **Structured concurrency `linked scope`** (recommended, see §10.5): child errors propagate to the parent scope automatically, routed through the correct channel (value-return enum errors via `catch`, panics via `catch panic`).

### 8.2 Panic channel

#### 8.2.1 What is a panic

A panic represents a **program bug or runtime invariant violation**, not business logic:

- Array out-of-bounds access
- Integer division by zero
- Non-exhaustive match
- Null reference dereference
- Runtime type assertion failure

Panics propagate via limited stack unwinding and generate `Exception` objects with stack traces.

#### 8.2.2 `catch panic`

`catch panic` catches runtime faults from the panic channel:

```xray
try {
    let arr: Array<int> = [1, 2, 3]
    let v = arr[10]                          // OOB → panic
} catch panic {
    log("runtime fault caught")
}

try {
    let n = 10 / 0                           // division by zero → panic
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

#### 8.2.3 The `Exception` class

`Exception` is now **used only by the panic channel**. The VM constructs `Exception` objects automatically on runtime faults:

```xray
@native
class Exception {
    message: string             // human-readable message (e.g. "index out of bounds")
    stack: Array<string>        // automatically captured call stack
    cause: Exception?           // chained cause
    code: int                   // error code
    data: Json?                 // additional data

    constructor(message: string = "", cause: Exception? = null)
    fn toString() -> string
}
```

User code generally does not construct `Exception` directly — use `throw <enum>` for business errors.

### 8.3 `defer` — resource cleanup

`defer` is a block-scoped cleanup statement guaranteed to run when the owning block exits (whether by fallthrough, `break` / `continue`, `return`, `throw`, or panic). Syntax: see §4.9.

```xray
fn fetch(url: string) -> string {
    let conn = open(url)
    defer conn.close()                       // conn is guaranteed to close

    let data = conn.read()
    if (data.isEmpty()) {
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
- `defer` blocks should not throw errors (behaviour is undefined)

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
enum ConnErr { Refused, Timeout, Reset }

fn fetchData(host: string) -> string {
    let conn = connect(host)
    defer conn.close()

    if (!conn.isAlive()) { throw ConnErr.Timeout }
    return conn.read()
}

fn main() {
    try {
        let data = fetchData("api.example.com")
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

#### Pattern 2: throw + catch for library APIs

```xray
enum ConfigErr { BadJson(string), BadField(string) }

fn parseConfig(text: string) -> Config {
    let json = parseJson(text)
    let port = json["port"].toInt()
    if (port == null) { throw ConfigErr.BadField("port") }
    return Config(port: port!)
}

fn main() {
    try {
        let cfg = parseConfig(configText)
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

#### Pattern 3: `??` for default values

```xray
let port = config?.port ?? 8080
let user = db.findUser(id) ?? guestUser
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

---

## 9. Generics

> Source of truth: `src/frontend/analyzer/xanalyzer_generic.c`, `src/frontend/analyzer/xanalyzer_subtype.c`.

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

let a = identity<int>(42)
let b = identity("hello")               // T inferred as string

// Generic class
class Box<T> {
    value: T
    constructor(v: T) { this.value = v }
    get() -> T { return this.value }
}

let b1 = Box<int>(42)
let b2 = Box<string>("hi")

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
| `Hashable` | usable as a `Map` key or `Set` element; built-in `int` / `float` / `string` / `bool` / `enum` / `BigInt` satisfy it by default, and user types must provide both `operator==(other: Self) -> bool` and `hash() -> int` |
| `Stringable` | callable via `.toString()`; almost every built-in type implements it by default |
| `Iterable<T>` | usable in `for-in`; Array, Map, Json, string, Range, enum, types with custom `iterator()` |

`Hashable` is a static contract: when a concrete class / struct / enum is used as a `Map<K, V>` key, a `Set<T>` element, or declares `implements Hashable`, the compiler must see a non-`static`, non-`private` `operator==(other: Self) -> bool` and `hash() -> int`. Legacy `hashCode()` does not satisfy the contract, and providing only one of `==` or `hash()` is a compile error. If the key/element is a type parameter, that parameter itself must be explicitly constrained, for example `fn f<K: Hashable>(m: Map<K, int>)`.

**Current limitations**:
- Constraints may only follow type parameters; there is no `where` clause.
- **Higher-kinded types** (`F<_>` as a parameter) are not supported.
- Default type parameters (`<T = int>`) are not supported.
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
let empty = Array<int>()              // no element to infer from
let m = Map<string, int>()
let result = identity<float>(0)            // 0 defaults to int; force float
```

### 9.4 Specialization and Monomorphization

**Implementation strategy**: build-time monomorphization, with different representation policies for different generic kinds.

- **Generic functions**: the compiler collects concrete call sites and applies rep-sharing by runtime representation. The current representation groups are I64 / F64 / PTR / BOOL, so one generic function produces at most four representation versions. Reference types that share the PTR representation reuse one function body, avoiding code-size growth proportional to the number of reference types.
- **Generic classes / structs**: each concrete type-argument combination is fully monomorphized and deduplicated by mangled name, not by PTR representation. `Box<string>` and `Box<MyClass>` remain distinct even though both use PTR representation, preserving exact type identity, field layout, and reflection semantics.
- Name mangling: `identity<int>` → `identity$i64`, `Pair<string, int>` → `Pair$str$i64`.
- The total number of monomorphization instances is capped by `XR_MONO_MAX_INSTANCES = 256` to prevent recursive or combinatorial explosion.
- Strict compile-time type checking ensures safety; concrete type-parameter information is retained at runtime for `Reflect.typeOf`.

> Source of truth: `src/frontend/analyzer/xanalyzer_mono.c` (monomorphization pass), `xanalyzer_mono.h` (API).

**Performance impact**:
- Function-level rep-sharing lets AOT generate unboxed fast paths for I64 / F64 / BOOL value representations while sharing one PTR version for reference types.
- Generic classes / structs do not use rep-sharing, so code and metadata size grow roughly with "type combinations x class body size"; this buys exact layout, faithful reflection, and per-type specialization. A future size-sensitive mode may add explicit opt-in rep-sharing for pure-PTR class generics.
- Built-in specialized containers (`Array<int>`, `Bytes`) further avoid boxing overhead.
- Cross-module generics are expanded during build-time whole-program / LTO analysis. Libraries that expose generic definitions must ship analyzable IR/AST form rather than only opaque precompiled artifacts.

**Deferred features**:
- Declaration-site variance annotations (`out T` / `in T`), default type parameters, and `where` clauses are not provided in this round; invariant containers remain the safe, AOT-friendly baseline.

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

```xray
class Container<T> {
    items: Array<T>
}
let c = Container<int>()
print(Reflect.typeOf(c))       // "Container<int>"
```

Type checks on concrete values use `is` / `as`.

---

## 10. Concurrency and Coroutines

> Source of truth: `src/runtime/coro/xcoro_*.c`, `src/runtime/sync/xchannel.c`, `src/runtime/sync/xscope.c`, `docs/rules/design-principles.md`.

xray's concurrency model is **goroutine-style coroutines + channels + strong static guarantees**. Design goal: writing `go { ... }` is as simple as writing an ordinary function call, while the **compiler guarantees no data race**.

### 10.1 Coroutine model

| Dimension | Choice |
|--|--|
| Scheduling model | M:N (user-space coroutines on multiple OS threads) |
| Scheduling policy | Cooperative (GC safepoints) + work stealing |
| Stack model | Stackless (per-coroutine VM value stack + frame array, grows on demand, no native C stack) |
| Creation cost | ~microsecond (initial VM value stack ~64 slots + 4 frames, not a native stack) |
| Context switch | VM context switch (save/restore VM frames), no native stack switch, no syscall |

Coroutines are distributed across multiple worker threads by default; the runtime sets a Go-style `GOMAXPROCS` parallelism level based on the CPU core count.

### 10.2 `go` — start a coroutine

```ebnf
GoExpr   ::= 'go' GoOptions? (Block | CallExpr | LambdaExpr CallArgs?)
GoOptions ::= '(' GoOption (',' GoOption)* ')'
GoOption ::= 'name' ':' StringLiteral
```

`go` is an **expression** returning a `Task<T>` handle. Three forms are valid:

```xray
// Form 1: call an existing function
let t1 = go worker(0, channel)

// Form 2: call a lambda literal (inline logic + explicit arguments)
let t2 = go fn(d: Json) -> int {
    return d.value * 2
}(payload)

// Form 3: block form (implicitly wrapped as a zero-argument lambda)
let t3 = go {
    return compute()
}

// Optional debugging name
let named = go(name: "worker-1") worker(1, channel)
```

**`move` lives in argument position**: cross-coroutine ownership transfer goes through the argument prefix `move`, **not** through a `go` option:

```xray
shared let data = { value: 10 }
let task = go fn(d: Json) -> int {
    return d.value + 1
}(move data)        // transfer data ownership to the coroutine; data is unusable afterwards
```

**Block-form restriction**: `go { ... }` is an implicit zero-argument lambda. It has no parameter list and does not bypass concurrency capture rules. The block may not capture ordinary mutable locals; external state used directly inside the block must be `shared const`, immutable global state, or an explicitly concurrency-safe object such as a Channel or Atomic. To pass local data into a coroutine, use the lambda-call or function-call form:

```xray
let n = 10
let task = go fn(x: int) -> int {
    return x + 1
}(n)
```

**Semantics**:
- Every `go` expression returns a `Task<T>`, where `T` is the callee's return type; functions returning `()` correspond to `Task<null>`.
- Coroutines are scheduled on idle worker threads (M:N).
- `go(name: ...)` only sets the debugging name and does not affect scheduling order.
- Uncaught exceptions are stored in the `Task` and rethrown when `await` is called.
- Plain locals (not `shared`, not `move`d) passed to `go` are **deep-copied automatically**; `shared const` is shared zero-copy; `shared let` must be `move`d.
- The `go { ... }` block form is equivalent to a zero-argument lambda and may use only external state that satisfies the coroutine capture rules; pass data with `go fn(x: T) -> R { ... }(arg)` or `go worker(arg)`.

### 10.3 `await` — wait for a result

```ebnf
AwaitExpr ::= 'await' Expression
           |  'await' 'all' Expression       // wait for all to complete
           |  'await' 'any' Expression       // wait for any one to complete
```

```xray
// single task
let task = go fetch("https://example.com")
let result = await task                    // yields the current coroutine until task completes

// await all: wait for all, returns the result array (in input order)
let t1 = go compute(2)
let t2 = go compute(3)
let t3 = go compute(4)
let results: Array<int> = await all [t1, t2, t3]
// also works on a variable directly, no brackets needed
let tasks = [t1, t2, t3]
let results2: Array<int> = await all tasks

// await any: wait for the first to complete, return its result; the others keep running
let first = await any [t1, t2, t3]

// await anySuccess: skip failing tasks; wait for the first successful one
let firstOk = await anySuccess [t1, t2, t3]
```

**Semantics**:

- `await` only applies to `Task<T>`; other types are a compile error.
- The current coroutine **yields** until the target completes (without blocking the OS thread).
- Exception propagation:
  - `await t` rethrows the exception thrown by `t`.
  - On success, `await t` returns `T`; if `T` is nullable, a returned `null` is the task's real result, not a cancellation or failure marker.
  - `await all` throws if any task throws (the others are cancelled).
  - `await any` throws only when **every** task fails; if any one completes, its result is returned.
  - `await anySuccess` is similar to `await any` but **skips** throwing tasks, awaiting only the first successful one.
- `all` / `any` / `anySuccess` are **contextual keywords** after `await`; they apply only in this position.
- The input to `await all` must be homogeneous: every element must have the same static `Task<T>` type, and the result type is `Array<T>`. Heterogeneous tasks such as mixed `Task<int>` and `Task<string>` are not automatically erased or boxed; await them individually, or convert inside each task to a shared enum / union / Json result type.

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
let t = go fetch(url)
if (!t.done) { /* still running */ }
let r = await t

match t.poll() {
    TaskResult.Pending -> print("running")
    TaskResult.Success(value) -> print(value)
    TaskResult.Failed(err) -> print(err)
    TaskResult.Cancelled -> print("cancelled")
    TaskResult.Timeout -> print("timeout")
}
```

**Cancellation semantics**: `cancel()` sets the cancellation flag; the coroutine throws a cancellation exception at the next safepoint (GC checkpoint, channel operation, `await`, `yield`). Plain `await` on a cancelled task throws `TaskCancelled`; use `awaitResult()` or `awaitTimeout(ms)` when you want a status value.

**Watchdog policy**: the runtime monitor thread (sysmon) observes the heartbeat of RUNNING coroutines. Pure Xray loops advance the heartbeat at back-edge safepoints, so they are observed as making progress; sysmon is mainly for long native/FFI calls or no-safepoint regions that stop progressing. If a heartbeat stays frozen for too long, the default behavior is **warn-only**: a stuck warning is printed after roughly 100ms, but the coroutine is not silently cancelled. Forced cancellation is explicit opt-in: set `XRAY_SYSMON_CANCEL_MS=N` (`N > 0`, milliseconds) to mark a coroutine for cancellation after its heartbeat remains frozen past that threshold; unset or `0` keeps warn-only behavior. Long pure-CPU loops may still insert `yield` for scheduling fairness and cancellation responsiveness, but `yield` is no longer required to avoid default watchdog cancellation.

### 10.5 Channel

```ebnf
ChannelType ::= 'Channel' '<' Type '>'
ChannelNew  ::= 'Channel' ('<' Type '>')? '(' Expression ')'
```

Channels are usually declared as `shared const` (cross-coroutine lifetime, reference semantics):

```xray
shared const ch  = Channel<int>(10)    // buffered, capacity = 10
shared const ch0 = Channel<int>(0)     // unbuffered (synchronous handshake)
shared const cha = Channel(3)          // element type inferred from the first send
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
shared const ch = Channel<int>(10)
ch.send(42)                             // blocking send
let v = match ch.recv() {
    Recv.Value(value) -> value
    Recv.Closed -> -1
    _ -> -1
}

let sent = ch.trySend(99)               // SendResult.Sent / Full / Closed
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

let value = ch.recvOr(-1)
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
shared const ch1 = Channel<int>(2)
shared const ch2 = Channel<int>(2)

select {
    msg from ch1 -> { print("got from ch1:", msg) }      // receive arm
    msg from ch2 -> { print("got from ch2:", msg) }      // receive arm
    100  to   ch1 -> { print("sent 100 to ch1") }        // send arm
    _ -> { print("no channel ready") }                   // default arm (non-blocking)
}
```

**Semantics**:
- Receive arm `name from ch -> body`: selected when ch has data, and binds the `Recv.Value(name)` payload to `name`.
- Send arm `value to ch -> body`: equivalent to `ch.send(value)`, but selected only when `ch` has capacity.
- Default arm `_ -> body`: runs immediately when no arm is ready; **omitting the default arm** makes `select` block until an arm becomes ready.
- When multiple arms are ready at the same time, one is selected **randomly** (matching Go).

### 10.7 `scope` (structured concurrency / lexical scope)

`scope` is a **statement keyword** that introduces a new lexical block. It serves two purposes:

1. **Pure lexical scope**: identical to a C/Rust `{ ... }` local block; `let` inside the block does not affect outer same-named variables.
2. **Structured concurrency** (semantic enhancement): coroutines started via `go` inside the block are **awaited automatically** before the block exits.

```ebnf
ScopeStmt          ::= 'scope' Block
LinkedScopeStmt    ::= 'linked' 'scope' Block          // sibling failure → cancel all + rethrow
SupervisorScopeExpr ::= 'supervisor' 'scope' Block     // collect every child result, return Array<TaskOutcome>
```

```xray
// lexical scope use
let x = 1
scope {
    let x = 10            // shadow the outer x; in effect inside the block
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

**Three scope variants**:

| Form | Behaviour when a child coroutine throws | Return value |
|---|---|---|
| `scope { ... }` | Siblings are not cancelled; exceptions do not propagate outward (each task is independent) | none (statement form) |
| `linked scope { ... }` | **Cancels all siblings** and **rethrows** the first exception outward | none |
| `supervisor scope { ... }` | **Collects** every child coroutine's completion result; siblings do not affect each other | `Array<TaskOutcome>` (one outcome per child) |

`TaskOutcome` is a prelude enum:

```xray
enum TaskOutcome {
    Success(unknown)    // child returned normally; payload is the return value
    Failed(unknown)     // child threw; payload is the original error value, not a forced string
    Cancelled           // child was cancelled
}
```

`supervisor scope` waits for all child coroutines started by `go` inside the block and appends an outcome for each completion. It does not flatten failures into a legacy error-message list; format the error explicitly inside a `TaskOutcome.Failed(err)` branch when text is needed.

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

// supervisor scope: collect every child outcome
let outcomes = supervisor scope {
    go failing("error1")
    go failing("error2")
    go ok()
}
print(outcomes.length)               // 3 (one outcome per child)
```

**General semantics**:
- `scope` is not a function call and does not require an import; it is a keyword block statement.
- All three forms await every coroutine started by `go` inside the block before exiting.

### 10.8 `move` — cross-coroutine ownership transfer

```ebnf
MoveExpr ::= 'move' Identifier        // only at call-argument position
```

`move` is an **argument-prefix modifier** (not a `go` option). It transfers ownership of a `shared let` variable from the current scope to the callee (including coroutines started by `go`, `ch.send()`, etc.). After `move`, the variable is statically marked as **moved**, and any subsequent reference is a compile error.

```xray
shared let buf = Bytes(1024 * 1024)

// hand off to a coroutine
let t = go fn(b: Bytes) -> int {
    return process(b)
}(move buf)
// compile error: buf has been moved
// print(buf.length)

// hand off to a channel
shared const ch = Channel<Bytes>(1)
shared let payload = Bytes(4096)
ch.send(move payload)
// compile error: payload has been moved
```

See §7.3 and §7.4 for the capture rules of shared variables.

### 10.9 Synchronisation primitives

xray's default concurrency model favours **message passing + immutable sharing**—`shared const`, `Channel`, `move`, and `scope` already eliminate most data races at compile time, so raw mutexes/locks are **discouraged**.

When mutual exclusion or atomic operations are unavoidable, the runtime provides:

| Primitive | Form | Description |
|---|---|---|
| Channel(1) | A single-element channel | The recommended mutex pattern (simulate lock/unlock via send/recv) |
| `shared let` + `move` | Compile-time exclusivity | Cross-coroutine exclusivity with no runtime overhead |
| `Atomic<T>` | Lock-free atomic wrapper | C11 atomic operations for `int`/`float`/`bool` |

> **Design note**: xray does not expose generic lock primitives such as `Mutex`/`RwLock`. For simple shared counters and flags, `Atomic<T>` is the recommended choice; for complex mutual exclusion, use `Channel(1)` to simulate lock/unlock.

#### `Atomic<T>` — lock-free atomic type

`Atomic<T>` wraps `int`, `float`, or `bool`, allocated on the system heap, using C11 atomic instructions for lock-free cross-coroutine reads and writes.

**Declaration constraint**: `Atomic<T>` variables must be declared as `shared const`; `move` is prohibited.

```xray
shared const counter = Atomic(0)         // Atomic<int>
shared const flag = Atomic(false)        // Atomic<bool>
shared const rate = Atomic(3.14)         // Atomic<float>
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
    Relaxed = 0,          // No cross-thread ordering guarantee
    Acquire = 1,          // Read barrier
    Release = 2,          // Write barrier
    AcquireRelease = 3,   // Read-write barrier
    SeqCst = 4,           // Sequential consistency (default)
}
```

The `Ordering` enum is automatically injected by the compiler (prelude); no import is needed.

```xray
shared const counter = Atomic(0)
counter.store(42, Ordering.Release)
let val = counter.load(Ordering.Acquire)
```


### 10.10 `yield` — yield the CPU

```ebnf
YieldStmt ::= 'yield'
```

```xray
for (i in 0..1000) {
    do_chunk(i)
    yield                       // explicit safepoint, lets other coroutines run
}
```

**Current implementation**: usable as a statement, equivalent to Go's `runtime.Gosched()`; valued `yield` is not supported.

### 10.11 Concurrency safety model

xray uses the type system to **eliminate most data races at compile time**:

| Rule | Enforced |
|--|--|
| `go` closures cannot capture ordinary `let` locals | ✅ |
| `shared const` is read-only and zero-copy across coroutines | ✅ |
| `shared let` must be `move`d to cross a coroutine boundary | ✅ |
| Channels for cross-coroutine values | ✅ |
| `Atomic<T>` must be declared as `shared const`; `move` prohibited | ✅ |

**Residual data-race risk** (detected at runtime, not compile time):
- Sending a mutable class reference via a channel (the receiver and sender may mutate concurrently)—prefer to send `shared const` / `Bytes` / immutable objects, or transfer ownership via `move`.

---

## 11. Modules

> Source of truth: `src/module/xmodule.c`, `src/module/xmodule_resolver.c`, `src/module/xmodule_graph.c`, `src/frontend/parser/xparse_import.c`.

### 11.1 Module Definition

- Each `.xr` file is one module.
- Module name = file name (with the `.xr` suffix removed).
- Module path mirrors directory structure: `src/utils/string.xr` → `utils.string`.

### 11.2 Project Layout

```
my_project/
├── xray.toml              # package manifest (name, dependencies, entry)
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
entry = "src/main.xr"

[dependencies]
http = "1.0"
json = "0.2"

[dev-dependencies]
test = "1.0"
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

`export` declarations **must appear at the module top level**. Xray supports three export forms:

```ebnf
ExportStmt ::= 'export' Declaration                              // export the declaration directly
            |  'export' '{' Identifier (',' Identifier)* '}'     // export already-declared identifiers
            |  'export' '{' ExportSpec (',' ExportSpec)* '}' 'from' StringLiteral
            |  'export' '*' 'from' StringLiteral
ExportSpec ::= Identifier ('as' Identifier)?
Declaration ::= FnDecl | ClassDecl | StructDecl | ConstDecl | TypeAlias
```

```xray
// 1. export a declaration directly
export fn helper() { return }
export class MyClass {
    value: int
    constructor() { this.value = 1 }
}
export const VERSION = "1.0"

// 2. export already-declared identifiers (declare internally first, expose at the end)
fn _helper() -> string { return "..." }
fn publicFn() -> string { return _helper() }
export { publicFn }

// 3. re-export (with optional renaming)
export { getUser, getUserAge as getAge } from "./user"

// 4. wildcard re-export (forward all exports of another module)
export * from "./product"
```

**Restrictions**:
- Declarations not marked `export` are **private** to the module.
- `export let` is not supported. Mutable bindings cannot be shared across modules; use `export const` instead.
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
let t = time.now()
time.sleep(100)
```

---

## 12. Testing

> Source of truth: `src/app/cli/xcli_test.c`, `stdlib/xray/test.xr`, `docs/testing-spec.md`.

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
    let result = compute()
    assert_eq(result, 42)
    assert(result > 0)
}
```

**Semantics**:
- Functions annotated with `@test` are auto-discovered and run by `xray test`; ordinary functions are not.
- Test naming convention: `test_xxx` (snake_case), descriptive.
- Test functions take no parameters and return nothing; expectations are expressed via the `assert*` family.
- A file may contain **any number** of `@test` functions; they run in declaration order.

### 12.2 Test Entry Points

Test file convention:
- Either co-located with the code under test or under `tests/regression/`.
- File name format: `XXXX_topic.xr` (four-digit number + topic).

Run:

```bash
xray test                                  # run all tests
xray test tests/regression/01_literals/    # run a whole group
xray test tests/regression/01_literals/0100_int_basic.xr   # single file
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
    let task = go fetch_data("http://...")
    let result = await task
    assert_eq(result.status, 200)
}
```

### 12.5 Attribute Overview

Xray attributes are prefixed with `@` followed by an identifier. The current parser only recognizes **three** attributes (source: `xparse_decl.c:xr_parse_single_attribute`):

| Attribute | Applies to | Description |
|---|---|---|
| `@test` | function | mark as a test function; accepts optional arguments: `@test(skip)` to skip, `@test(timeout: 30)` to set a timeout |
| `@native` | class / struct / fn | declare a native implementation; method bodies are provided by C (used in stdlib type declarations) |
| `@deprecated` | any declaration | deprecation warning; optional message: `@deprecated("use X instead")` |

```xray
@test                                 // mark as a test
fn test_basic() { return }

@test(skip)                           // skip this test
fn test_wip() { return }

@native                               // C implementation
class Array<T> {
    length: int
    push(v: T)
    // no method bodies — provided by src/runtime/object/xarray_methods.c
}

@deprecated("use newAPI() instead")
fn oldAPI() { return }
```

> Attributes that do not exist (do not use them in user code): `@before_each` / `@after_all` / `@async` / `@override` etc. — these trigger an "unknown attribute name" error.

### 12.6 `xray run` / `xray test` / `xray repl`

| Command | Purpose |
|--|--|
| `xray run main.xr` | run the main program |
| `xray test` | run the test suite |
| `xray repl` | start the REPL |
| `xray build --aot` | AOT compile |
| `xray fmt` | format code |

---

## 13. Built-in Functions

> Source of truth: `src/ir/xi_lower_expr.c`, `src/vm/xvm_dispatch_*.inc.c`, `src/runtime/object/builtins/`, `src/frontend/analyzer/xanalyzer_builtins.c`.

These global functions and built-in constructor/static functions are usable without any `import`. In the tables below, `value` denotes "any runtime value" — it is **not** a writable `any` type; xray no longer has an `any` type in the source language.

### 13.1 I/O and Debugging

| Function | Signature | Description |
|--|--|--|
| `print` | `(...values) -> ()` | print to stdout, automatically appending a newline; multiple arguments are separated by spaces |
| `dump` | `(value, indent?) -> ()` | structured debug output |

### 13.2 Type Conversion

| Function | Signature | Description |
|--|--|--|
| `int(x)` | `(value) -> int` | convert to int; `char` converts to its Unicode scalar code point; throws if string parsing fails |
| `float(x)` | `(value) -> float` | convert to float |
| `string(x)` | `(value) -> string` | convert to string; `char` converts to a one-scalar string |
| `bool(x)` | `(value) -> bool` | convert to bool; rules in §2.3.3 |
| `char(n)` | `(int) -> char` | construct a Unicode scalar from an integer; surrogate and out-of-range values throw |
| `chr(n)` | `(int) -> string` | Unicode code point → one-scalar string |
| `copy(x)` | `(T) -> T` | deep copy, preserving runtime type |

### 13.3 Type Checking

| Function / expression | Signature | Description |
|---|---|---|
| `typeof(x)` | `(value) -> string` | returns the runtime type-name string |
| `x is T` | expression | runtime type check; the analyzer may narrow types |

```xray
let x = 42
print(typeof(x))                // "int"
print(x is int)                 // true
print(typeof(x) == "int")       // true
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

BigInt uses the `123n` literal or `int.toBigInt()`; Json uses `Json.parse` / `Json.stringify`; DateTime uses factory functions in the `datetime` module.

---

## 14. Built-in Type Methods

> Source of truth: prelude / analyzer / runtime built-in type registration and method definitions.
> MCP knowledge only consumes the generated analyzer metadata; it does not maintain its own copy of built-in method signatures.

This section is a **method index** for each type (grouped by topic). Concrete signatures, parameter descriptions, and behavioral details are governed by the implementation source.

### 14.1 `int` Methods

| Method | Signature | Description |
|--|--|--|
| `abs()` | `() -> int` | absolute value |
| `toString()` | `() -> string` | decimal string |
| `toBigInt()` | `() -> BigInt` | convert to BigInt |
| `toFloat()` | `() -> float` | convert to float |
| `toHex()` | `() -> string` | hexadecimal string |
| `max(other)` / `min(other)` | `(int) -> int` | binary max/min |
| `floor()` / `ceil()` / `round()` | `() -> int` | for `int`, returns self |
| `sqrt()` | `() -> float` | square root |
| `pow(exp)` | `(float) -> float` | power |
| `checkedAdd(other)` / `checkedSub(other)` / `checkedMul(other)` | `(int) -> int?` | returns `null` on overflow |
| `saturatingAdd(other)` / `saturatingSub(other)` / `saturatingMul(other)` | `(int) -> int` | clamps overflow to the `int` boundary |
| `wrappingAdd(other)` / `wrappingSub(other)` / `wrappingMul(other)` | `(int) -> int` | explicit two's-complement wrap |

`abs()` follows integer wrap semantics: `(-9223372036854775807 - 1).abs()` returns itself. `toHex()` keeps a sign prefix for negative values, for example `-0x8000000000000000`.

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

### 14.4.1 `char` Methods

| Method | Signature | Description |
|--|--|--|
| `toString()` | `() -> string` | return a one-Unicode-scalar string |
| `ord()` | `() -> int` | return the Unicode scalar code point |
| `isLetter()` | `() -> bool` | whether the scalar is a Unicode letter |
| `isNumber()` | `() -> bool` | whether the scalar is a Unicode number |
| `isAlphanumeric()` | `() -> bool` | whether the scalar is a letter or number |
| `isWhitespace()` | `() -> bool` | whether the scalar is whitespace |

`char` is an independent primitive type and does not inherit integer methods; use `ord()` or `int(c)` explicitly when the code point is needed.

### 14.5 `string` Methods

| Member | Type / Description |
|--|--|
| `length` / `size` | Unicode scalar count property |
| `charAt(i)` | one-scalar string at the given Unicode scalar index |
| `charCodeAt(i)` | code point at the given Unicode scalar index |
| `concat(...others)` | concatenate strings |
| `includes(s)` | substring containment test |
| `indexOf(s)` / `lastIndexOf(s)` | substring search |
| `slice(start, end?)` / `substring(start, end?)` / `substr(start, len?)` | substrings |
| `toLowerCase()` / `toUpperCase()` | case conversion |
| `trim()` / `trimStart()` / `trimEnd()` | whitespace trimming |
| `split(sep, limit?)` | split into `Array<string>` |
| `replace(from, to)` / `replaceAll(from, to)` | replacement |
| `repeat(n)` | repeat |
| `startsWith(s)` / `endsWith(s)` | prefix/suffix check |
| `padStart(len, pad?)` / `padEnd(len, pad?)` | padding |
| `match(pattern)` | regex match |
| `iterator()` | `() -> Iterator<char>` |
| `entriesIterator()` | `() -> Iterator<(int, char)>` |
| `entries()` | `() -> Array<(int, char)>` |

The string index expression `s[i]` returns `char`; `charAt(i)` keeps the JavaScript-style string return value. `slice(start, end?)` uses the same half-open range and negative-index rules as slice expressions: a negative index is first converted as `length + index`, then clamped into `[0, length]`.

### 14.6 `Bytes`

`Bytes` is a prelude type; construction is handled via builtin paths such as `Bytes(n)` / `Bytes(n, fill)`. String conversion and encoding-related operations should prefer the `encoding` / `base64` modules. There is currently no separate `stdlib/types/bytes.xr` declaration; tooling should not assume a complete Array-isomorphic API.

### 14.7 `Array<T>` Methods

| Member | Type / Description |
|--|--|
| `length` | `int` property |
| `arr[i]` / `arr[i] = v` | indexed read/write |
| `push(x)` / `pop()` | tail insert/remove |
| `shift()` / `unshift(x)` | head insert/remove |
| `slice(start?, end?)` | slicing |
| `splice(start, deleteCount, ...items)` | in-place insert/remove |
| `concat(...arrays)` | concatenation |
| `indexOf(x)` / `includes(x)` | search |
| `join(sep?)` | concatenate into a string |
| `reverse()` / `sort(cmp?)` | in-place reorder |
| `map(fn)` / `filter(fn)` / `reduce(fn, init)` | functional helpers |
| `forEach(fn)` / `find(fn)` / `findIndex(fn)` / `every(fn)` / `some(fn)` | traversal and predicates |
| `flat(depth?)` / `fill(v, start?, end?)` / `copyWithin(target, start, end?)` | array utilities |
| `iterator()` / `entriesIterator()` / `entries()` | iteration protocol |

`slice(start?, end?)` uses the same half-open range and negative-index rules as slice expressions; it returns an independent array and leaves the original array unchanged.

### 14.8 `Map<K, V>` Methods

| Member | Type / Description |
|--|--|
| `length` | `int` property |
| `m[k]` / `m[k] = v` | indexed read/write |
| `get(k)` / `set(k, v)` | read/write |
| `has(k)` / `delete(k)` / `clear()` | query and remove |
| `keys()` / `values()` / `entries()` | keys, values, key/value pairs |
| `forEach(fn)` | traversal |
| `iterator()` / `entriesIterator()` | iteration protocol |

**Map literal**: `#{"k1": v1, "k2": v2}` or `#{}`; entries use `:`, distinguished from Object/Json literals by the `#` prefix.

### 14.9 `Set<T>` Methods

| Member | Type / Description |
|--|--|
| `length` | `int` property |
| `add(x)` / `has(x)` / `delete(x)` | insert, query, remove |
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
| `trySend(v)` | non-blocking send, returns `SendResult` |
| `tryRecv()` | non-blocking receive, returns `Recv<T>`; empty is `Recv.Empty` |
| `sendTimeout(v, ms)` | timed send, returns `SendResult`; timeout is `SendResult.Timeout` |
| `recvTimeout(ms)` | timed receive, returns `Recv<T>`; timeout is `Recv.Timeout` |
| `close()` | close the channel |
| `isClosed` / `isClosed()` | closed state; both runtime property and method are supported |

`Recv.Value(v)` carries the channel payload, so `Channel<int?>` can distinguish a real `Recv.Value(null)` from `Recv.Closed`.

### 14.11 `Json`

`Json` is a dynamic structured-data type. Ordinary field access uses `j.field` / `j["field"]`; generic queries and serialization go through `Json` static functions to avoid colliding with user field names.

| Static function | Description |
|--|--|
| `Json.keys(obj)` / `Json.values(obj)` / `Json.entries(obj)` | enumerate object fields |
| `Json.has(obj, key)` | field existence |
| `Json.get(obj, key, default?)` | field read; returns `default` or `null` if absent |
| `Json.size(obj)` | number of fields |
| `Json.isEmpty(obj)` | emptiness predicate |
| `Json.parse(s)` / `Json.tryParse(s)` / `Json.isValid(s)` | JSON parsing and validation |
| `Json.stringify(value, indent?)` | serialization |

**Literal**: `{ name: "alice", age: 30 }` has dynamic type `Json`. For sealed objects, annotate with `type T = { name: string, age: int }`.

### 14.12 `Range`

`a..b` is the half-open interval `[a, b)`, used in expressions and `for-in`. Common members: `start`, `end`, `length`, `includes(x)`, `toArray()`, `toString()`.

### 14.13 `DateTime`

The `import datetime` module provides factory functions: `now`, `utc`, `create`, `createUTC`, `fromTimestamp`, `fromTimestampMs`, `parse`, `offset`. `DateTime` instances are registered by the prelude, so the type name need not be imported.

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
| `replace(s, replacement)` | replacement |
| `split(s)` | split |

### 14.15 `StringBuilder`

| Method | Description |
|--|--|
| `length` | current length property |
| `append(s)` | append and return self |
| `toString()` | output string |
| `clear()` | empty and return self |

### 14.16 `Exception`

The built-in `Exception` class has fields `message`, `stack`, `cause`, `code`, `data`, the constructor `constructor(message: string = "", cause: Exception? = null)`, and `toString()`.

### 14.17 `Task<T>` / `EnumValue` / `EnumType`

`Task<T>` properties: `done`, `status`; methods: `cancel()`, `poll()`, `awaitResult()`, `awaitTimeout(ms)`. `poll()` and explicit wait methods return `TaskResult<T>`; plain `await task` returns `T` on success and uses the exception path for failure or cancellation. `EnumValue` properties: `name`, `value`, `ordinal`; methods: `toString()`. `EnumType` properties: `name`, `memberCount`; methods: `getMember(name)`.

### 14.18 Other Prelude Types (`Logger` / `NetConn` / `NetListener`)

These types are registered by the prelude; instances are constructed by factory functions in modules such as `log` / `net`. The complete runtime capability follows the corresponding stdlib module.

### 14.19 `Atomic<T>` Methods

`Atomic<T>` wraps `int`, `float`, or `bool` with lock-free atomic operations. Must be declared as `shared const`; `move` is prohibited.

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

> Source of truth: stdlib implementations and analyzer builtin metadata.
> MCP knowledge fetches API signatures via `xray builtin-dump` and injects per-module knowledge cards at generation time.
> See [Appendix D — stdlib module index](#d-stdlib-module-index).

> **Authoritative native module list** (22 modules; source: `stdlib/<module>/*.c`):
>
> `base64`, `cluster`, `compress`, `crypto`, `csv`, `datetime`, `encoding`, `mem`, `http`, `io`, `log`, `math`, `net`, `os`, `path`, `regex`, `time`, `toml`, `url`, `ws`, `xml`, `yaml`.
>
> Built-in types that need no import are registered by the prelude (`Array`, `Map`, `Set`, `Json`, `Channel`, `Bytes`, `BigInt`, `StringBuilder`, `Exception`, `Regex`, `Logger`, `NetConn`, `NetListener`, etc.). See §1.5.6 / §2.2.

### 15.1 File I/O and System

| Module | Topic | Key APIs |
|--|--|--|
| `io` | file I/O + filesystem | `readFile` `writeFile` `exists` `mkdir` `remove` `readdir` `stat` `stdin` `stdout` `stderr` |
| `path` | path manipulation | `join` `dirname` `basename` `extname` `normalize` `isAbsolute` `resolve` `relative` `parse` `format` |
| `os` | OS interface | `getenv` `setenv` `environ` `exit` `getpid` `getcwd` `chdir` `hostname` `tmpdir` `homedir` `cpuCount` `sleep` `exec`; constants `platform` `arch` `sep` `eol` |

> Xray has **no** standalone `fs` module; filesystem operations live in `io`. Process arguments / process information are exposed through the global `process` object (`process.args` / `process.file` / `process.dir`, see §16.5), not `os`.
> `os.platform` / `os.arch` / `os.sep` / `os.eol` are **constant strings** (no parentheses); other `os.*` are function calls.

### 15.2 Networking

| Module | Topic | Key APIs |
|--|--|--|
| `net` | TCP / UDP / TLS sockets + DNS | `listen` `dial` `accept` `read` `readInto` `write` `writeBytes` `copy` `copyBidirectional` `setDeadline` `lastError` `lookup` `dialTLS` `NetConn` `NetListener` |
| `http` | HTTP / HTTPS client + server + HTTP/2 | `get` `post` `request` `Server` `urlEncode` `urlDecode` |
| `ws` | WebSocket | client/server connections |
| `url` | URL parsing and construction | `parse` `format` `parseQuery` `buildQuery` `encode` `decode` |

> DNS lookups go through `net.lookup(host)`; there is no standalone `dns` module.

#### 15.2.1 TCP Data Paths

The `net` TCP API intentionally has three data paths:

- `read(conn)` / `write(conn, data)`: message path. Payload is exposed as an Xray `string`, suitable for protocol parsing, text handling, and logic that must inspect bytes.
- `readInto(conn, bytes, maxlen?)` / `writeBytes(conn, bytes)`: reusable `Bytes` buffer path for binary protocol hot loops without per-packet temporary strings.
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

> JSON encoding/decoding is **not** in a separate `json` module; use the built-in type `Json`'s static methods `Json.parse(s)` / `Json.stringify(v)` (no import required; see §14.10).

### 15.4 Cryptography and Hashing

| Module | Key APIs |
|--|--|
| `crypto` | `md5` `sha1` `sha256` `sha512` `hmac` `aes` `rsa` etc.; full API in stdlib source |

> stdlib has **no** standalone `random` module; for pseudo-random numbers use `crypto`'s random source or `math` utilities.

### 15.5 Compression

| Module | Key APIs |
|--|--|
| `compress` | `gzip` / `gunzip`, `deflate` / `inflate`, etc. |

### 15.6 Time

| Module | Key APIs |
|--|--|
| `time` | `now()` `monotonic()` `sleep(ms)` `Duration` |
| `datetime` | `DateTime` / `Date` / `Time` parsing and formatting (see §14.12) |

### 15.7 Math

| Module | Key APIs |
|--|--|
| `math` | `sin` `cos` `tan` `log` `pow` `sqrt` `floor` `ceil` `round` `abs` `min` `max` etc.; constants `PI` / `E` / `MAX_INT` / `MIN_INT` |

### 15.8 Text

| Module | Key APIs |
|--|--|
| `regex` | `compile(pattern)` returns `Regex`; see §14.13. The `/pattern/flags` literal form is also supported |

> stdlib has **no** `strconv` module; for string ↔ numeric conversions use the built-ins `int(s)` / `float(s)` / `string(n)` (see §13.2).

### 15.9 Logging and Diagnostics

| Module | Key APIs |
|--|--|
| `log` | `debug` / `info` / `warn` / `error` / `fatal` / `child()`, source-position toggles, async write mode |
| `mem` | `collectCycles()` `isCycleCollectionEnabled()` `liveBytes()` `liveObjects()` `info()` |

### 15.10 Distributed

| Module | Topic |
|--|--|
| `cluster` | node discovery, health checks, topic-based message bus (see `stdlib/cluster/`) |

### 15.11 Testing

The `@test` attribute together with the global `assert*` family is enough; **no** separate `test` module is needed (see §12).

### 15.12 Modules That **Do Not Exist**

Modules that may have been referenced historically but are **not** part of the current stdlib (to avoid confusion):

`fs` · `process` · `dns` · `random` · `strconv` · `sync` · `runtime` · `json`

Their functionality has either moved into other modules (see the per-section notes above) or has not yet been implemented.

> **Full index**: see [Appendix D](#d-stdlib-module-index).

---

## 16. Runtime Model

> Source of truth: `src/runtime/`, `src/vm/`, `src/runtime/mem/`, `docs/rules/architecture.md`.

### 16.1 Value Representation

Xray values are uniformly represented as `XrValue`. The current implementation requires a 64-bit platform and uses a **16-byte tagged struct-of-union**:

- **Descriptor (8 bytes)**: `tag: uint8`, `flags: uint8`, `heap_type: uint16`, and `ext: uint32`. The `tag` is the single entry point for type dispatch; `heap_type` is meaningful only when `tag == PTR`.
- **Payload (8 bytes)**: one of `int64`, `double`, or pointer, interpreted by the tag.
- **No NaN-boxing / no low-bit pointer tagging**: integers keep the full 64-bit payload; object references are ordinary heap pointers, with type metadata in the descriptor.
- **Strings are not value-level SSO**: `string` is always an `XrString` heap object, with bytes stored inside the object's `data[]` flexible array. Runtime short strings are coroutine-local with lock-free allocation by default; only literals/symbols, explicit `intern()`, and map/set keys are interned in the global pool, and strings are promoted to shared (atomic RC) on demand when crossing coroutine boundaries (channel send, `go` arguments, task/scope results). These are object-storage policies and do not change the `XrValue` representation.

| Value type | Internal representation |
|--|--|
| `int` | `XR_TAG_I64` + 64-bit signed payload |
| `float` | `XR_TAG_F64` + IEEE-754 double payload |
| `bool` | `XR_TAG_BOOL` + `0/1` payload |
| `char` | `XR_TAG_CHAR` + Unicode scalar payload |
| `null` | `XR_TAG_NULL` + zero payload |
| `string` | `XR_TAG_PTR` + `XR_TSTRING` + `XrString*` |
| `Bytes` | `XR_TAG_PTR` + bytes heap object |
| Other objects | `XR_TAG_PTR` + heap type + heap pointer |

Typed-array element layout is part of the container metadata. `Array<char>` uses `XR_ELEM_CHAR`; its data area is a contiguous `uint32_t[]` of Unicode scalars. Loads re-box values as `XR_TAG_CHAR`, and stores reject non-`char` values, so it cannot be confused with `Array<uint32>`.

### 16.2 Memory Allocation

| Region | Use |
|--|--|
| **System heap** | C `malloc/free`, used for native data structures |
| **Global heap** | `shared const` / `shared let`, reference counting |
| **Coroutine heap** | per-coroutine RC object heap; strong cycles are reclaimed by the cycle collector |
| **Stack** | `struct` values, local immediates, function frames |
| **Arena** | parser temporary allocation, frame allocation |

### 16.3 Memory Model

- Default reclamation is **per-coroutine reference counting**. When the last strong reference is released, the object enters its release path immediately.
- **Cycle collection** handles strong reference cycles; the explicit user entrypoint is `mem.collectCycles()`.
- **Memory safepoints**: function calls, backward branches, explicit `mem.collectCycles()`.
- **User-visible introspection**: `mem.liveBytes()` / `mem.liveObjects()` / `mem.info()` report the current coroutine heap's live-memory view.

See `src/runtime/mem/` for details.

### 16.4 Coroutine Scheduling

- M:N scheduling (M OS threads × N coroutines).
- **work-stealing**: idle workers steal tasks from other workers' queues.
- **Cooperative preemption**: coroutines yield at safepoints (no forced preemption).
- **Fairness**: a single runnable queue works with local run-next, global injection, and work-stealing; scheduling order does not expose user-level priorities.
- **Stack management**: segmented stacks grow on demand.

See `src/runtime/coro/` for details.

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
os.arch                   // constant: "arm64" / "x64" / "x86"
os.sep                    // constant: path separator
os.eol                    // constant: end-of-line
os.sleep(100)             // sleep in milliseconds (equivalent to `time.sleep`)
```

> **Naming convention**: `os.*` follows POSIX function names (`getenv` / `getcwd` / `getpid`); it does not track Node.js. Node-style `process.env` mapping is not provided — use `os.getenv(name)` / `os.environ()`.

See `stdlib/os/` for details.

### 16.6 Panic Runtime

The built-in `Exception` class is a prelude type (declared in `stdlib/types/exception.xr`) and belongs only to the **panic channel**. Runtime faults (out-of-bounds, division by zero, non-exhaustive `match`, runtime invariant violations, and similar faults) are represented by `Exception` objects constructed by the VM/AOT runtime:

```xray
@native
class Exception {
    message: string             // human-readable message
    stack: Array<string>        // automatically captured call stack, one formatted line per frame
    cause: Exception?           // chained cause
    code: int                   // error code (auto-parsed from "E0xxx: ..." prefix; default 0)
    data: Json?                 // optional structured data for a runtime fault

    constructor(message: string = "", cause: Exception? = null)
    fn toString() -> string
}
```

Recoverable user-level errors do not use `Exception`: a `throw` expression accepts enum variant values only (see §8.1.1); non-enum error values are rejected at compile time (error code `E0370`).

Stack unwinding (panic channel only): the VM's `xvm_unwind_stack()` walks the try-table to find `catch panic` handlers, releasing locals frame by frame and running `defer` along the way before jumping to the handler. Recoverable errors use the value-return channel and never unwind the stack. See §8 for details.

### 16.7 Value-return Error Channel Runtime

`throw <enum>` writes the enum value into the frame's `pending_error` slot, sets the error flag, and returns. The caller detects the flag via `OP_ERR_CHECK` and either enters a `catch` handler or continues returning upward. This channel performs no stack unwinding and allocates no `Exception`; at call boundaries that may propagate or catch errors, the happy path goes through only a predictable error-flag branch.

### 16.8 Object Reclamation & Finalizer Timing Contract

xray **currently exposes no user-visible deterministic destructor (destructor / finalizer / `Drop`)**. The **timing** of object reclamation is NOT part of the observable semantic contract; it is implementation-defined and differs across execution backends:

- Reclamation may occur at "the moment the last reference disappears", "some GC point", or "process exit"; VM and AOT reclamation timing is **not guaranteed to agree**.
- Programs **must not depend on**: (a) an object being reclaimed at any particular moment; (b) whether any destructor / finalizer runs, in what order, or on which thread.

The only deterministic, cross-backend (VM / AOT) consistent cleanup mechanism is **`defer`**, which runs at owning-scope exit in LIFO order (including the panic-unwind path), independent of object reclamation timing. Code that must deterministically release external resources (files / handles / locks) must use `defer` rather than relying on object finalization.

> Evolution note: once deterministic destruction (RAII / `Drop`) is formally added to the language, this section will be upgraded to a **deterministic reclamation contract** (specifying destruction points and order), gated byte-for-byte by cross-backend differential tests. Until then, "reclamation timing / finalizer behavior" is explicitly declared an implementation-defined, non-deterministic aspect.

---

## 17. Compilation Pipeline

> Source of truth: `src/frontend/`, `src/vm/`, `src/jit/`, `src/aot/`, `docs/rules/architecture.md`.

### 17.1 Pipeline Overview

```
Source (.xr)
    ↓ lexer
Token stream
    ↓ parser
AST
    ↓ analyzer (semantic analysis, type checking, scope/capture/generic)
Typed AST
    ↓ ssa-gen
SSA IR
    ↓ optimize (const fold, DCE, inline, TCO, escape analysis)
Optimized SSA
    ↓ codegen
Bytecode  →  AOT (machine code)
    ↓ VM
    ↓ Profiler → JIT (machine code)
Execution
```

### 17.2 Lexical Analysis (Lexer)

- Source of truth: `src/frontend/lexer/xlexer.c`.
- Outputs an `XrToken` stream; each token carries `kind`, `value`, `pos(line, col)`.
- Handles: string interpolation (producing `${...}` concatenation sequences), raw strings, regex literals.

### 17.3 Syntax Analysis (Parser)

- Source of truth: `src/frontend/parser/` (split by file: expr, stmt, decl, match).
- Style: hand-written Pratt parser (expressions) + recursive descent (declarations / statements).
- Error recovery: after an error, jumps to the next synchronization point (`;` `}` `)`) and tries to keep parsing.
- Output: an `XrAstNode*` root (i.e., the module).

### 17.4 Semantic Analysis (Analyzer)

- Source of truth: `src/frontend/analyzer/xanalyzer_*.c` (split by topic).
- **Scoping**: nested symbol tables, name resolution, shadowing checks.
- **Type checking**: bidirectional type inference, union narrowing, Json structural matching.
- **Generics**: build-time monomorphization, constraint checking, call-site rewriting; cross-module generics are expanded during whole-program / LTO analysis, so generic libraries must provide analyzable IR/AST.
- **Closure analysis**: upvalue tagging, `go` closure capture restrictions.
- **Error codes**: the `XR_ERR_ANALYZE_*` family.

### 17.5 SSA and Optimization

- Source of truth: `src/ir/xi_opt*.c`, `src/ir/xi_pass.h`, `src/jit/`.
- **Constant folding**: compile-time evaluation.
- **DCE** (dead-code elimination): removes unused code.
- **Inlining**: small-function inlining.
- **TCO** (tail-call optimization): converts accumulator-style tail recursion into loops.
- **Escape analysis**: stack-vs-heap allocation decisions.

### 17.6 Bytecode and VM

- Source of truth: `src/vm/`, `include/xray_opcodes.h`.
- A hybrid register/stack VM.
- IC (inline cache) accelerates property access and method dispatch.

### 17.7 JIT and AOT

- **JIT** (runtime): once a hot function is selected by the profiler → it is compiled into native machine code. Source: `src/jit/`.
- **AOT** (ahead-of-time): `xray build --aot` → the entire module is compiled into a native binary. Source: `src/aot/`.
- They share the same SSA IR; only the back-end differs (interpret / JIT / AOT).

---

## 18. Error Code Reference

> Source of truth: `src/runtime/xerror_codes.h`, `src/runtime/xerror.h`.

> Xray has **two error-code systems**:
>
> - Numeric codes (`#define`s in `xerror_codes.h`): used by lexer / parser / VM runtime, allocated in ranges.
> - Enum codes (the `XrErrorCode` enum in `xerror.h`): used by the analyzer (type / binding / closure), allocated in ranges.
>
> The tables below cover the **principal** error codes; the full list and triggering conditions are governed by the source. The `error.name` field on a thrown error matches the "Name" column.

### Error-code categories (numeric)

| Range | Category |
|--|--|
| `E0101`-`E0199` | Lexical errors |
| `E0201`-`E0299` | Syntax errors |
| `E0301`-`E0399` | Compile errors |
| `E0401`-`E0499` | Runtime errors |
| `E0501`-`E0599` | Module errors |
| `E0801`-`E0899` | Rejected syntax |

### 18.1 Lexical Errors

| Code | Name | Description |
|--|--|--|
| `E0101` | `XR_ERR_LEX_INVALID_CHAR` | invalid character |
| `E0102` | `XR_ERR_LEX_UNTERMINATED_STR` | unterminated string |
| `E0103` | `XR_ERR_LEX_INVALID_NUMBER` | malformed numeric literal |
| `E0104` | `XR_ERR_LEX_INVALID_ESCAPE` | invalid escape sequence |

### 18.2 Syntax Errors

| Code | Name | Description |
|--|--|--|
| `E0201` | `XR_ERR_SYN_UNEXPECTED_TOKEN` | unexpected token |
| `E0202` | `XR_ERR_SYN_EXPECTED_EXPR` | expected expression |
| `E0203` | `XR_ERR_SYN_EXPECTED_STMT` | expected statement |
| `E0204` | `XR_ERR_SYN_UNCLOSED_PAREN` | unclosed `(` |
| `E0205` | `XR_ERR_SYN_UNCLOSED_BRACE` | unclosed `{` |
| `E0206` | `XR_ERR_SYN_UNCLOSED_BRACKET` | unclosed `[` |
| `E0207` | `XR_ERR_SYN_INVALID_ASSIGN` | illegal assignment target (e.g., assigning to a literal) |

### 18.3 Compile-time / Name-resolution Errors

Numeric codes (basic):

| Code | Name | Description |
|--|--|--|
| `E0301` | `XR_ERR_CMP_UNDEFINED_VAR` | undefined name |
| `E0302` | `XR_ERR_CMP_REDEFINED_VAR` | redeclaration |
| `E0303` | `XR_ERR_CMP_CONST_ASSIGN` | assignment to `const` |
| `E0304` | `XR_ERR_CMP_INVALID_BREAK` | `break` outside a loop |
| `E0305` | `XR_ERR_CMP_INVALID_CONTINUE` | `continue` outside a loop |
| `E0306` | `XR_ERR_CMP_INVALID_RETURN` | `return` outside a function |
| `E0307` | `XR_ERR_CMP_TOO_MANY_PARAMS` | parameter count exceeds limit |
| `E0308` | `XR_ERR_CMP_TOO_MANY_LOCALS` | local-variable count exceeds limit |

Analyzer enum codes (`XrErrorCode`, defined in the 350+ section of `xerror.h`):

| Enum | Description |
|--|--|
| `XR_ERR_ANALYZE_UNDEFINED_VAR` | undeclared variable |
| `XR_ERR_ANALYZE_TYPE_MISMATCH` | type not assignable |
| `XR_ERR_ANALYZE_CONST_ASSIGN` | cannot assign to `const` |
| `XR_ERR_ANALYZE_NOT_CALLABLE` | value is not callable |
| `XR_ERR_ANALYZE_WRONG_ARG_COUNT` | argument count mismatch |
| `XR_ERR_ANALYZE_ARG_TYPE` | argument type mismatch |
| `XR_ERR_ANALYZE_GENERIC_COUNT` | wrong number of type arguments |
| `XR_ERR_ANALYZE_GENERIC_CONSTRAINT` | type argument violates constraint |
| `XR_ERR_ANALYZE_SUPER_FIRST` | derived constructor's first line is not `super(...)` |
| `XR_ERR_ANALYZE_SUPER_THIS` | accessed `this` before `super(...)` |
| `XR_ERR_ANALYZE_SUPER_REQUIRED` | derived class did not call `super()` |
| `XR_ERR_ANALYZE_SUPER_INVALID` | non-derived class used `super()` |
| `XR_ERR_ANALYZE_CLOSURE_CAPTURE` | coroutine closure captured an unsafe variable |
| `XR_ERR_ANALYZE_AWAIT_TYPE` | `await` operand is not a `Task` |
| `XR_ERR_ANALYZE_MISSING_TYPE` | variable requires a type annotation or initializer |
| `XR_ERR_ANALYZE_ENUM_MIXED_TYPE` | enum members have mixed backing types |
| `XR_ERR_ANALYZE_INTERFACE_NOT_IMPLEMENTED` | class does not implement a declared interface |
| `XR_ERR_ANALYZE_TUPLE_FIELD_NAME` | tuple accessed with a non-numeric key |
| `XR_ERR_ANALYZE_TUPLE_FIELD_RANGE` | tuple field index out of range |
| `XR_ERR_ANALYZE_OVERRIDE_MISMATCH` | `override` did not match a same-name, same-signature instance method in the parent chain |
| `XR_ERR_ANALYZE_HASHABLE_CONTRACT` | type used as Map key / Set element lacks `operator==` / `hash` contract |
| `XR_ERR_ANALYZE_CONDITION_TYPE` | condition is not `bool` or nullable presence (`T?`, `T != bool`) |

### 18.4 Runtime Errors

#### Types and methods (E040x-E041x)

| Code | Name | Description |
|--|--|--|
| `E0401` | `XR_ERR_TYPE_NO_PROPERTY` | property does not exist on the type |
| `E0402` | `XR_ERR_TYPE_NO_INDEX` | type is not indexable |
| `E0403` | `XR_ERR_TYPE_NO_CALL` | value is not callable |
| `E0404` | `XR_ERR_TYPE_MISMATCH` | type mismatch |
| `E0405` | `XR_ERR_TYPE_NO_METHOD` | method does not exist on the type |
| `E0406` | `XR_ERR_TYPE_NO_OPERATOR` | type does not support the operator |

#### Null-related (E041x)

| Code | Name | Description |
|--|--|--|
| `E0410` | `XR_ERR_NULL_PROPERTY` | property access on null |
| `E0411` | `XR_ERR_NULL_INDEX` | indexing into null |
| `E0412` | `XR_ERR_NULL_CALL` | call on null |

#### Arithmetic (E042x)

| Code | Name | Description |
|--|--|--|
| `E0420` | `XR_ERR_DIV_BY_ZERO` | integer division by zero |
| `E0421` | `XR_ERR_MOD_BY_ZERO` | integer modulo by zero |
| `E0422` | `XR_ERR_OVERFLOW` | integer overflow |

#### Indexing/keys (E043x)

| Code | Name | Description |
|--|--|--|
| `E0430` | `XR_ERR_INDEX_OUT_OF_BOUNDS` | array / string / Bytes out of bounds |
| `E0431` | `XR_ERR_KEY_NOT_FOUND` | Map key not found |

#### Memory and stack (E044x)

| Code | Name | Description |
|--|--|--|
| `E0440` | `XR_ERR_STACK_OVERFLOW` | stack overflow |
| `E0441` | `XR_ERR_OUT_OF_MEMORY` | out of memory |

#### Call arguments (E045x)

| Code | Name | Description |
|--|--|--|
| `E0450` | `XR_ERR_WRONG_ARG_COUNT` | actual argument count mismatch |
| `E0451` | `XR_ERR_INVALID_ARG_TYPE` | actual argument type mismatch |

#### Coroutines (E046x)

| Code | Name | Description |
|--|--|--|
| `E0460` | `XR_ERR_CORO_DEAD` | operation on a dead coroutine |
| `E0461` | `XR_ERR_CORO_CANCELLED` | coroutine was cancelled |

### 18.5 Module Errors

| Code | Name | Description |
|--|--|--|
| `E0501` | `XR_ERR_MOD_NOT_FOUND` | module not found |
| `E0502` | `XR_ERR_MOD_LOAD_FAILED` | module load failed (I/O / parsing error) |
| `E0503` | `XR_ERR_MOD_NO_EXPORT` | imported name is not exported |
| `E0504` | `XR_ERR_MOD_CIRCULAR` | module dependency graph contains a circular dependency |

### 18.6 Rejected Syntax

> The parser rejects the following forms outright and reports the correct replacement.

| Code | Name | Rejected form | Correct form |
|--|--|--|--|
| `E0801` | `XR_ERR_SYN_RETURN_MULTI_REMOVED` | `return a, b` | `return (a, b)` |
| `E0802` | `XR_ERR_SYN_LET_MULTI_REMOVED` | `let x, y = ...` | `let (x, y) = ...` |
| `E0803` | `XR_ERR_SYN_FOR_FLAT_REMOVED` | `for k, v in m` (bare KV) | `for (k, v in m)` |
| `E0804` | `XR_ERR_SYN_VOID_REMOVED` | `-> void` | `-> ()` or omit the return type |

### 18.7 Error Handling (E082x)

| Code | Name | Description |
|--|--|--|
| `E0820` | `XR_ERR_THROW_NOT_EXCEPTION` | historical name preserved to avoid reuse; non-enum `throw` operands are now reported as `E0370` (see §8.1.1) |
| `E0821` | `XR_ERR_TRY_BANG_BAD_OPERAND` | deprecated (`try!` removed); code preserved to avoid reuse |
| `E0822` | `XR_ERR_TRY_BANG_NON_EXCEPTION_ERR` | deprecated (`try!` removed); code preserved to avoid reuse |
| `E0823` | `XR_ERR_MATCH_NOT_EXHAUSTIVE` | merged into `E0371` (see §6.3.3); code preserved to avoid reuse |
| `E0824` | `XR_ERR_UNWRAP_NON_EXCEPTION_ERR` | deprecated (`Result` removed); code preserved to avoid reuse |

### 18.8 Panic Error-Object Layout

Runtime faults in the panic channel use the prelude `Exception` class (declared in `stdlib/types/exception.xr`):

```xray
@native
class Exception {
    message: string             // human-readable message including error code and context
    stack: Array<string>        // auto-captured call stack, one formatted line per frame
    cause: Exception?           // chained cause
    code: int                   // error code (auto-parsed from "E0xxx: ..." prefix; default 0)
    data: Json?                 // optional structured data for a runtime fault

    constructor(message: string = "", cause: Exception? = null)
    fn toString() -> string
}
```

The static type of a user-level `throw` operand **must** be an enum variant value (see §8.1.1 / `E0370`). Structured business errors use ADT enums rather than `Exception` inheritance:

```xray
enum HttpErr {
    NotFound(string),
    ServerError(int, string),
    Timeout,
}

throw HttpErr.ServerError(500, "upstream failed")
```

`Exception` represents panic-channel runtime faults only; business errors propagate through the `throw <enum>` / `catch` value-return channel (see §8.1).

---

## Appendix A. EBNF Grammar

> Source of truth: `src/frontend/parser/xparse_*.c`. This appendix is a compact, curated EBNF; the parser implementation is the authoritative resolver of any conflicts.

### A.1 Lexical Layer

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
RawStringLiteral ::= 'r' '"' [^"]* '"'
CharLiteral ::= "'" CharBody "'"
CharBody ::= UnicodeScalar | EscapeSeq | '\u{' HexDigit{1,6} '}'
RegexLiteral ::= '/' RegexBody '/' RegexFlags?

BoolLiteral ::= 'true' | 'false'
NullLiteral ::= 'null'
```

### A.2 Types

```ebnf
Type ::= UnionType
UnionType ::= IntersectionType ('|' IntersectionType)*
IntersectionType ::= NullableType
NullableType ::= PrimaryType '?'?
PrimaryType ::= FFIPointerType | CFunctionType | NamedType | FunctionType | TupleType | ObjectType
FFIPointerType ::= ('RawPtr' | 'RawMut') '<' Type '>'
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

TernaryExpr ::= LogicOrExpr ('?' Expression ':' Expression)?
LogicOrExpr ::= LogicAndExpr ('||' LogicAndExpr)*
            |   NullCoalesce
LogicAndExpr ::= BitOrExpr ('&&' BitOrExpr)*
NullCoalesce ::= LogicAndExpr ('??' LogicAndExpr)*
BitOrExpr   ::= BitXorExpr ('|' BitXorExpr)*
BitXorExpr  ::= BitAndExpr ('^' BitAndExpr)*
BitAndExpr  ::= EqualityExpr ('&' EqualityExpr)*
EqualityExpr ::= RelationalExpr (('==' | '!=') RelationalExpr)*
RelationalExpr ::= ShiftExpr (('<' | '<=' | '>' | '>=') ShiftExpr)*
ShiftExpr   ::= AdditiveExpr (('<<' | '>>') AdditiveExpr)*
AdditiveExpr ::= MultiplicativeExpr (('+' | '-') MultiplicativeExpr)*
MultiplicativeExpr ::= TypeOpExpr (('*' | '/' | '%') TypeOpExpr)*
TypeOpExpr  ::= UnaryExpr (('as' | 'is') Type)*           // safe cast is `x as T?` where T? is a nullable type
RangeExpr   ::= AdditiveExpr ('..' AdditiveExpr)?

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
// use `fn(p: T) -> R { ... }` or annotate the binding (`let f: (T) -> R = ...`) instead.

MatchExpr ::= 'match' '(' Expression ')' '{' MatchArm (','? MatchArm)* ','? '}'
MatchArm  ::= Pattern ('if' '(' Expression ')')? '->' (Expression | Block)

ThrowExpr   ::= 'throw' Expression                // operand's static type must be an enum variant

ArgList ::= Expression (',' Expression)* ','?
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
RangePattern    ::= Expression '..' Expression
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
           // Note: print/dump are calls inside ExprStmt; go is an expression (GoExpr)

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

// print is a normal global function call, syntactically an ExprStmt.

// go is an expression returning Task<T>. It is not a separate statement category (it appears wrapped in ExprStmt).

ScopeStmt ::= 'scope' Block            // lexical scope + structured concurrency

SelectStmt ::= 'select' '{' SelectArm+ '}'
SelectArm  ::= Identifier 'from' Expression '->' Block      // receive
            |  Expression 'to' Expression '->' Block        // send
            |  'after' Expression '->' Block                // timeout
            |  '_' '->' Block                                // default

YieldStmt ::= 'yield'
```

### A.6 Declarations

```ebnf
VarDecl ::= ('let' | 'const' | 'shared' ('const' | 'let')) Binding (',' Binding)*
Binding ::= BindingPattern (':' Type)? ('=' Expression)?
BindingPattern ::= Identifier
                |  '[' BindingPattern (',' BindingPattern)* ','? ']'
                |  '(' BindingPattern (',' BindingPattern)+ ','? ')'
                |  '{' ObjectBinding (',' ObjectBinding)* ','? '}'
ObjectBinding ::= Identifier (':' Identifier)?

FnDecl ::= AttrList? Modifier* 'fn' Identifier TypeParams? '(' ParamList? ')' ReturnType? FnBody
FnBody ::= Block | ';'?                         // empty body is only allowed for @extern
ParamList ::= Param (',' Param)* ','?
Param     ::= Modifier* Identifier ':' Type ('=' Expression)?
           |  '...' Identifier ':' Type
ReturnType ::= '->' Type | '->' '(' Type (',' Type)+ ')'
Modifier  ::= 'in' | 'ref' | 'private' | 'protected' | 'static' | 'const' | 'final' | 'abstract' | 'override'
              // public visibility is the default; override is optional

TypeParams ::= '<' TypeParam (',' TypeParam)* ','? '>'
TypeParam  ::= Identifier (':' Type ('&' Type)*)?         // constraints use ':', multiple use '&'
AliasTypeParams ::= '<' Identifier (',' Identifier)* ','? '>'

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
                |  Identifier '=' BackingValue                  // simple enum (no payload)
EnumMethod     ::= 'fn' Identifier TypeParams? '(' ParamList? ')' ReturnType? Block
VariantPayload ::= '(' VariantField (',' VariantField)* ')'
VariantField   ::= (Identifier ':')? Type
BackingValue   ::= IntLiteral | FloatLiteral | StringLiteral | BoolLiteral

TypeAliasDecl ::= 'type' Identifier AliasTypeParams? '=' Type

ImportDecl ::= 'import' ImportMembers 'from' ImportModule
            |  'import' ImportModule ('as' Identifier)?
ExportDecl ::= 'export' Declaration                                         // export the declaration directly
            |  'export' Identifier                                          // export an already-declared identifier
            |  'export' '*' 'from' StringLiteral                            // forwarding export
ImportMembers ::= '{' ImportMember (',' ImportMember)* ','? '}'
ImportMember  ::= Identifier ('as' Identifier)?
ImportModule  ::= StringLiteral | Identifier ('/' Identifier)?

AttrList ::= ('@' Identifier ('(' ArgList? ')')?)*  // e.g. @extern("C"), @dylib("m"), @c_export("sym"), @repr(C)

OperatorToken ::= '+' | '-' | '*' | '/' | '%'
               |  '&' | '|' | '^'
               |  '==' | '!=' | '<' | '<=' | '>' | '>='
               |  '[]' | '[]='
               |  '!' | '~'
```

> Note: this EBNF is curated for guidance. Precedence, associativity, and disambiguation are determined by the parser implementation; in case of ambiguity, treat `src/frontend/parser/xparse_*.c` as authoritative.

---

## Appendix B. Keyword Index

The full set of 63 reserved keywords sorted alphabetically; see [§1.5](#15-keywords) for the authoritative list.

| Keyword | Section |
|--|--|
| `abstract` | §5.3 |
| `as` | §3.8 |
| `await` | §10.3 |
| `bool` | §2.3.3 |
| `break` | §4.6 |
| `catch` | §8 |
| `char` | §2.3.5 |
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
| `private` | §5.3 |
| `protected` | §5.3 |
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
| `unsafe` | §3.2 |
| `while` | §4.3 |
| `yield` | §3.16 / §10.10 |

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
| Other | `..` `..=` `??` `?.` `?[` `!` `->` |

---

## Appendix D. Standard Library Module Index

The full set of 22 native modules is documented in [§15](#15-standard-library-overview).

| Module | Purpose |
|--|--|
| `base64` | Base64 encode/decode |
| `cluster` | distributed cluster |
| `compress` | compression (gzip/zlib/deflate) |
| `crypto` | cryptographic hashes |
| `csv` | CSV parsing/serialization |
| `datetime` | date and time |
| `encoding` | character encoding conversion |
| `mem` | memory and cycle-collection introspection |
| `http` | HTTP/REST |
| `io` | file I/O |
| `log` | structured logging |
| `math` | math functions |
| `net` | TCP/UDP/TLS |
| `os` | operating system |
| `path` | path manipulation |
| `regex` | regular expressions |
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
| Truthiness | truthy / falsy | truthy / falsy (similar to JS), but `bool` itself rejects implicit assignment from int/null |
| Equality | `===` is strict, `==` is weak (string↔number coercion) | Only `==`/`!=`; value equality only promotes numeric int↔float, and `===`/`!==` are not operators |
| Closure capture | by reference | by reference (default); `go` closures are strictly restricted |
| Objects | dynamic fields | dynamic by default; sealed once annotated `type T = {...}` |
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
| GC | concurrent tri-color | per-coroutine Mark-Sweep / Immix |
| Classes / inheritance | none (struct + interface only) | classes with inheritance |
| Generics | since 1.18 | yes, monomorphization + runtime reified |

### E.3 vs Rust

| Dimension | Rust | xray |
|--|--|--|
| Memory safety | full borrow checker | only `move` across coroutines; otherwise GC |
| Errors | `Result<T, E>` | value-return error channel (`throw` / `catch`) |
| Type inference | strong Hindley-Milner | bidirectional inference |
| Traits | full | similar to `interface`, fewer features |
| Performance | near C | VM/JIT, hot paths near native |
| Compile-time | macros / const | simple constant folding |

### E.4 vs Python

| Dimension | Python | xray |
|--|--|--|
| Typing | dynamic (optional hints) | static |
| GIL | yes | none (M:N coroutines) |
| Strings | unicode str | utf-8 string |
| Indentation | mandatory | free-form (`{}`) |
| Classes | dynamic attributes | static fields |
| Performance | CPython slow | JIT close to V8/JVM |

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
| **AOT** | Ahead-of-Time compilation: precompiles to machine code at build time |
| **AST** | Abstract Syntax Tree: intermediate representation produced by the parser |
| **Arena** | Bulk allocator: every allocation is freed together |
| **Bytes** | Byte buffer type (see §2.4.5) |
| **char** | Primitive type for one Unicode scalar value; not numeric and not an alias of `uint32` (see §2.3.5) |
| **Channel** | Typed inter-coroutine communication pipe (see §10.5) |
| **closure** | Function value that captures outer variables |
| **coroutine** | User-space, suspendable/resumable execution flow |
| **defer** | Deferred execution: runs before function exit (see §4.9) |
| **enum** | Enumeration type (see §5.6) |
| **GC** | Garbage Collector |
| **GC-safepoint** | Instruction location at which the GC may safely begin |
| **goroutine** | Equivalent of xray coroutine; launched via `go {...}` |
| **hoisting** | Implicit declaration of a name before its first use |
| **IC** | Inline Cache: optimization of property/method dispatch |
| **interface** | Interface type (see §5.5) |
| **JIT** | Just-In-Time compilation: compiles hot paths at runtime |
| **lvalue / rvalue** | Assignable left-hand-side value vs. value-only right-hand-side |
| **monomorphization** | Build-time specialization of generics into concrete type/representation versions; generic functions may share I64 / F64 / PTR / BOOL representation versions, while generic classes / structs are fully specialized by concrete type |
| **move** | Ownership transfer: enforced when crossing coroutine boundaries (see §7.3) |
| **nullable** | A nullable type `T?` whose value may be `null` |
| **pattern** | A pattern used in `match` and destructuring (see §6) |
| **scope** | Lexical scope |
| **shared** | Storage class for cross-coroutine sharing (see §5.1.3) |
| **SSA** | Static Single Assignment: IR where each variable is assigned only once |
| **struct** | Value-type class (see §5.4) |
| **TCO** | Tail-Call Optimization |
| **trait** | Rust terminology; xray uses `interface` |
| **condition expression** | Control-flow condition: must be `bool` or nullable presence `T?` (`T != bool`); see §2.3.3 |
| **grapheme cluster** | User-perceived character that may contain multiple Unicode scalars; current `string.length` / indexing / iteration operate on Unicode scalars, not grapheme clusters |
| **union** | Union type `A \| B` |
| **Unicode scalar value** | Legal Unicode code point in `U+0000..U+10FFFF`, excluding the surrogate range `U+D800..U+DFFF` |
| **upvalue** | Outer variable captured by a closure |
| **VM** | Virtual Machine: xray bytecode VM |
| **write barrier** | Hook inserted by the GC on pointer updates |

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
| **Error handling** | Exception machinery (throw / try / catch / finally) + `Result<T,E>` + nullable types (T?) + `defer`-based resource management |
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

Xray supports two kinds of comments, **neither of which nests**:

```xray
// line comment, from // to end-of-line
/* block comment,
   may span lines;
   does not nest: an inner /* does not start a new layer */
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
| `new` | instantiation |
| `this` `super` | self / parent reference |
| `constructor` | constructor |
| `static` `private` `public` | visibility modifiers (`public` is the **default** and is almost never written explicitly) |
| `abstract` `final` `override` | class/method modifiers (`override` is **optional** — overriding a parent method does not require an explicit annotation) |
| `operator` | operator overloading |
| `is` `as` | runtime type check / cast |

#### 1.5.3 Exception Handling

`try` `catch` `finally` `throw`

#### 1.5.4 Module System

`import` `export`

#### 1.5.5 Coroutines and Concurrency

`go` `await` `select` `defer` `scope`

#### 1.5.6 Type Names (reserved)

`int` `int8` `int16` `int32` `int64` `uint8` `uint16` `uint32` `uint64`
`float` `float32` `float64` `bool` `string` `unknown`

> **Note**: the following names are **not** lexer keywords; they are built-in type symbols automatically introduced by the prelude:
> `Array` · `BigInt` · `Bytes` · `Channel` · `DateTime` · `Exception` · `Json` · `Logger` · `Map` · `NetConn` · `NetListener` · `Range` · `Regex` · `Set` · `StringBuilder`.
> `Result<T, E>` is the built-in ADT enum used by current error-handling paths and `catch!`; it is also directly available.
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

`==` `!=` `===` `!==` `<` `<=` `>` `>=`

- `==` `!=`: value equality (with implicit numeric promotion: int→float).
- `===` `!==`: strict equality (type + value; no promotion).
- `<` etc.: supported by numbers and strings; not supported by other types.

#### 1.7.5 Logical

`&&` `||` `!`

Short-circuit evaluation.

#### 1.7.6 Assignment

`=` `+=` `-=` `*=` `/=` `%=` `&=` `|=` `^=` `<<=` `>>=`

#### 1.7.7 Increment / Decrement

`++` `--`

Only the **postfix** form `x++` / `x--` is supported; the prefix form `++x` / `--x` is a compile error. See §3.2.

#### 1.7.8 Type-related

| Token | Use |
|--|--|
| `?` | nullable type (`T?`), ternary, optional-chain prefix |
| `?.` | optional chain (`obj?.prop`) |
| `??` | null coalescing (`a ?? b`) |
| `!` | force unwrap (postfix, `expr!`) / logical not (prefix) |
| `\|` | union type (`int \| string`) / bitwise or |
| `->` | unified arrow: function return type, function type, closures, `match` / `select` arms |
| `...` | rest / spread |
| `..` | range (`0..10`) |
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
| Primitive | `int`, `float`, `bool`, `string`, `()` (Unit, no return value) |
| Sized integers | `int8`, `int16`, `int32`, `int64`, `uint8`..`uint64` |
| Sized floats | `float32`, `float64` |
| Containers | `Array<T>`, `Map<K,V>`, `Set<T>`, `Channel<T>`, `Bytes` (equivalent to `Array<uint8>`) |
| Special | `Json`, `BigInt`, `Range`, `DateTime`, `Regex`, `StringBuilder`, `Logger`, `NetConn`, `NetListener` |
| Error-handling prelude | `Exception`, `Result<T, E>` (see §8) |
| Weak containers | `WeakMap`, `WeakSet` |
| Nullable | `T?` |
| Union | `A \| B \| ...` |
| Tuple | `(T1, T2, ...)` |
| Function | `fn(T1, T2) -> R` |
| Class / Struct / Interface | user-defined (nominal) |
| Enum | user-defined (incl. ADT enum, see §5.6) |
| Type alias | `type Name = SomeType` |

### 2.3 Primitive Types

#### 2.3.1 Integer Types

| Type | Range | Alias |
|--|--|--|
| `int8` | `[-128, 127]` | — |
| `int16` | `[-32768, 32767]` | — |
| `int32` | `[-2³¹, 2³¹-1]` | — |
| `int64` | `[-2⁶³, 2⁶³-1]` | `int` (default integer type) |
| `uint8`..`uint64` | unsigned counterparts | — |

- Literals default to `int`; the type may be narrowed by context (e.g., assigned to an `int32` variable).
- Arithmetic: two's-complement wrap-around semantics (no debug/release distinction).

#### 2.3.2 Floating-Point Types

| Type | Standard |
|--|--|
| `float32` | IEEE-754 single precision |
| `float64` | IEEE-754 double precision; alias of `float` |

Literals default to `float`.

#### 2.3.3 `bool`

`true` / `false`, a standalone type. **No implicit conversion** to/from numeric types (cannot write `let x: int = true` or `let b: bool = 1`).

**Truthy / falsy context** (applies only at control-flow positions such as `if` / `while` / `?:` / `??` / `&&` / `||`; **does not** change a variable's type):

| Value | Treated as |
|---|---|
| `false`, `null`, `0`, `0.0`, `""`, `Bytes(0)`, empty array / empty Map | **falsy** |
| Everything else (including `0.0001`, non-empty strings/collections, object references) | **truthy** |

```xray
let x: int? = maybe_int()
if (x) {                  // truthy context: enters when x is neither null nor 0
    print(x + 1)          // x is narrowed to int in this branch
}

let s: string = ""
if (s) { ... } else { ... }    // falsy: enters else

let m: Map<string, int> = #{}
if (m) { ... }                  // falsy: empty Map

let a: int? = null
let b = a ?? 0                  // null coalescing: b = 0
```

**Note**: explicit comparisons such as `x is T` and `x != null` are preferred; truthy/falsy is mainly for concise "existence" checks (such as `if (user)`).

#### 2.3.4 `string`

Immutable UTF-8 strings. Supports `length`, indexing, slicing, and a rich method set (see §14.2).

Internally uses ARC + string interning optimizations.

#### 2.3.5 Unit `()` (no return value)

Xray uses the **0-tuple `()`** to represent "no return value" (the Unit type):

```xray
fn log(msg: string) -> () { print(msg) }   // explicit Unit return
fn ping() { print("pong") }                  // omitted return type = ()
let r: () = log("hi")                        // allowed; r is a Unit value
```

- A function omitting its return type is equivalent to `-> ()`.
- `void` is not a type name: `fn f() -> void` is rejected (`E0804`); use `-> ()` or omit the return type to indicate no return value.

### 2.4 Composite Types

#### 2.4.1 `Array<T>`

Ordered mutable array. See §14.1.

```xray
let a: Array<int> = [1, 2, 3]
let b = [1, 2, 3]                // inferred as Array<int>
let c: Array<string> = []         // explicit empty array
```

The `T` in `Array<T>` must be determinable at compile time. An empty `[]` without a type annotation is a compile error: `Empty array '[]' requires a type annotation`.

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

`K` must implement `Hashable` (see §14.14): typically `int`, `string`, `bool`, `enum`, or a custom class implementing `Hashable`.

#### 2.4.3 `Set<T>`

Deduplicated collection. See §14.4.

```xray
let s: Set<int> = #[1, 2, 3]
```

#### 2.4.4 `Channel<T>`

Inter-coroutine communication channel. **Must** be declared `const` (see §10.5).

```xray
const ch: Channel<int> = new Channel<int>(10)
```

#### 2.4.5 `Bytes`

Typed byte buffer. Semantically equivalent to `Array<uint8>`, but stored as contiguous memory.

```xray
let buf = new Bytes(1024)
let init = new Bytes([72, 101, 108, 108, 111])
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
```

Aliases are **purely syntactic** equivalences; they do not introduce new types.

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
| 17 | `(...)` `[...]` `.x` `?.x` `f()` `e!` | left | postfix: grouping, index, member, optional chain, call, force unwrap |
| 16 | prefix `-` `+` `!` `~` `new` `move` `await` `go` | right | unary prefix + coroutine operators (`++` / `--` are postfix only) |
| 15 | `as` `is` | left | type cast / check (`as T?` is the safe form via a nullable target type, not a separate `as?` operator) |
| 14 | `*` `/` `%` | left | multiplication / division / modulo |
| 13 | `+` `-` | left | addition / subtraction |
| 12 | `<<` `>>` | left | shifts |
| 11 | `<` `<=` `>` `>=` | left | relational |
| 10 | `==` `!=` `===` `!==` | left | equality |
| 9 | `&` | left | bitwise AND |
| 8 | `^` | left | bitwise XOR |
| 7 | `\|` | left | bitwise OR (also union types) |
| 6 | `&&` | left | logical AND (short-circuit) |
| 5 | `\|\|` | left | logical OR (short-circuit) |
| 4 | `??` | left | null coalescing |
| 3 | `..` | left | range |
| 2 | `? :` | right | ternary |
| 1 | `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` | right | assignment and compound assignment |
| 0 | `,` (only in `match` multi-value arms, argument lists, etc.) | — | not a real operator |

Implementation: Pratt-parser style in `src/frontend/parser/xparse_expr.c`.

### 3.2 Unary Expressions

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

| Operator | Applicable types | Result type | Notes |
|--|--|--|--|
| `-x` | numeric | same | negation; preserves float NaN |
| `+x` | numeric | same | identity, almost never useful |
| `!x` | `bool` | `bool` | logical not; **rejects non-bool** (unlike JS) |
| `~x` | integer | same | bitwise complement |
| `x++` `x--` | integer | same | postfix inc/dec; returns the **updated** value (unlike C/Java; essentially the result of the assignment `x = x + 1`) |

**Inc/dec semantics**:
- Xray provides **only postfix** `x++` / `x--`; prefix `++x` / `--x` is a compile error ("prefix ++/-- not supported, use postfix form").
- Equivalent to the assignment `x = x + 1` / `x = x - 1`; the expression value is the new value after assignment.
- Cannot be inlined within a binary expression (`a + x++`, `f(x++)`, `x++ + 1` etc.); the parser reports "++/-- must be standalone statement". `let y = x++` is allowed (assignment on the immediate left), and `y` receives the post-update value.
- Applies only to lvalues (variables, fields, indexes).
- Floating-point values **do not support** `++`/`--` (compile error).

### 3.3 Binary Expressions

```ebnf
BinaryExpr ::= UnaryExpr (BinOp UnaryExpr)*
BinOp ::= '+' | '-' | '*' | '/' | '%'
       | '&' | '|' | '^' | '<<' | '>>'
       | '<' | '<=' | '>' | '>='
       | '==' | '!=' | '===' | '!=='
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
| `%` | int | float (IEEE remainder) | float | ❌ | ❌ |

**Special semantics**:
- `int / 0` → throws `XR_ERR_DIV_BY_ZERO` (E0420) at runtime.
- `int % 0` → throws `XR_ERR_MOD_BY_ZERO` (E0421) at runtime.
- `float / 0.0` → `+inf` / `-inf` / `NaN` (IEEE-754 semantics; **does not** throw).
- Integer overflow: see §2.3.1.
- `string + string` is O(n) concatenation; for heavy concatenation use `StringBuilder`.

#### 3.3.2 Bitwise Operators

`&` `|` `^` `~` `<<` `>>`

- Apply only to integer types.
- Shift counts are taken modulo 64 (unlike C: always defined in xray).
- `>>` is an **arithmetic right shift** (preserves the sign bit). For unsigned shifts, use the corresponding `uintN`.
- `bool` does not participate in bitwise operations (use `&&` `||`).

#### 3.3.3 Comparison Operators

| Operator | Semantics |
|--|--|
| `==` | value equality. `int` and `float` are comparable (with int→float implicit promotion). Strings compare by content. class/struct uses `==` overload or default identity. |
| `!=` | inverse of `==` |
| `===` | **strict** equality: both type and value must match; no implicit conversion. |
| `!==` | inverse of `===` |
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

### 3.6 Null Coalescing `??` and Optional Chaining `?.`

See §3.3.5 (`??`) and below (`?.`).

#### Optional chaining `?.`

```ebnf
OptionalChain ::= Primary ('?.' (Identifier | '[' Expr ']'))+
```

```xray
let len = name?.length          // returns null when name is null
let item = arr?.[0]             // optional index
```

**Semantics**:
- If the LHS of `?.` is `null`, the entire expression short-circuits to `null`.
- **Propagation**: in `a?.b.c.d`, if `a` is null the whole chain returns null; intermediate `.` operations are not re-checked.
- Result type: the original type plus `?` (already-nullable types remain unchanged).
- Currently `?.` supports property and index access; optional function call `func?.()` is not part of the current grammar.

### 3.7 Force Unwrap `!` / `try?` / `try!` / `catch!`

> Full error-handling semantics are in §8. This section only lists the expression syntax and brief semantics.

#### Force unwrap `expr!`

```xray
let v: int = nullable_int!      // throws NullThrowError (E0410) at runtime when null
```

Legal only when `expr` is known to be a nullable type (`T?`) at compile time; using `!` on a non-null `T` is a compile error.

#### `try?` expression — collapse failure to null

```ebnf
TryOptional ::= 'try?' Expression
```

`try? e` collapses any failure to `null`, discarding the cause:

| Static type of `e` | Type of `try? e` | Behavior |
|--|--|--|
| `Result<T, E>` | `T?` | `Err` → `null`; `Ok(v)` → `v` |
| `T` (ordinary type, call may throw) | `T?` | thrown → `null`; otherwise → `v` |
| `T?` | `T?` | passes through |

```xray
let n: int? = try? parseInt(input)         // Result.Err → null
let v: Json? = try? http.get(url).json()   // thrown → null
```

#### `try!` expression — early return or cross-track promotion

```ebnf
TryForce ::= 'try!' Expression
```

The static type of the expression following `try!` **must** be `Result<T, E>` or `T?` (other types are a compile error `E0821`). Behavior is double-dispatched by expression type + the current function's return type:

| `e` type | Current function return | Failure behavior | Success value |
|--|--|--|--|
| `Result<T, E>` | `Result<_, E>` | early return `return Result.Err(e)` | `v` |
| `Result<T, E>` | other | `throw e` (requires `E` to be `Exception`-derived) | `v` |
| `T?` | `_?` | early return `return null` | `v` |
| `T?` | other | `throw new NullThrowError(...)` | `v` |

```xray
fn pipeline(s: string) -> Result<int, ParseError> {
    let n = try! parseInt(s)      // Err early-returns Err
    return Result.Ok(n + 1)
}
```

Full details in §8.3.1. **`try!` is not a mandatory ceremony for exception-throwing calls** — xray does not require `try!` before every call that might throw.

#### `catch!` block expression — condense an exception block into a Result

```ebnf
CatchBlock ::= 'catch!' Block
```

`catch! { ... }` wraps any exceptions that might escape the block into a `Result<T, Exception>`:

```xray
let r: Result<int, Exception> = catch! {
    let x = riskyOp()
    let y = another(x)
    return y + 1                   // a return inside is a "return from the catch! block"
}
```

- A `return v` inside the block returns from the `catch!` block (does not affect the outer function).
- The block's last expression is taken as the `Ok(v)` payload.
- **Any exception** escaping the block is wrapped as `Err(e)`; `e` is statically typed `Exception`.
- Type filtering is not supported — when needed, use a handwritten `try/catch`.

Full details in §8.3.3.

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

### 3.9 Range `..` and Spread `...`

#### Range `a..b`

```ebnf
RangeExpr ::= AddExpr ('..' AddExpr)?
```

```xray
0..10                  // 0..10, left-closed right-open (includes 0, excludes 10)
let r = 1..100
for (i in 0..n) { ... }
```

- Type: `Range` (int ranges only).
- Half-open interval `[a, b)`: `a` is included, `b` is not. `for-in`, `Range.includes`, `Range.length`, `Range.toArray()`, and the `a..b` pattern in `match` all share the same semantics.
- Primary uses: `for-in` loops, range checks in pattern matching.
- The closed-interval syntax (`a..=b`) is not currently provided; to include `b`, write `a..(b+1)`.

#### Spread `...`

Allowed in the following positions only:
- **Function rest parameter declaration**: `fn f(...args: int)`
- **Function call spread**: `f(...args)`; the spread source must be a tuple whose arity is statically known.
- **Tuple literal spread**: `(head, ...tail)`; the spread source must be a tuple whose arity is statically known.

### 3.10 Literal Construction

#### Array `[...]`

```ebnf
ArrayLit ::= '[' (Expr (',' Expr)* ','?)? ']'
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
```

```xray
let p = { name: "Alice", age: 30 }
let users = "Bob"
let obj = { users }              // shorthand
```

- Defaults to an **extensible** structured object type (see §2.4.6 / §2.10 Json behavior).
- Fix the structure with a `type` alias: `let u: User = {...}` (compile-time field check, sealed).

#### Bytes `new Bytes(...)`

See §2.4.5 and §14.5.

#### Channel `new Channel<T>(buf?)`

```xray
const ch: Channel<int> = new Channel<int>(10)
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
str[i]                  // returns a single-character string
```

- `Array` indexing: `int`; out-of-bounds throws `E0430`.
- `Map` indexing: key type; missing key → `E0431`.
- `string` indexing: returns a length-1 string (**not** a char/int).
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
- `string` slicing supports negative indexes (counting from the end); for `Array`, a negative `start` is clamped to `0` and a negative `end` is treated as the array length.
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

// ── Arrow lambda: any position, supports multi-param and type annotation ──
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
| Arrow lambda | `(x, y) -> expr` | multi-param, type annotation, or non-call positions |
| fn expression | `fn(x: T) -> R { ... }` | multi-statement body, return-type annotation, generics |

**Key rules**:
- **Bare lambda** (`x -> expr`): restricted to **call-argument position**; the single parameter is unparenthesized. The parameter type is inferred from the callee signature or the container element type.
- **Arrow lambda** (`(x) -> expr`, `(x, y) -> expr`): usable in any position. Parameter types may be omitted and inferred from context; inference failure raises `E0365`.
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
let result = match x {
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

**Used for**:
- Class and struct instantiation.
- Constructing built-in container types (`Array` / `Map` / `Set` / `Channel` / `Bytes` / `StringBuilder`, etc.).

**Relation to literals**:
```xray
let a = [1, 2, 3]              // equivalent to new Array<int>() + push
let m = #{}                    // equivalent to new Map<...>()
let p = Point{x: 1, y: 2}      // struct literal
```

### 3.15 String Interpolation

See §1.6.5. In brief:

```xray
"Hello, ${name}! Age: ${user.age + 1}"
```

- `${...}` accepts any expression (calls, object access, arithmetic).
- Embedded string literals inside the interpolation require escaped quotes or a switch to single-quoted outer strings.
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
ExprStmt ::= Expression (';' | LineBreak)
Block    ::= '{' Statement* '}'
```

```xray
foo()                  // expression statement
x = 1                  // assignment expression as a statement
{                      // block
    let y = 2
    y + 1              // expression with discarded result
}
```

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
- The condition is evaluated under truthy/falsy context (see §2.3.3); explicit `bool` expressions or comparisons such as `x != null` / `x is T` are recommended for readability.
- Branch bodies must be blocks `{...}`; **no** single-statement-without-braces form.
- `if` is not an expression; for an expression form use the ternary `? :` or `match`.

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

There is no `do-while` form.

### 4.4 `for` (C-style) and `for-in`

#### C-style `for`

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

- Variables declared in `ForInit` are scoped to the loop body.
- All three sections may be omitted: `for (;;)` is an infinite loop.

#### Single-variable `for-in`

```ebnf
ForInStmt ::= 'for' '(' Identifier 'in' Expression ')' Block
```

```xray
for (item in [1, 2, 3]) { print(item) }
for (i in 0..n) { print(i) }                  // range iteration (half-open)
for (ch in "hello") { print(ch) }             // string characters (by codepoint)
for (key in someMap) { print(key) }           // single variable over Map → key
for (key in someJson) { print(key) }          // single variable over Json → key
for (day in Color) { print(day.name) }        // enum iteration (declaration order)
for (_ in 0..n) { count++ }                   // discard with placeholder
```

#### Two-variable `for-in` destructuring

Xray supports two equivalent two-variable forms:

```ebnf
ForInPairStmt ::= 'for' '(' Identifier ',' Identifier 'in' Expression ')' Block
              |  'for' '(' '(' Identifier ',' Identifier ')' 'in' Expression ')' Block
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
| `string` | char (1-codepoint string) | (index, char) |
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
continue               // proceed to the next iteration
```

**Constraints**:
- Must appear inside a `while` / `for`; otherwise the compile errors `E0304` / `E0305`.
- `break` / `continue` inside a `match` does **not** affect `match` itself; it exits the enclosing loop.
- **No labeled** break/continue (unlike Java/Rust).

### 4.7 `return`

```ebnf
ReturnStmt ::= 'return' ReturnValue? (';' | LineBreak)
ReturnValue ::= Expression | '(' Expression (',' Expression)+ ')'
```

```xray
return                 // implicitly returns () (Unit)
return 42
return (a, b)          // multi-value return must wrap a tuple in parens
```

> **Note**: multi-value returns must use the tuple form `return (a, b)`; the bare-comma form `return a, b` is the compile error `E0801`.

**Constraints**:
- Allowed only inside a function body (including closures); a top-level `return` is the compile error `E0306`.
- The returned value's type must be compatible with the function's declared return type.

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

throw new Exception("error message")        // ✅ Exception-derived
throw new HttpError(500, "internal")        // ✅ user Exception subclass
// throw "msg"                              // ❌ E0370: must be Exception-derived
```

**Semantics**:
- A `try` must be followed by at least one of `catch` or `finally`.
- The `e` in `catch (e)` defaults to type `Exception`; a type annotation `catch (e: HttpError)` performs type filtering; multiple `catch` clauses match in declaration order.
- `finally` is **always executed**, regardless of whether an exception was thrown or `return` was used.
- The static type of the `throw` operand must be `Exception`-derived (`E0370`).
- For full exception semantics see [§8](#8-error-handling).

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
- Runs at the **end of the function scope** (not the block scope, unlike Swift).
- **LIFO**: multiple `defer` statements run in reverse declaration order.
- **Always executes**: runs even if the function exits via an exception (similar to `finally`).
- Difference from `finally`: `defer` is bound to the function scope, `finally` is bound to a `try` block.
- An exception in a `defer` body **replaces** any in-flight exception (Go-style semantics).

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
         | '{' Identifier (',' Identifier)* ','? '}'            // object destructure
```

#### 5.1.1 `let` — mutable binding

```xray
let x = 1                         // type inferred as int
let name: string = "Alice"        // explicit type
let count: int                    // no initializer: zero value used
```

- Reassignable.
- Must have an initializer **or** a type annotation; otherwise compile error `E0303`.
- Without an initializer, the value defaults to the type's zero value (`int` → `0`, `string` → `""`, `bool` → `false`, `T?` → `null`).

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
shared let buffer = new Bytes(1024)
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

// object destructuring (extract by name; **no** rename syntax)
let { name, age } = { name: "Alice", age: 30 }
```

Constraints:
- The number of destructured bindings must match (except with rest patterns).
- Object destructuring accepts only an `Identifier` list; `{ name: localName }` style renaming is **not** supported.

### 5.2 `fn` function declaration

```ebnf
FnDecl ::= AttrList? Modifier* 'fn' Identifier TypeParams? '(' ParamList? ')' ReturnType? Block
ParamList ::= Param (',' Param)*
Param     ::= Modifier* Identifier ':' Type ('=' DefaultValue)?
            | '...' Identifier ':' Type
Modifier  ::= 'in' | 'ref'
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

- Default values are evaluated at the callee entry; if the caller omits an argument, `null` is passed and the entry replaces it with the default expression.
- Parameters with default values must appear consecutively at the tail of the parameter list.

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
Modifier ::= 'private' | 'public' | 'static' | 'final' | 'abstract' | 'override'
```

> **About `public` and `override`**: both modifiers **are valid keywords lexically**, but in practice they **are almost never written**:
>
> - `public` is the **default visibility**—every field/method without `private` is public, so writing `public` explicitly is redundant.
> - `override` is **optional**—an override happens automatically when the derived class declares a method with the same signature; an explicit `override` annotation is not required.
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
        return new Animal(name)
    }
}

let a = new Animal("Rex")
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
- **Overriding requires no keyword**—any subclass method with the same name and signature automatically overrides the parent (the `override` modifier exists but is **optional**).
- A `final class` cannot be inherited.
- A `final` method cannot be overridden.
- An `abstract` method **must** be implemented by subclasses (unless the subclass is also `abstract`).
- `super.method()` invokes the shadowed parent method from inside an override.

#### 5.3.3 Modifiers

| Modifier | Applies to | Semantics |
|--|--|--|
| (none) | field/method | Default public—externally visible |
| `public` | field/method | **Redundant**—same as default; never written in practice |
| `private` | field/method | Class-internal access only; subclasses cannot access directly but may go through public parent methods |
| `static` | field/method | Class-level, not part of an instance; called as `ClassName.method()` |
| `final` | class/method/field | Class: cannot be inherited. Method: cannot be overridden. Field: cannot be reassigned after initialization |
| `abstract` | class/method | Cannot be instantiated / must be implemented by subclasses |
| `override` | method | **Optional**—overrides do not require explicit annotation; documenting only |

**Modifiers may combine**: `private final secret: string = "key123"`, `static final pi() -> float`, `private static counter: int = 0`.

xray has **no** `protected` modifier—subclasses go through public parent methods to reach private fields when needed.

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
- A `struct` may have **no** constructor (`new Point()` produces a zero-initialized instance which is then assigned manually; see §5.4).

#### 5.3.5 Operator overloading

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

**Cannot** be overloaded: `&&` `\|\|` `=` `?.` `?:` `??` `,` `.`

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
let p = new Point()                  // default-construct (zero-valued fields), then assign
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
| `static` / `private` / `final` | ✅ | ✅ |
| Operator overload | ✅ | ✅ |
| Constructor | `constructor(...)` | **Optional**: `new Point()` yields a zero-valued instance |
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
EnumMethod     ::= 'fn' Identifier TypeParams? '(' ParamList? ')' ReturnType? Block
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
enum Result<T, E> {
    Ok(T),
    Err(E),
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
| Generics | ❌ | ✅ `enum Result<T, E> { ... }` |
| `match` destructuring | By value only | By variant + payload destructuring |
| `for-in` iteration | ✅ In declaration order | ❌ Meaningless when payloads are present |
| Memory layout | Integer/string value | tag + payload |

Mixing: a single enum may contain both payload-free and payload-bearing variants (see `NetEvent` / `ConnState` above).

#### 5.6.3 Construction and destructuring

Construction:

```xray
let c = Color.Red                                   // simple
let r1 = Result.Ok(42)                              // positional payload
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

ADT variants with payloads do **not** support `.value` / `.ordinal` / `getMember`, but `.name` and `toString()` are still available (the latter includes a payload summary, e.g. `Result.Ok(42)`).

#### 5.6.5 Iteration

Simple enums can be iterated with `for-in` in declaration order:

```xray
for (c in Color) { print(c.name) }        // "Red" "Green" "Blue"
```

ADT enums with payloads do **not** support direct `for-in`—iterating "all possible values" is meaningless (`Result<int, string>` has infinitely many).

#### 5.6.6 Reverse lookup (value to member)

Simple integer enums benefit from reverse-lookup optimization (Tier 1/2 contiguous/sparse; other types fall back to a linear scan). ADT variants do not support reverse lookup.

#### 5.6.7 Enum instance methods

Instance methods may be defined inside `enum` bodies with the same syntax as `class` methods (no `impl` keyword is introduced). Methods are callable on every variant; method bodies typically `match (this)` to dispatch on the variant:

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

> Note that `Triangle(...)` is not followed by a comma — the last variant is separated from the method block by whitespace (a trailing comma is allowed but not required).

**Rules**:

- Method syntax matches `class` methods: `fn name(params) -> ReturnType { body }`.
- Inside a method, the static type of `this` is the enum itself (e.g. `Result<T, E>`); use `match (this)` to extract a variant's payload.
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
- Static methods are **not yet supported** (use top-level "factory" functions).

> This design matches Java enums, Swift enums, and Kotlin sealed classes. Rust's `impl` blocks are **not** introduced in xray—xray defines methods inside the type body uniformly.

### 5.7 `type` aliases

```ebnf
TypeAliasDecl ::= 'type' Identifier TypeParams? '=' Type
```

```xray
type Outcome = int | string                          // union alias (do not collide with the prelude's Result)
type Mapper = fn(int) -> int                         // function-type alias
type Point = { x: float, y: float }                  // structural object alias (sealed)
```

**Semantics**:
- An alias is **purely a syntactic** substitution; it does not introduce a new nominal type.
- A `type Point = {...}` object alias is **sealed** when used as an annotation: accessing or assigning an undeclared field is a compile error.
- `type T = Json` equals `Json` (not sealed).
- Aliases may be referenced before their declaration but **must not be cyclic**.
- `type` aliases currently do not take type parameters; generic abstraction is provided by generic functions and generic class/struct/enum/interface.

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

### 6.2 Range Patterns `a..b`

```xray
match (age) {
    0..13 -> "child"
    13..20 -> "teen"
    20..65 -> "adult"
    _ -> "senior"
}
```

- Half-open interval `[a, b)`; integer-only.

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

// Result patterns (positional)
match (result) {
    Result.Ok(v)  -> print("got:", v),
    Result.Err(e) -> print("failed:", e),
}

// Wildcards skip payload fields you don't care about
match (event) {
    NetEvent.Error(code, _) if (code >= 500) -> throw new Exception("server error: ${code}"),
    _                                         -> continue,
}

// Nested destructuring
match (msg) {
    Result.Ok(NetEvent.DataReceived(bytes)) -> process(bytes),
    Result.Err(e)                            -> log.error(e),
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

- The guard expression sits inside `if (...)` parentheses and is evaluated under truthy/falsy context (see §2.3.3, identical to `if` / `while`); explicit `bool` expressions are recommended for readability.
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

See §5.1.5 for details. Within `match`, tuple and ADT-variant destructuring are currently supported; structural object/array destructuring is not part of the current `match` pattern syntax.

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
shared let big_buffer = new Bytes(1024 * 1024)

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
shared let big = new Bytes(1024)
let t3 = go fn(b: Bytes) -> int {
    return process(b)
}(move big)
// big is inaccessible from this point

// Pattern 4: Channel communication (capturable)
shared const ch = new Channel<int>(10)
let t4 = go fn(c: Channel<int>) -> int {
    return c.recv()
}(ch)
ch.send(42)
```

### 7.5 GC and Object Lifetimes

Xray uses a layered memory management strategy:

| Storage | Mechanism | Reclamation |
|--|--|--|
| Global heap (`shared const`) | refcount | when refcount reaches 0 |
| Local heap (general objects) | Mark-Sweep GC | when unreachable |
| Stack (`struct` values, locals) | RAII | when scope exits |
| Arena (low-level temporary allocations) | bulk free | at arena end |

**GC observation points**:
- Default: incremental Mark-Sweep.
- Mark phase traverses from the root set (globals, stack, registers).
- Sweep phase reclaims unmarked objects.
- The GC requires GC-safepoints; safepoints in the instruction stream include function calls, backward branches, and explicit `gc.collect()`.

**Write barriers** and **generational GC** design: see `docs/rules/gc-memory.md`.

---

## 8. Error Handling

> Source of truth: `src/runtime/error/xerror.c`, `src/vm/xvm_exception.c`, `stdlib/types/exception.xr`, `stdlib/types/result.xr`.

### 8.0 Design philosophy: dual track

Xray ships two complementary error-handling mechanisms side by side:

| Mechanism | When to use | Failure visibility |
|--|--|--|
| **Exceptions** (`throw` / `try` / `catch`) | Genuinely rare; propagation through many layers; fatal errors; framework / top-level fallback | Implicit (does not pollute intermediate signatures) |
| **Result** (`Result<T, E>` enum) | Library APIs with an explicit failure mode; the caller must handle exhaustively; serializable errors | Explicit (visible at compile time) |

The two tracks have **complementary, non-overlapping responsibilities**:

- Function signatures **do not** mark `throws` (no Java/Swift-style mandatory throws declarations). When you want compile-time visibility into possible failure, return `Result<T, E>`.
- `Exception` is the unified base class of the exception track (see §8.1.4); `Result<T, E>` is a prelude enum (see §8.2).
- The three sugar keywords `try!` / `try?` / `catch!` bridge between the two tracks (see §8.3).

### 8.1 Exception mechanism

#### 8.1.1 `throw` expression

`throw expr` raises an exception. **The static type of `expr` must be `Exception` or a subclass.** Anything else is rejected at compile time (error code `E0370` `XR_ERR_ANALYZE_THROW_NON_EXCEPTION`):

```xray
throw new Exception("oops")                 // ✅
throw new HttpError(404, "not found")       // ✅ custom Exception subclass
throw "oops"                                // ❌ E0370: throw must be an Exception subclass
throw 42                                    // ❌ E0370
throw { code: 500 }                         // ❌ E0370
throw null                                  // ❌ E0370 (static type is null)
```

> **Design note**: early versions of xray allowed `throw` of arbitrary values (strings, integers, objects). The current version tightens this rule, in line with Python 3 / Java / Swift, to require an Exception subclass. This matches xray's "no `any` type" principle—`e` in `catch (e)` always has the static type `Exception`, so tools can provide stable completion and type analysis.

After a throw:

```
throw point → unwind the call stack → run defer / finally on the way → catch handles → otherwise keep unwinding → coroutine terminates
```

An unhandled top-level exception terminates the current coroutine:

- Child coroutine: by default the stack is printed to stderr and the coroutine ends; the parent is **not** notified automatically (exceptions **do not propagate across coroutines**, see §8.1.6).
- Main coroutine: the process exits with code `1`.

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

**Execution order**:

1. Run the `try` block.
2. If an exception escapes, try each `catch` clause in declaration order; the first match runs its body.
3. The `finally` block runs whether or not an exception occurred (guaranteed).
4. With no `catch`, the exception keeps propagating after `finally`.

**Typed catch and multiple catch clauses**:

A `catch` variable may be typed `catch (e: T)`; the runtime uses `is T` to test for a match. Multiple `catch` clauses are matched in declaration order; the first match runs:

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

**Rules**:
- An untyped `catch (e)` is the "catch-all" clause and matches any exception; the static type of `e` is `Exception`.
- A typed `catch (e: T)` matches only when the exception value satisfies `is T`; the static type of `e` is `T`.
- Multiple `catch` clauses are tried in declaration order; the first match wins.
- If every typed clause fails to match and there is no catch-all, the exception is rethrown automatically.
- A `try` **must** be followed by at least one of `catch` or `finally`.

#### 8.1.3 Rethrowing

A `catch` block may rethrow the original exception or throw a new one:

```xray
try {
    fetch(url)
} catch (e) {
    log.error("network failed:", e.message)
    throw new ServiceError("upstream unavailable", e)  // chain the original through `cause`
}
```

#### 8.1.4 The `Exception` class

`Exception` is the prelude's built-in class (declared in `stdlib/types/exception.xr`); it can be `new`'d directly:

```xray
@native
class Exception {
    message: string             // human-readable message
    stack: Array<string>        // automatically captured stack, one formatted frame per entry
    cause: Exception?           // chained cause
    code: int                   // error code (auto-parsed from "E0xxx: ..." prefix; defaults to 0)
    data: Json?                 // when a non-Exception value is thrown, the original is wrapped here

    constructor(message: string = "", cause: Exception? = null)
    fn toString() -> string
}
```

Field semantics:

- `message`: human-readable error message (passed at construction time).
- `stack`: `Array<string>`. Empty at construction; as the throw passes through other frames, a formatted frame description (e.g. `"at f (line N)"`) is pushed for each. Users may iterate, join, or take its length; reads and writes are allowed but the runtime does not depend on user mutation.
- `cause`: optional cause exception, used for error chains.
- `code`: integer error code. When the VM raises an exception, the message has an `"E0xxx: ..."` prefix; the primitive constructor parses the prefix and writes `code`. For user-thrown exceptions `code` is `0`.
- `data`: `Json?`. When `throw <non-Exception value>` is wrapped at runtime, the original value is stored here; `catch` can recover it via `e.data`.

Construction:

```xray
throw new Exception("connection refused")
throw new Exception("upstream failed", originalErr)
```

#### 8.1.5 Custom `Exception` subclasses

Define business-specific errors by extending `Exception`:

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

Subclasses inherit `message` / `stack` / `cause` and `toString()` automatically and may add arbitrary business fields.

#### 8.1.6 Exceptions and coroutine boundaries

Exceptions **do not propagate across coroutines**. An unhandled exception inside a child coroutine:

- Terminates the child coroutine immediately.
- Default behaviour: prints the exception's `toString()` and `stack` to stderr.
- The parent coroutine is **not** notified automatically.

To pass child errors back, use a Channel explicitly:

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

Or use the structured-concurrency `scope` block (see §10.5), which propagates a child exception to the parent automatically.

#### 8.1.7 `defer`

`defer` is a resource-cleanup statement that runs **whenever** the enclosing scope exits (with or without an exception). Syntax: see §4.9. Relationship with `try / finally`:

- The two can be mixed.
- Multiple `defer`s in the same scope run in **LIFO** order.
- Mandatory order during stack unwinding:
  1. The `finally` block of inner `try` runs first.
  2. Then the current scope's `defer`s run in LIFO order.
  3. Control returns to the caller (or the exception keeps unwinding).

```xray
fn fetch(url: string) -> string {
    let conn = open(url)
    defer conn.close()                       // conn is guaranteed to close, no matter what

    try {
        return conn.read()
    } catch (e) {
        log.error(e.message)
        throw e                              // rethrow; defer still runs
    }
}
```

### 8.2 `Result<T, E>`

#### 8.2.1 Type and construction

`Result<T, E>` is the prelude's built-in ADT enum (declared in `stdlib/types/result.xr`):

```xray
@native
enum Result<T, E> {
    Ok(T),
    Err(E),
}
```

Construction and destructuring:

```xray
let r1: Result<int, ParseError> = Result.Ok(42)
let r2: Result<int, ParseError> = Result.Err(ParseError.Empty)

match (r1) {
    Result.Ok(v)  -> print("got:", v),
    Result.Err(e) -> print("failed:", e),
}
```

#### 8.2.2 Choosing the error type `E`

`E` may be any type; recommended styles:

| Failure shape | E type | Example |
|--|--|--|
| Multiple enumerable failure causes | User-defined ADT enum | `enum ParseError { Empty, NotNumber(s: string), Overflow }` |
| Single string reason | `string` | `Result<int, string>` |
| Exception object (bridge to the exception track) | `Exception` subclass | `Result<T, Exception>` |

**Strongly prefer ADT enums**—they let `match` check exhaustiveness at compile time.

#### 8.2.3 Methods on `Result`

`Result<T, E>` is an enum with methods (see §5.6.7). The full signature (declared in `stdlib/types/result.xr`):

```xray
@native
enum Result<T, E> {
    Ok(T),
    Err(E)

    fn isOk() -> bool
    fn isErr() -> bool

    fn ok() -> T?                                          // Ok(v) -> v; Err -> null
    fn err() -> E?                                         // Err(e) -> e; Ok -> null

    fn unwrap() -> T                                       // throw on Err (requires E to subclass Exception)
    fn unwrapOr(default: T) -> T                           // return default on Err
    fn unwrapOrElse(handler: (E) -> T) -> T                // compute via handler on Err

    fn map<U>(transform: (T) -> U) -> Result<U, E>         // transform the Ok value
    fn mapErr<F>(transform: (E) -> F) -> Result<T, F>      // transform the Err value
    fn andThen<U>(transform: (T) -> Result<U, E>) -> Result<U, E>  // chain (flatMap)
}
```

`map` / `mapErr` are the "transform the inner without breaking the outer" shortcut, replacing repetitive `match` boilerplate:

```xray
// Without map: 5 lines
let r2: Result<float, ParseError>
match (parseInt(s)) {
    Result.Ok(n)  -> r2 = Result.Ok(n.toFloat()),
    Result.Err(e) -> r2 = Result.Err(e),
}

// With map: 1 line
let r2 = parseInt(s).map(n -> n.toFloat())
```

#### 8.2.4 Error type conversion: explicit `mapErr` is mandatory

When composing `Result`s across layers, the `Err` type is **not** converted automatically. The caller must call `.mapErr(...)` explicitly:

```xray
fn loadConfig(text: string) -> Result<Config, ConfigError> {
    let json = try! parseJson(text).mapErr(e -> ConfigError.BadJson(e))
    //                              ^^^^^^^^ explicit: ParseError → ConfigError
    let port = try! json["port"].toInt().mapErr(e -> ConfigError.BadField("port", e))
    return Result.Ok(Config(port: port))
}
```

Each `.mapErr(...)` is one human-readable line of **error-upgrade path**—better than Rust's implicit `From::from` conversion.

### 8.3 Bridges: `try!` / `try?` / `catch!`

These three sugar keywords cover every track-bridging case and also serve as same-track early-exit sugar.

#### 8.3.1 `try! e` — early exit or cross-track upgrade

The static type of the expression after `try!` **must** be `Result<T, E>` or `T?`. Other types are a compile error (`E0821` `XR_ERR_TRY_BANG_BAD_OPERAND`).

The behaviour is double-dispatched on the type of `e` and the enclosing function's return type:

| Type of `e` | Enclosing return type | Behaviour |
|--|--|--|
| `Result<T, E>` | `Result<_, E>` | `Err(e)` → `return Result.Err(e)`; `Ok(v)` → `v` |
| `Result<T, E>` | other | `Err(e)` → `throw e` (requires `E` to subclass `Exception`, otherwise `E0822`); `Ok(v)` → `v` |
| `T?` | `_?` | `null` → `return null`; `v` → `v` |
| `T?` | other | `null` → `throw new NullThrowError("try! on null")`; `v` → `v` |

Examples:

```xray
// Same-track early exit
fn parsePair(s: string) -> Result<(int, int), ParseError> {
    let parts = s.split(",")
    if (parts.length != 2) return Result.Err(ParseError.Empty)
    let a = try! parseInt(parts[0])              // Err early-returns Err
    let b = try! parseInt(parts[1])
    return Result.Ok((a, b))
}

// Cross-track upgrade (Result → exception)
fn dangerous(s: string) -> int {
    let n = try! parseInt(s)                     // Err throws
    return n * 2
}

// Optional early exit
fn lookupTwo(m: Map<string, int>, k1: string, k2: string) -> int? {
    let v1 = try! m.get(k1)                      // null early-returns null
    let v2 = try! m.get(k2)
    return v1 + v2
}
```

> **`try!` is not a mandatory ceremony for throwing calls**—xray does **not** force `try` before every potentially throwing call, unlike Swift. `try!` is only for `Result` / `Optional` early exit or upgrade.

#### 8.3.2 `try? e` — failure becomes null

`try?` collapses any failure into `null`, discarding the cause.

| Type of `e` | Type of `try? e` | Behaviour |
|--|--|--|
| `Result<T, E>` | `T?` | `Err` → `null` (cause discarded); `Ok(v)` → `v` |
| Ordinary throwing call returning `T` | `T?` | Throw → `null`; success → `v` |
| `T?` | `T?` | Pass-through |

```xray
let n: int? = try? parseInt(s)              // Err becomes null
let v: Json? = try? http.get(url).json()    // throw becomes null
```

`try?` fits "the caller does not care about the cause, only wants a default" cases, often paired with `??`:

```xray
let port = (try? parseConfig(text).map(c -> c.port)) ?? 8080
```

#### 8.3.3 `catch! { ... }` — condense an exception block into a Result

`catch!` condenses a block that may throw into `Result<T, Exception>`:

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

Rules:

- The block's last expression or `return v` becomes the `Ok(v)` value.
- **Any exception** that escapes the block is caught and wrapped as `Err(e)`.
- The static type of `e` is `Exception` (so the result is always `Result<T, Exception>`).
- `return v` inside the block returns from the **`catch!` block**, not from the enclosing function.
- `defer` inside the block runs as usual.
- Type filtering is not supported—use a hand-written `try` / `catch` to filter specific exceptions.

#### 8.3.4 `Result.unwrap()` — Result upgraded to an exception

`unwrap()` is the reverse bridge: turn `Err(e)` into `throw e`:

```xray
let cfg = loadConfig(text).unwrap()         // throws on Err (requires E to subclass Exception)
```

If `E` is not an `Exception` subclass, this is a compile error (use `unwrapOr(...)` or `match` instead).

#### 8.3.5 Bridging matrix

```
                   ┌──────────────────────────────────────────┐
                   │  ↓ Exception track (throw / catch)       │
                   │                                          │
       catch! { } ─┤◄── exception → Result<T, Exception>     │
                   │                                          │
       try? expr  ─┤◄── exception → T? (cause discarded)     │
                   │                                          │
       try { } catch (e) { ... }    full exception flow      │
                   └──────────────────────────────────────────┘
                                    ▲
                                    │  unwrap / unwrapOr / try!
                                    │
                   ┌──────────────────────────────────────────┐
                   │  ↑ Result track (Result<T, E>)           │
                   │                                          │
       try! result ─┤── same-track early exit / cross throw  │
                   │                                          │
       try? result ─┤── Err → null (cause discarded)         │
                   │                                          │
       result.ok()  ─── Result<T,E> → T?                     │
                   └──────────────────────────────────────────┘
```

### 8.4 Optional and error handling

`T?` is sugar for `T | null` and fits the binary "value or absent" case. See §2.5. Relation to error handling:

- **Failure with no cause**: `T?` is more concise than `Result<T, ()>`.
- Cooperates with `try!` / `try?` (see §8.3.1 / §8.3.2).
- Pairs with `??` (default value) / `?.` (optional chain) / `e!` (force unwrap).
- Do not use `T?` as a generic error return—if a cause is needed, return `Result<T, E>`.

### 8.5 Decision tree: which mechanism to choose

Choose by "**how the caller has to handle the failure**":

```
Does the caller need to handle the failure?
│
├─ No (fatal / unrecoverable / cross-layer fallback)
│   ↓
│   throw / try-catch
│
├─ Yes, with structured causes; the caller should match exhaustively
│   ↓
│   Result<T, E>, with E as an ADT enum
│
├─ Yes, but the failure simply means "no value" without a cause
│   ↓
│   T? + ?? / ?. / try?
│
├─ Yes, and the function has ≥3 normal states (not just success/fail)
│   ↓
│   Use a user ADT enum directly as the return type
│
└─ Yes, returning multiple co-equal values (not "success/failure")
    ↓
    tuple (a, b, ...)
```

Reference table:

| Case | Recommended | Example |
|--|--|--|
| Parsing, decoding, state transitions | `Result<T, E>` | `parseInt(s) -> Result<int, ParseError>` |
| Map lookup, optional fields | `T?` | `map.get(k) -> Value?` |
| IO, network, unrecoverable | `throw` + top-level `catch` | `readFile(p) -> Bytes` (may throw IOError) |
| Multi-branch result | enum | `nextEvent() -> NetEvent` |
| Primary result + metadata | tuple | `parse(s) -> (Ast, int)` // ast + bytes consumed |

### 8.6 Common patterns

#### Pattern 1: library APIs use Result, business boundaries use exceptions

```xray
// Library layer: Result
fn parseConfig(text: string) -> Result<Config, ConfigError> {
    let json = try! parseJson(text).mapErr(e -> ConfigError.BadJson(e))
    return Result.Ok(Config(port: json["port"].toInt().unwrap()))
}

// Business layer: compose Results
fn loadConfig(path: string) -> Result<Config, ConfigError> {
    let text = readFile(path)                    // may throw IOError
    return parseConfig(text)
}

// Top level: exception catch as the safety net (must be called explicitly; there is no implicit main)
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

main()                                     // explicit entry
```

#### Pattern 2: condense an exception block into a Result (RPC / serialization)

```xray
fn handleRequest(req: Request) -> Response {
    let r: Result<Json, Exception> = catch! {
        let user = db.query(req.userId)         // may throw DbError
        return user.toJson()                    // may throw SerializeError
    }
    match (r) {
        Result.Ok(json) -> Response.success(json),
        Result.Err(e)   -> Response.error(500, e.message),
    }
}
```

#### Pattern 3: `try?` + `??` for default values

```xray
let port = (try? parseConfig(text).map(c -> c.port)) ?? 8080
let user = (try? db.findUser(id)) ?? guestUser
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

let b1 = new Box<int>(42)
let b2 = new Box<string>("hi")

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
```

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

**Built-in constraint interfaces** (see §14.14 for details):

| Interface | Meaning |
|---|---|
| `Comparable` | usable with `<` `<=` `>` `>=`; int/float/string and types implementing `Comparable` |
| `Hashable` | usable as a `Map` / `Set` key; int/float/string/bool/enum and types implementing `Hashable` |
| `Stringable` | callable via `.toString()`; almost every built-in type implements it by default |
| `Iterable<T>` | usable in `for-in`; Array, Map, Json, string, Range, enum, types with custom `iterator()` |

**Current limitations**:
- Constraints may only follow type parameters; there is no `where` clause.
- **Higher-kinded types** (`F<_>` as a parameter) are not supported.
- Default type parameters (`<T = int>`) are not supported.
- Interface implementation still requires **explicit `implements`** at the class declaration site (not at the constraint site; see §5.4).

### 9.3 Type Inference and Explicit Instantiation

#### Type inference

```xray
identity(42)                    // T inferred as int
new Box("hello")                // T inferred as string
new Pair("key", 100)            // K=string, V=int
```

The inference algorithm is **bidirectional**:
- From arguments (call-site argument types → type parameters).
- From the return type (contextual expected type → type parameters).

#### Explicit instantiation

When inference fails or precision is needed:

```xray
let empty = new Array<int>()              // no element to infer from
let m = new Map<string, int>()
let result = identity<float>(0)            // 0 defaults to int; force float
```

### 9.4 Specialization and Monomorphization

**Implementation strategy**: compile-time monomorphization.

- The compiler collects all concrete instantiation sites of generic functions/classes, generates a dedicated AST clone for each type combination, and compiles each into independent bytecode.
- Name mangling: `identity<int>` → `identity$i64`, `Pair<string, int>` → `Pair$str$i64`.
- Sharing by representation (rep-sharing): types with the same pointer representation share a single specialization (at most three versions: I64 / F64 / PTR).
- Strict compile-time type checking ensures safety; concrete type-parameter information is retained at runtime for `Reflect.typeOf`.

> Source of truth: `src/frontend/analyzer/xanalyzer_mono.c` (monomorphization pass), `xanalyzer_mono.h` (API).

**Performance impact**:
- Monomorphized generic functions can be optimized directly by JIT / AOT into native-typed operations (no boxing).
- Built-in specialized containers (`Array<int>`, `Bytes`) further avoid boxing overhead.
- Compiled binary size grows linearly with the number of instantiation combinations; the ceiling `XR_MONO_MAX_INSTANCES` prevents explosion.

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
render(new Square())     // OK
// render(new Wrong())   // compile error: Wrong is not Drawable
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
let c = new Container<int>()
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
| Stack model | Segmented stacks (grow on demand) |
| Creation cost | ~microsecond, KB-scale initial stack |
| Context switch | User-space stack switch, no syscall |

Coroutines are distributed across multiple worker threads by default; the runtime sets a Go-style `GOMAXPROCS` parallelism level based on the CPU core count.

### 10.2 `go` — start a coroutine

```ebnf
GoExpr ::= 'go' (Block | CallExpr | LambdaExpr CallArgs?)
```

`go` is an **expression** returning a `Task<T>` handle. Three forms are valid:

```xray
// Form 1: call an existing function
let t1 = go worker(0, channel)

// Form 2: call a lambda literal (inline logic + captured arguments)
let t2 = go fn(d: Json) -> int {
    return d.value * 2
}(payload)

// Form 3: block form (implicitly wrapped as a zero-argument lambda)
let t3 = go {
    return compute()
}
```

**`move` lives in argument position**: cross-coroutine ownership transfer goes through the argument prefix `move`, **not** through a `go` option:

```xray
shared let data = { value: 10 }
let task = go fn(d: Json) -> int {
    return d.value + 1
}(move data)        // transfer data ownership to the coroutine; data is unusable afterwards
```

**Semantics**:
- Every `go` expression returns a `Task<T>`, where `T` is the callee's return type; functions returning `()` correspond to `Task<null>`.
- Coroutines are scheduled on idle worker threads (M:N).
- Uncaught exceptions are stored in the `Task` and rethrown when `await` is called.
- Plain locals (not `shared`, not `move`d) passed to `go` are **deep-copied automatically**; `shared const` is shared zero-copy; `shared let` must be `move`d.

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
  - `await all` throws if any task throws (the others are cancelled).
  - `await any` throws only when **every** task fails; if any one completes, its result is returned.
  - `await anySuccess` is similar to `await any` but **skips** throwing tasks, awaiting only the first successful one.
- `all` / `any` / `anySuccess` are **contextual keywords** after `await`; they apply only in this position.

### 10.4 `Task<T>` handle

`go expr` returns `Task<T>`, where `T` is the callee's return type. Task handles support:

| Method / property | Type | Description |
|--|--|--|
| `t.done` | `bool` (property) | Whether the task has completed (success, failure, or cancellation) |
| `t.cancelled` | `bool` (property) | Whether the task was cancelled |
| `t.result` | `Json` (property) | Task return value; `null` if incomplete or failed |
| `t.error` | `string?` (property) | Task exception message; `null` if it has not failed |
| `t.cancel()` | `() -> ()` | Request cooperative cancellation |

```xray
let t = go fetch(url)
if (!t.done) { /* still running */ }
let r = await t

// read properties directly (no await required)
print(t.done, t.cancelled, t.result, t.error)
```

**Cancellation semantics**: `cancel()` sets the cancellation flag; the coroutine throws a cancellation exception at the next safepoint (GC checkpoint, channel operation, `await`, `yield`). `await` on a cancelled task returns `null`; `t.cancelled` becomes `true`.

### 10.5 Channel

```ebnf
ChannelType ::= 'Channel' '<' Type '>'
ChannelNew  ::= 'new' 'Channel' ('<' Type '>')? '(' Expression ')'
```

Channels are usually declared as `shared const` (cross-coroutine lifetime, reference semantics):

```xray
shared const ch  = new Channel<int>(10)    // buffered, capacity = 10
shared const ch0 = new Channel<int>(0)     // unbuffered (synchronous handshake)
shared const cha = new Channel(3)          // element type inferred from the first send
```

**API** (note that all method names are **camelCase**):

| Method | Signature | Behaviour |
|--|--|--|
| `send(v)` | `(T) -> ()` | Blocking send; waits for a consumer when full; throws if the channel is closed |
| `recv()` | `() -> T?` | Blocking receive; waits for a producer when empty; returns `null` once the channel is closed and the buffer is drained |
| `trySend(v)` | `(T) -> bool` | Non-blocking: `true` on success, `false` if full or closed |
| `tryRecv()` | `() -> (T, bool)` | Non-blocking; returns `(value, ok)`; `ok=false` when empty or closed |
| `sendTimeout(v, ms)` | `(T, int) -> bool` | Send with timeout; returns `false` on timeout |
| `recvTimeout(ms)` | `(int) -> (T, bool)` | Receive with timeout; `ok=false` on timeout |
| `close()` | `() -> ()` | Close the channel; idempotent |
| `isClosed` | `bool` (property) | Whether the channel is closed |

```xray
shared const ch = new Channel<int>(10)
ch.send(42)                             // blocking send
let v = ch.recv()                       // blocking receive (null after close + drain)

let sent = ch.trySend(99)               // non-blocking send: true / false
let (next, ok) = ch.tryRecv()           // non-blocking receive: value + ok flag
if (ok) { print(next) }

ch.close()
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
- After close: `send` throws; `recv` returns remaining values, then `null` once drained; `tryRecv` returns `(zero, false)`.

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
shared const ch1 = new Channel<int>(2)
shared const ch2 = new Channel<int>(2)

select {
    msg from ch1 -> { print("got from ch1:", msg) }      // receive arm
    msg from ch2 -> { print("got from ch2:", msg) }      // receive arm
    100  to   ch1 -> { print("sent 100 to ch1") }        // send arm
    _ -> { print("no channel ready") }                   // default arm (non-blocking)
}
```

**Semantics**:
- Receive arm `name from ch -> body`: equivalent to `name = ch.recv()`, but selected only when `ch` has data.
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
SupervisorScopeExpr ::= 'supervisor' 'scope' Block     // collect all errors, return Array<string>
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
| `supervisor scope { ... }` | **Collects** failure messages from every failing child; siblings do not affect each other | `Array<string>` (error list; empty means all succeeded) |

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

// supervisor scope: collect errors
let errors = supervisor scope {
    go failing("error1")
    go failing("error2")
    go ok()
}
print(errors.length)                 // 2 (only the failures are counted)
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
shared let buf = new Bytes(1024 * 1024)

// hand off to a coroutine
let t = go fn(b: Bytes) -> int {
    return process(b)
}(move buf)
// compile error: buf has been moved
// print(buf.length)

// hand off to a channel
shared const ch = new Channel<Bytes>(1)
shared let payload = new Bytes(4096)
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

> **Design note**: xray does not expose generic concurrency primitives such as `Mutex`/`RwLock`/`Atomic*` in the standard library. If introduced in the future, they would be released as a separate unstable module (see `docs/known_bugs.md` and forthcoming design RFCs).


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
| Shared mutable state requires explicit Mutex | Doc convention |

**Residual data-race risk** (detected at runtime, not compile time):
- Sending a mutable class reference via a channel (the receiver and sender may mutate concurrently)—prefer to send `shared const` / `Bytes` / immutable objects, or transfer ownership via `move`.

---

## 11. Modules

> Source of truth: `src/module/xmodule.c`, `src/module/xmodule_resolve.c`, `src/frontend/parser/xparse_import.c`.

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

```ebnf
ImportStmt ::= 'import' ImportMembers 'from' ImportModule
            |  'import' ImportModule ('as' Identifier)?
ImportMembers ::= '{' ImportMember (',' ImportMember)* ','? '}'
ImportMember  ::= Identifier ('as' Identifier)?
ImportModule  ::= StringLiteral | ModuleName
ModuleName    ::= Identifier ('/' Identifier)?
```

```xray
// 1. stdlib: bare identifier; without `as`, the alias equals the module name
import time
import datetime
import http as httpClient

// 2. third-party packages: owner/name form
import alice/utils
import bob/http_client as httpClient

// 3. file-path or directory-path: string literal, optional explicit alias, otherwise inferred from the trailing path segment
import "./modules/mod_a.xr" as a
import "../utils/string_utils.xr" as utils
import "models/user" as user

// 4. named imports: members may be renamed; the `from` operand may be a quoted path or a bare module name
import { readFile, writeFile as write } from io
import { publicFn } from "./modules/mod_a.xr"
```

JavaScript-style default import (`import name from "module"`) is **not supported**. In Xray, use `import "module" as name`, `import module`, or `import { name } from module`.

**Resolution algorithm** (in priority order):
1. **stdlib name resolution**: a bare identifier `import time` → the built-in stdlib module table.
2. **Relative path**: `"./xxx.xr"` and `"../xxx.xr"` are resolved relative to the current file.
3. **Project-root path**: a quoted path that does not start with `./` or `../` is resolved as a project-relative directory import.
4. **Third-party packages**: `owner/name` is resolved through the `[dependencies]` section in `xray.toml`.

**Source of truth**: `xparse_import.c` and `xmodule_resolve_path()`.

### 11.4 `export` and Visibility

Xray supports three export forms:

```ebnf
ExportStmt ::= 'export' Declaration                              // export the declaration directly
            |  'export' Identifier                               // export an already-declared identifier
            |  'export' '{' ExportSpec (',' ExportSpec)* '}' 'from' StringLiteral
            |  'export' '*' 'from' StringLiteral
ExportSpec ::= Identifier ('as' Identifier)?
```

```xray
// 1. export a declaration directly
export fn helper() { return }
export class MyClass {
    value: int
    constructor() { this.value = 1 }
}
export const VERSION = "1.0"

// 2. export an already-declared identifier (declare internally first, expose at the end)
fn _helper() -> string { return "..." }
fn publicFn() -> string { return _helper() }
export publicFn

// 3. re-export (with optional renaming)
export { getUser, getUserAge as getAge } from "./user"

// 4. wildcard re-export (forward all exports of another module)
export * from "./product"
```

- Declarations not marked `export` are **private** to the module.
- Internal state (`let _x`, `const _VERSION`, `fn _helper`) does not collide across modules even with the same name.
- Re-exports and wildcard re-exports are commonly used in `index.xr` to aggregate public APIs of submodules.

### 11.5 Naming Conventions

- Module names: `snake_case` (`http_client.xr` / `string_utils.xr`).
- Public symbols: `camelCase` or `PascalCase` (for classes / interfaces).
- Internal symbols: prefix with `_` (`_internal_helper`).

### 11.6 Circular Dependencies

Xray **forbids** circular dependencies. Module loading builds a DAG; a detected cycle is a compile error.

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

See the "native module registration" section of `docs/rules/architecture.md` for details.

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
| `int(x)` | `(value) -> int` | convert to int; throws if string parsing fails |
| `float(x)` | `(value) -> float` | convert to float |
| `string(x)` | `(value) -> string` | convert to string |
| `bool(x)` | `(value) -> bool` | convert to bool; rules in §2.4.1 |
| `chr(n)` | `(int) -> string` | Unicode code point → single-character string |
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

### 14.5 `string` Methods

| Member | Type / Description |
|--|--|
| `length` | string-length property |
| `charAt(i)` | character at the given index |
| `charCodeAt(i)` | code point at the given index |
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
| `iterator()` / `entriesIterator()` / `entries()` | iteration protocol |

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
| `recv()` | blocking receive; returns `null` when closed and the buffer is empty |
| `trySend(v)` | non-blocking send, returns bool |
| `tryRecv()` | non-blocking receive, returns `(T, bool)` |
| `sendTimeout(v, ms)` | timed send; returns false on timeout/close |
| `recvTimeout(ms)` | timed receive; returns `(T, bool)` |
| `close()` | close the channel |
| `isClosed` / `isClosed()` | closed state; both runtime property and method are supported |

> `stdlib/types/channel.xr` still declares a `closed` property, but the runtime symbol table and VM dispatch use `isClosed`; this declaration drift is recorded as a known issue.

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

`Task<T>` properties: `done`, `cancelled`, `result`, `error`; methods: `cancel()`. `EnumValue` properties: `name`, `value`, `ordinal`; methods: `toString()`. `EnumType` properties: `name`, `memberCount`; methods: `getMember(name)`.

### 14.18 Other Prelude Types (`Logger` / `NetConn` / `NetListener`)

These types are registered by the prelude; instances are constructed by factory functions in modules such as `log` / `net`. The complete runtime capability follows the corresponding stdlib module.

---

## 15. Standard Library Overview

> Source of truth: stdlib implementations and analyzer builtin metadata.
> MCP knowledge fetches API signatures via `xray builtin-dump` and injects per-module knowledge cards at generation time.
> See [Appendix D — stdlib module index](#d-stdlib-module-index).

> **Authoritative native module list** (22 modules; source: `stdlib/<module>/*.c`):
>
> `base64`, `cluster`, `compress`, `crypto`, `csv`, `datetime`, `encoding`, `gc`, `http`, `io`, `log`, `math`, `net`, `os`, `path`, `regex`, `time`, `toml`, `url`, `ws`, `xml`, `yaml`.
>
> Built-in types that need no import are registered by the prelude (`Array`, `Map`, `Set`, `Json`, `Channel`, `Bytes`, `BigInt`, `StringBuilder`, `Exception`, `Regex`, `Logger`, `NetConn`, `NetListener`, etc.); `Result<T, E>` is the built-in ADT enum used on error-handling paths. See §1.5.6 / §2.2.

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
| `net` | TCP / UDP / TLS sockets + DNS | `listen` `dial` `lookup` `Socket` `Listener` `hasTLS` |
| `http` | HTTP / HTTPS client + server + HTTP/2 | `get` `post` `request` `Server` `urlEncode` `urlDecode` |
| `ws` | WebSocket | client/server connections |
| `url` | URL parsing and construction | `parse` `format` `parseQuery` `buildQuery` `encode` `decode` |

> DNS lookups go through `net.lookup(host)`; there is no standalone `dns` module.

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
| `gc` | `collect()` `isrunning()` `count()` `state()` `stats()` |

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

> Source of truth: `src/runtime/`, `src/vm/`, `docs/rules/gc-memory.md`, `docs/rules/architecture.md`.

### 16.1 Value Representation

Xray values are uniformly represented as `xray_value_t`. Layout strategy:

- **NaN-boxing** (on 64-bit platforms): unused IEEE-754 NaN bit space encodes small integers, booleans, and pointer tags.
- **Pointer tagging**: low-bit tags distinguish object kinds.
- **Object references**: heap objects are referenced via tagged pointers; the current GC does not move objects.

| Value type | Internal representation |
|--|--|
| `int` | 53-bit immediate (NaN-box) |
| `float` | double precision stored directly |
| `bool` | tag |
| `null` | single global value |
| `string` | heap object + short-string inline (≤ 7 bytes) |
| `Bytes` | heap object + capacity/length |
| Other objects | heap pointer |

### 16.2 Memory Allocation

| Region | Use |
|--|--|
| **System heap** | C `malloc/free`, used for native data structures |
| **Global heap** | `shared const` / `shared let`, refcount GC |
| **Coroutine heap** | per-coroutine independent Mark-Sweep GC heap |
| **Stack** | `struct` values, local immediates, function frames |
| **Arena** | parser temporary allocation, frame allocation |

### 16.3 GC Model

- Default **per-coroutine Mark-Sweep / Immix Mark-Region**.
- **Tri-color marking**: white (unvisited) / gray (pending) / black (scanned).
- **Write barriers**: triggered when writing GC values into object fields, module fields, static fields, and containers; Sticky Immix uses a backward barrier to maintain young/old relationships.
- **GC-safepoints**: function calls, backward branches, explicit `gc.collect()`.

See `docs/rules/gc-memory.md` for details.

### 16.4 Coroutine Scheduling

- M:N scheduling (M OS threads × N coroutines).
- **work-stealing**: idle workers steal tasks from other workers' queues.
- **Cooperative preemption**: coroutines yield at safepoints (no forced preemption).
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

### 16.6 Exception Runtime

The built-in `Exception` class is a prelude type (declared in `stdlib/types/exception.xr`); users may directly `new` it or inherit from it:

```xray
@native
class Exception {
    message: string             // human-readable message
    stack: Array<string>        // automatically captured call stack, one formatted line per frame
    cause: Exception?           // chained cause
    code: int                   // error code (auto-parsed from "E0xxx: ..." prefix; default 0)
    data: Json?                 // when a non-exception value is thrown, the original value is wrapped here

    constructor(message: string = "", cause: Exception? = null)
    fn toString() -> string
}
```

The operand of a `throw` expression must have a static type that is `Exception` or one of its subclasses (see §8.1.1); other types are rejected at compile time (error code `E0370`). Runtime errors thrown by the VM also use this `Exception` type.

Stack unwinding: the VM's `xvm_unwind_stack()` walks the try-table to find catch handlers, releasing locals frame by frame and running `finally` / `defer` along the way before jumping to the catch. See §8 for details.

### 16.7 Result Runtime

`Result<T, E>` is a prelude ADT enum (see §8.2 / §5.6.2). Runtime representation: tag (1 byte) + payload. `Result.Ok(v)` and `Result.Err(e)` are value objects, and `match` destructuring is lowered at the IR level into tag-test + payload-extract.

Because `Result` has zero overhead on the no-exception path (no throwing, no stack unwinding), code paths using `Result` perform the same as a tagged union.

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
- **Generics**: monomorphization, constraint checking, call-site rewriting.
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
| `E0504` | `XR_ERR_MOD_CIRCULAR` | circular dependency |

### 18.6 Rejected Syntax

> The parser rejects the following forms outright and reports the correct replacement.

| Code | Name | Rejected form | Correct form |
|--|--|--|--|
| `E0801` | `XR_ERR_SYN_RETURN_MULTI_REMOVED` | `return a, b` | `return (a, b)` |
| `E0802` | `XR_ERR_SYN_LET_MULTI_REMOVED` | `let x, y = ...` | `let (x, y) = ...` |
| `E0803` | `XR_ERR_SYN_FOR_FLAT_REMOVED` | `for k, v in m` (bare KV) | `for (k, v in m)` |
| `E0804` | `XR_ERR_SYN_VOID_REMOVED` | `-> void` | `-> ()` or omit the return type |

### 18.7 Error Handling and Result (E082x)

| Code | Name | Description |
|--|--|--|
| `E0820` | `XR_ERR_THROW_NOT_EXCEPTION` | merged into `E0370` (see §8.1.1); the code is preserved in the table only to avoid reuse |
| `E0821` | `XR_ERR_TRY_BANG_BAD_OPERAND` | `try!` operand is neither `Result<T,E>` nor `T?` |
| `E0822` | `XR_ERR_TRY_BANG_NON_EXCEPTION_ERR` | `try!` cross-track promotion where `E` is not an `Exception` subclass |
| `E0823` | `XR_ERR_MATCH_NOT_EXHAUSTIVE` | merged into `E0371` (see §6.3.3); the code is preserved only to avoid reuse |
| `E0824` | `XR_ERR_UNWRAP_NON_EXCEPTION_ERR` | `Result<T, E>.unwrap()` where `E` is not an `Exception` subclass |

### 18.8 Error-Object Layout

Runtime errors thrown by the VM use the prelude `Exception` class (declared in `stdlib/types/exception.xr`):

```xray
@native
class Exception {
    message: string             // human-readable message including error code and context
    stack: Array<string>        // auto-captured call stack, one formatted line per frame
    cause: Exception?           // chained cause
    code: int                   // error code (auto-parsed from "E0xxx: ..." prefix; default 0)
    data: Json?                 // when a non-exception value is thrown, the original value is wrapped here

    constructor(message: string = "", cause: Exception? = null)
    fn toString() -> string
}
```

The static type of a `throw` operand **must** be a subclass of `Exception` (see §8.1.1 / `E0370`). For structured errors, inherit `Exception` and add business fields:

```xray
class HttpError extends Exception {
    statusCode: int
    constructor(statusCode: int, message: string, cause: Exception? = null) {
        super(message, cause)
        this.statusCode = statusCode
    }
}
```

Alternatively, use an ADT enum + `Result<T, E>` to express enumerable failure modes (see §8.2).

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
                | "'" StringChar* "'"
RawStringLiteral ::= 'r' '"' [^"]* '"'
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
EqualityExpr ::= RelationalExpr (('==' | '!=' | '===' | '!==') RelationalExpr)*
RelationalExpr ::= ShiftExpr (('<' | '<=' | '>' | '>=') ShiftExpr)*
ShiftExpr   ::= AdditiveExpr (('<<' | '>>') AdditiveExpr)*
AdditiveExpr ::= MultiplicativeExpr (('+' | '-') MultiplicativeExpr)*
MultiplicativeExpr ::= TypeOpExpr (('*' | '/' | '%') TypeOpExpr)*
TypeOpExpr  ::= UnaryExpr (('as' | 'is') Type)*           // safe cast is `x as T?` where T? is a nullable type
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

ThrowExpr   ::= 'throw' Expression                // operand's static type must be Exception-derived (E0370)

ArgList ::= Expression (',' Expression)*
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

LiteralPattern  ::= IntLiteral | FloatLiteral | StringLiteral | BoolLiteral | NullLiteral
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
                |  '{' Identifier (',' Identifier)* ','? '}'

FnDecl ::= AttrList? Modifier* 'fn' Identifier TypeParams? '(' ParamList? ')' ReturnType? Block
ParamList ::= Param (',' Param)*
Param     ::= Modifier* Identifier ':' Type ('=' Expression)?
           |  '...' Identifier ':' Type
ReturnType ::= '->' Type | '->' '(' Type (',' Type)+ ')'
Modifier  ::= 'in' | 'ref' | 'private' | 'public' | 'static' | 'final' | 'abstract' | 'override'
              // public/override are accepted but never required (default/implicit behavior)

TypeParams ::= '<' TypeParam (',' TypeParam)* '>'
TypeParam  ::= Identifier (':' Type ('&' Type)*)?         // constraints use ':', multiple use '&'

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

TypeAliasDecl ::= 'type' Identifier TypeParams? '=' Type

ImportDecl ::= 'import' ImportMembers 'from' ImportModule
            |  'import' ImportModule ('as' Identifier)?
ExportDecl ::= 'export' Declaration                                         // export the declaration directly
            |  'export' Identifier                                          // export an already-declared identifier
            |  'export' '*' 'from' StringLiteral                            // forwarding export
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
| `unknown` | §2.2 (compiler-internal) |
| `while` | §4.3 |
| `yield` | §3.16 / §10.10 |

---

## Appendix C. Operator Index

The complete operator listing organized by purpose is in [§1.7](#17-operators-and-tokens); detailed precedence is in [§3.1](#31-precedence-and-associativity).

| Group | Operators |
|--|--|
| Arithmetic | `+` `-` `*` `/` `%` |
| Bitwise | `&` `\|` `^` `~` `<<` `>>` |
| Comparison | `==` `!=` `===` `!==` `<` `<=` `>` `>=` |
| Logical | `&&` `\|\|` `!` |
| Assignment | `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` |
| Other | `..` `??` `?.` `!` `->` |

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
| `gc` | GC control |
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
| `===` vs `==` | `===` strict, `==` weak (string↔number coercion) | `==`/`!=` is value equality (only int↔float promotion); `===`/`!==` requires both type and value to be strictly equal |
| Closure capture | by reference | by reference (default); `go` closures are strictly restricted |
| Objects | dynamic fields | dynamic by default; sealed once annotated `type T = {...}` |
| import | ES Modules | xray-specific syntax (stdlib uses unquoted form) |
| Concurrency | async / Promise | coroutines + channels |

### E.2 vs Go

| Dimension | Go | xray |
|--|--|--|
| Type system | simple + implicit interfaces | richer + explicit `implements` |
| Error handling | multiple return values + `err != nil` | exceptions + `Result<T,E>` + `try?`/`try!` |
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
| Errors | `Result<T, E>` | exceptions + `Result<T,E>` |
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
| `try?` / `try!` semantics | `try?` collapses to nil; `try!` aborts on failure | `try?` collapses to null; `try!` **rethrows the original exception** (does not abort) |
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
| **monomorphization** | Specializing generics into concrete-type versions (xray does not do this) |
| **move** | Ownership transfer: enforced when crossing coroutine boundaries (see §7.3) |
| **NaN-boxing** | Storing tagged values inside the unused bits of an IEEE-754 NaN |
| **nullable** | A nullable type `T?` whose value may be `null` |
| **pattern** | A pattern used in `match` and destructuring (see §6) |
| **scope** | Lexical scope |
| **shared** | Storage class for cross-coroutine sharing (see §5.1.3) |
| **SSA** | Static Single Assignment: IR where each variable is assigned only once |
| **struct** | Value-type class (see §5.4) |
| **TCO** | Tail-Call Optimization |
| **trait** | Rust terminology; xray uses `interface` |
| **truthy** | A value treated as true in control flow when it is not `false` / `null` / `0` / `""` / an empty collection (see §2.3.3) |
| **monomorphization** | Specializing generic type parameters at compile time into concrete versions while retaining runtime type information |
| **union** | Union type `A \| B` |
| **upvalue** | Outer variable captured by a closure |
| **VM** | Virtual Machine: xray bytecode VM |
| **write barrier** | Hook inserted by the GC on pointer updates |

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

### 1.1 Encoding

Xray source files are UTF-8. The lexer treats strings, comments, and raw string bodies as UTF-8 byte sequences. Identifiers are currently ASCII-only.

A UTF-8 BOM at the beginning of a file may be skipped by the scanner.

### 1.2 Line Endings and Whitespace

Line endings are `\n` or `\r\n`. A bare `\r` is not a normal line terminator.

Whitespace characters are space, horizontal tab, and line terminators. Whitespace separates tokens. In most locations it does not carry semantic meaning, but the parser may use token adjacency for a few disambiguations such as generic closing brackets.

### 1.3 Comments

Xray supports line comments and block comments:

```xray
// line comment

/* block comment
   spanning multiple lines */
```

Comments do not nest. Comments are collected as trivia for formatters/LSP but are not part of the AST semantics. Documentation comments such as `///` or `/** ... */` are conventions only.

### 1.4 Identifiers

```ebnf
Identifier ::= IdentStart IdentCont*
IdentStart ::= 'a'..'z' | 'A'..'Z' | '_'
IdentCont  ::= IdentStart | '0'..'9'
```

Identifiers are ASCII. A reserved keyword cannot be used as an identifier, except where the grammar explicitly treats it as a member name after `.`.

A single `_` is special:

- Wildcard in patterns.
- Discard marker in destructuring and selected loop positions.
- Not a normal binding name.

Names such as `__tmp` are ordinary identifiers.

### 1.5 Keywords

The lexer keyword table contains 63 reserved keywords. They include:

| Group | Keywords |
|--|--|
| Declarations and flow | `let`, `const`, `shared`, `fn`, `return`, `if`, `else`, `while`, `for`, `in`, `break`, `continue`, `match` |
| OOP and types | `class`, `struct`, `interface`, `enum`, `type`, `extends`, `implements`, `constructor`, `this`, `super`, `new`, `static`, `final`, `abstract`, `override`, `operator`, `is`, `as` |
| Error handling | `try`, `catch`, `finally`, `throw` |
| Modules | `import`, `export` |
| Concurrency | `go`, `await`, `select`, `defer`, `scope` |
| Primitive type names | `int`, `int8`, `int16`, `int32`, `int64`, `uint8`, `uint16`, `uint32`, `uint64`, `float`, `float32`, `float64`, `bool`, `string`, `unknown` |
| Literal keywords | `true`, `false`, `null` |

Context-sensitive words are parsed by position and may otherwise be used as ordinary identifiers:

| Word | Context |
|--|--|
| `from` | named import/re-export; `select` receive arm |
| `to` | `select` send arm |
| `after` | `select` timeout arm |
| `move` | ownership transfer expression |
| `ref` | parameter modifier |
| `linked` | linked coroutine/scope modifier |
| `supervisor` | supervisor scope modifier |
| `cancelled` | cancellation check call |

Prelude type names are not lexer keywords. They are automatically available type symbols: `Array`, `BigInt`, `Bytes`, `Channel`, `DateTime`, `Exception`, `Json`, `Logger`, `Map`, `NetConn`, `NetListener`, `Range`, `Regex`, `Set`, and `StringBuilder`. The built-in `Result<T, E>` ADT is used by error-handling paths such as `catch!`.

### 1.6 Literals

#### Integer Literals

```xray
0
42
1_000_000
0xff
0o755
0b1010
```

Underscores may separate digits. Supported bases are decimal, hexadecimal, octal, and binary.

#### Floating-Point Literals

```xray
3.14
.5
1e9
6.02e23
```

Floating literals have type `float`.

#### BigInt Literals

```xray
123n
0xffn
0b1010n
```

A trailing `n` creates an arbitrary-precision integer value.

#### Boolean and Null Literals

```xray
true
false
null
```

`true` and `false` have type `bool`. `null` is the singleton null value and is assignable to nullable types.

#### Strings

```xray
"hello"
'hello'
"hello ${name}"
r"C:\Users\${USER}"
r'raw string'
```

Single- and double-quoted strings are supported. Raw strings use an `r` prefix and still support interpolation. Backtick strings are not supported.

#### Regex Literals

```xray
/[a-z]+/i
/^xray$/
```

Regex literals are lexed contextually to avoid confusion with division. Runtime behavior is implemented by the regex support in the standard library/runtime.

### 1.7 Tokens and Operators

Important punctuation:

| Token | Meaning |
|--|--|
| `(` `)` | grouping, calls, parameter lists |
| `{` `}` | blocks, object/Json literals, match/select bodies |
| `[` `]` | arrays, indexing, slicing |
| `#{` | Map literal start |
| `#[` | Set literal start |
| `.` `?.` | member access and optional chaining |
| `:` | type annotations, object/Map entry separator |
| `;` | optional statement separator |
| `@` | attribute marker |
| `?` | nullable types and ternary operator |

Operators include arithmetic, bitwise, comparison, logical, assignment, increment/decrement, range, optional chaining, force unwrap, and nullish coalescing operators.

The arrow token is `->`. Xray does not use `=>` for match arms, select arms, or Map literals.

---

## 2. Type System

### 2.1 Overview

Xray is statically typed. Local variables usually rely on inference, while function parameters, public APIs, fields, and complex values often use explicit annotations.

There is no source-level `any` type. When this document says “value” in an API signature, it means an arbitrary runtime value accepted by an intrinsic path, not a type you can write in source.

### 2.2 Primitive Types

| Type | Description |
|--|--|
| `int` | integer value |
| `float` | floating-point value |
| `bool` | boolean |
| `string` | UTF-8 string |
| `null` | singleton null value |
| `()` | unit type/value |

Width-specific numeric type names are recognized, but the exact lowering is implementation-defined for current releases.

### 2.3 Nullable Types

```xray
let s: string? = null
let n: int? = 42
```

`T?` means `T | null`. Nullable values can be checked against `null`, accessed with optional chaining, or force-unwrapped with `!`.

### 2.4 Union Types

```xray
let id: int | string = 42
id = "abc"
```

Union types are written with `|`. Type narrowing is performed in supported control-flow and `is` checks.

### 2.5 Function Types

```xray
type Mapper = (int) -> int
let pred: (string) -> bool
```

Function types use `->`. Parameter names are not part of a function type.

### 2.6 Tuple Types

```xray
let p: (int, string) = (1, "x")
print(p.0)
print(p.1)
```

Tuples are fixed-arity product values. Tuple fields can be accessed by numeric member names.

### 2.7 Object and Json Types

Object type aliases are sealed when used as annotations:

```xray
type User = { name: string, age: int }
let u: User = { name: "alice", age: 30 }
```

Access to undeclared fields on a sealed object is rejected. Unannotated object literals generally produce dynamic `Json`-style values.

`Json` is the dynamic structured data type for JSON-like values.

### 2.8 Nominal Types

`class`, `struct`, `interface`, and `enum` declarations create nominal types. Classes are reference-oriented. Structs are value-oriented declarations sharing class-like member syntax. Interfaces define contracts. Enums support both backed simple enums and ADT payload variants.

### 2.9 Built-in Container Types

| Type | Meaning |
|--|--|
| `Array<T>` | ordered mutable sequence |
| `Map<K, V>` | insertion-ordered key/value table |
| `Set<T>` | unique-value collection |
| `Channel<T>` | typed coroutine channel |
| `Range` | range created by `a..b` |
| `Bytes` | byte-buffer type |
| `StringBuilder` | mutable string builder |
| `Regex` | compiled regex value |
| `DateTime` | native date/time value |

### 2.10 Assignability

Values are assignable when their static type is equal to or safely compatible with the target type. Nullable assignment requires a nullable target. Union assignment requires the source to be assignable to at least one union arm.

Numeric implicit conversion is intentionally limited. Use explicit conversion functions such as `int(x)`, `float(x)`, and `string(x)` when needed.

### 2.11 Truthiness

Control-flow conditions accept truthy/falsy values. Common falsy values are `false`, `null`, numeric zero, empty string, and empty containers. Static assignment to `bool` remains type-checked and does not mean arbitrary truthy values are implicitly bools.

---

## 3. Expressions

### 3.1 Precedence

From low to high:

1. Assignment and compound assignment.
2. Ternary `? :`.
3. Nullish coalescing and logical operators.
4. Bitwise operators.
5. Equality and comparison.
6. Type checks/casts (`is`, `as`).
7. Shifts.
8. Addition/subtraction.
9. Multiplication/division/modulo.
10. Unary operators.
11. Range.
12. Calls, member access, indexing, slicing, optional chaining, force unwrap.

### 3.2 Literals

```xray
let a = [1, 2, 3]
let m = #{"a": 1, "b": 2}
let s = #[1, 2, 3]
let obj = { name: "alice", age: 30 }
let tup = (1, "x")
```

Array literals use `[ ... ]`. Map literals use `#{ ... }` with `:` key/value separators. Set literals use `#[ ... ]`. Object/Json literals use `{ ... }`.

### 3.3 Variables and Assignment

```xray
let x = 1
x = 2
x += 3
```

Assignment requires an assignable lvalue. `const` bindings and readonly fields cannot be assigned after initialization.

### 3.4 Indexing and Slicing

```xray
arr[0]
arr[1] = 10
arr[start:end]
arr[:end]
arr[start:]
arr[:]
```

The parser distinguishes indexing from slicing by the presence of `:` inside brackets.

### 3.5 Member Access

```xray
obj.field
obj.method(arg)
tuple.0
json.class
```

After `.`, identifiers, keywords, and integer literals are accepted as member names. Integer member names are used for tuple fields.

### 3.6 Calls

```xray
f()
f(1, 2)
obj.method(x)
```

Calls evaluate the callee and arguments, then dispatch according to function, closure, native function, class constructor, static method, or instance method semantics.

### 3.7 Closures

```xray
let inc = (x: int) -> x + 1
let add = (a: int, b: int) -> {
    return a + b
}
```

Arrow closures cannot declare an explicit return type. Annotate the binding or use a named `fn` if an explicit return type is required.

### 3.8 Type Operators

```xray
if (x is string) {
    print(x)
}
let y = x as int?
```

`is` checks runtime type and can narrow static type. `as` performs a cast; nullable target types are used for safe failure paths.

### 3.9 Nullable Operators

```xray
user?.name
value!
left ?? fallback
```

Optional chaining returns `null` if the receiver is null. Force unwrap throws or fails if the operand is null. Nullish coalescing returns its right operand only when the left operand is null.

### 3.10 `new`

```xray
let p = new Point(1, 2)
let ch = new Channel<int>(10)
```

`new` constructs class, struct, and supported native/prelude values. Constructor dispatch follows nominal type semantics.

### 3.11 `move`

```xray
ch.send(move payload)
```

`move` transfers ownership across boundaries, most importantly across channels and coroutine boundaries.

### 3.12 Range Expressions

```xray
let r = 1..10
for (i in 1..10) { print(i) }
```

Ranges are half-open in normal iteration: `[start, end)`.

### 3.13 `match` Expressions

```xray
let label = match (x) {
    0 -> "zero"
    1..10 -> "small"
    _ -> "other"
}
```

The scrutinee must be parenthesized: `match (x)`. Arms use `->`. Guards use `if (condition)` after the pattern.

### 3.14 Try Expressions

```xray
let maybe = try? mayThrow()
let value = try! mayThrow()
let result = catch! { mayThrow() }
```

`try?` converts exceptions to `null`. `try!` rethrows. `catch!` wraps success/error paths into a Result-style value.

### 3.15 Throw Expressions

```xray
throw new Exception("error")
```

The operand must be statically typed as `Exception` or an `Exception` subclass.

---

## 4. Statements

### 4.1 Blocks and Expression Statements

```xray
{
    let x = 1
    print(x)
}
```

A block introduces a lexical scope. Expression statements evaluate an expression for side effects.

### 4.2 `if`

```xray
if (cond) {
    a()
} else if (other) {
    b()
} else {
    c()
}
```

Conditions are parenthesized.

### 4.3 `while`

```xray
while (cond) {
    step()
}
```

### 4.4 `for` and `for-in`

```xray
for (let i = 0; i < 10; i++) { print(i) }
for (v in values) { print(v) }
for (k, v in map) { print(k, v) }
for ((i, v) in arr.entries()) { print(i, v) }
```

Single-variable iteration yields elements/keys depending on container type. Pair iteration yields `(index, element)` for arrays/strings and `(key, value)` for maps/Json objects.

### 4.5 `match` Statement

```xray
match (action) {
    "start" -> start()
    "stop" -> stop()
    _ -> print("unknown")
}
```

`match` can be used as a statement or expression.

### 4.6 `break` and `continue`

`break` and `continue` are only valid inside loops. They are not labeled.

### 4.7 `return`

```xray
return
return 42
return (a, b)
```

Multiple return values are represented as a tuple and must be parenthesized.

### 4.8 `throw`, `try`, `catch`, `finally`

```xray
try {
    risky()
} catch (e: Exception) {
    print(e.message)
} finally {
    cleanup()
}
```

A `try` statement must have at least one `catch` or a `finally` block.

### 4.9 `defer`

```xray
fn read(path: string) -> string {
    let f = open(path)
    defer f.close()
    return f.readAll()
}
```

`defer` runs at function exit in reverse declaration order. It is function-scoped, not block-scoped.

### 4.10 `yield`

```xray
yield
```

`yield` explicitly gives the scheduler a safepoint. It does not yield a value.

---

## 5. Declarations

### 5.1 Variables

```xray
let x = 1
let y: int
const PI = 3.14159
shared const CONFIG = { host: "localhost" }
shared let state = 0
```

`let` creates a mutable binding. `const` creates an immutable binding and must be initialized. A declaration without an initializer needs a type annotation and receives a zero value.

`shared const` stores immutable data in a shared/global region. `shared let` is explicit mutable shared state and is subject to move/concurrency restrictions.

### 5.2 Functions

```xray
fn add(a: int, b: int) -> int {
    return a + b
}

fn log(msg: string) {
    print(msg)
}
```

A no-return function may omit `-> ()`. Function parameters use `name: Type`. Rest parameters use `...name: Type`.

### 5.3 Classes

```xray
class Animal {
    name: string

    constructor(name: string) {
        this.name = name
    }

    fn speak() -> string {
        return "..."
    }
}
```

Classes support fields, methods, constructors, inheritance, interfaces, visibility/modifier syntax, static members where implemented, and operator overload declarations where supported by the runtime/analyzer.

### 5.4 Structs

```xray
struct Point {
    x: float
    y: float
}
```

Struct declarations use class-like member syntax. They are value-oriented declarations and can implement interfaces.

### 5.5 Interfaces

```xray
interface Drawable {
    draw() -> ()
}
```

Interfaces describe method contracts. A class/struct/enum may declare `implements InterfaceName`.

### 5.6 Enums

#### Simple Backed Enums

```xray
enum Color {
    Red = "red",
    Green = "green",
    Blue = "blue"
}
```

Backed enum values may be `int`, `float`, `string`, or `bool`. All members in one backed enum must use the same backing type. Mixed backing value types are a compile-time analyzer error.

#### ADT Enums

```xray
enum Result<T, E> {
    Ok(T)
    Err(E)
}
```

ADT variants may carry payloads and are destructured with `match`.

### 5.7 Type Aliases

```xray
type Mapper = (int) -> int
type User = { name: string, age: int }
```

Aliases do not introduce new nominal types. Object aliases are sealed when used for annotations.

### 5.8 Import and Export

```xray
import time
import http as httpClient
import alice/utils as utils
import "./helper.xr" as helper
import "models/user" as user
import { readFile, writeFile as write } from io
import { publicFn } from "./helper.xr"

export fn publicFn() -> string { return "hi" }
export const VERSION = "1.0"
export publicFn, VERSION
export { publicFn as fnAlias } from "./other"
export * from "./other"
```

JavaScript default imports (`import name from "module"`) are not supported.

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

### 8.1 Exceptions

```xray
throw new Exception("message")
```

Only `Exception` and subclasses may be thrown. Throwing a string, number, Json value, or null is a compile-time error.

`Exception` fields:

| Field | Meaning |
|--|--|
| `message` | human-readable message |
| `stack` | stack trace lines |
| `cause` | optional chained cause |
| `code` | numeric error code |
| `data` | optional structured payload |

### 8.2 Try/Catch/Finally

```xray
try {
    risky()
} catch (e: HttpError) {
    handle(e)
} catch (e) {
    fallback(e)
} finally {
    cleanup()
}
```

`finally` executes during both normal and exceptional exits.

### 8.3 `try?` and `try!`

`try? expr` converts an exception into `null`. `try! expr` rethrows the original exception.

### 8.4 `catch!`

`catch! { ... }` evaluates a block and wraps success/failure into a Result-style value when the built-in Result enum is available.

### 8.5 `defer`

`defer` is a resource-management tool that runs at function exit and is compatible with exceptional exits.

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
    draw() -> () { ... }
}

class Wrong {
    draw() -> () { ... }
}

fn render(d: Drawable) { ... }
render(new Square())     // OK
render(new Wrong())      // compile error: Wrong is not Drawable
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

### 10.1 `go`

```xray
let task = go compute()
```

`go` starts a coroutine and returns a `Task<T>` handle.

### 10.2 `await`

```xray
let r = await task
let results = await all [t1, t2, t3]
let first = await any [t1, t2, t3]
let ok = await anySuccess [t1, t2, t3]
```

`await` propagates exceptions from the awaited task. `await all` fails when any task fails. `await anySuccess` skips failing tasks until one succeeds or all fail.

### 10.3 `Task<T>`

Task members:

| Member | Meaning |
|--|--|
| `done` | task completed |
| `cancelled` | task was cancelled |
| `result` | completed result or null |
| `error` | error message/value or null |
| `cancel()` | request cooperative cancellation |

### 10.4 `Channel<T>`

```xray
shared const ch = new Channel<int>(2)
ch.send(10)
let v = ch.recv()
let ok = ch.trySend(20)
let (value, got) = ch.tryRecv()
```

Channel methods:

| Method | Meaning |
|--|--|
| `send(v)` | blocking send; throws on closed channel |
| `recv()` | blocking receive; returns buffered values, then null when closed and empty |
| `trySend(v)` | non-blocking send |
| `tryRecv()` | non-blocking receive returning `(value, ok)` |
| `sendTimeout(v, ms)` | send with timeout |
| `recvTimeout(ms)` | receive with timeout |
| `close()` | close the channel |
| `isClosed` / `isClosed()` | closed state |

### 10.5 `select`

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
    msg from ch1 -> { print("got from ch1:", msg) }
    msg from ch2 -> { print("got from ch2:", msg) }
    100 to ch1 -> { print("sent 100 to ch1") }
    after 1000 -> { print("timeout") }
    _ -> { print("no channel ready") }
}
```

Semantics:

- Receive arm `name from ch -> body`: equivalent to `name = ch.recv()`, but selected only when `ch` has data.
- Send arm `value to ch -> body`: equivalent to `ch.send(value)`, but selected only when `ch` has capacity.
- Timeout arm `after ms -> body`: selected when no channel arm becomes ready before the timeout.
- Default arm `_ -> body`: runs immediately when no arm is ready; omitting it makes `select` block until an arm becomes ready.
- When multiple arms are ready at the same time, one is selected randomly, matching Go.

### 10.6 `scope`

```xray
scope { ... }
linked scope { ... }
supervisor scope { ... }
```

A scope controls the lifetime of child coroutines. Linked and supervisor scopes provide different cancellation/failure propagation policies.

### 10.7 Safety Model

The language design makes cross-coroutine sharing explicit. Ordinary locals are isolated. Shared immutable data is marked `shared const`. Mutable sharing is explicit and constrained. Channels are the preferred way to communicate mutable values.

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
export fn publicFn() -> string { return "hi" }
export class PublicClass { ... }
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
fn test_basic() { ... }

@test(skip)                           // skip this test
fn test_wip() { ... }

@native                               // C implementation
class Array<T> {
    length: int
    push(v: T)
    // no method bodies — provided by src/runtime/object/xarray_methods.c
}

@deprecated("use newAPI() instead")
fn oldAPI() { ... }
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

## 18. Error Codes

Error code definitions live in runtime error headers. Families include:

- Lexical errors.
- Parser errors.
- Compiler/analyzer errors.
- Runtime errors.
- Json errors.
- Coroutine/channel errors.
- Assertion errors.

Important semantic error cases:

- `break`, `continue`, or `return` outside a valid context.
- Throwing a non-`Exception` value.
- Mixed backed enum value types.
- Invalid sealed object/Json field access.
- Invalid assignment to readonly/const targets.
- Invalid channel operations such as sending to a closed channel.

---

## Appendix A. EBNF

```ebnf
Program ::= Statement* EOF

Statement ::= ExprStmt
            | VarDecl
            | FnDecl
            | ClassDecl
            | StructDecl
            | InterfaceDecl
            | EnumDecl
            | TypeAliasDecl
            | ImportDecl
            | ExportDecl
            | IfStmt
            | WhileStmt
            | ForStmt
            | ForInStmt
            | MatchStmt
            | ScopeStmt
            | SelectStmt
            | ReturnStmt
            | BreakStmt
            | ContinueStmt
            | ThrowStmt
            | TryStmt
            | DeferStmt
            | YieldStmt
            | Block

Block ::= '{' Statement* '}'

Type ::= UnionType
UnionType ::= PrimaryType ('|' PrimaryType)*
PrimaryType ::= NamedType | FunctionType | TupleType | ObjectType | PrimaryType '?'
NamedType ::= QualifiedIdent TypeArgs?
FunctionType ::= '(' TypeList? ')' '->' Type
TupleType ::= '(' Type (',' Type)+ ')'
ObjectType ::= '{' ObjectField (',' ObjectField)* ','? '}'
ObjectField ::= Identifier ':' Type

Expression ::= Assignment
Assignment ::= Ternary (AssignOp Expression)?
Ternary ::= LogicOr ('?' Expression ':' Expression)?
Primary ::= Literal | Identifier | ArrayLit | MapLit | SetLit | ObjectLit
          | Closure | MatchExpr | '(' Expression ')' | TupleExpr

ArrayLit ::= '[' (Expression (',' Expression)* ','?)? ']'
MapLit ::= '#{' (Expression ':' Expression (',' Expression ':' Expression)* ','?)? '}'
SetLit ::= '#[' (Expression (',' Expression)* ','?)? ']'
ObjectLit ::= '{' ObjectFieldExpr (',' ObjectFieldExpr)* ','? '}'
ObjectFieldExpr ::= Identifier ':' Expression | Identifier
Closure ::= '(' ParamList? ')' '->' (Expression | Block)
MatchExpr ::= 'match' '(' Expression ')' '{' MatchArm+ '}'
MatchArm ::= Pattern ('if' '(' Expression ')')? '->' (Expression | Block)

ImportDecl ::= 'import' ImportMembers 'from' ImportModule
             | 'import' ImportModule ('as' Identifier)?
ImportMembers ::= '{' ImportMember (',' ImportMember)* ','? '}'
ImportMember ::= Identifier ('as' Identifier)?
ImportModule ::= StringLiteral | Identifier ('/' Identifier)?

ExportDecl ::= 'export' Declaration
             | 'export' Identifier (',' Identifier)*
             | 'export' '{' ExportSpec (',' ExportSpec)* '}' 'from' StringLiteral
             | 'export' '*' 'from' StringLiteral
ExportSpec ::= Identifier ('as' Identifier)?

SelectStmt ::= 'select' '{' SelectArm+ '}'
SelectArm  ::= RecvArm | SendArm | TimeoutArm | DefaultArm
RecvArm    ::= Identifier 'from' Expression '->' Block
SendArm    ::= Expression 'to' Expression '->' Block
TimeoutArm ::= 'after' Expression '->' Block
DefaultArm ::= '_' '->' Block
```

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

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

### 0.1 Scope

This document is the reference manual for the Xray programming language. It describes the implemented lexical grammar, syntax, type system, expression and statement semantics, declarations, patterns, module system, concurrency model, error handling model, built-in APIs, standard library surface, runtime model, compilation pipeline, and error-code conventions.

The goals are:

1. A human reader can write valid Xray code with predictable behavior.
2. Compiler, analyzer, IDE, LSP, documentation, and AI tooling can use this as a structured reference.
3. The document stays aligned with the main repository implementation.

This manual is not a tutorial. Runnable examples are under `demos/`; regression and behavioral examples are under `tests/`.

### 0.2 Versioning

The spec version follows the repository version in `CMakeLists.txt`, `project(Xray VERSION x.y.z)`. Xray is still in the `0.x` series; backward compatibility is not guaranteed.

### 0.3 Design Philosophy

Xray is a lightweight statically typed scripting language with native concurrency.

| Dimension | Choice |
|--|--|
| Types | Static types plus local inference; source-level `any` is not available. |
| Concurrency | M:N coroutines, `go`, `await`, `Channel<T>`, `select`, and structured `scope`. |
| Execution | VM, JIT, and AOT execution modes share the same semantics. |
| Errors | Exceptions, `try?`, `try!`, `catch!`, nullable values, Result-style ADTs, and `defer`. |
| Interop | Native modules are implemented in C and loaded through the runtime module ABI. |
| Tooling | The parser/analyzer/runtime source is the ground truth for syntax and behavior. |

Xray borrows ideas from TypeScript, Go, Rust, Swift, and Python, but it is not a clone of any of them.

### 0.4 Notation

EBNF notation in this document uses:

| Notation | Meaning |
|--|--|
| `Term` | non-terminal |
| `'token'` | literal token |
| `A B` | sequence |
| `A | B` | choice |
| `A?` | optional |
| `A*` | zero or more |
| `A+` | one or more |
| `(A)` | grouping |

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

Patterns appear in `match` arms and destructuring.

### 6.1 Literal Patterns

```xray
match (x) {
    0 -> "zero"
    "hello" -> "greeting"
    true -> "yes"
    null -> "nothing"
    _ -> "other"
}
```

### 6.2 Range Patterns

```xray
match (age) {
    0..13 -> "child"
    13..20 -> "teen"
    _ -> "adult"
}
```

### 6.3 Wildcards and Bindings

`_` matches without binding. A bare identifier binds the matched value.

### 6.4 Tuple Patterns

```xray
match (pair) {
    (0, y) -> y
    (x, y) -> x + y
}
```

### 6.5 Type Patterns

```xray
match (x) {
    is string s -> s.length
    is int n -> n
    _ -> 0
}
```

### 6.6 ADT Variant Patterns

```xray
match (result) {
    Result.Ok(v) -> v
    Result.Err(e) -> throw new Exception(string(e))
}
```

### 6.7 Guards

```xray
match (n) {
    x if (x > 0) -> "positive"
    _ -> "other"
}
```

---

## 7. Scoping and Name Resolution

Each block creates a lexical scope. Declarations are resolved through lexical parent scopes and then module/prelude scopes as appropriate.

Functions may capture outer variables. Coroutines started with `go` use stricter capture rules to avoid accidental data races:

- `shared const` may be captured directly.
- Ordinary local variables cannot be captured directly by `go` closures.
- Mutable shared state must be explicit and often transferred or synchronized.
- Channels are the preferred communication mechanism.

Modules are private by default. Only exported declarations are visible to importers.

Circular module dependencies are rejected.

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

Generic functions, classes, structs, interfaces, and ADT enums use type parameters:

```xray
fn id<T>(x: T) -> T { return x }
class Box<T> { value: T }
enum Option<T> { Some(T) None }
```

Constraints use `:`:

```xray
fn max<T: Comparable>(a: T, b: T) -> T {
    return a > b ? a : b
}
```

Multiple constraints use `&`:

```xray
fn f<T: Comparable & Stringable>(x: T) -> string {
    return x.toString()
}
```

Built-in constraint interfaces include:

| Constraint | Meaning |
|--|--|
| `Comparable` | supports ordering comparisons |
| `Hashable` | valid as a Map/Set key |
| `Stringable` | has string conversion |
| `Iterable<T>` | can be used in `for-in` |

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

### 11.1 Module Identity

Each `.xr` file is a module. Module paths follow directory structure. A module exports only declarations explicitly marked with `export`.

### 11.2 Import Syntax

```ebnf
ImportStmt ::= 'import' ImportMembers 'from' ImportModule
            |  'import' ImportModule ('as' Identifier)?
ImportMembers ::= '{' ImportMember (',' ImportMember)* ','? '}'
ImportMember  ::= Identifier ('as' Identifier)?
ImportModule  ::= StringLiteral | ModuleName
ModuleName    ::= Identifier ('/' Identifier)?
```

Examples:

```xray
import time
import http as h
import alice/utils
import "./helper.xr" as helper
import "models/user" as user
import { readFile, writeFile as write } from io
```

### 11.3 Export Syntax

```xray
export fn f() {}
export const VERSION = "1.0"
export f, VERSION
export { a, b as c } from "./m"
export * from "./m"
```

### 11.4 Resolution

Resolution distinguishes:

1. Standard library modules.
2. Relative quoted paths (`./`, `../`).
3. Project-root quoted paths.
4. Third-party `owner/name` package names.

JavaScript default import syntax is rejected.

---

## 12. Testing

Tests are marked with `@test`:

```xray
@test
fn test_addition() {
    assert_eq(1 + 1, 2)
}
```

Test functions are discovered by `xray test`. They usually take no parameters and use global assertion builtins.

Recognized attributes include:

| Attribute | Applies to | Meaning |
|--|--|--|
| `@test` | function | mark a test function |
| `@native` | function/class/struct | native implementation |
| `@deprecated` | declaration | deprecation warning |

Assertion functions:

| Function | Meaning |
|--|--|
| `assert(cond, msg?)` | require condition |
| `assert_true(cond)` | require true |
| `assert_false(cond)` | require false |
| `assert_eq(a, b)` | require equality |
| `assert_ne(a, b)` | require inequality |
| `assert_throws(fn)` | require thrown exception |

---

## 13. Built-in Functions

### 13.1 I/O and Debugging

| Function | Signature | Meaning |
|--|--|--|
| `print` | `(...values) -> ()` | print to stdout with trailing newline |
| `dump` | `(value, indent?) -> ()` | structured debug output |

### 13.2 Conversion

| Function | Signature | Meaning |
|--|--|--|
| `int(x)` | `(value) -> int` | convert to int |
| `float(x)` | `(value) -> float` | convert to float |
| `string(x)` | `(value) -> string` | convert to string |
| `bool(x)` | `(value) -> bool` | convert to bool |
| `chr(n)` | `(int) -> string` | code point to string |
| `copy(x)` | `(T) -> T` | deep copy |

### 13.3 Type Inspection

| Function | Signature | Meaning |
|--|--|--|
| `typeof(x)` | `(value) -> string` | runtime type name |
| `x is T` | expression | runtime type check and possible narrowing |

### 13.4 Assertions

`assert`, `assert_true`, `assert_false`, `assert_eq`, `assert_ne`, and `assert_throws` are global builtins.

### 13.5 Constructors and Static Functions

| API | Meaning |
|--|--|
| `Array()` / `Array(n)` / `Array(n, value)` | create arrays |
| `Array.from(iterable)` | create from string/array/set/map |
| `Array.range(start, end)` | inclusive integer array |
| `Array.withCapacity(n)` | allocate capacity with length 0 |
| `Map()` | create empty map |
| `Map.from(entries)` / `Map.from(keys, values)` | create maps |
| `Set()` / `Set(array)` | create sets |
| `Set.from(iterable)` | create set from iterable |
| `Set.range(start, end)` | inclusive integer set |

---

## 14. Built-in Type Methods

### 14.1 Numeric and Bool Methods

`int`: `abs`, `toString`, `toBigInt`, `toFloat`, `toHex`, `max`, `min`, `floor`, `ceil`, `round`, `sqrt`, `pow`.

`float`: `abs`, `toString`, `toFixed`, `toInt`, `floor`, `ceil`, `round`, `sqrt`, `pow`.

`BigInt`: `abs`, `toString`, `sign`, `isZero`, `isNegative`, `isPositive`, `toInt`, `toFloat`.

`bool`: `toString`.

### 14.2 `string`

Supported members include `length`, `charAt`, `charCodeAt`, `concat`, `includes`, `indexOf`, `lastIndexOf`, `slice`, `substring`, `substr`, `toLowerCase`, `toUpperCase`, `trim`, `trimStart`, `trimEnd`, `split`, `replace`, `replaceAll`, `repeat`, `startsWith`, `endsWith`, `padStart`, `padEnd`, `match`, `iterator`, `entriesIterator`, and `entries`.

### 14.3 `Bytes`

`Bytes` is a prelude byte-buffer type. Construction is handled by builtin paths such as `Bytes(n)` and `Bytes(n, fill)`. Encoding/decoding helpers live in modules such as `encoding` and `base64`.

### 14.4 `Array<T>`

Supported members include `length`, indexing, `push`, `pop`, `shift`, `unshift`, `slice`, `splice`, `concat`, `indexOf`, `includes`, `join`, `reverse`, `sort`, `map`, `filter`, `reduce`, `forEach`, `find`, `findIndex`, `every`, `some`, `flat`, `fill`, `copyWithin`, `iterator`, `entriesIterator`, and `entries`.

### 14.5 `Map<K, V>`

Supported members include `length`, indexing, `get`, `set`, `has`, `delete`, `clear`, `keys`, `values`, `entries`, `forEach`, `iterator`, and `entriesIterator`.

Map literals are written with a `#` prefix and colon entries:

```xray
let m = #{"k": 1}
```

### 14.6 `Set<T>`

Supported members include `length`, `add`, `has`, `delete`, `clear`, `values`, `forEach`, and `iterator`.

Set literals use `#[...]`.

### 14.7 `Json`

Json field data is accessed with normal field/index syntax. Generic utility functions are static methods on `Json`:

`keys`, `values`, `entries`, `has`, `get`, `size`, `isEmpty`, `parse`, `tryParse`, `isValid`, and `stringify`.

### 14.8 `Channel<T>`

Methods: `send`, `recv`, `trySend`, `tryRecv`, `sendTimeout`, `recvTimeout`, `close`, and `isClosed`/`isClosed()`.

### 14.9 `DateTime`

`DateTime` instances provide component properties (`year`, `month`, `day`, `hour`, `minute`, `second`, `millisecond`, `weekday`, `yearday`, `timestamp`) and methods (`toString`, `format`, `toISOString`, `add`, `diff`, `toUTC`, `toLocal`, `isBefore`, `isAfter`, `equals`, `isLeapYear`, `daysInMonth`).

The `datetime` module exports factory functions: `now`, `utc`, `create`, `createUTC`, `fromTimestamp`, `fromTimestampMs`, `parse`, and `offset`.

### 14.10 `Regex`

Methods: `test`, `find`, `findAll`, `replace`, and `split`.

### 14.11 `StringBuilder`

Members: `length`, `append`, `toString`, and `clear`.

### 14.12 `Exception`, `Task`, and Enum Runtime Types

`Exception` exposes `message`, `stack`, `cause`, `code`, `data`, and `toString()`.

`Task<T>` exposes `done`, `cancelled`, `result`, `error`, and `cancel()`.

`EnumValue` exposes `name`, `value`, `ordinal`, and `toString()`. `EnumType` exposes `name`, `memberCount`, and `getMember(name)`.

---

## 15. Standard Library

Native stdlib modules exported by C module loaders:

`base64`, `cluster`, `compress`, `crypto`, `csv`, `datetime`, `encoding`, `gc`, `http`, `io`, `log`, `math`, `net`, `os`, `path`, `regex`, `time`, `toml`, `url`, `ws`, `xml`, and `yaml`.

Runtime/analyzer built-in modules also include `Coro`, `CoroPool`, and `Reflect`.

### 15.1 File I/O and System

| Module | APIs |
|--|--|
| `io` | file and stream I/O |
| `path` | path manipulation |
| `os` | environment, process, platform, sleep, exec |

### 15.2 Networking

| Module | APIs |
|--|--|
| `net` | TCP/UDP/TLS and DNS-like lookup |
| `http` | HTTP client/server helpers |
| `ws` | WebSocket support |
| `url` | URL parsing/formatting/query helpers |

### 15.3 Data Formats

| Module | APIs |
|--|--|
| `yaml` | YAML parsing/writing |
| `toml` | TOML parsing/writing |
| `xml` | XML parsing/writing |
| `csv` | CSV parsing/writing |
| `base64` | Base64 encoding/decoding |
| `encoding` | generic encodings |

JSON does not use a separate `json` module; use the built-in `Json` type static methods.

### 15.4 Other Areas

`crypto` handles hashing and crypto helpers. `compress` handles compression. `time` and `datetime` handle time. `math` provides math functions and constants. `log` provides structured logging. `gc` provides diagnostics/control. `cluster` provides distributed coordination helpers.

Modules that do not exist as separate current stdlib modules include `fs`, `process`, `dns`, `random`, `strconv`, `sync`, `runtime`, and `json`.

---

## 16. Runtime Model

Xray values are represented by tagged runtime values and GC-managed heap objects. The runtime includes class descriptors, native body descriptors, arrays, maps, sets, tuples, strings, BigInts, Json objects, exceptions, tasks, coroutines, channels, and native handles.

Memory regions include:

- System/native heap.
- Global/shared heap.
- Per-coroutine heap.
- VM frames and stacks.
- Parser/compiler arenas.

Garbage collection is based on per-coroutine collection and shared-object handling. Safepoints include calls, backward branches, channel operations, `await`, `yield`, and explicit GC operations.

Coroutines are scheduled M:N over worker threads with cooperative safepoints and work stealing.

---

## 17. Compilation Pipeline

Pipeline:

```text
source
  -> lexer
  -> tokens
  -> parser
  -> AST
  -> analyzer/type checker
  -> typed AST
  -> IR/lowering
  -> bytecode
  -> VM / JIT / AOT
```

Primary implementation areas:

| Area | Directory |
|--|--|
| Lexer/parser | `src/frontend/lexer`, `src/frontend/parser` |
| Analyzer | `src/frontend/analyzer` |
| IR/lowering | `src/ir` |
| VM | `src/vm` |
| JIT | `src/jit` |
| AOT | `src/aot` |
| Runtime objects | `src/runtime` |
| Stdlib native modules | `stdlib` |

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

The authoritative keyword list is in the lexer keyword table and currently contains 63 reserved keywords.

---

## Appendix C. Operator Index

Important operators:

| Group | Operators |
|--|--|
| Arithmetic | `+`, `-`, `*`, `/`, `%` |
| Bitwise | `&`, `|`, `^`, `~`, `<<`, `>>` |
| Comparison | `==`, `!=`, `===`, `!==`, `<`, `<=`, `>`, `>=` |
| Logical | `&&`, `||`, `!` |
| Assignment | `=`, `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=` |
| Other | `..`, `??`, `?.`, `!`, `->` |

The arrow token is `->`.

---

## Appendix D. Standard Library Module Index

| Module | Purpose |
|--|--|
| `base64` | Base64 |
| `cluster` | distributed coordination |
| `compress` | compression |
| `crypto` | hashing/crypto |
| `csv` | CSV |
| `datetime` | DateTime factories |
| `encoding` | encoding helpers |
| `gc` | GC control/diagnostics |
| `http` | HTTP |
| `io` | file/stream I/O |
| `log` | logging |
| `math` | math |
| `net` | networking |
| `os` | OS/environment/process |
| `path` | paths |
| `regex` | regex utilities |
| `time` | time/sleep |
| `toml` | TOML |
| `url` | URL |
| `ws` | WebSocket |
| `xml` | XML |
| `yaml` | YAML |

---

## Appendix E. Differences from Other Languages

### JavaScript / TypeScript

- Xray is statically typed; there is no source-level `any`.
- `Map` literals use `#{...}`, not object syntax.
- Imports are Xray-specific and do not support JS default import syntax.
- Concurrency is coroutine/channel based, not Promise-based.

### Go

- Xray has classes, structs, interfaces, enums, exceptions, and nullable types.
- `go` is an expression returning `Task<T>`.
- Channels are `Channel<T>` objects with methods.
- `select` uses `x from ch ->`, `v to ch ->`, `after ms ->`, and `_ ->` arms.

### Rust

- Xray uses GC and explicit cross-coroutine sharing instead of a full borrow checker.
- ADT enums and Result-style values are supported, but exceptions are also part of the language.

### Swift

- Xray has `T?`, `try?`, and `try!`, but `try!` rethrows rather than aborting.
- Concurrency is coroutine/channel based.

---

## Appendix F. Glossary

| Term | Meaning |
|--|--|
| AOT | Ahead-of-time compilation |
| AST | Abstract syntax tree |
| Channel | Typed coroutine communication pipe |
| Closure | Function value capturing outer variables |
| Coroutine | User-space schedulable execution unit |
| GC | Garbage collection |
| JIT | Just-in-time compilation |
| Nullable | Type that can contain `null` |
| Pattern | Match/destructuring form |
| Prelude | Built-in symbols available without import |
| Safepoint | Point where coroutine/GC scheduling can observe execution |
| SSA | Static single assignment IR |
| VM | Bytecode virtual machine |

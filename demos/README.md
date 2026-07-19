# Xray Language Demos

Learn Xray step by step — from basics to advanced concurrency.

Every demo is a standalone `.xr` file you can run directly:

```bash
xray demos/01-basics/hello.xr
```

## Learning Path

### 01 — Basics

| File | Topics |
|------|--------|
| [hello.xr](01-basics/hello.xr) | print, string interpolation |
| [variables.xr](01-basics/variables.xr) | var, const, types, nullable, destructuring |
| [functions.xr](01-basics/functions.xr) | fn, arrow functions, default params, rest params, recursion |
| [tuples.xr](01-basics/tuples.xr) | tuple literals, `.N` access, destructure, match patterns, spread, generics |
| [control_flow.xr](01-basics/control_flow.xr) | if/else, while, for, for-in, match, try/catch |

### 02 — Collections

| File | Topics |
|------|--------|
| [arrays.xr](02-collections/arrays.xr) | Array creation, slicing, map/filter/reduce |
| [maps_and_sets.xr](02-collections/maps_and_sets.xr) | Map, Set, object literals |
| [json_processing.xr](02-collections/json_processing.xr) | Json type (prelude), dynamic access, structural typing |

### 03 — Object-Oriented Programming

| File | Topics |
|------|--------|
| [classes.xr](03-oop/classes.xr) | class, inheritance, override, static, polymorphism |
| [structs_and_interfaces.xr](03-oop/structs_and_interfaces.xr) | struct, interface, enum, match with enum |

### 04 — Functional Programming

| File | Topics |
|------|--------|
| [functional.xr](04-functional/functional.xr) | closures, higher-order functions, currying, composition, memoization |

### 05 — Concurrency ⭐

Xray's core differentiator. **If it compiles, it's concurrency-safe.**

| File | Topics |
|------|--------|
| [goroutines.xr](05-concurrency/goroutines.xr) | go, await, await all, await any, Task |
| [channels.xr](05-concurrency/channels.xr) | Channel, send/recv, producer-consumer, fan-out |
| [select_and_scope.xr](05-concurrency/select_and_scope.xr) | select, defer, scope (structured concurrency) |
| [shared_data.xr](05-concurrency/shared_data.xr) | shared, Channel, parameter passing — the 3 sharing rules |
| [atomic.xr](05-concurrency/atomic.xr) | Atomic&lt;T&gt; for int/float/bool — load, store, add, CAS, toggle |

### 06 — Networking

| File | Topics |
|------|--------|
| [http_server.xr](06-networking/http_server.xr) | HTTP server, routes, JSON API |

### 07 — Advanced

| File | Topics |
|------|--------|
| [generics.xr](07-advanced/generics.xr) | generic classes, nullable types, optional chaining |
| [testing.xr](07-advanced/testing.xr) | @test, assertions, skip, timeout |
| [borrow_span_examples.xr](07-advanced/borrow_span_examples.xr) | value/in/ref/out parameters, copy/move, Slice, managed Buffer |

Run tests with: `xray test demos/07-advanced/testing.xr`

## Quick Reference

```xray
// Variables
var x = 1              // mutable
const PI = 3.14        // immutable

// Functions (params MUST have type annotations)
fn add(a: int, b: int) -> int { return a + b }
var double = (x: int) -> x * 2

// Tuples (heterogeneous, fixed arity, .N access)
var p: (int, string) = (1, "hi")
print(p.0); print(p.1)
fn divmod(a: int, b: int) -> (int, int) { return (a / b, a % b) }
var (q, r) = divmod(17, 5)
var combined = (...p, true)         // spread → (1, "hi", true)

// Concurrency
var task = go compute(42)      // spawn coroutine
var result = await task         // wait for result
shared ch = Channel<int>(10)
ch.send(val); match (ch.recv()) { Recv.Value(v) -> use(v); _ -> {} }
shared CFG = { ... }     // immutable cross-coroutine data
```

## `owned` Bindings

`owned` is a **local storage declaration**, not a type modifier. For example, the type of `payload` below is still `Array<byte>`; `owned` states that this binding holds a unique, mutable reference identity.

- An `owned` declaration must be a local, single-name declaration with an initializer. Module-level and destructuring `owned` declarations are rejected.
- The binding name cannot be reassigned, but the owned object can be mutated according to its type.
- A freshly constructed reference value can initialize an `owned` binding directly. An existing reference binding must be transferred with `move source` or duplicated with `copy(source)`.
- `move` can transfer the identity to another `owned` or `shared` binding, a function or coroutine, a Channel, or a return value. The source binding is invalid after the move.
- A `Slice` or raw pointer borrowed from an owned object must not remain live when that object is moved.

```xray
fn byteCount(data: Array<byte>) -> int {
    return len(data)
}

fn ownedExample() {
    owned payload = Array<byte>(4, 0)
    payload[0] = 7
    owned snapshot = copy(payload)
    var task = go byteCount(move payload)
    var count = await task
    print("moved length: ${count}")
    print("snapshot first byte: ${snapshot[0]}")
}

ownedExample()
```

## Concurrency Safety Rules

| Mechanism | Purpose | How it works |
|-----------|---------|-------------|
| `owned` | Unique local identity | Creates a non-rebindable local owner that can be transferred with `move` |
| `shared` | Stable shared identity | Crosses execution boundaries directly; mutation is limited by the shared type's API |
| `Channel` | Boundary communication | Uses the same explicit `copy` / `move` / `shared` transfer rules as `go` |
| `copy(value)` | Independent data | Deep-copies an execution-local reference graph; the source remains usable |
| `move value` | Ownership transfer | Transfers a local `var` or `owned` identity and invalidates the source |

Every `go` argument, `go` closure capture, and Channel send uses the same transfer plan. Inline values, published module-readonly values, and shared identities may cross directly. Execution-local reference graphs require explicit `copy(value)` or `move value`, while mutable captured locals and borrowed views or pointers that cannot outlive the boundary are rejected.

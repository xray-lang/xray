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
| [variables.xr](01-basics/variables.xr) | let, const, types, nullable, destructuring |
| [functions.xr](01-basics/functions.xr) | fn, arrow functions, default params, rest params, recursion |
| [control_flow.xr](01-basics/control_flow.xr) | if/else, while, for, for-in, match, try/catch |

### 02 — Collections

| File | Topics |
|------|--------|
| [arrays.xr](02-collections/arrays.xr) | Array creation, slicing, map/filter/reduce |
| [maps_and_sets.xr](02-collections/maps_and_sets.xr) | Map, Set, object literals |
| [json_processing.xr](02-collections/json_processing.xr) | Json type, dynamic access, json module |

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
| [goroutines.xr](05-concurrency/goroutines.xr) | go, await, await.all, await.any, Task |
| [channels.xr](05-concurrency/channels.xr) | Channel, send/recv, producer-consumer, fan-out |
| [select_and_scope.xr](05-concurrency/select_and_scope.xr) | select, defer, scope (structured concurrency) |
| [shared_data.xr](05-concurrency/shared_data.xr) | shared const, Channel, parameter passing — the 3 sharing rules |

### 06 — Networking

| File | Topics |
|------|--------|
| [http_server.xr](06-networking/http_server.xr) | HTTP server, routes, JSON API |

### 07 — Advanced

| File | Topics |
|------|--------|
| [generics.xr](07-advanced/generics.xr) | generic classes, nullable types, optional chaining |
| [testing.xr](07-advanced/testing.xr) | @test, assertions, skip, timeout |

Run tests with: `xray test demos/07-advanced/testing.xr`

## Quick Reference

```xray
// Variables
let x = 1              // mutable
const PI = 3.14        // immutable

// Functions (params MUST have type annotations)
fn add(a: int, b: int): int { return a + b }
let double = (x: int): int => x * 2

// Concurrency
let task = go compute(42)      // spawn coroutine
let result = await task         // wait for result
const ch = Channel(10)          // channel (must be const)
ch.send(val); ch.recv()         // communicate
shared const CFG = { ... }     // immutable cross-coroutine data
```

## Concurrency Safety Rules

| Mechanism | Purpose | How it works |
|-----------|---------|-------------|
| `shared const` | Immutable sharing | Zero-copy reads across coroutines |
| `Channel` | Communication | Deep-copies values on send |
| Function params | Pass data to `go` | Deep-copied to child coroutine |
| `move` | Transfer ownership | Original becomes inaccessible |

Regular `let`/`const` variables **cannot** be captured by `go` closures — the compiler will reject it.

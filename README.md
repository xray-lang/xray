[**English**](README.md) | [中文](README_CN.md)

# Xray

A statically-typed scripting language with native concurrency, built in C.

```ts
import http

fn dashboard(req) {
    let [users, orders] = await.all [
        go loadUsers(),
        go loadOrders()
    ]
    return { users: users, count: orders.length }
}

http.route("GET", "/dashboard", dashboard)
http.listen(8080)
```

## Features

- **Static typing with inference** — generics, union types, type narrowing, optional types
- **Native concurrency** — `go` / `await` / `Channel` / `scope` with M:N work-stealing scheduler
- **Structured concurrency** — `linked scope` (error propagation) / `supervisor scope` (error isolation)
- **ARM64 JIT compiler** — method-based compilation with on-stack replacement (OSR)
- **AOT compilation** — compile to standalone native binary via C transpilation
- **Per-coroutine GC** — Immix mark-region collector, no global stop-the-world
- **First-class JSON** — dynamic, typed, extensible, partial-immutable — all in one value type
- **Built-in HTTP server** — one coroutine per connection, auto JSON serialization
- **Built-in cluster** — named channels, RPC, pub/sub, LAN auto-discovery — no external deps
- **Full toolchain** — `run`, `test`, `fmt`, `build`, `check`, `repl`, `lsp`, `dap`
- **Cross-platform** — macOS, Linux (io_uring), Windows (IOCP); arm64 + x86_64
- **Embeddable** — C API for embedding into host applications

## Install

```bash
git clone https://github.com/xray-lang/xray.git
cd xray
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
./build/xray --version   # e.g. xray v0.5.x (JIT, arm64-darwin)
```

```bash
xray app.xr              # run a script
xray -e 'print("hi")'    # evaluate expression
xray test                 # run @test functions
xray repl                 # interactive REPL
```

## Language

### Type System

```ts
// Inference + explicit annotations
let name = "Alice"
const PI: float = 3.14159

// Generics
class Stack<T> {
    items: Array<T>
    constructor() { this.items = [] }

    push(item: T): void { this.items.push(item) }
    pop(): T? { return this.items.pop() }
}

let s = new Stack<int>()
s.push(42)

// Union types + type narrowing
type Result = int | string | null

fn parse(input: string): Result {
    if (input == "42") { return 42 }
    if (input == "") { return null }
    return input
}

let r = parse("42")
if (r is int) { print("got int:", r) }
```

### Concurrency — go / await / Channel / scope

```ts
// Launch a coroutine and await its result
let task = go fn(): int { return 42 }
print(await task)   // 42

// Structured concurrency: scope waits for all children
shared const results: Array<string> = []

fn work(id: int, out: Array<string>): void {
    out.push("task " + id.toString())
}

scope {
    go work(1, results)
    go work(2, results)
    go work(3, results)
}
// all tasks finished here
print(results.length)   // 3

// Channel-based producer/consumer
fn producer(ch: Channel<int>): void {
    for (i in 0..5) { ch.send(i) }
    ch.close()
}

shared const pipe = Channel(8)
scope {
    go producer(pipe)
    for (msg in pipe) { print(msg) }
}
```

### Structs — Value Types with Operator Overloading

```ts
import math

struct Vec2 {
    x: float
    y: float

    length(): float {
        return math.sqrt(this.x * this.x + this.y * this.y)
    }

    operator +(other: Vec2): Vec2 {
        return Vec2{x: this.x + other.x, y: this.y + other.y}
    }
}

let a = Vec2{x: 3.0, y: 4.0}
let b = Vec2{x: 1.0, y: 2.0}
let c = a + b
print(c.x, c.y)    // 4 6
```

### Classes, Interfaces, Enums

```ts
interface Serializable {
    serialize(): string
}

class User implements Serializable {
    name: string
    age: int

    constructor(name: string, age: int) {
        this.name = name
        this.age = age
    }

    serialize(): string {
        return this.name + ":" + this.age.toString()
    }
}

enum Status { Active, Inactive, Banned }

let s = Status.Active
let label = match s {
    Status.Active => "active",
    Status.Inactive => "inactive",
    Status.Banned => "banned"
}
```

### Built-in Json — From Dynamic to Typed

Json is a first-class value type. Combined with `type` definitions, it supports a spectrum from fully dynamic to strictly typed — no need to choose between "define a full class" and "give up type checking".

```ts
// Fully dynamic — add any field at any time
let obj = { name: "Alice", age: 30 }
obj.email = "alice@test.com"
obj["score"] = 100

// Strict type — only declared fields allowed
type Point = { x: int, y: int }
let p: Point = { x: 10, y: 20 }

// Extensible type — core fields are typed, extras are free
type Config = { host: string, port: int, ... }
let cfg: Config = { host: "localhost", port: 8080 }
cfg.debug = true          // allowed
cfg.timeout = 30          // allowed

// Partial immutability with const fields
type Entity = { const id: int, name: string, ... }
let e: Entity = { id: 1, name: "test" }
e.name = "updated"        // ok
e.extra = "allowed"       // ok
// e.id = 2               // compile error: const field

// Optional fields
type User = { name: string, email?: string }
let u: User = { name: "Alice" }

// Computed property keys
let prefix = "user"
let record = { [prefix + "_id"]: 1001, [prefix + "_name"]: "Alice" }

// Iterate key-value pairs
for (k, v in obj) {
    print(k, "=", v)      // name = Alice, age = 30, ...
}
```

### Functional

```ts
let nums = [3, 1, 4, 1, 5, 9, 2, 6]

let result = nums
    .filter((x: int) => x > 3)
    .map((x: int) => x * x)
    .reduce((a: int, b: int) => a + b, 0)

print(result)   // 158
```

### Error Handling

```ts
try {
    let data = json.parse(raw)
    process(data)
} catch (e) {
    print("error:", e)
} finally {
    cleanup()
}

// defer runs at function exit (LIFO order)
fn readFile(path: string): string {
    let f = io.open(path, "r")
    defer f.close()
    return f.readAll()
}
```

## HTTP Server

Each connection runs in its own coroutine — no callbacks, no async/await ceremony.
Handler return values are automatically converted: strings become text responses,
Json objects are stringified with `Content-Type: application/json`.

```ts
import http

fn hello(req) {
    return "Hello, Xray!"
}

fn getUser(req) {
    let id = req.params.id
    return { id: id, name: "Alice" }    // auto JSON response
}

fn createUser(req) {
    let body = json.parse(req.body)
    if (body == null || body.name == null) {
        return http.response(400, { error: "name is required" })
    }
    return http.response(201, { id: 1, name: body.name })
}

http.route("GET", "/", hello)
http.route("GET", "/users/:id", getUser)
http.route("POST", "/users", createUser)
http.listen(8080)
```

## Toolchain

| Command | Description |
| ------- | ----------- |
| `xray run` | Run script or project |
| `xray test` | Run `@test` functions |
| `xray fmt` | Format source code |
| `xray build` | Compile to standalone binary (AOT → C → gcc/clang) |
| `xray compile` | Compile to bytecode |
| `xray check` | Static analysis |
| `xray repl` | Interactive REPL |
| `xray lsp` | Language Server Protocol server |
| `xray dap` | Debug Adapter Protocol server |
| `xray init` | Create new project |
| `xray add/remove` | Manage dependencies |

## Standard Library

| Category | Modules |
| -------- | ------- |
| **Network** | `http` (HTTP/1.1 client/server), `http2`, `ws` (WebSocket), `net` (TCP/UDP/DNS/TLS) |
| **Data** | `json`, `csv`, `toml`, `xml`, `yaml` |
| **Crypto** | `crypto` (SHA-256/512, AES-256, HMAC, UUID) |
| **Encoding** | `base64`, `encoding` (hex), `url`, `compress` (deflate/gzip/zlib) |
| **System** | `io` (files), `os` (env/exec/signals), `path`, `datetime` |
| **Runtime** | `math`, `time`, `log`, `gc`, `regex` |
| **Distributed** | `cluster` (named channels, pub/sub, service mesh — beta) |


## Distributed — Built-in Cluster

No external dependencies. Named Channels, RPC, Pub/Sub, and LAN auto-discovery — all built in C.

```ts
import cluster

// Start a node
cluster.start({ name: "node-A", port: 9001, secret: "my-secret" })

// Join an existing cluster
cluster.join("192.168.1.10:9001")

// Named Channels — first creator is owner, others get transparent proxies
shared const jobs = cluster.channel("jobs", 100)
jobs.send({ task: "render", frame: 42 })      // works across nodes
let job = jobs.recv()

// RPC Services
shared const req_ch = cluster.serve("image_resize")
scope {
    go fn() {
        for (req in req_ch) {
            let result = resize(req.args)
            cluster.reply(req, result)
        }
    }
}
let resized = cluster.call("image_resize", { url: "pic.jpg", width: 800 })

// Pub/Sub with NATS-style wildcards
shared const orders = cluster.subscribe("orders.>")    // wildcard
cluster.publish("orders.new", { id: 1, total: 99.9 })

// Node monitoring
shared const mon = cluster.monitor("*")     // all nodes
// mon receives node name when a node disconnects

// LAN auto-discovery (UDP multicast)
cluster.discover()
print(cluster.nodes())    // ["node-A", "node-B", ...]

cluster.stop()
```

**Architecture**: Owner-model Named Channels (single owner = no consensus overhead),
PHI Accrual failure detector, SHA-256 challenge-response auth, per-node writer coroutines.


## Advanced Features

### select — Channel Multiplexing

```ts
shared const ch1 = Channel(2)
shared const ch2 = Channel(2)

ch1.trySend(100)

select {
    msg from ch1 => { print(msg) }       // 100
    msg from ch2 => { print(msg) }
    _ => { print("no data") }
}
```

### Coroutine Priority

```ts
fn task(label: string): string { return label }

// Inline priority (preferred)
let t1 = go(priority: Coro.HIGH) task("high")
let t2 = go(priority: Coro.LOW) task("low")

// Or set after creation
let t3 = go task("normal")
Coro.setPriority(t3, Coro.HIGH)

await t1
await t2
await t3
```

### linked / supervisor scope — Error Propagation

```ts
// linked scope: child error propagates to parent
try {
    linked scope {
        go ok_worker()
        go failing_worker()   // throws → propagates
    }
} catch (e) {
    print("caught:", e)
}

// supervisor scope: child errors do not cancel siblings
supervisor scope {
    go failing_worker()       // throws → isolated
    go ok_worker()            // still runs
}
```

### Coroutine Naming

```ts
fn worker(id: int): int { return id * 10 }

let t1 = go(name: "worker-1") worker(1)
let t2 = go(name: "counter") {
    let sum = 0
    for (i in 1..5) { sum = sum + i }
    return sum
}

print(await t1)   // 10
print(await t2)   // 10
```

### await.all — Await Multiple Tasks

```ts
fn task1(): int { return 10 }
fn task2(): int { return 20 }

let t1 = go task1()
let t2 = go task2()

let results = await.all [t1, t2]
print(results[0], results[1])   // 10 20

// Destructuring also works
let [x, y] = await.all [go task1(), go task2()]
print(x, y)   // 10 20
```

### cancelled() — Cooperative Cancellation

```ts
fn worker(): string {
    for (i in 0..1000) {
        if (cancelled()) {
            return "stopped at " + i.toString()
        }
        // do work...
    }
    return "done"
}

let task = go worker()
task.cancel()
let result = await task   // null (cancelled before checked)
```

### shared / move — Ownership & Cross-Coroutine Safety

```ts
// shared const: zero-copy concurrent read (reference counted)
shared const config = { name: "app", port: 8080 }

// move: explicit ownership transfer to coroutine or channel
shared let data = { count: 10 }
let task = go fn(d: Json): int {
    return d.count + 1
}(move data)
// data is no longer usable (compile error if referenced after move)

// Normal variable: deep copy to coroutine (no move needed)
let arr = [1, 2, 3]
let t = go fn(d: Array<int>) { d.push(4); return d.length }(arr)
print(arr)             // [1, 2, 3] (unchanged)
```

## Architecture

```
Source → Lexer → Parser → Analyzer → Bytecode → VM
                                          ↓
                                   ARM64 JIT (hot paths)
                                          ↓
                              AOT (C transpilation → native binary)
```

- **Scheduler**: M:N work-stealing (Go-inspired P/M split, lock-free steal queues)
- **GC**: Per-coroutine Immix Mark-Region (bump-pointer alloc, line-granular reclaim)
- **JIT**: Method-based compiler with OSR (on-stack replacement)
  - ✅ ARM64
  - 🔜 x86\_64, ARM32, RISC-V64, LoongArch64
- **AOT**: XIR → C source → gcc/clang → standalone executable
- **Platforms**: macOS (arm64, x86\_64), Linux (arm64, x86\_64, io\_uring), Windows (IOCP)

## Embedding

```c
#include "xray_embedding.h"

int main(void) {
    XrayVM *vm = xray_vm_new();
    xray_vm_dostring(vm, "print('Hello from C')");
    xray_vm_close(vm);
    return 0;
}
```

## License

MIT — see [LICENSE](LICENSE)

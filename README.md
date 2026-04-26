[**English**](README.md) | [中文](README_CN.md)

# Xray

> A statically-typed scripting language with native concurrency.
> **TypeScript syntax · Go-style concurrency · native binaries.**

```ts
import http

fn dashboard(req: Json): Json {
    let [users, orders] = await.all [
        go loadUsers(),
        go loadOrders()
    ]
    return { users, count: orders.length }
}

http.route("GET", "/dashboard", dashboard)
http.listen(8080)
```

A concurrent HTTP handler. `await.all` runs both `go` tasks in parallel,
and the compiler **statically guarantees** there are no data races.

---

## Why Xray?

You'll feel at home if you...

- **like Go's concurrency** but want `let`, `class`, `enum`, `match`, exceptions, and type inference;
- **like TypeScript's types** but want it to compile to a native binary, not to JavaScript;
- want concurrency that's **safe at compile time**, not patched up at runtime.

If it compiles, it's concurrency-safe. No locks. No race detectors.

---

## Install

```bash
git clone https://github.com/xray-lang/xray.git
cd xray
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
./build/xray --version
```

```bash
xray app.xr              # run a script
xray -e 'print("hi")'    # evaluate an expression
xray test                # run @test functions
xray repl                # interactive REPL
xray build app.xr        # compile to a standalone native binary
```

---

## A taste

### TypeScript-like, without `any`

```ts
type User = { name: string, age: int }

let r: int? = parse(input)
if (r is int) { print(r * 2) }      // narrowed

// Json is a first-class type, from fully dynamic to strictly typed:
let cfg: { host: string, port: int, ... } = loadConfig()
cfg.timeout = 30                     // extensible fields
```

Generics, union types, type narrowing, `?.`, `??`, destructuring, template strings —
it all looks like TypeScript, **but the `any` escape hatch is gone**.

### Go-like concurrency, statically safe

```ts
// Structured concurrency — scope waits for all children
scope {
    go loadUsers()
    go loadOrders()
}

// Channels — first-class, typed, buffered
const jobs = Channel<int>(10)
go fn() { for (i in 0..100) { jobs.send(i) } }()
for (job in jobs) { process(job) }

// select — multiplex channels with timeout
select {
    msg from ch1 => print(msg)
    after 1000  => print("timeout")
}

// What the compiler rejects:
let x = [1, 2, 3]
go fn() { x.push(4) }()              // ✗ cannot capture 'x' in go closure
shared let data = ...                // mutable shared requires explicit `move`
let ch = Channel(1)                   // ✗ Channel must be const
```

Three (and only three) ways to share data across coroutines:
`shared const` (zero-copy reads), `Channel` (deep-copy on send), and function parameters.
Anything else is a **compile error**.

### Three execution modes — one source

```
        your .xr source
              │
   ┌──────────┼──────────┐
   ▼          ▼          ▼
  VM         JIT        AOT
< 50 ms    hot paths   .xr → C → gcc/clang
 startup   on demand   single native binary
```

Develop with the VM (instant startup, REPL, scripts).
Hot paths are auto-compiled by the JIT (ARM64; x86_64 is in beta).
Ship as a **single-file native binary** via AOT (C transpile + link).

---

## Status — honest

We're at v0.5.x. We don't pretend it's 1.0.

| Component | State |
| --- | --- |
| VM, GC, scheduler, ARM64 JIT | ✅ Stable |
| AOT (single + multi-module, classes, exceptions, generics) | ✅ Stable |
| HTTP / HTTP2 / WebSocket / TLS / regex / json / crypto | ✅ Stable |
| LSP, VSCode extension | ✅ Stable |
| x86_64 JIT | 🚧 Beta |
| AOT coroutines / full stdlib coverage | 🚧 Beta |
| io_uring (Linux), IOCP (Windows), DAP, cluster | 🚧 Beta |
| ARM32 / RISC-V64 / LoongArch backends | 🗓️ Roadmap |
| Package registry | 🗓️ Roadmap |

Built in ~8,000 lines of C with 280+ regression tests passing on every commit.

---

## What's in the box

**Toolchain** — `xray run` · `test` · `fmt` · `build` · `check` · `repl` · `lsp` · `dap` · `init` · `pkg` · `eval` · `compile`.
One binary, zero runtime dependencies.

**Standard library** — `http` · `http2` · `ws` · `net` · `json` · `csv` · `toml` · `xml` · `yaml` · `crypto` · `base64` · `compress` · `regex` · `io` · `os` · `path` · `time` · `datetime` · `math` · `gc` · `log` · `cluster`.

**Platforms** — macOS, Linux, Windows · arm64, x86_64.

**Embeddable** — C API in `include/xray_embedding.h` for hosting Xray inside C/C++ apps.

---

## Learn more

- [`demos/`](demos/) — runnable examples organized by topic (basics → concurrency → networking)
- [`docs/rules/language-spec.md`](docs/rules/language-spec.md) — full language specification
- [`docs/design/`](docs/design/) — architecture deep dives (VM, JIT, AOT, GC, scheduler, cluster)

---

## License

MIT — see [LICENSE](LICENSE).

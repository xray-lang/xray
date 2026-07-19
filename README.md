[**English**](README.md) | [中文](README_CN.md)

# Xray

> A lightweight, statically typed language with explicit concurrency and both bytecode VM and native AOT execution.

```xray
fn count(xs: Array<int>) -> int {
    var total = 0
    for (x in xs) {
        total += x
    }
    return total
}

var first = [1, 2, 3]
var second = [4, 5, 6]
var left = go count(copy(first))
var right = go count(copy(second))
var leftTotal = await left
var rightTotal = await right
print(leftTotal + rightTotal)  // 21
```

Xray is designed for scripts, services, and command-line programs that benefit from static types, built-in concurrency, and a direct path from source code to standalone executables. The implementation is written in C and includes the compiler, VM, runtime, standard library, native AOT backend, and development tools.

## Highlights

- **Static typing** — type inference, nullable types (`T?`), unions, generics, tuples, records, and control-flow narrowing, without a universal `any` type.
- **Explicit concurrency** — `go`, `await`, `Channel<T>`, `select`, `scope`, and ownership-aware transfer rules are part of the language.
- **Language-level data modeling** — `class`, `struct`, `interface`, algebraic `enum`, pattern matching, collection literals, and modules.
- **Structured error handling** — typed enum errors use the value-return `throw` / `try` / `catch` channel; runtime faults use `PanicInfo`; `defer` provides deterministic cleanup.
- **Two execution paths** — scripts run on the bytecode VM, while `xray build --native` lowers Xi IR to C and invokes a host, Clang, or Zig C toolchain.
- **Integrated tooling** — formatter, test runner, REPL, bytecode compiler, LSP server, DAP adapter, MCP server, and AOT toolchain diagnostics ship in the repository.
- **C embedding API** — [`include/xray_vm.h`](include/xray_vm.h) exposes the VM API for host applications.

## Install

On macOS or Linux:

```bash
curl -fsSL https://xray-lang.org/install.sh | sh
xray --version
```

The installer selects the release archive for the current operating system and architecture and installs `xray` into `~/.xray/bin`. Windows users can download `xray-windows-x64.zip` from [GitHub Releases](https://github.com/xray-lang/xray/releases/latest). See the [installation guide](https://xray-lang.org/docs/getting-started/install) for PATH setup, upgrades, and source builds.

To build the repository directly, use a C11 compiler and CMake 3.12 or newer:

```bash
git clone https://github.com/xray-lang/xray.git
cd xray
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
./build/xray --version
```

## Command line

Use `xray` after installing a release, or `./build/xray` from a source build:

```bash
xray app.xr                             # run source on the bytecode VM
xray -e 'print("hello")'               # evaluate source text
xray check app.xr                       # parse and type-check
xray fmt app.xr                         # format in place
xray fmt --check app.xr                 # check formatting without writing
xray test                               # discover and run @test functions
xray repl                               # start the interactive REPL
xray compile app.xr                     # emit bytecode (.xrc by default)
xray build app.xr -o app                # standalone VM-backed executable
xray build --native app.xr -o app-native # native AOT executable
xray toolchain doctor                   # validate AOT toolchains
```

`xray run` and the shorthand `xray file.xr` execute bytecode in the VM. The default `xray build` packages bytecode with the runtime as a standalone executable. Native compilation is selected explicitly with `xray build --native`; the native pipeline is Xi IR → optimized C → the selected C toolchain. Xray does not use a JIT.

## Language at a glance

```xray
fn greet(name: string?) -> string {
    if (name == null) {
        return "hello, guest"
    }
    return "hello, ${name!}"
}

var scores = #{ "alice": 95, "bob": 88 }
var user = { "name": "alice", "score": scores["alice"] }
for (i in 1..=3) {
    print(i)
}
print(greet(user.name))
```

The range operator `a..b` excludes `b`; `a..=b` includes it. Collection literals use `[value]` for arrays, `#[value]` for sets, and `#{ key: value }` for maps. A record literal such as `{ "name": "alice", "score": 95 }` is checked against its structural type.

### Concurrency and data transfer

Heap-backed execution-local values do not cross a coroutine or channel boundary implicitly. Use `copy(value)` for an independent graph, `move value` to transfer an owned source, or `shared` for shared identity. Scalars, strings, channels, tasks, atomics, and already-shared identities can cross directly.

```xray
shared ch = Channel<int>(2)
go fn() {
    ch.send(42)
}()
select {
    value from ch -> {
        print("received ${value}")
    }
    after 1000    -> {
        print("timeout")
    }
}
ch.close()
```

In a `select`, `value from ch` receives, `value to ch` sends, `after milliseconds` adds a timeout, and `_ ->` adds an immediate default branch. A `scope` waits for the coroutines started inside it before the scope exits.

### Memory management

Xray does not use a concurrent tracing garbage collector. Ordinary execution-local reference values use compiler-inserted, per-coroutine reference counting; `shared` and system-owned values use atomic reference counting. Eligible coroutine-local reference cycles are reclaimed by a Bacon–Rajan trial-deletion cycle collector, which can run automatically or through `runtime.collectCycles()`. Resource cleanup is expressed explicitly with `defer`.

## Standard library

Importable standard-library modules include:

`base64`, `cluster`, `compress`, `crypto`, `csv`, `datetime`, `encoding`, `http`, `io`, `log`, `math`, `mem`, `net`, `os`, `parallel`, `path`, `regex`, `runtime`, `strconv`, `sync`, `sys`, `text`, `time`, `toml`, `url`, `ws`, `xml`, and `yaml`.

The prelude makes these types available without an explicit import:

`Array`, `Atomic`, `BigInt`, `Channel`, `Json`, `Map`, `NetConn`, `NetListener`, `OsBarrier`, `OsCondvar`, `OsMutex`, `OsOnce`, `OsRwLock`, `PanicInfo`, `Path`, `Range`, `Regex`, `Set`, `StringBuilder`, and `Thread`.

Module-owned types are imported explicitly. For example, `DateTime` comes from `datetime`, synchronization primitives such as `Mutex` come from `sync`, and CPU-parallel `Plan` comes from `parallel`.

## Project status

Xray is a pre-1.0 language under active development. The repository contains the working parser, analyzer, bytecode compiler and VM, runtime, standard library, native AOT pipeline, regression suites, and editor/debugging services. Language and library APIs may change before 1.0.

## Documentation

- [Language guide](https://xray-lang.org/docs/getting-started/quickstart) — installation, tutorials, and standard-library guides
- [`LANGUAGE_SPEC.md`](LANGUAGE_SPEC.md) — English language reference
- [`LANGUAGE_SPEC_CN.md`](LANGUAGE_SPEC_CN.md) — Chinese language reference
- [`demos/`](demos/) — runnable examples organized by topic
- [`tests/`](tests/) — VM, AOT, differential, regression, and unit tests

## License

MIT — see [LICENSE](LICENSE).

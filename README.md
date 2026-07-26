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

## Install and Try Xray

### 1. Install the Xray CLI

On macOS or Linux:

```bash
curl -fsSL https://xray-lang.org/install.sh | sh
xray --version
```

The installer selects the release archive for the current operating system and architecture and installs `xray` into `~/.xray/bin`. Windows users can download `xray-windows-x64.zip` from [GitHub Releases](https://github.com/xray-lang/xray/releases/latest). See the [installation guide](https://xray-lang.org/docs/getting-started/install) for PATH setup, upgrades, and detailed Windows steps.

### 2. Install the VS Code Extension

Install **Xray Language** for syntax highlighting, completion, diagnostics, formatting, navigation, rename, run, and debugging support:

```bash
code --install-extension xray-lang.xray-lang
```

You can also search for `Xray Language` in the Extensions view or install it from the [Visual Studio Marketplace](https://marketplace.visualstudio.com/items?itemName=xray-lang.xray-lang) / [Open VSX Registry](https://open-vsx.org/extension/xray-lang/xray-lang). Each platform package bundles the `xray` binary used by its LSP and DAP; the separately installed CLI is still used for terminal commands, builds, tests, and MCP.

### 3. Connect an AI Coding Client through Xray MCP

The Xray CLI includes a stdio MCP server. For VS Code, create `.vscode/mcp.json` in your project:

```json
{
  "servers": {
    "xray": {
      "type": "stdio",
      "command": "xray",
      "args": ["mcp-server"]
    }
  }
}
```

Run **MCP: List Servers** from the Command Palette and start `xray`. The AI client can then query real Xray syntax and standard-library APIs and check generated code with the analyzer and formatter. Code execution is disabled by default; add `--enable-runner` only when you explicitly want to run restricted snippets. See the [MCP and AI-assisted development guide](https://xray-lang.org/docs/getting-started/mcp) for other client formats, the capability list, and troubleshooting.

### Build from Source

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

Native library projects declare their C ABI with `[[export.c]]` entries in
`xray.toml`. `--c-export-prefix PREFIX` prefixes every public manifest export
while leaving hidden support symbols unchanged, and
`--c-export-exclude A,B,...` removes selected manifest exports from the AOT
root set. Both options validate identifiers and symbol names fail-closed and
participate in the native-plan fingerprint, so package systems can implement
namespaced or feature-sliced libraries without source rewriting. `--c-only`
emits one compilable amalgamated translation unit even for multi-module
programs.

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

Quoted literals use one quote for single-line inline text, two quotes for an empty payload, and three or more quotes for blocks whose opener is followed immediately by a newline and whose exact-count closer occupies its own line. Use no prefix / `r` for strings, `b/br` for fixed bytes, and `c/cr` for fixed C bytes with an appended NUL; `r/br/cr` preserve backslashes.

### Concurrency and data transfer

Heap-backed execution-local values do not cross a coroutine or channel boundary implicitly. Use `copy(value)` for an independent graph or `move value` to transfer an inferred-unique root. Inline values, published `const` data, and audited synchronization handles such as channels, tasks, and atomics can cross directly.

```xray
const ch = Channel<int>(2)
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

Xray does not use a concurrent tracing garbage collector. Ordinary execution-local reference values use compiler-inserted, per-coroutine reference counting; published const roots and audited synchronization/runtime handles use their verified shared storage plan. Eligible coroutine-local reference cycles are reclaimed by a Bacon–Rajan trial-deletion cycle collector, which can run automatically or through `runtime.collectCycles()`. Resource cleanup is expressed explicitly with `defer`.

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
- [MCP and AI-assisted development](https://xray-lang.org/docs/getting-started/mcp) — connect AI clients to Xray syntax, APIs, analysis, and formatting
- [`LANGUAGE_SPEC.md`](LANGUAGE_SPEC.md) — English language reference
- [`LANGUAGE_SPEC_CN.md`](LANGUAGE_SPEC_CN.md) — Chinese language reference
- [`demos/`](demos/) — runnable examples organized by topic
- [`tests/`](tests/) — VM, AOT, differential, regression, and unit tests

## License

MIT — see [LICENSE](LICENSE).

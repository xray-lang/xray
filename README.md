[**English**](README.md) | [中文](README_CN.md)

# Xray

> A statically typed scripting language with native concurrency.
> TypeScript-like syntax, Go-style coroutines, VM/AOT execution.

```xray
fn count(xs: Array<int>) -> int {
    let total = 0
    for (x in xs) { total += x }
    return total
}

let left = go count([1, 2, 3])
let right = go count([4, 5, 6])

let a = await left
let b = await right
print(a + b)
```

Xray is designed for scripts and services that need static types, fast startup, safe concurrency, and a path to native binaries.

---

## Highlights

- **Static typing without `any`** — inference, nullable types (`T?`), unions, generics, tuples, sealed object types, and type narrowing.
- **Native concurrency** — `go`, `await`, `Channel<T>`, `select`, and structured `scope` are built into the language.
- **Modern language features** — `class`, `struct`, `interface`, ADT-style `enum`, `match`, exceptions, `Result`, modules, and `defer`.
- **VM + AOT execution** — use the VM for development/debugging and AOT for high-performance native binaries.
- **C implementation and VM API** — native stdlib modules and `include/xray_vm.h` for host applications.

---

## Build

```bash
git clone https://github.com/xray-lang/xray.git
cd xray
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
./build/xray --version
```

## Use

```bash
./build/xray app.xr              # run a script
./build/xray -e 'print("hi")'    # evaluate code
./build/xray test                # run @test functions
./build/xray repl                # interactive REPL
./build/xray build app.xr        # build a native binary
```

---

## Language at a glance

```xray
type User = { name: string, age: int }

fn greet(user: User?) -> string {
    match (user) {
        null -> "hello, guest"
        u -> "hello, ${u.name}"
    }
}

let scores = #{"alice": 10, "bob": 8}
let names = #["alice", "bob"]
```

Concurrency is explicit. Ordinary local variables cannot be accidentally shared across `go` coroutines; use parameters, `shared const`, or `Channel<T>`.

```xray
shared const ch = new Channel<int>(2)

go fn() { ch.send(42) }()

select {
    value from ch -> { print(value) }
    after 1000 -> { print("timeout") }
    _ -> { print("no value") }
}
```

---

## Standard library

Native modules include:

`base64`, `cluster`, `compress`, `crypto`, `csv`, `datetime`, `encoding`, `gc`, `http`, `io`, `log`, `math`, `net`, `os`, `path`, `regex`, `time`, `toml`, `url`, `ws`, `xml`, `yaml`.

`Json`, `Array`, `Map`, `Set`, `Channel`, `DateTime`, `Regex`, `StringBuilder`, `Exception`, and related built-in types are available from the prelude.

---

## Status

Xray is pre-1.0 and under active development. The VM, parser/analyzer, runtime, standard library, tests, LSP/DAP/MCP tooling, and AOT pipeline are all in the repository, but language and API details may still change.

Use the language specs as the current source of truth:

- [`LANGUAGE_SPEC.md`](LANGUAGE_SPEC.md) — English language reference
- [`LANGUAGE_SPEC_CN.md`](LANGUAGE_SPEC_CN.md) — Chinese language reference

---

## More

- [`demos/`](demos/) — runnable examples
- [`tests/`](tests/) — regression and unit tests

## License

MIT — see [LICENSE](LICENSE).

[English](README.md) | [**中文**](README_CN.md)

# Xray

> 静态类型脚本语言，原生并发。
> TypeScript 风格语法，Go 风格协程，支持 VM / JIT / AOT 执行。

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

Xray 面向需要静态类型、快速启动、安全并发，并且未来可编译为原生二进制的脚本和服务场景。

---

## 特性

- **静态类型，没有 `any`**：类型推断、可空类型 (`T?`)、Union、泛型、Tuple、sealed object、类型收窄。
- **原生并发**：`go`、`await`、`Channel<T>`、`select`、结构化 `scope` 是语言内置能力。
- **现代语言结构**：`class`、`struct`、`interface`、ADT 风格 `enum`、`match`、异常、`Result`、模块、`defer`。
- **多执行模式**：VM 直接运行，JIT 加速热点，AOT 构建原生二进制。
- **C 实现与嵌入 API**：stdlib native 模块，以及 `include/xray_embedding.h` 宿主嵌入接口。

---

## 构建

```bash
git clone https://github.com/xray-lang/xray.git
cd xray
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
./build/xray --version
```

## 使用

```bash
./build/xray app.xr              # 运行脚本
./build/xray -e 'print("hi")'    # 执行代码
./build/xray test                # 运行 @test 函数
./build/xray repl                # 交互式 REPL
./build/xray build app.xr        # 构建原生二进制
```

---

## 语言一瞥

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

并发共享是显式的。普通局部变量不会被 `go` 协程意外共享；需要通过参数、`shared const` 或 `Channel<T>` 传递。

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

## 标准库

Native 模块包括：

`base64`、`cluster`、`compress`、`crypto`、`csv`、`datetime`、`encoding`、`gc`、`http`、`io`、`log`、`math`、`net`、`os`、`path`、`regex`、`time`、`toml`、`url`、`ws`、`xml`、`yaml`。

`Json`、`Array`、`Map`、`Set`、`Channel`、`DateTime`、`Regex`、`StringBuilder`、`Exception` 等内置类型由 prelude 提供，通常无需显式 import。

---

## 状态

Xray 仍处于 pre-1.0 阶段，正在快速迭代。仓库中已包含 VM、parser/analyzer、runtime、标准库、测试、LSP/DAP/MCP 工具、JIT 与 AOT 管线，但语言和 API 仍可能调整。

当前语言规范以以下文件为准：

- [`LANGUAGE_SPEC_CN.md`](LANGUAGE_SPEC_CN.md) —— 中文语言参考
- [`LANGUAGE_SPEC.md`](LANGUAGE_SPEC.md) —— English language reference

---

## 更多

- [`demos/`](demos/) —— 可运行示例
- [`tests/`](tests/) —— 回归测试与单元测试

## License

MIT —— 见 [LICENSE](LICENSE)。

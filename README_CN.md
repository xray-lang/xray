[English](README.md) | [**中文**](README_CN.md)

# Xray

> 静态类型脚本语言，原生并发，编译期安全。
> **TypeScript 的语法 · Go 的并发 · 原生的性能。**

```ts
import http

fn dashboard(req: Json): Json {
    let [users, orders] = await all [
        go loadUsers(),
        go loadOrders()
    ]
    return { users, count: orders.length }
}

http.route("GET", "/dashboard", dashboard)
http.listen(8080)
```

一段并发的 HTTP handler。`await all` 同时跑两个 `go`，
**编译器静态保证**没有数据竞争。

---

## 为什么是 Xray？

如果你符合下面任一条件，Xray 就是为你设计的：

- 喜欢 **Go 的并发**，但想要 `let` / `class` / `enum` / `match` / 异常 / 类型推断；
- 喜欢 **TypeScript 的类型**，但希望它能编译为原生二进制，而不是 JavaScript；
- 想要**编译期就保证**的并发安全，而不是运行时再来打补丁。

如果它编译通过，它就是并发安全的。不需要锁，不需要 race detector。

---

## 安装

```bash
git clone https://github.com/xray-lang/xray.git
cd xray
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
./build/xray --version
```

```bash
xray app.xr              # 运行脚本
xray -e 'print("hi")'    # 求值表达式
xray test                # 运行 @test 函数
xray repl                # 交互式 REPL
xray build app.xr        # 编译为独立原生二进制
```

---

## 语言一瞥

### 像 TypeScript，但没有 `any`

```ts
type User = { name: string, age: int }

let r: int? = parse(input)
if (r is int) { print(r * 2) }      // 类型收窄

// Json 是 first-class 类型，从全动态到严格类型自由切换：
let cfg: { host: string, port: int, ... } = loadConfig()
cfg.timeout = 30                     // 可扩展字段
```

泛型 / 联合类型 / 类型收窄 / `?.` / `??` / 解构 / 模板字符串 —— 都在。
**但 `any` 这扇后门被关上了。**

### 像 Go 的并发，编译期就安全

```ts
// 结构化并发 —— scope 自动等待所有子协程
scope {
    go loadUsers()
    go loadOrders()
}

// Channel —— 类型化、缓冲、first-class
const jobs = Channel<int>(10)
go fn() { for (i in 0..100) { jobs.send(i) } }()
for (job in jobs) { process(job) }

// select —— 多路复用 + 超时
select {
    msg from ch1 => print(msg)
    after 1000  => print("timeout")
}

// 编译器拒绝的代码：
let x = [1, 2, 3]
go fn() { x.push(4) }()              // ✗ cannot capture 'x' in go closure
shared let data = ...                // 可变共享必须显式 move
let ch = Channel(1)                   // ✗ Channel 必须是 const
```

跨协程共享数据**有且只有三种方式**：
`shared const`（零拷贝读）/ `Channel`（send 时深拷贝）/ 函数参数（深拷贝传递）。
其它写法一律 **编译错误**。

### 三种执行模式 · 同一份源码

```
       你的 .xr 源码
             │
   ┌─────────┼─────────┐
   ▼         ▼         ▼
  VM        JIT       AOT
启动 < 50ms 热路径加速  .xr → C → gcc/clang
零编译延迟  按需触发    单文件原生二进制
```

开发期用 VM（即时启动、REPL、脚本）。
热路径自动 JIT 加速（ARM64 已稳定，x86_64 进行中）。
发布时 AOT 编译为**单文件原生二进制**（经 C transpile 链 gcc/clang）。

---

## 成熟度 —— 诚实声明

当前版本 v0.5.x。我们不假装已经是 1.0。

| 组件 | 状态 |
| --- | --- |
| VM、GC、调度器、ARM64 JIT | ✅ Stable |
| AOT（单 / 多模块、类、异常、泛型） | ✅ Stable |
| HTTP / HTTP2 / WebSocket / TLS / regex / crypto | ✅ Stable |
| LSP、VSCode 插件 | ✅ Stable |
| x86_64 JIT | 🚧 Beta |
| AOT 协程 / stdlib 全覆盖 | 🚧 Beta |
| io_uring (Linux)、IOCP (Windows)、DAP、cluster | 🚧 Beta |
| ARM32 / RISC-V64 / LoongArch 后端 | 🗓️ Roadmap |
| 包注册中心 | 🗓️ Roadmap |

约 8,000 行 C 实现，每次提交跑 280+ 项回归测试。

---

## 内置工具

**工具链** —— `xray run` · `test` · `fmt` · `build` · `check` · `repl` · `lsp` · `dap` · `init` · `pkg` · `eval` · `compile`。
一个 binary，零运行时依赖。

**标准库** —— `http` · `ws` · `net` · `csv` · `toml` · `xml` · `yaml` · `crypto` · `base64` · `compress` · `regex` · `io` · `os` · `path` · `time` · `datetime` · `math` · `gc` · `log` · `cluster` · `url` · `encoding`。（`Json` 是一等公民类型（prelude），无需 `import`。）

**平台** —— macOS · Linux · Windows × arm64 / x86_64。

**可嵌入** —— `include/xray_embedding.h` 提供 C API，可嵌入 C/C++ 宿主应用。

---

## 深入了解

- [`demos/`](demos/) —— 按主题组织的可运行示例（basics → concurrency → networking）
- [`docs/rules/language-spec.md`](docs/rules/language-spec.md) —— 完整语法规范
- [`docs/design/`](docs/design/) —— 架构深入（VM / JIT / AOT / GC / 调度器 / cluster）

---

## License

MIT —— 见 [LICENSE](LICENSE)。

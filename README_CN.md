[English](README.md) | [**中文**](README_CN.md)

# Xray

静态类型脚本语言，内建原生并发，用 C 实现。

```xray
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

## 特性

- **静态类型 + 推断** — 泛型、联合类型、类型收窄、可选类型
- **原生并发** — `go` / `await` / `Channel` / `scope`，M:N 工作窃取调度器
- **结构化并发** — `linked scope`（错误传播）/ `supervisor scope`（错误隔离）
- **ARM64 JIT 编译器** — 基于方法的编译，支持栈上替换（OSR）
- **AOT 编译** — 通过 C 转译生成独立原生二进制
- **每协程 GC** — Immix 标记-区域回收，无全局 Stop-the-World
- **一等 JSON** — 动态、严格类型、可扩展、部分不可变 — 统一值类型
- **内建 HTTP 服务器** — 每连接一协程，自动 JSON 序列化
- **内建集群** — 命名通道、RPC、发布/订阅、局域网自动发现 — 无需外部依赖
- **完整工具链** — `run`、`test`、`fmt`、`build`、`check`、`repl`、`lsp`、`dap`
- **跨平台** — macOS、Linux（io_uring）、Windows（IOCP）；arm64 + x86_64
- **可嵌入** — C API，支持嵌入宿主应用

## 安装

```bash
git clone https://github.com/xray-lang/xray.git
cd xray
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
./build/xray --version   # xray v0.5.0 (JIT, arm64-darwin)
```

```bash
xray app.xr              # 运行脚本
xray -e 'print("hi")'    # 执行表达式
xray test                 # 运行 @test 函数
xray repl                 # 交互式 REPL
```

## 语言

### 类型系统

```xray
// 推断 + 显式标注
let name = "Alice"
const PI: float = 3.14159

// 泛型
class Stack<T> {
    items: Array<T>
    constructor() { this.items = [] }

    push(item: T): void { this.items.push(item) }
    pop(): T? { return this.items.pop() }
}

let s = new Stack<int>()
s.push(42)

// 联合类型 + 类型收窄
type Result = int | string | null

fn parse(input: string): Result {
    if (input == "42") { return 42 }
    if (input == "") { return null }
    return input
}

let r = parse("42")
if (r is int) { print("got int:", r) }
```

### 并发 — go / await / Channel / scope

```xray
// 启动协程并等待结果
let task = go fn(): int { return 42 }
print(await task)   // 42

// 结构化并发：scope 等待所有子协程完成
shared const results: Array<string> = []

fn work(id: int, out: Array<string>): void {
    out.push("task " + id.toString())
}

scope {
    go work(1, results)
    go work(2, results)
    go work(3, results)
}
// 所有任务在此处已完成
print(results.length)   // 3

// 基于 Channel 的生产者/消费者模式
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

### 结构体 — 值类型与运算符重载

```xray
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

### 类、接口、枚举

```xray
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

### 内建 Json — 从动态到强类型

Json 是一等值类型。结合 `type` 定义，支持从完全动态到严格类型的全谱覆盖 —— 无需在"定义完整类"和"放弃类型检查"之间取舍。

```xray
// 完全动态 —— 随时添加任意字段
let obj = { name: "Alice", age: 30 }
obj.email = "alice@test.com"
obj["score"] = 100

// 严格类型 —— 只允许声明过的字段
type Point = { x: int, y: int }
let p: Point = { x: 10, y: 20 }

// 可扩展类型 —— 核心字段有类型约束，其余字段自由扩展
type Config = { host: string, port: int, ... }
let cfg: Config = { host: "localhost", port: 8080 }
cfg.debug = true          // 允许
cfg.timeout = 30          // 允许

// 部分不可变 —— 用 const 锁定特定字段
type Entity = { const id: int, name: string, ... }
let e: Entity = { id: 1, name: "test" }
e.name = "updated"        // ok
e.extra = "allowed"       // ok
// e.id = 2               // 编译错误：const 字段

// 可选字段
type User = { name: string, email?: string }
let u: User = { name: "Alice" }

// 计算属性键
let prefix = "user"
let record = { [prefix + "_id"]: 1001, [prefix + "_name"]: "Alice" }

// 遍历键值对
for (k, v in obj) {
    print(k, "=", v)      // name = Alice, age = 30, ...
}
```

### 函数式

```xray
let nums = [3, 1, 4, 1, 5, 9, 2, 6]

let result = nums
    .filter((x: int) => x > 3)
    .map((x: int) => x * x)
    .reduce((a: int, b: int) => a + b, 0)

print(result)   // 158
```

### 错误处理

```xray
try {
    let data = json.parse(raw)
    process(data)
} catch (e) {
    print("error:", e)
} finally {
    cleanup()
}

// defer 在函数退出时执行（LIFO 顺序）
fn readFile(path: string): string {
    let f = io.open(path, "r")
    defer f.close()
    return f.readAll()
}
```

## HTTP 服务器

每个连接运行在独立协程中 —— 没有回调，没有 async/await 仪式。
处理函数的返回值自动转换：字符串变文本响应，Json 对象自动序列化并设置 `Content-Type: application/json`。

```xray
import http

fn hello(req) {
    return "Hello, Xray!"
}

fn getUser(req) {
    let id = req.params.id
    return { id: id, name: "Alice" }    // 自动 JSON 响应
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

## 工具链

| 命令 | 说明 |
| ---- | ---- |
| `xray run` | 运行脚本或项目 |
| `xray test` | 运行 `@test` 函数 |
| `xray fmt` | 格式化源代码 |
| `xray build` | 编译为独立二进制（AOT → C → gcc/clang） |
| `xray compile` | 编译为字节码 |
| `xray check` | 静态分析 |
| `xray repl` | 交互式 REPL |
| `xray lsp` | Language Server Protocol 服务器 |
| `xray dap` | Debug Adapter Protocol 服务器 |
| `xray init` | 创建新项目 |
| `xray add/remove` | 管理依赖 |

## 标准库

| 分类 | 模块 |
| ---- | ---- |
| **网络** | `http`（HTTP/1.1 客户端/服务器）、`http2`、`ws`（WebSocket）、`net`（TCP/UDP/DNS/TLS） |
| **数据** | `json`、`csv`、`toml`、`xml`、`yaml` |
| **加密** | `crypto`（SHA-256/512、AES-256、HMAC、UUID） |
| **编码** | `base64`、`encoding`（hex）、`url`、`compress`（deflate/gzip/zlib） |
| **系统** | `io`（文件）、`os`（环境变量/进程/信号）、`path`、`datetime` |
| **运行时** | `math`、`time`、`log`、`gc`、`regex` |
| **分布式** | `cluster`（命名通道、发布/订阅、服务网格 — beta） |


## 分布式 — 内建集群

无需外部依赖。命名通道、RPC、发布/订阅、局域网自动发现 —— 全部用 C 实现。

```xray
import cluster

// 启动节点
cluster.start({ name: "node-A", port: 9001, secret: "my-secret" })

// 加入已有集群
cluster.join("192.168.1.10:9001")

// 命名通道 —— 首次创建者为 Owner，其他节点获得透明代理
shared const jobs = cluster.channel("jobs", 100)
jobs.send({ task: "render", frame: 42 })      // 跨节点透明工作
let job = jobs.recv()

// RPC 服务
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

// 发布/订阅（支持 NATS 风格通配符）
shared const orders = cluster.subscribe("orders.>")    // 通配符
cluster.publish("orders.new", { id: 1, total: 99.9 })

// 节点监控
shared const mon = cluster.monitor("*")     // 所有节点
// mon 在节点离线时接收节点名

// 局域网自动发现（UDP 多播）
cluster.discover()
print(cluster.nodes())    // ["node-A", "node-B", ...]

cluster.stop()
```

**架构说明**：Owner 模型命名通道（单一 Owner，无需共识开销）、
PHI Accrual 故障检测器、SHA-256 挑战-响应认证、每节点独立写协程。


## 高级特性

### select — 通道多路复用

```xray
shared const ch1 = Channel(2)
shared const ch2 = Channel(2)

ch1.trySend(100)

select {
    msg from ch1 => { print(msg) }       // 100
    msg from ch2 => { print(msg) }
    _ => { print("no data") }
}
```

### 协程优先级

```xray
fn task(label: string): string { return label }

// 内联优先级（推荐）
let t1 = go(priority: Coro.HIGH) task("high")
let t2 = go(priority: Coro.LOW) task("low")

// 或者创建后设置
let t3 = go task("normal")
Coro.setPriority(t3, Coro.HIGH)

await t1
await t2
await t3
```

### linked / supervisor scope — 错误传播

```xray
// linked scope：子协程错误传播到父级
try {
    linked scope {
        go ok_worker()
        go failing_worker()   // 抛出异常 → 向上传播
    }
} catch (e) {
    print("caught:", e)
}

// supervisor scope：子协程错误不影响兄弟协程
supervisor scope {
    go failing_worker()       // 抛出异常 → 被隔离
    go ok_worker()            // 继续运行
}
```

### 协程命名

```xray
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

### await.all — 等待多个任务

```xray
fn task1(): int { return 10 }
fn task2(): int { return 20 }

let t1 = go task1()
let t2 = go task2()

let results = await.all [t1, t2]
print(results[0], results[1])   // 10 20

// 也支持解构
let [x, y] = await.all [go task1(), go task2()]
print(x, y)   // 10 20
```

### cancelled() — 协作式取消

```xray
fn worker(): string {
    for (i in 0..1000) {
        if (cancelled()) {
            return "stopped at " + i.toString()
        }
        // 执行工作...
    }
    return "done"
}

let task = go worker()
task.cancel()
let result = await task   // null（在检查前已取消）
```

### shared / move — 所有权与跨协程安全

```xray
// shared const：零拷贝并发读取（引用计数）
shared const config = { name: "app", port: 8080 }

// move：显式将所有权转移给协程或通道
shared let data = { count: 10 }
let task = go fn(d: Json): int {
    return d.count + 1
}(move data)
// data 不再可用（move 后引用会触发编译错误）

// 普通变量：深拷贝到协程（无需 move）
let arr = [1, 2, 3]
let t = go fn(d: Array<int>) { d.push(4); return d.length }(arr)
print(arr)             // [1, 2, 3]（未被修改）
```

## 架构

```
源代码 → 词法分析 → 语法分析 → 静态分析 → 字节码 → 虚拟机
                                                ↓
                                         ARM64 JIT（热路径）
                                                ↓
                                    AOT（C 转译 → 原生二进制）
```

- **调度器**：M:N 工作窃取（Go 风格 P/M 分离，无锁窃取队列）
- **GC**：每协程 Immix 标记-区域回收（bump-pointer 分配，行粒度回收）
- **JIT**：基于方法的编译器，支持 OSR（栈上替换）
  - ✅ ARM64
  - 🔜 x86\_64、ARM32、RISC-V64、LoongArch64
- **AOT**：XIR → C 源码 → gcc/clang → 独立可执行文件
- **平台**：macOS（arm64、x86\_64）、Linux（arm64、x86\_64、io\_uring）、Windows（IOCP）

## 嵌入

```c
#include "xray_embedding.h"

int main(void) {
    XrayVM *vm = xray_vm_new();
    xray_vm_dostring(vm, "print('Hello from C')");
    xray_vm_close(vm);
    return 0;
}
```

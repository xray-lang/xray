[English](README.md) | [**中文**](README_CN.md)

# Xray

> 一门轻量级静态类型语言，提供显式并发、字节码 VM 与原生 AOT 两种执行路径。

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

Xray 面向脚本、服务和命令行程序，适合需要静态类型、语言内置并发，以及从源码直接构建独立可执行文件的场景。实现使用 C 编写，仓库包含编译器、VM、运行时、标准库、原生 AOT 后端和开发工具。

## 主要特性

- **静态类型**：支持类型推断、可空类型（`T?`）、Union、泛型、Tuple、Record 和控制流类型收窄，不提供通用 `any` 类型。
- **显式并发**：`go`、`await`、`Channel<T>`、`select`、`scope` 和所有权感知的跨边界传递规则都是语言组成部分。
- **语言级数据建模**：支持 `class`、`struct`、`interface`、代数数据类型风格的 `enum`、模式匹配、集合字面量和模块。
- **结构化错误处理**：类型化 enum 错误通过值返回 `throw` / `try` / `catch` 通道传播；运行时故障使用 `PanicInfo`；`defer` 用于确定性清理。
- **两种执行路径**：脚本在字节码 VM 上运行；`xray build --native` 将 Xi IR 降低为 C，再调用 host、Clang 或 Zig C 工具链。
- **集成开发工具**：仓库提供格式化器、测试运行器、REPL、字节码编译器、LSP 服务、DAP 适配器、MCP 服务和 AOT 工具链诊断。
- **C 嵌入 API**：[`include/xray_vm.h`](include/xray_vm.h) 提供宿主应用使用的 VM 接口。

## 安装与快速体验

### 1. 安装 Xray 命令行

macOS 或 Linux：

```bash
curl -fsSL https://xray-lang.org/install.sh | sh
xray --version
```

安装脚本会根据当前操作系统和处理器架构选择发行包，并把 `xray` 安装到 `~/.xray/bin`。Windows 用户可以从 [GitHub Releases](https://github.com/xray-lang/xray/releases/latest) 下载 `xray-windows-x64.zip`。PATH 配置、升级和 Windows 详细步骤见[安装教程](https://xray-lang.org/docs/getting-started/install)。

### 2. 安装 VS Code 扩展

推荐安装 **Xray Language**，获得语法高亮、补全、诊断、格式化、跳转、重命名、运行和调试支持：

```bash
code --install-extension xray-lang.xray-lang
```

也可以在扩展面板中搜索 `Xray Language`，或从 [Visual Studio Marketplace](https://marketplace.visualstudio.com/items?itemName=xray-lang.xray-lang) / [Open VSX Registry](https://open-vsx.org/extension/xray-lang/xray-lang) 安装。各平台扩展包已经内置供 LSP 和 DAP 使用的 `xray`；独立安装的命令行仍用于终端运行、构建、测试和 MCP。

### 3. 让 AI 编程客户端连接 Xray MCP

Xray 命令行内置 stdio MCP 服务。在 VS Code 项目中创建 `.vscode/mcp.json`：

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

在命令面板中运行 **MCP: List Servers** 并启动 `xray`。AI 客户端随后可以查询真实的 Xray 语法和标准库，并使用 analyzer 与 formatter 检查生成代码。默认不开放代码执行；需要运行受限短代码片段时才显式加入 `--enable-runner`。其它客户端格式、能力清单和排错方法见 [MCP 与 AI 编程指南](https://xray-lang.org/docs/getting-started/mcp)。

### 从源码构建

直接构建仓库需要 C11 编译器和 CMake 3.12 或更高版本：

```bash
git clone https://github.com/xray-lang/xray.git
cd xray
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
./build/xray --version
```

## 命令行

安装发行版后使用 `xray`；从源码构建时也可以使用 `./build/xray`：

```bash
xray app.xr                             # 在字节码 VM 上运行源码
xray -e 'print("hello")'               # 执行一段源码
xray check app.xr                       # 解析并进行类型检查
xray fmt app.xr                         # 原地格式化
xray fmt --check app.xr                 # 仅检查格式，不写入文件
xray test                               # 发现并运行 @test 函数
xray repl                               # 启动交互式 REPL
xray compile app.xr                     # 生成字节码（默认扩展名 .xrc）
xray build app.xr -o app                # 构建由 VM 执行字节码的独立程序
xray build --native app.xr -o app-native # 构建原生 AOT 程序
xray toolchain doctor                   # 检查 AOT 工具链
```

`xray run` 和简写形式 `xray file.xr` 都会在 VM 中执行字节码。默认 `xray build` 将字节码与运行时打包为独立可执行文件；只有显式使用 `xray build --native` 才会选择原生编译，管线为 Xi IR → 优化后的 C → 所选 C 工具链。Xray 不使用 JIT。

## 语言一瞥

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

区间运算符 `a..b` 不包含右端点 `b`，`a..=b` 包含右端点。数组、Set 和 Map 字面量分别写作 `[value]`、`#[value]` 和 `#{ key: value }`。`{ "name": "alice", "score": 95 }` 这样的 Record 字面量会按照结构类型进行检查。

### 并发与数据传递

由堆对象组成的 execution-local 值不会隐式跨越协程或 Channel 边界。需要独立引用图时使用 `copy(value)`，需要转移 owned 源时使用 `move value`，需要共享同一身份时声明 `shared`。标量、字符串、Channel、Task、Atomic 和已经共享的身份可以直接跨越边界。

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

在 `select` 中，`value from ch` 表示接收，`value to ch` 表示发送，`after milliseconds` 表示超时，`_ ->` 表示立即执行的默认分支。`scope` 会在退出作用域前等待其中启动的协程结束。

### 内存管理

Xray 不使用并发 tracing GC。普通 execution-local 引用值使用编译器插入的、按协程归属的引用计数；`shared` 和 system-owned 值使用原子引用计数。符合条件的协程局部强引用环由 Bacon–Rajan trial-deletion cycle collector 回收，它既可自动触发，也可通过 `runtime.collectCycles()` 显式触发。资源清理由 `defer` 显式表达。

## 标准库

可导入的标准库模块包括：

`base64`、`cluster`、`compress`、`crypto`、`csv`、`datetime`、`encoding`、`http`、`io`、`log`、`math`、`mem`、`net`、`os`、`parallel`、`path`、`regex`、`runtime`、`strconv`、`sync`、`sys`、`text`、`time`、`toml`、`url`、`ws`、`xml`、`yaml`。

Prelude 提供以下无需显式导入的类型：

`Array`、`Atomic`、`BigInt`、`Channel`、`Json`、`Map`、`NetConn`、`NetListener`、`OsBarrier`、`OsCondvar`、`OsMutex`、`OsOnce`、`OsRwLock`、`PanicInfo`、`Path`、`Range`、`Regex`、`Set`、`StringBuilder`、`Thread`。

由模块导出的类型需要显式导入。例如，`DateTime` 来自 `datetime`，`Mutex` 等协程同步原语来自 `sync`，用于 CPU 并行的 `Plan` 来自 `parallel`。

## 项目状态

Xray 仍处于 1.0 之前的活跃开发阶段。仓库包含可工作的 parser、analyzer、字节码编译器和 VM、运行时、标准库、原生 AOT 管线、回归测试，以及编辑器和调试服务。语言与标准库 API 在 1.0 前仍可能调整。

## 文档

- [语言教程](https://xray-lang.org/docs/getting-started/quickstart) —— 安装、入门教程和标准库指南
- [MCP 与 AI 编程](https://xray-lang.org/docs/getting-started/mcp) —— 为 AI 客户端接入 Xray 语法、API、分析和格式化能力
- [`LANGUAGE_SPEC_CN.md`](LANGUAGE_SPEC_CN.md) —— 中文语言参考
- [`LANGUAGE_SPEC.md`](LANGUAGE_SPEC.md) —— English language reference
- [`demos/`](demos/) —— 按主题组织的可运行示例
- [`tests/`](tests/) —— VM、AOT、差分、回归和单元测试

## 许可证

MIT —— 见 [LICENSE](LICENSE)。

# HTTP 服务端性能对比测试

对比 xray、Go、Node.js、Python 作为 HTTP 服务端的性能表现。

## 测试项目

| 测试 | 说明 | 关键指标 |
|------|------|----------|
| **延迟** | 单连接 keep-alive 往返延迟（plaintext、JSON） | min / avg / p95 / p99 (ms) |
| **吞吐量** | 单连接 keep-alive 最大吞吐 | req/s, MB/s |
| **并发** | 多连接聚合吞吐，1 ~ 100 连接 | 总 req/s |
| **POST Echo** | POST 回显吞吐，64B ~ 16KB body | req/s, MB/s |
| **连接抖动** | 每请求新建连接（无 keep-alive） | conns/s |
| **持续管线** | 多连接持续 keep-alive 吞吐 | 总 req/s |

## 测试端点

| 端点 | 方法 | 说明 |
|------|------|------|
| `/plaintext` | GET | 返回 "Hello, World!"（13 字节静态文本） |
| `/json` | GET | 返回 JSON `{"message":"Hello, World!"}` |
| `/echo` | POST | 回显请求 body |

## 对比对象

| 语言 | 库 | 文件 |
|------|------|------|
| **xray** | stdlib/http (C) | `http_server.xr` |
| **Go** | net/http (标准库) | `http_server.go` |
| **Go** | fasthttp | `http_server_fasthttp.go` |
| **Node.js** | http (标准库) | `http_server.js` |
| **Python** | aiohttp | `http_server.py` |

所有服务端都是最简实现，无额外中间件开销，保证可比性。

> **多核说明**：Go 默认使用所有 CPU 核心（goroutine-per-connection），Node.js 和 xray 为单线程事件循环 + 协程，Python aiohttp 为单线程异步。这是各语言**默认/标准**用法的对比，不做 cluster 或多进程配置。

## 环境要求

- **必须**：Python 3 + `aiohttp`（`pip3 install aiohttp`）
- **必须**：xray 可执行文件
- **可选**：Go（自动编译 net/http / fasthttp）、Node.js

## 跨平台一键对比

`run_compare.py` 不依赖 `wrk`，Windows / Linux / macOS 都可运行。它会依次启动各语言服务端，用同一个 `http_bench.py` 客户端生成 JSON 结果，并输出并排对比表。

```bash
# 快速对比所有可用服务端
python tests/benchmarks/http/run_compare.py --quick

# Windows 上显式指定 xray.exe
python tests/benchmarks/http/run_compare.py --quick --xray-bin build-http-debug/xray.exe

# 只对比部分服务端
python tests/benchmarks/http/run_compare.py --quick --only xray,node,python

# 只汇总已有结果
python tests/benchmarks/http/run_compare.py --compare-only
```

结果写入 `tests/benchmarks/http/results/compare/`。

## wrk 极限压测

```bash
# 完整测试（约 5-8 分钟）
./tests/benchmarks/http/run_bench.sh

# 快速模式（约 2 分钟）
./tests/benchmarks/http/run_bench.sh --quick

# 只测某个服务端
./tests/benchmarks/http/run_bench.sh --only xray
./tests/benchmarks/http/run_bench.sh --only go

# 查看对比结果
./tests/benchmarks/http/run_bench.sh --compare
```

## 单独运行

```bash
# 1. 启动某个 HTTP 服务
./build/xray tests/benchmarks/http/http_server.xr

# 2. 运行压测客户端
python3 tests/benchmarks/http/http_bench.py --url http://127.0.0.1:8080

# 3. 快速模式
python3 tests/benchmarks/http/http_bench.py --url http://127.0.0.1:8080 --quick
```

## 结果对比

运行 `--compare` 会从 `results/` 目录读取各服务端的 JSON 结果文件，
输出并排对比表格，最优项标 `*`。

## 文件说明

| 文件 | 说明 |
|------|------|
| `run_compare.py` | 跨平台一键对比脚本，依次启动服务端并运行统一客户端 |
| `run_bench.sh` | wrk 压测脚本，依次启动各服务端并压测 |
| `http_bench.py` | 统一 Python 压测客户端（6 项测试） |
| `compare.py` | 读取 JSON 结果文件，输出对比表格 |
| `http_server.*` | 各语言的最简 HTTP 服务 |
| `results/` | 压测结果（JSON + 日志），gitignore |

## 与 wrk/ab 的区别

本测试套件侧重**标准化对比**，所有服务端在完全相同的客户端和测试参数下运行。
如果需要极限压测，可以配合 `wrk` 使用：

```bash
# 启动服务后，用 wrk 压测
wrk -t4 -c100 -d10s http://127.0.0.1:8080/plaintext
```

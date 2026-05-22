# WebSocket 服务端性能对比测试

对比 xray、Go、Node.js、Python 作为 WebSocket 服务端的性能表现。

## 测试项目

| 测试 | 说明 | 关键指标 |
|------|------|----------|
| **延迟** | 单连接往返延迟，64B ~ 64KB | min / avg / p95 / p99 (ms) |
| **吞吐量** | 单连接最大吞吐，64B ~ 64KB | msg/s, MB/s |
| **并发** | 多连接聚合吞吐，1 ~ 50 连接 | 总 msg/s, MB/s |
| **大消息** | 64KB ~ 1MB 载荷吞吐 | msg/s, MB/s |
| **连接抖动** | 快速建连/断连循环 | conns/s |

## 对比对象

| 语言 | 库 | 文件 |
|------|------|------|
| **xray** | stdlib/ws (C + xr) | `echo_server.xr` |
| **Go** | gorilla/websocket | `echo_server.go` |
| **Node.js** | ws | `echo_server.js` |
| **Python** | websockets (asyncio) | `echo_server.py` |

所有服务端都是最简 echo 实现，无额外逻辑开销，保证可比性。

## 环境要求

- **必须**：Python 3 + `websockets`（`pip3 install websockets`）
- **必须**：xray 可执行文件
- **可选**：Go（自动编译）、Node.js + npm（自动安装 ws）

## 一键运行

```bash
# 完整测试（约 3-5 分钟）
./tests/ws_benchmark/run_bench.sh

# 快速模式（约 1 分钟）
./tests/ws_benchmark/run_bench.sh --quick

# 只测某个服务端
./tests/ws_benchmark/run_bench.sh --only xray
./tests/ws_benchmark/run_bench.sh --only go

# 查看对比结果
./tests/ws_benchmark/run_bench.sh --compare
```

## 单独运行

```bash
# 1. 启动某个 echo 服务
./build/xray tests/ws_benchmark/echo_server.xr

# 2. 运行压测客户端
python3 tests/ws_benchmark/ws_bench.py --url ws://127.0.0.1:9001

# 3. 或用 xray 客户端压测
./build/xray tests/ws_benchmark/bench_client.xr
```

## 结果对比

运行 `--compare` 会从 `results/` 目录读取各服务端的 JSON 结果文件，
输出并排对比表格，最优项标 `*`。

## 文件说明

| 文件 | 说明 |
|------|------|
| `run_bench.sh` | 一键运行脚本，依次启动各服务端并压测 |
| `ws_bench.py` | 统一 Python 压测客户端（5 项测试） |
| `compare.py` | 读取 JSON 结果文件，输出对比表格 |
| `bench_client.xr` | xray 客户端压测（补充视角） |
| `echo_server.*` | 各语言的最简 echo 服务 |
| `results/` | 压测结果（JSON + 日志），gitignore |

# TCP Echo Server 性能测试

对比 xray、Go、Node.js、Python 四种语言/运行时的 TCP echo server 性能。

## 测试项目

| # | 测试 | 说明 | 核心指标 |
|---|------|------|----------|
| 1 | **延迟** | 单连接串行 echo 1000次，64B 消息 | avg/p50/p95/p99 µs |
| 2 | **吞吐量** | 单连接批量 echo，1KB × 10000次 | msg/s, MB/s |
| 3 | **并发** | 100 并发连接，每个 echo 100次 | 总 msg/s, avg/p99 延迟 |
| 4 | **连接速率** | 1000次 connect→echo→close 循环 | conn/s |
| 5 | **大消息** | 单连接发送 1MB 数据 | 传输时间, MB/s |
| 6 | **消息大小扫描** | 32B/256B/1KB/4KB/16KB/64KB | 各大小的 msg/s |

## 对比对象

- **xray** — 协程 + kqueue/epoll 非阻塞 I/O
- **Go** — goroutine + net poller
- **Node.js** — libuv 事件循环
- **Python** — asyncio 事件循环

## 环境要求

- xray 二进制（`build/xray` 或 PATH 中）
- Go 1.21+
- Node.js 18+
- Python 3.8+

## 使用方法

```bash
# 运行全部测试
./run_bench.sh

# 只测特定语言
./run_bench.sh xray go

# 手动运行单个测试
python3 tcp_bench.py --host 127.0.0.1 --port 9001 --server xray --output results/xray.json

# 对比结果
python3 compare.py results/
```

## 文件说明

| 文件 | 说明 |
|------|------|
| `echo_server.xr` | xray TCP echo server |
| `echo_server.go` | Go TCP echo server |
| `echo_server.js` | Node.js TCP echo server |
| `echo_server.py` | Python TCP echo server (asyncio) |
| `tcp_bench.py` | 统一 Python 压测客户端 |
| `compare.py` | 结果对比脚本 |
| `run_bench.sh` | 一键运行脚本 |

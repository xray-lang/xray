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
| 7 | **Message Path** | `net.read` / `net.write` echo | msg/s, avg/p99 延迟 |
| 8 | **Upload** | client → server 单向上传，server discard | MB/s |
| 9 | **Download** | server → client 单向下载 | MB/s |
| 10 | **Proxy** | client → proxy → upstream echo | msg/s, MB/s |
| 11 | **Bytes Path** | Xray `net.readInto` / `net.writeBytes` echo | msg/s, MB/s, sweep |
| 12 | **Slow Upload** | client 写入，server 慢读 | MB/s, complete |
| 13 | **Slow Download** | server 写入，client 慢读 | MB/s, complete |
| 14 | **Idle Connections** | 大量空闲连接后 ping | opened, ping p99, errors |
| 15 | **TLS** | 本地 TLS echo server + Xray `dialTLS` client | handshake/s, latency, throughput |

## 对比对象

- **xray** — 协程 + kqueue/epoll 非阻塞 I/O + `net.copy(conn, conn)` native stream path
- **Go** — goroutine + net poller
- **Node.js** — libuv 事件循环
- **Python** — asyncio 事件循环

## 环境要求

- xray 二进制（优先使用 `build-release/xray`，其次 `build/xray` 或 PATH 中）
- Go 1.21+
- Node.js 18+
- Python 3.8+

## 使用方法

```bash
# 运行全部测试
./run_bench.sh

# 运行 Phase 0 扩展场景（message/upload/download/proxy）
./run_bench.sh --suite phase0

# 运行 Phase 1 Bytes 路径对比（Xray string path vs Bytes path）
./run_bench.sh --suite phase1

# 运行 Phase 4 backpressure / idle 场景
./run_bench.sh --suite phase4

# 运行 Phase 5 TLS 场景（本地自签证书，不依赖公网）
./run_bench.sh --suite phase5 xray

# 只测特定语言
./run_bench.sh xray go
./run_bench.sh --suite phase0 xray go

# 手动运行单个测试
python3 tcp_bench.py --host 127.0.0.1 --port 9001 --server xray --output results/xray.json
python3 tcp_bench.py --host 127.0.0.1 --port 9001 --server xray --scenario message --test message_latency,message_throughput

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
| `tls_bench.py` | 本地 TLS server + Xray TLS client harness |
| `tls_client.xr` | Xray `net.dialTLS` benchmark client |
| `compare.py` | 结果对比脚本 |
| `run_bench.sh` | 一键运行脚本 |

## Server 模式

四个对照 server 都支持相同的启动模式：

| 模式 | 场景 | 说明 |
|------|------|------|
| `stream` | echo stream | xray 使用 `net.copy(conn, conn)`；其他语言退化为 read/write echo |
| `message` | protocol/message path | 显式 read/write echo，用于测试协议型路径 |
| `bytes` | reusable buffer path | xray 使用 `net.readInto` / `net.writeBytes`，payload 不创建临时 string |
| `discard` | upload | server 只读并丢弃，用于 client → server 单向传输 |
| `slow_discard` | slow upload | server 小块慢读，用于写背压测试 |
| `source` | download | server 主动发送固定字节数后关闭 |
| `idle` | idle connections | 保持连接并在客户端 ping 时 echo |
| `proxy` | relay | server 连接 upstream echo，并做双向转发 |

## xray TCP 设计说明

xray 的 `net` 模块现在区分两条 TCP 数据路径：

- `net.read` / `net.write`：把 payload 暴露为 Xray `string`，适合协议解析和文本处理。
- `net.readInto` / `net.writeBytes`：payload 进入用户提供的可复用 `Bytes` 缓冲区，适合二进制协议热路径。
- `net.copy(src, dst)`：payload 留在可复用 native buffer 中，适合 proxy、relay、`net.copy(conn, conn)` echo 等不需要逐字节进入语言层的高吞吐场景。

默认 echo suite 使用 `net.copy(conn, conn, 65536)`，目的是测试 TCP runtime 和调度器本身；Phase 1 suite 则用同一客户端负载对比 string path 与 Bytes path。

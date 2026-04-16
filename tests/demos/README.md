# xray 语言演示代码

展示 xray 语言核心特性的完整场景演示。

## 一、语言特性演示

单进程运行，展示 xray 语言本身的能力。

- **`demo_01_concurrent_pipeline.xr`** — 并发数据管道（Channel, scope, CSP 模式）
- **`demo_02_http_microservice.xr`** — HTTP 微服务（REST API, JSON, 路由）
- **`demo_03_type_system.xr`** — 类型系统（泛型类, 接口, 枚举, match, 继承）
- **`demo_04_structured_concurrency.xr`** — 结构化并发（scope, await.all/any, monitor, select）
- **`demo_05_event_system.xr`** — 实时事件系统（Select 多路复用, 定时器, 发布订阅）
- **`demo_06_functional.xr`** — 函数式编程（闭包, 高阶函数, 组合, 柯里化, 管道）
- **`demo_07_worker_pool.xr`** — 并发 Worker Pool（生产者消费者, 负载均衡）
- **`demo_08_json_data_processing.xr`** — JSON 数据处理（动态对象, null safety, 验证）
- **`demo_09_oop_patterns.xr`** — OOP 设计模式（Builder, Strategy, Observer, 状态机）
- **`demo_10_blockchain_prototype.xr`** — 区块链原型（并发共识, 交易池, 链验证）

```bash
./build/xray tests/demos/demo_01_concurrent_pipeline.xr
```

## 二、Cluster 分布式区块链演示

多节点运行，展示 xray cluster 模块在区块链场景的应用。每个文件是一个独立节点程序，多个节点组成 P2P 网络。

### 1. 完整区块链节点 — `cluster_blockchain_node.xr`

集成所有 cluster 原语的全功能区块链节点。

```
┌─────────────────────────────────────────────┐
│              Blockchain Node                 │
│                                              │
│  ┌─────────────┐  ┌──────────────────────┐  │
│  │ tx_submit    │  │ block_query          │  │
│  │ (serve/call) │  │ (serve/call)         │  │
│  └──────┬───── ┘  └──────────────────────┘  │
│         ↓                                    │
│  ┌──────────────┐     ┌──────────────────┐  │
│  │ mempool      │────→│ Block Producer   │  │
│  │ (channel)    │     │ (PoW + publish)  │  │
│  └──────────────┘     └──────────────────┘  │
│                              ↓               │
│  ┌──────────────┐     ┌──────────────────┐  │
│  │ monitor("*") │     │ subscribe        │  │
│  │ (故障检测)    │     │ ("block.>")      │  │
│  └──────────────┘     └──────────────────┘  │
└─────────────────────────────────────────────┘
```

**Cluster API 覆盖**: `start`, `join`, `serve`, `call`, `reply`, `channel`, `publish`, `subscribe`, `monitor`, `info`

```bash
./build/xray cluster_blockchain_node.xr -- --name node-A --port 9001
./build/xray cluster_blockchain_node.xr -- --name node-B --port 9002 --join 127.0.0.1:9001
./build/xray cluster_blockchain_node.xr -- --name node-C --port 9003 --join 127.0.0.1:9001
```

### 2. PBFT 共识协议 — `cluster_consensus_pbft.xr`

经典三阶段 PBFT 共识，用 Named Channel 实现消息广播，支持 View Change 容错。

```
Leader                Validator-1           Validator-2           Validator-3
  │                       │                     │                     │
  │─── pre-prepare ──────→│─────────────────────→│─────────────────────→│
  │                       │                     │                     │
  │←── prepare ──────────│←────────────────────│←────────────────────│
  │                       │                     │                     │
  │    [2f+1 quorum?]     │                     │                     │
  │                       │                     │                     │
  │←── commit ───────────│←────────────────────│←────────────────────│
  │                       │                     │                     │
  │    [2f+1 quorum? → FINALIZED]               │                     │
  │                       │                     │                     │
  │─── publish("consensus.decided") ────────────→────────────────────→│
```

**研究价值**: CSP Channel 天然建模 BFT 消息传递，`monitor` 驱动 View Change

```bash
./build/xray cluster_consensus_pbft.xr -- --name validator-0 --port 9101 --leader
./build/xray cluster_consensus_pbft.xr -- --name validator-1 --port 9102 --join 127.0.0.1:9101
./build/xray cluster_consensus_pbft.xr -- --name validator-2 --port 9103 --join 127.0.0.1:9101
./build/xray cluster_consensus_pbft.xr -- --name validator-3 --port 9104 --join 127.0.0.1:9101
```

### 3. 去中心化交易所 (DEX) — `cluster_defi_dex.xr`

分布式订单簿撮合引擎：Matcher 节点运行撮合、Gateway 转发订单、Observer 分析交易流。

**研究价值**: Named Channel 建模订单流水线，`serve/call` 建模 RPC 交易提交，`publish/subscribe` 建模实时行情

```bash
./build/xray cluster_defi_dex.xr -- --name matcher --port 9201 --role matcher
./build/xray cluster_defi_dex.xr -- --name gateway-1 --port 9202 --join 127.0.0.1:9201 --role gateway
./build/xray cluster_defi_dex.xr -- --name observer --port 9203 --join 127.0.0.1:9201 --role observer
```

### 4. Layer-2 状态通道 — `cluster_state_channel.xr`

链下支付通道：两方通过 Named Channel 交换签名状态更新，协作结算或争议强制关闭。

**研究价值**: Named Channel 天然建模链下双向通道，`monitor` 检测对手方掉线触发强制关闭

```bash
./build/xray cluster_state_channel.xr -- --name hub --port 9301 --role hub
./build/xray cluster_state_channel.xr -- --name alice --port 9302 --join 127.0.0.1:9301 --role user
./build/xray cluster_state_channel.xr -- --name bob --port 9303 --join 127.0.0.1:9301 --role user
```

### 5. 预言机网络 — `cluster_oracle_network.xr`

去中心化数据喂价网络：多个 Oracle 节点独立采集数据，Aggregator 做阈值聚合 + 异常过滤。

**研究价值**: `publish/subscribe` 建模数据广播，`serve/call` 建模链上查询，`monitor` 管理 Oracle 信誉

```bash
./build/xray cluster_oracle_network.xr -- --name aggregator --port 9401 --role aggregator
./build/xray cluster_oracle_network.xr -- --name oracle-1 --port 9402 --join 127.0.0.1:9401 --role oracle
./build/xray cluster_oracle_network.xr -- --name oracle-2 --port 9403 --join 127.0.0.1:9401 --role oracle
./build/xray cluster_oracle_network.xr -- --name oracle-3 --port 9404 --join 127.0.0.1:9401 --role oracle
./build/xray cluster_oracle_network.xr -- --name dapp --port 9405 --join 127.0.0.1:9401 --role consumer
```

## 三、Cluster API 与区块链概念映射

| xray Cluster API | 区块链概念 | 使用场景 |
| --- | --- | --- |
| `cluster.channel(name, size)` | 交易池 / 区块队列 / 状态通道 | 跨节点有序消息传递 |
| `cluster.serve(name)` | 节点 RPC 端点 | 交易提交、区块查询 |
| `cluster.call(name, args)` | 远程过程调用 | 智能合约调用、共识投票 |
| `cluster.reply(req, result)` | RPC 响应 | 查询结果返回 |
| `cluster.publish(topic, val)` | 区块广播 / 事件通知 | 新区块传播、交易确认 |
| `cluster.subscribe(pattern)` | 事件监听 | 监听新区块、价格更新 |
| `cluster.monitor(node)` | 故障检测 / 心跳 | Leader 崩溃检测、节点退出 |
| `cluster.nodes()` | 对等节点发现 | 网络拓扑感知 |
| `cluster.info()` | 网络诊断 | 集群健康监控 |
| `scope { go ... }` | 并行子任务管理 | 同时运行多个节点子系统 |
| `select { ... after N }` | 超时/多路复用 | 共识轮次定时、链上确认等待 |

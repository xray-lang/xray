# xray vs Go 协程性能对比测试

本目录包含 xray 和 Go 协程性能对比的基准测试用例。

## 测试用例

### 基础测试

| 目录 | 测试内容 | 关键指标 |
|------|---------|---------|
| `spawn/` | 大量协程创建销毁 | 创建速度, 内存占用 |
| `pingpong/` | 两协程互发消息 | 切换延迟 |
| `ring/` | N协程环形消息传递 | 消息吞吐量 |
| `fanout/` | 扇出扇入模式 | 任务分发效率 |
| `producer_consumer/` | 生产者消费者模式 | 吞吐量, 延迟 |
| `skynet/` | 经典skynet基准 | 综合性能 |
| `parallel_sum/` | 并行求和 | 并行效率 |
| `sleep_storm/` | 大量协程sleep唤醒 | 调度延迟 |

### 高级测试

| 目录 | 测试内容 | 关键指标 |
|------|---------|---------|
| `concurrent_sieve/` | CSP管道素数筛 | 管道吞吐, 级联协程 |
| `select_multiplex/` | 多通道select多路复用 | select调度效率 |
| `chameneos/` | CLBG变色龙对称会合 | 配对同步效率 |
| `work_pool/` | 动态任务池 | 负载均衡, 队列竞争 |
| `chain_spawn/` | 链式递归创建 | 深度创建, 栈管理 |
| `thundering_herd/` | 惊群唤醒 | 大量同时就绪处理 |
| `pipeline/` | 多级数据管道 | 管道吞吐, 背压 |
| `dining_philosophers/` | 哲学家就餐 | 通道同步, 死锁避免 |
| `starvation/` | 公平调度 | safepoint抢占验证 |
| `cancel_storm/` | 大量取消 | cancel路径效率 |
| `blocking_storm/` | 阻塞调用风暴 | handoff M 上限；需 `XR_BUILD_TEST_MODULES=ON` |

## 运行方式

### 单个测试

```bash
# xray（VM 解释执行）
./build/xray run tests/benchmarks/coro/spawn/spawn.xr

# xray（AOT 原生）
./build/xray build --native tests/benchmarks/coro/spawn/spawn.xr -o /tmp/spawn_aot && /tmp/spawn_aot

# Go
cd tests/benchmarks/coro/spawn && go run spawn.go
```

> 批量基准运行器与扩展曲线门禁（旧 `run_coro_benchmark.sh` / `run_coro_scaling_gate.sh`）
> 随 JIT 后端移除一并删除——它们的设计前提是 VM/JIT/AOT 多后端对比与 JIT 专项指标分析。
> 双后端（VM/AOT/Go）的统一基准运行器将按需重写。

## 等价性规则

每个 benchmark 必须能证明自己完成了同等工作量，推荐输出以下机器可读字段：

```text
completed_ops: <int>
expected_ops: <int>
checksum: <int>
correctness: true
reported_time_ms: <float>
```

旧 benchmark 的中文输出仍会被兼容解析，例如 `正确: true`、
`总会合次数/预期会合次数`、`成功取消: X / Y`、`短任务完成: X / Y`。
新 benchmark 不应只打印局部阶段耗时；`reported_time_ms` 或 `总时间` 必须覆盖完整工作、
等待子任务和清理路径。失败时必须非零退出或输出 `correctness: false`，不能吞掉错误后
继续输出成功结果。

## 测试参数

每个测试支持通过命令行参数调整规模：
- `spawn`: 协程数量（默认 100万）
- `pingpong`: 消息次数（默认 100万）
- `ring`: 协程数量、消息轮数
- `skynet`: 深度（默认 6，即 10^6 协程）

## 测试环境建议

- 关闭其他程序，减少干扰
- 多次运行取平均值
- 记录 CPU、内存、Go 版本、xray 版本

## 指标说明

- **ops/sec**: 每秒操作数
- **latency**: 单次操作延迟（纳秒/微秒）
- **memory**: 峰值内存占用
- **time**: 总执行时间

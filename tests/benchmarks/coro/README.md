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

### 099 VM-first 推荐门禁

当前协程性能主线先把 VM 正确性和 VM/Go baseline 压稳。JIT/AOT 暂停作为
主性能线外的专项；日常性能结论默认只看 `xray-vm` 和 `go`：

```bash
cmake -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j8
XRAY_BIN=build-release/xray scripts/run_coro_scaling_gate.sh \
  --vm-go \
  --repeats 5 \
  --workers 1,2,4,8,16 \
  --tests spawn,chain_spawn,pingpong,producer_consumer,work_pool,pipeline,skynet,select_multiplex,cancel_storm \
  --sched-stats \
  --json /tmp/xray_coro_vm_go_gate.json \
  --markdown /tmp/xray_coro_vm_go_gate.md
```

runtime 约定：

- `xray-vm`: 固定 `xray run --no-jit file.xr -- args...`
- `go`: 先 `go build -o ...`，计时只跑已构建二进制

JSON 会区分 `reported_time_ms`、`wall_time_ms`、`runtime_time_ms`、RSS、
`scaling_efficiency` 和 `vm_go_comparisons`。小 cardinality 只用于 correctness；
性能结论应使用 reported time 不再归零的 focused baseline。

需要 JIT 语义对照时显式使用 `--vm-jit` 或 `--vm-jit-go`，但 JIT 结果不再定义
协程语义。需要完整旧矩阵时再使用 `--all-backends`。

AOT 默认不进入 VM/JIT 性能门禁。需要观察 AOT correctness 时显式追加：

```bash
XRAY_BIN=build-release/xray scripts/run_coro_scaling_gate.sh \
  --vm-go \
  --include-aot-correctness \
  --tests pingpong,pipeline \
  --workers 1,4 \
  --repeats 1 \
  --json /tmp/xray_coro_aot_correctness.json \
  --markdown /tmp/xray_coro_aot_correctness.md
```

`--include-aot-correctness` 会报告 AOT 结果，但不会让已知 AOT blocker 污染
VM gate 的退出码。`--all-backends` 仍会把 AOT 失败计入矩阵失败。

### xray 测试
```bash
# 单个测试
./build/xray run --no-jit tests/benchmarks/coro/spawn/spawn.xr

# 全部测试
./scripts/run_coro_benchmark.sh

# 指定 worker 数并输出 JSON
./scripts/run_coro_benchmark.sh --workers 1,2,4,8 --json docs/bench/coro-latest.json

# 同时采集调度器统计指标
./scripts/run_coro_benchmark.sh --workers 1,2,4,8 --sched-stats --json docs/bench/coro-sched.json

# 同时运行 xray VM 和 xray JIT
./scripts/run_coro_benchmark.sh --vm-jit --tests pingpong,pipeline --args "100000"

# 增加 xray AOT 对照；AOT 编译失败会记录为 build-fail，不计作运行速度
./scripts/run_coro_benchmark.sh --include-aot-correctness --tests pingpong,parallel_sum --args "100000"

# 只跑部分场景
./scripts/run_coro_benchmark.sh --tests producer_consumer,select_multiplex,thundering_herd
```

### 扩展曲线门禁

`run_coro_scaling_gate.sh` 用于阶段性性能门禁。它会预编译 Go benchmark，
使用相同参数矩阵运行 Xray VM/JIT/Go，以及显式 opt-in 的 AOT，记录 repeated runs 的 median/min/max，
并把正确性、wall time、reported time、RSS 与调度指标写入 JSON。

```bash
cmake -B build-release -DCMAKE_BUILD_TYPE=Release && cmake --build build-release -j8
XRAY_BIN=build-release/xray scripts/run_coro_scaling_gate.sh \
  --vm-go --repeats 5 --workers 1,2,4,8,16 \
  --tests spawn,pingpong,producer_consumer,pipeline \
  --json /tmp/xray_coro_scaling.json \
  --markdown /tmp/xray_coro_scaling.md
```

Go 对照必须运行已构建二进制，不能使用 `go run` 计时。Xray VM 参数必须走
`xray run --no-jit file.xr -- args...`，确保 benchmark 规模参数传给脚本而不是 CLI。

## 等价性规则

每个 benchmark 必须能证明自己完成了同等工作量，推荐输出以下机器可读字段：

```text
completed_ops: <int>
expected_ops: <int>
checksum: <int>
correctness: true
reported_time_ms: <float>
```

旧 benchmark 的中文输出仍会被 gate 兼容解析，例如 `正确: true`、
`总会合次数/预期会合次数`、`成功取消: X / Y`、`短任务完成: X / Y`。
新 benchmark 不应只打印局部阶段耗时；`reported_time_ms` 或 `总时间` 必须覆盖完整工作、
等待子任务和清理路径。失败时必须非零退出或输出 `correctness: false`，不能吞掉错误后
继续输出成功结果。

### Go 测试
```bash
# 单个测试
cd tests/coro_benchmark/spawn && go run spawn.go

# 全部测试
./scripts/run_coro_benchmark.sh --go-only

# 同时设置 Go 的 GOMAXPROCS 与 xray 的 XRAY_WORKERS
./scripts/run_coro_benchmark.sh --all --workers 1,2,4,8 --json docs/bench/coro-xray-go.json

# 同时运行 xray VM、xray JIT、xray AOT 和 Go
./scripts/run_coro_benchmark.sh --all-backends --workers 1,2,4,8 --json docs/bench/coro-all-backends.json
```

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

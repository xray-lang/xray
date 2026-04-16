# 协程安全测试套件

作者：xingleixu@gmail.com

## 测试目标

验证 xray 协程系统的以下特性：

1. **Per-Coroutine GC**：每个协程独立堆，协程结束时整体释放
2. **无锁并发**：协程间不共享可变状态
3. **消息传递**：Channel 深拷贝/零拷贝混合方案
4. **协程参数**：const 类型安全传递
5. **闭包捕获**：upvalue 的正确处理
6. **抢占调度**：长时间运行协程不会饿死其他协程
7. **死锁预防**：超时机制和非阻塞操作
8. **监控调试**：协程状态查询和错误捕获

## 测试文件

| 文件 | 测试内容 |
|------|----------|
| 01_channel_deep_copy.xr | Channel 深拷贝隔离 |
| 02_channel_const_share.xr | Channel const 零拷贝共享 |
| 03_go_param_isolation.xr | 协程参数隔离 |
| 04_closure_upvalue.xr | 闭包 upvalue 隔离 |
| 05_concurrent_gc.xr | 并发 GC 压力测试 |
| 06_producer_consumer.xr | 生产者-消费者模式 |
| 07_broadcast.xr | 广播模式（零拷贝） |
| 08_pipeline.xr | 流水线处理 |
| 09_long_running.xr | 长时间运行与抢占调度 |
| 10_deadlock_detection.xr | 死锁场景与预防 |
| 11_monitoring_debug.xr | 协程监控与调试 |

## 运行测试

```bash
# 运行单个测试
./build/xray tests/coroutine_safety/01_channel_deep_copy.xr

# 运行所有测试
for f in tests/coroutine_safety/*.xr; do
    echo "Testing: $f"
    ./build/xray "$f" || echo "FAILED: $f"
done
```

## 验证要点

- 无段错误（SIGSEGV）
- 无数据竞争
- GC 正确回收
- 结果正确

## 调试建议

### 长时间运行问题

当协程长时间运行时：
- 使用 `cancelled()` 函数检查取消状态
- 在循环中周期性检查，响应取消请求
- 使用超时机制避免无限等待

```xray
fn long_task() {
    while (running) {
        if (cancelled()) {
            return "cancelled"
        }
        // ... 工作代码 ...
    }
}
```

### 死锁预防

避免死锁的最佳实践：
- 使用 `sendTimeout()` / `recvTimeout()` 代替阻塞操作
- 使用 `trySend()` / `tryRecv()` 进行非阻塞尝试
- 避免循环等待模式
- 使用 Channel 作为信号量控制并发

```xray
// 使用超时避免死锁
let sent = ch.sendTimeout(value, 1000)  // 1秒超时
if (!sent) {
    print("发送超时，可能存在死锁")
}

// 非阻塞尝试
let val = ch.tryRecv()
if (val == null) {
    print("Channel 为空")
}
```

### 协程监控

监控协程状态：
- `task.done` - 是否完成
- `task.cancelled` - 是否被取消
- `task.result` - 获取结果
- 使用命名协程便于调试：`go(name: "worker-1") task()`

```xray
let t = go(name: "data-processor") processData(data)

// 检查状态
if (!t.done) {
    print("任务仍在运行")
}

// 获取结果
await t
print("结果:", t.result)
```

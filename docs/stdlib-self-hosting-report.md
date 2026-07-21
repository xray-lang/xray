# Xray 标准库自举完成报告

> 任务：148 / 196 / 201 / 221 统一收口
>
> 完成基线：`agent/221-compiler-batch`，2026-07-21
>
> 判定方式：源清单、类型边界、VM/AOT 契约、性能样本和残留扫描共同判定；不以“移除全部 native 代码”为完成条件。

## 1. 完成判定

标准库自举机器门禁已经达到完成态：

- 登记模块：32；L0/L1/L2/L3/L4/L5 分层分别为 2/5/4/3/13/5。
- 语义策略：`xray_semantic` 19、`native_primitive` 10、`native_library` 3。
- 公开符号语义所有权：Xray 741、native primitive 334、native library 33。
- 动态迁移债务：0；获批 VM fastpath：0。
- 正确性契约：32 个模块、61 个用例；32 个 legacy oracle 均可执行。
- 性能治理：32 个 suite、32 个活跃 benchmark，全部保留原始样本。
- 源一致性：通过；自举完成门禁：通过；完成阻塞项：0。

“自举完成”在这里表示：标准库公共语义由 `.xr` 或显式类型化 native 边界拥有，VM/AOT 受同一契约约束，不再存在未登记动态债务、隐式 VM 快路径或重复 AOT helper。它不要求把 OS、网络、TLS、压缩、密码学等不可移植能力重新用 Xray 实现。

## 2. 仍保留的 native 边界

18 个模块仍有可审计 native 边界；它们均在清单中声明层级、策略、公开符号或私有源文件及保留理由。

| 模块 | 层 | 策略 | 公开 native 符号数 | 私有 native 源 | 边界理由摘要 |
| --- | --- | --- | ---: | ---: | --- |
| prelude | L1 | native_primitive | 0 | 1 | bootstrap 基元 |
| time | L1 | native_primitive | 8 | 0 | 系统时钟与休眠 |
| math | L1 | native_primitive | 51 | 0 | 数学基元 |
| base64 | L4 | xray_semantic | 0 | 1 | WebSocket 数据面辅助 |
| regex | L3 | native_library | 26 | 1 | 成熟正则引擎 |
| mem | L1 | native_primitive | 36 | 0 | 内存与布局基元 |
| runtime | L0 | native_primitive | 8 | 0 | 运行时堆与循环收集控制 |
| sync | L4 | xray_semantic | 5 | 1 | OS 同步数据面 |
| sys | L2 | native_primitive | 22 | 1 | 线程、进程、动态库与信号 |
| io | L2 | native_primitive | 24 | 0 | 文件系统与字节 I/O |
| os | L2 | native_primitive | 30 | 0 | 操作系统查询与进程能力 |
| test_yield | L0 | native_primitive | 0 | 1 | 内部调度测试边界 |
| net | L2 | native_primitive | 40 | 1 | socket、TLS 与字节 I/O |
| http | L5 | xray_semantic | 0 | 1 | HTTP/2、TLS、socket、parser buffer 与连接池数据面 |
| ws | L5 | xray_semantic | 10 | 1 | WebSocket transport 数据面 |
| crypto | L3 | native_library | 10 | 1 | 成熟密码学实现 |
| compress | L3 | native_library | 10 | 1 | zlib 兼容压缩与校验内核 |
| cluster | L5 | xray_semantic | 17 | 1 | 分布式 transport、发现与节点生命周期数据面 |

HTTP 的 `Router<T>`、请求/响应结构，Coro 的诊断、本地存储与池提交，cluster 控制面、net 字节 I/O、ws 连接选项均已改为类型化表面。任意 JSON 只允许出现在显式 wire/bridge 语义中。

## 3. 动态表面

未完成动态迁移债务为 0。保留的 14 个入口均在 allowlist 中，且不代表类型系统缺口：

- `Logger.child/debug/error/fatal/info/warn` 与对应的 `log.*` 六个函数：显式 structured-log `Json` bridge，共 12 项。
- `cluster.publish`：显式 JSON wire payload 输入。
- `HttpResponse.json`：显式把响应体解码为任意 JSON 的输出。

固定结构的请求、响应、配置、诊断、节点元数据及错误结果不得借上述 allowlist 回退到 `unknown`。

## 4. VM/AOT 正确性证据

- 32 个模块均有迁移契约，合计 61 个用例。
- 每个契约都固定 legacy commit，并具备可执行 legacy oracle。
- 每个模块按 `value`、`error`、`effect`、`complexity` 四个维度声明等价性。
- 所有契约用例均在 VM 与 AOT 执行并通过；`runtime` 与 `test_yield` 的控制面也已在 AOT 上真实执行，不再以 VM-only 结果替代。
- 完成态普通构建的全量 CTest：293/293 通过；6 个按平台/构建模式条件跳过。
- 关键专项：native backend 38/38、Xi C generator 181/181、xglobal summary 209/209、MCP protocol 91/91。

## 5. VM/AOT 性能基线

以下为非 quick 模式、每项 1 次 warmup + 5 次测量的中位数；原始数据保存在 `build/stdlib-governance/benchmarks-full-final.json`。比值为 `VM/AOT`，32/32 均满足各自清单预算。

| 模块 | VM 中位数 ns | AOT 中位数 ns | VM/AOT | AOT 二进制 bytes |
| --- | ---: | ---: | ---: | ---: |
| base64 | 5,980,375 | 3,478,834 | 1.72 | 114,544 |
| encoding | 5,969,125 | 3,302,708 | 1.81 | 167,680 |
| compress | 5,062,792 | 3,402,167 | 1.49 | 91,632 |
| crypto | 4,109,417 | 3,024,708 | 1.36 | 91,600 |
| path | 6,462,250 | 3,586,208 | 1.80 | 148,400 |
| http | 63,433,625 | 3,541,750 | 17.91 | 264,128 |
| url | 7,409,708 | 3,889,542 | 1.91 | 187,616 |
| cluster | 5,052,542 | 2,797,792 | 1.81 | 168,536 |
| csv | 8,714,834 | 3,547,500 | 2.46 | 246,512 |
| ws | 43,623,917 | 8,149,542 | 5.35 | 307,608 |
| _probe | 4,728,834 | 3,366,333 | 1.40 | 92,952 |
| datetime | 8,254,375 | 3,586,791 | 2.30 | 173,512 |
| io | 7,259,541 | 3,938,291 | 1.84 | 110,288 |
| log | 9,703,292 | 3,642,000 | 2.66 | 215,888 |
| math | 4,272,625 | 3,458,584 | 1.24 | 52,544 |
| mem | 3,824,250 | 3,921,667 | 0.98 | 50,464 |
| net | 7,437,667 | 2,754,500 | 2.70 | 50,520 |
| os | 7,924,916 | 4,921,750 | 1.61 | 91,472 |
| parallel | 13,280,958 | 3,485,917 | 3.81 | 361,672 |
| prelude | 6,564,833 | 3,686,375 | 1.78 | 50,472 |
| regex | 5,069,500 | 3,240,875 | 1.56 | 159,176 |
| runtime | 5,335,208 | 4,201,792 | 1.27 | 279,160 |
| simd | 8,823,958 | 3,337,833 | 2.64 | 90,344 |
| strconv | 319,632,542 | 76,263,208 | 4.19 | 128,424 |
| sync | 4,334,667 | 3,329,458 | 1.30 | 53,000 |
| sys | 4,658,250 | 3,250,375 | 1.43 | 71,984 |
| test_yield | 4,475,042 | 3,424,584 | 1.31 | 296,440 |
| text | 284,893,625 | 63,616,208 | 4.48 | 128,520 |
| time | 3,877,250 | 3,715,041 | 1.04 | 52,432 |
| toml | 20,322,542 | 2,609,000 | 7.79 | 395,360 |
| xml | 16,354,250 | 3,237,041 | 5.05 | 292,080 |
| yaml | 25,720,208 | 3,304,500 | 7.78 | 348,176 |

这些数字是回归预算与趋势基线，不是跨语言或绝对吞吐结论；`mem` 的 0.98 仍在其预算内。

## 6. 安全、模糊测试与残留风险

使用 Homebrew LLVM 22.1.4 的 ASAN+UBSAN libFuzzer 构建，以固定 seed 221 重放原始 corpus 并各执行 1,000 次变异：

| 目标 | 结果 | 峰值 RSS |
| --- | --- | ---: |
| fuzz_lexer | 通过 | 37 MB |
| fuzz_parser | 通过 | 41 MB |
| fuzz_stdlib_data | 通过 | 1,670 MB |

本轮模糊测试修复了 recoverable parser 的空左值崩溃、类型列表无进展导致的 OOM、非法 label 恢复死循环，并把 stdlib data harness 更新到当前 XML/YAML recovery API。项目 sanitizer 约定使用 `detect_leaks=0`；开启 leak 检测时观察到的 56-byte 分配来自 libFuzzer 自身 RSS 线程，而非 Xray 路径。

与任务 221 native 边界直接相关的 sanitizer 聚焦组 11/11 通过，覆盖 FFI ASAN smoke、native backend、HTTP parser/buffer、channel/scope/work queue/async pool/scheduler/timer/xglobal。

全仓 sanitizer CTest 仍为红色，包含 coroutine heap UAF/stack overflow、FFI 越界、latch 生命周期、AOT map/runtime、UBSAN shift/alignment 及超时等跨路线历史问题。因此本报告只声明“标准库自举完成门禁”和“相关 native 边界聚焦验证”通过，不声明全仓 sanitizer clean；这些问题应作为后续 runtime/AOT/FFI 安全路线继续治理。

此外，核心协程类型 `TaskResult<T>.Failed(unknown)` 仍存在于 `stdlib/types/coroutine.xr`。它不属于 196/221 的 32 个模块动态 API 债扫描，而由任务 202 P3 的 `Task<T,E>` / `TaskResult<T,E>` 原子切换独立治理；当前 `check_source_unknown_aot_baseline.py` 资产通过，但本报告不声明“全仓 source unknown 已清零”。

## 7. 重复表面与残留清理

以下 16 个模块已禁止重新引入旧 `.def` 语义入口：`_probe`、`base64`、`csv`、`datetime`、`encoding`、`log`、`parallel`、`path`、`simd`、`strconv`、`sync`、`text`、`toml`、`url`、`xml`、`yaml`。

以下 13 个模块已禁止重新引入 AOT 专用 helper：`base64`、`csv`、`datetime`、`encoding`、`log`、`path`、`simd`、`strconv`、`text`、`toml`、`url`、`xml`、`yaml`。

API、analyzer/LSP、language/knowledge、MCP 查询和生成文档均由 postmerge gate 检查同步；MCP 精确模块查询优先于模糊匹配。

## 8. 可复现门禁

```sh
cmake --build build --target stdlib-self-hosting-complete
python3 scripts/report_stdlib_self_hosting.py --root . --check --require-complete
python3 scripts/stdlib_migration.py verify --root . --xray build/xray
python3 scripts/check_stdlib_module_merge.py --root . postmerge --xray build/xray --require-complete
python3 tests/benchmarks/stdlib/run.py --xray build/xray --results build/stdlib-governance/benchmarks-full-final.json
ctest --test-dir build -j8 --output-on-failure
```

机器完成目标只接受上述源派生结果；本报告是完成时的审计快照，不替代清单、契约、原始 benchmark JSON 或测试日志。

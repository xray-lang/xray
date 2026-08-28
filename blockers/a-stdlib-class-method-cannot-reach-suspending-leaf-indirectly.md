# Blocker：stdlib 类方法无法间接到达可挂起叶子

- **Lane**：6-9（io 写侧文件描述符叶子）
- **状态**：`BLOCKED`
- **请求归属**：编译器（`src/ir/`、`src/frontend/analyzer/`、`src/stdlib/xstdlib_metadata.h`）
- **严重度**：把一个可挂起的原生叶子包进 Xray 函数之后，任何 stdlib 类方法都不能再调用它。
  这不是 io 独有的，`sync` 的 `Mutex` / `RwLock` 已经先撞上了。

## 精确来源

| 项 | 值 |
|---|---|
| 基线提交 | `34be0379c` |
| 工作分支 | `work/6-9-io-write-leaf-34be0379c` |

## 现象

编译器对下面这个形状 fail-closed：

```
[xcompiler] Xi IR pipeline failed at ownership: coroutine lowering failed closed
before publishing CoroLowered (func=<main>)
```

`func=<main>` 是**被编译模块的根 XiFunc**，不是真正失败的函数
（`src/frontend/codegen/xcompiler.c:251-253` 只渲染根）。这条消息本身没有判据，
仓库自己已经把它登记为「无结构化诊断的不透明拒绝」——
`tests/diff/survey_refusals.py:354` 与 `scripts/check_live_refusal_manifest.py:756`
给它的分类是 `opaque-refusal-without-structured-diagnostic`，
`required_action = emit-stable-diagnostic-and-source-owned-structured-refusal`。

## 触发条件（实测归纳）

`<main>` 里存在一个 `XI_CALL_METHOD` 挂起点，**且该方法是间接挂起的**——
即挂起点不在方法体里，而在它调用的某个 Xray 函数下面。

| 形状 | 结果 |
|---|---|
| 模块级 fn → Xray 中间函数 → 可挂起叶子 | **通过** |
| 类方法 → **直接**可挂起叶子 | **通过**（`sys.Process.wait` → `__processWait`） |
| 类方法 → Xray 中间函数 → 可挂起叶子 | **失败** |
| 用户程序里同形的类方法 → 同模块 fn → 跨模块 export | **通过** |

最后一行很重要：同样的形状写在**用户程序**里是通的，只有写在 **stdlib 模块**里才失败。

## 这不是本 lane 引入的

两个与 io 完全无关的既有用例报**逐字相同**的错：

```
tests/diff/cases/semantics/concurrency/mutex_generic_compose.xr
tests/diff/cases/semantics/concurrency/rwlock_generic_compose.xr
```

它们只 `import { Semaphore, ... } from sync`，形状是「用户类方法（`Mutex.lock` /
`RwLock.read`）内部经 `Semaphore.acquire()` 间接挂起，从 `<main>` 调用」。
同族的三个在更后面的 stage 才炸，读数是同一个矛盾：

```
tests/diff/cases/semantics/concurrency/barrier_compose.xr
  → XR_SEM_0019: coroutine state count disagrees with grounded call authority
    ... selector=wait expected=0 actual=1
condvar_compose.xr → 同，selector=lock
once_compose.xr    → 同，selector=call
```

`expected=0 actual=1` 就是下面那组 oracle 互相矛盾的直接读数：Xi 在类方法调用点
建了 1 个协程状态，SemanticPlan 认为该调用点没有挂起授权。

这五个都登记在 `tests/diff/known_failures_not_comparable.txt:118-125`。

> **顺带一个需要单独确认的观察**：该清单的头注写着这些用例「VM 已经能跑，只有
> target plan 还在拒」。实测它们**在当前 VM 上就已经拒了**。要么清单陈旧，要么是
> 回归。不在本 lane 的影响面内（它们不碰 io），但值得单独查。

## 根因：四套「可挂起性」oracle，种子不一致

同一个「这个函数会不会挂起」的事实，编译器里有四个来源：

| # | 位置 | 种子 | `io.__fileWrite` 是否命中 |
|---|---|---|---|
| A | `src/ir/xi_coro_analyze.c:111-119` → `src/stdlib/xstdlib_metadata.h:182` | `vm_binding: "yieldable"` | **是** |
| B | `src/frontend/analyzer/xanalyzer_suspend.c:397-411` → `xstdlib_metadata.h:188-196` | **硬编码 `strcmp(module, "net") != 0` 直接返回 false**，再加 7 个成员名白名单 | 否 |
| C | `src/analysis/xglobal_producer.c:8962-8983` | 先问 B，再问 `..._func_is_yieldable` | **是** |
| D | `src/ir/xi_lower_expr.c:5404-5417`（`XI_FLAG_MAY_SUSPEND` 戳） | 同 B | 否 |

两处消费点各取一套，于是自相矛盾：

- `src/ir/xi_pipeline.c:295-306` `xi_pipeline_coro_func_suspendability` 用 **B**
- `src/ir/xi_pipeline.c:368-369` `..._call_suspendability` 用 **C**

`net` 之所以从来没暴露这个：它的叶子被四套 oracle 一致承认，并且还有第五道保险
`xi_coro_is_net_io_call`（`src/ir/xi_coro_analyze.c:338-348`）——写死
`{"accept","read","write","writeBytes"} × "net"`，专门让「纯 Xray wrapper 也算挂起点」。
**这个逃生舱只给了 net。**

fail-closed 的具体落点在 `src/ir/xi_coro_lower.c:1103-1105`：
`xi_coro_analyze(f, resolver)` 返回 NULL，而它的第一道闸
（`src/ir/xi_coro_analyze.c:2032`）是 `xi_coro_all_calls_resolved`——只要有一个 call
既解析不到静态目标、又拿不到闭世界 callsite 分类（返回 -1），整个函数的分析就失败。

## 请求的能力

把 B 和 D 的种子统一到 A/C 用的 `xr_stdlib_metadata_func_is_yieldable`
（`src/stdlib/xstdlib_metadata.h:182`）。

`src/analysis/xglobal_producer.c:8970-8975` 的注释**已经表达了这个立场**：
「声明式 metadata 是可挂起性的唯一来源，在这里枚举名字会悄悄漏掉新声明的」。
只是 analyzer 和 lowering 两侧还没跟上。

两处硬编码是：

1. `src/stdlib/xstdlib_metadata.h:188-196` `xr_stdlib_metadata_func_resumes_by_netpoll_retry`
2. `src/ir/xi_coro_analyze.c:338-348` `xi_coro_is_net_io_call`

顺带希望修掉的：`src/ir/xi_coro_lower.c:1112-1118` 递归子函数失败时**丢掉子函数身份**，
只报根，这是 `func=<main>` 毫无信息量的原因。

## 本 lane 绕开的方式

`stdlib/io/io.xr` 的 `BufWriter` 把 publish 步骤从方法改成模块级函数：

```
// 之前
w.flush()
// 现在
io.flushWriter(w)
```

这和 `stdlib/net/net.xr` 已有的设计一致——它的每一个 I/O 入口
（`net.xr:186-209` 的 `readInto` / `writeBytes` / `accept` / `copy`）都是模块级函数
操作不透明句柄，**一个带 I/O 的类方法都没有**。那不是巧合。

`BufWriter` 在仓库里没有任何调用方，所以这次形状改变的代价只是 API 形式本身。

**这个能力落地后可以回滚的东西**：把 `flushWriter` 变回 `BufWriter.flush()` 方法。
`io.xr` 里那段解释为什么它是模块级函数的注释也可以一并删掉。

## 有意未改的文件

```
src/ir/**
src/frontend/**
src/analysis/**
src/stdlib/xstdlib_metadata.h
```

本 lane 的允许改动面不含这些，而且统一四套 oracle 的种子是会波及全部 19 个
`vm_binding: "yieldable"` 叶子的改动，属于编译器归属。

## 复现

```bash
./build-nofp/xray run tests/diff/cases/semantics/concurrency/mutex_generic_compose.xr
```

与 io 无关，报同一条错。这是「不是本 lane 引入」的最短证明。

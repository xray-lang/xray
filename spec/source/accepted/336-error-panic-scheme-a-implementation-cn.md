# 已接受目标规则：Task336 — 错误与 panic 方案 A：语义定界与实施

> 2026-11-16 从任务文档迁入统一规范源。此处维护已接受的目标定义；原任务只维护实施、依赖、理由与验收记录。
> 迁入不表示实现完成。下面保留的示例是目标验收材料，不能据此声称当前编译器、VM 或 AOT 已支持。历史修订和已明确被取代的规则保持历史身份；不能将旧版本重新激活。
> 实施任务：[336-error-panic-scheme-a-implementation-cn](../../../../xray-docs/tasks/336-error-panic-scheme-a-implementation-cn.md)。

## 2. 唯一的源码与通道规则

### 2.1 三类非正常退出

| 通道 | 进入签名 E | 产生/传播方式 | 处理方式 |
|---|---|---|---|
| 业务错误 | 是 | `throw`、声明可抛调用、具有业务 E 的 await/scope | 普通 catch；try? 转 Optional；try! 转 panic |
| panic | 否 | 语言定义的运行时故障、失败的 try!、关闭后的 send/select 发送 | `catch panic`；未处理时按 panic 根边界规则结束 |
| 取消 | 否 | 规定的取消检查点、await 已取消任务 | 不可捕获的取消展开；awaitResult/poll 以值观察 |

`throws(Never)` 或省略 throws 只表示没有业务错误，不表示不会 panic、不会取消、不会挂起。普通 catch 不拦截 panic；try? 不把 panic/取消变成 null；try! 也不把取消转成 panic。PanicInfo 不变成 Error 的子类型，Cancelled 不重新进入业务错误 union。

### 2.2 表达式和块各自承担什么

1. `try expr` 标记并传播业务错误，`try? expr` 消费为 Optional，`try! expr` 在业务失败时产生 panic。保留 Task329 的表达式范围与 Optional 规则；一个 try 可以覆盖整个规定范围内的表达式，不要求每个嵌套调用都重复写。
2. `try { body } catch ...` 的主体许可其中的业务调用，不强制再写 try。块内显式 try 仍合法，但它自己的范围必须确实含业务错误来源；“外层已许可”本身不构成 E0336。
3. 主体许可不延伸到嵌套函数/闭包的定义体或本语句的 handler。调用闭包时按该调用的声明签名检查；创建闭包本身不等于执行其函数体。
4. handler 可以处于更外层 try 的保护区域。一个 try 的普通 handler 与 panic handler 不互相保护；handler 新抛出的错误或 panic 寻找外层处理器。
5. panic-only 指“只有 panic handler”，不是“主体业务 E 必须为 Never”。它也许可主体中的业务调用，但不处理业务 E；剩余 E 必须由外层普通 catch 或所在函数的 throws 接收，否则仍报 E0331。
6. 直接 `throw e` 自身就是业务错误来源，不另加 try；它同样接受错误类型与逃逸签名检查。

以下为目标语义片段，辅助函数签名按注释约定；实施时必须补成可执行夹具，不把片段当作本轮测试通过记录。

```xray
// readConfig: throws(ConfigErr)；validate: throws(ConfigErr)
// ch.send: 没有业务错误；关闭后产生 E0702 panic
try {
    var cfg = readConfig()
    validate(cfg)
    ch.send(cfg)                 // 不加 try；普通 catch 不捕获它的 panic
} catch (e: ConfigErr) {
    print("配置失败", e)
} catch panic (p) {
    print("运行故障", p.message)
}
```

```xray
try {
    ch.send(value)               // 纯 panic 边界合法，主体可没有业务 E
} catch panic (p) {
    print(p.code)
}

// try ch.send(value)            // E0336
// try { ch.send(value) }
// catch (e) { print(e) }
// catch panic (p) { print(p) }  // 仍是 E0336；应删无业务来源的普通 catch
```

### 2.3 E0336 的精确定义

定义一个区域的“业务来源事实”为：该区域内、属于当前函数检查上下文的操作，按声明与实际求值规则可能产生业务错误。它是静态潜力，不是“这一次一定失败”，也不是“最终一定逃到某个 handler”。

| 语法 | E0336 判定 |
|---|---|
| try / try? / try! 表达式 | 自身作用范围没有业务来源即拒绝；外层 handler 类型不改变判定 |
| 含任意普通 catch 的 try 块 | 主体保护区域没有业务来源即拒绝；同时含 catch panic 也不豁免 |
| 仅含 catch panic 的 try 块 | 不要求业务来源；不做“静态证明可能 panic”的对称检查 |
| 没有 handler 的 try 块 | 按原有文法拒绝，不借 E0336 开放新块形式 |

业务来源包括：声明 E 非 Never 的调用、业务 E 非 Never 的 await/scope，以及直接 throw。实参求值、实际省略的默认参数和生成初始化器按 Task329 的现有规则贡献错误；不能只看最外层被调函数的 E。

**定义处优先、局部检查、优化无关：**

- 普通函数、接口/间接调用按调用所见声明检查，不通过读取被调函数体来消掉 E。声明 throws(E) 即使当前函数体总成功，也仍是可抛调用。
- 泛型定义中的未替换错误类型参数视为可能抛错；单态化为 Never 不回头拒绝已经合法的泛型体。在泛型体外调用完整实例化为 Never 的函数，调用方多余 try 仍应拒绝。
- comptime 折叠、常量传播、内联和死代码消除不改变标记合法性。正常参与类型检查的条件分支按声明检查，不根据本次运行输入推断；语言原本不参与检查的分支不由本任务重新定义。
- 默认表达式在定义处独立检查；调用方只承担本次实际求值的默认值。字段默认值和隐式构造器复用已有生成初始化合同，不把未知合同临时猜成 Never。
- `try scope` 检查 scope 对调用者暴露的业务 E；只可能取消或 panic 的 scope 不足以使业务 try 合法。主体操作的检查仍遵守 scope 自己的词法边界。

**区分“有错误来源”和“handler 可达”。** 当前 A 的区域判定会记录内层 try?、try! 或已捕获调用的原始声明错误。因此，外层普通 try 里存在这类来源时，不应仅因为错误已在内层消费就报 E0336。反过来，仅在 handler 中抛错，不能证明对应主体有错误来源。闭包定义中的 throw 也不能使外层区域合法。

这保持一条可解释的 E0336 规则。完整死 handler 分析还需扣除内层消费、模式覆盖与先前 handler；它应使用另一个诊断，不能将来悄悄扩大 E0336 的含义。本次不加入强制死 handler 检查。

# 已接受目标规则：340 - 函数类型参数默认不逃逸

> 2026-11-16 从任务文档迁入统一规范源。此处维护已接受的目标定义；原任务只维护实施、依赖、理由与验收记录。
> 迁入不表示实现完成。下面保留的示例是目标验收材料，不能据此声称当前编译器、VM 或 AOT 已支持。历史修订和已明确被取代的规则保持历史身份；不能将旧版本重新激活。
> 实施任务：[340-non-escaping-function-parameters-cn](../../../../xray-docs/tasks/340-non-escaping-function-parameters-cn.md)。

## 3. 语义

### 3.1 规则

1. **默认与标注。** 函数类型的参数默认不逃逸，包括函数类型内部的参数位置。写成 `escaping fn(...)` 的参数可以逃逸。`escaping` 是上下文关键字，只在参数类型前识别，和 `nosuspend` 一样不全局保留（[Task331](../../../../xray-docs/tasks/331-language-surface-convergence-cn.md) G12）；两者可以同时出现。
2. **不逃逸的值允许做的事：**
   - 调用它；
   - 作为实参传给不逃逸的参数；
   - 被不逃逸的局部闭包捕获；
   - 绑定到局部常量或变量（这个局部同样不逃逸）；
   - 出现在条件表达式的分支里（结果同样不逃逸）。
3. **不逃逸的值禁止做的事：**
   - 存进字段、数组、Map、Set 或模块状态；
   - 从函数返回，包括被返回的闭包捕获；
   - 被逃逸的闭包捕获；
   - 作为 `go` 或 Task 的实参，或被它们的闭包捕获；
   - 作为泛型类型参数 `T` 的值（`T` 绑定为函数类型时，泛型代码可以把 `T` 存起来）；
   - 传给 `escaping` 参数；
   - 写进 `ref` 参数指向的变量。
4. **局部闭包。** 捕获了不逃逸值的闭包本身也不逃逸，适用第 3 条的全部限制。
5. **照旧的部分：**
   - 字段、局部变量、集合元素、返回值和 `ref` 参数所指变量上的函数类型照旧，里面存的都是可以逃逸的值；
   - `CFn` 类型照旧，C 侧可能存住指针，仍然必须是 `nosuspend`。
6. **挂起性。**
   - 函数的 `suspends` 摘要，等于函数自身的挂起性，并上每个不逃逸函数参数在调用点的实参的挂起性。
   - `escaping` 参数和存起来的函数值按 A5 用类型上的位：书写出的类型默认可能挂起，可以写 `nosuspend`。
   - 诊断给出原因链，例如"经由 `each` 的参数 `f` → `ch.send`"。
7. **错误不变。** throws 仍按 Task329 E1、E4 写在签名上，"随参数抛错"仍用泛型错误参数 `E` 加 `Never` 表达。
8. **借用。** 不逃逸的闭包可以捕获 Slice、`ref` 等借用，不报 E0383；逃逸的闭包捕获借用，仍报 E0383。
9. **覆写与函数类型。** 第一版要求覆写方法、接口实现和被覆写的签名在每个函数类型参数上 `escaping` 与否完全一致；函数类型之间也要求一致，不做子类型放宽。
10. **诊断。** 违规时在出问题的那一行报"参数 `X` 不能逃逸"，说明原因（被存进哪里、被返回、交给了 `go`、作为泛型值），并提示改成 `X: escaping fn(...)`。新诊断码在 `src/shared/xerror_codes.h` 与规范源 `spec/source/sections/019-18-error-codes.md` 登记，沿用现有诊断注册表。

### 3.2 正例

```xray
fn each(xs: Array<i64>, f: fn(i64)) {
    for (x in xs) { f(x) }
}

fn report(xs: Array<i64>, ch: Channel<i64>) {
    defer { each(xs, (x) -> { print(x) }) }   // 通过：这次传入的 lambda 不挂起
    var total: i64 = 0
    each(xs, (x) -> { total = total + x })    // 通过：total 不装 Cell
    each(xs, (x) -> { ch.send(x) })           // 这次调用可能挂起，原因链：each 的参数 f → ch.send
}

fn countIf(n: i64, pred: fn(i64) -> bool) -> i64 {
    var c: i64 = 0
    for (i in 0..n) { if (pred(i)) { c = c + 1 } }
    return c
}

fn zeros(data: Slice<u8>) -> i64 {
    return countIf(len(data), (i) -> data[i] == 0)   // 通过：不逃逸的闭包可以捕获借用
}

fn apply(callback: fn() -> i64) -> i64 {
    const thunk = fn() -> i64 { return callback() + 1 }   // 通过：局部闭包不逃逸
    return countIf(1, (i) -> thunk() > 0)                 // 通过：只传给不逃逸参数
}

class EventBus {
    handlers: Array<fn(string)>                     // 字段类型照旧
    init() { this.handlers = [] }
    subscribe(handler: escaping fn(string)) { this.handlers.push(handler) }
}

fn compose(f: escaping fn(i64) -> i64, g: escaping fn(i64) -> i64) -> fn(i64) -> i64 {
    return fn(x: i64) -> i64 { return f(g(x)) }
}

fn replace(f: ref fn(i64) -> i64) { f = fn(n: i64) -> i64 { return 99 } }   // 通过：ref 指向的变量照旧
```

### 3.3 反例

```xray
class Bus {
    handlers: Array<fn(string)>
    init() { this.handlers = [] }
    subscribe(handler: fn(string)) {
        this.handlers.push(handler)             // 报错：参数 handler 不能逃逸，被存进了 this.handlers
    }
}

fn compose(f: fn(i64) -> i64, g: fn(i64) -> i64) -> fn(i64) -> i64 {
    return fn(x: i64) -> i64 { return f(g(x)) }   // 报错：返回的闭包捕获了不逃逸的参数 f 和 g
}

fn wrap(callback: fn() -> i64) -> fn() -> i64 {
    const thunk = fn() -> i64 { return callback() }
    return thunk                                  // 报错：thunk 捕获了不逃逸的 callback，不能返回
}

fn keep<T>(value: T) -> T { return value }
fn relay(f: fn(i64) -> i64) -> i64 {
    const g = keep(f)                             // 报错：不逃逸的 f 不能作为泛型 T 的值
    return g(1)
}

fn startWorker(job: fn()) {
    go run(job)                                   // 报错：job 交给了 go，任务可能比这次调用活得久
}

fn applyVia(transform: fn(fn(i64) -> i64) -> fn(i64) -> i64, f: fn(i64) -> i64) -> i64 {
    return transform(f)(1)                        // 如果 transform 的实现返回了它的参数，就在那个实现里报错；
}                                                 // 它的类型应写成 fn(escaping fn(i64) -> i64) -> fn(i64) -> i64，f 也要写 escaping
```

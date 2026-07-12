---
id: spec.7_scoping_and_name_resolution
order: 008
---

<!-- xr-spec:cn -->
---

## 7. 作用域与名字解析 (Scoping)

> 真值源：`src/frontend/analyzer/xanalyzer_scope.c`、`src/frontend/analyzer/xanalyzer_capture.c`。

### 7.1 词法作用域与提升

Xray 采用**词法作用域**：名字的可见性由源代码结构决定。

**作用域类型**：

| 作用域 | 触发 | 示例 |
|--|--|--|
| 模块 | 每个 `.xr` 文件 | 顶层 `var` `const` `shared` `fn` `class` |
| 函数 / 闭包 | `fn` / 箭头函数进入 | 参数 + 函数体 |
| 块 | `{...}` | `if` `while` `for` `match` 分支体 |
| `scope` 块 | `scope { ... }` 关键字 | 显式词法作用域 + 结构化并发（见 §10.7） |
| `for` 头 | `for (var i=0; ...)` | `i` 仅循环体可见 |
| `catch` 参数 | `catch (e)` | `e` 仅 catch 体可见 |
| 类体 | `class` 定义 | 字段、方法 |

**提升规则**：

- 顶层 `fn` `class` `struct` `interface` `enum` `type` **提升**至当前作用域顶部——可在定义前引用。
- `var` / `const` / `shared` **不提升**——必须在定义后使用。
- 同名重复声明：同作用域内 2 个同名变量 → 编译错误（嵌套作用域可 shadow）。

```xray
main()                    // OK：使用提升后的 fn
fn main() { ... }

var y = x                 // 错误：x 未声明
var x = 10
```

#### Shadow 规则

嵌套块可 shadow 外层同名变量：

```xray
var x = 1
{
    var x = "hello"           // shadow：OK
    print(x)                 // "hello"
}
print(x)                     // 1
```

### 7.2 闭包捕获语义

闭包捕获外层作用域的变量为 **upvalue**。

#### 普通同步闭包

默认按 **引用捕获**：

```xray
fn make_counter() -> (() -> int) {
    var count = 0
    return fn() -> int {
        count += 1                  // 修改外层 count
        return count
    }
}

var c = make_counter()
print(c())      // 1
print(c())      // 2
```

- 闭包与原变量**共享**。
- 外层作用域退出后，被闭包引用的变量会被 GC 保活（提升到堆）。

#### 闭包优化

编译器会分析 upvalue：
- 仅读 → 可能隐式复制（避免闭包转换）。
- 读写 → 提升为闭包 box。
- 详见 §17.5。

### 7.3 所有权与 move

Xray **不**是全面 ownership/borrow checker 语言（不像 Rust）。但在**跨协程数据传递**中使用 move 语义：

```xray
var big_buffer = Array<byte>(1024 * 1024)

var t = go fn(b: Array<byte>) -> int {
    return process(b)
}(big_buffer)             // 编译错误：owned heap 值不能裸跨协程传递

var t2 = go fn(b: Array<byte>) -> int {
    return process(b)
}(move big_buffer)        // OK：所有权转移

print(len(big_buffer))    // 编译错误：move 后访问
```

**move 使用场景**：`move` 作为**实参前缀**出现在调用位置（参见 §10.8）：

- `go f(move x)`、`go fn(...){...}(move x)`：把所有权转给协程。
- `ch.send(move data)`：跨协程发送时转移所有权（避免拷贝）。
- 普通函数调用 `f(move x)`：把所有权传入函数（被调函数独占）。

### 7.4 协程数据传递规则（避免数据竞争）

"保证编译期消除数据竞争"是 xray 并发模型的核心设计原则。

所有跨 execution 边界（`go` 闭包、`go` 实参、Channel send、deferred task、线程入口和导出 callback）消费同一个已验证 capture plan。合法性由值的 **storage owner、provenance、可变性与类型表示**共同决定，不由 `var` / `const` 关键字单独决定：

| Capture action | 适用值 | 边界行为 |
|---|---|---|
| inline value | 标量、不可变小值 | 直接复制位表示 |
| deep copy | 显式 `copy(x)` 的 owned graph | 在目标 execution owner 中物化独立图 |
| move | 显式 `move x` 的可转移局部 `var` | 转移所有权并静态废弃源绑定 |
| module readonly | 已冻结并发布的模块只读值 | 保留模块只读 owner，不复制到 root/task heap |
| shared ref | `shared`、Channel、Task、Atomic 等稳定共享身份 | 保留 shared/system owner 的引用 |
| reject | execution-local graph、可变 module state、悬垂 slice/pointer/upvalue | 编译错误并报告 owner/provenance 与所需显式动作 |

因此，局部 `const` 若只含 inline 值可以直接跨界；若它仍指向 execution-local graph，则仍需显式 `copy(...)`，且因为 `const` 不能作为 move 源，不能写成 `move constValue`。模块级 `const` 只有在完成冻结与发布后才属于 module readonly；模块级 `var` 属于 module mutable，并不因“全局可见”而自动线程安全。

```xray
var local = 0
go { local += 1 }                        // ❌ 编译错误：不能捕获可变局部变量
```

#### 正确姿势

```xray
// 方法 1：显式复制 owned graph
var arr = [1, 2, 3]
var t = go fn(data: Array<int>) -> int {
    data.push(4)            // 拷贝上修改，不影响原值
    return len(data)
}(copy(arr))
print(arr)                  // [1, 2, 3] 未变

// 方法 2：shared 零拷贝只读（可被捕获）
shared config = { rate: 100 }
var t2 = go fn(c: Json) -> int {
    return c.rate
}(config)

// 方法 3：move 转移所有权
var big = Array<byte>(1024)
var t3 = go fn(b: Array<byte>) -> int {
    return process(b)
}(move big)
// big 在此处不可访问

// 方法 4：Channel 通信（可被捕获）
shared ch = Channel<int>(10)
var t4 = go fn(c: Channel<int>) -> int {
    return match (c.recv()) {
        Recv.Value(v) -> v
        _ -> 0
    }
}(ch)
ch.send(42)
```

### 7.5 GC 与对象生命周期

Xray 采用多层内存管理：

| 存储 | 机制 | 释放时机 |
|--|--|--|
| 模块只读存储（顶层 `const`） | consteval rodata，或 module allocator 初始化后 freeze + publish | 模块卸载 |
| 模块可变存储（顶层 `var`） | module owner；默认不具备并发安全性 | 模块卸载 |
| 共享存储（`shared`） | shared/system owner + refcount | 最后共享引用释放 |
| execution-local heap（一般局部对象） | execution owner + 引用计数 + 循环引用回收 | execution 结束或最后引用释放；强引用环由 cycle collector 回收 |
| 栈（`struct` 值、本地） | RAII | 作用域退出 |
| Arena（底层临时分配） | 批量释放 | arena 结束 |

**内存观察点**：
- 默认以引用计数立即释放对象。
- 强引用环由 cycle collector 在安全点或显式 `runtime.collectCycles()` 时处理。
- 指令列表中成为内存安全点的点包括函数调用、后向跳转、显式 `runtime.collectCycles()`。

循环引用回收与堆布局设计：见 `src/runtime/mem/`。
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 7. Scoping and Name Resolution

> Source of truth: `src/frontend/analyzer/xanalyzer_scope.c`, `src/frontend/analyzer/xanalyzer_capture.c`.

### 7.1 Lexical Scoping and Hoisting

Xray uses **lexical scoping**: a name's visibility is determined entirely by the source code structure.

**Scope kinds**:

| Scope | Triggered by | Example |
|--|--|--|
| Module | Each `.xr` file | top-level `var` `const` `shared` `fn` `class` |
| Function / closure | Entering `fn` / arrow function | parameters + function body |
| Block | `{...}` | `if` `while` `for` `match` arm body |
| `scope` block | `scope { ... }` keyword | explicit lexical scope + structured concurrency (see §10.7) |
| `for` header | `for (var i=0; ...)` | `i` is visible only within the loop body |
| `catch` parameter | `catch (e)` | `e` is visible only within the catch body |
| Class body | `class` definition | fields, methods |

**Hoisting rules**:

- Top-level `fn` `class` `struct` `interface` `enum` `type` are **hoisted** to the top of the current scope — they may be referenced before their textual definition.
- `var` / `const` / `shared` are **not hoisted** — they must appear before any use.
- Duplicate names: declaring two same-named variables in the same scope is a compile error (nested scopes may shadow).

```xray
main()                    // OK: uses the hoisted fn
fn main() { ... }

var y = x                 // error: x is not declared
var x = 10
```

#### Shadow rules

A nested block may shadow a same-named variable in an outer scope:

```xray
var x = 1
{
    var x = "hello"           // shadow: OK
    print(x)                 // "hello"
}
print(x)                     // 1
```

### 7.2 Closure Capture Semantics

A closure captures variables from outer scopes as **upvalues**.

#### Plain synchronous closures

The default capture mode is **by reference**:

```xray
fn make_counter() -> (() -> int) {
    var count = 0
    return fn() -> int {
        count += 1                  // mutates the outer count
        return count
    }
}

var c = make_counter()
print(c())      // 1
print(c())      // 2
```

- The closure and the original variable **share state**.
- After the outer scope exits, variables referenced by the closure are kept alive by the GC (promoted to the heap).

#### Closure optimization

The compiler analyzes upvalues:
- read-only → may be implicitly copied (avoiding closure conversion).
- read/write → promoted to a closure box.
- See §17.5 for details.

### 7.3 Ownership and `move`

Xray is **not** a full ownership/borrow-checked language (unlike Rust). However, **cross-coroutine data transfer** uses move semantics:

```xray
var big_buffer = Array<byte>(1024 * 1024)

var t = go fn(b: Array<byte>) -> int {
    return process(b)
}(big_buffer)             // compile error: owned heap value cannot cross bare

var t2 = go fn(b: Array<byte>) -> int {
    return process(b)
}(move big_buffer)        // OK: ownership transferred

print(len(big_buffer))    // compile error: accessed after move
```

**`move` usage**: `move` appears as an **argument prefix** at call sites (see §10.8):

- `go f(move x)`, `go fn(...){...}(move x)`: transfer ownership to the coroutine.
- `ch.send(move data)`: transfer ownership when sending across coroutines (avoiding a copy).
- Plain function call `f(move x)`: transfer ownership into the function (which becomes the sole owner).

### 7.4 Cross-Coroutine Data Transfer Rules (Race Avoidance)

"Statically eliminating data races at compile time" is a core design principle of xray's concurrency model.

Every cross-execution boundary (`go` closure, `go` argument, Channel send, deferred task, thread entry, and exported callback) consumes the same verified capture plan. Legality is determined jointly by the value's **storage owner, provenance, mutability, and type representation**, never by the `var` / `const` keyword alone:

| Capture action | Values | Boundary behavior |
|---|---|---|
| inline value | scalars and small immutable values | copy the bits directly |
| deep copy | owned graph under explicit `copy(x)` | materialize an independent graph in the destination execution owner |
| move | transferable local `var` under explicit `move x` | transfer ownership and statically invalidate the source binding |
| module readonly | frozen and published module values | retain the module-readonly owner; do not copy into a root/task heap |
| shared ref | `shared`, Channel, Task, Atomic, and other stable shared identities | retain a reference owned by the shared/system owner |
| reject | execution-local graphs, mutable module state, dangling slices/pointers/upvalues | compile error reporting the owner/provenance and required explicit action |

Consequently, a local `const` containing only inline values may cross directly. If it still points at an execution-local graph, it requires explicit `copy(...)`; because a `const` cannot be a move source, `move constValue` is invalid. A module-level `const` becomes module-readonly only after freeze-and-publish. A module-level `var` is module-mutable and is not made thread-safe merely by being globally visible.

```xray
var local = 0
go { local += 1 }                        // ❌ compile error: cannot capture mutable local
```

#### Recommended patterns

```xray
// Pattern 1: explicitly copy an owned graph
var arr = [1, 2, 3]
var t = go fn(data: Array<int>) -> int {
    data.push(4)            // mutates the copy, original is unaffected
    return len(data)
}(copy(arr))
print(arr)                  // [1, 2, 3] unchanged

// Pattern 2: shared, zero-copy read-only (capturable)
shared config = { rate: 100 }
var t2 = go fn(c: Json) -> int {
    return c.rate
}(config)

// Pattern 3: move ownership
var big = Array<byte>(1024)
var t3 = go fn(b: Array<byte>) -> int {
    return process(b)
}(move big)
// big is inaccessible from this point

// Pattern 4: Channel communication (capturable)
shared ch = Channel<int>(10)
var t4 = go fn(c: Channel<int>) -> int {
    return match (c.recv()) {
        Recv.Value(v) -> v
        _ -> 0
    }
}(ch)
ch.send(42)
```

### 7.5 GC and Object Lifetimes

Xray uses a layered memory management strategy:

| Storage | Mechanism | Reclamation |
|--|--|--|
| Module-readonly storage (top-level `const`) | consteval rodata, or module allocator followed by freeze + publish | at module unload |
| Module-mutable storage (top-level `var`) | module owner; not concurrency-safe by default | at module unload |
| Shared storage (`shared`) | shared/system owner + reference counting | when the last shared reference is released |
| Execution-local heap (ordinary local objects) | execution owner + reference counting + cycle collection | when the execution ends or the last reference is released; strong cycles are reclaimed by the cycle collector |
| Stack (`struct` values, locals) | RAII | when scope exits |
| Arena (low-level temporary allocations) | bulk free | at arena end |

**Memory observation points**:
- Default reclamation is reference-counted.
- Strong reference cycles are handled by the cycle collector at safepoints or explicit `runtime.collectCycles()`.
- Memory safepoints in the instruction stream include function calls, backward branches, and explicit `runtime.collectCycles()`.

Cycle collection and heap-layout design: see `src/runtime/mem/`.
<!-- /xr-spec:en -->

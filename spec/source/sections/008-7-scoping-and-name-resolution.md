---
id: spec.7_scoping_and_name_resolution
order: 008
---

<!-- xr-spec:cn -->
---

## 7. 作用域与名字解析 (Scoping)

> 真值源：`src/frontend/analyzer/xanalyzer_symbol.c`、`xanalyzer_escape.c`、`xaddressability.c`、`xanalyzer_visitor_stmt.c` 与 `src/analysis/xglobal_producer.c` 的 capture/storage plan。

### 7.1 词法作用域与提升

Xray 采用**词法作用域**：名字的可见性由源代码结构决定。

**作用域类型**：

| 作用域 | 触发 | 示例 |
|--|--|--|
| 模块 | 每个 `.xr` 文件 | 顶层 `var` `const` `fn` `class` |
| 函数 / 闭包 | `fn` / 箭头函数进入 | 参数 + 函数体 |
| 块 | `{...}` | `if` `while` `for` `match` 分支体 |
| `scope` 块 | `scope { ... }` 关键字 | 显式词法作用域 + 结构化并发（见 §10.7） |
| `for` 头 | `for (var i=0; ...)` | `i` 仅循环体可见 |
| `catch` 参数 | `catch (e)` | `e` 仅 catch 体可见 |
| 类体 | `class` 定义 | 字段、方法 |

**提升规则**：

- 顶层 `fn` `class` `struct` `interface` `enum` `type` **提升**至当前作用域顶部——可在定义前引用。
- `var` / `const` **不提升**——必须在定义后使用。
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
fn make_counter() -> (() -> i64) {
    var count = 0
    return fn() -> i64 {
        count += 1                  // 修改外层 count
        return count
    }
}

var c = make_counter()
print(c())      // 1
print(c())      // 2
```

- 闭包与原变量**共享**。
- 外层作用域退出后，被捕获变量由闭包 cell / upvalue 与相应引用计数继续保活。
- 每个被读写捕获的变量只有一个共享 cell。外层赋值与所有闭包读写都访问同一 cell；
  cell 及其中的新值按普通强引用计数转移和释放，不存在额外的隐藏保活规则。

#### 闭包优化

编译器会分析 upvalue：
- 仅读 → 可能隐式复制（避免闭包转换）。
- 读写 → 提升为闭包 box。
- 详见 §17.5。

### 7.3 所有权与 move

Xray **不**是全面 ownership/borrow checker 语言（不像 Rust）。但在**跨协程数据传递**中使用 move 语义：

```xray
var big_buffer = Array<u8>(1024 * 1024)

var t = go fn(b: Array<u8>) -> i64 {
    return process(b)
}(big_buffer)             // 编译错误：execution-local heap 值不能裸跨协程传递

var t2 = go fn(b: Array<u8>) -> i64 {
    return process(b)
}(move big_buffer)        // OK：所有权转移

print(len(big_buffer))    // 编译错误：move 后访问
```

**move 使用场景**：`move` 是一元所有权转移表达式，常见于实参与初始化器（参见 §5.1.4 / §10.8）：

- `go f(move x)`、`go fn(...){...}(move x)`：把所有权转给协程。
- `ch.send(move data)`：跨协程发送时转移所有权（避免拷贝）。
- 普通函数调用 `f(move x)`：把所有权传入函数（被调函数独占）。
- `var dst = move src` / `const dst = move src`：把 verifier 证明为唯一的局部 `var` 根转给新的 owner 或 const 能力。
- 函数返回统一写 `return value`。返回边终结当前 continuation，编译器对 unique local 或 `move` 参数自动发布 owner-forward proof；`return move value` 不属于公开写法。

### 7.4 协程数据传递规则（避免数据竞争）

"保证编译期消除数据竞争"是 xray 并发模型的核心设计原则。

所有跨 execution 边界（`go` 闭包、`go` 实参、Channel send、deferred task、线程入口和导出 callback）消费同一个已验证 capture plan。合法性由值的 **storage owner、provenance、可变性与类型表示**共同决定，不由 `var` / `const` 关键字单独决定：

| Capture action | 适用值 | 边界行为 |
|---|---|---|
| inline value | 显式 go 实参中的标量、不可变小值 | 直接复制位表示 |
| deep copy | 显式 `copy(x)` 的 execution-local graph | 在目标 storage domain 中物化独立图 |
| move | 显式 `move x` 的推断唯一局部 `var` | 转移所有权并静态废弃源绑定 |
| module readonly | 已 seal 并发布的模块只读值 | 保留模块只读 owner，不复制到 root/task heap |
| synchronized ref | Channel、Task、Atomic 等受审计稳定身份 | 保留 verified synchronized domain 的引用 |
| reject | execution-local graph、可变 module state、悬垂 slice/pointer/upvalue | 编译错误并报告 owner/provenance 与所需显式动作 |

因此，局部 `const` 若只含 inline 值可以直接跨界；managed/aggregate const 图只有在 StoragePlan 证明其直接构造或 O(1) seal 可发布时才能跨界，不存在边界隐式复制。模块级 `const` 只有在完成 seal 与发布后才属于 module readonly；模块级 `var` 属于 module mutable，并不因“全局可见”而自动线程安全。

`go` 闭包还有一条更强、易记的表面规则：**不得捕获任何外层 `var`，无论闭包只读还是写入，也无论当前程序只启动一个还是多个协程。** 需要的数据必须作为 `go` 的显式实参传入，并在边界写清 `copy(...)`、`move` 或同步共享能力。多个协程共享可变状态只能通过 `Channel`、`Atomic`、`sync` 锁/句柄等受审计并发原语；直接捕获修改普通 `var` 是编译错误，`unsafe` 也不能放宽。

```xray
var local = 0
go fn() { local += 1 }()                 // ❌ 编译错误：不能捕获可变局部变量
```

#### 正确姿势

```xray
// 方法 1：显式复制 execution-local graph
var arr = [1, 2, 3]
var t = go fn(data: Array<i64>) -> i64 {
    data.push(4)            // 拷贝上修改，不影响原值
    return len(data)
}(copy(arr))
print(arr)                  // [1, 2, 3] 未变

// 方法 2：const 零拷贝只读（可被捕获）
const config = { rate: 100 }
var t2 = go fn(c: JSON.Object) -> i64 {
    return c.rate
}(config)

// 方法 3：move 转移所有权
var big = Array<u8>(1024)
var t3 = go fn(b: Array<u8>) -> i64 {
    return process(b)
}(move big)
// big 在此处不可访问

// 方法 4：Channel 通信（可被捕获）
const ch = Channel<i64>(10)
var t4 = go fn(c: Channel<i64>) -> i64 {
    return match (c.recv()) {
        Recv.Value { value: v } -> v
        _ -> 0
    }
}(ch)
ch.send(42)
```

### 7.5 内存管理与对象生命周期

Xray 采用多层内存管理：

| 存储 | 机制 | 释放时机 |
|--|--|--|
| 模块只读存储（顶层 `const`） | consteval rodata，或 module allocator 初始化后 seal + publish | 模块卸载 |
| 模块可变存储（顶层 `var`） | module owner；默认不具备并发安全性 | 模块卸载 |
| const/sync 共享域 | verified const root 或同步句柄的 root-only 原子引用计数 | 最后跨执行强引用释放 |
| execution-local 回收域（一般局部对象） | 编译器插入的引用计数 + VM coroutine heap / AOT execution arena | 最后强引用释放时立即回收；引用环不被收集，随 physical coroutine 结束与域内残余图一起批量处置（§16.8） |
| 栈（`struct` 值、本地） | 词法存储期 | 作用域退出；语言没有用户可见的确定性析构 / `Drop` hook |
| Arena（底层临时分配） | 批量释放 | arena 结束 |

**内存观察点**：
- 普通局部对象由编译器插入 retain/drop；最后一个强引用释放时进入 RC 销毁路径。
- 编译器只把可能形成引用环的类型标为 cycle candidate；该标记服务于诊断，不驱动任何运行时回收动作。
- 引用环不由运行时收集：静态证明（L0）、`weak` 显式断环（L1）、执行局部回收域批量处置封顶（L2）。
- 运行时不存在任何环回收机制——既不是并发 tracing GC，也不是 cycle collector。开发构建可开启环检测器，它只观察和报告，不改变堆。

<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 7. Scoping and Name Resolution

> Source of truth: `src/frontend/analyzer/xanalyzer_symbol.c`, `xanalyzer_escape.c`, `xaddressability.c`, `xanalyzer_visitor_stmt.c`, and the capture/storage plans produced by `src/analysis/xglobal_producer.c`.

### 7.1 Lexical Scoping and Hoisting

Xray uses **lexical scoping**: a name's visibility is determined entirely by the source code structure.

**Scope kinds**:

| Scope | Triggered by | Example |
|--|--|--|
| Module | Each `.xr` file | top-level `var` `const` `fn` `class` |
| Function / closure | Entering `fn` / arrow function | parameters + function body |
| Block | `{...}` | `if` `while` `for` `match` arm body |
| `scope` block | `scope { ... }` keyword | explicit lexical scope + structured concurrency (see §10.7) |
| `for` header | `for (var i=0; ...)` | `i` is visible only within the loop body |
| `catch` parameter | `catch (e)` | `e` is visible only within the catch body |
| Class body | `class` definition | fields, methods |

**Hoisting rules**:

- Top-level `fn` `class` `struct` `interface` `enum` `type` are **hoisted** to the top of the current scope — they may be referenced before their textual definition.
- `var` / `const` are **not hoisted** — they must appear before any use.
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
fn make_counter() -> (() -> i64) {
    var count = 0
    return fn() -> i64 {
        count += 1                  // mutates the outer count
        return count
    }
}

var c = make_counter()
print(c())      // 1
print(c())      // 2
```

- The closure and the original variable **share state**.
- After the outer scope exits, a captured variable remains alive through its closure cell/upvalue and the corresponding reference counts.
- Every read/write capture has exactly one shared cell. Outer assignments and
  every capturing closure access that same cell; the cell and each newly stored
  value follow ordinary strong-reference transfers and releases, with no hidden
  keepalive rule.

#### Closure optimization

The compiler analyzes upvalues:
- read-only → may be implicitly copied (avoiding closure conversion).
- read/write → promoted to a closure box.
- See §17.5 for details.

### 7.3 Ownership and `move`

Xray is **not** a full ownership/borrow-checked language (unlike Rust). However, **cross-coroutine data transfer** uses move semantics:

```xray
var big_buffer = Array<u8>(1024 * 1024)

var t = go fn(b: Array<u8>) -> i64 {
    return process(b)
}(big_buffer)             // compile error: an execution-local heap value cannot cross bare

var t2 = go fn(b: Array<u8>) -> i64 {
    return process(b)
}(move big_buffer)        // OK: ownership transferred

print(len(big_buffer))    // compile error: accessed after move
```

**`move` usage**: `move` is a unary ownership-transfer expression commonly used in arguments and initializers (see §5.1.4 / §10.8):

- `go f(move x)`, `go fn(...){...}(move x)`: transfer ownership to the coroutine.
- `ch.send(move data)`: transfer ownership when sending across coroutines (avoiding a copy).
- Plain function call `f(move x)`: transfer ownership into the function (which becomes the sole owner).
- `var dst = move src` / `const dst = move src`: transfer a verifier-proven unique local `var` root to a new owner or const capability.
- Function returns are always written `return value`. The return edge terminates the current continuation, so the compiler automatically publishes owner-forward proof for a unique local or `move` parameter; `return move value` is not a public form.

### 7.4 Cross-Coroutine Data Transfer Rules (Race Avoidance)

"Statically eliminating data races at compile time" is a core design principle of xray's concurrency model.

Every cross-execution boundary (`go` closure, `go` argument, Channel send, deferred task, thread entry, and exported callback) consumes the same verified capture plan. Legality is determined jointly by the value's **storage owner, provenance, mutability, and type representation**, never by the `var` / `const` keyword alone:

| Capture action | Values | Boundary behavior |
|---|---|---|
| inline value | scalars and small immutable values passed as explicit `go` arguments | copy the bits directly |
| deep copy | execution-local graph under explicit `copy(x)` | materialize an independent graph in the destination storage domain |
| move | inferred-unique local `var` under explicit `move x` | transfer ownership and statically invalidate the source binding |
| module readonly | sealed and published module values | retain the module-readonly owner; do not copy into a root/task heap |
| synchronized ref | audited stable identities such as Channel, Task, and Atomic | retain a reference in the verified synchronized domain |
| reject | execution-local graphs, mutable module state, dangling slices/pointers/upvalues | compile error reporting the owner/provenance and required explicit action |

Consequently, a local `const` containing only inline values may cross directly. A managed/aggregate const graph may cross only when StoragePlan proves direct construction or O(1) publication seal; the boundary never copies implicitly. A module-level `const` becomes module-readonly only after seal-and-publish. A module-level `var` is module-mutable and is not made thread-safe merely by being globally visible.

`go` closures additionally follow one simple, stronger surface rule: **they may not capture any outer `var`, whether they only read it or mutate it, and whether the current program launches one coroutine or many.** Pass data as explicit `go` arguments and state `copy(...)`, `move`, or an audited synchronization capability at the boundary. Mutable state shared by multiple coroutines must flow through `Channel`, `Atomic`, or audited `sync` locks/handles. Directly capturing and modifying an ordinary `var` is a compile error, and `unsafe` does not relax this rule.

```xray
var local = 0
go fn() { local += 1 }()                 // ❌ compile error: cannot capture mutable local
```

#### Recommended patterns

```xray
// Pattern 1: explicitly copy an execution-local graph
var arr = [1, 2, 3]
var t = go fn(data: Array<i64>) -> i64 {
    data.push(4)            // mutates the copy, original is unaffected
    return len(data)
}(copy(arr))
print(arr)                  // [1, 2, 3] unchanged

// Pattern 2: const, zero-copy read-only (capturable)
const config = { rate: 100 }
var t2 = go fn(c: JSON.Object) -> i64 {
    return c.rate
}(config)

// Pattern 3: move ownership
var big = Array<u8>(1024)
var t3 = go fn(b: Array<u8>) -> i64 {
    return process(b)
}(move big)
// big is inaccessible from this point

// Pattern 4: Channel communication (capturable)
const ch = Channel<i64>(10)
var t4 = go fn(c: Channel<i64>) -> i64 {
    return match (c.recv()) {
        Recv.Value { value: v } -> v
        _ -> 0
    }
}(ch)
ch.send(42)
```

### 7.5 Memory Management and Object Lifetimes

Xray uses a layered memory management strategy:

| Storage | Mechanism | Reclamation |
|--|--|--|
| Module-readonly storage (top-level `const`) | consteval rodata, or module allocator followed by seal + publish | at module unload |
| Module-mutable storage (top-level `var`) | module owner; not concurrency-safe by default | at module unload |
| Const/synchronized shared domain | verified const root or synchronized handle with root-only atomic reference counting | when the last cross-execution strong reference is released |
| Execution-local reclamation domain (ordinary local objects) | compiler-inserted reference counting plus a VM coroutine heap or AOT execution arena | immediately when the last strong reference is released; a reference cycle is not collected and the residual graph is disposed in bulk when the physical coroutine ends (§16.8) |
| Stack (`struct` values, locals) | lexical storage duration | scope exit; the language exposes no deterministic destructor / `Drop` hook |
| Arena (low-level temporary allocations) | bulk free | at arena end |

**Memory observation points**:
- The compiler inserts retain/drop operations for ordinary local objects; releasing the last strong reference enters the RC destruction path.
- The compiler marks only types that may form reference cycles as cycle candidates; the mark serves diagnostics and drives no runtime reclamation.
- Reference cycles are not collected at runtime: they are prevented statically (L0), broken explicitly with `weak` (L1), and bounded by bulk disposal of the execution-local reclamation domain (L2).
- No cycle-reclaiming mechanism exists at runtime — neither a concurrent tracing GC nor a cycle collector. A development build can enable a cycle detector, which only observes and reports; it never mutates the heap.

<!-- /xr-spec:en -->

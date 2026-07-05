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
var big_buffer = Bytes(1024 * 1024)

var t = go fn(b: Bytes) -> int {
    return process(b)
}(big_buffer)             // 编译错误：owned heap 值不能裸跨协程传递

var t2 = go fn(b: Bytes) -> int {
    return process(b)
}(move big_buffer)        // OK：所有权转移

print(big_buffer.length)  // 编译错误：move 后访问
```

**move 使用场景**：`move` 作为**实参前缀**出现在调用位置（参见 §10.8）：

- `go f(move x)`、`go fn(...){...}(move x)`：把所有权转给协程。
- `ch.send(move data)`：跨协程发送时转移所有权（避免拷贝）。
- 普通函数调用 `f(move x)`：把所有权传入函数（被调函数独占）。

### 7.4 协程数据传递规则（避免数据竞争）

"保证编译期消除数据竞争"是 xray 并发模型的核心设计原则。

`go` 启动的协程**不能直接捕获**外层作用域的可变变量；数据必须通过**参数传递**进入协程。普通局部引用值必须显式 `copy(...)` 或 `move`；`shared` 绑定是稳定共享身份，可直接跨协程使用：

| 变量种类 | 跨协程传递规则 |
|---|---|
| 普通 `var` / `const`（局部） | 引用型 owned heap 值作为实参时必须显式 `copy(...)` 或 `move`；不能被闭包捕获修改 |
| 函数参数 | ✅ 完全自由（已经是拷贝 / move 进来的） |
| `shared` | ✅ 可直接跨协程传递/捕获；绑定不可重新赋值，也不能 `move` |
| `Channel<T>` | ✅ 可被闭包捕获（生命周期由 channel 自身管理） |
| `this` / 闭包 upvalue（可变） | ❌ 不能跨协程；必须通过参数显式传递 |
| 全局 `import` 的函数/类 | ✅ 不可变定义，可自由引用 |

```xray
var local = 0
go { local += 1 }                        // ❌ 编译错误：不能捕获可变局部变量
```

#### 正确姿势

```xray
// 方法 1：作为参数传值（普通变量自动深拷贝）
var arr = [1, 2, 3]
var t = go fn(data: Array<int>) -> int {
    data.push(4)            // 拷贝上修改，不影响原值
    return data.length
}(arr)
print(arr)                  // [1, 2, 3] 未变

// 方法 2：shared 零拷贝只读（可被捕获）
shared config = { rate: 100 }
var t2 = go fn(c: Json) -> int {
    return c.rate
}(config)

// 方法 3：move 转移所有权
shared big = Bytes(1024)
var t3 = go fn(b: Bytes) -> int {
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
| 全局堆（`shared`） | refcount | refcount 变 0 |
| 局部堆（一般对象） | 引用计数 + 循环引用回收 | 最后引用释放；强引用环由 cycle collector 回收 |
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
var big_buffer = Bytes(1024 * 1024)

var t = go fn(b: Bytes) -> int {
    return process(b)
}(big_buffer)             // compile error: owned heap value cannot cross bare

var t2 = go fn(b: Bytes) -> int {
    return process(b)
}(move big_buffer)        // OK: ownership transferred

print(big_buffer.length)  // compile error: accessed after move
```

**`move` usage**: `move` appears as an **argument prefix** at call sites (see §10.8):

- `go f(move x)`, `go fn(...){...}(move x)`: transfer ownership to the coroutine.
- `ch.send(move data)`: transfer ownership when sending across coroutines (avoiding a copy).
- Plain function call `f(move x)`: transfer ownership into the function (which becomes the sole owner).

### 7.4 Cross-Coroutine Data Transfer Rules (Race Avoidance)

"Statically eliminating data races at compile time" is a core design principle of xray's concurrency model.

A coroutine launched by `go` **cannot directly capture** mutable variables from the outer scope; data must enter the coroutine through **parameter passing**. Plain local reference values must use explicit `copy(...)` or `move`; `shared` bindings are stable shared identities and may cross coroutine boundaries directly:

| Variable kind | Cross-coroutine transfer rule |
|---|---|
| Plain `var` / `const` (local) | Owned heap values must use explicit `copy(...)` or `move` when passed as arguments; cannot be captured and mutated by closures |
| Function parameters | ✅ Fully free (already copied / moved in) |
| `shared` | ✅ May be passed/captured across coroutines directly; the binding cannot be reassigned or moved |
| `Channel<T>` | ✅ May be captured by closures (lifetime managed by the channel itself) |
| `this` / mutable closure upvalues | ❌ Cannot cross coroutines; must be passed explicitly through parameters |
| Globally imported functions/classes | ✅ Immutable definitions, freely referenceable |

```xray
var local = 0
go { local += 1 }                        // ❌ compile error: cannot capture mutable local
```

#### Recommended patterns

```xray
// Pattern 1: pass by value (plain variables are deep-copied)
var arr = [1, 2, 3]
var t = go fn(data: Array<int>) -> int {
    data.push(4)            // mutates the copy, original is unaffected
    return data.length
}(arr)
print(arr)                  // [1, 2, 3] unchanged

// Pattern 2: shared, zero-copy read-only (capturable)
shared config = { rate: 100 }
var t2 = go fn(c: Json) -> int {
    return c.rate
}(config)

// Pattern 3: move ownership
shared big = Bytes(1024)
var t3 = go fn(b: Bytes) -> int {
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
| Global heap (`shared`) | refcount | when refcount reaches 0 |
| Local heap (general objects) | reference counting + cycle collection | when the last reference is released; strong cycles are reclaimed by the cycle collector |
| Stack (`struct` values, locals) | RAII | when scope exits |
| Arena (low-level temporary allocations) | bulk free | at arena end |

**Memory observation points**:
- Default reclamation is reference-counted.
- Strong reference cycles are handled by the cycle collector at safepoints or explicit `runtime.collectCycles()`.
- Memory safepoints in the instruction stream include function calls, backward branches, and explicit `runtime.collectCycles()`.

Cycle collection and heap-layout design: see `src/runtime/mem/`.
<!-- /xr-spec:en -->

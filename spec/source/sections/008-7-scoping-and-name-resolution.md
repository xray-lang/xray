---
id: spec.7_scoping_and_name_resolution
order: 008
---

<!-- xr-spec:cn -->
---

## 7. 作用域与名字解析 (Scoping)

> 真值源：`src/xir/xxir_source.c` 的名字解析、闭包捕获与 `go` 调用检查。

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
| `catch` 参数 | `catch (e)`、`catch (E.V { x })` | `e` 与 pattern 绑定 `x` 仅 catch 体可见 |
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

可变 `var` 按 **共享 cell 引用捕获**，`const` 按值捕获：

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

#### XIR 源码闭包准入

普通闭包按绑定类别捕获：局部 `const` 与可复制、非视图的普通 read 参数保存逻辑副本；局部 `var` 保持一个共享 cell。仅按实际词法使用收集自由变量，重复使用同一绑定只捕获一次；shadow、初始化前不可见以及for头作用域遵循普通名字解析。嵌套闭包需要的自由变量逐层传递。模块名字仍经原有模块状态与权限规则访问，不作为局部环境快照。

XIR源码入口准入参数显式标注类型的 `fn(...) -> R { ... }` 与箭头闭包。没有返回标注时，在定义处从返回表达式推导唯一精确类型；所有返回必须一致，非unit结果必须覆盖所有路径。不从调用目标或一次实例化推断参数、约束或效应。闭包不能声明自己的一等泛型参数；外围T及其已冻结约束进入私有实现函数，在Checked上与捕获类型一起特化、复验并封存。

此族支持不可变按值捕获与普通var共享cell捕获；未使用的外层绑定不进入环境。共享cell只允许单实例内访问，不授予Sendable或跨worker能力；强环回收仍按§17.19保持OPEN。ref/move/视图捕获、noescape及Sendable/效应承诺仍按尚未准入的合同拒绝，不将它们降成普通复制或静默添加承诺。源码生成的函数和捕获仅进入§17.18的唯一XIR/环境路径。

### 7.3 所有权与 move

`move x` 把绑定持有的值交给新的所有者，保证不复制；源绑定随即失效，重新初始化前再使用是编译错误。可用作源的绑定见 §2.14.5。跨任务边界并不要求 `move`：可复制且 Sendable 的值跨任务时本来就保留逻辑副本（§7.4、§10.2），`move` 只省去复制。

绑定在每条路径上的最后一次使用都落在取得持有权的位置时，这次使用是隐式移动，结果与显式写 `move` 相同（§2.14.5、§16.8）。

```xray
fn total(xs: Array<i64>) -> i64 {
    var sum = 0
    for (x in xs) { sum += x }
    return sum
}

fn consume(xs: move Array<i64>) -> i64 {
    xs.push(0)
    return len(xs)
}

fn main() {
    var data = [1, 2, 3]
    var t1 = go total(data)         // 子任务得到逻辑副本，data 仍可使用
    print(len(data))
    var t2 = go consume(move data)  // 交出，不复制；此后不能再读 data
}

main()
```

**move 使用场景**：`move` 是一元的转移表达式，常见于实参与初始化器（参见 §5.1.4 / §10.8）：

- `go f(move x)`：把值交给新任务的 `move` 参数。
- `ch.send(move data)`：发送时交出，不复制。
- 普通函数调用 `f(move x)`：传给 `move` 参数，被调函数取得持有权。
- `var dst = move src` / `const dst = move src`：交给新的绑定。
- 函数返回统一写 `return value`；返回本身就交出值，`return move value` 不属于公开写法。

### 7.4 协程数据传递规则（避免数据竞争）

"保证编译期消除数据竞争"是 xray 并发模型的核心设计原则；这条保证的精确形式与边界见 §16.9.5。

所有跨任务边界——`go` 的实参与捕获、Channel 发送（含 `select` 的发送分支）、Task 与 `scope` 的结果——遵守同一组规则，合法性由值的完整类型决定，不由 `var` / `const` 关键字决定：

| 值 | 跨任务时 |
|---|---|
| 可复制且 Sendable 的值：标量、string，元素为 Sendable 的 Array / Map / Set，以及由它们组成的 struct、enum、tuple | 保留逻辑副本：两侧此后各自修改，互不影响；不要求写 `copy` 或 `move` |
| Sendable class 与同步句柄（Channel、Task、Atomic、`sync` 的锁与句柄） | 共享身份：两侧引用同一个对象，同步由对象自身保证 |
| 不可复制且 Sendable 的资源 | 只能按消费参数交出 |
| 非 Sendable 的值：普通 class、`ref` 借用、`Slice` / `MutSlice`、Cell | 编译错误 |

`move x` 保证不复制，但不是跨界的前提（§10.8）。`const` 不证明 Sendable；模块级 `var` 也不因全局可见而线程安全。

`go` 闭包还有一条更强、易记的表面规则：**不得捕获任何外层 `var`，无论闭包只读还是写入，也无论当前程序只启动一个还是多个协程。** 需要的数据作为 `go` 的显式实参传入。多个协程共享可变状态只能经 `Channel`、`Atomic`、`sync` 的锁与句柄等受审计并发原语；直接捕获修改普通 `var` 是编译错误，`unsafe` 也不能放宽。

```xray
var local = 0
go fn() { local += 1 }()                 // ❌ 编译错误：不能捕获可变局部变量
```

#### 正确姿势

```xray
fn grow(data: Array<i64>) -> i64 {
    var mine = data             // 保存逻辑副本后修改，不影响调用方
    mine.push(4)
    return len(mine)
}

fn fill(buf: move Array<u8>) -> i64 {
    buf.push(1)
    return len(buf)
}

fn receive(ch: Channel<i64>) -> i64 {
    return ch.recvOr(0)
}

fn main() {
    // 方法 1：按值传入，子任务得到逻辑副本
    var arr = [1, 2, 3]
    var t1 = go grow(arr)
    print(len(arr))             // 3：调用方的 arr 未变

    // 方法 2：move 交出，不复制
    var big = Array<u8>(1024)
    var t2 = go fill(move big)

    // 方法 3：经 Channel 传值，Channel 本身共享身份
    const ch = Channel<i64>(10)
    var t3 = go receive(ch)
    ch.send(42)
}

main()
```

### 7.5 内存管理与对象生命周期

Xray 采用多层内存管理：

| 存储 | 机制 | 释放时机 |
|--|--|--|
| 模块 `const` | consteval 只读数据，或初始化后发布（§16.9.2 的边 7） | 模块卸载 |
| 模块 `var` | 归属模块；默认不具备并发安全性 | 模块卸载 |
| 本地带（一般对象） | 编译器插入的非原子引用计数；所在任务的回收域 | 最后一个强引用释放时立即回收；引用环在任务结束时由回收域断开（§16.3） |
| 共享带（可被多个任务到达的对象） | 原子引用计数；在提升点由发布方提升（§16.3.1） | 最后一个强引用释放时立即回收 |
| 栈（`struct` 值、本地） | 词法存储期 | 作用域退出；语言没有用户可见的确定性析构 / `Drop` hook |
| Arena（底层临时分配） | 批量释放 | arena 结束 |

**内存观察点**：
- 普通对象由编译器插入 retain/drop；最后一个强引用释放时进入 RC 销毁路径。局部绑定何时释放它持有的引用见 §16.8。
- 编译器只把可能形成引用环的类型标为 cycle candidate；该标记决定任务回收域登记哪些身份对象（§16.3 的 L2），不驱动任何环收集。
- 引用环不由运行时收集：静态证明（L0）、`weak` 显式断环（L1）、任务回收域在任务结束时断开剩余的环（L2）。
- 运行时不存在任何环回收机制——既不是并发 tracing GC，也不是 cycle collector。开发构建可开启环检测器，它只观察和报告，不改变堆。


READ结构体方法值 `receiver.method` 在绑定时对receiver求值一次，保存其逻辑值快照，不执行方法体。所得函数签名只包含显式参数与返回类型，结构体泛型实参先替换；原绑定后续写入不改变快照，身份句柄仍保持其身份语义。私有/protected访问在绑定处检查，声明内部可显式返回已授权的函数值，调用者不会因此取得直接成员访问权。函数值仍受所属Instance的撤销与清理约束，不隐含Sendable/no_suspend承诺。ref/move接收者不得退化为READ绑定。

<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 7. Scoping and Name Resolution

> Source of truth: name resolution, closure capture and `go` call checking in `src/xir/xxir_source.c`.

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
| `catch` parameter | `catch (e)`, `catch (E.V { x })` | `e` and pattern binding `x` are visible only within the catch body |
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

Ordinary `var` captures retain a shared cell; copyable non-view `const` and ordinary read parameters are captured by logical value copy:

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

#### XIR source closure admission

Ordinary closures capture according to the binding: local const and ordinary read parameters of copyable, non-view types produce logical value copies; local var retains one shared cell. Only lexically used free bindings are collected, once per binding. Shadowing, invisibility before initialization and for-header scope obey ordinary name resolution. Free variables needed by nested closures pass through each enclosing closure. Module names continue to use module state and authority rules rather than local environment snapshots.

The XIR source producer admits fn expressions and arrow closures with explicitly typed parameters. An omitted return annotation infers one exact type from return expressions at definition; all returns must agree and a non-unit result must cover every path. No parameter, constraint or effect is inferred from an invocation target or one instantiation. A closure cannot declare first-class generic parameters of its own. Enclosing type parameters and frozen constraints belong to its private implementation, specialized and rechecked with capture types on Checked before sealing.

Immutable value captures and ordinary var shared cells are admitted; unused external bindings do not enter environments. Shared cells permit only same-instance access, convey no Sendable or cross-worker capability, and retain the OPEN strong-cycle reclamation boundary in section 17.19. Ref/move/view captures and unadmitted noescape, Sendable or effect promises fail closed instead of becoming copies or implicit promises. Source functions and captures use only the XIR/environment path in section 17.18.

### 7.3 Ownership and `move`

`move x` hands the value a binding holds to a new owner and guarantees that nothing is copied; the source binding becomes invalid at once, and using it before reinitialization is a compile error. §2.14.5 lists the bindings that may be moved from. Crossing a task boundary does not require `move`: a copyable, Sendable value already keeps a logical copy when it crosses (§7.4, §10.2), and `move` only saves the copy.

When a binding's last use on every path is a position that takes ownership, that use is an implicit move with the same result as writing `move` (§2.14.5, §16.8).

```xray
fn total(xs: Array<i64>) -> i64 {
    var sum = 0
    for (x in xs) { sum += x }
    return sum
}

fn consume(xs: move Array<i64>) -> i64 {
    xs.push(0)
    return len(xs)
}

fn main() {
    var data = [1, 2, 3]
    var t1 = go total(data)         // the child gets a logical copy; data stays usable
    print(len(data))
    var t2 = go consume(move data)  // handed over without a copy; data cannot be read afterwards
}

main()
```

**`move` usage**: `move` is a unary transfer expression commonly used in arguments and initializers (see §5.1.4 / §10.8):

- `go f(move x)`: hand the value to a `move` parameter of the new task.
- `ch.send(move data)`: hand the value over when sending, without a copy.
- Plain function call `f(move x)`: pass it to a `move` parameter; the callee takes ownership.
- `var dst = move src` / `const dst = move src`: hand it to a new binding.
- Function returns are always written `return value`. A return hands the value over by itself; `return move value` is not a public form.

### 7.4 Cross-Coroutine Data Transfer Rules (Race Avoidance)

"Statically eliminating data races at compile time" is a core design principle of xray's concurrency model; §16.9.5 gives the precise form of that guarantee and its boundary.

Every cross-task boundary — `go` arguments and captures, a Channel send (including a `select` send arm), and Task and `scope` results — follows one set of rules. Legality is decided by the value's complete type, not by the `var` / `const` keyword:

| Value | When it crosses |
|---|---|
| Copyable, Sendable values: scalars, string, Array / Map / Set of Sendable elements, and structs, enums and tuples built from them | Keeps a logical copy: each side may mutate its own value without affecting the other; neither `copy` nor `move` is required |
| Sendable classes and synchronisation handles (Channel, Task, Atomic, `sync` locks and handles) | Shares identity: both sides reference the same object, which provides its own synchronisation |
| Noncopyable, Sendable resources | Handed over only through a consuming parameter |
| Values that are not Sendable: ordinary classes, `ref` borrows, `Slice` / `MutSlice`, Cell | Compile error |

`move x` guarantees no copy but is not a precondition for crossing (§10.8). `const` does not prove Sendable, and a module-level `var` is not made thread-safe by being globally visible.

`go` closures additionally follow one simple, stronger surface rule: **they may not capture any outer `var`, whether they only read it or mutate it, and whether the current program launches one coroutine or many.** Pass the data as explicit `go` arguments. Mutable state shared by multiple coroutines must flow through audited concurrency primitives such as `Channel`, `Atomic`, or `sync` locks and handles. Directly capturing and modifying an ordinary `var` is a compile error, and `unsafe` does not relax this rule.

```xray
var local = 0
go fn() { local += 1 }()                 // ❌ compile error: cannot capture mutable local
```

#### Recommended patterns

```xray
fn grow(data: Array<i64>) -> i64 {
    var mine = data             // mutate a saved logical copy; the caller is unaffected
    mine.push(4)
    return len(mine)
}

fn fill(buf: move Array<u8>) -> i64 {
    buf.push(1)
    return len(buf)
}

fn receive(ch: Channel<i64>) -> i64 {
    return ch.recvOr(0)
}

fn main() {
    // Pattern 1: pass by value; the child gets a logical copy
    var arr = [1, 2, 3]
    var t1 = go grow(arr)
    print(len(arr))             // 3: the caller's arr is unchanged

    // Pattern 2: hand over with move, without a copy
    var big = Array<u8>(1024)
    var t2 = go fill(move big)

    // Pattern 3: pass values through a Channel, whose identity is shared
    const ch = Channel<i64>(10)
    var t3 = go receive(ch)
    ch.send(42)
}

main()
```

### 7.5 Memory Management and Object Lifetimes

Xray uses a layered memory management strategy:

| Storage | Mechanism | Reclamation |
|--|--|--|
| Module `const` | consteval read-only data, or initialization followed by publication (edge 7 in §16.9.2) | at module unload |
| Module `var` | owned by the module; not concurrency-safe by default | at module unload |
| Local band (ordinary objects) | compiler-inserted non-atomic reference counting; the task's reclamation domain | immediately when the last strong reference is released; a reference cycle is broken by the domain when the task ends (§16.3) |
| Shared band (objects reachable from several tasks) | atomic reference counting; promoted by the publisher at a promotion point (§16.3.1) | immediately when the last strong reference is released |
| Stack (`struct` values, locals) | lexical storage duration | scope exit; the language exposes no deterministic destructor / `Drop` hook |
| Arena (low-level temporary allocations) | bulk free | at arena end |

**Memory observation points**:
- The compiler inserts retain/drop operations for ordinary objects; releasing the last strong reference enters the RC destruction path. §16.8 states when a local binding releases the reference it holds.
- The compiler marks only types that may form reference cycles as cycle candidates; the mark decides which identity objects the task reclamation domain registers (L2 in §16.3) and drives no cycle collection.
- Reference cycles are not collected at runtime: they are prevented statically (L0), broken explicitly with `weak` (L1), and broken by the task reclamation domain when the task ends (L2).
- No cycle-reclaiming mechanism exists at runtime — neither a concurrent tracing GC nor a cycle collector. A development build can enable a cycle detector, which only observes and reports; it never mutates the heap.


A READ struct method value `receiver.method` evaluates the receiver once at binding and saves its logical value snapshot without executing the method body. The callable signature contains only explicit parameters and the result, after substituting the struct arguments. Later writes to the original binding do not change the snapshot; identity handles retain their identity semantics. Private/protected access is checked at binding; a declaring body may explicitly return an authorized callable without granting callers direct member access. The callable remains subject to its originating Instance's revocation and cleanup, with no implicit Sendable/no_suspend promise. Ref/move receivers must not degrade to READ binding.

<!-- /xr-spec:en -->

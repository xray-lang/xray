---
id: spec.7_scoping_and_name_resolution
order: 008
---

<!-- xr-spec:cn -->
---

## 7. 作用域与名字解析 (Scoping)



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

### 7.3 所有权与 `move`

可复制的值通过赋值、参数和返回保留逻辑副本。身份类型的副本指向同一身份。`move x` 保证转移时不复制，并使源绑定不可再用。允许的源位置与借用限制见 §2.14.5，参数模式见 §5.2.4。

编译器可在每条路径的最后一次拥有式使用处隐式移动。隐式移动与显式 `move` 的可观察结果相同，释放时机遵循 §16.8。相关优化及完整语法的实现资格见 §17。

### 7.4 跨任务数据传递

跨任务传递要求类型满足 `Sendable`。可复制的 Sendable 值保留独立的逻辑副本，Sendable class 和同步句柄保留同一身份。可复制值无需额外标记即可作为显式参数传入。`move` 可用于保证转移时不复制，不可复制的 Sendable 资源遵循消费参数规则。

`go` 闭包不得捕获外层 `var`，包括只读使用。需要该值时将它作为显式参数传入。共享可变身份经 `Channel`、`Atomic`、`sync.Mutex` 或 `sync.RwLock` 的同步操作访问。模块 `const` 的初始化、发布与可达身份分别遵循 §5.1.2、§16.9 和身份类型的同步规则。

修改值参数时，先建立可变局部绑定。该绑定持有逻辑副本，修改遵循值语义。跨任务入口和同步原语的当前实现资格见 §17。

### 7.5 内存管理与对象生命周期

绑定在离开作用域时按 §16.8 释放所拥有的值，已转移的绑定除外。对象的最后一份强引用释放后进入引用计数销毁路径。借用保持原 owner 的生命周期，不取得一份独立所有权。

环的处理分三层。L0 静态排除不可能成环的类型，L1 通过 `weak` 显式断开引用，L2 在任务结束时清空已登记身份对象的拥有式出边，再由普通引用计数释放。只有可能成环的身份对象进入 L2，具体边界见 §16.3。当前实现资格见 §17。

READ结构体方法值 `receiver.method` 在绑定时对receiver求值一次，保存其逻辑值快照，不执行方法体。所得函数签名只包含显式参数与返回类型，结构体泛型实参先替换；原绑定后续写入不改变快照，身份句柄仍保持其身份语义。私有/protected访问在绑定处检查，声明内部可显式返回已授权的函数值，调用者不会因此取得直接成员访问权。函数值仍受所属Instance的撤销与清理约束，不隐含Sendable/no_suspend承诺。ref/move接收者不得退化为READ绑定。

<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 7. Scoping and Name Resolution



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

Assignment, parameters and returns preserve logical copies of copyable values. Copies of identity types refer to the same identity. `move x` guarantees transfer without copying and makes the source binding unavailable. Permitted source positions and loan restrictions are specified in §2.14.5, and parameter modes in §5.2.4.

The compiler may move implicitly at the last owning use on every path. An implicit move and explicit `move` have the same observable result, with release timing governed by §16.8. Implementation qualification for these optimizations and the complete syntax is specified in §17.

### 7.4 Cross-Task Data Transfer

Cross-task transfer requires `Sendable`. Copyable Sendable values preserve independent logical copies, while Sendable classes and synchronization handles preserve identity. Copyable values can be passed as explicit arguments without an additional marker. `move` can guarantee transfer without copying, and noncopyable Sendable resources obey consuming-parameter rules.

A `go` closure cannot capture an outer `var`, including read-only uses. Pass the required value as an explicit argument. Shared mutable identities are accessed through synchronized operations of `Channel`, `Atomic`, `sync.Mutex` or `sync.RwLock`. Initialization, publication and reachable identities of a module `const` follow §5.1.2, §16.9 and the synchronization rules of its identity types.

To mutate a value parameter, first create a mutable local binding. That binding holds a logical copy and mutations obey value semantics. Current qualification of cross-task entry points and synchronization primitives is specified in §17.

### 7.5 Memory Management and Object Lifetime

On scope exit, bindings release the values they own under §16.8, excluding bindings whose ownership has transferred. Releasing the final strong reference starts reference-counted destruction. A loan preserves its owner's lifetime and acquires no independent ownership.

Cycle handling has three layers. L0 statically excludes types that cannot form cycles, L1 uses `weak` to break references explicitly, and L2 clears owned outgoing edges of registered identity objects at task completion before ordinary reference counting releases them. Only potentially cyclic identity objects enter L2, under the boundaries in §16.3. Current implementation qualification is specified in §17.

A READ struct method value `receiver.method` evaluates the receiver once at binding and saves its logical value snapshot without executing the method body. The callable signature contains only explicit parameters and the result, after substituting the struct arguments. Later writes to the original binding do not change the snapshot; identity handles retain their identity semantics. Private/protected access is checked at binding; a declaring body may explicitly return an authorized callable without granting callers direct member access. The callable remains subject to its originating Instance's revocation and cleanup, with no implicit Sendable/no_suspend promise. Ref/move receivers must not degrade to READ binding.

<!-- /xr-spec:en -->

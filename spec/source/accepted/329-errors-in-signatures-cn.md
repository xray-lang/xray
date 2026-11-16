# 已接受目标规则：329 - 错误进入签名：`throws(E)`、`try` 标记与局部检查

> 2026-11-16 从任务文档迁入统一规范源。此处维护已接受的目标定义；原任务只维护实施、依赖、理由与验收记录。
> 迁入不表示实现完成。下面保留的示例是目标验收材料，不能据此声称当前编译器、VM 或 AOT 已支持。历史修订和已明确被取代的规则保持历史身份；不能将旧版本重新激活。
> 实施任务：[329-errors-in-signatures-cn](../../../../xray-docs/tasks/329-errors-in-signatures-cn.md)。

## 1. 冻结的决策

| ID | 决策 | 内容 |
|---|---|---|
| E1 | 不写即不抛错 | 签名没有 `throws` 子句，就表示保证不抛错。函数体中有未捕获的错误时编译报错，并给出补上 `throws(...)` 的修复建议。不采用 Zig 式的"不写就推导" |
| E2 | 无类型 `throws` | `throws` 等价于 `throws(Error)`，即擦除后的"任意错误"。标准库公开 API 必须写带类型的 `throws(E)` |
| E3 | 保留 `try { } catch` | `try` 后面紧跟 `{` 时是代码块，表示就地处理，块内的调用不需要再写 `try`；`try` 后面跟表达式时表示传播；`try?` 和 `try!` 只修饰表达式。不引入 `do` |
| E4 | 高阶函数 | 用泛型错误参数 `E: Error` 配合 `Never` 表达"随参数而抛错"，不引入 `rethrows` |
| E5 | Task | 类型为 `Task<T, E = Never>`；2026-11-02 起 `await t` 的错误类型只有 `E`，被等待任务的取消改为使等待方自身取消展开，见 [Task331 G11](../../../../xray-docs/tasks/331-language-surface-convergence-cn.md#await-cancel-propagation-and-closed-send-2026-11-02)（原决定为 `E \| Cancelled`，已被取代） |
| E6 | freestanding | 只允许带类型的 `throws(E)`；`@strict(alloc)` 范围内同样不允许无类型 `throws`（擦除后的错误在载荷超过内联缓冲时会分配内存，见 §2 的表示）。`@noalloc` 已由 Task328 A16 取消，改为 profile 级约束加 `@strict(alloc)` |
| E7 | 导出函数 | 标准库强制写具体错误类型；其它代码使用无类型 `throws` 时只给 lint 提示 |

## 2. 语法与语义（规范定稿内容）

### 2.1 文法

```ebnf
FnDecl       ::= AttrList? 'fn' Identifier TypeParams? '(' ParamList? ')' ThrowsClause? ReturnType? Block
MethodDecl   ::= Modifier* Identifier '(' ParamList? ')' ThrowsClause? ReturnType? Block
InitDecl     ::= 'init' '(' ParamList? ')' ThrowsClause? Block
FunctionType ::= 'fn' '(' FunctionTypeParams? ')' ThrowsClause? ReturnType?
FunctionTypeParams ::= ParamType (',' ParamType)* ','?
FnExpression ::= 'fn' TypeParams? '(' ArrowParams? ')' ThrowsClause? ('->' Type)? Block   // fn lambda（Task331 G6）
ThrowsClause ::= 'throws' ( '(' Type ')' )?
TryExpr      ::= 'try' ( '?' | '!' )? TernaryExpr    // 非赋值操作数；'try' 后面不是 '{' 或 'scope'
TryStmt      ::= 'try' Block CatchClause+            // 'try' 后面紧跟 '{'
ScopeStmt    ::= 'scope' ('(' 'isolated' ')')? Block  // Task331 G2
TryScopeStmt ::= 'try' ScopeStmt                    // 语句，不是值表达式
CatchClause  ::= 'catch' '(' CatchHead ')' Block
               | 'catch' 'panic' ( '(' Identifier ( ':' 'PanicInfo' )? ')' )? Block
ThrowStmt    ::= 'throw' Expression                  // 不变
```

函数类型统一写成 `fn(T) -> R`，同时修正 EBNF 附录中 `FunctionType ::= '(' TypeList? ')' '->' Type` 与 §2.2 写法不一致的问题。

`throws` 按 Task331 G12 是上下文关键字。保留规范 §2.8 已冻结的 Unit 拼写：返回 Unit 的函数类型省略箭头，如 `fn()`、`fn() throws(E)`，类型位置不写 `-> ()`。上述文法不改变声明位置可显式返回 `()` 的规则。`TernaryExpr` 对应 EBNF 的非赋值层级，括号、参数分隔符和语句结束仍限定操作数范围。

### 2.2 签名

```xray
fn add(a: i64, b: i64) -> i64                             // 保证不抛错
fn parse(s: string) throws(ParseErr) -> i64               // 只抛 ParseErr
fn load(path: string) throws(IoErr | ParseErr) -> Config  // 多个错误枚举写成 union
fn runAll() throws                                        // 任意错误，返回 ()
```

- `throws(E)` 中，`E` 必须是以下之一：错误枚举、带 `Error` 约束的泛型参数、`Error`、`Never`，或这些错误类型的 union（包括 `E | IoErr`）。`throws(Never)` 等价于不写 `throws`。
- `throws` 子句是函数类型的一部分，参与重载判定、函数值赋值和接口匹配。
- 方法与构造器写法相同，`init(...) throws(E)` 表示可能失败的构造。

### 2.3 调用点

```xray
var n = try parse(s)                  // 传播给调用方
var cfg = try load(readText(path))    // 一个 try 覆盖它右侧整个表达式中的所有调用
var port = (try? parsePort(s)) ?? 8080
var re = try! Regex("^[a-z]+$")
var v = try await task
```

- 可能抛错的调用（包括 `await` 一个可能失败的 Task，以及 `scope` 块）必须处于以下两种位置之一，否则编译报错并给出修复建议：
  - 某个 `try` / `try?` / `try!` 表达式的范围内；
  - 某个 `try { }` 代码块内（见 §2.4）。
- `try`、`try?`、`try!` 覆盖其右侧、直到赋值运算符为止的整个表达式。
- `try?` 与 `??` 连用时必须加括号，否则报诊断，避免出现 `T??` 这种含义不清的类型。
- `try?` 的结果类型为 `T?`。`T` 本身已经是 Optional 时保持原类型，不新增外层，也不递归展平已有层次；一般 Optional 的独立层规则不变。`T=Unit`时仍须区分Some和None：Some使用已验证类型的零payload，不凭空创建普通Unit SSA值；`??`的分支检查与实际副作用保持。Array<Nullable<Unit>>不等于开放Array<Unit>或普通Unit参数。
- `try!` 遇到错误时触发 panic，panic 信息中带上错误值。
- 与 `await` 连用时，顺序固定为 `try await`；`Task<T, Never>` 直接写 `await t`。
- 标记只写在有业务错误来源的位置：try、try?、try! 的表达式范围，以及含普通 catch 的 try 主体没有来源时，报 `E0336`。仅含 catch panic 的块是合法 panic 边界，不要求业务来源；同时含普通 catch 就不享有此豁免，块内表达式标记仍独立检查。来源包含声明可抛的调用、await、scope 和直接 throw；依据定义处签名与实际求值规则，错误类型含类型参数时算可能抛错（2026-11-02 决定、2026-11-03 选择 A 定界，不做兼容，见 [Task336 §2](../../../../xray-docs/tasks/336-error-panic-scheme-a-implementation-cn.md#2-唯一的源码与通道规则)及 [Task331 G11 第 7 条](../../../../xray-docs/tasks/331-language-surface-convergence-cn.md#await-cancel-propagation-and-closed-send-2026-11-02)）。

#### 默认参数与构造初始化（2026-10-22 用户已确认）

- **默认参数与函数体分别检查。** 函数的 `throws(E)` 只约束函数体，不包含调用方执行的参数默认表达式。默认表达式在声明处独立检查，其中调用抛错函数仍须写自己的 `try`；调用方的传播标记不能代替这个标记。
- **只计入实际省略的参数。** 直接调用的错误来自被调声明及本次实际求值的表达式，包括省略实参所执行的默认表达式。显式实参不执行对应默认表达式，也不承担它的错误；不能把所有默认值的错误无条件并入函数声明。实际泛型替换后再按相同规则检查。
- **字段默认值属于构造初始化。** 隐式构造器从字段初始化局部推导错误类型，调用按这个错误类型要求传播标记。显式 `init` 的 `throws(E)` 必须覆盖字段初始化和构造器体逃逸的错误；省略子句时两者均不得有业务错误逃逸。构造参数的默认表达式仍按上面的调用点规则检查。

```xray
enum ReadErr { Missing }
fn read() throws(ReadErr) -> i64 { throw ReadErr.Missing }
fn f(x: i64 = try read()) -> i64 { return x }
f(1)                  // 不执行默认值，不需要 try
try f()               // 执行默认值，调用方承担 ReadErr
// f()                // 负例：默认值可能抛错，缺少传播标记
// fn bad(x: i64 = read()) {} // 负例：默认表达式自身缺少 try
```

上述代码说明已定目标语义，不是当前编译器通过记录。实现须同时覆盖默认参数显式／省略、缺少内层／外层 `try`、字段默认值不抛错／抛错、隐式构造器及显式 `init` 的覆盖／漏声明正负例。只读取真实默认绑定和初始化步骤的错误，不复活全程序错误推导，不借此提前交付 Callable／Task 字段或 union 规范化。

初始化步骤互相依赖时，采用[生成初始化环的精确业务错误规则](../../../../xray-docs/design/frontend/initializer-error-cycles.md)（2026-10-29用户确认方案一）：只求解编译器自动生成的字段默认值、参数默认表达式和隐式构造初始化的依赖；普通函数、导入函数和显式 `init` 都以其已声明且实际替换的 E 为叶，不读取其函数体。已确认没有业务错误的纯生成环得到 `Never`，调用 `Node()` 不需要 `try`；实际引入的业务错误必须保留。

- 每个生成步骤保留自己的实际逃逸 E，应用原有局部 `catch`、`try?`、`try!` 和真实泛型替换规则；不能把环中所有成员强行设成同一错误 union。已知宽界保持其真实宽界，未确认的依赖或无效来源不得猜作 `Never`。
- 生成合同必须在相关函数值/Callable类型消费及ready发布前完成；禁止先用临时 `Never` 通过检查再修补已消费的签名。未有真实就绪合同的匿名推导仍明确拒绝，不借本片开放其它闭包或泛型能力。
- 受控Source可先用同一证明核心的声明值约束阶段准备依赖；生成E稳定后必须再过原严格完整声明证明。值阶段不能替代Checked/packet/reader/owned的完整门，不能把未知Unit合同发布为已检查产物；既有可确认声明E仍受原检查。
- 实施先闭合实际生成依赖及局部处理关系，再把已解合同接回现役构造/默认值调用者，并随消费者原子移除旧的“环尚未支持”拒绝；不恢复旧全程序ErrorFlow、跨请求错误缓存、公共work/quota或按错误数量裁剪签名。
- 验收包含纯环免 `try` 的编译/packet/请求退休链、真实ReadErr环的精确声明及缺 `try`/漏声明负控、组内各自catch、显式init及普通/导入声明截断、未知依赖拒绝，以及有界真实错误路径的VM/C/native/SDK生命周期。无限递归例只验证编译，不实际运行；递归、panic、取消和内存失败继续走已有独立通道。
- 此切片已完成实现、独审与主干集成：纯环只编译/packet及退休链，不运行无限递归；完整有界ReadErr环、per-node处理/泛型/default、严格未知合同负控、跨模块取消位置、CLI/VM/C/MSVC及搬移SDK请求退休后读取释放42均有实际资格。现役旧成环拒绝已随消费者移除；完整P4、CTFE query等仍开放，动态证据统一见STATUS。
- 编译内存、取消/deadline、首因、失败输出保全和同配置重试使用现有请求机制；补受影响普通/真实AU、metadata及生成C门。上述为已定设计及出口，当前是否实现和主干证据统一见STATUS。

### 2.4 `try { } catch` 代码块

```xray
try {
    var cfg = loadConfig("app.toml")      // 块内不需要再写 try
    run(cfg)
} catch (e: IoErr) {
    print("io:", e)
} catch (ParseErr.BadChar { value }) {
    print("bad char:", value)
} catch (e) {
    print("other:", e)
} catch panic (p) {
    print("fault:", p.message)
}
```

- **区域规则**：块内的可能抛错调用都视为已标记，不需要再写 `try`。区域不延伸到块内定义的闭包，闭包体按它自己的签名独立检查。
- **穷尽与传播**：块内可能抛出的集合减去各 `catch` 分支覆盖的部分，剩下的继续向外传播；所在函数必须用 `throws` 声明这部分，否则编译报错。全部覆盖时，整个 `try` 语句不抛错。
- **catch 头部**：保留现有写法 `catch (e)`、`catch (e: T)`、变体模式和 `catch panic`。新增一条规则：括号里只写一个以大写字母开头的标识符（如 `catch (NetErr)`）时报编译错误，并提示改为 `catch (e: NetErr)`，消除"误把类型名当成绑定名"的易错点。
- **2026-11-03 方案 A 已选定**：仅含 panic handler 的块仍许可主体业务调用，但不会消费业务 E；逃逸签名检查照常执行。区域不覆盖自身 handler 或嵌套函数定义。E0336 检查区域有无业务来源，不等价于完整死 handler 分析，详见 [Task336](../../../../xray-docs/tasks/336-error-panic-scheme-a-implementation-cn.md)。
- 原“可选 lint 强制块内写 try”不再作为本方案交付项。保留合法的自愿表达式标记，以准确诊断、文档和后续 IDE 声明 E 展示改善可读性，不引入第二套标记合法性规则。

### 2.5 函数类型、闭包与高阶函数

```xray
fn map<T, U, E: Error>(xs: Array<T>, f: fn(T) throws(E) -> U) throws(E) -> Array<U> {
    var out: Array<U> = []
    for (x in xs) { out.push(try f(x)) }
    return out
}
var lens = map(names, fn(s: string) -> i64 { return len(s) })   // E = Never，调用处不写 try
var nums = try map(texts, parse)                                // E = ParseErr
```

- **`Never`** 对用户开放：它是没有值的底类型，也是编译器内部收窄规则中 `never` 的公开名字。
- **子类型**：不抛错的函数类型可以赋给任何会抛错的函数类型；`throws(A)` 可以赋给 `throws(A | B)` 和 `throws`。
- **闭包**：fn lambda 显式写 `throws(E)` 时先按该子句检查函数体，再按错误子集匹配期望类型；期望类型不能放宽显式声明。无显式子句时，有期望的函数类型就按期望检查，没有时才由闭包体局部推导 `throws(E)`。箭头 lambda 不书写 `throws`（Task331 G6）。
- **泛型错误参数**在调用处由实参推导，单态化后每种情况各生成一份代码，没有运行时开销。
- **调用标记**按实例化后的完整错误类型判断。`E = Never` 只有在完整错误类型也为 `Never` 时免除 `try`；例如 `throws(E | IoErr)` 仍要求标记。

### 2.6 接口、覆写与 Task

- **接口方法**声明 `throws(E)`；实现和覆写可以声明它的子集，也可以不抛错。
- **Task**：`go f(...)` 中 `f` 声明 `throws(E)` 时，类型为 `Task<T, E>`；不抛错时为 `Task<T>`，即 `E = Never`。
- **等待**（Task API 按 Task331 G11 定稿）：
  - `await t` 的错误类型为 `E`，`E = Never` 时不写 `try`（2026-11-02 起，原为 `E | Cancelled`）；
  - `Task.all(ts)`、`Task.first(ts)` 的错误类型为各 Task 错误类型的 union；
  - `Task.firstSuccess(ts)` 全部失败时抛 `TaskFailures<E>`；
  - `Task.timeout(t, ms)` 的错误类型为 `E | Timeout`；
  - `awaitResult()` 返回 `TaskResult<T, E>`，其变体为 `Success(T) | Error(E) | Panic(PanicInfo) | Cancelled`；`poll()` 返回 `TaskResult<T, E>?`，任务未结束时为 `null`。
- **取消**：只在挂起点检查。被取消的任务在挂起点展开，执行 `defer`，不能被 `catch` 捕获，也不写进函数签名；用 `await` 或聚合函数等待它的一方自身进入取消展开，`awaitResult()` / `poll()` 以 `TaskResult.Cancelled` 报告（Task331 G11 2026-11-02 决定）。
- **`scope`**（Task331 G2）：
  - 设 B 为主体经过局部 catch 后仍逃逸的业务错误集合（包括 handler 的新错误），E 仅为词法直接 go 子任务的声明错误集合。普通 scope 的完整业务错误为 `B | E`；isolated 为 `B | TaskFailures<E>`，E=Never 时没有子业务汇总通道；
  - 普通 scope 在子任务失败时取消兄弟，isolated 子失败本身不取消兄弟。两者在全体结束后均优先传播首个成功发布的 panic，panic 不进入业务汇总；无 panic 时普通 scope 选择主体/子任务业务错误的首发布者，isolated 优先直接传播主体 B，没有主体错误时才汇总子 E；
  - B、E 都为 Never 时，两种 scope 均免传播标记；可能 panic 不改变此规则。这落实用户“保留免 try，panic 只走 panic 通道，不进入 TaskFailures 汇总”的裁决，不定义所有类型位置的 `TaskFailures<Never> = Never`；
  - `try scope` 只标记退出处隐式传播，不建立 `try { }` 保护区域；主体调用仍须有自己的传播标记或真实外层 try 主体保护，最终完整逃逸集合仍检查所在函数签名；
  - 正常块末 join；主体 Error/Panic 或向外 return/break/continue 时两者均 cancel+drain。pending 正常转移拥有其值并保活主体帧及有效借用，没有更高失败才履行。父已在合法挂起点进入取消展开时继续不可捕获取消；请求本身不改变帧或终态，次级失败单独报告；
  - scope 隐式等待与显式 `await` 都不向 B 加入 `Cancelled`；默认 scope 因成员失败请求的取消使主体进入的展开止于 scope 边界（Task331 G11 2026-11-02 决定）。T、E 和完整拥有式 PanicInfo 图都须 copyable+Sendable；ABI、内存序和执行联验仍待共同完成。

TaskResult的本片Source/VM/生成C、CLI/native与SDK消费者已交付：采用真实内建声明身份治理的nominal类型，逻辑类型参数T/E属于同一类型池；Error(E)复用现有XR_ERROR物理载体，Panic复用完整拥有式PanicInfo图，Success支持Unit及现役Task结果类型。Source、共同raw构造/投影检查及运行时值准入均须保留并验证E的真实bound，Never不能构造Error变体，错误union不因此取得普通值布局，也不增加另一套ErrorSet或union盒。复制与Sendable资格按逻辑T/E及完整PanicInfo证明，不能从擦除后的载体推定。poll只读取经过认证的终态快照、不驱动任务；awaitResult在子取消转为业务Cancelled之前接收四态，自身取消、引擎/协议失败与fatal仍走原独立通道。新合同按common106原子切换，其余ABI/schema只按实际字段和布局变化裁；旧单参数/五变体登记与其生产者随消费者退出，不保旧ordinal解释、别名或兼容路径。

2026-10-29用户确认：TaskResult.Panic(info)在本地构造时也统一要求完整PanicInfo图copyable且Sendable，包含cause和replacedError等拥有式子图；Source、raw指令、packet和owned值准入均执行同一不变量，跨任务发布时再次验证。因此TaskResult的静态Sendable证明可按逻辑T/E递归，不能给一般PanicInfo或普通enum添加无条件Sendable承诺。非法本地图的构造明确拒绝并保原图/owner及更早资源失败，不改装成业务Error；验收包含非法本地图拒绝、Unit/Never、Array包装和真实go返回TaskResult的正向，以及producer退休后的完整图读取与释放。本片已通过这些真实消费者的普通/真实AU及拥有式生命周期验证；完整结构化并发与整个阶段仍开放，精确资格见STATUS。

#### 2.6.1 Iterator 的声明错误类型（2026-10-27 用户决定）

采用唯一的 `Iterator<T, E = Never>`。E 复用本任务的 canonical 错误类型：错误枚举、受 Error 约束的参数、错误 union、Error 或 Never；不增加第二套错误推导机制。`Iterator<T>` 是省略默认参数的正常拼写，不是旧接口别名。普通集合迭代声明 E=Never；擦除边界可以显式采用 E=Error，不能从已擦除句柄恢复更窄的承诺。

- `hasNext()`、`next()`、`nth(...)` 等实际可能推进生成器的方法声明 `throws(E)`；hasNext 即使缓冲一个元素，也不能漏掉推进错误。`iterator()` 等仅交回句柄且不推进的操作不因此增加 E。
- 含 yield 的生成器声明从返回 `Iterator<T,E>` 取得 producer body 的唯一业务错误界；创建句柄不执行该 body，公共工厂 error_type 为 Never。生成器的非 Never 函数 throws 子句拒绝并提示把潜在迭代错误写进返回类型，不重复维护两份上界。
- 普通函数即使返回 Iterator，也仍按自己的函数 throws 检查实际创建工作；参数默认表达式与调用实参的错误继续按 §2.3 独立检查。它们不能与以后推进的 E 混为一谈。
- 推进调用从句柄类型读取 E，不回看生成器函数体。业务错误、panic、取消及资源失败保持各自通道；本决定不把后三者并入任意 E。借用、挂起帧与清理仍遵守 Task316。
- 先兑现已有显式推进调用的 try 与 try 块保护；for 内隐式推进同样必须被局部检查覆盖。工厂表达式上的 try 不自动保护以后的推进。本片不自行新增 for 传播拼写；尚未有可表达保护的组合明确拒绝，不能偷偷按旧推导接受。

实施顺序与依赖：

1. 与 `Task<T,E>` 共用必填 canonical E 的类型形状、遍历、替换和拥有者规则；复用现有类型字段并不等于 wire 已兼容。所有生产/读取、Checked/Program 编解码和身份必须原子接入，缺 E、错误 E、旧格式直接拒绝，不从缺字段补 Never。
2. 将 Source 的生成器工厂与 producer 分开检查，迁 native 声明、生成器、泛型绑定、默认 Iterator 参数及实际集合/生成器消费者。受影响的共享生成器由主会话安排唯一后继作者；不得并行维护两套签名事实。
3. VM/原生产物的推进与 Task outcome 读取真实声明 E，替代旧 ErrorFlow/有限关系查询；保留独立 root/suspend 语义，不把要删除的全程序错误推导搬入 HIR 或资源模型。
4. 按具体接口开放，不等待整个 HIR/新 VM，但不能绕过声明验证、所有权和清理前置。门包含普通集合免 try、typed generator 成功/错误、hasNext 失败、工厂与推进分离、缺 try/错 E 拒绝、跨 packet/模块/请求寿命、实际 Source 执行和适用安全及生成 C 检查。

非目标：本决定不引入新继承/反射框架、第二套动态迭代器、兼容 reader、统一编译 work 或额外公共缓存。完成标准是这些实际消费者及旧推导删除闭包通过，而不是仅增加第二类型实参；错误union、Cancelled/TaskResult已有本片运行资格，生成器/结构化并发的其余实际接口仍须在各自依赖中闭合。

### 2.7 `defer`、顶层代码与 FFI

- **`defer`**：清理块必须不抛业务错误，现在可以在编译期完全判定（E0387 保留）。删除业务错误的运行时兜底 E0443；规范 §8.3.1 的 D3 改写为 §2.8 的 panic 规则，只有双重故障才不可捕获地结束。不能把不抛业务错误等同于不会 panic。
- **入口文件与顶层代码**（2026-10-22 用户决定）：
  - 语言没有固定的 `main` 入口函数。程序从入口文件的顶层开始顺序执行。入口文件是 `xray run`、`xray build` 等命令直接给出的文件，或 `xray.toml` 中 `main` 字段指定的文件；
  - **只有入口文件可以包含可执行的顶层语句。** 被导入的模块，顶层只能包含声明和模块状态的初始化（`const`、`var` 及其初始化表达式）；出现独立的表达式语句或控制流语句时编译报错；
  - 入口文件的顶层语句和各模块状态的初始化表达式隐式允许抛任意错误，传播点同样要写 `try`；未捕获时输出 `[Uncaught Error] <值>` 并以退出码 1 结束。
- **`main` 是普通函数**（2026-10-22 用户决定）：
  - 和其他具名函数遵守同一规则：省略 `throws` 即不抛错，会抛错的写 `fn main() throws(E)`，并在入口文件顶层写 `try main()` 调用；
  - 不再有任何以名字 `main` 区分的规则。同日早些时候"按函数体推导 `main` 的错误类型"的决定，以及 lead 的 [main 入口角色](../../../../xray-docs/review/329-main-role-lead-2026-10-22.md)，都已撤回；
  - 规范修改（§5.2.8、§11.6 和 §5 中关于隐式错误许可的一句）已由 a 评审 `user/329-entry-spec`（`70aeffd58`），经队列 13 以 `368df2f97` 合入 `main`。
- **FFI**：`extern "C"` 函数和 `CFn` 回调必须不抛错；导出给 C 的 Xray 函数必须不抛错，否则编译报错，需要自己包一层，把错误转成状态码。
- **源码之外的合同**：
  - 删除 `xray verify` 合同中的 `no_throw`，改由签名表达；
  - 删除 `xray.toml` `[declarations]` 中与错误相关的字段；
  - 其中与挂起相关的 `no_suspend` 由 Task328 A5 的 `nosuspend` 承接；
  - `xray verify` 只保留与目标产物形状相关的要求。

### 329-05 整数filter闭包的业务错误界精度前置（已集成）

独立对照已确认：旧clean d3的Source生产字节与当前main相同，最小I64 remainder/equality filter分别实际输出2及1加LF；a的字段WIP会以callback bound拒绝，常量回调两边仍4加LF。它是新的精度回退，不能将functional.xr首诊从既有61行递归缺口移到49行并冒认旧F17；smoke清单和demo算法保持原样。原件D:/xray-build/lead-tmp/callable-filter-regression/result.json，WIP未冻结也未获门资格。

lead作者、a有限Source接口意见、d独立评审；准确两路径src/xir/xxir_source_closure_error_bound.inc.c与tests/unit/xir/test_xir_never_type.c，生产最多60增删行。只在既有未完成anonymous／omitted throws／currentError／非checked library／非generator guard中，补真实I64 REM和I64 ==为无业务E：前者结果I64，后者XR_XIR_EQUAL结果BOOL且immediate0，两操作数各自真实I64。!=、非I64/一般相等及其它未知操作仍保守Error。Panic和资源／执行失败不写业务E，不据此承诺不panic或新fault执行资格。

finish_body尚未发布region，值类型只能读真实function.parameters（含capture）或同owner、较当前ordinal更早的unfinished recipe并复制type标量；空指针／越界／未定型／前向或不匹配value均不能证明Never。不读function.instructions、不从BOOL结果／AST外形猜I64、不保留跨增长元素指针。原实际callee声明E和指令真实span一次替换、资源失败路径均保持；不扩全程序推导、外来宽Error缩窄、间接推导或局部body检查，无格式35/95／SDK／ABI变化。

新persistent断言先在旧helper鲜构复现FAIL，再验证I64参数、前序REM值及capture的canonical Function E=Never；小NE／非I64控制用局部closure保留Error，原indirect／宽import Error控制不变。复用既有Library→packet→Catalog→Source→输入owner退休及独立再读全链。新SHA十门smoke、meta_ownership_inventory、source_repl_cli、test_lir_lower、source_public_instance、test_xir_library_literal_owner、test_parser_errors、test_source_entry、test_xir_never_type、test_xir_p11_typed_literal_text按Release／真实address+undefined重建执行，含normal及inline Source、真实生成C／依赖／CRT；另核CLI最小2／1LF与constant4LF。作者固定片已通过双profile各十门及实际生成C补门，d独立终审后经真实队列59合入；当前L1 63/63、ownership 1/1、同SHA fresh CLI clean，最小CLI为2／1／4LF；完整ASan红名单NOT_RUN，旧L3不作退出。超过两路径或60生产行先回范围，不并进B；B从本片新main重新合并／复验其原40路径，字段启用、局部检查与false-Never全体缺口仍各自开放。

实际交付仅两路径、29生产增删行；同一新增测试在旧helper重建后先报filterEqual的Error界失败，新片在producer／packet／Catalog／输入owner退休和独立reader后均通过。完成源码枝已按整片吸收和已验证bundle执行D4；采集器首次误把normal／SourceSDK两个recipe算成一个的失败单独保留，未移用dirty门。准确原件见D:/xray-build/queue/reviews/329-05-queue59-verified.json。此片只提供字段B的精度前置，B必须重新冻结并跑自己的门，不关闭一般闭包推导或局部body检查。

### 329-06 捕获整数写入和实际callable声明界的精度准备（已集成准备，间接正向待B36）

bd9字段候选在329-05主干重放后仍L1 62/63；lead/a独立干净CLI对照证实counter和compose原可执行，新候选在闭包返回fn Never时拒绝，旧smoke首诊不改。两者分别缺少Cell<I64>写入和真实callee Callable E=Never的精度；twice最小例两边已有的检查失败另记，不混作新增回退。

准入作者lead、a有限Source意见、d独立评审，仍仅src/xir/xxir_source_closure_error_bound.inc.c与tests/unit/xir/test_xir_never_type.c，预计40–55生产增删行、上限60，超界先停。复用真实参数／先前同owner/value recipe取型；CELL_WRITE限Unit结果、实际Cell<I64>目的与I64来源。CALL_INDIRECT的callee逻辑值在immediate，必须非负且不超UINT32，再取真实类型并要求kind-checked Callable的canonical E恰为Never；不读目标body、不从参数/结果/名字猜E，Unit／Error／generic／unknown均保守Error。原anonymous／omitted／currentError／非library／非generator guard、真实direct声明及一次替换保持；不改ABI/格式或启局部body检查。

旧35主干只可交付Cell写入的真实正向、旧Unit间接调用的保守反例，以及Source／owned／packet／Catalog／输入退休／独立reader全链；同新增counter断言须先在旧helper重建失败。间接Never的正向资格明确依赖后续B36：旧35真实Callable.element依法为Unit，不能伪造Never描述符来冒认已测。B从此准备片新main重放后，须在原测试文件验证makeIndirect／compose的实际参数E=Never与匿名Function E=Never，以及实际扩大raw声明为Error后间接捕获调用仍Error；counter／compose CLI和原smoke都须通过。B仍35＋5路径，helper作为真实前置继承；未取得该后继正向前，不称两项字段回退已全部修复。

本片新固定SHA须先重建相关目标，跑与329-05相同十门及生成C补门的Release／真实address+undefined，normal／inline、owner、实际依赖／CRT和完整现行队列L1／cleanCLI分身份记录。完整ASan红名单NOT_RUN，旧L3／平台／性能／发布、一般闭包推导和字段B新SDK／SAN／局部检查均未关闭。实际交付两路径、27生产增删行；旧helper同counter断言先FAIL，新片双profile各十门及真实生成C补门通过，d独立终审后经队列61合入，当前L1 63/63、ownership1/1及fresh CLI clean。源码枝已bundle及吸收证明后D4。准确原件D:/xray-build/queue/reviews/329-06-queue61-verified.json；间接Never正向仍必须后续B36真实资格，不据准备片通过关闭字段回退。

### 2.8 panic

panic 通道（越界、除零、match 失败等运行时故障）保持不变，不写进签名，`catch panic` 也保持不变。

**清理边界（2026-10-22 用户决定，取代 2026-10-21 lead 裁决）**：panic 可以在 cleanup 内由其内部的 `catch panic` 捕获。panic 逃出 cleanup 时，按逃出时的背景处理：

| 逃出时的背景 | 结果 |
|---|---|
| 正常离开作用域（包括 `return`、`break`、`continue`） | 该 panic 成为当前 panic，照常向外传播，可以被 `catch panic` 捕获 |
| 业务错误正在传播 | 同上；原业务错误不再传播，记入 `PanicInfo` 的上下文，panic 未被捕获时与其一起打印 |
| 已有 panic 正在展开 | 双重故障，立即不可捕获地结束：独立运行的程序终止进程，退出码 70；嵌入宿主中只结束本次执行，宿主存活（与 Task328 A18 同一结束路径）。诊断同时报告原 panic 和新 panic |
| 取消正在展开 | 同上，按双重故障处理 |

这与 Rust 的规则一致：Drop 中的 panic 只在已经展开时才 abort。cleanup 中一处程序错误不会越过外层已经写好的 `catch panic`；只有确实无法安全继续的双重故障才终止。

删除 E0443 后，双重故障的诊断使用新 panic 的编号和消息，附 cleanup 上下文和原 panic 的信息；不新建业务错误 wrapper。用户同日确认 `PanicInfo` 增加只读 `replacedError: Error?`：它拥有被 cleanup panic 取代的原业务错误，没有时为 `null`。未捕获 panic 的主诊断下一行打印 `[Replaced Error] <原错误值>`，复用既有业务错误值格式；没有被取代错误时不打印该行。`cause` 仍只表示 panic 链，不用它包装业务错误。a 在规范 §8.2 定义这些规则，P2 同步各后端的黄金输出。违反 `Never` 的坏产物或 native 协议属于无效执行边界，不能当作合法业务错误继续传播。

CTFE 保持独立宿主边界：cleanup 中的单个 panic 按普通 CTFE panic 报告编译失败；双重故障返回 `CLEANUP_PANIC` 编译失败。两者都只释放本作业资源，不退出编译器进程。现役CTFE按真实父frame的pending背景及登记处handler区分传播，只有已有panic展开时再次逃逸才进入CLEANUP_PANIC；完整拥有式cause/replacedError与剩余cleanup收取保持。原完整Source、CLI/native和SDK生命周期已验证；CTFE query与单遍CTFE仍开放。

2026-10-28运行时首片已交付（Call37/common103）：原完整plain/try?输入均由外层捕获并输出420LF；拥有式replaced Error、446自身Error与替代Error并存、内部catch、跨Task/Iterator/Instance与host的完整双故障终态、退休后格式化已通过真实VM/生成C/native/SDK消费者。普通退出与业务Error背景不误判fatal；只有实际pending panic/取消再panic才fatal。硬host失败只物理关闭，正常关闭在free前收取完整selected owner；status-only资源/IO/协议拒绝不发布成Source_OK结果。原139输出限制回归及同FAILED模块探针drain9的错误因果推断保留，真正已READY依赖的drain24诊断与canonical修后资格分开；未因此放宽模块权限。本段所述当时尚未交付的错误union/typed await/TaskResult现已有后继资格；CTFE cleanup本段出口已有后继资格，query及完整329仍开放。当前精确证据和主干门禁统一见STATUS。

### 2.9 诊断编号（329-02 交付要求）

E0330–E0349 保留给错误签名和入口检查。E0330–E0334 已在 329-02 候选登记；E0335 分配给 329-04 的被导入模块顶层语句检查。下表不代表相应检查器均已实现或合入主干：

| 编号 | 名称 | 条件与修复建议 |
|---|---|---|
| E0330 | `XR_ERR_ANALYZE_TRY_REQUIRED` | 会抛错的调用缺少标记；使用 `try`、`try?`、`try!` 或局部处理 |
| E0331 | `XR_ERR_ANALYZE_UNDECLARED_ERROR` | 逃逸错误不在声明集合中；捕获或补充 `throws(E)` |
| E0332 | `XR_ERR_ANALYZE_ERROR_OVERRIDE` | 实现/覆写扩大错误集合；收窄或内部处理，其余覆写不匹配仍用 E0374 |
| E0333 | `XR_ERR_ANALYZE_ERASED_THROWS` | freestanding 或 `@strict(alloc)` 使用擦除错误；改用具体错误类型，同时覆盖 `throws` 与等价的 `throws(Error)` |
| E0334 | `XR_ERR_ANALYZE_CATCH_TYPE_NAME` | 单独大写标识符 `catch (NetErr)`；改用 `catch (e: NetErr)` |
| E0335 | `XR_ERR_ANALYZE_IMPORTED_TOP_LEVEL` | 被导入模块出现独立的表达式或控制流语句；把可执行语句移到入口文件，模块只保留声明及 `const`、`var` 的初始化 |
| E0336 | `XR_ERR_ANALYZE_REDUNDANT_TRY` | `try` 形式的作用范围内没有可能抛错的位置；删去该 `try`（2026-11-02 用户决定） |

头文件、中英文规范和正反例须一并登记。E0393/E0394/E0396/E0397 已实际使用，规范漏列不能视为空位；E0445 已在 `src/xir/xxir_fault.h` 用作 assertion panic，也需同步编号真值源。此表不代表新诊断已经实现。

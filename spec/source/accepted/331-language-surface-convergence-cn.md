# 已接受目标规则：331 - 语言表面收敛：结构化并发、溢出审查、正则字面量、并发 API 与关键字表

> 2026-11-16 从任务文档迁入统一规范源。此处维护已接受的目标定义；原任务只维护实施、依赖、理由与验收记录。
> 迁入不表示实现完成。下面保留的示例是目标验收材料，不能据此声称当前编译器、VM 或 AOT 已支持。历史修订和已明确被取代的规则保持历史身份；不能将旧版本重新激活。
> 实施任务：[331-language-surface-convergence-cn](../../../../xray-docs/tasks/331-language-surface-convergence-cn.md)。

## 1. 冻结的决策

| ID | 决策 | 内容 | 来源 |
|---|---|---|---|
| G1 | class 继承：维持现状 | 保留 `extends`、`super`、`protected`、`final`：class 默认可继承，`final class` 禁止继承；覆写必须写 `override`（Task308 §11）。实现归 Task327 F03。性能由 Task328 P3 的 class 方法去虚化承担：`final class` 的方法和没有被任何子类覆写的方法直接调用，其余经虚表。整体评审 L3 改为不采纳 | L3（不采纳）；用户 2026-10-20 |
| G2 | `scope` 只表示结构化并发 | 见 G2 小节：默认一个失败全部取消；isolated 失败互不影响，只汇总业务错误为 `TaskFailures<E>`，panic 独立传播；E=Never 保留免 `try`；删除 `linked` | L5；用户 2026-10-20；2026-10-21 明确 panic 通道 |
| G3 | 整数溢出：语义不变，增加两种检查 | 见 G3 小节：所有配置下都环绕；静态溢出审查（`[check] overflow`，默认 `off`）；可选运行时溢出检查（`overflow-checks`），冒烟门和 CI 默认打开；`wrapping*` / `checked*` / `saturating*` / `overflowing*` 扩展到全部定宽整数。取代原 G3"默认 panic" | L6（改判）；用户 2026-10-20 |
| G4 | 泛型实参按语法消歧，不依赖空白 | 采用 C# 的规则：`<` 之后的内容能解析为类型实参列表，并且与之配对的 `>` 后面紧跟 `(`、`)`、`]`、`}`、`:`、`;`、`,`、`.`、`?`、`==`、`!=` 或行尾时，按类型实参解析；否则按比较运算解析。删除规范 §1.2"`<` 前有无空白决定含义"的规则。`a < b > (c)` 按规则解析为泛型调用；`b` 不是类型或 `a` 不是泛型时报错，提示写成 `(a < b) > (c)`。类型位置的写法不变，嵌套泛型的 `>>` 照旧按语法位置拆分。格式化器统一输出：泛型不加空格（`Array<i64>`），比较运算两侧加空格（`a < b`），格式只是风格，不影响含义 | L7；用户 2026-10-20 |
| G5 | 正则字面量改为 `re"..."` | 与原始字符串 `r"..."` 同一种词法形式：`re` 后面紧跟双引号时才是正则字面量，单独的 `re` 仍是普通标识符；内容按原始字符串处理，不解释转义。标志写在正则内部：`(?i)` 忽略大小写、`(?m)` 多行、`(?s)` 点号匹配换行，不再有后缀标志。删除 `g` 标志，全局匹配用 `findAll`、`replaceAll`，首个匹配用 `find`、`replace`。字面量在编译期解析并预编译，写错时报编译错误并指出位置；动态拼出的正则用 `Regex(text)`，在运行时检查。`/` 只表示除号，§1.2.1 续行规则中与 `/` 有关的特例删除。本条取代 Task288 的 `/pattern/ims` 写法，288 的编译期 RegexPlan 语义保留 | 评审 §3.8；用户 2026-10-20 |
| G6 | 匿名函数维持两种形式 | 保留箭头 lambda（`x => expr`、`(x: T) -> expr`、`(x: ref T) -> { ... }`）与 fn lambda（`fn(x: T) -> R { ... }`、`fn<T>(x: T) -> T { ... }`），不引入 `=>`（2026-11-14 起由 [Task343](../../../../xray-docs/tasks/343-type-arrow-and-value-arrow-cn.md) 取代：lambda、`match` 与 `select` 分支改用 `=>`，`->` 只写类型；两种 lambda 形式保留）。fn lambda 补上 `ThrowsClause`：`FnExpression ::= 'fn' GenericParams? '(' ArrowParams? ')' ThrowsClause? ('->' Type)? Block`。箭头 lambda 不书写 `throws`，按 Task329 §2.5 由函数体局部推导，或取自期望的函数类型 | 评审 §3.8；用户 2026-10-20 |
| G7 | `go` 的写法：维持现状 | 不增加 `go { ... }` 块形式；`go` 后面仍只接受调用表达式，包括 `go fn() { ... }()` | 用户 2026-10-20 |
| G8 | 构造器：维持现状 | 不自动生成逐成员构造器；保留"`init` 参数与字段同名时省略类型、按字段推断"的规则 | 用户 2026-10-20 |
| G9 | 删除计算属性（沿用 Task287） | 删除 `name: T { fn() { ... } }` 形式的 getter / setter、接口中的属性签名，以及 `obj.p` 隐式调用访问器；统一为显式无参方法，接口只声明方法，例如 `interface HasArea { area() -> i64 }`。普通存储字段不受影响。现有 49 处用法（其中 stdlib 27 处）改写为方法 | Task287；用户 2026-10-20 |
| G10 | 值传递与所有权规则统一（2026-10-20 修订） | 按设计记录[所有权与零开销模型](../../../../xray-docs/design/native/ownership-zero-overhead-cn.md)与 Task328 A16 定稿：默认仍得到逻辑副本（值语义加 COW）；`copy x` 改为表达式（`copy` 是上下文关键字，操作数必须是位置），删除 `copy(x)` 函数形式；`move x` 与 `move` 参数、`move` receiver 扩展到所有非平凡值；新增严格模式注解 `@strict`（第一版子规则 `copy`）。改写规范 §2.14.5、§5.2.4、§7.3、§7.4、§10.6、§10.8、§10.11 中相互矛盾的内容。本条初版"删除 `copy()`、`move` 只用于不可复制资源"已被取代。2026-11-13 增加原地遍历 `for (ref x in xs)` 与 `Array.swap(i, j)`，并定下 `ref` 实参的静态重叠规则（[决策记录](../../../../xray-docs/analysis/313-decision-exclusive-access-2026-11-13-cn.md)）。2026-11-13 定：规范的所有权部分现在一次收敛，删除旧模型条文、写入已定规则（未实现的标 §17 资格、不放示例），生成物与标准库内建 `copy(...)` 同批改，逐节清单见[规范收敛决策](../../../../xray-docs/analysis/331-decision-ownership-spec-convergence-2026-11-13-cn.md) §4 | 评审 §3.2、§3.4；用户 2026-10-20 确认 |
| G11 | 并发 API 精简 | 见 G11 小节 | 评审 §3.6；用户 2026-10-21 |
| G12 | 关键字表 | 见 G12 小节 | 评审 §3.2、§3.8；用户 2026-10-21 |
| G13 | 规范只写用户可见的合同，示例全部执行 | 见 G13 小节 | 评审 §3.3；用户 2026-10-21 |
| G14 | `match` 分支之间必须写逗号（沿用 Task287） | `p => e, q => e`；末尾逗号可写可不写，格式化器统一补齐。删除"分支之间省略逗号"时依赖换行和下一个 token 的猜测规则。`..=` 与 `?[` 保留（Task287） | Task287；用户 2026-10-21 |

### G2 `scope`

用户明确裁决：**“保留免 try，panic 只走 panic 通道，不进入 TaskFailures 汇总”**。该规则适用于全部 isolated。

设 B 为主体经局部 catch 后实际逃逸的业务错误集合（含 handler 自身逃逸），E 仅为直接 go 成员的业务错误并集。

| 行为 | 普通 scope | isolated scope |
|---|---|---|
| 子成员首次 Error/Panic | 发布时立即请求兄弟取消 | 不因子失败取消兄弟 |
| 主体 Error/Panic 或向外正常控制转移 | cancel+drain 全部已接管成员 | 相同 |
| 结束仲裁 | 全部 drain 后普通 Panic 优先；同类取首个线性化发布者 | 全部 drain 后普通 Panic 优先，其次直接传播主体 B，否则仅汇总子业务 Error |
| 完整静态业务错误 | B ∪ E | B ∪ TaskFailures<E>；E=Never 时不产生子汇总错误通道 |
| 无业务错误 | B/E 均 Never 时免 scope 前 try | 相同；可能 panic 不增加 throws |

- 子成员是块内（不含嵌套函数）直接 go；块内无 go 时提示改为普通块。删除 linked scope、linked go 和上下文关键字 linked，块外 go 仍独立。
- 首发布指同步点上的线性化顺序，不是墙钟、worker ID 或源码顺序。Error→Panic 与 Panic→Error 均最终传播 Panic；更早 Error 已触发的普通 scope 取消继续生效。不能在首个 Error 到达时提前结束 join。
- isolated 的子业务错误按实际成功接管成员的注册序号升序汇总。循环产生不同成员，序号不得回绕重用；主体 B 不伪造为子成员。次级 Error/Panic 均保留独立拥有式诊断，Panic 不进入 TaskFailures。
- 隐式 join 不增加 Cancelled；子成员因 scope、父或独立取消而结束都不算失败，独立子取消不取消父或兄弟。主体显式 await 被取消成员仍按 Task329 E5 观察类型化 Cancelled，它可能进入 B。
- 父已在合法检查点进入自身取消展开时，cancel+drain 后继续不可捕获的父取消。请求标志不直接改帧，也不替换已发布终态；不新增回边取消检查。cleanup 内可局部捕获 panic；逃逸时按 [Task329 §2.8](../../../../xray-docs/tasks/329-errors-in-signatures-cn.md#28-panic) 处理：正常离开或业务错误展开时成为当前可捕获的 panic，并拥有被取代的业务错误；只有已有 panic 或取消正在展开时才是不可捕获的双重故障。独立程序在双重故障时退出 70；CTFE 的单个 cleanup panic 按普通 panic 报告编译失败，双重故障才返回 CLEANUP_PANIC，两者均保住宿主。
- try scope 只标记退出时的隐式传播，不建立 try 块保护区。主体调用仍须 try/try?/try! 或位于真实 try 主体，保护不跨嵌套函数或同级 handler。普通 catch 和 try? 不得借聚合吞 Panic。
- 向外 return/break/continue 先成为拥有式 pending 转移，保活返回值、主体帧及有效借用，cancel+drain 后才履行；更高优先级失败覆盖时恰好释放 pending 值。
- scope(...) 本轮只定义 isolated；不为 nominal TaskFailures<Never> 发明全局 Never 别名。

最终逻辑协议及可控直接测试见[Task/scope 调度器接口](../../../../xray-docs/design/vm/task-scheduler-interface.md)。a/c/d 已共同接受语义、等待交接和拥有责任；C 布局、函数签名、原子算法及实现资格未冻结。先交最小执行器，再由 VM/前端/AOT 接入，不等待 Task331 全阶段。

### G3 整数溢出

**语义（与现行规范一致）：**

- 定宽整数的 `+`、`-`、`*` 和取负在所有配置下都按位宽环绕；
- 除零始终 panic；`MIN / -1` 按现行规范，结果为 `MIN`；
- 移位的位数按位宽取模；
- 编译期常量溢出仍在编译期报错。

**显式方法：** `wrapping*`、`checked*`、`saturating*` 从 `i64` 扩展到全部定宽整数；新增 `overflowing*`，返回 `(值, 是否溢出)`。

**静态溢出审查：**

- 值范围分析先排除能证明不溢出的运算，例如常量运算、以数组长度为上界的循环变量、窄类型加宽后的运算、有 `if` 边界保护的分支，只报告剩下的位置。
- 开关：命令行 `xray check --overflow`，或在 `xray.toml` 中设置 `[check] overflow = "off" | "warn" | "error"`，默认 `off`。
- 用户确认后有三种处理：
  - 确实需要环绕，改写为 `wrapping*`；
  - 需要处理溢出，改写为 `checked*` 或 `saturating*`；
  - 确认不会溢出，用 `@allow(overflow)` 标注。
- 局限：值范围分析证明不了的地方仍会报告，所以默认关闭，由需要的项目打开。
- 先例：Rust clippy 的 `arithmetic_side_effects`（默认关闭），MISRA C 的整数规则。

**运行时溢出检查：**

- 开关：命令行 `--overflow-checks`，或在 `xray.toml` 中设置 `overflow-checks = true`。
- 开启后，环绕运算发生溢出时 panic，并报告源码位置；显式调用的 `wrapping*` 不受影响。
- VM 与原生代码都支持；差分测试在同一配置下比较两者。
- 发布构建默认关闭；Task327 的冒烟门和 CI 默认开启。
- 先例：Rust 的 `overflow-checks`，C/C++ UBSan 的 `-fsanitize=integer`。

**实现位置：** 值范围分析放在 Checked XIR 上，旧的 `src/ir/xi_range.c`（609 行）可作参考；运行时检查在 LIR 上插入（Task328），VM 读取同一个开关。

### G11 并发 API 精简

当前已交付typed await的E|Cancelled、最小定向cancel和TaskResult四态/poll/awaitResult，真实Source、VM、生成C/native及SDK消费者见[STATUS](../../../../xray-docs/STATUS.md)。poll不驱动任务；TaskResult.Panic本地构造也要求完整图可复制且Sendable，按[Task329 §2.6](../../../../xray-docs/tasks/329-errors-in-signatures-cn.md#26-接口覆写与-task)统一检查。Task.all/first/firstSuccess/timeout、yieldNow/isCancelled及完整结构化并发/取消模型仍须各自完成，不能以这批句柄消费者关闭整个G11。

**1. 等待多个任务改为库函数。** 它们是挂起函数，直接返回结果，前面不写 `await`；通过 intrinsic 注册表（Task327 R2.6）映射到现有的聚合等待实现，不新增 opcode。

| 函数 | 返回 | 错误类型 | 其余任务 |
|---|---|---|---|
| `Task.all(ts)` | `Array<T>`，顺序与输入一致 | `E` | 失败时先请求取消其余，再传播；不等待清理 |
| `Task.first(ts)` | 最先结束的任务：成功时返回它的值，失败时抛出它的错误 | `E` | 先请求取消其余，再返回或传播；不等待清理 |
| `Task.firstSuccess(ts)` | 第一个成功的值 | 全部失败时抛 `TaskFailures<E>` | 先请求取消其余，再返回或传播；不等待清理 |
| `Task.timeout(t, ms)` | `T` | `E \| Timeout` | 超时后取消 `t` |

错误类型列已按[等待与取消传播决定](../../../../xray-docs/tasks/331-language-surface-convergence-cn.md#await-cancel-propagation-and-closed-send-2026-11-02)去掉 `Cancelled`。

- 输入必须是同一类型的 `Array<Task<T, E>>`；类型不同的任务，用 `scope` 加逐个 `await`。
- 原 `await any` 改名为 `Task.first`，避免与 JS 中含义不同的 `any` / `race` 混淆。现行规范中 `await any` 与 `await anySuccess` 的描述实际相同，由这两个函数明确区分。
- `TaskFailures<E>` 是内建的业务错误汇总类型，由 `try scope(isolated)` 和 `Task.firstSuccess` 共用（G2 讨论中的 `ScopeFailures<E>` 改名为此）。它逐个列出发生业务错误的任务：调试名称（`go(name: ...)`）、位置、错误值 E；不包含 `PanicInfo` 或 Cancelled。scope 的位置是实际成员注册序号，库聚合函数的位置是输入位置，scope 排序已按 G2 冻结，库函数按输入位置。panic 保持独立通道；`Task.firstSuccess` 的成功与 panic 竞争、全部未成功时的混合选择须在实现前明确，不从汇总类型反推。

<a id="task-aggregate-return-and-cleanup-2026-11-02"></a>
**聚合返回与清理（2026-11-02 用户决定）。** 本条冻结调用返回的时机，实施仍须完成下述验收，不构成功能已交付声明。

- `Task.first`、`Task.firstSuccess` 及 `Task.all` 的失败分支按各自规则选定结果后，先向其余尚未结束的输入任务发出取消请求，再返回或传播；不把余任务终结、`defer` 或资源清理完成作为返回前置。成功的 `Task.all` 仍等待全部输入成功，并按输入顺序返回。
- 余任务继续由原执行器持有、驱动至终态，保留原有 scope 成员关系。聚合调用不转移或解除 scope 的接管；scope 退出前仍等待全部成员终结和清理完成。原生/VM均不得保留调用者短命帧、视图或请求的借用以代替拥有式运行状态。
- 余任务后续结局不追溯改写已返回的聚合结果；后续业务错误、panic及清理失败按原归属与各自通道处理，不能因早返回丢弃。当前等待者自身的不可捕获取消展开规则不变。合作式取消不承诺抢占或固定返回时限，根执行器的对外完成仍按其完整收尾规则。
- 本决定不改变 `Task.timeout`，也不补定下述 `Task.firstSuccess` 成功与panic竞争或全部未成功的混合仲裁；这些边界不能从本条推定。

实现沿本节现役intrinsic库调用接缝：Source准入真实普通CALL，现行执行消费者接canonical begin与既有TASK_AWAIT，不新增聚合opcode，不以Task328 P4为前置，不保留旧await修饰词的兼容路径。

退出条件：用真实任务握手证明取消请求先于聚合返回；余任务仍未完成清理时调用者可以继续；同一scope的出口必须等清理完成。覆盖first成功/失败、firstSuccess选中成功及all失败、all成功的输入顺序、原scope归属、重复观察和取消/清理失败后的拥有式释放。采用可控调度与握手，不以sleep阈值证明时序；真实Source、VM和C/MSVC原生输出一致，实际ASan＋UBSan及受影响L2按D6一次完成。未决结果仲裁的正负例在其规则冻结后才能授完整firstSuccess资格。

<a id="await-cancel-propagation-and-closed-send-2026-11-02"></a>
**等待的错误类型、取消传播与关闭后发送（2026-11-02 用户决定）。** 本条取代本节第 1 条表格原有的 `Cancelled` 列、第 3 条“等待它的一方看到类型化的 `Cancelled`”及 Task329 E5 的 `E | Cancelled`。xray 规范源（`spec/source/sections` 的 §2.2.2、§8.0、§8.1.5、§8.1.6、§10.1–§10.7、§14、§16.4、§16.9、§18 与 MCP 知识卡片）已同步，**本节与Task336的现役规则已在2026-11-03随代码main `89930974e` 完成Windows接收**。取消传播、关闭发送、timeout 与 E0336 的实测及 panic-only try 定界见[实施记录](../../../../xray-docs/analysis/task-331-await-cancel-implementation-2026-11-03-cn.md)。用户在“取消时 panic”“取消向等待方传播”“维持 `E | Cancelled`”三者中选定第二项；完整G11及firstSuccess混合仲裁仍未关闭。

2026-11-03 用户在阅读[错误与 panic 的方案 A/B 深度比较](../../../../xray-docs/analysis/error-panic-scheme-a-vs-b-2026-11-03-cn.md)后选择 **A**，并要求吸收 B 中适用的改进。实施规则、诊断、性能约束及验收矩阵见 [Task336](../../../../xray-docs/tasks/336-error-panic-scheme-a-implementation-cn.md)。panic-only try、严格业务错误来源判定及规范生成已同批接收；未来性能约束不据此取得实现资格，不解冻 P4。

1. `await t` 的业务错误类型只有 `E`，`E = Never` 时不写 `try`。`Task.all`、`Task.first` 为 `E`，`Task.firstSuccess` 全部失败时为 `TaskFailures<E>`，`Task.timeout` 为 `E | Timeout`。任何等待的业务错误类型都不再含 `Cancelled`。
2. 被等待的任务以 `Cancelled` 结束时，等待方在该 `await` 处进入自身的取消展开：执行 `defer`，`catch (e)` 与 `catch panic` 都不能捕获，自身终态为 `Cancelled`，再按同一规则影响它的等待者（Kotlin 式传播）。聚合函数遇到非自己请求的输入取消时同样处理：`Task.all` 任一输入被取消、`Task.first` 最先结束的是取消、`Task.timeout` 的 `t` 在超时前被取消。被取消的输入如何参与 `Task.firstSuccess`，随其未决的混合结果仲裁一并冻结。
3. `awaitResult()` / `poll()` 不变，以 `TaskResult.Cancelled` 值报告且不传播；这是观察取消而不传播的唯一途径。
4. 默认 scope 因成员失败而取消其余成员后，主体在等待其中某个成员时进入的取消展开止于 scope 边界，scope 照常 drain 并按 G2 传播首个失败。父任务自身被取消、或成员被 `t.cancel()` 独立取消时不吸收，继续向外传播。
5. 取消展开到达逻辑根任务时，独立程序在 stderr 打印 `[Uncaught Cancellation]` 并以退出码 1 结束（与未捕获的 Error、panic 一致），嵌入宿主只结束本次执行。本条由评估补定，用户可另行改判。
6. `Channel.send` 与 `select` 发送分支对已关闭的 channel 产生 `E0702` panic；`send` 不再声明业务错误。`trySend` / `sendTimeout` 仍返回 `SendResult.Closed`。2026-11-10 起 Channel 拆为 `Sender<T>`/`Receiver<T>`，这条规则作用于已被 `close()` 的发送端；接收端全部消失后 `send` 取消发送任务（[Task335](../../../../xray-docs/tasks/335-coroutine-execution-and-scheduler-cn.md) §6.16）。
7. **多余的 `try` 是编译错误 `E0336`（2026-11-02 决定；2026-11-03 选择 A 定界，不做兼容）。** `try`、`try?`、`try!` 的作用范围没有业务错误来源时报错；含普通 catch 的 `try { }` 主体同样检查，即使还含 `catch panic` 也不豁免。**仅含 `catch panic` 的块作为 panic 捕获边界合法，不要求主体具有业务错误来源**；块内的 try 表达式仍独立检查，业务错误仍须满足外层处理或所在函数签名。来源包括可能抛业务错误的调用、`await`、`scope` 和直接 `throw`；按定义处声明与实际求值规则判断，`Never` 不算，含未替换类型参数的 E 算可能抛错，泛型体不因某次实例化为 `Never` 而误报。因此 `Task<T, Never>` 上的 `try await t`、新 `send` 上的 `try ch.send(x)` 直接报 E0336。区域内有来源不等于 handler 可达，不把内层已消费错误的死 handler 分析混入本诊断；完整词法范围、默认值与 CTFE 规则见 [Task336 §2](../../../../xray-docs/tasks/336-error-panic-scheme-a-implementation-cn.md#2-唯一的源码与通道规则)。

用户知情的代价：`t.cancel()` 之后再 `await t` 会让调用链一路取消到根，中途没有诊断，只在根处报告；需要观察取消的代码改用 `awaitResult()`。

**实施**（R3 产品项，载体为现役 Source、共同检查、XIR VM 与 Checked→C，不等 Task328 P4，不新增 opcode）：

- 前端：TASK_AWAIT 与聚合库函数的错误集合删去合成的 `Cancelled` 成员；新增第 7 条的 `E0336` 检查（`XR_ERR_ANALYZE_REDUNDANT_TRY`，登记到 `src/runtime/xerror_codes.h` 的 330–349 段）。多余的 `try` 现行可以编译（已用 `3b72a1f81` 实测），`E0336` 接通后树中所有多余的 `try` 都会报错，须在同一提交内全部删去。
- 运行时：等待观察到子任务 `Cancelled` 时转入当前执行自身的不可捕获取消展开，复用已有路径，不再合成业务错误；scope 边界按第 4 条识别“本 scope 因成员失败请求的取消”；根任务输出未捕获取消诊断与退出码。
- Channel：`send` 对已关闭 channel 走 `E0702` panic 通道；标准库声明去掉 `send` 的业务错误。
- 迁移（同批原子切换，不留兼容路径）：demos/05-concurrency 的 goroutines、shared_data、select_and_scope 中的 `try await`、`throws(SendResult)` 与 `try ch.send`，README/README_CN 的 #xray-3，标准库中的 `send` 调用，以及依赖 `E | Cancelled` 或捕获 `SendResult.Closed` 的测试夹具（typed_await、task_result、error_union、SDK consumer 等）。按 Task327 §0.2 的先例，这是授权的源码迁移：算法与独立预期输出不变，只改写法。实测原 demos/README/stdlib 中 `try await` 7 处、`try …send` 10 处；其余多余的 `try` 以接通 `E0336` 后的编译结果为准逐一删去。
- `tests/mcp/test_knowledge_generation.py` 的 explain-error（`ch.send(42)` 不写 `try`）与 concurrency-pattern（`Never` 任务的 `await` 不写 `try`）两个提示词示例本来就按新写法编写，在 `3b72a1f81` 上都因 E0330 失败，是该门在 macOS 上仅有的两项红；实施后应转绿。

**退出条件**：E=Never 的 `await` 无 `try` 通过，E 非空仍要求标记；子任务被取消后 `await` 使等待方执行 `defer`、不可捕获、其等待者继续传播、根退出码 1 并输出诊断；`awaitResult()` 观察 `Cancelled` 不传播；聚合函数三种情形与 scope 吸收/不吸收两种情形均有正负例；关闭后 `send` 及 `select` 发送分支产生 `E0702`，`trySend` / `sendTimeout` 不变；`E0336` 对 `try`、`try?`、`try!`、`try { }` 四种形式各有正负例，泛型体中错误类型为类型参数的调用不误报。以上 VM 与原生输出一致；迁移后原并发 demos 与 README 例双侧通过、黄金输出不变，冒烟棘轮与首诊断不降。

2026-11-03 选择 A 后，E0336 退出还须覆盖仅 panic handler 合法、混合 handler 不豁免、内部多余表达式仍拒绝、直接 throw 与嵌套区域、业务 E 穿过 panic-only 边界的签名检查；以 [Task336 §5 验收矩阵](../../../../xray-docs/tasks/336-error-panic-scheme-a-implementation-cn.md#5-验收矩阵)补齐证据，不以原四种形式的粗粒度通过代替。

**2. Task 句柄。**

- 保留 `await t`、`cancel()`、`poll()`、`awaitResult()`。
- 删除 `done`、`status`（连同 `TaskStatus`）和 `awaitTimeout(ms)`。
- `poll()` 返回 `TaskResult<T, E>?`，任务未结束时返回 `null`。
- `TaskResult<T, E>` 只有四个变体：`Success(T) | Error(E) | Panic(PanicInfo) | Cancelled`。删除 `Pending` 和 `Timeout`：`awaitResult()` 不会返回它们，保留只会让每个 match 多一条走不到的分支。

**3. 取消模型。**

- 只在挂起点检查取消：`await`、Channel 操作、`select`、`Task.yieldNow()`、sleep 与 IO（Task328 已定）。删除现行规范 §10.4 中"后向跳转"这个检查点。
- 被取消的任务在挂起点开始展开：照常执行 `defer`，但不能被 `catch (e)` 或 `catch panic` 捕获，也不写进函数签名。
- 用 `await` 或聚合函数等待它的一方自身进入取消展开，取消沿等待链传播；`awaitResult()` / `poll()` 以 `TaskResult.Cancelled` 报告而不传播（2026-11-02 用户决定，见[等待与取消传播](../../../../xray-docs/tasks/331-language-surface-convergence-cn.md#await-cancel-propagation-and-closed-send-2026-11-02)，取代原 Task329 E5 的 `E | Cancelled`）。
- `cancel()` 幂等（Task316 §5.1）。
- `Task.isCancelled()` 用于长循环中主动检查、提前收尾。

**4. 改名。**

- `Coro.yield()` 改为 `Task.yieldNow()`，不再与生成器的 `yield expr` 撞名；
- `cancelled()` 改为 `Task.isCancelled()`；
- 删除 `Coro` 命名空间。
- 先例：Swift 的 `Task.yield()` 与 `Task.isCancelled`。

**5. Channel 必须给出元素类型。** 写 `Channel<T>(n)`，或者提供类型上下文，例如 `const ch: Channel<i64> = Channel(3)`；删除"从首次 `send` 推断"。这与空数组 `[]` 需要类型上下文的规则一致，Task330 的类型化 HIR 也不必为它做跨语句推断。

### G12 关键字表

以编译器的 `src/frontend/lexer/xkeywords.def`为准核对规范 §1.5：

| 处理 | 项 | 说明 |
|---|---|---|
| 删除保留字 | `new` | 从未使用，构造统一写 `T(...)`。删除后可作普通名字；解析器遇到 `new T(` 时提示去掉 `new` |
| 删除上下文关键字 | `default` | 标为"保留"，从未启用 |
| | `linked` | 随 G2 删除 |
| | `cancelled` | 随 G11 删除，改用 `Task.isCancelled()` |
| | `await` 后的 `all`、`any`、`anySuccess` | 随 G11 删除 |
| 保留 | `super`、`protected`、`final`、`extends` | 随 G1 维持现状 |
| | 上下文关键字 `override`、`init`、`ref`、`move`、`from`、`to`、`after`、`panic` | 不变 |
| | `yield` | 今后只用于生成器 |
| 收窄 | `where` | 只用于条件方法：泛型类型的方法用 `where` 约束外层类型的参数（例如 `Atomic<T>` 的 `add` 要求 `where T: AtomicNumber`）。函数自己的类型参数只能写 `<T: C>`。`where` 由保留字改为上下文关键字。这修正了 Task287"整个删除 `where`"的结论：规范 §9 在 2026-10-05 加入的方法级约束无法用 `<T: C>` 表达。先例：Swift 的方法级 `where`、Rust 的 `impl<T: Num>` |
| 新增上下文关键字 | `copy` | G10 |
| | `weak` | 字段修饰（Task328 A13） |
| | `throws` | Task329 |
| | `nosuspend` | 函数类型前缀，如 `nosuspend fn(i64) -> S`（Task328 A5）；仅类型语境识别，普通同名identifier不全局保留 |
| | `isolated` | `scope(isolated)`（G2） |
| 不是关键字 | `re"..."`、`r"..."` | 由扫描器在"后面紧跟引号"时识别，`re`、`r` 仍是普通名字 |
| 注解表（§12.5） | `@strict`、`@allow(overflow)` | G10、G3 |
| 类型名 | `bool`、`string`、`rune`、`i8`…`f64` 共 15 个 | 都不作词法关键字，统一放进内建类型注册表，与 `Array`、`Channel` 一样不可重新声明。`bool`、`string`、`rune` 移出 `xkeywords.def`，规范不再把数值类型列为保留字 |

该nosuspend源码切片已贯通实际alias/E替换、AST身份/格式化重解析、现役A2保证及SourceSDK退休后执行；普通同名identifier和未承诺的fn值规则保持。完整parallel导入入口在VM/native严格输出1LF，未据此关闭其它G12关键字迁移；当前资格统一见STATUS。

其余修正：

- §1.5.1 中 `const` 的说明删除"在类型位置表达深只读能力"（§2.14 已删除类型位置的 `const T`）；
- `defer` 不再重复列出；`unsafe` 从"协程与并发"组移到单独的底层能力组；
- §1.5 的保留字表从 `xkeywords.def` 生成，或由测试比对两边，规范不再手写个数。

以上是已接受的目标分类变更；当前保留集和数量由实际词法声明生成，不在任务规则中手写目标总数。

### G13 规范只写用户可见的合同，示例全部执行

现状：规范源 `spec/source/sections` 中英文合计 18,402 行；§17（`018-17-compilation-pipeline.md`）514KB、2,494 行，几乎都是 XIR 实现合同；每种语言 219 个 xray 代码块，其中 204 个带 `@id`，但 `@id` 只用于生成 MCP 知识卡，代码块从未被执行；中英文代码块去掉注释后 214 个相同、5 个不同；19 个代码块含错误示例，其中 8 个把正确和错误的写法混在一起。

1. **只写用户可见的合同。**
   - §17 缩为几页"编译与执行模型"：VM 与 AOT 语义相同、哪些错误在编译期发现、`comptime`、freestanding 与沙箱的约束。
   - 现有实现合同原样移到 `xray-docs/design/xir/pipeline-contracts-cn.md`，现在不改写（Task328、Task330 会重构这条管线）。
   - §16 运行时模型同样处理：引用计数、调度保证等用户可见的部分留下，其余移走。§18 错误码保留。
2. **实现术语由门禁检查。**
   - 正文禁用：准入、资格、Checked、Lowered、Program、Instance、verifier，以及 commit 哈希、"当前新 XIR"之类的说法。
   - `owner` 不禁用：它是所有权模型中用户可见的概念（Task284、G10），在附录 F 术语表中按用户含义定义；指运行时对象的用法改写。
3. **实现进度不手写。** 不建手写的 `STATUS.md`；按冒烟门结果生成功能支持表，列出每个示例在 VM 和 AOT 上是否通过。
4. **代码块标注。** 每个代码块必须带下列标记之一，不带标记的代码块使门禁失败；`@id` 照常保留，与这些标记互不影响。

   | 标记 | 含义 | 门禁检查 |
   |---|---|---|
   | `@run` | 后面紧跟一个 `output` 块，写期望的标准输出 | 在 VM 和 AOT 上分别运行，输出都与期望完全一致 |
   | `@error=E0382` | 必须编译失败并报该错误码；出错的那一行标 `// error` | 一个代码块只放一个错误 |
   | `@check` | 必须通过编译，但不运行（网络、文件、常驻服务等） | 只做编译检查 |
   | `@fragment` | 不完整的片段 | 只检查能否解析；门禁报告数量，目标是逐步减少 |

   - 正确与错误写法混在一起的代码块拆开。
   - 尚未实现的功能，其示例先登记在 Task327 R1.1 的棘轮清单中，清单只减不增。
5. **中英文一致。** 门禁执行中文那一份代码块，检查英文代码块去掉注释后完全相同、期望输出完全相同；先修复现有 5 处不一致。
6. **顺序。** 先移走 §17、§16 的实现部分并接通标注与门禁机制，再写入 G2–G14 的改动，最后补全示例标注。

### 1.1 非目标

- 不去掉 `if (…)`、`for (…)`、`match (…)`、`catch (…)` 中的括号（收益低、改动面大）。
- 类方法的书写方式保持现状，与 Task329 的文法一致。
- `len(x)` 继续作为内建函数，不改为属性。
- import 的两种写法（`import time` 与 `import "std/io/output"`）暂不统一。
- 是否保留 structural object 类型（`{ f: T }`）不在本任务讨论，按现行规范实现（Task327 F10）。
- 错误模型由 Task329 负责。引用环沿用 Task247 的纯引用计数模型（不做运行时环收集），迁移到新架构由 Task328 A13 负责。
- 类继承的实现不在本任务内（G1 维持现状，由 Task327 F03 实现）。

# 已接受目标规则：313 - 值语义、所有权、callable 与模块状态合同

> 2026-11-16 从任务文档迁入统一规范源。此处维护已接受的目标定义；原任务只维护实施、依赖、理由与验收记录。
> 迁入不表示实现完成。下面保留的示例是目标验收材料，不能据此声称当前编译器、VM 或 AOT 已支持。历史修订和已明确被取代的规则保持历史身份；不能将旧版本重新激活。
> 实施任务：[313-value-ownership-callable-and-module-contracts-cn](../../../../xray-docs/tasks/313-value-ownership-callable-and-module-contracts-cn.md)。

## 2. 必须落实的语义

匿名记录与结构约束已由 [Map/名义类型决定](342-remove-structural-objects-map-braces-named-arguments-cn.md) 删除，下面只保留仍有效的类型域。

### 2.1 值、身份与 const

1. Array/Map/Set/string、struct、enum 按值复制；COW 只改变物理共享，不改变逻辑副本。
2. 值语义止于 class 和同步身份边界；复制 struct 的 class 字段只复制引用。
3. const 固定绑定及其值语义部分，不能称为整个可达图深度不可变；`const xs = [Counter()]` 不冻结 Counter。
4. class 方法不写 ref/move；只读 class 参数可调用修改对象的方法，但不能重新绑定参数。
5. struct/enum 的修改方法显式 ref；只读值参数可保存副本，不能修改该参数或泄出借用。
6. class 字段的 ref/视图访问动态检查独占并保活对象；局部无冲突证明只可省检查。
7. 普通闭包的 var 捕获共享 cell，const 按值捕获；closure→cell→closure 也可能成环，纳入 315。

### 2.2 Sendable 是完整类型性质

1. 可复制值按所有字段/元素的 Sendable 性质推导；普通可变 class 不成立。
2. class 显式 implements Sendable，构造后字段不能重新赋值且字段类型必须 Sendable；同步字段允许内部受保护状态变化。
3. Sendable 基类的所有派生类必须继续满足该约束；上转型不能隐藏新增的可变字段。
4. Mutex<T>/RwLock<T>/Channel<T> 的公开载荷要求 T: Sendable；Atomic 还受其可原子操作类型集合约束。
5. `Mutex(Counter())` 不能自动使已有别名安全；首版不提供包住非 Sendable class 后跨协程的特例。
6. 同步类型自身的 unsafe 实现须有独立审计 owner；“原语可信”不等于任意泛型实参可信。
7. go 的参数与捕获必须 Sendable；禁止捕获 var。scope 缩短生命周期不能免除并发安全条件。
8. sendable fn 只能安全捕获 Sendable 值；可转换为普通 fn，反向转换不得靠运行时猜测。
9. 跨任务边界不要求唯一性证据（2026-11-13 决定，[决策记录](../../../../xray-docs/analysis/313-decision-cross-task-boundary-2026-11-13-cn.md)）。引用计数分本地带与共享带，本地带对象只被一个任务可达。值在 `go` 实参与捕获、Channel 发送、Task／scope 结果、存入同步句柄、模块 const 发布、TaskLocal 提供值等提升点由发布方提升到共享带，遇到已共享的对象即停。共享带的对象不写：值类型对象需要独占修改时，计数为 −1 先降回本地带，否则写时复制。实现可以对不含身份的值改用复制，身份类型必须共享。
10. `Mutex<T>` 只有 `lock<U>(body: fn(ref T) -> U) -> U` 与 `tryLock<U>(body: fn(ref T) -> U) -> U?`；`RwLock<T>` 有 `read<U>(body: fn(T) -> U) -> U` 与 `write<U>(body: fn(ref T) -> U) -> U`；两者删除 `replace`。body 是不逃逸参数，原地修改槽里的值，不标记中毒（2026-11-13 决定，[决策记录](../../../../xray-docs/analysis/313-decision-mutex-in-place-2026-11-13-cn.md)）。

### 2.3 noncopyable、move 与析构责任

1. 仅 struct/enum 可声明 noncopyable；含不可复制字段/载荷的 struct/enum 必须不可复制。
2. 赋值、返回和 move 参数/receiver 转移持有权；不可复制值可用可选 move x 表达式显式标出该转移，调用仍由声明模式决定。只读与 ref 参数借用，不转移持有权。
3. move 转移析构责任，不立即析构资源；`forward(u: move Uart) -> Uart` 返回后仅最终持有者负责释放。
4. 最终持有者正常离域时执行 deinit 一次；未初始化、部分初始化、异常与取消的责任表交 315/316 实现。
5. 禁止作为普通泛型实参、集合元素或接口实现者，Sendable 标记除外；Task 结果必须可复制且 Sendable。
6. 因而 `Uart?`/Optional<Uart>、Channel<Uart>、Task<Uart> 首版均非法；需要可空资源时用具名 noncopyable enum。
7. 首版允许 `go worker(resource)` 直接传给声明为 move Uart 的参数；调用成功接受实参即结束源持有权。
8. go 入队失败或启动前取消也必须释放已接管资源；不得把资源退回已失效的源变量，责任记录见 316。
9. 不可复制资源不能被普通可复制闭包按值捕获；首版拒绝捕获其消费 receiver 的方法值，只允许直接 `resource.finish()`。
10. 按 2026-10-20 Task331 G10 的后续决定，可复制类型也可声明 move 参数/receiver，显式 `move x` 转移后源绑定失效；class 方法仍不声明 receiver 模式。普通可复制值在默认模式下保留逻辑复制，自动移动只省复制，不能改变 deinit、错误或外部副作用的既定顺序。2026-11-13 补充（[释放时机决策](../../../../xray-docs/analysis/313-decision-release-timing-2026-11-13-cn.md)）：最后一次使用落在取得所有权的位置时，隐式移动与显式 `move` 结果相同，绑定此后不再持有值；其余绑定在作用域结束时按声明逆序释放，只有不含身份的值可以更早。`@strict(copy)` 的实现随 H3 冻结，不作为 R3 消费参数的前置，完整规则见[零开销所有权决定](../../../../xray-docs/design/native/ownership-zero-overhead-cn.md)。
11. 资源句柄（2026-11-13 决定，[决策记录](../../../../xray-docs/analysis/313-decision-resource-lifetime-2026-11-13-cn.md)）：标准库句柄由运行时持有的存储拥有 OS 资源，最后一个引用释放时关闭，显式 `close()` 保留；用户包装 C 资源用持有句柄与 `nosuspend` `CFn` 释放函数的标准库原语；不给 class 增加用户可写的 `deinit`。本节的不可复制资源加 `deinit` 只用于 freestanding 与驱动，标准库句柄不改成不可复制。

### 2.4 Slice/MutSlice 与返回来源

1. Slice 共享借用；MutSlice 独占借用，赋值即移动、普通调用临时重借用；创建前完成必要的 COW 分离。
2. 只读视图存活期 owner 不得修改；可写视图存活期 owner 不得读、写或复制。嵌套投影锁定所属根及必要路径。
3. class 字段借用使用动态访问令牌；被普通闭包共享 cell 捕获的变量不能产生视图。
4. 视图仅允许局部、参数、返回、tuple、Optional；包含视图的包装继承同样限制，不进入普通集合/用户聚合/go。
5. **返回来源首版只按签名消歧**：整个签名必须恰有一个视图输入或 ref 输入候选；ref receiver 也计为候选。
6. 零候选或多个候选一律拒绝返回视图，即使函数体始终返回第一个参数；不靠函数体推导跨模块来源。
7. tuple/Optional 中每个返回视图单独验证来源和可写性，均依附唯一候选；可写返回不得来自只读候选。2026-11-13 补充（[决策记录](../../../../xray-docs/analysis/313-decision-projection-and-view-source-2026-11-13-cn.md)）：没有视图参数和 `ref` 参数时，receiver 是唯一候选，只读 receiver 可返回 `Slice`，`ref` receiver 可返回 `Slice` 与 `MutSlice`；不新增返回 `ref T` 的方法，用户容器以 `modify(i, body: fn(ref T) -> U)` 这类不逃逸回调提供原地修改。
8. 返回两个 MutSlice 还须有显式不重叠构造合同；首版普通用户函数不能仅凭下标算式证明并返回可写分割对。
9. MutSlice 到 Slice 是共享重借用；派生只读视图存活期原 MutSlice 不能写入。
10. 帧稳定不等于 owner 永生。普通调用中的视图可以跨挂起；生成器首版不得保存借用参数，具体约束归 316。2026-11-13 收紧（[决策记录](../../../../xray-docs/analysis/313-decision-borrows-across-suspension-2026-11-13-cn.md)）：借用的根不在当前帧（class 字段、模块变量、共享捕获的 cell 及其元素）时，借用不得跨挂起点；同步原语建立的借用除外。
11. 捕获视图的闭包可立即调用；跨参数传入须有显式且冻结的 noescape/借用调用合同，否则拒绝。
12. 上述合同保证回调不被保存/返回/派生逃逸闭包，调用结束前完成；不得从被调方实现推导这一承诺。
13. 合同的源码承载与接口编码在 F2 冻结后方可修改相关回调声明；不在本文件发明新关键字或声称现已支持。
14. 同一次调用的全部 `ref` 实参（含 `ref` receiver）两两不得重叠，按路径静态判断（2026-11-13 决定，[决策记录](../../../../xray-docs/analysis/313-decision-exclusive-access-2026-11-13-cn.md)）：同根且在某层分到不同字段名才不相交；下标永远不证明不相交，数组用 `swap(i, j)`；不同的根经同名 class 字段到达时运行时检查是否同一对象。同一 class 对象的不同字段不相交。
15. `for (ref x in xs)`、`for (i, ref x in xs)`、`for (k, ref v in m)` 原地遍历：循环前取得容器的独占借用并完成写时复制分离，每轮的 `x` 是当前元素的 `ref`；循环体不能经其他路径访问容器或改变其长度。不写 `ref` 的 for-in 仍遍历快照。

### 2.5 callable、receiver 与效应 ABI

1. 函数类型记录有序参数类型/模式、返回类型、Sendable 承诺、借用返回映射和显式借用调用合同；2026-10-07 [322](../../../../xray-docs/tasks/322-inferred-root-execution-effects-and-library-state-cn.md)追加独立的 `requires_root` 效果上界和有限符号依赖，实施尚未完成。捕获Sendable不能替代调用权限；效果不可在赋值、返回或容器中被清空。
2. 参数模式必须一致；ref T 不变，普通值参数逆变、返回值协变只在不破坏所有权/借用合同的条件下适用。
3. `fn(ref Animal)` 不能替代 `fn(ref Dog)`；move 不能替代只读，noescape 承诺不能被更弱实现覆盖。
4. 值类型方法的 ref/move 模式由声明决定，直接 dot-call 遵循同一规则；接口 ref 方法的 class 实现可写普通方法。
5. 可复制值的只读方法值捕获逻辑副本；class 方法值捕获强引用；值的 ref 方法值只走显式借用调用合同，不可逃逸。
6. 不可复制 receiver 的消费方法值按 §2.3 拒绝；其只读/ref借用方法值仅在显式、已冻结的借用调用合同下允许，不可逃逸。不暗中生成可复制、多次调用的消费闭包。
7. 普通跨模块/间接调用采用保守可恢复 ABI；推导的 NO_SUSPEND 仅用于已知、封闭且可达的优化范围，AOT 导入 stdlib XIR 后可跨源模块精化，不改写公开承诺。
8. 已验证的公开 no_suspend 合同可选择不挂起 ABI，但必须进接口身份、override/实现兼容和链接检查。
9. 不能将实现体推导出的 no_suspend 当公开承诺；更改公开承诺是接口变化，旧调用方必须重建或拒绝链接。
10. 保留现有 xray verify 合同入口；不新增未经实现验证的 CLI 拼写。热更的进一步验证属于可选 321。
11. AOT 内部不为优化强制逐函数标注 no_suspend；析构、ISR、受限回调、泛型接口和显式 verify 仍须稳定且验证过的效应合同。公开承诺不缩减为仅 VM/native、动态库和热更三个边界。

### 2.5.1 普通间接 ref 调用的唯一物理表示（2026-10-09）

首个无捕获`fn(ref T)`沿[Cell owner合同](../../../../xray-docs/design/xir/module-state-cell-owner-cn.md#普通间接-ref-调用)实施：既有`XrXirCallableParameter`使用真实`Cell<T>`类型及`REF`模式，Source/query保留逻辑T。READ仍禁止Cell；未知/MOVE模式拒绝，不同时保留logical-T matcher或另建参数证明表。当前真实codec已经逐项保存type+mode，冻结表示不证明reader、特化、Program或runtime已接通。

实施依赖与退出门：先共同types/signature/matcher/codec/特化精确验证type+mode和REF不变性；再由真实FUNCTION_REF绑定的无捕获suffix发布SCOPED来源，完整保root/Sendable/声明上界；最后接Source indirect实参与现Call loan准备/失败释放。分别鲜构独立Source正反例、Checked往返/producer死亡、VM/native/mixed、全部FI/三轴/物理释放、SDK/旧对象和默认安全，最后随Cell semantic身份一次切换消费者。当前状态为合同已决、实施与资格OPEN。

此首窄不开放捕获ref的普通闭包、bound ref receiver、借用返回或MOVE；§2.4/§2.5要求的显式借用调用承载仍须另行冻结并完成，不能因此移出总目标。函数签名含Cell不等于捕获Cell，也不授予GO/ref运输权限；实际owner、完整root公式及Sendable承诺仍独立检查。

### 2.6 模块状态

2026-10-07目标由[322](../../../../xray-docs/tasks/322-inferred-root-execution-effects-and-library-state-cn.md)修订；当前实现仍拒绝库var，须先通过322 P1–P3对应保护退出门，再在P4原子开放并验证运行正例，随后P5完成整体验收。下列不是已经运行通过的能力。

1. 允许库模块私有顶层 var，仍禁止 export var 和库非 Sendable const；共享可变状态仍可使用 const 加合格同步类型。
2. 库和入口的模块 var、入口非 Sendable const，均归所在实例主执行流。编译器推导 `requires_root`，沿调用及函数值传播；worker入口要求无根依赖的完整证明，未知时保守拒绝。Sendable和普通var捕获规则独立保留。
3. 含私有var的库可被import；不再以该声明本身拒绝整份库。模块可见性、实例身份与初始化发布照常检查，不能把所有模块视为共享同一文件作用域。库非Sendable const禁令不随本次修订开放。
4. 根执行流属于每个程序/测试/REPL/嵌入实例，不是OS主线程。Library本身无入口，链接后其状态绑定真实实例；不能伪造root/entry或给go隐式继承根权限。
5. 模块初始化、失败粘滞、并发进入、销毁顺序由315统一实现；状态不得存进可复用Program或C进程全局。权限摘要不证明生命周期。
6. Sendable const不自动等于永久对象；外逃结果按315保活。库var开放不新增模块槽借用/裸指针跨流逃逸通道，也不把class别名转换成独立值。

### 2.7 按约束检查的普通泛型

1. 泛型声明在定义处完成名称、成员、类型、初始化、所有权、借用与 Sendable 检查；调用处证明实参满足准入和约束。实例化不得按具体类型重新寻找未在约束中声明的成员。
2. 普通类型参数的准入域为可复制、可保存的类型；noncopyable 与视图不准入。§2.4 的内建视图包装例外不开放普通 T；class 引用可复制不意味着 Sendable。赋值、复制、传参和容器保存也必须满足各自模式与容器约束，不能写成“所有类型都通用”。
3. 可用操作来自显式接口、已冻结的内置能力以及准入域保证。约束中的方法/属性必须保留声明身份、参数模式、receiver 模式、可读/可写性、借用返回来源、noescape 和效应合同。
4. 泛型成员访问在定义处绑定到约束成员身份和见证需求，实例化按已冻结规则绑定实际见证；不依赖导入顺序、裸成员名或具体类型的额外成员。交叉约束同名成员的合并、歧义、实现唯一性及继承规则须在 F2 明确；不能让单态化猜选一个实现。
5. 泛型转发必须检查约束蕴含，例如调用要求 Sendable 的泛型时，外层 T 的合同必须已能证明 Sendable；不能等某次实际类型恰好安全才接受泛型体。
6. 容器本身仅要求存储所需能力。排序、字符串化、查找等方法按操作分别增加条件，不能把整个 Array<T> 强制为 Comparable、Stringable 或 Hashable。必须支持方法对外层 T 的条件约束；具体源码语法与接口编码仍为 OPEN，冻结后才改这些声明。
7. 首批能力覆盖 Comparable、Hashable、Stringable、Iterable<T>、Sendable，并明确相等与键等价。保留当前浮点 IEEE `==` 和容器键等价的区分；不得顺手合并成一个未定义的 Equatable。通用相等操作的能力名称/表示和适用 API 集合须在 F2 冻结。
8. Numeric、Default 暂不作为首版必需的大约束。数值转换、加法、位运算、零值和默认构造分别按真实库需要决定；没有 Default 不允许对任意 T 偷做零初始化。`WorkQueue.pushRange` 的整数转 T 先按 317 审查，不能反向给整个队列强加 Numeric。
9. 泛型形式可持有抽象的复制、销毁和投影义务；布局、具体字段清理与 RC/COW 展开在特化后确定。单态化位于 Checked 之后、Lowered 之前，并复验 Checked 不变量；具体 pass 与序列化由 314 唯一实现。
10. 语义实例身份仍为“泛型声明身份＋有序类型实参”；实现内容、效应特化、目标/profile 与 ABI 属于代码变体和缓存身份，不能让不同回调效应误命中同一代码缓存。
11. 约束见证解析不消除语言已有动态分派。具体且稳定的目标可直接调用；接口值、可覆写方法、运行时回调、热更入口仍遵循各自动态调用合同。首版不增加运行时泛型字典传递或类型擦除 fallback。
12. 普通泛型定义错误与调用约束错误分别定位到定义和调用；实例化预算、目标能力、布局和 CTFE 仍可能失败，须报告用户实例化入口及必要依赖链，不能承诺“实例化阶段永无错误”。
13. 显式 derive／反射若生成依赖具体字段的新代码，该代码重新经过 Built→Checked；诊断指向派生入口和字段路径。320 不得把未检查 AST 放入 Checked 模板，普通泛型也不得借反射恢复隐式 duck typing。描述查询和成员存在性分支不补足普通泛型缺失的约束，不授予字段读写/调用/构造权限；按 [320 §3.3](../../../../xray-docs/tasks/320-comptime-reflection-and-library-derivation-cn.md#33-已决反射边界2026-09-26) 检查显式授权与生成实现，编码授权不自动包含解码构造权。

### 2.8 泛型效应与待冻结的声明面

模板保存“本地操作效应 ∪ 回调效应 ∪ 约束成员调用效应”的符号依赖；throw、挂起、阻塞、分配以及清理路径分别表达，不能提前压成一个 MAY_THROW 位。调用处代入实参的公开合同检查符号效应上界，实例化和封闭调用图的实现体推导再精化优化；推导不能替代所要求的公开承诺，未知动态目标按合同上界保守处理。递归依赖须有收敛与资源预算，超限明确拒绝，不能降到另一条运行路径。

Task322的 `requires_root` 是执行权限的安全效果，定义处推导、沿调用/函数值及有限高阶依赖组合、调用处代入并复验；不能套用上段“只精化优化”的含义。该任务不要求新增用户效果注解或公开效果参数，也不免除定义处约束、Sendable与捕获检查。其闭合上界合同与通用符号效应声明面分开：322 §§6.2–6.3已冻结闭合上界、拥有式有限公式、真实绑定与Checked26/semantic71的原子切换；该子集不受下表通用符号效应OPEN行阻塞。完整实现与消费者资格仍按322分阶段取得，冻结机器合同不等于P1–P5完成。

以下方向已确定，但尚未完成源码语法、类型和兼容规则冻结；**相关声明在对应行 CLOSED 之前不得实施**：

| 冻结项 | 必须给出的合同与证据 | 状态 |
|---|---|---|
| 普通read参数与Sendable标记子集 | 实际spec §17.11、xir-generic-templates合同；显式实参、定义处准入/转发、模板序列化和特化正负矩阵 | CLOSED（仅此标记族；不含下列成员/条件约束） |
| 条件方法的 Sendable/Error 标记子集 | 实际 spec §9.2；[条件方法合同](../../../../xray-docs/design/xir/xir-conditional-methods.md)规定方法自有条件、外层/自身参数作用域、幂等合取、Checked 字段和正负矩阵 | CLOSED（合同；[范围实现与验证](../../../../xray-docs/analysis/task-313-319-conditional-methods-2026-09-30-cn.md)已记录，完整安全/产品资格 OPEN，不含成员见证/override） |
| 方法对外层 T 的条件约束 | 合法语法、作用域、方法可见/可调用条件、泛型转发、接口 round-trip；不污染容器本身准入域 | OPEN |
| 普通 read 方法接口与静态泛型见证 | 实际 spec §5.5“方法要求身份与静态泛型见证”；[精确合同](../../../../xray-docs/design/xir/xir-interface-witnesses.md)包含身份、继承合并、签名/权限/效应、统一约束字段、拥有式映射及正负/生命周期矩阵 | CLOSED（仅合同；源码/Checked/特化/执行实现 OPEN，不含完整属性、其他 receiver、泛型方法、class/override 或接口值） |
| 见证与约束蕴含 | 实现唯一性、交叉约束同名歧义、继承、结构字段精确匹配、属性读写及借用/noescape/receiver 兼容 | OPEN |
| 能力集合与相等关系 | 内置能力精确操作集、IEEE 相等与键等价、各容器 API 所需约束；Numeric/Default 仅在有用例时增补 | OPEN |
| 有限 Equal 值相等子集 | 实际 spec §2.2/§9.2/§13.5/§17.33；[唯一 typed 相等合同](../../../../xray-docs/design/xir/xir-value-equality.md)冻结 ordinary TYPE + builtin Equal bit4、inline/where、同T ==/!=/assertEqual、有限D、EQUAL117 immediate0/1、Checked57/wire22、typed状态/预算及独立正负/物理矩阵 | CLOSED（仅合同；正式实现/完整资格 PENDING；不关闭上一行，也不涵盖nominal/derive、nullable、Map/Set/JSON及原四CLI） |
| 内部根权限与有限高阶公式 | 322 §§6.2–6.3的root上界、owned TEMPLATE/INSTANCE、physical参数及actual绑定、const捕获/清理/透传与原子产物身份；不新增用户注解 | CLOSED（合同；f68私有编译子集已验证，完整实现/VM/native/Library/SDK/P1–P5资格OPEN） |
| 通用符号效应声明面 | 每个回调/见证成员的效应参数和上界、公开承诺语法、override/接口实现兼容、递归/未知调用与清理路径 | OPEN（throw/挂起/阻塞/分配等通用声明面；不阻塞上一行已冻结的322根权限子集） |
| 生产端方法 no_suspend 承诺 | [声明效应合同](../../../../xray-docs/design/xir/xir-declared-effects.md)：模块/owner/方法选择、显式参数排除 receiver、静态/绑定方法的期望 callable 检查；[实施记录](../../../../xray-docs/analysis/task-313-316-method-promises-2026-09-30-cn.md) | CLOSED（合同；范围实现与验证已记录，完整安全资格 FAIL，产品载体及接口/override OPEN） |
| 特化与生成代码 | 语义实例/代码变体身份、可达性与预算、模板/特化复验边界、derive 重新检查及错误定位 | OPEN |

F2 必须给每行补源码 spec 精确锚点、机器合同字段和正负矩阵，不能仅把状态从 OPEN 改成 CLOSED。314/317/320 只消费已冻结的子集；未使用的语法不阻塞非泛型首个纵切。

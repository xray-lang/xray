---
id: spec.17_compilation_pipeline
order: 018
---

<!-- xr-spec:cn -->
---

## 17. 编译流水线 (Compilation Pipeline)

> 真值源：`src/frontend/`、`src/ir/`、`src/vm/`、`src/aot/`、`src/toolchain/`、`src/app/cli/xcmd_build.c`、`src/app/cli/xcmd_compile.c`。

### 17.1 两条执行路径

```
源码 (.xr) -> lexer -> AST -> analyzer / canonical evidence
   -> xi_lower -> Xi IR -> optimization
        |-> tagged 表示                        -> xi_emit -> XrProto -> VM
        `-> native 表示 + backend lowering     -> C source
                 -> host/cross C toolchain -> native binary
```

Xray 当前没有 JIT。默认 `xray run` 和默认 `xray build` 使用字节码/VM 路径；`xray build --native` 才选择 AOT 路径。

两条路径共享 parser、analyzer、模块图、**Xi IR** 与运行时语义。分叉发生在 Xi 流水线内部的表示选择与后端下降阶段（`xi_pipeline_default_config` 与 `xi_pipeline_aot_config`）：VM 路径使用 tagged 表示政策，不运行 select-rep 与 backend lowering，由 `xi_emit` 发射 `XrProto`；AOT 路径运行 select-rep（native 表示政策）与 backend lowering，再由 `src/aot/` 生成 C。因此两者的输出表示和后端优化并不相同。

### 17.2 前端

- lexer：`src/frontend/lexer/xlex.c` / `xlex.h`，关键字表为 `xkeywords.def`；输入必须是合法 UTF-8。
- parser：`src/frontend/parser/xparse*.c`，手写递归下降与优先级表达式解析，输出 `AstNode` 模块树。
- analyzer：`src/frontend/analyzer/`，执行名称解析、类型/泛型/可见性/所有权/捕获/错误集检查。
- canonical evidence：`src/frontend/canonical/` 与 module/toolchain 层把跨模块事实收敛成供后端消费的稳定证据。

### 17.3 字节码与 VM

`src/frontend/codegen/xcompiler.c` 运行分析流水线（类型推断、单态化、逃逸分析）后委托 Xi IR 流水线 `xi_pipeline_compile_program` 完成 lowering、验证、优化与字节码发射，产物是 `XrProto`；它没有独立于 Xi IR 的字节码生成路径。opcode 的唯一清单是 `src/runtime/value/xinstruction_table.h`，VM 实现在 `src/vm/`。VM 使用寄存器式指令布局，并包含属性/调用快路径以及 coroutine、错误通道、tail-call 等专用 opcode。

独立 `.xrc` 产品已移除；`xray compile file.xr` 必须使用 `--format c|header --output FILE.c|FILE.h` 显式生成供编译器开发使用的离线 C 源/头文件字节码容器。`bytecode`/`bc` 格式和 `.xrc` 输出会在创建 isolate 前拒绝，运行时也不会执行 XRC。这里的 `--format c` 是**字节码容器的 C 表示**，不是 native AOT C 后端。

字节码必须以确定性顺序序列化 extern 聚合布局及目标 ABI 指纹。加载器必须在执行前校验布局深度、递归环、字段范围、总大小、尾随数据和 ABI 一致性；任何损坏或目标不匹配都必须拒绝加载，不能回退到宿主机布局。

### 17.4 Xi IR 与优化

两条路径都将程序 lowering 为 `src/ir/` 中的 Xi IR，区别在优化等级与表示政策：VM 路径为 light 等级、tagged 表示政策，AOT 路径为 full 等级、native 表示政策。优化流水线由 `xi_pipeline.c` / `xi_pass.h` 组织，当前实现包括 SCCP、DCE/CFG 简化、内联、devirtualization、tail-call、escape/ownership、循环优化、GVN/PRE、边界检查消除和向量化等 passes；具体启用集合由优化等级、目标和合法性检查决定，不能把某个 pass 的存在理解为每个程序都保证发生该优化。

### 17.5 Native AOT

`xray build --native file.xr` 经 `src/aot/` 的 prepare/verify/representation/container/link plan，把 Xi IR 生成 C，再调用 host、Clang 或 Zig C toolchain 编译链接。hosted native 仍链接 Xray runtime；`--profile freestanding` 使用受限的 freestanding capability 集。`--target`、`--toolchain`、`--cpu`、`--lto` 等选项以 `xray build --help` 为准。

native AOT 不是直接从 SSA 发射机器码，也不是 JIT；最终机器码由所选 C toolchain 产生。
### 17.6 分阶段 XIR 合同

内部协议的现行版本只由下列实现常量定义；语言章节引用其责任，不复制递增编号。
所有声明族共享这些现行协议，不存在按章节选择的版本。旧版本拒绝，不保留兼容reader或适配。

| 协议 | 唯一版本定义 |
|---|---|
| 值边界 | `src/xir/xxir_value.h`：`XR_XIR_VALUE_ABI_VERSION` |
| 可恢复调用 | `src/xir/xxir_call.h`：`XR_XIR_CALL_ABI_VERSION` |
| 不可变Program | `src/xir/xxir_program.h`：`XR_XIR_PROGRAM_ABI_VERSION` |
| Checked wire | `src/xir/xxir_checked.h`：`XR_XIR_CHECKED_SCHEMA` |
| Checked语义 | `src/xir/xxir_checked.h`：`XR_XIR_CHECKED_CONTRACT` |

新编译主干唯一阶段顺序为 `Built → Checked → 特化与复验 → Lowered`。
本节先冻结非泛型标量子集；前述旧产品实现尚未迁移，本节不宣称 CLI 已切换。
普通泛型定义按约束检查，单态化位于 Checked；显式派生的新内容重新检查。
普通实例在不可变可执行 image 封存前完成，VM 只执行 Lowered。

首个内部子集接受 `unit`、`bool`、`i64`，标量复制保持值，`i64` 加法按二补码环绕，
相等及有符号小于比较产生 `bool`，条件分支只接受 `bool`，返回类型须与声明相同。此内部子集参数
只含 bool/i64；unit 可作结果和终结操作类型，不产生值 ID。
此子集没有托管值、借用、泛型或新源码拼写；未实现操作必须拒绝。内部直接调用另见下文。
CFG 的块完整划分指令，均从入口可达，并以一个终结操作结束；入口无前驱。
SSA 使用必须由同块较早定义或支配块定义提供；unit 指令不能作为值使用。
抽象 copy 只在 Built/Checked，物理 scalar-copy 只在 Lowered，不能仅改阶段标签。
阶段转换重新验证并复制所有元数据，成功产物不借用输入；失败不发布部分产物。
验证的结构、类型、支配关系和工作量有预算，未知阶段、操作和类型均拒绝。
精确内部字段由 `test_xir_stages` 等断言验证；这些断言不授予执行资格。

内部标量执行由 `test_xir_execution` 与 `test_xir_native` 验证：首个目标为x86_64小端、
值边界使用本节所列现行 ABI。布局查询同时包含类型、目标、用途与ABI；lowering保存唯一的
帧槽偏移和边界布局，VM/C后端不得另行决定。bool/i64的SSA及帧槽为8字节，
边界值为16字节（类型、零保留字段、i64载荷），bool载荷只准0/1，unit无帧槽。
参数必须精确匹配，错误清空结果；每条指令消耗一步，帧和步骤均受预算限制。
有符号算术须用无符号计算及受检除法避免宿主未定义行为，所有退出释放帧，标量结果独立存活。
Checked不可执行；未知目标/ABI拒绝。此内部子集不授予模块、调用或可恢复ABI资格。

内部可恢复调用由 `test_xir_calls` 与 `test_xir_call_native` 验证。直接CALL引用模块函数身份，
CALL参数通过函数自有操作数表传递，数量和类型须精确匹配声明；SUSPEND保存下一指令，THROW传递i64错误token。
这些是内部XIR操作，不引入源码关键字，也不授予模块、泛型或stdlib缓存资格。
VM与生成C均通过同一typed帧/结果协议交还trampoline，调用者不把待恢复的子调用留在C栈上。
帧地址稳定，深度、实际字节与恢复工作分别有预算；挂起token只可消费一次，重入驱动/销毁拒绝。
取消在回调交还控制后按子帧到父帧清理；清理不能再次挂起或重新争取完成权。
当前内部接口只允许单宿主线程独占驱动，不声称并发取消、分段池或完整image/instance已完成；托管string扩展见17.7。
闭合标量叶子可使用经明确验证的非挂起入口，但CALL/SUSPEND/THROW不能退回该入口。

### 17.7 内部托管值与结果交接

内部值与调用使用§17.6的现行 ABI，不保留旧边界别名。string 保存严格 UTF-8，
允许内嵌 NUL，不隐式归一化或替换；字节数、Unicode 标量数与字素数是不同概念。
复制保留原子引用，修改执行独占窗口内的写时复制；共享副本不因其他副本修改而改变。
所有分配属于显式、计费且可独立存活的域，字符串不依赖执行帧、实例或代码镜像寿命。
借用字节视图不能跨越所有者修改或释放。计数溢出拒绝，非法释放不因 Release 构建而忽略。

调用参数在准入时复制到帧，恢复回调和 return action 的值是借用。调用驱动器在子帧清理前
取得结果的拥有权；poll 返回借用结果，take 仅一次转移拥有权，未取走结果由调用销毁释放。
独立结果在调用、输入和镜像释放后仍有效。typed output 按显式 stdout/stderr 流和真实
bool/i64/string 类型以借用组调用实例配置的同步 provider；缺失或拒绝是运行时失败。
原始输出组只有一个值，不加空格或换行；print 输出组在参数从左到右求值后一次发布，
渲染时值间恰一个 ASCII 空格，末尾一个 LF，零参数也输出 LF。渲染器先完整验证、
预算检查和分配，再调用字节 sink 一次，失败不发布部分组。PRINT 和 native group 均准入
0–65536 个值，并受资源预算限制；未实现的源码类型与显示协议仍需单独验证。

以上仅冻结内部 C 边界；源码 string 声明、模块实例、泛型库发布和最终产品资格仍分别验收。
实际预算、并发、失败原子性及释放义务由 `test_xir_values` 与 `test_xir_value_allocations` 验证。
XIR准入string参数/结果、COPY→OWNED_RETAIN、CONCAT_STRING与OUTPUT；
lowering保存并复验全部拥有槽，VM与生成C在覆盖和退出时按此清理。字面量表由Program封存，源码生产仍待接入。


WRITE_STREAM借用一个string并以immediate 1/2选择stdout/stderr，结果为bool。
已配置provider的拒绝返回false；缺失provider仍为运行时OUTPUT_ERROR。PRINT/OUTPUT的
拒绝仍终止执行。provider重入取消优先于写入结果，已接受字节不回滚。该内部原语不代表
源码stdlib绑定、host flush或文件短写策略已实现；旧调用ABI准入失败，无适配路径。

### 17.8 不可变Program与模块实例

内部多模块闭包在Lowered后封存；Program拥有模块/函数身份、依赖、声明槽、字面量、
初始化顺序及代码环境租约，Instance拥有运行时状态和分配域。模块DAG依赖优先，
同批可初始化模块按UTF-8名称字节序选最小者；循环、重复依赖与闭包外模块拒绝。
每模块的私有无参unit initializer仅执行一次，入口为另一个无参i64函数。

const槽仅由所属initializer发布一次；模块var仅准入根模块主执行流，不扩大库级var。
const Atomic<i64>持有每实例创建的同步身份对象，复制保留同一对象；首批SeqCst load和
fetchAdd遵循原子合同，fetchAdd返回旧值并按二进制补码环绕。它不采用string CoW。
初始化跨挂起保留单发布者；未发布读拒绝，失败清理逆序执行并保持失败粘滞。
实例销毁禁止新准入并完成活动调用清理；独立结果不保留无关模块状态。
恢复必须匹配执行epoch和wake，防止跨调用迟到恢复。首版仅准入单宿主线程独占驱动。
精确准入、预算与租约责任由 `test_xir_instances` 验证；这不授予源码或缓存格式资格。

### 17.9 新 XIR 源码准入边界

新 source owner 复用实际 parser 与模块 resolver/graph，直接检查声明和构造 Built，
只发布拥有数据的 Checked；不调用旧 analyzer、IR 或执行器。首批按声明族准入
bool/i64/string、只读参数的普通具名函数、直接调用、局部绑定、模块状态、string拼接、
print及默认SeqCst的Atomic<i64>构造/load/fetchAdd。未实施语法明确拒绝；不将未约束泛型
按实例猜测检查。每个函数体都检查，包括不可达函数。未标注返回类型的值返回推断尚不准入。

顶层函数提升，顶层执行语句及绑定初始化按源码顺序每实例执行一次；main只是普通函数。
各模块私有unit initializer与返回0的合成i64入口分开。库模块状态必须是const Sendable，
最初准入scalar/string/Atomic<i64>；后续类型按本章各节准入，Array见§17.26。根var属于实例。跨模块调用要求直接导入与export，
模块身份来自resolver，重复/私有/未解析声明与环拒绝。字符串AST已经解码，不重复解码。

CALL/PRINT用args[0]/args[1]表示函数自有操作数表的起点/数量；非空范围按指令顺序
完整分割该表，空范围必须为0/0，PRINT immediate为0。单组上限65536，每个值均检查
类型与支配关系。嵌套参数从左到右求值完成后才追加外层范围。Lowered保存并复验最大
出站参数数量，稳定帧为其分配typed value暂存区并计入物理字节预算。
泛型、完整stdlib、协程语法、foreign provider、parser完整
OOM恢复与输入预算、默认CLI及产品资格仍单独验收。接口由 `test_xir_source_admission` 与 `test_xir_source` 验证。

标准库源码子模块`std/io/output`以普通函数发布writeStdout(string)->bool和
writeStderr(string)->bool，只报告宿主typed provider接受结果。该模块的精确stdlib身份
才可调用私有__writeStdout/__writeStderr，降为WRITE_STREAM；导入与函数体仍正常检查。
文件路径、函数拼写或项目同名模块不能取得权限。此接口不承诺File的flush/短写行为，
也不替代既有yieldable文件操作；同源Checked/native产物发布另行验收。

### 17.10 自持有 Checked 产物边界

当前已实现的封闭声明、类型与操作子集使用唯一的目标无关 Checked 包。
头部绑定 schema 和语义合同版本、Checked 阶段、精确负载长度及 SHA-256 内容标识。
摘要不是认证。固定小端字段编码不包含宿主指针、物理布局、native 代码、运行时状态
或已验证标记。未知版本、非法长度、保留字段和尾随字节必须拒绝；不提供 Built、
Lowered 或旧格式读取器。

读取器在分配和遍历前限制字节数、累计计数、分配内存和工作量，自持有全部解码数据，
发布前执行现有完整 Checked 语义与布局复验。重算摘要不能绕过类型、CFG/支配关系、
可见性、import、初始化或槽权限。加载成功后输入存储可立即销毁；失败不发布产物，
释放全部部分构造。VM 与 native 消费者继续使用同一复验后的 Lowered 转换。
精确字段与预算由 `test_xir_checked` 验证。

该子集（含§17.11的标记约束模板）不代表完整成员约束/见证、效应、诊断来源序列化、包链接、native 缓存配对、
installer 发布或完整无源码标准库分发已经验收。

### 17.11 定义处检查与 Checked 泛型特化

普通类型参数准入域为可复制、可保存的值，不包括 unit、视图和 noncopyable 资源。
最初具体类型为 bool/i64/string/Atomic<i64>；后续数值、函数与Array类型按本章各节准入。函数可以声明 `<T, U:Sendable>`，
或使用该声明自身参数的 `where U:Sendable`；调用须显式给出全部类型实参。
参数名不重复，Sendable 是保留标记名。已准入标记的合取按集合处理，重复要求幂等。
条件方法的实施合同允许方法对外层及自身参数增加 Sendable/Error 要求，要求归方法所有，
不加强名义类型本身；定义处证明和调用处义务沿同一 Checked 路径处理。
默认类型参数、完整成员/见证约束及类型推断尚未准入；条件方法的执行资格须单独验证。

每个模板体（包括未使用模板）在定义处检查。复制、读传参、保存与返回由普通准入域
保证；Sendable 不授予成员、算术、显示或默认构造。无约束 T 不能调用要求 Sendable
的泛型，即使当前具体类型恰好全都 Sendable。泛型转发必须从声明本身证明约束。
复制保持 string 值语义、Atomic 身份语义；具体 retain/清理和布局在特化后确定。

Built/Checked 使用函数局部类型参数 ID，保存约束和独立调用类型实参表。
CALL 的类型实参范围与值实参范围分别规范化并复验；替换后的参数/结果须精确匹配，
正常可见性、模块和支配关系规则仍有效。Checked 包使用§17.6的现行 schema与语义合同（局部存储见§17.12，整数见§17.14），
旧schema或语义版本直接拒绝。解码后重新验证模板定义与转发证明。

特化只读取 Checked，不访问 AST。按声明身份与有序具体类型实参建立有界工作队列，
重复或递归实例复用同一条目；先完成实例闭包并复验，再进入唯一 Lowered 管线。
当前宿主函数 ID 接口保留所有普通定义为根；仅发射从这些根可达的泛型实例。
模板独占包没有普通根时不能直接成为可执行程序。实例数、累计参数/块/指令、内存
及工作量有预算，失败发布空结果并释放临时存储。Lowered 和不可变 Program 不保留
开放类型参数，不在执行期追加实例。实例调试名字不是 native 缓存身份凭证。

成员见证、符号回调效应、用户声明的泛型类型构造器、类型推断、诊断来源序列化与完整标准库包
链接/缓存配对仍待实现；本族未准入的效应/借用/callable 声明仍拒绝。精确接口由
`test_xir_generics` 与 `test_xir_source_generics` 验证，不得用本族通过代替普通泛型完整资格。

### 17.12 局部可变存储与结构化控制流

局部var必须有初始化值；它表示当前调用帧中的typed place，const与read参数仍是值。
读取取得独立值快照，赋值先完整求值再替换，不改变之前保存的string副本；Atomic复制仍保留身份。
place不能逃逸、返回或直接作为值实参。初始化支配每次访问，类型精确一致；泛型在定义处检查。
Built/Checked保留LOCAL_NEW/READ/WRITE，Lowered唯一选择标量或托管存储动作和帧偏移。
托管写入先retain后drop，帧所有退出路径负责清理；精确最后使用释放和不可复制资源析构尚未据此验收。

if/else与while条件必须bool；只执行选中分支，循环每次重新求条件。局部更新跨分支与回边保存，
块内名字不外泄；无标签break/continue指向最内层while。值函数每条存活路径必须返回；return、throw、break、continue 之后的语句不可达，不被降级，其中的闭包被拒绝。
本族接通具体i64加法/相等/小于与既有string加法；不授予无约束T任何运算见证。
块/指令/内存/深度/工作预算以及运行步数/取消合同继续有效。Checked wire schema与语义合同遵循§17.6，
旧语义版本拒绝；没有第二条兼容检查路径。精确接口由 `test_xir_locals` 与 `test_xir_local_native` 验证。

### 17.13 布尔短路与C风格for

新源码入口的!、&&、||严格要求bool；定义处检查右侧，即使运行时必然跳过也不放松约束。
左侧先且仅求值一次，&&在true时、||在false时才求右侧；!返回反值。统一降为已有CFG与typed place，
没有新增运行时或IR分支。可空收窄尚未据此接通。

for(init; condition; step)初始化一次，循环初始化绑定只在该循环内可见；省略条件为true。
正常迭代和continue先执行step再求条件，break/return跳过step；step看不到循环体内的绑定。
嵌套while/for的无标签出口指向最内层。即使step没有入边也检查其源码类型，丢弃其临时指令/块及
操作数、类型实参范围且不返还预算，最终CFG不保留不可达块；字面量仍按现有闭包表政策保留。
独立语句及step中的name++/name--只接受可变i64，使用同一环绕加法与赋值路径，
不能用作表达式。标签、for-in协议与其他数值族仍待接通。现有Checked语义已完整表达这些构造，包与ABI版本不变。

### 17.14 XIR i64算术与关系比较

新源码族按§2.3.1统一采用二补码环绕：+、-、*、一元负号与增减均按2^64取模；
取代早期内部checked-add子集。二元实参从左到右各求值一次，不因交换比较方向重排副作用。
==、!=、<、<=、>、>=按有符号值比较并返回规范bool，不能通过减法比较。
除法向零截断，非零余数与被除数同号；INT64_MIN/-1为INT64_MIN，INT64_MIN%-1为零。
除数为零产生独立DIVIDE_BY_ZERO fault，没有结果并逆序清理所有帧；初始化中失败保持粘滞，
已初始化实例的普通调用失败允许后续调用。生成C在宿主/和%之前检查特殊对，不执行有符号溢出。
一元负号降为零减操作数，其他运算经同一Checked→Lowered→共享标量运行时。
当前Checked包及值/调用/Program边界统一遵循§17.6，无旧reader或适配路径。
具体i64能力不能通过实例化补给普通泛型；其他数值族与显式checked/saturating库方法尚未接通。

### 17.15 XIR i64位运算

具体i64的&、|、^按64位二补码位型运算，~降为与-1异或。<<丢弃高位并补零；
>>为算术右移并补符号位。计数按64取模，包括负数：-1为63、-64为0，零有效计数保持原值。
宿主实现仅移位无符号位型；负数右移用补码变换明确补符号，不依赖C有符号右移。
操作数从左到右各求值一次，位运算拒绝bool/string/Atomic及未约束T。变量复合赋值先读左值，
再求右侧，计算后写回并返回新值；右侧即使修改同一变量也不能改变已读取的快照，失败不写回。
+=另准入string拼接及快照，其余复合运算要求i64；const/read、成员与索引目标暂不准入。
当前Checked包及执行边界遵循§17.6，无旧包或ABI适配。其他定宽整数与显式转换见§17.21，完整上下文推导仍未准入。

### 17.16 XIR拥有式函数值与间接调用

本族沿用§2.8的函数类型拼写，准入read参数（外围泛型参数遵循§17.17）、普通具名函数值、嵌套函数参数/返回、
局部可变存储、入口模块槽、条件选择及普通泛型身份传递。未准入的ref/move、捕获闭包、
借用来源、Sendable和显式效应承诺继续拒绝，不能从选中的函数体补充证明。
函数类型按有序参数type/mode、返回和承诺形成模块拥有的规范表；嵌套条目严格后向引用。
函数引用检查声明可见性和import权限，不允许取initializer；间接调用先捕获callee，
再从左到右求实参，验证callee的值角色、支配关系、完整签名及结果，不能枚举目标替代合同。

Built→Checked深复制描述；普通特化和复验后，唯一Lowered布局把函数值纳入明确的copy/drop、
局部place和PHI所有权。VM与生成native均使用相同可恢复CALL和结果协议。Program封存时拥有
全部签名、入口和必要代码。函数值只保活分配域及独立准入记录；记录保活Program代码租约，
但不强持有全部Instance槽、provider和执行帧，因而入口槽持有函数值不会构成Instance引用环。
停止/释放Instance先撤销准入；外逃值仍可读取、复制和合法释放，最后租约释放代码。
当前同一宿主线程的驱动器只接受原实例内调用；另一实例或Program即使数字ID相同也拒绝。
跨实例转交及并发准入仍待完整资格，不能据当前拒绝边界缩减总任务。

当前唯一包和执行边界使用§17.6的现行版本；
没有旧reader、boxed适配器或第二执行链。源码/包验证、VM/native独立预期、挂起/取消、
结果外逃、代码租约和逐处分配失败测试按机器合同验证；这不构成完整语言或产品资格。

### 17.17 函数签名中的外围泛型参数

普通read函数类型准入`fn(T)->T`及嵌套签名。自由参数序号在使用该类型的函数中解释；
所有使用在定义处按外围声明参数和约束检查，调用传入函数只依据其签名。
显式类型实参递归替换参数和结果签名；结构相等不产生Sendable，不读取未来实例的函数体。

Checked保存每个规范签名的准确自由参数范围，含嵌套签名贡献；验证器沿后向引用独立复算。
全局槽必须闭合。特化重新生成闭合规范类型表，并重映射参数、结果、指令、显式实参和槽，
随后复验。Lowered和不可变Program拒绝全部残留开放签名，包括未使用条目。
结构替换与匹配消耗工作量和元数据预算，最多允许128层活动结构替换。
Checked与Program、值和调用边界使用§17.6的现行版本，不接受先前版本。
捕获、其他mode、效应承诺、类型推断及成员见证继续按各自合同和门独立接通。

### 17.18 XIR 拥有式不可变捕获环境

函数值可以保存一组有序的按值捕获。FUNCTION_REF 的操作数范围是捕获值；目标函数的参数前缀逐项匹配捕获类型，剩余参数和返回匹配公开函数签名。显式泛型替换、值角色、支配、可见性和初始化限制全部在 Checked 验证，特化后复验；捕获不授予额外调用权限。

已闭合的按值叶 Function 捕获须验证其真实 Program producer、目标、声明类型及完整 captures，不能仅凭 NONE 广告或地址取得准入。运行期以原 FUNCTION_REF 捕获操作数的已拥有声明上界验证捕获前缀，并与封存执行节点的完整实参行逐字相符。无环境的具体叶函数不继承外层 closure 的无关环境列；symbolic 或有环境的 producer 保留完整关系并继续普通精确验证。其它 constructed 捕获、Function 跨 GO 的 Sendable、NO_SUSPEND、私有可见性及资源责任不因这一有限能力解除。

捕获环境在构造时取得每个值的逻辑副本，失败不发布部分函数值；复制函数值共享不可变环境。string 保持值语义，Atomic 保持同步身份，函数捕获保留原有执行准入。环境不得保存调用方帧指针。最后一个函数引用释放全部捕获及代码租约；嵌套环境清理不得随捕获深度递归消耗宿主栈，也不得为清理再次分配。

间接调用仍发布同一可恢复 CALL action，携带借用的函数值；唯一帧驱动器把环境前缀和显式实参复制到新帧。在新帧完成拥有前，调用方帧继续保活环境；挂起、抛出、取消和返回沿用统一清理。宿主函数调用入口同样保留捕获和实参，再释放前次结果。实例 stop/free 撤销执行准入，但外逃环境和值仍可读取、复制和释放。

该内部合同不把 var 变成按值捕获，也不授予 Sendable、noescape 或 no_suspend。普通 var 的共享 cell、循环回收及源码闭包生产入口须分别接通并验收；内部构造通过不代表源码闭包完成。唯一现行协议由§17.6列出的实现常量定义，旧版本拒绝。

### 17.19 XIR 共享捕获 cell

内部cell类型编码为`0x40000000 | 元素类型`；元素是非unit普通值类型或作用域内类型参数，不允许cell嵌套。它是实现能力，不是源码类型或Sendable证明；公开callable签名及模块槽不暴露cell，私有实现的捕获参数前缀可携带cell。Checked替换保留包装并复验元素，Lowered只接收闭合元素。CELL_NEW复制初值，CELL_READ产生拥有式逻辑副本，CELL_WRITE先复制新值、再交换并drop旧值；失败不改变内容。

普通var捕获保留同一共享存储身份，所有别名看到相同更新；读取结果自身仍具有值语义。cell RC始终原子，但内容访问只获当前Instance单宿主线程准入，不授予跨worker或原子换值承诺。cell内函数与捕获的cell必须属于同一实例值域，不能经cell绕过函数能力的实例隔离。最后引用释放使用现有无分配工作链处理内容，不递归销毁环境。强环仍可能形成，可在保留owner时显式断环；不可达强环回收保持OPEN，不引入收集器，也不据此声称所有合法图无泄漏。

当前唯一协议遵循§17.6，无旧版本读取或ABI适配。源码var不得以快照冒充共享cell。
### 17.20 定宽整数运行时基础

统一无分配整数核心显式接收8/16/32/64位和符号；有符号payload符号扩展，无符号payload零扩展，u64保留全部64位。非规范payload和非法宽度拒绝。算术、比较、显式转换及§3.3.2移位遵循§2.3.1；失败返回状态并将数值输出清零。当前i64的VM与native路径均使用该核心，旧入口已删除，现行调用ABI拒绝旧native条目。Checked与值/Program边界遵循§17.6。本节确认运行时基础；八种整数的XIR、显式cast、同符号运算提升和输出接入见§17.21，完整上下文推导与其他容器仍OPEN；Array子集见§17.26。

### 17.21 类型化定宽整数XIR

整数类型ID为i64=2、i8=5、i16=6、i32=7、u8=8、u16=9、u32=10、u64=11。唯一CONST_INT与算术/比较/移位INT操作族替代旧i64专用名称；CONVERT_NUMBER显式转换具体整数，所有指令继续检查类型、值角色和支配。算术两侧及结果类型相同；比较两侧同类型并返回bool；移位保留左类型，计数可为任意整数。无约束T不能通过特化获得算术资格。

常量和边界值保持规范payload；copy、phi、调用、模块槽、cell、callable签名和输出保留精确类型。存储宽度/对齐为1/2/4/8字节，frame仍为8字节，boxed/参数/结果仍16字节且8对齐。无符号输出按完整无符号十进制解释，u64全一位型为18446744073709551615。输出预算及一次发布不变。仅接受§17.6的现行协议，旧包与条目拒绝。源码已接通八种类型名称、具体整数as、已定型操作数的同符号宽化、原宽度一元运算/增减、共享cell、模块状态及print。nullable cast、bool/rune/float与无约束T转换拒绝。直接整数字面量与赋值/参数/返回宽化见§17.22；完整推断、浮点和其他容器仍OPEN；Array子集见§17.26。


### 17.22 整数上下文与无损宽化

声明注解、赋值目标、已声明返回类型、普通/间接调用参数（显式泛型替换后）给出唯一上下文。直接整数字面量包含括号和一次直接负号；无整数上下文时默认i64，有上下文时先检查数学值范围再产生该类型常量。因此u64最大值与i64最小值不经过宿主有符号溢出；负零可表示为无符号零。已有类型的表达式仅同符号向更宽整数转换，不隐式窄化、改符号或把无约束T当整数。转换显式进入CONVERT_NUMBER，仍由Checked复验。推断返回类型的闭包不从外部调用签名获得未经实现的推断权限。

二元算术/比较中的直接字面量由另一已定型操作数获得上下文；两侧均为直接字面量时采用外层整数上下文，否则默认i64。上下文不穿透任意已成形子表达式做常量折叠。取反的直接字面量可用外层整数上下文，已定型一元操作保留其操作数类型。移位左侧直接字面量可用外层上下文，但计数独立定型，不因左侧窄类型而拒绝256或负数；结果仍按§3.3.2处理。复合赋值的非移位RHS采用绑定类型；先读取左侧快照，再执行RHS，成功才写回。显式as的输入独立定型，`200 as i8`继续环绕，不变为越界字面量错误。唯一例外是超过i64正范围且仍可由u64表示的直接正字面量（包括括号）：它保留完整u64幅度作为转换源，再按目标宽度取模；负字面量不借此绕过i64下界检查。

有明确上下文的条件表达式分别检查并宽化分支，在各自前驱块产生转换，运行时只执行选中分支；无外部上下文时先分别定型两个分支；同符号整数取较宽类型，在对应前驱路径显式转换再由PHI合流。直接分支字面量仍独立默认i64，不从另一分支暗取窄类型。其他类型仍要求相同，本节不声称完成union或通用分支类型推断。普通/间接调用保持callee先于按序实参求值；仅无副作用的直接字面量允许延后产生IR以取得另一操作数类型。未调用函数和泛型定义体照常检查，不以实例化绕过约束。此批只扩展源码生产者，已有XIR操作语义、wire与ABI版本不变；浮点、完整推断和其他容器仍OPEN；Array子集见§17.26。


### 17.23 二进制浮点转换基础

共同运行时浮点核心显式接收IEEE binary32/binary64位型（binary32高32位必须为零），整数侧沿用位宽/符号与规范payload。整数到浮点、f64到f32采用round-to-nearest ties-to-even，溢出为带符号无穷，保留subnormal渐进下溢和负零；f32到f64精确。任意NaN位型统一为正canonical quiet NaN（f32为0x7fc00000，f64为0x7ff8000000000000）。同宽转换也规范化NaN。

浮点到整数先向零截断再检查目标完整范围；(-1,0)可转unsigned零。NaN、无穷或截断后越界返回RANGE；非法宽度/编码返回BAD_ARGUMENT，失败清零输出。比较返回less/equal/greater/unordered，任一NaN为unordered，正负零相等；负号翻转非NaN符号并规范化NaN。全部通过整数位运算实现，不分配、不执行宿主浮点运算/转换、不更改宿主舍入模式或异常标志，也不依赖旧scalar标签/生成元数据。

此处只确认共同转换/比较/负号基础；XIR类型、packet、layout及源码执行边界见17.24；浮点算术和typed output按§17.27准入，不能以辅助函数测试代替VM/native浮点贯穿。17.24修订唯一准入版本，完整浮点与跨平台资格仍OPEN。


### 17.24 XIR 浮点值与执行边界

运行 f64 接受全部 binary64 位型，含带符号、任意 payload 的 quiet/signaling NaN；运行 f32 高32位为零，NaN 仍只准正 canonical quiet NaN。
CONST_FLOAT 是独立 wire 编码边界：两精度的 NaN 常量仍必须正 canonical quiet，不能因 runtime f64 扩域接受非 canonical 包常量。
存储为4/8字节，SSA/frame 为8字节，boxed/参数/结果为16字节且对齐8。
CONST_FLOAT、NEG_FLOAT、六种浮点比较与唯一 CONVERT_NUMBER 经同一 Checked/Lowered 管线执行。
CONVERT_NUMBER 原子替换先前仅限整数的转换指令；浮点转整数先截断再检查完整范围，失败为 NUMERIC_RANGE，清理后结果为 unit。
NaN 的 EQ 为 false、NE 为 true，其余关系均为 false；正负零相等。所有转换遵循17.23且不改变宿主浮点环境。
源码接受显式数值 cast、浮点负号/比较、f32 到 f64 隐式宽化及条件合流；整数/浮点混合必须显式 cast。
拷贝、槽、cell、闭包、泛型实例、参数/返回、Tuple/Array/Nullable/已准入名义存储与挂起保留合法值的精确位型；跨界仍核 exact 类型、arena、owner 和既有执行权限。浮点比较遇任一 NaN 为 unordered（EQ 为 false、NE 为 true、有序关系为 false）；整数 payload 搬运不能暗中 canonicalize。十进制字面量及精确整数上下文准入见§17.25；四则、转换和负号的 NaN 结果仍 canonical，浮点输出见§17.27。
唯一协议版本遵循§17.6，旧版本拒绝，无兼容路径。

### 17.25 精确十进制字面量

十进制浮点token的完整拼写由AST拥有；共享转换不截断输入，不使用strtod、locale或宿主浮点指令。按目标binary32/binary64精度一次ties-to-even舍入，溢出为带符号无穷，渐进下溢保留负零；所有指数位和分隔符仍验证。固定大整数保存有理数，1152有效数字及非零尾标志足以区分所有IEEE舍入中点；不把缓冲截断当作数学截断。
无唯一浮点上下文默认f64；注解、赋值、已声明返回和实例化后参数直接选f32/f64，括号及直接负号保留上下文。已成形f64不隐式窄化。直接整数处于浮点上下文时必须完整精确可表示；显式as允许舍入。泛型定义仍先按声明约束检查。格式化保留原始拼写，AST拷贝复制到目标owner。现有CONST_FLOAT、Checked复验与唯一Lowered执行不变，浮点算术/输出按§17.27准入。

### 17.26 XIR Array值与源码边界

`stdlib/types/array.xr`的`struct Array<T>`是此声明族的唯一源码权威。编译器、成员适配器和API清单消费同源结构化声明；当前32个成员中，冻结READ `get(index: i64) -> T`、REF `set(index: i64, value: T) -> ()`及REF `push(value: T) -> ()`三原语，以及map/filter/reduce/forEach/find/findIndex/every/some/contains/indexOf/join/clear十二个声明recipe及reverse/unshift/pop/shift/resize，共20个operation。其余12个成员保留清单身份，不因此获得执行资格。Source先查找准确public成员声明，再按已准入native operation/recipe identity选择实现，query记录原声明身份与CALL引用；不按名字另立执行表。回调、查询、清空的类型、求值、效应与所有权合同见§14.7。索引读写使用同一操作；全局`len`独立按core intrinsic身份解析，本子集只接受Array并返回i64。词法、模块及import解析先于prelude；用户定义的同名`len`仍是普通函数。

源码准入显式`Array<T>`、同类型元素字面量、有Array上下文的空字面量、普通赋值复制、参数、返回、局部及入口模块状态。空字面量没有元素上下文时拒绝。元素从左到右求值，使用既有上下文类型规则；嵌套Array与函数元素遵循统一构造类型池的可复制、可存储准入，unit、内部CELL、视图和未准入的不可复制资源不能成为普通元素。`Array<T>`泛型体在定义处按约束检查，在Checked上特化后复验；普通实例在Program封存前完成，不能按具体实参补造定义处未证明的能力。其他容器与用户struct/class声明族仍须分别准入。

Array复制保留独立逻辑值，无需显式`copy`或`move`。共享backing在写入前取得独立存储，独占且容量充足时可原地修改；紧凑元素存储使用唯一类型布局与copy/drop规则。const与READ参数不能成为可写接收者；把其可复制值保存到普通局部var后，可修改该副本。backing是否独占不授予写权限。已有多模块可见性与库模块可变状态限制继续生效。

跨分配域写入不能把当前执行域的敏感身份植入外域可复制容器。既有有预算的准入遍历在同一次访问中汇总实值的 Cell、Function、Class 与 Task 身份，不增加未计费类型遍历或第二次实值遍历；Error 按实际 enum backing 的活动载荷检查。写入含这些身份时，Array SET/PUSH、独立 struct SET 和字段/索引路径上的全部外域可复制祖先先取得当前准入域拥有的新存储，包括独占且容量充足的 backing。Cell/Class 保持身份语义和原域权限，不以复制放宽准入。纯标量/String 聚合保留原可用的原地路径，纯 String 外域独占余量写入仍零分配。准备、复制、峰值和累计费用使用原账本；失败保持旧值与别名。独占 SET 重定位实际转移新槽所有权，先发表完整新值，再执行旧槽释放回调；清理不临时分配。本规则保护已覆盖的敏感写边，不证明 Task 跨域完整闭环、任意 Source 图可达性或所有循环已被处理。

已准入的可复制 struct/enum 可作为 Array 元素，元素及嵌套值字段按封存的唯一布局 inline 保存；只有 enum 活动载荷持有所有权，不以逐元素 boxed 指针替代聚合步幅。共享存储分离直接复制字段并保留叶子所有权；独占重定位转移已有所有权。读取在既有值边界物化独立拥有的结果，需要接收分配域时按该域计费；失败释放已完成字段和未发布父对象。零大小聚合仍有类型化 backing 与 i64 范围内的逻辑长度/容量，数据区零分配。最终释放使用预留遍历空间和迭代释放工作链，不依赖临时分配。此准入不授予额外可见性、构造、回调或写权限，也不表示嵌套字段/索引地点写回已准入。

GET及索引读返回独立拥有的元素，并保留索引求值前选定的接收者值。SET、索引写及PUSH先确定支持的根绑定，再按序求值实参，最后读取该绑定当前的Array；RHS重绑根后，写入针对新值。`a.push(a[0])`有效。越界严格要求`0 <= index < len`，否则产生E0430并保留有符号index与实际length；初始化期间的失败保持原有粘滞语义。set/push返回unit，索引赋值表达式返回已转换RHS的独立逻辑值，其可能失败的结果retain在提交前完成。失败不提交本次修改，但不撤销此前实参求值的副作用。len只借用接收者，以有界常数工作量读取长度，不分配或遍历元素；GET对嵌套函数元素的传递准入可能分配有界scratch。

返回的Array拥有所需TypeArena与backing寿命，可在原调用、Instance和Program宿主句柄释放后继续读取、复制和释放；纯Array不为类型元数据反向保留Program代码。同Program的纯Array可在Instance间按值传递；不同arena即使数字类型ID相同也拒绝。函数元素仍须通过真实存活的执行gate，Array包装不增加调用权限。新分离或增长存储计入当前接收Instance的物理分配域，已有存储仍由原域记账；准入、元素搬移及调用工作遵守累计预算。最终释放按实际owner归还物理存储，不按嵌套深度递归销毁。

源码查询快照独立拥有Array类型节点、成员签名、泛型声明owner、源码位置及内容身份，编译会话释放后仍有效；查询不重新解析、推断或执行。原生声明的查询来源不产生可执行initializer。失败的私有、别名或未准入成员查找不发布成功绑定；资源失败不发布Checked或快照。查询中的core len记录其稳定身份，未知源码来源保持未知。

此子集的Windows源码VM、native、混合条目及Checked重载按独立预期验证两模块字符串程序，输出为`red\nblue\n2\ngreen\nblue\ngreen\n3\n`；实例隔离、结果寿命、初始化越界粘滞与分配失败物理释放另有断言。输出golden不替代OOM、retain、工作预算或全量sanitizer门。完整批次资格仍要求受影响目标重建、回归、合同和sanitizer门；本节不声明这些完整门或macOS已通过。

其余13个Array成员以及尚未接通的capacity、切片、ptr/mutPtr和iterator方法族仍须分别准入；不因已有recipe的冻结而自动完成。完整move、用户结构体族、跨Program结构导入、stdlib Checked/native包发布及旧链最终删除不由本节完成。源码Array for-in沿既有拥有式快照合同，不能视为iterator API资格。唯一现行协议仍由§17.6的实现常量定义，无旧reader或兼容接口；本节合同同步不宣称完整安全或全回归通过。

### 17.27 浮点四则与 typed 输出

ADD/SUB/MUL/DIV_FLOAT 在历史引入时编号70–73、OP_COUNT为74；现行opcode编号以唯一实现表为准。stage mask为7，两个输入及结果为同一具体f32/f64，其他字段为零。唯一Checked语义、schema及值/调用/Program版本由§17.6的实现常量定义；新增准入规则拒绝旧语义包，既有值与调用布局不变。普通泛型必须在定义处取得具体数值证明，特化与复验不授予额外权限。源码四则和复合赋值沿既有求值/提交顺序，f32/f64混合先精确宽化，typed整数与浮点混合、浮点余数和位运算拒绝。

每次算术按原宽度round-to-nearest ties-to-even，不融合或重排，不使用宿主浮点状态。完整宽乘和除法余数保留最终舍入依据，逐次保留subnormal与负零，溢出为带符号无穷；零除零、无穷除无穷、零乘无穷、异号无穷相加和NaN输入得到canonical NaN。有限非零数除零为带符号无穷，不是整数除零fault。

PRINT/OUTPUT按§17.24接受合法运行f32/f64值，包括全部f64 NaN payload；所有NaN文本仍为`nan`，不会修改输入位型。有限非零数使用原精度最少有效数字往返十进制：同长度候选选最接近者，精确中点取末位偶数，不通过先宽化f32决定格式。最短系数无尾零；十进制指数k在[-4,16)时用普通形式，整数结果加`.0`；其他值用一位整数部分、小写`e`、显式指数正负号且无冗余指数零。特殊值固定`nan`、`inf`、`-inf`、`0.0`、`-0.0`。全部ASCII、固定整数空间、无堆分配；单值32字节缓冲包含终止符足够。宿主locale、舍入模式及异常标志不影响结果。

输出仍先完成所有参数求值，再验证预算、渲染整组并一次发布；失败不发布半组，沿用OUTPUT_ERROR和清理责任。上述合同不代表完整源码、默认产品、安全或跨平台资格已通过；这些必须由重建后的独立VM/native/包消费者/混合程序、所有权与物理释放、失败注入和批次门逐项证明。

### 17.28 Class 字段定义证明与 Unit 模块槽

普通 class 的 T 字段在真实声明 owner 下按既有可复制/保存前提检查，不增加 Scalar、Copyable 或 Sendable 条件。内联与 where 条件保留为同一条件表示；定义处符号字段留在 Checked，实例替换完整有序实参、复验来源后再 Lowered。闭合载体能力与泛型证明分开，未实现载体不伪装成约束失败。显式 final 根 class 的构造、private/const、must/may 和 no_suspend 规则继续适用。class 是身份复制边界，字段 GET 产生拥有式逻辑快照，整体 SET 为所有相同身份的引用可见。

Unit 模块声明有真实 owner、slot ordinal、known 类型、mutability 和发布位。Unit INIT/STORE 的唯一编码有零 value operand，两 args 都为零；LOAD 仍执行发布与权限检查，不产生 payload SSA/frame。RHS 效应、yield、throw 与清理执行一次，成功才发布；const initializer 一次规则、mutable root 规则、失败粘滞和逆序回收不变。推断的 `const x = Coro.yield()`、显式 `const x:()` 和默认 `var x:()`在本族准入，`const x:i64 = Coro.yield()`拒绝。Unit marker 的 Sendable 证明不顺带开放普通参数或泛型保存。

### 17.29 有限内联 class 字段与物理所有权

本族仅准入无循环的 inline struct/enum 字段图，叶子为现有数值/bool/string 和 Array<i64>/Array<string>。闭合字段图通过同一有界能力遍历；Source 完整收集字段后检查，真实临时 scratch 退款，work 累计扣减。普通 T 的合法语言域不缩减；未 reify 的泛型 inline 字段、nested Array、callable/Cell/Error、class 引用图、资源和视图仍是明确的未实现边界。

GET 的唯一四参数 runtime 接口显式接收实际 arena、work/scratch 和可选分配域。scalar/string/Array handle 纯读不分配，保留的 metadata 可在 Program/Instance 销毁后使用；inline 快照物化需要调用方 live domain，新字节记在该域，已有叶子仍向原 backing owner 退款，不授执行权限。SET 使用同一拥有式事务准备机制，完成所有可失败步骤后，在无可观察中间态的提交区释放旧字节并发布新值；失败保留字段、别名和旧快照。NEW 不发布部分 this。对象创建时预留释放帧，drop 对实际活动 enum payload 迭代释放、零新分配，退款至真实 owner。该无环子集不证明完整 L0/weak/L2 残余图回收。

唯一当前协议由§17.6实现常量定义；旧 value target、旧有效摘要 Checked 包及同 Program 的旧 embedded proof 必须由实际 seal/read 拒绝，不保留旧 GET、别名或第二 reader。独立 VM/native/packet/mixed 预期、真实旧缓存、逐 OOM、拥有结果寿命、物理释放和完整适用安全须分别验证；本节不声明完整语言、stdlib 同源配对发布、默认产品或跨平台资格通过。

### 17.30 无入口 Checked Library 与真实导入

编译期 Module 显式携带 PROGRAM/LIBRARY kind，不能从 entry 哨兵推断权限。Library 保留真实声明、初始化函数、imports 和定义处约束检查，从 inventory 起不分配用户 entry；它可以拥有式检查、复验、序列化和读入，但不得直接特化为可执行 Program、Lowered 或 seal。链接后的 PROGRAM 必须有真实合法 root/entry，普通实例化仍在 Checked 上完成并复验、在不可变 Program 封存前结束。每个 wire module（包括 provenance）编码实际 kind，未知 kind、Library 假 entry、Program 缺 entry 和旧格式全部拒绝；不保留第二 reader 或 fake entry。

编译器借用的 opaque catalog 独立拥有已检查库体，完整包摘要、真实 resolver authority、canonical/logical 身份和 source locator 必须匹配。表示在图节点发现时绑定一次，已存在 SOURCE 不能因导入被改成 CHECKED_LIBRARY；匹配的坏库不得回退 AST。未匹配导入继续普通 Source 解析。查询导出事实不授调用权限；私有调用仍按真实声明 owner 拒绝。最终首次 owned Checked 深复制全部所需事实，原 SourceResult/Session/graph/catalog 销毁后仍可复验并执行，不允许用重新解析库源码补能力。

唯一 remapper 先预留全部已使用的目标身份，再复制 body 并重定位 CALL；真实 initializer 可位于输入函数数组任意位置。临时映射按实际 scratch 预留，成功、OOM、work 耗尽或 copy 失败均释放退款；每元素累计 work，永久 body/name 按 metadata 收费。唯一 reader 的 header、摘要、payload 解码和普通复验共同消耗累计 work，连续读取或恶意有效摘要前缀失败也不得重置余额；owned metadata 按唯一发布表示收费，临时 scratch 退出退款。此 reader 合同不声明 Source 所有阶段预算已经统一。

Source 构建上下文的 metadata_bytes 是唯一真实剩余余额：SourceMemory 的完整 header/payload、保留的声明 manifest、Library/implementation 复验及 query 快照复制使用同一余额；成功拥有存储即扣减，不再以另一累计 allocated 计数相减。失败不得用无符号下溢恢复额度或继续越额分配。临时 Source arena 提前释放不返还累计 metadata 费用；已有 scratch 临时映射退款政策不变。最终 Built→Checked 检查仍采用独立阶段预算，整个 Source 多阶段累计预算尚未完成，不以此修复宣称统一。

首个库导入实现边界为同目录 file-backed script authority、单模块、零参数 Unit/i64 和 CONST_INT/ADD_INT/CALL/RETURN；尚未接线的表明确拒绝，不收窄语言的合法泛型、nominal、接口、默认参数或 export const。完整 canonical remap、声明/default helper 事实、公开 const 权限、包认证、stdlib 同源 Checked/native 配对及缓存命中/miss仍须同一管线实现和独立验证。有限导入闭环的 Source/packet/native/mixed、实例、逐 OOM、物理释放及真实旧身份拒绝，不代替完整语言、安全或产品资格。


### 17.31 参数默认值的Checked目的关系

参数默认值在原始Built/Checked的唯一拥有式DefaultTable中记录真实owner、完整形参序数和helper身份；不从名字、Source私有ID数组、query或导出顺序推断。所有记录和未调用默认体在声明作用域检查。helper零runtime参数、非export/entry/init/cleanup，持有owner同构的完整nominal-parent加method-own参数/约束和精确形参结果类型；作用域可引用声明方私有名称，不能继承构造/清理或body效应承诺。helper内部合法defer子函数仍遵守普通清理来源。

Built/Checked的CALL_DEFAULT和INVOKE_DEFAULT先检查caller对owner的完整约束、模块/成员及构造权限，再按owner/ordinal从表解析helper；普通CALL/INVOKE/FUNCTION_REF不能直接获得helper权限。存在局部错误处理时，默认计算使用INVOKE_DEFAULT及普通成功/错误continuation、结果/错误投影和拥有式清理；错误不得绕过catch，也不能因调用owner成功而被吞掉。CALL_DEFAULT的targets保存owner/ordinal，INVOKE_DEFAULT的args保存同一声明元数据、targets保存真实CFG边；元数据不是SSA操作数。两者均无runtime实参、immediate为0，保留完整泛型向量及精确形参结果类型。所有现有本地自由函数、static、READ、构造器及泛型默认族原子迁移，导入首族的有限接受域不降低本地语言合同。

特化使用现有Checked实例收集及逐指令Origin来源，将CALL_DEFAULT仅变为普通CALL、INVOKE_DEFAULT仅变为普通INVOKE，清除目的元数据并独立复验；错误边及投影身份保持不变。特化Checked/Lowered不保留第二实例默认表，唯一原表深拥有于provenance.source；按相同caller/instruction位置重算owner/ordinal/helper和完整参数向量，精确验证对应操作和CFG边，不接受同签名替代、角色标签或存在任意provenance的权限豁免。局部catch沿错误continuation推导，不能按普通CALL直接逃逸汇总。全显式调用的generic owner不为表存在强制实例化未使用helper。原始有defaults的闭合模块也走同一特化管线；Lowered拒绝残留目的操作，runtime没有另一默认值解释器。

表、wire、clone、来源和canonical remap统一计实际metadata/scratch/work并在失败不发布；只在Program封存前完成普通实例化。新格式包含每个module及嵌套来源的默认记录，旧格式拒绝。实际单一schema/semantic及runtime ABI由§17.6所述实现常量承担；新增编译期表或操作不机械增加公开runtime ABI，但同ABI旧mandatory proof仍须在真实消费者拒绝。完整能力只有新鲜VM/native独立预期、来源销毁后寿命、每个真实故障点/物理释放和批次资格通过才成立。

### 17.32 多输入 Checked Catalog 与库字符串

有限库导入沿§17.30的同一Catalog、reader和Source remapper扩展，不能建立第二链接或运行路径。唯一构造接口为xr_xir_compile_library_catalog_new(context,inputs,size_t count,output)，替换旧单输入签名；唯一资源视图为xr_xir_compile_library_catalog_resources(catalog,size_t *count)，删除旧单资源接口。构造要求output、非NULL inputs及count>0，失败保持output；没有库由SourceRequest的NULL catalog表示，不分配空拥有对象。视图要求非NULL count，先写0；NULL catalog返回NULL，成功借用真实连续只读数组至catalog_free。count=NULL返回NULL。Catalog独立拥有每个Checked artifact和全部身份字符串，不借输入包、输入结构或调用方文本；资源数组和artifact owner数组分别零初始化并按真实存储收费，任何部分前缀失败均完整销毁，全部成功后只发布一次。

每个input仍是独立无依赖的单模块SCRIPT Library和单leaf逻辑路径。真实authority、完整包SHA、既有canonical身份算法及声明身份必须一致；相同canonical无论内容相同或不同、authority或physical root相同或不同均BAD_STRUCTURE，不去重、不选择首尾、不另造(canonical,root)键。不同canonical可共享authority，实际resolver仍逐次验证精确authority，匹配坏库不回退AST。多个input不表示已实现Library间依赖、包认证、泛型或完整声明链接。

构造只建立一次实际remaining预算。count存储的乘法/加法溢出、metadata容量及count<=remaining.work先于任何input解引用；container、两个数组、真实文本、每input hash、read_remaining、shape和重复检测累计消费同一余额，不逐项重置或双扣reader拥有存储。每input访问和每重复候选比较至少收费1；实际所比较的字节另按字节收费，允许有界pairwise扫描但不得有无收费二次循环。临时scratch所有出口释放退款，累计work/metadata不退款。成功输出是全体原子发布，不存在部分Catalog。

有限Catalog准入STRING与现有i64形参、Unit/i64/STRING结果，在原CONST_INT/ADD_INT/CALL/CALL_DEFAULT/RETURN集合中仅增加既有CONST_STRING/CONCAT_STRING。Unit形参、构造类型、泛型、slots、implementation、provenance和依赖仍属于未接线域；本片不顺带准入INVOKE_DEFAULT或错误CFG，也不收窄语言的完整合法集合。所有定义、未使用literal、私有/default helper仍由普通Checked验证器检查；普通调用与默认目的权限保持§17.31。既有literal长度wire、STRING指令和runtime语义不变，没有新增proof权限事实或runtime载荷；保持唯一Checked21/53与Program25/Value15/Call19。若实现暴露真实语义或布局变更，须先重新分析并冻结，不能机械沿用该结论。

Source用唯一不发recipe的source_literal_append(ctx,node,bytes,length,&id)管理既有literal表；本地AST和已经拥有的enum文字保持原借用寿命，原source_string_literal仅append后发原CONST_STRING。非法长度、count、非空NULL及增长溢出先拒绝；合法append收费1，vector增长搬运按实际旧count收费，容量保持首次16/其后倍增；失败不发布count或ID。导入每个非空literal先按精确length在SourceMemory拥有并按length计copy work，再append；空literal为NULL/0，不分配payload，不使用strlen、终止符或内容去重。metadata经§17.30真实余额扣完整header/payload，不恢复allocated影子账本；Source提前释放不退累计metadata，Query无runtime literal表也不新增权限接口。

同一SourceLibraryMap记录literal_begin/count，按原库顺序追加无去重区间。CONST_STRING原ID须非负且先小于该库自己的count，再经溢出检查加入目标begin；最终merged表中的合法位置不能挽救原库越界ID。CONCAT_STRING仍使用SSA操作数，CALL_DEFAULT沿唯一function/default映射。原函数scratch map全部出口退款，next_function只在整库复制成功后提交，部分builder不得发布Checked；普通语义失败的query snapshot遵守既有complete合同，不能笼统要求全部snapshot为NULL。

资格须包含两个真实库各自literal0且字节不同、反转Catalog输入顺序后的固定结果、默认与显式STRING、私有helper拒绝、原OwnedChecked在所有producer销毁后执行、Source/独立packet VM/真实native/mixed、双Instance、Program销毁后按length读取拥有结果及最终physical0。低层空、UTF8与a\0b三字节literal须四消费者保真，源码parser的NUL拒绝保持原规则，不能当新源码语法。独立手工包与双层有效重hash攻击覆盖原ID越界但merged合法、unused坏UTF8、长度/计数溢出、身份/authority/摘要及重复canonical；逐新真实OOM、第二库失败释放首库、累计预算exact/minus1及scratch退款均验证。保留既有全部默认/来源/预算/故障测试。本有限增量不替代可变库状态隔离、stdlib配对cache命中/miss、完整产品安全或最终旧链删除。

### 17.33 有限 typed 值相等

唯一 Checked57/wire22 承载本有限域；完整相等声明族及公开CLI迁移仍未完成。Equal 使用已有
marker:u32 的 bit4，ordinary TYPE 的定义处证明、inline/where 合取及来源复验均适用。
首域仅 bool、八整数、f32/f64、string 和递归 Array<D>；其余域继续明确拒绝且目标保持OPEN。
同T的 ==、!= 与真实 Core assertEqual<T: Equal> 使用同一 typed 关系。EQUAL=117、
OP_COUNT=118、stage mask7、两个 VALUE 操作数及 BOOL 结果；immediate仅0(EQ)/1(NE)，
其它闲置字段为零。NE 仅在成功比较后反转同一结果。旧56及更早重hash包在body分配前拒绝。

FLOAT保持IEEE NaN不等与正负零相等；STRING比较完整length/bytes；Array按length/索引
逐叶比较，相同指针或共享backing不能绕过NaN。比较用有预算显式DFS，无C递归或深度截断false。
唯一 typed helper 由 VM/native 共同消费，borrowed输入不copy/drop，只有OK才发布bool；
BAD_ARGUMENT/OOM/LIMIT保持output和输入owner，所有出口归还真实scratch/domain分配。
STRING字节、Array元素和扩容工作累计计费，超限不能当作不等或assertPanics成功。

Source/Owned query/clone、默认helper、Library、writer/reader、Checked特化与Lowered复验
携带同一约束身份。不得借旧Equatable、boxed值adapter、名字白名单或第二executor准入。
本族不增加公开Value/Program字段；当前Value16/Call21/Program26由§17.6与§17.34定义，仍须真实fresh布局/provider拒绝和生成C验证。
资格须含独立完整包与valid-rehash、无约束未调用定义、同名用户接口、NaN共享backing、
NUL/深度257、逐实际OOM与exact/minus1、producer销毁/双Instance/physical退款、VM/native/mixed
分别固定预期及中央默认/ASan。原完整断言/容器/CLI、stdlib配对发布与完整安全保持OPEN。

### 17.34 Typed output与显式配置初始化

唯一XrXirOutputStatus为OK=0、OOM=1、LIMIT=2、OUTPUT_ERROR=3、BAD_ARGUMENT=4、BAD_ABI=5；renderer、byte sink及group provider返回同一封闭状态。完整group准备失败不得调用sink；成功最多调用一次，按真实length保留embedded NUL、UTF8、空print换行及长字符串。IO已写出的bytes不承诺回滚。未知status归为BAD_ARGUMENT，不允许bool转换丢失资源失败。

PRINT失败保留准确Call状态。WRITE_STREAM在合法已配置provider下将OK映为true、OUTPUT_ERROR映为false；缺provider仍为Call OUTPUT_ERROR，OOM/LIMIT/BAD_ARGUMENT/BAD_ABI不得映为false。取消保持既有优先级。资源失败不成为panic，不能被assertPanics消费为成功。

当前Call ABI为21。Provider/Sink以abi_version与reserved=0开头；有callback先验21，无provider仅准canonical全零。CallConfig/InstanceConfig以abi_version和struct_size开头，在读取其它字段前验21及exact sizeof。Windows x86_64实测Provider24、Sink32、CallConfig136、InstanceConfig88；这些数值不代替其它target的实际ABI验证。唯一xr_xir_call_config_init(config,size)与xr_xir_instance_config_init(config,size)返回CallStatus，成功才写，失败保留全部output bytes；旧按值defaults符号删除，无别名或wrapper。旧Call20 entry必须在执行callback前拒绝；旧embedding defaults对象必须链接拒绝，不能用重编旧对象替代证据。

Checked22/57、Value16和Program26保持各自实际合同；输出接口迁移不授Source/Target/SDK权限，不证明完整安全、默认stdlib缓存或最终旧链删除。VM/native独立预期、每个真实分配失败、准确状态、旧对象拒绝、producer寿命及物理释放分别验证。

### 17.35 平坦 Tuple 解构与模块整组发布

平坦 `var` / `const` 模式接受名字或 `_`、精确有序 arity、Unit 与 `(x,)` singleton；RHS 一次求值，全部字段投影和可变 Cell 准备完成后再发布局部名字，闭包捕获沿原 Cell/拥有合同。空模式只接受 Unit；全 discard 不产生绑定或 module group。字段类型和 Sendable/visibility/构造/执行资格沿真实类型与 Checked 定义处约束及特化复验，不能借 Tuple 扩大泛型 Unit 或被包装资源权限。嵌套/rest、数组/对象模式、spread、match 和叶注解在当前入口具体拒绝，继续承接原合法语言目标。

唯一新增操作 SLOT_GROUP_INIT 追加 ordinal145，当前 OP_COUNT146，旧 ordinal 不变。type=Unit；args0 为 operands 表 offset，args1=m 为非Unit payload数；immediate 高u32为 first_slot，低u32为 n 个连续实际bindings。n>0、整区间属于同模块、实际initializer、未发布、精确 n/m、类型与权限全部预验；unused字段为零，Unit项不占SSA/payload，也不准用伪值ID占位。outgoing layout 为 m。全部阶段使用同一操作，VM 与生成C调用唯一 xr_xir_instance_slot_group_init(view,first,n,compact_values,m)。

真实 Call allocation domain、scratch、值上限和有界work承担整组准备：私有owned表先取得全部准入/复制/持有与清理资源，再核 execution_status。任一失败或提交前取消释放全部准备且不产生 published slot或trace。不可失败的commit先写完整slots、flags/count/order，再开始trace；trace重入可见整组，首trace取消不回滚已提交绑定。清理不临时分配，按真实backing owner返还物理存储；初始化失败继续粘滞。

此操作的 Checked semantic 为65，wire25与 Value21/Call26/Program29结构未变；现行版本仍只由 §17.6 实现常量定义。旧完整25/64包必须header早拒且零分配，当前包由各族独立固定角色构造，旧64完整前像保留为历史拒例，不能以writer输出刷新oracle。新SDK helper须在同源165项（160源+5archive）recipe及真实五archive消费者复验，原345 ABI表达式和139对象职责不减少。完整资格保留 Checked forged-shape、独立VM/native/mixed预期、全部真实OOM/三轴、retain饱和、取消、producer/Program/Instance先drop与最终physical0；平坦子片不代替完整产品默认门、完整安全或最后旧链删除。

### 17.36 Checked Library 普通泛型导入

本节冻结下一导入族，实施与资格尚未完成。沿§17.30–17.32的唯一Catalog、reader和Source remapper接入普通自由泛型函数及私有函数闭包。库自身为单模块、无依赖，TYPE binder只使用现有内建marker约束（无接口见证）；形参/结果为已准入i64/string/bool、Unit结果或该函数binder，操作仍为Catalog已准入集合。无约束参数、显式/推断/转发、所有未使用定义均由普通定义处检查；Sendable不授算术、成员或构造能力。调用方的合法新用户类型可经其接收Checked类型池实例化，不能因库自身没有nominal表而仅允许标量实参。RESULT binder、库自身constructed/nominal、slots、implementation、provenance和依赖仍待实际映射，不开放未接线表或新操作。

真实initializer、普通/私有function先保留目标ID，再用同一SourceLibraryMap重定位CALL、CALL_DEFAULT和literal。每函数Generic的constraints、arguments、parameter_kinds及计数在接收账本深拥有；binder BASE+p保持声明局部序数，指令type_arguments范围归其接收function。块/SSA属于函数局部。SourceContext.has_generics、SourceFunction.generic_owner和query声明的generic count/constraints/kinds对应接收身份；自由函数没有generic_parent，查询不免检查或授访问权限。全部生产者销毁后仍须使用原owned Checked完成特化/复验→Lowered→Program封存，不补AST或运行时实例化。复制计实际metadata/work，所有scratch出口退款、整库成功才发布。实际wire/Checked结构语义或runtime ABI未变不机械涨版本；若暴露真实变更须先冻结并补独立拒例。

准入验收须含private generic helper、两个库binder0/function0、反转输入顺序及改变接收ordinal、显式/推断/转发、新用户类型、未使用坏定义和private外部调用拒绝，销毁producer后的VM/独立packet/真native及双Instance与owned结果。保留原library/string/output门及全部新真实OOM、三轴exact/minus1、取消、物理释放和适用安全。此有限族不宣称完整库状态/依赖、native缓存配对、安装分发或全产品完成。

### 17.37 Checked 标准库出版与普通执行

标准库只提供一份 Checked XIR 语义输入，包含声明、泛型定义、私有实现闭包、模块初始化、效果和所有权事实。当前出版族为 io/output，不代表其余标准库族已获资格。库模板是经过验证的输入，不是可执行 Program 授权。Catalog 在调用方累计资源账本下验证完整 Checked body、规范模块身份和依赖；字节摘要不能代替语义复验。Catalog 深拥有物理根，输入和 Source 生产者销毁后仍有效。缺失或畸形的 advertised 输入拒绝，不回退到 Source。

Checked producer 只进行 Source 检查与 Checked 编码，不依赖 CGen、native 编译、object 清单或 runtime SDK；构建 producer 可执行文件本身仍需要支持的宿主 C 工具链。嵌入的不可变 Checked 字节和独立组件 manifest 不包含机器码回调。CLI、LSP、VM、AOT 独立于 native 代码取得同一 Catalog 输入。VM 对可达实例特化、复验、Lower 后按普通不可变 Program seal 接收，不查找或替换标准库函数为 cached native entries，执行期不需要 C 编译器。AOT 将库与用户 XIR 共同交给普通 C backend 编译。

删除标准库 native registry、matching factory、cache lease、配对 build identity、object/C 镜像及缓存专用 seal API；不保留兼容 flag、空缓存或第二执行路径。普通 native 函数、双向 VM/native mixed、共同 Value/Call/Program/Instance、可恢复 ABI、CGen、必要平台 host 和独立验证的 runtime SDK 保留。SDK 的实际 target/profile/ABI 及生成 native 代码准入仍有各自拒绝边界。

Core 只安装 Checked 组件及精确 module/length/digest manifest。SDK 组件独立验证 target、ABI、Source 和 archive manifest；SDK-only 安装不要求标准库配对。组件及规范相对路径在出版前拒绝畸形输入；验证失败保全已有 manifest。真实 Core/SDK/full 安装、搬移、无源码 VM/AOT、用户新泛型类型及缺失/损坏组件需要独立测试。完整标准库交付与删除当前已支持族的缓存是不同完成责任。

保留独立输出预期：stdout 为 out\0 后接 U+4E2D 的七个字节，stderr 为 err! 四个字节及空输入；保留两 Instance、四种 provider 结果、导入 leaf 与用户 wrapper、同名用户声明、producer death、escaped Function/result 寿命和物理释放。只退役 cache match/lease/object-pair 断言。每个实际 preparation、bind/seal 分配故障，资源三轴 exact/minus1、不补账本的 retry、失败保全 take 和零分配 cleanup 仍由普通路径覆盖。W1–W4、可移植 C11、适用 provider/sanitizer 门及既有 generic/library/output 责任全部保留。

### 17.38 共同根执行流事实与原因查询

在已完整验证的Checked/Lowered上，同一owned XrXirEffects推导每函数独立的requires_root与unresolved。两项可同时为真；缺字段、推导失败或空初始化不是NONE证明。mutable模块槽读写/PLACE及真实module initializer产生根要求；const读/PLACE在真实定义上下文证明Sendable，不满足产生根要求，OOM/预算失败原样拒绝。初始化group的first slot按既有high32编码读取。库var和库非Sendable const当前禁令保留。

复用唯一反向执行图传播CALL/INVOKE、真实default helper及已注册cleanup，cleanup只传播根事实而不改变既有控制效果规则。间接/requirement调用读取真实callable闭合上界，NO_SUSPEND与根权限完全独立。FUNCTION_REF不执行目标body；局部var/cell不因可变自动产生根要求。GO child不是父方普通调用边，父方实参准备的真实普通执行仍计入父方。未分类执行op拒绝；普通运行期路径取保守并集，不依赖优化删除。

查询返回同summary拥有的事实和两条独立最短numeric原因链；生产artifact死亡后仍有效，直到summary释放。局部原因distance0，CALL/cleanup步的callee distance严格下降；等距按真实instruction/callee/cause/slot身份排序，不靠邻接顺序。全部求解/原因/类型证明共享累计资源账本，失败不发布半摘要，occupied输出不覆盖。无参数化布尔图为O(I+F+C)，类型证明和高阶变量成本另外计费。本查询合同不改变wire/opcode/ABI，不代表GO消费者、函数值参数上界、执行身份或库var保护已完成。

GO消费者从同一summary的已闭合root事实取得权限，不再逐槽或按indirect种类重复推导；known root或unresolved均拒绝，二者并存时诊断优先known root且不丢两条原因。当前SLOT_PLACE借用形状限制独立传播，Sendable const的root NONE不解除该尚未资格的限制；其失败不得伪报root/unknown。角色、参数/结果及捕获Sendable、Task错误和所有类型/所有权/可见性规则保持独立，GO父实参准备仍在父方执行。Source区分需要当前实例根执行流与缺少worker效果证明；本片只保留既有函数层定位，完整用户函数/变量原因载荷与精确GO表达式位置继续未完成。

拥有式原因链复制接口从同一效果owner复制known与unresolved两条有限numeric链；facts与steps只借用独立trace自身，效果summary、Checked/Lowered产物及原生产者释放后仍可读取。有效NONE可得到拥有式空链。copy要求相同实际资源owner和空输出，先拒非法参数，累计预算预准入后原子发布；OOM/预算失败清理临时owner且保留已收费。free不再分配或格式化，最后实际资源引用与物理计数归零。该接口不增加wire或runtime ABI，不是完整Source名称、来源与GO位置诊断。

闭合callable沿现有u32 flags保存根效果上界：NO_SUSPEND=1、ROOT_NONE=2、ROOT_REQUIRED=4、ROOT_UNRESOLVED=8；根mask仅2/4/8/12合法，12保留requires_root与unresolved两事实。旧0/1、NONE与其它根位组合、未知位拒绝。普通显式fn默认UNKNOWN，不把NO_SUSPEND当NONE。身份包括完整flags及全部嵌套签名；仅最外层允许弱化，参数类型/模式、result与嵌套身份不变。NONE可进入NONE/ROOT/UNKNOWN/mixed，ROOT可进入ROOT/UNKNOWN/mixed，UNKNOWN或mixed仅可进入UNKNOWN/mixed。NO_SUSPEND只能保留或丢弃。失精度后不根据目标地址恢复权限。join独立并集两根事实、NONE中性，只有两边都有NO_SUSPEND才保留该位。

FUNCTION_REF构造不执行body，但其广告上界必须覆盖目标的同owner真实body事实；未使用的ref同样验真。每个真实interface implementation body也必须满足原requirement上界，不能靠接收上下文授予承诺。共同Checker在完整结构/类型/约束/访问验证后使用同一execution图验root上界及GO义务。ordinary refs的精准producer后继使用不可发布的owned prepared owner，精确绑定已验证Module及拥有式INFERRED/FIXED value-flow；固定显式fn边界不能被猜目标精化，未知依赖保守UNKNOWN。有限worklist闭合并重新普通复验前，prepared状态不构成Checked或NONE证明；没有skip_go、unchecked公共入口或第二产品管线。Source recipe/AST死亡后不能借用其指针。

这一flags语义采用唯一Checked semantic70，schema25的既有u32字段形状不变；公开Value22/Call28/Program29字段和布局不变。旧semantic69及旧0/1callable拒绝，不双读。SDK source fingerprint、独立stdlib Checked组件和消费者向量必须按真实资格更新。这里只冻结closed上界基础；Source精准refs、sealed Program真实entry上界准入、实际child indirect执行、有限参数公式、完整library types映射、用户完整原因链与library状态保护分别仍须实现验证。

普通引用准备与闭合：Source 在 region recipes 释放前拥有式记录 INFERRED/FIXED 值流身份。普通 direct ref 与 closure 在既有 Effects owner 上按真实目标闭合；显式 callable 参数、注解、结果及转换上下文保持固定上界。暂态 UNKNOWN 相等不能消除边界：私有 COPY 保留身份，闭合后有差异时转为唯一 FUNCTION_WEAKEN。私有结构/SSA/约束/可见性准备不产生 Checked，闭合后共同完整 Checker 对源与独立拥有式副本执行全部效果及权限义务，只发布该副本。

COPY/PHI/local Cell、闭合 capture、Nullable 包装、只读 Tuple/Array 构造和投影同步身份。PHI 和同元素 Array 的外层 callable 输入按唯一弱化规则归一化，根两事实分别求闭合。固定复合的嵌套身份不能协变；无法表达嵌套弱化的共享、元素变更或 Nullable 分支合流保守保留上界或拒绝，有限高阶依赖及容器别名流后继仍未完成。NO_SUSPEND、GO 借用形状、错误和 Sendable 义务独立保留。准备、闭合及查询复制共用原有限资源账本，不增加 wire 字段、schema、semantic 或公开 ABI。这一实现候选仍须鲜编和两配置资源、安全资格；runtime 许可与 VM/native 执行资格未由此取得。

### 17.39 有界 CTFE 的标量局部存储与短路

编译期执行以完整复验的 Lowered 为输入；bool/integer 普通函数的整个可达直接调用闭包先准入，再执行。SCALAR_LOCAL_NEW、SCALAR_LOCAL_READ 和 SCALAR_LOCAL_WRITE 使用与 VM 相同的 scalar step 和目标帧偏移；写操作产生 Unit 并继续顺序执行，不是控制流终结。读/写按实际帧访问收费，原 steps、全栈 depth/frame bytes、output bytes 和编译作业累计账本保持；callee 不重置预算，失败不退款并保全规范 Unit 输出。

Source 的 &&/|| 在该局部存储上保留短路语义，经原 Checked→特化复验→Lowered CTFE 求值，再独立重放完整 Source 并检查普通常量 recipe。未执行的纯算术错误分支可以跳过；未执行分支中可达 I/O 仍使整个闭包不准入。根权限或 effect=NONE 不授予确定性。

本节不准入普通 var 生成的拥有型 Cell、owned/path 操作、聚合/字符串导出、未知函数值或 native/Instance 执行；它们需要各自实际类型、生命周期与费用接口及独立资格，不从栈内标量存储外推。

### 17.40 CTFE有界CFG回边与声明身份接收

leaf CTFE接受经过完整Lowered复验的标量CFG回边；PHI在共同scalar edge核中按前驱选择并行复制。整个执行共享原steps/watchdog和编译账本：回边不重置或退还费用，非终结循环在有限steps耗尽时拒绝并保全规范Unit输出。实际Phi前驱、target、offset和签名仍由完整复验检查，执行器不按block ordinal推断可终结性。

CTFE要求实际普通函数声明身份。结构合法但没有declarations/functions表的Lowered不能授予执行资格；leaf/direct-call入口分别拒绝为NOT_LEAF/NOT_SUPPORTED，并保留输出和已消费费用。Module initializer、nominal/cleanup owner、实际不支持的操作及可达I/O仍逐项拒绝，root或effect标志不授予确定性。leaf保持无CALL边界，有限调用使用独立全闭包接收入口。

### 17.41 CTFE的规范Unit返回与普通Source重放

没有值参数的普通Unit结果函数可进入CTFE的实际闭包准入；其Unit RETURN保留零type/reserved/payload，目标逻辑输出大小为零。Unit返回没有SSA存储槽，子调用返回不得向该槽写入；调用者的标量参数与已活跃值保持。Unit不作为普通值参数开放，入口仍按实际类型和完整Lowered身份复验。

Source的Unit comptime结果经同一有限CTFE管线接收，再独立重放为普通Unit表达式，保留站点匹配、覆盖和规范结果验证。空块或末尾为普通语句的块产生Unit，拥有型局部状态继续依赖实际值域接点；comptime块不授予从外围函数非局部return的许可。I/O、非终结循环及不可接收的实际操作仍分别按闭包准入或预算规则拒绝，失败输出和原编译账本保持。

### 17.42 有界CTFE的Rune值与普通Source常量

Rune与bool/integer共同进入实际标量参数、返回、局部存储和直接调用闭包准入。CONST_RUNE、INTEGER_TO_RUNE、RUNE_TO_INTEGER使用已有共同执行核；Rune相等比较验证两边完整Unicode标量后比较码点，不把Rune解释为整数类型或宿主字符。合法范围为0..0x10ffff，排除0xd800..0xdfff；非法Rune输入拒绝为BAD_ARGUMENT，整数转Rune越界拒绝为NUMERIC_RANGE，失败保留规范Unit输出。CONVERT_NUMBER仍只准入整数之间转换，不由Rune开放浮点。

导出大小由规范目标layout确定，Rune为4字节，output_bytes=3拒绝；不使用宿主载体sizeof或UTF-8长度决定预算。Source通过实际Checked/特化复验/Lowered计算后，在独立完整重放中写入普通CONST_RUNE recipe，保持完整站点、类型、规范值和覆盖验证。嵌套comptime和普通Rune调用不绕过闭包准入；原steps/depth/frame/output上限、同一累计编译账本、真实物理OOM和最后owner释放责任不变。本族不授予managed聚合、字符串、未知函数值或运行期fallback资格。

### 17.43 原编译账本上的私有Cell执行与Unit尾赋值

CTFE在整个实际调用闭包准入后，允许函数帧内的Cell<Unit/bool/integer/Rune>。普通var的CELL_NEW/READ/WRITE、懒构造CELL_LOCAL_WRITE和LOCAL_UNINIT使用既有真实Value操作；类型元数据独立复制进编译TypeArena，值存储经原XrCompileResources支持的Domain分配。所有分配、复制、遍历与Domain work保持同一累计账本和真实物理FI；局部接收fuel只限制一次标量检查，不复制、重置或返还编译预算。无Cell操作的原标量入口不创建该Domain/arena。

实际owned offsets在正常返回、子帧返回、步数/预算/OOM/算术失败中全部释放。帧建立预付其清理offset扫描费用；清理不依赖剩余执行预算，也不分配。每次Cell访问验证同一私有Domain，参数仍限bool/integer/Rune，导出仍限规范Unit/bool/integer/Rune结果，不接收外部Cell、ref借用、模块状态或任意拥有值。类型、前驱/PHI、target、身份、签名、可达I/O、depth/steps/activation bytes及原输出上限继续全量复验，不从root/effect标志授予纯度，不调用Instance/native/fallback。

Unit块的最后赋值可带普通表达式的内部编号，但helper的Unit RETURN必须使用零编号，不构造Unit SSA槽或传递该编号。Source仍完整独立重放、核对站点与规范值并重新Checker接收；私有Cell执行只导出实际标量/Unit结果。本族不授予Cell/聚合/字符串导出、函数值、清理函数或挂起资格。

### 17.44 CTFE字符串拥有型导出与独立Source接收

String参数、结果及Cell<String>使用实际Value/TypeArena和原compiler Domain。外部字符串参数先按完整UTF-8字节复制为本编译账本上的独立快照；内部调用通过真实owned slot retain/copy/move，正常、子返回和所有失败释放全部slot。常量、拼接、拥有型局部复制、字符数len与相等比较使用既有Value语义，扫描/复制/比较span在执行前付原累计work，Domain分配/resize也接同ledger。不调用Instance/native/AST fallback，不因root或effect授纯度。

String输出大小是实际UTF-8字节数，不是宿主指针、carrier sizeof或字符数；空串在output_bytes=0可导出，超过调用者在17.61政策下选定的逻辑限额拒绝OUTPUT_LIMIT，失败输出保持Unit。发布前取得独立拥有引用，帧/Artifact/Source/ledger调用者死亡不使结果悬空；最后drop不分配、不退累计费用，仅退live。

Source每个原站点及重放站点分别持有真实结果；独立重放把完整字节复制到自己的普通literal/CONST_STRING recipe，再经完整Checker/query发布。内嵌NUL按长度保存，未知/重复/多余/类型错配站点仍拒绝。所有部分结果与第二次构造/费用/发布失败的结果均释放。目标/profile与原steps/depth/frame/output及编译三轴保持，未开放任意聚合/函数值/资源句柄导出。

### 17.45 有界CTFE的String比较和只读搜索

CTFE准入完整Lowered函数闭包中的String字节序比较（<、<=、>、>=）、contains、startsWith、endsWith、indexOf和lastIndexOf，调用既有Value操作确定语义，不引入native、Instance或AST求值路线。indexOf的start与两种索引结果都以Unicode标量计数，String长度也以标量计数；比较使用完整UTF-8字节序，嵌入NUL不截断。空模式、未找到的-1及start等于标量长度保持普通运行期规则；负start或超过标量长度返回NUMERIC_RANGE，Source给出value range violation诊断并保全输出。

可变扫描在调用Value操作前从原累计compiler ledger预付保守上界：字节比较跨度、每个候选字节的模式比较和循环责任、实际搜索算法需要的skip表初始化及模式访问、索引换算的UTF-8扫描均计入。所有加乘检查溢出；无法支付或溢出拒绝BUDGET，不开展无界宿主搜索。空模式与已判定的范围错误无需扫描。该预付不是新额度，不退款、不重置原128M work及64MiB allocated/8MiB live限制。

输入仍先复制到compiler Domain快照，异常、OOM、step或work拒绝释放全部帧拥有槽并保全调用者String。Source继续独立两次构造、普通常量recipe、完整Checker和查询发布；即使搜索条件使I/O分支不执行，实际可达闭包中的I/O仍拒绝。本族不授予任意聚合、函数值、资源或清理路径准入。

### 17.46 编译域内的不可变Tuple与独立常量接收

CTFE接收已经完整复验的类型池与普通函数闭包，迭代准入由Unit、bool、integer、Rune、String及这些类型组成的不可变Tuple。类型ID只在实际owner的描述池中有意义；循环描述、未闭合类型和含函数值/Cell/资源权限的字段拒绝，不从root或effect标志取得确定性。TUPLE_NEW按完整有序字段向量构造真实Value，Unit字段保留位置而无SSA槽；TUPLE_FIELD、内部Tuple参数/子返回、owned局部/并行PHI及私有Cell<Tuple>保持真实拥有引用和同compiler Domain。外部Tuple参数仍需有界跨arena接收接口，当前失败关闭拒绝。

类型描述DFS、摘要与所有实际元数据分配进入原ledger。嵌套接收费用按完整展开的类型形状预付，重复字段位置逐次计数，数据共享不退费；加乘溢出或无力支付拒绝BUDGET。构造参数向量按完整arity占真实activation字节，遍历scratch使用原剩余frame额度并计入resize新旧块同时存活的峰值。steps65536、depth128、frame65536、17.61规定的输出上限及原64MiB allocated/8MiB live/128M work保持，不重置或退款。

Tuple导出大小是按字段顺序递归相加的目标标量存储大小与String实际UTF-8字节，Unit贡献0；不计host指针、拥有头、type ID、carrier sizeof或填充。超原请求上限保全Unit输出。发布先取得独立拥有引用，类型arena、嵌套Tuple和String字段在Source/Artifact/ledger调用者死亡后仍有效；最后drop不分配，仅退live。

Source以实际类型结构跨owner匹配helper及重放结果，而非假定局部type ID相同。独立接收以迭代拥有帧保存字段引用，将每个字段写成普通常量和TUPLE_NEW recipe，并完整Checker/查询发布。NUL完整长度、Unit字段位置、闭合泛型和原站点/owner/expected覆盖保持；字段失败、OOM、预算、构造与发布失败均释放全部临时结果。当前准入不授予任意Array/struct/enum、Tuple相等、函数值或cleanup路径完整资格。

### 17.47 有界CTFE浮点位运算与目标常量

完整Lowered重验与整个直接调用闭包接收后，CTFE允许f32/f64参数、结果、普通局部、私有Cell与不可变Tuple字段。CONST_FLOAT、负号、加减乘除、比较及具体数值转换复用现有仅整数实现的IEEE位运算，不依赖宿主FPU、Instance、native或AST回退。次正规数、最近偶数舍入、规范NaN、无穷和有符号零沿用普通执行语义；浮点除零产生IEEE结果。浮点转整数越界、NaN和无穷返回NUMERIC_RANGE，失败保持Unit输出。

每条浮点操作在进入数值API前向原累计compiler ledger预付1024单位work。此保守固定上界覆盖64位格式的解码/打包、归一化、乘积进位扫描和长除法；这些循环只受固定格式位宽限制。预付不复制、重置或退还配额；未付清则先返回BUDGET。完整类型、身份、CFG、可达I/O及原steps/depth/frame/output/allocated/live/work上限继续生效。

f32和f64逻辑输出分别占四和八个目标字节，Tuple字段继续累加目标存储大小。f32输入必须没有高位且NaN编码规范；f64沿用普通Value接收规则。Source在独立重放中再次校验目标规范浮点payload，记录普通CONST_FLOAT配方并交完整Checker。外部Tuple、任意Array/struct/enum、函数值和cleanup路径资格仍未由此授予。

### 17.48 编译域内Nullable活跃分支与有界常量接收

完整Lowered重验和整个可达直接调用闭包接收后，Nullable可以包装已准入的基本值、String、不可变Tuple及合法嵌套Nullable。类型ID仍只属于实际不可变pool；迭代类型族接收拒绝环、非法element和带Cell/函数/资源权限的字段。NULLABLE_NONE/SOME、IS_SOME和UNWRAP调用真实Value操作，在原compiler Domain构造、观察或取得独立payload引用。内部参数/子返回、owned局部/并行PHI、Tuple字段和私有Cell<Nullable>保持完整拥有寿命。解包None返回独立NULL_UNWRAP状态，Source报告带位置的编译错误，失败保全Unit输出并释放所有帧。

逻辑导出从唯一TypeArena storage layout取得判别字节数，加上Some活跃payload的递归逻辑大小；None没有payload费用，不计算inactive存储、对齐填充、指针或owner头。当前canonical Nullable tag为一字节：None<i64>占一字节、Some<i8>占两字节，Some<i64>需九字节并触发原17.61规定的输出上限上限。迭代导出scratch继续计入原frame额度及resize时新旧块峰值；目标标量大小、String UTF-8长度和Unit Tuple字段零字节规则不变。

完整有限类型形状在每次Value接收前预付原ledger，活跃分支不能借None免掉型别准入。Nullable相等仅接既有共同Value相等支持的Nullable链及基本/String叶；规范NaN、有符号零和完整NUL字节沿用同一个语义owner。固定数值work和活跃String扫描跨度在比较前支付原累计ledger，溢出或无力支付先返回BUDGET。没有另建语义解释器、配额、退款或Instance/native回退。

Source独立重放按真实跨owner类型结构匹配，持有真实分支和字段owner，迭代记录普通NULLABLE_NONE/SOME与payload常量/TUPLE_NEW配方，再交完整Checker和查询发布。原站点/owner/expected覆盖以及每个失败前缀释放保持。外部Nullable/其他复合参数仍需有界跨arena完整快照接收；当前明确拒绝。Array/struct/enum、函数值和cleanup完整资格不由本族授予，原所有预算与正式安全门继续生效。

### 17.49 有界编译Array、私有根修改与capacity接收

完整Lowered和整个实际直接调用闭包重验后，CTFE接真实ARRAY_NEW/REPEAT/GET/LEN、CAPACITY/WITH_CAPACITY/RESERVE、SET/PUSH及私有Array Cell的CELL_PLACE/PLACE_READ/WRITE。类型族接收允许基本/String叶、纯Array链、接收跨度静态可界定的Tuple/Nullable元素及合法外层Tuple/Nullable包装。编译域内参数、子返回、Cell、owned局部与PHI仍用原Value/TypeArena/Domain，不运行Instance/native或AST fallback。root/effect不证明确定性。只有真实当前私有Cell根的同步place可修改；module根、外部Cell/ref和未接通的field/index投影继续拒绝。

类型形状只报告费用和可变接收跨度，不是Value/执行授权。所有元数据、向量、复制、扫描、扩容和清理预付都进入原累计ledger。Array浅层长度可在有界header观察后取得；普通接收跨度按长度乘完整静态元素形状的保守上界在调用Value之前支付，检查加乘溢出。Tuple/Nullable包装中的可变Array跨度用迭代拥有访问定位并提前付清，不依靠宿主递归。scratch计入剩余原frame额度，导出stack与Value接收scratch的同时峰值一并计算。未付清先BUDGET；原预算、步数、深度和失败不退款规则不变。

现有array_get会先接收整个Array。若某元素又包含自身需遍历可变Array内容的接收边界，如Array<Array<Tuple<...>>>或Array<Array<i8?>>，调用前无法借现有API有界取得内部长度。该具体族当前fail closed，需Value owner提供在每次访问前付费的浅层子观察/完整接收接口；不改成另一Value表示、不先无预算扫描、不将该接口缺口描述为整个Array目标完成。纯Array<Array<i8>>无需这种深接收，可以执行。任意Array相等、其他Array方法与完整路径仍未由本节授予。

Array逻辑导出递归累加活跃元素的目标标量、String字节、Nullable判别和Tuple字段大小；空数组零字节，不计capacity、指针、owner头或inactive存储。实际capacity仍是普通语言可观察值，独立Source重放以WITH_CAPACITY、私有Cell、逐元素PUSH和CELL_READ配方精确保留它，不能把Array重新建成具有不同capacity的普通字面量。所有临时owner失败前缀均释放；导出类型、元素及账本在Source/Artifact/调用者死亡后仍有效，最后drop无分配且只退live。

真实COW修改保留旧快照。负长度/capacity返回NUMERIC_RANGE，索引越界返回追加的compiler-only BOUNDS状态；Source给带位置的编译错误。失败保全Unit输出及借用值。外部复合快照、有嵌套可变接收的元素、nominal/函数值/cleanup和正式共享/完整安全资格继续按原目标实施，不能从此有限开发资格推断完成。

### 17.50 原账本预付的共同Array/Nullable相等

在完整Lowered、实际类型族及整个可达调用闭包接收后，XR_EQUAL可比较已准入的Array/Nullable链与共同Value相等支持的基本/String叶。只由既有xr_xir_value_equal产生布尔结果；预付遍历不另写数值、String或容器相等规则。共享backing或地址相同不能证明自反性，NaN数组与自身仍不相等，正负零沿共同IEEE规则相等。capacity不参与内容相等，NUL保持完整String长度。Tuple/nominal或带执行/资源权限的比较族仍要求其真实owner支持，不能凭类型名字猜能力。

迭代拥有的成对预付遍历在每次header/型别/child访问前收费。浅层Array长度和Nullable活跃分支限定后续跨度；每次真实Array getter之前付清其完整接收跨度，String完整比较字节和各浮点叶的固定宽度费用在共同比较前支付原累计ledger。上界保守，溢出/无力支付先BUDGET，所有费用不重置、不退款。长度或None分支不需要未访问payload扫描；实际比较仍调用同一Value语义。没有unbounded host walker、Instance/native执行或AST回退。

预付stack使用原剩余frame限额，包括resize新旧块和getter接收scratch同时峰值；共同比较开始前释放该stack和所有临时引用，再独立约束共同Value scratch。任一物理OOM、work/step/frame失败都保全Unit输出、原借用值及累计收费，释放全部临时/帧owner。Nullable旧专用相等分支由此共同路径原子替换；Source仍两次构造、独立普通BOOL常量与完整Checker/query发布。原全部预算、安全门和更深变量接收的具体Value观察接口缺口继续生效，开发资格不等于完整P11。

### 17.51 私有Array索引路径的有界接收与写入

CTFE在完整Lowered及整个可达闭包准入后支持以真实私有Cell为根的INDEX_PLACE链。每个投影只保存类型和已计算索引；执行读写时重新解析当前根，不持有跨操作内部地址。索引链必须回到已准入、当前compiler Domain内的Cell；字段路径遵守17.53，私有Class对象根遵守17.60；模块根、外部Cell及投影Cell权限不会因此获得准入。完整类型族限制不变，尚无有界接收接口的更深变量Array仍明确拒绝。

路径计数与构造、每次类型/头观察、完整getter接收跨度、最终公共路径接收以及可能复制的每个祖先capacity字节，均在实际访问前预付原累计账本。push还预付叶Array可能增长的容量。溢出或无法预付先返回BUDGET。路径步骤本身占用原frame剩余额度；观察和最终Value操作的scratch只可使用扣除路径后的余额。所有临时观察owner在共同写入前释放，避免人为制造共享；唯一Value path read/write/push决定普通语义、COW及发布，不能另造CTFE写入算法。

嵌套set/push、PLACE_READ/WRITE及普通reserve读/构造/写回配方沿这条路径执行。现有普通Array方法配方可继续组合已准入操作，包括contains/indexOf、String join、clear/reverse/unshift/pop/shift/resize/fill/concat/entries；各自仍须通过实际opcode、类型和完整闭包检查。内部函数值回调遵守17.55，数值格式化遵守17.52。Source保留独立普通常量重放；路径结果导出仍按活跃逻辑字节与原output帽。

已有快照保持普通COW语义。任一物理OOM、越界或work/step/frame/output错误保全空输出并释放全部帧与临时owner，累计费用不退款。Source/Artifact及ledger调用者死亡后，最后拥有结果仍保留真实类型/Domain寿命，最终drop无分配。路径开发资格不替代完整P11、公共sanitizer、provider和其他原门。

### 17.52 共同标量文本与原账本的格式化费用

TO_STRING在完整Lowered及整个可达闭包准入后支持bool、整数、Rune和f32/f64。唯一`xr_xir_scalar_text`产生普通十进制整数、true/false、完整Rune UTF-8和最短往返浮点文本；CTFE不使用Instance输出或另一套转换器。浮点仍沿仅整数的共同IEEE格式化，正负零、subnormal、NaN/inf和指数记法不依赖宿主FPU或locale。NUL按长度传递。普通Array数值/bool/Rune join配方由相同TO_STRING接通；该准入不授予I/O、身份或外部状态权限。

每次格式化前先支付有界元数据观察，随后预付原累计账本的算法费用和原frame中的固定工作区。整数/bool/Rune使用256 work、512 frame字节；浮点零及非有限值使用1024 work、512 frame字节。有限非零浮点以正常数的无偏二进制指数、或subnormal最低可能指数的绝对值d，预付131072+(d+2)×2048 work和16384 frame字节，覆盖至多17位/每位9次减法、全部bigint复制和正规化；费用判定只观察位宽/指数，不决定文本语义。共同decimal workspace的sizeof≤1024由编译断言保持，扩大它必须重新证明费用和frame上界。剩余frame不足或无法预付先失败；不得先进入格式化再检查预算。String构造的真实复制、分配和所有权继续计入原Domain/ledger。

逻辑输出仍按完整UTF-8字节数遵守原17.61规定的输出上限；内部较长文本可用于比较，不能把内部长度误当导出权限。独立Source重放普通String/Bool常量并经过完整Checker/query。物理OOM、work/step/frame/output拒绝保全空输出与借用参数，费用不退款，释放全部帧/临时owner。真正格式化得到的最后String在Source/Artifact/ledger调用者死亡后保持有效，最终drop无分配。声明绑定nominal与内部函数值分别遵守17.53/17.55；cleanup、foreign接收及正式完整安全资格仍按原P11责任推进。

### 17.53 声明绑定的结构体、活动枚举与私有字段路径

CTFE在唯一完整Lowered及整个可达闭包准入之后执行已关闭的普通struct/enum。类型族逐字段检查、拒绝循环及未准入native类型；内部Class身份与实际图接收遵守17.60；声明owner的lexical准入见17.54；它不代替函数体、效果或权限检查，也不代表class实例。STRUCT_NEW/GET、ENUM_NEW/TAG/GET使用唯一Value构造、接收和拥有slot；普通方法与泛型特化仍经过既有Source/Checker和闭包准入。枚举仅观察实际活动variant，不执行失活payload。

FIELD_PLACE与INDEX_PLACE可以组合，但根必须是本次compiler Domain内真实私有Cell。STRUCT_SET和普通字段/Array写入、push沿同一Value path实现。每次观察及完整接收前预付原累计ledger费用，可能复制的struct字段向量和Array祖先capacity一并预付。路径、显式遍历栈和同时存在的接收scratch共享剩余frame；观察owner在共同变更前释放，保留正常COW和单次发布。类型形状只用于计费，不是另一种Value表示或权限证明。

拥有结果按活动字段递归计算目标标量与String字节、Array元素、Nullable标签和enum真实storage标签字节；空struct为0，单variant空enum可为0，多variant空分支仍计实际标签。padding、capacity、拥有头和指针不计输出。拥有结果不得先调用不适用的独立nominal storage布局查询再按指针大小放行；唯一TypeArena storage及逻辑遍历决定导出，原17.61规定的输出上限不变。

Source从同一不可变输入图独立重建普通STRUCT_NEW/ENUM_NEW配方；跨池接收核完整声明顺序/identity、字段权限/名称、variant范围、native指纹和关闭字段类型，不能只比较局部type ID。元数据和字节比较在访问前计入原账本，最终完整Checker及query继续决定权限与发布。外部nominal/Class参数仍不准入，内部Class执行遵守17.60；缺少有界观察接口的struct动态child和更深Array接收仍明确拒绝，不能无预算预扫描。外部私有拥有结果的物化权限与Unit字段Source前置尚需完整资格，不据内部方法成功宣称覆盖。

物理OOM及work/step/frame/output拒绝保全空输出和借用输入，全部临时/帧owner释放，费用不退款。真正struct/enum结果在Source、Artifact和ledger调用者死亡后仍有效，最后drop不分配。该范围开发证据不替代函数值/cleanup、foreign接收、P12以及原全部共享/provider/正式安全门，完整P11目标保持开放。

### 17.54 编译期helper的真实lexical声明权限

普通成员内的comptime helper与普通嵌套函数沿相同规则继承真实lexical nominal owner，并使用既有MEMBER_HELPER身份。它不捕获运行期参数/局部状态，不授予调用者其他声明的权限，也不因helper名或结果类型伪造owner。嵌套comptime继续继承实际外层helper的声明上下文；开放泛型等原拒绝边界保持。

Checked特化及完整Lowered接收之后，helper查找须匹配模块、唯一名称、零参数、关闭结果类型、真实nominal owner及member kind，并拒绝cleanup身份变化，不能假定所有helper都是无owner的顶层函数。观察与比较先付原累计work，任何错配失败保全已有结果和临时owner。

声明owner本身不是class值或效果授权。纯class静态普通函数可在完整闭包准入下计算；内部临时Class与身份操作须满足17.60，Class身份导出及I/O/native/外部状态仍不获准。私有字段构造/调用只在真实声明owner中合法，独立Source重放保留该普通Checker权限。已经得到私有拥有值不授予外部函数私有构造权限；此类结果的完整物化机制仍属原目标，未实现处须拒绝，不能借反射/内存写入或runtime fallback绕过。

### 17.55 内部真实Function值与有界间接调用

CTFE在每次完整Lowered、identity/target/profile复验后，将实际direct CALL和全部FUNCTION_REF目标一并纳入本次可达闭包；尚未执行的回调目标也不能隐藏I/O或其他未准入操作。FUNCTION_REF、FUNCTION_WEAKEN及CALL_INDIRECT使用既有Value Function工厂、不可变capture向量、owned slot和同一有界activation机器。普通函数返回函数、高阶参数、PHI、Tuple/Nullable内部持有，以及已有Array map/filter/reduce普通配方可以组合已准入操作；不调用Instance/native/宿主函数体或AST解释器。

每个实际Function带本次执行私有lease，按原资源owner登记。接收时先在活跃登记表匹配owner，再核原Artifact、真实producer指令/type、原FUNCTION_REF entry/capture及本次可达集合，不能先解引用任意binding.owner。弱化必须通过既有callable weakening规则；新lease独立保存原producer事实，不借悬空父lease。capture由共同Value持有，最后释放仅摘链和释放原资源，不分配。外部Function参数在读取payload或调用其准入回调之前拒绝；Function及任何含Function的结果不能导出或写成Source常量。

按值capture保持共同Value所有权。可变capture只接受本次新推导的真实Cell provenance中OWNED_CAPTURE角色，且必须处于当前compiler Domain、无借用、非投影、非module storage；Function capture仍拒绝scoped/ref和cleanup角色；内部调用参数与静态cleanup另遵守17.56。每次私有Cell/path写入之前，迭代观察输入的真实Function capture与Cell快照，拒绝通向目标Cell的回边，保全旧值；无环共享和替换并不因此禁止。该检查不制造另一种值表示，也不读取私有对象头。缺少有界接收的动态struct child、Array<Function>等仍fail closed。

类型/签名、登记表扫描、capture向量、Cell元素形状、完整getter/复制和最终共同Value接收均先付原累计work；显式观察栈和同时接收scratch占原剩余frame。所有调用共享原steps/depth/frame，仍为steps65536、depth128、frame65536、17.61规定的输出上限及work128M/allocated64MiB/live8MiB。OOM、耗尽、非法闭包或循环写入保全空输出/借用值并释放每个activation、capture和lease，累计费用不退款或重置。内部Function准入不授予外部权限；静态cleanup/ref调用遵守17.56，P12和正式共享/provider/安全资格仍须独立验证。

### 17.56 静态cleanup frontier与真实编译期Cell借用

完整Lowered/identity/target/profile复验后，direct CALL、全部FUNCTION_REF与CLEANUP_REGISTER目标共同构成本次准入闭包；未执行的cleanup目标也不能藏I/O。CLEANUP_REGISTER激活既有不可变注册记录，CLEANUP_LEAVE沿实际frontier的祖先链按LIFO调用真实cleanup函数。只有该child正常返回后才退休注册记录；嵌套cleanup、作用域结束及循环continue/break沿同一规则执行。所有控制流边及并行PHI仍走共同scalar edge核，不另建表达式或Value解释器。

RETURN遇到活跃frontier时，先用共同Value保留实际返回快照，再退出到frontier0；cleanup改变私有Cell不能追溯改变已捕获的返回值。原拥有slot、COW和Value copy/drop决定寿命。最终结果按原目标逻辑大小导出；临时返回快照在失败或frame释放时也必须释放。预算/物理OOM等编译作业中止不发布部分结果，仍须在不分配、不退款的终态路径释放全部frame、快照、lease和loan。

内部Cell参数继续由本次新推导的真实provenance区分：Function capture只接受OWNED_CAPTURE；普通ref调用接受SCOPED_REF；cleanup参数接受LEXICAL_CLEANUP且必须匹配实际cleanup_owner和正在退出的直接父frame。callable的ref模式描述存储，不单独授予借用权限。实际Cell必须属于当前compiler Domain、非module storage；scoped projection另遵守17.57，拥有capture仍不得接projection。外部Cell参数仍在payload接收之前拒绝，cleanup声明也不能作为独立外部CTFE入口调用。

每个真实compiler activation使用私有身份、稳定frame及本次有限epoch构成既有CellAuthority。loan记录在进入前按全部Cell参数数量预留，计入原live frame；现有Cell loan prepare/commit/abort/release拥有唯一借用语义。只有直接活跃父authority可以重借；所有参数完成准备且后续发布费用可支付后才统一commit。任一失败逆序abort已准备前缀；正常或失败pop逆序release，精确恢复父loan。Cell读写及同步Array/字段路径使用既有authorized Value操作，不能凭type ID、持有引用或root标志绕过权限。借用期间不把scoped Cell伪装成拥有capture。

参数类型、元素形状、别名检查、frontier查找/迁移及所有可变接收先付原累计work；loan终态释放费用预付，scratch仍限原剩余frame。所有cleanup和ref调用共享原steps/depth/frame及原128M/64MiB/8MiB、17.61规定的输出上限限额。缺省declarations继续明确拒绝，入口guard不能先解引用缺省表。scoped projection遵守17.57；Error cleanup遵守17.58，panic handler/cleanup遵守17.59；外部有界接收及其他未准入族仍fail closed；内部ref/cleanup不授予P12、共享/provider或正式全安全资格。

### 17.57 有界scoped projection与当前根的真实借用

完整Lowered及整个direct/FUNCTION_REF/cleanup闭包准入后，CELL_PROJECT接既有Value工厂，为当前compiler Domain私有Cell的INDEX_PLACE/FIELD_PLACE链构造真实描述符。描述符拥有已求值selector和规范存储根，不缓存内部地址或叶快照。先完成receiver与全部实参求值，再由真实loan_prepare_path重验当前根、完整路径和父scope；所有准备成功且费用可支付后统一commit。借用期间通过既有authorized path read/write/push访问，空suffix表示当前借用scope本身；再投影扩展现有selector，只有真实父authority可以重借。既有provenance角色、别名拒绝、精确父loan恢复和静态cleanup规则继续适用，不授予外部Cell、module/object根或拥有capture权限。

只在闭包实际含CELL_PROJECT时，预扫描真实可达Cell创建点的Array/struct元素形状，元数据操作之前向原ledger支付最大形状的128倍。首次投影借用准备时，父authority仍可合法读取完整规范根；实际有界接收遍历的已付work与类型形状保存在该loan记录，临时根快照在借用准备之前释放。后继重借只从当前父loan继承费用上界；该数字不授予读取根或扩大scope的权限，真实Value loan仍独立复验。当前可准入动态根是元素接收跨度静态可界定的Array，其长度不能经严格后代loan改变；其他可准入根接收跨度静态可界定。尚无完整有界观察接口的更深变量族继续拒绝，不无预算探测私有头。

每次投影访问预付根接收work乘以4×(根形状+1)，覆盖公共path操作的前缀、suffix及完整接收。修改还先读真实Domain live统计，预付live_bytes×根形状×32，覆盖同一无环路径可能发生的祖先COW与保留；Array叶增长另按真实容量及元素storage付费。所有乘法先检溢出，费用不可支付时在Value访问/修改前BUDGET，累计账本从不退款或重置。观察快照在修改之前释放，保留既有COW副本；可变拥有结果可在Source/Artifact/ledger调用者死亡后继续持有，最后drop不分配。

临时selector、共同接收scratch和所有同时存活activation共享原frame；child分配后，参数接收和loan准备只能使用再扣除child字节的余额。Value的scratch不足可能沿既有LIMIT映射为BUDGET，显式frame检查仍返回FRAME_LIMIT；两者均保全空输出并释放已准备前缀，不能伪装为成功或放宽上限。所有step/depth/output与原128M/64MiB/8MiB及65536 frame不变。该族没有公共Value/Call/Instance/SourceContext或schema/opcode变更。Error cleanup遵守17.58，panic handler/cleanup遵守17.59；普通Source ref lambda、foreign/deep-variable接收、P12及完整共享/provider/安全资格仍为独立剩余责任。

### 17.58 内部Error、显式错误continuation与有界cleanup

每次完整Lowered/identity/target/profile复验后，CTFE把INVOKE与direct CALL、FUNCTION_REF和cleanup目标共同纳入实际可达闭包。INVOKE_INDIRECT仍只调用本次真实Function lease已经验证的目标；未执行的错误分支或cleanup也不能隐藏I/O、挂起或其他未准入操作。ERROR_ERASE/IS/NARROW直接使用共同Value的真实enum视图、精确声明身份与既有权限规则，不制造另一Error包装、Value表示或解释器。Error可在准入族内作内部参数/返回、Cell、owned局部、PHI及Tuple/Nullable/enum持有值；外部Error参数及Error或含Error的拥有常量导出继续拒绝，不能借Error擦除扩大类型/声明或执行权限。

真实可达enum创建点的完整形状，与投影所需的Cell根形状共同决定128倍浅层metadata预付上界。opaque Error先付该原ledger费用，再经公开error_borrow取得实际enum视图；完整有界接收另按实际活动字段、String/Array跨度、Function capture及Cell付费。Tuple getter和Nullable unwrap返回Error时也先付实际opaque元数据上界。内部Error类型只标记变量接收，不为其他拥有族绕过原类型限制；尚缺有界观察接口的Array<Error>、动态struct child及其他深变量族继续明确拒绝。私有Cell写入前，Error内的Function/Cell图仍沿既有观察路径检查目标Cell回边；合法无环持有与替换允许，环不能由Error隐藏。

成功INVOKE沿真实正常边进入专属INVOKE_RESULT或INVOKE_DISCARD，Unit正常边不伪造值槽。错误先保留真实concrete enum快照，沿该frame的frontier执行LIFO cleanup；每个注册仍只在cleanup child正常返回后退休。跨普通CALL传播先结束child、逆序释放其loan、恢复真实父authority，再将保留错误交给父frame退出。对应INVOKE接收则沿专属INVOKE_ERROR边保留共同Error视图，原shared scalar edge负责真实predecessor与并行PHI；marker必须绑定实际callsite。当前frame的CLEANUP_ERROR保持已求值错误快照，在离开相应scope后沿原错误continuation继续，不能因为cleanup改变Cell而追溯改变抛出的String/Array值。

cleanup内部可有完整处理的try/catch与调用，但其定义仍须满足真实Source/Checker的不逃逸throw和不挂起规则；不能把NO_THROW或root标志当确定性证明。预算/OOM等编译作业失败从不进入语言catch，仍保全空输出并在不分配、不退款的终态路径释放全部frame、pending error、返回快照、lease与loan。未捕获语言错误在适用cleanup完成后返回compiler-only UNCAUGHT_ERROR，Source在原comptime站点报告编译错误；无限cleanup仍受原step/work/frame/depth上限而不能抢先发布未捕获错误。

所有metadata、接收、retention、continuation与清理迁移费用计原累计128M work/64MiB allocated/8MiB live；steps65536、depth128、frame65536、17.61规定的输出上限不变，错误槽和同时存活child/receiving scratch计原frame。Error不授予外部输入快照、Error常量导出或宿主执行。panic handler/cleanup遵守17.59；普通Source ref lambda、外部私有物化/Unit前置、有界foreign/deep-variable接收、P12及完整共享/provider/正式安全资格仍为独立责任。本族不改变公共Value/Call/Instance/SourceContext布局、schema或opcode。

### 17.59 有界panic handler、真实PanicInfo与cleanup传播

完整准入的Lowered闭包可含panic保护块、PANIC_CATCH/CODE/MESSAGE、ASSERT_CONDITION与MATCH_FAIL；所有handler、cleanup及Function目标仍须每次完整检查，未执行分支不能藏I/O或挂起。除零/余数零、数值范围、null解包、Array/投影路径越界、断言和不完整match沿既有共同numeric/Value语义产生panic。只有语言故障进入panic通道；原ledger耗尽、OOM、step/depth/frame/output上限、非法artifact或权限拒绝绝不成为可捕获panic，assertPanics也不能把这些失败当成功。

每个compiler activation独立拥有pending panic payload，并把该carrier计入原frame；pending返回值、Error和panic使用各自真实拥有状态。bounds原语返回的code/index/length在该次失败时立即保存，不重复求值索引、不重查cleanup变更后的长度。断言保留原已求值String快照与精确字节/长度，包括嵌入NUL；cleanup之后的Cell写入不能追溯更改message。PanicInfo使用共同不可变Value工厂，CODE/MESSAGE使用公开getter与唯一规范消息格式化器，不读取私有对象头，不创建第二种Value或宿主异常对象。固定metadata/work与格式化scratch先付原ledger/frame；内部PanicInfo形状按固定32 work单元计入复合接收上界，不能将其用作外部权限证明。

同frame故障按实际故障指令所在block选择panic handler，先沿frontier执行跨越的LIFO cleanup，再进入专属PANIC_CATCH marker；handler不伪造普通CFG predecessor。跨frame先完成child cleanup、释放真实loan、恢复直接父authority和剩余frame，再根据原真实callsite的保护块传播。INVOKE的Error边不能消费panic，panic handler也不捕获enum Error；普通成功、Error及panic仍是独立continuation。嵌套handler及handler再次panic使用各自当前位置的保护关系，不能错误复用上一次landing。

cleanup可在内部完整捕获panic，包括外层已持有返回/Error/panic时；每帧payload防止内层捕获覆盖外层正在展开的消息。panic逃逸cleanup时，CTFE在无分配终态释放本作业全部拥有值和loan，返回compiler-only CLEANUP_PANIC并在原comptime站点报告编译错误，绝不调用运行期fatal/进程终止接口，也不能交给外层语言handler消费。此编译边界不修改运行期cleanup逃逸panic的既有fatal规则。未捕获普通panic完成适用cleanup后返回相应编译失败；无限cleanup先受原step/work上限，不能提前发布未捕获panic。

真实PanicInfo可在已准入族内作内部参数/返回、Cell、PHI、Tuple、Nullable、Array、struct/enum和Function捕获，仍使用共同拥有/接收规则；它不是Sendable，不能由CTFE外部真实PanicInfo参数或含PanicInfo常量导出获得权限。普通Source独立recipe只接已准入可导出结果，不重放PanicInfo。原128M work/64MiB allocated/8MiB live、steps65536、depth128、frame65536及17.61规定的输出上限保持；失败输出为空，费用不退款/重置，最后拥有值释放不分配。外部私有物化/Unit前置、有界foreign/deep-variable接收、普通Source ref lambda、P12与完整共享/provider/正式安全资格仍独立。本族只追加compiler-only失败状态与私有执行状态，不改公共Value/Call/Instance/SourceContext布局、schema或opcode。

### 17.60 私有临时Class身份与有界实际字段图

在每次完整Lowered/identity/target/profile及整个可达闭包准入后，CLASS_NEW/GET/SET使用现有Class Value工厂与get/set，保持真实Checked声明、构造和字段权限。对象只由本次compiler Domain内可达工厂创建；没有Instance、宿主callback或第二Value表示。内部Class可经普通参数/返回、拥有局部、Cell、PHI、Tuple、Nullable、活动enum/Error和Function capture流动，仍须满足各族限制。外部真实Class输入、Class及包含Class身份的常量导出拒绝；身份句柄不是常量字节。

类型费用将Class句柄的固定header验证计32单元，不递归猜测可能递归的声明图。实际接收在原ledger上的显式拥有栈逐字段访问真实对象；只调用浅层公开class_get，先付根/字段metadata、实际Array接收span与同时存活scratch。它不读取私有对象头，也不先运行完整未付费receiver扫描。每次写入先观察完整当前receiver及输入图，拒绝通向实际目标Class的回边；句柄相同仅用于回边拒绝，真实Domain/arena、存储和借用仍由Value API认证。无环共享、别名可见字段修改和替换保留普通身份语义；不能把抛出的Class身份当不可变字段快照。

OBJECT_PLACE代表完整Class根，FIELD_PLACE才增加字段selector。经已求值selector的字段/Array路径先付接收与可能祖先复制，再使用真实帧authority prepare/commit短整对象loan，通过共同authorized path操作，所有终态allocation-free释放loan。路径不保存内部地址；若路径在首根之外跨另一Class身份，当前入口明确拒绝，须从实际子对象另建合法根。ref投影仍由唯一Cell projection工厂与真实父子loan管理。Class根的初始投影借用要求所有字段为可导出纯数据类型，且leaf接收形状静态；因此窄loan期间原根接收上界不因另一可变身份或leaf变量图增长而失效。scalar/String及受支持纯Array链可借用、重借用和cleanup写回，Array标量leaf可增长；Class-bearing根和动态leaf组合仍明确拒绝，未以扩大预付预算绕过。

字段get返回真实拥有值；返回String快照不受随后Class字段替换影响，并在Source/Artifact/ledger调用者死亡后保持有效。普通返回、Error与panic的LIFO cleanup、真实父loan恢复和资源失败语义继续遵守17.56–17.59。完整物理FI包括构造、字段图接收、投影/cleanup、拥有String和回边拒绝的所有已观测分配点，失败后同一原owner重试，保持空输出及live归零。遍历栈/loan/路径与共同scratch计原frame；work耗尽后重试不能重置费用。

原128M work、64MiB累计分配、8MiB live、65536 steps/frame、128 depth和17.61规定的最大逻辑输出；其他限额不变。本族不改Value/Call/Instance/SourceContext ABI、schema或opcode。Array<Class>、动态struct child、缺少有界观察的foreign/deep-variable接收、私有外部Source物化、原P12较大输出目标及完整共享/provider/正式安全资格仍有独立未完成项；私有Class通过不代表完整P11或生产发布。

### 17.61 版本化8MiB逻辑输出政策与原预算保全

compiler-private输出政策版本为2，XR_XIR_CTFE_MAX_OUTPUT_BYTES固定为8388608字节（8MiB）。leaf、有限调用与Source独立重放使用同一上限和唯一既有evaluator；不保留旧硬帽开关、第二入口、分片或截断。调用者仍可请求0、7、8等更小逻辑上限，实际结果超过请求返回OUTPUT_LIMIT。大于8388608的请求在任何求值/账本消费前BAD_ARGUMENT，保持规范空输出；等于硬帽的请求合法，但不保证能在其他原资源限制内生成同样大小的结果。

逻辑大小继续由真实目标活动值决定：整数/Float/Rune/bool按目标存储宽度，String按完整UTF-8字节，Tuple按有序字段总和，Nullable/enum按真实活动标签与payload，Array逐活动元素计数，Unit为零。指针、宿主carrier、对齐padding、容量、对象头和失活字段都不计输出。十项Fibonacci [0,1,1,2,3,5,8,13,21,34] 的Array<i8>为10字节，(21,21)的(i64,i64)为16字节；显式8字节请求仍拒绝两者，10/16精确请求成功而9/15拒绝。Nullable<i64>、(i64,bool)及带单个i64活动payload的双variant enum均为逻辑9字节，不因存储对齐变成16；旧9字节String与11字节布尔join在新政策下按真实完整值接收。

原同一compiler ledger的128000000 work、64MiB累计分配、8MiB live、65536 steps/frame及128 depth不变；更大的逻辑上限不能增加、克隆、重置或退款任何费用。输出遍历、接收、独立Source普通recipe、完整Checker/query和所有失败前缀继续计原owner，临时栈及共同scratch继续计剩余frame。结果可能在达到逻辑硬帽前先耗尽work/heap/frame，保留对应原资源拒绝；不能把这种拒绝改成成功或推定OUTPUT_LIMIT。资源失败不进入语言catch，失败后同owner重试仍受累计消耗约束。

结果保留真实TypeArena、Domain与嵌套拥有字段；完整向量在Source/Artifact/ledger调用者死亡后有效，最后drop无分配并归零live。Source从同一不可变图独立重建完整Array/Tuple/String等普通配方，保留真实容量、类型与字节长度；没有CTFE指针写入Checker。版本2不授予Class/Function/Error/PanicInfo身份导出、foreign跨arena接收、私有构造权限或新副作用。完整闭包、target/profile/Checked摘要与已有准入表继续复验，不修改Value/Call/Instance ABI、schema或opcode。

政策版本2及硬帽属于compiler产物/未来持久CTFE缓存身份，不能只用公开签名或Checked函数摘要。任何持久结果键还必须包含完整私有依赖/准入/编译器与XIR身份、目标profile、授权、全部请求限制和原预算策略；较大预算成功不能变成较小预算的免费命中，失效失败不能由旧政策复用。当前evaluator与Source路径没有持久结果缓存；每次请求均完整验证、执行和计费，内部类型费用缓存只在单次执行内存活。该政策身份不宣称已实现缓存系统。局部Windows验证不等于完整P11、完整provider/sanitizer资格或生产发布。

### 17.62 Source每请求逻辑结果限额

compiler-private Source请求提供可选XrXirSourceComptimeLimits.output_bytes，借用仅在同步检查内有效。NULL政策使用17.61的8388608上限；显式0只允许逻辑零字节的comptime结果，不表示默认。请求数字在发现/构造前复制进SourceContext，第二次独立Source构造保留同值；每个comptime站点向唯一既有CTFE请求传递此上界，未改变evaluator、Value表示、输出计量或任何其它原预算。

上界大于8388608的Source请求返回BAD_STRUCTURE，求值/额外账本收费之前拒绝且不发布结果；占用Checked/snapshot输出同样保全。8字节Source请求对原9字节Array、十项Fibonacci及16字节pair返回BUDGET、位置有效且无Checked/snapshot；9/10/16精确请求可接收完整独立recipe并经普通Checker/packet/VM核值，minus-one仍拒绝。Unit与空String在0限额可接收；普通运行期返回值不因未发生comptime求值而受此输出帽限制。

失败后可在同一原owner上使用合适限额重试；所有失败前缀、重试、描述和独立receiving仍累计原allocated/work，不退款或重置。政策不授予字段权限、构造权或生成声明免检资格，不改变完整FI、600/60/原普通时限及安全/provider门。Source/authority/policy/session生产者死亡后，已发布结果仍独立拥有。当前evaluator没有持久CTFE结果缓存，较大限额成功不能为较小请求提供免费成功。

### 17.63 拥有式生成 Built 与普通完整第二检查

生成收件接口只接收 Built 模块、真实 construction owner 和按函数/指令逐项提供的用户入口、模块、文件及字段路径。它在原编译账本复制函数名、参数、block、指令、operands、canonical 类型、声明、泛型、default、construction 和所有 origin 字节，保存独立的未检查 owner。物理范围及元数据准入不等于函数体检查；生成器的 Checked 标签、效果摘要、查询结果或 Sendable 事实不能作为此收件输入或检查豁免。

该 owner 每次申请 Checked 时调用既有普通完整 Built checker，重新执行当前类型、约束、可见性、CFG/SSA、所有权、借用逃逸、真实调用与捕获、清理和效果/root 上界检查；成功才发布独立 Checked。生成错型、借用载体逃逸和 no_suspend 效果升级必须拒绝。函数体错误映射到对应指令的已拥有用户 origin；函数声明错误使用函数入口，模块级错误使用程序入口或 library 首函数的入口。诊断借用生成 owner 的 origin，销毁 owner 前须复制；origin 自身不授予任何私有读、写或构造权限。

生成者输入和 construction 可在收件后销毁，生成 owner 可在 Checked 发布后销毁；后续 packet、Lowered 和执行不得借用这些存储。失败保全空输出；同一 owner 重试必须重新完整检查并累计原费用，不退款、不重置。该物理 owner 不新增语言宏、任意宿主生成器或另一类型/执行语义；Source 类型化授权、普通库派生接线及其完整资源/安全资格仍须分别完成。

### 17.64 定义策略、精确生成绑定与普通操作复验

内部派生策略先于生成器执行而存在，独立拥有目标的开放规范类型表达式、真实定义模块、普通协议应用及有序实参、真实生成入口、显式字段DESCRIBE/READ/WRITE集合、可选唯一合法init和用户源位置。描述、读取、写入和构造分别验证；WRITE不能授予不可变字段，编码策略不含构造权。生成器必须是可见的普通函数，不能冒充派生函数、成员或cleanup。策略可以尚无生成函数，不能通过预先发布空body解决生成顺序。

生成函数绑定只引用原Source策略，保存当前函数、精确规范目标、原Source行及生成root、逐指令拥有式用户位置。定义模块、目标声明和protocol约束必须由普通类型服务验证；函数的nominal_owner不扩大策略。生成实现仍经过原普通implementation/witness检查，其protocol及有序实参必须与策略一致，各root实际绑定对应要求。手写implementation不能挪用派生函数实现额外协议。派生函数不能占据测试、模块initializer或constructor角色；cleanup与其owner必须属于相同策略/root/目标。

真实字段操作使用当前规范实例，而非仅声明ordinal；交换泛型实参不能命中授权。派生函数访问目标字段逐项证明操作，不能直接按布局构造目标或借同模块身份调用任意私有函数、访问模块存储。唯一显式init授权只适用于真实CALL/INVOKE、对应原声明和精确目标结果。普通要求/default绑定仍独立验证其来源，不因派生标记跳过可见性、借用、效应或root复验。

特化后的绑定保留原Source行/root/策略，通过真实Source provenance验证有序类型替换及全部位置字节；不能丢弃绑定或携带一套新的运行期策略。Lowered重映射当前目标，CGen与Program独立比较完整绑定及位置。Checked线格式29/语义74遍历真实函数绑定、implementation策略及完整策略/生成位置数据；旧28/73包拒绝。函数身份新增绑定索引、声明新增拥有表，Program ABI为31，Value ABI24和Call ABI29未变。SDK身份实际包含459项独立布局观察；版本标签不能代替完整接收检查。

该接点实现内部共同接收与操作约束，不表示语言层TypeInfo、普通生成器、JSON或其他消费者已经接线。生成输入仍从Built进入17.63的普通完整第二检查；所有原编译预算、物理OOM、独立owner及完整FI/安全/provider门保留。

### 17.65 拥有式静态类型描述与普通数据

未被普通声明遮蔽的`typeInfo<T>()`对应core intrinsic身份7，要求一个显式闭合静态类型和零值参数。它在Source中通过唯一canonical类型/字段描述服务物化普通Tuple、Array和String recipes；不增加运行期反射指令、Value种类或evaluator。开放类型参数和畸形实参拒绝，普通同名函数仍按词法绑定调用。闭合泛型字段使用已有Source类型替换，不能按具体成员补未声明的泛型约束。

返回值是`(root,nodes,fields,arguments,parameters,variants)`六元Tuple，后五项为普通Array。root为0，类型边是本图的i64编号，不是跨pool的XIR类型ID。相同canonical类型只登记一次；递归class身份图使用回边，非法递归值布局仍按普通类型规则拒绝。仅包含可达事实，不返回宿主地址或物理路径。

| 行 | 有序字段 |
|---|---|
| fields | `(string name,i64 type,i64 flags,i64 ordinal)` |
| parameters | `(i64 type,i64 mode)` |
| variants | `(string name,i64 fieldBegin,i64 fieldCount)` |
| nodes | `(i64 kind,i64 builtin,string module,string name,i64 nominalKind,i64 flags,i64 fieldBegin,i64 fieldCount,i64 argumentBegin,i64 argumentCount,i64 element,i64 parameterBegin,i64 parameterCount,i64 result,i64 variantBegin,i64 variantCount)` |

各begin/count引用唯一对应平面表的精确区间；变体使用同一fields表。element/result缺省为-1；kind取唯一类型服务的类别，0表示builtin，此时builtin槽为其稳定标识，构造类型该槽为-1。字段ordinal保留声明中的原序，过滤字段不重编号。定义模块可描述私有字段；其他模块只得到普通可见字段及其可达类型。module是可重定位的逻辑声明身份。flags、参数mode和可见描述均不授予读取、写入、调用或构造权限。

```xray @id=type-info-static-array
export fn run()->i64 {
    return comptime {
        const info=typeInfo<Array<i64>>()
        info.1[info.0].0
    }
}
```

该例结果为Array类别2。普通函数可用结构相同的Tuple别名接收完整事实，普通无源码Checked库及其私有helper可以处理之后定义的用户类型。完整TypeInfo也可作为comptime结果，经独立Source重放成为普通拥有式数据，前驱销毁后由正常Program/Instance读取。所有访问、替换、图遍历、物化和接收仍收费到原ledger，服从17.61每请求结果限额及其余原预算。描述不等于生成：Source显式授权、生成声明启动、普通JSON/Regex等完整消费者与全部FI/安全门仍独立承担，不因这些数据或生成器既有Checked状态取得免检资格。

### 17.66 显式普通库派生的 Source 启动与接收

具名 struct、class 或 enum 使用 `@derive(Protocol, generator, describe: ["field"], read: ["field"], write: ["field"], construct: true)` 选择一个普通协议应用和普通可见函数。generator 可为已导入的名字或 `module.name`。选项可省略；construct 默认 false；字段列表中名称必须真实存在，各操作独立，不从 READ 推得 DESCRIBE 或从编码推得构造。相同选项、字段操作或目标上的相同协议应用重复时拒绝。每个未限定的字段名称必须唯一解析到目标的一个真实字段；enum 不同 case 的同名 payload 造成歧义时在 g 执行前拒绝，不能静默选择最后一个或扩大到全部字段。`@derive(JSON)` 等旧固定位集语法不保留；没有编译器专属协议白名单或隐含 Hash→Eq 授权。

Source 在真实声明、字段和签名建立后、普通用户 body 构造前建立并独立验证定义策略，再执行生成器。当前闭合目标首族从完整 Checked 依赖取得 g 及其私有帮助函数；依赖源码移除后仍能处理未来用户类型。启动代码只使用真实依赖 body 和真实结果 thunk，经普通 Built 全检、专用化、Checked 身份、Lowered 与唯一 CTFE；i64 Program 入口真实调用 thunk 并读取结果版本字段。未检查用户 body 不进入假 Checked 模块，也不以空 body 解开生成次序。

生成器是普通 `fn(TypeInfo, DeriveContext)->Generated`。TypeInfo 沿17.65的逻辑图；在派生上下文中，目标私有字段只在显式 DESCRIBE 集合内出现，嵌套类型不会继承目标模块的全部私有可见性。目标事实查询和 g 的 recipe 发射可在不同 canonical 池上，但必须共享原 ledger，跨池只传逻辑引用。

DeriveContext 为 `(Array<MethodInfo>, Array<CalleeInfo>, Array<i64> parameters, i64 constructor)`。MethodInfo 为 `(string name,i64 resultType,i64 parameterBegin,i64 parameterCount,i64 ownParameterCount)`；CalleeInfo 为 `(string module,string name,i64 index,i64 parameterBegin,i64 parameterCount,i64 resultType)`。参数区间引用唯一平面表，类型引用都指向同一 TypeInfo 图。callee index 对应接收方选择的普通可见 public 函数及显式授权的唯一 init；未授权构造时 constructor=-1。g 的私有帮助函数参与编译期计算，不因此成为生成 body 的私有运行期调用许可。

Generated 为版本1普通8元Tuple `(version,functions,parameters,blocks,instructions,operands,literals,typeArguments)`，后七项均为平面 Array。FunctionRow14为 `(name,member,root,resultType,parameterBegin,parameterCount,blockBegin,blockCount,instructionBegin,instructionCount,operandBegin,operandCount,typeArgumentBegin,typeArgumentCount)`；BlockRow4为 `first/count/panic/frontier`；InstructionRow10为 `opcode/type/arg0/arg1/target0/target1/immediate/typeArgumentBegin/typeArgumentCount/originField`。区间精确按序分割对应表；不接受未使用尾部。非负类型引用由接收方当前图解析，负数 `-builtin-1` 只接受唯一服务承认的 builtin；调用非负数索引本输出函数，负数 `-calleeIndex-1` 索引接收方 callee 表；CONST_STRING 只索引本输出 literal。

接收方强制定义模块、目标、export、普通 READ 方法/内部 helper 角色、真实协议 requirement/root、显式策略及每条指令的用户字段 origin。member=-1 为 helper，否则必须对应实际 requirement；root 必须最终绑定该协议方法。新函数不携带 g 的 Checked/NONE 证明；生成内容与完整用户 Built 一起重新检查实现见证、类型、约束、私有访问、借用、所有权、初始化、构造和根效果。失败不发布 Checked 或查询快照，字段错误指向用户字段路径；enum payload 使用真实 `Type.Case.field` 路径和该字段名字的源码位置。第二次独立 Source 按原 occurrence/属性/真实库 g 身份接收拥有式结果，在自己的当前池重建逻辑引用，不复用第一次池的整数 ID。

生成方法的查询角色是 DERIVED_METHOD，选区指向真实 derive token；没有伪造的方法名或参数词法选区。LSP 在调用处按普通方法显示，definition 指向触发派生的属性，reference/hover/semantic 消费拥有式快照。这个闭合 Checked 依赖首族不宣称完整泛型模板、同源码依赖启动、JSON/enum/Regex 各库消费者或全部 FI/安全/provider 资格已经完成；这些仍须沿相同边界实现。原8MiB每请求逻辑结果政策和全部其它预算/验证门不变。

<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 17. Compilation Pipeline

> Sources of truth: `src/frontend/`, `src/ir/`, `src/vm/`, `src/aot/`, `src/toolchain/`, `src/app/cli/xcmd_build.c`, and `src/app/cli/xcmd_compile.c`.

### 17.1 Two Execution Paths

```
source (.xr) -> lexer -> AST -> analyzer / canonical evidence
   -> xi_lower -> Xi IR -> optimization
        |-> tagged representations                  -> xi_emit -> XrProto -> VM
        `-> native representations + backend lower  -> C source
                 -> host/cross C toolchain -> native binary
```

Xray currently has no JIT. `xray run` and default `xray build` use the bytecode/VM path; `xray build --native` selects AOT.

Both paths share the parser, analyzer, module graph, **Xi IR**, and runtime semantics. They diverge inside the Xi pipeline at representation selection and backend lowering (`xi_pipeline_default_config` versus `xi_pipeline_aot_config`): the VM path uses the tagged representation policy, runs neither select-rep nor backend lowering, and emits an `XrProto` through `xi_emit`; the AOT path runs select-rep under the native representation policy plus backend lowering, and then generates C from `src/aot/`. Their output representations and backend optimizations therefore differ.

### 17.2 Frontend

- Lexer: `src/frontend/lexer/xlex.c` / `xlex.h`, with `xkeywords.def` as the keyword table; input must be valid UTF-8.
- Parser: handwritten recursive-descent and precedence expression parsing in `src/frontend/parser/xparse*.c`, producing an `AstNode` module tree.
- Analyzer: `src/frontend/analyzer/` performs name, type, generic, visibility, ownership, capture, and error-set checks.
- Canonical evidence: `src/frontend/canonical/` plus the module/toolchain layer stabilize cross-module facts for backend consumers.

### 17.3 Bytecode and VM

`src/frontend/codegen/xcompiler.c` runs the analysis pipeline (type inference, monomorphization, escape analysis) and then delegates to the Xi IR pipeline `xi_pipeline_compile_program` for lowering, verification, optimization, and bytecode emission, producing an `XrProto`; there is no bytecode path independent of Xi IR. The single opcode list is `src/runtime/value/xinstruction_table.h`; the VM lives in `src/vm/`. Its register-oriented instruction set includes property/call fast paths and dedicated coroutine, error-channel, and tail-call operations.

The standalone `.xrc` product is removed. `xray compile file.xr` requires explicit `--format c|header --output FILE.c|FILE.h` to emit an offline C source/header bytecode container for compiler development. The `bytecode`/`bc` formats and `.xrc` outputs are rejected before isolate creation, and the runtime does not execute XRC. `--format c` here is **not** the native AOT C backend.

Bytecode must serialize extern aggregate layouts and the target ABI fingerprint in deterministic order. Before execution, the loader must validate layout depth, recursion cycles, field bounds, total size, trailing data, and ABI compatibility; corrupt or target-mismatched input is rejected rather than falling back to host layout.

### 17.4 Xi IR and Optimization

Both paths lower the program to Xi IR in `src/ir/`; they differ in optimization level and representation policy — light and tagged for the VM path, full and native for the AOT path. `xi_pipeline.c` / `xi_pass.h` organize passes including SCCP, DCE/CFG simplification, inlining, devirtualization, tail-call rewriting, escape/ownership processing, loop transforms, GVN/PRE, bounds-check elimination, and vectorization. The enabled set depends on optimization level, target, and legality; the existence of a pass does not guarantee that every program is transformed by it.

### 17.5 Native AOT

`xray build --native file.xr` uses the prepare/verify/representation/container/link plans in `src/aot/`, generates C from Xi IR, and invokes a host, Clang, or Zig C toolchain. Hosted native binaries still link the Xray runtime; `--profile freestanding` uses a restricted freestanding capability set. Consult `xray build --help` for current `--target`, `--toolchain`, `--cpu`, `--lto`, and related options.

Native AOT does not emit machine code directly from SSA and is not a JIT; the selected C toolchain produces the final machine code.
### 17.6 Staged XIR Contract

Current internal protocol revisions are defined only by the implementation constants below.
Language sections describe responsibilities rather than duplicating incrementing numbers.
Every admitted family shares these revisions; sections do not select different protocols.
Older versions reject without compatibility readers or adapters.

| Protocol | Authoritative revision definition |
|---|---|
| Value boundary | `src/xir/xxir_value.h`: `XR_XIR_VALUE_ABI_VERSION` |
| Resumable calls | `src/xir/xxir_call.h`: `XR_XIR_CALL_ABI_VERSION` |
| Immutable Program | `src/xir/xxir_program.h`: `XR_XIR_PROGRAM_ABI_VERSION` |
| Checked wire | `src/xir/xxir_checked.h`: `XR_XIR_CHECKED_SCHEMA` |
| Checked semantics | `src/xir/xxir_checked.h`: `XR_XIR_CHECKED_CONTRACT` |

The new compiler has one stage order: `Built → Checked → specialization and
reverification → Lowered`. This section freezes the nongeneric scalar subset;
the preceding legacy product implementation has not migrated and no CLI cutover
is claimed. Ordinary generics are checked against constraints at definition and
specialized on Checked. Explicitly derived content is checked again. Ordinary
instances close before immutable executable-image sealing; the VM executes Lowered.

The initial internal subset admits unit, bool, and i64. Scalar copy preserves the
value; signed i64 addition wraps modulo 2^64; equality and signed less-than produce bool; branches
require bool; returns match their declaration. Parameters in this internal subset
are bool/i64; unit is a result/terminator type without a value ID. Managed values, borrowing, generics, and new source spellings are absent;
unsupported operations reject. Internal direct calls are defined below.
Blocks partition instructions, are reachable from the entry, and end in exactly
one terminator; entry has no predecessors. SSA uses require an earlier same-block
definition or a dominating definition; unit instructions do not produce values.
Abstract copy belongs to Built/Checked and physical scalar-copy to Lowered;
changing a stage tag is insufficient. Transitions reverify and copy all metadata,
publish no partial artifact on failure, and never borrow input storage. Structure,
types, dominance, and verification work are bounded. Unknown stages, operations,
and types reject. The exact internal fields are verified by assertions such as
`test_xir_stages`; these assertions do not grant execution qualification.

Internal scalar execution is verified by `test_xir_execution` and `test_xir_native`.
The initial target is little-endian x86_64 with the current value boundary ABI defined in this section. Layout
queries include type, target, context, and ABI. Lowering stores authoritative
frame offsets and boundary layouts; VM/C consumers cannot choose another layout.
Bool/i64 SSA and frame lanes use eight bytes. Boundary values use sixteen bytes
(type, zero reserved field, i64 payload); bool payloads are zero or one and unit
has no frame lane. Arguments match exactly and failures clear the result. Each
instruction consumes one step; frames and steps are bounded. Signed addition
avoids host signed overflow, every exit releases its frame, and inline
scalar results survive independently. Checked cannot execute; unknown target/ABI
rejects. This subset does not qualify module, call, or resumable ABI behavior.

Internal resumable calls are verified by `test_xir_calls` and `test_xir_call_native`.
Direct CALL names a module function and passes an owned operand-table range;
argument count, types and result exactly match the declaration. SUSPEND saves the next
instruction; THROW carries an i64 error token. These are internal XIR operations,
not new source keywords or qualification of modules, generics, or stdlib caches.
VM and generated C return typed frame/result actions to the same trampoline.
A caller never keeps a resumable child on its native stack. Frames do not move;
depth, physical bytes, and resume work have separate budgets. Wake tokens are
single-use; reentrant drive/destruction rejects. Cancellation cleans children
before parents after the running callback yields control. Cleanup cannot suspend
or reopen completion arbitration. The current interface admits exclusive driving
by one host thread; concurrent cancellation, segmented pools, and
complete image/instance ownership remain unqualified. Verified closed scalar
leaves may use a non-suspending entry, but CALL/SUSPEND/THROW cannot fall back to it.

### 17.7 Internal Managed Values and Result Transfer

The value and call boundaries use the current ABIs in §17.6 without aliases.
Strings own strict UTF-8 with embedded NUL, no implicit normalization/replacement,
atomic reference counts, and copy-on-write mutation under an exclusive handle
borrow. Byte length and Unicode scalar count are distinct from grapheme count.
A budgeted allocation domain outlives its host handle when strings still own it;
strings never depend on activation, instance, or code-image lifetime.

Call admission owns argument copies. Resume views and return actions borrow values.
The driver owns returns before child cleanup, and owns inboxes and terminal results.
Poll borrows; take transfers a result exactly once. Untaken results are destroyed
with the activation. Typed synchronous output carries an explicit stdout/stderr
stream and a borrowed bool/i64/string group to the configured provider. Raw groups
contain one value without separators or a newline. Print evaluates arguments left
to right before one group call, renders one ASCII space between values and one
final LF (also for zero arguments). Validation, budget checks and complete buffer
allocation precede one byte-sink call; failures publish no partial group. The
PRINT and native groups both admit 0–65536 values within resource budgets.
Unimplemented source types and display protocols require separate qualification.
Missing/rejecting providers are runtime failures. Source admission and complete
Program/Instance qualification remain separate from this internal contract.
String parameters/results, COPY to OWNED_RETAIN, CONCAT_STRING and OUTPUT
consume a lowering-owned, reverified list of owned frame slots. Both backends
release previous values on replacement and clear slots on all exits. Literal tables are sealed with Program metadata; source production remains to be connected.


WRITE_STREAM borrows one string, selects stdout/stderr with immediate 1/2 and
returns bool through a typed inbox. A configured provider's refusal returns false;
a missing provider remains runtime OUTPUT_ERROR. PRINT/OUTPUT rejection still
terminates execution. Reentrant cancellation takes precedence over the result;
accepted bytes are not rolled back. This primitive does not qualify source stdlib
binding, host flushing or filesystem short-write policy. Old call ABIs reject.

### 17.8 Immutable Programs and Module Instances

A Lowered module closure is sealed before execution. Programs own declaration,
dependency, literal, initialization-order and code-environment metadata; instances
own runtime slots and allocation domains. Dependencies initialize first, choosing
the lexicographically smallest UTF-8 name among ready modules. Cycles, duplicate
edges and modules outside the root closure reject. Private zero-argument unit
initializers execute once; the zero-argument i64 root entry is a distinct function.

CONST slots publish once from their declaring initializer. Module var is confined
to the root module's execution flow. Const Atomic<i64> handles refer to per-instance
synchronized identity objects; copies share identity instead of using string CoW.
The initial SeqCst load and fetchAdd operations use two's-complement wrapping for
fetchAdd and return the old value. Initialization retains one publisher across
suspension, rejects unpublished reads and keeps failures sticky after reverse
cleanup. Shutdown stops admission and drains execution; independent results do not
retain unrelated module state. Resume matches both execution epoch and wake token.
The initial instance interface admits exclusive driving by one host thread only.
The precise internal interface is verified by `test_xir_instances`; source and
serialized/cache admission require separate qualification.

### 17.9 New XIR Source Admission Boundary

The source owner reuses the real parser and module resolver/graph, checks source
declarations directly into Built, and publishes only owned Checked snapshots.
It does not invoke the legacy analyzer, IR or executors. Initial admission covers
bool/i64/string, ordinary named functions with read parameters, direct calls,
local/module bindings, string concatenation, print, and default SeqCst Atomic<i64>
construction/load/fetchAdd. Unimplemented syntax fails closed. All function bodies
are checked, including unreachable ones; value-return inference without an
annotation is not yet admitted. Generic bodies cannot bypass constraints.

Functions are hoisted, while top-level statements and initializers run once per
instance in source order. A function named main is ordinary. Private unit module
initializers precede a separate synthesized i64 entry returning zero. Library
state must be const Sendable; initial carriers are scalar/string/Atomic<i64>.
Later type admission follows this chapter, including Array in §17.26.
Root mutable state belongs to the instance. Cross-module calls require direct
imports and export visibility, using resolver-owned identities. Duplicate,
private, unresolved and cyclic declarations fail. Parser string payloads are
already decoded and must not be decoded again.

CALL/PRINT args[0]/args[1] encode first/count ranges into owned function operand
tables. Nonempty ranges partition each table in instruction order; empty ranges
are zero/zero and PRINT immediate is zero. Each group admits up to 65536 values,
all type- and dominance-checked. Nested arguments evaluate left to right before
appending the outer range. Lowered records and reverifies the maximum outgoing
count; stable frames reserve typed-value staging and charge its physical bytes.
Full generics, stdlib, coroutine
syntax, foreign providers, parser OOM/input budgets, default CLI and product
qualification remain separate. The interface is verified by `test_xir_source_admission` and `test_xir_source`.

The standard-library source submodule std/io/output publishes ordinary
writeStdout(string)->bool and writeStderr(string)->bool functions reporting host
provider acceptance. Only its exact stdlib identity admits private
__writeStdout/__writeStderr calls, lowered to WRITE_STREAM. Imports and bodies
remain normally checked; matching local paths or names confer no privileges.
This does not promise File flush/short-write behavior or replace yieldable file
operations. Same-source Checked/native publication requires separate qualification.

### 17.10 Owned Checked Packet Boundary

The current closed declaration/type/op subset has one canonical target-neutral
Checked packet format. Its header binds schema and semantic-contract revisions,
the Checked stage, exact payload length and SHA-256 content identity. The digest
is not authentication. Fixed little-endian field encoding excludes host pointers,
physical layouts, native code, runtime state and cached verification decisions.
Unknown revisions, malformed lengths, reserved fields and trailing bytes reject;
there is no Built, Lowered or legacy format reader.

The reader bounds wire bytes, aggregate counts, allocation bytes and work before
allocation/traversal, owns every decoded buffer, and runs the existing full
Checked semantic and layout verification before publication. Recomputed digests
cannot bypass types, CFG/dominance, visibility, imports, initialization or slot
permissions. Input storage may be destroyed immediately after loading. Failure
publishes nothing and frees all partial storage. Both VM and native consumers
then use the same reverified Lowered transition. The precise wire and budget
rules are verified by `test_xir_checked`.

This subset (including §17.11 marker templates) does not qualify full member constraints/witnesses,
effects, diagnostic provenance, package linking, Checked library publication, installer
publication or complete source-free standard-library distribution.

### 17.11 Definition Checking and Checked Generic Specialization

Ordinary type parameters admit copyable, storable values, excluding unit, views
and noncopyable resources. The initial concrete domain is bool/i64/string/Atomic<i64>;
later numeric, function and Array admission follows this chapter.
Named functions can declare `<T, U:Sendable>` or an own-parameter `where U:Sendable`;
calls supply all type arguments explicitly. Parameter names are distinct and
Sendable is a reserved marker name. Conjunctions of admitted markers are sets;
repeated requirements are idempotent. The conditional-method implementation contract
allows method-owned Sendable/Error requirements on enclosing and own parameters,
without strengthening the nominal type itself. Definition proofs and call obligations
use the same Checked path. Default type parameters, full member/witness constraints
and inference remain unavailable; conditional-method execution requires separate qualification.

Every body, including unused templates, is checked at its definition. Copying,
read passing, storage and return follow from the ordinary domain. Sendable adds
no member, arithmetic, display or default-construction capability. Unconstrained
T cannot be forwarded to a Sendable requirement merely because every currently
implemented concrete type is Sendable. Forwarding proves constraints from the
caller declaration. Copies preserve string values and Atomic identity; concrete
retains, cleanup and layout are determined after specialization.

Built/Checked retain function-local type parameter IDs, constraints and separate
call type-argument tables. CALL type and value ranges are canonical and reverified;
substituted parameters/results match exactly, with normal visibility, module and
dominance rules. The current Checked schema and semantic contract in §17.6 preserve templates
and local places (§17.12). Older schema or semantic revisions reject. Loading rechecks definitions and forwarding proofs.

Specialization consumes only Checked, never AST. A bounded work queue interns
declaration identity plus ordered concrete arguments, reusing recursive instances.
The instance closure is completed and reverified before the sole Lowered pipeline.
The current host function-ID interface retains all ordinary definitions as roots;
only generic instances reachable from these roots are emitted. A template-only
package with no ordinary roots is not directly executable. Instance, aggregate
parameter/block/instruction, memory and work budgets bound construction; failure
publishes no result and releases temporary storage. Lowered and immutable Programs
contain no open parameters and acquire no execution-time instances. Debug names
are not native-cache identity proofs.

Member witnesses, symbolic callback effects, user-declared generic type constructors, inference,
diagnostic provenance serialization and complete stdlib package/cache pairing are
still unavailable. Unadmitted effect/borrow/callable declarations continue to
reject. The precise interface is verified by `test_xir_generics` and `test_xir_source_generics`;
this family does not qualify all ordinary generic facilities.

### 17.12 Mutable Local Places and Structured Control Flow

A local var needs an initializer and denotes a typed activation-local place;
const bindings and read parameters remain values. Reading makes an independent
value snapshot; assignment evaluates its right side before replacement. Existing
string copies remain unchanged, while Atomic copies preserve identity. Places
cannot escape, be returned or serve directly as value arguments. Initialization
dominates every access and types match exactly, including at generic definitions.
Built/Checked carry LOCAL_NEW/READ/WRITE; Lowered alone selects scalar/owned
storage actions and frame offsets. Owned writes retain before dropping. Every
activation exit clears owned offsets; precise last-use release and noncopyable
scope destructors are not qualified by this family.

If/else and while require bool conditions. Only the selected branch executes and
loop conditions are reevaluated. Local updates survive joins and backedges while
block names stay scoped. Unlabelled break/continue target the innermost while.
Every live path in a value function must return; statements after a return, throw, break or continue are unreachable and are not lowered, and closures in them reject.
Concrete i64 addition/equality/less-than and existing string addition are admitted;
unconstrained T acquires no operator witness. Block/instruction/memory/depth/work
budgets and runtime step/cancellation contracts continue to apply. Checked wire
schema and semantic revisions follow §17.6; older revisions reject without a second checking path. The interface is verified by
`test_xir_locals` and `test_xir_local_native`.

### 17.13 Boolean Short Circuit and C-style For

The new source owner requires bool for !, && and ||. Both operands are checked
at definition time even if runtime evaluation necessarily skips the right side.
The left side runs once first; && evaluates the right side only when true and ||
only when false. ! negates its operand. Existing CFG and typed places express
these forms without new runtime or IR operations. Nullable narrowing is not yet
qualified by this implementation.

For(init; condition; step) initializes once with loop-local bindings; an absent
condition is true. Normal iteration and continue run the step before the next
condition; break/return skip it. The step cannot see body-local bindings. Nested
while/for exits select the innermost loop. A step with no incoming path is still
source-typechecked; temporary instructions, blocks, operand and type-argument
ranges are discarded without refunding budgets, so executable CFG has no dead
blocks. Literals keep the existing whole-closure table policy. Standalone and
for-step name++/name-- require mutable i64 and use the same wrapping add/store path. They cannot be used as expressions. Labels,
for-in protocols and other numeric families remain unqualified. Existing Checked
semantics fully describe these constructs; packet and ABI revisions are unchanged.

### 17.14 XIR i64 Arithmetic and Comparisons

The admitted source family follows §2.3.1: +, -, *, unary negation and increment/decrement
wrap modulo 2^64, replacing the initial internal checked-add subset. Binary operands
evaluate once, left to right. Signed ==, !=, <, <=, > and >= return canonical bool;
comparison never subtracts or reorders side effects. Division truncates toward zero,
and a nonzero remainder has the dividend's sign. INT64_MIN/-1 wraps to INT64_MIN;
INT64_MIN%-1 is zero. A zero divisor faults with DIVIDE_BY_ZERO, has no result and
unwinds owned frames. Initializer failure is sticky; later ordinary calls may proceed
after an arithmetic fault in an initialized instance. Generated C guards special pairs
before host / or % and never executes signed overflow. Negation lowers to zero minus
the operand. Current Checked and value/call/Program boundaries follow §17.6 without a compatibility reader or adapter.
Concrete arithmetic cannot supply a missing generic constraint. Other numeric families
and explicit checked/saturating library methods remain outside this admitted subset.

### 17.15 XIR i64 Bitwise Operations

Concrete i64 &, | and ^ operate on 64-bit two's-complement patterns; ~ lowers to
xor with -1. Left shift discards high bits and zero-fills. Right shift is arithmetic
and sign-fills. Counts are normalized modulo 64, including negatives: -1 selects
63, -64 selects 0, and an effective zero count preserves the value. The host only
shifts unsigned patterns; complement transformations define negative right shift
without depending on C signed right-shift behavior. Operands evaluate once, left
to right. Bitwise operators reject bool, string, Atomic and unconstrained T. Variable
compound assignments read the old value before evaluating the RHS, compute, store
and return the new value; an RHS assignment cannot change that earlier snapshot.
Failure skips the store. String += also admits owned concat snapshots; other compound
operators require i64. Const/read, member and indexed targets are not admitted. Current Checked and execution boundaries follow §17.6 without compatibility paths. Other
integer widths and conversions are not yet admitted.

### 17.16 XIR Owned Function Values and Indirect Calls

This family uses the function-type spelling in §2.8: read parameters (including
enclosing generic parameters under §17.17),
ordinary named function values, nested callable parameters/results, mutable local
storage, root-module slots, conditional selection and ordinary generic identity
transmission. Unadmitted ref/move, captures, borrow origins,
Sendable and explicit effects remain rejected. A selected body cannot
supply a missing proof. Module-owned canonical tables preserve ordered parameter
types/modes, results and promises; nested references point strictly backwards.
References enforce declaration visibility and imports and cannot name initializers.
Indirect calls capture the callee before evaluating arguments left to right; value
role, dominance, full signature and result checks never enumerate possible targets
as a replacement for the declared contract.

Built-to-Checked owns copied descriptions. After specialization and rechecking,
the unique Lowered layout includes explicit function copy/drop, local places and
PHI ownership. VM and generated native code use the same resumable CALL and result
protocol. Sealed Programs own all signatures, entries and required code. Function
values retain their allocation domain and a separate admission record; that record
retains Program code but only observes its instance. It does not retain all module
slots, providers or frames, so function-valued root slots create no instance cycle.
Stopping/freeing the instance revokes admission before cleanup. Escaped values remain
readable, copyable and releasable; the last lease releases code. The current single
host-thread driver admits calls only in the originating instance. Another instance
or Program rejects coincident numeric IDs. Cross-instance transfer and concurrent
admission still require implementation and qualification.

The unique packet and execution boundaries use the current revisions in §17.6; old versions reject
without readers, boxed adapters or alternate execution. Source/packet validation,
independent VM/native expectations, suspension/cancellation, escaped results, code
leases and individual allocation failures are covered by machine contracts. This
family does not certify the complete language or product.

### 17.17 Enclosing generic parameters in callable signatures

The admitted ordinary read callable family includes `fn(T)->T` and nested
signatures in generic definitions. A free type ordinal is interpreted in the
enclosing function, and every use is checked against its declared constraints
before specialization. Calling a supplied function uses that signature alone.
Explicit call type arguments substitute recursively through parameter and result
signatures; structural equality neither grants Sendable nor inspects a body.

Checked stores the exact free-parameter span of each canonical signature,
including nested signatures. The verifier independently recomputes that span
from backward references. Global slots require closed types. Specialization
builds a fresh canonical closed type table and remaps parameters, results,
instructions, explicit type arguments and slots before rechecking. No open
signature may enter Lowered or an immutable Program. Traversal spends work and
metadata budgets and admits at most 128 active structural substitution levels.
Checked and Program boundaries use the current revisions in §17.6 and reject earlier versions.
Captures, other parameter modes, effect promises, inference and member witnesses
remain outside this admitted subset and retain their independent gates.

### 17.18 XIR owned immutable capture environments

A function value may own an ordered sequence of value captures. The operand range of FUNCTION_REF contains captures. The target parameter prefix matches their types; its remaining parameters and result match the public callable signature. Checked verification enforces explicit generic substitution, value roles, dominance, visibility and initializer restrictions, and specialization rechecks them. Captures grant no additional authority.

A closed captured leaf Function value requires its authentic Program producer, target, declared type and complete captures; a NONE advertisement or address alone grants no admission. Runtime checks the owned original FUNCTION_REF capture operand's declared bound and matches the complete actual row against the sealed execution node. A concrete leaf with no environment does not inherit unrelated columns from its enclosing closure; symbolic or owning producers keep their complete relation and ordinary exact verification. This finite capability does not admit other constructed captures or relax Function transport across GO, Sendable, NO_SUSPEND, private visibility or resource obligations.

Construction obtains a logical copy of every captured value and publishes no partial value on failure. Function copies share the immutable environment. Strings preserve value semantics, Atomic preserves synchronized identity, and captured functions retain their existing admission. No caller frame pointer is stored. The last function reference releases all captures and its code lease. Nested environment destruction uses neither host recursion proportional to depth nor cleanup allocations.

An indirect call publishes the same resumable CALL action with a borrowed function value. The sole frame driver copies the environment prefix and explicit arguments into the new frame while the caller still owns the environment. Suspension, throw, cancellation and return use the existing cleanup path. Host function entry likewise retains captures and arguments before releasing a previous result. Instance stop/free revokes execution admission while escaped environments remain readable, copyable and releasable.

This internal contract does not turn var captures into copies or grant Sendable, noescape or no_suspend. Shared mutable cells, cycle reclamation and the source closure producer require separate implementation and qualification. Internal construction does not certify source closures. The implementation constants listed in §17.6 define the sole current revisions; predecessors are rejected.

### 17.19 XIR shared capture cells

An internal cell type is `0x40000000 | element type`. Elements are non-unit ordinary values or in-scope type parameters, never nested cells. This implementation capability is not a source type or Sendable proof. Public callable signatures and module slots do not expose cells; private capture prefixes may carry them. Checked substitution preserves the wrapper and rechecks the element; Lowered requires closed elements. CELL_NEW copies an initial value, CELL_READ produces an owned logical copy, and CELL_WRITE copies before exchanging and dropping the old content. Failure leaves content unchanged.

Ordinary var captures retain one shared storage identity; every alias observes updates while read results keep value semantics. Cell RC is atomic, but content access only has the current instance's single-host-thread admission, not a cross-worker or atomic-update promise. Function contents and captured cells belong to the same instance value domain, so cells cannot bypass callable instance isolation. Last-reference release queues contents on the existing allocation-free destruction worklist, without recursive environment destruction. Strong cycles remain possible and can be explicitly broken while an owner remains. Unreachable strong-cycle reclamation stays OPEN; this introduces no collector or universal leak-freedom claim.

The unique current revisions follow §17.6, without old readers or ABI adapters. Source var capture must not substitute snapshots for shared cells.
### 17.20 Fixed-width integer runtime foundation

The sole allocation-free integer core receives an explicit 8/16/32/64-bit width and signedness. Signed payloads are sign extended; unsigned payloads are zero extended, with u64 retaining all 64 bits. Noncanonical payloads and invalid widths reject. Arithmetic, comparison, explicit conversion and the shifts in §3.3.2 follow §2.3.1; failure returns a status and clears the numeric output. Current i64 VM and native arithmetic use this core; the old entry point is removed and the current call ABI rejects old native entries. Checked and value/Program boundaries follow §17.6. This section qualifies the runtime foundation. Typed XIR, explicit casts, same-signedness arithmetic widening and output follow §17.21; full contextual inference and other containers remain OPEN; the Array subset follows §17.26.

### 17.21 Typed fixed-width integer XIR

Integer type IDs are i64=2, i8=5, i16=6, i32=7, u8=8, u16=9, u32=10 and u64=11. The sole CONST_INT and arithmetic/comparison/shift INT family replace the old i64-specific names. CONVERT_NUMBER explicitly converts concrete integers. All instructions retain type, value-role and dominance checks. Arithmetic operands and results share an exact type; comparison operands share a type and return bool; shifts keep the left type and accept any integer count. Unconstrained T gains no arithmetic authority from specialization.

Constants and boundaries preserve canonical payloads. Copy, phi, calls, module slots, cells, callable signatures and output retain exact type identity. Storage width/alignment is 1/2/4/8 bytes; frame slots remain 8 bytes, boxed/parameter/result boundaries remain 16 bytes aligned to 8. Unsigned output uses the complete unsigned decimal value, including 18446744073709551615 for all-ones u64. Output budgets and single publication remain unchanged. Only the current revisions in §17.6 are admitted; old packets and entries reject. The source producer admits all eight names, concrete integer as, same-signedness widening of already typed arithmetic operands, width-preserving unary/increment operations, shared cells, module state and print. Nullable casts and bool/rune/float or unconstrained-T conversion reject. Direct integer literal contexts and assignment/parameter/return widening follow §17.22; full inference, floating point and other containers remain OPEN; the Array subset follows §17.26.


### 17.22 Integer contexts and lossless widening

Binding annotations, assignment targets, declared return types and direct/indirect call parameters (after explicit generic substitution) provide a unique context. Direct integer literals include parentheses and one direct negation. Without an integer context they default to i64; with one, the mathematical value is range checked before producing that type. The u64 maximum and i64 minimum avoid host signed overflow; negative zero can represent unsigned zero. Already typed expressions only widen within the same signedness, never implicitly narrow, change signedness or treat unconstrained T as an integer. Conversion becomes explicit CONVERT_NUMBER and is rechecked by Checked. Inferred-result closures gain no unimplemented inference authority from an outer callable signature.

A direct literal in binary arithmetic/comparison uses the other typed operand's context. Two direct literals use an outer integer context, otherwise i64. Context does not penetrate arbitrary formed subexpressions to fold constants. A direct literal under complement can use the outer integer context; typed unary operations preserve the operand type. A direct shift-left operand can use outer context, but the count is typed independently and is not restricted by a narrow left operand: 256 and negative counts remain legal under §3.3.2. Non-shift compound RHS uses the binding type; read the left snapshot before evaluating the RHS and write only on success. Explicit as types its input independently, so `200 as i8` still wraps rather than becoming a literal range error. The sole exception is a direct positive literal above the i64 maximum but representable in u64 (including parentheses): it retains its complete u64 magnitude as the conversion source before reducing to the target width. Negative literals cannot bypass the i64 lower bound this way.

A conditional with explicit context checks and widens each branch in its own predecessor block; only the selected branch executes. Without outer context, both branches are first typed independently. Same-signedness integers use the wider type, converting on the corresponding predecessor path before PHI. Direct branch literals still default independently to i64 and do not infer a narrow type from the other arm. Other branch types must match; unions and general branch inference are not qualified here. Calls evaluate the callee then arguments in order. Only effect-free direct literals may defer IR emission to obtain the other operand's type. Uncalled functions and generic definitions are checked without specialization bypasses. This producer extension does not alter XIR operation semantics, wire or ABI versions. Floating point, full inference and other containers remain OPEN; the Array subset follows §17.26.


### 17.23 Binary floating conversion foundation

The common runtime floating core takes explicit IEEE binary32/binary64 bit patterns (binary32 must have zero upper bits); integers retain explicit width/signedness and canonical payloads. Integer-to-float and f64-to-f32 conversions use round-to-nearest ties-to-even, overflow to signed infinity, preserve gradual subnormal underflow and signed zero, and widen f32 to f64 exactly. Every NaN pattern normalizes to positive canonical quiet NaN: 0x7fc00000 for f32 and 0x7ff8000000000000 for f64. Same-width conversion also normalizes NaN.

Float-to-integer truncates toward zero before checking the complete target range; (-1,0) may produce unsigned zero. NaN, infinity and out-of-range truncated values return RANGE. Invalid widths/encodings return BAD_ARGUMENT; failures clear outputs. Comparison returns less/equal/greater/unordered, any NaN is unordered, and signed zeros compare equal. Negation flips a non-NaN sign and canonicalizes NaN. All operations use integer bits, allocate nothing, execute no host floating arithmetic or conversion, preserve host rounding/exception state, and depend on no legacy scalar tags or generated metadata.

This section defines the common conversion/comparison/negation foundation. XIR type, packet, layout and source execution admission is specified in 17.24; floating arithmetic and typed output follow §17.27. Helper tests alone do not qualify VM/native execution. Section 17.24 revises the sole protocol/ABI versions. Full float and cross-platform qualification remain OPEN.


### 17.24 XIR floating values and execution boundary

Runtime f64 accepts all binary64 encodings, including signed quiet/signaling NaNs with arbitrary payloads. Runtime f32 retains zero high bits and only the positive canonical quiet NaN.
CONST_FLOAT is a separate wire-encoding boundary: NaN constants at either width still require positive canonical quiet encoding; expanded runtime f64 never admits noncanonical packet constants.
Storage uses 4/8 bytes, SSA/frame payloads use 8 bytes, and boxed/parameter/result values use 16 bytes aligned to 8.
CONST_FLOAT, NEG_FLOAT, six floating comparisons and the sole CONVERT_NUMBER run through the same Checked/Lowered pipeline.
CONVERT_NUMBER atomically replaces the former integer-only conversion. Float-to-integer truncates then checks the full range; NUMERIC_RANGE faults clean up with a unit result.
NaN yields false for EQ, true for NE and false for ordered relations; signed zeros compare equal. Conversions follow 17.23 without changing the host floating environment.
Source admits explicit numeric casts, floating negation/comparison, implicit f32-to-f64 widening and conditional joins; integer/float mixing requires explicit casts.
Copies, slots, cells, closures, generic instances, parameters/returns, Tuple/Array/Nullable/admitted nominal storage and suspension preserve exact admitted bits. Boundaries still check exact type, arena, ownership and existing execution authority. Floating comparison with any NaN is unordered: EQ is false, NE is true and ordered relations are false; integer-payload transport never silently canonicalizes. Decimal literals and exact contextual integers follow §17.25. Arithmetic, conversion and negation NaN results remain canonical; floating output follows §17.27.
The sole current revisions follow §17.6; older versions reject without compatibility paths.

### 17.25 Exact decimal literals

The AST owns each complete decimal floating token. Shared conversion never truncates input or uses strtod, locale or host floating instructions. It rounds once at the requested binary32/binary64 precision, ties to even; overflow produces signed infinity and gradual underflow preserves signed zero. Every exponent digit and separator is validated. A bounded integer rational with 1152 significant digits and a nonzero sticky tail distinguishes all IEEE rounding boundaries; buffer truncation never substitutes for mathematical rounding.
Without a unique floating context the default is f64. Annotations, assignments, declared returns and substituted parameters directly select f32/f64; grouping and direct negation retain context. Formed f64 expressions never narrow implicitly. Direct integers in a floating context must be exactly representable in full; explicit as permits rounding. Generic definitions remain constraint checked. Formatting preserves spelling and AST cloning copies it into the destination owner. Existing CONST_FLOAT, Checked revalidation and the sole Lowered execution remain unchanged; floating arithmetic/output follow §17.27.

### 17.26 XIR Array values and source boundary

The `struct Array<T>` declaration in `stdlib/types/array.xr` is this family's sole source authority. The compiler, member adapters and API inventory consume the same structured declaration. Of its current 32 members, the frozen boundary contains the three READ `get(index: i64) -> T`, REF `set(index: i64, value: T) -> ()` and REF `push(value: T) -> ()` primitives plus twelve declaration recipes: map/filter/reduce/forEach/find/findIndex/every/some/contains/indexOf/join/clear. Together with reverse/unshift/pop/shift/resize these are 20 operations; the other 12 retain inventory identities without execution authority. Source finds the exact public member declaration before selecting implementation by its admitted native operation/recipe identity; queries record the declaration identity and CALL reference without a separate name-based execution table. See §14.7 for callback/query/clear typing, evaluation, effects and ownership. Index reads/writes use the same operations. Global `len` resolves separately by its core intrinsic identity; this subset accepts only Array and returns i64. Lexical, module and import resolution precede the prelude, so a user function named `len` remains an ordinary function.

Source admits explicit `Array<T>`, homogeneous literals, empty literals with an Array context, ordinary assignment copies, parameters, returns, locals and entry-module state. An empty literal without an element context rejects. Elements evaluate left to right under ordinary contextual typing. Nested arrays and function elements use the unified constructed pool's copyable/storable admission; unit, internal CELL, views and unadmitted noncopyable resources are not ordinary elements. Generic bodies using `Array<T>` are checked against their declared constraints, specialized on Checked, then reverified. Ordinary instances finish before Program sealing; concrete arguments cannot invent capabilities missing at definition checking. Other container and user struct/class families require separate admission.

Array copies preserve independent logical values without explicit `copy` or `move`. Shared backing detaches before mutation; unique storage with enough capacity may mutate in place. Compact elements use the sole type layout and copy/drop rules. Const and READ parameters cannot supply writable receivers, but saving their copyable values into an ordinary local var permits mutation of that copy. Backing uniqueness grants no write authority. Existing cross-module visibility and restrictions on mutable library-module state remain in force.

A cross-allocation-domain write must not plant the current execution domain’s sensitive identities in a foreign copyable container. The existing budgeted admission traversal summarizes actual Cell, Function, Class and Task identities during the same visits, without an uncharged type walk or a second value walk; Error follows its actual enum backing and active payload. For an identity-bearing write, Array SET/PUSH, standalone struct SET and every foreign copyable ancestor on a field/index path obtain storage owned by the current admission domain, even for unique backing with spare capacity. Cell/Class retain identity semantics and their original domain authority. Pure scalar/String aggregates keep their available in-place paths; a pure String write into unique foreign spare storage still allocates nothing. Preparation, copying, peak storage and cumulative costs use the existing ledgers; failure preserves old values and aliases. Unique SET relocation transfers the prepared replacement slot, publishes the complete new value, then runs old-slot release callbacks; cleanup allocates nothing. This protects the covered sensitive write edges, without establishing complete cross-domain Task closure, arbitrary Source graph reachability or universal cycle handling.

GET and index reads return independently owned elements and preserve the receiver value selected before evaluating the index. SET, index writes and PUSH first bind a supported root, evaluate arguments in order, then resolve that binding's current Array; an RHS that rebinds the root changes the target value. `a.push(a[0])` is valid. Bounds strictly require `0 <= index < len`; failure produces E0430 with the signed index and observed length, including the existing sticky initialization-failure behavior. Set/push return unit. An index-assignment expression returns an independent logical copy of its converted RHS, with fallible result retention completed before commit. Failure does not commit the mutation or undo earlier argument effects. Len borrows the receiver and reads length with bounded constant work, without allocation or element traversal. GET's transitive admission of nested function elements can allocate bounded scratch.

An escaped Array owns the TypeArena and backing lifetime needed for reading, copying and release after the originating call, Instance and Program host handles are gone. Pure arrays do not retain Program code merely for type metadata. Pure arrays may pass by value between Instances of one Program; different arenas reject even matching numeric type IDs. Function elements still require their real live execution gate; wrapping them in Array grants no call authority. New detach/growth storage is charged to the receiving Instance's current physical allocation domain; existing storage remains charged to its original domain. Admission, element relocation and call work obey cumulative budgets. Final release returns physical storage to its actual owner without recursive destruction proportional to nesting depth.

Source query snapshots independently own Array type nodes, member signatures, generic declaration owners, source positions and content identities, and remain valid after compiler-session release. Queries do not resolve, infer or execute. Native declaration provenance does not create an executable initializer. Failed private, alias or unadmitted-member lookups publish no successful binding; resource failure publishes neither Checked nor a snapshot. A core len query records its stable identity and leaves unknown source provenance unknown.

The Windows source VM, native, mixed-entry and Checked-reload paths verify the two-module string program against independent expectations: `red\nblue\n2\ngreen\nblue\ngreen\n3\n`. Separate assertions cover instance isolation, result lifetime, sticky initialization bounds and physical release after allocation failure. The output golden does not replace OOM, retain, work-budget or full sanitizer gates. Complete batch qualification still requires fresh affected targets, regression, contract and sanitizer gates; this section does not claim those complete gates or macOS have passed.

The other 13 Array members, and unfinished capacity, slice, ptr/mutPtr and iterator method families, still require individual admission; freezing existing recipes does not complete them automatically. Full move, user-struct families, cross-Program structural import, stdlib Checked/native package publication and final old-chain deletion are outside this subset. Source Array for-in follows its existing owned-snapshot contract and does not qualify iterator APIs. The sole current protocols remain defined by the implementation constants in §17.6, without old readers or compatibility interfaces. This contract update claims neither full safety nor full regression success.

### 17.27 Floating arithmetic and typed output

ADD/SUB/MUL/DIV_FLOAT were introduced with tags70–73 and OP_COUNT74; current opcode numbers come only from the implementation table. They use stage mask7, two equal concrete f32/f64 operands, the same result type, and zero unused fields. The sole Checked semantic, schema and value/call/Program versions are defined by the implementation constants in §17.6. The new admission rejects older semantic packets while preserving existing value and call layouts. Ordinary generic definitions need concrete numeric proof; specialization and revalidation grant no additional permission. Source arithmetic and compound assignment preserve existing evaluation/commit order. Mixed f32/f64 widens exactly first; typed integer/float mixing, floating remainder and bitwise operations reject.

Each operation rounds at its original width, nearest with ties to even, without fusion, reassociation or host FPU dependence. Wide multiplication and division remainders preserve final rounding information. Subnormals and signed zero survive; overflow yields signed infinity. Zero/zero, infinity/infinity, zero times infinity, opposite infinities added and NaN operands produce canonical NaN. Finite nonzero division by zero yields signed infinity, not an integer division fault.

PRINT/OUTPUT admit runtime f32/f64 values under §17.24, including every f64 NaN payload. Every NaN still formats as `nan` without modifying the input bits. Finite nonzero text uses the fewest significant decimal digits that round back at the original precision. Equal-length candidates choose the nearest exact decimal; an exact midpoint chooses an even last digit. Binary32 is not widened before formatting. The shortest coefficient has no trailing zeros. Decimal exponent k in [-4,16) uses ordinary notation, adding `.0` to integral results; other values use one leading digit, lowercase `e`, an explicit exponent sign and no redundant exponent zeros. Specials are exactly `nan`, `inf`, `-inf`, `0.0`, `-0.0`. Formatting uses ASCII and fixed integer workspace without heap allocation; 32 bytes suffice including termination. Locale, host rounding mode and exception flags do not affect results.

All arguments still evaluate before complete-group budgeting/rendering and one publication. Failures publish no partial group and retain existing OUTPUT_ERROR/cleanup rules. These contracts do not establish complete source, default product, safety or cross-platform qualification: rebuilt independent VM/native/packet/mixed programs, ownership/physical release, fault injection and batch gates must prove those separately.

### 17.28 Class field definition proofs and Unit module slots

Ordinary class T fields are checked at their authentic declaration owner using the existing copy/save premise, without new Scalar, Copyable or Sendable constraints. Inline bounds and where clauses share one condition representation. Symbolic fields remain in Checked; instances substitute complete ordered arguments and reverify provenance before Lowered. Closed carrier capability is separate from generic proof, and unsupported carriers never masquerade as missing constraints. Explicit final root classes retain construction, private/const, must/may and no_suspend rules. Class identity is the copy boundary; field GET creates an owned logical snapshot and whole SET is visible to all references of that identity.

Unit module declarations retain authentic owners, slot ordinals, known types, mutability and publication bits. Unit INIT/STORE have one encoding: zero value operands and both args zero. LOAD still checks publication and authority without payload SSA/frame storage. RHS effects, yield, throw and cleanup execute once, and publication follows success. Const initializer, mutable-root, sticky failure and reverse cleanup rules remain. This family admits inferred const x = Coro.yield(), explicit const x:() and default var x:(), while const x:i64 = Coro.yield() rejects. A Unit Sendable marker proof does not admit ordinary parameters or generic storage.

### 17.29 Finite inline class fields and physical ownership

This family admits only acyclic inline struct/enum field graphs with existing numeric/bool/string and Array<i64>/Array<string> leaves. One bounded capability traversal checks closed graphs after complete Source field collection; real temporary scratch is refunded and work consumed cumulatively. Ordinary T's legal language domain is unchanged. Unreified generic inline fields, nested Arrays, callable/Cell/Error, class-reference graphs, resources and views remain explicit implementation boundaries.

The sole four-argument GET runtime interface receives the actual arena, work/scratch and optional allocation domain. Scalar/string/Array-handle reads allocate nothing and use retained metadata after Program/Instance destruction. Inline snapshot materialization requires the caller's live domain; new bytes belong to that domain while existing leaves refund their original backing owners, without execution authority. SET uses one owned-storage transaction helper, completing all fallible work before a commit region releases old storage and publishes the replacement without an observable intermediate state. Failure preserves the field, aliases and prior snapshots. NEW never publishes partial this. Creation reserves release frames; drop iterates actual active enum payloads without allocation and refunds actual owners. This acyclic subset does not establish complete L0/weak/L2 residual-graph disposal.

The sole current protocols follow implementation constants in §17.6. Real seal/read consumers reject old value targets, valid-digest old Checked packets and old embedded proofs in the same Program ABI, without an old GET alias or second reader. Independent VM/native/packet/mixed expectations, authentic old caches, each OOM point, owned-result lifetimes, physical release and full applicable safety are separate verification obligations. This section does not claim complete language, complete stdlib Checked publication, default product or cross-platform qualification.

### 17.30 Entryless Checked Libraries and authentic imports

Compiler Modules carry an explicit PROGRAM/LIBRARY kind; an entry sentinel never determines authority. A Library retains authentic declarations, initializers, imports and definition-time constraint checks without allocating a user entry during inventory. It can be owned, checked, reverified, written and read, but cannot specialize directly into an executable Program, lower or seal. The linked PROGRAM requires authentic legal root/entry facts. Ordinary instantiation and provenance reverification still occur on Checked before immutable Program sealing. Every wire module, including provenance, encodes its kind. Unknown kinds, fake Library entries, missing Program entries and old formats reject without a second reader or synthetic entry.

A borrowed opaque compiler catalog independently owns checked library bodies. Complete packet digests, actual resolver authority, canonical/logical identities and source locators must match. Graph representation binds once at discovery; an existing SOURCE node cannot become CHECKED_LIBRARY through an import. A matched invalid library never falls back to AST, while unmatched imports continue ordinary Source resolution. Export queries grant no call authority; private calls still reject at the authentic declaration owner. The first owned Checked result deep-copies required facts and remains verifiable and executable after SourceResult, Session, graph and catalog destruction, without reparsing library source to supplement capability.

One remapper reserves all used target identities before copying bodies and relocating CALL. The authentic initializer may appear anywhere in the input function array. Temporary maps reserve actual scratch and free/refund it after success, OOM, work exhaustion or copy failure. Each mapped element consumes cumulative work; permanent bodies/names consume metadata. One reader cumulatively charges header, digest, payload decoding and ordinary reverification. Continuous reads and malicious valid-digest prefixes never reset the remaining work. Owned metadata is charged for the single published representation and temporary scratch refunded on exit. This reader contract does not claim that every Source phase now shares a single cumulative budget.

Within the Source construction context, metadata_bytes is the sole actual remaining balance. Complete SourceMemory headers/payloads, retained declaration manifests, Library/implementation reverification and query snapshot copying use that balance. Successfully owned storage is charged immediately, without subtracting a second cumulative allocated counter. Failure must not replenish the quota through unsigned underflow or continue allocating beyond it. Early release of private Source arena storage does not refund cumulative metadata charges; existing temporary scratch-map refunds remain unchanged. Final Built-to-Checked checking still has its independent stage budget. This correction does not establish a cumulative budget across every Source phase.

The first library-import implementation admits same-directory file-backed script authority, one module, zero-parameter Unit/i64 functions and CONST_INT/ADD_INT/CALL/RETURN. Unconnected tables fail explicitly without narrowing legal generics, nominals, interfaces, defaults or export const. Complete canonical remapping, declaration/default-helper facts, public const authority, package authentication and same-source stdlib Checked/native pairs with cache hits/misses remain obligations of the same pipeline. Source/packet/native/mixed expectations, instances, every OOM point, physical release and authentic old-identity rejection for this finite import loop do not establish complete language, safety or product qualification.


### 17.31 Checked purpose relations for parameter defaults

The original Built/Checked owns one DefaultTable containing authentic owners, full formal ordinals and helper identities. Names, Source-private ID arrays, queries and export order do not authenticate it. Validate every relation and unused default body in its declaration scope. Helpers have zero runtime parameters, are not exported entries, initializers or cleanup bodies, and carry the owner's isomorphic complete nominal-parent plus method-own parameters/constraints and exact formal result type. Their declaration scope can use private names without inheriting constructor, cleanup or body-effect promises. Valid defer children inside helpers retain ordinary cleanup origins.

Built/Checked CALL_DEFAULT and INVOKE_DEFAULT prove the caller's complete owner constraints, module/member and construction permissions before resolving the helper from owner/ordinal. Ordinary CALL/INVOKE/FUNCTION_REF cannot directly obtain helper authority. Within a local error handler, default computation uses INVOKE_DEFAULT with ordinary success/error continuations, result/error projections and owned cleanup. Errors cannot bypass catch or disappear when the owner call succeeds. CALL_DEFAULT targets hold owner/ordinal; INVOKE_DEFAULT args hold the same declaration metadata and its targets hold actual CFG edges. Metadata is not an SSA operand. Both have zero runtime arguments, zero immediate, the complete type argument vector and the exact formal result type. Migrate every existing local free/static/READ/constructor and generic default family atomically; finite initial library admission does not reduce the local language contract.

Specialization uses the existing Checked instance collector and per-instruction Origin proof, replacing CALL_DEFAULT only with ordinary CALL and INVOKE_DEFAULT only with ordinary INVOKE, clearing purpose metadata and independently revalidating it. Error edges and projection identities remain exact. Specialized Checked/Lowered keep no second instance-default table: the sole original table is deeply owned by provenance.source. Recompute owner/ordinal/helper and the full argument vector at the same caller/instruction position, with exact operation and CFG correspondence; equal-signature substitutes, role labels or arbitrary provenance grant no exemption. Local catches derive errors through the error continuation rather than joining ordinary CALL escape summaries. A fully explicit call to a generic owner does not instantiate unused helpers merely to populate a table. Closed original modules with defaults use the same specialization pipeline; Lowered rejects remaining purpose operations, and runtime has no separate default interpreter.

Tables, wire, cloning, provenance and canonical remapping account for actual metadata/scratch/work and publish nothing on failure. Ordinary instantiation completes before Program sealing. The sole format includes default records for every module and nested source, rejecting older formats. The implementation constants described in §17.6 own the actual sole schema/semantic and runtime ABI: compiler metadata or operations do not mechanically bump public runtime ABI, while actual consumers must still reject stale mandatory proofs at the same ABI. Complete capability requires freshly built independent VM/native expectations, producer-destruction lifetimes, all actual fault/physical-release points and batch qualification.

### 17.32 Multiple Checked Catalog inputs and Library strings

Extend the same Catalog, reader and Source remapper from §17.30 without a second linker or execution path. The sole constructor is xr_xir_compile_library_catalog_new(context,inputs,size_t count,output), replacing the old single-input signature. The sole resource view is xr_xir_compile_library_catalog_resources(catalog,size_t *count), removing the singular accessor. Construction requires output, non-NULL inputs and count>0; failures preserve output. A NULL SourceRequest catalog represents absence; no owned empty catalog. The view requires count, clears it first, returns NULL for NULL catalog, otherwise borrows the actual contiguous immutable bindings until catalog_free. NULL count returns NULL. Each independently owned Checked artifact and all identity text survive input packet/descriptor/text destruction. Separately zero-initialized binding and artifact-owner arrays are charged for actual storage, safely destroy any partial prefix, and publish only after every input succeeds.

Each input remains one independent dependency-free SCRIPT Library with a leaf logical path. Actual authority, complete packet SHA, existing canonical identity algorithm and declaration identity must agree. Every duplicate canonical rejects BAD_STRUCTURE, regardless of equal/different content, authority or physical root; no deduplication, first/last winner or new (canonical,root) key. Distinct canonicals may share an authority; actual resolution still checks exact authority and never falls back to AST for a matched bad binding. Multiple inputs do not imply inter-Library dependencies, package authentication, generic or general declaration linking.

Create one actual remaining budget. Check count storage multiplication/addition, metadata capacity and count<=remaining.work before dereferencing any input. Container, both arrays, actual text, every hash, read_remaining, shape and duplicate check cumulatively consume that balance without per-input resets or duplicate owned-reader charges. Charge at least one work unit for each input visit and duplicate candidate comparison, plus the bytes actually compared. A bounded pairwise scan is allowed; an uncharged quadratic loop is not. Refund temporary scratch on every exit, never cumulative work/metadata. Publish all inputs atomically, never a partial Catalog.

Finite Catalog admission adds STRING alongside existing i64 formals and Unit/i64 results, and only existing CONST_STRING/CONCAT_STRING to CONST_INT/ADD_INT/CALL/CALL_DEFAULT/RETURN. Unit formals, constructed types, generics, slots, implementations, provenance and dependencies remain unconnected; this consumer does not incidentally admit INVOKE_DEFAULT or error CFG and does not restrict the complete legal language. The ordinary Checked verifier checks every definition, unused literal and private/default helper; ordinary and default call permissions remain §17.31. Existing literal-length wire, STRING operations and runtime semantics add no proof authority or runtime payload, so retain sole Checked21/53 and Program25/Value15/Call19. Any discovered actual semantic/layout change requires renewed analysis and a freeze before implementation.

One no-emission source_literal_append(ctx,node,bytes,length,&id) manages the existing Source literal table. Local AST and already owned enum bytes retain established borrowed lifetimes; source_string_literal appends then emits the original CONST_STRING recipe. Reject invalid lengths/counts, nonempty NULL and growth overflow before charging legal append work. Charge one per legal append and the actual old element count when moving a vector; retain initial capacity16 and doubling. Failure publishes neither count nor ID. For imported nonempty literals, own exact length in SourceMemory and charge length copy work before append. Empty literals use NULL/0 without payload allocation. Never use strlen, terminator framing or content deduplication. Charge full headers/payloads through the true §17.30 balance, without an allocated shadow counter or early-release metadata refunds. Query has no runtime literal table or new authority API.

The sole SourceLibraryMap records literal_begin/count for a contiguous original-order range. Check CONST_STRING's nonnegative original ID against its own Library count before overflow-checked base addition; a valid merged index cannot rescue an invalid original definition. CONCAT_STRING still uses SSA operands and CALL_DEFAULT uses the existing function/default map. Refund the original function scratch map on all exits and commit next_function only after copying the complete Library. Never publish a partial Checked builder. Ordinary semantic-failure query snapshots retain the existing complete contract rather than requiring NULL for every failure.

Qualification includes two real Libraries each owning literal0 with different bytes, reversed Catalog order with fixed results, omitted/explicit STRING arguments, private-helper rejection, the original OwnedChecked executed after all producers die, Source/independent packet VM/actual native/mixed, two Instances, retained owned bytes read by length after Program destruction, and final physical zero. Empty, UTF8 and low-level three-byte a\0b literals must survive all four consumers; Source parser NUL rejection remains unchanged. Independent manual packets and correctly rehashed attacks cover original-ID-outside-source-but-inside-merged, unused invalid UTF8, length/count overflow, identities/authority/digests and every duplicate canonical variant. Fail every new actual allocation, release the first Library when the second fails, test cumulative exact/minus1 budgets and scratch refunds. Preserve all existing default, origin, budget and fault gates. This finite extension does not qualify mutable Library state isolation, complete Checked stdlib publication, complete product safety or final legacy deletion.

### 17.33 Finite typed value equality

The sole Checked57/wire22 carries this finite domain; the complete equality family and
public CLI migration remain incomplete. Equal uses bit4 of the existing marker:u32, with ordinary TYPE
definition-site proofs, inline/where conjunction and provenance revalidation. The first domain
is only bool, eight integers, f32/f64, string and recursive Array<D>. Other domains still reject
explicitly and remain goals. Same-T ==, != and real Core assertEqual<T: Equal> share one typed
relation. EQUAL=117, OP_COUNT=118 and stage mask7 take two VALUE operands and produce BOOL;
immediate is only0(EQ)/1(NE), with zero unused fields. NE inverts only a successful comparison.
Correctly rehashed56 and older packets reject before body allocation.

FLOAT retains IEEE NaN inequality and signed-zero equality; STRING compares full length/bytes;
Array compares length then indexed leaves, without pointer/shared-backing shortcuts around NaN.
Use budgeted explicit DFS rather than C recursion or a depth cutoff returning false. VM/native
consume one typed helper: borrowed inputs are neither copied nor dropped, and only OK publishes
bool. BAD_ARGUMENT/OOM/LIMIT preserve output and input owners; all exits refund real scratch/domain
allocations. Charge cumulative STRING bytes, Array elements and growth work. Exhaustion cannot
mean inequality or satisfy assertPanics.

Source/Owned query/clone, default helpers, Library, writer/reader, Checked specialization and
Lowered revalidation retain one constraint identity. No legacy Equatable, boxed-value adapter,
name whitelist or second executor grants authority. This family adds no public Value/Program
fields; current Value16/Call21/Program26 are defined by §17.6 and §17.34 and require actual
fresh layout/provider-rejection/generated-C gates. Require
independent full packets/valid-rehash, unused unconstrained definitions, same-named user
interfaces, shared-backing NaN, NUL/depth257, every actual OOM and exact/minus1, dead producers,
two Instances and physical refunds, separate fixed VM/native/mixed outcomes and central
default/ASan. Complete assertions/containers/CLI, paired stdlib publication and full safety remain OPEN.

### 17.34 Typed output and explicit configuration initialization

The sole XrXirOutputStatus is OK=0, OOM=1, LIMIT=2, OUTPUT_ERROR=3, BAD_ARGUMENT=4 or BAD_ABI=5. Renderer, byte sink and group provider return the same closed status. Failure while preparing a complete group never calls the sink; success calls it at most once, preserving actual lengths, embedded NUL, UTF8, empty-print newline and long strings. Already written IO bytes do not promise rollback. Unknown statuses become BAD_ARGUMENT, without a boolean conversion that loses resource failures.

PRINT failure preserves the exact Call status. For a valid configured provider, WRITE_STREAM maps OK to true and OUTPUT_ERROR to false. An absent provider remains Call OUTPUT_ERROR; OOM/LIMIT/BAD_ARGUMENT/BAD_ABI never become false. Existing cancellation priority remains. Resource failure is not panic and cannot satisfy assertPanics.

Current Call ABI is21. Provider/Sink start with abi_version and reserved=0: callbacks require21, while no provider requires canonical all-zero state. CallConfig/InstanceConfig start with abi_version and struct_size, checking21 and exact sizeof before reading other fields. Actual Windows x86_64 sizes are Provider24, Sink32, CallConfig136 and InstanceConfig88; these do not qualify other targets. Sole xr_xir_call_config_init(config,size) and xr_xir_instance_config_init(config,size) return CallStatus, writing only on success and preserving all output bytes on failure. Old by-value defaults symbols are removed without aliases or wrappers. Actual Call20 entries reject before callbacks; old embedding defaults objects must fail to link rather than being rebuilt as purported old evidence.

Checked22/57, Value16 and Program26 retain their own actual contracts. Output migration grants no Source/Target/SDK authority and does not qualify complete safety, default stdlib caching or final legacy deletion. Independently expected VM/native behavior, every actual allocation failure, exact status, old-object rejection, producer lifetimes and physical release need separate verification.

### 17.35 Flat Tuple destructuring and atomic module publication

Flat `var` / `const` patterns admit identifier or `_` leaves, exact ordered arity, Unit and `(x,)` singletons. Evaluate the RHS once and prepare all field projections and mutable Cells before publishing local names; closure capture retains the existing Cell/ownership contract. An empty pattern accepts only Unit and an all-discard pattern creates no binding or module group. Actual types, Sendable, visibility, construction/execution authority and definition-time constraints with Checked specialization/revalidation remain mandatory; wrapping does not widen ordinary Unit or resource permissions. Nested/rest, array/object patterns, spread, match and leaf annotations reject specifically at the current entry while remaining language obligations.

The sole new SLOT_GROUP_INIT appends ordinal145; current OP_COUNT146 preserves every earlier ordinal. type=Unit; args0 is an operand-table offset and args1=m counts non-Unit payloads. The immediate packs first_slot in high u32 and n contiguous actual bindings in low u32. Prevalidate n>0, the entire same-module range, actual initializer, unpublished state, exact n/m, types and permissions. Unused fields are zero; Unit consumes no SSA/payload and forged placeholder IDs reject. Outgoing layout is m. Every stage shares this operation and VM/generated C call the sole xr_xir_instance_slot_group_init(view,first,n,compact_values,m).

The actual Call allocation domain, scratch, value limit and bounded work own preparation. Obtain every admission, copy, retained hold and cleanup resource in a private owned table before checking execution_status. Any failure or precommit cancellation frees preparation without a published slot or trace. The infallible commit installs all slots, flags/count/order before tracing; trace reentry sees the complete group and cancellation on the first trace does not roll back committed bindings. Cleanup allocates nothing and refunds each actual backing owner; initialization failures remain sticky.

This operation uses Checked semantic65 while wire25 and Value21/Call26/Program29 structures remain unchanged; §17.6 implementation constants still solely define current versions. Complete previous25/64 packets reject at the header with zero allocation. Independent fixed family roles construct current packets and preserve complete64 preimages as rejection evidence rather than deriving expectations from the writer. The new SDK helper needs same-source qualification through the actual165-entry (160 source+5 archive) recipe and five-archive consumers; all original345 ABI expressions and139 object responsibilities remain. Qualification retains forged Checked shapes, independent VM/native/mixed outcomes, every actual OOM/three-axis boundary, retain saturation, cancellation, producer/Program/Instance first-drop lifetimes and final physical zero. The flat subset does not replace full default/product safety or final legacy deletion.

### 17.36 Ordinary generic imports from Checked Libraries

This freezes the next import family; implementation and qualification remain pending. Extend the sole Catalog, reader and Source remapper from sections17.30–17.32 with ordinary free generic functions and their private function closure. The Library itself has one dependency-free module, TYPE binders and existing builtin marker constraints without interface witnesses. Parameters/results use admitted i64/string/bool, Unit results or that function's binders; operations remain the Catalog's admitted set. Ordinary definition checking proves unconstrained parameters, explicit/inferred/forwarded arguments and every unused definition. Sendable grants no arithmetic, member or construction authority. Valid new caller-defined types instantiate through the receiving Checked type pool; absence of Library-owned nominal tables must not restrict callers to scalar arguments. RESULT binders, Library-owned constructed/nominal tables, slots, implementations, provenance and dependencies await authentic mapping, without opening unconnected tables or new operations.

Reserve destination IDs for the authentic initializer and ordinary/private functions before relocating CALL, CALL_DEFAULT and literals through the same SourceLibraryMap. Deeply own each Generic's constraints, arguments, parameter_kinds and counts on the receiving ledger. Binder BASE+p remains declaration-local and instruction type_arguments ranges belong to the receiving function; blocks/SSA stay function-local. SourceContext.has_generics, SourceFunction.generic_owner and query generic counts/constraints/kinds represent the receiving declaration; free functions have no generic_parent and queries grant neither verification exemptions nor access. After all producers die, use the original owned Checked for specialization/revalidation, Lowering and Program sealing, without missing ASTs or runtime instantiation. Charge actual metadata/work, refund scratch on every exit and publish only a complete import. Do not mechanically bump versions if wire, Checked structural semantics and runtime ABI are unchanged; freeze any actual discovered change first and add independent rejection evidence.

Qualification includes a private generic helper, two Libraries both using binder0/function0, reversed input order and shifted destination ordinals, explicit/inferred/forwarded calls, new caller types, unused invalid definitions and denied external private calls. Execute VM, independent packets and actual native output after producers die, with two Instances and owned results. Preserve existing library/string/output gates and every new actual OOM, exact/minus1 axis, cancellation, physical-release and applicable safety responsibility. This finite family does not qualify complete Library state/dependencies, complete Checked library publication, installation or the whole product.

### 17.37 Checked standard library publication and ordinary execution

The standard library supplies one Checked XIR semantic input, including declarations, generic definitions, private implementation closure, module initialization, effects and ownership facts. The current published family is io/output; this does not qualify the remaining library families. Library templates are verified input rather than executable Program authority. Catalog admission verifies the complete Checked body, its canonical module identity and dependencies under the caller's cumulative resource ledger. Byte digests never replace semantic verification. Catalog ownership survives input/source destruction and copies physical roots. Missing or malformed advertised input rejects without a source fallback.

The Checked producer performs Source checking and Checked encoding without CGen, native compilation, object inventory or a runtime SDK dependency. Building the producer executable still needs the supported host C toolchain. Embedded immutable Checked bytes and their own component manifest contain no machine-code callbacks. CLI, LSP, VM and AOT obtain library input from this same Catalog independently of native code. VM specializes, reverifies and lowers reachable instances before ordinary immutable Program sealing. VM binding does not look up or replace standard library functions with cached native entries and needs no C compiler at execution time. AOT compiles library and user XIR together through the ordinary C backend.

Standard library native registries, matching factories, cache leases, paired build identities, object/C mirrors and cache-specific sealing APIs are removed. No compatibility flag, empty cache or second execution path remains. Ordinary native functions, bidirectional VM/native mixed execution, shared Value/Call/Program/Instance, resumable ABI, CGen, necessary platform hosts and the independently verified runtime SDK remain. Actual SDK target/profile/ABI and generated native code admission retain their own rejection boundaries.

Core publication installs only the Checked component and its exact module/length/digest manifest. The SDK component verifies its own target, ABI, source and archive manifest independently; SDK-only installation needs no standard library pair. Components and canonical relative paths reject before publication, and failed verification preserves any previously published manifest. Real Core/SDK/full installs, relocation, source-free VM/AOT, user-created generic types and missing/corrupt components require independent tests. Complete library delivery is separate from removal of the cache for the currently supported family.

Preserve the independent seven stdout bytes out\0 followed by U+4E2D, four stderr bytes err! and empty; two Instances, four provider outcomes, imported leaves and user wrappers, same-spelling user declarations, producer death, escaped Function/result lifetimes and physical release. Retire only cache match/lease/object-pair assertions. Every actual preparation and bind/seal allocation failure, exact/minus1 resource axis, retry without ledger refill, failure-preserving take and zero-allocation cleanup remains covered on the ordinary paths. W1–W4, portable C11, applicable provider/sanitizer gates and all existing generic/library/output obligations remain.

### 17.38 Shared root execution facts and owned causes

After complete Checked/Lowered verification, the sole owned XrXirEffects derives independent requires_root and unresolved facts per function; both may be true. Missing data, zero initialization and failed inference never prove NONE. Mutable module slot reads/writes/PLACE and authentic module initializer identities require root. Const reads/PLACE prove Sendable in the actual definition context; a type failure requires root, while allocation/budget failures abort unchanged. Group initialization uses the existing high32 first-slot encoding. Library mutable variables and nonSendable constants remain forbidden.

Use the sole reverse execution graph for CALL/INVOKE, authentic default helpers and registered cleanup. Cleanup propagates root facts without changing control-effect rules. Indirect/requirement calls consume the authentic callable closed upper bound; NO_SUSPEND remains independent of root permission. FUNCTION_REF does not execute its body, and local variables/cells are not root state merely because they are mutable. A GO child is not a same-flow parent call edge; actual parent argument preparation still contributes to the parent. Unclassified executing operations reject, and runtime paths conservatively join independently of optimization.

Queries borrow facts and two separate shortest numeric cause chains from the same summary, which survives producer artifact destruction until freed. Local terminals have distance0; CALL/cleanup steps strictly decrease callee distance. Equal-distance choices order actual instruction/callee/cause/slot identities rather than adjacency insertion. Solving, causes and type proofs share cumulative resources; failure publishes no partial summary and never overwrites occupied output. The nonparameterized boolean graph costs O(I+F+C), excluding type proofs and higher-order variables. This query contract changes no wire/opcode/ABI and does not complete GO consumers, callable parameter bounds, execution identities or library-state protection.

GO consumers obtain permission from the final root facts in that same summary, without repeating slot or indirect-call root inference. Known root and unresolved both reject; when both occur, the primary diagnostic selects known root while retaining both causes. The current SLOT_PLACE borrow-shape limit propagates independently: root NONE for Sendable const does not remove this unqualified limit, and shape failure is never mislabeled root or unknown. Roles, parameter/result and capture Sendable, Task errors and all type/ownership/visibility obligations stay separate; GO argument preparation still executes in the parent. Source distinguishes required instance-root execution from missing worker-effect proof. This slice keeps the existing function-level location; complete user function/variable cause payloads and precise GO-expression locations remain open.

The owned cause-trace copy interface copies finite known and unresolved numeric chains from the same effects owner. Facts and steps borrow only the independent trace; they remain readable after effects, Checked/Lowered artifacts and their original producer are released. Valid NONE may produce an owned empty trace. Copy requires the same actual resource owner and an empty output, rejects invalid arguments before work, admits cumulative resources before atomic publication, and releases temporary ownership on OOM or budget failure without refunding charges. Free neither allocates nor formats; the final real resource references and physical counts reach zero. This interface adds no wire or runtime ABI and is not complete Source-name, origin or GO-location reporting.

Closed callable root bounds occupy the existing u32 flags: NO_SUSPEND=1, ROOT_NONE=2, ROOT_REQUIRED=4, ROOT_UNRESOLVED=8. Only root masks 2/4/8/12 are valid; 12 retains both requires_root and unresolved. Old 0/1, NONE combined with another root bit and unknown bits reject. Ordinary explicit fn defaults to UNKNOWN; NO_SUSPEND never proves NONE. Identity includes all flags and nested signatures. Only the outer callable can weaken with unchanged parameter types/modes, result and nested identities: NONE fits NONE/ROOT/UNKNOWN/mixed, ROOT fits ROOT/UNKNOWN/mixed, and UNKNOWN or mixed fits only UNKNOWN/mixed. NO_SUSPEND may remain or be discarded. Lost precision cannot be recovered from a target address. Joining unions the two root facts with NONE as neutral; NO_SUSPEND survives only when both operands promise it.

FUNCTION_REF construction does not execute its body, but its advertised bound must cover the authentic target facts from the same effects owner, even when unused. Every authentic interface implementation body must also satisfy its original requirement bound; receiving context does not grant a body promise. The common Checker validates complete structure/types/constraints/access before using the sole execution graph for root bounds and GO obligations. Precise ordinary references subsequently use an unpublished owned prepared owner bound exactly to the verified Module and owned INFERRED/FIXED value-flow records. Fixed explicit fn boundaries cannot refine by guessed targets; unknown dependencies remain UNKNOWN. Before the finite worklist closes and ordinary reverification succeeds, prepared state proves neither Checked nor NONE. There is no skip_go, unchecked public entry or second product pipeline, and no recipe/AST borrow survives its owner.

This flags meaning uses the sole Checked semantic70; schema25 retains its existing u32 shape. Public Value22/Call28/Program29 fields and layouts remain unchanged. Old semantic69 and old 0/1 callable flags reject without a second reader. SDK source fingerprints, independent stdlib Checked components and consumer vectors require actual renewed qualification. This freezes only closed-bound foundations; precise Source references, sealed Program authentic entry-bound admission, real child indirect execution, finite parameter formulas, complete Library type mapping, full user causes and Library-state protection still require separate implementation and validation.

Prepared ordinary reference refinement records owned INFERRED/FIXED value identities before region recipes are released. Ordinary direct references and closure sites close against authentic targets on the existing effects owner. Explicit callable parameters, annotations, results and conversion contexts remain fixed upper bounds; equal provisional UNKNOWN identities do not erase a boundary. A private COPY preserves it and becomes the sole ordinary FUNCTION_WEAKEN when closed identities differ. Private structure/SSA/constraint/access preparation publishes no Checked. The common complete Checker verifies all effects and permissions on both construction memory and its independent owned copy before publishing only that copy.

COPY/PHI/local Cell, closed captures, Nullable wrappers and readonly Tuple/Array construction and projection synchronize identities. Callable PHI and uniform Array inputs use the sole outer weakening relation; the two root facts close independently. Fixed composite nested identities cannot vary. Unsupported nested weakening, shared mutation, element updates or Nullable branch joins retain a conservative bound or reject; finite higher-order dependencies and container alias flow remain incomplete. NO_SUSPEND, GO borrow shapes, errors and Sendable obligations remain independent. Preparation, closure and query copying share the original finite ledger without wire, schema, semantic or public ABI additions. This implementation candidate still requires fresh builds and both configurations' resource and safety gates; it grants no runtime or VM/native execution qualification.

### 17.39 Bounded CTFE scalar local storage and short circuiting

Compiler execution receives completely reverified Lowered input. It admits the entire reachable direct-call closure of bool/integer ordinary functions before execution. SCALAR_LOCAL_NEW, SCALAR_LOCAL_READ and SCALAR_LOCAL_WRITE use the same scalar step and target frame offsets as the VM. A local write produces Unit and falls through; it is not a control-flow terminator. Actual frame accesses consume work. Existing steps, whole-stack depth/frame bytes, output bytes and cumulative compiler ledger limits remain; a callee does not reset them, failures refund no charges and preserve canonical Unit output.

Source &&/|| preserve short-circuit semantics through this storage, the ordinary Checked→specialization reverification→Lowered CTFE pipeline, and an independent complete Source replay that checks ordinary constant recipes. An unexecuted pure arithmetic error may be skipped; reachable I/O in an unexecuted branch still rejects the entire closure. Root authority or effect=NONE grants no determinism.

This section does not admit owned Cells produced by ordinary var, owned/path operations, aggregate/string exports, unknown function values or native/Instance execution. Each requires authentic type, lifetime and charging interfaces plus independent qualification; stack scalar storage does not establish those capabilities.

### 17.40 Bounded CTFE CFG backedges and declaration identity receiving

Leaf CTFE accepts scalar CFG backedges after full Lowered reverification. PHI values select the actual predecessor and copy in parallel through the shared scalar edge kernel. Execution shares the original steps/watchdog and compiler ledger: a backedge neither resets nor refunds charges, and a nonterminating loop rejects at its finite step limit while preserving canonical Unit output. Full receiving still checks PHI predecessors, targets, offsets and signatures; block ordinals do not prove termination.

CTFE requires actual ordinary function declaration identity. Structurally valid Lowered input without a declarations/functions table grants no execution qualification; leaf and direct-call entries reject with NOT_LEAF and NOT_SUPPORTED respectively, preserving output and spent charges. Module initializers, nominal/cleanup owners, unsupported actual operations and reachable I/O retain individual rejection; root or effect flags grant no determinism. Leaf retains its no-CALL boundary; finite calls use the separate complete-closure receiving entry.

### 17.41 Canonical CTFE Unit returns and ordinary Source replay

Ordinary Unit-result functions without value parameters can enter actual CTFE closure admission. Their Unit RETURN preserves zero type/reserved/payload and has zero target logical output size. A Unit return has no SSA storage slot, so a child return must not write that slot; the caller retains scalar parameters and other live values. Unit is not admitted as an ordinary value argument, and receiving still repeats actual type and complete Lowered identity checks.

Source receives Unit comptime results through the same bounded CTFE pipeline and independently replays ordinary Unit expressions, preserving site matching, coverage and canonical result verification. Empty blocks and blocks ending in ordinary statements produce Unit; owned local state continues to require the actual value-domain interface. A comptime block grants no nonlocal return from its enclosing function. I/O, nonterminating loops and unsupported actual operations retain closure or budget rejection, with failure outputs and the original compiler ledger preserved.

### 17.42 Bounded CTFE Rune values and ordinary Source constants

Rune joins bool/integer admission for actual scalar parameters, results, locals and direct-call closures. CONST_RUNE, INTEGER_TO_RUNE and RUNE_TO_INTEGER use the existing shared execution kernel. Rune equality validates both complete Unicode scalars and compares code points; it does not interpret Rune as an integer type or host character. Valid payloads are 0..0x10ffff excluding 0xd800..0xdfff. Invalid Rune arguments reject with BAD_ARGUMENT; out-of-range integer conversions reject with NUMERIC_RANGE, preserving canonical Unit output. CONVERT_NUMBER continues to admit only integer-to-integer conversion and grants no floating admission.

Canonical target layout determines exported size: Rune occupies four bytes and output_bytes=3 rejects. Host carrier sizeof and UTF-8 length do not determine that budget. After actual Checked/specialization reverification/Lowered execution, Source independently replays an ordinary CONST_RUNE recipe with complete site, type, canonical-value and coverage checks. Nested comptime and ordinary Rune calls retain full closure admission. Existing steps/depth/frame/output ceilings, the sole cumulative compiler ledger, physical OOM and final-owner cleanup obligations remain. This family grants no managed aggregate, string, unknown function-value or runtime-fallback qualification.

### 17.43 Private Cell execution on the compiler ledger and Unit tail assignment

After admitting the entire actual call closure, CTFE allows frame-private Cell<Unit/bool/integer/Rune>. Ordinary var CELL_NEW/READ/WRITE, lazy CELL_LOCAL_WRITE and LOCAL_UNINIT use authentic existing Value operations. Type metadata is independently copied into a compiler TypeArena; value storage uses the original XrCompileResources-backed Domain. Allocation, copy, traversal and Domain work retain one cumulative ledger and physical FI. Local receiving fuel bounds one scalar check; it neither clones, resets nor refunds the compiler budget. Scalar entries without Cell operations create no such Domain/arena.

Actual owned offsets are released on normal and child returns and on step, budget, OOM or arithmetic failure. Frame setup prepays its cleanup offset scans; release needs no remaining execution budget and allocates nothing. Each Cell access checks the same private Domain. Parameters and exports remain limited to canonical Unit/bool/integer/Rune results and ordinary scalar arguments; external Cells, ref loans, module state and arbitrary owned values are not admitted. Full type, predecessor/PHI, target, identity, signature, reachable-I/O, depth/steps/activation-byte and original output checks remain. Root/effect flags grant no purity, and no Instance/native/fallback runs.

A Unit block's tail assignment may carry an ordinary expression's internal ID, but its helper Unit RETURN must use zero, creating no Unit SSA slot or forwarding that ID. Source independently replays the complete input, matches sites and canonical results and receives it through the ordinary Checker. Private Cell execution exports only actual scalar/Unit results. This family grants no Cell/aggregate/string export, function-value, cleanup-function or suspension qualification.

### 17.44 Owning CTFE String exports and independent Source receiving

String parameters, results and Cell<String> use authentic Value/TypeArena storage and the original compiler Domain. External String arguments first become independent full-UTF-8 snapshots on this compiler ledger. Internal calls use owned-slot retain/copy/move; normal, child and all failed exits release every slot. Constants, concatenation, owned local copies, rune-count length and equality retain existing Value semantics. Scanning/copy/comparison spans pay original cumulative work before execution, and Domain allocation/resize uses the same ledger. No Instance/native/AST fallback runs; root/effect flags grant no purity.

String output size is its actual UTF-8 byte count, not a host pointer, carrier sizeof or rune count. Empty strings can export with output_bytes=0; exceeding the caller-selected logical output limit under17.61 rejects with OUTPUT_LIMIT and preserves Unit output. Publication first obtains an independent owned reference. Frame/Artifact/Source/ledger-caller death cannot dangle the result; final drop allocates nothing, refunds no cumulative fees and only decreases live bytes.

Each original and replay Source site holds its own real result. Independent replay copies complete bytes into its ordinary literal/CONST_STRING recipe before complete Checker/query publication. Embedded NUL retains explicit length; unknown/duplicate/extra/type-mismatched sites still reject. Every partial result and second-construction/charging/publication failure releases its owners. Target/profile and original steps/depth/frame/output and compiler three-axis bounds remain; arbitrary aggregate/function-value/resource-handle exports are not granted.

### 17.45 Bounded CTFE String comparison and readonly search

CTFE admits String byte-order comparisons (<, <=, >, >=), contains, startsWith, endsWith, indexOf and lastIndexOf in the complete Lowered function closure. Existing Value operations determine semantics; no native, Instance or AST evaluation path is introduced. The indexOf start and both search results count Unicode scalars, as does String length. Comparison uses complete UTF-8 byte order and embedded NUL retains its explicit length. Empty patterns, missing-result -1 and start equal to scalar length retain ordinary runtime rules. A negative or excessive start returns NUMERIC_RANGE; Source reports a value range violation and preserves output.

Variable scans prepay a conservative upper bound on the original cumulative compiler ledger before calling Value operations: comparison byte spans, each candidate byte's pattern comparison and loop bookkeeping, the search algorithm's skip-table initialization and pattern visits, and UTF-8 index-conversion scans. Every sum and product checks overflow. Unpayable or overflowing work rejects BUDGET before an unbounded host search. Empty patterns and established range errors require no scan. This prepayment creates no quota, refund or reset of the original 128M work, 64MiB allocated and 8MiB live limits.

Inputs still become compiler Domain snapshots. Arithmetic-range, OOM, step and work failures release all frame owners and preserve caller Strings. Source retains two independent constructions, ordinary constant recipes, complete Checker and query publication. Reachable I/O still rejects even when a search condition leaves its branch unexecuted. This family grants no arbitrary aggregate, function-value, resource or cleanup-path admission.

### 17.46 Immutable compiler Tuples and independent constant receiving

CTFE receives completely reverified type pools and ordinary function closures. Iterative admission accepts Unit, bool, integer, Rune, String and immutable Tuples of these families. A type ID has meaning only in its actual owning descriptor pool. Cyclic descriptors, open types and fields carrying function, Cell or resource authority reject; root/effect flags grant no determinism. TUPLE_NEW constructs authentic Values from the complete ordered field vector. Unit fields retain their position without an SSA slot. TUPLE_FIELD, internal Tuple arguments/child returns, owned locals/parallel PHI and private Cell<Tuple> preserve real owned references on the same compiler Domain. External Tuple arguments still require bounded cross-arena receiving and currently reject fail closed.

Descriptor DFS, summaries and every metadata allocation use the original ledger. Nested receiving prepays the complete expanded type shape; repeated field positions count each occurrence and data sharing refunds nothing. Overflow or unpayable work rejects BUDGET. Constructor argument vectors use complete arity in actual activation bytes. Traversal scratch uses the remaining original frame quota, including simultaneous old/new blocks during resize. Original steps65536, depth128, frame65536, the output ceiling in17.61, allocated64MiB, live8MiB and work128M remain without reset or refund.

Tuple export size recursively sums target scalar storage sizes and actual String UTF-8 bytes in field order. Unit contributes zero; host pointers, ownership headers, type IDs, carrier sizeof and padding do not count. Exceeding the original request ceiling preserves Unit output. Publication first obtains independent ownership. The type arena, nested Tuples and String fields remain live after Source/Artifact/ledger-caller death. Final drop allocates nothing and refunds only live bytes.

Source matches actual type structure across owners for helpers and replay results, without assuming equal local type IDs. Independent receiving uses iterative owning frames to record ordinary field constants and TUPLE_NEW recipes, followed by the complete Checker and query publication. Explicit NUL length, Unit positions, closed generics and original site/owner/expected coverage remain. Field, OOM, budget, construction and publication failures release all temporary results. This admission grants no complete Array/struct/enum, Tuple equality, function-value or cleanup-path qualification.

### 17.47 Bounded CTFE floating bits and target constants

After complete Lowered reverification and entire direct-call closure admission, CTFE permits f32/f64 arguments, results, ordinary locals, private Cells and immutable Tuple fields. CONST_FLOAT, negation, arithmetic, relations and concrete numeric conversions reuse the existing integer-only IEEE bit kernel. They depend on no host FPU, Instance, native or AST fallback. Subnormals, nearest-even rounding, canonical NaN, Infinity and signed zero retain ordinary execution semantics; floating division by zero produces IEEE results. Out-of-range, NaN and infinite floating-to-integer conversions return NUMERIC_RANGE and preserve Unit output.

Each floating operation prepays 1024 work units on the original cumulative compiler ledger before entering a numeric API. This conservative fixed upper bound covers 64-bit decode/pack, normalization, product carry scans and long division; these loops depend only on fixed format widths. Prepayment creates no cloned quota, reset or refund; an unpayable reserve returns BUDGET first. Complete type, identity, CFG, reachable-I/O and original steps/depth/frame/output/allocated/live/work limits remain.

Logical f32 and f64 outputs occupy four and eight target bytes respectively; Tuple fields continue to sum target storage sizes. f32 inputs require zero high bits and canonical NaN encodings; f64 retains ordinary Value admission rules. Independent Source replay rechecks target canonical floating payloads, records ordinary CONST_FLOAT recipes and invokes the complete Checker. External Tuple, arbitrary Array/struct/enum, function-value and cleanup-path qualification is not granted by this admission.

### 17.48 Compiler-owned Nullable variants and bounded constant receiving

After complete Lowered reverification and entire reachable direct-call closure admission, Nullable wraps admitted primitive values, String, immutable Tuples and legal nested Nullable values. Type IDs still belong only to their actual immutable pool. Iterative family admission rejects cycles, illegal elements and Cell/function/resource authority. NULLABLE_NONE/SOME, IS_SOME and UNWRAP use authentic Value operations on the original compiler Domain to construct, observe or independently retain the active payload. Internal arguments/child returns, owned locals/parallel PHI, Tuple fields and private Cell<Nullable> preserve ownership. Unwrapping None returns the distinct NULL_UNWRAP status; Source reports a positioned compile error, preserving Unit output and releasing every frame.

Logical export obtains discriminant bytes from the sole TypeArena storage layout and adds the recursively measured active Some payload. None has no payload cost. Inactive storage, alignment padding, pointers and ownership headers do not count. The current canonical Nullable tag occupies one byte: None<i64> uses one byte, Some<i8> two, while Some<i64> requires nine and exceeds the original the output ceiling in17.61 ceiling. Iterative export scratch retains the original frame quota and simultaneous old/new resize peak. Target scalar sizes, String UTF-8 lengths and zero-byte Unit Tuple fields remain unchanged.

The complete finite type shape prepays original ledger work before each Value receiving operation; None does not evade type admission. Nullable equality admits only the Nullable chains and primitive/String leaves supported by the existing common Value equality owner. NaN, signed zero and complete NUL bytes retain that sole semantics. Fixed numeric work and active String scan spans pay the original cumulative ledger before comparison; overflow or an unpayable reserve returns BUDGET first. No second semantics interpreter, quota, refund or Instance/native fallback exists.

Independent Source replay matches authentic type structure across owners, retains actual variant and field owners, and iteratively records ordinary NULLABLE_NONE/SOME and payload constant/TUPLE_NEW recipes before complete Checker and query publication. Original site/owner/expected coverage and every failed-prefix release remain. External Nullable and other composite arguments still require bounded cross-arena complete snapshot receiving and currently reject explicitly. Array/struct/enum, function-value and complete cleanup qualification are not granted by this family; original budgets and mandatory safety gates remain.

### 17.49 Bounded compiler Arrays, private mutation and capacity receiving

After complete Lowered and entire actual direct-call closure reverification, CTFE uses authentic ARRAY_NEW/REPEAT/GET/LEN, CAPACITY/WITH_CAPACITY/RESERVE, SET/PUSH and private Array Cell CELL_PLACE/PLACE_READ/WRITE operations. Family admission accepts primitive/String leaves, pure Array chains, Tuple/Nullable elements with statically bounded receiving spans and legal outer Tuple/Nullable wrappers. Internal arguments, child returns, Cells, owned locals and PHI retain the original Value/TypeArena/Domain. No Instance/native or AST fallback runs, and root/effect flags prove no determinism. Only synchronous places from actual current private Cell roots may mutate; module roots, external Cell/ref and unsupported field/index projections reject.

Type shapes report costs and variable admission spans, never Value or execution authority. Metadata, vectors, copy, scans, growth and cleanup prepayment use the original cumulative ledger. Bounded shallow header observation obtains Array length; length times the complete static element shape prepays a conservative receiving bound before calling Value, with overflow checks. Iterative owning observation locates variable Array spans inside Tuple/Nullable wrappers without host recursion. Scratch consumes the remaining original frame quota, including simultaneous export-stack and Value-admission scratch peaks. Unpayable work returns BUDGET first; original budgets, steps, depth and nonrefundable charges remain.

Existing array_get admits the entire Array first. If an element contains another boundary requiring variable Array-content admission, such as Array<Array<Tuple<...>>> or Array<Array<i8?>>, current APIs cannot obtain its internal length through bounded observation before that call. This specific family currently fails closed pending Value-owner observation/receiving that charges before each access. No second Value representation or unpaid preliminary scan is introduced, and this interface gap does not mean the complete Array objective is achieved. Pure Array<Array<i8>> needs no such deep receiving and can execute. Arbitrary Array equality, other Array methods and complete paths are not granted here.

Logical export recursively sums active elements' target scalars, String bytes, Nullable discriminants and Tuple fields. Empty Arrays use zero bytes; capacity, pointers, ownership headers and inactive storage do not count. Actual capacity remains observable ordinary semantics. Independent Source replay preserves it through WITH_CAPACITY, private Cell, element PUSH and CELL_READ recipes, rather than rebuilding a literal with different capacity. Every failed prefix releases temporary owners. Exported types, elements and ledger remain valid after Source/Artifact/caller death; final drop allocates nothing and refunds only live bytes.

Authentic COW writes preserve prior snapshots. Negative length/capacity returns NUMERIC_RANGE; bounds errors return the appended compiler-only BOUNDS status. Source reports positioned compile errors, preserving Unit output and borrowed values. Foreign composite snapshots, elements with nested variable receiving, nominal/function/cleanup and mandatory shared/full safety qualifications remain part of the original objective; this limited development qualification proves no completion.

### 17.50 Original-ledger prepayment for common Array/Nullable equality

After complete Lowered, actual family and entire reachable closure admission, XR_EQUAL compares admitted Array/Nullable chains and the primitive/String leaves supported by common Value equality. Only existing xr_xir_value_equal determines the boolean. The prepayment walk adds no numeric, String or container comparison semantics. Shared backing or identical addresses do not prove reflexivity: a NaN Array remains unequal to itself, while signed zeros retain common IEEE equality. Capacity does not participate in content equality; NUL retains complete String length. Tuple/nominal or execution/resource-bearing equality still requires authentic owner support, never a guessed capability from a type name.

An iterative owning pair walk charges before each header/type/child access. Shallow Array lengths and active Nullable variants bound later spans. Each authentic Array getter prepays its entire receiving span; complete String comparison bytes and fixed-width work for every floating leaf pay the original cumulative ledger before common comparison. Bounds are conservative; overflow or unpayable work returns BUDGET first, with no reset or refund. Length and None branches require no unused-payload scans. Actual comparison still calls the same Value semantics, without an unbounded host walker, Instance/native execution or AST fallback.

The prepayment stack consumes the original remaining frame limit, including old/new resize blocks and simultaneous getter-admission scratch. It releases every stack and temporary reference before common comparison, whose Value scratch remains independently bounded. Physical OOM and work/step/frame failure preserve Unit output, original borrowed values and spent charges, while releasing all temporary/frame owners. This common path atomically replaces the old Nullable-specific equality branch. Source retains two constructions, independent ordinary BOOL constants and complete Checker/query publication. Original budgets, safety gates and the precise Value observation gap for deeper variable receiving remain; development qualification is not complete P11.

### 17.51 Bounded receiving and mutation of private Array index paths

After complete Lowered and entire reachable closure admission, CTFE supports INDEX_PLACE chains rooted in authentic private Cells. Each projection carries a type and an already evaluated index; each read or write resolves the current root again, retaining no interior address across operations. The chain must terminate at an admitted Cell in the current compiler Domain. Field paths follow17.53 and private Class object roots follow17.60; module roots, foreign Cells and projected Cell authority gain no admission. Complete family restrictions remain, including explicit refusal of deeper variable Arrays that lack bounded receiving interfaces.

Path counting and construction, every type/header observation, complete getter receiving spans, final public path receiving and the capacity bytes of every possibly copied ancestor prepay the original cumulative ledger before access. Push additionally prepays possible leaf growth. Overflow or unpayable work returns BUDGET first. Path steps consume the original remaining frame quota; observation and final Value scratch receive only the balance after those steps. All temporary observation owners are dropped before common mutation to avoid manufacturing sharing. Sole Value path read/write/push operations determine ordinary semantics, COW and publication; CTFE introduces no second mutation algorithm.

Nested set/push, PLACE_READ/WRITE and the ordinary reserve read/construct/writeback recipe follow this path. Existing ordinary Array method recipes can compose admitted operations, including contains/indexOf, String join, clear/reverse/unshift/pop/shift/resize/fill/concat/entries; each still requires actual opcode, type and whole closure admission. Internal function-value callbacks follow 17.55; numeric formatting follows 17.52. Source retains independent ordinary constant replay. Exported path results still count active logical bytes under the original output ceiling.

Existing snapshots retain ordinary COW semantics. Physical OOM, bounds and work/step/frame/output failure preserve empty output and release every frame and temporary owner without refunding cumulative charges. After Source/Artifact and ledger caller death, the last owned result retains authentic type/Domain lifetimes and final drop allocates nothing. Path development qualification does not replace complete P11, public sanitizer, provider or other original gates.

### 17.52 Common scalar text and original-ledger formatting work

After complete Lowered and entire reachable closure admission, TO_STRING supports bool, integers, Rune and f32/f64. Sole xr_xir_scalar_text produces ordinary decimal integers, true/false, complete Rune UTF-8 and shortest round-trip floating text. CTFE invokes neither Instance output nor another converter. Floating text retains the common integer-only IEEE formatter; signed zeros, subnormals, NaN/inf and exponent notation depend on neither the host FPU nor locale. NUL retains explicit lengths. Ordinary numeric/bool/Rune Array join recipes use this same TO_STRING admission, granting no I/O, identity or external-state authority.

Before each formatter call, bounded metadata observation and algorithm work pay the original cumulative ledger and fixed workspace consumes remaining frame. Integer/bool/Rune reserve 256 work and 512 frame bytes; floating zero/nonfinite values reserve 1024 work and 512 frame bytes. For finite nonzero floats, d is the magnitude of the normal unbiased binary exponent or the lowest possible subnormal exponent. A reserve of 131072+(d+2)*2048 work and 16384 frame bytes covers at most 17 digits/9 subtractions per digit, complete bigint copies and normalization. Cost selection reads width/exponent only and determines no text semantics. A compile assertion bounds the common decimal workspace sizeof to 1024; growing it requires reproving work and frame limits. Insufficient frame or unpayable work fails before entering formatting. Authentic String copies, allocations and owners remain on the original Domain/ledger.

Logical export still counts complete UTF-8 bytes under the original the output ceiling in17.61 ceiling. Longer internal text may participate in comparisons without gaining export permission. Source independently replays ordinary String/Bool constants through complete Checker/query publication. Physical OOM and work/step/frame/output refusal preserve empty output and borrowed arguments, retain spent charges and release every frame/temporary owner. An actual formatted last String remains valid after Source/Artifact/ledger caller death and final drop allocates nothing. Declaration-bound nominals and internal Function values follow 17.53/17.55. Cleanup, foreign receiving and complete mandatory safety qualification remain original P11 responsibilities.

### 17.53 Declaration-bound structs, active enums and private field paths

After sole complete Lowered and entire reachable closure admission, CTFE executes closed ordinary structs/enums. Iterative type-family admission examines all fields and rejects cycles and unadmitted native types; internal Class identities and actual graph receiving follow17.60. Lexical declaration-owner admission follows 17.54 and replaces neither body/effect admission nor permission checks; it denotes no class instance. STRUCT_NEW/GET and ENUM_NEW/TAG/GET use sole Value construction, receiving and owned slots. Ordinary methods and generic specialization retain existing Source/Checker and closure admission. Enums observe only the actual active variant, never inactive payloads.

FIELD_PLACE and INDEX_PLACE may compose only beneath an authentic private Cell in this compiler Domain. STRUCT_SET and ordinary field/Array writes and pushes use the same Value path implementation. Before observation and complete receiving, work prepays the original cumulative ledger, including possibly copied struct field vectors and Array ancestor capacities. Paths, explicit traversal stacks and simultaneous receiving scratch share remaining frame. Observation owners drop before common mutation, preserving normal COW and single publication. Type shapes provide costs, never another Value representation or authority proof.

Owned export recursively counts active target scalars and String bytes, Array elements, Nullable tags and the actual enum storage tag bytes. Empty structs use zero; single-variant empty enums may use zero, while an empty branch of a multi-variant enum still counts its actual tag. Padding, capacity, ownership headers and pointers do not count. An owned result must not first query an inapplicable standalone nominal storage layout and then accept pointer-sized output. Sole TypeArena storage and logical traversal determine export under the output ceiling in17.61.

Source independently rebuilds ordinary STRUCT_NEW/ENUM_NEW recipes from the same immutable input graph. Cross-pool receiving checks complete declaration order/identity, field permissions/names, variant ranges, native fingerprints and closed field types, never local type IDs alone. Metadata and byte comparisons prepay the original ledger; final complete Checker/query retain permission and publication authority. External nominal/Class arguments remain unadmitted; internal Class execution follows17.60. Struct dynamic children and deeper Array receiving that lack bounded observation fail closed without unpaid preliminary scans. External private-owned materialization authority and Unit-field Source prerequisites still need complete qualification; successful internal methods do not establish that coverage.

Physical OOM and work/step/frame/output refusal preserve empty output and borrowed inputs, release every temporary/frame owner and retain spent charges. Actual struct/enum results survive Source, Artifact and ledger caller death, and final drop allocates nothing. Scoped development evidence replaces neither function-value/cleanup, foreign receiving, P12 nor original shared/provider/mandatory safety gates; the complete P11 objective remains open.

### 17.54 Authentic lexical declaration access for compiler helpers

A comptime helper inside an ordinary member inherits the actual lexical nominal owner by the same rule as an ordinary nested function and uses existing MEMBER_HELPER identity. It captures no runtime parameters/local state, grants callers no access to another declaration, and derives no owner from a helper name or result type. Nested comptime inherits the actual enclosing helper's declaration context; existing open-generic and other refusal boundaries remain.

After Checked specialization and complete Lowered receiving, helper lookup matches module, unique name, zero parameters, closed result type, actual nominal owner and member kind, rejecting changed cleanup identity. It must not assume all helpers are ownerless top-level functions. Observation and comparison prepay the original cumulative work; mismatch preserves existing results and releases temporary owners.

A declaration owner is neither a class value nor an effect grant. Pure ordinary class static functions may compute after complete closure admission. Internal temporary Class identities and operations require17.60; Class identity export and I/O/native/external state remain unadmitted. Private field construction/calls remain legal only under the actual declaration owner, including independent Source replay through the ordinary Checker. Receiving a private owned value grants no external private construction access. Complete materialization of such results remains part of the original objective; missing mechanisms must fail closed without reflection/memory-write or runtime fallback bypasses.

### 17.55 Authentic internal Function values and bounded indirect calls

After complete Lowered, identity, target and profile reverification on each request, CTFE admits actual direct CALL and every FUNCTION_REF target as one current reachable closure. An unexecuted callback target cannot hide I/O or other unadmitted operations. FUNCTION_REF, FUNCTION_WEAKEN and CALL_INDIRECT use existing Value Function construction, immutable capture vectors, owned slots and the same bounded activation machine. Ordinary returned functions, higher-order parameters, PHI, internal Tuple/Nullable holders and existing Array map/filter/reduce recipes may compose admitted operations. No Instance, native, host function body or AST interpreter executes.

Each authentic Function owns a private lease registered under this execution's original resource owner. Receiving first matches the owner in the live registry, then checks the original Artifact, actual producer instruction/type, originating FUNCTION_REF entry/captures and current reachable set; it never dereferences arbitrary binding.owner first. Weakening must pass the existing callable weakening rule. A new lease independently retains numeric origin facts rather than borrowing a parent lease. Common Value owns captures; final release only unlinks and frees original resources without allocating. External Function arguments reject before payload access or their admission callback. Functions and results containing Functions cannot be exported or replayed as Source constants.

By-value captures preserve common Value ownership. Mutable captures require freshly derived authentic Cell provenance with OWNED_CAPTURE role, the current compiler Domain, no borrow, no projection and no module storage. Function captures still reject scoped/ref and cleanup roles; internal parameters and static cleanup separately follow 17.56. Before private Cell/path writes, iterative observation of authentic Function captures and Cell snapshots refuses a back-edge to the destination Cell, preserving its prior value. Acyclic sharing and replacement remain legal. This check creates no second Value representation and reads no private object headers. Dynamic struct children, Array<Function> and other families lacking bounded receiving still fail closed.

Type/signature and registry scans, capture vectors, Cell element shapes, complete getter/copy spans and final common Value receiving prepay original cumulative work. Explicit observation stacks and simultaneous receiving scratch consume the original remaining frame quota. All calls share original steps, depth and frame: steps65536, depth128, frame65536, the output ceiling in17.61 and work128M/allocated64MiB/live8MiB remain. OOM, exhaustion, illegal closure or cyclic writes preserve empty output and borrowed values, release every activation/capture/lease and never refund or reset cumulative charges. Internal Function admission grants no external authority. Static cleanup/ref calls follow 17.56; P12 and complete shared/provider/safety qualification remain independently required.

### 17.56 Static cleanup frontiers and authentic compiler Cell loans

After complete Lowered, identity, target and profile reverification, direct CALL, every FUNCTION_REF and CLEANUP_REGISTER target form the current admitted closure. An unexecuted cleanup target cannot hide I/O. CLEANUP_REGISTER activates an existing immutable registration; CLEANUP_LEAVE follows its actual frontier ancestry and calls authentic cleanup functions in LIFO order. A registration retires only after that child returns normally. Nested cleanup, scope exits and loop continue/break follow the same rule. All control-flow edges and parallel PHI still use the common scalar edge kernel, without another expression or Value interpreter.

When RETURN meets an active frontier, common Value first retains the actual return snapshot, then exits toward frontier0. Cleanup mutation of a private Cell cannot retroactively change that captured result. Existing owned slots, COW and Value copy/drop determine lifetimes. Final export retains original target logical sizes; temporary return snapshots must also drop on failure or frame release. Compiler-job abortion by budget or physical OOM publishes no partial result and releases every frame, snapshot, lease and loan through allocation-free terminal paths without refunding spent charges.

Fresh authentic provenance still distinguishes internal Cell parameters: Function captures require OWNED_CAPTURE; ordinary ref calls require SCOPED_REF; cleanup parameters require LEXICAL_CLEANUP, their actual cleanup_owner and an exiting immediate parent frame. Callable ref mode describes storage rather than granting loan authority. Actual Cells must belong to the current compiler Domain, with no module storage. Scoped projections follow 17.57; owned captures still reject projections. External Cell parameters still reject before payload receiving; a cleanup declaration cannot be selected as an independent external CTFE entry.

Each actual compiler activation supplies private identity, stable frame and a finite per-request epoch to the existing CellAuthority. Loan records reserve space for all Cell parameters before entry and count toward original live frame bytes. Existing Cell loan prepare/commit/abort/release own the sole loan semantics. Only immediate live parent authority can reborrow. All arguments prepare successfully and publication work must be payable before commit. Failure aborts the prepared prefix in reverse order; normal or failed pop releases committed loans in reverse order, restoring the exact parent. Cell access and synchronous Array/field paths use existing authorized Value operations. Type IDs, retained references and root flags cannot bypass authority, and a scoped Cell cannot become an owned capture while borrowed.

Parameter types, element shapes, alias checks, frontier lookup/transitions and variable receiving prepay original cumulative work. Terminal loan release work is prepaid; scratch retains the original remaining frame limit. All cleanup/ref calls share original steps/depth/frame and work128M/allocated64MiB/live8MiB/the output ceiling in17.61. Missing declarations still reject explicitly; entry guards cannot dereference an absent table first. Scoped projections follow 17.57. Error cleanup follows17.58 and panic handlers/cleanup follow17.59. Bounded foreign receiving and other unadmitted families still fail closed. Internal ref/cleanup grants no P12, shared/provider or complete safety qualification.

### 17.57 Bounded scoped projections and authentic loans on current roots

After complete Lowered and entire direct/FUNCTION_REF/cleanup closure admission, CELL_PROJECT uses the existing Value factory to construct an authentic descriptor from INDEX_PLACE/FIELD_PLACE chains on private Cells in the current compiler Domain. The descriptor owns evaluated selectors and canonical storage, never an interior address or leaf snapshot. Receiver and all arguments evaluate before authentic loan_prepare_path rechecks the current root, complete path and parent scope. All preparations and publication work must succeed before commit. Existing authorized path read/write/push provide access; an empty suffix denotes the current loan scope itself. Reprojection appends selectors and only authentic parent authority can reborrow. Existing provenance roles, alias refusal, exact parent restoration and static cleanup rules remain; no external Cell, module/object root or owned-capture authority is granted.

Only a closure actually containing CELL_PROJECT scans reachable Cell creation sites for Array/struct element shapes. Metadata operations prepay128 times the maximum shape on the original ledger. Before the first projected loan prepares, parent authority can still legally read the complete canonical root. Its actual bounded receiving prepayment and type shape become costs in that loan record; the temporary root snapshot drops before loan preparation. Descendant reborrows inherit costs only from their current parent's loan. Numeric bounds grant no root access or wider scope; authentic Value loans independently revalidate authority. Currently admitted dynamic roots are Arrays with statically bounded element receiving spans, whose length cannot change through a proper descendant loan. Other admitted roots have static receiving spans. Deeper variable families lacking complete bounded observation remain refused; private headers are never probed without payment.

Each projected access prepays root receiving work times4*(root shape+1), covering common path prefixes, suffixes and complete receiving. Before mutation, authentic Domain live statistics supply an additional live_bytes*root shape*32 reserve for possible ancestor COW and retention on one acyclic path. Array leaf growth separately pays actual capacity and element storage. All products check overflow and unpayable reserves return BUDGET before Value access or mutation. Cumulative charges never refund or reset. Observation snapshots drop before mutation, preserving existing COW copies. An owned mutated result can outlive Source/Artifact/ledger callers and its final drop allocates nothing.

Temporary selectors, common receiving scratch and all simultaneous activations share the original frame limit. After allocating a child, argument receiving and loan preparation receive only the balance after subtracting that child's bytes. Common Value scratch exhaustion may retain its existing LIMIT-to-BUDGET mapping; explicit frame checks return FRAME_LIMIT. Both preserve empty output and release every prepared prefix, without widening limits or reporting success. Original steps/depth/output,128M/64MiB/8MiB and65536 frame remain unchanged. This family changes no public Value/Call/Instance/SourceContext layout or schema/opcode. Error cleanup follows17.58 and panic handlers/cleanup follow17.59. Ordinary Source ref lambdas, foreign/deep-variable receiving, P12 and complete shared/provider/safety qualification remain independent responsibilities.

### 17.58 Internal Error values, explicit continuations and bounded cleanup

After complete Lowered/identity/target/profile reverification on each request, CTFE includes INVOKE targets in the actual closure alongside direct CALL, FUNCTION_REF and cleanup targets. INVOKE_INDIRECT still calls only a target validated through this execution's authentic Function lease. Unexecuted error branches or cleanup cannot hide I/O, suspension or other unadmitted operations. ERROR_ERASE/IS/NARROW use the common Value's authentic enum views, exact declaration identity and existing permissions, without another Error wrapper, Value representation or interpreter. Within admitted families, Error may occur in internal parameters/returns, Cells, owned locals, PHI and Tuple/Nullable/enum holders. External Error arguments and Error-bearing constant exports remain refused; erasure grants no additional type, declaration or execution authority.

Complete shapes of actual reachable enum factories join the Cell root shapes required for projections to determine a128-times shallow metadata prepayment bound. Opaque Error pays this original-ledger cost before public error_borrow reveals its concrete enum view. Complete bounded receiving separately pays actual active fields, String/Array spans, Function captures and Cells. Tuple getters and Nullable unwrapping returning Error also prepay opaque metadata. Internal Error marks variable receiving rather than bypassing other family restrictions. Array<Error>, dynamic struct children and deeper variable families lacking bounded observation remain refused. Before private Cell writes, existing observation still follows Function/Cell graphs inside Error and rejects a back-edge to the destination Cell. Acyclic holders and replacement remain legal; Error cannot hide a cycle.

Successful INVOKE follows its actual normal edge to the dedicated INVOKE_RESULT or INVOKE_DISCARD; a Unit continuation fabricates no value slot. A language error first retains its authentic concrete enum snapshot and runs that frame's frontier cleanup in LIFO order. A registration retires only after its cleanup child returns normally. Propagation through ordinary CALL ends the child, releases its loans in reverse order and restores authentic parent authority before transferring the retained error to that parent's exit. INVOKE receiving instead takes its dedicated INVOKE_ERROR edge and retains the common Error view. The shared scalar edge still selects actual predecessors and parallel PHI; markers must bind the actual callsite. Local CLEANUP_ERROR retains its evaluated error snapshot while leaving the selected scope, then follows the original error continuation. Cleanup mutation of a Cell cannot retroactively change thrown String/Array values.

Cleanup bodies may contain fully handled try/catch and calls, while their definitions still satisfy authentic Source/Checker restrictions on escaping throw and suspension. NO_THROW and root flags do not prove determinism. Compiler budget/OOM failures never enter language catch: they preserve empty output and release all frames, pending errors, return snapshots, leases and loans on allocation-free terminal paths without refunds. An uncaught language error returns compiler-only UNCAUGHT_ERROR after applicable cleanup completes, and Source reports a compile error at the original comptime site. Infinite cleanup remains bounded by original step/work/frame/depth limits and cannot prematurely publish an uncaught error.

Metadata, receiving, retention, continuation and cleanup transitions share original cumulative128M work/64MiB allocated/8MiB live. Steps65536, depth128, frame65536 and the output ceiling in17.61 remain; error slots and simultaneous child/receiving scratch count toward the same frame. Error grants no external snapshots, Error constant exports or host execution. Panic handlers/cleanup follow17.59. Ordinary Source ref lambdas, external private materialization/Unit prerequisites, bounded foreign/deep-variable receiving, P12 and complete shared/provider/formal safety qualification remain independent responsibilities. This family changes no public Value/Call/Instance/SourceContext layout, schema or opcode.

### 17.59 Bounded panic handlers, authentic PanicInfo and cleanup propagation

A fully admitted Lowered closure may contain protected blocks, PANIC_CATCH/CODE/MESSAGE, ASSERT_CONDITION and MATCH_FAIL. All handlers, cleanup and Function targets still undergo complete admission on each request; unexecuted branches cannot hide I/O or suspension. Divide/remainder zero, numeric range, null unwrap, Array/projected-path bounds, assertion and incomplete match use existing common numeric/Value panic semantics. Only language faults enter this channel. Original ledger exhaustion, OOM, step/depth/frame/output limits, invalid artifacts and authority refusal never become catchable panic; assertPanics cannot consume these failures as success.

Each compiler activation independently owns its pending panic payload and counts that carrier toward the original frame limit. Pending returns, Error and panic retain their own actual ownership. A bounds primitive's code/index/length are captured at the actual failure, without reevaluating an index or querying a length changed by cleanup. Assertions retain the evaluated String snapshot with exact bytes/length, including embedded NUL; later Cell writes cannot retroactively change the message. PanicInfo uses the common immutable Value factory. CODE/MESSAGE use public getters and the sole canonical message formatter, without private object headers, another Value representation or host exception objects. Fixed metadata/work and formatting scratch prepay the original ledger/frame. Internal PanicInfo contributes32 work units to composite receiving shape; that cost bound proves no external authority.

A same-frame fault selects the handler of its actual faulting instruction's block, runs crossed frontier cleanup in LIFO order, then enters the dedicated PANIC_CATCH marker without fabricating an ordinary CFG predecessor. Cross-frame propagation completes child cleanup, releases authentic loans, restores immediate parent authority and remaining frame, then consults the protected block at the original actual callsite. An INVOKE Error edge cannot consume panic, and a panic handler does not catch enum Error. Ordinary success, Error and panic retain distinct continuations. Nested handlers and a panic raised inside a handler use their current protection relationship rather than reusing an old landing.

Cleanup may fully handle its own panic while an outer return/Error/panic is pending. Per-frame payloads prevent inner handling from replacing the outer message being unwound. If panic escapes cleanup, CTFE releases every owned value and loan on allocation-free terminal paths and returns compiler-only CLEANUP_PANIC, reported at the original comptime site. It never invokes a runtime fatal/process-abort interface or delivers that escaping panic to an outer language handler. This compiler boundary does not change the runtime fatal rule for escaping cleanup panic. Ordinary uncaught panic reports its corresponding compiler failure after applicable cleanup; infinite cleanup hits the original step/work limit before uncaught publication.

Authentic PanicInfo may occur in internal parameters/returns, Cells, PHI, Tuples, Nullable, Arrays, structs/enums and Function captures within admitted families, using common ownership/receiving. It is not Sendable. Authentic external PanicInfo arguments and PanicInfo-bearing constant exports remain refused. Ordinary independent Source recipes receive only admitted exportable results and do not replay PanicInfo. Original128M work/64MiB allocated/8MiB live, steps65536, depth128, frame65536 and the output ceiling in17.61 remain. Failure preserves empty output, never refunds/resets charges, and final owned release allocates nothing. External private materialization/Unit prerequisites, bounded foreign/deep-variable receiving, ordinary Source ref lambdas, P12 and complete shared/provider/formal safety qualification remain independent. This family only adds compiler-only failure and private execution state; public Value/Call/Instance/SourceContext layouts, schema and opcodes remain unchanged.

### 17.60 Private temporary Class identities and bounded actual field graphs

After complete Lowered/identity/target/profile and entire reachable closure admission on each request, CLASS_NEW/GET/SET use existing Class Value factories and get/set, retaining authentic Checked declaration, construction and field permissions. Only reachable factories in this compiler Domain create objects; no Instance, host callback or second Value representation executes. Internal Class may flow through ordinary parameters/returns, owned locals, Cells, PHI, Tuple, Nullable, active enum/Error and Function captures within each family's restrictions. Authentic external Class arguments and Class-bearing constant exports reject; identity handles are not constant bytes.

Type costs count fixed Class header validation as32 units without recursively guessing a possibly recursive declaration graph. Actual receiving visits authentic fields through an explicit owning stack on the original ledger. It uses only shallow public class_get after prepaying root/field metadata, actual Array receiving spans and simultaneous scratch. It reads no private object headers and performs no unpaid whole-receiver scan. Before each write, observation visits the complete current receiver and incoming graph, rejecting a backedge to the actual destination Class. Handle equality only rejects backedges; Value APIs still authenticate actual Domain/arena, storage and loans. Acyclic sharing, alias-visible field changes and replacement retain ordinary identity semantics; a thrown Class identity is not an immutable snapshot of its fields.

OBJECT_PLACE denotes the complete Class root; FIELD_PLACE adds the field selector. Paths with evaluated selectors prepay receiving and possible ancestor copies, then prepare/commit a short whole-object loan under real frame authority and use common authorized path operations. Every terminal releases the loan without allocation. Paths retain no interior address. A path crossing another Class identity beyond its first root currently rejects; a legal route must instead start at the actual child object. Ref projections still use the sole Cell projection factory and authentic parent/child loans. Initial projection loans on Class roots require all fields to be exportable data types and the leaf receiving shape to be static. Thus another mutable identity or variable leaf graph cannot invalidate the prepaid root bound during a narrow loan. Scalar/String and supported pure Array chains allow borrowing, reborrowing and cleanup writes; scalar Array leaves may grow. Class-bearing roots and dynamic leaf combinations remain refused without broadening prepayment quotas.

Field getters return real owners. A returned String snapshot survives subsequent Class field replacement and Source/Artifact/ledger caller death. Ordinary returns, Error and panic retain LIFO cleanup, authentic parent loan restoration and resource-failure rules from17.56–17.59. Complete physical FI includes every observed allocation point in construction, field graph receiving, projection/cleanup, owning String and cycle refusal, with retry on the same original owner and empty output/live restoration. Traversal stacks, loans, paths and common scratch share the original frame. Retrying work exhaustion cannot reset charges.

Original128M work,64MiB cumulative allocation,8MiB live,65536 steps/frame,128 depth remain unchanged; maximum logical output follows17.61. This family changes no Value/Call/Instance/SourceContext ABI, schema or opcode. Array<Class>, dynamic struct children, foreign/deep-variable receiving lacking bounded observation, external private Source materialization, the original larger P12 output targets and complete shared/provider/formal safety qualifications retain independent open work. Private Class qualification establishes neither complete P11 nor a production release.

### 17.61 Versioned8MiB logical output policy with original budgets

The compiler-private output policy version is2 and XR_XIR_CTFE_MAX_OUTPUT_BYTES is8388608 bytes (8MiB). Leaf execution, finite calls and independent Source receiving share this ceiling and the sole existing evaluator. No old-ceiling switch, second entry, chunking or truncation remains. Callers may still request smaller logical limits such as0,7 or8; an actual result exceeding that request returns OUTPUT_LIMIT. Requests above8388608 return BAD_ARGUMENT before evaluation or ledger consumption, preserving canonical empty output. A request equal to the hard ceiling is valid but does not promise that other original limits can materialize a result of that size.

Logical size still follows authentic active target values: target scalar storage widths, complete String UTF-8 bytes, ordered Tuple fields, actual active Nullable/enum tags and payloads, and each active Array element; Unit contributes zero. Pointers, host carriers, alignment padding, capacity, object headers and inactive fields do not count. The ten-element Fibonacci Array<i8> [0,1,1,2,3,5,8,13,21,34] occupies10 logical bytes and the (i64,i64) pair (21,21) occupies16. Explicit8-byte requests still reject both; exact10/16 requests succeed and9/15 reject. Nullable<i64>, (i64,bool) and a two-variant enum with one active i64 payload occupy9 logical bytes despite storage alignment. Existing9-byte Strings and11-byte boolean joins receive their complete values under the new policy.

The same original compiler ledger retains128000000 work,64MiB cumulative allocation,8MiB live,65536 steps/frame and128 depth. A larger logical ceiling increases, clones, resets or refunds none of these quotas. Output traversal, receiving, independent ordinary Source recipes, complete Checker/query and every failed prefix still pay the original owner; temporary stacks and common scratch share remaining frame. Work/heap/frame may exhaust before the logical ceiling, preserving the corresponding original refusal without inventing success or OUTPUT_LIMIT. Resource failures never enter language catch and same-owner retries retain cumulative consumption.

Results retain authentic TypeArena, Domain and nested field ownership. Complete vectors survive Source/Artifact/ledger caller death and final drop allocates nothing while restoring live bytes. Source independently rebuilds complete ordinary Array/Tuple/String recipes from the same immutable graph, preserving actual capacity, types and byte lengths; no CTFE pointer enters Checker storage. Version2 grants no Class/Function/Error/PanicInfo identity exports, foreign cross-arena receiving, private construction permission or new effects. Complete closure, target/profile/Checked digest and existing admission still reverify. Value/Call/Instance ABI, schema and opcodes remain unchanged.

Policy version2 and its hard ceiling belong to compiler artifact and future persistent CTFE cache identity; a public signature or Checked function digest alone is insufficient. Any persistent result key must also bind complete private dependencies/admission/compiler and XIR identities, target profile, authority, every requested limit and original budget policy. Success at a larger budget cannot become a free smaller-budget hit and old policy results cannot override current refusals. The current evaluator and Source path have no persistent result cache: each request fully verifies, executes and pays; internal type-cost caches live only within one execution. This policy identifier claims no implemented cache system. Local Windows evidence establishes neither complete P11, full provider/sanitizer qualification nor production release.

### 17.62 Per-request Source logical result limits

The compiler-private Source request accepts optional XrXirSourceComptimeLimits.output_bytes, borrowed only during synchronous checking. A NULL policy selects the8388608 maximum from17.61; explicit0 admits only logically empty comptime results and does not select a default. The numeric limit is copied into SourceContext before discovery/construction and retained by the independent second Source construction. Every comptime site forwards this bound to the sole existing CTFE request without changing the evaluator, Value representation, logical measurement or any other original quota.

A Source limit above8388608 returns BAD_STRUCTURE before evaluation or additional ledger charges and publishes no result. Occupied Checked/snapshot output is preserved too. An8-byte Source request rejects the original9-byte Array, ten-element Fibonacci and16-byte pair with BUDGET, an actual location and no Checked/snapshot. Exact9/10/16 requests receive complete independent recipes through ordinary Checker/packet/VM value checks; minus-one still rejects. Unit and empty String accept0; an ordinary runtime result is not subject to this comptime output cap when no comptime evaluation occurs.

A suitable limit may retry on the same original owner. Every failed prefix, retry, description and independent receiving still accumulates original allocated/work charges without refunds or reset. This policy grants no field access, construction authority or unchecked generated declarations, and changes no complete FI,600/60/original normal timeout or safety/provider gate. Published results own their data after Source/authority/policy/session producer death. The current evaluator has no persistent CTFE result cache; a larger-limit success cannot grant a free smaller-request success.

### 17.63 Owned generated Built and the ordinary complete second check

Generated receiving accepts only a Built module, an actual construction owner and explicit per-function/per-instruction user entry, module, file and field-path origins. It copies function names, parameters, blocks, instructions, operands, canonical types, declarations, generics, defaults, construction and every origin byte onto the original compiler ledger, producing an independent unchecked owner. Physical extents and metadata admission do not check function bodies. A generator's Checked tag, effect summary, query result or Sendable fact cannot substitute this input or exempt later checking.

Every request for Checked output invokes the existing ordinary complete Built checker anew, including current types, constraints, visibility, CFG/SSA, ownership, borrow escape, actual calls and captures, cleanup, effects and root bounds. Only success publishes an independent Checked owner. Generated type mismatch, borrowed-carrier escape and no_suspend effect escalation reject. Body diagnostics map to the owning user origin of the actual instruction; declaration failures use the function entry, and global failures use the program entry or a library's first function entry. Diagnostics borrow origins from the generated owner and must be copied before it is freed. Origins grant no private read, write or construction access.

Generating input and construction may die after receiving; the generated owner may die after Checked publication. Subsequent packet, Lowered and execution consumers cannot borrow that storage. Failure preserves empty output. Retrying on the same owner repeats complete checking and accumulates the original charges without refunds or reset. This physical owner adds no language macro, arbitrary host generator or second type/execution semantics. Typed Source grants, ordinary library derivation wiring and their complete resource/safety qualification remain separate obligations.

### 17.64 Definition policies, exact generated bindings and ordinary operation rechecking

An internal derivation policy exists before generator execution. It independently owns the target's open canonical type expression, authentic defining module, ordinary protocol application and ordered arguments, actual generator entry, explicit field DESCRIBE/READ/WRITE set, optional sole legal initializer and user origin. Description, read, write and construction are checked separately. WRITE cannot grant an immutable field; encoding grants no construction right. The generator must be an ordinarily visible function, never a derived function, member or cleanup. Policies may precede all generated functions; publishing empty bodies cannot resolve generation order.

A generated function binding only references the original Source policy and records its current function, exact canonical target, original Source row and generated root, and owned per-instruction user origins. Ordinary type services verify defining module, target declaration and protocol constraints. nominal_owner cannot widen a policy. Generated implementations still pass the original implementation/witness checker: protocol and ordered arguments must equal the policy, and every root must actually bind its corresponding requirement. Manual implementations cannot reuse derived functions for additional protocols. Derived functions cannot take test, module-initializer or constructor roles; cleanup and its owner must share policy, root and target.

Actual field operations use the current canonical instance, not only a declaration ordinal. Swapped generic arguments cannot match a grant. Each target member operation must prove its permission. Derived functions cannot construct the target by layout, call arbitrary private functions through same-module identity or access module storage. Explicit initializer authority applies only to authentic CALL/INVOKE, the corresponding original declaration and exact target result. Ordinary requirement/default bindings still independently prove their origins; a derivation marker never exempts visibility, borrowing, effects or root rechecking.

Specialized bindings retain original Source row/root/policy, using authentic Source provenance to verify ordered type substitution and every origin byte. They cannot lose a binding or carry replacement runtime policies. Lowered remaps the current target; CGen and Program independently compare complete bindings and origins. Checked wire29/semantic74 traverses actual function binding, implementation policy and complete policy/generated-origin data; old28/73 packets reject. The added function index and owned declaration table require Program ABI31; Value ABI24 and Call ABI29 are unchanged. SDK identity includes459 independently observed layout facts. Version labels never substitute complete receiving.

This boundary implements internal common receiving and operation constraints. It does not claim Source TypeInfo, ordinary generators, JSON or other consumers are wired. Generated inputs still enter the ordinary complete second check from Built under17.63. All original compiler budgets, physical OOM, independent owners and complete FI/safety/provider gates remain.

### 17.65 Owning static type descriptions as ordinary data

An unshadowed `typeInfo<T>()` names core intrinsic7 and requires one explicit closed static type with zero value arguments. Source materializes ordinary Tuple, Array and String recipes through the sole canonical type/field description service. It adds no runtime reflection opcode, Value kind or evaluator. Open parameters and malformed arguments reject; ordinary same-name functions retain lexical binding. Closed generic fields use existing Source substitution, never an undeclared generic member capability.

The result is `(root,nodes,fields,arguments,parameters,variants)`, a six-field Tuple whose final five fields are ordinary Arrays. Root is0; i64 type edges are local graph indices rather than cross-pool XIR type IDs. Each canonical type occurs once. Recursive class identity graphs use back-edges; invalid recursive value layouts still reject under ordinary type rules. Only reachable facts appear, without host addresses or physical paths.

| Row | Ordered fields |
|---|---|
| fields | `(string name,i64 type,i64 flags,i64 ordinal)` |
| parameters | `(i64 type,i64 mode)` |
| variants | `(string name,i64 fieldBegin,i64 fieldCount)` |
| nodes | `(i64 kind,i64 builtin,string module,string name,i64 nominalKind,i64 flags,i64 fieldBegin,i64 fieldCount,i64 argumentBegin,i64 argumentCount,i64 element,i64 parameterBegin,i64 parameterCount,i64 result,i64 variantBegin,i64 variantCount)` |

Each begin/count names an exact span in its sole flat table; variants share the fields table. Absent element/result is-1. Kind follows the canonical service, with0 for a builtin whose stable ID occupies the builtin slot; constructed types use-1 there. Field ordinals retain declaration order across visibility filtering. A defining module may describe its private fields; other modules receive only ordinarily visible fields and their reachable types. Module is a relocatable logical declaration identity. Flags, parameter modes and visible descriptions grant no read, write, call or construction authority.

```xray @id=type-info-static-array
export fn run()->i64 {
    return comptime {
        const info=typeInfo<Array<i64>>()
        info.1[info.0].0
    }
}
```

This returns Array kind2. Ordinary functions may receive complete facts through structurally identical Tuple aliases; a source-free Checked library and its private helper can process subsequently defined user types. A complete TypeInfo may also become a comptime result, replayed through independent Source construction as ordinary owned data and read by a normal Program/Instance after precursor death. Access, substitution, graph traversal, materialization and receiving charge the original ledger, including17.61 request-specific output ceilings and every other original budget. Facts do not constitute generation: Source authorization and generated-declaration startup, complete JSON/Regex consumers and full FI/safety qualification remain separate obligations. Neither these data nor a generator's prior Checked state grants a checking exemption.

### 17.66 Source startup and receiving for explicit ordinary library derivation

A named struct, class or enum selects an ordinary protocol application and visible ordinary function with `@derive(Protocol, generator, describe: ["field"], read: ["field"], write: ["field"], construct: true)`. The generator is an imported name or `module.name`. Options are optional; construct defaults to false. Field names must exist, and each operation is independent: READ does not imply DESCRIBE, and encoding does not imply construction. Duplicate options, field operations or protocol applications on one target reject. Each unqualified field name must resolve to exactly one actual target field. Equal payload names in different enum cases reject as ambiguous before g executes, rather than selecting the last field or granting all of them. Old fixed-bit syntax such as `@derive(JSON)` is removed; there is no compiler-specific protocol whitelist or implicit Hash-to-Eq grant.

After authentic declarations, fields and signatures exist, Source creates and independently validates the definition policy before generator execution and ordinary user-body construction. The current closed-target family obtains complete g/private-helper bodies from a Checked dependency and works after its source has been removed. Actual dependency bodies and a real result thunk pass ordinary Built checking, specialization, Checked identity, Lowered and the sole CTFE. The i64 Program entry actually calls the thunk and reads its result version. Unchecked user bodies never enter a fake Checked module, and empty bodies do not resolve generation order.

The ordinary generator signature is `fn(TypeInfo, DeriveContext)->Generated`. TypeInfo uses17.65's logical graph. In this context, private target fields appear only under explicit DESCRIBE; nested types do not inherit all private visibility of the target module. Fact queries and g recipes may use different canonical pools while retaining the original ledger; only logical references cross pools.

DeriveContext is `(Array<MethodInfo>,Array<CalleeInfo>,Array<i64> parameters,i64 constructor)`. MethodInfo is `(string name,i64 resultType,i64 parameterBegin,i64 parameterCount,i64 ownParameterCount)`. CalleeInfo is `(string module,string name,i64 index,i64 parameterBegin,i64 parameterCount,i64 resultType)`. Parameter spans address the single flat table, and type references address the same TypeInfo graph. Callee indices identify receiver-selected visible public ordinary functions and the explicitly authorized sole init; absent construction authority yields constructor=-1. Private helpers used during CTFE do not thereby become private runtime call permissions for generated bodies.

Generated is the version1 eight-field ordinary Tuple `(version,functions,parameters,blocks,instructions,operands,literals,typeArguments)`, with seven flat Array tables. FunctionRow14 is `(name,member,root,resultType,parameterBegin,parameterCount,blockBegin,blockCount,instructionBegin,instructionCount,operandBegin,operandCount,typeArgumentBegin,typeArgumentCount)`. BlockRow4 is `first/count/panic/frontier`; InstructionRow10 is `opcode/type/arg0/arg1/target0/target1/immediate/typeArgumentBegin/typeArgumentCount/originField`. Ordered spans exactly partition their tables with no unused tail. Nonnegative types resolve in the receiver's current graph; negative `-builtin-1` references must identify an admitted builtin. Nonnegative calls index newly generated functions; negative `-calleeIndex-1` calls use the receiver's callee table. CONST_STRING indexes only newly returned literals.

The receiver forces the defining module, target, export, ordinary READ-method/helper role, authentic requirement/root, explicit policy and owned instruction origins. member=-1 identifies a helper; each other selector names an actual requirement, and its root must bind that method. New bodies inherit no Checked/NONE proof from g. The complete user Built independently rechecks witnesses, types, constraints, visibility, borrowing, ownership, initialization, construction and root effects. Failure publishes neither Checked nor query output; field errors identify the user's field path. Enum payload origins use the authentic `Type.Case.field` path and that field name's source location. Independent second Source receiving binds the original occurrence, attribute and actual library/g identity, reconstructing logical references in its own pool instead of copying integer IDs from the first pool.

DERIVED_METHOD query facts select the actual derive token without invented lexical method or parameter names. LSP shows ordinary method calls, navigates definitions to their triggering attribute, and serves references/hover/semantic tokens from owned snapshots. This closed Checked-dependency family does not claim complete generic templates, same-source dependency startup, JSON/enum/Regex consumers or all FI/safety/provider qualification. Those obligations retain the same boundaries. The original8MiB request-specific logical output policy and every other budget and verification gate remain.

<!-- /xr-spec:en -->

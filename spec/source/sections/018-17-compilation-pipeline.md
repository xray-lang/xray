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

`stdlib/types/array.xr`的`struct Array<T>`是此声明族的唯一源码权威。编译器、成员适配器和API清单消费同源结构化声明；当前32个成员中，冻结READ `get(index: i64) -> T`、REF `set(index: i64, value: T) -> ()`及REF `push(value: T) -> ()`三原语，以及map/filter/reduce/forEach/find/findIndex/every/some/contains/indexOf/join/clear十二个声明recipe及reverse/unshift/pop/shift，共19个operation。其余13个成员保留清单身份，不因此获得执行资格。Source先查找准确public成员声明，再按已准入native operation/recipe identity选择实现，query记录原声明身份与CALL引用；不按名字另立执行表。回调、查询、清空的类型、求值、效应与所有权合同见§14.7。索引读写使用同一操作；全局`len`独立按core intrinsic身份解析，本子集只接受Array并返回i64。词法、模块及import解析先于prelude；用户定义的同名`len`仍是普通函数。

源码准入显式`Array<T>`、同类型元素字面量、有Array上下文的空字面量、普通赋值复制、参数、返回、局部及入口模块状态。空字面量没有元素上下文时拒绝。元素从左到右求值，使用既有上下文类型规则；嵌套Array与函数元素遵循统一构造类型池的可复制、可存储准入，unit、内部CELL、视图和未准入的不可复制资源不能成为普通元素。`Array<T>`泛型体在定义处按约束检查，在Checked上特化后复验；普通实例在Program封存前完成，不能按具体实参补造定义处未证明的能力。其他容器与用户struct/class声明族仍须分别准入。

Array复制保留独立逻辑值，无需显式`copy`或`move`。共享backing在写入前取得独立存储，独占且容量充足时可原地修改；紧凑元素存储使用唯一类型布局与copy/drop规则。const与READ参数不能成为可写接收者；把其可复制值保存到普通局部var后，可修改该副本。backing是否独占不授予写权限。已有多模块可见性与库模块可变状态限制继续生效。

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

有限库导入沿§17.30的同一Catalog、reader和Source remapper扩展，不能建立第二链接或运行路径。唯一构造接口为xr_xir_library_catalog_new(inputs,size_t count,budget,output)，替换旧单输入签名；唯一资源视图为xr_xir_library_catalog_resources(catalog,size_t *count)，删除旧单资源接口。构造要求output、非NULL inputs及count>0，失败output=NULL；没有库由SourceRequest的NULL catalog表示，不分配空拥有对象。视图要求非NULL count，先写0；NULL catalog返回NULL，成功借用真实连续只读数组至catalog_free。count=NULL返回NULL。Catalog独立拥有每个Checked artifact和全部身份字符串，不借输入包、输入结构或调用方文本；资源数组和artifact owner数组分别零初始化并按真实存储收费，任何部分前缀失败均完整销毁，全部成功后只发布一次。

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
effects, diagnostic provenance, package linking, native-cache pairing, installer
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

The `struct Array<T>` declaration in `stdlib/types/array.xr` is this family's sole source authority. The compiler, member adapters and API inventory consume the same structured declaration. Of its current 32 members, the frozen boundary contains the three READ `get(index: i64) -> T`, REF `set(index: i64, value: T) -> ()` and REF `push(value: T) -> ()` primitives plus twelve declaration recipes: map/filter/reduce/forEach/find/findIndex/every/some/contains/indexOf/join/clear. Together with reverse/unshift/pop/shift these are 19 operations; the other 13 retain inventory identities without execution authority. Source finds the exact public member declaration before selecting implementation by its admitted native operation/recipe identity; queries record the declaration identity and CALL reference without a separate name-based execution table. See §14.7 for callback/query/clear typing, evaluation, effects and ownership. Index reads/writes use the same operations. Global `len` resolves separately by its core intrinsic identity; this subset accepts only Array and returns i64. Lexical, module and import resolution precede the prelude, so a user function named `len` remains an ordinary function.

Source admits explicit `Array<T>`, homogeneous literals, empty literals with an Array context, ordinary assignment copies, parameters, returns, locals and entry-module state. An empty literal without an element context rejects. Elements evaluate left to right under ordinary contextual typing. Nested arrays and function elements use the unified constructed pool's copyable/storable admission; unit, internal CELL, views and unadmitted noncopyable resources are not ordinary elements. Generic bodies using `Array<T>` are checked against their declared constraints, specialized on Checked, then reverified. Ordinary instances finish before Program sealing; concrete arguments cannot invent capabilities missing at definition checking. Other container and user struct/class families require separate admission.

Array copies preserve independent logical values without explicit `copy` or `move`. Shared backing detaches before mutation; unique storage with enough capacity may mutate in place. Compact elements use the sole type layout and copy/drop rules. Const and READ parameters cannot supply writable receivers, but saving their copyable values into an ordinary local var permits mutation of that copy. Backing uniqueness grants no write authority. Existing cross-module visibility and restrictions on mutable library-module state remain in force.

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

The sole current protocols follow implementation constants in §17.6. Real seal/read consumers reject old value targets, valid-digest old Checked packets and old embedded proofs in the same Program ABI, without an old GET alias or second reader. Independent VM/native/packet/mixed expectations, authentic old caches, each OOM point, owned-result lifetimes, physical release and full applicable safety are separate verification obligations. This section does not claim complete language, same-source stdlib pair publication, default product or cross-platform qualification.

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

Extend the same Catalog, reader and Source remapper from §17.30 without a second linker or execution path. The sole constructor is xr_xir_library_catalog_new(inputs,size_t count,budget,output), replacing the old single-input signature. The sole resource view is xr_xir_library_catalog_resources(catalog,size_t *count), removing the singular accessor. Construction requires output, non-NULL inputs and count>0; failures clear output. A NULL SourceRequest catalog represents absence; no owned empty catalog. The view requires count, clears it first, returns NULL for NULL catalog, otherwise borrows the actual contiguous immutable bindings until catalog_free. NULL count returns NULL. Each independently owned Checked artifact and all identity text survive input packet/descriptor/text destruction. Separately zero-initialized binding and artifact-owner arrays are charged for actual storage, safely destroy any partial prefix, and publish only after every input succeeds.

Each input remains one independent dependency-free SCRIPT Library with a leaf logical path. Actual authority, complete packet SHA, existing canonical identity algorithm and declaration identity must agree. Every duplicate canonical rejects BAD_STRUCTURE, regardless of equal/different content, authority or physical root; no deduplication, first/last winner or new (canonical,root) key. Distinct canonicals may share an authority; actual resolution still checks exact authority and never falls back to AST for a matched bad binding. Multiple inputs do not imply inter-Library dependencies, package authentication, generic or general declaration linking.

Create one actual remaining budget. Check count storage multiplication/addition, metadata capacity and count<=remaining.work before dereferencing any input. Container, both arrays, actual text, every hash, read_remaining, shape and duplicate check cumulatively consume that balance without per-input resets or duplicate owned-reader charges. Charge at least one work unit for each input visit and duplicate candidate comparison, plus the bytes actually compared. A bounded pairwise scan is allowed; an uncharged quadratic loop is not. Refund temporary scratch on every exit, never cumulative work/metadata. Publish all inputs atomically, never a partial Catalog.

Finite Catalog admission adds STRING alongside existing i64 formals and Unit/i64 results, and only existing CONST_STRING/CONCAT_STRING to CONST_INT/ADD_INT/CALL/CALL_DEFAULT/RETURN. Unit formals, constructed types, generics, slots, implementations, provenance and dependencies remain unconnected; this consumer does not incidentally admit INVOKE_DEFAULT or error CFG and does not restrict the complete legal language. The ordinary Checked verifier checks every definition, unused literal and private/default helper; ordinary and default call permissions remain §17.31. Existing literal-length wire, STRING operations and runtime semantics add no proof authority or runtime payload, so retain sole Checked21/53 and Program25/Value15/Call19. Any discovered actual semantic/layout change requires renewed analysis and a freeze before implementation.

One no-emission source_literal_append(ctx,node,bytes,length,&id) manages the existing Source literal table. Local AST and already owned enum bytes retain established borrowed lifetimes; source_string_literal appends then emits the original CONST_STRING recipe. Reject invalid lengths/counts, nonempty NULL and growth overflow before charging legal append work. Charge one per legal append and the actual old element count when moving a vector; retain initial capacity16 and doubling. Failure publishes neither count nor ID. For imported nonempty literals, own exact length in SourceMemory and charge length copy work before append. Empty literals use NULL/0 without payload allocation. Never use strlen, terminator framing or content deduplication. Charge full headers/payloads through the true §17.30 balance, without an allocated shadow counter or early-release metadata refunds. Query has no runtime literal table or new authority API.

The sole SourceLibraryMap records literal_begin/count for a contiguous original-order range. Check CONST_STRING's nonnegative original ID against its own Library count before overflow-checked base addition; a valid merged index cannot rescue an invalid original definition. CONCAT_STRING still uses SSA operands and CALL_DEFAULT uses the existing function/default map. Refund the original function scratch map on all exits and commit next_function only after copying the complete Library. Never publish a partial Checked builder. Ordinary semantic-failure query snapshots retain the existing complete contract rather than requiring NULL for every failure.

Qualification includes two real Libraries each owning literal0 with different bytes, reversed Catalog order with fixed results, omitted/explicit STRING arguments, private-helper rejection, the original OwnedChecked executed after all producers die, Source/independent packet VM/actual native/mixed, two Instances, retained owned bytes read by length after Program destruction, and final physical zero. Empty, UTF8 and low-level three-byte a\0b literals must survive all four consumers; Source parser NUL rejection remains unchanged. Independent manual packets and correctly rehashed attacks cover original-ID-outside-source-but-inside-merged, unused invalid UTF8, length/count overflow, identities/authority/digests and every duplicate canonical variant. Fail every new actual allocation, release the first Library when the second fails, test cumulative exact/minus1 budgets and scratch refunds. Preserve all existing default, origin, budget and fault gates. This finite extension does not qualify mutable Library state isolation, stdlib paired-cache hits/misses, complete product safety or final legacy deletion.

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

<!-- /xr-spec:en -->

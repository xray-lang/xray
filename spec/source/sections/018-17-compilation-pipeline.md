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
精确内部字段及断言入口见 `contracts/xir-stages.md`；该合同不授予执行资格。

内部标量执行按 `contracts/xir-scalar-execution.md` 冻结：首个目标为x86_64小端、
值边界ABI版本5。布局查询同时包含类型、目标、用途与ABI；lowering保存唯一的
帧槽偏移和边界布局，VM/C后端不得另行决定。bool/i64的SSA及帧槽为8字节，
边界值为16字节（类型、零保留字段、i64载荷），bool载荷只准0/1，unit无帧槽。
参数必须精确匹配，错误清空结果；每条指令消耗一步，帧和步骤均受预算限制。
有符号算术须用无符号计算及受检除法避免宿主未定义行为，所有退出释放帧，标量结果独立存活。
Checked不可执行；未知目标/ABI拒绝。此内部子集不授予模块、调用或可恢复ABI资格。

内部可恢复调用受 `contracts/xir-resumable-calls.md` 约束。直接CALL引用模块函数身份，
CALL参数通过函数自有操作数表传递，数量和类型须精确匹配声明；SUSPEND保存下一指令，THROW传递i64错误token。
这些是内部XIR操作，不引入源码关键字，也不授予模块、泛型或stdlib缓存资格。
VM与生成C均通过同一typed帧/结果协议交还trampoline，调用者不把待恢复的子调用留在C栈上。
帧地址稳定，深度、实际字节与恢复工作分别有预算；挂起token只可消费一次，重入驱动/销毁拒绝。
取消在回调交还控制后按子帧到父帧清理；清理不能再次挂起或重新争取完成权。
当前内部接口只允许单宿主线程独占驱动，不声称并发取消、分段池或完整image/instance已完成；托管string扩展见17.7。
闭合标量叶子可使用经明确验证的非挂起入口，但CALL/SUSPEND/THROW不能退回该入口。

### 17.7 内部托管值与结果交接

内部值 ABI 5 与调用 ABI 8 取代首版标量边界，不保留别名。string 保存严格 UTF-8，
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
实际预算、并发、失败原子性及释放义务见 `contracts/xir-managed-values.md`。
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
精确准入、预算与租约责任见 `contracts/xir-program-instance.md`；这不授予源码或缓存格式资格。

### 17.9 新 XIR 源码准入边界

新 source owner 复用实际 parser 与模块 resolver/graph，直接检查声明和构造 Built，
只发布拥有数据的 Checked；不调用旧 analyzer、IR 或执行器。首批按声明族准入
bool/i64/string、只读参数的普通具名函数、直接调用、局部绑定、模块状态、string拼接、
print及默认SeqCst的Atomic<i64>构造/load/fetchAdd。未实施语法明确拒绝；不将未约束泛型
按实例猜测检查。每个函数体都检查，包括不可达函数。未标注返回类型的值返回推断尚不准入。

顶层函数提升，顶层执行语句及绑定初始化按源码顺序每实例执行一次；main只是普通函数。
各模块私有unit initializer与返回0的合成i64入口分开。库模块状态必须是const Sendable，
当前准入scalar/string/Atomic<i64>；根var属于实例。跨模块调用要求直接导入与export，
模块身份来自resolver，重复/私有/未解析声明与环拒绝。字符串AST已经解码，不重复解码。

CALL/PRINT用args[0]/args[1]表示函数自有操作数表的起点/数量；非空范围按指令顺序
完整分割该表，空范围必须为0/0，PRINT immediate为0。单组上限65536，每个值均检查
类型与支配关系。嵌套参数从左到右求值完成后才追加外层范围。Lowered保存并复验最大
出站参数数量，稳定帧为其分配typed value暂存区并计入物理字节预算。
泛型、完整stdlib、协程语法、foreign provider、parser完整
OOM恢复与输入预算、默认CLI及产品资格仍单独验收。接口合同见 `contracts/xir-source-owner.md`。

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
精确字段与预算合同见 `contracts/xir-checked-packet.md`。

该子集（含§17.11的标记约束模板）不代表完整成员约束/见证、效应、诊断来源序列化、包链接、native 缓存配对、
installer 发布或完整无源码标准库分发已经验收。

### 17.11 定义处检查与 Checked 泛型特化

普通类型参数准入域为可复制、可保存的值，不包括 unit、视图和 noncopyable 资源。
当前具体类型为 bool/i64/string/Atomic<i64>。函数可以声明 `<T, U:Sendable>`，
或使用该声明自身参数的 `where U:Sendable`；调用须显式给出全部类型实参。
参数名不重复，Sendable 是保留标记名；首批每个参数最多一个 Sendable 约束，
不准入默认类型参数、交叉/成员/条件方法约束或类型推断。

每个模板体（包括未使用模板）在定义处检查。复制、读传参、保存与返回由普通准入域
保证；Sendable 不授予成员、算术、显示或默认构造。无约束 T 不能调用要求 Sendable
的泛型，即使当前具体类型恰好全都 Sendable。泛型转发必须从声明本身证明约束。
复制保持 string 值语义、Atomic 身份语义；具体 retain/清理和布局在特化后确定。

Built/Checked 使用函数局部类型参数 ID，保存约束和独立调用类型实参表。
CALL 的类型实参范围与值实参范围分别规范化并复验；替换后的参数/结果须精确匹配，
正常可见性、模块和支配关系规则仍有效。Checked 包 schema为4，语义合同为10（局部存储见§17.12，整数见§17.14），
旧schema或语义版本直接拒绝。解码后重新验证模板定义与转发证明。

特化只读取 Checked，不访问 AST。按声明身份与有序具体类型实参建立有界工作队列，
重复或递归实例复用同一条目；先完成实例闭包并复验，再进入唯一 Lowered 管线。
当前宿主函数 ID 接口保留所有普通定义为根；仅发射从这些根可达的泛型实例。
模板独占包没有普通根时不能直接成为可执行程序。实例数、累计参数/块/指令、内存
及工作量有预算，失败发布空结果并释放临时存储。Lowered 和不可变 Program 不保留
开放类型参数，不在执行期追加实例。实例调试名字不是 native 缓存身份凭证。

成员见证、符号回调效应、泛型类型构造器、类型推断、诊断来源序列化与完整标准库包
链接/缓存配对仍待实现；本族未准入的效应/借用/callable 声明仍拒绝。精确接口合同为
`contracts/xir-generic-templates.md`，不得用本族通过代替普通泛型完整资格。

### 17.12 局部可变存储与结构化控制流

局部var必须有初始化值；它表示当前调用帧中的typed place，const与read参数仍是值。
读取取得独立值快照，赋值先完整求值再替换，不改变之前保存的string副本；Atomic复制仍保留身份。
place不能逃逸、返回或直接作为值实参。初始化支配每次访问，类型精确一致；泛型在定义处检查。
Built/Checked保留LOCAL_NEW/READ/WRITE，Lowered唯一选择标量或托管存储动作和帧偏移。
托管写入先retain后drop，帧所有退出路径负责清理；精确最后使用释放和不可复制资源析构尚未据此验收。

if/else与while条件必须bool；只执行选中分支，循环每次重新求条件。局部更新跨分支与回边保存，
块内名字不外泄；无标签break/continue指向最内层while。值函数每条存活路径必须返回，拒绝不可达语句。
本族接通具体i64加法/相等/小于与既有string加法；不授予无约束T任何运算见证。
块/指令/内存/深度/工作预算以及运行步数/取消合同继续有效。Checked wire schema为4，语义合同为10，
旧语义版本拒绝；没有第二条兼容检查路径。精确接口见 `contracts/xir-local-control-flow.md`。

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
当前Checked schema 4/语义合同10、Call ABI 8、Value5/Program4按§17.16替换旧版本，无reader或适配路径。
具体i64能力不能通过实例化补给普通泛型；其他数值族与显式checked/saturating库方法尚未接通。

### 17.15 XIR i64位运算

具体i64的&、|、^按64位二补码位型运算，~降为与-1异或。<<丢弃高位并补零；
>>为算术右移并补符号位。计数按64取模，包括负数：-1为63、-64为0，零有效计数保持原值。
宿主实现仅移位无符号位型；负数右移用补码变换明确补符号，不依赖C有符号右移。
操作数从左到右各求值一次，位运算拒绝bool/string/Atomic及未约束T。变量复合赋值先读左值，
再求右侧，计算后写回并返回新值；右侧即使修改同一变量也不能改变已读取的快照，失败不写回。
+=另准入string拼接及快照，其余复合运算要求i64；const/read、成员与索引目标暂不准入。
当前Checked schema4/语义合同10、Call8/Value5/Program4遵循§17.16，无旧包或ABI适配。其他整数宽度和转换仍未准入。

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

当前唯一包为schema4/语义合同10，Value ABI5/Call ABI8/Program ABI4原子取代旧版本；
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
唯一Checked schema4/语义合同10、Program ABI4原子替代旧版本；Value5/Call8保持当前协议。
捕获、其他mode、效应承诺、类型推断及成员见证继续按各自合同和门独立接通。

### 17.18 XIR 拥有式不可变捕获环境

函数值可以保存一组有序的按值捕获。FUNCTION_REF 的操作数范围是捕获值；目标函数的参数前缀逐项匹配捕获类型，剩余参数和返回匹配公开函数签名。显式泛型替换、值角色、支配、可见性和初始化限制全部在 Checked 验证，特化后复验；捕获不授予额外调用权限。

捕获环境在构造时取得每个值的逻辑副本，失败不发布部分函数值；复制函数值共享不可变环境。string 保持值语义，Atomic 保持同步身份，函数捕获保留原有执行准入。环境不得保存调用方帧指针。最后一个函数引用释放全部捕获及代码租约；嵌套环境清理不得随捕获深度递归消耗宿主栈，也不得为清理再次分配。

间接调用仍发布同一可恢复 CALL action，携带借用的函数值；唯一帧驱动器把环境前缀和显式实参复制到新帧。在新帧完成拥有前，调用方帧继续保活环境；挂起、抛出、取消和返回沿用统一清理。宿主函数调用入口同样保留捕获和实参，再释放前次结果。实例 stop/free 撤销执行准入，但外逃环境和值仍可读取、复制和释放。

该内部合同不把 var 变成按值捕获，也不授予 Sendable、noescape 或 no_suspend。普通 var 的共享 cell、循环回收及源码闭包生产入口须分别接通并验收；内部构造通过不代表源码闭包完成。Value ABI 5、Call ABI 8、Program ABI 4、Checked schema4/语义合同10为唯一现行版本，旧版本拒绝。

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
and types reject. The exact internal fields and assertions are owned by
`contracts/xir-stages.md`; this contract does not grant execution qualification.

Internal scalar execution is governed by `contracts/xir-scalar-execution.md`.
The initial target is little-endian x86_64, value boundary ABI version 5. Layout
queries include type, target, context, and ABI. Lowering stores authoritative
frame offsets and boundary layouts; VM/C consumers cannot choose another layout.
Bool/i64 SSA and frame lanes use eight bytes. Boundary values use sixteen bytes
(type, zero reserved field, i64 payload); bool payloads are zero or one and unit
has no frame lane. Arguments match exactly and failures clear the result. Each
instruction consumes one step; frames and steps are bounded. Signed addition
avoids host signed overflow, every exit releases its frame, and inline
scalar results survive independently. Checked cannot execute; unknown target/ABI
rejects. This subset does not qualify module, call, or resumable ABI behavior.

Internal resumable calls are governed by `contracts/xir-resumable-calls.md`.
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

Value ABI 5 and call ABI 8 replace the initial scalar boundary without aliases.
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
The precise internal contract is `contracts/xir-program-instance.md`; source and
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
state must be const Sendable; admitted carriers are scalar/string/Atomic<i64>.
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
qualification remain separate. The interface contract is `contracts/xir-source-owner.md`.

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
contract is `contracts/xir-checked-packet.md`.

This subset (including §17.11 marker templates) does not qualify full member constraints/witnesses,
effects, diagnostic provenance, package linking, native-cache pairing, installer
publication or complete source-free standard-library distribution.

### 17.11 Definition Checking and Checked Generic Specialization

Ordinary type parameters admit copyable, storable values, excluding unit, views
and noncopyable resources. The current concrete domain is bool/i64/string/Atomic<i64>.
Named functions can declare `<T, U:Sendable>` or an own-parameter `where U:Sendable`;
calls supply all type arguments explicitly. Parameter names are distinct and
Sendable is a reserved marker name. Each parameter currently admits at most one
Sendable constraint; defaults, intersections, member/conditional constraints and
inference remain unavailable.

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
dominance rules. Checked schema 4 preserves templates; semantic contract 10 also covers local
places (§17.12). Older schema or semantic revisions reject. Loading rechecks definitions and forwarding proofs.

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

Member witnesses, symbolic callback effects, generic type constructors, inference,
diagnostic provenance serialization and complete stdlib package/cache pairing are
still unavailable. Unadmitted effect/borrow/callable declarations continue to
reject. The precise interface contract is `contracts/xir-generic-templates.md`;
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
Every live path in a value function must return; unreachable statements reject.
Concrete i64 addition/equality/less-than and existing string addition are admitted;
unconstrained T acquires no operator witness. Block/instruction/memory/depth/work
budgets and runtime step/cancellation contracts continue to apply. Checked wire
schema is 4 and its semantic contract is 10; older semantic
revisions reject without a second checking path. The interface is frozen in
`contracts/xir-local-control-flow.md`.

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
the operand. Current Checked schema 4 / semantic contract 10, Call ABI 8, Value5 and Program4 follow
§17.16 without a compatibility reader or adapter.
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
operators require i64. Const/read, member and indexed targets are not admitted. Current Checked schema4 / semantic contract10
and Call8/Value5/Program4 follow §17.16 without compatibility paths. Other
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

The unique packet is schema4/semantic10 with Value5/Call8/Program4; old versions reject
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
Checked schema 4/semantic contract 10 and Program ABI 4 replace earlier versions.
Captures, other parameter modes, effect promises, inference and member witnesses
remain outside this admitted subset and retain their independent gates.

### 17.18 XIR owned immutable capture environments

A function value may own an ordered sequence of value captures. The operand range of FUNCTION_REF contains captures. The target parameter prefix matches their types; its remaining parameters and result match the public callable signature. Checked verification enforces explicit generic substitution, value roles, dominance, visibility and initializer restrictions, and specialization rechecks them. Captures grant no additional authority.

Construction obtains a logical copy of every captured value and publishes no partial value on failure. Function copies share the immutable environment. Strings preserve value semantics, Atomic preserves synchronized identity, and captured functions retain their existing admission. No caller frame pointer is stored. The last function reference releases all captures and its code lease. Nested environment destruction uses neither host recursion proportional to depth nor cleanup allocations.

An indirect call publishes the same resumable CALL action with a borrowed function value. The sole frame driver copies the environment prefix and explicit arguments into the new frame while the caller still owns the environment. Suspension, throw, cancellation and return use the existing cleanup path. Host function entry likewise retains captures and arguments before releasing a previous result. Instance stop/free revokes execution admission while escaped environments remain readable, copyable and releasable.

This internal contract does not turn var captures into copies or grant Sendable, noescape or no_suspend. Shared mutable cells, cycle reclamation and the source closure producer require separate implementation and qualification. Internal construction does not certify source closures. Value ABI 5, Call ABI 8, Program ABI 4 and Checked schema4/semantic contract10 are the sole current versions; predecessors are rejected.

<!-- /xr-spec:en -->

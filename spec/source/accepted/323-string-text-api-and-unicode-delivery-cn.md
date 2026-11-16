# 已接受目标规则：323 - 字符串、Unicode 文本 API 与新 XIR 交付方案

> 2026-11-16 从任务文档迁入统一规范源。此处维护已接受的目标定义；原任务只维护实施、依赖、理由与验收记录。
> 迁入不表示实现完成。下面保留的示例是目标验收材料，不能据此声称当前编译器、VM 或 AOT 已支持。历史修订和已明确被取代的规则保持历史身份；不能将旧版本重新激活。
> 实施任务：[323-string-text-api-and-unicode-delivery-cn](../../../../xray-docs/tasks/323-string-text-api-and-unicode-delivery-cn.md)。

## 3. 不增加使用负担的基础设计

### 3.1 保留一个普通文本值类型

1. string 始终是有效 UTF-8、拥有式、不可变值；赋值可以共享存储，但必须遵循普通持有失败与释放规则，不宣称复制永不失败。
2. len(s) 继续返回 Unicode scalar 数量，O(1)；len(s.bytes()) 表示字节数。缓存 scalar 计数随合法构造一起建立，不能为一次查询重新解码。
3. indexOf/lastIndexOf/slice 继续使用 rune ordinal；sliceBytes 使用 byte offset，半开区间，必须落在 scalar 边界。rune 不是用户看到的完整字素簇。
4. bytes() 返回只读借用 `Slice<u8>`；copyBytes() 返回独立 `Array<u8>`。fromUtf8 严格验证并复制；fromUtf8Lossy 显式替换非法输入。任意 `Slice<u8>` 都不是文本。
5. slice、split、去前后缀等返回可独立保存的 string。真子区间的默认结果不能只为省一次复制而长久保活整个大源串；全串/空串允许符合值语义的共享。
6. ==、排序、hash 继续基于原字节内容；大小写折叠与归一化必须显式调用。不隐式引入 locale，也不把归一化插入字符串构造。

不新增 StringView、Utf8String、Substring、CStr 等公共字符串类型；不为普通调用暴露生命周期参数。若后续实测证明独立的 UTF-8 借用文本类型有足够收益，再另行设计，不能以 `Slice<u8>` 偷代它。

### 3.2 内部扫描与输出能力

增加或复用一套私有 UTF-8 cursor，维护 byte offset 与 rune ordinal，支持 forward/previous、边界检查与标量读取。它不是可逃逸的公共类型，也不能越过挂起点携带裸指针。生成器帧持有拥有式源串和整数 offset，恢复时重新取得内部读取位置。

所有文本变换使用共同的受预算管理的输出缓冲机制：检查长度加法/乘法溢出、记录实际 byte/rune 数、准备完整结果再交接。它可以位于 XIR runtime/shared 层，但只通过标准声明身份和唯一执行管线调用，不能增加按源码名字绕过检查的 special case。

最早交付内部 cursor/输出 worker，随后接公开 API。因此修复 lower/trim/reverse 不必等待公开 borrowed iterator 或完整 CTFE。私有构建缓冲不作为可导入的标准库类型，不授予用户源码按名字绕过检查的入口。

## 4. 公共 API 与行为决策

以下是目标签名，尚不是“现在即可运行”的示例。新增方法都要验证普通泛型定义处约束、特化复验和导出效果。

### 4.1 核心 string 与便利方法

保留当前 string 的完整成员责任，不删掉尚未在新链接通的声明来缩小分母。S0 逐项列出 fromUtf8/fromUtf8Lossy/fromRune、join、五个查询、slice/sliceBytes、split、replace/replaceAll、repeat、toString、bytes/copyBytes、runes 的资格。

新增以下三个方法：

| 目标签名 | 精确语义 |
|---|---|
| splitOnce(separator: string) -> (string, string)? | 首次匹配，去掉分隔符；无匹配为 None；左右允许空串；输出都是拥有式值 |
| stripPrefix(prefix: string) -> string? | 匹配才返回剩余部分；不匹配为 None；空前缀返回 Some(原值) |
| stripSuffix(suffix: string) -> string? | 同上，作用于后缀 |

统一边界：

- split(separator) 保留首尾和相邻分隔符产生的空字段；空输入遇非空 separator 返回一个空字符串元素。不增加 limit 参数，原 spec/191 中该目标在新声明族切换时一并更新。
- split/splitOnce/replace/replaceAll 遇空模式抛新增 StringPatternError.EmptyPattern；不按字节拆开 UTF-8、不返回未处理输入掩盖错误。该错误使用 198/313 的普通 enum 错误管线。
- replace 只替换首个匹配；replaceAll 从左到右处理非重叠匹配，不继续搜索新插入的 replacement。未命中返回相同内容的拥有式值。
- contains/startsWith/endsWith/indexOf/lastIndexOf 的空模式行为保持现行 spec；不能为了统一代码路径把查询也改成拒绝空模式。
- repeat(0) 返回空串；负数走既有 NUMERIC_RANGE/E0422；真实尺寸不可表示或预算不足走 LIMIT，不包装成模式错误。
- 接收者先求值并保存快照，再从左到右求参数，各一次。方法检查发生在参数完成后；异常不回滚已经发生的参数副作用。
- fromUtf8 保留 Utf8Error.InvalidUtf8；sliceBytes 保留 StringSliceError.InvalidByteRange。解码失败、参数错误、OOM/预算、取消必须分流，不改成 null、空字符串或第二套 panic 兼容路径。

#### 4.1.1 严格 UTF-8 构造的实施边界（2026-10-28）

队列 123 已将严格构造交付到本地主干 `604382f7`：`static fromUtf8(bytes: Slice<u8>) throws(Utf8Error) -> string` 使用真正的普通 throwing callable，经 CALL/INVOKE、真实 Enum/THROW 和原错误边执行。完整 Base64 属性、isValid 直接入口及模块入口的两配置 VM/native 原预期均通过；不删 helper、不缩小输入、不改算法。

1. **声明与身份**：canonical native-callable ID 的 0 表示普通函数，ordinal 来自实际函数表；完整核对 static 签名、owner、真实 Utf8Error enum17、NULL0 body 与无 capture。身份已运输到 Source、Checked、Closed、Lowered/Library、克隆/导入、函数引用/间接调用、Program 与生成 C。wire39/common100、40 字节 FunctionIdentity 和 ProgramABI37 对应真实新增维度；Value30、Call35、SourceAPI3 未改。旧 wire38/common99 packet 作为真实历史负例保留，不提供兼容路径。
2. **执行与所有权**：VM/C 的严格 Slice→String 桥仅将 BAD_UTF8 转成 Utf8Error.InvalidUtf8；OOM、受控内存、overflow、坏 loan、取消/deadline 独立分流。native frame 拥有结果，action 借用，driver 在 frame 释放前复制后关闭；state 尺寸/对齐/恢复/释放/环境/身份在读取或 drop 之前认证。真实头分配失败、字节分配失败及分配后取消已入 L1；不增加 opcode 或伪造 THROW/CFG。
3. **实际资格**：独立 source/packet/Library、typed catch、Unicode/NUL/截断及生命周期消费者通过；原 Base64 属性七行、直接245字节、模块166字节预期均匹配。00e154ee 生产输入在当前普通27/真实 A+U26聚焦、实际 CLI 与 Windows 普通 MD 冷安装 SDK 原61命令通过；canonical 八份核心 COFF 证实双插桩。604 仅追加四处 regex 官方 try! 调用与仍为 GAP 的真实首诊断，重跑两配置八门和完整冒烟63后，经队列123完整 L1 134/134、metadata 与切片 sanitizer 合入。SDK61 沿用00输入身份，当前快照 bundle 刷新不冒充新冷装资格；完整SAN、插桩安装与跨平台未通过。
4. **CTFE 后继已交付**：队列124 / `2f79f8a0` 将四个CTFE执行文件和真实消费者独立合入，沿既有只读Slice/callee授权，复用同一canonical native身份、严格桥、frame.returned所有权和return/error unwind/invoke-deliver。完整Unicode/NUL、空输入、String根快照及五类非法编码输出52B；当前普通/真实A+U各29门、无测试TU替代的实际CLI VM/native逐字匹配。原结果String在Source/session/packet/前级和执行产物退休后独立存活；实际两处OOM、控制/内存、loan和伪造身份均覆盖，独审后完整L1 142/142。AU原aggregate因三份同名编译对象的collector断言FAILED保留，后补canonical实际对象双插桩证据，未重写旧状态。此后队列125的e0bf5bf7组合完整142门再次通过。借用返回、外层Slice输入/结果、捕获及cleanup参数仍精确NOT_SUPPORTED；MutSlice和旧P11全量未交付，未以owned Array或无检查复制替换Slice求值。

严格构造首片封存实际52路径（39生产、3生成、10消费者），主协调追加 L1 与 packet 版本消费者后为54路径，regex 后继另2路径。Source ordering 仅发现 prelude 依赖，名字不授予资格；完整 canonical 认证仍在实际接点进行。既有错误分析只承载真实 native leaf 的声明 E，不扩建旧推导，后续由329统一退出。模块 String/Array 发布是此前独立前置，不暗计到本片。regex 原完整入口现精确失败于 `227:12 try! expressions are not yet supported by the compiler`，四处源调用已按语义迁移，仍是 GAP，不能记功能通过。本片不关闭 lossy/fromRune、其它 String 方法、crypto 熵源或全 Task323。

### 4.2 text 模块

| API | 本任务的语义 |
|---|---|
| lower / upper | Unicode 17.0.0 默认完整大小写转换，允许一对多和上下文相关映射，无 locale 参数 |
| lowerAscii / upperAscii | 只改 ASCII A–Z/a–z，其余 scalar 原样保留 |
| trimStart / trimEnd / trim | Unicode 17.0.0 White_Space 属性；一次确定边界再构造结果 |
| reverseRunes | 反转 scalar 次序；保持名字和语义，不偷偷升级成字素反转 |
| padStart / padEnd | targetLength 按 scalar；重复 padding，最后一段按 scalar 截断；目标不大于原长度或 padding 为空时返回原内容，保持已有行为 |
| lines(value: string) -> `Iterator<string>` | LF 分行，CRLF 作为一个分隔符；裸 CR 为普通字符；不含分隔符，不额外产出末尾终止符之后的空行；空输入无元素 |
| fields(value: string) -> `Iterator<string>` | 按上述 White_Space 连续区域分割，忽略空字段；空输入无元素 |
| caseFold(value: string) -> string | Unicode 默认完整、非 Turkic case folding；不隐式归一化 |
| normalize(value: string, form: NormalizationForm = NormalizationForm.NFC) -> string | NormalizationForm 为 NFC/NFD/NFKC/NFKD；四种均交付，不把兼容归一化冒充无损转换 |
| graphemes(value: string) -> `Iterator<string>` | Unicode 17 默认 extended grapheme cluster，无 tailoring；每项是独立拥有式 string |

lines 示例：空串 -> []；"a\n" -> ["a"]；"\n" -> [""]；"a\n\n" -> ["a", ""]。它不是完整 Unicode 换行算法。

实现要求：

- trimEnd 使用反向 cursor，或一次正向扫描记录最后一个非空白边界；trim 不先构造 trimStart 临时串。
- reverseRunes 倒序读完整 scalar，再按合法 UTF-8 写出；绝不按字节反转。
- padding 重用 cursor/已知完整 UTF-8 片段，不能为第 i 项反复 nth(i)。输出 scalar 数和长度溢出都要检查。
- lower 的 final sigma 等上下文判断不得对每个位置重扫长段 Case_Ignorable；使用有限状态、延迟输出或线性预处理。输出扩张计入预算。
- normalization 处理递归分解、canonical ordering、组合排除与 Hangul 算法；长 combining sequence 不能使用无界二次插入排序。采用稳定的有界 CCC 分桶或具有明确复杂度上界的算法，辅助空间计费。
- translate 保持逐 scalar key 的既有含义；改用一次 map 查询，不为每个 scalar 扫全表。单次预期复杂度受 Map 自己的保证约束，不声称任意 hash 输入严格 O(1)。
- 三个拥有式迭代器只保存拥有式源串、整数位置及必要边界状态；提前 break/drop、异常、取消释放帧和源串。允许结果外逃和跨调用保存，禁止借此让 Slice 外逃。

### 4.3 普通字符串追加与格式化

不提供内置或标准库 StringBuilder，不实现原计划的泛型 append、capacityBytes、reserveBytes、保容量 clear 或快照方法。普通循环使用 string +=，已知片段集合使用 join；需要独立身份、复用策略或业务格式的用户可以用普通 string、Array 和 class 自行封装，语言不为其引入新能力或豁免。

string 仍保持值语义；+= 更新变量的逻辑值，不能改变其他字符串副本。实现目标：

1. 未被真实旧副本、活动借用或其他观察者共享的缓冲允许原地追加；容量不足时几何增长，连续追加不反复复制全部前缀。存在共享时 COW，旧副本保持不变。
2. 编译器产生的临时 owner 不能让本应独占的简单追加永久走共享复制路径。需要端到端的所有权/活跃性证明及正式 Checked/Lowered 表示；不能在 C helper 中直接删除 retain 或提前 drop 输入来伪造唯一性。
3. 保留现行复合赋值合同：绑定目标和读取左值快照在右值求值前完成，右值只求一次，最终结果写回既定目标；右值改绑、用户调用和挂起不能改变已取得的左值。不能套用 Array ref 方法“实参后读当前值”的不同语义。
4. RHS、长度/预算、分配、持有或提交前取消失败，不发布本次追加的部分结果；已完成 RHS 副作用不回滚。原地写入前完成所有可能失败的准备，提交区间的取消观察和费用严格定义；不能为了实现失败回滚而每次复制整个旧串。
5. 优化必须遵守 313/315 的失败与费用规则。S0 先列出源码可观察的求值/持有/计费/提交序列，区分逻辑操作与实际存储请求；若证明需要修改已冻结语义，先显式更新拥有该规则的 spec/任务及语义身份，不能静默抹去既有失败或虚报“纯优化”。
6. 首个必达性能场景是无逃逸副本的局部累加循环；源码、VM、native 都须实测。有快照、字段路径、self-append、右值改绑或挂起时，只有满足同一证明才复用，否则走保持语义的 COW 路径。每轮保留旧快照的程序不承诺整体线性。

~~~xray
var result = ""
for (part in parts) {
    result += part
}
~~~

该写法是本任务必须优化并验收的普通语言用法，不是当前性能已经合格的示例。复杂别名场景不能被当作全部调用都退化的理由，简单场景也不能代表全部副作用语义已通过。

首轮不新增公共 string.reserve/capacity/保容量 clear。317 的预留容量场景明确收敛到已知长度 join 与内部输出缓冲；容量可控制的自定义构建器可复用普通 Array 能力。内部缓冲继续可以预留、扩容和复用，不形成另一个官方公共类型。

格式化第一轮复用现有数值 toString、整数 toHex、浮点 toFixed 与字符串插值，保证精度参数、范围错误、负零、NaN/Inf 和 VM/native 输出一致；不能直接依赖宿主 printf/locale。用户类型通过 Stringable.toString，特殊显示样式通过普通返回 string 的函数组合。格式化和用户 toString 只执行一次，效果按 322 传播，不能为预估长度重复调用。

不新增通用格式字符串语法、Format trait、类型擦除参数数组、locale formatter 或重复的 fmt.int/fmt.float 家族。任意进制/指数格式/终端列宽排版留待真实需求单独设计。text.pad 的 scalar 宽度不能宣传成终端显示宽度。

公共 StringBuilder 的删除范围和顺序：

- S0 盘点类型声明、prelude 保留名/注册、analyzer/runtime/VM/AOT 特殊分支、stdlib 调用、正反例、spec、LSP/MCP 生成来源、库产物与消费者；不得仅删除 stringbuilder.xr。
- 按 2026-10-27 决定直接以目标 string 实施，同一切换批次补齐追加能力、迁移 base64/text 及全部现役消费者，删除官方类型、构造/方法、专用分派、生成记录和无消费者的旧实现，再按完整行为与性能要求验收。不将旧 Builder 的编译、执行或对照产物重新转绿设为迁移前置，也不等待 S5 Unicode 高层处理；缺少替代能力时修复普通 string，不恢复旧公开接口。私有输出 worker 不得暴露官方 Builder 身份；不保留别名、shim、隐藏官方 Builder 或隐式旧链 fallback。
- 解除 StringBuilder 的内置保留名。用户自定义同名 class 必须合法、按普通类规则检查执行，不享有特殊构造、格式化或权限；无用户声明的旧调用按普通未解析名字拒绝。同步旧保留名负例与用户自定义同名正例。
- 内部实现 helper 不公开类型身份；旧 Checked/native/API 身份按实际版本规则拒绝，不把旧 native ID 重新解释为用户类。
- 本次只更新设计与删除责任，当前源码中的旧声明和调用尚未删除；S4 必须记录真实迁移清单与残留检查，不能凭文档完成状态关闭 317 删除门。

### 4.4 regex 坐标原子切换

RegexMatch.start/end 改为 startByte/endByte，find 的 offset 参数改为 offsetByte；同步构造参数、named argument 调用、文档和消费者，不保留旧别名。若范围型匹配结果另有公共坐标，也按同一 byte 命名审计，不能只改两个字段。

offsetByte 合法范围为 0..len(s.bytes())，且必须为 UTF-8 scalar 边界；越界或落在编码中间抛 StringSliceError.InvalidByteRange。不隐式 clamp 或从半个字符开始匹配。公开匹配 startByte/endByte 必须可直接用于原串 sliceBytes。

空匹配迭代按一个完整 scalar 前进；EOF 的空匹配至多返回一次，不构造半个 UTF-8 的 text。例："Aé中" 的 é 匹配是 [1,3)，rune 区间是 [1,2)；测试必须刻意让两种坐标不同。

本任务不接管整个 regex 引擎重写、缺失捕获组表示或全部语法问题。若当前 byte matcher 会产生非法边界，则先修复受影响路径并留拒绝保护；不能通过仅改字段名声称这个成员族完成。

## 6. Unicode 数据与一致性

统一固定 Unicode 17.0.0；输入放在现有 third_party/unicode/17.0.0，登记 SOURCE.md、LICENSE、SHA256SUMS。在现有字素数据外增加所需 UnicodeData、SpecialCasing、CaseFolding、PropList、CompositionExclusions、DerivedNormalizationProps、NormalizationTest 等正式输入；已有 DerivedCoreProperties 复用，不创建第二份版本源。

构建和生成离线、确定性执行；生成器校验输入摘要，--check 比较完整产物。算法与数据更新同批交付。大小写、White_Space、归一化、字素版本要一致；审计 rune.isWhitespace 的实际数据，若不一致就同步改为相同属性定义。

规范依据：

- [Unicode 17 第 3 章](https://www.unicode.org/versions/Unicode17.0.0/core-spec/chapter-3/)：默认大小写与一致性要求。
- [SpecialCasing 17.0.0](https://www.unicode.org/Public/17.0.0/ucd/SpecialCasing.txt)：完整、上下文及 locale 条件映射；默认 API 排除 locale 专属规则。
- [CaseFolding 17.0.0](https://www.unicode.org/Public/17.0.0/ucd/CaseFolding.txt)：默认 full folding 使用 C/F，不能拿 lower 替代。
- [UAX #15 revision 57](https://www.unicode.org/reports/tr15/tr15-57.html)：归一化与正式一致性测试。
- [UAX #29 revision 47](https://www.unicode.org/reports/tr29/tr29-47.html)：默认 extended grapheme cluster；继续无 tailoring。

使用只读生成表和每次调用/每个迭代器私有状态，不引入进程可变缓存、全局构建缓冲或库级 scratch。Unicode 算法不依赖新 comptime/reflection 功能。

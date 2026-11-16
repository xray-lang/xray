# 已接受目标规则：Task337：宿主路径、显式词法路径与逻辑路径

> 2026-11-16 从任务文档迁入统一规范源。此处维护已接受的目标定义；原任务只维护实施、依赖、理由与验收记录。
> 迁入不表示实现完成。下面保留的示例是目标验收材料，不能据此声称当前编译器、VM 或 AOT 已支持。历史修订和已明确被取代的规则保持历史身份；不能将旧版本重新激活。
> 实施任务：[337-native-and-logical-paths-cn](../../../../xray-docs/tasks/337-native-and-logical-paths-cn.md)。

## 2. 类型与公共表面

| 模块／导入 | 类型与职责 | 宿主依赖 |
|---|---|---|
| `import path` | 既有Path／PathInfo；默认文件系统路径 | flavour来自执行宿主；resolve可查询宿主cwd |
| `import "std/path/posix" as posix` | PosixPath／PosixPathInfo；固定POSIX词法工具 | 无 |
| `import "std/path/windows" as windows` | WindowsPath／WindowsPathInfo；固定Windows词法工具 | 无 |
| `import "std/path/logical" as logical` | LogicalPath；与平台无关的相对逻辑组件序列 | 仅显式映射使用调用者提供的Path |

不同类型没有继承、隐式转换或通用IO资格。字符串的显式`from`／构造与`toString`是文本边界，不承诺跨flavour保语义。`JSON.Path`与本任务无关。不得用字段同形、类名相同、模块alias或raw字段存在授予canonical Path读取权。

### 2.1 默认path

保留12个函数及PathInfo现有五字段：`from`、`isAbsolute`、`joinAll`、`join`、`dirname`、`basename`、`extname`、`normalize`、`relative`、`resolve`、`parse`、`format`。新增`hasRoot(p: Path) -> bool`；`isAbsolute`精确定义为fully-qualified，不依赖当前目录即可定位。`relative(fromPath: Path, toPath: Path) -> Path?`原子改为可空结果：不能形成相对定位时返回null，调用方显式处理，不保留绝对路径伪装相对结果。`sep`、`delimiter`仍是导出const，按执行宿主初始化：Windows为反斜杠及分号，POSIX为斜杠及冒号；交叉编译不以编译器运行宿主替代执行宿主。host-flavour入口不能被Source生成或CTFE按编译机折叠；模块按现役NoSuspend全局初始化承载真实const。初始化后的flavour读取与cwd／fullpath上下文查询分别计数，绝对输入的零上下文查询不把初始化算作cwd查询。纯POSIX／Windows模块不得调用任何host入口。

`Path(raw)`／`from(raw)`保留原始文本，`toString`精确返回，不自动归一、改大小写、去扩展前缀或检查存在。Path是定位文本，不等于已验证文件。现有UTF8／UTF16、NUL及IO失败合同保持，不在本任务扩为任意OS字节字符串。

### 2.2 显式纯词法模块

各模块提供上述同族函数、`hasRoot`、固定sep／delimiter，签名中的Path和Info分别替换为各自名义类型。与默认模块的主要区别是纯`resolve(base, ...parts) -> PosixPath? / WindowsPath?`：base必须fully-qualified，不能读取cwd。Windows同盘drive-relative可用base；需另一盘cwd或其它未提供上下文则返回null，不猜盘根。外flavour纯模块在每个平台都执行同一规则。

### 2.3 LogicalPath

LogicalPath通过`logical.from(raw: string) -> LogicalPath?`创建，模块函数委托唯一验证入口`LogicalPath.from(raw: string) -> LogicalPath?`静态方法；后者在类内部调用private init，保持现有私有构造访问规则，不为模块factory放宽语言权限。不开放绕过验证的public raw构造；内部表示为private const且无公开变异出口，不能创建后改成非法组件。其公开`toString() -> string`输出canonical相对逻辑文本：`/`连接组件，空路径输出空字符串。空输入合法；非空输入拒绝开头／末尾`/`、空组件、`.`、`..`、反斜杠、NUL。普通组件文本不假装已在任何具体文件系统验证，例如冒号可作为逻辑文本，但映射到不接受它的宿主时失败。

空LogicalPath是join的左右单位元，join只连接已验证组件，不引入空组件。提供`logical.join(a, b) -> LogicalPath`及`logical.toNative(root: Path, value: LogicalPath) -> Path?`。root必须是fully-qualified的宿主路径；映射逐组件验证宿主可表示性并连接，不从进程cwd推根、不接受drive／root注入、不把斜杠替换后直接当授权。Windows映射拒绝组件中的`<>:"|?*`、控制字符、尾点／尾空格及DOS保留设备组件；COM／LPT的数字与Windows认可的上标数字同样检查。POSIX仍拒绝NUL和组件分隔符。失败返回null，内存／取消等真实执行故障保持原路径，不折成null。

本轮不增加反向推导逻辑root、URI、URL、包资源定位或路径sandbox。映射结果的词法前缀不证明文件对象在root内：符号链接、junction、重命名及TOCTOU需由未来基于目录句柄的独立安全API处理。禁止把toNative用于未经验证的安全包含判断。

### 2.4 Checked发布与共享模块闭包

Path的默认模块、三个显式子模块及共同词法实现由**一份真实Checked Library产物**拥有；output、time分别保有独立产物。生成器在独立临时输入根写入私有bundle Source，真实导入五个完整原始Path源文件，再走既有Source检查、Checked编码与重读。不得复制五份重叠依赖包、按名字跨包去重、篡改root或伪造Program身份。公开源文件、canonical模块身份、类型和导入路径保持真实；Library的root哨兵不改为Program入口。

私有stdlib publication manifest原子采用schema3，逐产物记录真实packet路径、长度、摘要及**按已解码declarations顺序**得到的完整模块表：ordinal、authority kind、namespace、logical path、canonical identity及依赖ordinal。私有bundle源文本、长度、摘要一并记录，并作为真实生成源码随stdlib安装以保持source locator可搬移；不能从预设模块数或文件名猜测元数据。全部公开Path模块都必须列为同一owner内的实际视图；私有helper／bundle模块同样如实记录。安装、搬移及Source-poison消费者同步验证精确schema、完整序号、唯一canonical owner和同产物依赖闭包，不留schema2兼容路径。

闭包还保留编译器物化的受管prelude模块，精确canonical为`stdlib-module-v1:module=7:prelude:path=27:prelude/builtin_symbols.def`。它的实际执行输入是同代`xir_prelude_source.text`生成的Xray源码，不能把原始`.def`声明表当作已执行的Xray文本。生产者须记录真实生成文本、长度、摘要及明确的generated-prelude来源，与原始Source文件分列；模块ordinal、依赖仍从实际解码结果取得，不写死序号。只承认这个既有受管非`.xr`身份，保留原prelude完整认证，不开放任意`.def`或伪造后缀。 Range构造器按327§30既定语义接受承诺位0或NoSuspend且拒绝其它位，普通count方法仍为0；0本身不是NoSuspend证明。真实非挂起证据继续来自完整受限body与CFG、参数／类型／操作数／目标／立即数及唯一构造几何认证；Common可接受但改变`_step`等构造行为的body必须被冷库认证拒绝，不能只凭canonical名字、输入摘要或承诺位授予。

Catalog使用既有v2多模块输入，按完整模块表建立typed authority及logical path；保留原canonical重建、全局唯一性和依赖属于同一artifact的检查，不以适配stdlib为由放宽通用Catalog。字符串查找只准入共同检查已验证且全部字段可由现有body复制／类型映射承载的局部指令；不扩大到任意op。此处是私有打包与导入适配，SDK manifest的schema2、公共ABI、wire42及共同语义136不因打包改动机械递增。

### 2.5 实际root与模块选择闭包

私有stdlib Catalog输入的physical root须通过与Resolver相同ledger／IO policy下的真实owned canonicalization，临时结果在全部出口释放；权限比较仍精确核canonical、kind、namespace、root和logical path，不以替换分隔符、路径前缀或名字相等放宽身份。此处规范化的是工具链实际stdlib目录，不改用户Path／IO的词法与符号链接行为。缺失、取消、期限、OOM及额度失败沿真实typed状态返回，不能回退Source或伪造空根。

同一artifact拥有多个模块时，Source只导入真实选中views及其依赖闭包。名义类型、接口与构造类型映射先置未映射标记，仅为当前graph内同artifact的实际owner建立声明及可映射类型；未选模块不能因同处一包而增加初始化边或host调用。选中签名、字段、约束、parent、interface、member、materialization或指令真正需要未映射项时必须拒绝，不能吞掉Source／OOM／取消错误。全部MAX消费者保持一致，ordinal加1之前先检查，implementation过滤后发布真实条数。完整Checked、same-artifact依赖和原私有调用、function／slot／default／comptime／borrow-return验证不减；Class递归字段继续沿现有后续阶段处理。

退出包含单view proper-subset、多view同owner且名义身份一致、未选initializer不执行、选中声明跨未选owner的真实引用拒绝及恢复。负控若先被Common拒绝须按该层记录，不能冒称覆盖Source接收器的MAX拒位。

### 2.6 同一Catalog输入的验证复用

沿Task333与验证政策的信任边界原则，Catalog构造仍对每个原始packet执行完整checked读取／共同检查、library shape、模块及ordinal、canonical唯一性、authority和same-artifact依赖校核，全部成功才发布。Source仅可从当前同步借用的opaque Catalog精确找到其真实resource row及其确切拥有的Artifact；相同名字、摘要、内容、复制row或另一个独立冷读Artifact均不能代替来源。不得增加trusted布尔开关、可逃逸证书、持久缓存或公开ABI。

只有Catalog已验上下文与当前Source的ledger以及functions、parameters、blocks、instructions、frame_bytes五项结构限制逐字段完全相等，才复用同一不可变owner已完成的完整验证，省去Source发现prelude与收集inventory时对该owner的重复整包effects／shape复验；不以含padding的memcmp判断上下文。任何限制差异均保留原当前上下文的完整验证，不直接拒绝原来合法的小包，也不让较窄Source限制被选中模块的较小数量遮蔽。Source的CHECKED／LIBRARY阶段、current ledger、每view canonical／ordinal／same-artifact依赖、权限及输出检查保持；CHECKED_HISTORY继续独立验证。

复用不继承Catalog创建时的请求控制结果：Source在接收入口、复用前后及有界查找处继续独立轮询当前取消／deadline／clock，保留首次typed失败、空输出及失败清理。借用只活到同步Source检查结束，不能写入可逃逸Graph、snapshot或result；产物仍完整拥有其接收数据并允许原Catalog先退休。

退出复用已有Path多view／proper-subset、冷读及producer退休、Range构造反控、错误类型／authority／依赖、VM与同源C门，另在同一现役测试中覆盖真实row正控、复制row与独立Artifact拒绝复用、同ledger宽Catalog／窄Source仍严格BUDGET、恢复原限制成功、当前取消与零deadline、Catalog退休后结果独立存活及held／physical清理。窄限制负例必须先证明选中Source图可容纳而完整owner超限，不能让输出超限替输入复验顶账。观测原件保留；完整冒烟保持原双向棘轮、首诊断、每命令30秒及300秒整门，不以提高超时完成本项。

## 3. 统一词法规则

### 3.1 POSIX

只有`/`分隔；反斜杠是普通字符。前导`/`为root及绝对路径；重复前导分隔归一为单根，不从`//`推UNC。join按最后绝对段重置；相对段不引入宿主查询。join／joinAll保留点段，不顺手normalize；只有显式normalize与resolve折叠点段。normalize按当前POSIX规则折叠分隔与点段，根以上的`..`被钳制，相对路径保留无法抵消的`..`；空结果为`.`。纯POSIX原有向量整体迁入显式模块，不能丢掉反斜杠文件名断言。

### 3.2 Windows根与命名空间

| 输入类别 | hasRoot | isAbsolute | 解释 |
|---|---|---|---|
| `a\b` | false | false | 当前目录相对 |
| `C:a` | false | false | C盘独立当前目录相对 |
| `\a`或`/a` | true | false | 当前drive或UNC share根相对 |
| `C:\a`或`C:/a` | true | true | 完整DOS drive路径 |
| `\\server\share\a` | true | true | 完整UNC share路径 |
| 完整`\\?\C:\…`、`\\?\UNC\server\share\…` | true | true | verbatim，保留文本身份 |
| `\\.\device`及其它完整device／verbatim namespace | true | true | namespace自身的root与后续文本 |

不完整UNC／prefix不能误判为fully-qualified。普通DOS／UNC和device同时识别斜杠与反斜杠；普通DOS／UNC的词法生成分隔使用反斜杠。verbatim仅按反斜杠扫描，保留正斜杠、点段、尾点／空格及重复分隔。device也保留原始IO文本；不得把它等同于verbatim的分隔规则。device与verbatim的normalize均保持原文本，不宣称能安全消解其namespace点段。

有限root分类按以下优先级实现：精确双反斜杠＋问号＋反斜杠是verbatim入口，其中drive必须为ASCII盘符加冒号及反斜杠，UNC必须有非空server和share；保留这两种前缀的判别，不能把残缺形式降格成通用namespace。其余verbatim以第一个非空namespace组件为根边界。精确双反斜杠＋点＋反斜杠是device入口，以第一个非空device组件为根边界。普通UNC以两个前导分隔及非空server／share为根边界；再判普通drive及仅根路径。裸入口、UNC仅server、空share、verbatim drive缺根均不是fully-qualified；完整UNC／namespace根不要求额外末尾分隔才成立。未识别文本保留raw，不猜root、不剥前缀；现有直达IO资格不因词法分类失败而消失。

join／joinAll保留点段。覆盖规则先于namespace追加规则：即使基底是verbatim／device，fully-qualified新段仍优先重置；只有root的新段保留已有drive／share并换根；同盘drive-relative沿已有该盘base连接，异盘drive-relative重置为该段并留给resolve查询；普通relative追加。`C:`后追加相对段不能擅自加根分隔变成`C:\`。normalize不得越过drive／share根，不删除drive或share，不替原路径推断cwd。verbatim／device的normalize返回原文本，无prefix／root的相对新段才按各自分隔边界追加，不消解点段；例如verbatim基底遇另一完整drive路径必须重置。

dirname、basename、parse遵守同一prefix／root边界；根的dirname是根、basename为空；Windows的`C:`作为drive-relative前缀保留，不被当作普通文件名。扩展名沿既有dotfile规则，单独`.`／`..`无扩展。format优先base，其次name+ext，优先非空dir再root，不得把`C:`无端改为drive root。

relative先词法归一，按完整组件比较；不对任意文件名全局lowercase。drive字母及UNC server／share的ASCII大小写可用于root匹配，普通组件精确比较。根／drive不同、绝对与相对混用、前置`..`导致关系不确定或输入无法在无上下文下比较时返回null，不制造跨卷`..`或绝对fallback；同路径返回Some(`.`)。verbatim／device仅相同原文本可返回Some(`.`)，其余返回null，不假装知道namespace关系。各纯模块同为可空relative。

### 3.3 resolve与realpath

默认resolve是**词法绝对化**，不要求目标存在，不跟随符号链接，不授文件身份／安全包含保证。先按统一join规则合并，再只在需要时取得真实宿主上下文；完整fully-qualified路径不为取得cwd而调用宿主。Windows必须正确处理`C:child`的per-drive cwd及`\child`的当前drive／share，不从去掉盘符的假POSIX cwd恢复。

可以给canonical path模块增加受管私有host-flavour及full-path查询；不引入import os→sys→path环。不直接复用名字相同但Unix分支执行真实realpath的helper。Windows可抽取已有GetFullPathNameW词法绝对化实现，沿Domain分配、UTF转换、请求取消／deadline及typed HOST／OOM／LIMIT路径；查询失败不发布半成品。宿主cwd可被其它线程改变，resolve不是进程范围稳定快照；需要稳定定位的调用方先取得明确绝对Path。

`io.realpath`继续查询真实文件对象，沿following handle／POSIX resolver合同；`readlink`返回链接目标文本，禁止用resolve伪造realpath或改写relative target。已交付的lossless realpath前缀投影约束保持。

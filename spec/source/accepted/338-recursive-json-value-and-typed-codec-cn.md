# 已接受目标规则：Task338：递归 JSON.Value 与类型化编解码

> 2026-11-16 从任务文档迁入统一规范源。此处维护已接受的目标定义；原任务只维护实施、依赖、理由与验收记录。
> 迁入不表示实现完成。下面保留的示例是目标验收材料，不能据此声称当前编译器、VM 或 AOT 已支持。历史修订和已明确被取代的规则保持历史身份；不能将旧版本重新激活。
> 实施任务：[338-recursive-json-value-and-typed-codec-cn](../../../../xray-docs/tasks/338-recursive-json-value-and-typed-codec-cn.md)。

## 2. 已定公开模型与不变量

- `JSON` 是无需 import 的 prelude 命名空间，不是值类型；`JSON.Value` 精确覆盖 `null | bool | i64 | f64 | string | Array<JSON.Value> | JSON.Object`，保留 arm 区别与有限递归数据。不是任意值擦除／`any`。
- `JSON.Object` 与 `Map<string, JSON.Value>` 是真实类型别名，不能变成外观相似的自定义对象、Class 或第二种动态字典。已有 Map 的键、顺序、READ／ref／move及复制、别名规则仍适用。
- 目标明确时 JSON 标量隐式 widening；typed 复合值必须显式 `JSON.value(...)`，裸结构对象不因上下文变成动态 object。没有 JSON.Value 的 dot、下标、遍历或 len 魔法；使用 typed decode、显式 asObject／asArray 或 path API。
- `asArray`／`asObject` 返回共享底层存储、无深拷贝的可空 array／object 视图，已定，不重新询问。这里的“视图”不能偷换为 `Slice` 借用或新增可逃逸裸指针；其拥有式寿命、变异及派生 alias 仍须同时满足既定容器和借用合同，不能按 RC 数目决定可观察语义。
- `JSON.Value?` 是独立外层 Optional：缺席与 `Some(JSON null)` 不同；`Map.get`、路径缺失与显式 null 不能折成同一载荷。失败报告仍按具体 API 的合同，不以 null 通吃语法、类型、OOM、取消或协议错误。
- `JSON.Path = Array<string | i64>` 是现有规范的非 error 联合需求，不新增公开PathSegment别名：string 是完整 key，含点号不拆段，i64 是索引。实施须为此真实准入、构造、接收、分支和同 C 留下闭环，不能复用 error-union 认证、改成 Array<Value> 或用编码字符串暗替。此限定需求不授权任意递归 union。
- 内部可采用 canonical tagged payload 并复用现有受管 Array／Map／String／Domain 与 TypeArena；只冻结语义和真实身份，不预先指定 ABI 布局、native ID 或新 opcode。按实际类型／编码／准入改变协调必要版本与前代拒绝，不能为消警或填表机械 bump。
- 编码和解码只处理规范准许的字段及明确资格，不能把递归值域当成任意Class私有字段读取权。所有公共操作必须认证真实 namespace／声明、类型与实际参数，保词法遮蔽、导入权限、effects、Root、NoSuspend 与当前执行入口，不因名字 JSON／同形结构／包内标签授予权限。不得保旧 Json／JsonValue 别名、旧 raw fallback、双签名或迁移开关。

## 3. API 完整性、错误、alias 与借用

以语言规范 §14.11 及 `stdlib/types/json.xr` 的完整声明清单逐项对齐，不能只登记函数名：

| 家族 | 本任务必须闭合的真实行为 |
|---|---|
| parse | `parse<T>`、`parseObject`、`parseValue`、`parseWithRest<T>`；完整语法／UTF-8／escape，任意 JSON 根与指定根区别；typed 目标直接构造，未知字段默认 Reject、显式 Ignore，rest 只保留既定顶层未知项 |
| encode／decode | `value<T>`、`stringify<T>`、`decode<T>`、`decodeObject<T>`、`merge<T>`；定义处可编码／可解码约束及真实 nominal／结构目标验证，不只改 tag 或类型 ID；错误字段、嵌套目标与类型参数都沿普通检查 |
| inspect／unwrap | kind／is 各七 arm、asArray／asObject、已规定的JSON类型判断与标量解包；不能把复合 typed 解码伪装 scalar cast，也不能因为空容器跳过类型约束 |
| path／mutation | `get<T>`、`require<T>`、`containsPath`、`set`、`remove`；混合 key／index、空 path、负／越界 index、错误中间 kind 与 createParents 保原规则；保 receiver 和实参一次求值、顺序、副作用及所有失败的持责 |

声明中的错误集必须与 Task329／331／336 当前规则一致。`get/decode` 的普通失配返回 Optional 不代表可吞 OOM／LIMIT／取消；`require` 的 `JSON.PathError` 与 parse 的业务错误必须有真正声明、enum 身份、throws(E)、调用点 try 和普通清理运输。现 `json.xr` 的未标 throws 声明与旧 PanicInfo 包装不构成精确错误集合的批准；P0 先定界，不擅改成 `Error` 擦除或一律 panic。borrow／ref／move 模式也须按真正 path mutation 与共享视图的实际写集核对，不把 READ handle 当全图写权限。

## 4. 实施前有限政策核对

下列是静态旧行为线索，不是本任务已选的新规则，也不是动态验证结论；只就冲突／空缺补齐最小决定，不能重问第 2 节。

| 接缝 | 当前事实及需冻结的边界 |
|---|---|
| 数值范围与类别 | 旧通用 C parser 对整数字面量先 strtoll，越界转 f64；普通 `jsonInteger` helper 对 i64 越界返回 None，`jsonFloat` helper拒非有限结果。这是不同目标／入口，不能机械当同一规则。明确动态 parse 对 `9223372036854775808`／`1e400`、typed parse 对越界和 int／float 类别的转换；不新增 BigInt／任意精度 arm。 |
| 负零、非有限与序列化 | 旧路径用 strtod 和 `%g`，`-0` 走整数路径会丢负号，`-0.0` 属浮点；旧 stringify 将 NaN／Infinity 写 null。规范七 arm 未精确定义全部往返词形与非有限策略。冻结 `-0/-0.0/1/1.0` 的 kind／数值／输出预期及非有限是拒绝还是既定转换；不能凭宿主 libc 输出反推新黄金。 |
| 重复解码 key | L0 DOM 保留重复 member；旧动态运行时经 Map set 覆盖；ordinary exact-schema helper明确拒 unknown／duplicate／missing。分别明确动态 Object 和 typed 目标对 `{"a":1,"\u0061":2}` 的行为，不能把词面不同当两个 decoded key，也不为 Map 别名保多值隐表。 |
| 精确业务失败 | 明确 parse／typed mismatch／require／set 的 error enum 与签名，既有 Optional 失配规则保持；真正资源失败、取消和 panic 独立。不从旧 wrapper 的 PanicInfo 运输推定目标错误 API。 |

递归类型不等于循环对象合法；按现有容器／JSON mutation 与 stringify 的实际环规则核对。旧 stringify 有环与深度拒绝逻辑，不得无界递归、默默输出 null 或以“用了 RC”承诺环回收；不得借此新增通用 GC／owner 框架。

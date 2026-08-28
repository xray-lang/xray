# native 表面可见性：能力已落地，判据在这里

- **lane**：6-1（stdlibgen 元数据使能）
- **基线**：`34be0379c`
- **分支**：`work/6-1-stdlibgen-internal-34be0379c`
- **面向**：6-4（mem/Buffer）、6-5（regex/RegexMatch）、6-10（sys/net）

## 0. 先读这一句

能力做完了：`stdlib/defs/core.def` 里**任何**条目现在都能写 `visibility: "internal"`，
native class 现在也能在自己模块的 `.xr` 体里被命名。

但有一条必须先知道：**给一个条目标 `internal` 没有任何编译期后果**（class 除外，见 §3）。
`.xr` 照样编过，用户代码照样编过，门的数字照样下降。

所以 **「标了之后还能编过」证明不了标对了**。判据只能是语义的，写在 §4。

## 1. 落地了什么

| 提交 | 内容 |
|---|---|
| `9ac33a122` | 10 种 `.def` 条目全部带 `visibility` 字段和统一的 `is_internal` |
| （本次） | native class 进入 analyzer 类型表，可在自己模块的 `.xr` 里被命名 |

拼法按种类分三档，理由在 `tools/stdlibgen/stdlibgen.py` 的模块 docstring：

| 种类 | 默认 | 显式键 |
|---|---|---|
| `fn` `const` `handle` `object` `enum` | 名字以 `__` 开头即 internal | 认，覆盖默认 |
| `class` `native_class` | 恒 public（**没有**前缀规则） | 只认显式键 |
| `type_method` `class_method` `class_field` | 继承同模块同名的类 | 认，退出继承 |

`class` / `native_class` 不设前缀规则，是因为类名是用户可见的类型名，
会进诊断、LSP 补全和错误信息，`__Buffer` 是更差的名字而不是更私有的名字。

成员继承在整份 `.def` 解析完之后统一 resolve，因为类可能声明在成员之后。
`Coro.CoroLocal` 只有 `type_method` 行没有 class 行，继承找不到父，保持 public。

## 2. 实测证据（三个方向都验了）

`RegexMatch` 是 `REF-native-class-nameability.md` 点名的那个"哪张表都没有"的名字。

| 方向 | 做法 | 结果 |
|---|---|---|
| 正向 | 在 `stdlib/regex/regex.xr` 里写 `fn __probe(m: RegexMatch) -> bool` | **构建通过**（基线是 `error[E0365]: undefined type 'RegexMatch'`） |
| 对照 | 同一位置换成 `__NoSuchTypeXYZ` | `error[E0365]: undefined type '__NoSuchTypeXYZ'`，构建 FAILED |
| 负向 | 用户脚本里写 `fn takes(m: RegexMatch)` | **仍报 `E0365`** |

对照那一条是必要的：没有它，"正向通过"也可能只是因为构建根本没重新分析 `regex.xr`
（第一次跑就踩到了这个——`build-nofp/generated/stdlib_embedded_sources.inc` 是编码过的，
grep 探针函数名找不到，看起来像没生效）。

负向那一条是这个方案相对于「往 `stdlib/prelude/builtin_symbols.def` 加一行」的**全部价值**：
名字只在自己模块的 `.xr` 体里可见，没有泄成语言的全局类型名。

## 3. 核心发现：标 internal 在 analyzer 侧不可见

`src/frontend/analyzer/xanalyzer.c:1853`：

```c
static bool xa_stdlib_native_type_is_internal(const char *name) {
    return name && name[0] == '_' && name[1] == '_';
}
```

handle（`:1876`）、object shape（`:1899`）、enum（`:1926`）的 `is_exported` 全部由它决定。
**它读名字，不读元数据。** 所以在 core.def 里给一个 object 或 enum 写
`visibility: "internal"`，门的数字会降，而 analyzer 侧的导出性一动不动。

这是一把可以骗门的枪。三条 lane 会各踩一次，所以写在这里。

**class 是例外，本 lane 已经修正**：`XaBuiltinClass` 带 `is_internal` 字段，
注册处读它而不是读名字。class 是新增路径，没有历史行为要保护，
所以直接采用单一权威。对当前树是零行为差异（生成表里 4 个 `__` 开头的
handle/object 在两种判据下答案相同，手写的 `g_rt_builtin_modules` 里没有 `__` 名字）。

handle / object / enum 的判据**没有**跟着改。那会改变既有类型的导出语义，
影响面超出使能范围，而且没有 lane 在等它。谁要改，先量 `is_exported` 到底影响什么。

## 3.1 两个会把绿门弄红的连锁（标之前必看）

**(a) 标 internal 会把条目变成 native leaf。**
`scripts/stdlib_symbol_inventory.py:848-850`：

```python
is_leaf = symbol.startswith("__") or str(getattr(entry, "visibility", "")) == "internal"
```

**任何种类**的任何一条 `.def` 记录标 internal，立刻被判为 native leaf，
于是必须在 `stdlib/native_leaf_allowlist.toml` 里有对应 record，否则算 `unclassified`，
`stdlib_native_leaf_allowlist` 就从 **PASS 变红**——那是这一轮唯一绿的门。

record 还要经得起三道校验（`stdlib_symbol_inventory.py:668-720`）：
`ownership` 必须等于 `.def` 的 `return_ownership`，`effect` 不能丢 `.def` 声明的 token，
`vm_binding: yieldable` 必须带 `suspends`；allowlist 里有 record 而没 leaf 命中，算 defect。

**(b) 让条目退出 api inventory 会打破 MCP 的双向等值。**
`tests/mcp/test_knowledge_generation.py:228-243` 要求
`src/app/mcp/xmcp_knowledge_generated.c` 与 api inventory **每模块双向相等**。
`ClusterDelivery`（`:323`）、`ClusterInfo`（`:358`）、`NetError`（`:3340`）都在里面。
所以给 `fn` / `const` / `handle` / `object` / `enum` 标 internal，必须同时重生成那个文件。

**只对 `type_method` / `native_class` / `class` / `class_method` / `class_field` 标 internal
不触发 (b)**：`scripts/gen_api_inventory.py:685-696` 把这五类解包成下划线变量后**从不引用**，
它们本来就不在 inventory 里。（顺带纠正一种容易有的误解：那不是"少了 is_internal 过滤"，
是这条路根本没接上。）

**(c) 把某模块最后一个 public `fn` 标 internal 会改变 private-leaf binder 集合**
（`stdlibgen.py:215-237` `derive_private_leaf_modules` 只看 fn），
而 `tests/unit/stdlib/test_stdlib_boundary_manifest.py:55` 对该集合是精确等值
`{"io", "net", "os"}`，一变就红。

## 3.2 本 lane 修掉的一个连锁（(a) 的根因）

`is_leaf` 那行判据是在「只有 `fn` 有 `visibility` 字段」的世界里写的：
`getattr(entry, "visibility", "")` 对其余九类恒返回 `""`。给全部十类加上字段之后，
它的语义漂了——**每一个 internal 的类、方法、字段都被判成 native leaf**，
实测把 `stdlib_native_leaf_allowlist` 从 PASS 打成 FAIL，27 个 unclassified。

根因是记账错误而不是缺 record：一个 `native_class` 声明或一个 `class_method` 绑定
**不是**到 C 的调用叶子，`classify_leaf` 要比对的 `return_ownership` 只有 `fn` 行才声明，
allowlist 的 schema 根本没有描述它们的形状。给它们补 record 等于伪造记录。

所以判据收回到 `fn`：

```python
is_leaf = symbol.startswith("__") or (
    type(entry).__name__ == "StdlibEntry" and entry.is_internal
)
```

改完之后，即使 4 个 native_class 标着 internal，该门仍是 **PASS，0 unclassified**。

## 4. 判据

一个 `.def` 条目该标 `internal`，当且仅当：**模块的公开 API 不依赖这个条目存在。**

不是"标了还能编过"，那永远成立（§3）。

按条目种类展开：

| 种类 | 它声明的是什么 | 标 internal 诚实吗 |
|---|---|---|
| `native_class` / `class` | 运行时类注册（`core_slot`、`native_body`、`flags`） | **通常诚实**——这是绑定层，不是 API 声明 |
| `class_method` | VM 的方法 dispatch 绑定 | **通常诚实**——同上 |
| `fn X`（构造函数形状） | 用户写 `sys.OsMutex()` 时的签名 | 只有当 `.xr` 提供了等价公开入口才诚实 |
| `type_method X.m` | analyzer 看到的方法签名 | 只有当 `.xr` 提供了等价能力才诚实 |
| `object` / `enum` | 用户能命名的类型 | 只有当 `.xr` 自己定义了这个类型才诚实 |

**⚠️ 双声明必须成对标。** `sys` 和 `mem` 的 22 个符号各有两个条目块：
`native_class X` + `class_method X.m`（绑定层）**并且** `fn X` + `type_method X.m`（类型层）。
`def_public_symbols` 从两者拼出**同一个名字**，只标一半门不降。

## 5. 那 40 个符号的诚实评估

先说结论：**它们绝大多数是真的公开表面，不是元数据缺口挡着。**

`.xr` 源码里的实际用量（`tests/` + `examples/` + `stdlib/`，裸名匹配）：

| 符号 | 用量 | 判定 |
|---|---|---|
| `Semaphore` | 86 处 | 公开并发原语 |
| `CountdownLatch` | 72 处 | 同上 |
| `WorkQueue` | 63 处 | 同上 |
| `ResultGroup` | 53 处 | 同上 |
| `EventCount` | 50 处 | 同上 |
| `OsMutex` | 15 处 | `sys.xr:192` 拿它当字段类型 |
| `ClusterDelivery` | 13 处 | `cluster.xr:18` 的返回类型 |
| `ClusterInfo` | 5 处 | `cluster.xr:16` 的返回类型 |
| `OsCondvar` / `OsBarrier` / `OsOnce` | 各 8 处 | 公开同步原语 |
| `OsRwLock` | 7 处 | 同上 |

把这些标 internal 会让门变绿而语义不变——那正是 §3 说的骗门。

**门的 docstring 说它在问什么**（`scripts/check_stdlib_full_xray_completion.py:749-753`）：

> A public native symbol is C semantics exposed directly as the module's API,
> so it cannot be replaced without changing what callers depend on.

要诚实地清空，得让**声明权威**从 `.def` 移到 `.xr`，不是给 `.def` 条目改个标签：

- `cluster` 的 5 个：`cluster.xr` 已经会写 `export type` / `export enum` / `export class`
  （`:36 :44 :51 :57`），所以它**能**自己定义这 5 个。代价是 `exact: true` 的布局和
  `stable_enum_layout_id` 要和 C 侧构造出来的值对齐——两处定义就是两个 owner，会漂。
  这是真正的工作量，不是标签。
- `sys` 的 Os* 五族：`sys.xr` 已经在走这条路（C 层只留 `__` 私有 helper，
  `fn __osMutexNew` 已经在 `core.def:120`），但 `fn OsMutex`（`:107`）和 13 个
  `type_method` 还公开着。缺的那一步是 `sys.xr` 把类型层接过去。
- `sync` 的 5 个：走 `manual_public_native`，**根本不在 core.def**（core.def 没有
  `module sync` 块），本 lane 的元数据能力对它们无效。权威是
  `stdlib/stdlib_boundary.toml:214-215` 的两个数组；`stdlib/sync/sync.c:46-50` 的
  `sync_export_native_class(...)` 只是给已在 manifest 里的名字找一个可归属的 C 出处
  （扫描器 `scripts/stdlib_symbol_inventory.py:239-243`、`:401-421`）。

  **技术上它们可以清空**：同时清空两个数组即可，`declared == def_symbols ∪ manual`
  变成 `∅ == ∅ ∪ ∅`，`sync.c` 一行不用动，VM 绑定不受影响
  （运行时导出表和 analyzer 符号表是两条路）。

  **本 lane 判断不动它，理由**：这 5 个类的**声明权威已经在 `.xr`**——
  `stdlib/types/{semaphore,countdownlatch,eventcount,workqueue,resultgroup}.xr` 是规格，
  C 是被检查的实现（`xa_native_verify_protocol`，
  `src/frontend/analyzer/xanalyzer_native_types.c:650`，遍历从这些 `.xr` 生成的
  声明表，逐个方法去运行时类里找，找不到就记 mismatch）。
  按这个读法，把它们记成 `public_native` 本来就是错的记账，清空是修正而不是骗门。

  但按另一个读法——"sync 模块导出的 `Semaphore` 名字，其实现是原生的"——记 5 是对的。
  两种读法都成立，而清空会让 `manual_native_class_exports` 这个专门为它们建的扫描机制
  失去唯一消费者。**这是一个真正的判断题，证据强度不足以由使能 lane 单方面推翻前人的记账，
  留给能拍这个板的人。** 收益也只有 5。
- `io` / `os` 的 `public_native = []` **不是可照搬的先例**：这两个模块在 core.def 里
  一个 `native_class` / `object` / `enum` 都没有（实测计数 0），
  它们没有类型面要迁，不是把类型面迁走了。

## 6. 给三条 lane 的一句话

- **6-4（mem/Buffer）**：`Buffer` 现在能标 internal 也能在 `mem.xr` 里命名。
  但 `cluster.xr:18` 用 `move Buffer` 当参数类型——`Buffer` 是跨模块公开类型，
  靠 `xtype_ref_resolve.c:1191-1193` 的 well-known 硬编码解析，不靠本 lane 这条路。
  标 internal 前先想清楚那个 well-known 条目怎么办。
- **6-5（regex/RegexMatch）**：名字解锁了，`find` / `fullFind` / `findAll` 可以进 `.xr` 了。
  但 `RegexMatch` 是它们的返回类型，仍然是公开表面，**别标 internal**。
  `Regex.*` 的 7 个 `class_method` 是绑定层，标 internal 是诚实的，
  但要和 `regex.xr` 已有的 `export fn` 对齐后再动。
- **6-10（sys/net）**：`net` 的 11 个被两处硬编码钉死
  （`check_binary_public_native_readiness.py:26-42`、`check_stdlib_boundary.py:260-268`），
  两处内容**逐字相同**，都是**精确相等**比对，只改一处另一处会报 missing。
  把两处的 `"net"` 都改成 `set()` 是**变严**不是放松（任何加回来的符号立刻报 extra），
  方向与它们 docstring 里的意图一致。
  `check_stdlib_boundary.py:283-297` 那条「io/os/net 只能有 `__` 私有 fn」的约束
  **只管 `fn`**，不管 enum / object / native_class / class_method，
  而且要求这三个模块必须保留 core.def 的 module 块（不能删块）。
  另：`enum NetError` 是 io/os/net 那条 85% Xray 所有权比例里**唯一**的非 `.xr` 来源
  （实测 total 159 / xr 148 / 93.08%，11 条全是 `NetError` 及其 10 个 variant）；
  把它标 internal 会让比例变成 100%，但它同时是用户 `import { NetError }` 的名字。
  `sys` 的路见 §5。

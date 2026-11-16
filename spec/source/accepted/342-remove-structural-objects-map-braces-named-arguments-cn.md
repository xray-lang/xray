# 已接受目标规则：342 - 删除结构对象，`{}` 改为 Map 字面量，增加命名实参

> 2026-11-16 从任务文档迁入统一规范源。此处维护已接受的目标定义；原任务只维护实施、依赖、理由与验收记录。
> 迁入不表示实现完成。下面保留的示例是目标验收材料，不能据此声称当前编译器、VM 或 AOT 已支持。历史修订和已明确被取代的规则保持历史身份；不能将旧版本重新激活。
> 实施任务：[342-remove-structural-objects-map-braces-named-arguments-cn](../../../../xray-docs/tasks/342-remove-structural-objects-map-braces-named-arguments-cn.md)。

## 3. 语义

### 3.1 Map 字面量

1. **形式**：`{k1: v1, k2: v2}`，末尾逗号可写可不写。表达式位置的 `{` 开始 Map 字面量。
2. **键是任意表达式**：字符串键写 `"name"`；裸标识符取该名字绑定的值，`var`、`const` 一样；`Color.Red`、`1`、`f()`、`user.id` 等直接写。
3. **类型**：有类型上下文时按上下文（`Map<K, V>` 或 `JSON.Object`）；没有上下文时，全部键须为同一类型 K、全部值须为同一类型 V，否则报错。与 `[]` 需要类型上下文是同一条规则。
4. **类型不一致**：没有上下文、键或值找不到同一类型时报错，不自动推成联合类型。这样错误留在字面量上，笔误不会被联合类型悄悄吞掉，也不会推迟到使用处才暴露。诊断见 §3.7。在 `JSON.Object` 上下文中，标量值按现有 JSON 标量拓宽规则进入 `JSON.Value`。
5. **空字面量**：表达式位置的 `{}` 为空 Map，需要类型上下文。
6. **诊断**：裸标识符键未定义时，报“未定义的名字 `name`；如果想用字符串键，请写 `"name"`”；类型不符时在类型错误中附同样提示。LSP 对变量键与字面量键做语义高亮。
7. **求值顺序**：按书写顺序，每一项先键后值，各求值一次。
8. **哈希能力**：键类型须满足现有键等价关系（规范 §9.2），不新增规则。

### 3.2 块与字面量的区分

- 语句开头的 `{` 是块；表达式位置的 `{` 是 Map 字面量。
- `=>` 之后的 `{` 一律是块，包括箭头 lambda 与 `match` 分支；要直接返回 Map，写 `x => ({"a": 1})`。类型箭头与值箭头按 [Task343](../../../../xray-docs/tasks/343-type-arrow-and-value-arrow-cn.md) 分开。
- struct 字面量必须带类型名前缀：`Point{x: 1, y: 2}`，`x`、`y` 是字段名。`if (...)` 强制括号，因此不会出现 Rust 中 `if x == Point {..} {` 那种歧义。

### 3.3 Set

`#[...]` 删除。构造写 `Set([1, 2, 3])`，空集合写 `Set<i64>()` 或在类型上下文中写 `Set()`。

### 3.4 命名实参

1. **形式**：`f(a, b, name: value)`。位置实参在前，命名实参在后；出现命名实参后不能再写位置实参。
2. **名字**：取自被调声明的参数名；每个参数至多给一次（位置或命名）。未知名字、重复给出、位置实参出现在命名实参之后，均为编译错误。
3. **与默认参数**：没有给出的参数若有默认值，按现有规则在调用点补入默认表达式；没有默认值的参数必须给出。保留“有默认值的参数在尾部连续出现”的现有规则。
4. **适用范围**：与默认参数一致，只用于具名函数、方法、构造器的直接调用；经函数值的间接调用没有参数名，不能使用命名实参。rest 参数 `...args` 只能按位置给出。
5. **参数模式**：`ref` 写在值之前，`swap(left: ref a, right: ref b)`；`move` 同理。
6. **求值顺序**：接收者或被调表达式先求值，显式实参按书写顺序从左到右求值，最后按声明顺序计算被省略的默认值。与现有 E2 规则一致，只是“书写顺序”不再等于“参数顺序”。
7. **泛型推导**：按参数而不是按书写位置收集推导证据；不改变现有推导规则。
8. **API 兼容**：参数名成为公开 API 的一部分，导出函数改参数名属于不兼容修改。

### 3.5 重复键

1. **规则**：Map 字面量中两个显式项的键在键等价关系下相等，即为重复。字面量保证显式项的键互不相同；没有展开时，结果的 `len` 等于项数。
2. **编译期确定的键**：字符串、整数、rune、bool 字面量，无载荷 enum 变体，`comptime` 表达式，以及初值直接或经其他 `const` 传递地属于这些形式的 `const` 绑定。这个集合由语法决定，不取决于优化器是否折叠。重复的两个键都属于这一类时，是编译错误。
3. **运行期**：其余重复在构造时发现，产生 panic。检查复用插入操作本来就有的“键是否已存在”结果，不额外做一次哈希查询；两处源位置的来源存储与清理由构造器拥有，实际构造成本仍须按 §7 测量。
4. **与展开**：展开提供的项不受本规则约束，见 §3.6。
5. **诊断**：编译期为 `error: E0325: duplicate key "name" in Map literal`，主位置在后一个键，附 `note: the key first appears here` 指向前一个；运行期 panic 消息为 `duplicate key "name" in Map literal (entries at app.xr:3:12 and 3:24)`。编译错误码 E0325、运行期 panic 码 E0434，登记于 `src/shared/xerror_codes.h`。
6. **理由**：重复几乎总是复制粘贴的笔误，“后者覆盖”会静默丢掉一项。编译期能确定的在编译期报，不能确定的在运行期报，是同一条语义的两个时机。先例：Go 对常量键重复报编译错误，Swift 对重复键在运行时陷入。

### 3.6 展开

数组、tuple、Map、struct 四种字面量都支持 `...`，一律按书写顺序叠加。struct 的字段集是固定的，因此多一条“每个字段恰好得到一个值”的检查。

1. **Map 展开**：`{...defaults, ...overrides, "debug": true}`。
   - 展开源须是 Map，键值类型与结果类型一致（按现有转换规则）；
   - 展开提供的项与显式项按书写顺序写入，后写的覆盖先写的；
   - §3.5 的重复检查只在两个显式项之间进行，因为展开的用途就是覆盖。
2. **struct 展开**：`T{...src, f: v}`。
   - 展开源须是 struct 值；class 实例、Map、tuple 不能展开进 struct；
   - 展开源的每个字段都必须是 `T` 中同名、同类型的字段；来源有 `T` 没有的字段时报错，不静默丢弃；
   - `T` 的每个字段恰好得到一个值：优先来自显式字段，否则来自唯一一个提供它的展开源；两个展开源都提供某字段而没有显式字段时报错。没有显式值或来源的字段继续按规范 §5.4 的声明默认值与可空字段缺省规则处理；仍没有值的必需字段报错。展开不改变 struct 已有的字段缺省规则（更正见 §8）；
   - 展开源与显式字段的值按书写顺序各求值一次；
   - 成本是逐字段复制，与手写相同；值语义与 COW 不变。
3. **两种用途**：
   - 同类型更新：`Point{...p, x: 1}`；
   - 扩展为新类型：`Contact{...user, phone: "123-456-7890"}`，`Contact` 须声明全部字段。
4. **demo 迁移**：`demos/02-collections/json_processing.xr` 中的 `{ ...user, phone: "123-456-7890" }`，`user` 改为 struct `User` 后写成：
   ```xray
   struct Contact {
       name: string
       age: i64
       tags: Array<string>
       phone: string
   }
   var contact = Contact{...user, phone: "123-456-7890"}
   print("typed JSON: ${JSON.stringify(contact)}")
   ```
5. **理由**：对象展开的真实用途只有上面两种，demo 与 tests 都有；struct 展开都能表达，而目标类型始终写明、字段集由编译器检查。Rust 的 `..base` 只允许同类型，本设计多允许“字段子集来源”，以覆盖扩展用途，但不引入结构化类型。Map 展开对应 Python 的 `{**a, **b}` 与 JS 的对象展开，用于合并配置和 JSON 数据。

### 3.7 集合字面量类型不一致的诊断

适用于 Map 的键或值、数组元素（`Set([...])` 经数组字面量同样适用）：没有类型上下文且找不到同一类型时报错。诊断由一个错误、一条指向首个元素的 note 和按条件给出的修法 note 组成。编译器诊断沿用英文，统一使用 E0326。

| 部分 | 文本 | 位置 | 何时给出 |
|---|---|---|---|
| error | `E0326: Map literal values have different types: string and i64` | 第一个与首值类型不同的值 | 总是 |
| note | `the first value is string; without a declared Map type, every value must be string` | 首个值 | 总是 |
| note | `for a fixed set of fields, declare a struct: struct User { name: string; age: i64 }, then write User{name: "A", age: 30}` | 字面量 | 所有键都是合法标识符的字符串字面量时；类型名取绑定名首字母大写，没有绑定名时用 `Record` |
| note | `for JSON data, declare the type: var user: JSON.Object = {...}` | 绑定名 | 所有值都能进入 `JSON.Value` 时 |
| note | `to keep both types in one Map, declare Map<string, string \| i64>` | 绑定名 | 这些类型能组成合法联合时 |

- 键不一致：`Map literal keys have different types: string and i64`，修法只给“声明 `Map<K, V>`”。
- 数组元素不一致：`Array literal elements have different types: i64 and string`；元素个数固定时提示改用 tuple `(1, "a")`，能组成合法联合时提示声明 `Array<i64 | string>`。
- 修法的顺序按可能的意图排列：固定字段最常见，其次是 JSON 数据，最后才是混合类型的 Map。

渲染示例：

```text
app.xr:1:33: error: E0326: Map literal values have different types: string and i64
    1 | var user = {"name": "A", "age": 30}
      |                                 ^^
app.xr:1:21: note: the first value is string; without a declared Map type, every value must be string
app.xr:1:12: note: for a fixed set of fields, declare a struct: struct User { name: string; age: i64 }, then write User{name: "A", age: 30}
app.xr:1:5: note: for JSON data, declare the type: var user: JSON.Object = {...}
app.xr:1:5: note: to keep both types in one Map, declare Map<string, string | i64>
```

### 3.8 删除的表面

| 删除 | 规范位置 |
|---|---|
| 对象字面量 `{ field: v }`、字段简写 `{ x }` | §2.4.7、§3.10 |
| `type X = {...}` 对象形状、结构化身份、精确与宽度规则、对象形状的可空字段省略（struct 字面量的字段缺省规则属于 §5.4，不受影响） | §2.4.7、§2.10.1、§9.5 |
| 结构化泛型约束 `T: { f: T }` | §9.2、§9.5 |
| 对象展开 `{...base, x: 1}`（由 struct 与 Map 展开取代，见本文 §3.6） | §3.9 |
| 对象解构 `var { name, age } = obj` | §5.1.3 |
| 对象模式 | §6.9 |
| 对象上的 `["field"]` 访问、`typeName` 的 `"object"` | §2.4.7、§2.11 |
| `#{}` Map 字面量、`#[]` Set 字面量及其单 token | §1、§3.10、附录 A |

### 3.9 替代写法

| 原写法 | 新写法 |
|---|---|
| `request({ url: u })`，`HttpRequestOptions` 可空字段 | `request(url: u)`，可选项改为带默认值的参数；一组选项需要作为值保存或复用时，声明带字段默认值的 struct，写 `request(HttpRequestOptions{url: u})` |
| `return { min: a, max: b }` | `return (a, b)`，或返回 struct |
| `type _PeerState = { ... }` | `struct _PeerState { ... }` |
| `JSON.parse<UserShape>(text)` | `JSON.parse<User>(text)`，`User` 为 struct |
| `fn f<T: { name: string }>(v: T)` | `interface Named { name() -> string }`，`fn f<T: Named>(v: T)` |
| `#{"a": 1}` | `{"a": 1}` |
| `#[1, 2]` | `Set([1, 2])` |
| `{...base, x: 1}` | 同类型更新 `Point{...base, x: 1}`；扩展字段 `Contact{...user, phone: p}`（§3.6） |

### 3.10 正例

```xray
fn request(url: string, method: string = "GET", followRedirects: bool = true) -> string {
    return "${method} ${url} ${followRedirects}"
}
request("https://a")                               // 位置实参
request(url: "https://a", followRedirects: false)  // 命名实参，method 取默认值

const KEY = "name"
var a = {"name": "A", "tag": "x"}                  // Map<string, string>
var b = {KEY: 1, "age": 2}                         // KEY 取值 "name"
var c: JSON.Object = {"name": "A", "age": 30}      // 值拓宽为 JSON.Value
var d = {Color.Red: 1, Color.Green: 2}             // enum 键
var e: Map<string, i64> = {}                       // 空 Map

fn divmod(a: i64, b: i64) -> (i64, i64) { return (a / b, a % b) }
var (q, r) = divmod(17, 5)                         // 多字段返回用 tuple

var defaults = {"timeout": 30, "retries": 3}
var config = {...defaults, "retries": 5}           // 展开后覆盖：retries 为 5
struct Point {
    x: i64
    y: i64
}
const p = Point{x: 1, y: 2}
const moved = Point{...p, x: 10}                   // 同类型更新
```

### 3.11 反例

```xray
var m = {"name": "A", "age": 30}   // 错误：值类型不一致，提示标注 JSON.Object 或联合类型
var n = {}                          // 错误：空 Map 需要类型上下文
var o = {missing: 1}                // 错误：未定义的名字 missing；字符串键请写 "missing"
request(followRedirects: false, "https://a")   // 错误：位置实参不能出现在命名实参之后
request("https://a", url: "b")      // 错误：url 已按位置给出
const f = request
f(url: "https://a")                 // 错误：函数值调用不能使用命名实参
var p = {"x": 1}.x                  // 错误：Map 没有字段访问；静态数据请声明 struct
type Shape = { x: i64 }             // 错误：对象形状已删除，请声明 struct
var s = #[1, 2]                     // 错误：请写 Set([1, 2])
var t = {"a": 1, "a": 2}            // 错误：Map 字面量键重复（编译期确定）
const k = readKey()
var u = {k: 1, "a": 2}              // 若运行期 k == "a"：构造时 panic，键重复
const w = Point{...p, z: 1}         // 错误：Point 没有字段 z
```

## 9. 已决项（2026-11-14）

用户要求只按理论最佳设计、不考虑兼容性与工作量，原三个待定项决定如下：

1. **Map 字面量中的重复键**：D8，规则见 §3.5。不采用“后者覆盖”，因为它会静默丢数据。
2. **对象展开的替代**：D9，规则见 §3.6，同时提供 Map 展开与 struct 展开。
3. **类型不一致的诊断**：D10，文本与出现条件见 §3.7。编译错误码 E0325、运行期 panic 码 E0434，登记于 `src/shared/xerror_codes.h`。

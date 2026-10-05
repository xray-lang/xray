---
id: spec.14_built_in_type_methods
order: 015
---

<!-- xr-spec:cn -->
---

## 14. 内置类型方法 (Built-in Type Methods)

> 真值源：prelude / analyzer / runtime 中的内置类型注册与方法定义。
> MCP knowledge 只消费生成后的 analyzer metadata，不独立维护内置类型方法签名。

本节按主题汇总每种内置类型的方法、签名和行为。

### 14.1 `i64` 方法

| 方法 | 签名 | 说明 |
|--|--|--|
| `abs()` | `() -> i64` | 绝对值 |
| `toString()` | `() -> string` | 十进制字符串 |
| `toBigInt()` | `() -> BigInt` | 转 BigInt |
| `toF64()` | `() -> f64` | 转 f64 |
| `toHex()` | `() -> string` | 十六进制字符串 |
| `max(other)` / `min(other)` | `(i64) -> i64` | 双值最值 |
| `sqrt()` | `() -> f64` | 平方根 |
| `pow(exp)` | `(f64) -> f64` | 幂运算 |
| `checkedAdd(other)` / `checkedSub(other)` / `checkedMul(other)` | `(i64) -> i64?` | 溢出返回 `null` |
| `saturatingAdd(other)` / `saturatingSub(other)` / `saturatingMul(other)` | `(i64) -> i64` | 溢出饱和到 `i64` 边界 |
| `wrappingAdd(other)` / `wrappingSub(other)` / `wrappingMul(other)` | `(i64) -> i64` | 显式二补码环绕 |
| `addOverflows(other)` / `subOverflows(other)` / `mulOverflows(other)` | `(i64) -> bool` | 仅报告有符号溢出（要结果用 `checked*`） |
| `popcount()` | `() -> i64` | 二补码位表示中置位的个数 |
| `leadingZeros()` / `trailingZeros()` | `() -> i64` | 前导/后缀零比特数（`0` 返回 `64`） |
| `byteswap()` | `() -> i64` | 反转字节序 |
| `rotateLeft(n)` / `rotateRight(n)` | `(i64) -> i64` | 循环移位（`n` 按模 64） |

`abs()` 遵循整数环绕语义：`(-9223372036854775807 - 1).abs()` 返回自身。`toHex()` 对负数使用带符号前缀，例如 `-0x8000000000000000`。位运算与溢出谓词在 VM 与 AOT 中具有相同语义。

### 14.2 `f64` 方法

| 方法 | 签名 | 说明 |
|--|--|--|
| `abs()` | `() -> f64` | 绝对值 |
| `toString()` | `() -> string` | 字符串化 |
| `toFixed(decimals?)` | `(i64?) -> string` | 固定位数小数字符串 |
| `toI64()` | `() -> i64` | 转 i64 |
| `floor()` / `ceil()` / `round()` | `() -> i64` | 取整 |
| `sqrt()` | `() -> f64` | 平方根 |
| `pow(exp)` | `(f64) -> f64` | 幂运算 |
| `isNaN()` | `() -> bool` | 是否为 IEEE NaN |

### 14.3 `BigInt` 方法

| 方法 | 签名 | 说明 |
|--|--|--|
| `abs()` | `() -> BigInt` | 绝对值 |
| `toString()` | `() -> string` | 字符串化 |
| `sign()` | `() -> i64` | -1 / 0 / 1 |
| `isZero()` / `isNegative()` / `isPositive()` | `() -> bool` | 符号判断 |
| `toI64()` | `() -> i64?` | 无法表示时返回 null |
| `toF64()` | `() -> f64` | 转 f64 |

### 14.4 `bool` 方法

| 方法 | 签名 | 说明 |
|--|--|--|
| `toString()` | `() -> string` | 返回 `"true"` 或 `"false"` |

### 14.4.1 `rune` 方法

| 方法 | 签名 | 说明 |
|--|--|--|
| `toString()` | `() -> string` | 返回单 Unicode scalar 字符串 |
| `toUInt32()` | `() -> u32` | 返回 Unicode scalar code point |
| `isLetter()` | `() -> bool` | 是否为 Unicode 字母 |
| `isNumber()` | `() -> bool` | 是否为 Unicode 数字 |
| `isAlphanumeric()` | `() -> bool` | 是否为字母或数字 |
| `isWhitespace()` | `() -> bool` | 是否为空白字符 |

`rune` 是独立原始类型，不继承整数方法；需要码点时显式使用 `toUInt32()`。 `toUInt32()`／`i64(c)` 返回完整码点，不失败、不分配、不挂起；`toString()`／`string(c)` 产生拥有式 UTF-8，U+0000 保留长度为 1 的 NUL 字节。

### 14.5 `string` 方法

| 成员 | 类型 / 说明 |
|--|--|
| `len(s)` | O(1) Unicode scalar 数量 |
| `bytes()` / `copyBytes()` | 借用的 `Slice<u8>` / 独立的 `Array<u8>` |
| `runes()` | `Iterator<rune>`；裸 `for (r in s)` 使用相同语义 |
| `string.fromRune(r)` | 从一个 Unicode scalar 构造字符串 |
| `string.fromUtf8(bytes)` | 复制并严格验证 `Slice<u8>`；非法 UTF-8 抛 `Utf8Error.InvalidUtf8` |
| `string.fromUtf8Lossy(bytes)` | 复制 `Slice<u8>`，非法序列替换为 U+FFFD |
| `string.join(parts, separator?)` | 拼接 `Array<string>` |
| `contains(s)` | 是否包含子串 |
| `indexOf(s, start?)` / `lastIndexOf(s)` | 返回 rune ordinal |
| `slice(start, end?)` | 按 rune ordinal 取得独立 string；范围必须合法 |
| `sliceBytes(start, end)` | 按 byte offset 切片；边界非法时抛 `StringSliceError.InvalidByteRange` |
| `split(sep, limit?)` | 分割为 `Array<string>` |
| `replace(from, to)` / `replaceAll(from, to)` | 替换 |
| `repeat(n)` | 重复 |
| `startsWith(s)` / `endsWith(s)` | 前缀/后缀判断 |
| `toString()` | 返回自身 |

`len(s)` 读取已验证 UTF-8 的 Unicode scalar 计数并返回 i64，空串为 0，内嵌 NUL 计为一个 scalar。查询本身不分配；实参按普通表达式只求值一次，仍保留其求值、初始化和持有失败。`==`/`!=` 接受两个 string，按完整字节长度和内容比较，不依赖对象身份，不做大小写折叠或 Unicode 归一化；组合字符与预组合字符可以不相等。左右操作数按从左到右的值快照语义求值。普通无约束泛型不能借此获得字符串或 Lengthable 能力。

`contains`、`startsWith`、`endsWith` 接受一个 string 并返回 bool，按完整 UTF-8 字节查询，不做大小写折叠或 Unicode 归一化，NUL 不终止匹配。空模式对任意字符串返回 true，非空模式对空字符串返回 false。接收者先求值并取得值快照，再求值模式，各一次；实参求值期间修改原变量不改变快照。查询本身不分配、不修改或转移输入，不引入挂起或语言异常；操作数求值及持有仍可失败。普通泛型必须在定义处具备相应能力。

`indexOf(search, start?)` 的 start 是 rune 序号，省略为 0，合法范围为 `0..len(receiver)`（含两端）；负数或超界触发 bounds panic，不截断。查询返回从 start 开始的首个匹配的 rune 序号，未命中为 -1，空模式返回 start。`lastIndexOf(search)` 返回最后匹配的 rune 序号，未命中为 -1，空模式返回 `len(receiver)`。匹配按完整 UTF-8 字节，不归一化；rune 是 Unicode scalar，不是 grapheme。接收者、模式、start 按该顺序各求值一次并保留值快照。查询自身无分配、不挂起、不改变所有权；实参求值/持有失败照常传播。Unicode 坐标转换可能扫描前缀；连续查询不承诺恒定时间。

string 的 `<`、`<=`、`>`、`>=` 比较完整显式长度的 UTF-8 无符号字节字典序；嵌入 NUL 参与比较，相同前缀后较短字符串较小。不使用 locale、归一化或 grapheme 排序。

string 不支持整数下标或 slice operator；显式使用 `s.runes().nth(i)`、`s.bytes()[i]` 或 `s.slice(start, end)`。字符串拼接使用 `+`；大小写、去空白、填充和反转等 Unicode 文本操作属于 `text` 模块。

### 14.6 `Array<u8>`

`Array<u8>` 是可直接使用的 `Array` 具体化，构造由 `Array<u8>(n)` / `Array<u8>(n, fill)` 等内置路径处理。它的 `toString()` 与所有 Array 一样返回容器格式；文本解码必须显式使用 `string.fromUtf8(bytes[:])` 或 `string.fromUtf8Lossy(bytes[:])`。当前没有单独的 `stdlib/types/bytes.xr` 声明；工具不要把它当成另一套与 Array 同构的独立 API。

### 14.7 `Array<T>` 方法

`Array<T>` 是可增长值类型，T 必须可复制且可保存。普通赋值、保存参数、索引读和返回遵循逻辑副本语义；共享 backing 不授予修改其他副本的权限。容器存储本身不要求 T 可比较、可哈希或可字符串化，相关方法另按各自合同准入。

首个拥有式数组执行子集冻结以下接口：`len(array: Array<T>) -> i64`、只读 `get(index: i64) -> T`、`ref set(index: i64, value: T) -> ()`、`ref push(value: T) -> ()`，以及数组字面量和 §3.4/§3.11 的索引表达式。`set` 方法返回 unit；索引赋值表达式返回右侧转换后的 T。len 自身不分配；读取/复制可能遇到持有计数限制；构造和写入可能因预算、分配或持有限制失败。set 和 push 都可能因 COW 分离而迁移元素存储。

只读 get 与索引读取遵循 §3.0 的值 receiver 快照规则：先取得 Array 逻辑快照，再求 index；不能因 index 中的改绑而晚读当前值。ref set/push 则先绑定根 place 一次，全部实参求值完成后才对 binding 的当前值建立独占访问，执行边界检查、分离、写入和回写。实参改绑同一 root 时，操作作用于新值；set 按新长度检查。`a.push(a[0])` 合法，元素结果必须在 push 前独立持有。失败不发布部分数组或改变提交前的逻辑值；已经完成的实参副作用和改绑不回滚。

普通 read 参数不能成为这些 ref 方法的可写 receiver；可复制 Array 不声明 move receiver，pop 删除元素也不消费整个 Array。下表的 ref 是声明模式，dot-call 仍写 `a.push(x)`。纯索引 GET-place 的快照消除及瞬时 retain/copy/drop 必须遵守 §3.0 的错误保持要求，不能以共享 backing 的唯一性猜测绕过访问权限。

```xray @id=array-ref-current-binding
var a: Array<string> = ["old"]
fn replaceForPush() -> string {
    print("arg")
    a = ["new"]
    return "tail"
}
a.push(replaceForPush())
print(a[0], a[1])           // 依次输出 arg、new tail
```

```xray @id=array-push-own-element
var a: Array<string> = ["head"]
a.push(a[0])
print(a[0], a[1])           // head head
```

下表保留完整方法分母。当前冻结的 XIR 边界为 get/set/push 三原语加 map/filter/reduce/forEach/find/findIndex/every/some/contains/indexOf/join/clear 十二个声明 recipe及reverse/unshift/pop/shift，共 19 个 operation；32 个成员中其余 13 个仍未准入。执行身份来自 `stdlib/types/array.xr` 的同源结构化声明，名字只用于成员查找。capacity 是显式可观察属性，其增长与分离保证须在该属性接入时冻结；不能把所有容量行为称为不可观察的优化。

这些 READ 回调/查询先取得 receiver 的拥有式快照，再按源码顺序各求实参一次；reduce 的顺序为 receiver、callback、initial。map/filter/forEach 按递增索引执行，可接受声明允许的省略尾 index 回调；find/findIndex/every/some 按单元素 bool 回调短路。空 reduce 返回 initial，空 every 为 true，空 some/contains 为 false，无匹配 find 为 none、findIndex/indexOf 为 -1。contains/indexOf 在定义处要求 T:Equal；join 本片只支持 string/bool/rune/已准入数值，separator 默认为空字符串并保留 UTF-8/NUL 字节。ref clear 使用既有可写 place 与写回合同，复制值保持独立。回调继续使用普通间接调用的 throw/panic/suspend/取消与所有权管线，不能从未来实例补足约束。本段冻结操作合同，完整回归、安全、OOM 与物理释放资格以实际批次证据为准。

下一声明片冻结 `ref unshift(value: T)`（普通 READ 的精确 T，返回 unit）和 `ref reverse() -> Array<T>`（零参数，拥有式结果）；冻结不表示已准入或验证。两者只要求 Array 原有的可复制、可保存能力，不额外要求 Equal、Compare 或 ToString；普通泛型在定义处检查，不从未来实例补权限。receiver 必须是可写逻辑 place，root/path 的选择器只求一次；unshift 实参成功后读取 binding 的当前 Array，实参的已完成副作用不回滚。负整数 T 是普通元素，不是长度参数。

两方法先准备独立拥有式候选，reverse 取反向排列，unshift 将输入置于原元素之前；空 reverse 返回空 Array，空 unshift 得到单元素 Array。元素复制在 class 身份处停止。reverse 的结果拥有关系在最终单次回写前准备完成，发布后结果与 receiver 是独立逻辑副本；方法准备或发布失败、发布前取消不改变提交前的逻辑值，临时拥有关系正常释放。方法发布后的后续语句、整个函数的结果交接失败或取消遵循普通调用规则，不撤销已完成的方法副作用；内部循环不新增语言挂起点或用户回调。私有候选构造可能重选 backing 容量，不能声称保持旧容量；capacity/withCapacity/reserve 的公开增长与分离保证仍在该声明族准入时另冻。

冻结 `ref pop() -> T?` 与 `ref shift() -> T?`：零显式、可选、默认、variadic及方法类型参数，分别移除尾／头元素，receiver不被消费。空数组返回外层None且不发布Array写入；非空取得精确T的独立拥有式逻辑副本，再包一层Some。T本身Nullable时不展平：Array<i64?>删除null得到Some(None):i64??，删除7得到Some(Some(7)):i64??，空数组才得到None:i64??。不增加Some/None源码构造语法或Equal/Compare/ToString/NotNullable要求；普通泛型定义处检查，Checked特化后复验。

receiver须为已有权限的可写逻辑place；root/path选择器只求值一次，路径完成后读取绑定当前Array，已完成路径副作用不回滚。const/read Array值参数、临时Array、不可访问或const字段拒绝；read class handle的可变Array字段沿用既有class身份与字段权限。非空先准备完整拥有式Nullable结果及私有Array候选（pop保前n−1项，shift保后n−1项原序），再只向真实place发布一次。复制在class或同步身份处停止，结果不借backing内部地址。所有可失败的方法结果持有／cell读取在发布前完成；准备／发布失败和发布前取消保全提交前逻辑值、别名及身份，正常释放临时拥有关系。发布后的普通父表达式、函数结果交接失败或取消不撤销方法副作用。内部循环不增加语言挂起点或用户回调；空路径无元素读取／发布，但不承诺零分配或免于准入、持有及预算失败。

pop/shift资格为allocation=may_heap、failures=allocation,retain,limit、ownership=owned，无callback效应。候选可重选backing容量，不保证旧capacity；capacity/withCapacity/reserve的公开增长／分离保证仍属其声明族另冻，与reverse/unshift一致。合同冻结不等于新操作已经通过实施验收。

冻结下一声明族 `ref resize(length: i64, fill: T) -> Array<T>`，两个必需READ实参按length、fill各一次，无默认／可选／variadic／方法类型参数，length精确i64、fill精确T。只要求原可复制／可保存能力，不加Equal/Compare/ToString/NotNullable；普通泛型定义处检查并在Checked特化后复验。receiver为有权限的可写逻辑place，选择器仅一次，全部实参成功后读binding当前Array，已完成副作用不回滚；const/read Array值参数、临时值及无权字段拒绝，read class handle可变字段沿既有权限。

n≥0保前min(oldLength,n)项，增长项为fill逻辑副本，结果长度n；返回拥有式Array与发布receiver为独立逻辑副本，旧alias保旧值，复制在class／同步身份处停止且保Nullable层。n=0返回拥有空Array，同长度不承诺零分配或backing不变。n<0在两个实参后经现有NUMERIC_RANGE/E0422 panic拒绝且无发布；不可表示尺寸及有限额度沿现有LIMIT／预算拒绝。候选允许重选容量，capacity/withCapacity/reserve观察保证另冻。

完整候选、结果cell读取和可失败持有先于唯一PLACE_WRITE；准备／发布失败及提交前取消保当前root／alias／identity并清临时owner，发布后父表达式／返回交接失败或取消不回滚方法副作用。内部无用户回调／语言挂起／跨挂起loan，外部量子取消按真实提交点；qualification=allocation:may_heap,failures:allocation,retain,limit,ownership:owned。合同冻结尚未开放resize，当前19/32不变；VM/native／两mixed固定预期、所有准入元素类、真实故障／三轴／逃逸结果与两域物理释放完成后才报告20/32。

| 成员 | 类型/说明 |
|--|--|
| `len(arr)` | `i64` 全局查询 |
| `capacity` / `arr[i]` / `arr[i] = v` | 只读容量查询、只读索引与可写 place 赋值；get 为只读，set 为 ref |
| `ref push(x)` / `ref pop()` | 尾部增删；pop 不是 move receiver |
| `ref shift()` / `ref unshift(x)` | 头部增删 |
| `concat(...arrays)` | 只读 receiver，产生拼接结果 |
| `indexOf(x)` / `contains(x)` | 只读查询 |
| `join(sep?)` | 只读 receiver，拼接为字符串 |
| `ref reverse()` / `ref sort(cmp?)` | 修改 receiver 的排列 |
| `map(fn)` / `filter(fn)` / `reduce(fn, init)` | 只读快照；map/filter 的元素回调可带 index，reduce 为 `fn(U,T)->U` |
| `forEach(fn)` / `find(fn)` / `findIndex(fn)` / `every(fn)` / `some(fn)` | 只读快照；forEach 可带 index，其余为 `fn(T)->bool` 的短路查询 |
| `ref fill(v, start?, end?)` / `ref clear()` | 填充或清空 |
| `ref reserve(capacity)` / `ref resize(length, fill)` | 容量与长度管理 |
| `ptr()` / `mutPtr()` | 返回借用的合同须另冻；mutPtr 不能以普通只读 receiver 授予可写访问 |
| `toString()` | 只读 receiver，容器字符串表示 |
| `iterator()` / `entriesIterator()` / `entries()` | 只读 receiver；这些方法的结果类型与所有权须在方法准入前另冻，不与已冻结的 Array for-in 快照混同 |

Array for-in 的集合表达式求值一次并持有拥有式逻辑值快照。源绑定的元素替换、追加或重绑不改变本次序列；获取元素形成拥有式值副本。复制在 class 身份处停止，修改被引用对象仍可被其它同身份引用观察。每轮绑定不可变，空数组执行零次 body，continue 先清理本轮再前进一次；挂起保留快照与索引，取消释放所有拥有关系。普通泛型的 `Array<T>` 可按定义处已知形状遍历，任意 `T` 不因某实例恰为 Array 获得迭代权限。iterator 方法族、其它集合和借用视图的准入分别验证，不能以快照遍历替代它们。

Array 没有 `slice()` / `splice()` / `flat()` / `copyWithin()` 方法。`arr[start:end]` 产生借用的 `Slice<T>`，必须有显式目标类型并遵守 §2.4.2 的借用规则；需要独立数据时使用 `copy(arr[start:end])`。

### 14.8 `Map<K, V>` 方法

| 成员 | 类型/说明 |
|--|--|
| `len(m)` | `i64` 全局查询 |
| `m[k]` / `m[k] = v` | 下标读写 |
| `get(k)` / `set(k, v)` | `get` 在缺失时返回 `null`；`set` 写入 |
| `containsKey(k)` / `containsValue(v)` / `delete(k)` / `clear()` | 查询与删除 |
| `keys()` / `values()` / `entries()` | 返回键、值、键值对 |
| `forEach(fn)` | 遍历 |
| `iterator()` / `entriesIterator()` | 迭代协议 |

**Map 字面量**：`#{"k1": v1, "k2": v2}` 或 `#{}`；使用 `:`，靠 `#` 前缀区别于精确结构对象字面量 `{ field: value }`。

`m[k]` 要求键存在；缺失键触发运行时错误 `E0431`。需要可选读取时使用 `m.get(k)`。

### 14.9 `Set<T>` 方法

| 成员 | 类型/说明 |
|--|--|
| `len(set)` | `i64` 全局查询 |
| `add(x)` / `contains(x)` / `delete(x)` | 插入、查询、删除 |
| `clear()` | 清空 |
| `values()` | 返回 `Array<T>` |
| `forEach(fn)` | 遍历 |
| `iterator()` | 迭代协议 |

**Set 字面量**：`#[1, 2, 3]` 或 `#[]`。

### 14.10 `Channel<T>` 方法

| 成员 | 类型/说明 |
|--|--|
| `send(v)` | 阻塞发送；channel 已关闭时抛异常 |
| `recv()` | 阻塞接收，返回 `Recv<T>`；关闭且缓冲为空时为 `Recv.Closed` |
| `recvOr(default)` | 接收 payload；没有值时返回给定默认值 |
| `trySend(v)` | 非阻塞发送，返回 `SendResult` |
| `tryRecv()` | 非阻塞接收，返回 `Recv<T>`；空时为 `Recv.Empty` |
| `sendTimeout(v, ms)` | 带超时发送，返回 `SendResult`；超时为 `SendResult.Timeout` |
| `recvTimeout(ms)` | 带超时接收，返回 `Recv<T>`；超时为 `Recv.Timeout` |
| `close()` | 关闭 channel |
| `capacity` / `isClosed` | 容量和关闭状态属性 |

`Recv.Value { value: v }` 中的 `v` 就是 channel payload，因此 `Channel<i64?>` 可以区分真实的 `Recv.Value { value: null }` 和 `Recv.Closed`。

### 14.11 `JSON` 命名空间

`JSON` 是 prelude 命名空间，不是可声明变量的值类型，也不需要 `import json`。schema-less JSON 使用 `JSON.Value`；确定为 object 的动态 JSON 使用 `JSON.Object`。`JSON.Object` 是 `Map<string, JSON.Value>` 的纯别名，因此枚举、动态下标、增删字段和 `len` 都直接使用 §14.8 的 Map API。

`JSON.Value` 是 `null | bool | i64 | f64 | string | Array<JSON.Value> | JSON.Object` 的递归边界值。它没有 dot、下标、迭代或 `len` 魔法：先用 typed decode 提交 schema，用 `asObject` / `asArray` 显式解包，或用 path API 访问任意深度。

| 静态函数 | 说明 |
|--|--|
| `JSON.parse<T>(text, unknown?)` | 直接按 `T` 构造 typed value；未知字段默认 `Reject`，可显式传 `JSON.UnknownFields.Ignore` |
| `JSON.parseObject(text)` / `JSON.parseValue(text)` | 分别解析 object 根或任意 JSON 根，不构造结构对象中间层 |
| `JSON.parseWithRest<T>(text, nestedUnknownFields?)` | 构造已知字段 `value: T`，并将顶层未知字段保留在 `rest: JSON.Object` |
| `JSON.decode<T>(value, unknown?)` / `JSON.decodeObject<T>(object, unknown?)` | 从已有 schema-less 值尝试构造 `T`，失败返回 `null` |
| `JSON.value<T>(value)` | 显式把可编码的复合值物化为 `JSON.Value` |
| `JSON.stringify<T>(value, indent?)` | 序列化可编码值；`indent` 必须是非空 `i64` |
| `JSON.isValid(text, strict?)` | 校验 JSON 文本 |
| `JSON.kindOf(value)` / `JSON.isNull/isBool/isInt/isFloat/isString/isArray/isObject(value)` | 查询 JSON arm |
| `JSON.asObject(value)` / `JSON.asArray(value)` | 返回共享底层存储的可空 object/array 视图，不复制 |
| `JSON.get<T>(root, path)` / `JSON.require<T>(root, path)` | 按 `JSON.Path` 读取并解码；前者失败返回 `null`，后者抛 `JSON.PathError` |
| `JSON.containsPath(root, path)` | 判断完整路径是否存在 |
| `JSON.set(root, path, value, createParents?)` / `JSON.remove(root, path)` | 修改或删除路径；路径可穿过 object key 与 array index |
| `JSON.merge(parts)` | 将 `JSON.WithRest<T>` 的 typed 部分和顶层 rest 重新组成 `JSON.Object` |

`JSON.Path` 是 `Array<string | i64>`：string segment 是完整 object key，i64 segment 是 array index。`["user", "profile", "name"]` 表示深层字段；`"user.profile"` 只是一个含点号的 key，不会解析成三段。动态 object key 也可直接使用 `JSON.Object` 的 Map 下标和方法。

```xray @id=json-boundary-and-path
type Request = { action: string, userId: i64 }

var request = JSON.parse<Request>(body)       // 未知字段默认拒绝
var payload = JSON.parseObject(body)          // JSON.Object，即 Map
payload["traceId"] = "req-42"               // 标量隐式 widening

var path: JSON.Path = ["user", "profile", "name"]
var name = JSON.get<string>(payload, path)
JSON.set(payload, path, "Ada", true)
```

JSON 标量 `null`、`bool`、`i64`、`f64`、`string` 可在有明确 `JSON.Value` 目标时隐式 widening；结构对象、数组和其他复合值必须写 `JSON.value(...)`。因此 `{ name: "alice" }` 始终是精确结构对象，不会因上下文悄悄变成动态 object。

### 14.12 `Range`

`a..b` 是半开区间 `[a, b)`，`a..=b` 是闭区间 `[a, b]`；两种范围都可用于表达式、`for-in` 和 `match` 范围模式。

| 成员 | 说明 |
|--|--|
| `start` / `end` | 起点与声明的终点 |
| `contains(x)` | 按半开或闭区间语义判断 `x` 是否在范围内 |
| `toArray()` | 按迭代顺序生成独立的 `Array<i64>` |
| `toString()` | 返回 `a..b` 或 `a..=b` 形式的字符串 |
| `iterator()` | 迭代协议；惰性产出与 `toArray()` 相同的元素序列 |
| `len(range)` | 返回范围中的元素数量 |

```xray @id=range-members
var pages = 1..=3
print(pages.start)          // 1
print(pages.end)            // 3
print(pages.contains(3))    // true
print(len(pages))           // 3
print(pages.toArray())      // [1, 2, 3]

var empty = 5..5
print(len(empty))           // 0
```

### 14.13 `DateTime`

通过 `import datetime` 获得工厂函数：`now`、`utc`、`create`、`createUTC`、`fromTimestamp`、`fromTimestampMs`、`parse`、`offset`。`DateTime` 不是 prelude 类型；需要类型名时使用 `import { DateTime } from datetime`。

| 成员 | 类型/说明 |
|--|--|
| `year` / `month` / `day` | 日期分量属性 |
| `hour` / `minute` / `second` / `millisecond` | 时间分量属性 |
| `weekday` / `yearday` / `timestamp` | 派生属性 |
| `toString()` / `format(pattern?)` / `toISOString()` | 格式化 |
| `add(amount, unit)` / `diff(other, unit?)` | 日期运算 |
| `toUTC()` / `toLocal()` | 时区转换 |
| `isBefore(other)` / `isAfter(other)` / `equals(other)` | 比较 |
| `isLeapYear()` / `daysInMonth()` | 日历查询 |

### 14.14 `Regex`

| 方法 | 说明 |
|--|--|
| `test(s)` | 是否匹配 |
| `find(s)` | 首个匹配 |
| `findAll(s)` | 所有匹配 |
| `findText(s)` / `findGroup(s, index)` | 首个匹配文本 / 捕获组文本 |
| `replace(s, replacement)` | 替换 |
| `split(s, limit?)` | 分割 |

### 14.15 `StringBuilder`

| 方法 | 说明 |
|--|--|
| `len(builder)` | 当前 rune 数量 |
| `append(s)` | 追加并返回自身 |
| `toString()` | 输出字符串 |
| `clear()` | 清空并返回自身 |

### 14.16 `PanicInfo`

内置 `PanicInfo` 类包含 `message`、`stack`、`cause`、`code`、`data` 字段，构造函数 `constructor(message: string = "", cause: PanicInfo? = null)`，以及 `toString()`。

### 14.17 `Task<T>` 与 enum 值

`Task<T>` 属性：`done`、`status`；方法：`cancel()`、`poll()`、`awaitResult()`、`awaitTimeout(ms)`。`poll()` 和显式等待方法返回 `TaskResult<T>`：`Success(T)`、`Failed(PanicInfo)`、`Cancelled`、`Timeout`、`Pending`。plain `await task` 成功时返回 `T`，失败或取消时走对应错误/panic 路径；结果必须可复制且 Sendable；Task 保留完整拥有式粘滞终态，重复 await 各取得逻辑副本，复制在 class 与同步身份边界停止，不使用 `await (move task)`，也不依引用计数选择单次 take。enum 值提供冷路径属性 `name`、`ordinal` 与方法 `toString()`。

### 14.18 线程与同步 handle

`Thread<T>` 是 prelude handle 类型，公开 `done` 属性以及 `join()`、`detach()` 方法。导入 `sys` 后，使用 `sys.Thread.spawn(body)` 或 `sys.Thread.spawn(ThreadOptions{...}, body)` 创建 OS 线程，并对返回的 handle 调用 `join()` 或 `detach()`。`CountdownLatch`、`EventCount`、`ResultGroup`、`Semaphore`、`WorkQueue` 等类型从 `sync` 模块导入；`Logger` 从 `log` 模块导入；连接与监听器类型从 `net` 模块导入。

### 14.19 `Atomic<T>` 方法

`Atomic<T>` 包装 `i64`、`f64` 或 `bool`，提供无锁原子操作。句柄以 `const` 命名；受审计原子方法提供同步内部修改。

| 方法 | 签名 | 说明 |
|--|--|--|
| `load(ord?)` | `(Ordering?) -> T` | 原子读取当前值 |
| `store(val, ord?)` | `(T, Ordering?) -> ()` | 原子写入 |
| `add(val, ord?)` | `(T, Ordering?) -> ()` | 原子加 |
| `sub(val, ord?)` | `(T, Ordering?) -> ()` | 原子减 |
| `fetchAdd(val, ord?)` | `(T, Ordering?) -> T` | 原子加并返回旧值 |
| `fetchSub(val, ord?)` | `(T, Ordering?) -> T` | 原子减并返回旧值 |
| `swap(val, ord?)` | `(T, Ordering?) -> T` | 原子交换，返回旧值 |
| `compareExchange(expected, desired, ord?)` | `(T, T, Ordering?) -> (T, bool)` | CAS，返回 `(旧值, 是否成功)` |
| `toggle(ord?)` | `(Ordering?) -> bool` | 原子取反（仅 bool），返回旧值 |
| `toString()` | `() -> string` | 返回当前值的字符串表示 |

`ord?` 参数接受 `Ordering` 枚举，默认 `Ordering.SeqCst`。详见 §10.9。

整族约束与操作准入按 §9.2/§10.9：类型声明要求 `T: AtomicValue`，数字四方法分别要求方法 `where T: AtomicNumber`，toggle 要求 `where T: AtomicBoolean`，其他方法不因这些附加条件缩小 AtomicValue 域。load/store 不接受非法内存序；compareExchange 返回拥有式真 Tuple，副作用前完成全部结果资源准备；f64 位型/CAS 与软件 RMW、i64 环绕和一次快照 toString 均沿 §10.9。当前 `stdlib/types/atomic.xr` 的无约束声明及固定 i64 指令仍待完整原子迁移，不代表本冻结合同已实现。
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 14. Built-in Type Methods

> Source of truth: prelude / analyzer / runtime built-in type registration and method definitions.
> MCP knowledge only consumes the generated analyzer metadata; it does not maintain its own copy of built-in method signatures.

This section summarizes the methods, signatures, and behavior of each built-in type by topic.

### 14.1 `i64` Methods

| Method | Signature | Description |
|--|--|--|
| `abs()` | `() -> i64` | absolute value |
| `toString()` | `() -> string` | decimal string |
| `toBigInt()` | `() -> BigInt` | convert to BigInt |
| `toF64()` | `() -> f64` | convert to f64 |
| `toHex()` | `() -> string` | hexadecimal string |
| `max(other)` / `min(other)` | `(i64) -> i64` | binary max/min |
| `sqrt()` | `() -> f64` | square root |
| `pow(exp)` | `(f64) -> f64` | power |
| `checkedAdd(other)` / `checkedSub(other)` / `checkedMul(other)` | `(i64) -> i64?` | returns `null` on overflow |
| `saturatingAdd(other)` / `saturatingSub(other)` / `saturatingMul(other)` | `(i64) -> i64` | clamps overflow to the `i64` boundary |
| `wrappingAdd(other)` / `wrappingSub(other)` / `wrappingMul(other)` | `(i64) -> i64` | explicit two's-complement wrap |
| `addOverflows(other)` / `subOverflows(other)` / `mulOverflows(other)` | `(i64) -> bool` | reports signed overflow only (use `checked*` for the value) |
| `popcount()` | `() -> i64` | number of set bits in the two's-complement representation |
| `leadingZeros()` / `trailingZeros()` | `() -> i64` | leading/trailing zero bit count (`0` yields `64`) |
| `byteswap()` | `() -> i64` | reverses the byte order |
| `rotateLeft(n)` / `rotateRight(n)` | `(i64) -> i64` | bit rotation (`n` taken modulo 64) |

`abs()` follows integer wrap semantics: `(-9223372036854775807 - 1).abs()` returns itself. `toHex()` keeps a sign prefix for negative values, for example `-0x8000000000000000`. Bit-manipulation methods and overflow predicates have the same semantics in VM and AOT builds.

### 14.2 `f64` Methods

| Method | Signature | Description |
|--|--|--|
| `abs()` | `() -> f64` | absolute value |
| `toString()` | `() -> string` | string conversion |
| `toFixed(decimals?)` | `(i64?) -> string` | fixed-decimal string |
| `toI64()` | `() -> i64` | convert to i64 |
| `floor()` / `ceil()` / `round()` | `() -> i64` | rounding |
| `sqrt()` | `() -> f64` | square root |
| `pow(exp)` | `(f64) -> f64` | power |
| `isNaN()` | `() -> bool` | whether the value is IEEE NaN |

### 14.3 `BigInt` Methods

| Method | Signature | Description |
|--|--|--|
| `abs()` | `() -> BigInt` | absolute value |
| `toString()` | `() -> string` | string conversion |
| `sign()` | `() -> i64` | -1 / 0 / 1 |
| `isZero()` / `isNegative()` / `isPositive()` | `() -> bool` | sign predicates |
| `toI64()` | `() -> i64?` | returns null when not representable as `i64` |
| `toF64()` | `() -> f64` | convert to f64 |

### 14.4 `bool` Methods

| Method | Signature | Description |
|--|--|--|
| `toString()` | `() -> string` | returns `"true"` or `"false"` |

### 14.4.1 `rune` Methods

| Method | Signature | Description |
|--|--|--|
| `toString()` | `() -> string` | return a one-Unicode-scalar string |
| `toUInt32()` | `() -> u32` | return the Unicode scalar code point |
| `isLetter()` | `() -> bool` | whether the scalar is a Unicode letter |
| `isNumber()` | `() -> bool` | whether the scalar is a Unicode number |
| `isAlphanumeric()` | `() -> bool` | whether the scalar is a letter or number |
| `isWhitespace()` | `() -> bool` | whether the scalar is whitespace |

`rune` is an independent primitive type and does not inherit integer methods; use `toUInt32()` explicitly when the code point is needed. `toUInt32()` / `i64(c)` return the complete code point without failure, allocation or suspension. `toString()` / `string(c)` produce owned UTF-8; U+0000 preserves one NUL byte with length one.

### 14.5 `string` Methods

The next declaration slice freezes `ref unshift(value: T)` (one ordinary READ argument of exactly T, returning unit) and `ref reverse() -> Array<T>` (no explicit arguments, an owned result). Freezing does not establish admission or qualification. Both require only the existing copyable/storable Array element capabilities, without Equal, Compare or ToString. Ordinary generic definitions are checked under their declared capabilities; future instances grant no missing authority. The receiver must be a writable logical place whose root/path selectors are evaluated once. unshift reads that binding's current Array after its argument succeeds; completed argument side effects are not rolled back. A negative integer T is an ordinary element, not a length argument.

Both methods prepare a private owned candidate: reverse reverses the element order, while unshift places the input before the original elements. Empty reverse returns an empty Array; unshift on an empty Array creates one element. Element copying stops at class identity. reverse prepares its result ownership before the final single writeback; its result and receiver are independent logical copies after publication. Failure during method preparation/publication, or cancellation before publication, preserves the pre-commit logical value and releases temporary ownership normally. Later statements, whole-function result handoff failures, and cancellation after method publication follow ordinary call semantics without undoing completed method effects. Internal loops introduce no language suspension point or user callback. Candidate construction may choose a different backing capacity and does not promise to preserve the old capacity; public growth/detachment guarantees for capacity/withCapacity/reserve remain subject to their separate declaration-family admission.

| Member | Type / Description |
|--|--|
| `len(s)` | O(1) Unicode scalar count |
| `bytes()` / `copyBytes()` | borrowed `Slice<u8>` / independent `Array<u8>` |
| `runes()` | `Iterator<rune>`; bare `for (r in s)` has the same semantics |
| `string.fromRune(r)` | constructs a string from one Unicode scalar |
| `string.fromUtf8(bytes)` | copies and strictly validates a `Slice<u8>`; invalid UTF-8 throws `Utf8Error.InvalidUtf8` |
| `string.fromUtf8Lossy(bytes)` | copies a `Slice<u8>`, replacing invalid sequences with U+FFFD |
| `string.join(parts, separator?)` | joins an `Array<string>` |
| `contains(s)` | substring containment test |
| `indexOf(s, start?)` / `lastIndexOf(s)` | return rune ordinals |
| `slice(start, end?)` | independent rune-ordinal slice; the range must be valid |
| `sliceBytes(start, end)` | slice by byte offset; invalid boundaries throw `StringSliceError.InvalidByteRange` |
| `split(sep, limit?)` | split into `Array<string>` |
| `replace(from, to)` / `replaceAll(from, to)` | replacement |
| `repeat(n)` | repeat |
| `startsWith(s)` / `endsWith(s)` | prefix/suffix check |
| `toString()` | return self |

`len(s)` reads the cached Unicode scalar count of validated UTF-8 and returns i64; the empty string has length zero and embedded NUL counts as one scalar. The query itself does not allocate; its argument evaluates exactly once with ordinary evaluation, initialization and retention failures. `==`/`!=` require two strings and compare their full byte lengths and contents, independently of object identity, with no case folding or Unicode normalization. Combining and precomposed sequences can differ. Operands evaluate left to right as value snapshots. These operations grant no string or Lengthable capability to an unconstrained generic definition.

`contains`, `startsWith`, and `endsWith` accept one string and return bool over complete UTF-8 bytes, without case folding or normalization; NUL does not end matching. Empty patterns match every string; nonempty patterns do not match empty strings. The receiver evaluates once into a value snapshot before the pattern evaluates once; changing the original variable during argument evaluation cannot change the snapshot. Queries allocate nothing, do not mutate or transfer inputs, and introduce no suspension or language exception; operand evaluation and retention can still fail. Generic definitions must have the required capability when checked.

`indexOf(search, start?)` takes a rune ordinal start, defaulting to 0, in the inclusive range `0..len(receiver)`; negative or excessive starts cause a bounds panic without clamping. It returns the first matching rune ordinal at or after start, -1 for no match, and start for an empty pattern. `lastIndexOf(search)` returns the last matching rune ordinal, -1 for no match, and `len(receiver)` for an empty pattern. Matching uses complete UTF-8 bytes without normalization; runes are Unicode scalars, not graphemes. Receiver, pattern and start are evaluated once in that order with value snapshots. The query allocates nothing, does not suspend or change ownership; argument evaluation and retention failures still propagate. Unicode coordinate conversion may scan a prefix; repeated queries have no constant-time guarantee.

String `<`, `<=`, `>` and `>=` compare the complete explicitly sized UTF-8 values in unsigned byte lexicographic order. Embedded NUL participates; after an equal prefix the shorter string is smaller. No locale, normalization or grapheme ordering applies.

Strings do not support integer indexing or the slice operator; use `s.runes().nth(i)`, `s.bytes()[i]`, or `s.slice(start, end)` explicitly. Concatenation uses `+`; Unicode text transforms such as case conversion, trimming, padding, and reversal belong to the `text` module.

### 14.6 `Array<u8>`

`Array<u8>` is a directly available specialization of `Array`; construction is handled via builtin paths such as `Array<u8>(n)` / `Array<u8>(n, fill)`. Its `toString()` uses the same container formatting as every Array; decode text explicitly with `string.fromUtf8(bytes[:])` or `string.fromUtf8Lossy(bytes[:])`. There is currently no separate `stdlib/types/bytes.xr` declaration; tooling should not treat it as a second, Array-isomorphic API surface.

### 14.7 `Array<T>` Methods

`Array<T>` is a growable value type whose element type is copyable and storable. Assignment, saved parameters, index reads and returns preserve logical copies; shared backing does not permit mutation of other copies. Storage alone does not require comparison, hashing or string conversion; each corresponding method has its own admission contract.

The first owned-array execution subset freezes `len(array: Array<T>) -> i64`, read-only `get(index: i64) -> T`, `ref set(index: i64, value: T) -> ()`, `ref push(value: T) -> ()`, array literals and the index expressions in §3.4/§3.11. The set method returns unit; an index-assignment expression returns the converted right-hand T. len itself does not allocate; reads/copies can reach retention limits; construction and mutation can fail for budget, allocation or retention limits. Both set and push can relocate element storage when detaching shared backing.

Read-only get and indexing follow the value-receiver snapshot rule in §3.0: take the Array logical snapshot before evaluating index; an index that rebinds the variable cannot cause a late read of its current value. Ref set/push instead bind the root place once. Only after all arguments finish do they establish exclusive access to the binding's current value, check bounds, detach, mutate and write back. If an argument rebinds that root, the operation applies to the new value; set checks its new length. `a.push(a[0])` is valid and independently owns its element result before push. Failure publishes no partial array and preserves the pre-commit logical value; earlier argument effects and rebinding are not rolled back.

An ordinary read parameter cannot be a writable receiver for these ref methods. Copyable Array has no move receiver; pop removes an element without consuming the whole Array. Ref in the table is a declaration mode: the dot call remains `a.push(x)`. Pure-index GET-place snapshot elimination and transient retain/copy/drop must preserve failures as required by §3.0; backing uniqueness never bypasses access permissions.

```xray @id=array-ref-current-binding
var a: Array<string> = ["old"]
fn replaceForPush() -> string {
    print("arg")
    a = ["new"]
    return "tail"
}
a.push(replaceForPush())
print(a[0], a[1])           // Prints arg, then new tail
```

```xray @id=array-push-own-element
var a: Array<string> = ["head"]
a.push(a[0])
print(a[0], a[1])           // head head
```

The table retains the complete method denominator. The current frozen XIR boundary comprises the three get/set/push primitives and twelve declaration recipes: map/filter/reduce/forEach/find/findIndex/every/some/contains/indexOf/join/clear. Together with reverse/unshift/pop/shift these are 19 operations; the other 13 of the 32 members remain unadmitted. Execution identities come from the same structured declarations in `stdlib/types/array.xr`; names serve member lookup only. Capacity is explicitly observable. Its growth/detachment guarantees must be frozen when that property is admitted; not all capacity behavior is an unobservable optimization.

These READ callbacks/queries first own the receiver snapshot, then evaluate each explicit argument once in source order; reduce orders receiver, callback, initial. map/filter/forEach visit increasing indices and allow the trailing index argument to be omitted where the declaration permits it. find/findIndex/every/some short-circuit using a single-element bool callback. Empty reduce returns initial, empty every is true, empty some/contains is false, and no match produces none for find or -1 for findIndex/indexOf. contains/indexOf require T:Equal at the definition. This join subset admits string/bool/rune/admitted numeric elements, defaults its separator to the empty string and preserves UTF-8/NUL bytes. ref clear uses existing writable-place and writeback contracts; copied values remain independent. Callbacks keep ordinary indirect-call throw/panic/suspend/cancellation and ownership semantics; future instances cannot supply missing constraints. This paragraph freezes operation contracts; full regression, safety, OOM and physical-release qualification requires actual batch evidence.


The `ref pop() -> T?` and `ref shift() -> T?` declarations are frozen: no explicit, optional, default, variadic or method type arguments; they remove the tail/head element without consuming the receiver. An empty Array returns outer None without publishing an Array write. A nonempty result owns an exact T logical copy wrapped in one Some. Nullable elements are not flattened: removing null from Array<i64?> returns Some(None):i64??, removing 7 returns Some(Some(7)):i64??, and only an empty Array returns None:i64??. This adds neither Some/None source syntax nor Equal/Compare/ToString/NotNullable requirements. Ordinary generic definitions are checked before Checked specialization and revalidation.

The receiver is an already authorized writable logical place. Root/path selectors run once; after path evaluation the current bound Array is read, and completed path effects are not rolled back. Const/read Array value parameters, temporary Arrays, inaccessible and const fields reject. Mutable Array fields reached through read class handles retain existing class identity and field authority. Before the sole publication, nonempty removal prepares the complete owned Nullable result and a private Array candidate: pop retains the first n-1 elements; shift retains the last n-1 in their original order. Copying stops at class or synchronization identity, and results never borrow backing interior addresses. Every fallible method result hold/cell read precedes publication. Preparation/publication failure or prepublication cancellation preserves the logical root, aliases and identities and releases temporary ownership normally. Later parent-expression/function result handoff failure or cancellation does not undo the method effect. Internal loops introduce no language suspension or user callback. The empty path reads no element and publishes no write, but promises neither zero allocation nor exemption from admission, retain or budget failure.

Both declarations have allocation=may_heap, failures=allocation,retain,limit and ownership=owned, without callback effects. Candidates may select another backing capacity; old capacity is not preserved by contract. Public growth/detachment guarantees for capacity/withCapacity/reserve remain separately frozen on that family's admission, as for reverse/unshift. Freezing this contract does not qualify the new operations.

The next declaration family is frozen as `ref resize(length: i64, fill: T) -> Array<T>`. Its two required READ arguments evaluate length then fill once each; there are no defaults, optional/variadic arguments or method type parameters. Length is exactly i64 and fill exactly T. Only existing copy/store capabilities are required, without Equal/Compare/ToString/NotNullable additions; ordinary generic definitions are checked before Checked specialization and revalidation. The receiver is an authorized writable logical place. Selectors run once, and the current bound Array is read after both arguments succeed; completed effects are not rolled back. Const/read Array value parameters, temporaries and unauthorized fields reject, while mutable fields reached through read class handles retain existing authority.

For n>=0, the first min(oldLength,n) elements are preserved and growth uses logical copies of fill, producing length n. The owned return and published receiver are independent logical copies; old aliases retain their old value, copying stops at class/synchronization identity, and Nullable layers remain intact. Zero returns an owned empty Array; equal length promises neither zero allocation nor unchanged backing. Negative n rejects after both arguments with the existing NUMERIC_RANGE/E0422 panic and no publication. Unrepresentable sizes and finite resource exhaustion use existing LIMIT/budget failures. Candidates may reselect capacity; capacity/withCapacity/reserve observation guarantees remain separately frozen.

The complete candidate, result cell read and every fallible hold precede one PLACE_WRITE. Preparation/publication failure or precommit cancellation preserves the current root, aliases and identities and drops temporary owners. Later parent-expression/result handoff failure or cancellation does not roll back the method effect. Internal loops introduce no user callback, language suspension or suspended loan; driver quantum cancellation observes the actual commit point. Qualification is allocation:may_heap, failures:allocation,retain,limit, ownership:owned. Freezing does not admit resize: current19/32 remains until independent VM/native/both mixed outputs, all admitted element categories, actual failure ordinals, resource axes, escaped results and physical release of both domains qualify20/32.

| Member | Type / Description |
|--|--|
| `len(arr)` | global `i64` query |
| `capacity` / `arr[i]` / `arr[i] = v` | read-only capacity/index query and writable-place assignment; get is read-only and set is ref |
| `ref push(x)` / `ref pop()` | tail insert/remove; pop is not a move receiver |
| `ref shift()` / `ref unshift(x)` | head insert/remove |
| `concat(...arrays)` | read-only receiver; produces a concatenated result |
| `indexOf(x)` / `contains(x)` | read-only query |
| `join(sep?)` | read-only receiver; concatenates into a string |
| `ref reverse()` / `ref sort(cmp?)` | changes the receiver's ordering |
| `map(fn)` / `filter(fn)` / `reduce(fn, init)` | read snapshot; map/filter callbacks may take an index; reduce uses `fn(U,T)->U` |
| `forEach(fn)` / `find(fn)` / `findIndex(fn)` / `every(fn)` / `some(fn)` | read snapshot; forEach may take an index; the others short-circuit with `fn(T)->bool` |
| `ref fill(v, start?, end?)` / `ref clear()` | fill or clear |
| `ref reserve(capacity)` / `ref resize(length, fill)` | capacity and length management |
| `ptr()` / `mutPtr()` | returned-borrow contracts remain to be frozen; mutPtr cannot grant writable access through an ordinary read receiver |
| `toString()` | read-only receiver; container representation |
| `iterator()` / `entriesIterator()` / `entries()` | read-only receiver; result types and ownership of these methods must be frozen before method admission, separately from the frozen Array for-in snapshot |

Array for-in evaluates its collection expression once and owns a logical value snapshot. Element replacement, append, or rebinding of the source does not change that sequence; element access produces an owned value copy. Copying stops at class identity, so changes to a referenced object remain observable through references to the same identity. Each iteration binding is immutable, an empty array runs the body zero times, and continue cleans up the iteration before advancing once. Suspension retains the snapshot and index; cancellation releases their ownership. A generic `Array<T>` can be traversed using its shape known at the definition; arbitrary `T` does not gain iteration authority because one instance happens to be an Array. Iterator methods, other collections, and borrowed views require their own admission checks.

Array has no `slice()` / `splice()` / `flat()` / `copyWithin()` methods. `arr[start:end]` produces a borrowed `Slice<T>` whose target type must be explicit and whose lifetime follows the borrow rules in §2.4.2; use `copy(arr[start:end])` for independent data.

### 14.8 `Map<K, V>` Methods

| Member | Type / Description |
|--|--|
| `len(m)` | global `i64` query |
| `m[k]` / `m[k] = v` | indexed read/write |
| `get(k)` / `set(k, v)` | `get` returns `null` when absent; `set` writes |
| `containsKey(k)` / `containsValue(v)` / `delete(k)` / `clear()` | query and remove |
| `keys()` / `values()` / `entries()` | keys, values, key/value pairs |
| `forEach(fn)` | traversal |
| `iterator()` / `entriesIterator()` | iteration protocol |

**Map literal**: `#{"k1": v1, "k2": v2}` or `#{}`; entries use `:`, distinguished by the `#` prefix from exact structural-object literals of the form `{ field: value }`.

`m[k]` requires the key to exist; a missing key raises runtime error `E0431`. Use `m.get(k)` for optional lookup.

The key position of a subscript is typed and checked against `K`, symmetrically with the value position against `V`: `m[1]` on a `Map<f64, V>` is the f64 key `1.0`, not an i64 key stored in a f64 map. Key matching uses the key equivalence relation from §9.2, not `==`.

### 14.9 `Set<T>` Methods

| Member | Type / Description |
|--|--|
| `len(set)` | global `i64` query |
| `add(x)` / `contains(x)` / `delete(x)` | insert, query, remove |
| `clear()` | empty the set |
| `values()` | returns `Array<T>` |
| `forEach(fn)` | traversal |
| `iterator()` | iteration protocol |

**Set literal**: `#[1, 2, 3]` or `#[]`.

### 14.10 `Channel<T>` Methods

| Member | Type / Description |
|--|--|
| `send(v)` | blocking send; throws if the channel is closed |
| `recv()` | blocking receive, returns `Recv<T>`; closed and drained is `Recv.Closed` |
| `recvOr(default)` | receives a payload, or returns the supplied default when none is available |
| `trySend(v)` | non-blocking send, returns `SendResult` |
| `tryRecv()` | non-blocking receive, returns `Recv<T>`; empty is `Recv.Empty` |
| `sendTimeout(v, ms)` | timed send, returns `SendResult`; timeout is `SendResult.Timeout` |
| `recvTimeout(ms)` | timed receive, returns `Recv<T>`; timeout is `Recv.Timeout` |
| `close()` | close the channel |
| `capacity` / `isClosed` | capacity and closed-state fields |

`Recv.Value { value: v }` carries the channel payload, so `Channel<i64?>` can distinguish a real `Recv.Value { value: null }` from `Recv.Closed`.

### 14.11 `JSON` Namespace

`JSON` is a prelude namespace, not a value type that variables can use, and it needs no `import json`. Use `JSON.Value` for schema-less JSON and `JSON.Object` when the dynamic value is known to be an object. `JSON.Object` is a pure alias for `Map<string, JSON.Value>`, so enumeration, dynamic subscripts, field insertion/removal, and `len` use the Map API in §14.8 directly.

`JSON.Value` is the recursive boundary domain `null | bool | i64 | f64 | string | Array<JSON.Value> | JSON.Object`. It has no magic dot access, subscript, iteration, or `len`: commit to a schema with typed decode, explicitly unwrap it with `asObject` / `asArray`, or use the path API for arbitrary depth.

| Static function | Description |
|--|--|
| `JSON.parse<T>(text, unknown?)` | constructs a typed value directly; unknown fields default to `Reject`, with explicit `JSON.UnknownFields.Ignore` available |
| `JSON.parseObject(text)` / `JSON.parseValue(text)` | parses an object root or any JSON root without a structural-object intermediate |
| `JSON.parseWithRest<T>(text, nestedUnknownFields?)` | constructs known fields as `value: T` and keeps unknown top-level fields in `rest: JSON.Object` |
| `JSON.decode<T>(value, unknown?)` / `JSON.decodeObject<T>(object, unknown?)` | attempts to construct `T` from an existing schema-less value; failure returns `null` |
| `JSON.value<T>(value)` | explicitly materializes an encodable composite value as `JSON.Value` |
| `JSON.stringify<T>(value, indent?)` | serializes an encodable value; `indent` must be a non-null `i64` |
| `JSON.isValid(text, strict?)` | validates JSON text |
| `JSON.kindOf(value)` / `JSON.isNull/isBool/isInt/isFloat/isString/isArray/isObject(value)` | inspects the active JSON arm |
| `JSON.asObject(value)` / `JSON.asArray(value)` | returns a nullable view sharing the underlying object/array storage, without copying |
| `JSON.get<T>(root, path)` / `JSON.require<T>(root, path)` | reads and decodes at a `JSON.Path`; the former returns `null` on failure, the latter throws `JSON.PathError` |
| `JSON.containsPath(root, path)` | tests whether the complete path exists |
| `JSON.set(root, path, value, createParents?)` / `JSON.remove(root, path)` | mutates or removes a path through object keys and array indices |
| `JSON.merge(parts)` | recombines the typed and top-level rest parts of `JSON.WithRest<T>` as a `JSON.Object` |

`JSON.Path` is `Array<string | i64>`: a string segment is one complete object key, and an i64 segment is an array index. `["user", "profile", "name"]` addresses nested fields; `"user.profile"` is only one key containing dots. A dynamic object key can also use the Map subscript and methods on `JSON.Object` directly.

```xray @id=json-boundary-and-path-en
type Request = { action: string, userId: i64 }

var request = JSON.parse<Request>(body)       // unknown fields reject by default
var payload = JSON.parseObject(body)          // JSON.Object, therefore a Map
payload["traceId"] = "req-42"               // scalar widening is implicit

var path: JSON.Path = ["user", "profile", "name"]
var name = JSON.get<string>(payload, path)
JSON.set(payload, path, "Ada", true)
```

JSON scalars (`null`, `bool`, `i64`, `f64`, and `string`) widen implicitly when an explicit `JSON.Value` target exists. Structural objects, arrays, and other composites require `JSON.value(...)`. Therefore `{ name: "alice" }` always remains an exact structural object; context never silently turns it into a dynamic object.

### 14.12 `Range`

`a..b` is the half-open interval `[a, b)`, while `a..=b` is the inclusive interval `[a, b]`. Both forms work in expressions, `for-in`, and range patterns in `match`.

| Member | Description |
|--|--|
| `start` / `end` | The start and the declared endpoint |
| `contains(x)` | Tests membership using the range's half-open or inclusive semantics |
| `toArray()` | Produces an independent `Array<i64>` in iteration order |
| `toString()` | Returns an `a..b` or `a..=b` string |
| `iterator()` | iteration protocol; yields the same elements as `toArray()`, lazily |
| `len(range)` | Returns the number of elements in the range |

```xray @id=range-members-en
var pages = 1..=3
print(pages.start)          // 1
print(pages.end)            // 3
print(pages.contains(3))    // true
print(len(pages))           // 3
print(pages.toArray())      // [1, 2, 3]

var empty = 5..5
print(len(empty))           // 0
```

### 14.13 `DateTime`

The `datetime` module provides factory functions through `import datetime`: `now`, `utc`, `create`, `createUTC`, `fromTimestamp`, `fromTimestampMs`, `parse`, and `offset`. `DateTime` is not a prelude type; import it explicitly with `import { DateTime } from datetime` when the name is used as a type.

| Member | Type / Description |
|--|--|
| `year` / `month` / `day` | date-component properties |
| `hour` / `minute` / `second` / `millisecond` | time-component properties |
| `weekday` / `yearday` / `timestamp` | derived properties |
| `toString()` / `format(pattern?)` / `toISOString()` | formatting |
| `add(amount, unit)` / `diff(other, unit?)` | date arithmetic |
| `toUTC()` / `toLocal()` | timezone conversion |
| `isBefore(other)` / `isAfter(other)` / `equals(other)` | comparison |
| `isLeapYear()` / `daysInMonth()` | calendar queries |

### 14.14 `Regex`

| Method | Description |
|--|--|
| `test(s)` | match predicate |
| `find(s)` | first match |
| `findAll(s)` | all matches |
| `findText(s)` / `findGroup(s, index)` | first matched text / capture-group text |
| `replace(s, replacement)` | replacement |
| `split(s, limit?)` | split |

### 14.15 `StringBuilder`

| Method | Description |
|--|--|
| `len(builder)` | current rune count |
| `append(s)` | append and return self |
| `toString()` | output string |
| `clear()` | empty and return self |

### 14.16 `PanicInfo`

The built-in `PanicInfo` class has fields `message`, `stack`, `cause`, `code`, `data`, the constructor `constructor(message: string = "", cause: PanicInfo? = null)`, and `toString()`.

### 14.17 `Task<T>` and Enum Values

`Task<T>` properties: `done`, `status`; methods: `cancel()`, `poll()`, `awaitResult()`, `awaitTimeout(ms)`. `poll()` and explicit wait methods return `TaskResult<T>` as `Success(T)`, `Failed(PanicInfo)`, `Cancelled`, `Timeout`, or `Pending`. Plain `await task` returns `T` on success and uses the matching error/panic path for failure or cancellation; results must be copyable and Sendable; the Task keeps its complete owned sticky outcome and repeated awaits obtain logical copies, stopping at class and synchronization identities, without an `await (move task)` form or reference-count-selected single take. Enum values provide the cold-path `name`, `ordinal`, and `toString()` surface.

### 14.18 Thread and Synchronization Handles

`Thread<T>` is a prelude handle type with the `done` field and the `join()` / `detach()` methods. After importing `sys`, create an OS thread with `sys.Thread.spawn(body)` or `sys.Thread.spawn(ThreadOptions{...}, body)`, then call `join()` or `detach()` on the returned handle. Import `CountdownLatch`, `EventCount`, `ResultGroup`, `Semaphore`, and `WorkQueue` from `sync`; import `Logger` from `log`; and import connection and listener types from `net`.

### 14.19 `Atomic<T>` Methods

`Atomic<T>` wraps `i64`, `f64`, or `bool` with lock-free atomic operations. Name the handle with `const`; audited atomic methods provide synchronized interior mutation.

| Method | Signature | Description |
|--|--|--|
| `load(ord?)` | `(Ordering?) -> T` | Atomically read the current value |
| `store(val, ord?)` | `(T, Ordering?) -> ()` | Atomic write |
| `add(val, ord?)` | `(T, Ordering?) -> ()` | Atomic add |
| `sub(val, ord?)` | `(T, Ordering?) -> ()` | Atomic subtract |
| `fetchAdd(val, ord?)` | `(T, Ordering?) -> T` | Atomic add, returning old value |
| `fetchSub(val, ord?)` | `(T, Ordering?) -> T` | Atomic subtract, returning old value |
| `swap(val, ord?)` | `(T, Ordering?) -> T` | Atomic swap, returns old value |
| `compareExchange(expected, desired, ord?)` | `(T, T, Ordering?) -> (T, bool)` | CAS, returns `(old_value, success)` |
| `toggle(ord?)` | `(Ordering?) -> bool` | Atomic negate (bool only), returns old value |
| `toString()` | `() -> string` | Returns string representation of current value |

The `ord?` parameter accepts an `Ordering` enum; defaults to `Ordering.SeqCst`. See §10.9.

The complete constraints and operation admission follow §9.2/§10.9: the type requires `T: AtomicValue`; each of the four numeric methods requires method `where T: AtomicNumber`, while toggle requires `where T: AtomicBoolean`. Other methods keep the full AtomicValue domain. load/store reject invalid orders. compareExchange returns an owned real Tuple with all result resources prepared before its side effect. f64 bits/CAS and software RMW, i64 wrapping and the single-snapshot toString follow §10.9. The unconstrained declaration currently in `stdlib/types/atomic.xr` and the fixed i64 instructions still await an atomic complete-family migration; they do not establish implementation of this frozen contract.
<!-- /xr-spec:en -->

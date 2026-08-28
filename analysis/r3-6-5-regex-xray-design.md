# 6-5 正则自动机重写为 Xray —— 中间表示与分工规格

- **lane**: 6-5
- **基线**: `34be0379c`
- **分支**: `work/6-5-regex-xray-34be0379c`
- **实测靶子**: regex 有 **159** 个 C 语义所有者（brief 写 172，是另一口径；本文一律用实测值）
  - `xregex_compile.c` 39、`xregex_parse.c` 35、`xregex.c` 32、`xregex_binding.c` 25、
    `xregex_dfa.c` 14、`xregex_nfa.c` 14 ＝ 147 个 c-function
  - ＋ 7 个 class-method（`Regex.test/find/findText/findGroup/findAll/replace/split`）
  - ＋ 5 个仍为 native 的模块函数（`find`×2、`fullFind`、`findAll`×2，两个是 argc 重载）

**本文是四段并行开发的唯一接口来源。任何一段都不得自造表示。**

---

## 0. 为什么 `Regex` 仍是 native class

不是妥协，是被编译器前端钉死的：正则字面量 `/pat/flags` 在
`src/frontend/parser/xparse_expr.c:451` 解析，在 `src/ir/xi_lower_expr.c:11349`
lower 成 `XI_REGEX_COMPILE`，结果类型写死为 `l->type_regex`
（`src/ir/xi_ops_gen.h:3419` 把它 rewrite 成 VM builtin `regex_compile`）。
`src/ir/` 与 `src/frontend/` 都在本 lane 的禁改清单里，所以 `Regex` 这个**壳**必须留下。

留下的只有壳。壳里装什么，本 lane 说了算。

**实测的两条地基**（都跑过 probe，不是推断）：

1. `XrNativeBodyDesc`（`src/runtime/class/xclass.h:65-72`）**没有 GC trace 回调**，
   只有 `init` / `destroy` / `deep_copy`。所以 native body 里存 `XrValue` 会被 GC 漏扫。
   → **native body 不能装 Xray 对象**，`RegexBody { XrRegex *regex; }` 这条路不能改造成装 Xray 值。
2. 但 `XrInstance.fields[]` 是**正常的 GC 可见字段**，而且 `.xr` 能**读也能写**。
   probe（本 lane 实测）：
   ```xray
   var m = regex.find(regex.compile("a(b)c"), "xabcy")
   m!.start = 99          // 写 native class 字段
   print(m!.start)        // -> 99
   ```
   → **程序镜像走 `class_field`，不走 native body。**

## 1. `Regex` 的新形状

`stdlib/types/regex.xr`（编译器输入声明）与 core.def 的 `class_field` 行同步改成：

```xray
class Regex {
    pattern: string
    flags: i64
    prog: Array<i64>?    // 懒编译的程序镜像；null = 尚未编译
}
```

- C 侧只剩「构造一个填好 pattern/flags、prog=null 的实例」。native body 整个删掉
  （连同 `RegexBody`、`regex_body_destroy`、`g_regex_body_desc`）。
- **懒编译**是被字面量路径逼出来的：`/pat/` 走 C 的 `regex_compile` builtin，而编译器
  已经搬进 Xray，C 侧编译不了。所以两条路（字面量、`regex.compile()`）统一成
  「只存 pattern+flags」，第一次用到时由 Xray 侧 `xrProgOf(re)` 编译并写回 `re.prog`。
- **并发**：`re.prog` 的写是幂等的（同 pattern+flags 编译出等值镜像），最坏是重复编译一次，
  不会撕裂。这正面回答了 REF 地雷 20 —— 见 §5。

## 2. 程序镜像：单个 `Array<i64>`

为什么是一个扁平 i64 数组而不是 class 图：
- 它要能当 native class 字段（`Array<i64>` 可以，Slice 不行 —— `E0383` 禁止逃逸）
- 纯标量数组索引是 O(1)，是 REF-xray-language-capability §4 点名的最快形状
- 一个数组 = 一个字段 = 无需多字段同步

### 2.1 Header（下标 0..15 固定）

| 下标 | 名字 | 含义 |
|---|---|---|
| 0 | `PH_MAGIC` | `0x58525831`（"XRX1"），版本兼形状自检 |
| 1 | `PH_FLAGS` | 编译期**最终** flags（含回写的 UNGREEDY，见地雷 2） |
| 2 | `PH_START` | 起始指令索引 |
| 3 | `PH_NINST` | 指令条数 |
| 4 | `PH_NCAPS` | caps 槽数 ＝ `2*(ngroups+1)` |
| 5 | `PH_NGROUPS` | 捕获组数，不含全匹配 |
| 6 | `PH_INST_BASE` | 指令区起始下标 |
| 7 | `PH_UNI_BASE` | unicode 区间区起始下标 |
| 8 | `PH_UNI_COUNT` | unicode 区间条数 |
| 9 | `PH_TRAITS` | 位掩码，见 §2.4 |
| 10 | `PH_ANCHORED` | 1 = 模式以 `^`/`\A` 起首（可只从 0 试） |
| 11..15 | 保留 | 恒 0 |

`const PH_HEADER_LEN: i64 = 16`

### 2.2 指令区：定长 4 槽

第 `i` 条指令在 `prog[PH_INST_BASE + i*4 + k]`，`k = 0..3`：

| k | 名字 | 含义 |
|---|---|---|
| 0 | `IN_OP` | opcode，见 §2.3 |
| 1 | `IN_OUT` | 主后继指令索引（`-1` = 无） |
| 2 | `IN_OUT1` | `ALT` 的第二分支；其余 opcode 为 `-1` |
| 3 | `IN_ARG` | 操作数：byte / lo / cap 槽号 / empty 掩码 / unicode 区间下标 |

`BYTE_RANGE` 需要两个操作数，占 `IN_ARG` 的低/高 16 位：`lo | (hi << 16)`。
`const INST_STRIDE: i64 = 4`

**修正 REF 地雷（opcode 只有 4 bit、`last` 位是死字段）**：i64 槽位没有位宽压力，
opcode 从 11 个扩到多少都行；`last` 字段直接不存在。

### 2.3 opcode（值必须与下表逐字一致）

| 值 | 名字 | 语义 |
|---|---|---|
| 0 | `OP_NOP` | ε 转移到 `IN_OUT` |
| 1 | `OP_MATCH` | 记录当前位置为 end |
| 2 | `OP_FAIL` | 线程死亡 |
| 3 | `OP_BYTE` | 消费 1 字节，需 == `IN_ARG` |
| 4 | `OP_BYTE_RANGE` | 消费 1 字节，需在 `[lo,hi]` |
| 5 | `OP_ANY_BYTE` | `.` 非 dotall：需 `!= '\n'` |
| 6 | `OP_ANY_BYTE_NL` | `.` dotall |
| 7 | `OP_ALT` | ε 分裂，`IN_OUT` 优先于 `IN_OUT1` |
| 8 | `OP_CAPTURE` | ε；写 `caps[IN_ARG]` |
| 9 | `OP_EMPTY_WIDTH` | ε；`IN_ARG` 掩码断言成立才继续 |
| 10 | `OP_UNICODE_RANGE` | 解码 1 个 UTF-8 标量，查 unicode 区间 `IN_ARG` |

`OP_ANY_BYTE` / `OP_ANY_BYTE_NL` 一律**消费一个完整 UTF-8 标量**。
REF §2 记录 C 侧「NFA 消费完整标量、DFA/fast 只消费 1 字节」是同一 opcode 的两套答案，
本重写取 NFA 那一套并让它成为唯一答案 —— 见 §5 的地雷表。

### 2.4 零宽掩码与 traits

零宽掩码（`OP_EMPTY_WIDTH` 的 `IN_ARG`），与 C 侧逐位相同：

```
EW_BEGIN_LINE = 1, EW_END_LINE = 2, EW_BEGIN_TEXT = 4,
EW_END_TEXT = 8, EW_WORD_BOUNDARY = 16, EW_NOT_WORD_BOUND = 32
```

`PH_TRAITS` 位：

```
TR_HAS_EMPTY_WIDTH = 1   TR_HAS_ANY_SCALAR = 2   TR_HAS_UNICODE = 4   TR_HAS_CAPTURE = 8
```

### 2.5 unicode 属性区

第 `j` 条在 `prog[PH_UNI_BASE + j*3 + k]`：`k=0` prop_id、`k=1` negated(0/1)、`k=2` 保留。
`const UNI_STRIDE: i64 = 3`

**存 prop_id 而不是展开的码点区间**，和 C 侧一致（`xregex_compile.c:144-145` 的
`unicode_ranges[idx].prop_id` / `.negated`）。理由是所有权，不是省事：`\p{Han}` 的码点区间
是 **unicode 的语义**，权威在 `src/base/xunicode.c` 的属性表，那里不在 `stdlib/` 下，
inventory 也不把它算作 regex 的 C 所有者。把几百条区间抄进 `regex.xr` 会给同一个问题
造出第二个所有者 —— 正是本 lane 要消灭的东西。

因此保留两个薄的私有叶子，它们转发到 unicode 的既有权威、不含 regex 语义：

```
__unicodePropId(name: string) -> i64      // 名字 → 属性 id，-1 = 无效
__unicodeHasProp(cp: i64, propId: i64) -> bool
```

## 3. AST：编译期 SoA，不用 enum

用 SoA 数组而不是递归 ADT，理由是 REF-xray-language-capability §5 记录的两个实测编译器坑
（枚举实例方法不能跨模块调用；match 臂返回类类型会把负载字段全局污染成可空）。
SoA 一条都不碰，而且节点索引是 i64，天然能给解析器加深度上限。

```xray
class RxAst {
    op: Array<i64>          // 节点类型，见 §3.1
    a: Array<i64>           // 子节点索引 / 操作数 1
    b: Array<i64>           // 子节点索引 / 操作数 2
    c: Array<i64>           // 额外：min / cap 槽号 / 字符类索引
    d: Array<i64>           // 额外：max（-1 = 无上界）/ negated

    // 字符类池：所有区间打平成 [lo,hi] 对
    clsRanges: Array<i64>   // 2 个 i64 一对
    clsStart: Array<i64>    // 每个类在 clsRanges 里的起始「对」下标
    clsLen: Array<i64>      // 对数
    clsNeg: Array<i64>      // 1 = negated

    root: i64               // 根节点索引，-1 = 空模式
    ngroups: i64            // 捕获组数，不含全匹配
    flags: i64              // 解析结束时的 flags（内联 (?i) 会改它）
    err: i64                // 0 = ok，否则错误码
    errPos: i64             // 出错字节偏移
    errMsg: string          // 具体消息（C 侧丢弃了它，本重写带出来）
}
```

### 3.1 AST 节点类型

| 值 | 名字 | 字段用法 |
|---|---|---|
| 0 | `AN_EMPTY` | 空（匹配空串） |
| 1 | `AN_LITERAL` | `a` = 字节值 |
| 2 | `AN_ANY` | `.` |
| 3 | `AN_CLASS` | `c` = 字符类池下标 |
| 4 | `AN_CONCAT` | `a`,`b` = 左右子节点 |
| 5 | `AN_ALT` | `a`,`b` = 左右分支 |
| 6 | `AN_REPEAT` | `a` = 子节点，`c` = min，`d` = max（-1 = ∞），`b` = 1 表示懒惰 |
| 7 | `AN_GROUP` | `a` = 子节点，`c` = 捕获槽号（-1 = 非捕获） |
| 8 | `AN_EMPTY_WIDTH` | `a` = 零宽掩码（§2.4） |
| 9 | `AN_UNICODE` | `a` = unicode 区间池下标（`\p{...}`） |

`AN_REPEAT` 的 `min`/`max` 在编译期**完全展开**（与 C 侧一致，见 REF §2）：
`min` 份必选拷贝 ＋ 自环或可选展开。贪婪与懒惰只体现在 `OP_ALT` 两个分支的先后。

## 4. 四段接口（签名逐字固定）

四段全部是**模块级自由函数**，命名前缀 `rx`，最终合并进同一个 `stdlib/regex/regex.xr`。

```xray
// 第 1 段 —— 解析器（替 xregex_parse.c 的 35 个 C 函数）
fn rxParse(pattern: string, flags: i64) -> RxAst

// 第 2 段 —— 编译器（替 xregex_compile.c 的 39 个）
// 返回 §2 的程序镜像；ast.err != 0 时返回空数组
fn rxCompile(ast: RxAst) -> Array<i64>

// 第 3 段 —— 执行引擎（替 xregex_nfa.c 14 + xregex_dfa.c 14）
// 在 text[from..end) 上搜索；找到把 2*(ngroups+1) 个偏移写进 caps 并返回 true。
// caps[2i]/caps[2i+1] 是第 i 组的字节起止；-1 = 未参与匹配。
// anchored = true 时只从 from 起试。
fn rxSearch(prog: Array<i64>, text: Array<u8>, from: i64, anchored: bool,
            caps: ref Array<i64>) -> bool

// 第 4 段 —— 顶层 API（替 xregex.c 的 32 个）
// 见 §4.1
```

### 4.1 顶层 API 与公开面

```xray
export fn compile(pattern: string, flags: string = "") -> Regex
export fn isValid(pattern: string) -> bool
export fn test(pattern: Regex, s: string) -> bool
export fn count(pattern: Regex, s: string) -> i64
export fn find(pattern: Regex, s: string, offset: i64 = 0) -> RegexMatch?
export fn fullFind(pattern: Regex, s: string) -> RegexMatch?
export fn findAll(pattern: Regex, s: string, limit: i64 = -1) -> Array<RegexMatch>
export fn findText(pattern: Regex, s: string) -> string?
export fn findGroup(pattern: Regex, s: string, index: i64) -> string?
export fn replace(pattern: Regex, s: string, repl: string) -> string
export fn replaceAll(pattern: Regex, s: string, repl: string) -> string
export fn split(pattern: Regex, s: string, limit: i64 = -1) -> Array<string>
export fn escape(s: string) -> string
```

`RegexMatch` 保持 native class（4 个 class_field），由 `.xr` 构造并逐字段写入 —— §0 的
probe 证明 `.xr` 能写 native class 字段，所以不需要等 6-1，也不需要 `__` 构造叶子之外的东西。

### 4.2 共享工具（第 4 段提供，其余三段可用）

```xray
fn rxIsWordByte(b: i64) -> bool          // [A-Za-z0-9_]，纯 ASCII
fn rxDecodeUtf8(text: Array<u8>, i: i64) -> i64   // 返回 (cp << 8) | width；非法返回 -1
fn rxCheckEmptyWidth(text: Array<u8>, pos: i64, mask: i64) -> bool
```

## 5. 二十条语义地雷的定夺

每条都必须在提交信息里出现。**修正 = 3 条真 bug ＋ 2 条一致性缺陷，其余保留。**

| # | 地雷 | 定夺 | 依据 |
|---|---|---|---|
| 1 | leftmost-longest（POSIX）而非 leftmost-first | **保留** | 8 个回归用例钉着它，改了是换语言方言 |
| 2 | 任一懒惰量词把**整个**程序翻成 UNGREEDY | **保留** | 见 §2.1 `PH_FLAGS`。行为怪但被 `1120_regex_re2_compat` 覆盖 |
| 3 | `match_at`/`count`/迭代器平移 text 基址，`^`/`\b` 在每个 offset 重新成立 | **保留** | `find(/^a/,"bba",2)` 会匹配；`1115_regex_advanced` 依赖 |
| 4 | `$`（非 multiline）是 `\z` 语义，不容尾随换行 | **保留** | 与 RE2 一致 |
| 5 | **空匹配在 `findAll`/`split`/`replaceAll` 慢路径重复产出一次** | **修正** | 真 bug。`findAll(/$/,"abc")` 给 2 条而 `count` 给 1，同一问题两个答案 |
| 6 | **`replace_all_fast` 空匹配复制错字符、跳过文本** | **修正** | 真 bug。快路径与慢路径对同一输入给不同结果 —— 本重写**删掉快路径**，一条路即无从分叉 |
| 7 | **`split` 静默截断到 256 段并丢尾段** | **修正** | 真 bug。`Array<string>` 无需上限，`XR_REGEX_CORE_SPLIT_MAX_PARTS` 一并删 |
| 8 | `findAll(limit=0)`=空数组、`split(limit=0)`=无限，语义相反 | **保留** | 两者各自被回归钉住；统一会破坏现有用例，且不是本 lane 的公开面决定 |
| 9 | 最多 31 个捕获组 | **保留上限但改为可诊断** | 超限从静默变成解析错误 |
| 10 | `$N` 贪婪读数字、组不存在静默插空、`\1` 不是反向引用 | **保留** | RE2 系惯例 |
| 11 | 字符类内 `\d \w \s \p{}` 退化成字面字符 | **保留** | 与 PCRE 的最大偏离，但改它会改掉 `[\d]` 的现有含义；属公开面决定，不在本 lane |
| 12 | POSIX `[:alpha:]` 静默错误解析 | **保留** | 同上 |
| 13 | `(?<=`/`(?<!` 走命名组分支，错误信息误导 | **修正（仅错误信息）** | 行为仍是拒绝，但报 "lookbehind is not supported" 而不是命名组的乱码 |
| 14 | `\u` 不支持，而 `parse.c:22` 注释声称支持 | **保留行为，删掉错注释** | 注释随 C 文件一起删 |
| 15 | `[^…]` 在字节层 `0..255` 求补，对多字节字符错 | **保留** | 改它要引入 unicode 补集运算，是独立的一块；写进残留 |
| 16 | `x`/`u` flag 未实现，flag 串只认 `i/m/s`，其余静默忽略 | **保留** | `xr_regex_core_parse_flags` 是 VM/AOT 共享权威，本重写照搬其掩码 |
| 17 | 内联 `(?i)` 不恢复 flags，影响其后全部 | **保留** | RE2 也是这样 |
| 18 | 无深度/步数/超时限制；`XR_RE_MAX_NESTED_DEPTH=100` 是零引用死常量；ε 转移是 C 递归 | **修正** | Xray 递归深度有限（REF 硬约束 ③）。解析器显式传 `depth` 并在 100 层拒绝，**让那个死常量第一次真正生效**；ε 闭包改**显式栈**，不递归 |
| 19 | 实际指令上限是 8192 而非声称的 10000 | **修正** | 上限就是 `RX_MAX_INSTS = 10000`，一个数字一个答案 |
| 20 | **DFA 是无锁共享可变缓存，多协程共用一个 Regex 会数据竞争** | **修正：整块删除 DFA** | 见下 |

### 5.1 关于地雷 20（brief 点名「必须重新设计」）

C 侧 `XrDFA` 挂在共享的 `XrRegex` 上，`dfa_next_state` 会写 `state->next[]` 和
`dfa->state_cache[]`，两个协程共用同一个 `Regex` 就是并发写同一块内存。

**重新设计的答案是不要这块可变缓存。** 通用 NFA 主循环本来就是线性的（REF §1 末尾：
只有 `nfa_search_fast` 和 `xr_dfa_search` 是 O(n²)，通用主循环是线性），
DFA 只是 `test`/`count`/`replace_all_fast` 的常数因子加速，而它换来的是
一个没有锁的共享可变状态机。删掉它：

- 474 行 C、14 个 C 语义所有者归零
- 数据竞争在结构上消失，不是靠加锁掩盖
- 唯一剩下的每实例可变状态是 `re.prog`，而它的写是幂等的（§1）

代价是 `test`/`count` 的常数因子变差。这是**记录在案的代价，不是失败**（brief 明示），
基准数字见最终报告。

## 6. 分工与并行纪律

四段各自在 `tests/regression/10_stdlib/` 之外的独立 `.xr` 文件里开发，
直接 `./build-nofp/xray run` 验证 —— **不改 stdlib，不需要重新构建编译器**。
合并由 lane 主体做。

| 段 | 文件（开发期） | 替掉的 C |
|---|---|---|
| 1 解析器 | `scratch/rx_parse.xr` | `xregex_parse.c` 35 |
| 2 编译器 | `scratch/rx_compile.xr` | `xregex_compile.c` 39 |
| 3 执行引擎 | `scratch/rx_exec.xr` | `xregex_nfa.c` 14 ＋ `xregex_dfa.c` 14 |
| 4 顶层 API | `scratch/rx_api.xr` | `xregex.c` 32 |

**命名前缀 `rx`，常量前缀 `RX_`/`PH_`/`IN_`/`OP_`/`AN_`/`EW_`/`TR_`。**
四段之间只能通过 §4 的签名交互，不得读对方的内部函数。

## 7. 性能预期

Xray 侧 NFA 每字节要跑一遍解释器循环，比 C 慢是必然的。可控的是常数：
- 全程 `Array<u8>` ＋ 索引，不用 `s.slice()`、不用 `s.runes().nth(i)`
  （REF 硬约束 ②：前者 O(n)＋分配，后者 O(i)）
- 线程列表用两个预分配的 `Array<i64>` 双缓冲 ＋ 一个 `Array<i64>` 的 generation 标记去重
- caps 用扁平 `Array<i64>`，`ref` 传递，不每步复制

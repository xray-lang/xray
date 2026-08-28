# Blocker: 29% 的编译器诊断没有算出位置，而渲染层把这件事藏了起来

- **Lane**: 9（诊断 location 化）
- **Status**: `OPEN`
- **Requested owner**: parser 位置填充（`src/frontend/parser/`）+ 各诊断发射点
- **Severity**: 这不是"caret 差了几列"。**903 条诊断里 262 条（29%）根本没有位置**，
  涉及 232 / 882 个用例（26.3%）。用户看不到这一点，因为渲染层把 `column == 0`
  钳制成了 `1`——于是"没算出列"和"故意指向语句开头"在屏幕上一模一样。

## 精确来源

| 项 | 值 |
|---|---|
| base commit | `00f665c5cfcdc5f0f938ba133c42a258a640d95f` |
| base tree | `afe293b71f96b8eae009027a876158d0a00375f2` |
| worker branch | `work/9-diagnostic-location-00f665c5c` |
| 客观量表 | `python3 tests/compile_errors/check_diagnostic_positions.py`（已入库，带棘轮预算） |
| 人工判断 | 872 个用例逐个核对，223 条 `misplaced` 已写进 `.expected` |

## 一、渲染层的钳制掩盖了缺陷

`src/frontend/xdiag_fmt.h:164`：

```c
if (column <= 0)
    column = 1;
```

对读者这是好事，对测量是灾难。同一个用例：

```
$ ./build-nofp/xray tests/compile_errors/type/010_invalid_index.xr
error[E0352]: Index type 'string' is not assignable to expected type 'i64'
  --> .../010_invalid_index.xr:5:1        <- 钳制后

$ ./build-nofp/xray check tests/compile_errors/type/010_invalid_index.xr
.../010_invalid_index.xr:5:0: error: Index type 'string' ...   <- 真值
```

`xray check` 走另一条渲染路径、不钳制，所以入库的普查脚本读它。
**这也意味着期望文件里的 `col 1` 混了两类东西**：真的指向行首的语句级诊断，
和压根没算列的退化诊断。人工审核时判别方法就是跑一次 `xray check` 看有没有 `:0:`。

## 二、客观基线（`check_diagnostic_positions.py` 实测）

```
cases:              882
diagnostics:        903
complete positions: 641   (71.0%)
line but no column: 250   (27.7%)
no line at all:      12   ( 1.3%)
cases affected:     232   (26.3%)
```

脚本带**棘轮预算**（250 / 12）：修好一个发射点这个数字就降，
新的发射点忘了传 span 这个 gate 就红。**不要为了让它变绿而抬高预算。**

## 三、根因：AST 节点构造 API 不收 column

五批独立审核（按目录切分，互不通气）**收敛到同一个根因**：

`src/frontend/parser/xast_api.h` 里绝大多数构造函数只收 `int line`：

```
xr_ast_return_stmt (xast.c:666)   xr_ast_member_set      xr_ast_tuple_literal
xr_ast_as_expr                    xr_ast_expr_stmt       xr_ast_call_expr
xr_ast_assignment                 xr_ast_array_literal   xr_ast_index_set
```

`alloc_node` 把 `column` 恒置 0；column 只靠 parser 里约 40 处零散的
`node->column = ...` 补写，其余节点 column 恒为 0。

**反证极干净**：`xr_ast_move_expr` 是少数收 column 的，
所以整个 `move` 系用例（`ownership/004/033/034/035/071/072/073/077/083/087/088`）位置全对。

**最硬的对照**在 `xa_parallel_callback_report_effect()`（`src/frontend/analyzer/xanalyzer_visitor.c:1205`）：
同一个函数、同一行代码取 `site->line` / `site->column`——

| 节点类型 | 位置 | 用例数（逐字节核对） |
|---|---|---|
| `AST_CALL_EXPR`、`AST_CHAN_SEND`、`AST_CHAN_RECV` | **全部正确** | 15 |
| `AST_AWAIT_EXPR`、`AST_GO_EXPR`、`AST_THROW_STMT`、`AST_SELECT_STMT`、`AST_FOR_IN_STMT` | column == 0 | 10 |

`AST_FOR_IN_STMT` 更连 `line` 都错（拿到外层 `parallel.forEach(` 那一行）。
所以**这不是诊断代码的问题，是 parser 没给这几类节点填位置**。

> 与 2 号经 10 号转达的发现一致：`xr_ast_*` 构造 API 绝大多数只收 `line`，
> `alloc_node` 把 `column` 恒置 0。2 号在 `xrepl.c` 补的那处是同一根因的下游点。
> **但根源比"desugar 合成节点"更广——手写源码里的这几类节点一样没有 column。**

## 四、按发射点分族的缺陷清单

每一族都能找到**同错误码、同措辞、但位置正确的兄弟用例**作对照组，
所以修复后的验收是机械的，不需要重新推理。

### 族 1：语句/复合表达式节点无 column（约 40 例，跨全部五批）

发射点：`xanalyzer_visitor_stmt.c:9594`（return 语句）、`xa_ownership.c:412`（defer 阻断 return）、
`xanalyzer_visitor.c:6005`（成员赋值）、`xanalyzer_visitor.c:5587`（元组字面量）。

**修 `xr_ast_return_stmt` / `xr_ast_member_set` / `xr_ast_tuple_literal` 三个构造函数即可批量回收。**
对照组：`ownership/036`、`ownership/086`（`return data` → col 12）、
`ownership/171`、`ownership/172`（`yield data` → col 11）。

### 族 2：parser 诊断用前瞻 token（约 20 例）

`xr_parser_error()`（`src/frontend/parser/xparse.c:521`）转 `xr_parser_error_at_current()`，
用的是**已越过被拒构造**的前瞻 token，caret 落在其后的 `)`、`=`、`>` 或下一行第一个 token。

涉及 `xparse_type.c:104`、`:408`、`xparse_coroutine.c:544`。
`@repr(C)` 的诊断指 `(` 而非 `repr`；`@derive(Debug)` 指 `)` 而非 `Debug`；
`ffi/012` 的 `@weak` 无括号，位置直接**跨到下一行的 `fn`**——这是这一族最干净的铁证。

同文件的 `xr_parse_reject_postfix_param_mode` 已在做 `Token mode_token = parser->current` 保存，可照抄。

### 族 3：类型引用节点无位置，行号退化为 0（12 例，全部 E0365）

发射点：`src/frontend/analyzer/xtype_ref_resolve.c:1558`（`undefined type '%s'`）。
零初始化 span，行列同时为 0。行号 0 在任何源文件都不存在，**IDE 跳转全部失效**。

涉及：`ffi/007_removed_raw_pointer_type_names`（2 条）、`type/053_enum_value_type_removed`、
`type/118_builtin_generic_bare_requires_args`、`type/119_builtin_generic_arity_rejected`、
`type/bytes_public_types_removed`（3 条）、`type/retired_scalar_type_spellings_removed`（3 条）、
`type/sync_workqueue_requires_import`。

### 族 4：显式类型实参路径丢位置，推断路径不丢（8 例）

嫌疑发射点 `xa_check_explicit_type_args`。同一行、同一方法，只差一个 `<T>`，
位置就从精确列塌成行首。

对照组：`type/139_..._inferred` → 32:9、`type/143_..._forwarding_unsatisfied` → 13:12、
`type/byte_slice_load_requires_type_arg` → 2:17、`type/byte_slice_reinterpret_requires_type_arg` → 3:30。

### 族 5：for-in 可迭代性检查跨行错位（7 例）

用的是**上一条语句**的行号，最离谱的指到了 `fn main() {` 和 enum 的收尾 `}`。

三个探针确认这不是 off-by-one，而是取了迭代变量的**声明位置**：

| 探针 | 声明行 | for-in 行 | 报告位置 |
|---|---|---|---|
| 声明紧邻 for | 1 | 2 | **1:1** |
| 声明与 for 隔两个空行 | 4 | 6 | **4:1** |
| 包在函数体里 | 2 | 3 | **2:1** |

**关键对照**：同在 for-in 上的**可空性**检查（`type/023_for_in_nullable_collection` → 7:15、
`type/024` → 4:15）位置完全正确——两条检查走了不同的位置来源。
另一个对照：`type/007_non_callable`（`var x = 42` 后 `x()`）报的是**调用点**而非声明点，
说明"指向使用点"编译器做得到，for-in 这条路径是孤立回退。

涉及：`type/019/020/021_for_in_range_keyvalue/022_for_in_range_literal_keyvalue/
023_for_in_unconstrained_type_param/054_enum_for_in_removed/072`、`type/009_non_iterable`。

### 族 6：可选链家族整族退化（4 例）

`?.` / `?.()` / `?[]` 四个子形式无一幸免，而**非可选形式位置全对**
（`type/134_enum_variants_on_value` → 7:13、`type/135` → 3:12、`type/136` → 7:24）。
optional-chain 节点构造处统一没填 span。

### 族 7：无名字可指的运算符表达式退化到语句行首（8 例）

`await` / `as` / `[:]` / `[]`——这些表达式没有标识符 token 可以"顺手"当位置，
而所有带名字的形式位置都对。提示位置取自"最近的命名子节点"而非表达式自身 span。
`await` 四个变体全中，同一发射点。

### 族 8：`mem` 布局内建五条诊断共用一个语句级 loc（3 例）

`xa_mem_layout_return_type()`（`src/frontend/analyzer/xanalyzer_visitor_call.c:4011`）
在函数入口一次性构造 `XrLocation loc`，然后喂给函数内**全部五条**诊断。
更精确的锚点就在同作用域现成可用：`call->type_args[0]`、`call->arguments[0]`。

最硬的一对：`stdlib/mem_pointer_constructor_requires_integer`（3:1）与
`stdlib/mem_pointer_constructor_requires_type_arg`（3:13）是**同一行形状**
`var p = mem.XXX<u8>(...)`，一个丢一个不丢。

### 族 9：parallel intrinsic 走裸 `fprintf`，位置被拼进消息正文（3 例）

`src/ir/xi_lower_expr.c:1680-1695`：

```c
fprintf(stderr, "error: parallel.%s expected (Range, inline (item) lambda[, literal "
                "parallel.Options(...)]) at line %d\n", source_name, node ? node->line : -1);
```

行号有，但被拼进消息文本（`at line 5`），没进可解析的位置栏，
于是期望文件只能记 `--> ?`。`parallel.Plan.*` 同族（`xi_lower_expr.c:9346-9371`）同样如此。
**这类诊断连 `-->` 行都没有。**

### 族 10：复合赋值 / 负数字面量 —— 一个消息两个调用方，只有一支漏了（5 例）

`type/044_narrow_integer_literal_overflow` 一个文件内就是完整对照：
`128`→9:13 ✅、`256`→11:13 ✅、`4294967296`→13:14 ✅，但 `-129`→10:0 ✗、`-1`→12:0 ✗。
一元负号包裹后 span 没上传。**最小复现一行**：`var b: i8 = -129`。

同型：`modulo operator '%' requires integer operands` 在二元形式
（`type/036/037/038`）精确指向 `%`，在复合赋值（`type/039` `x %= 2`、`type/040`）无列。

### 其余小族

- **块字符串**诊断一律锚在开定界符；对"闭定界符同行多余 token""某行缩进不足"
  这类谈论后续行的诊断是错的（`syntax/039`、`syntax/041`）。
- **声明级诊断锚到已走完的那一行**：`syntax/036_posthoc_local_export_removed`
  指到**文件末尾之外**（文件 7 行，诊断报第 8 行）；
  `syntax/012_range_inclusive_missing_end` 同样越界（1 行文件报第 2 行）。
- **arrow lambda 返回类型诊断被参数列表劫持**（5 例）：空参和无括号时指得准，
  一有 `(...)` 参数列表就塌到左括号后第一个 token。
- **聚合字面量未知字段指类型名而非字段名**（3 例）：而同为 E0380 的成员读取路径指得准，
  E0381 缺失字段指字面量**是对的**——同一个字面量检查器该分叉却没分。
- **`'unknown' type has been removed`** 报的是 `unknown` 之后那个 token 的位置（5 例）。
  `type/source_unknown_cast_removed` 是决定性证据：`unknown` 是第 1 行最后一个 token，
  诊断**跨行跑到第 2 行第 1 列**。

## 五、一个需要编译器团队定夺的策略问题（**没有**标记为缺陷）

变量声明处的类型不匹配（`var x: T = expr`）全套一致地锚在**声明名**而非右值上，
两批 agent 各自独立数了 13 例和 3 例，**零例外**。与 TypeScript 的 TS2322 行为一致
（`var x: number = "hello"` 报在 `x` 上）。两批都判定「这是刻意一致，不是偶发丢失」，
因此都没有标记。

**要改是全套级决策，不该由测试审计单方面钉死。** 若决定改成指向右值，
这 16 例需要一起改，`bless_expected.py --write` 会自动重锚。

## 六、修复后怎么验收

```bash
# 1. 客观数字应当下降，并把脚本里的预算同步调低
XRAY=$PWD/build-nofp/xray python3 tests/compile_errors/check_diagnostic_positions.py

# 2. 重锚位置（只改位置与错误码，从不改措辞）
XRAY=$PWD/build-nofp/xray python3 tests/compile_errors/bless_expected.py --write

# 3. 删掉已修复用例 .expected 里的 misplaced 行，gate 从此要求正确位置
XRAY=$PWD/build-nofp/xray python3 tests/compile_errors/run_compile_error_tests.py
```

第 3 步的 `Misplaced diagnostic (position gap)` 计数应当相应下降。

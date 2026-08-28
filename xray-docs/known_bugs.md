# 已知问题登记簿

本文件按条目登记两类事实，每个技术断言都必须附 `文件:行` 证据：

- **`DEFECT`** —— 行为与设计意图不符，需要修。
- **`CAPABILITY GAP`** —— 行为与当前实现一致，只是某个形状还没有承载它的实现路径。
  **这类条目不是待修 bug，也不是 TODO。** 登记它是为了让后来的人一眼看出"已经查过了、
  结论是目前无族承载"，不必再花人力重查。

条目格式沿用 `blockers/*.md`：一句话标题说清事实，其下是元数据项、精确来源标识表、
逐节带行号的证据，以及故意未修改的文件清单。

---

## `CAPABILITY GAP`：`arg_spec = "i"` 的 stdlib 符号目前不被任何 exact-authority 族承载

- **类别**：`CAPABILITY GAP` —— 不是缺陷。谓词的行为与它自己写下的契约一致，
  C ABI 与发射代码也没有任何不一致。只是"带不透明整数句柄参数"这个形状
  **目前无族承载，需要时按新族纵切实现**。
- **影响面**：`stdlib/defs/core.def` 的 232 个 `fn` 条目中的 2 个。
- **本轮不实施的理由**：见 §4。是收益为零，不是难度问题——改动本身是分钟级的。
- **登记原因**：这个形状看起来像"一个字符即可修好的 bug"，实际不是；而且仓库里
  存在一条与源码矛盾的注释，已经造成过一次误判（§6）。本条把证据钉死，避免重复调查。

### 精确来源标识

| item | value |
|---|---|
| 分支头 | `00f665c5cfcdc5f0f938ba133c42a258a640d95f` |
| 分支 | `work/5-native-direct-member-00f665c5c` |
| 树状态 | clean |

### 1. 受影响的符号：全表 232 个条目里只有 2 个

`stdlib/defs/core.def` 共 232 个 `fn` 条目，其中 `arg_spec` 含 `'i'` 的只有两条，
都在 `io` 模块：

| 符号 | 脚本签名 | `arg_spec` | def 位置 | 生成物位置 |
|---|---|---|---|---|
| `io.__fileClose` | `(handle: i64): bool` | `"i"` | `stdlib/defs/core.def:2130`（`arg_spec` 在 `:2136`） | `src/stdlib/xstdlib_defs_generated.h:278` |
| `io.__fileRead` | `(handle: i64, maxBytes: i64): Array<u8>?` | `"ii"` | `stdlib/defs/core.def:2156` | `src/stdlib/xstdlib_defs_generated.h:280` |

同族的 `io.__fileOpen` 用的是 `arg_spec: "p"`（`src/stdlib/xstdlib_defs_generated.h:279`），
不在本条范围内。

### 2. 它差在哪：谓词的十三项条件里只差 `arg_spec` 一项

准入谓词是 `xr_stdlib_metadata_exact_native_direct_member`
（`src/stdlib/xstdlib_metadata.h:149`）。它在 `src/stdlib/xstdlib_metadata.h:165`
要求 `arg_spec` 的每一位都是 `'v'`：

```c
if (entry->arg_spec[spec] != 'v')
    return NULL;
```

`io.__fileClose` 的那一行（`src/stdlib/xstdlib_defs_generated.h:278`）对照谓词其余
**12 项条件全部满足**：

| # | 条件 | `io.__fileClose` 实际值 | 结果 |
|---|---|---|---|
| 1 | `xr_stdlib_metadata_unique_func_arity`：模块/选择子/元数唯一 | 全表唯一 | 通过 |
| 2 | `aot_direct` | `true` | 通过 |
| 3 | `aot[0] != '\0'` | `"xrt_io_file_close"` | 通过 |
| 4 | `vm[0] != '\0'` | `"io_fileClose"` | 通过 |
| 5 | `aot_kind == "method"` | `"method"` | 通过 |
| 6 | `ret == "value"` | `"value"` | 通过 |
| 7 | `vm_binding == "normal"` | `"normal"` | 通过 |
| 8 | `aot_enum` 为空 | `""` | 通过 |
| 9 | `vm_ifdef` 为空 | `""` | 通过 |
| 10 | `define` 为空 | `""` | 通过 |
| 11 | `runtime_capabilities == 0` | `0` | 通过 |
| 12 | `spec` 长度 == 实参个数 | `1 == 1` | 通过 |
| — | **`arg_spec` 每一位都是 `'v'`** | **`"i"`** | **不通过** |

派生族 `xr_stdlib_metadata_exact_native_target_leaf`
（`src/stdlib/xstdlib_metadata.h:172`）以上面的谓词为前置，因此同样不承载。

### 3. `'i'` 与 `'v'` 在机制上完全等价

这一节是本条的关键。它推翻了"要让这个形状进族，先得实现原生 int 参数 ABI"的判断——
**不存在需要实现的 ABI**，两者走的是同一条码。

**（a）AOT 发射器只区分 `'s'` 和 `'p'`。**
`src/aot/xi_cgen_stdlib_helpers.inc.c:504` 分支 `'s'`，`:510` 分支 `'p'`，
`:516` 起的 `else` 分支同时接住 `'i'` 和 `'v'`，二者发射的是同一句：

```c
emit_value_as_rep_ctx(ctx, out, arg, XR_REP_TAGGED);
```

**（b）字段注释自己就是这么写的。**
`src/aot/xi_cgen_stdlib_helpers.inc.c:45-48` 的 `arg_spec` 字段说明：

```
 *   's' = string, lowered to specialized (const char *data, int64_t len)
 *   'p' = Path owner, lowered to specialized (const char *data, int64_t len)
 *   'v' = tagged XrValue passed as-is
 *   'i' = opaque int handle, passed as a tagged XrValue like 'v'
```

第 48 行明说 `'i'` 就是"像 `'v'` 一样按 tagged `XrValue` 传"。

**（c）C 侧实现签名一致。**
`'i'` 符号与 `'v'` 符号的形参类型相同，都是 tagged `XrValue`，都在函数体内自己拆箱：

| 符号 | `arg_spec` | C 签名 | 位置 |
|---|---|---|---|
| `io.__fileClose` | `"i"` | `static inline XrValue xrt_io_file_close(XrValue handle_value)` | `src/aot/xrt_io.h:832` |
| `mem.pageFree` | `"vv"` | `static inline XrValue xrt_mem_page_free(XrValue ptr, XrValue bytes)` | `src/aot/xrt_mem.h:331` |

`xrt_io_file_close` 的函数体在 `src/aot/xrt_io.h:833-836` 自己做 `XR_IS_INT` / `XR_TO_INT`，
与 `'v'` 符号的做法没有区别。

**（d）最强反证：一个货真价实的 i64 参数用的是 `"v"`。**
`time.__utcOffsetAt` 的脚本签名是 `(seconds: i64): i64`
（`stdlib/defs/core.def:44-45`，生成物 `src/stdlib/xstdlib_defs_generated.h:150`），
`arg_spec` 却是 `"v"`（`stdlib/defs/core.def:51`）。
所以 `'i'` 标注的从来不是"这是整数参数"，`'v'` 也从不排斥整数参数。

**（e）VM 侧根本不读 `arg_spec`。**
`grep -rn "arg_spec" src/vm/ src/runtime/` 零命中；VM 绑定统一是
`XrValue *args, int argc` 形状。`arg_spec` 是纯 AOT 发射器输入。

**（f）`'i'` 的来历。**
首次引入于 commit `b2cf77ccf`（2026-07-25，`feat(io): add safe streaming binary input`）。
该 commit 的改动清单里**没有** `src/aot/xi_cgen_stdlib_helpers.inc.c`——即它是被静默塞进
def 表的、发射器一侧从未声明过的字符。
commit `677316c9d`（2026-08-09，`fix(aot): fail closed on stdlib arg_spec/argc mismatches`）
事后补上了 §3(b) 那行注释，追认其与 `'v'` 等价，并把 `'i'` 写进生成器的合法字符表
（`tools/stdlibgen/stdlibgen.py:640`，字母表为 `"ipsv"`）。

### 4. 为什么本轮不实施

因为**收益为零**，不是因为改不动。

`XR_SEM_INTRINSIC_NATIVE_MODULE_SCALAR_CALL`（`src/plan/semantic/xr_semantic_plan.h:79`）
**目前没有任何后端消费点**。全仓库对它的引用只有 6 处，没有一处是发射点：

| 位置 | 角色 |
|---|---|
| `src/plan/semantic/xr_semantic_plan.h:79` | 枚举定义 |
| `src/plan/semantic/xr_semantic_builder.c:4981` | 赋值（授予该 kind） |
| `src/plan/semantic/xr_semantic_verify.c:2621` | 校验 |
| `src/plan/target/xr_target_builder.c:1697` | 校验 |
| `src/plan/target/xr_target_verify.c:5544` | 校验 |
| `src/aot/refine/xr_aot_representation_refinement.c:2624` | 表示层形状复核，不发射 |

对 `src/vm/`、`src/runtime/`、`src/aot/emit_c/`、`src/aot/xi_cgen*` 的 grep 均为零命中。

也就是说，这个族只是一张「此调用点有已证目标」的**准入票**，不是**发射配方**。

作为对照，target-leaf 族（`XR_TARGET_CALL_TARGET_NATIVE_TARGET_LEAF_SCALAR` /
`XR_TARGET_CALL_CONVENTION_NATIVE_TARGET_LEAF_SCALAR`）是一路打通到 VM 与 cgen 的：

| 位置 | 角色 |
|---|---|
| `src/vm/xr_typed_dispatch.c:1562` | VM 分发 |
| `src/aot/xi_cgen_dispatch_helpers.inc.c:5576` | cgen 分发 |
| `src/aot/xaot_boundary.c:87` | AOT 边界 |
| `src/aot/emit_c/xr_c_program_emission.c:544` | C 程序发射 |

**所以在该族有发射路径之前，让 `io.__fileClose` 进族的收益是零。**

### 5. 将来要做时怎么做

两条路都可行，代价不同，此处不替后人决策：

**路 A：改 def。** 把 `stdlib/defs/core.def:2136` 的 `"i"` 改成 `"v"`（`__fileRead`
的 `"ii"` 同理）。生成器已接受 `'v'`，无需改任何 C。
代价：抹掉目前**唯一**标注"这个参数是裸 `FILE*` 强转出来的不透明句柄"的信号。
该 hazard 记录在 `analysis/a-stdlib-native-leaf-dossier.md:320-321`：
`io.__fileOpen` 把 `FILE*` 转成 `i64` 交给脚本，`io.__fileRead`/`__fileClose`
再把任意脚本整数直接转回 `FILE*`（VM 侧 `stdlib/io/io.c:235`、`:245`；
AOT 侧同形，`src/aot/xrt_io.h:821`、`:832`，强转本身在 `:826`）。
注意 dossier 该条引用的 AOT 行号 `xrt_io.h:923/934` 已经失效——该文件现共 857 行，
上表是本条登记时实测的当前行号。

**路 B：改谓词。** 让 `src/stdlib/xstdlib_metadata.h:165` 的 `arg_spec` 检查同时接受
`'v'` 和 `'i'`。保留句柄标注，但需要在该函数的头注释里论证"接纳不透明句柄参数对
exact 授权是安全的"——鉴于 §3 已确证两者的 C ABI 与发射代码完全一致，该论证成立。

两条都是**分钟级**工作量。真正的工作量在于**先给
`XR_SEM_INTRINSIC_NATIVE_MODULE_SCALAR_CALL` 建立后端发射路径**，那才是纵切；
在那之前做哪一条都只是改了个标注。

### 6. 一条与源码矛盾的注释（先前误判的根源）

`tests/unit/ir/test_xi_cgen.c:2907-2911` 是一段注释，其中 `:2909-2911` 写道：

```
 * `io.__fileClose` looked like a direct import too, but its declaration
 * passes a native int and carries no target-leaf entry, so no family
 * covers it and the plan is refused before code generation is reached.
```

其中 **"its declaration passes a native int" 是错的**：`io.__fileClose` 的 AOT 声明
是 `xrt_io_file_close(XrValue handle_value)`（`src/aot/xrt_io.h:832`），
传的是 tagged `XrValue`，不是原生 int（§3(a)(b)(c) 已证）。
同句的后半（"carries no target-leaf entry，因此无族承载"）是对的：
该行的 `target_leaf` 字段确为 `XR_STDLIB_TARGET_LEAF_NONE`
（`src/stdlib/xstdlib_defs_generated.h:278`）。

先前"需要实现原生 int 参数 ABI"的判断很可能就源于这条注释。
**本条不修改该注释**（见下节），但记明其与源码矛盾，供后来者对照。

### 故意未修改的文件

```
stdlib/defs/core.def
src/stdlib/xstdlib_metadata.h
src/aot/xi_cgen_stdlib_helpers.inc.c
tests/unit/ir/test_xi_cgen.c
```

本条只做记账。改 def、改谓词、订正 §6 那条注释都属于"将来要做时"的动作，
且都应当与该族的后端发射路径一并落地，而不是先单独改掉标注。

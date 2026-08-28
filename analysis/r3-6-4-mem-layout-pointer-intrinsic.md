# 6-4：mem 的 layout 与 pointer intrinsic 落地记录

- **分支**：`work/6-4-mem-intrinsic-34be0379c`
- **基线**：`34be0379c`
- **门的变化**：`stdlib_no_public_native_surface` 128 → 117；mem 从 17 → **6**

## 一、这 11 个符号最终长什么样

`sizeOf` `alignOf` `offsetOf` `ptr` `mutPtr` `addr` `load` `store` `slice`
`withSliceMut` `assumeInitialized` —— 它们现在**在 `stdlib/` 下没有任何一行声明**。
唯一的声明是 `src/frontend/analyzer/xa_intrinsic_registry.def` 的 MEMORY 家族段里
11 条 `XA_INTRINSIC(MEM_*, 3101..3111, "mem.<member>", ...)`。

### 为什么 registry key 可以就是全部声明

编译器里已经有两个地方按 `"<模块>.<成员>"` 拼 key 查 registry，**不需要被调用方先有符号**：

| 位置 | 作用 |
|---|---|
| `xanalyzer_visitor_call.c:520-568` `xa_record_resolved_intrinsic_call` | 调用点解析：拿 `xa_call_object_module_name(callee->object)` + 成员名拼 key，命中就把 intrinsic id 写进 resolved-call 表 |
| 同文件 `:362-390` `xa_refresh_imported_symbol_metadata` | `from mem import X` 这种导入形式，模块导出里查不到时也走同一张表 |

所以删掉 core.def 的 fn 行**不会**让 `mem.sizeOf<i64>()` 变成
`stdlib module 'mem' has no member 'sizeOf'`：分析器在成员解析之前就按 registry
身份接管了这个调用（`xanalyzer_visitor_call.c` 的
`xa_mem_module_intrinsic_return_type`），成员访问根本不会被 infer。

### 改造前后的识别权威

| | 改造前 | 改造后 |
|---|---|---|
| analyzer 识别 | 6 个断言函数，各自 `strcmp(ma->name, "sizeOf")` + `xa_call_object_is_module(ctx, ma->object, "mem")` | 一次 `xa_intrinsic_by_key("<模块>.<成员>")`，模块名和成员名都不再硬编码 |
| 成员名从哪来 | C 字符串字面量 | `xa_intrinsic_source_member(desc)`（registry key 的 `.` 后半段） |
| lowering 识别 | `xi_lower_expr.c` 三处 `lower_call_object_is_module(l, ..., "mem")` 块 | `lower_resolved_intrinsic_call` 按 `desc->lowering` 分派（`XA_INTRINSIC_LOWERING_MEM_*`） |
| allocation 契约 | core.def 的 `allocation: "no_heap"` | `desc->allocation`（`xanalyzer_allocation.c` 的 `alloc_module_contract` 在 builtin 表查不到时回退到 registry） |

**产出的 Xi 值一个字节都没变**：`lower_mem_*_call` 这批函数原样保留，只是改由
canonical identity 分派进去。所以 IR 层的期望没有漂移。

### 顺手删掉的特例

`xa_call_is_mem_addr` 按成员名 `"addr"` 匹配一个叫 `mem` 的裸变量，然后让
`:7920` 附近的通用实参循环**跳过 slot 0** —— 因为 .def 签名写的是
`(ptr: Ptr<u8>)`，而调用点接受任意 `Ptr<T>`。这个特例连同它在两处的消费点一起删了，
`mem.addr` 的实参契约（指针类型检查 + 借用溯源逃逸检查）现在和另外十个并排放在
`xa_mem_addr_return_type` 里。

## 二、那个 blocker **没有**被解掉，而且解不掉

`blockers/r3-6-xray-wrapper-over-private-leaf-has-no-exact-target-authority.md`
说的形状是：**stdlib 模块的 Xray 函数体调用本模块私有原生叶子、且无返回值**。
`mem.xr` 里有 9 个这种函数：

```
fence  prefetch  cacheFlush  cacheInvalidate  nontemporalStore
copy   move      set         volatileStore
```

**这 9 个和本 lane 的 11 个符号没有一个重合。** 它们是 `mem.xr` 里真正的 Xray 包装体
（`export fn fence(ordering: i64) { __fence(ordering) }` 这种），本轮一行没动；
我的 11 个恰恰相反，是**从来就没有 Xray 体、现在连 .def 行也没有**的编译期 intrinsic。

差分数据也印证了这一点：blocker 点名的两个用例
`semantics/ffi/rawptr_static_null.xr` 和 `semantics/stdlib/mem_fence_shared_core.xr`
在本分支**仍然**停在 `Cases that stopped building` 里。前者用了 3 次 `mem.addr`
（已经是 registry intrinsic，lowering 直出 `XI_CONVERT`，不再是叶子调用），
但它 `import mem` 让程序变成两模块图，`mem.xr` 的 9 个包装体照样要过目标层 ——
**卡住的是模块图里的别人，不是这个用例自己写的那几行。**

结论：要解这个 blocker，得动 `mem.xr` 那 9 个包装体的形状，或者按 blocker 自己的
建议由 `src/plan/target/` 的负责人补上 exact target authority。**都不在 6-4 的文件边界内。**

## 三、差分网的实测（本分支）

**必须用 `XRAY_DIFF_JOBS=4`**：16 并发跑会因为
`[RUN_PROBE_FAILED] native-run: probe stage timed out after 10000 ms`
凭空造出 11 条 refusal，把 `basic/loops.xr`、四个 `limits/*`、四个 `cleanup/defer_*`
这些跟改动毫无关系的用例算成"停止构建"。串行复跑后它们全部回来。

| | 数值 |
|---|---|
| passed | **150** |
| failed | 0 |
| refused | 525 |
| not comparable | 525（listed 515） |
| Cases that stopped building | 14（全部是既有条目） |

150 passed 与 blocker 里记录的「mem 迁移后 150」**逐字吻合**，说明本 lane 没有让
差分面再退一步。

顺带按脚本要求删掉了 4 条已经能构建的过期条目
（`force_unwrap_error_shape` / `object_width_nullable_field_omission` /
`pass_ifconv_select_shape` / `object_optional_sparse_fields`）——
`run_backend_diff.py` 明说「The list may only shrink」，它们和 mem 无关，是上一轮的遗留。

## 四、留给后面的人

- `stdlib/mem/mem.c` 里那 11 个入口函数（3 个 `return xr_int(0)` 的 layout 桩、
  5 个自称 "never valid dynamic calls" 的 `return xr_null()` 桩、以及
  `mem_ptr`/`mem_mut_ptr`/`mem_addr`）一并删了。前 8 个本来就不可达；
  后 3 个的 AOT 入口 `xrt_mem_ptr` / `xrt_mem_mut_ptr` / `xrt_mem_addr` 从来没有被
  lowering 发射过 —— `lower_mem_pointer_constructor_call` 和
  `lower_mem_addr_pointer_call` 产出的是 `XI_CONVERT`，这正是
  `freestanding_mem_core.expect:11` 的 `c_not_contains=xrt_mem_mut_ptr` 和另外
  8 处 `c_not_contains=xrt_mem_addr` 一直在断言的事。
- mem 剩下的 6 个公开原生符号是 `Buffer` + 它的 4 个方法（6-1 的元数据缺口）
  和 `pageAlloc`（双 arity，语言级，无函数重载）。

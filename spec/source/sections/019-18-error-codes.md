---
id: spec.18_error_codes
order: 019
---

<!-- xr-spec:cn -->
---

## 18. 错误码 (Error Codes)

> 唯一真值源：`src/runtime/xerror_codes.h`。`XrErrorCode` 在 `src/runtime/xerror.h` 中是 `i64`；用户可见格式为 `Exxxx`。编号可保留空洞，不得用文档中不存在的名称补齐。

本章普通表项以头文件为准；下列错误签名编号已登记，但发射点与语义检查仍待接通。旧 defer 错误兜底已从目标运行时表移除，实际执行路径的删除仍属于 Task329 P2，必须保留 §8.2.5 的独立 panic 清理边界。

### 18.1 Lexer 与 parser

| 代码 | C 名称 | 含义 |
|--|--|--|
| `E0101` | `XR_ERR_LEX_INVALID_CHAR` | 非法字符 |
| `E0102` | `XR_ERR_LEX_UNTERMINATED_STR` | 字符串未终止 |
| `E0103` | `XR_ERR_LEX_INVALID_NUMBER` | 非法数字字面量 |
| `E0104` | `XR_ERR_LEX_INVALID_ESCAPE` | 非法转义 |

#### Parser

| 代码 | C 名称 | 含义 |
|--|--|--|
| `E0201` | `XR_ERR_SYN_UNEXPECTED_TOKEN` | 意外 token |
| `E0202` | `XR_ERR_SYN_EXPECTED_EXPR` | 缺少表达式 |
| `E0203` | `XR_ERR_SYN_EXPECTED_STMT` | 缺少语句 |
| `E0204` | `XR_ERR_SYN_UNCLOSED_PAREN` | 圆括号未闭合 |
| `E0205` | `XR_ERR_SYN_UNCLOSED_BRACE` | 花括号未闭合 |
| `E0206` | `XR_ERR_SYN_UNCLOSED_BRACKET` | 方括号未闭合 |
| `E0207` | `XR_ERR_SYN_INVALID_ASSIGN` | 非法赋值目标或形式 |
| `E0208` | `XR_ERR_SYN_EFFECTLESS_STMT` | 表达式语句没有任何效果，结果被丢弃（见 §1.2.1） |

### 18.2 编译与静态分析

| 代码 | C 名称 | 含义 |
|--|--|--|
| `E0301` | `XR_ERR_CMP_UNDEFINED_VAR` | 编译器阶段未定义变量 |
| `E0302` | `XR_ERR_CMP_REDEFINED_VAR` | 重复定义变量 |
| `E0303` | `XR_ERR_CMP_CONST_ASSIGN` | 给常量赋值 |
| `E0304` | `XR_ERR_CMP_INVALID_BREAK` | 非法 `break` |
| `E0305` | `XR_ERR_CMP_INVALID_CONTINUE` | 非法 `continue` |
| `E0306` | `XR_ERR_CMP_INVALID_RETURN` | 非法 `return` |
| `E0307` | `XR_ERR_CMP_TOO_MANY_PARAMS` | 参数过多 |
| `E0308` | `XR_ERR_CMP_TOO_MANY_LOCALS` | 局部变量过多 |
| `E0309` | `XR_ERR_CMP_TOO_MANY_CONSTANTS` | 常量过多 |
| `E0310` | `XR_ERR_CMP_TOO_MANY_UPVALUES` | upvalue 过多 |
| `E0311` | `XR_ERR_CMP_JUMP_TOO_LARGE` | 跳转偏移超限 |
| `E0321` | `XR_ERR_TYPE_NOT_CALLABLE` | 静态类型不可调用 |
| `E0322` | `XR_ERR_TYPE_NOT_INDEXABLE` | 静态类型不可下标 |
| `E0323` | `XR_ERR_TYPE_NOT_ITERABLE` | 静态类型不可迭代 |
| `E0324` | `XR_ERR_TYPE_INVALID_OPERAND` | 操作数类型非法 |

#### 错误签名诊断（编号已登记，实现待接通）

下表按 Task329 §2.9 的裁决定号，头文件与位置/代码断言已登记；解析器和语义发射点尚未接通，负例仍未通过。编号登记不表示编译器已经能够发出这些诊断。

| 代码 | C 名称 | 触发条件 |
|--|--|--|
| `E0330` | `XR_ERR_ANALYZE_TRY_REQUIRED` | 可能抛错的调用缺少 `try`，且不在 `try { }` 保护区域内 |
| `E0331` | `XR_ERR_ANALYZE_UNDECLARED_ERROR` | 逃逸错误不包含于所在函数声明的错误类型 |
| `E0332` | `XR_ERR_ANALYZE_ERROR_OVERRIDE` | 实现或覆写的错误类型不是被实现签名的子集 |
| `E0333` | `XR_ERR_ANALYZE_ERASED_THROWS` | freestanding 或 `@strict(alloc)` 中使用擦除错误类型，包括裸 `throws` 与 `throws(Error)` |
| `E0334` | `XR_ERR_ANALYZE_CATCH_TYPE_NAME` | catch 头部单个大写开头标识符，提示使用 `catch (e: T)` |
| `E0335` | `XR_ERR_ANALYZE_IMPORTED_TOP_LEVEL` | 被导入模块包含独立的表达式语句或控制流语句；移到入口文件，模块只保留声明及状态初始化 |

#### Analyzer

| 代码 | C 名称 | 含义 |
|--|--|--|
| `E0350` | `XR_ERR_ANALYZE` | 通用 analyzer 错误 |
| `E0351` | `XR_ERR_ANALYZE_UNDEFINED_VAR` | 名称未定义 |
| `E0352` | `XR_ERR_ANALYZE_TYPE_MISMATCH` | 类型不匹配 |
| `E0353` | `XR_ERR_ANALYZE_CONST_ASSIGN` | 静态检查发现常量赋值 |
| `E0354` | `XR_ERR_ANALYZE_NOT_CALLABLE` | 被调用值不可调用 |
| `E0355` | `XR_ERR_ANALYZE_WRONG_ARG_COUNT` | 实参数量错误 |
| `E0356` | `XR_ERR_ANALYZE_ARG_TYPE` | 实参类型错误 |
| `E0357` | `XR_ERR_ANALYZE_GENERIC_COUNT` | 泛型实参数量错误 |
| `E0358` | `XR_ERR_ANALYZE_GENERIC_CONSTRAINT` | 泛型约束不满足 |
| `E0359` | `XR_ERR_ANALYZE_SUPER_FIRST` | `super(...)` 不是首个构造动作 |
| `E0360` | `XR_ERR_ANALYZE_SUPER_THIS` | `super(...)` 前访问 `this` |
| `E0361` | `XR_ERR_ANALYZE_SUPER_REQUIRED` | 派生构造函数缺少 `super(...)` |
| `E0362` | `XR_ERR_ANALYZE_SUPER_INVALID` | 非派生类非法使用 `super` |
| `E0363` | `XR_ERR_ANALYZE_CLOSURE_CAPTURE` | 闭包捕获不安全 |
| `E0364` | `XR_ERR_ANALYZE_AWAIT_TYPE` | `await` 操作数类型非法 |
| `E0365` | `XR_ERR_ANALYZE_MISSING_TYPE` | 缺少可推断的类型或初始化器 |
| `E0366` | `XR_ERR_ANALYZE_INTRINSIC_NOT_A_VALUE` | 将 intrinsic 当作普通值使用 |
| `E0367` | `XR_ERR_ANALYZE_INTERFACE_NOT_IMPLEMENTED` | 未完整实现 interface |
| `E0368` | `XR_ERR_ANALYZE_TUPLE_FIELD_NAME` | tuple 字段名非法 |
| `E0369` | `XR_ERR_ANALYZE_TUPLE_FIELD_RANGE` | tuple 字段越界 |
| `E0370` | `XR_ERR_ANALYZE_THROW_NON_EXCEPTION` | `throw` 操作数不是允许的 enum 错误值 |
| `E0371` | `XR_ERR_ANALYZE_MATCH_NOT_EXHAUSTIVE` | `match` 不穷尽 |
| `E0372` | `XR_ERR_ANALYZE_USED_BEFORE_ASSIGN` | 赋值前使用 |
| `E0373` | `XR_ERR_ANALYZE_TUPLE_IMMUTABLE` | 修改不可变 tuple |
| `E0374` | `XR_ERR_ANALYZE_OVERRIDE_MISMATCH` | override 契约不匹配 |
| `E0375` | `XR_ERR_ANALYZE_HASHABLE_CONTRACT` | Map/Set 元素缺少 hash/equality 契约 |
| `E0376` | `XR_ERR_ANALYZE_CONDITION_TYPE` | 条件类型非法 |
| `E0377` | `XR_ERR_ANALYZE_VISIBILITY` | 可见性违规 |
| `E0378` | `XR_ERR_ANALYZE_CONST_FIELD` | 修改 const 字段 |
| `E0379` | `XR_ERR_ANALYZE_POSSIBLY_NULL` | 可能为 null 的值被不安全使用（触发规则见 §2.13 N-12）|
| `E0380` | `XR_ERR_ANALYZE_UNKNOWN_FIELD` | 访问或设置类型上不存在的字段 / 成员 |
| `E0381` | `XR_ERR_ANALYZE_MISSING_FIELD` | 聚合字面量缺少必填字段 |
| `E0382` | `XR_ERR_ANALYZE_BORROW_CONFLICT` | 借用（`Slice<T>` 视图或 `ref` / 原始指针）存活期间使 owner 失效 |
| `E0383` | `XR_ERR_ANALYZE_BORROW_ESCAPE` | 借用值逃逸出 owner 作用域（返回、字段、容器、闭包捕获、跨执行边界） |
| `E0384` | `XR_ERR_ANALYZE_BORROW_SOURCE` | 借用来源不是稳定且唯一可推断的 owner（临时 owner、多来源返回、借自局部值） |
| `E0385` | `XR_ERR_ANALYZE_GENERATOR_SUSPEND` | 旧分析器的生成器调度禁令；当前 Checked 生成器按 §3.16.2 的独立 frame/消费者等待规则及 §17.39 的实际准入边界检查 |
| `E0386` | `XR_ERR_ANALYZE_GENERATOR_DEFER` | 旧分析器的生成器 defer 禁令；当前 Checked 生成器已由 §3.16.3 的登记清理和关闭规则替代，不使用此禁令 |
| `E0387` | `XR_ERR_ANALYZE_DEFER_MAY_THROW` | 错误可从 `defer` cleanup 块逃逸（见 §8.3.1） |
| `E0388` | `XR_ERR_ANALYZE_MONO_BUDGET` | 程序超出单态化实例预算（广度）；每个实例克隆一份完整声明——见 §9.4 |
| `E0389` | `XR_ERR_ANALYZE_MONO_DEPTH` | 单态化嵌套超出深度预算；多态递归（`f<T>` 请求 `f<Box<T>>`）没有有限特化，必然触发——见 §9.4 |
| `E0390` | `XR_ERR_ANALYZE_UNION_INDISCRIMINABLE` | union 成员在运行期无法判别（同一数值族出现多个成员）|
| `E0391` | `XR_ERR_ANALYZE_MOVE_NOT_UNIQUE` | `move` 的唯一性证据不成立（存活别名 / 已发布根 / 来源未知 / 存储计划不完整）|
| `E0392` | `XR_ERR_ANALYZE_DEFER_SUSPEND` | defer 体抵达调度器挂起点或创建任务——见 §4.9 / §8.3.1 |
| `E0393` | `XR_ERR_ANALYZE_CLOSURE_CYCLE` | 闭包与捕获对象形成引用环 |
| `E0394` | `XR_ERR_ANALYZE_WEAK_FIELD` | weak 字段类型或执行域约束违规 |
| `E0395` | `XR_ERR_ANALYZE_DEFER_CONTROL` | defer 体以 `return` 退出所在函数，或以 `break` / `continue` 跳出 cleanup 边界 |
| `E0396` | `XR_ERR_ANALYZE_BINDING_STATE` | 读取已移动或可能已移动的绑定 |
| `E0397` | `XR_ERR_ANALYZE_MOVE_CAPABILITY` | move 的能力或约束不满足 |

### 18.3 运行时

| 代码 | C 名称 | 含义 |
|--|--|--|
| `E0400` | `XR_ERR_RUNTIME` | 通用运行时错误 |
| `E0401` | `XR_ERR_TYPE_NO_PROPERTY` | 类型没有该属性 |
| `E0402` | `XR_ERR_TYPE_NO_INDEX` | 值不可下标 |
| `E0403` | `XR_ERR_TYPE_NO_CALL` | 值不可调用 |
| `E0404` | `XR_ERR_TYPE_MISMATCH` | 运行时类型不匹配 |
| `E0405` | `XR_ERR_TYPE_NO_METHOD` | 类型没有该方法 |
| `E0406` | `XR_ERR_TYPE_NO_OPERATOR` | 类型不支持该运算符 |

#### Null、算术与容器

| 代码 | C 名称 | 含义 |
|--|--|--|
| `E0410` | `XR_ERR_NULL_PROPERTY` | 对 null 取属性 |
| `E0411` | `XR_ERR_NULL_INDEX` | 对 null 下标 |
| `E0412` | `XR_ERR_NULL_CALL` | 调用 null |
| `E0413` | `XR_ERR_NULL_UNWRAP` | 强制解包 null |
| `E0420` | `XR_ERR_DIV_BY_ZERO` | 除零 |
| `E0421` | `XR_ERR_MOD_BY_ZERO` | 模零 |
| `E0422` | `XR_ERR_OVERFLOW` | 算术溢出或数值转换越界（含 NaN / 无穷大转整数） |
| `E0430` | `XR_ERR_INDEX_OUT_OF_BOUNDS` | 下标越界 |
| `E0431` | `XR_ERR_KEY_NOT_FOUND` | Map 键不存在 |
| `E0432` | `XR_ERR_ITERATOR_EXHAUSTED` | 耗尽后仍调用 `Iterator<T>` 的 `next()` / `nth()`，违反两步拉取协议——见 §5.3.6 |
| `E0433` | `XR_ERR_STRING_SLICE_RANGE` | `string.slice` 的 rune 半开区间非法；消息为 `string slice range is invalid` |

#### 系统、调用、coroutine 与 stdlib

| 代码 | C 名称 | 含义 |
|--|--|--|
| `E0440` | `XR_ERR_STACK_OVERFLOW` | 栈溢出 |
| `E0441` | `XR_ERR_OUT_OF_MEMORY` | 内存不足 |
| `E0442` | `XR_ERR_MATCH_FAILURE` | 运行时 match 失败 |
| `E0444` | `XR_ERR_DEFER_ASYNC` | defer cleanup 尝试创建任务或抵达调度器；在任何该类侧效前失败关闭 |
| `E0445` | `XR_ERR_ASSERTION_FAILED` | 断言失败的 panic |
| `E0450` | `XR_ERR_WRONG_ARG_COUNT` | 运行时实参数量错误 |
| `E0451` | `XR_ERR_INVALID_ARG_TYPE` | 运行时实参类型错误 |
| `E0452` | `XR_ERR_INVALID_ARG_VALUE` | 非法实参值的保留编号；当前没有发射点 |
| `E0460` | `XR_ERR_CORO_DEAD` | 操作已结束 coroutine |
| `E0461` | `XR_ERR_CORO_CANCELLED` | coroutine 已取消 |
| `E0462` | `XR_ERR_TASK_ALREADY_TAKEN` | Task 的 transferable 结果已被取走，不能再次取用 |
| `E0470` | `XR_ERR_JSON_PARSE` | JSON 解析失败 |
| `E0471` | `XR_ERR_JSON_INVALID` | JSON 值或操作非法 |
| `E0475` | `XR_ERR_REGEX_COMPILE` | regex 编译失败 |
| `E0476` | `XR_ERR_REGEX_PATTERN` | regex pattern 非法 |
| `E0480` | `XR_ERR_TLS_UNAVAILABLE` | TLS 能力不可用 |

### 18.4 模块、IO 与 coroutine

| 代码 | C 名称 | 含义 |
|--|--|--|
| `E0501` | `XR_ERR_MOD_NOT_FOUND` | 模块不存在 |
| `E0502` | `XR_ERR_MOD_LOAD_FAILED` | 模块加载失败 |
| `E0503` | `XR_ERR_MOD_NO_EXPORT` | 名称未导出 |
| `E0504` | `XR_ERR_MOD_CIRCULAR` | 循环模块依赖 |
| `E0601` | `XR_ERR_IO_FILE_NOT_FOUND` | 文件不存在 |
| `E0602` | `XR_ERR_IO_READ_FAILED` | 读取失败 |
| `E0603` | `XR_ERR_IO_WRITE_FAILED` | 写入失败 |
| `E0604` | `XR_ERR_IO_PERMISSION_DENIED` | IO 权限不足 |
| `E0701` | `XR_ERR_CORO_DEADLOCK` | coroutine 死锁 |
| `E0702` | `XR_ERR_CORO_CHANNEL_CLOSED` | channel 已关闭 |
| `E0703` | `XR_ERR_CORO_LIMIT_EXCEEDED` | coroutine 限额超出 |

### 18.5 语法引导与内部错误

| 代码 | C 名称 | 被拒形式 / 含义 |
|--|--|--|
| `E0801` | `XR_ERR_SYN_RETURN_MULTI_REMOVED` | `return a, b` 无效；元组返回写作 `return (a, b)` |
| `E0802` | `XR_ERR_SYN_BINDING_CAPABILITY_REMOVED` | `owned` / `shared` 绑定语法无效；使用 `var` 或 `const`，所有权与共享由值能力和显式 copy/move 边界推断 |
| `E0803` | `XR_ERR_SYN_FOR_FLAT_REMOVED` | 裸 key/value `for` 形式无效 |
| `E0804` | `XR_ERR_SYN_VOID_REMOVED` | `-> void` 无效；无返回值写作 `-> ()` 或省略 |
| `E0805` | `XR_ERR_SYN_PARAM_MODE_PREFIX_REMOVED` | 参数 mode 必须写在冒号与类型之间 |
| `E0806` | `XR_ERR_SYN_PARAM_MOVE_MODE_REMOVED` | `move` 是实参转移表达式，不是参数 mode |
| `E0807` | `XR_ERR_SYN_PARAM_MODE_COMBINED_REMOVED` | 非法组合参数 mode |
| `E0808` | `XR_ERR_SYN_PARAM_MODE_POSTFIX_REMOVED` | 参数 mode 不能写在类型之后 |
| `E0809` | `XR_ERR_SYN_CALL_IN_MARKER_REMOVED` | call-site `in` marker；普通 `in` 参数调用不写 marker |
| `E0900` | `XR_ERR_INTERNAL` | 内部错误 |
| `E0901` | `XR_ERR_NOT_IMPLEMENTED` | 尚未实现 |
| `E0999` | `XR_ERR_UNKNOWN` | 未知错误 |

运行时 panic 通道使用 prelude `PanicInfo`；用户级 `throw <enum>` 走值返回错误通道。二者的语义见 §8 与 §16，不应仅凭错误码区段推断传播机制。
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 18. Error Codes

> The single source of truth is `src/runtime/xerror_codes.h`. `XrErrorCode` is an `i64` in `src/runtime/xerror.h`; user-facing rendering is `Exxxx`. Gaps are allowed and must not be filled with names absent from the header.

Ordinary entries follow the header; the error-signature numbers below are registered, while emitters and semantic checking remain pending. The old defer-error fallback is removed from the target runtime table. Removing its execution path remains Task329 P2 work and must preserve the independent cleanup-panic boundary in §8.2.5.

### 18.1 Lexer and Parser

| Code | C name | Meaning |
|--|--|--|
| `E0101` | `XR_ERR_LEX_INVALID_CHAR` | invalid character |
| `E0102` | `XR_ERR_LEX_UNTERMINATED_STR` | unterminated string |
| `E0103` | `XR_ERR_LEX_INVALID_NUMBER` | invalid numeric literal |
| `E0104` | `XR_ERR_LEX_INVALID_ESCAPE` | invalid escape |

#### Parser

| Code | C name | Meaning |
|--|--|--|
| `E0201` | `XR_ERR_SYN_UNEXPECTED_TOKEN` | unexpected token |
| `E0202` | `XR_ERR_SYN_EXPECTED_EXPR` | expression expected |
| `E0203` | `XR_ERR_SYN_EXPECTED_STMT` | statement expected |
| `E0204` | `XR_ERR_SYN_UNCLOSED_PAREN` | unclosed parenthesis |
| `E0205` | `XR_ERR_SYN_UNCLOSED_BRACE` | unclosed brace |
| `E0206` | `XR_ERR_SYN_UNCLOSED_BRACKET` | unclosed bracket |
| `E0207` | `XR_ERR_SYN_INVALID_ASSIGN` | invalid assignment target or form |
| `E0208` | `XR_ERR_SYN_EFFECTLESS_STMT` | expression statement has no effect; its result is discarded (see §1.2.1) |

### 18.2 Compilation and Static Analysis

| Code | C name | Meaning |
|--|--|--|
| `E0301` | `XR_ERR_CMP_UNDEFINED_VAR` | undefined variable at compiler stage |
| `E0302` | `XR_ERR_CMP_REDEFINED_VAR` | redefined variable |
| `E0303` | `XR_ERR_CMP_CONST_ASSIGN` | assignment to a constant |
| `E0304` | `XR_ERR_CMP_INVALID_BREAK` | invalid `break` |
| `E0305` | `XR_ERR_CMP_INVALID_CONTINUE` | invalid `continue` |
| `E0306` | `XR_ERR_CMP_INVALID_RETURN` | invalid `return` |
| `E0307` | `XR_ERR_CMP_TOO_MANY_PARAMS` | too many parameters |
| `E0308` | `XR_ERR_CMP_TOO_MANY_LOCALS` | too many locals |
| `E0309` | `XR_ERR_CMP_TOO_MANY_CONSTANTS` | too many constants |
| `E0310` | `XR_ERR_CMP_TOO_MANY_UPVALUES` | too many upvalues |
| `E0311` | `XR_ERR_CMP_JUMP_TOO_LARGE` | jump offset too large |
| `E0321` | `XR_ERR_TYPE_NOT_CALLABLE` | static type is not callable |
| `E0322` | `XR_ERR_TYPE_NOT_INDEXABLE` | static type is not indexable |
| `E0323` | `XR_ERR_TYPE_NOT_ITERABLE` | static type is not iterable |
| `E0324` | `XR_ERR_TYPE_INVALID_OPERAND` | invalid operand type |

#### Error-signature diagnostics (registered numbers, implementation pending)

These numbers follow the Task329 §2.9 decision. Header definitions and position/code assertions are registered; parser and semantic emitters remain unimplemented and the negative cases do not yet pass. Registration is not evidence that the compiler emits these diagnostics.

| Code | C name | Trigger |
|--|--|--|
| `E0330` | `XR_ERR_ANALYZE_TRY_REQUIRED` | Potentially throwing call has no marker or protected try region |
| `E0331` | `XR_ERR_ANALYZE_UNDECLARED_ERROR` | Escaping error is outside the enclosing declared error type |
| `E0332` | `XR_ERR_ANALYZE_ERROR_OVERRIDE` | Implementation or override error type is not a subset of its required signature |
| `E0333` | `XR_ERR_ANALYZE_ERASED_THROWS` | Erased errors in freestanding or @strict(alloc), including bare throws and throws(Error) |
| `E0334` | `XR_ERR_ANALYZE_CATCH_TYPE_NAME` | Single uppercase-leading catch identifier; suggest catch (e: T) |
| `E0335` | `XR_ERR_ANALYZE_IMPORTED_TOP_LEVEL` | Imported module contains a standalone expression or control-flow statement; move it to the entry file and retain only declarations and state initialization |

#### Analyzer

| Code | C name | Meaning |
|--|--|--|
| `E0350` | `XR_ERR_ANALYZE` | generic analyzer error |
| `E0351` | `XR_ERR_ANALYZE_UNDEFINED_VAR` | undefined name |
| `E0352` | `XR_ERR_ANALYZE_TYPE_MISMATCH` | type mismatch |
| `E0353` | `XR_ERR_ANALYZE_CONST_ASSIGN` | analyzer-detected const assignment |
| `E0354` | `XR_ERR_ANALYZE_NOT_CALLABLE` | called value is not callable |
| `E0355` | `XR_ERR_ANALYZE_WRONG_ARG_COUNT` | wrong argument count |
| `E0356` | `XR_ERR_ANALYZE_ARG_TYPE` | wrong argument type |
| `E0357` | `XR_ERR_ANALYZE_GENERIC_COUNT` | wrong generic argument count |
| `E0358` | `XR_ERR_ANALYZE_GENERIC_CONSTRAINT` | generic constraint not satisfied |
| `E0359` | `XR_ERR_ANALYZE_SUPER_FIRST` | `super(...)` is not the first construction action |
| `E0360` | `XR_ERR_ANALYZE_SUPER_THIS` | `this` accessed before `super(...)` |
| `E0361` | `XR_ERR_ANALYZE_SUPER_REQUIRED` | derived constructor omits `super(...)` |
| `E0362` | `XR_ERR_ANALYZE_SUPER_INVALID` | invalid `super` in a non-derived class |
| `E0363` | `XR_ERR_ANALYZE_CLOSURE_CAPTURE` | unsafe closure capture |
| `E0364` | `XR_ERR_ANALYZE_AWAIT_TYPE` | invalid `await` operand type |
| `E0365` | `XR_ERR_ANALYZE_MISSING_TYPE` | no inferable type or initializer |
| `E0366` | `XR_ERR_ANALYZE_INTRINSIC_NOT_A_VALUE` | Intrinsic used as an ordinary value |
| `E0367` | `XR_ERR_ANALYZE_INTERFACE_NOT_IMPLEMENTED` | incomplete interface implementation |
| `E0368` | `XR_ERR_ANALYZE_TUPLE_FIELD_NAME` | invalid tuple field name |
| `E0369` | `XR_ERR_ANALYZE_TUPLE_FIELD_RANGE` | tuple field out of range |
| `E0370` | `XR_ERR_ANALYZE_THROW_NON_EXCEPTION` | `throw` operand is not an allowed enum error value |
| `E0371` | `XR_ERR_ANALYZE_MATCH_NOT_EXHAUSTIVE` | non-exhaustive `match` |
| `E0372` | `XR_ERR_ANALYZE_USED_BEFORE_ASSIGN` | use before assignment |
| `E0373` | `XR_ERR_ANALYZE_TUPLE_IMMUTABLE` | mutation of an immutable tuple |
| `E0374` | `XR_ERR_ANALYZE_OVERRIDE_MISMATCH` | override contract mismatch |
| `E0375` | `XR_ERR_ANALYZE_HASHABLE_CONTRACT` | Map/Set element lacks hash/equality contract |
| `E0376` | `XR_ERR_ANALYZE_CONDITION_TYPE` | invalid condition type |
| `E0377` | `XR_ERR_ANALYZE_VISIBILITY` | visibility violation |
| `E0378` | `XR_ERR_ANALYZE_CONST_FIELD` | mutation of a const field |
| `E0379` | `XR_ERR_ANALYZE_POSSIBLY_NULL` | unsafe use of a possibly-null value (trigger rules in §2.13 N-12) |
| `E0380` | `XR_ERR_ANALYZE_UNKNOWN_FIELD` | reading or setting a field / member the type does not declare |
| `E0381` | `XR_ERR_ANALYZE_MISSING_FIELD` | aggregate literal omits a required field |
| `E0382` | `XR_ERR_ANALYZE_BORROW_CONFLICT` | owner invalidated while a borrow (`Slice<T>` view, `ref`, or raw pointer) is live |
| `E0383` | `XR_ERR_ANALYZE_BORROW_ESCAPE` | a borrowed value escapes its owner's scope (return, field, container, closure capture, execution boundary) |
| `E0384` | `XR_ERR_ANALYZE_BORROW_SOURCE` | the borrow source is not a stable, uniquely inferable owner (temporary owner, multi-source return, borrowed from a local) |
| `E0385` | `XR_ERR_ANALYZE_GENERATOR_SUSPEND` | Legacy analyzer prohibition on generator scheduling; current Checked generators use the independent frame/consumer wait rules of §3.16.2 and actual admission limits of §17.39 |
| `E0386` | `XR_ERR_ANALYZE_GENERATOR_DEFER` | Legacy analyzer prohibition on generator defer; current Checked generators use the registered-cleanup and close rules of §3.16.3 instead |
| `E0387` | `XR_ERR_ANALYZE_DEFER_MAY_THROW` | an error can escape a `defer` cleanup block (see §8.3.1) |
| `E0388` | `XR_ERR_ANALYZE_MONO_BUDGET` | the program exceeds the monomorphization instance budget (breadth); each instance clones a whole declaration; see §9.4 |
| `E0389` | `XR_ERR_ANALYZE_MONO_DEPTH` | monomorphization nested past the depth budget; polymorphic recursion (`f<T>` requesting `f<Box<T>>`) has no finite specialization and always reaches it; see §9.4 |
| `E0390` | `XR_ERR_ANALYZE_UNION_INDISCRIMINABLE` | union members are not discriminable at run time (two members of the same numeric family) |
| `E0391` | `XR_ERR_ANALYZE_MOVE_NOT_UNIQUE` | the uniqueness evidence `move` requires does not hold (live alias, published root, unknown provenance, incomplete storage plan) |
| `E0392` | `XR_ERR_ANALYZE_DEFER_SUSPEND` | a defer body reaches a scheduler suspension point or creates a task; see §4.9 / §8.3.1 |
| `E0393` | `XR_ERR_ANALYZE_CLOSURE_CYCLE` | Closure and captured object form a reference cycle |
| `E0394` | `XR_ERR_ANALYZE_WEAK_FIELD` | Invalid weak field type or execution domain |
| `E0395` | `XR_ERR_ANALYZE_DEFER_CONTROL` | a defer body exits its function with `return`, or crosses the cleanup boundary with `break` / `continue` |
| `E0396` | `XR_ERR_ANALYZE_BINDING_STATE` | Read of a moved or possibly moved binding |
| `E0397` | `XR_ERR_ANALYZE_MOVE_CAPABILITY` | Move capability or constraint violation |

### 18.3 Runtime

| Code | C name | Meaning |
|--|--|--|
| `E0400` | `XR_ERR_RUNTIME` | generic runtime error |
| `E0401` | `XR_ERR_TYPE_NO_PROPERTY` | property absent on type |
| `E0402` | `XR_ERR_TYPE_NO_INDEX` | value is not indexable |
| `E0403` | `XR_ERR_TYPE_NO_CALL` | value is not callable |
| `E0404` | `XR_ERR_TYPE_MISMATCH` | runtime type mismatch |
| `E0405` | `XR_ERR_TYPE_NO_METHOD` | method absent on type |
| `E0406` | `XR_ERR_TYPE_NO_OPERATOR` | operator unsupported by type |

#### Null, Arithmetic, and Containers

| Code | C name | Meaning |
|--|--|--|
| `E0410` | `XR_ERR_NULL_PROPERTY` | property access on null |
| `E0411` | `XR_ERR_NULL_INDEX` | index on null |
| `E0412` | `XR_ERR_NULL_CALL` | call on null |
| `E0413` | `XR_ERR_NULL_UNWRAP` | force-unwrapping null |
| `E0420` | `XR_ERR_DIV_BY_ZERO` | division by zero |
| `E0421` | `XR_ERR_MOD_BY_ZERO` | modulo by zero |
| `E0422` | `XR_ERR_OVERFLOW` | arithmetic overflow or out-of-range numeric conversion (including NaN / infinity to integer) |
| `E0430` | `XR_ERR_INDEX_OUT_OF_BOUNDS` | index out of bounds |
| `E0431` | `XR_ERR_KEY_NOT_FOUND` | missing Map key |
| `E0432` | `XR_ERR_ITERATOR_EXHAUSTED` | `next()` / `nth()` on an exhausted `Iterator<T>`, violating the two-step pull protocol; see §5.3.6 |
| `E0433` | `XR_ERR_STRING_SLICE_RANGE` | invalid half-open rune range in `string.slice`; message `string slice range is invalid` |

#### System, Calls, Coroutines, and Stdlib

| Code | C name | Meaning |
|--|--|--|
| `E0440` | `XR_ERR_STACK_OVERFLOW` | stack overflow |
| `E0441` | `XR_ERR_OUT_OF_MEMORY` | out of memory |
| `E0442` | `XR_ERR_MATCH_FAILURE` | runtime match failure |
| `E0444` | `XR_ERR_DEFER_ASYNC` | defer cleanup attempted to create a task or reach the scheduler; fails closed before any such side effect |
| `E0445` | `XR_ERR_ASSERTION_FAILED` | Assertion-failure panic |
| `E0450` | `XR_ERR_WRONG_ARG_COUNT` | runtime argument-count mismatch |
| `E0451` | `XR_ERR_INVALID_ARG_TYPE` | runtime argument-type mismatch |
| `E0452` | `XR_ERR_INVALID_ARG_VALUE` | Reserved invalid argument value code; currently no emitter |
| `E0460` | `XR_ERR_CORO_DEAD` | operation on a dead coroutine |
| `E0461` | `XR_ERR_CORO_CANCELLED` | coroutine cancelled |
| `E0462` | `XR_ERR_TASK_ALREADY_TAKEN` | a Task's transferable result has already been taken and cannot be taken again |
| `E0470` | `XR_ERR_JSON_PARSE` | JSON parse failure |
| `E0471` | `XR_ERR_JSON_INVALID` | invalid JSON value or operation |
| `E0475` | `XR_ERR_REGEX_COMPILE` | regex compilation failure |
| `E0476` | `XR_ERR_REGEX_PATTERN` | invalid regex pattern |
| `E0480` | `XR_ERR_TLS_UNAVAILABLE` | TLS capability unavailable |

### 18.4 Modules, I/O, and Coroutines

| Code | C name | Meaning |
|--|--|--|
| `E0501` | `XR_ERR_MOD_NOT_FOUND` | module not found |
| `E0502` | `XR_ERR_MOD_LOAD_FAILED` | module load failed |
| `E0503` | `XR_ERR_MOD_NO_EXPORT` | name is not exported |
| `E0504` | `XR_ERR_MOD_CIRCULAR` | circular module dependency |
| `E0601` | `XR_ERR_IO_FILE_NOT_FOUND` | file not found |
| `E0602` | `XR_ERR_IO_READ_FAILED` | read failed |
| `E0603` | `XR_ERR_IO_WRITE_FAILED` | write failed |
| `E0604` | `XR_ERR_IO_PERMISSION_DENIED` | I/O permission denied |
| `E0701` | `XR_ERR_CORO_DEADLOCK` | coroutine deadlock |
| `E0702` | `XR_ERR_CORO_CHANNEL_CLOSED` | channel closed |
| `E0703` | `XR_ERR_CORO_LIMIT_EXCEEDED` | coroutine limit exceeded |

### 18.5 Syntax Guidance and Internal Errors

| Code | C name | Rejected form / meaning |
|--|--|--|
| `E0801` | `XR_ERR_SYN_RETURN_MULTI_REMOVED` | `return a, b` is invalid; return a tuple with `return (a, b)` |
| `E0802` | `XR_ERR_SYN_BINDING_CAPABILITY_REMOVED` | `owned` / `shared` binding syntax is invalid; use `var` or `const` — ownership and sharing are inferred from value capability and explicit copy/move boundaries |
| `E0803` | `XR_ERR_SYN_FOR_FLAT_REMOVED` | bare key/value `for` form is invalid |
| `E0804` | `XR_ERR_SYN_VOID_REMOVED` | `-> void` is invalid; use `-> ()` or omit the return type |
| `E0805` | `XR_ERR_SYN_PARAM_MODE_PREFIX_REMOVED` | parameter modes belong between the colon and the type |
| `E0806` | `XR_ERR_SYN_PARAM_MOVE_MODE_REMOVED` | `move` is an argument transfer expression, not a parameter mode |
| `E0807` | `XR_ERR_SYN_PARAM_MODE_COMBINED_REMOVED` | invalid combined parameter modes |
| `E0808` | `XR_ERR_SYN_PARAM_MODE_POSTFIX_REMOVED` | parameter modes cannot follow the type |
| `E0809` | `XR_ERR_SYN_CALL_IN_MARKER_REMOVED` | call-site `in` marker; ordinary `in` calls have no marker |
| `E0900` | `XR_ERR_INTERNAL` | internal error |
| `E0901` | `XR_ERR_NOT_IMPLEMENTED` | not implemented |
| `E0999` | `XR_ERR_UNKNOWN` | unknown error |

The runtime panic channel uses the prelude `PanicInfo`; user-level `throw <enum>` uses the value-return error channel. See §8 and §16 for propagation semantics; the numeric range alone does not determine the channel.
<!-- /xr-spec:en -->

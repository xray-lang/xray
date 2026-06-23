---
id: spec.18_error_codes
order: 019
---

<!-- xr-spec:cn -->
---

## 18. 错误码参考 (Error Code Reference)

> 真值源：`src/runtime/xerror_codes.h`、`src/runtime/xerror.h`。

> xray 有**两套错误码系统**：
>
> - 数值码（`xerror_codes.h` 中的 `#define`）：lexer / parser / VM 运行时使用，按区间分布。
> - 枚举码（`xerror.h` 中的 `XrErrorCode` 枚举）：分析器（type/binding/closure）使用，按区间分布。
>
> 下表列出**主要**错误码；详细的全列表与触发条件以源码为准。错误抛出时携带的 `error.name` 字段与下表"名称"列对应。

### 错误码分类（数值码）

| 范围 | 类别 |
|--|--|
| `E0101`-`E0199` | 词法错误 (Lexer) |
| `E0201`-`E0299` | 语法错误 (Syntax) |
| `E0301`-`E0399` | 编译错误 (Compile) |
| `E0401`-`E0499` | 运行时错误 (Runtime) |
| `E0501`-`E0599` | 模块错误 (Module) |
| `E0801`-`E0899` | 禁止写法 (Rejected Syntax) |

### 18.1 词法错误

| 码 | 名称 | 描述 |
|--|--|--|
| `E0101` | `XR_ERR_LEX_INVALID_CHAR` | 非法字符 |
| `E0102` | `XR_ERR_LEX_UNTERMINATED_STR` | 字符串未闭合 |
| `E0103` | `XR_ERR_LEX_INVALID_NUMBER` | 数字字面量格式错误 |
| `E0104` | `XR_ERR_LEX_INVALID_ESCAPE` | 非法转义序列 |

### 18.2 语法错误 (Syntax)

| 码 | 名称 | 描述 |
|--|--|--|
| `E0201` | `XR_ERR_SYN_UNEXPECTED_TOKEN` | 未预期的 token |
| `E0202` | `XR_ERR_SYN_EXPECTED_EXPR` | 缺少表达式 |
| `E0203` | `XR_ERR_SYN_EXPECTED_STMT` | 缺少语句 |
| `E0204` | `XR_ERR_SYN_UNCLOSED_PAREN` | 未闭合 `(` |
| `E0205` | `XR_ERR_SYN_UNCLOSED_BRACE` | 未闭合 `{` |
| `E0206` | `XR_ERR_SYN_UNCLOSED_BRACKET` | 未闭合 `[` |
| `E0207` | `XR_ERR_SYN_INVALID_ASSIGN` | 非法赋值目标（如赋值给字面量） |

### 18.3 编译期 / 名字解析错误

数值码（基础）：

| 码 | 名称 | 描述 |
|--|--|--|
| `E0301` | `XR_ERR_CMP_UNDEFINED_VAR` | 未定义名字 |
| `E0302` | `XR_ERR_CMP_REDEFINED_VAR` | 重复声明 |
| `E0303` | `XR_ERR_CMP_CONST_ASSIGN` | 赋值给 `const` |
| `E0304` | `XR_ERR_CMP_INVALID_BREAK` | `break` 不在循环内 |
| `E0305` | `XR_ERR_CMP_INVALID_CONTINUE` | `continue` 不在循环内 |
| `E0306` | `XR_ERR_CMP_INVALID_RETURN` | `return` 不在函数内 |
| `E0307` | `XR_ERR_CMP_TOO_MANY_PARAMS` | 参数数量超过限制 |
| `E0308` | `XR_ERR_CMP_TOO_MANY_LOCALS` | 局部变量数量超过限制 |

分析器枚举码（`XrErrorCode`，定义在 `xerror.h` 350+ 段）：

| 枚举名 | 描述 |
|--|--|
| `XR_ERR_ANALYZE_UNDEFINED_VAR` | 未声明变量 |
| `XR_ERR_ANALYZE_TYPE_MISMATCH` | 类型不可赋值 |
| `XR_ERR_ANALYZE_CONST_ASSIGN` | 不能给 `const` 赋值 |
| `XR_ERR_ANALYZE_NOT_CALLABLE` | 值不可调用 |
| `XR_ERR_ANALYZE_WRONG_ARG_COUNT` | 参数数量不匹配 |
| `XR_ERR_ANALYZE_ARG_TYPE` | 参数类型不匹配 |
| `XR_ERR_ANALYZE_GENERIC_COUNT` | 类型参数数量错误 |
| `XR_ERR_ANALYZE_GENERIC_CONSTRAINT` | 类型实参不满足约束 |
| `XR_ERR_ANALYZE_SUPER_FIRST` | 派生类构造器首行不是 `super(...)` |
| `XR_ERR_ANALYZE_SUPER_THIS` | `super(...)` 之前访问 `this` |
| `XR_ERR_ANALYZE_SUPER_REQUIRED` | 派生类未调 `super()` |
| `XR_ERR_ANALYZE_SUPER_INVALID` | 非派生类使用 `super()` |
| `XR_ERR_ANALYZE_CLOSURE_CAPTURE` | 协程闭包捕获了不安全变量 |
| `XR_ERR_ANALYZE_AWAIT_TYPE` | `await` 操作数不是 `Task` |
| `XR_ERR_ANALYZE_MISSING_TYPE` | 变量需要类型注解或初始化器 |
| `XR_ERR_ANALYZE_ENUM_MIXED_TYPE` | enum 成员 backing type 混合 |
| `XR_ERR_ANALYZE_INTERFACE_NOT_IMPLEMENTED` | 类未实现声明的接口 |
| `XR_ERR_ANALYZE_TUPLE_FIELD_NAME` | 用非数字 key 访问 tuple |
| `XR_ERR_ANALYZE_TUPLE_FIELD_RANGE` | tuple 字段下标越界 |
| `XR_ERR_ANALYZE_OVERRIDE_MISMATCH` | `override` 未匹配父类链中的同名同签实例方法 |

### 18.4 运行时错误 (Runtime)

#### 类型与方法 (E040x-E041x)

| 码 | 名称 | 描述 |
|--|--|--|
| `E0401` | `XR_ERR_TYPE_NO_PROPERTY` | 类型上不存在该属性 |
| `E0402` | `XR_ERR_TYPE_NO_INDEX` | 类型不可索引 |
| `E0403` | `XR_ERR_TYPE_NO_CALL` | 值不可调用 |
| `E0404` | `XR_ERR_TYPE_MISMATCH` | 类型不匹配 |
| `E0405` | `XR_ERR_TYPE_NO_METHOD` | 类型上不存在该方法 |
| `E0406` | `XR_ERR_TYPE_NO_OPERATOR` | 类型不支持该运算符 |

#### Null 相关 (E041x)

| 码 | 名称 | 描述 |
|--|--|--|
| `E0410` | `XR_ERR_NULL_PROPERTY` | 对 null 访问属性 |
| `E0411` | `XR_ERR_NULL_INDEX` | 对 null 索引 |
| `E0412` | `XR_ERR_NULL_CALL` | 对 null 调用 |

#### 算术 (E042x)

| 码 | 名称 | 描述 |
|--|--|--|
| `E0420` | `XR_ERR_DIV_BY_ZERO` | 除零（整数或浮点） |
| `E0421` | `XR_ERR_MOD_BY_ZERO` | 整数求模零 |
| `E0422` | `XR_ERR_OVERFLOW` | 整数溢出 |

#### 索引/键 (E043x)

| 码 | 名称 | 描述 |
|--|--|--|
| `E0430` | `XR_ERR_INDEX_OUT_OF_BOUNDS` | 数组 / 字符串 / Bytes 越界 |
| `E0431` | `XR_ERR_KEY_NOT_FOUND` | Map 键不存在 |

#### 内存与栈 (E044x)

| 码 | 名称 | 描述 |
|--|--|--|
| `E0440` | `XR_ERR_STACK_OVERFLOW` | 栈溢出 |
| `E0441` | `XR_ERR_OUT_OF_MEMORY` | 内存不足 |

#### 调用参数 (E045x)

| 码 | 名称 | 描述 |
|--|--|--|
| `E0450` | `XR_ERR_WRONG_ARG_COUNT` | 实参数量不匹配 |
| `E0451` | `XR_ERR_INVALID_ARG_TYPE` | 实参类型不匹配 |

#### 协程 (E046x)

| 码 | 名称 | 描述 |
|--|--|--|
| `E0460` | `XR_ERR_CORO_DEAD` | 在已死的协程上操作 |
| `E0461` | `XR_ERR_CORO_CANCELLED` | 协程被取消 |

### 18.5 模块错误 (Module)

| 码 | 名称 | 描述 |
|--|--|--|
| `E0501` | `XR_ERR_MOD_NOT_FOUND` | 找不到模块 |
| `E0502` | `XR_ERR_MOD_LOAD_FAILED` | 模块加载失败（IO / 解析错误） |
| `E0503` | `XR_ERR_MOD_NO_EXPORT` | import 的名字未被 export |
| `E0504` | `XR_ERR_MOD_CIRCULAR` | 模块依赖图包含循环依赖 |

### 18.6 禁止写法 (Rejected Syntax)

> parser 遇到下列写法时直接报错，并给出正确替代方案。

| 码 | 名称 | 禁止写法 | 正确写法 |
|--|--|--|--|
| `E0801` | `XR_ERR_SYN_RETURN_MULTI_REMOVED` | `return a, b` | `return (a, b)` |
| `E0802` | `XR_ERR_SYN_LET_MULTI_REMOVED` | `let x, y = ...` | `let (x, y) = ...` |
| `E0803` | `XR_ERR_SYN_FOR_FLAT_REMOVED` | `for k, v in m`（裸 KV） | `for (k, v in m)` |
| `E0804` | `XR_ERR_SYN_VOID_REMOVED` | `-> void` | `-> ()` 或省略返回类型 |

### 18.7 错误处理 (E082x)

| 码 | 名称 | 描述 |
|--|--|--|
| `E0820` | `XR_ERR_THROW_NOT_EXCEPTION` | 历史名称保留以免重复分配；当前 `throw` 非 enum 错误统一由 `E0370` 表达（见 §8.1.1） |
| `E0821` | `XR_ERR_TRY_BANG_BAD_OPERAND` | 已废弃（`try!` 已移除）；代码仅保留以免重复分配 |
| `E0822` | `XR_ERR_TRY_BANG_NON_EXCEPTION_ERR` | 已废弃（`try!` 已移除）；代码仅保留以免重复分配 |
| `E0823` | `XR_ERR_MATCH_NOT_EXHAUSTIVE` | 已合并到 `E0371`（见 §6.3.3）；代码仅保留以免重复分配 |
| `E0824` | `XR_ERR_UNWRAP_NON_EXCEPTION_ERR` | 已废弃（`Result` 已移除）；代码仅保留以免重复分配 |

### 18.8 Panic 错误对象结构

panic 通道的运行时故障使用 prelude `Exception` 类（声明：`stdlib/types/exception.xr`）：

```xray
@native
class Exception {
    message: string             // 人类可读消息，含错误码与上下文
    stack: Array<string>        // 自动 capture 的调用栈，每帧一行格式化字符串
    cause: Exception?           // 链式 cause
    code: int                   // 错误码（从 "E0xxx: ..." 前缀自动解析，默认 0）
    data: Json?                 // 运行时故障的可选结构化附加数据

    constructor(message: string = "", cause: Exception? = null)
    fn toString() -> string
}
```

用户级 `throw` 操作数**必须**是 enum 变体值（见 §8.1.1 / `E0370`）。结构化业务错误使用 ADT enum，而不是继承 `Exception`：

```xray
enum HttpErr {
    NotFound(string),
    ServerError(int, string),
    Timeout,
}

throw HttpErr.ServerError(500, "upstream failed")
```

`Exception` 只表示 panic 通道的运行时故障；业务错误通过 `throw <enum>` / `catch` 的值返回通道传播（见 §8.1）。
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 18. Error Code Reference

> Source of truth: `src/runtime/xerror_codes.h`, `src/runtime/xerror.h`.

> Xray has **two error-code systems**:
>
> - Numeric codes (`#define`s in `xerror_codes.h`): used by lexer / parser / VM runtime, allocated in ranges.
> - Enum codes (the `XrErrorCode` enum in `xerror.h`): used by the analyzer (type / binding / closure), allocated in ranges.
>
> The tables below cover the **principal** error codes; the full list and triggering conditions are governed by the source. The `error.name` field on a thrown error matches the "Name" column.

### Error-code categories (numeric)

| Range | Category |
|--|--|
| `E0101`-`E0199` | Lexical errors |
| `E0201`-`E0299` | Syntax errors |
| `E0301`-`E0399` | Compile errors |
| `E0401`-`E0499` | Runtime errors |
| `E0501`-`E0599` | Module errors |
| `E0801`-`E0899` | Rejected syntax |

### 18.1 Lexical Errors

| Code | Name | Description |
|--|--|--|
| `E0101` | `XR_ERR_LEX_INVALID_CHAR` | invalid character |
| `E0102` | `XR_ERR_LEX_UNTERMINATED_STR` | unterminated string |
| `E0103` | `XR_ERR_LEX_INVALID_NUMBER` | malformed numeric literal |
| `E0104` | `XR_ERR_LEX_INVALID_ESCAPE` | invalid escape sequence |

### 18.2 Syntax Errors

| Code | Name | Description |
|--|--|--|
| `E0201` | `XR_ERR_SYN_UNEXPECTED_TOKEN` | unexpected token |
| `E0202` | `XR_ERR_SYN_EXPECTED_EXPR` | expected expression |
| `E0203` | `XR_ERR_SYN_EXPECTED_STMT` | expected statement |
| `E0204` | `XR_ERR_SYN_UNCLOSED_PAREN` | unclosed `(` |
| `E0205` | `XR_ERR_SYN_UNCLOSED_BRACE` | unclosed `{` |
| `E0206` | `XR_ERR_SYN_UNCLOSED_BRACKET` | unclosed `[` |
| `E0207` | `XR_ERR_SYN_INVALID_ASSIGN` | illegal assignment target (e.g., assigning to a literal) |

### 18.3 Compile-time / Name-resolution Errors

Numeric codes (basic):

| Code | Name | Description |
|--|--|--|
| `E0301` | `XR_ERR_CMP_UNDEFINED_VAR` | undefined name |
| `E0302` | `XR_ERR_CMP_REDEFINED_VAR` | redeclaration |
| `E0303` | `XR_ERR_CMP_CONST_ASSIGN` | assignment to `const` |
| `E0304` | `XR_ERR_CMP_INVALID_BREAK` | `break` outside a loop |
| `E0305` | `XR_ERR_CMP_INVALID_CONTINUE` | `continue` outside a loop |
| `E0306` | `XR_ERR_CMP_INVALID_RETURN` | `return` outside a function |
| `E0307` | `XR_ERR_CMP_TOO_MANY_PARAMS` | parameter count exceeds limit |
| `E0308` | `XR_ERR_CMP_TOO_MANY_LOCALS` | local-variable count exceeds limit |

Analyzer enum codes (`XrErrorCode`, defined in the 350+ section of `xerror.h`):

| Enum | Description |
|--|--|
| `XR_ERR_ANALYZE_UNDEFINED_VAR` | undeclared variable |
| `XR_ERR_ANALYZE_TYPE_MISMATCH` | type not assignable |
| `XR_ERR_ANALYZE_CONST_ASSIGN` | cannot assign to `const` |
| `XR_ERR_ANALYZE_NOT_CALLABLE` | value is not callable |
| `XR_ERR_ANALYZE_WRONG_ARG_COUNT` | argument count mismatch |
| `XR_ERR_ANALYZE_ARG_TYPE` | argument type mismatch |
| `XR_ERR_ANALYZE_GENERIC_COUNT` | wrong number of type arguments |
| `XR_ERR_ANALYZE_GENERIC_CONSTRAINT` | type argument violates constraint |
| `XR_ERR_ANALYZE_SUPER_FIRST` | derived constructor's first line is not `super(...)` |
| `XR_ERR_ANALYZE_SUPER_THIS` | accessed `this` before `super(...)` |
| `XR_ERR_ANALYZE_SUPER_REQUIRED` | derived class did not call `super()` |
| `XR_ERR_ANALYZE_SUPER_INVALID` | non-derived class used `super()` |
| `XR_ERR_ANALYZE_CLOSURE_CAPTURE` | coroutine closure captured an unsafe variable |
| `XR_ERR_ANALYZE_AWAIT_TYPE` | `await` operand is not a `Task` |
| `XR_ERR_ANALYZE_MISSING_TYPE` | variable requires a type annotation or initializer |
| `XR_ERR_ANALYZE_ENUM_MIXED_TYPE` | enum members have mixed backing types |
| `XR_ERR_ANALYZE_INTERFACE_NOT_IMPLEMENTED` | class does not implement a declared interface |
| `XR_ERR_ANALYZE_TUPLE_FIELD_NAME` | tuple accessed with a non-numeric key |
| `XR_ERR_ANALYZE_TUPLE_FIELD_RANGE` | tuple field index out of range |
| `XR_ERR_ANALYZE_OVERRIDE_MISMATCH` | `override` did not match a same-name, same-signature instance method in the parent chain |

### 18.4 Runtime Errors

#### Types and methods (E040x-E041x)

| Code | Name | Description |
|--|--|--|
| `E0401` | `XR_ERR_TYPE_NO_PROPERTY` | property does not exist on the type |
| `E0402` | `XR_ERR_TYPE_NO_INDEX` | type is not indexable |
| `E0403` | `XR_ERR_TYPE_NO_CALL` | value is not callable |
| `E0404` | `XR_ERR_TYPE_MISMATCH` | type mismatch |
| `E0405` | `XR_ERR_TYPE_NO_METHOD` | method does not exist on the type |
| `E0406` | `XR_ERR_TYPE_NO_OPERATOR` | type does not support the operator |

#### Null-related (E041x)

| Code | Name | Description |
|--|--|--|
| `E0410` | `XR_ERR_NULL_PROPERTY` | property access on null |
| `E0411` | `XR_ERR_NULL_INDEX` | indexing into null |
| `E0412` | `XR_ERR_NULL_CALL` | call on null |

#### Arithmetic (E042x)

| Code | Name | Description |
|--|--|--|
| `E0420` | `XR_ERR_DIV_BY_ZERO` | integer division by zero |
| `E0421` | `XR_ERR_MOD_BY_ZERO` | integer modulo by zero |
| `E0422` | `XR_ERR_OVERFLOW` | integer overflow |

#### Indexing/keys (E043x)

| Code | Name | Description |
|--|--|--|
| `E0430` | `XR_ERR_INDEX_OUT_OF_BOUNDS` | array / string / Bytes out of bounds |
| `E0431` | `XR_ERR_KEY_NOT_FOUND` | Map key not found |

#### Memory and stack (E044x)

| Code | Name | Description |
|--|--|--|
| `E0440` | `XR_ERR_STACK_OVERFLOW` | stack overflow |
| `E0441` | `XR_ERR_OUT_OF_MEMORY` | out of memory |

#### Call arguments (E045x)

| Code | Name | Description |
|--|--|--|
| `E0450` | `XR_ERR_WRONG_ARG_COUNT` | actual argument count mismatch |
| `E0451` | `XR_ERR_INVALID_ARG_TYPE` | actual argument type mismatch |

#### Coroutines (E046x)

| Code | Name | Description |
|--|--|--|
| `E0460` | `XR_ERR_CORO_DEAD` | operation on a dead coroutine |
| `E0461` | `XR_ERR_CORO_CANCELLED` | coroutine was cancelled |

### 18.5 Module Errors

| Code | Name | Description |
|--|--|--|
| `E0501` | `XR_ERR_MOD_NOT_FOUND` | module not found |
| `E0502` | `XR_ERR_MOD_LOAD_FAILED` | module load failed (I/O / parsing error) |
| `E0503` | `XR_ERR_MOD_NO_EXPORT` | imported name is not exported |
| `E0504` | `XR_ERR_MOD_CIRCULAR` | module dependency graph contains a circular dependency |

### 18.6 Rejected Syntax

> The parser rejects the following forms outright and reports the correct replacement.

| Code | Name | Rejected form | Correct form |
|--|--|--|--|
| `E0801` | `XR_ERR_SYN_RETURN_MULTI_REMOVED` | `return a, b` | `return (a, b)` |
| `E0802` | `XR_ERR_SYN_LET_MULTI_REMOVED` | `let x, y = ...` | `let (x, y) = ...` |
| `E0803` | `XR_ERR_SYN_FOR_FLAT_REMOVED` | `for k, v in m` (bare KV) | `for (k, v in m)` |
| `E0804` | `XR_ERR_SYN_VOID_REMOVED` | `-> void` | `-> ()` or omit the return type |

### 18.7 Error Handling (E082x)

| Code | Name | Description |
|--|--|--|
| `E0820` | `XR_ERR_THROW_NOT_EXCEPTION` | historical name preserved to avoid reuse; non-enum `throw` operands are now reported as `E0370` (see §8.1.1) |
| `E0821` | `XR_ERR_TRY_BANG_BAD_OPERAND` | deprecated (`try!` removed); code preserved to avoid reuse |
| `E0822` | `XR_ERR_TRY_BANG_NON_EXCEPTION_ERR` | deprecated (`try!` removed); code preserved to avoid reuse |
| `E0823` | `XR_ERR_MATCH_NOT_EXHAUSTIVE` | merged into `E0371` (see §6.3.3); code preserved to avoid reuse |
| `E0824` | `XR_ERR_UNWRAP_NON_EXCEPTION_ERR` | deprecated (`Result` removed); code preserved to avoid reuse |

### 18.8 Panic Error-Object Layout

Runtime faults in the panic channel use the prelude `Exception` class (declared in `stdlib/types/exception.xr`):

```xray
@native
class Exception {
    message: string             // human-readable message including error code and context
    stack: Array<string>        // auto-captured call stack, one formatted line per frame
    cause: Exception?           // chained cause
    code: int                   // error code (auto-parsed from "E0xxx: ..." prefix; default 0)
    data: Json?                 // optional structured data for a runtime fault

    constructor(message: string = "", cause: Exception? = null)
    fn toString() -> string
}
```

The static type of a user-level `throw` operand **must** be an enum variant value (see §8.1.1 / `E0370`). Structured business errors use ADT enums rather than `Exception` inheritance:

```xray
enum HttpErr {
    NotFound(string),
    ServerError(int, string),
    Timeout,
}

throw HttpErr.ServerError(500, "upstream failed")
```

`Exception` represents panic-channel runtime faults only; business errors propagate through the `throw <enum>` / `catch` value-return channel (see §8.1).
<!-- /xr-spec:en -->

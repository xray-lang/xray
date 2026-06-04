---
id: spec.12_testing
order: 013
---

<!-- xr-spec:cn -->
---

## 12. 测试系统 (Testing)

> 真值源：`src/app/cli/xcli_test.c`、`stdlib/xray/test.xr`、`docs/testing-spec.md`。

### 12.1 测试声明：`@test` 注解

xray 用 **`@test` 注解**标注测试函数，**不**通过 `test("...")` 函数调用形式。

```ebnf
TestDecl ::= '@test' FnDecl
```

```xray @id=testing-basic
@test
fn test_addition() {
    assert_eq(1 + 1, 2)
}

@test
fn test_with_assertions() {
    let result = compute()
    assert_eq(result, 42)
    assert(result > 0)
}
```

**语义**：
- `@test` 标注的函数会被 `xray test` 自动发现并运行；普通函数不会。
- 测试函数命名约定：`test_xxx`（snake_case），描述性命名。
- 测试函数无参数无返回值；通过 assert 系列函数表达预期。
- 同一文件可包含**任意数量**的 `@test` 函数；它们按声明顺序运行。

### 12.2 测试入口

测试文件约定：
- 与被测代码同目录或 `tests/regression/` 目录下。
- 文件名形如 `XXXX_topic.xr`（四位数字编号 + 主题）。

运行：

```bash
xray test                                  # 运行所有测试
xray test tests/regression/01_literals/    # 整个分组
xray test tests/regression/01_literals/0100_int_basic.xr   # 单文件
```

### 12.3 断言 API

xray 把断言函数作为**全局内置**（不需 `import test`）。完整签名见 [§13.5](#135-断言测试用)。

| 函数 | 语义 |
|--|--|
| `assert(cond, msg?)` | `cond` 为 false 时抛异常 |
| `assert_eq(a, b)` | `a == b` 失败时输出两值 |
| `assert_ne(a, b)` | `a != b` |
| `assert_true(cond)` / `assert_false(cond)` | 等价 `assert(cond)` / `assert(!cond)` |
| `assert_throws(fn)` | 期望 `fn()` 抛异常 |

> **命名一致性**：所有断言函数为 `snake_case`（`assert_eq`，不是 `assertEq`）。

### 12.4 异步测试

`@test` 函数体内可使用 `go` / `await` / `await all` / `await any`：

```xray @id=testing-async
@test
fn test_async_fetch() {
    let task = go fetch_data("http://...")
    let result = await task
    assert_eq(result.status, 200)
}
```

### 12.5 注解（Attributes）总览

xray 的注解前缀为 `@`，紧接标识符。当前 parser 仅识别**三种**注解（源码：`xparse_decl.c:xr_parse_single_attribute`）：

| 注解 | 适用 | 说明 |
|---|---|---|
| `@test` | 函数 | 标记为测试函数；接受可选参数：`@test(skip)` 跳过、`@test(timeout: 30)` 超时设置 |
| `@native` | class / struct / fn | 声明 native 实现，方法体由 C 提供；用于 stdlib 类型声明 |
| `@deprecated` | 任意声明 | 弃用警告；可选消息：`@deprecated("use X instead")` |

```xray @id=testing-attributes
@test                                 // 标记测试
fn test_basic() { return }

@test(skip)                           // 跳过此测试
fn test_wip() { return }

@native                               // C 实现
class Array<T> {
    length: int
    push(v: T)
    // 无方法体——由 src/runtime/object/xarray_methods.c 提供
}

@deprecated("use newAPI() instead")
fn oldAPI() { return }
```

> 不存在的注解（用户代码不要使用）：`@before_each` / `@after_all` / `@async` / `@override` 等——这些会触发"unknown attribute name"错误。

### 12.6 `xray run` / `xray test` / `xray repl`

| 命令 | 用途 |
|--|--|
| `xray run main.xr` | 执行主程序 |
| `xray test` | 运行测试套件 |
| `xray repl` | 启动 REPL |
| `xray build --aot` | AOT 编译 |
| `xray fmt` | 格式化 |
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 12. Testing

> Source of truth: `src/app/cli/xcli_test.c`, `stdlib/xray/test.xr`, `docs/testing-spec.md`.

### 12.1 Declaring Tests: the `@test` Attribute

Xray marks test functions with the **`@test` attribute**, **not** with a `test("...")` function call.

```ebnf
TestDecl ::= '@test' FnDecl
```

```xray @id=testing-basic
@test
fn test_addition() {
    assert_eq(1 + 1, 2)
}

@test
fn test_with_assertions() {
    let result = compute()
    assert_eq(result, 42)
    assert(result > 0)
}
```

**Semantics**:
- Functions annotated with `@test` are auto-discovered and run by `xray test`; ordinary functions are not.
- Test naming convention: `test_xxx` (snake_case), descriptive.
- Test functions take no parameters and return nothing; expectations are expressed via the `assert*` family.
- A file may contain **any number** of `@test` functions; they run in declaration order.

### 12.2 Test Entry Points

Test file convention:
- Either co-located with the code under test or under `tests/regression/`.
- File name format: `XXXX_topic.xr` (four-digit number + topic).

Run:

```bash
xray test                                  # run all tests
xray test tests/regression/01_literals/    # run a whole group
xray test tests/regression/01_literals/0100_int_basic.xr   # single file
```

### 12.3 Assertion API

Xray provides assertion functions as **global builtins** (no `import test` needed). Full signatures are in [§13.5](#135-assertions-for-testing).

| Function | Semantics |
|--|--|
| `assert(cond, msg?)` | throws when `cond` is false |
| `assert_eq(a, b)` | prints both values when `a == b` fails |
| `assert_ne(a, b)` | `a != b` |
| `assert_true(cond)` / `assert_false(cond)` | equivalent to `assert(cond)` / `assert(!cond)` |
| `assert_throws(fn)` | expects `fn()` to throw |

> **Naming consistency**: all assertion functions are `snake_case` (`assert_eq`, not `assertEq`).

### 12.4 Async Tests

A `@test` function body may use `go` / `await` / `await all` / `await any`:

```xray @id=testing-async
@test
fn test_async_fetch() {
    let task = go fetch_data("http://...")
    let result = await task
    assert_eq(result.status, 200)
}
```

### 12.5 Attribute Overview

Xray attributes are prefixed with `@` followed by an identifier. The current parser only recognizes **three** attributes (source: `xparse_decl.c:xr_parse_single_attribute`):

| Attribute | Applies to | Description |
|---|---|---|
| `@test` | function | mark as a test function; accepts optional arguments: `@test(skip)` to skip, `@test(timeout: 30)` to set a timeout |
| `@native` | class / struct / fn | declare a native implementation; method bodies are provided by C (used in stdlib type declarations) |
| `@deprecated` | any declaration | deprecation warning; optional message: `@deprecated("use X instead")` |

```xray @id=testing-attributes
@test                                 // mark as a test
fn test_basic() { return }

@test(skip)                           // skip this test
fn test_wip() { return }

@native                               // C implementation
class Array<T> {
    length: int
    push(v: T)
    // no method bodies — provided by src/runtime/object/xarray_methods.c
}

@deprecated("use newAPI() instead")
fn oldAPI() { return }
```

> Attributes that do not exist (do not use them in user code): `@before_each` / `@after_all` / `@async` / `@override` etc. — these trigger an "unknown attribute name" error.

### 12.6 `xray run` / `xray test` / `xray repl`

| Command | Purpose |
|--|--|
| `xray run main.xr` | run the main program |
| `xray test` | run the test suite |
| `xray repl` | start the REPL |
| `xray build --aot` | AOT compile |
| `xray fmt` | format code |
<!-- /xr-spec:en -->

---
id: spec.12_testing
order: 013
---

<!-- xr-spec:cn -->
---

## 12. 测试系统 (Testing)

> 真值源：`src/app/cli/xcmd_test.c`、`src/api/xtest_runner.c`、`src/frontend/parser/xparse_decl.c` 与 analyzer 的全局 assertion builtin 表。

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
    var result = compute()
    assert_eq(result, 42)
    assert(result > 0)
}
```

**语义**：
- `@test` 标注的函数会被 `xray test` 自动发现并运行；普通函数不会。
- 测试函数命名约定：`test_xxx`（snake_case），描述性命名。
- 测试函数无参数无返回值；通过 assert 系列函数表达预期。
- 同一文件可包含**任意数量**的 `@test` 函数；单文件内按声明顺序运行。多个文件可用 `-j N` 并行，每个文件使用独立 isolate。

### 12.2 测试入口

`xray test` 不强制测试目录或文件名；目录输入会递归收集所有 `.xr` 文件，并按路径排序。仓库自己的 regression suite 使用 `tests/regression/XXXX_topic.xr` 只是项目约定。

运行：

```bash
xray test tests/                           # 必须显式给出至少一个文件或目录
xray test tests/regression/01_literals/    # 整个分组
xray test tests/regression/01_literals/0100_int_basic.xr   # 单文件
xray test -j 4 tests/                      # 文件级并行
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
    var task = go fetch_data("http://...")
    var result = await task
    assert_eq(result.status, 200)
}
```

### 12.5 注解（Attributes）总览

xray 的注解前缀为 `@`，真值表在 `xparse_decl.c:xr_parse_single_attribute`。测试 runner 识别以下测试注解：

| 注解 | 说明 |
|---|---|
| `@test` / `@test(skip)` / `@test(timeout: N)` | 测试、跳过测试、单测试超时秒数 |
| `@before_all` / `@after_all` | 单文件 suite 前后各执行一次 |
| `@before_each` / `@after_each` | 每个未跳过测试前后执行 |

其它注解包括：`@deprecated("...")`；内置声明专用的 `@native`；AOT/C ABI 的 `@c_export("sym")`、`@section("name")`、`@weak`、`@used`、`@naked`、`@interrupt("abi")`、`@no_alloc`；以及 `@derive(Inspect, Json, Eq, Hash, Clone)`（其中 `Hash` 要求同时 `Eq`）。外部 C 声明使用 `extern "C" ... {}` 块。

```xray @id=testing-attributes
@test                                 // 标记测试
fn test_basic() { return }

@test(skip)                           // 跳过此测试
fn test_wip() { return }

@before_each
fn reset_fixture() { resetState() }

@deprecated("use newAPI() instead")
fn oldAPI() { return }
```

> `@async`、`@override`、`@beforeEach` 等不在当前表中，会触发 `unknown attribute name`。异步能力直接在测试体内使用 `go` / `await`。

### 12.6 `xray run` / `xray test` / `xray repl`

| 命令 | 用途 |
|--|--|
| `xray run main.xr` | 执行主程序 |
| `xray test` | 运行测试套件 |
| `xray repl` | 启动 REPL |
| `xray build --native main.xr` | AOT native 编译 |
| `xray fmt` | 格式化 |
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 12. Testing

> Source of truth: `src/app/cli/xcmd_test.c`, `src/api/xtest_runner.c`, `src/frontend/parser/xparse_decl.c`, and the analyzer's global assertion-builtin table.

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
    var result = compute()
    assert_eq(result, 42)
    assert(result > 0)
}
```

**Semantics**:
- Functions annotated with `@test` are auto-discovered and run by `xray test`; ordinary functions are not.
- Test naming convention: `test_xxx` (snake_case), descriptive.
- Test functions take no parameters and return nothing; expectations are expressed via the `assert*` family.
- A file may contain **any number** of `@test` functions; they run in declaration order within that file. Multiple files may run in parallel with `-j N`, each in its own isolate.

### 12.2 Test Entry Points

`xray test` enforces no directory or filename convention. A directory argument is scanned recursively for every `.xr` file, sorted by path. This repository's `tests/regression/XXXX_topic.xr` form is only a project convention.

Run:

```bash
xray test tests/                           # at least one file or directory is required
xray test tests/regression/01_literals/    # run a whole group
xray test tests/regression/01_literals/0100_int_basic.xr   # single file
xray test -j 4 tests/                      # file-level parallelism
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
    var task = go fetch_data("http://...")
    var result = await task
    assert_eq(result.status, 200)
}
```

### 12.5 Attribute Overview

Xray attributes begin with `@`; the source table is `xparse_decl.c:xr_parse_single_attribute`. The test runner recognizes:

| Attribute | Description |
|---|---|
| `@test` / `@test(skip)` / `@test(timeout: N)` | test, skipped test, or per-test timeout in seconds |
| `@before_all` / `@after_all` | run once before/after the file's suite |
| `@before_each` / `@after_each` | run before/after every non-skipped test |

Other attributes include `@deprecated("...")`; builtin-declaration-only `@native`; AOT/C ABI attributes `@c_export("sym")`, `@section("name")`, `@weak`, `@used`, `@naked`, `@interrupt("abi")`, and `@no_alloc`; plus `@derive(Inspect, Json, Eq, Hash, Clone)` (`Hash` requires `Eq`). External C declarations use an `extern "C" ... {}` block.

```xray @id=testing-attributes
@test                                 // mark as a test
fn test_basic() { return }

@test(skip)                           // skip this test
fn test_wip() { return }

@before_each
fn reset_fixture() { resetState() }

@deprecated("use newAPI() instead")
fn oldAPI() { return }
```

> `@async`, `@override`, and camel-case spellings such as `@beforeEach` are not in the current table and trigger `unknown attribute name`. Use `go` / `await` directly in an async test body.

### 12.6 `xray run` / `xray test` / `xray repl`

| Command | Purpose |
|--|--|
| `xray run main.xr` | run the main program |
| `xray test` | run the test suite |
| `xray repl` | start the REPL |
| `xray build --native main.xr` | AOT native compile |
| `xray fmt` | format code |
<!-- /xr-spec:en -->

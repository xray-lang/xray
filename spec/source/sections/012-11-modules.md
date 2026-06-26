---
id: spec.11_modules
order: 012
---

<!-- xr-spec:cn -->
---

## 11. 模块系统 (Modules)

> 真值源：`src/module/xmodule.c`、`src/module/xmodule_resolver.c`、`src/module/xmodule_graph.c`、`src/frontend/parser/xparse_import.c`。

### 11.1 模块定义

- 每个 `.xr` 文件是一个模块。
- 模块名 = 文件名（去除 `.xr` 后缀）。
- 模块路径反映目录结构：`src/utils/string.xr` → `utils.string`。

### 11.2 项目结构

```
my_project/
├── xray.toml              # 包清单（包名、依赖、入口）
├── src/
│   ├── main.xr            # 入口
│   ├── utils.xr
│   └── lib/
│       └── helper.xr
├── tests/
│   └── test_utils.xr
└── docs/
```

`xray.toml` 示例：

```toml
[package]
name = "my_project"
version = "0.1.0"
entry = "src/main.xr"

[dependencies]
http = "1.0"
json = "0.2"

[dev-dependencies]
test = "1.0"
```

### 11.3 `import` 语法

`import` 声明**只能出现在模块顶层**。在函数、类或任何嵌套作用域内使用 `import` 会产生编译错误。

```ebnf
ImportStmt ::= 'import' ImportMembers 'from' ImportModule
            |  'import' ImportModule ('as' Identifier)?
ImportMembers ::= '{' ImportMember (',' ImportMember)* ','? '}'
ImportMember  ::= Identifier ('as' Identifier)?
ImportModule  ::= StringLiteral | ModuleName
ModuleName    ::= Identifier
```

```xray @id=modules-import
// 1. stdlib：裸标识符；没有 `as` 时别名等于模块名
import time
import datetime
import http as httpClient

// 2. 第三方包：引号 + owner/name 形式
import "alice/utils"
import "bob/http_client" as httpClient

// 3. 文件路径或目录路径：字符串字面量，可显式 alias，也可从路径尾段推导（不含 .xr 扩展名）
import "./modules/mod_a" as a
import "../utils/string_utils" as utils
import "models/user" as user

// 4. 命名 import：成员可重命名；`from` 后可接字符串路径或裸模块名
import { readFile, writeFile as write } from io
import { publicFn } from "./modules/mod_a"
```

**不支持**：
- JavaScript 默认导入 `import name from "module"`。使用 `import "module" as name`、`import module` 或 `import { name } from module`。
- 动态导入 `import("module")`。所有导入必须是静态声明。

**解析算法**（按优先级）：
1. **stdlib 命名解析**：裸标识符 `import time` → 内置 stdlib 模块表。
2. **相对路径**：`"./xxx"` 与 `"../xxx"` 相对当前文件解析（自动补 `.xr` 扩展名或 `index.xr` 目录入口）。
3. **项目根目录路径**：不以 `./` 或 `../` 开头的 quoted path 作为项目目录 import。
4. **第三方包**：`"owner/name"` 由 `xray.toml` 的 `[dependencies]` 解析。

**Specifier 校验**（编译错误）：
- 禁止包含 `.xr` 扩展名：`import "./a.xr"` → 错误
- 禁止末尾斜杠：`import "./a/"` → 错误
- 禁止显式 `index`：`import "./a/index"` → 错误
- 禁止绝对路径：`import "/etc/foo"` → 错误

### 11.4 `export` 与可见性

`export` 声明**只能出现在模块顶层**。xray 支持三种 export 形式：

```ebnf
ExportStmt ::= 'export' Declaration                              // 直接 export 声明
            |  'export' '{' Identifier (',' Identifier)* '}'     // export 已声明的标识符
            |  'export' '{' ExportSpec (',' ExportSpec)* '}' 'from' StringLiteral
            |  'export' '*' 'from' StringLiteral
ExportSpec ::= Identifier ('as' Identifier)?
Declaration ::= FnDecl | ClassDecl | StructDecl | ConstDecl | TypeAlias
```

```xray @id=modules-export
// 1. 直接 export 声明
export fn helper() { return }
export class MyClass {
    value: int
    constructor() { this.value = 1 }
}
export const VERSION = "1.0"

// 2. export 已声明的标识符（用于内部先声明、最后统一暴露）
fn _helper() -> string { return "..." }
fn publicFn() -> string { return _helper() }
export { publicFn }

// 3. 重导出（带可选重命名）
export { getUser, getUserAge as getAge } from "./user"

// 4. 通配重导出（把另一个模块的全部 export 转出）
export * from "./product"
```

**限制**：
- 未标 `export` 的声明仅模块内可见（**私有**）。
- `export let` 不被支持。可变绑定不能跨模块共享，使用 `export const` 代替。
- 模块的内部状态在不同模块中互不冲突，即使同名。
- 重导出与通配重导出常用于 `index.xr` 聚合子模块的公开 API。

### 11.5 命名约定

- 模块名 `snake_case`：`http_client.xr` / `string_utils.xr`。
- 公开符号 `camelCase` 或 `PascalCase`（类/接口）。
- 内部符号约定前缀 `_`：`_internal_helper`。

### 11.6 编译期模块图与循环依赖

xray 在编译期构建完整的**模块依赖图**（DAG）：

1. 从入口文件开始，递归解析所有 `import` 声明，构建依赖图。
2. 对依赖图进行拓扑排序，确定模块初始化顺序。
3. 如果检测到**循环依赖**（SCC 大小 > 1 或自环），产生编译错误。
4. 按拓扑序从叶子模块（无依赖）到入口模块依次初始化。

选择性导入（`import { foo } from "./m"`）在编译期解析为固定的模块索引和导出槽位，运行时为 O(1) 索引访问，不涉及字符串查找。

### 11.7 native 模块

C 层暴露的模块（如 `time`、`http`、`os`）通过 native ABI 注册：

```c
// C 端
XRAY_API void register_time_module(xray_vm_t* vm) {
    xray_module_t* m = xray_module_create(vm, "time");
    xray_module_add_fn(m, "now", time_now);
    xray_module_add_fn(m, "sleep", time_sleep);
    xray_module_register(vm, m);
}
```

xray 端用法相同：

```xray @id=modules-use
import time
let t = time.now()
time.sleep(100)
```
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 11. Modules

> Source of truth: `src/module/xmodule.c`, `src/module/xmodule_resolver.c`, `src/module/xmodule_graph.c`, `src/frontend/parser/xparse_import.c`.

### 11.1 Module Definition

- Each `.xr` file is one module.
- Module name = file name (with the `.xr` suffix removed).
- Module path mirrors directory structure: `src/utils/string.xr` → `utils.string`.

### 11.2 Project Layout

```
my_project/
├── xray.toml              # package manifest (name, dependencies, entry)
├── src/
│   ├── main.xr            # entry
│   ├── utils.xr
│   └── lib/
│       └── helper.xr
├── tests/
│   └── test_utils.xr
└── docs/
```

`xray.toml` example:

```toml
[package]
name = "my_project"
version = "0.1.0"
entry = "src/main.xr"

[dependencies]
http = "1.0"
json = "0.2"

[dev-dependencies]
test = "1.0"
```

### 11.3 `import` Syntax

`import` declarations **must appear at the module top level**. Using `import` inside a function, class, or any nested scope is a compile error.

```ebnf
ImportStmt ::= 'import' ImportMembers 'from' ImportModule
            |  'import' ImportModule ('as' Identifier)?
ImportMembers ::= '{' ImportMember (',' ImportMember)* ','? '}'
ImportMember  ::= Identifier ('as' Identifier)?
ImportModule  ::= StringLiteral | ModuleName
ModuleName    ::= Identifier
```

```xray @id=modules-import
// 1. stdlib: bare identifier; without `as`, the alias equals the module name
import time
import datetime
import http as httpClient

// 2. third-party packages: quoted owner/name form
import "alice/utils"
import "bob/http_client" as httpClient

// 3. file-path or directory-path: string literal, optional explicit alias, otherwise inferred from the trailing path segment (no .xr extension)
import "./modules/mod_a" as a
import "../utils/string_utils" as utils
import "models/user" as user

// 4. named imports: members may be renamed; the `from` operand may be a quoted path or a bare module name
import { readFile, writeFile as write } from io
import { publicFn } from "./modules/mod_a"
```

**Not supported**:
- JavaScript-style default import (`import name from "module"`). Use `import "module" as name`, `import module`, or `import { name } from module`.
- Dynamic import (`import("module")`). All imports must be static declarations.

**Resolution algorithm** (in priority order):
1. **stdlib name resolution**: a bare identifier `import time` → the built-in stdlib module table.
2. **Relative path**: `"./xxx"` and `"../xxx"` are resolved relative to the current file (auto-appends `.xr` extension or `index.xr` directory entry).
3. **Project-root path**: a quoted path that does not start with `./` or `../` is resolved as a project-relative directory import.
4. **Third-party packages**: `"owner/name"` is resolved through the `[dependencies]` section in `xray.toml`.

**Specifier validation** (compile errors):
- Including `.xr` extension: `import "./a.xr"` → error
- Trailing slash: `import "./a/"` → error
- Explicit `index`: `import "./a/index"` → error
- Absolute paths: `import "/etc/foo"` → error

### 11.4 `export` and Visibility

`export` declarations **must appear at the module top level**. Xray supports three export forms:

```ebnf
ExportStmt ::= 'export' Declaration                              // export the declaration directly
            |  'export' '{' Identifier (',' Identifier)* '}'     // export already-declared identifiers
            |  'export' '{' ExportSpec (',' ExportSpec)* '}' 'from' StringLiteral
            |  'export' '*' 'from' StringLiteral
ExportSpec ::= Identifier ('as' Identifier)?
Declaration ::= FnDecl | ClassDecl | StructDecl | ConstDecl | TypeAlias
```

```xray @id=modules-export
// 1. export a declaration directly
export fn helper() { return }
export class MyClass {
    value: int
    constructor() { this.value = 1 }
}
export const VERSION = "1.0"

// 2. export already-declared identifiers (declare internally first, expose at the end)
fn _helper() -> string { return "..." }
fn publicFn() -> string { return _helper() }
export { publicFn }

// 3. re-export (with optional renaming)
export { getUser, getUserAge as getAge } from "./user"

// 4. wildcard re-export (forward all exports of another module)
export * from "./product"
```

**Restrictions**:
- Declarations not marked `export` are **private** to the module.
- `export let` is not supported. Mutable bindings cannot be shared across modules; use `export const` instead.
- Internal state does not collide across modules even with the same name.
- Re-exports and wildcard re-exports are commonly used in `index.xr` to aggregate public APIs of submodules.

### 11.5 Naming Conventions

- Module names: `snake_case` (`http_client.xr` / `string_utils.xr`).
- Public symbols: `camelCase` or `PascalCase` (for classes / interfaces).
- Internal symbols: prefix with `_` (`_internal_helper`).

### 11.6 Compile-Time Module Graph and Circular Dependencies

Xray builds a complete **module dependency graph** (DAG) at compile time:

1. Starting from the entry file, all `import` declarations are recursively resolved to build the dependency graph.
2. The graph is topologically sorted to determine module initialization order.
3. If a **circular dependency** is detected (SCC size > 1 or self-loop), compilation fails with `E0504` and reports a concrete cycle path such as `a.xr -> b.xr -> a.xr`.
4. Modules are initialized in topological order, from leaf modules (no dependencies) to the entry module.

Selective imports (`import { foo } from "./m"`) are resolved at compile time to fixed module indices and export slots, resulting in O(1) indexed access at runtime with no string lookup.

Xray does not use partial module initialization or cache-based cycle breaking. A module that is still loading is not observable through `import`; mutual dependencies must be refactored into a DAG.

### 11.7 Native Modules

Modules exposed from the C layer (`time`, `http`, `os`, etc.) are registered through the native ABI:

```c
// C side
XRAY_API void register_time_module(xray_vm_t* vm) {
    xray_module_t* m = xray_module_create(vm, "time");
    xray_module_add_fn(m, "now", time_now);
    xray_module_add_fn(m, "sleep", time_sleep);
    xray_module_register(vm, m);
}
```

Usage from Xray code is identical:

```xray @id=modules-use
import time
let t = time.now()
time.sleep(100)
```
<!-- /xr-spec:en -->

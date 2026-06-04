---
id: spec.0_preface
order: 001
---

<!-- xr-spec:cn -->
---

## 0. 前言

### 0.1 关于本规范

本文档是 Xray 编程语言的**参考手册**（reference manual），描述语言的词法、语法、类型系统、语义、并发模型、运行时与标准库的接口。它的目标是：

1. 让人类阅读后能写出合法且行为可预测的 xray 代码；
2. 作为编译器、分析器、IDE、AI 助手、文档生成器等工具的**结构化真值源**；
3. 与 `xray` 主仓的实际实现保持一致——任何不一致都视为本文档或代码的 bug。

**本手册不是教程**。完整的入门材料见 xray 官网与 `demos/` 目录。

### 0.2 版本约定

本手册版本号与 `xray` 主仓 (`CMakeLists.txt` 的 `project(Xray VERSION x.y.z)`) 严格一致。重大破坏性改动以章节内的"自 vX.Y.Z 起"标注。

xray 当前处于 v0.x 阶段。**不承诺任何向后兼容**。规范每次发版可能引入破坏性改动。

### 0.3 语言设计哲学

Xray 是一个**轻量级静态类型脚本语言，原生支持并发**。设计目标：

| 维度 | 取舍 |
|--|--|
| **类型** | 静态类型 + 类型推断；变量声明几乎不需要写类型标注，但类型系统在编译期完全可见 |
| **并发** | 内置 M:N 协程（go / await / Channel / scope / select），并发安全在编译期由"显式共享"规则保证 |
| **运行模式** | VM 解释 / JIT / AOT 三档，对开发者透明；语义在三种模式下严格一致 |
| **错误处理** | 值返回错误通道（throw / try / catch + enum 错误）+ panic 边界（catch panic）+ 可空类型（T?）+ defer 资源管理 |
| **元编程** | 注解（`@test` / `@native` / `@deprecated`）+ 运行时反射（Reflect）+ 泛型 reified |
| **互操作** | C ABI 内置；stdlib 模块可由 C 编写并通过 `XR_DEFINE_BUILTIN` 暴露 |

设计参考来源：TypeScript（类型推断 + nullable）、Go（结构化并发 + Channel）、Rust（所有权语义的轻量版 move）、Swift（协议 + 可空链）。**Xray 不是其中任何一者的克隆**。

### 0.4 阅读约定

#### 0.4.1 语法记法

本文档使用一种轻量化的 EBNF 风格描述语法：

| 符号 | 含义 |
|--|--|
| `Term` | 非终结符（capitalised） |
| `'literal'` | 字面 token |
| `A B` | 序列 |
| `A \| B` | 选择 |
| `A?` | 可选 |
| `A*` | 零次或多次 |
| `A+` | 一次或多次 |
| `(A)` | 分组 |

完整 EBNF 见 [附录 A](#附录-a-ebnf-语法)。

#### 0.4.2 源码引用

本文档大量引用 xray 主仓源码作为真值源。引用格式：

```
路径:行号
```

例：`src/frontend/lexer/xkeywords.def`、`src/frontend/parser/xast_types.h:42-58`。

#### 0.4.3 状态标记

| 标记 | 含义 |
|--|--|
| **稳定** | 默认状态；行为不会无预警变化 |
| **实验性** | 实现存在但可能改变 |
| **保留** | 关键字/语法已识别但当前禁用 |
| **未实现** | spec 中描述但代码暂未支持，应显式标注实现状态 |

#### 0.4.4 错误码引用

错误码使用 `E0xxx` 格式（如 `E0101`），完整列表见 [第 18 章](#18-错误码参考-error-code-reference)。源码定义在 `src/runtime/xerror_codes.h` 与 `src/runtime/xerror.h`。
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 0. Preface

### 0.1 About This Specification

This document is the **reference manual** for the Xray programming language. It describes the lexical structure, syntax, type system, semantics, concurrency model, runtime, and standard-library surface. Its goals are:

1. Allow a human reader to write valid Xray code with predictable behavior.
2. Serve as a **structured source of truth** for compilers, analyzers, IDEs, AI assistants, documentation generators, and other tooling.
3. Stay aligned with the actual implementation in the main `xray` repository — any divergence is a bug in either the document or the code.

**This manual is not a tutorial.** For introductory material, see the Xray website and the `demos/` directory.

### 0.2 Versioning

The version of this manual is kept in strict lock-step with the main `xray` repository (the `project(Xray VERSION x.y.z)` line in `CMakeLists.txt`). Major breaking changes are noted in-section as "since vX.Y.Z".

Xray is currently in the `v0.x` series. **No backward compatibility is promised.** Each release of the spec may introduce breaking changes.

### 0.3 Language Design Philosophy

Xray is a **lightweight statically typed scripting language with native concurrency support**. Design goals:

| Dimension | Choice |
|--|--|
| **Types** | Static typing + type inference; declarations rarely require explicit type annotations, but the type system is fully visible at compile time |
| **Concurrency** | Built-in M:N coroutines (go / await / Channel / scope / select); concurrency safety is enforced at compile time by the "explicit sharing" rules |
| **Execution** | VM interpreter / JIT / AOT — all transparent to the developer; semantics are strictly identical across modes |
| **Error handling** | Value-return error channel (throw / try / catch + enum errors) + panic boundary (catch panic) + nullable types (T?) + `defer`-based resource management |
| **Metaprogramming** | Attributes (`@test` / `@native` / `@deprecated`) + runtime reflection (Reflect) + reified generics |
| **Interop** | C ABI is built-in; stdlib modules can be authored in C and exposed via `XR_DEFINE_BUILTIN` |

Design influences: TypeScript (type inference + nullable), Go (structured concurrency + Channel), Rust (a lightweight take on ownership/`move`), Swift (protocols + optional chaining). **Xray is not a clone of any of them.**

### 0.4 Reading Conventions

#### 0.4.1 Grammar Notation

This document uses a lightweight EBNF-style notation:

| Symbol | Meaning |
|--|--|
| `Term` | non-terminal (capitalised) |
| `'literal'` | literal token |
| `A B` | sequence |
| `A \| B` | choice |
| `A?` | optional |
| `A*` | zero or more |
| `A+` | one or more |
| `(A)` | grouping |

The complete EBNF is in [Appendix A](#appendix-a-ebnf).

#### 0.4.2 Source-Code References

This document references the main `xray` repository extensively as its source of truth. Citation format:

```
path:lineno
```

Examples: `src/frontend/lexer/xkeywords.def`, `src/frontend/parser/xast_types.h:42-58`.

#### 0.4.3 Status Markers

| Marker | Meaning |
|--|--|
| **Stable** | default state; behavior will not change without notice |
| **Experimental** | implementation exists but may change |
| **Reserved** | keyword/syntax recognized but currently disabled |
| **Unimplemented** | described in the spec but not yet supported in code; must be marked explicitly |

#### 0.4.4 Error-Code References

Error codes use the `E0xxx` format (e.g., `E0101`); the full list is in [Chapter 18](#18-error-code-reference). Source-level definitions are in `src/runtime/xerror_codes.h` and `src/runtime/xerror.h`.
<!-- /xr-spec:en -->

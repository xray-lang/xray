---
id: spec.17_compilation_pipeline
order: 018
---

<!-- xr-spec:cn -->
---

## 17. 编译流水线 (Compilation Pipeline)

> 真值源：`src/frontend/`、`src/vm/`、`src/jit/`、`src/aot/`、`docs/rules/architecture.md`。

### 17.1 阶段总览

```
源码 (.xr)
    ↓ lexer
Token Stream
    ↓ parser
AST
    ↓ analyzer (语义分析、类型检查、scope/capture/generic)
Typed AST
    ↓ ssa-gen
SSA IR
    ↓ optimize（const fold、DCE、inline、TCO、escape analysis）
Optimized SSA
    ↓ codegen
Bytecode  →  AOT (machine code)
    ↓ VM
    ↓ Profiler → JIT (machine code)
执行
```

### 17.2 词法分析 (Lexer)

- 真值源：`src/frontend/lexer/xlexer.c`。
- 输出 `XrToken` 流，每个 token 含 `kind`、`value`、`pos(line, col)`。
- 处理：字符串插值（产生 `${...}` 拼接序列）、原始字符串、正则字面量。

### 17.3 语法分析 (Parser)

- 真值源：`src/frontend/parser/`（分文件：expr、stmt、decl、match）。
- 风格：手写 Pratt parser（表达式）+ 递归下降（声明 / 语句）。
- 错误恢复：遇到错误后跳到下一同步点（`;` `}` `)`），尽量继续解析。
- 输出：`XrAstNode*` 根（即 module）。

### 17.4 语义分析 (Analyzer)

- 真值源：`src/frontend/analyzer/xanalyzer_*.c`（按主题拆分）。
- **作用域**：嵌套符号表、变量解析、shadowing 检查。
- **类型检查**：双向类型推断、union 收窄、Json 结构匹配。
- **泛型**：monomorphization、约束检查、调用点重写。
- **闭包分析**：upvalue 标记、`go` 闭包捕获禁令。
- **错误码**：`XR_ERR_ANALYZE_*` 系列。

### 17.5 SSA 与优化

- 真值源：`src/ir/xi_opt*.c`、`src/ir/xi_pass.h`、`src/jit/`。
- **常量折叠**：编译期求值。
- **DCE**（dead-code elimination）：删除未使用代码。
- **inlining**：小函数内联。
- **TCO**（tail-call optimization）：accumulator 风格尾递归转循环。
- **escape analysis**：栈分配 vs 堆分配决策。

### 17.6 字节码与 VM

- 真值源：`src/vm/`、`include/xray_opcodes.h`。
- 寄存器栈混合 VM。
- IC（inline cache）加速属性访问与方法分派。

### 17.7 JIT 与 AOT

- **JIT**（运行时）：热函数被 profiler 选中后 → 编译为本地机器码。源码：`src/jit/`。
- **AOT**（提前）：`xray build --aot` → 整个模块编译为 native binary。源码：`src/aot/`。
- 共享 SSA IR；后端选择不同（解释 / JIT / AOT）。
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## 17. Compilation Pipeline

> Source of truth: `src/frontend/`, `src/vm/`, `src/jit/`, `src/aot/`, `docs/rules/architecture.md`.

### 17.1 Pipeline Overview

```
Source (.xr)
    ↓ lexer
Token stream
    ↓ parser
AST
    ↓ analyzer (semantic analysis, type checking, scope/capture/generic)
Typed AST
    ↓ ssa-gen
SSA IR
    ↓ optimize (const fold, DCE, inline, TCO, escape analysis)
Optimized SSA
    ↓ codegen
Bytecode  →  AOT (machine code)
    ↓ VM
    ↓ Profiler → JIT (machine code)
Execution
```

### 17.2 Lexical Analysis (Lexer)

- Source of truth: `src/frontend/lexer/xlexer.c`.
- Outputs an `XrToken` stream; each token carries `kind`, `value`, `pos(line, col)`.
- Handles: string interpolation (producing `${...}` concatenation sequences), raw strings, regex literals.

### 17.3 Syntax Analysis (Parser)

- Source of truth: `src/frontend/parser/` (split by file: expr, stmt, decl, match).
- Style: hand-written Pratt parser (expressions) + recursive descent (declarations / statements).
- Error recovery: after an error, jumps to the next synchronization point (`;` `}` `)`) and tries to keep parsing.
- Output: an `XrAstNode*` root (i.e., the module).

### 17.4 Semantic Analysis (Analyzer)

- Source of truth: `src/frontend/analyzer/xanalyzer_*.c` (split by topic).
- **Scoping**: nested symbol tables, name resolution, shadowing checks.
- **Type checking**: bidirectional type inference, union narrowing, Json structural matching.
- **Generics**: monomorphization, constraint checking, call-site rewriting.
- **Closure analysis**: upvalue tagging, `go` closure capture restrictions.
- **Error codes**: the `XR_ERR_ANALYZE_*` family.

### 17.5 SSA and Optimization

- Source of truth: `src/ir/xi_opt*.c`, `src/ir/xi_pass.h`, `src/jit/`.
- **Constant folding**: compile-time evaluation.
- **DCE** (dead-code elimination): removes unused code.
- **Inlining**: small-function inlining.
- **TCO** (tail-call optimization): converts accumulator-style tail recursion into loops.
- **Escape analysis**: stack-vs-heap allocation decisions.

### 17.6 Bytecode and VM

- Source of truth: `src/vm/`, `include/xray_opcodes.h`.
- A hybrid register/stack VM.
- IC (inline cache) accelerates property access and method dispatch.

### 17.7 JIT and AOT

- **JIT** (runtime): once a hot function is selected by the profiler → it is compiled into native machine code. Source: `src/jit/`.
- **AOT** (ahead-of-time): `xray build --aot` → the entire module is compiled into a native binary. Source: `src/aot/`.
- They share the same SSA IR; only the back-end differs (interpret / JIT / AOT).
<!-- /xr-spec:en -->

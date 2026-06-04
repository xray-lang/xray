---
id: spec.preamble
order: 000
---

<!-- xr-spec:cn -->
# Xray 语言参考手册

> 版本：基于 `xray` v0.7.1 源码（截止 2026-05-21）
> 性质：语言规范与参考手册。本文档是描述 xray 语言**实际行为**的真值源。
> 实现：所有语义以 `xray` 当前主仓代码为准；本文档与代码不一致以代码为准并视为本文档需更新。
> 受众：xray 编写者、IDE / AI 工具实现者、编译器内部贡献者。

## 目录

- [0. 前言](#0-前言)
- [1. 词法结构 (Lexical Structure)](#1-词法结构-lexical-structure)
- [2. 类型系统 (Type System)](#2-类型系统-type-system)
- [3. 表达式 (Expressions)](#3-表达式-expressions)
- [4. 语句 (Statements)](#4-语句-statements)
- [5. 声明 (Declarations)](#5-声明-declarations)
- [6. 模式 (Patterns)](#6-模式-patterns)
- [7. 作用域与名字解析 (Scoping)](#7-作用域与名字解析-scoping)
- [8. 错误处理 (Error Handling)](#8-错误处理-error-handling)
- [9. 泛型 (Generics)](#9-泛型-generics)
- [10. 并发与协程 (Concurrency)](#10-并发与协程-concurrency)
- [11. 模块系统 (Modules)](#11-模块系统-modules)
- [12. 测试 (Testing)](#12-测试-testing)
- [13. 内置函数 (Built-in Functions)](#13-内置函数-built-in-functions)
- [14. 内置类型方法 (Built-in Type Methods)](#14-内置类型方法-built-in-type-methods)
- [15. 标准库概览 (Standard Library)](#15-标准库概览-standard-library)
- [16. 运行时模型 (Runtime Model)](#16-运行时模型-runtime-model)
- [17. 编译流水线 (Compilation Pipeline)](#17-编译流水线-compilation-pipeline)
- [18. 错误码参考 (Error Code Reference)](#18-错误码参考-error-code-reference)
- [附录 A. EBNF 语法](#附录-a-ebnf-语法)
- [附录 B. 关键字索引](#附录-b-关键字索引)
- [附录 C. 操作符索引](#附录-c-操作符索引)
- [附录 D. 标准库模块索引](#附录-d-标准库模块索引)
- [附录 E. 与其他语言的差异](#附录-e-与其他语言的差异)
- [附录 F. 词汇表](#附录-f-词汇表)
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
# Xray Language Reference

> Version: based on the `xray` source tree version v0.7.1 (audited on 2026-05-21).
> Status: this is a reference manual for the implemented language. When this document and the implementation disagree, the implementation is authoritative and this document must be updated.
> Chinese version: [`LANGUAGE_SPEC_CN.md`](LANGUAGE_SPEC_CN.md).

## Table of Contents

- [0. Preface](#0-preface)
- [1. Lexical Structure](#1-lexical-structure)
- [2. Type System](#2-type-system)
- [3. Expressions](#3-expressions)
- [4. Statements](#4-statements)
- [5. Declarations](#5-declarations)
- [6. Patterns](#6-patterns)
- [7. Scoping and Name Resolution](#7-scoping-and-name-resolution)
- [8. Error Handling](#8-error-handling)
- [9. Generics](#9-generics)
- [10. Concurrency and Coroutines](#10-concurrency-and-coroutines)
- [11. Modules](#11-modules)
- [12. Testing](#12-testing)
- [13. Built-in Functions](#13-built-in-functions)
- [14. Built-in Type Methods](#14-built-in-type-methods)
- [15. Standard Library](#15-standard-library)
- [16. Runtime Model](#16-runtime-model)
- [17. Compilation Pipeline](#17-compilation-pipeline)
- [18. Error Codes](#18-error-codes)
- [Appendix A. EBNF](#appendix-a-ebnf)
- [Appendix B. Keyword Index](#appendix-b-keyword-index)
- [Appendix C. Operator Index](#appendix-c-operator-index)
- [Appendix D. Standard Library Module Index](#appendix-d-standard-library-module-index)
- [Appendix E. Differences from Other Languages](#appendix-e-differences-from-other-languages)
- [Appendix F. Glossary](#appendix-f-glossary)
<!-- /xr-spec:en -->

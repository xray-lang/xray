/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_source_ast.h - Owned syntax at the ordinary public compiler boundary
 */
#ifndef XRAY_SOURCE_AST_H
#define XRAY_SOURCE_AST_H
#include "xray_source.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct XrSourceAst XrSourceAst;
/* Parses once on this compiler's cumulative ledger. The AST copies the input
 * and locator, and may outlive the compiler. Parsing grants no authority to
 * execute or import. Only successful parsing publishes the initially empty
 * AST output; an optional diagnostic is owned independently. */
XRAY_API XrSourceStatus xray_source_parse_text(XrSourceCompiler *, const XrSourceText *,
    XrSourceAst **, XrSourceDiagnostic **);
/* Borrows the exact owning tree from the same compiler. Ordinary authority,
 * imports, checking, specialization, lowering and Program admission still run.
 * No entry bytes are reparsed or written to disk. The AST remains owned on
 * every return; a successful Program outlives both AST and compiler. Output
 * owner pointers must be empty and remain unchanged on refusal. */
XRAY_API XrSourceStatus xray_source_compile_ast(XrSourceCompiler *, const XrSourceAst *,
    XrSourceProgram **, XrSourceDiagnostic **);
XRAY_API void xray_source_ast_free(XrSourceAst *);
#ifdef __cplusplus
}
#endif
#endif // XRAY_SOURCE_AST_H
